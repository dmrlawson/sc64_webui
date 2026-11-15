// Minimal non-blocking TCP server interface. Implemented on lwIP for the Pico
// (net_lwip.c) and on BSD sockets for the host simulator (host/net_posix.c).
#ifndef NET_H
#define NET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct net_conn net_conn_t;

bool net_listen(uint16_t port);
// Returns a connection that has received data and is not yet being served,
// or NULL. Also reaps connections that were closed or idle for too long.
net_conn_t *net_next_ready(void);
// >0: bytes copied, 0: nothing available right now, -1: closed by peer/error.
int net_recv(net_conn_t *conn, uint8_t *buf, size_t max);
// >=0: bytes accepted (may be fewer than len), -1: connection is gone.
int net_send(net_conn_t *conn, const uint8_t *buf, size_t len);
// Queued data is still delivered before the connection closes.
void net_close(net_conn_t *conn);

#endif
