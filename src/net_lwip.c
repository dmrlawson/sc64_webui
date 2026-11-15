// lwIP raw API implementation of net.h (polling mode, main loop only).

#include <string.h>

#include "lwip/pbuf.h"
#include "lwip/tcp.h"

#include "net.h"
#include "platform.h"

#define MAX_CONNS 8
#define IDLE_TIMEOUT_MS 15000

struct net_conn {
    struct tcp_pcb *pcb;
    struct pbuf *rx;      // received, not yet consumed data
    size_t rx_offset;     // consumed bytes of the first pbuf
    bool used, serving, eof;
    uint32_t since;
};

static struct net_conn conns[MAX_CONNS];

static void conn_free(struct net_conn *c) {
    if (c->pcb) {
        tcp_arg(c->pcb, NULL);
        tcp_recv(c->pcb, NULL);
        tcp_err(c->pcb, NULL);
        if (c->rx) {
            // Re-open the window for what we never read, otherwise lwIP
            // answers the close with a reset and the response is lost.
            tcp_recved(c->pcb, (u16_t)(c->rx->tot_len - c->rx_offset));
        }
        if (tcp_close(c->pcb) != ERR_OK) {
            tcp_abort(c->pcb);
        }
        c->pcb = NULL;
    }
    if (c->rx) {
        pbuf_free(c->rx);
        c->rx = NULL;
    }
    c->used = false;
}

static err_t on_recv(void *arg, struct tcp_pcb *pcb, struct pbuf *p, err_t err) {
    struct net_conn *c = arg;
    (void)pcb;
    if (!p || err != ERR_OK) {
        if (p) {
            pbuf_free(p);
        }
        c->eof = true;
        return ERR_OK;
    }
    // Data is acknowledged to the peer (tcp_recved) only once consumed, so
    // the TCP window provides the back-pressure for uploads.
    if (c->rx) {
        pbuf_cat(c->rx, p);
    } else {
        c->rx = p;
        c->rx_offset = 0;
    }
    return ERR_OK;
}

static void on_err(void *arg, err_t err) {
    struct net_conn *c = arg;
    (void)err;
    c->pcb = NULL;   // already freed by lwIP
    c->eof = true;
}

static err_t on_accept(void *arg, struct tcp_pcb *pcb, err_t err) {
    (void)arg;
    if (err != ERR_OK || !pcb) {
        return ERR_VAL;
    }
    for (int i = 0; i < MAX_CONNS; i++) {
        struct net_conn *c = &conns[i];
        if (!c->used) {
            memset(c, 0, sizeof(*c));
            c->used = true;
            c->pcb = pcb;
            c->since = plat_millis();
            tcp_arg(pcb, c);
            tcp_recv(pcb, on_recv);
            tcp_err(pcb, on_err);
            tcp_nagle_disable(pcb);
            return ERR_OK;
        }
    }
    tcp_abort(pcb);
    return ERR_ABRT;
}

bool net_listen(uint16_t port) {
    struct tcp_pcb *pcb = tcp_new_ip_type(IPADDR_TYPE_ANY);
    if (!pcb || tcp_bind(pcb, IP_ANY_TYPE, port) != ERR_OK) {
        return false;
    }
    pcb = tcp_listen_with_backlog(pcb, MAX_CONNS);
    if (!pcb) {
        return false;
    }
    tcp_accept(pcb, on_accept);
    return true;
}

net_conn_t *net_next_ready(void) {
    static int next;
    for (int i = 0; i < MAX_CONNS; i++) {
        struct net_conn *c = &conns[(next + i) % MAX_CONNS];
        if (!c->used || c->serving) {
            continue;
        }
        if (c->rx && c->pcb) {
            next = (next + i + 1) % MAX_CONNS;
            c->serving = true;
            return c;
        }
        // Reset connections are dropped even with a request still queued.
        if (c->eof || !c->pcb || (plat_millis() - c->since) > IDLE_TIMEOUT_MS) {
            conn_free(c);
        }
    }
    return NULL;
}

int net_recv(net_conn_t *c, uint8_t *buf, size_t max) {
    size_t copied = 0;
    while (c->rx && copied < max) {
        struct pbuf *head = c->rx;
        size_t n = head->len - c->rx_offset;
        if (n > (max - copied)) {
            n = max - copied;
        }
        memcpy(buf + copied, (const uint8_t *)head->payload + c->rx_offset, n);
        copied += n;
        c->rx_offset += n;
        if (c->rx_offset == head->len) {
            c->rx = head->next;
            if (c->rx) {
                pbuf_ref(c->rx);
            }
            pbuf_free(head);
            c->rx_offset = 0;
        }
    }
    if (copied > 0) {
        if (c->pcb) {
            tcp_recved(c->pcb, (u16_t)copied);
        }
        return (int)copied;
    }
    return (c->eof || !c->pcb) ? -1 : 0;
}

int net_send(net_conn_t *c, const uint8_t *buf, size_t len) {
    if (!c->pcb) {
        return -1;
    }
    size_t room = tcp_sndbuf(c->pcb);
    if (len > room) {
        len = room;
    }
    if (len == 0) {
        return 0;
    }
    err_t err = tcp_write(c->pcb, buf, (u16_t)len, TCP_WRITE_FLAG_COPY);
    if (err == ERR_MEM) {
        return 0;   // out of segments for now, retry after some ACKs
    }
    if (err != ERR_OK) {
        return -1;
    }
    tcp_output(c->pcb);
    return (int)len;
}

void net_close(net_conn_t *c) {
    conn_free(c);
}
