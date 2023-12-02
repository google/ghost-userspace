#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include <functional>
#include <iostream>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "event_signal.h"
#include "helpers.h"
#include "orca.h"
#include "protocol.h"

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return 0;
    }
    int port = atoi(argv[1]);

    // start TCP server for IPC messages
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        panic("socket");
    }

    int yesval = 1;
    if (setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &yesval, sizeof(yesval)) ==
        -1) {
        panic("setsockopt");
    }

    struct sockaddr_in saddr;
    memset(&saddr, 0, sizeof(saddr));
    saddr.sin_family = AF_INET;
    saddr.sin_addr.s_addr = htonl(INADDR_ANY);
    saddr.sin_port = htons(port);

    if (bind(sockfd, (struct sockaddr *)&saddr, sizeof(saddr)) == -1) {
        panic("bind");
    }

    if (listen(sockfd, 10) == -1) {
        panic("listen");
    }

    // put orca_agent ptr in static memory (so SIGINT handler can clean it up)
    static std::unique_ptr<orca::Orca> orca_agent;
    orca_agent = std::make_unique<orca::Orca>();

    signal(SIGINT, [](int signum) {
        // call Orca destructor
        orca_agent = nullptr;

        exit(signum);
    });

    EventSignal<int> sched_ready;

    printf("Orca listening on port %d...\n", port);
    while (true) {
        // get stdout of scheduler
        int schedfd = orca_agent->get_sched_stdout_fd();

        // set up fd set for select()
        fd_set readfds;
        FD_ZERO(&readfds);
        FD_SET(sockfd, &readfds);
        if (schedfd != -1) {
            FD_SET(schedfd, &readfds);
        }

        // set timeout of 1ms
        struct timeval timeout;
        timeout.tv_sec = 0;
        timeout.tv_usec = 1000;

        int ready = select(FD_SETSIZE, &readfds, NULL, NULL, &timeout);

        if (ready == -1) {
            panic("select");
        } else if (ready == 0) {
            // timeout occurred, continue loop
            continue;
        } else {
            if (FD_ISSET(sockfd, &readfds)) {
                int connfd = accept(sockfd, NULL, NULL);
                if (connfd == -1) {
                    printf("accept returned -1\n");
                    continue;
                }

                char buf[orca::MAX_MESSAGE_SIZE];
                memset(buf, 0, sizeof(buf));

                recv_full(connfd, buf, sizeof(orca::OrcaHeader));
                auto *header = (orca::OrcaHeader *)buf;

                switch (header->type) {
                case orca::MessageType::SetScheduler: {
                    recv_full(connfd, buf + sizeof(orca::OrcaHeader),
                              sizeof(orca::OrcaSetScheduler) -
                                  sizeof(orca::OrcaHeader));

                    auto *msg = (orca::OrcaSetScheduler *)buf;
                    printf("Received SetScheduler. type=%d, "
                           "preemption_interval_us=%d\n",
                           (int)msg->config.type,
                           msg->config.preemption_interval_us);

                    orca_agent->set_scheduler(msg->config);

                    EventSignal<int>::handle_t handle =
                        sched_ready.sub([connfd, handle, &sched_ready](int) {
                            printf("connfd=%d handle=%d\n", connfd, handle);

                            // send ack
                            orca::OrcaHeader ack;
                            ack.type = orca::MessageType::Ack;
                            send_full(connfd, (const char *)&ack, sizeof(ack));
                            sched_ready.unsub(handle);

                            close(connfd);
                        });

                    break;
                }
                default:
                    panic("unimplemented message type");
                }
            }
            if (schedfd != -1 && FD_ISSET(schedfd, &readfds)) {
                char buf[8192];
                memset(buf, 0, sizeof(buf));

                if (read(schedfd, buf, sizeof(buf) - 1) == -1) {
                    panic("read");
                }

                // forward scheduler's output to our stdout
                std::cout << buf << std::flush;

                if (strstr(buf, "Initialization complete, ghOSt active.")) {
                    sched_ready.fire(0);
                }

                /**
                 *  If we find a substring indicating crash:
                 *      Call orca_agent->set_scheduler()
                 */
            }
        }
    }

    /**
     * Pseudocode:
     *
     *  Start CFS in ghOst.
     *  While True:
     *      Receive data from bidirectional socket.
     *      Do some computation on data to determine best scheduler.
     *      Switch scheduler if needed.
     *
     *  While Recv(recvbuf, udpsocket):
     *      Handle data from socket.
     *      Switch scheduler if needed.
     */
    // }
}