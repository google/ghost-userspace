#pragma once

#include <stdio.h>
#include <stdlib.h>

void panic(const char *s) {
    perror(s);
    exit(EXIT_FAILURE);
}
