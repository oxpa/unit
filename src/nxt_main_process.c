
/*
 * Copyright (C) Igor Sysoev
 * Copyright (C) NGINX, Inc.
 */

#include <nxt_main.h>
#include <nxt_runtime.h>
#include <nxt_port.h>
#include <nxt_main_process.h>
#include <nxt_conf.h>
#include <nxt_application.h>


typedef struct {
    nxt_socket_t        socket;
    nxt_socket_error_t  error;
    u_char              *start;
    u_char              *end;
} nxt_listening_socket_t;


static nxt_int_t nxt_main_process_port_create(nxt_task_t *task,
    nxt_runtime_t *rt);
static void nxt_main_process_title(nxt_task_t *task);
static nxt_int_t nxt_main_start_controller_process(nxt_task_t *task,
    nxt_runtime_t *rt);
static nxt_int_t nxt_main_start_router_process(nxt_task_t *task,
    nxt_runtime_t *rt);
static nxt_int_t nxt_main_start_discovery_process(nxt_task_t *task,
    nxt_runtime_t *rt);
static nxt_int_t nxt_main_start_worker_process(nxt_task_t *task,
    nxt_runtime_t *rt, nxt_common_app_conf_t *app_conf, uint32_t stream);
static nxt_int_t nxt_main_create_worker_process(nxt_task_t *task,
    nxt_runtime_t *rt, nxt_process_init_t *init);
static void nxt_main_process_sigterm_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_main_process_sigquit_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_main_process_sigusr1_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_main_process_sigchld_handler(nxt_task_t *task, void *obj,
    void *data);
static void nxt_main_cleanup_worker_process(nxt_task_t *task, nxt_pid_t pid);
static void nxt_main_port_socket_handler(nxt_task_t *task,
    nxt_port_recv_msg_t *msg);
static nxt_int_t nxt_main_listening_socket(nxt_sockaddr_t *sa,
    nxt_listening_socket_t *ls);
static void nxt_main_port_modules_handler(nxt_task_t *task,
    nxt_port_recv_msg_t *msg);
static int nxt_cdecl nxt_app_lang_compare(const void *v1, const void *v2);
static void nxt_main_port_conf_store_handler(nxt_task_t *task,
    nxt_port_recv_msg_t *msg);


const nxt_sig_event_t  nxt_main_process_signals[] = {
    nxt_event_signal(SIGINT,  nxt_main_process_sigterm_handler),
    nxt_event_signal(SIGQUIT, nxt_main_process_sigquit_handler),
    nxt_event_signal(SIGTERM, nxt_main_process_sigterm_handler),
    nxt_event_signal(SIGCHLD, nxt_main_process_sigchld_handler),
    nxt_event_signal(SIGUSR1, nxt_main_process_sigusr1_handler),
    nxt_event_signal_end,
};


static nxt_bool_t  nxt_exiting;


nxt_int_t
nxt_main_process_start(nxt_thread_t *thr, nxt_task_t *task,
    nxt_runtime_t *rt)
{
    rt->types |= (1U << NXT_PROCESS_MAIN);

    if (nxt_main_process_port_create(task, rt) != NXT_OK) {
        return NXT_ERROR;
    }

    nxt_main_process_title(task);

    /*
     * The dicsovery process will send a message processed by
     * nxt_main_port_modules_handler() which starts the controller
     * and router processes.
     */
    return nxt_main_start_discovery_process(task, rt);
}


static nxt_conf_map_t  nxt_common_app_conf[] = {
    {
        nxt_string("type"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_common_app_conf_t, type),
    },

    {
        nxt_string("user"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_common_app_conf_t, user),
    },

    {
        nxt_string("group"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_common_app_conf_t, group),
    },

    {
        nxt_string("working_directory"),
        NXT_CONF_MAP_CSTRZ,
        offsetof(nxt_common_app_conf_t, working_directory),
    },

    {
        nxt_string("workers"),
        NXT_CONF_MAP_INT32,
        offsetof(nxt_common_app_conf_t, workers),
    },

    {
        nxt_string("path"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_common_app_conf_t, u.python.path),
    },

    {
        nxt_string("module"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_common_app_conf_t, u.python.module),
    },

    {
        nxt_string("root"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_common_app_conf_t, u.php.root),
    },

    {
        nxt_string("script"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_common_app_conf_t, u.php.script),
    },

    {
        nxt_string("index"),
        NXT_CONF_MAP_STR,
        offsetof(nxt_common_app_conf_t, u.php.index),
    },

    {
        nxt_string("executable"),
        NXT_CONF_MAP_CSTRZ,
        offsetof(nxt_common_app_conf_t, u.go.executable),
    },
};


static void
nxt_port_main_data_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    nxt_debug(task, "main data: %*s",
              nxt_buf_mem_used_size(&msg->buf->mem), msg->buf->mem.pos);
}


static void
nxt_port_main_start_worker_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    u_char                 *start;
    nxt_mp_t               *mp;
    nxt_int_t              ret;
    nxt_buf_t              *b;
    nxt_port_t             *port;
    nxt_conf_value_t       *conf;
    nxt_common_app_conf_t  app_conf;

    static nxt_str_t nobody = nxt_string("nobody");

    ret = NXT_ERROR;

    b = msg->buf;

    nxt_debug(task, "main start worker: %*s", b->mem.free - b->mem.pos,
              b->mem.pos);

    mp = nxt_mp_create(1024, 128, 256, 32);

    nxt_memzero(&app_conf, sizeof(nxt_common_app_conf_t));

    start = b->mem.pos;

    app_conf.name.start = start;
    app_conf.name.length = nxt_strlen(start);

    start += app_conf.name.length + 1;

    conf = nxt_conf_json_parse(mp, start, b->mem.free, NULL);

    if (conf == NULL) {
        nxt_log(task, NXT_LOG_CRIT, "configuration parsing error");

        goto failed;
    }

    app_conf.user = nobody;

    ret = nxt_conf_map_object(mp, conf, nxt_common_app_conf,
                              nxt_nitems(nxt_common_app_conf), &app_conf);
    if (ret != NXT_OK) {
        nxt_log(task, NXT_LOG_CRIT, "root map error");

        goto failed;
    }

    ret = nxt_main_start_worker_process(task, task->thread->runtime,
                                        &app_conf, msg->port_msg.stream);

failed:

    if (ret == NXT_ERROR) {
        port = nxt_runtime_port_find(task->thread->runtime, msg->port_msg.pid,
                                     msg->port_msg.reply_port);
        if (nxt_fast_path(port != NULL)) {
            nxt_port_socket_write(task, port, NXT_PORT_MSG_RPC_ERROR,
                                    -1, msg->port_msg.stream, 0, NULL);
        }
    }

    nxt_mp_destroy(mp);
}


static nxt_port_handler_t  nxt_main_process_port_handlers[] = {
    NULL, /* NXT_PORT_MSG_QUIT         */
    NULL, /* NXT_PORT_MSG_NEW_PORT     */
    NULL, /* NXT_PORT_MSG_CHANGE_FILE  */
    NULL, /* NXT_PORT_MSG_MMAP         */
    nxt_port_main_data_handler,
    NULL, /* NXT_PORT_MSG_REMOVE_PID   */
    nxt_port_ready_handler,
    nxt_port_main_start_worker_handler,
    nxt_main_port_socket_handler,
    nxt_main_port_modules_handler,
    nxt_main_port_conf_store_handler,
    nxt_port_rpc_handler,
    nxt_port_rpc_handler,
};


static nxt_int_t
nxt_main_process_port_create(nxt_task_t *task, nxt_runtime_t *rt)
{
    nxt_int_t      ret;
    nxt_port_t     *port;
    nxt_process_t  *process;

    process = nxt_runtime_process_get(rt, nxt_pid);
    if (nxt_slow_path(process == NULL)) {
        return NXT_ERROR;
    }

    port = nxt_port_new(task, 0, nxt_pid, NXT_PROCESS_MAIN);
    if (nxt_slow_path(port == NULL)) {
        return NXT_ERROR;
    }

    ret = nxt_port_socket_init(task, port, 0);
    if (nxt_slow_path(ret != NXT_OK)) {
        return ret;
    }

    nxt_process_port_add(task, process, port);

    nxt_runtime_port_add(rt, port);

    /*
     * A main process port.  A write port is not closed
     * since it should be inherited by worker processes.
     */
    nxt_port_enable(task, port, nxt_main_process_port_handlers);

    process->ready = 1;

    return NXT_OK;
}


static void
nxt_main_process_title(nxt_task_t *task)
{
    u_char      *p, *end;
    nxt_uint_t  i;
    u_char      title[2048];

    end = title + sizeof(title) - 1;

    p = nxt_sprintf(title, end, "unit: main [%s", nxt_process_argv[0]);

    for (i = 1; nxt_process_argv[i] != NULL; i++) {
        p = nxt_sprintf(p, end, " %s", nxt_process_argv[i]);
    }

    if (p < end) {
        *p++ = ']';
    }

    *p = '\0';

    nxt_process_title(task, "%s", title);
}


static nxt_int_t
nxt_main_start_controller_process(nxt_task_t *task, nxt_runtime_t *rt)
{
    ssize_t             n;
    nxt_int_t           ret;
    nxt_str_t           conf;
    nxt_file_t          file;
    nxt_file_info_t     fi;
    nxt_process_init_t  *init;

    conf.length = 0;

    nxt_memzero(&file, sizeof(nxt_file_t));

    file.name = (nxt_file_name_t *) rt->conf;

    if (nxt_file_open(task, &file, NXT_FILE_RDONLY, NXT_FILE_OPEN, 0) == NXT_OK) {

        if (nxt_fast_path(nxt_file_info(&file, &fi) == NXT_OK
                          && nxt_is_file(&fi)))
        {
            conf.length = nxt_file_size(&fi);
            conf.start = nxt_malloc(conf.length);

            if (nxt_slow_path(conf.start == NULL)) {
                nxt_file_close(task, &file);
                return NXT_ERROR;
            }

            n = nxt_file_read(&file, conf.start, conf.length, 0);

            if (nxt_slow_path(n != (ssize_t) conf.length)) {
                conf.length = 0;
                nxt_free(conf.start);

                nxt_log(task, NXT_LOG_ALERT,
                        "failed to restore previous configuration: "
                        "cannot read the file");
            }
        }

        nxt_file_close(task, &file);
    }

    init = nxt_malloc(sizeof(nxt_process_init_t));
    if (nxt_slow_path(init == NULL)) {
        return NXT_ERROR;
    }

    init->start = nxt_controller_start;
    init->name = "controller";
    init->user_cred = &rt->user_cred;
    init->port_handlers = nxt_controller_process_port_handlers;
    init->signals = nxt_worker_process_signals;
    init->type = NXT_PROCESS_CONTROLLER;
    init->data = &conf;
    init->stream = 0;
    init->restart = 1;

    ret = nxt_main_create_worker_process(task, rt, init);

    if (ret == NXT_OK && conf.length != 0) {
        nxt_free(conf.start);
    }

    return ret;
}


static nxt_int_t
nxt_main_start_discovery_process(nxt_task_t *task, nxt_runtime_t *rt)
{
    nxt_process_init_t  *init;

    init = nxt_malloc(sizeof(nxt_process_init_t));
    if (nxt_slow_path(init == NULL)) {
        return NXT_ERROR;
    }

    init->start = nxt_discovery_start;
    init->name = "discovery";
    init->user_cred = &rt->user_cred;
    init->port_handlers = nxt_discovery_process_port_handlers;
    init->signals = nxt_worker_process_signals;
    init->type = NXT_PROCESS_DISCOVERY;
    init->data = rt;
    init->stream = 0;
    init->restart = 0;

    return nxt_main_create_worker_process(task, rt, init);
}


static nxt_int_t
nxt_main_start_router_process(nxt_task_t *task, nxt_runtime_t *rt)
{
    nxt_process_init_t  *init;

    init = nxt_malloc(sizeof(nxt_process_init_t));
    if (nxt_slow_path(init == NULL)) {
        return NXT_ERROR;
    }

    init->start = nxt_router_start;
    init->name = "router";
    init->user_cred = &rt->user_cred;
    init->port_handlers = nxt_router_process_port_handlers;
    init->signals = nxt_worker_process_signals;
    init->type = NXT_PROCESS_ROUTER;
    init->data = rt;
    init->stream = 0;
    init->restart = 1;

    return nxt_main_create_worker_process(task, rt, init);
}


static nxt_int_t
nxt_main_start_worker_process(nxt_task_t *task, nxt_runtime_t *rt,
    nxt_common_app_conf_t *app_conf, uint32_t stream)
{
    char                *user, *group;
    u_char              *title, *last, *end;
    size_t              size;
    nxt_process_init_t  *init;

    size = sizeof(nxt_process_init_t)
           + sizeof(nxt_user_cred_t)
           + app_conf->user.length + 1
           + app_conf->group.length + 1
           + app_conf->name.length + sizeof("\"\" application");

    init = nxt_malloc(size);
    if (nxt_slow_path(init == NULL)) {
        return NXT_ERROR;
    }

    init->user_cred = nxt_pointer_to(init, sizeof(nxt_process_init_t));
    user = nxt_pointer_to(init->user_cred, sizeof(nxt_user_cred_t));

    nxt_memcpy(user, app_conf->user.start, app_conf->user.length);
    last = nxt_pointer_to(user, app_conf->user.length);
    *last++ = '\0';

    init->user_cred->user = user;

    if (app_conf->group.start != NULL) {
        group = (char *) last;

        nxt_memcpy(group, app_conf->group.start, app_conf->group.length);
        last = nxt_pointer_to(group, app_conf->group.length);
        *last++ = '\0';

    } else {
        group = NULL;
    }

    if (nxt_user_cred_get(task, init->user_cred, group) != NXT_OK) {
        return NXT_ERROR;
    }

    title = last;
    end = title + app_conf->name.length + sizeof("\"\" application");

    nxt_sprintf(title, end, "\"%V\" application%Z", &app_conf->name);

    init->start = nxt_app_start;
    init->name = (char *) title;
    init->port_handlers = nxt_app_process_port_handlers;
    init->signals = nxt_worker_process_signals;
    init->type = NXT_PROCESS_WORKER;
    init->data = app_conf;
    init->stream = stream;
    init->restart = 0;

    return nxt_main_create_worker_process(task, rt, init);
}


static nxt_int_t
nxt_main_create_worker_process(nxt_task_t *task, nxt_runtime_t *rt,
    nxt_process_init_t *init)
{
    nxt_int_t      ret;
    nxt_pid_t      pid;
    nxt_port_t     *port;
    nxt_process_t  *process;

    /*
     * TODO: remove process, init, ports from array on memory and fork failures.
     */

    process = nxt_runtime_process_new(rt);
    if (nxt_slow_path(process == NULL)) {
        return NXT_ERROR;
    }

    process->init = init;

    port = nxt_port_new(task, 0, 0, init->type);
    if (nxt_slow_path(port == NULL)) {
        nxt_runtime_process_remove(rt, process);
        return NXT_ERROR;
    }

    nxt_process_port_add(task, process, port);

    ret = nxt_port_socket_init(task, port, 0);
    if (nxt_slow_path(ret != NXT_OK)) {
        nxt_mp_release(port->mem_pool, port);
        return ret;
    }

    pid = nxt_process_create(task, process);

    switch (pid) {

    case -1:
        return NXT_ERROR;

    case 0:
        /* A worker process, return to the event engine work queue loop. */
        return NXT_AGAIN;

    default:
        /* The main process created a new process. */

        nxt_port_read_close(port);
        nxt_port_write_enable(task, port);

        return NXT_OK;
    }
}


void
nxt_main_stop_worker_processes(nxt_task_t *task, nxt_runtime_t *rt)
{
    nxt_port_t     *port;
    nxt_process_t  *process;

    nxt_runtime_process_each(rt, process) {

        if (nxt_pid != process->pid) {
            process->init = NULL;

            nxt_process_port_each(process, port) {

                (void) nxt_port_socket_write(task, port, NXT_PORT_MSG_QUIT,
                                             -1, 0, 0, NULL);

            } nxt_process_port_loop;
        }

    } nxt_runtime_process_loop;
}



static void
nxt_main_process_sigterm_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_debug(task, "sigterm handler signo:%d (%s)",
              (int) (uintptr_t) obj, data);

    /* TODO: fast exit. */

    nxt_exiting = 1;

    nxt_runtime_quit(task);
}


static void
nxt_main_process_sigquit_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_debug(task, "sigquit handler signo:%d (%s)",
              (int) (uintptr_t) obj, data);

    /* TODO: graceful exit. */

    nxt_exiting = 1;

    nxt_runtime_quit(task);
}


static void
nxt_main_process_sigusr1_handler(nxt_task_t *task, void *obj, void *data)
{
    nxt_mp_t        *mp;
    nxt_int_t       ret;
    nxt_uint_t      n;
    nxt_file_t      *file, *new_file;
    nxt_runtime_t   *rt;
    nxt_array_t     *new_files;

    nxt_log(task, NXT_LOG_NOTICE, "signal %d (%s) recevied, %s",
            (int) (uintptr_t) obj, data, "log files rotation");

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return;
    }

    rt = task->thread->runtime;

    n = nxt_list_nelts(rt->log_files);

    new_files = nxt_array_create(mp, n, sizeof(nxt_file_t));
    if (new_files == NULL) {
        nxt_mp_destroy(mp);
        return;
    }

    nxt_list_each(file, rt->log_files) {

        /* This allocation cannot fail. */
        new_file = nxt_array_add(new_files);

        new_file->name = file->name;
        new_file->fd = NXT_FILE_INVALID;
        new_file->log_level = NXT_LOG_CRIT;

        ret = nxt_file_open(task, new_file, O_WRONLY | O_APPEND, O_CREAT,
                            NXT_FILE_OWNER_ACCESS);

        if (ret != NXT_OK) {
            goto fail;
        }

    } nxt_list_loop;

    new_file = new_files->elts;

    ret = nxt_file_stderr(&new_file[0]);

    if (ret == NXT_OK) {
        n = 0;

        nxt_list_each(file, rt->log_files) {

            nxt_port_change_log_file(task, rt, n, new_file[n].fd);
            /*
             * The old log file descriptor must be closed at the moment
             * when no other threads use it.  dup2() allows to use the
             * old file descriptor for new log file.  This change is
             * performed atomically in the kernel.
             */
            (void) nxt_file_redirect(file, new_file[n].fd);

            n++;

        } nxt_list_loop;

        nxt_mp_destroy(mp);
        return;
   }

fail:

    new_file = new_files->elts;
    n = new_files->nelts;

    while (n != 0) {
        if (new_file->fd != NXT_FILE_INVALID) {
            nxt_file_close(task, new_file);
        }

        new_file++;
        n--;
    }

    nxt_mp_destroy(mp);
}


static void
nxt_main_process_sigchld_handler(nxt_task_t *task, void *obj, void *data)
{
    int                    status;
    nxt_err_t              err;
    nxt_pid_t              pid;

    nxt_debug(task, "sigchld handler signo:%d (%s)",
              (int) (uintptr_t) obj, data);

    for ( ;; ) {
        pid = waitpid(-1, &status, WNOHANG);

        if (pid == -1) {

            switch (err = nxt_errno) {

            case NXT_ECHILD:
                return;

            case NXT_EINTR:
                continue;

            default:
                nxt_log(task, NXT_LOG_CRIT, "waitpid() failed: %E", err);
                return;
            }
        }

        nxt_debug(task, "waitpid(): %PI", pid);

        if (pid == 0) {
            return;
        }

        if (WTERMSIG(status)) {
#ifdef WCOREDUMP
            nxt_log(task, NXT_LOG_CRIT, "process %PI exited on signal %d%s",
                    pid, WTERMSIG(status),
                    WCOREDUMP(status) ? " (core dumped)" : "");
#else
            nxt_log(task, NXT_LOG_CRIT, "process %PI exited on signal %d",
                    pid, WTERMSIG(status));
#endif

        } else {
            nxt_trace(task, "process %PI exited with code %d",
                      pid, WEXITSTATUS(status));
        }

        nxt_main_cleanup_worker_process(task, pid);
    }
}


static void
nxt_main_cleanup_worker_process(nxt_task_t *task, nxt_pid_t pid)
{
    nxt_buf_t           *buf;
    nxt_port_t          *port;
    nxt_runtime_t       *rt;
    nxt_process_t       *process;
    nxt_process_init_t  *init;

    rt = task->thread->runtime;

    process = nxt_runtime_process_find(rt, pid);

    if (process) {
        init = process->init;

        nxt_runtime_process_remove(rt, process);

        if (!nxt_exiting) {
            nxt_runtime_process_each(rt, process) {

                if (process->pid == nxt_pid
                    || process->pid == pid
                    || nxt_queue_is_empty(&process->ports))
                {
                    continue;
                }

                port = nxt_process_port_first(process);

                buf = nxt_buf_mem_alloc(port->mem_pool, sizeof(pid), 0);
                buf->mem.free = nxt_cpymem(buf->mem.free, &pid, sizeof(pid));

                nxt_port_socket_write(task, port, NXT_PORT_MSG_REMOVE_PID,
                                      -1, init->stream, 0, buf);
            } nxt_runtime_process_loop;
        }

        if (nxt_exiting) {

            if (rt->nprocesses == 2) {
                nxt_runtime_quit(task);
            }

        } else if (init != NULL) {
            if (init->restart != 0) {
                (void) nxt_main_create_worker_process(task, rt, init);

            } else {
                nxt_free(init);
            }
        }
    }
}


static void
nxt_main_port_socket_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    size_t                  size;
    nxt_int_t               ret;
    nxt_buf_t               *b, *out;
    nxt_port_t              *port;
    nxt_sockaddr_t          *sa;
    nxt_port_msg_type_t     type;
    nxt_listening_socket_t  ls;
    u_char                  message[2048];

    b = msg->buf;
    sa = (nxt_sockaddr_t *) b->mem.pos;

    out = NULL;

    ls.socket = -1;
    ls.error = NXT_SOCKET_ERROR_SYSTEM;
    ls.start = message;
    ls.end = message + sizeof(message);

    port = nxt_runtime_port_find(task->thread->runtime, msg->port_msg.pid,
                                 msg->port_msg.reply_port);

    nxt_debug(task, "listening socket \"%*s\"",
              sa->length, nxt_sockaddr_start(sa));

    ret = nxt_main_listening_socket(sa, &ls);

    if (ret == NXT_OK) {
        nxt_debug(task, "socket(\"%*s\"): %d",
                  sa->length, nxt_sockaddr_start(sa), ls.socket);

        type = NXT_PORT_MSG_RPC_READY_LAST | NXT_PORT_MSG_CLOSE_FD;

    } else {
        size = ls.end - ls.start;

        nxt_log(task, NXT_LOG_CRIT, "%*s", size, ls.start);

        out = nxt_buf_mem_alloc(port->mem_pool, size + 1, 0);
        if (nxt_slow_path(out == NULL)) {
            return;
        }

        *out->mem.free++ = (uint8_t) ls.error;

        out->mem.free = nxt_cpymem(out->mem.free, ls.start, size);

        type = NXT_PORT_MSG_RPC_ERROR;
    }

    nxt_port_socket_write(task, port, type, ls.socket, msg->port_msg.stream,
                          0, out);
}


static nxt_int_t
nxt_main_listening_socket(nxt_sockaddr_t *sa, nxt_listening_socket_t *ls)
{
    nxt_err_t         err;
    nxt_socket_t      s;

    const socklen_t   length = sizeof(int);
    static const int  enable = 1;

    s = socket(sa->u.sockaddr.sa_family, sa->type, 0);

    if (nxt_slow_path(s == -1)) {
        err = nxt_errno;

#if (NXT_INET6)

        if (err == EAFNOSUPPORT && sa->u.sockaddr.sa_family == AF_INET6) {
            ls->error = NXT_SOCKET_ERROR_NOINET6;
        }

#endif

        ls->end = nxt_sprintf(ls->start, ls->end,
                              "socket(\\\"%*s\\\") failed %E",
                              sa->length, nxt_sockaddr_start(sa), err);

        return NXT_ERROR;
    }

    if (setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &enable, length) != 0) {
        ls->end = nxt_sprintf(ls->start, ls->end,
                              "setsockopt(\\\"%*s\\\", SO_REUSEADDR) failed %E",
                              sa->length, nxt_sockaddr_start(sa), nxt_errno);
        goto fail;
    }

#if (NXT_INET6)

    if (sa->u.sockaddr.sa_family == AF_INET6) {

        if (setsockopt(s, IPPROTO_IPV6, IPV6_V6ONLY, &enable, length) != 0) {
            ls->end = nxt_sprintf(ls->start, ls->end,
                               "setsockopt(\\\"%*s\\\", IPV6_V6ONLY) failed %E",
                               sa->length, nxt_sockaddr_start(sa), nxt_errno);
            goto fail;
        }
    }

#endif

    if (bind(s, &sa->u.sockaddr, sa->socklen) != 0) {
        err = nxt_errno;

#if (NXT_HAVE_UNIX_DOMAIN)

        if (sa->u.sockaddr.sa_family == AF_UNIX) {
            switch (err) {

            case EACCES:
                ls->error = NXT_SOCKET_ERROR_ACCESS;
                break;

            case ENOENT:
            case ENOTDIR:
                ls->error = NXT_SOCKET_ERROR_PATH;
                break;
            }

            goto next;
        }

#endif

        switch (err) {

        case EACCES:
            ls->error = NXT_SOCKET_ERROR_PORT;
            break;

        case EADDRINUSE:
            ls->error = NXT_SOCKET_ERROR_INUSE;
            break;

        case EADDRNOTAVAIL:
            ls->error = NXT_SOCKET_ERROR_NOADDR;
            break;
        }

        ls->end = nxt_sprintf(ls->start, ls->end, "bind(\\\"%*s\\\") failed %E",
                              sa->length, nxt_sockaddr_start(sa), err);
        goto fail;
    }

#if (NXT_HAVE_UNIX_DOMAIN)

next:

    if (sa->u.sockaddr.sa_family == AF_UNIX) {
        char     *filename;
        mode_t   access;

        filename = sa->u.sockaddr_un.sun_path;
        access = (S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

        if (chmod(filename, access) != 0) {
            ls->end = nxt_sprintf(ls->start, ls->end,
                                  "chmod(\\\"%*s\\\") failed %E",
                                  filename, nxt_errno);
            goto fail;
        }
    }

#endif

    ls->socket = s;

    return NXT_OK;

fail:

    (void) close(s);

    return NXT_ERROR;
}


static nxt_conf_map_t  nxt_app_lang_module_map[] = {
    {
        nxt_string("type"),
        NXT_CONF_MAP_STR_COPY,
        offsetof(nxt_app_lang_module_t, type),
    },

    {
        nxt_string("version"),
        NXT_CONF_MAP_STR_COPY,
        offsetof(nxt_app_lang_module_t, version),
    },

    {
        nxt_string("file"),
        NXT_CONF_MAP_CSTRZ,
        offsetof(nxt_app_lang_module_t, file),
    },
};


static void
nxt_main_port_modules_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    uint32_t               index;
    nxt_mp_t               *mp;
    nxt_int_t              ret;
    nxt_buf_t              *b;
    nxt_runtime_t          *rt;
    nxt_conf_value_t       *conf, *root, *value;
    nxt_app_lang_module_t  *lang;

    static nxt_str_t   root_path = nxt_string("/");

    rt = task->thread->runtime;

    if (msg->port_msg.pid != rt->port_by_type[NXT_PROCESS_DISCOVERY]->pid) {
        return;
    }

    b = msg->buf;

    if (b == NULL) {
        return;
    }

    nxt_debug(task, "application languages: \"%*s\"",
              b->mem.free - b->mem.pos, b->mem.pos);

    mp = nxt_mp_create(1024, 128, 256, 32);
    if (mp == NULL) {
        return;
    }

    conf = nxt_conf_json_parse(mp, b->mem.pos, b->mem.free, NULL);
    if (conf == NULL) {
        goto fail;
    }

    root = nxt_conf_get_path(conf, &root_path);
    if (root == NULL) {
        goto fail;
    }

    for (index = 0; /* void */ ; index++) {
        value = nxt_conf_get_array_element(root, index);
        if (value == NULL) {
            break;
        }

        lang = nxt_array_add(rt->languages);
        if (lang == NULL) {
            goto fail;
        }

        lang->module = NULL;

        ret = nxt_conf_map_object(rt->mem_pool, value, nxt_app_lang_module_map,
                                  nxt_nitems(nxt_app_lang_module_map), lang);

        if (ret != NXT_OK) {
            goto fail;
        }

        nxt_debug(task, "lang %V %V \"%s\"",
                  &lang->type, &lang->version, lang->file);
    }

    qsort(rt->languages->elts, rt->languages->nelts,
          sizeof(nxt_app_lang_module_t), nxt_app_lang_compare);

fail:

    nxt_mp_destroy(mp);

    ret = nxt_main_start_controller_process(task, rt);

    if (ret == NXT_OK) {
        (void) nxt_main_start_router_process(task, rt);
    }
}


static int nxt_cdecl
nxt_app_lang_compare(const void *v1, const void *v2)
{
    int                          n;
    size_t                       length;
    const nxt_app_lang_module_t  *lang1, *lang2;

    lang1 = v1;
    lang2 = v2;

    if (lang1->type.length != lang2->type.length) {
        return lang1->type.length - lang2->type.length;
    }

    n = nxt_strncmp(lang1->type.start, lang2->type.start, lang1->type.length);

    if (n != 0) {
        return n;
    }

    length = nxt_min(lang1->version.length, lang2->version.length);

    n = nxt_strncmp(lang1->version.start, lang2->version.start, length);

    if (n == 0) {
        n = lang1->version.length - lang2->version.length;
    }

    /* Negate result to move higher versions to the beginning. */

    return -n;
}


static void
nxt_main_port_conf_store_handler(nxt_task_t *task, nxt_port_recv_msg_t *msg)
{
    ssize_t        n, size;
    nxt_buf_t      *b;
    nxt_int_t      ret;
    nxt_file_t     file;
    nxt_runtime_t  *rt;

    nxt_memzero(&file, sizeof(nxt_file_t));

    rt = task->thread->runtime;

    file.name = (nxt_file_name_t *) rt->conf_tmp;

    if (nxt_slow_path(nxt_file_open(task, &file, NXT_FILE_WRONLY,
                                    NXT_FILE_TRUNCATE, NXT_FILE_OWNER_ACCESS)
                      != NXT_OK))
    {
        goto error;
    }

    for (b = msg->buf; b != NULL; b = b->next) {
        size = nxt_buf_mem_used_size(&b->mem);

        n = nxt_file_write(&file, b->mem.pos, size, 0);

        if (nxt_slow_path(n != size)) {
            nxt_file_close(task, &file);
            (void) nxt_file_delete(file.name);
            goto error;
        }
    }

    nxt_file_close(task, &file);

    ret = nxt_file_rename(file.name, (nxt_file_name_t *) rt->conf);

    if (nxt_fast_path(ret == NXT_OK)) {
        return;
    }

error:

    nxt_log(task, NXT_LOG_ALERT, "failed to store current configuration");
}
