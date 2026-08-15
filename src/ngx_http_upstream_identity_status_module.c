#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>

#include "ngx_http_upstream_identity_export.h"
#include "ngx_http_upstream_identity_state.h"

extern ngx_module_t ngx_http_upstream_identity_module;

typedef struct {
    ngx_str_t history_socket;
} ngx_http_upstream_identity_status_main_conf_t;

typedef struct {
    ngx_shm_zone_t *state_zone;
} ngx_http_upstream_identity_status_loc_conf_t;

static void *ngx_http_upstream_identity_status_create_main_conf(ngx_conf_t *cf);
static void *ngx_http_upstream_identity_status_create_loc_conf(ngx_conf_t *cf);
static char *ngx_http_upstream_identity_status_merge_loc_conf(ngx_conf_t *cf,
    void *parent, void *child);
static char *ngx_http_upstream_identity_history_socket(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static char *ngx_http_upstream_identity_status(ngx_conf_t *cf,
    ngx_command_t *cmd, void *conf);
static ngx_int_t ngx_http_upstream_identity_status_handler(
    ngx_http_request_t *r);

static ngx_command_t ngx_http_upstream_identity_status_commands[] = {
    {
        ngx_string("upstream_identity_history_socket"),
        NGX_HTTP_MAIN_CONF | NGX_CONF_TAKE1,
        ngx_http_upstream_identity_history_socket,
        NGX_HTTP_MAIN_CONF_OFFSET,
        0,
        NULL
    },
    {
        ngx_string("upstream_identity_status"),
        NGX_HTTP_LOC_CONF | NGX_CONF_TAKE1,
        ngx_http_upstream_identity_status,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },
    ngx_null_command
};

static ngx_http_module_t ngx_http_upstream_identity_status_module_ctx = {
    NULL,
    NULL,
    ngx_http_upstream_identity_status_create_main_conf,
    NULL,
    NULL,
    NULL,
    ngx_http_upstream_identity_status_create_loc_conf,
    ngx_http_upstream_identity_status_merge_loc_conf
};

ngx_module_t ngx_http_upstream_identity_status_module = {
    NGX_MODULE_V1,
    &ngx_http_upstream_identity_status_module_ctx,
    ngx_http_upstream_identity_status_commands,
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
ngx_http_upstream_identity_status_create_main_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_identity_status_main_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) {
        return NULL;
    }

    return conf;
}

static void *
ngx_http_upstream_identity_status_create_loc_conf(ngx_conf_t *cf)
{
    ngx_http_upstream_identity_status_loc_conf_t *conf;

    conf = ngx_pcalloc(cf->pool, sizeof(*conf));
    if (conf == NULL) {
        return NULL;
    }

    return conf;
}

static char *
ngx_http_upstream_identity_status_merge_loc_conf(ngx_conf_t *cf, void *parent,
    void *child)
{
    ngx_http_upstream_identity_status_loc_conf_t *prev = parent;
    ngx_http_upstream_identity_status_loc_conf_t *conf = child;

    (void) cf;

    if (conf->state_zone == NULL) {
        conf->state_zone = prev->state_zone;
    }

    return NGX_CONF_OK;
}

static char *
ngx_http_upstream_identity_history_socket(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_upstream_identity_status_main_conf_t *mcf = conf;
    ngx_str_t *value;

    (void) cmd;

    if (mcf->history_socket.len != 0) {
        return "is duplicate";
    }

    value = cf->args->elts;

    if (ngx_http_upstream_identity_export_configure(cf->pool, &value[1])
        != NGX_OK)
    {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "invalid upstream identity history socket \"%V\"",
                           &value[1]);
        return NGX_CONF_ERROR;
    }

    mcf->history_socket = value[1];
    return NGX_CONF_OK;
}

static char *
ngx_http_upstream_identity_status(ngx_conf_t *cf, ngx_command_t *cmd,
    void *conf)
{
    ngx_http_upstream_identity_status_loc_conf_t *slcf = conf;
    ngx_http_core_loc_conf_t *clcf;
    ngx_str_t *value;
    ngx_shm_zone_t *zone;

    (void) cmd;

    if (slcf->state_zone != NULL) {
        return "is duplicate";
    }

    value = cf->args->elts;
    zone = ngx_shared_memory_add(cf, &value[1], 0,
                                 &ngx_http_upstream_identity_module);
    if (zone == NULL) {
        return NGX_CONF_ERROR;
    }

    if (zone->shm.size == 0) {
        ngx_conf_log_error(NGX_LOG_EMERG, cf, 0,
                           "upstream identity status references undefined "
                           "state zone \"%V\"; define "
                           "upstream_identity_state_zone first",
                           &value[1]);
        return NGX_CONF_ERROR;
    }

    slcf->state_zone = zone;

    clcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_core_module);
    clcf->handler = ngx_http_upstream_identity_status_handler;

    return NGX_CONF_OK;
}

static ngx_int_t
ngx_http_upstream_identity_status_handler(ngx_http_request_t *r)
{
    ngx_http_upstream_identity_status_loc_conf_t *slcf;
    ngx_str_t json;
    ngx_int_t rc;
    ngx_buf_t *b;
    ngx_chain_t out;

    if (!(r->method & (NGX_HTTP_GET | NGX_HTTP_HEAD))) {
        return NGX_HTTP_NOT_ALLOWED;
    }

    slcf = ngx_http_get_module_loc_conf(
        r, ngx_http_upstream_identity_status_module);
    if (slcf == NULL || slcf->state_zone == NULL) {
        return NGX_HTTP_SERVICE_UNAVAILABLE;
    }

    rc = ngx_http_discard_request_body(r);
    if (rc != NGX_OK) {
        return rc;
    }

    rc = ngx_http_upstream_identity_state_render_json(slcf->state_zone,
                                                       r->pool, &json);
    if (rc == NGX_BUSY) {
        return NGX_HTTP_REQUEST_ENTITY_TOO_LARGE;
    }
    if (rc != NGX_OK) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    r->headers_out.status = NGX_HTTP_OK;
    r->headers_out.content_length_n = (off_t) json.len;
    ngx_str_set(&r->headers_out.content_type, "application/json");

    rc = ngx_http_send_header(r);
    if (rc == NGX_ERROR || rc > NGX_OK || r->header_only) {
        return rc;
    }

    b = ngx_calloc_buf(r->pool);
    if (b == NULL) {
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    b->pos = json.data;
    b->last = json.data + json.len;
    b->memory = 1;
    b->last_buf = 1;

    out.buf = b;
    out.next = NULL;

    return ngx_http_output_filter(r, &out);
}
