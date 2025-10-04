#include <stdio.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>

#include "debug.h"

#define MAX_CLIENTS 128

#include <stdlib.h>
#include <sys/errno.h>

#include "fifo.h"
#include "rr.h"
#include "sjf.h"
#include "mlfq.h"
#include "msg.h"
#include "queue.h"

static uint32_t PID = 0;



/**
 * @brief Set up the server socket for the scheduler.
 *
 * This function creates a UNIX domain socket, binds it to a specified path,
 * and sets it to listen for incoming connections. It also sets the socket to
 * non-blocking mode.
 *
 * @param socket_path The path where the socket will be created
 * @return int Returns the server file descriptor on success, or -1 on failure
 */
int setup_server_socket(const char *socket_path) {
    int server_fd;
    struct sockaddr_un addr;

    // Clean up old socket file
    unlink(socket_path);

    // Create UNIX socket
    if ((server_fd = socket(AF_UNIX, SOCK_STREAM, 0)) < 0) {
        perror("socket");
        return -1;
    }

    memset(&addr, 0, sizeof(struct sockaddr_un));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path) - 1);

    // Bind
    if (bind(server_fd, (struct sockaddr *) &addr, sizeof(struct sockaddr_un)) < 0) {
        perror("bind");
        close(server_fd);
        return -1;
    }

    // Listen
    if (listen(server_fd, MAX_CLIENTS) < 0) {
        perror("listen");
        close(server_fd);
        return -1;
    }
    // Set the socket to non-blocking mode
    int flags = fcntl(server_fd, F_GETFL, 0); // Get current flags
    if (flags != -1) {
        if (fcntl(server_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
            perror("fcntl: set non-blocking");
        }
    }
    return server_fd;
}

/**
 * @brief Check for new client connections and add them to the queue.
 *
 * This function accepts new client connections on the server socket,
 * sets the client sockets to non-blocking mode, and enqueues them
 * into the provided queue.
 *
 * @param command_queue The queue to which new pcb will be added
 * @param server_fd The server socket file descriptor
 */
void check_new_commands(queue_t *command_queue, queue_t *blocked_queue, queue_t *ready_queue, int server_fd, uint32_t current_time_ms) {
    // Accept new client connections
    int client_fd;
    do {
        client_fd = accept(server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EMFILE || errno == ENFILE) {
                perror("accept: too many fds");
                break;
            }
            if (errno == EINTR)        continue;   // interrupted -> retry
            if (errno == ECONNABORTED) continue;   // aborted handshake -> next
            if ((errno != EAGAIN) && (errno != EWOULDBLOCK)) {
                perror("accept");
            }
            // No more clients to accept right now
            break;
        }
        int flags = fcntl(client_fd, F_GETFL, 0); // Get current flags
        if (flags != -1) {
            if (fcntl(client_fd, F_SETFL, flags | O_NONBLOCK) == -1) {
                perror("fcntl: set non-blocking");
            }
        }
        // Set close-on-exec flag
        int fdflags = fcntl(client_fd, F_GETFD, 0);
        if (fdflags != -1) {
            fcntl(client_fd, F_SETFD, fdflags | FD_CLOEXEC);
        }
        DBG("[Scheduler] New client connected: fd=%d\n", client_fd);
        // New PCBs do not have a time yet, will be set when we receive a RUN message
        pcb_t *pcb = new_pcb(++PID, client_fd, 0);
        enqueue_pcb(command_queue, pcb);
    } while (client_fd > 0);

    // Check queue for new commands in the command queue
    queue_elem_t * elem = command_queue->head;
    while (elem != NULL) {
        pcb_t *current_pcb = elem->pcb;
        msg_t msg;
        int n = read(current_pcb->sockfd, &msg, sizeof(msg_t));
        if (n <= 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                // No data available right now, move to next
                elem = elem->next;
            } else {
                if (n < 0) {
                    perror("read");
                } else {
                    DBG("Connection closed by remote host\n");
                }
                // Remove from queue
                queue_elem_t *tmp = elem;
                elem = elem->next;
                free(current_pcb);
                free(tmp);
            }
            continue;
        }
        // We have received a message
        if (msg.request == PROCESS_REQUEST_RUN) {
            current_pcb->pid = msg.pid; // Set the pid from the message
            current_pcb->time_ms = msg.time_ms;
            current_pcb->ellapsed_time_ms = 0;
            current_pcb->status = TASK_RUNNING;
            current_pcb->last_update_time_ms = current_time_ms;
            enqueue_pcb(ready_queue, current_pcb);
            DBG("Process %d requested RUN for %d ms\n", current_pcb->pid, current_pcb->time_ms);
        } else if (msg.request == PROCESS_REQUEST_BLOCK) {
            current_pcb->pid = msg.pid; // Set the pid from the message
            current_pcb->time_ms = msg.time_ms;
            current_pcb->status = TASK_BLOCKED;
            current_pcb->last_update_time_ms = current_time_ms;
            enqueue_pcb(blocked_queue, current_pcb);
            DBG("Process %d requested BLOCK for %d ms\n", current_pcb->pid);
        } else {
            printf("Unexpected message received from client\n");
            continue;
        }
        // Remove from command queue
        remove_queue_elem(command_queue, elem);
        queue_elem_t *tmp = elem;
        elem = elem->next;
        free(tmp);

        // Send ack message
        msg_t ack_msg = {
            .pid = current_pcb->pid,
            .request = PROCESS_REQUEST_ACK,
            .time_ms = current_time_ms
        };
        if (write(current_pcb->sockfd, &ack_msg, sizeof(msg_t)) != sizeof(msg_t)) {
            perror("write");
        }
        DBG("Send ACK message to process %d with time %d\n", current_pcb->pid, current_time_ms);
    }

}

/**
 * @brief Check the blocked queue for messages from clients.
 *
 * This function iterates through the blocked queue, checking each client
 * socket for incoming messages. If a RUN message is received, the corresponding
 * pcb is moved to the command queue and an ACK message is sent back to the client.
 * If a client disconnects or an error occurs, the client is removed from the blocked queue.
 *
 * @param blocked_queue The queue containing PCBs in I/O wait stated (blocked) from CPU
 * @param command_queue The queue where PCBs ready for new instructions will be moved
 * @param current_time_ms The current time in milliseconds
 */
void check_blocked_queue(queue_t *blocked_queue, queue_t *command_queue, uint32_t current_time_ms) {
    // Garante que só processamos 1x por tick global (TICKS_MS)
    static uint32_t last_tick_processed = (uint32_t)-1;
    if (last_tick_processed == current_time_ms) {
        return; // já processámos este tick
    }
    last_tick_processed = current_time_ms;

    queue_elem_t *elem = blocked_queue->head;
    while (elem != NULL) {
        pcb_t *pcb = elem->pcb;

        // Desconta exatamente 1 tick de I/O, de forma determinística
        if (pcb->time_ms > 0) {
            if (pcb->time_ms > TICKS_MS) {
                pcb->time_ms -= TICKS_MS;
            } else {
                pcb->time_ms = 0;
            }
        }

        // Opcional: debug para ver o avanço
        // printf("[DBG] BLOCK pid=%d left=%u at t=%u\n", pcb->pid, pcb->time_ms, current_time_ms);

        if (pcb->time_ms == 0) {
            // Terminou o I/O: envia DONE e move de volta para a command_queue
            msg_t msg = {
                .pid     = pcb->pid,
                .request = PROCESS_REQUEST_DONE,
                .time_ms = current_time_ms
            };
            (void)!write(pcb->sockfd, &msg, sizeof(msg));
            pcb->status = TASK_COMMAND;

            // Re-enfileira no COMMAND
            enqueue_pcb(command_queue, pcb);

            // Remove da blocked_queue (liberta apenas o elemento)
            queue_elem_t *tmp = elem;
            elem = elem->next;                 // avança antes de libertar
            remove_queue_elem(blocked_queue, tmp);
            free(tmp);
            continue;
        }

        elem = elem->next;
    }
}


static const char *SCHEDULER_NAMES[] = {
    "FIFO",
    "SJF",
    "RR",
    "MLFQ",
    NULL
};

typedef enum  {
    NULL_SCHEDULER = -1,
    SCHED_FIFO = 0,
    SCHED_SJF,
    SCHED_RR,
    SCHED_MLFQ
} scheduler_en;

scheduler_en get_scheduler(const char *name) {
    char buf[32]; size_t n = 0;

    // trim left
    while (*name && isspace((unsigned char)*name)) name++;

    // copia token e faz uppercase
    while (*name && !isspace((unsigned char)*name) && n < sizeof(buf)-1) {
        buf[n++] = (char)toupper((unsigned char)*name++);
    }
    buf[n] = '\0';

    if (!strcmp(buf, "FIFO")) return SCHED_FIFO;
    if (!strcmp(buf, "SJF"))  return SCHED_SJF;
    if (!strcmp(buf, "RR"))   return SCHED_RR;
    if (!strcmp(buf, "MLFQ")) return SCHED_MLFQ;

    printf("Scheduler %s not recognized. Available options: FIFO SJF RR MLFQ\n", buf);
    return NULL_SCHEDULER;
}

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <scheduler>\nScheduler options: FIFO SJF RR MLFQ\n", argv[0]);
        exit(EXIT_FAILURE);
    }

    // Parse arguments
    scheduler_en scheduler_type = get_scheduler(argv[1]);
    if (scheduler_type == NULL_SCHEDULER) {
        return EXIT_FAILURE;
    }

    // We set up 3 queues: 1 for the simulator and 2 for scheduling
    // - COMMAND queue: for PCBs that are waiting for (new) instructions from the app
    // - READY queue: for PCBs that are ready to run on the CPU
    // - BLOCKED queue: for PCBs that are blocked waiting for I/O
    queue_t command_queue = {.head = NULL, .tail = NULL};
    queue_t ready_queue = {.head = NULL, .tail = NULL};
    queue_t blocked_queue = {.head = NULL, .tail = NULL};

    // We only have a single CPU that is a pointer to the actively running PCB on the CPU
    pcb_t *CPU = NULL;

    int server_fd = setup_server_socket(SOCKET_PATH);
    if (server_fd < 0) {
        fprintf(stderr, "Failed to set up server socket\n");
        return 1;
    }
    printf("Scheduler server listening on %s...\n", SOCKET_PATH);
    uint32_t current_time_ms = 0;

    while (1) {
        // Check for new connections and/or instructions
        check_new_commands(&command_queue, &blocked_queue, &ready_queue, server_fd, current_time_ms);

        if (current_time_ms%1000 == 0) {
            printf("Current time: %d s\n", current_time_ms/1000);
        }
        // Check the status of the PCBs in the blocked queue
        check_blocked_queue(&blocked_queue, &command_queue, current_time_ms);
        // Tasks from the blocked queue could be moved to the command queue, check again
        usleep(TICKS_MS * 1000/2);
        check_new_commands(&command_queue, &blocked_queue, &ready_queue, server_fd, current_time_ms);


        // The scheduler handles the READY queue
        switch (scheduler_type) {
            case SCHED_FIFO:
                fifo_scheduler(current_time_ms, &ready_queue, &CPU);
                break;
            case SCHED_SJF:
                sjf_scheduler(current_time_ms, &ready_queue, &CPU);
                break;
            case SCHED_RR:
                rr_scheduler(current_time_ms, &ready_queue, &CPU);
                break;
            case SCHED_MLFQ:
                mlfq_scheduler(current_time_ms, &ready_queue, &CPU);
                break;
            default:
                printf("Unknown scheduler type\n");
                break;
        }

        // Simulate a tick
        usleep(TICKS_MS * 1000/2);
        current_time_ms += TICKS_MS;
    }

    // Unreachable, because of the infinite loop
    return 0;
}
