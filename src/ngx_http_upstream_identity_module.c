#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <ngx_http_upstream_round_robin.h>

#if (NGX_SSL)
#include <ngx_event_openssl.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

typedef struct {
    ngx_flag_t enabled;
} ngx_http_upstream_identity_loc_conf_t;

typedef struct {
    ngx_flag_t configured;
    ngx_http_upstream_init_pt original_init_upstream;
    ngx_http_upstream_init_peer_pt original_init_peer;
} ngx_http_upstream_identity_srv_conf_t;

typedef struct {
    void *original_data;
    ngx_event_get_peer_pt original_get;
    ngx_event_free_peer_pt original_free;
    ngx_event_notify_peer_pt original_notify;
#if (NGX_HTTP_SSL)
    ngx_event_set_peer_session_pt original_set_session;
    ngx_event_save_peer_session_pt original_save_session;
#endif
} ngx_http_upstream_identity_peer_data_t;

static ngx_http_output_header_filter_pt ngx_http_upstream_identity_next_header_filter;

static ngx_int_t ngx_http_upstream_identity_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_upstream_identity_init(ngx_conf_t *cf);
static void *ngx_http_upstream_identity_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_upstream_identity_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);
static void *ngx_http_upstream_identity_create_srv_conf(ngx_conf_t *cf);
static char *ngx_http_upstream_identity_peer_monitor(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_upstream_identity_init_upstream(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_upstream_identity_init_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us);
static ngx_int_t ngx_http_upstream_identity_get_peer(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_identity_free_peer(ngx_peer_connection_t *pc,
    void *data, ngx_uint_t state);
static void ngx_http_upstream_identity_notify_peer(ngx_peer_connection_t *pc,
    void *data, ngx_uint_t type);
static void ngx_http_upstream_identity_observe_connection(ngx_connection_t *c,
    ngx_log_t *log, const char *stage, ngx_uint_t state, ngx_uint_t cached);

#if (NGX_HTTP_SSL)
static ngx_int_t ngx_http_upstream_identity_set_session(ngx_peer_connection_t *pc,
    void *data);
static void ngx_http_upstream_identity_save_session(ngx_peer_connection_t *pc,
    void *data);
#endif

#if (NGX_SSL)
static void ngx_http_upstream_identity_hex(const unsigned char *src,
    size_t src_len, char *dst, size_t dst_len);
static void ngx_http_upstream_identity_copy_printable(const unsigned char *src,
    size_t src_len, char *dst, size_t dst_len);
static ngx_int_t ngx_http_upstream_identity_cert_sha256(X509 *cert,
    char *dst, size_t dst_len);
static ngx_int_t ngx_http_upstream_identity_spki_sha256(X509 *cert,
    char *dst, size_t dst_len);
static void ngx_http_upstream_identity_dns_sans(X509 *cert,
    char *dst, size_t dst_len);
#endif

static ngx_command_t ngx_http_upstream_identity_commands[] = {
    {
        ngx_string("upstream_identity_monitor"),
        NGX_HTTP_MAIN_CONF | NGX_HTTP_SRV_CONF | NGX_HTTP_LOC_CONF | NGX_CONF_FLAG,
        ngx_conf_set_flag_slot,
        NGX_HTTP_LOC_CONF_OFFSET,
        offsetof(ngx_http_upstream_identity_loc_conf_t, enabled),
        NULL
    },
    {
        ngx_string("upstream_identity_peer_monitor"),
        NGX_HTTP_UPS_CONF | NGX_CONF_NOARGS,
        ngx_http_upstream_identity_peer_monitor,
        NGX_HTTP_SRV_CONF_OFFSET,
        0,
        NULL
    },
    ngx_null_command
};

static ngx_http_module_t ngx_http_upstream_identity_module_ctx = {
    NULL,
    ngx_http_upstream_identity_init,
    NULL,
    NULL,
    ngx_http_upstream_identity_create_srv_conf,
    NULL,
    ngx_http_upstream_identity_create_loc_conf,
    ngx_http_upstream_identity_merge_loc_conf
};

ngx_module_t ngx_http_upstream_identity_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_identity_module_ctx,
    ngx_http_upstream_identity_commands,
    NGX_HTTP_MODULE,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NULL,
    NGX_MODULE_V1_PADDING
};

static void *
ngx_http_upstream_identity_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_identity_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) {
        return NULL;
    }

    conf->enabled = NGX_CONF_UNSET;
    return conf;
}

static char *
ngx_http_upstream_identity_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child)
{
    ngx_http_upstream_identity_loc_conf_t *prev = parent;
    ngx_http_upstream_identity_loc_conf_t *conf = child;

    ngx_conf_merge_value(conf->enabled, prev->enabled, 0);
    return NGX_CONF_OK;
}

static void *
ngx_http_upstream_identity_create_srv_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_identity_srv_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) {
        return NULL;
    }

    return conf;
}

static char *
ngx_http_upstream_identity_peer_monitor(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_upstream_srv_conf_t *uscf;
    ngx_http_upstream_identity_srv_conf_t *identity_conf = conf;

    (void) cmd;

    if (identity_conf->configured) {
        return "is duplicate";
    }

    uscf = ngx_http_conf_get_module_srv_conf(cf, ngx_http_upstream_module);
    if (uscf == NULL) {
        return NGX_CONF_ERROR;
    }

    identity_conf->original_init_upstream = uscf->peer.init_upstream
        ? uscf->peer.init_upstream
        : ngx_http_upstream_init_round_robin;

    uscf->peer.init_upstream = ngx_http_upstream_identity_init_upstream;
    identity_conf->configured = 1;

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_upstream_identity_init_upstream(ngx_conf_t *cf,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_identity_srv_conf_t *conf;

    conf = ngx_http_conf_upstream_srv_conf(us,
                                           ngx_http_upstream_identity_module);
    if (conf == NULL || conf->original_init_upstream == NULL) {
        return NGX_ERROR;
    }

    if (conf->original_init_upstream(cf, us) != NGX_OK) {
        return NGX_ERROR;
    }

    conf->original_init_peer = us->peer.init;
    if (conf->original_init_peer == NULL) {
        return NGX_ERROR;
    }

    us->peer.init = ngx_http_upstream_identity_init_peer;
    return NGX_OK;
}

static ngx_int_t
ngx_http_upstream_identity_init_peer(ngx_http_request_t *r,
    ngx_http_upstream_srv_conf_t *us)
{
    ngx_http_upstream_identity_srv_conf_t *conf;
    ngx_http_upstream_identity_peer_data_t *peer_data;

    conf = ngx_http_conf_upstream_srv_conf(us,
                                           ngx_http_upstream_identity_module);
    if (conf == NULL || conf->original_init_peer == NULL) {
        return NGX_ERROR;
    }

    if (conf->original_init_peer(r, us) != NGX_OK) {
        return NGX_ERROR;
    }

    peer_data = ngx_pcalloc(r->pool, sizeof(*peer_data));
    if (peer_data == NULL) {
        return NGX_ERROR;
    }

    peer_data->original_data = r->upstream->peer.data;
    peer_data->original_get = r->upstream->peer.get;
    peer_data->original_free = r->upstream->peer.free;
    peer_data->original_notify = r->upstream->peer.notify;
#if (NGX_HTTP_SSL)
    peer_data->original_set_session = r->upstream->peer.set_session;
    peer_data->original_save_session = r->upstream->peer.save_session;
#endif

    r->upstream->peer.data = peer_data;
    r->upstream->peer.get = ngx_http_upstream_identity_get_peer;
    r->upstream->peer.free = ngx_http_upstream_identity_free_peer;

    if (peer_data->original_notify != NULL) {
        r->upstream->peer.notify = ngx_http_upstream_identity_notify_peer;
    }

#if (NGX_HTTP_SSL)
    if (peer_data->original_set_session != NULL) {
        r->upstream->peer.set_session = ngx_http_upstream_identity_set_session;
    }
    if (peer_data->original_save_session != NULL) {
        r->upstream->peer.save_session = ngx_http_upstream_identity_save_session;
    }
#endif

    return NGX_OK;
}

static ngx_int_t
ngx_http_upstream_identity_get_peer(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_identity_peer_data_t *peer_data = data;
    ngx_int_t rc;

    if (peer_data == NULL || peer_data->original_get == NULL) {
        return NGX_ERROR;
    }

    rc = peer_data->original_get(pc, peer_data->original_data);

    if (rc == NGX_DONE && pc->connection != NULL) {
        ngx_http_upstream_identity_observe_connection(pc->connection, pc->log,
                                                      "peer_get_reused", 0, 1);
    }

    return rc;
}

static void
ngx_http_upstream_identity_free_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t state)
{
    ngx_http_upstream_identity_peer_data_t *peer_data = data;

    if (pc != NULL && pc->connection != NULL) {
        ngx_http_upstream_identity_observe_connection(pc->connection, pc->log,
                                                      "peer_free", state,
                                                      pc->cached);
    } else if (pc != NULL) {
        ngx_log_error(NGX_LOG_NOTICE, pc->log, 0,
                      "upstream_identity unavailable stage=\"peer_free\" "
                      "reason=peer_connection_missing state=%ui",
                      state);
    }

    if (peer_data != NULL && peer_data->original_free != NULL) {
        peer_data->original_free(pc, peer_data->original_data, state);
    }
}

static void
ngx_http_upstream_identity_notify_peer(ngx_peer_connection_t *pc, void *data,
    ngx_uint_t type)
{
    ngx_http_upstream_identity_peer_data_t *peer_data = data;

    if (peer_data != NULL && peer_data->original_notify != NULL) {
        peer_data->original_notify(pc, peer_data->original_data, type);
    }
}

#if (NGX_HTTP_SSL)
static ngx_int_t
ngx_http_upstream_identity_set_session(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_identity_peer_data_t *peer_data = data;

    if (peer_data == NULL || peer_data->original_set_session == NULL) {
        return NGX_OK;
    }

    return peer_data->original_set_session(pc, peer_data->original_data);
}

static void
ngx_http_upstream_identity_save_session(ngx_peer_connection_t *pc, void *data)
{
    ngx_http_upstream_identity_peer_data_t *peer_data = data;

    if (peer_data != NULL && peer_data->original_save_session != NULL) {
        peer_data->original_save_session(pc, peer_data->original_data);
    }
}
#endif

static ngx_int_t
ngx_http_upstream_identity_init(ngx_conf_t *cf)
{
    ngx_http_upstream_identity_next_header_filter = ngx_http_top_header_filter;
    ngx_http_top_header_filter = ngx_http_upstream_identity_header_filter;
    return NGX_OK;
}

static ngx_int_t
ngx_http_upstream_identity_header_filter(ngx_http_request_t *r)
{
    ngx_http_upstream_identity_loc_conf_t *conf;
    ngx_connection_t *upstream_connection;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_upstream_identity_module);

    if (conf == NULL || !conf->enabled || r->upstream == NULL) {
        return ngx_http_upstream_identity_next_header_filter(r);
    }

    upstream_connection = r->upstream->peer.connection;
    if (upstream_connection == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "upstream_identity unavailable stage=\"header_filter\" "
                      "reason=peer_connection_missing");
        return ngx_http_upstream_identity_next_header_filter(r);
    }

    ngx_http_upstream_identity_observe_connection(upstream_connection,
                                                  r->connection->log,
                                                  "header_filter", 0,
                                                  r->upstream->peer.cached);

    return ngx_http_upstream_identity_next_header_filter(r);
}

static void
ngx_http_upstream_identity_observe_connection(ngx_connection_t *c,
    ngx_log_t *log, const char *stage, ngx_uint_t state, ngx_uint_t cached)
{
    u_char peer[NGX_SOCKADDR_STRLEN];
    size_t peer_len;

    if (c == NULL || c->sockaddr == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "upstream_identity unavailable stage=\"%s\" "
                      "reason=connection_address_missing state=%ui cached=%ui",
                      stage, state, cached);
        return;
    }

    peer_len = ngx_sock_ntop(c->sockaddr, c->socklen,
                             peer, sizeof(peer), 1);
    if (peer_len >= sizeof(peer)) {
        peer_len = sizeof(peer) - 1;
    }
    peer[peer_len] = '\0';

#if (NGX_SSL)
    if (c->ssl != NULL && c->ssl->connection != NULL) {
        SSL *ssl = c->ssl->connection;
        X509 *cert;
        const char *tls_version;
        const unsigned char *alpn_data = NULL;
        unsigned int alpn_len = 0;
        char alpn[64] = "";
        char cert_sha256[65] = "";
        char spki_sha256[65] = "";
        char issuer[512] = "";
        char sans[512] = "";
        char issuer_raw[512] = "";

#if OPENSSL_VERSION_NUMBER >= 0x30000000L
        cert = SSL_get1_peer_certificate(ssl);
#else
        cert = SSL_get_peer_certificate(ssl);
#endif

        tls_version = SSL_get_version(ssl);
        SSL_get0_alpn_selected(ssl, &alpn_data, &alpn_len);
        ngx_http_upstream_identity_copy_printable(alpn_data, alpn_len,
                                                   alpn, sizeof(alpn));

        if (cert != NULL) {
            (void) ngx_http_upstream_identity_cert_sha256(
                cert, cert_sha256, sizeof(cert_sha256));
            (void) ngx_http_upstream_identity_spki_sha256(
                cert, spki_sha256, sizeof(spki_sha256));

            if (X509_NAME_oneline(X509_get_issuer_name(cert), issuer_raw,
                                  sizeof(issuer_raw)) != NULL)
            {
                ngx_http_upstream_identity_copy_printable(
                    (const unsigned char *) issuer_raw,
                    ngx_strlen(issuer_raw), issuer, sizeof(issuer));
            }

            ngx_http_upstream_identity_dns_sans(cert, sans, sizeof(sans));
            X509_free(cert);
        }

        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "upstream_identity stage=\"%s\" peer=\"%s\" "
                      "state=%ui cached=%ui tls=\"%s\" alpn=\"%s\" "
                      "cert_sha256=\"%s\" spki_sha256=\"%s\" issuer=\"%s\" "
                      "san_dns=\"%s\"",
                      stage,
                      peer,
                      state,
                      cached,
                      tls_version != NULL ? tls_version : "",
                      alpn,
                      cert_sha256,
                      spki_sha256,
                      issuer,
                      sans);
    } else {
        ngx_log_error(NGX_LOG_NOTICE, log, 0,
                      "upstream_identity stage=\"%s\" peer=\"%s\" "
                      "state=%ui cached=%ui tls=\"\" reason=not_tls",
                      stage, peer, state, cached);
    }
#else
    ngx_log_error(NGX_LOG_NOTICE, log, 0,
                  "upstream_identity stage=\"%s\" peer=\"%s\" "
                  "state=%ui cached=%ui reason=nginx_built_without_ssl",
                  stage, peer, state, cached);
#endif
}

#if (NGX_SSL)
static void
ngx_http_upstream_identity_hex(const unsigned char *src, size_t src_len,
    char *dst, size_t dst_len)
{
    static const char digits[] = "0123456789abcdef";
    size_t i;

    if (dst_len == 0) {
        return;
    }

    if (src == NULL || dst_len < (src_len * 2 + 1)) {
        dst[0] = '\0';
        return;
    }

    for (i = 0; i < src_len; ++i) {
        dst[i * 2] = digits[(src[i] >> 4) & 0x0f];
        dst[i * 2 + 1] = digits[src[i] & 0x0f];
    }

    dst[src_len * 2] = '\0';
}

static void
ngx_http_upstream_identity_copy_printable(const unsigned char *src,
    size_t src_len, char *dst, size_t dst_len)
{
    size_t i;
    size_t out = 0;

    if (dst_len == 0) {
        return;
    }

    if (src == NULL) {
        dst[0] = '\0';
        return;
    }

    for (i = 0; i < src_len && out + 1 < dst_len; ++i) {
        unsigned char ch = src[i];
        dst[out++] = (ch >= 0x20 && ch <= 0x7e && ch != '"' && ch != '\\')
            ? (char) ch : '_';
    }

    dst[out] = '\0';
}

static ngx_int_t
ngx_http_upstream_identity_cert_sha256(X509 *cert, char *dst, size_t dst_len)
{
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;

    if (cert == NULL
        || X509_digest(cert, EVP_sha256(), digest, &digest_len) != 1
        || digest_len != 32)
    {
        if (dst_len > 0) {
            dst[0] = '\0';
        }
        return NGX_ERROR;
    }

    ngx_http_upstream_identity_hex(digest, digest_len, dst, dst_len);
    return NGX_OK;
}

static ngx_int_t
ngx_http_upstream_identity_spki_sha256(X509 *cert, char *dst, size_t dst_len)
{
    X509_PUBKEY *pubkey;
    unsigned char *der = NULL;
    unsigned char *cursor;
    unsigned char digest[EVP_MAX_MD_SIZE];
    unsigned int digest_len = 0;
    int der_len;

    if (cert == NULL) {
        return NGX_ERROR;
    }

    pubkey = X509_get_X509_PUBKEY(cert);
    if (pubkey == NULL) {
        return NGX_ERROR;
    }

    der_len = i2d_X509_PUBKEY(pubkey, NULL);
    if (der_len <= 0) {
        return NGX_ERROR;
    }

    der = OPENSSL_malloc((size_t) der_len);
    if (der == NULL) {
        return NGX_ERROR;
    }

    cursor = der;
    if (i2d_X509_PUBKEY(pubkey, &cursor) != der_len
        || EVP_Digest(der, (size_t) der_len, digest, &digest_len,
                      EVP_sha256(), NULL) != 1
        || digest_len != 32)
    {
        OPENSSL_free(der);
        if (dst_len > 0) {
            dst[0] = '\0';
        }
        return NGX_ERROR;
    }

    OPENSSL_free(der);
    ngx_http_upstream_identity_hex(digest, digest_len, dst, dst_len);
    return NGX_OK;
}

static void
ngx_http_upstream_identity_dns_sans(X509 *cert, char *dst, size_t dst_len)
{
    GENERAL_NAMES *names;
    int i;
    size_t used = 0;

    if (dst_len == 0) {
        return;
    }
    dst[0] = '\0';

    names = X509_get_ext_d2i(cert, NID_subject_alt_name, NULL, NULL);
    if (names == NULL) {
        return;
    }

    for (i = 0; i < sk_GENERAL_NAME_num(names); ++i) {
        GENERAL_NAME *name = sk_GENERAL_NAME_value(names, i);
        const unsigned char *data;
        int len;
        size_t j;

        if (name == NULL || name->type != GEN_DNS) {
            continue;
        }

        data = ASN1_STRING_get0_data(name->d.dNSName);
        len = ASN1_STRING_length(name->d.dNSName);
        if (data == NULL || len <= 0) {
            continue;
        }

        if (used != 0 && used + 1 < dst_len) {
            dst[used++] = ',';
        }

        for (j = 0; j < (size_t) len && used + 1 < dst_len; ++j) {
            unsigned char ch = data[j];
            dst[used++] = (ch >= 0x20 && ch <= 0x7e && ch != '"' && ch != '\\')
                ? (char) ch : '_';
        }

        dst[used] = '\0';
    }

    GENERAL_NAMES_free(names);
}
#endif
