/*
 * Copyright (C) Max Romanov
 * Copyright (C) Valentin V. Bartenev
 * Copyright (C) NGINX, Inc.
 */

#include "php.h"
#include "SAPI.h"
#include "php_main.h"
#include "php_variables.h"

#include <nxt_main.h>
#include <nxt_router.h>
#include <nxt_unit.h>
#include <nxt_unit_request.h>


#if PHP_VERSION_ID >= 50400
#define NXT_HAVE_PHP_IGNORE_CWD 1
#endif

#if PHP_VERSION_ID >= 70100
#define NXT_HAVE_PHP_LOG_MESSAGE_WITH_SYSLOG_TYPE 1
#else
#define NXT_HAVE_PHP_INTERRUPTS 1
#endif

#if PHP_VERSION_ID >= 70000
#define NXT_PHP7 1
#endif

/* PHP 8 */
#ifndef TSRMLS_CC
#define TSRMLS_CC
#define TSRMLS_DC
#define TSRMLS_D  void
#define TSRMLS_C
#endif


typedef struct {
    nxt_str_t  root;
    nxt_str_t  index;
    nxt_str_t  script_name;
    nxt_str_t  script_dirname;
    nxt_str_t  script_filename;
} nxt_php_target_t;


typedef struct {
    char                     *cookie;
    nxt_str_t                *root;
    nxt_str_t                *index;
    nxt_str_t                path_info;
    nxt_str_t                script_name;
    nxt_str_t                script_filename;
    nxt_str_t                script_dirname;
    nxt_unit_request_info_t  *req;

    uint8_t                  chdir;  /* 1 bit */
} nxt_php_run_ctx_t;


#ifdef NXT_PHP7
typedef int (*nxt_php_disable_t)(char *p, size_t size);
#else
typedef int (*nxt_php_disable_t)(char *p, uint TSRMLS_DC);
#endif

#if PHP_VERSION_ID < 70200
typedef void (*zif_handler)(INTERNAL_FUNCTION_PARAMETERS);
#endif


static nxt_int_t nxt_php_start(nxt_task_t *task, nxt_process_data_t *data);
static nxt_int_t nxt_php_set_target(nxt_task_t *task, nxt_php_target_t *target,
    nxt_conf_value_t *conf);
static void nxt_php_set_options(nxt_task_t *task, nxt_conf_value_t *options,
    int type);
static nxt_int_t nxt_php_alter_option(nxt_str_t *name, nxt_str_t *value,
    int type);
static void nxt_php_disable(nxt_task_t *task, const char *type,
    nxt_str_t *value, char **ptr, nxt_php_disable_t disable);

static nxt_int_t nxt_php_dirname(const nxt_str_t *file, nxt_str_t *dir);
static void nxt_php_str_trim_trail(nxt_str_t *str, u_char t);
static void nxt_php_str_trim_lead(nxt_str_t *str, u_char t);
nxt_inline u_char *nxt_realpath(const void *c);

static void nxt_php_request_handler(nxt_unit_request_info_t *req);
static void nxt_php_dynamic_request(nxt_php_run_ctx_t *ctx,
    nxt_unit_request_t *r);
static void nxt_php_execute(nxt_php_run_ctx_t *ctx, nxt_unit_request_t *r);
nxt_inline void nxt_php_vcwd_chdir(nxt_unit_request_info_t *req, u_char *dir);

static int nxt_php_startup(sapi_module_struct *sapi_module);
static int nxt_php_send_headers(sapi_headers_struct *sapi_headers TSRMLS_DC);
static void *nxt_php_hash_str_find_ptr(const HashTable *ht,
    const nxt_str_t *str);
static char *nxt_php_read_cookies(TSRMLS_D);
static void nxt_php_set_sptr(nxt_unit_request_info_t *req, const char *name,
    nxt_unit_sptr_t *v, uint32_t len, zval *track_vars_array TSRMLS_DC);
nxt_inline void nxt_php_set_str(nxt_unit_request_info_t *req, const char *name,
    nxt_str_t *s, zval *track_vars_array TSRMLS_DC);
static void nxt_php_set_cstr(nxt_unit_request_info_t *req, const char *name,
    const char *str, uint32_t len, zval *track_vars_array TSRMLS_DC);
static void nxt_php_register_variables(zval *track_vars_array TSRMLS_DC);
#ifdef NXT_HAVE_PHP_LOG_MESSAGE_WITH_SYSLOG_TYPE
static void nxt_php_log_message(char *message, int syslog_type_int);
#else
static void nxt_php_log_message(char *message TSRMLS_DC);
#endif

#ifdef NXT_PHP7
static size_t nxt_php_unbuffered_write(const char *str,
    size_t str_length TSRMLS_DC);
static size_t nxt_php_read_post(char *buffer, size_t count_bytes TSRMLS_DC);
#else
static int nxt_php_unbuffered_write(const char *str, uint str_length TSRMLS_DC);
static int nxt_php_read_post(char *buffer, uint count_bytes TSRMLS_DC);
#endif


PHP_MINIT_FUNCTION(nxt_php_ext);
ZEND_NAMED_FUNCTION(nxt_php_chdir);

zif_handler  nxt_php_chdir_handler;


static zend_module_entry  nxt_php_unit_module = {
    STANDARD_MODULE_HEADER,
    "unit",
    NULL,                        /* function table */
    PHP_MINIT(nxt_php_ext),      /* initialization */
    NULL,                        /* shutdown */
    NULL,                        /* request initialization */
    NULL,                        /* request shutdown */
    NULL,                        /* information */
    NXT_VERSION,
    STANDARD_MODULE_PROPERTIES
};


PHP_MINIT_FUNCTION(nxt_php_ext)
{
    zend_function  *func;

    static const nxt_str_t  chdir = nxt_string("chdir");

    func = nxt_php_hash_str_find_ptr(CG(function_table), &chdir);
    if (nxt_slow_path(func == NULL)) {
        return FAILURE;
    }

    nxt_php_chdir_handler = func->internal_function.handler;
    func->internal_function.handler = nxt_php_chdir;

    return SUCCESS;
}


ZEND_NAMED_FUNCTION(nxt_php_chdir)
{
    nxt_php_run_ctx_t  *ctx;

    ctx = SG(server_context);
    ctx->chdir = 1;

    nxt_php_chdir_handler(INTERNAL_FUNCTION_PARAM_PASSTHRU);
}


static sapi_module_struct  nxt_php_sapi_module =
{
    (char *) "cli-server",
    (char *) "unit",

    nxt_php_startup,             /* startup */
    php_module_shutdown_wrapper, /* shutdown */

    NULL,                        /* activate */
    NULL,                        /* deactivate */

    nxt_php_unbuffered_write,    /* unbuffered write */
    NULL,                        /* flush */
    NULL,                        /* get uid */
    NULL,                        /* getenv */

    php_error,                   /* error handler */

    NULL,                        /* header handler */
    nxt_php_send_headers,        /* send headers handler */
    NULL,                        /* send header handler */

    nxt_php_read_post,           /* read POST data */
    nxt_php_read_cookies,        /* read Cookies */

    nxt_php_register_variables,  /* register server variables */
    nxt_php_log_message,         /* log message */
    NULL,                        /* get request time */
    NULL,                        /* terminate process */

    NULL,                        /* php_ini_path_override */
#ifdef NXT_HAVE_PHP_INTERRUPTS
    NULL,                        /* block_interruptions */
    NULL,                        /* unblock_interruptions */
#endif
    NULL,                        /* default_post_reader */
    NULL,                        /* treat_data */
    NULL,                        /* executable_location */

    0,                           /* php_ini_ignore */
#ifdef NXT_HAVE_PHP_IGNORE_CWD
    1,                           /* php_ini_ignore_cwd */
#endif
    NULL,                        /* get_fd */

    NULL,                        /* force_http_10 */

    NULL,                        /* get_target_uid */
    NULL,                        /* get_target_gid */

    NULL,                        /* input_filter */

    NULL,                        /* ini_defaults */
    0,                           /* phpinfo_as_text */

    NULL,                        /* ini_entries */
    NULL,                        /* additional_functions */
    NULL                         /* input_filter_init */
};


static uint32_t  compat[] = {
    NXT_VERNUM, NXT_DEBUG,
};


NXT_EXPORT nxt_app_module_t  nxt_app_module = {
    sizeof(compat),
    compat,
    nxt_string("php"),
    PHP_VERSION,
    NULL,
    0,
    NULL,
    nxt_php_start,
};


static nxt_php_target_t  *nxt_php_targets;
static nxt_int_t         nxt_php_last_target = -1;

static nxt_unit_ctx_t    *nxt_php_unit_ctx;
#if defined(ZTS) && PHP_VERSION_ID < 70400
static void              ***tsrm_ls;
#endif


static nxt_int_t
nxt_php_start(nxt_task_t *task, nxt_process_data_t *data)
{
    u_char                 *p;
    uint32_t               next;
    nxt_str_t              ini_path, name;
    nxt_int_t              ret;
    nxt_uint_t             n;
    nxt_port_t             *my_port, *main_port;
    nxt_runtime_t          *rt;
    nxt_unit_ctx_t         *unit_ctx;
    nxt_unit_init_t        php_init;
    nxt_conf_value_t       *value;
    nxt_php_app_conf_t     *c;
    nxt_common_app_conf_t  *conf;

    static nxt_str_t  file_str = nxt_string("file");
    static nxt_str_t  user_str = nxt_string("user");
    static nxt_str_t  admin_str = nxt_string("admin");

    conf = data->app;
    c = &conf->u.php;

    n = (c->targets != NULL) ? nxt_conf_object_members_count(c->targets) : 1;

    nxt_php_targets = nxt_zalloc(sizeof(nxt_php_target_t) * n);
    if (nxt_slow_path(nxt_php_targets == NULL)) {
        return NXT_ERROR;
    }

    if (c->targets != NULL) {
        next = 0;

        for (n = 0; /* void */; n++) {
            value = nxt_conf_next_object_member(c->targets, &name, &next);
            if (value == NULL) {
                break;
            }

            ret = nxt_php_set_target(task, &nxt_php_targets[n], value);
            if (nxt_slow_path(ret != NXT_OK)) {
                return NXT_ERROR;
            }
        }

    } else {
        ret = nxt_php_set_target(task, &nxt_php_targets[0], conf->self);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }
    }

#ifdef ZTS

#if PHP_VERSION_ID >= 70400
    php_tsrm_startup();
#else
    tsrm_startup(1, 1, 0, NULL);
    tsrm_ls = ts_resource(0);
#endif

#endif

#if defined(NXT_PHP7) && defined(ZEND_SIGNALS)

#if (NXT_ZEND_SIGNAL_STARTUP)
    zend_signal_startup();
#elif defined(ZTS)
#error PHP is built with thread safety and broken signals.
#endif

#endif

    sapi_startup(&nxt_php_sapi_module);

    if (c->options != NULL) {
        value = nxt_conf_get_object_member(c->options, &file_str, NULL);

        if (value != NULL) {
            nxt_conf_get_string(value, &ini_path);

            p = nxt_malloc(ini_path.length + 1);
            if (nxt_slow_path(p == NULL)) {
                return NXT_ERROR;
            }

            nxt_php_sapi_module.php_ini_path_override = (char *) p;

            p = nxt_cpymem(p, ini_path.start, ini_path.length);
            *p = '\0';
        }
    }

    if (nxt_slow_path(nxt_php_startup(&nxt_php_sapi_module) == FAILURE)) {
        nxt_alert(task, "failed to initialize SAPI module and extension");
        return NXT_ERROR;
    }

    if (c->options != NULL) {
        value = nxt_conf_get_object_member(c->options, &admin_str, NULL);
        nxt_php_set_options(task, value, ZEND_INI_SYSTEM);

        value = nxt_conf_get_object_member(c->options, &user_str, NULL);
        nxt_php_set_options(task, value, ZEND_INI_USER);
    }

    rt = task->thread->runtime;

    main_port = rt->port_by_type[NXT_PROCESS_MAIN];
    if (nxt_slow_path(main_port == NULL)) {
        nxt_alert(task, "main process not found");
        return NXT_ERROR;
    }

    my_port = nxt_runtime_port_find(rt, nxt_pid, 0);
    if (nxt_slow_path(my_port == NULL)) {
        nxt_alert(task, "my_port not found");
        return NXT_ERROR;
    }

    nxt_memzero(&php_init, sizeof(nxt_unit_init_t));

    php_init.callbacks.request_handler = nxt_php_request_handler;

    php_init.ready_port.id.pid = main_port->pid;
    php_init.ready_port.id.id = main_port->id;
    php_init.ready_port.out_fd = main_port->pair[1];

    nxt_fd_blocking(task, main_port->pair[1]);

    php_init.ready_stream = my_port->process->stream;

    php_init.read_port.id.pid = my_port->pid;
    php_init.read_port.id.id = my_port->id;
    php_init.read_port.in_fd = my_port->pair[0];

    nxt_fd_blocking(task, my_port->pair[0]);

    php_init.log_fd = 2;
    php_init.shm_limit = conf->shm_limit;

    unit_ctx = nxt_unit_init(&php_init);
    if (nxt_slow_path(unit_ctx == NULL)) {
        return NXT_ERROR;
    }

    nxt_php_unit_ctx = unit_ctx;

    nxt_unit_run(unit_ctx);

    nxt_unit_done(unit_ctx);

    exit(0);

    return NXT_OK;
}


static nxt_int_t
nxt_php_set_target(nxt_task_t *task, nxt_php_target_t *target,
    nxt_conf_value_t *conf)
{
    u_char            *tmp, *p;
    nxt_str_t         str;
    nxt_int_t         ret;
    nxt_conf_value_t  *value;

    static nxt_str_t  root_str = nxt_string("root");
    static nxt_str_t  script_str = nxt_string("script");
    static nxt_str_t  index_str = nxt_string("index");

    value = nxt_conf_get_object_member(conf, &root_str, NULL);

    if (value == NULL) {
        nxt_alert(task, "no php root specified");
        return NXT_ERROR;
    }

    nxt_conf_get_string(value, &str);

    tmp = nxt_malloc(str.length + 1);
    if (nxt_slow_path(tmp == NULL)) {
        return NXT_ERROR;
    }

    p = tmp;

    p = nxt_cpymem(p, str.start, str.length);
    *p = '\0';

    p = nxt_realpath(tmp);
    if (nxt_slow_path(p == NULL)) {
        nxt_alert(task, "root realpath(%s) failed %E", tmp, nxt_errno);
        return NXT_ERROR;
    }

    nxt_free(tmp);

    target->root.length = nxt_strlen(p);
    target->root.start = p;

    nxt_php_str_trim_trail(&target->root, '/');

    value = nxt_conf_get_object_member(conf, &script_str, NULL);

    if (value != NULL) {
        nxt_conf_get_string(value, &str);

        nxt_php_str_trim_lead(&str, '/');

        tmp = nxt_malloc(target->root.length + 1 + str.length + 1);
        if (nxt_slow_path(tmp == NULL)) {
            return NXT_ERROR;
        }

        p = tmp;

        p = nxt_cpymem(p, target->root.start, target->root.length);
        *p++ = '/';

        p = nxt_cpymem(p, str.start, str.length);
        *p = '\0';

        p = nxt_realpath(tmp);
        if (nxt_slow_path(p == NULL)) {
            nxt_alert(task, "script realpath(%s) failed %E", tmp, nxt_errno);
            return NXT_ERROR;
        }

        nxt_free(tmp);

        target->script_filename.length = nxt_strlen(p);
        target->script_filename.start = p;

        if (!nxt_str_start(&target->script_filename,
                           target->root.start, target->root.length))
        {
            nxt_alert(task, "script is not under php root");
            return NXT_ERROR;
        }

        ret = nxt_php_dirname(&target->script_filename,
                              &target->script_dirname);
        if (nxt_slow_path(ret != NXT_OK)) {
            return NXT_ERROR;
        }

        target->script_name.length = target->script_filename.length
                                     - target->root.length;
        target->script_name.start = target->script_filename.start
                                    + target->root.length;

    } else {
        value = nxt_conf_get_object_member(conf, &index_str, NULL);

        if (value != NULL) {
            nxt_conf_get_string(value, &str);

            tmp = nxt_malloc(str.length);
            if (nxt_slow_path(tmp == NULL)) {
                return NXT_ERROR;
            }

            nxt_memcpy(tmp, str.start, str.length);

            target->index.length = str.length;
            target->index.start = tmp;

        } else {
            nxt_str_set(&target->index, "index.php");
        }
    }

    return NXT_OK;
}


static void
nxt_php_set_options(nxt_task_t *task, nxt_conf_value_t *options, int type)
{
    uint32_t          next;
    nxt_str_t         name, value;
    nxt_conf_value_t  *value_obj;

    if (options != NULL) {
        next = 0;

        for ( ;; ) {
            value_obj = nxt_conf_next_object_member(options, &name, &next);
            if (value_obj == NULL) {
                break;
            }

            nxt_conf_get_string(value_obj, &value);

            if (nxt_php_alter_option(&name, &value, type) != NXT_OK) {
                nxt_log(task, NXT_LOG_ERR,
                        "setting PHP option \"%V: %V\" failed", &name, &value);
                continue;
            }

            if (nxt_str_eq(&name, "disable_functions", 17)) {
                nxt_php_disable(task, "function", &value,
                                &PG(disable_functions),
                                zend_disable_function);
                continue;
            }

            if (nxt_str_eq(&name, "disable_classes", 15)) {
                nxt_php_disable(task, "class", &value,
                                &PG(disable_classes),
                                zend_disable_class);
                continue;
            }
        }
    }
}


#ifdef NXT_PHP7

static nxt_int_t
nxt_php_alter_option(nxt_str_t *name, nxt_str_t *value, int type)
{
    zend_string     *zs;
    zend_ini_entry  *ini_entry;

    ini_entry = nxt_php_hash_str_find_ptr(EG(ini_directives), name);
    if (nxt_slow_path(ini_entry == NULL)) {
        return NXT_ERROR;
    }

    /* PHP exits on memory allocation errors. */
    zs = zend_string_init((char *) value->start, value->length, 1);

    if (ini_entry->on_modify
        && ini_entry->on_modify(ini_entry, zs, ini_entry->mh_arg1,
                                ini_entry->mh_arg2, ini_entry->mh_arg3,
                                ZEND_INI_STAGE_ACTIVATE)
           != SUCCESS)
    {
        zend_string_release(zs);
        return NXT_ERROR;
    }

    ini_entry->value = zs;
    ini_entry->modifiable = type;

    return NXT_OK;
}

#else  /* PHP 5. */

static nxt_int_t
nxt_php_alter_option(nxt_str_t *name, nxt_str_t *value, int type)
{
    char            *cstr;
    zend_ini_entry  *ini_entry;

    ini_entry = nxt_php_hash_str_find_ptr(EG(ini_directives), name);
    if (nxt_slow_path(ini_entry == NULL)) {
        return NXT_ERROR;
    }

    cstr = nxt_malloc(value->length + 1);
    if (nxt_slow_path(cstr == NULL)) {
        return NXT_ERROR;
    }

    nxt_memcpy(cstr, value->start, value->length);
    cstr[value->length] = '\0';

    if (ini_entry->on_modify
        && ini_entry->on_modify(ini_entry, cstr, value->length,
                                ini_entry->mh_arg1, ini_entry->mh_arg2,
                                ini_entry->mh_arg3, ZEND_INI_STAGE_ACTIVATE
                                TSRMLS_CC)
           != SUCCESS)
    {
        nxt_free(cstr);
        return NXT_ERROR;
    }

    ini_entry->value = cstr;
    ini_entry->value_length = value->length;
    ini_entry->modifiable = type;

    return NXT_OK;
}

#endif


static void
nxt_php_disable(nxt_task_t *task, const char *type, nxt_str_t *value,
    char **ptr, nxt_php_disable_t disable)
{
    char  c, *p, *start;

    p = nxt_malloc(value->length + 1);
    if (nxt_slow_path(p == NULL)) {
        return;
    }

    /*
     * PHP frees this memory on module shutdown.
     * See core_globals_dtor() for details.
     */
    *ptr = p;

    nxt_memcpy(p, value->start, value->length);
    p[value->length] = '\0';

    start = p;

    do {
        c = *p;

        if (c == ' ' || c == ',' || c == '\0') {

            if (p != start) {
                *p = '\0';

#ifdef NXT_PHP7
                if (disable(start, p - start)
#else
                if (disable(start, p - start TSRMLS_CC)
#endif
                    != SUCCESS)
                {
                    nxt_log(task, NXT_LOG_ERR,
                            "PHP: failed to disable \"%s\": no such %s",
                            start, type);
                }
            }

            start = p + 1;
        }

        p++;

    } while (c != '\0');
}


static nxt_int_t
nxt_php_dirname(const nxt_str_t *file, nxt_str_t *dir)
{
    size_t  length;

    nxt_assert(file->length > 0 && file->start[0] == '/');

    length = file->length;

    while (file->start[length - 1] != '/') {
        length--;
    }

    dir->length = length;
    dir->start = nxt_malloc(length + 1);
    if (nxt_slow_path(dir->start == NULL)) {
        return NXT_ERROR;
    }

    nxt_memcpy(dir->start, file->start, length);

    dir->start[length] = '\0';

    return NXT_OK;
}


static void
nxt_php_str_trim_trail(nxt_str_t *str, u_char t)
{
    while (str->length > 0 && str->start[str->length - 1] == t) {
        str->length--;
    }

    str->start[str->length] = '\0';
}


static void
nxt_php_str_trim_lead(nxt_str_t *str, u_char t)
{
    while (str->length > 0 && str->start[0] == t) {
        str->length--;
        str->start++;
    }
}


nxt_inline u_char *
nxt_realpath(const void *c)
{
    return (u_char *) realpath(c, NULL);
}


static void
nxt_php_request_handler(nxt_unit_request_info_t *req)
{
    nxt_php_target_t    *target;
    nxt_php_run_ctx_t   ctx;
    nxt_unit_request_t  *r;

    r = req->request;
    target = &nxt_php_targets[r->app_target];

    nxt_memzero(&ctx, sizeof(ctx));

    ctx.req = req;
    ctx.root = &target->root;
    ctx.index = &target->index;

    if (target->script_filename.length == 0) {
        nxt_php_dynamic_request(&ctx, r);
        return;
    }

    ctx.script_filename = target->script_filename;
    ctx.script_dirname = target->script_dirname;
    ctx.script_name = target->script_name;

    ctx.chdir = (r->app_target != nxt_php_last_target);

    nxt_php_execute(&ctx, r);

    nxt_php_last_target = ctx.chdir ? -1 : r->app_target;
}


static void
nxt_php_dynamic_request(nxt_php_run_ctx_t *ctx, nxt_unit_request_t *r)
{
    u_char     *p;
    nxt_str_t  path, script_name;
    nxt_int_t  ret;

    path.length = r->path_length;
    path.start = nxt_unit_sptr_get(&r->path);

    nxt_str_null(&script_name);

    ctx->path_info.start = (u_char *) strstr((char *) path.start, ".php/");
    if (ctx->path_info.start != NULL) {
        ctx->path_info.start += 4;
        path.length = ctx->path_info.start - path.start;

        ctx->path_info.length = r->path_length - path.length;

    } else if (path.start[path.length - 1] == '/') {
        script_name = *ctx->index;

    } else {
        if (nxt_slow_path(path.length < 4
                          || nxt_memcmp(path.start + (path.length - 4),
                                        ".php", 4)))
        {
            nxt_unit_request_done(ctx->req, NXT_UNIT_ERROR);

            return;
        }
    }

    ctx->script_filename.length = ctx->root->length
                                  + path.length
                                  + script_name.length;

    p = nxt_malloc(ctx->script_filename.length + 1);
    if (nxt_slow_path(p == NULL)) {
        nxt_unit_request_done(ctx->req, NXT_UNIT_ERROR);

        return;
    }

    ctx->script_filename.start = p;

    ctx->script_name.length = path.length + script_name.length;
    ctx->script_name.start = p + ctx->root->length;

    p = nxt_cpymem(p, ctx->root->start, ctx->root->length);
    p = nxt_cpymem(p, path.start, path.length);

    if (script_name.length > 0) {
        p = nxt_cpymem(p, script_name.start, script_name.length);
    }

    *p = '\0';

    ctx->chdir = 1;

    ret = nxt_php_dirname(&ctx->script_filename, &ctx->script_dirname);
    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_unit_request_done(ctx->req, NXT_UNIT_ERROR);
        nxt_free(ctx->script_filename.start);

        return;
    }

    nxt_php_execute(ctx, r);

    nxt_free(ctx->script_filename.start);
    nxt_free(ctx->script_dirname.start);

    nxt_php_last_target = -1;
}


static void
nxt_php_execute(nxt_php_run_ctx_t *ctx, nxt_unit_request_t *r)
{
    nxt_unit_field_t  *f;
    zend_file_handle  file_handle;

    nxt_unit_req_debug(ctx->req, "PHP execute script %s",
                       ctx->script_filename.start);

    SG(server_context) = ctx;
    SG(options) |= SAPI_OPTION_NO_CHDIR;
    SG(request_info).request_uri = nxt_unit_sptr_get(&r->target);
    SG(request_info).request_method = nxt_unit_sptr_get(&r->method);

    SG(request_info).proto_num = 1001;

    SG(request_info).query_string = r->query.offset
                                    ? nxt_unit_sptr_get(&r->query) : NULL;
    SG(request_info).content_length = r->content_length;

    if (r->content_type_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->content_type_field;

        SG(request_info).content_type = nxt_unit_sptr_get(&f->value);
    }

    if (r->cookie_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->cookie_field;

        ctx->cookie = nxt_unit_sptr_get(&f->value);
    }

    SG(sapi_headers).http_response_code = 200;

    SG(request_info).path_translated = NULL;

#ifdef NXT_PHP7
    if (nxt_slow_path(php_request_startup() == FAILURE)) {
#else
    if (nxt_slow_path(php_request_startup(TSRMLS_C) == FAILURE)) {
#endif
        nxt_unit_req_debug(ctx->req, "php_request_startup() failed");

        nxt_unit_request_done(ctx->req, NXT_UNIT_ERROR);
        return;
    }

    if (ctx->chdir) {
        ctx->chdir = 0;
        nxt_php_vcwd_chdir(ctx->req, ctx->script_dirname.start);
    }

    nxt_memzero(&file_handle, sizeof(file_handle));

    file_handle.type = ZEND_HANDLE_FILENAME;
    file_handle.filename = (char *) ctx->script_filename.start;

    php_execute_script(&file_handle TSRMLS_CC);

    php_request_shutdown(NULL);

    nxt_unit_request_done(ctx->req, NXT_UNIT_OK);
}


nxt_inline void
nxt_php_vcwd_chdir(nxt_unit_request_info_t *req, u_char *dir)
{
    if (nxt_slow_path(VCWD_CHDIR((char *) dir) != 0)) {
        nxt_unit_req_alert(req, "VCWD_CHDIR(%s) failed (%d: %s)",
                           dir, errno, strerror(errno));
    }
}


static int
nxt_php_startup(sapi_module_struct *sapi_module)
{
    return php_module_startup(sapi_module, &nxt_php_unit_module, 1);
}


#ifdef NXT_PHP7
static size_t
nxt_php_unbuffered_write(const char *str, size_t str_length TSRMLS_DC)
#else
static int
nxt_php_unbuffered_write(const char *str, uint str_length TSRMLS_DC)
#endif
{
    int                rc;
    nxt_php_run_ctx_t  *ctx;

    ctx = SG(server_context);

    rc = nxt_unit_response_write(ctx->req, str, str_length);
    if (nxt_fast_path(rc == NXT_UNIT_OK)) {
        return str_length;
    }

    php_handle_aborted_connection();
    return 0;
}


static int
nxt_php_send_headers(sapi_headers_struct *sapi_headers TSRMLS_DC)
{
    int                      rc, fields_count;
    char                     *colon, *value;
    uint16_t                 status;
    uint32_t                 resp_size;
    nxt_php_run_ctx_t        *ctx;
    sapi_header_struct       *h;
    zend_llist_position      zpos;
    nxt_unit_request_info_t  *req;

    ctx = SG(server_context);
    req = ctx->req;

    nxt_unit_req_debug(req, "nxt_php_send_headers");

    if (SG(request_info).no_headers == 1) {
        rc = nxt_unit_response_init(req, 200, 0, 0);
        if (nxt_slow_path(rc != NXT_UNIT_OK)) {
            return SAPI_HEADER_SEND_FAILED;
        }

        return SAPI_HEADER_SENT_SUCCESSFULLY;
    }

    resp_size = 0;
    fields_count = zend_llist_count(&sapi_headers->headers);

    for (h = zend_llist_get_first_ex(&sapi_headers->headers, &zpos);
         h;
         h = zend_llist_get_next_ex(&sapi_headers->headers, &zpos))
    {
        resp_size += h->header_len;
    }

    status = SG(sapi_headers).http_response_code;

    rc = nxt_unit_response_init(req, status, fields_count, resp_size);
    if (nxt_slow_path(rc != NXT_UNIT_OK)) {
        return SAPI_HEADER_SEND_FAILED;
    }

    for (h = zend_llist_get_first_ex(&sapi_headers->headers, &zpos);
         h;
         h = zend_llist_get_next_ex(&sapi_headers->headers, &zpos))
    {
        colon = memchr(h->header, ':', h->header_len);
        if (nxt_slow_path(colon == NULL)) {
            nxt_unit_req_warn(req, "colon not found in header '%.*s'",
                              (int) h->header_len, h->header);
            continue;
        }

        value = colon + 1;
        while(isspace(*value)) {
            value++;
        }

        nxt_unit_response_add_field(req, h->header, colon - h->header,
                                    value,
                                    h->header_len - (value - h->header));
    }

    rc = nxt_unit_response_send(req);
    if (nxt_slow_path(rc != NXT_UNIT_OK)) {
        nxt_unit_req_debug(req, "failed to send response");

        return SAPI_HEADER_SEND_FAILED;
    }

    return SAPI_HEADER_SENT_SUCCESSFULLY;
}


#ifdef NXT_PHP7
static size_t
nxt_php_read_post(char *buffer, size_t count_bytes TSRMLS_DC)
#else
static int
nxt_php_read_post(char *buffer, uint count_bytes TSRMLS_DC)
#endif
{
    nxt_php_run_ctx_t  *ctx;

    ctx = SG(server_context);

    nxt_unit_req_debug(ctx->req, "nxt_php_read_post %d", (int) count_bytes);

    return nxt_unit_request_read(ctx->req, buffer, count_bytes);
}


static char *
nxt_php_read_cookies(TSRMLS_D)
{
    nxt_php_run_ctx_t  *ctx;

    ctx = SG(server_context);

    nxt_unit_req_debug(ctx->req, "nxt_php_read_cookies");

    return ctx->cookie;
}


static void
nxt_php_register_variables(zval *track_vars_array TSRMLS_DC)
{
    const char               *name;
    nxt_unit_field_t         *f, *f_end;
    nxt_php_run_ctx_t        *ctx;
    nxt_unit_request_t       *r;
    nxt_unit_request_info_t  *req;

    ctx = SG(server_context);

    req = ctx->req;
    r = req->request;

    nxt_unit_req_debug(req, "nxt_php_register_variables");

    php_register_variable_safe((char *) "SERVER_SOFTWARE",
                               (char *) nxt_server.start,
                               nxt_server.length, track_vars_array TSRMLS_CC);

    nxt_php_set_sptr(req, "SERVER_PROTOCOL", &r->version, r->version_length,
                     track_vars_array TSRMLS_CC);

    /*
     * 'PHP_SELF'
     * The filename of the currently executing script, relative to the document
     * root.  For instance, $_SERVER['PHP_SELF'] in a script at the address
     * http://example.com/foo/bar.php would be /foo/bar.php.  The __FILE__
     * constant contains the full path and filename of the current (i.e.
     * included) file.  If PHP is running as a command-line processor this
     * variable contains the script name since PHP 4.3.0. Previously it was not
     * available.
     */

    if (ctx->path_info.length != 0) {
        nxt_php_set_sptr(req, "PHP_SELF", &r->path, r->path_length,
                         track_vars_array TSRMLS_CC);

        nxt_php_set_str(req, "PATH_INFO", &ctx->path_info,
                        track_vars_array TSRMLS_CC);

    } else {
        nxt_php_set_str(req, "PHP_SELF", &ctx->script_name,
                        track_vars_array TSRMLS_CC);
    }

    /*
     * 'SCRIPT_NAME'
     * Contains the current script's path.  This is useful for pages which need
     * to point to themselves.  The __FILE__ constant contains the full path and
     * filename of the current (i.e. included) file.
     */

    nxt_php_set_str(req, "SCRIPT_NAME", &ctx->script_name,
                    track_vars_array TSRMLS_CC);

    /*
     * 'SCRIPT_FILENAME'
     * The absolute pathname of the currently executing script.
     */

    nxt_php_set_str(req, "SCRIPT_FILENAME", &ctx->script_filename,
                    track_vars_array TSRMLS_CC);

    /*
     * 'DOCUMENT_ROOT'
     * The document root directory under which the current script is executing,
     * as defined in the server's configuration file.
     */

    nxt_php_set_str(req, "DOCUMENT_ROOT", ctx->root,
                    track_vars_array TSRMLS_CC);

    nxt_php_set_sptr(req, "REQUEST_METHOD", &r->method, r->method_length,
                     track_vars_array TSRMLS_CC);
    nxt_php_set_sptr(req, "REQUEST_URI", &r->target, r->target_length,
                     track_vars_array TSRMLS_CC);
    nxt_php_set_sptr(req, "QUERY_STRING", &r->query, r->query_length,
                     track_vars_array TSRMLS_CC);

    nxt_php_set_sptr(req, "REMOTE_ADDR", &r->remote, r->remote_length,
                     track_vars_array TSRMLS_CC);
    nxt_php_set_sptr(req, "SERVER_ADDR", &r->local, r->local_length,
                     track_vars_array TSRMLS_CC);

    nxt_php_set_sptr(req, "SERVER_NAME", &r->server_name, r->server_name_length,
                     track_vars_array TSRMLS_CC);
    nxt_php_set_cstr(req, "SERVER_PORT", "80", 2, track_vars_array TSRMLS_CC);

    if (r->tls) {
        nxt_php_set_cstr(req, "HTTPS", "on", 2, track_vars_array TSRMLS_CC);
    }

    f_end = r->fields + r->fields_count;
    for (f = r->fields; f < f_end; f++) {
        name = nxt_unit_sptr_get(&f->name);

        nxt_php_set_sptr(req, name, &f->value, f->value_length,
                         track_vars_array TSRMLS_CC);
    }

    if (r->content_length_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->content_length_field;

        nxt_php_set_sptr(req, "CONTENT_LENGTH", &f->value, f->value_length,
                         track_vars_array TSRMLS_CC);
    }

    if (r->content_type_field != NXT_UNIT_NONE_FIELD) {
        f = r->fields + r->content_type_field;

        nxt_php_set_sptr(req, "CONTENT_TYPE", &f->value, f->value_length,
                         track_vars_array TSRMLS_CC);
    }
}


static void
nxt_php_set_sptr(nxt_unit_request_info_t *req, const char *name,
    nxt_unit_sptr_t *v, uint32_t len, zval *track_vars_array TSRMLS_DC)
{
    char  *str;

    str = nxt_unit_sptr_get(v);

    nxt_unit_req_debug(req, "php: register %s='%.*s'", name, (int) len, str);

    php_register_variable_safe((char *) name, str, len,
                               track_vars_array TSRMLS_CC);
}


nxt_inline void
nxt_php_set_str(nxt_unit_request_info_t *req, const char *name,
    nxt_str_t *s, zval *track_vars_array TSRMLS_DC)
{
    nxt_php_set_cstr(req, name, (char *) s->start, s->length,
                     track_vars_array TSRMLS_CC);
}


#ifdef NXT_PHP7

static void *
nxt_php_hash_str_find_ptr(const HashTable *ht, const nxt_str_t *str)
{
    return zend_hash_str_find_ptr(ht, (const char *) str->start, str->length);
}

#else

static void *
nxt_php_hash_str_find_ptr(const HashTable *ht, const nxt_str_t *str)
{
    int   ret;
    void  *entry;
    char  buf[256];

    if (nxt_slow_path(str->length >= (sizeof(buf) - 1))) {
        return NULL;
    }

    nxt_memcpy(buf, str->start, str->length);
    buf[str->length] = '\0';

    ret = zend_hash_find(ht, buf, str->length + 1, &entry);
    if (nxt_fast_path(ret == SUCCESS)) {
        return entry;
    }

    return NULL;
}

#endif


static void
nxt_php_set_cstr(nxt_unit_request_info_t *req, const char *name,
    const char *cstr, uint32_t len, zval *track_vars_array TSRMLS_DC)
{
    if (nxt_slow_path(cstr == NULL)) {
        return;
    }

    nxt_unit_req_debug(req, "php: register %s='%.*s'", name, (int) len, cstr);

    php_register_variable_safe((char *) name, (char *) cstr, len,
                               track_vars_array TSRMLS_CC);
}


#ifdef NXT_HAVE_PHP_LOG_MESSAGE_WITH_SYSLOG_TYPE
static void
nxt_php_log_message(char *message, int syslog_type_int)
#else
static void
nxt_php_log_message(char *message TSRMLS_DC)
#endif
{
    nxt_php_run_ctx_t  *ctx;

    ctx = SG(server_context);

    if (ctx != NULL) {
        nxt_unit_req_log(ctx->req, NXT_UNIT_LOG_NOTICE,
                         "php message: %s", message);

    } else {
        nxt_unit_log(nxt_php_unit_ctx, NXT_UNIT_LOG_NOTICE,
                     "php message: %s", message);
    }
}
