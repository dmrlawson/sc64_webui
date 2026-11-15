#include <stdarg.h>
#include <stdio.h>

#include "platform.h"

#define LOG_SIZE 4096

static char ring[LOG_SIZE];
static size_t head;   // next write position
static size_t used;

void log_printf(const char *fmt, ...) {
    char line[200];
    int n = snprintf(line, sizeof(line), "[%7lu.%03lu] ", (unsigned long)(plat_millis() / 1000),
                     (unsigned long)(plat_millis() % 1000));
    va_list ap;
    va_start(ap, fmt);
    n += vsnprintf(line + n, sizeof(line) - (size_t)n - 1, fmt, ap);
    va_end(ap);
    if (n > (int)sizeof(line) - 2) {
        n = (int)sizeof(line) - 2;
    }
    line[n++] = '\n';
    fwrite(line, 1, (size_t)n, stdout);
    fflush(stdout);
    for (int i = 0; i < n; i++) {
        ring[head] = line[i];
        head = (head + 1) % LOG_SIZE;
        if (used < LOG_SIZE) {
            used++;
        }
    }
}

size_t log_snapshot(char *out, size_t cap) {
    size_t n = used < cap ? used : cap;
    size_t start = (head + LOG_SIZE - n) % LOG_SIZE;
    for (size_t i = 0; i < n; i++) {
        out[i] = ring[(start + i) % LOG_SIZE];
    }
    return n;
}
