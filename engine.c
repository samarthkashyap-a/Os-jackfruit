/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
 *
 * Implements:
 *   - Long-running supervisor daemon (Task 1, 2)
 *   - Bounded-buffer concurrent logging pipeline (Task 3)
 *   - Kernel monitor integration via ioctl (Task 4)
 *   - Full CLI: supervisor / start / run / ps / logs / stop
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "monitor_ioctl.h"

/* ---------------------------------------------------------------
 * Constants
 * --------------------------------------------------------------- */
#define STACK_SIZE          (1024 * 1024)
#define CONTAINER_ID_LEN    64
#define CONTROL_PATH        "/tmp/mini_runtime.sock"
#define LOG_DIR             "logs"
#define CONTROL_MESSAGE_LEN 512
#define CHILD_COMMAND_LEN   512
#define LOG_CHUNK_SIZE      4096
#define LOG_BUFFER_CAPACITY 32
#define DEFAULT_SOFT_LIMIT  (40UL << 20)   /* 40 MiB */
#define DEFAULT_HARD_LIMIT  (64UL << 20)   /* 64 MiB */

/* Response status codes */
#define RESP_OK    0
#define RESP_ERR  -1
#define RESP_MORE  1   /* streaming: more chunks follow */
#define RESP_DONE  2   /* streaming: end of stream      */

/* ---------------------------------------------------------------
 * Types
 * --------------------------------------------------------------- */

typedef enum {
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum {
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,   /* manual stop via CLI (stop_requested) */
    CONTAINER_KILLED,    /* hard-limit kill from kernel monitor  */
    CONTAINER_EXITED     /* normal/self exit                     */
} container_state_t;

typedef struct container_record {
    char              id[CONTAINER_ID_LEN];
    pid_t             host_pid;
    time_t            started_at;
    container_state_t state;
    unsigned long     soft_limit_bytes;
    unsigned long     hard_limit_bytes;
    int               exit_code;
    int               exit_signal;
    int               stop_requested;  /* set BEFORE sending SIGTERM from stop */
    char              log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char   container_id[CONTAINER_ID_LEN];
    size_t length;
    char   data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t      items[LOG_BUFFER_CAPACITY];
    size_t          head;
    size_t          tail;
    size_t          count;
    int             shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t  not_empty;
    pthread_cond_t  not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t  kind;
    char            container_id[CONTAINER_ID_LEN];
    char            rootfs[PATH_MAX];
    char            command[CHILD_COMMAND_LEN];
    unsigned long   soft_limit_bytes;
    unsigned long   hard_limit_bytes;
    int             nice_value;
} control_request_t;

typedef struct {
    int  status;                        /* RESP_OK / RESP_ERR / RESP_MORE / RESP_DONE */
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int  nice_value;
    int  log_write_fd;
} child_config_t;

typedef struct {
    int                server_fd;
    int                monitor_fd;
    volatile int       should_stop;
    pthread_t          logger_thread;
    bounded_buffer_t   log_buffer;
    pthread_mutex_t    metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global pointer for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

/* ---------------------------------------------------------------
 * Usage
 * --------------------------------------------------------------- */
static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command>"
            " [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run   <id> <container-rootfs> <command>"
            " [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

/* ---------------------------------------------------------------
 * Argument parsing
 * --------------------------------------------------------------- */
static int parse_mib_flag(const char *flag, const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;
    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0') {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }
    if (mib > ULONG_MAX / (1UL << 20)) {
        fprintf(stderr, "Value for %s is too large: %s\n", flag, value);
        return -1;
    }
    *target_bytes = mib * (1UL << 20);
    return 0;
}

static int parse_optional_flags(control_request_t *req, int argc,
                                char *argv[], int start_index)
{
    for (int i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long  nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }
        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i+1],
                               &req->soft_limit_bytes) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i+1],
                               &req->hard_limit_bytes) != 0) return -1;
            continue;
        }
        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i+1], &end, 10);
            if (errno != 0 || end == argv[i+1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i+1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }
        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }
    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "soft limit cannot exceed hard limit\n");
        return -1;
    }
    return 0;
}

static const char *state_to_string(container_state_t s)
{
    switch (s) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "hard_limit_killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ---------------------------------------------------------------
 * Bounded Buffer (Task 3)
 * --------------------------------------------------------------- */
static int bounded_buffer_init(bounded_buffer_t *b)
{
    int rc;
    memset(b, 0, sizeof(*b));
    if ((rc = pthread_mutex_init(&b->mutex, NULL)) != 0) return rc;
    if ((rc = pthread_cond_init(&b->not_empty, NULL)) != 0) {
        pthread_mutex_destroy(&b->mutex); return rc;
    }
    if ((rc = pthread_cond_init(&b->not_full, NULL)) != 0) {
        pthread_cond_destroy(&b->not_empty);
        pthread_mutex_destroy(&b->mutex); return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *b)
{
    pthread_cond_destroy(&b->not_full);
    pthread_cond_destroy(&b->not_empty);
    pthread_mutex_destroy(&b->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *b)
{
    pthread_mutex_lock(&b->mutex);
    b->shutting_down = 1;
    pthread_cond_broadcast(&b->not_empty);
    pthread_cond_broadcast(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
}

/*
 * Push (producer): blocks when buffer is full.
 * Returns 0 on success, -1 if shutdown was initiated while waiting.
 */
int bounded_buffer_push(bounded_buffer_t *b, const log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == LOG_BUFFER_CAPACITY && !b->shutting_down)
        pthread_cond_wait(&b->not_full, &b->mutex);
    if (b->shutting_down) {
        pthread_mutex_unlock(&b->mutex);
        return -1;
    }
    b->items[b->tail] = *item;
    b->tail = (b->tail + 1) % LOG_BUFFER_CAPACITY;
    b->count++;
    pthread_cond_signal(&b->not_empty);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/*
 * Pop (consumer): blocks when buffer is empty.
 * Returns 0 on success, 1 when shutdown+drained (thread should exit).
 */
int bounded_buffer_pop(bounded_buffer_t *b, log_item_t *item)
{
    pthread_mutex_lock(&b->mutex);
    while (b->count == 0 && !b->shutting_down)
        pthread_cond_wait(&b->not_empty, &b->mutex);
    if (b->count == 0) {          /* shutdown + drained */
        pthread_mutex_unlock(&b->mutex);
        return 1;
    }
    *item = b->items[b->head];
    b->head = (b->head + 1) % LOG_BUFFER_CAPACITY;
    b->count--;
    pthread_cond_signal(&b->not_full);
    pthread_mutex_unlock(&b->mutex);
    return 0;
}

/* ---------------------------------------------------------------
 * Logging consumer thread (Task 3)
 *
 * Drains the bounded buffer. On shutdown, drains remaining items
 * before exiting — no log lines are dropped.
 * --------------------------------------------------------------- */
void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;
    char path[PATH_MAX];
    int  rc;

    while (1) {
        rc = bounded_buffer_pop(&ctx->log_buffer, &item);
        if (rc != 0) break;   /* shutdown + drained */

        snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, item.container_id);
        int fd = open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
        if (fd < 0) {
            fprintf(stderr, "[logger] Cannot open %s: %s\n",
                    path, strerror(errno));
            continue;
        }
        ssize_t written = 0;
        while ((size_t)written < item.length) {
            ssize_t n = write(fd, item.data + written,
                              item.length - (size_t)written);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            written += n;
        }
        close(fd);
    }

    fprintf(stderr, "[logger] Thread exiting cleanly.\n");
    return NULL;
}

/* ---------------------------------------------------------------
 * Log reader thread (producer, one per container, Task 3)
 *
 * Reads the container stdout/stderr pipe and pushes chunks into
 * the shared bounded buffer. Exits when pipe EOF or shutdown.
 * --------------------------------------------------------------- */
typedef struct {
    int              read_fd;
    char             container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *log_buffer;
} log_reader_args_t;

static void *log_reader_thread(void *arg)
{
    log_reader_args_t *lra = (log_reader_args_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(&item, 0, sizeof(item));
    snprintf(item.container_id, CONTAINER_ID_LEN, "%s", lra->container_id);

    while (1) {
        n = read(lra->read_fd, item.data, LOG_CHUNK_SIZE);
        if (n <= 0) break;
        item.length = (size_t)n;
        if (bounded_buffer_push(lra->log_buffer, &item) != 0)
            break;
    }

    close(lra->read_fd);
    free(lra);
    return NULL;
}

/* ---------------------------------------------------------------
 * Container child entrypoint (runs inside clone'd namespaces)
 * Task 1: PID + UTS + mount isolation, chroot, /proc, nice
 * --------------------------------------------------------------- */
int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* UTS namespace: set hostname to container ID */
    if (sethostname(cfg->id, strlen(cfg->id)) != 0)
        perror("sethostname");

    /* Redirect stdout + stderr to log pipe */
    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0 ||
        dup2(cfg->log_write_fd, STDERR_FILENO) < 0) {
        perror("dup2");
        return 1;
    }
    close(cfg->log_write_fd);

    /* Mount namespace + chroot: filesystem isolation */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir /");
        return 1;
    }

    /* Mount /proc so tools like ps work inside the container */
    if (mount("proc", "/proc", "proc",
              MS_NOEXEC | MS_NOSUID | MS_NODEV, NULL) != 0) {
        fprintf(stderr, "[container] Warning: mount /proc: %s\n",
                strerror(errno));
    }

    /* Scheduling priority */
    if (cfg->nice_value != 0) {
        errno = 0;
        (void)nice(cfg->nice_value);
        if (errno != 0) perror("nice");
    }

    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    return 1;
}

/* ---------------------------------------------------------------
 * Monitor ioctl wrappers (Task 4)
 * --------------------------------------------------------------- */
int register_with_monitor(int monitor_fd, const char *container_id,
                          pid_t host_pid, unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid              = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0 ? -1 : 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id,
                            pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    return ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0 ? -1 : 0;
}

/* ---------------------------------------------------------------
 * Metadata helpers
 * --------------------------------------------------------------- */
static container_record_t *find_container(supervisor_ctx_t *ctx,
                                          const char *id)
{
    container_record_t *c;
    for (c = ctx->containers; c; c = c->next)
        if (strncmp(c->id, id, CONTAINER_ID_LEN) == 0) return c;
    return NULL;
}

static container_record_t *add_container(supervisor_ctx_t *ctx,
                                         const control_request_t *req)
{
    container_record_t *c = calloc(1, sizeof(*c));
    if (!c) return NULL;
    strncpy(c->id, req->container_id, CONTAINER_ID_LEN - 1);
    c->state            = CONTAINER_STARTING;
    c->started_at       = time(NULL);
    c->soft_limit_bytes = req->soft_limit_bytes;
    c->hard_limit_bytes = req->hard_limit_bytes;
    c->stop_requested   = 0;
    snprintf(c->log_path, sizeof(c->log_path),
             "%s/%s.log", LOG_DIR, c->id);
    c->next = ctx->containers;
    ctx->containers = c;
    return c;
}

/* ---------------------------------------------------------------
 * Spawn a container (Task 1)
 * --------------------------------------------------------------- */
static int spawn_container(supervisor_ctx_t *ctx,
                           container_record_t *record,
                           const control_request_t *req)
{
    int log_pipe[2];
    if (pipe(log_pipe) != 0) { perror("pipe"); return -1; }

    child_config_t *cfg = calloc(1, sizeof(*cfg));
    if (!cfg) { close(log_pipe[0]); close(log_pipe[1]); return -1; }
    strncpy(cfg->id,      req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs,  req->rootfs,       PATH_MAX - 1);
    strncpy(cfg->command, req->command,      CHILD_COMMAND_LEN - 1);
    cfg->nice_value   = req->nice_value;
    cfg->log_write_fd = log_pipe[1];

    char *stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg); close(log_pipe[0]); close(log_pipe[1]); return -1;
    }

    int clone_flags = CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD;
    pid_t pid = clone(child_fn, stack + STACK_SIZE, clone_flags, cfg);
    free(stack);

    if (pid < 0) {
        perror("clone");
        free(cfg); close(log_pipe[0]); close(log_pipe[1]); return -1;
    }

    /* Supervisor: close write end of pipe */
    close(log_pipe[1]);

    /* Start per-container log-reader (producer) thread */
    log_reader_args_t *lra = calloc(1, sizeof(*lra));
    if (lra) {
        lra->read_fd    = log_pipe[0];
        lra->log_buffer = &ctx->log_buffer;
        snprintf(lra->container_id, CONTAINER_ID_LEN, "%s",
                 req->container_id);
        pthread_t tid;
        if (pthread_create(&tid, NULL, log_reader_thread, lra) == 0)
            pthread_detach(tid);
        else { free(lra); close(log_pipe[0]); }
    } else {
        close(log_pipe[0]);
    }

    record->host_pid = pid;
    record->state    = CONTAINER_RUNNING;

    /* Register with kernel memory monitor */
    if (ctx->monitor_fd >= 0) {
        if (register_with_monitor(ctx->monitor_fd, record->id, pid,
                                  record->soft_limit_bytes,
                                  record->hard_limit_bytes) != 0) {
            fprintf(stderr,
                    "[supervisor] Warning: monitor register failed: %s\n",
                    strerror(errno));
        }
    }

    return 0;
}

/* ---------------------------------------------------------------
 * Signal handlers (Task 2)
 * --------------------------------------------------------------- */

/*
 * SIGCHLD: reap children, update metadata, unregister from monitor.
 *
 * Termination classification (spec Task 4):
 *   stop_requested=1              → CONTAINER_STOPPED
 *   SIGKILL, stop_requested=0    → CONTAINER_KILLED (hard_limit_killed)
 *   normal exit                  → CONTAINER_EXITED
 */
static void sigchld_handler(int sig)
{
    (void)sig;
    int   status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;
        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *c;
        for (c = g_ctx->containers; c; c = c->next) {
            if (c->host_pid != pid) continue;

            if (WIFEXITED(status)) {
                c->exit_code = WEXITSTATUS(status);
                c->state = c->stop_requested
                               ? CONTAINER_STOPPED : CONTAINER_EXITED;
            } else if (WIFSIGNALED(status)) {
                c->exit_signal = WTERMSIG(status);
                if (c->stop_requested)
                    c->state = CONTAINER_STOPPED;
                else if (WTERMSIG(status) == SIGKILL)
                    c->state = CONTAINER_KILLED;
                else
                    c->state = CONTAINER_EXITED;
            }
            if (g_ctx->monitor_fd >= 0)
                unregister_from_monitor(g_ctx->monitor_fd, c->id, pid);
            break;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx) g_ctx->should_stop = 1;
}

/* ---------------------------------------------------------------
 * Streaming response helpers (for ps / logs)
 * --------------------------------------------------------------- */
static void send_stream_chunk(int fd, const char *data, size_t len)
{
    control_response_t r;
    memset(&r, 0, sizeof(r));
    r.status = RESP_MORE;
    size_t chunk = len < sizeof(r.message) - 1
                       ? len : sizeof(r.message) - 1;
    memcpy(r.message, data, chunk);
    send(fd, &r, sizeof(r), 0);
}

static void send_stream_done(int fd)
{
    control_response_t r;
    memset(&r, 0, sizeof(r));
    r.status = RESP_DONE;
    send(fd, &r, sizeof(r), 0);
}

static void send_response(int fd, int status, const char *msg)
{
    control_response_t r;
    memset(&r, 0, sizeof(r));
    r.status = status;
    strncpy(r.message, msg, sizeof(r.message) - 1);
    send(fd, &r, sizeof(r), 0);
}

/* ---------------------------------------------------------------
 * Handle one connected CLI client (Task 2)
 * --------------------------------------------------------------- */
static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    ssize_t n = recv(client_fd, &req, sizeof(req), MSG_WAITALL);
    if (n != (ssize_t)sizeof(req)) {
        send_response(client_fd, RESP_ERR, "Bad request size");
        return;
    }

    switch (req.kind) {

    /* ---- START: launch container in background ---- */
    case CMD_START: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *ex = find_container(ctx, req.container_id);
        if (ex && ex->state == CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(client_fd, RESP_ERR, "Container already running");
            break;
        }
        container_record_t *rec = add_container(ctx, &req);
        if (!rec) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(client_fd, RESP_ERR, "Out of memory");
            break;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (spawn_container(ctx, rec, &req) != 0) {
            pthread_mutex_lock(&ctx->metadata_lock);
            rec->state = CONTAINER_EXITED;
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(client_fd, RESP_ERR, "spawn failed");
            break;
        }
        char msg[CONTROL_MESSAGE_LEN];
        snprintf(msg, sizeof(msg), "Container %s started (pid %d)\n",
                 req.container_id, rec->host_pid);
        send_response(client_fd, RESP_OK, msg);
        break;
    }

    /* ---- RUN: launch and wait, return exit status ---- */
    case CMD_RUN: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *ex = find_container(ctx, req.container_id);
        if (ex && ex->state == CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(client_fd, RESP_ERR, "Container already running");
            break;
        }
        container_record_t *rec = add_container(ctx, &req);
        if (!rec) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(client_fd, RESP_ERR, "Out of memory");
            break;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (spawn_container(ctx, rec, &req) != 0) {
            pthread_mutex_lock(&ctx->metadata_lock);
            rec->state = CONTAINER_EXITED;
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(client_fd, RESP_ERR, "spawn failed");
            break;
        }

        /* Notify client that we are running (so it can install signal handler) */
        char msg[CONTROL_MESSAGE_LEN];
        snprintf(msg, sizeof(msg), "Container %s running (pid %d)\n",
                 req.container_id, rec->host_pid);
        send_response(client_fd, RESP_MORE, msg);

        /* Block waiting for the container to finish */
        int wstatus;
        pid_t waited = waitpid(rec->host_pid, &wstatus, 0);

        pthread_mutex_lock(&ctx->metadata_lock);
        if (waited > 0) {
            if (WIFEXITED(wstatus)) {
                rec->exit_code = WEXITSTATUS(wstatus);
                rec->state = rec->stop_requested
                                 ? CONTAINER_STOPPED : CONTAINER_EXITED;
            } else if (WIFSIGNALED(wstatus)) {
                rec->exit_signal = WTERMSIG(wstatus);
                if (rec->stop_requested)
                    rec->state = CONTAINER_STOPPED;
                else if (WTERMSIG(wstatus) == SIGKILL)
                    rec->state = CONTAINER_KILLED;
                else
                    rec->state = CONTAINER_EXITED;
            }
            if (ctx->monitor_fd >= 0)
                unregister_from_monitor(ctx->monitor_fd,
                                        rec->id, rec->host_pid);
        }

        /* Per spec: exit_code or 128+signal */
        int client_exit;
        if (WIFEXITED(wstatus))
            client_exit = WEXITSTATUS(wstatus);
        else if (WIFSIGNALED(wstatus))
            client_exit = 128 + WTERMSIG(wstatus);
        else
            client_exit = 1;

        container_state_t final = rec->state;
        pthread_mutex_unlock(&ctx->metadata_lock);

        snprintf(msg, sizeof(msg),
                 "Container %s finished: state=%s exit_status=%d\n",
                 req.container_id, state_to_string(final), client_exit);
        send_response(client_fd, RESP_OK, msg);
        break;
    }

    /* ---- PS: list all containers (streaming) ---- */
    case CMD_PS: {
        char line[CONTROL_MESSAGE_LEN];
        snprintf(line, sizeof(line),
                 "%-20s %-8s %-20s %-20s %-10s\n",
                 "ID", "PID", "STARTED", "STATE", "EXIT");
        send_stream_chunk(client_fd, line, strlen(line));

        pthread_mutex_lock(&ctx->metadata_lock);
        for (container_record_t *c = ctx->containers; c; c = c->next) {
            char tsbuf[32], exitbuf[16];
            struct tm *tm_info = localtime(&c->started_at);
            strftime(tsbuf, sizeof(tsbuf), "%Y-%m-%d %H:%M:%S", tm_info);

            if (c->state == CONTAINER_EXITED ||
                c->state == CONTAINER_STOPPED)
                snprintf(exitbuf, sizeof(exitbuf), "exit(%d)", c->exit_code);
            else if (c->state == CONTAINER_KILLED)
                snprintf(exitbuf, sizeof(exitbuf), "sig(%d)", c->exit_signal);
            else
                snprintf(exitbuf, sizeof(exitbuf), "-");

            snprintf(line, sizeof(line),
                     "%-20s %-8d %-20s %-20s %-10s\n",
                     c->id, c->host_pid, tsbuf,
                     state_to_string(c->state), exitbuf);
            send_stream_chunk(client_fd, line, strlen(line));
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        send_stream_done(client_fd);
        break;
    }

    /* ---- LOGS: stream log file contents ---- */
    case CMD_LOGS: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (!c) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(client_fd, RESP_ERR, "No such container");
            break;
        }
        char log_path[PATH_MAX];
        strncpy(log_path, c->log_path, PATH_MAX - 1);
        pthread_mutex_unlock(&ctx->metadata_lock);

        int log_fd = open(log_path, O_RDONLY);
        if (log_fd < 0) {
            send_response(client_fd, RESP_ERR, "Log file not found");
            break;
        }
        char buf[LOG_CHUNK_SIZE];
        ssize_t nr;
        while ((nr = read(log_fd, buf, sizeof(buf))) > 0)
            send_stream_chunk(client_fd, buf, (size_t)nr);
        close(log_fd);
        send_stream_done(client_fd);
        break;
    }

    /* ---- STOP: terminate a running container ---- */
    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *c = find_container(ctx, req.container_id);
        if (!c || c->state != CONTAINER_RUNNING) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            send_response(client_fd, RESP_ERR, "Container not running");
            break;
        }
        /* Per spec: set stop_requested BEFORE sending signal */
        c->stop_requested = 1;
        pid_t target = c->host_pid;
        pthread_mutex_unlock(&ctx->metadata_lock);

        kill(target, SIGTERM);

        char msg[CONTROL_MESSAGE_LEN];
        snprintf(msg, sizeof(msg), "Sent SIGTERM to %s (pid %d)\n",
                 req.container_id, target);
        send_response(client_fd, RESP_OK, msg);
        break;
    }

    default:
        send_response(client_fd, RESP_ERR, "Unknown command");
        break;
    }
}

/* ---------------------------------------------------------------
 * Supervisor daemon (Task 1 + 2)
 * --------------------------------------------------------------- */
static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd  = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc) { errno = rc; perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc) {
        errno = rc; perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock); return 1;
    }

    mkdir(LOG_DIR, 0755);

    /* 1) Open kernel monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] Warning: /dev/container_monitor: %s\n",
                strerror(errno));

    /* 2) UNIX domain socket control channel */
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); goto cleanup; }

    unlink(CONTROL_PATH);
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);
    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); goto cleanup;
    }
    if (listen(ctx.server_fd, 16) < 0) {
        perror("listen"); goto cleanup;
    }

    /* 3) Signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags   = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags   = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* 4) Start logging consumer thread */
    rc = pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);
    if (rc) { errno = rc; perror("pthread_create"); goto cleanup; }

    fprintf(stderr, "[supervisor] Ready. rootfs=%s  socket=%s\n",
            rootfs, CONTROL_PATH);

    /* 5) Event loop */
    while (!ctx.should_stop) {
        fd_set rfds;
        struct timeval tv = {1, 0};
        FD_ZERO(&rfds);
        FD_SET(ctx.server_fd, &rfds);

        int sel = select(ctx.server_fd + 1, &rfds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select"); break;
        }
        if (sel == 0) continue;

        if (FD_ISSET(ctx.server_fd, &rfds)) {
            int cfd = accept(ctx.server_fd, NULL, NULL);
            if (cfd < 0) { if (errno != EINTR) perror("accept"); continue; }
            handle_client(&ctx, cfd);
            close(cfd);
        }
    }

    fprintf(stderr, "[supervisor] Shutting down...\n");

    /* Terminate all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    for (container_record_t *c = ctx.containers; c; c = c->next) {
        if (c->state == CONTAINER_RUNNING) {
            c->stop_requested = 1;
            kill(c->host_pid, SIGTERM);
        }
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    sleep(1);   /* allow graceful exit */

    /* SIGKILL any remaining stragglers */
    pthread_mutex_lock(&ctx.metadata_lock);
    for (container_record_t *c = ctx.containers; c; c = c->next)
        if (c->state == CONTAINER_RUNNING)
            kill(c->host_pid, SIGKILL);
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Reap all children */
    while (waitpid(-1, NULL, WNOHANG) > 0)
        ;

cleanup:
    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    if (ctx.logger_thread)
        pthread_join(ctx.logger_thread, NULL);

    bounded_buffer_destroy(&ctx.log_buffer);

    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *c = ctx.containers;
    while (c) {
        container_record_t *nx = c->next;
        free(c);
        c = nx;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.server_fd >= 0) { close(ctx.server_fd); unlink(CONTROL_PATH); }
    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);

    fprintf(stderr, "[supervisor] Exited cleanly.\n");
    g_ctx = NULL;
    return 0;
}

/* ---------------------------------------------------------------
 * Client-side: connect to supervisor, send request, print response.
 *
 * For CMD_RUN: install SIGINT/SIGTERM handler that forwards a stop
 * request to the supervisor, then continues waiting for final status.
 * --------------------------------------------------------------- */
static int g_run_client_fd = -1;
static char g_run_id[CONTAINER_ID_LEN];

static void run_client_sighandler(int sig)
{
    (void)sig;
    /* Open a new connection to forward stop */
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return;

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
        control_request_t req;
        memset(&req, 0, sizeof(req));
        req.kind = CMD_STOP;
        strncpy(req.container_id, g_run_id, CONTAINER_ID_LEN - 1);
        send(fd, &req, sizeof(req), 0);
        control_response_t r;
        recv(fd, &r, sizeof(r), MSG_WAITALL);
    }
    close(fd);
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("connect (is the supervisor running?)");
        close(fd); return 1;
    }
    if (send(fd, req, sizeof(*req), 0) != (ssize_t)sizeof(*req)) {
        perror("send"); close(fd); return 1;
    }

    /* CMD_RUN: install signal forwarding handler */
    if (req->kind == CMD_RUN) {
        g_run_client_fd = fd;
        snprintf(g_run_id, CONTAINER_ID_LEN, "%s", req->container_id);
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = run_client_sighandler;
        sa.sa_flags   = SA_RESTART;
        sigaction(SIGINT,  &sa, NULL);
        sigaction(SIGTERM, &sa, NULL);
    }

    int ret = 0;
    while (1) {
        ssize_t n = recv(fd, &resp, sizeof(resp), MSG_WAITALL);
        if (n != (ssize_t)sizeof(resp)) { ret = 1; break; }

        if (resp.status == RESP_DONE) break;

        if (resp.status == RESP_ERR) {
            fprintf(stderr, "Error: %s", resp.message);
            ret = 1; break;
        }

        /* RESP_OK or RESP_MORE: print message */
        if (resp.message[0])
            fputs(resp.message, stdout);

        if (resp.status == RESP_OK) break;
        /* RESP_MORE: keep reading */
    }

    fflush(stdout);
    close(fd);
    g_run_client_fd = -1;
    return ret;
}

/* ---------------------------------------------------------------
 * CLI command handlers
 * --------------------------------------------------------------- */
static int cmd_start(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <rootfs> <command> [opts]\n",
                argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <rootfs> <command> [opts]\n", argv[0]);
        return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    strncpy(req.rootfs,       argv[3], PATH_MAX - 1);
    strncpy(req.command,      argv[4], CHILD_COMMAND_LEN - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_ps(void)
{
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_PS;
    return send_control_request(&req);
}

static int cmd_logs(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    if (argc < 3) {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1;
    }
    control_request_t req;
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], CONTAINER_ID_LEN - 1);
    return send_control_request(&req);
}

/* ---------------------------------------------------------------
 * main
 * --------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    if (argc < 2) { usage(argv[0]); return 1; }

    if (strcmp(argv[1], "supervisor") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }
    if (strcmp(argv[1], "start") == 0) return cmd_start(argc, argv);
    if (strcmp(argv[1], "run")   == 0) return cmd_run(argc, argv);
    if (strcmp(argv[1], "ps")    == 0) return cmd_ps();
    if (strcmp(argv[1], "logs")  == 0) return cmd_logs(argc, argv);
    if (strcmp(argv[1], "stop")  == 0) return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
