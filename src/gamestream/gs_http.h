// Minimal HTTP/HTTPS client over mbedTLS for the GameStream/Sunshine API.
// One connection per request (Connection: close), GET only.
#pragma once

#include <stddef.h>
#include "certgen.h"

typedef struct {
    char *body;
    size_t len;
} http_resp_t;

// Identity is used as the client certificate for HTTPS.
void http_init(client_identity_t *id, int verbose);

// Per-request read timeout (ms). Default 7000; pairing needs ~120s.
void http_set_timeout_ms(unsigned ms);

int http_get(const char *host, unsigned short port, int use_tls,
             const char *path, http_resp_t *resp);

void http_resp_free(http_resp_t *resp);
