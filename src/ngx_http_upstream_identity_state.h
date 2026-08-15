#ifndef NGX_HTTP_UPSTREAM_IDENTITY_STATE_H
#define NGX_HTTP_UPSTREAM_IDENTITY_STATE_H

#include <ngx_config.h>
#include <ngx_core.h>

ngx_int_t ngx_http_upstream_identity_state_init_zone(ngx_shm_zone_t *shm_zone,
    void *data);

ngx_int_t ngx_http_upstream_identity_state_track(ngx_shm_zone_t *shm_zone,
    ngx_log_t *log, const ngx_str_t *upstream, const u_char *peer,
    size_t peer_len, const char *cert_sha256, const char *spki_sha256);

ngx_int_t ngx_http_upstream_identity_state_render_json(
    ngx_shm_zone_t *shm_zone, ngx_pool_t *pool, ngx_str_t *out);

#endif
