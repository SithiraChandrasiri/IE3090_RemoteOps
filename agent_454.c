#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <pthread.h>
#include <errno.h>
#include <signal.h>
#include <time.h>

#define PORT 9410
#define SID "4540"
#define AUTH_TOKEN "OPS-0454"

#define BUFFER_SIZE 4096
#define LINE_SIZE 1024

#define STORAGE_PATH "./agentfiles/IT24100454/"
#define LOG_FILE "remoteops_IT24100454.log"

#define MAX_FILE_SIZE (10 * 1024 * 1024)

/*
 * ============================================================
 * CLIENT INFORMATION
 * ============================================================
 */

typedef struct
{
    int client_socket;
    struct sockaddr_in client_address;

} client_info;


/*
 * ============================================================
 * MONITOR INFORMATION
 * ============================================================
 */

typedef struct
{
    int udp_socket;

    volatile int running;

    int udp_port;

    struct sockaddr_in destination;

    pthread_t thread;

} monitor_state;


/*
 * ============================================================
 * LOGGING
 * ============================================================
 *
 * Multiple client threads may write to the log at the same
 * time, so a mutex is used to protect the log file.
 */

pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;


/*
 * Write one timestamped event to the log.
 */
void log_event(const char *event)
{
    time_t current_time;

    struct tm time_info;

    char timestamp[64];


    time(&current_time);

    localtime_r(&current_time,
                &time_info);


    strftime(timestamp,
             sizeof(timestamp),
             "%Y-%m-%d %H:%M:%S",
             &time_info);


    pthread_mutex_lock(&log_mutex);


    FILE *log_file =
        fopen(LOG_FILE, "a");


    if (log_file != NULL)
    {
        fprintf(log_file,
                "[%s] %s\n",
                timestamp,
                event);

        fflush(log_file);

        fclose(log_file);
    }


    pthread_mutex_unlock(&log_mutex);
}


/*
 * ============================================================
 * SEND ALL
 * ============================================================
 */

int send_all(int socket,
             const void *data,
             size_t length)
{
    size_t total_sent = 0;


    while (total_sent < length)
    {
        ssize_t sent =
            send(socket,
                 (const char *)data + total_sent,
                 length - total_sent,
                 0);


        if (sent <= 0)
        {
            return -1;
        }


        total_sent += sent;
    }


    return 0;
}


/*
 * ============================================================
 * RECEIVE ONE LINE
 * ============================================================
 */

int receive_line(int socket,
                 char *buffer,
                 size_t buffer_size)
{
    size_t position = 0;


    while (position < buffer_size - 1)
    {
        char character;


        ssize_t received =
            recv(socket,
                 &character,
                 1,
                 0);


        if (received == 0)
        {
            return 0;
        }


        if (received < 0)
        {
            return -1;
        }


        if (character == '\n')
        {
            break;
        }


        if (character != '\r')
        {
            buffer[position] = character;

            position++;
        }
    }


    buffer[position] = '\0';


    return 1;
}


/*
 * ============================================================
 * VALIDATE FILENAME
 * ============================================================
 */

int valid_filename(const char *filename)
{
    if (filename == NULL)
    {
        return 0;
    }


    if (strlen(filename) == 0)
    {
        return 0;
    }


    if (strchr(filename, '/') != NULL)
    {
        return 0;
    }


    if (strchr(filename, '\\') != NULL)
    {
        return 0;
    }


    if (strcmp(filename, ".") == 0 ||
        strcmp(filename, "..") == 0)
    {
        return 0;
    }


    return 1;
}


/*
 * ============================================================
 * GET SYSTEM INFORMATION
 * ============================================================
 */

int get_system_info(double *cpu_load,
                    unsigned long *memory_used_mb,
                    unsigned long *uptime_seconds)
{
    FILE *file;


    /*
     * CPU load
     */

    file = fopen("/proc/loadavg", "r");


    if (file == NULL)
    {
        return -1;
    }


    if (fscanf(file,
               "%lf",
               cpu_load) != 1)
    {
        fclose(file);

        return -1;
    }


    fclose(file);


    /*
     * Memory
     */

    unsigned long mem_total_kb = 0;

    unsigned long mem_available_kb = 0;


    file = fopen("/proc/meminfo", "r");


    if (file == NULL)
    {
        return -1;
    }


    char line[256];


    while (fgets(line,
                 sizeof(line),
                 file) != NULL)
    {
        sscanf(line,
               "MemTotal: %lu kB",
               &mem_total_kb);

        sscanf(line,
               "MemAvailable: %lu kB",
               &mem_available_kb);
    }


    fclose(file);


    if (mem_total_kb == 0)
    {
        return -1;
    }


    unsigned long memory_used_kb =
        mem_total_kb - mem_available_kb;


    *memory_used_mb =
        memory_used_kb / 1024;


    /*
     * Uptime
     */

    double uptime;


    file = fopen("/proc/uptime", "r");


    if (file == NULL)
    {
        return -1;
    }


    if (fscanf(file,
               "%lf",
               &uptime) != 1)
    {
        fclose(file);

        return -1;
    }


    fclose(file);


    *uptime_seconds =
        (unsigned long)uptime;


    return 0;
}


/*
 * ============================================================
 * SYSINFO
 * ============================================================
 */

void handle_sysinfo(int client_socket)
{
    double cpu_load;

    unsigned long memory_used_mb;

    unsigned long uptime_seconds;


    if (get_system_info(&cpu_load,
                        &memory_used_mb,
                        &uptime_seconds) != 0)
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 003 SYSINFO_FAILED SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return;
    }


    char response[BUFFER_SIZE];


    snprintf(response,
             sizeof(response),
             "OK SYSINFO %.2f %lu %lu SID:%s\n",
             cpu_load,
             memory_used_mb,
             uptime_seconds,
             SID);


    send_all(client_socket,
             response,
             strlen(response));
}


/*
 * ============================================================
 * LISTPROC
 * ============================================================
 */

void handle_listproc(int client_socket)
{
    FILE *processes =
        popen("ps -eo comm=,pid=", "r");


    if (processes == NULL)
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 003 PROCESS_LIST_FAILED SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return;
    }


    char response[BUFFER_SIZE];


    snprintf(response,
             sizeof(response),
             "OK PROCS ");


    size_t current_length =
        strlen(response);


    char process_line[256];


    int first_process = 1;


    while (fgets(process_line,
                 sizeof(process_line),
                 processes) != NULL)
    {
        char process_name[128];

        int pid;


        if (sscanf(process_line,
                   "%127s %d",
                   process_name,
                   &pid) != 2)
        {
            continue;
        }


        char process_entry[200];


        snprintf(process_entry,
                 sizeof(process_entry),
                 "%s%s/%d",
                 first_process ? "" : ",",
                 process_name,
                 pid);


        size_t entry_length =
            strlen(process_entry);


        if (current_length +
            entry_length +
            20 >=
            sizeof(response))
        {
            break;
        }


        strcat(response,
               process_entry);


        current_length +=
            entry_length;


        first_process = 0;
    }


    pclose(processes);


    strcat(response,
           " SID:");

    strcat(response,
           SID);

    strcat(response,
           "\n");


    send_all(client_socket,
             response,
             strlen(response));
}


/*
 * ============================================================
 * EXEC
 * ============================================================
 */

void handle_exec(int client_socket,
                 char *command)
{
    const char *system_command = NULL;


    if (strcmp(command,
               "DATE") == 0)
    {
        system_command = "date";
    }

    else if (strcmp(command,
                    "UPTIME") == 0)
    {
        system_command = "uptime";
    }

    else if (strcmp(command,
                    "DISKFREE") == 0)
    {
        system_command =
            "df -h / | tail -n 1";
    }

    else if (strcmp(command,
                    "HOSTNAME") == 0)
    {
        system_command = "hostname";
    }

    else if (strcmp(command,
                    "WHOAMI") == 0)
    {
        system_command = "whoami";
    }

    else
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 002 COMMAND_NOT_ALLOWED SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return;
    }


    FILE *process =
        popen(system_command,
              "r");


    if (process == NULL)
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 003 EXEC_FAILED SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return;
    }


    char output[1024];


    memset(output,
           0,
           sizeof(output));


    if (fgets(output,
              sizeof(output),
              process) == NULL)
    {
        pclose(process);


        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 003 EXEC_FAILED SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return;
    }


    pclose(process);


    output[strcspn(output,
                   "\r\n")] = '\0';


    char response[BUFFER_SIZE];


    snprintf(response,
             sizeof(response),
             "OK EXEC_RESULT %s SID:%s\n",
             output,
             SID);


    send_all(client_socket,
             response,
             strlen(response));
}


/*
 * ============================================================
 * PUT
 * ============================================================
 */

void handle_put(int client_socket,
                char *line)
{
    char filename[256];

    unsigned long long file_size;


    if (sscanf(line,
               "PUT %255s %llu",
               filename,
               &file_size) != 2)
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 006 INVALID_PUT SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return;
    }


    if (!valid_filename(filename))
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 006 INVALID_FILENAME SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return;
    }


    if (file_size > MAX_FILE_SIZE)
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 004 FILE_TOO_LARGE SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return;
    }


    char filepath[512];


    snprintf(filepath,
             sizeof(filepath),
             "%s%s",
             STORAGE_PATH,
             filename);


    FILE *file =
        fopen(filepath,
              "wb");


    if (file == NULL)
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 006 FILE_OPEN_FAILED SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return;
    }


    char buffer[BUFFER_SIZE];


    unsigned long long remaining =
        file_size;


    int transfer_failed = 0;


    while (remaining > 0)
    {
        size_t chunk_size;


        if (remaining >
            sizeof(buffer))
        {
            chunk_size =
                sizeof(buffer);
        }

        else
        {
            chunk_size =
                (size_t)remaining;
        }


        ssize_t received =
            recv(client_socket,
                 buffer,
                 chunk_size,
                 0);


        if (received <= 0)
        {
            transfer_failed = 1;

            break;
        }


        size_t written =
            fwrite(buffer,
                   1,
                   received,
                   file);


        if (written !=
            (size_t)received)
        {
            transfer_failed = 1;

            break;
        }


        remaining -=
            received;
    }


    fclose(file);


    if (transfer_failed)
    {
        remove(filepath);


        char log_message[512];


        snprintf(log_message,
                 sizeof(log_message),
                 "FILE_TRANSFER_FAILED PUT %s",
                 filename);


        log_event(log_message);


        return;
    }


    char response[BUFFER_SIZE];


    snprintf(response,
             sizeof(response),
             "OK FILE_RECEIVED %s SID:%s\n",
             filename,
             SID);


    send_all(client_socket,
             response,
             strlen(response));


    char log_message[512];


    snprintf(log_message,
             sizeof(log_message),
             "FILE_RECEIVED PUT %s %llu bytes",
             filename,
             file_size);


    log_event(log_message);
}


/*
 * ============================================================
 * GET
 * ============================================================
 */

void handle_get(int client_socket,
                char *line)
{
    char filename[256];


    if (sscanf(line,
               "GET %255s",
               filename) != 1)
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 006 INVALID_GET SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return;
    }


    if (!valid_filename(filename))
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 006 INVALID_FILENAME SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return;
    }


    char filepath[512];


    snprintf(filepath,
             sizeof(filepath),
             "%s%s",
             STORAGE_PATH,
             filename);


    FILE *file =
        fopen(filepath,
              "rb");


    if (file == NULL)
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 005 FILE_NOT_FOUND SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return;
    }


    fseek(file,
          0,
          SEEK_END);


    long file_size_long =
        ftell(file);


    if (file_size_long < 0)
    {
        fclose(file);

        return;
    }


    rewind(file);


    unsigned long long file_size =
        (unsigned long long)file_size_long;


    char response[BUFFER_SIZE];


    snprintf(response,
             sizeof(response),
             "OK FILE_SEND %s %llu SID:%s\n",
             filename,
             file_size,
             SID);


    if (send_all(client_socket,
                 response,
                 strlen(response)) != 0)
    {
        fclose(file);

        return;
    }


    char buffer[BUFFER_SIZE];

    size_t bytes_read;


    while ((bytes_read =
                fread(buffer,
                      1,
                      sizeof(buffer),
                      file)) > 0)
    {
        if (send_all(client_socket,
                     buffer,
                     bytes_read) != 0)
        {
            fclose(file);


            char log_message[512];


            snprintf(log_message,
                     sizeof(log_message),
                     "FILE_TRANSFER_FAILED GET %s",
                     filename);


            log_event(log_message);


            return;
        }
    }


    fclose(file);


    char log_message[512];


    snprintf(log_message,
             sizeof(log_message),
             "FILE_SENT GET %s %llu bytes",
             filename,
             file_size);


    log_event(log_message);
}


/*
 * ============================================================
 * UDP MONITOR THREAD
 * ============================================================
 */

void *monitor_worker(void *arg)
{
    monitor_state *monitor =
        (monitor_state *)arg;


    char log_message[256];


    snprintf(log_message,
             sizeof(log_message),
             "MONITOR_THREAD_STARTED UDP_PORT:%d",
             monitor->udp_port);


    log_event(log_message);


    while (monitor->running)
    {
        double cpu_load;

        unsigned long memory_used_mb;

        unsigned long uptime_seconds;


        if (get_system_info(&cpu_load,
                            &memory_used_mb,
                            &uptime_seconds) == 0)
        {
            char message[BUFFER_SIZE];


            snprintf(message,
                     sizeof(message),
                     "SYSINFO %.2f %lu %lu SID:%s\n",
                     cpu_load,
                     memory_used_mb,
                     uptime_seconds,
                     SID);


            sendto(monitor->udp_socket,
                   message,
                   strlen(message),
                   0,
                   (struct sockaddr *)&monitor->destination,
                   sizeof(monitor->destination));


            printf("UDP monitor: %s",
                   message);
        }


        for (int i = 0; i < 5; i++)
        {
            if (!monitor->running)
            {
                break;
            }

            sleep(1);
        }
    }


    log_event("MONITOR_THREAD_STOPPED");


    return NULL;
}


/*
 * ============================================================
 * START MONITOR
 * ============================================================
 */

int start_monitor(int client_socket,
                  monitor_state *monitor,
                  int udp_port,
                  struct sockaddr_in *client_address)
{
    if (monitor->running)
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 006 MONITOR_ALREADY_RUNNING SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return -1;
    }


    if (udp_port < 1024 ||
        udp_port > 65535)
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 006 INVALID_UDP_PORT SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return -1;
    }


    monitor->udp_socket =
        socket(AF_INET,
               SOCK_DGRAM,
               0);


    if (monitor->udp_socket < 0)
    {
        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 003 MONITOR_SOCKET_FAILED SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return -1;
    }


    memset(&monitor->destination,
           0,
           sizeof(monitor->destination));


    monitor->destination.sin_family =
        AF_INET;


    monitor->destination.sin_addr =
        client_address->sin_addr;


    monitor->destination.sin_port =
        htons(udp_port);


    monitor->udp_port =
        udp_port;


    monitor->running = 1;


    if (pthread_create(&monitor->thread,
                       NULL,
                       monitor_worker,
                       monitor) != 0)
    {
        monitor->running = 0;


        close(monitor->udp_socket);


        char response[BUFFER_SIZE];


        snprintf(response,
                 sizeof(response),
                 "ERR 003 MONITOR_THREAD_FAILED SID:%s\n",
                 SID);


        send_all(client_socket,
                 response,
                 strlen(response));


        return -1;
    }


    char response[BUFFER_SIZE];


    snprintf(response,
             sizeof(response),
             "OK MONITOR_STARTED SID:%s\n",
             SID);


    send_all(client_socket,
             response,
             strlen(response));


    log_event("MONITOR_STARTED");


    return 0;
}


/*
 * ============================================================
 * STOP MONITOR
 * ============================================================
 */

void stop_monitor(monitor_state *monitor)
{
    if (!monitor->running)
    {
        return;
    }


    monitor->running = 0;


    pthread_join(monitor->thread,
                 NULL);


    close(monitor->udp_socket);


    monitor->udp_socket = -1;


    log_event("MONITOR_STOPPED");
}


/*
 * ============================================================
 * HANDLE CLIENT
 * ============================================================
 */

void *handle_client(void *arg)
{
    client_info *client =
        (client_info *)arg;


    int client_socket =
        client->client_socket;


    struct sockaddr_in client_address =
        client->client_address;


    char client_ip[INET_ADDRSTRLEN];


    inet_ntop(AF_INET,
              &client_address.sin_addr,
              client_ip,
              sizeof(client_ip));


    free(client);


    /*
     * Log connection.
     */

    char log_message[512];


    snprintf(log_message,
             sizeof(log_message),
             "CLIENT_CONNECTED %s",
             client_ip);


    log_event(log_message);


    printf("Client connected from %s\n",
           client_ip);


    /*
     * Each client gets its own monitor.
     */

    monitor_state monitor;


    memset(&monitor,
           0,
           sizeof(monitor));


    monitor.udp_socket = -1;


    int authenticated = 0;


    char line[LINE_SIZE];


    while (1)
    {
        int result =
            receive_line(client_socket,
                         line,
                         sizeof(line));


        /*
         * Normal disconnect.
         */

        if (result == 0)
        {
            snprintf(log_message,
                     sizeof(log_message),
                     "CLIENT_DISCONNECTED %s",
                     client_ip);


            log_event(log_message);


            break;
        }


        /*
         * Receive error.
         */

        if (result < 0)
        {
            snprintf(log_message,
                     sizeof(log_message),
                     "CLIENT_DISCONNECTED_ERROR %s",
                     client_ip);


            log_event(log_message);


            break;
        }


        if (strlen(line) == 0)
        {
            continue;
        }


        printf("Received: %s\n",
               line);


        /*
         * Log every command.
         */

        snprintf(log_message,
                 sizeof(log_message),
                 "COMMAND %s",
                 line);


        log_event(log_message);


        /*
         * ====================================================
         * AUTH
         * ====================================================
         */

        if (strncmp(line,
                    "AUTH ",
                    5) == 0)
        {
            char token[100];


            if (sscanf(line,
                       "AUTH %99s",
                       token) == 1)
            {
                if (strcmp(token,
                           AUTH_TOKEN) == 0)
                {
                    authenticated = 1;


                    char response[BUFFER_SIZE];


                    snprintf(response,
                             sizeof(response),
                             "OK AUTHENTICATED SID:%s\n",
                             SID);


                    send_all(client_socket,
                             response,
                             strlen(response));


                    log_event("AUTH_SUCCESS");
                }

                else
                {
                    authenticated = 0;


                    char response[BUFFER_SIZE];


                    snprintf(response,
                             sizeof(response),
                             "ERR 001 AUTH_FAILED SID:%s\n",
                             SID);


                    send_all(client_socket,
                             response,
                             strlen(response));


                    log_event("AUTH_FAILED");
                }
            }


            continue;
        }


        /*
         * ====================================================
         * AUTH REQUIRED
         * ====================================================
         */

        if (!authenticated)
        {
            char response[BUFFER_SIZE];


            snprintf(response,
                     sizeof(response),
                     "ERR 001 AUTH_REQUIRED SID:%s\n",
                     SID);


            send_all(client_socket,
                     response,
                     strlen(response));


            continue;
        }


        /*
         * ====================================================
         * SYSINFO
         * ====================================================
         */

        if (strcmp(line,
                   "SYSINFO") == 0)
        {
            handle_sysinfo(client_socket);

            continue;
        }


        /*
         * ====================================================
         * LISTPROC
         * ====================================================
 */

        if (strcmp(line,
                   "LISTPROC") == 0)
        {
            handle_listproc(client_socket);

            continue;
        }


        /*
         * ====================================================
         * EXEC
         * ====================================================
         */

        if (strncmp(line,
                    "EXEC ",
                    5) == 0)
        {
            char command[100];


            if (sscanf(line,
                       "EXEC %99s",
                       command) == 1)
            {
                handle_exec(client_socket,
                            command);
            }

            else
            {
                char response[BUFFER_SIZE];


                snprintf(response,
                         sizeof(response),
                         "ERR 002 COMMAND_NOT_ALLOWED SID:%s\n",
                         SID);


                send_all(client_socket,
                         response,
                         strlen(response));
            }


            continue;
        }


        /*
         * ====================================================
         * PUT
         * ====================================================
         */

        if (strncmp(line,
                    "PUT ",
                    4) == 0)
        {
            handle_put(client_socket,
                       line);

            continue;
        }


        /*
         * ====================================================
         * GET
         * ====================================================
         */

        if (strncmp(line,
                    "GET ",
                    4) == 0)
        {
            handle_get(client_socket,
                       line);

            continue;
        }


        /*
         * ====================================================
         * MONITOR START
         * ====================================================
         */

        if (strncmp(line,
                    "MONITOR START ",
                    14) == 0)
        {
            int udp_port;


            if (sscanf(line,
                       "MONITOR START %d",
                       &udp_port) != 1)
            {
                char response[BUFFER_SIZE];


                snprintf(response,
                         sizeof(response),
                         "ERR 006 INVALID_MONITOR_START SID:%s\n",
                         SID);


                send_all(client_socket,
                         response,
                         strlen(response));


                continue;
            }


            start_monitor(client_socket,
                          &monitor,
                          udp_port,
                          &client_address);


            continue;
        }


        /*
         * ====================================================
         * MONITOR STOP
         * ====================================================
         */

        if (strcmp(line,
                   "MONITOR STOP") == 0)
        {
            if (!monitor.running)
            {
                char response[BUFFER_SIZE];


                snprintf(response,
                         sizeof(response),
                         "ERR 006 MONITOR_NOT_RUNNING SID:%s\n",
                         SID);


                send_all(client_socket,
                         response,
                         strlen(response));


                continue;
            }


            stop_monitor(&monitor);


            char response[BUFFER_SIZE];


            snprintf(response,
                     sizeof(response),
                     "OK MONITOR_STOPPED SID:%s\n",
                     SID);


            send_all(client_socket,
                     response,
                     strlen(response));


            continue;
        }


        /*
         * ====================================================
         * QUIT
         * ====================================================
         */

        if (strcmp(line,
                   "QUIT") == 0)
        {
            if (monitor.running)
            {
                stop_monitor(&monitor);
            }


            char response[BUFFER_SIZE];


            snprintf(response,
                     sizeof(response),
                     "OK BYE SID:%s\n",
                     SID);


            send_all(client_socket,
                     response,
                     strlen(response));


            log_event("CLIENT_QUIT");


            break;
        }


        /*
         * ====================================================
         * UNKNOWN COMMAND
         * ====================================================
         */

        {
            char response[BUFFER_SIZE];


            snprintf(response,
                     sizeof(response),
                     "ERR 006 UNKNOWN_COMMAND SID:%s\n",
                     SID);


            send_all(client_socket,
                     response,
                     strlen(response));
        }
    }


    /*
     * If the Controller disappears while monitoring,
     * cleanly stop the monitor.
     */

    if (monitor.running)
    {
        stop_monitor(&monitor);
    }


    close(client_socket);


    return NULL;
}


/*
 * ============================================================
 * MAIN
 * ============================================================
 */

int main()
{
    /*
     * Prevent a broken TCP connection from terminating the
     * entire Agent process with SIGPIPE.
     */

    signal(SIGPIPE,
           SIG_IGN);


    /*
     * Create storage directory if it does not already exist.
     */

    mkdir("./agentfiles",
          0755);

    mkdir(STORAGE_PATH,
          0755);


    /*
     * Create initial log entry.
     */

    log_event("AGENT_STARTED");


    int server_socket =
        socket(AF_INET,
               SOCK_STREAM,
               0);


    if (server_socket < 0)
    {
        perror("socket");

        return 1;
    }


    int option = 1;


    if (setsockopt(server_socket,
                   SOL_SOCKET,
                   SO_REUSEADDR,
                   &option,
                   sizeof(option)) < 0)
    {
        perror("setsockopt");

        close(server_socket);

        return 1;
    }


    struct sockaddr_in server_address;


    memset(&server_address,
           0,
           sizeof(server_address));


    server_address.sin_family =
        AF_INET;


    server_address.sin_addr.s_addr =
        INADDR_ANY;


    server_address.sin_port =
        htons(PORT);


    if (bind(server_socket,
             (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0)
    {
        perror("bind");

        close(server_socket);

        return 1;
    }


    if (listen(server_socket,
               5) < 0)
    {
        perror("listen");

        close(server_socket);

        return 1;
    }


    printf("====================================\n");

    printf("RemoteOps Agent\n");

    printf("Registration: IT24100454\n");

    printf("TCP Port: %d\n",
           PORT);

    printf("SID: %s\n",
           SID);

    printf("Storage: %s\n",
           STORAGE_PATH);

    printf("Log: %s\n",
           LOG_FILE);

    printf("Waiting for connections...\n");

    printf("====================================\n");


    while (1)
    {
        client_info *client =
            malloc(sizeof(client_info));


        if (client == NULL)
        {
            perror("malloc");

            continue;
        }


        socklen_t address_length =
            sizeof(client->client_address);


        client->client_socket =
            accept(server_socket,
                   (struct sockaddr *)&client->client_address,
                   &address_length);


        if (client->client_socket < 0)
        {
            perror("accept");

            free(client);

            continue;
        }


        pthread_t thread;


        if (pthread_create(&thread,
                           NULL,
                           handle_client,
                           client) != 0)
        {
            perror("pthread_create");

            close(client->client_socket);

            free(client);

            continue;
        }


        pthread_detach(thread);
    }


    close(server_socket);


    return 0;
}
