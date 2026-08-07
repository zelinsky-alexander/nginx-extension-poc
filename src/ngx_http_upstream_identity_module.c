#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#if (NGX_SSL)
#include <ngx_event_openssl.h>
#include <openssl/evp.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>
#endif

typedef struct {
    ngx_flag_t enabled;
} ngx_http_upstream_identity_loc_conf_t;

static ngx_http_output_header_filter_pt ngx_http_upstream_identity_next_header_filter;

static ngx_int_t ngx_http_upstream_identity_header_filter(ngx_http_request_t *r);
static ngx_int_t ngx_http_upstream_identity_init(ngx_conf_t *cf);
static void *ngx_http_upstream_identity_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_upstream_identity_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);

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
    ngx_null_command
};

static ngx_http_module_t ngx_http_upstream_identity_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_upstream_identity_init,       /* postconfiguration */
    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */
    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */
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
    u_char peer[NGX_SOCKADDR_STRLEN];
    size_t peer_len;

    conf = ngx_http_get_module_loc_conf(r, ngx_http_upstream_identity_module);

    if (conf == NULL || !conf->enabled || r->upstream == NULL) {
        return ngx_http_upstream_identity_next_header_filter(r);
    }

    upstream_connection = r->upstream->peer.connection;
    if (upstream_connection == NULL || upstream_connection->sockaddr == NULL) {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "upstream_identity unavailable reason=peer_connection_missing");
        return ngx_http_upstream_identity_next_header_filter(r);
    }

    peer_len = ngx_sock_ntop(upstream_connection->sockaddr,
                             upstream_connection->socklen,
                             peer, sizeof(peer), 1);
    if (peer_len >= sizeof(peer)) {
        peer_len = sizeof(peer) - 1;
    }
    peer[peer_len] = '\0';

#if (NGX_SSL)
    if (upstream_connection->ssl != NULL
        && upstream_connection->ssl->connection != NULL)
    {
        SSL *ssl = upstream_connection->ssl->connection;
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

        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "upstream_identity peer=\"%s\" tls=\"%s\" alpn=\"%s\" "
                      "cert_sha256=\"%s\" spki_sha256=\"%s\" issuer=\"%s\" "
                      "san_dns=\"%s\"",
                      peer,
                      tls_version != NULL ? tls_version : "",
                      alpn,
                      cert_sha256,
                      spki_sha256,
                      issuer,
                      sans);
    } else {
        ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                      "upstream_identity peer=\"%s\" tls=\"\" reason=not_tls",
                      peer);
    }
#else
    ngx_log_error(NGX_LOG_NOTICE, r->connection->log, 0,
                  "upstream_identity peer=\"%s\" reason=nginx_built_without_ssl",
                  peer);
#endif

    return ngx_http_upstream_identity_next_header_filter(r);
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
