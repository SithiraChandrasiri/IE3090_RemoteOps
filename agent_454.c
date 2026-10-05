#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>

#define PORT 9410
#define SID "4540"
#define AUTH_TOKEN "OPS-0454"

#define BUFFER_SIZE 4096
#define LINE_SIZE 1024

#define STORAGE_PATH "./agentfiles/IT24100454/"
#define MAX_FILE_SIZE (10 * 1024 * 1024)


/*
 * Information about a connected Controller.
 */
typedef struct
{
    int client_socket;
    struct sockaddr_in client_address;

} client_info;


/*
 * ============================================================
 * SEND ALL
 * ============================================================
 *
 * send() is not guaranteed to send all requested bytes
 * in one call.
 *
 * This function keeps sending until all bytes are sent.
 */
int send_all(int socket, const char *data, size_t length)
{
    size_t total_sent = 0;

    while (total_sent < length)
    {
        ssize_t sent =
            send(socket,
                 data + total_sent,
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
 *
 * Reads one TCP command until '\n'.
 *
 * TCP does not preserve message boundaries, so we cannot
 * assume one recv() equals one command.
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
            /*
             * Client disconnected.
             */
            return 0;
        }

        if (received < 0)
        {
            /*
             * Receive error.
             */
            return -1;
        }

        /*
         * Newline means the command is complete.
         */
        if (character == '\n')
        {
            break;
        }

        /*
         * Ignore carriage return.
         */
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
 * RECEIVE EXACT NUMBER OF BYTES
 * ============================================================
 *
 * Used for file transfers.
 *
 * If the Controller says:
 *
 * PUT test.txt 1000
 *
 * the Agent must receive exactly 1000 bytes.
 *
 * One recv() may receive only part of those bytes, so this
 * function continues until the requested number is received.
 */
int receive_exact(int socket,
                  void *buffer,
                  size_t length)
{
    size_t total_received = 0;

    while (total_received < length)
    {
        ssize_t received =
            recv(socket,
                 (char *)buffer + total_received,
                 length - total_received,
                 0);

        if (received == 0)
        {
            /*
             * Connection closed before all bytes arrived.
             */
            return -1;
        }

        if (received < 0)
        {
            return -1;
        }

        total_received += received;
    }

    return 0;
}


/*
 * ============================================================
 * VALIDATE FILENAME
 * ============================================================
 *
 * Files are stored inside:
 *
 * ./agentfiles/IT24100454/
 *
 * We do not allow directory separators because a Controller
 * should not be able to escape this directory.
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

    /*
     * Reject Linux/Unix directory separators.
     */
    if (strchr(filename, '/') != NULL)
    {
        return 0;
    }

    /*
     * Reject Windows-style separators too.
     */
    if (strchr(filename, '\\') != NULL)
    {
        return 0;
    }

    /*
     * Reject current/parent directory names.
     */
    if (strcmp(filename, ".") == 0 ||
        strcmp(filename, "..") == 0)
    {
        return 0;
    }

    return 1;
}


/*
 * ============================================================
 * SYSTEM INFORMATION
 * ============================================================
 *
 * Gets:
 *
 * CPU load
 * Memory used
 * Uptime
 */
int get_system_info(double *cpu_load,
                    unsigned long *memory_used_mb,
                    unsigned long *uptime_seconds)
{
    FILE *file;


    /*
     * --------------------------------------------------------
     * CPU LOAD
     * --------------------------------------------------------
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
     * --------------------------------------------------------
     * MEMORY
     * --------------------------------------------------------
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
        if (sscanf(line,
                   "MemTotal: %lu kB",
                   &mem_total_kb) == 1)
        {
            continue;
        }

        if (sscanf(line,
                   "MemAvailable: %lu kB",
                   &mem_available_kb) == 1)
        {
            continue;
        }
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
     * --------------------------------------------------------
     * UPTIME
     * --------------------------------------------------------
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
 *
 * Gets a snapshot of currently running processes.
 */
void handle_listproc(int client_socket)
{
    FILE *processes;


    processes =
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


        /*
         * Make sure we leave room for:
         *
         * " SID:4540\n"
         */
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
 *
 * Only these five commands are allowed:
 *
 * DATE
 * UPTIME
 * DISKFREE
 * HOSTNAME
 * WHOAMI
 *
 * Arbitrary commands are NOT executed.
 */
void handle_exec(int client_socket,
                 char *command)
{
    const char *system_command = NULL;


    /*
     * --------------------------------------------------------
     * COMMAND WHITELIST
     * --------------------------------------------------------
     */

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
        /*
         * Command is NOT in the whitelist.
         */
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


    /*
     * --------------------------------------------------------
     * EXECUTE PREDEFINED COMMAND
     * --------------------------------------------------------
     */

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


    /*
     * The five allowed commands produce small outputs,
     * so 1024 bytes is enough.
     */
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


    /*
     * Remove newline from command output.
     */
    output[strcspn(output,
                   "\r\n")] = '\0';


    /*
     * Build response.
     */
    char response[BUFFER_SIZE];


    int written =
        snprintf(response,
                 sizeof(response),
                 "OK EXEC_RESULT %s SID:%s\n",
                 output,
                 SID);


    if (written < 0)
    {
        return;
    }


    send_all(client_socket,
             response,
             strlen(response));
}


/*
 * ============================================================
 * PUT
 * ============================================================
 *
 * Controller sends:
 *
 * PUT filename filesize\n
 *
 * followed immediately by:
 *
 * exactly filesize raw bytes
 */
void handle_put(int client_socket,
                char *line)
{
    char filename[256];

    unsigned long long file_size;


    /*
     * Parse:
     *
     * PUT filename filesize
     */
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


    /*
     * Check filename.
     */
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


    /*
     * Check maximum file size.
     */
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


    /*
     * Build personalised storage path.
     */
    char filepath[512];

    snprintf(filepath,
             sizeof(filepath),
             "%s%s",
             STORAGE_PATH,
             filename);


    /*
     * Open destination file.
     */
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


    /*
     * --------------------------------------------------------
     * RECEIVE FILE DATA
     * --------------------------------------------------------
     *
     * We use chunks instead of allocating the whole file
     * in memory.
     */
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


    /*
     * Remove incomplete file.
     */
    if (transfer_failed)
    {
        remove(filepath);

        printf("PUT failed: %s\n",
               filename);

        return;
    }


    /*
     * Successful upload.
     */
    char response[BUFFER_SIZE];

    snprintf(response,
             sizeof(response),
             "OK FILE_RECEIVED %s SID:%s\n",
             filename,
             SID);


    send_all(client_socket,
             response,
             strlen(response));


    printf("PUT completed: %s (%llu bytes)\n",
           filename,
           file_size);
}


/*
 * ============================================================
 * GET
 * ============================================================
 *
 * Controller sends:
 *
 * GET filename\n
 *
 * Agent responds:
 *
 * OK FILE_SEND filename filesize SID:sid\n
 *
 * followed immediately by exactly filesize bytes.
 */
void handle_get(int client_socket,
                char *line)
{
    char filename[256];


    /*
     * Parse:
     *
     * GET filename
     */
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


    /*
     * Validate filename.
     */
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


    /*
     * Build storage path.
     */
    char filepath[512];

    snprintf(filepath,
             sizeof(filepath),
             "%s%s",
             STORAGE_PATH,
             filename);


    /*
     * Open file.
     */
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


    /*
     * Find file size.
     */
    if (fseek(file,
              0,
              SEEK_END) != 0)
    {
        fclose(file);
        return;
    }


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


    /*
     * --------------------------------------------------------
     * SEND FILE HEADER
     * --------------------------------------------------------
     */
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


    /*
     * --------------------------------------------------------
     * SEND FILE DATA
     * --------------------------------------------------------
     */
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
            return;
        }
    }


    fclose(file);


    printf("GET completed: %s (%llu bytes)\n",
           filename,
           file_size);
}


/*
 * ============================================================
 * HANDLE CLIENT
 * ============================================================
 *
 * Each Controller gets its own thread.
 */
void *handle_client(void *arg)
{
    client_info *client =
        (client_info *)arg;


    int client_socket =
        client->client_socket;


    free(client);


    /*
     * Authentication belongs to this connection.
     */
    int authenticated = 0;


    char line[LINE_SIZE];


    printf("Client connected.\n");


    while (1)
    {
        /*
         * Read one complete command.
         */
        int result =
            receive_line(client_socket,
                         line,
                         sizeof(line));


        /*
         * Client disconnected.
         */
        if (result == 0)
        {
            printf("Client disconnected.\n");
            break;
        }


        /*
         * Receive error.
         */
        if (result < 0)
        {
            perror("recv");
            break;
        }


        /*
         * Ignore empty lines.
         */
        if (strlen(line) == 0)
        {
            continue;
        }


        printf("Received: %s\n",
               line);


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


                    printf("Authentication successful.\n");
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


                    printf("Authentication failed.\n");
                }
            }


            continue;
        }


        /*
         * ====================================================
         * AUTHENTICATION CHECK
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
         * QUIT
         * ====================================================
         */
        if (strcmp(line,
                   "QUIT") == 0)
        {
            char response[BUFFER_SIZE];


            snprintf(response,
                     sizeof(response),
                     "OK BYE SID:%s\n",
                     SID);


            send_all(client_socket,
                     response,
                     strlen(response));


            printf("Client requested QUIT.\n");


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
     * Close Controller connection.
     */
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
    int server_socket;

    struct sockaddr_in server_address;


    /*
     * ========================================================
     * CREATE TCP SOCKET
     * ========================================================
     */
    server_socket =
        socket(AF_INET,
               SOCK_STREAM,
               0);


    if (server_socket < 0)
    {
        perror("socket");
        return 1;
    }


    /*
     * ========================================================
     * ALLOW PORT REUSE
     * ========================================================
     */
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


    /*
     * ========================================================
     * SERVER ADDRESS
     * ========================================================
     */
    memset(&server_address,
           0,
           sizeof(server_address));


    server_address.sin_family =
        AF_INET;


    server_address.sin_addr.s_addr =
        INADDR_ANY;


    server_address.sin_port =
        htons(PORT);


    /*
     * ========================================================
     * BIND
     * ========================================================
     */
    if (bind(server_socket,
             (struct sockaddr *)&server_address,
             sizeof(server_address)) < 0)
    {
        perror("bind");

        close(server_socket);

        return 1;
    }


    /*
     * ========================================================
     * LISTEN
     * ========================================================
     */
    if (listen(server_socket,
               5) < 0)
    {
        perror("listen");

        close(server_socket);

        return 1;
    }


    /*
     * ========================================================
     * STARTUP INFORMATION
     * ========================================================
     */
    printf("====================================\n");

    printf("RemoteOps Agent\n");

    printf("Registration: IT24100454\n");

    printf("TCP Port: %d\n",
           PORT);

    printf("SID: %s\n",
           SID);

    printf("Storage: %s\n",
           STORAGE_PATH);

    printf("Waiting for connections...\n");

    printf("====================================\n");


    /*
     * ========================================================
     * ACCEPT CONTROLLERS
     * ========================================================
     */
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


        /*
         * ====================================================
         * CREATE THREAD
         * ====================================================
         */
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


        /*
         * The Agent does not need to wait for
         * this client thread.
         */
        pthread_detach(thread);
    }


    close(server_socket);

    return 0;
}
