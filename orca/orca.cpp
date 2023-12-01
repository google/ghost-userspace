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

#include "orca.h"
#include "panic.h"
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

    printf("Orca listening on port %d...\n", port);
    while (true) {
        int connfd = accept(sockfd, NULL, NULL);
        if (connfd == -1) {
            printf("accept returned -1\n");
            continue;
        }

        char buf[orca::MAX_MESSAGE_SIZE];
        memset(buf, 0, sizeof(buf));

        ssize_t recvd = 0;
        ssize_t rval;
        do {
            rval = recv(connfd, buf + recvd, orca::MAX_MESSAGE_SIZE - recvd, 0);
            if (rval == -1) {
                panic("recv");
            }
            recvd += rval;
        } while (rval > 0);

        auto *header = (orca::OrcaHeader *)buf;
        switch (header->type) {
        case orca::MessageType::SetScheduler: {
            auto *msg = (orca::OrcaSetScheduler *)buf;
            printf(
                "Received SetScheduler. type=%d, preemption_interval_us=%d\n",
                (int)msg->config.type, msg->config.preemption_interval_us);
            orca_agent->set_scheduler(msg->config);
            break;
        }
        default:
            panic("unimplemented message type");
        }

        close(connfd);
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