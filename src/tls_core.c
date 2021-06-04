/* Copyright 2021, ISRG (https://www.abetterinternet.org)
 *
 * This software is licensed as described in the file LICENSE, which
 * you should have received as part of this distribution.
 *
 */

#include <assert.h>
#include <apr_lib.h>
#include <apr_strings.h>
#include <apr_network_io.h>

#include <httpd.h>
#include <http_core.h>
#include <http_log.h>
#include <http_protocol.h>
#include <http_ssl.h>
#include <http_vhost.h>
#include <http_main.h>
#include <ap_socache.h>

#include <crustls.h>

#include "tls_proto.h"
#include "tls_cert.h"
#include "tls_conf.h"
#include "tls_core.h"
#include "tls_ocsp.h"
#include "tls_util.h"
#include "tls_cache.h"
#include "tls_var.h"


extern module AP_MODULE_DECLARE_DATA tls_module;
APLOG_USE_MODULE(tls);

tls_conf_conn_t *tls_conf_conn_get(conn_rec *c)
{
    return ap_get_module_config(c->conn_config, &tls_module);
}

void tls_conf_conn_set(conn_rec *c, tls_conf_conn_t *cc)
{
    ap_set_module_config(c->conn_config, &tls_module, cc);
}

int tls_conn_check_ssl(conn_rec *c)
{
    tls_conf_conn_t *cc = tls_conf_conn_get(c->master? c->master : c);
    if (TLS_CONN_ST_IS_ENABLED(cc)) {
        return OK;
    }
    return DECLINED;
}

static int we_listen_on(tls_conf_global_t *gc, server_rec *s, tls_conf_server_t *sc)
{
    server_addr_rec *sa, *la;

    if (gc->tls_addresses && sc->base_server) {
        /* The base server listens to every port and may be selected via SNI */
        return 1;
    }
    for (la = gc->tls_addresses; la; la = la->next) {
        for (sa = s->addrs; sa; sa = sa->next) {
            if (la->host_port == sa->host_port
                && la->host_addr->ipaddr_len == sa->host_addr->ipaddr_len
                && !memcmp(la->host_addr->ipaddr_ptr,
                    la->host_addr->ipaddr_ptr, (size_t)la->host_addr->ipaddr_len)) {
                /* exact match */
                return 1;
            }
        }
    }
    return 0;
}

static apr_status_t tls_core_free(void *data)
{
    server_rec *base_server = (server_rec *)data;
    tls_conf_server_t *sc = tls_conf_server_get(base_server);
    server_rec *s;

    if (sc && sc->global && sc->global->rustls_hello_config) {
        rustls_server_config_free(sc->global->rustls_hello_config);
        sc->global->rustls_hello_config = NULL;
    }
    tls_cache_free(base_server);

    /* free all rustls things we are owning. */
    for (s = base_server; s; s = s->next) {
        sc = tls_conf_server_get(s);
        if (sc) {
            if (sc->rustls_config) {
                rustls_server_config_free(sc->rustls_config);
                sc->rustls_config = NULL;
            }
        }
    }
    return APR_SUCCESS;
}

static apr_status_t load_certified_keys(
    tls_conf_server_t *sc, server_rec *s,
    apr_array_header_t *cert_specs,
    tls_cert_reg_t *cert_reg)
{
    apr_status_t rv = APR_SUCCESS;
    const rustls_certified_key *ckey;
    tls_cert_spec_t *spec;
    int i;

    if (cert_specs && cert_specs->nelts > 0) {
        for (i = 0; i < cert_specs->nelts; ++i) {
            spec = APR_ARRAY_IDX(cert_specs, i, tls_cert_spec_t*);
            rv = tls_cert_reg_get_certified_key(cert_reg, s, spec, &ckey);
            if (APR_SUCCESS != rv) {
                ap_log_error(APLOG_MARK, APLOG_ERR, rv, s, APLOGNO()
                     "Failed to load certificate %d[cert=%s(%d), key=%s(%d)] for %s",
                     i, spec->cert_file, (int)(spec->cert_pem? strlen(spec->cert_pem) : 0),
                     spec->pkey_file, (int)(spec->pkey_pem? strlen(spec->pkey_pem) : 0),
                     s->server_hostname);
                goto cleanup;
            }
            assert(ckey);
            APR_ARRAY_PUSH(sc->certified_keys, const rustls_certified_key*) = ckey;
        }

    }
cleanup:
    return rv;
}

static apr_status_t use_local_key(
    conn_rec *c, const char *cert_pem, const char *pkey_pem)
{
    tls_conf_conn_t *cc = tls_conf_conn_get(c);
    const rustls_certified_key *ckey = NULL;
    tls_cert_spec_t spec;
    apr_status_t rv = APR_SUCCESS;

    memset(&spec, 0, sizeof(spec));
    spec.cert_pem = cert_pem;
    spec.pkey_pem = pkey_pem;
    rv = tls_cert_load_cert_key(c->pool, &spec, NULL, &ckey);
    if (APR_SUCCESS != rv) goto cleanup;

    cc->local_keys = apr_array_make(c->pool, 2, sizeof(const rustls_certified_key*));
    APR_ARRAY_PUSH(cc->local_keys, const rustls_certified_key*) = ckey;
    ckey = NULL;

cleanup:
    if (ckey != NULL) rustls_certified_key_free(ckey);
    return rv;
}

static void add_file_specs(
    apr_array_header_t *certificates,
    apr_pool_t *p,
    apr_array_header_t *cert_files,
    apr_array_header_t *key_files)
{
    tls_cert_spec_t *spec;
    int i;

    for (i = 0; i < cert_files->nelts; ++i) {
        spec = apr_pcalloc(p, sizeof(*spec));
        spec->cert_file = APR_ARRAY_IDX(cert_files, i, const char*);
        spec->pkey_file = (i < key_files->nelts)? APR_ARRAY_IDX(key_files, i, const char*) : NULL;
        *(const tls_cert_spec_t**)apr_array_push(certificates) = spec;
    }
}

static apr_status_t set_ciphers(
    apr_pool_t *pool, tls_conf_server_t *sc,
    rustls_server_config_builder *builder)
{
    apr_array_header_t *ordered_ciphers;
    const apr_array_header_t *ciphers;
    apr_array_header_t *unsupported = NULL;
    rustls_result rr = RUSTLS_RESULT_OK;
    apr_status_t rv = APR_SUCCESS;
    apr_uint16_t id;
    int i;


    /* remove all suppressed ciphers from the ones supported by rustls */
    ciphers = tls_util_array_uint16_remove(pool,
        sc->global->proto->supported_cipher_ids, sc->tls_supp_ciphers);
    ordered_ciphers = NULL;
    /* if preferred ciphers are actually still present in allowed_ciphers, put
     * them into `ciphers` in this order */
    for (i = 0; i < sc->tls_pref_ciphers->nelts; ++i) {
        id = APR_ARRAY_IDX(sc->tls_pref_ciphers, i, apr_uint16_t);
        ap_log_error(APLOG_MARK, APLOG_TRACE4, rv, sc->server,
                     "checking preferred cipher %s: %d",
                     sc->server->server_hostname, id);
        if (tls_util_array_uint16_contains(ciphers, id)) {
            ap_log_error(APLOG_MARK, APLOG_TRACE4, rv, sc->server,
                         "checking preferred cipher %s: %d is known",
                         sc->server->server_hostname, id);
            if (ordered_ciphers == NULL) {
                ordered_ciphers = apr_array_make(pool, ciphers->nelts, sizeof(apr_uint16_t));
            }
            APR_ARRAY_PUSH(ordered_ciphers, apr_uint16_t) = id;
        }
        else if (!tls_proto_is_cipher_supported(sc->global->proto, id)) {
            ap_log_error(APLOG_MARK, APLOG_TRACE4, rv, sc->server,
                         "checking preferred cipher %s: %d is unsupported",
                         sc->server->server_hostname, id);
            if (!unsupported) unsupported = apr_array_make(pool, 5, sizeof(apr_uint16_t));
            APR_ARRAY_PUSH(unsupported, apr_uint16_t) = id;
        }
    }
    /* if we found ciphers with preference among allowed_ciphers,
     * append all other allowed ciphers. */
    if (ordered_ciphers) {
        for (i = 0; i < ciphers->nelts; ++i) {
            id = APR_ARRAY_IDX(ciphers, i, apr_uint16_t);
            if (!tls_util_array_uint16_contains(ordered_ciphers, id)) {
                APR_ARRAY_PUSH(ordered_ciphers, apr_uint16_t) = id;
            }
        }
        ciphers = ordered_ciphers;
    }

    if (ciphers != sc->global->proto->supported_cipher_ids) {
        apr_array_header_t *suites = tls_proto_get_rustls_suites(
            sc->global->proto, ciphers, pool);
        /* this changed the default rustls ciphers, configure it. */
        if (APLOGtrace2(sc->server)) {
            tls_proto_conf_t *conf = sc->global->proto;
            ap_log_error(APLOG_MARK, APLOG_TRACE2, 0, sc->server,
                         "tls ciphers configured[%s]: %s",
                         sc->server->server_hostname,
                         tls_proto_get_cipher_names(conf, ciphers, pool));
        }
        rr = rustls_server_config_builder_set_ciphersuites(builder,
            (const rustls_supported_ciphersuite* const*)suites->elts,
            (apr_size_t)suites->nelts);
        if (RUSTLS_RESULT_OK != rr) goto cleanup;
    }

    if (unsupported && unsupported->nelts) {
        ap_log_error(APLOG_MARK, APLOG_WARNING, rv, sc->server, APLOGNO()
                     "Server '%s' has TLSCiphersPrefer configured that are not "
                     "supported by rustls. These will not have an effect: %s",
                     sc->server->server_hostname,
                     tls_proto_get_cipher_names(sc->global->proto, unsupported, pool));
    }

cleanup:
    if (RUSTLS_RESULT_OK != rr) {
        const char *err_descr;
        rv = tls_util_rustls_error(pool, rr, &err_descr);
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, sc->server, APLOGNO()
                     "Failed to configure ciphers %s: [%d] %s",
                     sc->server->server_hostname, (int)rr, err_descr);
    }
    return rv;
}

static apr_array_header_t *complete_cert_specs(
    apr_pool_t *p, tls_conf_server_t *sc)
{
    apr_array_header_t *cert_adds, *key_adds, *specs;

    /* Take the configured certificate specifications and ask
     * around for other modules to add specifications to this server.
     * This is the way mod_md provides certificates.
     *
     * If the server then still has no cert specifications, ask
     * around for `fallback` certificates which are commonly self-signed,
     * temporary ones which let the server startup in order to
     * obtain the `real` certificates from sources like ACME.
     * Servers will fallbacks will answer all requests with 503.
     */
    specs = apr_array_copy(p, sc->cert_specs);
    cert_adds = apr_array_make(p, 2, sizeof(const char*));
    key_adds = apr_array_make(p, 2, sizeof(const char*));

    ap_ssl_add_cert_files(sc->server, p, cert_adds, key_adds);
    ap_log_error(APLOG_MARK, APLOG_TRACE1, 0, sc->server,
                 "init server: complete_cert_specs added %d certs", cert_adds->nelts);
    add_file_specs(specs, p, cert_adds, key_adds);

    if (apr_is_empty_array(specs)) {
        ap_log_error(APLOG_MARK, APLOG_TRACE1, 0, sc->server,
                     "init server: no certs configured, looking for fallback");
        ap_ssl_add_fallback_cert_files(sc->server, p, cert_adds, key_adds);
        if (cert_adds->nelts > 0) {
            add_file_specs(specs, p, cert_adds, key_adds);
            sc->service_unavailable = 1;
            ap_log_error(APLOG_MARK, APLOG_WARNING, 0, sc->server, APLOGNO()
                         "Init: %s will respond with '503 Service Unavailable' for now. There "
                         "are no SSL certificates configured and no other module contributed any.",
                         sc->server->server_hostname);
        }
        else if (!sc->base_server) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, sc->server, APLOGNO()
                         "Init: %s has no certificates configured. Use 'TLSCertificate' to "
                         "configure a certificate and key file.",
                         sc->server->server_hostname);
        }
    }
    return specs;
}

static const rustls_certified_key *select_certified_key(
    void* userdata, const rustls_client_hello *hello)
{
    conn_rec *c = userdata;
    tls_conf_conn_t *cc = tls_conf_conn_get(c);
    tls_conf_server_t *sc;
    apr_array_header_t *keys;
    const rustls_certified_key *clone;
    rustls_result rr = RUSTLS_RESULT_OK;
    apr_status_t rv;

    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, c, "client hello select certified key");
    if (!cc || !cc->server) goto cleanup;
    sc = tls_conf_server_get(cc->server);
    if (!sc) goto cleanup;

    cc->key = NULL;
    cc->key_cloned = 0;
    if (cc->local_keys && cc->local_keys->nelts > 0) {
        keys = cc->local_keys;
    }
    else {
        keys = sc->certified_keys;
    }
    if (!keys || keys->nelts <= 0) goto cleanup;

    rr = rustls_client_hello_select_certified_key(hello,
        (const rustls_certified_key**)keys->elts, (size_t)keys->nelts, &cc->key);
    if (RUSTLS_RESULT_OK != rr) goto cleanup;

    if (APR_SUCCESS == tls_ocsp_update_key(c, cc->key, &clone)) {
        /* got OCSP response data for it, meaning the key was cloned and we need to remember */
        cc->key_cloned = 1;
        cc->key = clone;
    }
    if (APLOGctrace2(c)) {
        const char *key_id = tls_cert_reg_get_id(sc->global->cert_reg, cc->key);
        ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, c, APLOGNO()
                      "client hello selected key: %s", key_id? key_id : "unknown");
    }
    return cc->key;

cleanup:
    if (RUSTLS_RESULT_OK != rr) {
        const char *err_descr;
        rv = tls_util_rustls_error(c->pool, rr, &err_descr);
        ap_log_cerror(APLOG_MARK, APLOG_ERR, rv, c, APLOGNO()
                      "Failed to select certified key: [%d] %s", (int)rr, err_descr);
    }
    return NULL;
}

static apr_status_t server_conf_setup(
    apr_pool_t *p, apr_pool_t *ptemp, tls_conf_server_t *sc, tls_cert_reg_t *cert_reg)
{
    rustls_server_config_builder *builder = NULL;
    rustls_root_cert_store *ca_store = NULL;
    apr_array_header_t *cert_specs;
    rustls_result rr = RUSTLS_RESULT_OK;
    apr_status_t rv = APR_SUCCESS;

    (void)p;
    ap_log_error(APLOG_MARK, APLOG_TRACE1, rv, sc->server,
                 "init server: %s", sc->server->server_hostname);
    if (sc->client_auth != TLS_CLIENT_AUTH_NONE) {
        if (!sc->client_ca) {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, sc->server, APLOGNO()
                         "TLSClientAuthentication is enabled for %s, but no client CA file is set. "
                          "Use 'TLSClientCA <file>' to specify the trust anchors.",
                         sc->server->server_hostname);
            rv = APR_EINVAL; goto cleanup;
        }

        if (sc->client_auth == TLS_CLIENT_AUTH_REQUIRED) {
            const rustls_client_cert_verifier *verifier;
            rv = tls_cert_client_verifiers_get(sc->global->verifiers, sc->client_ca, &verifier);
            if (APR_SUCCESS != rv) goto cleanup;
            builder = rustls_server_config_builder_with_client_verifier(verifier);
        }
        else {
            const rustls_client_cert_verifier_optional *verifier;
            rv = tls_cert_client_verifiers_get_optional(sc->global->verifiers, sc->client_ca, &verifier);
            if (APR_SUCCESS != rv) goto cleanup;
            builder = rustls_server_config_builder_with_client_verifier_optional(verifier);
        }
    }
    else {
        builder = rustls_server_config_builder_new();
    }

    if (!builder) {
        rv = APR_ENOMEM; goto cleanup;
    }

    cert_specs = complete_cert_specs(ptemp, sc);
    sc->certified_keys = apr_array_make(p, 3, sizeof(rustls_certified_key *));
    rv = load_certified_keys(sc, sc->server, cert_specs, cert_reg);
    if (APR_SUCCESS != rv) goto cleanup;
    ap_log_error(APLOG_MARK, APLOG_TRACE1, rv, sc->server,
                 "init server: %s with %d certificates loaded",
                 sc->server->server_hostname, sc->certified_keys->nelts);

    rustls_server_config_builder_set_hello_callback(builder, select_certified_key);

    if (sc->tls_protocol_min > 0) {
        apr_array_header_t *tls_versions;

        ap_log_error(APLOG_MARK, APLOG_TRACE1, rv, sc->server,
                     "init server: set protocol min version %04x", sc->tls_protocol_min);
        tls_versions = tls_proto_create_versions_plus(
            sc->global->proto, (apr_uint16_t)sc->tls_protocol_min, ptemp);
        if (tls_versions->nelts > 0) {
            rr = rustls_server_config_builder_set_versions(builder,
                (const apr_uint16_t*)tls_versions->elts, (apr_size_t)tls_versions->nelts);
            if (RUSTLS_RESULT_OK != rr) goto cleanup;
            if (sc->tls_protocol_min != APR_ARRAY_IDX(tls_versions, 0, apr_uint16_t)) {
                ap_log_error(APLOG_MARK, APLOG_WARNING, 0, sc->server, APLOGNO()
                             "Init: the minimum protocol version configured for %s (%04x) "
                             "is not supported and version %04x was selected instead.",
                             sc->server->server_hostname, sc->tls_protocol_min,
                             APR_ARRAY_IDX(tls_versions, 0, apr_uint16_t));
            }
        }
        else {
            ap_log_error(APLOG_MARK, APLOG_ERR, 0, sc->server, APLOGNO()
                         "Unable to configure the protocol version for %s: "
                          "neither the configured minimum version (%04x), nor any higher one is "
                         "available.", sc->server->server_hostname, sc->tls_protocol_min);
            rv = APR_ENOTIMPL; goto cleanup;
        }
    }

    rv = set_ciphers(ptemp, sc, builder);
    if (APR_SUCCESS != rv) goto cleanup;

    rr = rustls_server_config_builder_set_ignore_client_order(
        builder, sc->honor_client_order == TLS_FLAG_FALSE);
    if (RUSTLS_RESULT_OK != rr) goto cleanup;

    /* whatever we negotiate later on a connection, the base we start with is http/1.1 */
    if (1) {
        rustls_slice_bytes rsb = {
            (const unsigned char*)"http/1.1",
            sizeof("http/1.1")-1,
        };
        rr = rustls_server_config_builder_set_protocols(builder, &rsb, 1);
        if (RUSTLS_RESULT_OK != rr) goto cleanup;
    }

    rv = tls_cache_init_server(builder, sc->server);
    if (APR_SUCCESS != rv) goto cleanup;

    sc->rustls_config = rustls_server_config_builder_build(builder);
    builder = NULL;
    if (!sc->rustls_config) {
        rv = APR_ENOMEM; goto cleanup;
    }

cleanup:
    if (builder) rustls_server_config_builder_free(builder);
    if (ca_store) rustls_root_cert_store_free(ca_store);
    if (RUSTLS_RESULT_OK != rr) {
        const char *err_descr;
        rv = tls_util_rustls_error(ptemp, rr, &err_descr);
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, sc->server, APLOGNO()
                     "Failed to configure server %s: [%d] %s",
                     sc->server->server_hostname, (int)rr, err_descr);
        goto cleanup;
    }
    return rv;
}

static const rustls_certified_key *extract_client_hello_values(
    void* userdata, const rustls_client_hello *hello)
{
    conn_rec *c = userdata;
    tls_conf_conn_t *cc = tls_conf_conn_get(c);
    size_t i, len;
    unsigned short n;

    ap_log_cerror(APLOG_MARK, APLOG_TRACE2, 0, c, "extractr client hello values");
    if (!cc) goto cleanup;
    cc->client_hello_seen = 1;
    if (hello->sni_name.len > 0) {
        cc->sni_hostname = apr_pstrndup(c->pool, hello->sni_name.data, hello->sni_name.len);
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c, "sni detected: %s", cc->sni_hostname);
    }
    else {
        cc->sni_hostname = NULL;
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c, "no sni from client");
    }
    if (APLOGctrace4(c) && hello->signature_schemes.len > 0) {
        for (i = 0; i < hello->signature_schemes.len; ++i) {
            n = hello->signature_schemes.data[i];
            ap_log_cerror(APLOG_MARK, APLOG_TRACE4, 0, c,
                "client supports signature scheme: %x", (int)n);
        }
    }
    if ((len = rustls_slice_slice_bytes_len(hello->alpn)) > 0) {
        apr_array_header_t *alpn = apr_array_make(c->pool, 5, sizeof(const char*));
        const char *protocol;
        ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c, "ALPN: client proposes %d protocols", (int)len);
        for (i = 0; i < len; ++i) {
            rustls_slice_bytes rs = rustls_slice_slice_bytes_get(hello->alpn, i);
            protocol = apr_pstrndup(c->pool, (const char*)rs.data, rs.len);
            APR_ARRAY_PUSH(alpn, const char*) = protocol;
            ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c,
                "ALPN: client proposes `%s`", protocol);
        }
        cc->alpn = alpn;
    }
cleanup:
    return NULL;
}

static apr_status_t setup_hello_config(apr_pool_t *p, server_rec *base_server, tls_conf_global_t *gc)
{
    rustls_server_config_builder *builder;
    rustls_result rr = RUSTLS_RESULT_OK;
    apr_status_t rv = APR_SUCCESS;

    builder = rustls_server_config_builder_new();
    if (!builder) {
        rr = RUSTLS_RESULT_PANIC; goto cleanup;
    }
    rustls_server_config_builder_set_hello_callback(builder, extract_client_hello_values);
    gc->rustls_hello_config = rustls_server_config_builder_build(builder);
    if (!gc->rustls_hello_config) {
        rr = RUSTLS_RESULT_PANIC; goto cleanup;
    }

cleanup:
    if (RUSTLS_RESULT_OK != rr) {
        const char *err_descr = NULL;
        rv = tls_util_rustls_error(p, rr, &err_descr);
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, base_server, APLOGNO()
                     "Failed to init generic hello config: [%d] %s", (int)rr, err_descr);
        goto cleanup;
    }
    return rv;
}

apr_status_t tls_core_init(apr_pool_t *p, apr_pool_t *ptemp, server_rec *base_server)
{
    tls_conf_server_t *sc = tls_conf_server_get(base_server);
    tls_conf_global_t *gc = sc->global;
    server_rec *s;
    apr_status_t rv = APR_ENOMEM;

    ap_log_error(APLOG_MARK, APLOG_TRACE2, 0, base_server, "tls_core_init");
    apr_pool_cleanup_register(p, base_server, tls_core_free,
                              apr_pool_cleanup_null);

    rv = tls_proto_post_config(p, ptemp, base_server);
    if (APR_SUCCESS != rv) goto cleanup;

    for (s = base_server; s; s = s->next) {
        sc = tls_conf_server_get(s);
        assert(sc);
        ap_assert(sc->global == gc);

        /* If 'TLSListen' has been configured, use those addresses to
         * decide if we are enabled on this server. */
        sc->base_server = (s == base_server);
        sc->enabled = we_listen_on(gc, s, sc)? TLS_FLAG_TRUE : TLS_FLAG_FALSE;
    }

    rv = tls_cache_post_config(p, ptemp, base_server);
    if (APR_SUCCESS != rv) goto cleanup;

    rv = setup_hello_config(p, base_server, gc);
    if (APR_SUCCESS != rv) goto cleanup;

    /* Setup server configs and collect all certificates we use. */
    gc->cert_reg = tls_cert_reg_make(p);
    gc->stores = tls_cert_root_stores_make(p);
    gc->verifiers = tls_cert_verifiers_make(p, gc->stores);
    for (s = base_server; s; s = s->next) {
        sc = tls_conf_server_get(s);
        rv = tls_conf_server_apply_defaults(sc, p);
        if (APR_SUCCESS != rv) goto cleanup;
        if (sc->enabled != TLS_FLAG_TRUE) continue;
        rv = server_conf_setup(p, ptemp, sc, gc->cert_reg);
        if (APR_SUCCESS != rv) {
            ap_log_error(APLOG_MARK, APLOG_ERR, rv, s, "server setup failed: %s",
                s->server_hostname);
            goto cleanup;
        }
    }

    /* register all loaded certificates for OCSP stapling */
    rv = tls_ocsp_prime_certs(gc, p, base_server);
    if (APR_SUCCESS != rv) goto cleanup;

cleanup:
    if (gc->verifiers) tls_cert_verifiers_clear(gc->verifiers);
    if (gc->stores) tls_cert_root_stores_clear(gc->stores);
    if (APR_SUCCESS != rv) {
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, base_server, "error during post_config");
    }
    return rv;
}


static apr_status_t tls_core_conn_free(void *data)
{
    tls_conf_conn_t *cc = data;

    /* free all rustls things we are owning. */
    if (cc->rustls_connection) {
        rustls_connection_free(cc->rustls_connection);
        cc->rustls_connection = NULL;
    }
    if (cc->key_cloned && cc->key) {
        rustls_certified_key_free(cc->key);
        cc->key = NULL;
    }
    if (cc->local_keys) {
        const rustls_certified_key *key;
        int i;

        for (i = 0; i < cc->local_keys->nelts; ++i) {
            key = APR_ARRAY_IDX(cc->local_keys, i, const rustls_certified_key*);
            rustls_certified_key_free(key);
        }
        apr_array_clear(cc->local_keys);
    }
    return APR_SUCCESS;
}

static tls_conf_conn_t *cc_get_or_make(conn_rec *c)
{
    tls_conf_conn_t *cc = tls_conf_conn_get(c);
    if (!cc) {
        cc = apr_pcalloc(c->pool, sizeof(*cc));
        cc->server = c->base_server;
        cc->state = TLS_CONN_ST_INIT;
        tls_conf_conn_set(c, cc);
        apr_pool_cleanup_register(c->pool, cc, tls_core_conn_free,
                                  apr_pool_cleanup_null);
    }
    return cc;
}

void tls_core_conn_disable(conn_rec *c)
{
    tls_conf_conn_t *cc;
    cc = cc_get_or_make(c);
    if (cc->state == TLS_CONN_ST_INIT) {
        cc->state = TLS_CONN_ST_DISABLED;
    }
}

void tls_core_conn_bind(conn_rec *c, ap_conf_vector_t *dir_conf)
{
    tls_conf_conn_t *cc = cc_get_or_make(c);
    cc->dc = dir_conf? ap_get_module_config(dir_conf, &tls_module) : NULL;
}


int tls_core_conn_init(conn_rec *c)
{
    tls_conf_server_t *sc = tls_conf_server_get(c->base_server);
    tls_conf_conn_t *cc;
    rustls_result rr = RUSTLS_RESULT_OK;

    cc = cc_get_or_make(c);
    if (cc->state == TLS_CONN_ST_INIT) {
        /* Need to decide if we TLS this connection or not */
        int enabled =
#if AP_MODULE_MAGIC_AT_LEAST(20210531, 0)
                !c->outgoing &&
#endif
                sc->enabled == TLS_FLAG_TRUE;
        cc->state = enabled? TLS_CONN_ST_PRE_HANDSHAKE : TLS_CONN_ST_DISABLED;
        ap_log_error(APLOG_MARK, APLOG_TRACE3, 0, c->base_server,
            "tls_core_conn_init: %s for tls: %s",
            enabled? "enabled" : "disabled", c->base_server->server_hostname);
    }
    else if (cc->state == TLS_CONN_ST_DISABLED) {
        ap_log_error(APLOG_MARK, APLOG_TRACE4, 0, c->base_server,
            "tls_core_conn_init, not our connection: %s",
            c->base_server->server_hostname);
        goto cleanup;
    }

    if (TLS_CONN_ST_IS_ENABLED(cc) && !cc->rustls_connection) {
        /* Use a generic rustls_connection with its defaults, which we feed
         * the first TLS bytes from the client. Its Hello message will trigger
         * our callback where we can inspect the (possibly) supplied SNI and
         * select another server.
         */
        rr = rustls_server_connection_new(sc->global->rustls_hello_config, &cc->rustls_connection);
        if (RUSTLS_RESULT_OK != rr) goto cleanup;
        rustls_connection_set_userdata(cc->rustls_connection, c);
        /* we might refuse requests on this connection, e.g. ACME challenge */
        cc->service_unavailable = sc->service_unavailable;
    }

cleanup:
    if (RUSTLS_RESULT_OK != rr) {
        const char *err_descr = NULL;
        apr_status_t rv = tls_util_rustls_error(c->pool, rr, &err_descr);
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, sc->server, APLOGNO()
                     "Failed to init pre_session for server %s: [%d] %s",
                     sc->server->server_hostname, (int)rr, err_descr);
        c->aborted = 1;
        cc->state = TLS_CONN_ST_DISABLED;
        goto cleanup;
    }
    return TLS_CONN_ST_IS_ENABLED(cc)? OK : DECLINED;
}

static int find_vhost(void *sni_hostname, conn_rec *c, server_rec *s)
{
    if (tls_util_name_matches_server(sni_hostname, s)) {
        tls_conf_conn_t *cc = tls_conf_conn_get(c);
        cc->server = s;
        return 1;
    }
    return 0;
}

static apr_status_t select_application_protocol(
    conn_rec *c, server_rec *s, rustls_server_config_builder *builder)
{
    tls_conf_conn_t *cc = tls_conf_conn_get(c);
    const char *proposed;
    rustls_result rr = RUSTLS_RESULT_OK;
    apr_status_t rv = APR_SUCCESS;

    /* The server always has a protocol it uses, normally "http/1.1".
     * if the client, via ALPN, proposes protocols, they are in
     * order of preference.
     * We propose those to modules registered in the server and
     * get the protocol back that someone is willing to run on this
     * connection.
     * If this is different from what the connection already does,
     * we tell the server (and all protocol modules) to switch.
     * If successful, we announce that protocol back to the client as
     * our only ALPN protocol and then do the 'real' handshake.
     */
    cc->application_protocol = ap_get_protocol(c);
    if (cc->alpn && cc->alpn->nelts > 0
        && (proposed = ap_select_protocol(c, NULL, s, cc->alpn))
        && strcmp(proposed, cc->application_protocol)) {
        rustls_slice_bytes rsb;

        ap_log_cerror(APLOG_MARK, APLOG_TRACE2, rv, c,
            "ALPN: switching protocol from `%s` to `%s`", cc->application_protocol, proposed);
        rv = ap_switch_protocol(c, NULL, cc->server, proposed);
        if (APR_SUCCESS != rv) goto cleanup;

        rsb.data = (const unsigned char*)proposed;
        rsb.len = strlen(proposed);
        rr = rustls_server_config_builder_set_protocols(builder, &rsb, 1);
        if (RUSTLS_RESULT_OK != rr) goto cleanup;

        cc->application_protocol = proposed;
        ap_log_cerror(APLOG_MARK, APLOG_TRACE2, rv, c,
            "ALPN: switched connection to protocol `%s`", cc->application_protocol);

        /* protocol was switched, this could be a challenge protocol
         * such as "acme-tls/1". Give handlers the opportunity to
         * override the certificate for this connection. */
        if (strcmp("h2", proposed) && strcmp("http/1.1", proposed)) {
            const char *cert_pem = NULL, *key_pem = NULL;
            if (ap_ssl_answer_challenge(c, cc->sni_hostname, &cert_pem, &key_pem)) {
                /* With ACME we can have challenge connections to a unknown domains
                 * that need to be answered with a special certificate and will
                 * otherwise not answer any requests. See RFC 8555 */
                rv = use_local_key(c, cert_pem, key_pem);
                if (APR_SUCCESS != rv) goto cleanup;

                cc->service_unavailable = 1;
                /* TODO: SSL_set_verify(ssl, SSL_VERIFY_NONE, ssl_callback_SSLVerify); */
            }
        }
    }
cleanup:
    if (rr != RUSTLS_RESULT_OK) {
        const char *err_descr = NULL;
        rv = tls_util_rustls_error(c->pool, rr, &err_descr);
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, s, APLOGNO()
                     "Failed to init session for server %s: [%d] %s",
                     s->server_hostname, (int)rr, err_descr);
        c->aborted = 1;
        goto cleanup;
    }
    return rv;
}

apr_status_t tls_core_conn_seen_client_hello(conn_rec *c)
{
    tls_conf_conn_t *cc = tls_conf_conn_get(c);
    tls_conf_server_t *sc, *initial_sc;
    rustls_server_config_builder *builder = NULL;
    const rustls_server_config *config = NULL;
    rustls_result rr = RUSTLS_RESULT_OK;
    apr_status_t rv = APR_SUCCESS;
    int sni_match = 0;

    /* The initial rustls generic session has been fed the client hello and
     * we have extraced SNI and ALPN values (so present).
     * Time to select the actual server_rec and application protocol that
     * will be used on this connection. */
    ap_assert(cc);
    initial_sc = sc = tls_conf_server_get(cc->server);
    if (!cc->client_hello_seen) goto cleanup;

    if (cc->sni_hostname) {
        if (ap_vhost_iterate_given_conn(c, find_vhost, (void*)cc->sni_hostname)) {
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, rv, c, APLOGNO()
                "vhost_init: virtual host found for SNI '%s'", cc->sni_hostname);
            sni_match = 1;
        }
        else if (tls_util_name_matches_server(cc->sni_hostname, ap_server_conf)) {
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, rv, c, APLOGNO()
                "vhost_init: virtual host NOT found, but base server[%s] matches SNI '%s'",
                ap_server_conf->server_hostname, cc->sni_hostname);
            cc->server = ap_server_conf;
            sni_match = 1;
        }
        else if (sc->strict_sni == TLS_FLAG_FALSE) {
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, rv, c, APLOGNO()
                "vhost_init: no virtual host found, relaxed SNI checking enabled, SNI '%s'",
                cc->sni_hostname);
        }
        else {
            ap_log_cerror(APLOG_MARK, APLOG_DEBUG, rv, c, APLOGNO()
                "vhost_init: no virtual host, nor base server[%s] matches SNI '%s'",
                c->base_server->server_hostname, cc->sni_hostname);
            cc->server = sc->global->ap_server;
            rv = APR_NOTFOUND; goto cleanup;
        }
    }
    else {
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c, APLOGNO()
            "vhost_init: no SNI hostname provided by client");
    }

    /* reinit, we might have a new server selected */
    sc = tls_conf_server_get(cc->server);
    /* on relaxed SNI matches, we do not enforce the 503 of fallback
     * certificates. */
    cc->service_unavailable = sni_match? sc->service_unavailable : 0;

    if (!sc->rustls_config && !initial_sc->rustls_config) {
        ap_log_cerror(APLOG_MARK, APLOG_DEBUG, 0, c, APLOGNO()
            "vhost_init: no base rustls config found, denying to serve");
        rv = APR_NOTFOUND; goto cleanup;
    }
    builder = rustls_server_config_builder_from_config(
        sc->rustls_config? sc->rustls_config : initial_sc->rustls_config);
    if (NULL == builder) {
        rv = APR_ENOMEM; goto cleanup;
    }

    /* decide on the application protocol we use */
    rv = select_application_protocol(c, cc->server, builder);
    if (APR_SUCCESS != rv) goto cleanup;

    /* if found or not, cc->server will be the server we use now to do
     * the real handshake and, if successful, the traffic after that.
     * Free the current session and create the real one for the
     * selected server. */
    rustls_connection_free(cc->rustls_connection);
    cc->rustls_connection = NULL;
    config = rustls_server_config_builder_build(builder);
    builder = NULL;
    rr = rustls_server_connection_new(config, &cc->rustls_connection);
    if (RUSTLS_RESULT_OK != rr) goto cleanup;
    rustls_connection_set_userdata(cc->rustls_connection, c);

cleanup:
    if (builder != NULL) rustls_server_config_builder_free(builder);
    if (config != NULL) rustls_server_config_free(config);
    if (rr != RUSTLS_RESULT_OK) {
        const char *err_descr = NULL;
        rv = tls_util_rustls_error(c->pool, rr, &err_descr);
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, sc->server, APLOGNO()
                     "Failed to init session for server %s: [%d] %s",
                     sc->server->server_hostname, (int)rr, err_descr);
        c->aborted = 1;
    }
    ap_log_error(APLOG_MARK, APLOG_TRACE1, rv, sc->server,
                 "tls_core_conn_server_init done: %s",
                 sc->server->server_hostname);
    return rv;
}

apr_status_t tls_core_conn_post_handshake(conn_rec *c)
{
    tls_conf_conn_t *cc = tls_conf_conn_get(c);
    tls_conf_server_t *sc = tls_conf_server_get(cc->server);
    const rustls_supported_ciphersuite *rsuite;
    const rustls_certificate *cert;
    apr_status_t rv = APR_SUCCESS;

    if (rustls_connection_is_handshaking(cc->rustls_connection)) {
        rv = APR_EGENERAL;
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, cc->server, APLOGNO()
                     "post handshake, but rustls claims to still be handshaking: %s",
                     cc->server->server_hostname);
        goto cleanup;
    }

    cc->tls_protocol_id = rustls_connection_get_protocol_version(cc->rustls_connection);
    cc->tls_protocol_name = tls_proto_get_version_name(sc->global->proto,
        cc->tls_protocol_id, c->pool);
    rsuite = rustls_connection_get_negotiated_ciphersuite(cc->rustls_connection);
    if (!rsuite) {
        rv = APR_EGENERAL;
        ap_log_error(APLOG_MARK, APLOG_ERR, rv, cc->server, APLOGNO()
                     "post handshake, but rustls does not report negotiated cipher suite: %s",
                     cc->server->server_hostname);
        goto cleanup;
    }
    cc->tls_cipher_id = rustls_supported_ciphersuite_get_suite(rsuite);
    cc->tls_cipher_name = tls_proto_get_cipher_name(sc->global->proto,
        cc->tls_cipher_id, c->pool);
    ap_log_cerror(APLOG_MARK, APLOG_TRACE1, 0, c, "post_handshake %s: %s [%s]",
        cc->server->server_hostname, cc->tls_protocol_name, cc->tls_cipher_name);

    cert = rustls_connection_get_peer_certificate(cc->rustls_connection, 0);
    if (cert) {
        size_t i = 0;

        cc->peer_certs = apr_array_make(c->pool, 5, sizeof(const rustls_certificate*));
        while (cert) {
            APR_ARRAY_PUSH(cc->peer_certs, const rustls_certificate*) = cert;
            cert = rustls_connection_get_peer_certificate(cc->rustls_connection, ++i);
        }
    }
    if (!cc->peer_certs && sc->client_auth == TLS_CLIENT_AUTH_REQUIRED) {
        ap_log_cerror(APLOG_MARK, APLOG_INFO, 0, c, APLOGNO()
              "A client certificate is required, but no acceptable certificate was presented.");
        rv = APR_ECONNABORTED;
    }

    rv = tls_var_handshake_done(c);
cleanup:
    return rv;
}

/**
 * Return != 0, if a connection also serve requests for server <other>.
 */
static int tls_conn_compatible_for(tls_conf_conn_t *cc, server_rec *other)
{
    tls_conf_server_t *oc;

    /*   - differences in certificates are the responsibility of the client.
     *     if it thinks the SNI server works for r->server, we are fine with that.
     *   - if there are differences in requirements to client certificates, we
     *     need to deny the request.
     */
    if (!cc->server || !other) return 0;
    if (cc->server == other) return 1;
    oc = tls_conf_server_get(other);
    if (!oc) return 0;

    /* If the connection TLS version is below other other min one, no */
    if (oc->tls_protocol_min > 0 && cc->tls_protocol_id < oc->tls_protocol_min) return 0;
    /* If the connection TLS cipher is listed as suppressed by other, no */
    if (oc->tls_supp_ciphers && tls_util_array_uint16_contains(
        oc->tls_supp_ciphers, cc->tls_cipher_id)) return 0;
    return 1;
}

int tls_core_request_check(request_rec *r)
{
    tls_conf_conn_t *cc = tls_conf_conn_get(r->connection);
    int rv = DECLINED; /* do not object to the request */

    /* If we are not enabled on this connection, leave. We are not renegotiating.
     * Otherwise:
     * - service is unavailable when we have only a fallback certificate or
     *   when a challenge protocol is active (ACME tls-alpn-01 for example).
     * - with vhosts configured and no SNI from the client, deny access.
     * - are servers compatible for connection sharing?
     */
    ap_log_rerror(APLOG_MARK, APLOG_TRACE2, 0, r,
                 "tls_core_request_check[%s, %d]: %s", r->hostname,
                 cc? cc->service_unavailable : 2, r->the_request);
    if (!TLS_CONN_ST_IS_ENABLED(cc)) goto cleanup;
    if (cc->service_unavailable) {
        rv = HTTP_SERVICE_UNAVAILABLE; goto cleanup;
    }
    if (!cc->sni_hostname && r->connection->vhost_lookup_data) {
        rv = HTTP_FORBIDDEN; goto cleanup;
    }
    if (!tls_conn_compatible_for(cc, r->server)) {
        rv = HTTP_MISDIRECTED_REQUEST;
        ap_log_rerror(APLOG_MARK, APLOG_ERR, rv, r, APLOGNO()
                     "Connection host %s, selected via SNI, and request host %s"
                     " have incompatible TLS configurations.",
                     cc->server->server_hostname, r->hostname);
        goto cleanup;
    }
cleanup:
    return rv;
}

apr_status_t tls_core_error(conn_rec *c, rustls_result rr, const char **perrstr)
{
    tls_conf_conn_t *cc = tls_conf_conn_get(c);
    apr_status_t rv;

    rv = tls_util_rustls_error(c->pool, rr, perrstr);
    if (cc) {
        cc->last_error = rr;
        cc->last_error_descr = *perrstr;
    }
    return rv;
}
