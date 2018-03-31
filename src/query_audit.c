#include <mysql/plugin.h>
#include <mysql/plugin_audit.h>

#include "cJSON.h"

static MYSQL_PLUGIN query_audit_plugin;

// How to access the pthread_mutex in mysql_mutex_t
#define mysql_mutex_real_mutex(A) &(A)->m_mutex
#define log_mutex_init(A,B,C) pthread_mutex_init(mysql_mutex_real_mutex(B), C)
#define log_mutex_destroy(A) pthread_mutex_destroy(mysql_mutex_real_mutex(A))
#define log_mutex_lock(A) pthread_mutex_lock(mysql_mutex_real_mutex(A))
#define log_mutex_unlock(A) pthread_mutex_unlock(mysql_mutex_real_mutex(A))

static PSI_mutex_key key_LOCK_operations;
static PSI_mutex_info mutex_key_list[] = {
    {
        &key_LOCK_operations, "QUERY_AUDIT_plugin::lock_operations",
        PSI_FLAG_GLOBAL
    }
};

static mysql_mutex_t lock_operations;

static char * log_file;
static FILE * log_file_p;
static char log_file_default[25] = "/var/log/query_audit.log";
static char log_file_buffer[256];

static char * log_databases;
static char * log_databases_list[32];
static char log_databases_buffer[512];

static char * log_tables;
static char * log_tables_list[32];
static char log_tables_buffer[512];

static char always_fflush;

static char empty_str[1] = { 0 };

static void update_log_file(MYSQL_THD, struct st_mysql_sys_var *, void *, const void *);
static void update_log_tables(MYSQL_THD, struct st_mysql_sys_var *, void *, const void *);
static void update_log_databases(MYSQL_THD, struct st_mysql_sys_var *, void *, const void *);
static void update_always_fflush(MYSQL_THD, struct st_mysql_sys_var *, void *, const void *);

static MYSQL_SYSVAR_STR(log_file, log_file, PLUGIN_VAR_RQCMDARG,
    "Path to log file.", NULL,update_log_file, log_file_default);

static MYSQL_SYSVAR_STR(log_tables, log_tables, PLUGIN_VAR_RQCMDARG,
    "Comma separated list of tables to log.", NULL, update_log_tables, NULL);

static MYSQL_SYSVAR_STR(log_databases, log_databases, PLUGIN_VAR_RQCMDARG,
    "Comma separated list of databases to log.", NULL, update_log_databases, NULL);

static MYSQL_SYSVAR_BOOL(always_fflush, always_fflush, PLUGIN_VAR_OPCMDARG,
    "Toggle calling fflush(3) after writing every log.", NULL, update_always_fflush, TRUE);

static MYSQL_THDVAR_STR(connection_user,
    PLUGIN_VAR_THDLOCAL | PLUGIN_VAR_READONLY | PLUGIN_VAR_NOSYSVAR | PLUGIN_VAR_NOCMDOPT | PLUGIN_VAR_MEMALLOC,
    "(internal)", NULL, NULL, NULL);

static struct st_mysql_sys_var * query_audit_system_variables[] = {
    MYSQL_SYSVAR(log_file),
    MYSQL_SYSVAR(log_tables),
    MYSQL_SYSVAR(log_databases),
    MYSQL_SYSVAR(always_fflush),
    MYSQL_SYSVAR(connection_user),
    NULL
};

static int
query_audit_plugin_init(MYSQL_PLUGIN plugin) {

    query_audit_plugin = plugin;

    my_plugin_log_message(&query_audit_plugin, MY_INFORMATION_LEVEL, "Initializing");

    PSI_server->register_mutex("query_audit", mutex_key_list, 1);

    log_mutex_init(key_LOCK_operations, &lock_operations, MY_MUTEX_INIT_FAST);

    log_mutex_lock(&lock_operations);

    log_file_p = fopen(log_file, "a+");

    if (log_file_p == NULL) {

        my_plugin_log_message(&query_audit_plugin, MY_ERROR_LEVEL,
            "Unable to create or open log file: %s", log_file);

        log_mutex_unlock(&lock_operations);

        log_mutex_destroy(&lock_operations);

        return 1;
    }

    my_plugin_log_message(&query_audit_plugin, MY_INFORMATION_LEVEL,
        "[query_audit_log_file] set to: %s", log_file);

    log_mutex_unlock(&lock_operations);

    update_log_databases(NULL, NULL, NULL, &log_databases);

    update_log_tables(NULL, NULL, NULL, &log_tables);

    update_always_fflush(NULL, NULL, NULL, &always_fflush);

    update_log_file(NULL, NULL, NULL, &log_file);

    return 0;
}

static int
query_audit_plugin_deinit(void *arg MY_ATTRIBUTE((unused))) {

    my_plugin_log_message(&query_audit_plugin, MY_INFORMATION_LEVEL, "Shutting down");

    fflush(log_file_p);

    fclose(log_file_p);

    log_mutex_destroy(&lock_operations);

    return 0;
}

static int in_list(const char * needle, char ** haystack) {

    int result, i;
    result = 0;
    i = 0;
    while (haystack[i] != NULL) {
        if (strcmp(haystack[i], needle) == 0) {
            result = 1;
            break;
        }
        i++;
    }

    return result;
}

static void
query_audit_write_log(const struct mysql_event_table_access *event_table, MYSQL_THD thd) {

    cJSON *root;
    char * json;
    time_t raw_time;
    struct tm * time_info;

    // "%F %T" = "YYYY-MM-DD HH:MM:SS" (19+1 bytes)
    char time_buffer[20];

    time(&raw_time);
    time_info = localtime (&raw_time);
    strftime(time_buffer, 20, "%F %T", time_info);

    root = cJSON_CreateObject();

    cJSON_AddItemToObject(root, "timestamp", cJSON_CreateString(time_buffer));

    cJSON_AddItemToObject(root, "connection_user", cJSON_CreateString(THDVAR(thd, connection_user)));

    cJSON_AddItemToObject(root, "db", cJSON_CreateString(event_table->table_database.str));

    cJSON_AddItemToObject(root, "table", cJSON_CreateString(event_table->table_name.str));

    cJSON_AddItemToObject(root, "query", cJSON_CreateString(event_table->query.str));

    json = cJSON_PrintUnformatted(root);

    fprintf(log_file_p, "%s\n", json);

    free(json);

    cJSON_Delete(root);

    if (always_fflush) {
        if (fflush(log_file_p) != 0) {
            my_plugin_log_message(&query_audit_plugin, MY_ERROR_LEVEL,
                "Failed to call fflush on log_file: %s", log_file);
        }
    }
}

static int
query_audit_notify(MYSQL_THD thd, mysql_event_class_t event_class, const void *event) {

    const struct mysql_event_connection * event_connection;
    const struct mysql_event_table_access * event_table;

    log_mutex_lock(&lock_operations);

    if (event_class == MYSQL_AUDIT_CONNECTION_CLASS) {

        event_connection = (const struct mysql_event_connection *) event;

        if (event_connection->event_subclass == MYSQL_AUDIT_CONNECTION_CONNECT ||
                event_connection->event_subclass == MYSQL_AUDIT_CONNECTION_CHANGE_USER) {

            asprintf(&THDVAR(thd, connection_user),
                "%s@%s",
                event_connection->user.str,
                (event_connection->host.str != NULL) ? event_connection->host.str : event_connection->ip.str);

        }

    } else if (event_class == MYSQL_AUDIT_TABLE_ACCESS_CLASS) {

        event_table = (const struct mysql_event_table_access *) event;

        if (event_table->event_subclass == MYSQL_AUDIT_TABLE_ACCESS_READ &&
                in_list(event_table->table_database.str, log_databases_list) &&
                in_list(event_table->table_name.str, log_tables_list)) {

            query_audit_write_log(event_table, thd);
        }
    }

    log_mutex_unlock(&lock_operations);

    return 0;
}

static void
update_always_fflush(MYSQL_THD thd __attribute__((unused)),
        struct st_mysql_sys_var * var  __attribute__((unused)),
        void *var_ptr  __attribute__((unused)), const void * save) {

    log_mutex_lock(&lock_operations);

    always_fflush = *(char *) save;

    my_plugin_log_message(&query_audit_plugin, MY_INFORMATION_LEVEL,
        "[query_audit_always_fflush] set to: %s", (always_fflush) ? "ON" : "OFF");

    log_mutex_unlock(&lock_operations);
}

static void
update_log_file(MYSQL_THD thd __attribute__((unused)),
        struct st_mysql_sys_var * var  __attribute__((unused)),
        void *var_ptr  __attribute__((unused)), const void *save) {

    char *new_log_file;

    log_mutex_lock(&lock_operations);

    new_log_file = (*(char **) save) ? *(char **) save : empty_str;

    my_plugin_log_message(&query_audit_plugin, MY_INFORMATION_LEVEL,
        "[query_audit_log_file] set to: %s", new_log_file);

    strncpy(log_file_buffer, new_log_file, sizeof(log_file_buffer));

    log_file_buffer[sizeof(log_file_buffer) - 1] = 0;

    log_file = log_file_buffer;

    fclose(log_file_p);

    log_file_p = fopen(log_file, "a+");

    if (log_file_p == NULL) {
        my_plugin_log_message(&query_audit_plugin, MY_ERROR_LEVEL,
            "Could not create or open file: %s", log_file);
    }

    log_mutex_unlock(&lock_operations);
}

// XXX handle values > log_tables_buffer length
static void
update_log_tables(MYSQL_THD thd __attribute__((unused)),
        struct st_mysql_sys_var * var __attribute__((unused)),
        void *var_ptr  __attribute__((unused)), const void *save) {

    char * new_tables;
    int i;
    char * p;

    log_mutex_lock(&lock_operations);

    new_tables = (*(char **) save) ? *(char **) save : empty_str;

    strncpy(log_tables_buffer, new_tables, sizeof(log_tables_buffer));

    log_tables_buffer[sizeof(log_tables_buffer) - 1] = 0;

    log_tables = log_tables_buffer;

    my_plugin_log_message(&query_audit_plugin, MY_INFORMATION_LEVEL,
        "[query_audit_log_tables] set to: %s", new_tables);

    i = 0;
    while (log_tables_list[i] != NULL) {
        free(log_tables_list[i]);
        i++;
    }

    i = 0;
    for (p = strtok(new_tables, ","); p != NULL; p = strtok(NULL, ",")) {
        log_tables_list[i] = strdup(p);
        i++;
    }

    log_tables_list[i] = NULL;

    log_mutex_unlock(&lock_operations);

}

// XXX handle values > log_databases_buffer length
static void
update_log_databases(MYSQL_THD thd __attribute__((unused)),
        struct st_mysql_sys_var * var __attribute__((unused)),
        void *var_ptr  __attribute__((unused)), const void *save) {

    char * new_databases;
    int i;
    char * p;

    log_mutex_lock(&lock_operations);

    new_databases = (*(char **) save) ? *(char **) save : empty_str;

    strncpy(log_databases_buffer, new_databases, sizeof(log_databases_buffer));

    log_databases_buffer[sizeof(log_databases_buffer) - 1] = 0;

    log_databases = log_databases_buffer;

    my_plugin_log_message(&query_audit_plugin, MY_INFORMATION_LEVEL,
        "[query_audit_log_databases] set to: %s", new_databases);

    i = 0;
    while (log_databases_list[i] != NULL) {
        free(log_databases_list[i]);
        i++;
    }

    i = 0;
    for (p = strtok(new_databases, ","); p != NULL; p = strtok(NULL, ",")) {
        log_databases_list[i] = strdup(p);
        i++;
    }

    log_databases_list[i] = NULL;

    log_mutex_unlock(&lock_operations);

}

static struct st_mysql_audit query_audit_descriptor = {
    MYSQL_AUDIT_INTERFACE_VERSION,
    NULL,
    query_audit_notify,
    {
        0,
        (unsigned long) MYSQL_AUDIT_CONNECTION_CONNECT | MYSQL_AUDIT_CONNECTION_CHANGE_USER,
        0,
        0,
        (unsigned long) MYSQL_AUDIT_TABLE_ACCESS_ALL,
        0,
        0,
        0,
        0,
        0,
        0
    }
};

mysql_declare_plugin(query_audit) {
    MYSQL_AUDIT_PLUGIN,
    &query_audit_descriptor,
    "QUERY_AUDIT",
    "VALID USA",
    "Log users that query particular tables",
    PLUGIN_LICENSE_BSD,
    query_audit_plugin_init,
    query_audit_plugin_deinit,
    0x0001,
    NULL,
    query_audit_system_variables,
    NULL,
    0
}
mysql_declare_plugin_end;
