#include <ngx_config.h>
#include <ngx_core.h>
#include <ngx_http.h>
#include <zlib.h>

typedef struct {
    ngx_list_part_t *part;
    ngx_uint_t index;
} ngx_rrb_hdr_t;

typedef struct {
    ngx_chain_t *bufs;
    size_t size;
    size_t max_size;
} ngx_rrb_buf_t;

typedef struct {
    ngx_rrb_buf_t buf;
    unsigned char *zbuf;
    size_t zbuf_size;
    ngx_rrb_hdr_t content_encoding;
    int compression_code;
    z_stream z;
} ngx_rrb_zlib_t;

typedef struct {
    ngx_rrb_buf_t raw;

    ngx_rrb_zlib_t zlib;

    unsigned done:1;
} ngx_http_read_request_body_ctx_t;

typedef struct {
    ngx_flag_t read_request_body;
} ngx_http_read_request_body_conf_t;

static const char HDR[] = "content-encoding";
static const size_t zbuf_size = 1 << 15;

static ngx_int_t
ngx_http_read_request_body_init(ngx_conf_t *cf);

static void *
ngx_http_read_request_body_create_cf(ngx_conf_t *cf);

static char *
ngx_http_read_request_body_merge_cf(ngx_conf_t *cf, void *parent, void *child);

static char *
ngx_http_read_request_body(ngx_conf_t *cf, ngx_command_t *cmd, void *conf);

static void ngx_http_read_request_body_post_handler(ngx_http_request_t *r);
static void when_done(ngx_http_request_t *r);
static ngx_int_t flush_body(ngx_http_request_t *r);
static void when_blocked(ngx_http_request_t *r);
static ngx_int_t read_body(ngx_http_request_t *r);
static void on_read(ngx_http_request_t *r);
static ngx_int_t do_read(ngx_http_request_t *r,
                         ngx_http_read_request_body_ctx_t *);
static ngx_int_t save(ngx_http_request_t *, ngx_rrb_buf_t *, ngx_uint_t *bytes);
static ngx_int_t cpy(ngx_http_request_t *, ngx_rrb_buf_t *out,
                     const unsigned char *in, size_t n);
static void finalize(ngx_http_request_t *, ngx_int_t rc);

static ngx_chain_t *make_bufs(ngx_http_request_t *r, size_t);
static size_t get_size(ngx_http_request_t *r, size_t max_size);
static ngx_int_t init_ctx(ngx_http_request_t *r,
                          ngx_http_read_request_body_ctx_t *ctx);
static ngx_int_t init_compression(ngx_http_request_t *r, ngx_rrb_zlib_t *);
static size_t max_body_size(ngx_http_request_t *r);
static void get_content_encoding(ngx_http_request_t *r, ngx_rrb_hdr_t *h);
static int get_compression_code(const ngx_rrb_hdr_t *h);
static void zlib_cleanup(z_stream *z);

static ngx_int_t handle_decompression(ngx_http_request_t *,
                                      ngx_http_read_request_body_ctx_t *);
static ngx_int_t decompression(ngx_http_request_t *, ngx_rrb_zlib_t *,
                               ngx_chain_t *);
static ngx_int_t fix_headers(ngx_http_request_t *, ngx_rrb_zlib_t *zlib);
static void remove_ce(ngx_rrb_hdr_t *h);
static void remove_multiple_hdr(ngx_table_elt_t *e);
static ngx_int_t replace_cl(ngx_http_request_t *r, size_t cl);
static ngx_int_t d2s(ngx_pool_t *p, size_t n, ngx_str_t *s);

static ngx_command_t ngx_http_read_request_body_module_commands[] = {
    {
        ngx_string("read_request_body"),
        NGX_HTTP_SRV_CONF | NGX_HTTP_SIF_CONF | NGX_HTTP_LOC_CONF | NGX_HTTP_LIF_CONF | NGX_CONF_NOARGS,
        ngx_http_read_request_body,
        NGX_HTTP_LOC_CONF_OFFSET,
        0,
        NULL
    },

    ngx_null_command
};

static ngx_http_module_t ngx_http_read_request_body_module_ctx = {
    NULL,                                  /* preconfiguration */
    ngx_http_read_request_body_init,       /* postconfiguration */

    NULL,                                  /* create main configuration */
    NULL,                                  /* init main configuration */

    NULL,                                  /* create server configuration */
    NULL,                                  /* merge server configuration */

    ngx_http_read_request_body_create_cf,  /* create location configuration */
    ngx_http_read_request_body_merge_cf    /* merge location configuration */
};

ngx_module_t ngx_http_read_request_body_module = {
    NGX_MODULE_V1,
    &ngx_http_read_request_body_module_ctx,       /* module context */
    ngx_http_read_request_body_module_commands,   /* module directives */
    NGX_HTTP_MODULE,                              /* module type */
    NULL,                                         /* init master */
    NULL,                                         /* init module */
    NULL,                                         /* init process */
    NULL,                                         /* init thread */
    NULL,                                         /* exit thread */
    NULL,                                         /* exit process */
    NULL,                                         /* exit master */
    NGX_MODULE_V1_PADDING
};

ngx_module_t *ngx_modules[] = {
    &ngx_http_read_request_body_module,
    NULL
};

char *ngx_module_names[] = {
    "ngx_http_read_request_body_module",
    NULL
};

char *ngx_module_order[] = {
    NULL
};

static ngx_int_t
ngx_http_read_request_body_handler(ngx_http_request_t *r)
{
    ngx_int_t                          rc;
    ngx_http_read_request_body_conf_t *rrbcf;
    ngx_http_read_request_body_ctx_t  *ctx;

    ngx_log_debug2(
        NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
        "read request body rewrite handler, uri:\"%V\" c:%ud",
        &r->uri, r->main->count
    );

    if (r->main != r) {
        return NGX_DECLINED;
    }

    rrbcf = ngx_http_get_module_loc_conf(r, ngx_http_read_request_body_module);

    if (!rrbcf->read_request_body) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0,
                       "read_request_body not being used");

        return NGX_DECLINED;
    }

    ctx = ngx_http_get_module_ctx(r, ngx_http_read_request_body_module);

    if (ctx == NULL) {
        ctx = ngx_pcalloc(r->pool, sizeof(ngx_http_read_request_body_ctx_t));
        if (ctx == NULL) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                          "Oom allocating ngx_http_read_request_body context");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        if (init_ctx(r, ctx) != NGX_OK) {
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        ngx_http_set_ctx(r, ctx, ngx_http_read_request_body_module);
    }

    if (!ctx->done) {
        r->request_body_no_buffering = 1;

        rc = ngx_http_read_client_request_body(r, ngx_http_read_request_body_post_handler);

        if (rc == NGX_ERROR) {
            return rc;
        }

        if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
#if (nginx_version < 1002006) ||                                             \
        (nginx_version >= 1003000 && nginx_version < 1003009)
            r->main->count--;
#endif
            return rc;
        }

        return NGX_DONE;
    }

    return NGX_DECLINED;
}

static void
ngx_http_read_request_body_post_handler(ngx_http_request_t *r)
{
    if (r->reading_body) {
        when_blocked(r);
    } else {
        when_done(r);
    }
}

static void
when_done(ngx_http_request_t *r) {
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "when done");

    finalize(r, flush_body(r));
}

static ngx_int_t
flush_body(ngx_http_request_t *r) {
    ngx_http_read_request_body_ctx_t *ctx;
    ngx_int_t rc;
    ngx_uint_t bytes;

    ctx = ngx_http_get_module_ctx(r, ngx_http_read_request_body_module);
    if (!ctx) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "The request context of the "
                      "ngx_http_read_request_body module is null");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    ctx->done = 1;

    rc = save(r, &ctx->raw, &bytes);
    if (rc != NGX_OK) {
        return rc;
    }

    /* the raw body is in ctx->raw.bufs. If no content-length and no chunked
       encoding then ctx->raw.bufs is null. */
    return handle_decompression(r, ctx);
}

static void
when_blocked(ngx_http_request_t *r) {
    ngx_log_debug0(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "when blocked");

    finalize(r, read_body(r));
}

static ngx_int_t
read_body(ngx_http_request_t *r) {
    ngx_http_read_request_body_ctx_t *ctx;
    ngx_int_t rc;
    ngx_uint_t bytes;

    ctx = ngx_http_get_module_ctx(r, ngx_http_read_request_body_module);
    if (!ctx) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "The request context of the "
                      "ngx_http_read_request_body module is null");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    rc = save(r, &ctx->raw, &bytes);
    if (rc != NGX_OK) {
        return rc;
    }

    r->read_event_handler = on_read;

    return do_read(r, ctx);
}

static void
on_read(ngx_http_request_t *r) {
    ngx_http_read_request_body_ctx_t *ctx;

    ctx = ngx_http_get_module_ctx(r, ngx_http_read_request_body_module);
    if (!ctx) {
        ngx_log_error(NGX_LOG_ALERT, r->connection->log, 0,
                      "The request context of the "
                      "ngx_http_read_request_body module is null");
        finalize(r, NGX_HTTP_INTERNAL_SERVER_ERROR);
        return;
    }

    finalize(r, do_read(r, ctx));
}

static ngx_int_t
do_read(ngx_http_request_t *r, ngx_http_read_request_body_ctx_t *ctx) {
    ngx_int_t rc, rc2;
    ngx_uint_t bytes;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "%s", __func__);

    do {
        rc = ngx_http_read_unbuffered_request_body(r);

        if (rc == NGX_ERROR) {
            rc = NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
        if (rc >= NGX_HTTP_SPECIAL_RESPONSE) {
            r->read_event_handler = ngx_http_block_reading;
            ctx->done = 1;
            r->request_body->bufs = ctx->raw.bufs;
            return rc;
        }

        rc2 = save(r, &ctx->raw, &bytes);
        if (rc2 != NGX_OK) {
            r->read_event_handler = ngx_http_block_reading;
            ctx->done = 1;
            r->request_body->bufs = ctx->raw.bufs;
            return rc2;
        }

        /* bytes >= 0 &&
           rc != NGX_ERROR &&
           rc < NGX_HTTP_SPECIAL_RESPONSE(300) */
        if (rc == NGX_OK) {
            r->read_event_handler = ngx_http_block_reading;
            ctx->done = 1;
            /* ctx->raw.bufs cannot be null here, it should contain at least one
               possibly empty buffer with last_buf = 1 */
            return handle_decompression(r, ctx);
        }

        /* bytes >= 0 &&
           rc != NGX_ERROR &&
           rc != NGX_OK &&
           rc < NGX_HTTP_SPECIAL_RESPONSE(300) */
    } while (bytes > 0);
    /* bytes == 0 &&
       rc != NGX_ERROR &&
       rc != NGX_OK &&
       rc < NGX_HTTP_SPECIAL_RESPONSE(300) */

    return NGX_AGAIN;
}

static ngx_int_t
save(ngx_http_request_t *r, ngx_rrb_buf_t *b, ngx_uint_t *bytes) {
    ngx_http_request_body_t *rb;
    ngx_chain_t *cl;

    *bytes = 0;

    rb = r->request_body;

    cl = rb->busy;
    if (!cl) {
        return NGX_OK;
    }

    if (!b->bufs) {
        b->bufs = make_bufs(r, b->max_size);
        if (!b->bufs) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                          "Oom allocating request body buffer chain");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }

    do {
        ngx_buf_t   *src;
        size_t       n;
        ngx_int_t    rc;
        

        src = cl->buf;
        n = src->last - src->pos;

        rc = cpy(r, b, src->pos, n);
        if (rc != NGX_OK) {
            return rc;
        }

        src->pos += n;
        *bytes += n;

        b->bufs->buf->last_buf = cl->buf->last_buf;

        cl = cl->next;
    } while (cl);

    /* cl == NULL */
    ngx_chain_update_chains(r->pool, &rb->free, &rb->busy, &cl,
                            (ngx_buf_tag_t)&ngx_http_read_client_request_body);
    /* rb->busy == NULL */
    rb->bufs = NULL;

    ngx_log_debug1(NGX_LOG_DEBUG_HTTP, r->connection->log, 0, "read request "
                   "body module: %d bytes", *bytes);

    return NGX_OK;
}

static void
finalize(ngx_http_request_t *r, ngx_int_t rc) {
    if (rc == NGX_AGAIN) {
        return;
    }

#if (defined(nginx_version) && nginx_version >= 8011)
    r->main->count--;
#endif

    if (rc != NGX_OK) {
        ngx_http_finalize_request(r, rc);
        return;
    }

    ngx_http_core_run_phases(r);
}

static ngx_int_t
init_ctx(ngx_http_request_t *r, ngx_http_read_request_body_ctx_t *ctx) {
    size_t m = max_body_size(r);

    ctx->raw.max_size = m;
    ctx->zlib.buf.max_size = m;

    get_content_encoding(r, &ctx->zlib.content_encoding);
    if (ctx->zlib.content_encoding.part) {
        ngx_int_t rc = init_compression(r, &ctx->zlib);
        if (rc != NGX_OK) {
            return rc;
        }
    }
    return NGX_OK;
}

static ngx_int_t
init_compression(ngx_http_request_t *r, ngx_rrb_zlib_t *zlib) {
    ngx_pool_cleanup_t *pcb;

    zlib->compression_code = get_compression_code(&zlib->content_encoding);
    if (!zlib->compression_code) {
        ngx_log_error(NGX_LOG_ERR, r->connection->log, NGX_EINVAL,
                      "Unimplemented compression algorithm");
        return NGX_HTTP_UNSUPPORTED_MEDIA_TYPE;
    }

    zlib->zbuf_size = zbuf_size;
    zlib->zbuf = ngx_palloc(r->pool, zlib->zbuf_size);
    if (!zlib->zbuf) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                      "Oom allocating decompression buffer");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    pcb = ngx_pool_cleanup_add(r->pool, 0);
    if (!pcb) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                      "Oom allocating pool cleanup handler");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    memset(&zlib->z, 0, sizeof(zlib->z));
    if (inflateInit2(&zlib->z, zlib->compression_code) != Z_OK) {
        ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                      "Error initializing zstream");
        return NGX_HTTP_INTERNAL_SERVER_ERROR;
    }

    pcb->handler = (ngx_pool_cleanup_pt)&zlib_cleanup;
    pcb->data = &zlib->z;

    return NGX_OK;
}

static size_t
max_body_size(ngx_http_request_t *r) {
    ngx_http_core_loc_conf_t  *clcf;
    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    return clcf->client_max_body_size;
}

static void
get_content_encoding(ngx_http_request_t *r, ngx_rrb_hdr_t *h) {
    ngx_list_part_t *part = &r->headers_in.headers.part;
    do {
        ngx_uint_t i;
        for (i = 0; i < part->nelts; ++i) {
            ngx_table_elt_t *e = (ngx_table_elt_t *)part->elts + i;
            if (e->hash && e->key.len == sizeof(HDR) - 1 &&
                ngx_strncmp(e->lowcase_key, HDR, sizeof(HDR) - 1) == 0) {
                h->part = part;
                h->index = i;
                return;
            }
        }
        part = part->next;
    } while (part != NULL);
}

/* precondition: h->part != NULL */
static int
get_compression_code(const ngx_rrb_hdr_t *hdr) {
    static const char GZIP[] = "gzip";
    static const char DEFLATE[] = "deflate";

    const ngx_table_elt_t *e =
        (const ngx_table_elt_t *)hdr->part->elts + hdr->index;
    const ngx_str_t *h = &e->value;
    if (h->len == sizeof(GZIP) - 1 &&
        ngx_strncasecmp(h->data, (unsigned char *)GZIP,
                        sizeof(GZIP) - 1) == 0) {
        return 31;
    }
    if (h->len == sizeof(DEFLATE) - 1 &&
        ngx_strncasecmp(h->data, (unsigned char *)DEFLATE,
                        sizeof(DEFLATE) - 1) == 0) {
        return -15;
    }
    return 0;
}

static ngx_chain_t *
make_bufs(ngx_http_request_t *r, size_t max_size) {
    size_t s;
    ngx_buf_t *b;
    ngx_chain_t *cl;

    s = get_size(r, max_size);
    b = ngx_create_temp_buf(r->pool, s);
    if (!b) {
        return NULL;
    }

    cl = ngx_alloc_chain_link(r->pool);
    if (!cl) {
        return NULL;
    }

    cl->buf = b;
    cl->next = NULL;

    return cl;
}

static size_t
get_size(ngx_http_request_t *r, size_t max_size) {
    ngx_http_core_loc_conf_t  *clcf;

    if (!r->headers_in.chunked) {
        if (r->headers_in.content_length_n <= 0) {
            return 0;
        }
        /* r->headers_in.content_length_n > 0 */
        if (r->headers_in.content_length_n <= max_size) {
            /* 0 < r->headers_in.content_length_n <= max_size */
            return r->headers_in.content_length_n;
        }
        /* 0        < r->headers_in.content_length_n */
        /* max_size < r->headers_in.content_length_n */
    }

    if (max_size > 0) {
        return max_size;
    }

    clcf = ngx_http_get_module_loc_conf(r, ngx_http_core_module);
    return clcf->client_body_buffer_size;
}

static void
zlib_cleanup(z_stream *z) {
    if (z) {
        inflateEnd(z);
    }
}

static ngx_int_t
handle_decompression(ngx_http_request_t *r,
                     ngx_http_read_request_body_ctx_t *ctx) {
    ngx_int_t rc;

    if (!ctx->zlib.compression_code) {
        r->request_body->bufs = ctx->raw.bufs;
        return NGX_OK;
    }

    rc = decompression(r, &ctx->zlib, ctx->raw.bufs);
    if (rc != NGX_OK) {
        return rc;
    }
    r->request_body->bufs = ctx->zlib.buf.bufs;

    return fix_headers(r, &ctx->zlib);
}

static ngx_int_t
decompression(ngx_http_request_t *r, ngx_rrb_zlib_t *zlib, ngx_chain_t *cl) {
    size_t n;
    int zrc;
    ngx_int_t rc;

    for (;; cl = cl->next) {
        if (!cl) {
            ngx_log_error(NGX_LOG_WARN, r->connection->log, NGX_EINVAL,
                          "Invalid compressed data");
            return NGX_HTTP_BAD_REQUEST;
        }
        if (ngx_buf_size(cl->buf) != 0) {
            break;
        }
    }

    /* cl->buf is not empty */
    zrc = Z_STREAM_END;
    do {
        ngx_buf_t *b = cl->buf;

        zlib->z.next_in = (Bytef *)b->pos;
        zlib->z.avail_in = ngx_buf_size(b);

        do {
            zlib->z.next_out  = (Bytef *)zlib->zbuf;
            zlib->z.avail_out = zlib->zbuf_size;
            zrc = inflate(&zlib->z, Z_NO_FLUSH);
            if (zrc != Z_OK && zrc != Z_STREAM_END) {
                ngx_log_error(NGX_LOG_WARN, r->connection->log, NGX_EINVAL,
                              "Invalid compressed data");
                return NGX_HTTP_BAD_REQUEST;
            }
            /* copy [buf, buf + size - zlib->z.avail_out) to buffer */
            n = zlib->zbuf_size - zlib->z.avail_out;
            if (n > 0) {
                if (!zlib->buf.bufs) {
                    zlib->buf.bufs = make_bufs(r, zlib->buf.max_size);
                    if (!zlib->buf.bufs) {
                        ngx_log_error(NGX_LOG_CRIT, r->connection->log,
                                      NGX_ENOMEM, "Oom allocating request "
                                      "body buffer chain");
                        return NGX_HTTP_INTERNAL_SERVER_ERROR;
                    }
                }

                rc = cpy(r, &zlib->buf, zlib->zbuf, n);
                if (rc != NGX_OK) {
                    return rc;
                }

                zlib->buf.bufs->buf->last_buf = 1;
            }
        } while (zrc == Z_OK && zlib->z.avail_in > 0);
        /* zrc == Z_STREAM_END || zrc == Z_OK && zlib->z.avail_in <= 0 */
        if (zrc == Z_STREAM_END) {
            /* zlib->z.avail_in could be > 0
               even if zlib->z.avail_in == 0 there could be other non-empty
               buffers in the chain => ignore them */
            break;
        }
        /* zrc == ZK_OK && zlib->z.avail.in <= 0 */
        cl = cl->next;
    } while (cl);
    if (zrc != Z_STREAM_END) {
        ngx_log_error(NGX_LOG_WARN, r->connection->log, NGX_EINVAL,
                      "Invalid compressed data");
        return NGX_HTTP_BAD_REQUEST;
    }

    /* zlib->buf.bufs could be NULL if the decompression produces an empty byte
       stream */

    return NGX_OK;
}

static ngx_int_t
cpy(ngx_http_request_t *r, ngx_rrb_buf_t *out,
    const unsigned char *in, size_t n) {

    size_t available;
    ngx_buf_t *dst;

    if (out->max_size > 0) {
        if (n > out->max_size - out->size) {
            return NGX_HTTP_REQUEST_ENTITY_TOO_LARGE;
        }
        /* out->size + n <= out->max_size */
    }

    dst = out->bufs->buf;

    available = dst->end - dst->last;

    if (n > available) {
        ngx_buf_t *tmp;

        size_t already = dst->last - dst->pos;
        size_t required = already + n;
        size_t size = 2 * (dst->end - dst->start);
        if (out->max_size > 0 && size > out->max_size) {
            size = out->max_size;
        }
        if (required > size) {
            size = required;
        }

        tmp = ngx_create_temp_buf(r->pool, size);
        if (!tmp) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                          "Oom reallocating request body buffer");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }

        memcpy(tmp->pos, dst->pos, already);
        ngx_pfree(r->pool, dst->start);
        tmp->last += already;
        dst = tmp;
        out->bufs->buf = dst;
    }

    memcpy(dst->last, in, n);
    dst->last += n;
    out->size += n;

    return NGX_OK;
}

static ngx_int_t
fix_headers(ngx_http_request_t *r, ngx_rrb_zlib_t *zlib) {
    remove_ce(&zlib->content_encoding);
    return replace_cl(r, zlib->buf.size);
}

static void
remove_ce(ngx_rrb_hdr_t *h) {
    ngx_list_part_t *part;
    ngx_table_elt_t *e;
    ngx_uint_t i;

    part = h->part;
    i = h->index;

    remove_multiple_hdr((ngx_table_elt_t *)part->elts + i);

    for (++i; i < part->nelts; ++i) {
        e = (ngx_table_elt_t *)part->elts + i;
        if (e->hash && e->key.len == sizeof(HDR) - 1 &&
            ngx_strncmp(e->lowcase_key, HDR, sizeof(HDR) - 1) == 0) {
            remove_multiple_hdr(e);
        }
    }
    for (part = part->next; part; part = part->next) {
        for (i = 0; i < part->nelts; ++i) {
            e = (ngx_table_elt_t *)part->elts + i;
            if (e->hash && e->key.len == sizeof(HDR) - 1 &&
                ngx_strncmp(e->lowcase_key, HDR, sizeof(HDR) - 1) == 0) {
                remove_multiple_hdr(e);
            }
        }
    }
}

static void
remove_multiple_hdr(ngx_table_elt_t *e) {
    e->hash = 0;
    /*
      The correct approach is to continue like this:
      However there's a bug in nginx that leaves e->next uninitialized.
      So this for-loop would crash.
    */
    /*
    for (e = e->next; e; e = e->next) {
        e->hash = 0;
    }
    */
}

static ngx_int_t
replace_cl(ngx_http_request_t *r, size_t cl) {
    if (r->headers_in.content_length_n < 0) {
        return NGX_OK;
    }

    r->headers_in.content_length_n = cl;
    if (r->headers_in.content_length) {
        if (d2s(r->pool, cl, &r->headers_in.content_length->value) != NGX_OK) {
            ngx_log_error(NGX_LOG_CRIT, r->connection->log, NGX_ENOMEM,
                          "Oom allocating content-length buffer");
            return NGX_HTTP_INTERNAL_SERVER_ERROR;
        }
    }
    return NGX_OK;
}

static ngx_int_t
d2s(ngx_pool_t *p, size_t n, ngx_str_t *s) {
    unsigned char *b, *e;

    b = ngx_palloc(p, NGX_INT_T_LEN);
    if (!b) {
        return NGX_ERROR;
    }
    e = ngx_snprintf(b, NGX_INT_T_LEN, "%uz", n);
    s->len = e - b;
    s->data = b;

    return NGX_OK;
}

static ngx_int_t
ngx_http_read_request_body_init(ngx_conf_t *cf)
{
    ngx_http_handler_pt               *h;
    ngx_http_core_main_conf_t         *cmcf;
    ngx_http_read_request_body_conf_t *rrbcf;

    rrbcf = ngx_http_conf_get_module_loc_conf(cf, ngx_http_read_request_body_module);

    if (!rrbcf->read_request_body) {
        ngx_log_debug0(NGX_LOG_DEBUG_HTTP, cf->log, 0, "read_request_body not being used");

        return NGX_OK;
    }

    cmcf = ngx_http_conf_get_module_main_conf(cf, ngx_http_core_module);

    h = ngx_array_push(&cmcf->phases[NGX_HTTP_REWRITE_PHASE].handlers);
    if (h == NULL) {
        return NGX_ERROR;
    }

    *h = ngx_http_read_request_body_handler;

    return NGX_OK;
}

static void *
ngx_http_read_request_body_create_cf(ngx_conf_t *cf)
{
    ngx_http_read_request_body_conf_t *rrbcf;

    rrbcf = ngx_palloc(cf->pool, sizeof(ngx_http_read_request_body_conf_t));
    if (rrbcf == NULL) {
        return NGX_CONF_ERROR;
    }

    rrbcf->read_request_body = NGX_CONF_UNSET;

    return rrbcf;
}

static char *
ngx_http_read_request_body_merge_cf(ngx_conf_t *cf, void *parent, void *child)
{
    ngx_http_read_request_body_conf_t *prev = parent;
    ngx_http_read_request_body_conf_t *conf = child;

    ngx_conf_merge_value(conf->read_request_body, prev->read_request_body, 0);

    return NGX_CONF_OK;
}


static char *
ngx_http_read_request_body(ngx_conf_t *cf, ngx_command_t *cmd, void *conf)
{
    ngx_http_read_request_body_conf_t *rrbcf = conf;

    rrbcf->read_request_body = 1;

    return NGX_CONF_OK;
}
