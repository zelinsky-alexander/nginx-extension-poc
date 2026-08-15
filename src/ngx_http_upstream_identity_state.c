#include <ngx_config.h>
#include <ngx_core.h>

#include "ngx_http_upstream_identity_export.h"
#include "ngx_http_upstream_identity_state.h"

typedef struct {
    ngx_rbtree_t rbtree;
    ngx_rbtree_node_t sentinel;
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

    (void) ngx_http_upstream_identity_export_event(log, &event,
                                                    upstream, peer, peer_len);

    return NGX_OK;
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
