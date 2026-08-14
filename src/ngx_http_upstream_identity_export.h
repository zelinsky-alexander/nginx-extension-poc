#ifndef NGX_HTTP_UPSTREAM_IDENTITY_EXPORT_H
#define NGX_HTTP_UPSTREAM_IDENTITY_EXPORT_H

#include <ngx_config.h>
#include <ngx_core.h>

#include "ngx_http_upstream_identity_event.h"

ngx_int_t ngx_http_upstream_identity_export_init(ngx_cycle_t *cycle,
    const ngx_str_t *socket_path);

void ngx_http_upstream_identity_export_close(ngx_cycle_t *cycle);

ngx_int_t ngx_http_upstream_identity_export_event(ngx_log_t *log,
    const ngx_http_upstream_identity_event_t *event,
    const ngx_str_t *upstream, const u_char *peer, size_t peer_len);

#endif
