#include <ngx_config.h>
#include <ngx_core.h>

#include "ngx_http_upstream_identity_export.h"
#include "ngx_http_upstream_identity_state.h"

#define NGX_HTTP_UPSTREAM_IDENTITY_STATUS_MAX  (1024 * 1024)

typedef struct {
    ngx_rbtree_t rbtree;
    ngx_rbtree_node_t sentinel;
    ngx_atomic_t peers_tracked;
    ngx_atomic_t events_generated;
    ngx_atomic_t events_exported;
    ngx_atomic_t events_dropped;
    ngx_atomic_t export_errors;
    ngx_atomic_t identity_changes;
} ngx_http_upstream_identity_state_ctx_t;

typedef struct {
    ngx_rbtree_node_t node;
    size_t upstream_len;
    size_t peer_len;
    time_t first_seen;
    time_t last_seen;
    ngx_uint_t observations;
    ngx_uint_t changes;
    u_char cert_sha256[65];
    u_char spki_sha256[65];
    u_char data[1];
} ngx_http_upstream_identity_state_node_t;

static void ngx_http_upstream_identity_state_insert(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel);
static ngx_http_upstream_identity_state_node_t *
ngx_http_upstream_identity_state_lookup(ngx_rbtree_t *tree, ngx_rbtree_key_t key,
    const ngx_str_t *upstream, const u_char *peer, size_t peer_len);
static ngx_int_t ngx_http_upstream_identity_state_compare_parts(
    const ngx_str_t *upstream, const u_char *peer, size_t peer_len,
    const ngx_http_upstream_identity_state_node_t *node);
static ngx_int_t ngx_http_upstream_identity_state_json_size(
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel, size_t *size);
static ngx_int_t ngx_http_upstream_identity_state_write_nodes(
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel,
    u_char **cursor, u_char *last, ngx_uint_t *first);
static u_char *ngx_http_upstream_identity_state_json_escape(
    const u_char *src, size_t src_len, u_char *dst, u_char *last);

ngx_int_t
ngx_http_upstream_identity_state_init_zone(ngx_shm_zone_t *shm_zone, void *data)
{
    ngx_slab_pool_t *shpool;
    ngx_http_upstream_identity_state_ctx_t *ctx;

    if (data != NULL) {
        shm_zone->data = data;
        return NGX_OK;
    }

    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    ctx = ngx_slab_alloc(shpool, sizeof(*ctx));
    if (ctx == NULL) {
        return NGX_ERROR;
    }

    ngx_memzero(ctx, sizeof(*ctx));
    ngx_rbtree_init(&ctx->rbtree, &ctx->sentinel,
                    ngx_http_upstream_identity_state_insert);
    shm_zone->data = ctx;

    return NGX_OK;
}

ngx_int_t
ngx_http_upstream_identity_state_track(ngx_shm_zone_t *shm_zone,
    ngx_log_t *log, const ngx_str_t *upstream, const u_char *peer,
    size_t peer_len, const char *cert_sha256, const char *spki_sha256)
{
    ngx_slab_pool_t *shpool;
    ngx_http_upstream_identity_state_ctx_t *ctx;
    ngx_http_upstream_identity_state_node_t *state;
    ngx_http_upstream_identity_event_t event;
    ngx_rbtree_key_t key;
    ngx_int_t export_rc;
    size_t size;
    u_char previous_cert[65] = "";
    u_char previous_spki[65] = "";
    const char *change = NULL;
    time_t event_time;

    ngx_memzero(&event, sizeof(event));

    if (shm_zone == NULL || shm_zone->data == NULL || upstream == NULL
        || peer == NULL || peer_len == 0 || cert_sha256 == NULL
        || cert_sha256[0] == '\0' || spki_sha256 == NULL)
    {
        return NGX_DECLINED;
    }

    ctx = shm_zone->data;
    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;
    key = ngx_crc32_short((u_char *) peer, peer_len);
    event_time = ngx_time();

    ngx_shmtx_lock(&shpool->mutex);

    state = ngx_http_upstream_identity_state_lookup(&ctx->rbtree, key,
                                                     upstream, peer, peer_len);
    if (state == NULL) {
        size = offsetof(ngx_http_upstream_identity_state_node_t, data)
             + upstream->len + 1 + peer_len;
        state = ngx_slab_alloc_locked(shpool, size);
        if (state == NULL) {
            ngx_shmtx_unlock(&shpool->mutex);
            ngx_log_error(NGX_LOG_WARN, log, 0,
                          "upstream_identity_state allocation_failed "
                          "upstream=\"%V\" peer=\"%*s\"",
                          upstream, peer_len, peer);
            return NGX_ERROR;
        }

        ngx_memzero(state, size);
        state->node.key = key;
        state->upstream_len = upstream->len;
        state->peer_len = peer_len;
        state->first_seen = event_time;
        state->last_seen = event_time;
        state->observations = 1;
        ngx_cpystrn(state->cert_sha256, (u_char *) cert_sha256,
                    sizeof(state->cert_sha256));
        ngx_cpystrn(state->spki_sha256, (u_char *) spki_sha256,
                    sizeof(state->spki_sha256));
        ngx_memcpy(state->data, upstream->data, upstream->len);
        state->data[upstream->len] = '\0';
        ngx_memcpy(state->data + upstream->len + 1, peer, peer_len);
        ngx_rbtree_insert(&ctx->rbtree, &state->node);
        (void) ngx_atomic_fetch_add(&ctx->peers_tracked, 1);
        change = "first_seen";

        event.type = NGX_HTTP_UPSTREAM_IDENTITY_EVENT_FIRST_SEEN;
        event.timestamp = event_time;
        ngx_cpystrn(event.current_cert_sha256, state->cert_sha256,
                    sizeof(event.current_cert_sha256));
        ngx_cpystrn(event.current_spki_sha256, state->spki_sha256,
                    sizeof(event.current_spki_sha256));
    } else {
        state->last_seen = event_time;
        state->observations++;

        if (ngx_strcmp(state->spki_sha256, spki_sha256) != 0) {
            ngx_cpystrn(previous_cert, state->cert_sha256,
                        sizeof(previous_cert));
            ngx_cpystrn(previous_spki, state->spki_sha256,
                        sizeof(previous_spki));
            ngx_cpystrn(state->cert_sha256, (u_char *) cert_sha256,
                        sizeof(state->cert_sha256));
            ngx_cpystrn(state->spki_sha256, (u_char *) spki_sha256,
                        sizeof(state->spki_sha256));
            state->changes++;
            (void) ngx_atomic_fetch_add(&ctx->identity_changes, 1);
            change = "public_key_changed";

            event.type = NGX_HTTP_UPSTREAM_IDENTITY_EVENT_PUBLIC_KEY_CHANGED;
            event.timestamp = event_time;
            ngx_cpystrn(event.previous_cert_sha256, previous_cert,
                        sizeof(event.previous_cert_sha256));
            ngx_cpystrn(event.current_cert_sha256, state->cert_sha256,
                        sizeof(event.current_cert_sha256));
            ngx_cpystrn(event.previous_spki_sha256, previous_spki,
                        sizeof(event.previous_spki_sha256));
            ngx_cpystrn(event.current_spki_sha256, state->spki_sha256,
                        sizeof(event.current_spki_sha256));
        } else if (ngx_strcmp(state->cert_sha256, cert_sha256) != 0) {
            ngx_cpystrn(previous_cert, state->cert_sha256,
                        sizeof(previous_cert));
            ngx_cpystrn(previous_spki, state->spki_sha256,
                        sizeof(previous_spki));
            ngx_cpystrn(state->cert_sha256, (u_char *) cert_sha256,
                        sizeof(state->cert_sha256));
            state->changes++;
            (void) ngx_atomic_fetch_add(&ctx->identity_changes, 1);
            change = "certificate_changed_same_key";

            event.type =
                NGX_HTTP_UPSTREAM_IDENTITY_EVENT_CERTIFICATE_CHANGED_SAME_KEY;
            event.timestamp = event_time;
            ngx_cpystrn(event.previous_cert_sha256, previous_cert,
                        sizeof(event.previous_cert_sha256));
            ngx_cpystrn(event.current_cert_sha256, state->cert_sha256,
                        sizeof(event.current_cert_sha256));
            ngx_cpystrn(event.previous_spki_sha256, previous_spki,
                        sizeof(event.previous_spki_sha256));
            ngx_cpystrn(event.current_spki_sha256, state->spki_sha256,
                        sizeof(event.current_spki_sha256));
        }
    }

    ngx_shmtx_unlock(&shpool->mutex);

    if (change == NULL) {
        return NGX_OK;
    }

    (void) ngx_atomic_fetch_add(&ctx->events_generated, 1);

    if (ngx_strcmp(change, "first_seen") == 0) {
        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "upstream_identity_change change=\"first_seen\" "
                      "upstream=\"%V\" peer=\"%*s\" "
                      "cert_sha256=\"%s\" spki_sha256=\"%s\"",
                      upstream, peer_len, peer, cert_sha256, spki_sha256);
    } else {
        ngx_log_error(NGX_LOG_WARN, log, 0,
                      "upstream_identity_change change=\"%s\" "
                      "upstream=\"%V\" peer=\"%*s\" "
                      "previous_cert_sha256=\"%s\" "
                      "current_cert_sha256=\"%s\" "
                      "previous_spki_sha256=\"%s\" "
                      "current_spki_sha256=\"%s\"",
                      change, upstream, peer_len, peer,
                      previous_cert, cert_sha256, previous_spki, spki_sha256);
    }

    export_rc = ngx_http_upstream_identity_export_event(log, &event,
                                                         upstream, peer,
                                                         peer_len);
    if (export_rc == NGX_OK) {
        (void) ngx_atomic_fetch_add(&ctx->events_exported, 1);
    } else if (export_rc == NGX_AGAIN) {
        (void) ngx_atomic_fetch_add(&ctx->events_dropped, 1);
    } else if (export_rc == NGX_ERROR) {
        (void) ngx_atomic_fetch_add(&ctx->export_errors, 1);
    }

    return NGX_OK;
}

ngx_int_t
ngx_http_upstream_identity_state_render_json(ngx_shm_zone_t *shm_zone,
    ngx_pool_t *pool, ngx_str_t *out)
{
    ngx_slab_pool_t *shpool;
    ngx_http_upstream_identity_state_ctx_t *ctx;
    size_t size = 512;
    u_char *buffer;
    u_char *p;
    u_char *last;
    ngx_uint_t first = 1;

    if (shm_zone == NULL || shm_zone->data == NULL || pool == NULL
        || out == NULL)
    {
        return NGX_DECLINED;
    }

    ctx = shm_zone->data;
    shpool = (ngx_slab_pool_t *) shm_zone->shm.addr;

    ngx_shmtx_lock(&shpool->mutex);

    if (ngx_http_upstream_identity_state_json_size(ctx->rbtree.root,
                                                    ctx->rbtree.sentinel,
                                                    &size)
        != NGX_OK
        || size > NGX_HTTP_UPSTREAM_IDENTITY_STATUS_MAX)
    {
        ngx_shmtx_unlock(&shpool->mutex);
        return NGX_BUSY;
    }

    buffer = ngx_pnalloc(pool, size);
    if (buffer == NULL) {
        ngx_shmtx_unlock(&shpool->mutex);
        return NGX_ERROR;
    }

    p = buffer;
    last = buffer + size;

    p = ngx_snprintf(p, (size_t) (last - p),
        "{\"schema_version\":2,\"generated_at\":%T,"
        "\"counters\":{\"peers_tracked\":%ui,"
        "\"events_generated\":%ui,\"events_exported\":%ui,"
        "\"events_dropped\":%ui,\"export_errors\":%ui,"
        "\"identity_changes\":%ui},\"peers\":[",
        ngx_time(),
        (ngx_uint_t) ctx->peers_tracked,
        (ngx_uint_t) ctx->events_generated,
        (ngx_uint_t) ctx->events_exported,
        (ngx_uint_t) ctx->events_dropped,
        (ngx_uint_t) ctx->export_errors,
        (ngx_uint_t) ctx->identity_changes);

    if (ngx_http_upstream_identity_state_write_nodes(ctx->rbtree.root,
                                                      ctx->rbtree.sentinel,
                                                      &p, last, &first)
        != NGX_OK
        || p + 2 > last)
    {
        ngx_shmtx_unlock(&shpool->mutex);
        return NGX_ERROR;
    }

    *p++ = ']';
    *p++ = '}';

    ngx_shmtx_unlock(&shpool->mutex);

    out->data = buffer;
    out->len = (size_t) (p - buffer);
    return NGX_OK;
}

static ngx_int_t
ngx_http_upstream_identity_state_json_size(ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel, size_t *size)
{
    ngx_http_upstream_identity_state_node_t *state;
    size_t add;

    if (node == sentinel) {
        return NGX_OK;
    }

    if (ngx_http_upstream_identity_state_json_size(node->left, sentinel, size)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    state = (ngx_http_upstream_identity_state_node_t *) node;
    add = 512 + state->upstream_len * 6 + state->peer_len * 6;

    if (*size > NGX_HTTP_UPSTREAM_IDENTITY_STATUS_MAX
        || add > NGX_HTTP_UPSTREAM_IDENTITY_STATUS_MAX - *size)
    {
        return NGX_BUSY;
    }

    *size += add;

    return ngx_http_upstream_identity_state_json_size(node->right,
                                                       sentinel, size);
}

static ngx_int_t
ngx_http_upstream_identity_state_write_nodes(ngx_rbtree_node_t *node,
    ngx_rbtree_node_t *sentinel, u_char **cursor, u_char *last,
    ngx_uint_t *first)
{
    ngx_http_upstream_identity_state_node_t *state;
    const u_char *peer;
    u_char *p;

    if (node == sentinel) {
        return NGX_OK;
    }

    if (ngx_http_upstream_identity_state_write_nodes(node->left, sentinel,
                                                      cursor, last, first)
        != NGX_OK)
    {
        return NGX_ERROR;
    }

    state = (ngx_http_upstream_identity_state_node_t *) node;
    peer = state->data + state->upstream_len + 1;
    p = *cursor;

    if (!*first) {
        if (p >= last) {
            return NGX_ERROR;
        }
        *p++ = ',';
    }
    *first = 0;

    p = ngx_snprintf(p, (size_t) (last - p), "{\"upstream\":\"");
    p = ngx_http_upstream_identity_state_json_escape(state->data,
                                                      state->upstream_len,
                                                      p, last);
    if (p == NULL) {
        return NGX_ERROR;
    }

    p = ngx_snprintf(p, (size_t) (last - p), "\",\"peer\":\"");
    p = ngx_http_upstream_identity_state_json_escape(peer, state->peer_len,
                                                      p, last);
    if (p == NULL) {
        return NGX_ERROR;
    }

    p = ngx_snprintf(p, (size_t) (last - p),
        "\",\"first_seen\":%T,\"last_seen\":%T,"
        "\"observations\":%ui,\"changes\":%ui,"
        "\"cert_sha256\":\"%s\",\"spki_sha256\":\"%s\"}",
        state->first_seen, state->last_seen,
        state->observations, state->changes,
        state->cert_sha256, state->spki_sha256);

    if (p > last) {
        return NGX_ERROR;
    }

    *cursor = p;

    return ngx_http_upstream_identity_state_write_nodes(node->right,
                                                         sentinel, cursor,
                                                         last, first);
}

static u_char *
ngx_http_upstream_identity_state_json_escape(const u_char *src, size_t src_len,
    u_char *dst, u_char *last)
{
    static const u_char hex[] = "0123456789abcdef";
    size_t i;
    u_char ch;

    for (i = 0; i < src_len; ++i) {
        ch = src[i];

        if (ch == '"' || ch == '\\') {
            if (last - dst < 2) {
                return NULL;
            }
            *dst++ = '\\';
            *dst++ = ch;
            continue;
        }

        if (ch < 0x20 || ch >= 0x7f) {
            if (last - dst < 6) {
                return NULL;
            }
            *dst++ = '\\';
            *dst++ = 'u';
            *dst++ = '0';
            *dst++ = '0';
            *dst++ = hex[(ch >> 4) & 0x0f];
            *dst++ = hex[ch & 0x0f];
            continue;
        }

        if (dst >= last) {
            return NULL;
        }
        *dst++ = ch;
    }

    return dst;
}

static void
ngx_http_upstream_identity_state_insert(ngx_rbtree_node_t *temp,
    ngx_rbtree_node_t *node, ngx_rbtree_node_t *sentinel)
{
    ngx_rbtree_node_t **p;
    ngx_http_upstream_identity_state_node_t *a;
    ngx_http_upstream_identity_state_node_t *b;
    ngx_int_t rc;
    size_t a_key_len;
    size_t b_key_len;

    for ( ;; ) {
        if (node->key < temp->key) {
            p = &temp->left;
        } else if (node->key > temp->key) {
            p = &temp->right;
        } else {
            a = (ngx_http_upstream_identity_state_node_t *) node;
            b = (ngx_http_upstream_identity_state_node_t *) temp;
            a_key_len = a->upstream_len + 1 + a->peer_len;
            b_key_len = b->upstream_len + 1 + b->peer_len;
            rc = ngx_memn2cmp(a->data, b->data, a_key_len, b_key_len);
            p = (rc < 0) ? &temp->left : &temp->right;
        }

        if (*p == sentinel) {
            break;
        }

        temp = *p;
    }

    *p = node;
    node->parent = temp;
    node->left = sentinel;
    node->right = sentinel;
    ngx_rbt_red(node);
}

static ngx_http_upstream_identity_state_node_t *
ngx_http_upstream_identity_state_lookup(ngx_rbtree_t *tree, ngx_rbtree_key_t key,
    const ngx_str_t *upstream, const u_char *peer, size_t peer_len)
{
    ngx_rbtree_node_t *node;
    ngx_rbtree_node_t *sentinel;
    ngx_http_upstream_identity_state_node_t *state;
    ngx_int_t rc;

    node = tree->root;
    sentinel = tree->sentinel;

    while (node != sentinel) {
        if (key < node->key) {
            node = node->left;
            continue;
        }
        if (key > node->key) {
            node = node->right;
            continue;
        }

        state = (ngx_http_upstream_identity_state_node_t *) node;
        rc = ngx_http_upstream_identity_state_compare_parts(upstream, peer,
                                                             peer_len, state);
        if (rc == 0) {
            return state;
        }
        node = (rc < 0) ? node->left : node->right;
    }

    return NULL;
}

static ngx_int_t
ngx_http_upstream_identity_state_compare_parts(const ngx_str_t *upstream,
    const u_char *peer, size_t peer_len,
    const ngx_http_upstream_identity_state_node_t *node)
{
    ngx_int_t rc;
    const u_char *node_peer;

    rc = ngx_memn2cmp(upstream->data, (u_char *) node->data,
                      upstream->len, node->upstream_len);
    if (rc != 0) {
        return rc;
    }

    node_peer = node->data + node->upstream_len + 1;
    return ngx_memn2cmp((u_char *) peer, (u_char *) node_peer,
                        peer_len, node->peer_len);
}
