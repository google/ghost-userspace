#pragma once

#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>

void panic(const char *s) {
    perror(s);
    exit(EXIT_FAILURE);
}

void send_full(int sockfd, const char *buf, size_t len) {
    size_t sent = 0;
    do {
        ssize_t sval = send(sockfd, buf + sent, len - sent, 0);
        if (sval == -1) {
            panic("send");
        }
        sent += sval;
    } while (sent < len);
}

void recv_full(int connectionfd, char *buf, size_t len) {
    size_t recvd = 0;
    ssize_t rval;
    do {
        rval = recv(connectionfd, buf + recvd, len - recvd, 0);
        if (rval == -1) {
            panic("recv");
        }
        recvd += rval;
    } while (recvd < len);
}
