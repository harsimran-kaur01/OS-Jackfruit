/*
 * engine.c - Supervised Multi-Container Runtime (User Space)
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

#define STACK_SIZE (1024 * 1024)
#define CONTAINER_ID_LEN 32
#define CONTROL_PATH "/tmp/mini_runtime.sock"
#define LOG_DIR "logs"
#define CONTROL_MESSAGE_LEN 256
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)

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
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record {
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int stop_requested;
    char log_path[PATH_MAX];
    struct container_record *next;
} container_record_t;

typedef struct {
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct {
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct {
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct {
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct {
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct {
    int server_fd;
    int monitor_fd;
    int should_stop;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

/* Global supervisor context pointer for signal handlers */
static supervisor_ctx_t *g_ctx = NULL;

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage:\n"
            "  %s supervisor <base-rootfs>\n"
            "  %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n"
            "  %s ps\n"
            "  %s logs <id>\n"
            "  %s stop <id>\n",
            prog, prog, prog, prog, prog, prog);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
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

static int parse_optional_flags(control_request_t *req,
                                int argc,
                                char *argv[],
                                int start_index)
{
    int i;

    for (i = start_index; i < argc; i += 2) {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc) {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0) {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0) {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0) {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19) {
                fprintf(stderr,
                        "Invalid value for --nice (expected -20..19): %s\n",
                        argv[i + 1]);
                return -1;
            }
            req->nice_value = (int)nice_value;
            continue;
        }

        fprintf(stderr, "Unknown option: %s\n", argv[i]);
        return -1;
    }

    if (req->soft_limit_bytes > req->hard_limit_bytes) {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state) {
    case CONTAINER_STARTING: return "starting";
    case CONTAINER_RUNNING:  return "running";
    case CONTAINER_STOPPED:  return "stopped";
    case CONTAINER_KILLED:   return "killed";
    case CONTAINER_EXITED:   return "exited";
    default:                 return "unknown";
    }
}

/* ---------------------------------------------------------------
 * Bounded Buffer
 * --------------------------------------------------------------- */

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;
    memset(buffer, 0, sizeof(*buffer));
    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0) return rc;
    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0) { pthread_mutex_destroy(&buffer->mutex); return rc; }
    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0) {
        pthread_cond_destroy(&buffer->not_empty);
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }
    return 0;
}

static void bounded_buffer_destroy(bounded_buffer_t *buffer)
{
    pthread_cond_destroy(&buffer->not_full);
    pthread_cond_destroy(&buffer->not_empty);
    pthread_mutex_destroy(&buffer->mutex);
}

static void bounded_buffer_begin_shutdown(bounded_buffer_t *buffer)
{
    pthread_mutex_lock(&buffer->mutex);
    buffer->shutting_down = 1;
    pthread_cond_broadcast(&buffer->not_empty);
    pthread_cond_broadcast(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
}

int bounded_buffer_push(bounded_buffer_t *buffer, const log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == LOG_BUFFER_CAPACITY && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_full, &buffer->mutex);

    if (buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    buffer->items[buffer->tail] = *item;
    buffer->tail = (buffer->tail + 1) % LOG_BUFFER_CAPACITY;
    buffer->count++;

    pthread_cond_signal(&buffer->not_empty);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

int bounded_buffer_pop(bounded_buffer_t *buffer, log_item_t *item)
{
    pthread_mutex_lock(&buffer->mutex);

    while (buffer->count == 0 && !buffer->shutting_down)
        pthread_cond_wait(&buffer->not_empty, &buffer->mutex);

    if (buffer->count == 0 && buffer->shutting_down) {
        pthread_mutex_unlock(&buffer->mutex);
        return -1;
    }

    *item = buffer->items[buffer->head];
    buffer->head = (buffer->head + 1) % LOG_BUFFER_CAPACITY;
    buffer->count--;

    pthread_cond_signal(&buffer->not_full);
    pthread_mutex_unlock(&buffer->mutex);
    return 0;
}

/* ---------------------------------------------------------------
 * Logging consumer thread
 * --------------------------------------------------------------- */

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0) {
        /* Find the container's log file and write to it */
        container_record_t *rec;
        pthread_mutex_lock(&ctx->metadata_lock);
        rec = ctx->containers;
        while (rec) {
            if (strcmp(rec->id, item.container_id) == 0)
                break;
            rec = rec->next;
        }
        if (rec) {
            int fd = open(rec->log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) {
                write(fd, item.data, item.length);
                close(fd);
            }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
    }

    /* Drain any remaining items after shutdown signal */
    pthread_mutex_lock(&ctx->log_buffer.mutex);
    while (ctx->log_buffer.count > 0) {
        item = ctx->log_buffer.items[ctx->log_buffer.head];
        ctx->log_buffer.head = (ctx->log_buffer.head + 1) % LOG_BUFFER_CAPACITY;
        ctx->log_buffer.count--;
        pthread_mutex_unlock(&ctx->log_buffer.mutex);

        container_record_t *rec;
        pthread_mutex_lock(&ctx->metadata_lock);
        rec = ctx->containers;
        while (rec) {
            if (strcmp(rec->id, item.container_id) == 0) break;
            rec = rec->next;
        }
        if (rec) {
            int fd = open(rec->log_path, O_WRONLY | O_CREAT | O_APPEND, 0644);
            if (fd >= 0) { write(fd, item.data, item.length); close(fd); }
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        pthread_mutex_lock(&ctx->log_buffer.mutex);
    }
    pthread_mutex_unlock(&ctx->log_buffer.mutex);

    return NULL;
}

/* ---------------------------------------------------------------
 * Producer thread: reads a container's pipe and pushes to buffer
 * --------------------------------------------------------------- */

typedef struct {
    int read_fd;
    char container_id[CONTAINER_ID_LEN];
    bounded_buffer_t *buffer;
} producer_args_t;

void *producer_thread(void *arg)
{
    producer_args_t *pargs = (producer_args_t *)arg;
    log_item_t item;
    ssize_t n;

    memset(item.container_id, 0, sizeof(item.container_id));
    strncpy(item.container_id, pargs->container_id, CONTAINER_ID_LEN - 1);

    while ((n = read(pargs->read_fd, item.data, LOG_CHUNK_SIZE)) > 0) {
        item.length = (size_t)n;
        bounded_buffer_push(pargs->buffer, &item);
    }

    close(pargs->read_fd);
    free(pargs);
    return NULL;
}

/* ---------------------------------------------------------------
 * Child container entrypoint (runs after clone())
 * --------------------------------------------------------------- */

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    /* Redirect stdout and stderr to the logging pipe */
    dup2(cfg->log_write_fd, STDOUT_FILENO);
    dup2(cfg->log_write_fd, STDERR_FILENO);
    close(cfg->log_write_fd);

    /* Set UTS hostname to container id */
    sethostname(cfg->id, strlen(cfg->id));

    /* chroot into the container's rootfs */
    if (chroot(cfg->rootfs) != 0) {
        perror("chroot");
        return 1;
    }
    if (chdir("/") != 0) {
        perror("chdir");
        return 1;
    }

    /* Mount /proc inside the container */
    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) != 0) {
        perror("mount /proc");
        /* Non-fatal: continue anyway */
    }

    /* Apply nice value */
    if (cfg->nice_value != 0)
        nice(cfg->nice_value);

    /* Execute the command */
    char *argv[] = { cfg->command, NULL };
    char *envp[] = {
        "PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin",
        "HOME=/root",
        NULL
    };
    execve(cfg->command, argv, envp);
    perror("execve");
    return 1;
}

/* ---------------------------------------------------------------
 * Monitor ioctl helpers
 * --------------------------------------------------------------- */

int register_with_monitor(int monitor_fd,
                          const char *container_id,
                          pid_t host_pid,
                          unsigned long soft_limit_bytes,
                          unsigned long hard_limit_bytes)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    req.soft_limit_bytes = soft_limit_bytes;
    req.hard_limit_bytes = hard_limit_bytes;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_REGISTER, &req) < 0)
        return -1;
    return 0;
}

int unregister_from_monitor(int monitor_fd, const char *container_id, pid_t host_pid)
{
    struct monitor_request req;
    memset(&req, 0, sizeof(req));
    req.pid = host_pid;
    strncpy(req.container_id, container_id, sizeof(req.container_id) - 1);
    if (ioctl(monitor_fd, MONITOR_UNREGISTER, &req) < 0)
        return -1;
    return 0;
}

/* ---------------------------------------------------------------
 * Launch a container: clone, pipe, producer thread, metadata
 * --------------------------------------------------------------- */

static int launch_container(supervisor_ctx_t *ctx,
                             const control_request_t *req,
                             control_response_t *resp)
{
    int pipefd[2];
    pid_t pid;
    char *stack;
    child_config_t *cfg;
    container_record_t *rec;
    producer_args_t *pargs;
    pthread_t prod_thread;

    /* Check for duplicate ID */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec = ctx->containers;
    while (rec) {
        if (strcmp(rec->id, req->container_id) == 0 &&
            (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING)) {
            pthread_mutex_unlock(&ctx->metadata_lock);
            resp->status = -1;
            snprintf(resp->message, CONTROL_MESSAGE_LEN,
                     "Container '%s' already running", req->container_id);
            return -1;
        }
        rec = rec->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Create pipe for container stdout/stderr → supervisor */
    if (pipe(pipefd) != 0) {
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "pipe: %s", strerror(errno));
        return -1;
    }

    /* Build child config */
    cfg = calloc(1, sizeof(*cfg));
    if (!cfg) {
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "calloc failed");
        return -1;
    }
    strncpy(cfg->id, req->container_id, CONTAINER_ID_LEN - 1);
    strncpy(cfg->rootfs, req->rootfs, PATH_MAX - 1);
    strncpy(cfg->command, req->command, CHILD_COMMAND_LEN - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    /* Allocate stack for clone */
    stack = malloc(STACK_SIZE);
    if (!stack) {
        free(cfg);
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "malloc stack failed");
        return -1;
    }

    /* Ensure log directory exists */
    mkdir(LOG_DIR, 0755);

    /* Create metadata record */
    rec = calloc(1, sizeof(*rec));
    if (!rec) {
        free(stack); free(cfg);
        close(pipefd[0]); close(pipefd[1]);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "calloc record failed");
        return -1;
    }
    strncpy(rec->id, req->container_id, CONTAINER_ID_LEN - 1);
    rec->state = CONTAINER_STARTING;
    rec->started_at = time(NULL);
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    snprintf(rec->log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req->container_id);

    /* clone() with PID, UTS, and mount namespaces */
    pid = clone(child_fn, stack + STACK_SIZE,
                CLONE_NEWPID | CLONE_NEWUTS | CLONE_NEWNS | SIGCHLD,
                cfg);

    /* Close write end in supervisor — child owns it */
    close(pipefd[1]);

    if (pid < 0) {
        free(stack); free(cfg); free(rec);
        close(pipefd[0]);
        resp->status = -1;
        snprintf(resp->message, CONTROL_MESSAGE_LEN, "clone: %s", strerror(errno));
        return -1;
    }

    rec->host_pid = pid;
    rec->state = CONTAINER_RUNNING;

    /* Register with kernel monitor */
    if (ctx->monitor_fd >= 0)
        register_with_monitor(ctx->monitor_fd, req->container_id, pid,
                               req->soft_limit_bytes, req->hard_limit_bytes);

    /* Add to container list */
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);

    /* Start producer thread to read container output */
    pargs = calloc(1, sizeof(*pargs));
    if (pargs) {
        pargs->read_fd = pipefd[0];
        strncpy(pargs->container_id, req->container_id, CONTAINER_ID_LEN - 1);
        pargs->buffer = &ctx->log_buffer;
        pthread_create(&prod_thread, NULL, producer_thread, pargs);
        pthread_detach(prod_thread);
    } else {
        close(pipefd[0]);
    }

    free(stack);
    /* Note: cfg is freed by child after exec, but since we clone (not fork),
       the child has its own copy. Free in parent too. */
    free(cfg);

    resp->status = 0;
    snprintf(resp->message, CONTROL_MESSAGE_LEN,
             "Started container '%s' with PID %d", req->container_id, pid);
    return 0;
}

/* ---------------------------------------------------------------
 * SIGCHLD handler — reaps children and updates metadata
 * --------------------------------------------------------------- */

static void sigchld_handler(int sig)
{
    (void)sig;
    int saved_errno = errno;
    int status;
    pid_t pid;

    while ((pid = waitpid(-1, &status, WNOHANG)) > 0) {
        if (!g_ctx) continue;

        pthread_mutex_lock(&g_ctx->metadata_lock);
        container_record_t *rec = g_ctx->containers;
        while (rec) {
            if (rec->host_pid == pid) {
                if (WIFEXITED(status)) {
                    rec->exit_code = WEXITSTATUS(status);
                    rec->exit_signal = 0;
                    rec->state = rec->stop_requested ? CONTAINER_STOPPED : CONTAINER_EXITED;
                } else if (WIFSIGNALED(status)) {
                    rec->exit_signal = WTERMSIG(status);
                    rec->exit_code = 128 + rec->exit_signal;
                    if (rec->stop_requested)
                        rec->state = CONTAINER_STOPPED;
                    else if (rec->exit_signal == SIGKILL)
                        rec->state = CONTAINER_KILLED;
                    else
                        rec->state = CONTAINER_EXITED;
                }
                /* Unregister from kernel monitor */
                if (g_ctx->monitor_fd >= 0)
                    unregister_from_monitor(g_ctx->monitor_fd, rec->id, pid);
                break;
            }
            rec = rec->next;
        }
        pthread_mutex_unlock(&g_ctx->metadata_lock);
    }

    errno = saved_errno;
}

static void sigterm_handler(int sig)
{
    (void)sig;
    if (g_ctx)
        g_ctx->should_stop = 1;
}

/* ---------------------------------------------------------------
 * Supervisor: handle one client connection
 * --------------------------------------------------------------- */

static void handle_client(supervisor_ctx_t *ctx, int client_fd)
{
    control_request_t req;
    control_response_t resp;
    ssize_t n;

    memset(&resp, 0, sizeof(resp));

    n = recv(client_fd, &req, sizeof(req), 0);
    if (n != (ssize_t)sizeof(req)) {
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "Bad request size");
        send(client_fd, &resp, sizeof(resp), 0);
        return;
    }

    switch (req.kind) {

    case CMD_START:
    case CMD_RUN:
        launch_container(ctx, &req, &resp);
        send(client_fd, &resp, sizeof(resp), 0);

        if (req.kind == CMD_RUN && resp.status == 0) {
            /* Wait for the container to exit, then send exit info */
            pid_t target_pid = -1;
            pthread_mutex_lock(&ctx->metadata_lock);
            container_record_t *rec = ctx->containers;
            while (rec) {
                if (strcmp(rec->id, req.container_id) == 0) {
                    target_pid = rec->host_pid;
                    break;
                }
                rec = rec->next;
            }
            pthread_mutex_unlock(&ctx->metadata_lock);

            /* Poll until container exits */
            if (target_pid > 0) {
                while (1) {
                    pthread_mutex_lock(&ctx->metadata_lock);
                    rec = ctx->containers;
                    int done = 0;
                    while (rec) {
                        if (rec->host_pid == target_pid) {
                            if (rec->state != CONTAINER_RUNNING &&
                                rec->state != CONTAINER_STARTING) {
                                resp.status = rec->exit_code;
                                snprintf(resp.message, CONTROL_MESSAGE_LEN,
                                         "Container '%s' exited: state=%s exit_code=%d",
                                         rec->id, state_to_string(rec->state), rec->exit_code);
                                done = 1;
                            }
                            break;
                        }
                        rec = rec->next;
                    }
                    pthread_mutex_unlock(&ctx->metadata_lock);
                    if (done) break;
                    usleep(100000); /* 100ms poll */
                }
                send(client_fd, &resp, sizeof(resp), 0);
            }
        }
        break;

    case CMD_PS: {
        char buf[4096];
        int pos = 0;
        pos += snprintf(buf + pos, sizeof(buf) - pos,
                        "%-16s %-8s %-10s %-12s %-12s %s\n",
                        "ID", "PID", "STATE", "SOFT(MiB)", "HARD(MiB)", "STARTED");
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = ctx->containers;
        while (rec && pos < (int)sizeof(buf) - 128) {
            char tstr[32];
            struct tm *tm = localtime(&rec->started_at);
            strftime(tstr, sizeof(tstr), "%H:%M:%S", tm);
            pos += snprintf(buf + pos, sizeof(buf) - pos,
                            "%-16s %-8d %-10s %-12lu %-12lu %s\n",
                            rec->id, rec->host_pid,
                            state_to_string(rec->state),
                            rec->soft_limit_bytes >> 20,
                            rec->hard_limit_bytes >> 20,
                            tstr);
            rec = rec->next;
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        resp.status = 0;
        strncpy(resp.message, buf, CONTROL_MESSAGE_LEN - 1);
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    case CMD_LOGS: {
        char log_path[PATH_MAX];
        snprintf(log_path, PATH_MAX, "%s/%s.log", LOG_DIR, req.container_id);
        FILE *f = fopen(log_path, "r");
        if (!f) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "No log for '%s'", req.container_id);
            send(client_fd, &resp, sizeof(resp), 0);
            break;
        }
        resp.status = 0;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "LOG_FOLLOWS");
        send(client_fd, &resp, sizeof(resp), 0);
        char chunk[1024];
        size_t r;
        while ((r = fread(chunk, 1, sizeof(chunk), f)) > 0)
            send(client_fd, chunk, r, 0);
        fclose(f);
        break;
    }

    case CMD_STOP: {
        pthread_mutex_lock(&ctx->metadata_lock);
        container_record_t *rec = ctx->containers;
        while (rec) {
            if (strcmp(rec->id, req.container_id) == 0) {
                if (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING) {
                    rec->stop_requested = 1;
                    kill(rec->host_pid, SIGTERM);
                    resp.status = 0;
                    snprintf(resp.message, CONTROL_MESSAGE_LEN,
                             "Sent SIGTERM to '%s' (PID %d)", rec->id, rec->host_pid);
                } else {
                    resp.status = -1;
                    snprintf(resp.message, CONTROL_MESSAGE_LEN,
                             "Container '%s' is not running", req.container_id);
                }
                break;
            }
            rec = rec->next;
        }
        if (!rec) {
            resp.status = -1;
            snprintf(resp.message, CONTROL_MESSAGE_LEN,
                     "Unknown container '%s'", req.container_id);
        }
        pthread_mutex_unlock(&ctx->metadata_lock);
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }

    default:
        resp.status = -1;
        snprintf(resp.message, CONTROL_MESSAGE_LEN, "Unknown command");
        send(client_fd, &resp, sizeof(resp), 0);
        break;
    }
}

/* ---------------------------------------------------------------
 * Supervisor main loop
 * --------------------------------------------------------------- */

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    struct sockaddr_un addr;
    int rc;

    (void)rootfs;

    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;
    g_ctx = &ctx;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0) { perror("pthread_mutex_init"); return 1; }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0) { perror("bounded_buffer_init"); return 1; }

    /* Open kernel monitor device */
    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "[supervisor] Warning: cannot open /dev/container_monitor: %s\n",
                strerror(errno));

    /* Create UNIX domain socket */
    unlink(CONTROL_PATH);
    ctx.server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (ctx.server_fd < 0) { perror("socket"); return 1; }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(ctx.server_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("bind"); return 1;
    }
    if (listen(ctx.server_fd, 8) < 0) {
        perror("listen"); return 1;
    }

    /* Signal handlers */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = sigchld_handler;
    sa.sa_flags = SA_RESTART | SA_NOCLDSTOP;
    sigaction(SIGCHLD, &sa, NULL);

    sa.sa_handler = sigterm_handler;
    sa.sa_flags = SA_RESTART;
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGINT,  &sa, NULL);

    /* Start logging consumer thread */
    pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx);

    fprintf(stderr, "[supervisor] Ready. Listening on %s\n", CONTROL_PATH);

    /* Event loop */
    while (!ctx.should_stop) {
        fd_set fds;
        struct timeval tv = {1, 0};
        FD_ZERO(&fds);
        FD_SET(ctx.server_fd, &fds);

        int sel = select(ctx.server_fd + 1, &fds, NULL, NULL, &tv);
        if (sel < 0) {
            if (errno == EINTR) continue;
            perror("select");
            break;
        }
        if (sel == 0) continue;

        int client_fd = accept(ctx.server_fd, NULL, NULL);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            perror("accept");
            continue;
        }

        handle_client(&ctx, client_fd);
        close(client_fd);
    }

    fprintf(stderr, "[supervisor] Shutting down...\n");

    /* Stop all running containers */
    pthread_mutex_lock(&ctx.metadata_lock);
    container_record_t *rec = ctx.containers;
    while (rec) {
        if (rec->state == CONTAINER_RUNNING || rec->state == CONTAINER_STARTING) {
            rec->stop_requested = 1;
            kill(rec->host_pid, SIGTERM);
        }
        rec = rec->next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);

    /* Wait a moment for children to exit */
    sleep(1);

    bounded_buffer_begin_shutdown(&ctx.log_buffer);
    pthread_join(ctx.logger_thread, NULL);
    bounded_buffer_destroy(&ctx.log_buffer);

    /* Free container list */
    pthread_mutex_lock(&ctx.metadata_lock);
    rec = ctx.containers;
    while (rec) {
        container_record_t *next = rec->next;
        free(rec);
        rec = next;
    }
    pthread_mutex_unlock(&ctx.metadata_lock);
    pthread_mutex_destroy(&ctx.metadata_lock);

    if (ctx.monitor_fd >= 0) close(ctx.monitor_fd);
    close(ctx.server_fd);
    unlink(CONTROL_PATH);

    fprintf(stderr, "[supervisor] Done.\n");
    return 0;
}

/* ---------------------------------------------------------------
 * Client: send a request to the supervisor
 * --------------------------------------------------------------- */

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
        fprintf(stderr, "Cannot connect to supervisor at %s: %s\n"
                        "Is the supervisor running? (sudo ./engine supervisor ./rootfs-base)\n",
                CONTROL_PATH, strerror(errno));
        close(fd);
        return 1;
    }

    send(fd, req, sizeof(*req), 0);

    /* For logs, stream raw data after the initial response */
    if (req->kind == CMD_LOGS) {
        if (recv(fd, &resp, sizeof(resp), 0) == (ssize_t)sizeof(resp)) {
            if (resp.status != 0) {
                fprintf(stderr, "Error: %s\n", resp.message);
                close(fd);
                return 1;
            }
            char buf[1024];
            ssize_t n;
            while ((n = recv(fd, buf, sizeof(buf), 0)) > 0)
                fwrite(buf, 1, n, stdout);
        }
        close(fd);
        return 0;
    }

    /* For run, we get two responses: one immediate, one when container exits */
    if (req->kind == CMD_RUN) {
        /* First response: container started */
        if (recv(fd, &resp, sizeof(resp), MSG_WAITALL) == (ssize_t)sizeof(resp)) {
            if (resp.status != 0) {
                fprintf(stderr, "Error: %s\n", resp.message);
                close(fd);
                return 1;
            }
            printf("%s\n", resp.message);
        }
        /* Second response: container exited */
        if (recv(fd, &resp, sizeof(resp), MSG_WAITALL) == (ssize_t)sizeof(resp)) {
            printf("%s\n", resp.message);
            close(fd);
            return resp.status;
        }
        close(fd);
        return 0;
    }

    /* Normal single response */
    if (recv(fd, &resp, sizeof(resp), MSG_WAITALL) == (ssize_t)sizeof(resp)) {
        if (resp.status == 0)
            printf("%s\n", resp.message);
        else
            fprintf(stderr, "Error: %s\n", resp.message);
    }

    close(fd);
    return resp.status == 0 ? 0 : 1;
}

/* ---------------------------------------------------------------
 * CLI command handlers
 * --------------------------------------------------------------- */

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s start <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_START;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
    req.soft_limit_bytes = DEFAULT_SOFT_LIMIT;
    req.hard_limit_bytes = DEFAULT_HARD_LIMIT;
    if (parse_optional_flags(&req, argc, argv, 5) != 0) return 1;
    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 5) {
        fprintf(stderr,
                "Usage: %s run <id> <container-rootfs> <command> [--soft-mib N] [--hard-mib N] [--nice N]\n",
                argv[0]);
        return 1;
    }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_RUN;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    strncpy(req.rootfs, argv[3], sizeof(req.rootfs) - 1);
    strncpy(req.command, argv[4], sizeof(req.command) - 1);
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
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s logs <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;
    if (argc < 3) { fprintf(stderr, "Usage: %s stop <id>\n", argv[0]); return 1; }
    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);
    return send_control_request(&req);
}

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
