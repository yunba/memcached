#include <stdio.h>
#include <stdlib.h>
#include <dlfcn.h>
#include <assert.h>

#include "config_parser.h"

#include <memcached/engine.h>

struct bucket_engine {
    ENGINE_HANDLE_V1 engine;
    bool initialized;
    char *proxied_engine_path;
    union {
        ENGINE_HANDLE *v0;
        ENGINE_HANDLE_V1 *v1;
    } proxied_engine;
    GET_SERVER_API get_server_api;
    SERVER_HANDLE_V1 *server;
};

ENGINE_ERROR_CODE create_instance(uint64_t interface,
                                  GET_SERVER_API gsapi,
                                  ENGINE_HANDLE **handle);

static const char* bucket_get_info(ENGINE_HANDLE* handle);
static ENGINE_ERROR_CODE bucket_initialize(ENGINE_HANDLE* handle,
                                          const char* config_str);
static void bucket_destroy(ENGINE_HANDLE* handle);
static ENGINE_ERROR_CODE bucket_item_allocate(ENGINE_HANDLE* handle,
                                             const void* cookie,
                                             item **item,
                                             const void* key,
                                             const size_t nkey,
                                             const size_t nbytes,
                                             const int flags,
                                             const rel_time_t exptime);
static ENGINE_ERROR_CODE bucket_item_delete(ENGINE_HANDLE* handle,
                                           const void* cookie,
                                           item* item);
static void bucket_item_release(ENGINE_HANDLE* handle, item* item);
static ENGINE_ERROR_CODE bucket_get(ENGINE_HANDLE* handle,
                                   const void* cookie,
                                   item** item,
                                   const void* key,
                                   const int nkey);
static ENGINE_ERROR_CODE bucket_get_stats(ENGINE_HANDLE* handle,
                                         const void *cookie,
                                         const char *stat_key,
                                         int nkey,
                                         ADD_STAT add_stat);
static void bucket_reset_stats(ENGINE_HANDLE* handle);
static ENGINE_ERROR_CODE bucket_store(ENGINE_HANDLE* handle,
                                     const void *cookie,
                                     item* item,
                                     uint64_t *cas,
                                     ENGINE_STORE_OPERATION operation);
static ENGINE_ERROR_CODE bucket_arithmetic(ENGINE_HANDLE* handle,
                                          const void* cookie,
                                          const void* key,
                                          const int nkey,
                                          const bool increment,
                                          const bool create,
                                          const uint64_t delta,
                                          const uint64_t initial,
                                          const rel_time_t exptime,
                                          uint64_t *cas,
                                          uint64_t *result);
static ENGINE_ERROR_CODE bucket_flush(ENGINE_HANDLE* handle,
                                     const void* cookie, time_t when);
static ENGINE_ERROR_CODE initalize_configuration(struct bucket_engine *me,
                                                 const char *cfg_str);
static ENGINE_ERROR_CODE bucket_unknown_command(ENGINE_HANDLE* handle,
                                               const void* cookie,
                                               protocol_binary_request_header *request,
                                               ADD_RESPONSE response);
static char* item_get_data(const item* item);
static char* item_get_key(const item* item);
static void item_set_cas(item* item, uint64_t val);
static uint64_t item_get_cas(const item* item);
static uint8_t item_get_clsid(const item* item);

struct bucket_engine bucket_engine = {
    .engine = {
        .interface = {
            .interface = 1
        },
        .get_info = bucket_get_info,
        .initialize = bucket_initialize,
        .destroy = bucket_destroy,
        .allocate = bucket_item_allocate,
        .remove = bucket_item_delete,
        .release = bucket_item_release,
        .get = bucket_get,
        .get_stats = bucket_get_stats,
        .reset_stats = bucket_reset_stats,
        .store = bucket_store,
        .arithmetic = bucket_arithmetic,
        .flush = bucket_flush,
        .unknown_command = bucket_unknown_command,
        .item_get_cas = item_get_cas,
        .item_set_cas = item_set_cas,
        .item_get_key = item_get_key,
        .item_get_data = item_get_data,
        .item_get_clsid = item_get_clsid
    },
    .initialized = true,
};

ENGINE_ERROR_CODE create_instance(uint64_t interface,
                                  GET_SERVER_API gsapi,
                                  ENGINE_HANDLE **handle) {
    if (interface != 1) {
        return ENGINE_ENOTSUP;
    }

    *handle = (ENGINE_HANDLE*)&bucket_engine;
    bucket_engine.get_server_api = gsapi;
    bucket_engine.server = gsapi(1);
    return ENGINE_SUCCESS;
}

static inline ENGINE_HANDLE *proxied_engine_v0(ENGINE_HANDLE *handle) {
    struct bucket_engine *e = (struct bucket_engine*)handle;
    return e->proxied_engine.v0;
}

static inline ENGINE_HANDLE_V1 *proxied_engine_v1(ENGINE_HANDLE *handle) {
    struct bucket_engine *e = (struct bucket_engine*)handle;
    return e->proxied_engine.v1;
}

static inline struct bucket_engine* get_handle(ENGINE_HANDLE* handle) {
    return (struct bucket_engine*)handle;
}

static const char* bucket_get_info(ENGINE_HANDLE* handle) {
    return "Bucket engine v0.1";
}

static ENGINE_ERROR_CODE bucket_initialize(ENGINE_HANDLE* handle,
                                          const char* config_str) {
    struct bucket_engine* se = get_handle(handle);

    ENGINE_ERROR_CODE ret = initalize_configuration(se, config_str);
    if (ret != ENGINE_SUCCESS) {
        return ret;
    }

    bucket_engine.proxied_engine.v0 = dlopen(se->proxied_engine_path,
                                            RTLD_LAZY | RTLD_LOCAL);
    if (bucket_engine.proxied_engine.v0 == NULL) {
        const char *msg = dlerror();
        fprintf(stderr, "Failed to open library \"%s\": %s\n",
                se->proxied_engine_path ? se->proxied_engine_path : "self",
                msg ? msg : "unknown error");
        return false;
    }

    void *symbol = dlsym(bucket_engine.proxied_engine.v0, "create_instance");
    if (symbol == NULL) {
        fprintf(stderr,
                "Could not find symbol \"create_instance\" in %s: %s\n",
                se->proxied_engine_path ? se->proxied_engine_path : "self",
                dlerror());
        return false;
    }
    union tronds_hack {
        CREATE_INSTANCE create;
        void* voidptr;
    } my_create = {.create = NULL };

    my_create.voidptr = symbol;

    /* request a instance with protocol version 1 */
    ENGINE_HANDLE *engine = proxied_engine_v0(handle);
    ENGINE_ERROR_CODE error = (*my_create.create)(1,
                                                  bucket_engine.get_server_api,
                                                  &engine);

    if (error != ENGINE_SUCCESS || engine == NULL) {
        fprintf(stderr, "Failed to create instance. Error code: %d\n", error);
        dlclose(handle);
        return false;
    }

    if (engine->interface == 1) {
        bucket_engine.proxied_engine.v0 = engine;
        bucket_engine.proxied_engine.v1 = (ENGINE_HANDLE_V1*)engine;
        if (bucket_engine.proxied_engine.v1->initialize(engine, config_str)
            != ENGINE_SUCCESS) {

            bucket_engine.proxied_engine.v1->destroy(engine);
            fprintf(stderr, "Failed to initialize instance. Error code: %d\n",
                    error);
            dlclose(handle);
            return ENGINE_FAILED;
        }
    } else {
        fprintf(stderr, "Unsupported interface level\n");
        dlclose(handle);
        return ENGINE_ENOTSUP;
    }

    fprintf(stderr, "Proxying to %s from %s\n",
            proxied_engine_v1(handle)->get_info(proxied_engine_v0(handle)),
            bucket_engine.proxied_engine_path);

    return ENGINE_SUCCESS;
}

static void bucket_destroy(ENGINE_HANDLE* handle) {
    struct bucket_engine* se = get_handle(handle);

    if (se->initialized) {
        proxied_engine_v1(handle)->destroy(proxied_engine_v0(handle));
        se->initialized = false;
    }
}

static ENGINE_ERROR_CODE bucket_item_allocate(ENGINE_HANDLE* handle,
                                             const void* cookie,
                                             item **item,
                                             const void* key,
                                             const size_t nkey,
                                             const size_t nbytes,
                                             const int flags,
                                             const rel_time_t exptime) {
    return proxied_engine_v1(handle)->allocate(proxied_engine_v0(handle),
                                               cookie, item,
                                               key, nkey,
                                               nbytes, flags, exptime);
}

static ENGINE_ERROR_CODE bucket_item_delete(ENGINE_HANDLE* handle,
                                           const void* cookie,
                                           item* item) {
    return proxied_engine_v1(handle)->remove(proxied_engine_v0(handle),
                                             cookie, item);
}

static void bucket_item_release(ENGINE_HANDLE* handle, item* item) {
    proxied_engine_v1(handle)->release(proxied_engine_v0(handle), item);
}

static ENGINE_ERROR_CODE bucket_get(ENGINE_HANDLE* handle,
                                   const void* cookie,
                                   item** item,
                                   const void* key,
                                   const int nkey) {
    return proxied_engine_v1(handle)->get(proxied_engine_v0(handle),
                                          cookie, item, key, nkey);
}

static ENGINE_ERROR_CODE bucket_get_stats(ENGINE_HANDLE* handle,
                                         const void* cookie,
                                         const char* stat_key,
                                         int nkey,
                                         ADD_STAT add_stat)
{
    const char *user = bucket_engine.server->get_auth_data(cookie);
    printf("Authenticated as %s\n", user ?: "<nobody>");
    return proxied_engine_v1(handle)->get_stats(proxied_engine_v0(handle),
                                                cookie, stat_key,
                                                nkey, add_stat);
}

static ENGINE_ERROR_CODE bucket_store(ENGINE_HANDLE* handle,
                                     const void *cookie,
                                     item* item,
                                     uint64_t *cas,
                                     ENGINE_STORE_OPERATION operation) {
    return proxied_engine_v1(handle)->store(proxied_engine_v0(handle), cookie,
                                            item, cas, operation);
}

static ENGINE_ERROR_CODE bucket_arithmetic(ENGINE_HANDLE* handle,
                                          const void* cookie,
                                          const void* key,
                                          const int nkey,
                                          const bool increment,
                                          const bool create,
                                          const uint64_t delta,
                                          const uint64_t initial,
                                          const rel_time_t exptime,
                                          uint64_t *cas,
                                          uint64_t *result) {
    return proxied_engine_v1(handle)->arithmetic(proxied_engine_v0(handle),
                                                 cookie, key, nkey,
                                                 increment, create,
                                                 delta, initial, exptime,
                                                 cas, result);
}

static ENGINE_ERROR_CODE bucket_flush(ENGINE_HANDLE* handle,
                                     const void* cookie, time_t when) {
    return proxied_engine_v1(handle)->flush(proxied_engine_v0(handle),
                                            cookie, when);

}

static void bucket_reset_stats(ENGINE_HANDLE* handle) {
    proxied_engine_v1(handle)->reset_stats(proxied_engine_v0(handle));
}

static ENGINE_ERROR_CODE initalize_configuration(struct bucket_engine *me,
                                                 const char *cfg_str) {
    ENGINE_ERROR_CODE ret = ENGINE_SUCCESS;

    if (cfg_str != NULL) {
        struct config_item items[] = {
            { .key = "engine",
              .datatype = DT_STRING,
              .value.dt_string = &me->proxied_engine_path },
            { .key = "config_file",
              .datatype = DT_CONFIGFILE },
            { .key = NULL}
        };

        ret = parse_config(cfg_str, items, stderr);
    }

    return ret;
}

static ENGINE_ERROR_CODE bucket_unknown_command(ENGINE_HANDLE* handle,
                                               const void* cookie,
                                               protocol_binary_request_header *request,
                                               ADD_RESPONSE response)
{
    return ENGINE_ENOTSUP;
}

static char* item_get_data(const item* item) {
    return bucket_engine.proxied_engine.v1->item_get_data(item);
}

static char* item_get_key(const item* item) {
    return bucket_engine.proxied_engine.v1->item_get_key(item);
}

static void item_set_cas(item* item, uint64_t val) {
    bucket_engine.proxied_engine.v1->item_set_cas(item, val);
}

static uint64_t item_get_cas(const item* item) {
    return bucket_engine.proxied_engine.v1->item_get_cas(item);
}

static uint8_t item_get_clsid(const item* item) {
    return bucket_engine.proxied_engine.v1->item_get_clsid(item);
}