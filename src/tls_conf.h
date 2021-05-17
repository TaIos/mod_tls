/* Copyright 2021, ISRG (https://www.abetterinternet.org)
 *
 * This software is licensed as described in the file LICENSE, which
 * you should have received as part of this distribution.
 *
 */
#ifndef tls_conf_h
#define tls_conf_h

/* Configuration flags */
#define TLS_FLAG_UNSET  (-1)
#define TLS_FLAG_FALSE  (0)
#define TLS_FLAG_TRUE   (1)

struct tls_proto_conf_t;
struct tls_cert_reg_t;
struct ap_socache_instance_t;
struct ap_socache_provider_t;
struct apr_global_mutex_t;

/* The global module configuration, created after post-config
 * and then readonly.
 */
typedef struct {
    server_rec *ap_server;            /* the gobal server we initialized on */
    server_addr_rec *tls_addresses;   /* the addresses/port we are active on */
    struct tls_proto_conf_t *proto;   /* TLS protocol/rustls specific globals */
    apr_hash_t *var_lookups;          /* variable lookup functions by var name */

    struct tls_cert_reg_t *cert_reg;  /* all certified keys loaded in post-config */
    const rustls_server_config *rustls_hello_config; /* config to use for initial client hello */

    const char *session_cache_spec;   /* how the session cache was specified */
    const struct ap_socache_provider_t *session_cache_provider; /* provider used for session cache */
    struct ap_socache_instance_t *session_cache; /* session cache instance */
    struct apr_global_mutex_t *session_cache_mutex; /* global mutex for access to session cache */
} tls_conf_global_t;

/* The module configuration for a server (vhost).
 * Populated during config parsing, merged and completed
 * in the post config phase. Readonly after that.
 */
typedef struct {
    server_rec *server;               /* server this config belongs to */
    tls_conf_global_t *global;        /* global module config, singleton */

    int enabled;                      /* TLS_FLAG_TRUE if mod_tls is active on this server */
    apr_array_header_t *cert_specs;   /* array of (tls_cert_spec_t*) of configured certificates */
    int tls_protocol_min;             /* the minimum TLS protocol version to use */
    apr_array_header_t *tls_pref_ciphers;  /* List of apr_uint16_t cipher ids to prefer */
    apr_array_header_t *tls_supp_ciphers;  /* List of apr_uint16_t cipher ids to suppress */
    int honor_client_order;           /* honor client cipher ordering */
    int strict_sni;

    apr_array_header_t *certified_keys; /* rustls_certified_key list configured */
    int base_server;                  /* != 0 iff this is the base server */
    int service_unavailable;          /* TLS not trustworthy configured, return 503s */
    const rustls_server_config *rustls_config; /* config to use for TLS against this very server */
} tls_conf_server_t;

typedef struct {
    int std_env_vars;
} tls_conf_dir_t;

/* our static registry of configuration directives. */
extern const command_rec tls_conf_cmds[];

/* create the modules configuration for a server_rec. */
void *tls_conf_create_svr(apr_pool_t *pool, server_rec *s);

/* merge (inherit) server configurations for the module.
 * Settings in 'add' overwrite the ones in 'base' and unspecified
 * settings shine through. */
void *tls_conf_merge_svr(apr_pool_t *pool, void *basev, void *addv);

/* create the modules configuration for a directory. */
void *tls_conf_create_dir(apr_pool_t *pool, char *dir);

/* merge (inherit) directory configurations for the module.
 * Settings in 'add' overwrite the ones in 'base' and unspecified
 * settings shine through. */
void *tls_conf_merge_dir(apr_pool_t *pool, void *basev, void *addv);


/* Get the server specific module configuration. */
tls_conf_server_t *tls_conf_server_get(server_rec *s);

/* Get the directory specific module configuration for the request. */
tls_conf_dir_t *tls_conf_dir_get(request_rec *r);

/* If any configuration values are unset, supply the global defaults. */
apr_status_t tls_conf_server_apply_defaults(tls_conf_server_t *sc, apr_pool_t *p);

#endif /* tls_conf_h */