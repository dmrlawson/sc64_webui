// BSD sockets implementation of net.h for the host simulator.

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <sys/socket.h>
#include <unistd.h>

#include "net.h"
#include "platform.h"

#define MAX_CONNS 8
#define IDLE_TIMEOUT_MS 15000
#define LINGER_MS 2000

struct net_conn {
    int fd;
    bool used, serving, closing;
    uint32_t since;
};

static int listen_fd = -1;
static struct net_conn conns[MAX_CONNS];

bool net_listen(uint16_t port) {
    listen_fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = htonl(INADDR_ANY)};
    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(listen_fd, 8) != 0) {
        perror("listen");
        return false;
    }
    fcntl(listen_fd, F_SETFL, O_NONBLOCK);
    return true;
}

static void drop(struct net_conn *c) {
    close(c->fd);
    c->used = false;
}

net_conn_t *net_next_ready(void) {
    for (int i = 0; i < MAX_CONNS; i++) {
        if (conns[i].used) {
            continue;
        }
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            break;
        }
        fcntl(fd, F_SETFL, O_NONBLOCK);
        int one = 1;
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        conns[i] = (struct net_conn){.fd = fd, .used = true, .since = plat_millis()};
    }
    for (int i = 0; i < MAX_CONNS; i++) {
        struct net_conn *c = &conns[i];
        if (!c->used || c->serving) {
            continue;
        }
        uint8_t scratch[4096];
        if (c->closing) {
            // Linger: discard whatever the peer still sends until it closes.
            ssize_t n = recv(c->fd, scratch, sizeof(scratch), 0);
            if (n == 0 || (n < 0 && errno != EAGAIN) || (plat_millis() - c->since) > LINGER_MS) {
                drop(c);
            }
            continue;
        }
        ssize_t n = recv(c->fd, scratch, 1, MSG_PEEK);
        if (n > 0) {
            c->serving = true;
            return c;
        }
        if (n == 0 || errno != EAGAIN || (plat_millis() - c->since) > IDLE_TIMEOUT_MS) {
            drop(c);
        }
    }
    return NULL;
}

int net_recv(net_conn_t *c, uint8_t *buf, size_t max) {
    ssize_t n = recv(c->fd, buf, max, 0);
    if (n > 0) {
        return (int)n;
    }
    return (n < 0 && errno == EAGAIN) ? 0 : -1;
}

int net_send(net_conn_t *c, const uint8_t *buf, size_t len) {
    ssize_t n = send(c->fd, buf, len, MSG_NOSIGNAL);
    if (n >= 0) {
        return (int)n;
    }
    return (errno == EAGAIN) ? 0 : -1;
}

void net_close(net_conn_t *c) {
    shutdown(c->fd, SHUT_WR);
    c->serving = false;
    c->closing = true;
    c->since = plat_millis();
}
