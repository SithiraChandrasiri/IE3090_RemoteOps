# IE3090 RemoteOps

## Network Programming - Remote System Monitoring and Management Tool

This project is my implementation of the RemoteOps assignment for IE3090 Network Programming.
The system has two C programs:

- **Agent** - runs on the managed Ubuntu machine and accepts connections.
- **Controller** - used by an administrator to connect to the Agent and send commands.

The main control connection uses TCP, while UDP is used for periodic system monitoring.

## Student / Personalisation

- Registration number: `IT24100454`
- Agent TCP port: `9410`
- Session ID (SID): `4540`
- Authentication token: `OPS-0454`
- Agent source: `agent_454.c`
- Controller source: `controller_454.c`
- Makefile: `Makefile_454`
- Storage path: `./agentfiles/IT24100454/`
- Log file: `remoteops_IT24100454.log`

## Main Features

The Agent implements the required RemoteOps functions:

- Multiple Controller connections using pthreads
- Authentication using the personalised token
- `SYSINFO` - CPU load, memory usage and uptime
- `LISTPROC` - current process snapshot
- `EXEC DATE`
- `EXEC UPTIME`
- `EXEC DISKFREE`
- `EXEC HOSTNAME`
- `EXEC WHOAMI`
- Rejection of commands outside the EXEC whitelist
- `PUT` - upload a file to the Agent
- `GET` - download a stored file from the Agent
- `MONITOR START <udp_port>` - start UDP monitoring
- `MONITOR STOP` - stop UDP monitoring
- `QUIT` - close the session cleanly
- Timestamped activity logging

## Communication

### TCP

The Agent listens on TCP port `9410`.

TCP is used for authentication, normal commands, responses and file transfers.
Text messages are line based and terminated with a newline. File transfers use the file size to keep track of the exact number of bytes sent or received.

### UDP

UDP is used only for periodic monitoring data.
The Agent sends system information to the Controller at regular intervals. The monitoring packet includes the personalised SID:

`SYSINFO <cpu_load> <memory_used_mb> <uptime_sec> SID:4540`

The monitoring interval used in this implementation is 5 seconds.

## Building the Project

From the project directory run:

```bash
make -f Makefile_454
```

To remove the compiled programs:

```bash
make -f Makefile_454 clean
```

The final project was checked with:

```bash
make -f Makefile_454 clean && make -f Makefile_454
```

The final build completed without compiler warnings or errors.

## Running the Programs

### 1. Start the Agent

On the Ubuntu machine:

```bash
./agent_454
```

The Agent listens on TCP port `9410`.

### 2. Start the Controller

From another terminal on the same machine or another reachable machine:

```bash
./controller_454
```

Then authenticate using:

```text
AUTH OPS-0454
```

After successful authentication, the other commands can be used.

## Example Commands

```text
AUTH OPS-0454
SYSINFO
LISTPROC
EXEC DATE
EXEC UPTIME
EXEC DISKFREE
EXEC HOSTNAME
EXEC WHOAMI
MONITOR START <udp_port>
MONITOR STOP
QUIT
```

For file transfers, the Controller uses the PUT and GET commands supported by the implementation.
Uploaded files are stored under:

```text
./agentfiles/IT24100454/
```

## Logging

The Agent writes activity to:

```text
remoteops_IT24100454.log
```

The log records timestamps and important events such as Agent startup, client connections, commands, authentication results, file transfers, monitoring activity and disconnects.

## Concurrency

The Agent uses a thread-per-client model. Each accepted TCP connection is handled by a separate pthread. A separate monitoring thread is used for UDP monitoring so that normal TCP command handling can continue while monitoring packets are being sent.

## Project Files

```text
agent_454.c
controller_454.c
Makefile_454
README.md
design_diary.txt
prompt_log.txt
reflection.txt
remoteops_IT24100454.log
```

## Repository

GitHub:

https://github.com/SithiraChandrasiri/IE3090_RemoteOps

## Notes

This project was developed and tested on Ubuntu using GCC, POSIX sockets and pthreads. The implementation was tested through actual Controller-Agent communication, file transfer, UDP monitoring, error handling and logging.
