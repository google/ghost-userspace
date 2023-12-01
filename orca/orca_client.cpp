#include <arpa/inet.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include <iostream>
#include <sstream>
#include <string>

#include "panic.h"
#include "protocol.h"

void send_message(int port, const char *buf, size_t len) {
    const char *hostname = "localhost";

    int sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);

    struct sockaddr_in addr;
    addr.sin_family = AF_INET;

    // resolve host
    struct hostent *host = gethostbyname(hostname);
    if (host == NULL) {
        panic("gethostbyname");
    }
    memcpy(&addr.sin_addr, host->h_addr_list[0], host->h_length);
    addr.sin_port = htons(port);

    if (connect(sockfd, (struct sockaddr *)&addr, sizeof(addr)) == -1) {
        panic("connect");
    }

    ssize_t sent = 0;
    do {
        const ssize_t n = send(sockfd, buf + sent, len - sent, 0);
        if (n == -1) {
            panic("send");
        }
        sent += n;
    } while (sent < len);

    close(sockfd);
}

void print_usage() {
    printf("Orca client usage:\n");
    printf("setsched <dfifo|cfifo> <preemption_interval_us=0>\n");
    printf("Press <C-d> to quit.\n");
    std::cout << std::flush;
}

void handle_input(int port, const std::string &input) {
    std::istringstream iss(input);

    std::string cmd;
    iss >> cmd;

    if (cmd == "setsched") {
        std::string sched_type;
        int preemption_interval_us = 0;
        iss >> sched_type;
        if (!(iss >> preemption_interval_us)) {
            preemption_interval_us = 0;
        }

        orca::SchedulerConfig config;

        if (sched_type[0] == 'd') {
            config.type = orca::SchedulerConfig::SchedulerType::dFCFS;
        } else if (sched_type[0] == 'c') {
            config.type = orca::SchedulerConfig::SchedulerType::cFCFS;
        } else {
            panic("unrecognized scheduler type");
        }

        if (preemption_interval_us != 0) {
            config.preemption_interval_us = preemption_interval_us;
        }

        orca::OrcaSetScheduler msg;
        msg.config = config;

        char buf[orca::MAX_MESSAGE_SIZE];
        memcpy(buf, &msg, sizeof(msg));
        send_message(port, buf, sizeof(msg));
    } else {
        printf("Invalid command: %s\n", input.c_str());
        print_usage();
    }
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        printf("Usage: %s <port>\n", argv[0]);
        return 0;
    }
    int port = atoi(argv[1]);

    if (argc > 2) {
        std::ostringstream oss;
        for (int i = 2; i < argc; ++i) {
            oss << argv[i] << " ";
        }
        handle_input(port, oss.str());
        return 0;
    }

    print_usage();

    std::string input;
    while (std::getline(std::cin, input)) {
        handle_input(port, input);
    }
}
