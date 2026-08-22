#ifndef AIRPRINT_HTTP_H
#define AIRPRINT_HTTP_H

#include <stddef.h>
#include <stdint.h>

int ap_http_open(void);
void ap_http_close(void);

int ap_http_post_ipp(
    const char *host,
    uint16_t port,
    const char *path,
    const uint8_t *request_body,
    size_t request_body_len,
    uint8_t **response_body,
    size_t *response_body_len,
    int *http_status);

void ap_http_free(void *memory);
const char *ap_http_last_error(void);

#endif
