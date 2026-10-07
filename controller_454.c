#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <pthread.h>
#include <errno.h>
#include <sys/time.h>

#define SERVER_PORT 9410
#define SERVER_IP_DEFAULT "127.0.0.1"

#define SID "4540"
#define AUTH_TOKEN "OPS-0454"

#define BUFFER_SIZE 4096
#define LINE_SIZE 1024


/*
 * ============================================================
 * UDP MONITOR INFORMATION
 * ============================================================
 */

typedef struct
{
    int udp_socket;
    volatile int running;

} monitor_info;


/*
 * Global monitor information.
 */
monitor_info monitor;

pthread_t monitor_thread_id;
/*
 * ============================================================
 * SEND ALL
 * ============================================================
 *
 * TCP send() may send only part of the requested data.
 *
 * This function continues until all bytes are sent.
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
 *
 * Reads one TCP line until '\n'.
 *
 * We read one byte at a time so that we do not accidentally
 * consume file bytes after a PUT/GET header.
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
 * RECEIVE EXACT NUMBER OF BYTES
 * ============================================================
 *
 * Used when downloading files.
 *
 * If the Agent says:
 *
 * OK FILE_SEND test.txt 100 SID:4540
 *
 * we must receive exactly 100 bytes.
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
 * PRINT TCP RESPONSE
 * ============================================================
 */
int receive_response(int socket)
{
    char response[BUFFER_SIZE];

    int result =
        receive_line(socket,
                     response,
                     sizeof(response));

    if (result <= 0)
    {
        printf("Connection closed or receive error.\n");

        return -1;
    }

    printf("Agent: %s\n",
           response);

    return 0;
}


/*
 * ============================================================
 * UDP MONITOR THREAD
 * ============================================================
 *
 * Receives periodic system-stat datagrams from the Agent.
 */
void *monitor_thread(void *arg)
{
    (void)arg;

    char buffer[BUFFER_SIZE];

    struct sockaddr_in sender_address;

    socklen_t sender_length =
        sizeof(sender_address);


    printf("UDP monitoring receiver started.\n");


    while (monitor.running)
    {
        ssize_t received =
            recvfrom(monitor.udp_socket,
                     buffer,
                     sizeof(buffer) - 1,
                     0,
                     (struct sockaddr *)&sender_address,
                     &sender_length);


        if (received < 0)
        {
            /*
             * Timeout allows us to check monitor.running.
             */
            if (errno == EAGAIN ||
                errno == EWOULDBLOCK)
            {
                continue;
            }

            /*
             * Socket was closed or another error occurred.
             */
            if (!monitor.running)
            {
                break;
            }

            perror("recvfrom");

            continue;
        }


        buffer[received] = '\0';


        printf("\nUDP Monitor: %s\n",
               buffer);

        printf("RemoteOps> ");

        fflush(stdout);
    }


    printf("\nUDP monitoring receiver stopped.\n");

    return NULL;
}


/*
 * ============================================================
 * START UDP MONITOR
 * ============================================================
 */
int start_monitor(int tcp_socket,
                  int udp_port)
{
    /*
     * Create UDP socket.
     */
    monitor.udp_socket =
        socket(AF_INET,
               SOCK_DGRAM,
               0);


    if (monitor.udp_socket < 0)
    {
        perror("UDP socket");

        return -1;
    }


    /*
     * Allow socket reuse.
     */
    int option = 1;

    setsockopt(monitor.udp_socket,
               SOL_SOCKET,
               SO_REUSEADDR,
               &option,
               sizeof(option));


    /*
     * Bind to the requested UDP port.
     */
    struct sockaddr_in local_address;

    memset(&local_address,
           0,
           sizeof(local_address));


    local_address.sin_family =
        AF_INET;

    local_address.sin_addr.s_addr =
        INADDR_ANY;

    local_address.sin_port =
        htons(udp_port);


    if (bind(monitor.udp_socket,
             (struct sockaddr *)&local_address,
             sizeof(local_address)) < 0)
    {
        perror("UDP bind");

        close(monitor.udp_socket);

        return -1;
    }


    /*
     * Set a receive timeout.
     *
     * This prevents the monitor thread from blocking forever
     * when MONITOR STOP is requested.
     */
    struct timeval timeout;

    timeout.tv_sec = 1;

    timeout.tv_usec = 0;


    setsockopt(monitor.udp_socket,
               SOL_SOCKET,
               SO_RCVTIMEO,
               &timeout,
               sizeof(timeout));


    /*
     * Tell Agent to start sending UDP datagrams.
     */
    char command[LINE_SIZE];


    snprintf(command,
             sizeof(command),
             "MONITOR START %d\n",
             udp_port);


    if (send_all(tcp_socket,
                 command,
                 strlen(command)) != 0)
    {
        perror("send");

        close(monitor.udp_socket);

        return -1;
    }


    /*
     * Wait for:
     *
     * OK MONITOR_STARTED SID:4540
     */
    if (receive_response(tcp_socket) != 0)
    {
        close(monitor.udp_socket);

        return -1;
    }


    /*
     * Start UDP receiving thread.
     */
    monitor.running = 1;


    if (pthread_create(&monitor_thread_id,
                       NULL,
                       monitor_thread,
                       NULL) != 0)
    {
        perror("pthread_create");

        monitor.running = 0;

        close(monitor.udp_socket);

        return -1;
    }


    return 0;
}


/*
 * ============================================================
 * STOP UDP MONITOR
 * ============================================================
 */
int stop_monitor(int tcp_socket)
{
    /*
     * Tell Agent to stop sending.
     */
    const char *command =
        "MONITOR STOP\n";


    if (send_all(tcp_socket,
                 command,
                 strlen(command)) != 0)
    {
        perror("send");

        return -1;
    }


    /*
     * Wait for:
     *
     * OK MONITOR_STOPPED SID:4540
     */
    receive_response(tcp_socket);


    /*
     * Stop local receiver.
     */
    monitor.running = 0;


    /*
     * Close UDP socket.
     *
     * The timeout on recvfrom() allows the thread to exit.
     */
    close(monitor.udp_socket);


    /*
     * Wait for receiver thread.
     */
    pthread_join(monitor_thread_id,
                 NULL);


    return 0;
}


/*
 * ============================================================
 * PUT FILE
 * ============================================================
 *
 * User command:
 *
 * PUT <localfile> [remotename]
 *
 * Example:
 *
 * PUT test.txt
 *
 * The remote file will also be called test.txt.
 *
 * Or:
 *
 * PUT test.txt hello.txt
 *
 * Local file:
 *
 * test.txt
 *
 * Agent file:
 *
 * hello.txt
 */
int put_file(int socket,
             const char *local_filename,
             const char *remote_filename)
{
    /*
     * Open local file.
     */
    FILE *file =
        fopen(local_filename,
              "rb");


    if (file == NULL)
    {
        perror("Cannot open local file");

        return -1;
    }


    /*
     * Find file size.
     */
    if (fseek(file,
              0,
              SEEK_END) != 0)
    {
        fclose(file);

        return -1;
    }


    long file_size_long =
        ftell(file);


    if (file_size_long < 0)
    {
        fclose(file);

        return -1;
    }


    rewind(file);


    unsigned long long file_size =
        (unsigned long long)file_size_long;


    /*
     * Build PUT protocol header.
     *
     * IMPORTANT:
     *
     * The header ends with \n.
     * File bytes immediately follow it.
     */
    char header[LINE_SIZE];


    snprintf(header,
             sizeof(header),
             "PUT %s %llu\n",
             remote_filename,
             file_size);


    /*
     * Send PUT header.
     */
    if (send_all(socket,
                 header,
                 strlen(header)) != 0)
    {
        perror("send");

        fclose(file);

        return -1;
    }


    /*
     * Send file bytes.
     */
    char buffer[BUFFER_SIZE];

    size_t bytes_read;


    while ((bytes_read =
                fread(buffer,
                      1,
                      sizeof(buffer),
                      file)) > 0)
    {
        if (send_all(socket,
                     buffer,
                     bytes_read) != 0)
        {
            perror("send");

            fclose(file);

            return -1;
        }
    }


    fclose(file);


    /*
     * Wait for Agent response.
     */
    printf("PUT sent: %s (%llu bytes)\n",
           remote_filename,
           file_size);


    return receive_response(socket);
}


/*
 * ============================================================
 * GET FILE
 * ============================================================
 *
 * User command:
 *
 * GET <remotefile> [localfile]
 *
 * Example:
 *
 * GET test.txt
 *
 * Saves as:
 *
 * test.txt
 *
 * Or:
 *
 * GET test.txt downloaded.txt
 */
int get_file(int socket,
             const char *remote_filename,
             const char *local_filename)
{
    /*
     * Send GET command.
     */
    char command[LINE_SIZE];


    snprintf(command,
             sizeof(command),
             "GET %s\n",
             remote_filename);


    if (send_all(socket,
                 command,
                 strlen(command)) != 0)
    {
        perror("send");

        return -1;
    }


    /*
     * Receive:
     *
     * OK FILE_SEND filename filesize SID:4540
     *
     * OR:
     *
     * ERR 005 FILE_NOT_FOUND SID:4540
     */
    char response[BUFFER_SIZE];


    int result =
        receive_line(socket,
                     response,
                     sizeof(response));


    if (result <= 0)
    {
        printf("Connection closed.\n");

        return -1;
    }


    printf("Agent: %s\n",
           response);


    /*
     * Check whether this is a FILE_SEND response.
     */
    if (strncmp(response,
                "OK FILE_SEND ",
                13) != 0)
    {
        /*
         * It is an error response.
         */
        return 0;
    }


    /*
     * Parse:
     *
     * OK FILE_SEND filename filesize SID:4540
     */
    char returned_filename[256];

    unsigned long long file_size;

    char returned_sid[100];


    if (sscanf(response,
               "OK FILE_SEND %255s %llu SID:%99s",
               returned_filename,
               &file_size,
               returned_sid) != 3)
    {
        printf("Invalid FILE_SEND response.\n");

        return -1;
    }


    /*
     * Open local destination file.
     */
    FILE *file =
        fopen(local_filename,
              "wb");


    if (file == NULL)
    {
        perror("Cannot create local file");

        return -1;
    }


    /*
     * Receive exactly file_size bytes.
     */
    char buffer[BUFFER_SIZE];

    unsigned long long remaining =
        file_size;


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


        /*
         * Receive this exact portion.
         */
        size_t received_total = 0;


        while (received_total < chunk_size)
        {
            ssize_t received =
                recv(socket,
                     buffer + received_total,
                     chunk_size - received_total,
                     0);


            if (received <= 0)
            {
                printf("File transfer interrupted.\n");

                fclose(file);

                return -1;
            }


            received_total +=
                (size_t)received;
        }


        /*
         * Write received bytes.
         */
        if (fwrite(buffer,
                   1,
                   received_total,
                   file) != received_total)
        {
            perror("File write");

            fclose(file);

            return -1;
        }


        remaining -=
            received_total;
    }


    fclose(file);


    printf("GET completed: %s -> %s (%llu bytes)\n",
           remote_filename,
           local_filename,
           file_size);


    return 0;
}


/*
 * ============================================================
 * PRINT HELP
 * ============================================================
 */
void print_help()
{
    printf("\n");
    printf("Available commands:\n");
    printf("\n");

    printf("AUTH <token>\n");
    printf("SYSINFO\n");
    printf("LISTPROC\n");

    printf("EXEC DATE\n");
    printf("EXEC UPTIME\n");
    printf("EXEC DISKFREE\n");
    printf("EXEC HOSTNAME\n");
    printf("EXEC WHOAMI\n");

    printf("PUT <localfile> [remotefile]\n");
    printf("GET <remotefile> [localfile]\n");

    printf("MONITOR START <udp_port>\n");
    printf("MONITOR STOP\n");

    printf("QUIT\n");

    printf("HELP\n");

    printf("\n");
}


/*
 * ============================================================
 * MAIN
 * ============================================================
 */
int main(int argc,
         char *argv[])
{
    /*
     * Server IP.
     *
     * If user provides an IP:
     *
     * ./controller_454 192.168.1.10
     *
     * otherwise localhost is used.
     */
    const char *server_ip =
        SERVER_IP_DEFAULT;


    if (argc >= 2)
    {
        server_ip = argv[1];
    }


    /*
     * ========================================================
     * CREATE TCP SOCKET
     * ========================================================
     */
    int socket_fd =
        socket(AF_INET,
               SOCK_STREAM,
               0);


    if (socket_fd < 0)
    {
        perror("socket");

        return 1;
    }


    /*
     * ========================================================
     * SERVER ADDRESS
     * ========================================================
     */
    struct sockaddr_in server_address;


    memset(&server_address,
           0,
           sizeof(server_address));


    server_address.sin_family =
        AF_INET;


    server_address.sin_port =
        htons(SERVER_PORT);


    if (inet_pton(AF_INET,
                  server_ip,
                  &server_address.sin_addr) <= 0)
    {
        printf("Invalid server IP: %s\n",
               server_ip);

        close(socket_fd);

        return 1;
    }


    /*
     * ========================================================
     * CONNECT
     * ========================================================
     */
    printf("Connecting to %s:%d...\n",
           server_ip,
           SERVER_PORT);


    if (connect(socket_fd,
                (struct sockaddr *)&server_address,
                sizeof(server_address)) < 0)
    {
        perror("connect");

        close(socket_fd);

        return 1;
    }


    printf("Connected to RemoteOps Agent.\n");


    /*
     * Monitor initially disabled.
     */
    monitor.running = 0;

    monitor.udp_socket = -1;


    /*
     * Command loop.
     */
    char line[LINE_SIZE];


    while (1)
    {
        printf("\nRemoteOps> ");

        fflush(stdout);


        if (fgets(line,
                  sizeof(line),
                  stdin) == NULL)
        {
            break;
        }


        /*
         * Remove newline.
         */
        line[strcspn(line,
                     "\r\n")] = '\0';


        /*
         * Ignore empty command.
         */
        if (strlen(line) == 0)
        {
            continue;
        }


        /*
         * ====================================================
         * HELP
         * ====================================================
         */
        if (strcmp(line,
                   "HELP") == 0)
        {
            print_help();

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
            char local_filename[256];

            char remote_filename[256];


            int count =
                sscanf(line,
                       "PUT %255s %255s",
                       local_filename,
                       remote_filename);


            if (count == 1)
            {
                /*
                 * Same local and remote name.
                 */
                strcpy(remote_filename,
                       local_filename);
            }

            else if (count != 2)
            {
                printf("Usage: PUT <localfile> [remotefile]\n");

                continue;
            }


            put_file(socket_fd,
                     local_filename,
                     remote_filename);


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
            char remote_filename[256];

            char local_filename[256];


            int count =
                sscanf(line,
                       "GET %255s %255s",
                       remote_filename,
                       local_filename);


            if (count == 1)
            {
                /*
                 * Save using remote filename.
                 */
                strcpy(local_filename,
                       remote_filename);
            }

            else if (count != 2)
            {
                printf("Usage: GET <remotefile> [localfile]\n");

                continue;
            }


            get_file(socket_fd,
                     remote_filename,
                     local_filename);


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
                printf("Usage: MONITOR START <udp_port>\n");

                continue;
            }


            if (monitor.running)
            {
                printf("Monitoring is already running.\n");

                continue;
            }


            if (start_monitor(socket_fd,
                              udp_port) != 0)
            {
                printf("Failed to start monitoring.\n");
            }


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
                /*
                 * Still send command to Agent so that the
                 * protocol is exercised.
                 */
                const char *command =
                    "MONITOR STOP\n";


                send_all(socket_fd,
                         command,
                         strlen(command));


                receive_response(socket_fd);

                continue;
            }


            stop_monitor(socket_fd);

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
            const char *command =
                "QUIT\n";


            if (send_all(socket_fd,
                         command,
                         strlen(command)) != 0)
            {
                perror("send");

                break;
            }


            receive_response(socket_fd);


            break;
        }


        /*
         * ====================================================
         * NORMAL TCP COMMAND
         * ====================================================
         *
         * AUTH
         * SYSINFO
         * LISTPROC
         * EXEC ...
         */
        char command[LINE_SIZE];


        snprintf(command,
                 sizeof(command),
                 "%.1022s\n",
                 line);


        if (send_all(socket_fd,
                     command,
                     strlen(command)) != 0)
        {
            perror("send");

            break;
        }


        if (receive_response(socket_fd) != 0)
        {
            break;
        }
    }


    /*
     * If monitoring is still running, stop local receiver.
     */
    if (monitor.running)
    {
        monitor.running = 0;

        close(monitor.udp_socket);

        pthread_join(monitor_thread_id,
                     NULL);
    }


    /*
     * Close TCP connection.
     */
    close(socket_fd);


    printf("Controller closed.\n");


    return 0;
}
