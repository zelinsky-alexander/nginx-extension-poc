#ifndef NGX_HTTP_UPSTREAM_IDENTITY_EVENT_H
#define NGX_HTTP_UPSTREAM_IDENTITY_EVENT_H

#include <ngx_config.h>
#include <ngx_core.h>

typedef enum {
    NGX_HTTP_UPSTREAM_IDENTITY_EVENT_NONE = 0,
    NGX_HTTP_UPSTREAM_IDENTITY_EVENT_FIRST_SEEN,
    NGX_HTTP_UPSTREAM_IDENTITY_EVENT_CERTIFICATE_CHANGED_SAME_KEY,
    NGX_HTTP_UPSTREAM_IDENTITY_EVENT_PUBLIC_KEY_CHANGED
} ngx_http_upstream_identity_event_type_e;

typedef struct {
    ngx_http_upstream_identity_event_type_e type;
    time_t timestamp;
    u_char previous_cert_sha256[65];
    u_char current_cert_sha256[65];
    u_char previous_spki_sha256[65];
    u_char current_spki_sha256[65];
} ngx_http_upstream_identity_event_t;

#endif
