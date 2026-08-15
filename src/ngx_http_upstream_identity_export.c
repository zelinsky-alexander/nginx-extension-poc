#include <ngx_config.h>
#include <ngx_core.h>

#include <stdlib.h>

#include "ngx_http_upstream_identity_export.h"

#if (NGX_HAVE_UNIX_DOMAIN)
#include <sys/socket.h>
#include <sys/un.h>
#endif

#define NGX_HTTP_UPSTREAM_IDENTITY_JSON_MAX 4096
#define NGX_HTTP_UPSTREAM_IDENTITY_ESCAPED_UPSTREAM_MAX 2048
#define NGX_HTTP_UPSTREAM_IDENTITY_ESCAPED_PEER_MAX 512
#define NGX_HTTP_UPSTREAM_IDENTITY_HISTORY_ENV \
    "NGX_UPSTREAM_IDENTITY_HISTORY_SOCKET"

static ngx_socket_t ngx_http_upstream_identity_export_socket = (ngx_socket_t) -1;
static ngx_uint_t ngx_http_upstream_identity_export_initialized = 0;

#if (NGX_HAVE_UNIX_DOMAIN)
static struct sockaddr_un ngx_http_upstream_identity_export_addr;
static socklen_t ngx_http_upstream_identity_export_addr_len;
#endif

static ngx_int_t ngx_http_upstream_identity_export_lazy_init(ngx_log_t *log);
static const char *ngx_http_upstream_identity_event_name(
    ngx_http_upstream_identity_event_type_e type);
static ngx_int_t ngx_http_upstream_identity_json_escape(const u_char *src,
    size_t src_len, u_char *dst, size_t dst_len);

static ngx_int_t
ngx_http_upstream_identity_export_lazy_init(ngx_log_t *log)
{
    const char *path;
    size_t path_len;

    if (ngx_http_upstream_identity_export_initialized) {
        return ngx_http_upstream_identity_export_socket == (ngx_socket_t) -1
            ? NGX_DECLINED : NGX_OK;
    }

    ngx_http_upstream_identity_export_initialized = 1;
    path = getenv(NGX_HTTP_UPSTREAM_IDENTITY_HISTORY_ENV);
    if (path == NULL || path[0] == '\0') {
        return NGX_DECLINED;
    }

#if !(NGX_HAVE_UNIX_DOMAIN)
    ngx_log_error(NGX_LOG_WARN, log, 0,
                  "upstream identity history export disabled: "
                  "Unix-domain sockets are unavailable");
    return NGX_DECLINED;
#else
    path_len = ngx_strlen(path);
    if (path_len >= sizeof(ngx_http_upstream_identity_export_addr.sun_path)) {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "upstream identity history export disabled: "
                      "socket path is too long");
        return NGX_DECLINED;
    }

    ngx_http_upstream_identity_export_socket = socket(AF_UNIX, SOCK_DGRAM, 0);
    if (ngx_http_upstream_identity_export_socket == (ngx_socket_t) -1) {
        ngx_log_error(NGX_LOG_WARN, log, ngx_socket_errno,
                      "upstream identity history socket() failed");
        return NGX_DECLINED;
    }

    if (ngx_nonblocking(ngx_http_upstream_identity_export_socket) == -1) {
        ngx_log_error(NGX_LOG_WARN, log, ngx_socket_errno,
                      "upstream identity history failed to set nonblocking mode");
        ngx_close_socket(ngx_http_upstream_identity_export_socket);
        ngx_http_upstream_identity_export_socket = (ngx_socket_t) -1;
        return NGX_DECLINED;
    }

    ngx_memzero(&ngx_http_upstream_identity_export_addr,
                sizeof(ngx_http_upstream_identity_export_addr));
    ngx_http_upstream_identity_export_addr.sun_family = AF_UNIX;
    ngx_memcpy(ngx_http_upstream_identity_export_addr.sun_path, path, path_len);
    ngx_http_upstream_identity_export_addr.sun_path[path_len] = '\0';
    ngx_http_upstream_identity_export_addr_len =
        (socklen_t) (offsetof(struct sockaddr_un, sun_path) + path_len + 1);

    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "upstream identity structured history export enabled "
                  "socket=\"%s\"", path);

    return NGX_OK;
#endif
}

ngx_int_t
ngx_http_upstream_identity_export_event(ngx_log_t *log,
    const ngx_http_upstream_identity_event_t *event,
    const ngx_str_t *upstream, const u_char *peer, size_t peer_len)
{
#if !(NGX_HAVE_UNIX_DOMAIN)
    (void) log;
    (void) event;
    (void) upstream;
    (void) peer;
    (void) peer_len;
    return NGX_DECLINED;
#else
    u_char escaped_upstream[NGX_HTTP_UPSTREAM_IDENTITY_ESCAPED_UPSTREAM_MAX];
    u_char escaped_peer[NGX_HTTP_UPSTREAM_IDENTITY_ESCAPED_PEER_MAX];
    u_char payload[NGX_HTTP_UPSTREAM_IDENTITY_JSON_MAX];
    u_char *last;
    size_t payload_len;
    ssize_t sent;
    ngx_err_t err;
    const char *event_name;

    if (event == NULL
        || event->type == NGX_HTTP_UPSTREAM_IDENTITY_EVENT_NONE
        || upstream == NULL || peer == NULL || peer_len == 0)
    {
        return NGX_DECLINED;
    }

    if (ngx_http_upstream_identity_export_lazy_init(log) != NGX_OK) {
        return NGX_DECLINED;
    }

    event_name = ngx_http_upstream_identity_event_name(event->type);
    if (event_name == NULL) {
        return NGX_DECLINED;
    }

    if (ngx_http_upstream_identity_json_escape(upstream->data, upstream->len,
                                                escaped_upstream,
                                                sizeof(escaped_upstream))
        != NGX_OK
        || ngx_http_upstream_identity_json_escape(peer, peer_len,
                                                   escaped_peer,
                                                   sizeof(escaped_peer))
           != NGX_OK)
    {
        ngx_log_debug1(NGX_LOG_DEBUG_HTTP, log, 0,
                       "upstream identity history event too large event=%s",
                       event_name);
        return NGX_ERROR;
    }

    last = ngx_snprintf(payload, sizeof(payload),
        "{\"schema_version\":1,\"timestamp\":%T,\"change\":\"%s\","
        "\"upstream\":\"%s\",\"peer\":\"%s\","
        "\"previous_cert_sha256\":\"%s\","
        "\"current_cert_sha256\":\"%s\","
        "\"previous_spki_sha256\":\"%s\","
        "\"current_spki_sha256\":\"%s\"}",
        event->timestamp, event_name, escaped_upstream, escaped_peer,
        event->previous_cert_sha256, event->current_cert_sha256,
        event->previous_spki_sha256, event->current_spki_sha256);

    payload_len = (size_t) (last - payload);
    if (payload_len == 0 || payload_len >= sizeof(payload)) {
        return NGX_ERROR;
    }

    sent = sendto(ngx_http_upstream_identity_export_socket,
                  payload, payload_len, 0,
                  (struct sockaddr *) &ngx_http_upstream_identity_export_addr,
                  ngx_http_upstream_identity_export_addr_len);

    if (sent == (ssize_t) payload_len) {
        return NGX_OK;
    }

    if (sent == -1) {
        err = ngx_socket_errno;
        ngx_log_debug2(NGX_LOG_DEBUG_HTTP, log, err,
                       "upstream identity history event dropped event=%s errno=%d",
                       event_name, err);

        if (err == NGX_EAGAIN) {
            return NGX_AGAIN;
        }
    }

    return NGX_ERROR;
#endif
}

static const char *
ngx_http_upstream_identity_event_name(
    ngx_http_upstream_identity_event_type_e type)
{
    switch (type) {
    case NGX_HTTP_UPSTREAM_IDENTITY_EVENT_FIRST_SEEN:
        return "first_seen";
    case NGX_HTTP_UPSTREAM_IDENTITY_EVENT_CERTIFICATE_CHANGED_SAME_KEY:
        return "certificate_changed_same_key";
    case NGX_HTTP_UPSTREAM_IDENTITY_EVENT_PUBLIC_KEY_CHANGED:
        return "public_key_changed";
    case NGX_HTTP_UPSTREAM_IDENTITY_EVENT_NONE:
    default:
        return NULL;
    }
}

static ngx_int_t
ngx_http_upstream_identity_json_escape(const u_char *src, size_t src_len,
    u_char *dst, size_t dst_len)
{
    static const u_char hex[] = "0123456789abcdef";
    size_t i;
    size_t out = 0;
    u_char ch;

    if (dst_len == 0) {
        return NGX_ERROR;
    }

    for (i = 0; i < src_len; ++i) {
        ch = src[i];

        if (ch == '"' || ch == '\\') {
            if (out + 2 >= dst_len) {
                return NGX_ERROR;
            }
            dst[out++] = '\\';
            dst[out++] = ch;
            continue;
        }

        if (ch < 0x20 || ch >= 0x7f) {
            if (out + 6 >= dst_len) {
                return NGX_ERROR;
            }
            dst[out++] = '\\';
            dst[out++] = 'u';
            dst[out++] = '0';
            dst[out++] = '0';
            dst[out++] = hex[(ch >> 4) & 0x0f];
            dst[out++] = hex[ch & 0x0f];
            continue;
        }

        if (out + 1 >= dst_len) {
            return NGX_ERROR;
        }
        dst[out++] = ch;
    }

    dst[out] = '\0';
    return NGX_OK;
}
