#ifndef HTTP_H
#define HTTP_H

#include <stdbool.h>
#include <stdint.h>

bool http_init(uint16_t port);
// Serve at most one pending request. Call from the main loop only.
void http_task(void);

#endif
