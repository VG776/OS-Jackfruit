#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/resource.h>
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
#define CONTROL_MESSAGE_LEN 4096
#define CHILD_COMMAND_LEN 256
#define LOG_CHUNK_SIZE 4096
#define LOG_BUFFER_CAPACITY 16
#define DEFAULT_SOFT_LIMIT (40UL << 20)
#define DEFAULT_HARD_LIMIT (64UL << 20)
#define STOP_GRACE_POLL_USEC 200000
#define STOP_GRACE_ATTEMPTS 10

typedef enum
{
    CMD_SUPERVISOR = 0,
    CMD_START,
    CMD_RUN,
    CMD_PS,
    CMD_LOGS,
    CMD_STOP
} command_kind_t;

typedef enum
{
    CONTAINER_STARTING = 0,
    CONTAINER_RUNNING,
    CONTAINER_STOPPED,
    CONTAINER_KILLED,
    CONTAINER_EXITED
} container_state_t;

typedef struct container_record
{
    char id[CONTAINER_ID_LEN];
    pid_t host_pid;
    time_t started_at;
    container_state_t state;
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int exit_code;
    int exit_signal;
    int finished;
    int stop_requested;
    char rootfs[PATH_MAX];
    char log_path[PATH_MAX];
    int log_read_fd;
    pthread_t producer_thread;
    int producer_joinable;
    pthread_mutex_t lock;
    pthread_cond_t exited_cv;
    void *clone_stack;
    struct container_record *next;
} container_record_t;

typedef struct
{
    char container_id[CONTAINER_ID_LEN];
    size_t length;
    char data[LOG_CHUNK_SIZE];
} log_item_t;

typedef struct
{
    log_item_t items[LOG_BUFFER_CAPACITY];
    size_t head;
    size_t tail;
    size_t count;
    int shutting_down;
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
} bounded_buffer_t;

typedef struct
{
    command_kind_t kind;
    char container_id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    unsigned long soft_limit_bytes;
    unsigned long hard_limit_bytes;
    int nice_value;
} control_request_t;

typedef struct
{
    int status;
    char message[CONTROL_MESSAGE_LEN];
} control_response_t;

typedef struct
{
    char id[CONTAINER_ID_LEN];
    char rootfs[PATH_MAX];
    char command[CHILD_COMMAND_LEN];
    int nice_value;
    int log_write_fd;
} child_config_t;

typedef struct
{
    int server_fd;
    int monitor_fd;
    int should_stop;
    int logger_started;
    pthread_t logger_thread;
    bounded_buffer_t log_buffer;
    pthread_mutex_t metadata_lock;
    container_record_t *containers;
} supervisor_ctx_t;

typedef struct
{
    supervisor_ctx_t *ctx;
    int client_fd;
} client_job_t;

typedef struct
{
    supervisor_ctx_t *ctx;
    container_record_t *record;
} producer_job_t;

static volatile sig_atomic_t g_supervisor_stop = 0;
static volatile sig_atomic_t g_supervisor_reap = 0;
static volatile sig_atomic_t g_run_client_stop = 0;

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

static int readn(int fd, void *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t rc = read(fd, (char *)buf + off, len - off);
        if (rc == 0)
            return -1;
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)rc;
    }
    return 0;
}

static int writen(int fd, const void *buf, size_t len)
{
    size_t off = 0;
    while (off < len)
    {
        ssize_t rc = write(fd, (const char *)buf + off, len - off);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            return -1;
        }
        off += (size_t)rc;
    }
    return 0;
}

static void response_set(control_response_t *resp, int status, const char *fmt, ...)
{
    va_list ap;
    resp->status = status;
    va_start(ap, fmt);
    vsnprintf(resp->message, sizeof(resp->message), fmt, ap);
    va_end(ap);
}

static int parse_mib_flag(const char *flag,
                          const char *value,
                          unsigned long *target_bytes)
{
    char *end = NULL;
    unsigned long mib;

    errno = 0;
    mib = strtoul(value, &end, 10);
    if (errno != 0 || end == value || *end != '\0')
    {
        fprintf(stderr, "Invalid value for %s: %s\n", flag, value);
        return -1;
    }

    if (mib > ULONG_MAX / (1UL << 20))
    {
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

    for (i = start_index; i < argc; i += 2)
    {
        char *end = NULL;
        long nice_value;

        if (i + 1 >= argc)
        {
            fprintf(stderr, "Missing value for option: %s\n", argv[i]);
            return -1;
        }

        if (strcmp(argv[i], "--soft-mib") == 0)
        {
            if (parse_mib_flag("--soft-mib", argv[i + 1], &req->soft_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--hard-mib") == 0)
        {
            if (parse_mib_flag("--hard-mib", argv[i + 1], &req->hard_limit_bytes) != 0)
                return -1;
            continue;
        }

        if (strcmp(argv[i], "--nice") == 0)
        {
            errno = 0;
            nice_value = strtol(argv[i + 1], &end, 10);
            if (errno != 0 || end == argv[i + 1] || *end != '\0' ||
                nice_value < -20 || nice_value > 19)
            {
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

    if (req->soft_limit_bytes > req->hard_limit_bytes)
    {
        fprintf(stderr, "Invalid limits: soft limit cannot exceed hard limit\n");
        return -1;
    }

    return 0;
}

static const char *state_to_string(container_state_t state)
{
    switch (state)
    {
    case CONTAINER_STARTING:
        return "starting";
    case CONTAINER_RUNNING:
        return "running";
    case CONTAINER_STOPPED:
        return "stopped";
    case CONTAINER_KILLED:
        return "hard_limit_killed";
    case CONTAINER_EXITED:
        return "exited";
    default:
        return "unknown";
    }
}

static int bounded_buffer_init(bounded_buffer_t *buffer)
{
    int rc;

    memset(buffer, 0, sizeof(*buffer));

    rc = pthread_mutex_init(&buffer->mutex, NULL);
    if (rc != 0)
        return rc;

    rc = pthread_cond_init(&buffer->not_empty, NULL);
    if (rc != 0)
    {
        pthread_mutex_destroy(&buffer->mutex);
        return rc;
    }

    rc = pthread_cond_init(&buffer->not_full, NULL);
    if (rc != 0)
    {
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

    if (buffer->shutting_down)
    {
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

    if (buffer->count == 0 && buffer->shutting_down)
    {
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

static int append_log_chunk(const char *container_id, const char *data, size_t length)
{
    char path[PATH_MAX];
    int fd;

    snprintf(path, sizeof(path), "%s/%s.log", LOG_DIR, container_id);
    fd = open(path, O_CREAT | O_WRONLY | O_APPEND, 0644);
    if (fd < 0)
        return -1;

    if (writen(fd, data, length) != 0)
    {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

void *logging_thread(void *arg)
{
    supervisor_ctx_t *ctx = (supervisor_ctx_t *)arg;
    log_item_t item;

    while (bounded_buffer_pop(&ctx->log_buffer, &item) == 0)
        (void)append_log_chunk(item.container_id, item.data, item.length);

    return NULL;
}

int child_fn(void *arg)
{
    child_config_t *cfg = (child_config_t *)arg;

    if (setpriority(PRIO_PROCESS, 0, cfg->nice_value) < 0)
        perror("setpriority");

    (void)sethostname(cfg->id, strlen(cfg->id));

    if (mount(NULL, "/", NULL, MS_REC | MS_PRIVATE, NULL) < 0)
        perror("mount private");

    if (chdir(cfg->rootfs) < 0)
    {
        perror("chdir rootfs");
        return 1;
    }

    if (chroot(".") < 0)
    {
        perror("chroot");
        return 1;
    }

    if (chdir("/") < 0)
    {
        perror("chdir /");
        return 1;
    }

    mkdir("/proc", 0555);
    if (mount("proc", "/proc", "proc", 0, NULL) < 0)
        perror("mount /proc");

    if (dup2(cfg->log_write_fd, STDOUT_FILENO) < 0)
        return 1;
    if (dup2(cfg->log_write_fd, STDERR_FILENO) < 0)
        return 1;
    close(cfg->log_write_fd);

    execl("/bin/sh", "sh", "-c", cfg->command, (char *)NULL);
    perror("execl");
    return 127;
}

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

static container_record_t *find_container_by_id_locked(supervisor_ctx_t *ctx, const char *id)
{
    container_record_t *it = ctx->containers;
    while (it)
    {
        if (strcmp(it->id, id) == 0)
            return it;
        it = it->next;
    }
    return NULL;
}

static container_record_t *find_container_by_pid_locked(supervisor_ctx_t *ctx, pid_t pid)
{
    container_record_t *it = ctx->containers;
    while (it)
    {
        if (it->host_pid == pid)
            return it;
        it = it->next;
    }
    return NULL;
}

static int rootfs_busy_locked(supervisor_ctx_t *ctx, const char *rootfs)
{
    container_record_t *it = ctx->containers;
    while (it)
    {
        if (!it->finished && strcmp(it->rootfs, rootfs) == 0)
            return 1;
        it = it->next;
    }
    return 0;
}

static void free_container_record(container_record_t *rec)
{
    if (!rec)
        return;

    if (rec->producer_joinable)
        pthread_join(rec->producer_thread, NULL);

    if (rec->log_read_fd >= 0)
        close(rec->log_read_fd);

    if (rec->clone_stack)
        free(rec->clone_stack);

    pthread_cond_destroy(&rec->exited_cv);
    pthread_mutex_destroy(&rec->lock);
    free(rec);
}

static void *producer_thread_fn(void *arg)
{
    producer_job_t *job = (producer_job_t *)arg;
    supervisor_ctx_t *ctx = job->ctx;
    container_record_t *rec = job->record;
    log_item_t item;
    ssize_t rc;

    free(job);

    for (;;)
    {
        rc = read(rec->log_read_fd, item.data, sizeof(item.data));
        if (rc == 0)
            break;
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }

        memset(item.container_id, 0, sizeof(item.container_id));
        strncpy(item.container_id, rec->id, sizeof(item.container_id) - 1);
        item.length = (size_t)rc;

        if (bounded_buffer_push(&ctx->log_buffer, &item) != 0)
            break;
    }

    return NULL;
}

static int add_container_record(supervisor_ctx_t *ctx, container_record_t *rec)
{
    pthread_mutex_lock(&ctx->metadata_lock);
    rec->next = ctx->containers;
    ctx->containers = rec;
    pthread_mutex_unlock(&ctx->metadata_lock);
    return 0;
}

static int start_container(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           container_record_t **out)
{
    container_record_t *rec;
    child_config_t *cfg;
    int pipefd[2] = {-1, -1};
    pid_t pid;
    producer_job_t *job;

    rec = calloc(1, sizeof(*rec));
    if (!rec)
        return -1;

    rec->log_read_fd = -1;
    pthread_mutex_init(&rec->lock, NULL);
    pthread_cond_init(&rec->exited_cv, NULL);

    strncpy(rec->id, req->container_id, sizeof(rec->id) - 1);
    strncpy(rec->rootfs, req->rootfs, sizeof(rec->rootfs) - 1);
    snprintf(rec->log_path, sizeof(rec->log_path), "%s/%s.log", LOG_DIR, rec->id);
    rec->started_at = time(NULL);
    rec->state = CONTAINER_STARTING;
    rec->soft_limit_bytes = req->soft_limit_bytes;
    rec->hard_limit_bytes = req->hard_limit_bytes;
    rec->exit_code = -1;
    rec->exit_signal = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    if (find_container_by_id_locked(ctx, req->container_id) != NULL)
    {
        pthread_mutex_unlock(&ctx->metadata_lock);
        free_container_record(rec);
        errno = EEXIST;
        return -1;
    }
    if (rootfs_busy_locked(ctx, req->rootfs))
    {
        pthread_mutex_unlock(&ctx->metadata_lock);
        free_container_record(rec);
        errno = EBUSY;
        return -1;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (pipe(pipefd) < 0)
    {
        free_container_record(rec);
        return -1;
    }

    cfg = calloc(1, sizeof(*cfg));
    if (!cfg)
    {
        close(pipefd[0]);
        close(pipefd[1]);
        free_container_record(rec);
        return -1;
    }

    strncpy(cfg->id, req->container_id, sizeof(cfg->id) - 1);
    strncpy(cfg->rootfs, req->rootfs, sizeof(cfg->rootfs) - 1);
    strncpy(cfg->command, req->command, sizeof(cfg->command) - 1);
    cfg->nice_value = req->nice_value;
    cfg->log_write_fd = pipefd[1];

    rec->clone_stack = malloc(STACK_SIZE);
    if (!rec->clone_stack)
    {
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        free_container_record(rec);
        return -1;
    }

    pid = clone(child_fn,
                (char *)rec->clone_stack + STACK_SIZE,
                CLONE_NEWUTS | CLONE_NEWPID | CLONE_NEWNS | SIGCHLD,
                cfg);
    if (pid < 0)
    {
        free(cfg);
        close(pipefd[0]);
        close(pipefd[1]);
        free_container_record(rec);
        return -1;
    }

    free(cfg);
    close(pipefd[1]);
    rec->log_read_fd = pipefd[0];
    rec->host_pid = pid;
    rec->state = CONTAINER_RUNNING;

    if (ctx->monitor_fd >= 0)
    {
        if (register_with_monitor(ctx->monitor_fd,
                                  rec->id,
                                  rec->host_pid,
                                  rec->soft_limit_bytes,
                                  rec->hard_limit_bytes) != 0)
        {
            fprintf(stderr,
                    "warning: failed to register %s with monitor (%s)\n",
                    rec->id,
                    strerror(errno));
        }
    }

    job = calloc(1, sizeof(*job));
    if (!job)
    {
        kill(rec->host_pid, SIGKILL);
        free_container_record(rec);
        return -1;
    }

    job->ctx = ctx;
    job->record = rec;
    if (pthread_create(&rec->producer_thread, NULL, producer_thread_fn, job) != 0)
    {
        free(job);
        kill(rec->host_pid, SIGKILL);
        free_container_record(rec);
        return -1;
    }
    rec->producer_joinable = 1;

    add_container_record(ctx, rec);
    *out = rec;
    return 0;
}

static void mark_exited(supervisor_ctx_t *ctx, pid_t pid, int status)
{
    container_record_t *rec;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec = find_container_by_pid_locked(ctx, pid);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!rec)
        return;

    pthread_mutex_lock(&rec->lock);
    if (!rec->finished)
    {
        rec->finished = 1;
        if (WIFEXITED(status))
        {
            rec->exit_code = WEXITSTATUS(status);
            rec->exit_signal = 0;
            if (rec->stop_requested)
                rec->state = CONTAINER_STOPPED;
            else
                rec->state = CONTAINER_EXITED;
        }
        else if (WIFSIGNALED(status))
        {
            rec->exit_code = 128 + WTERMSIG(status);
            rec->exit_signal = WTERMSIG(status);
            if (rec->stop_requested)
                rec->state = CONTAINER_STOPPED;
            else if (WTERMSIG(status) == SIGKILL)
                rec->state = CONTAINER_KILLED;
            else
                rec->state = CONTAINER_EXITED;
        }
        pthread_cond_broadcast(&rec->exited_cv);
    }
    pthread_mutex_unlock(&rec->lock);

    if (ctx->monitor_fd >= 0)
        (void)unregister_from_monitor(ctx->monitor_fd, rec->id, rec->host_pid);
}

static void reap_children(supervisor_ctx_t *ctx)
{
    int status;
    pid_t pid;

    for (;;)
    {
        pid = waitpid(-1, &status, WNOHANG);
        if (pid <= 0)
            break;
        mark_exited(ctx, pid, status);
    }
}

static int request_stop_locked(container_record_t *rec)
{
    rec->stop_requested = 1;
    if (kill(rec->host_pid, SIGTERM) < 0)
        return -1;
    return 0;
}

static int ensure_stopped(supervisor_ctx_t *ctx, container_record_t *rec)
{
    int i;

    pthread_mutex_lock(&rec->lock);
    if (!rec->finished && request_stop_locked(rec) != 0)
    {
        pthread_mutex_unlock(&rec->lock);
        return -1;
    }
    pthread_mutex_unlock(&rec->lock);

    for (i = 0; i < STOP_GRACE_ATTEMPTS; i++)
    {
        reap_children(ctx);
        pthread_mutex_lock(&rec->lock);
        if (rec->finished)
        {
            pthread_mutex_unlock(&rec->lock);
            return 0;
        }
        pthread_mutex_unlock(&rec->lock);
        usleep(STOP_GRACE_POLL_USEC);
    }

    pthread_mutex_lock(&rec->lock);
    rec->stop_requested = 1;
    if (!rec->finished)
        (void)kill(rec->host_pid, SIGKILL);
    pthread_mutex_unlock(&rec->lock);

    for (i = 0; i < STOP_GRACE_ATTEMPTS; i++)
    {
        reap_children(ctx);
        pthread_mutex_lock(&rec->lock);
        if (rec->finished)
        {
            pthread_mutex_unlock(&rec->lock);
            return 0;
        }
        pthread_mutex_unlock(&rec->lock);
        usleep(STOP_GRACE_POLL_USEC);
    }

    return -1;
}

static void build_ps_output(supervisor_ctx_t *ctx, control_response_t *resp)
{
    container_record_t *it;
    size_t used = 0;

    used += (size_t)snprintf(resp->message + used,
                             sizeof(resp->message) - used,
                             "id pid state soft_mib hard_mib exit log\n");

    pthread_mutex_lock(&ctx->metadata_lock);
    it = ctx->containers;
    while (it && used < sizeof(resp->message))
    {
        int exit_value;
        const char *state;

        pthread_mutex_lock(&it->lock);
        exit_value = it->finished ? it->exit_code : -1;
        state = state_to_string(it->state);
        used += (size_t)snprintf(resp->message + used,
                                 sizeof(resp->message) - used,
                                 "%s %d %s %lu %lu %d %s\n",
                                 it->id,
                                 (int)it->host_pid,
                                 state,
                                 it->soft_limit_bytes >> 20,
                                 it->hard_limit_bytes >> 20,
                                 exit_value,
                                 it->log_path);
        pthread_mutex_unlock(&it->lock);
        it = it->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    resp->status = 0;
}

static void build_logs_output(supervisor_ctx_t *ctx,
                              const char *container_id,
                              control_response_t *resp)
{
    container_record_t *rec;
    int fd;
    ssize_t rc;
    size_t used = 0;

    pthread_mutex_lock(&ctx->metadata_lock);
    rec = find_container_by_id_locked(ctx, container_id);
    pthread_mutex_unlock(&ctx->metadata_lock);

    if (!rec)
    {
        response_set(resp, 1, "container not found: %s", container_id);
        return;
    }

    fd = open(rec->log_path, O_RDONLY);
    if (fd < 0)
    {
        response_set(resp, 1, "cannot open log file: %s", rec->log_path);
        return;
    }

    while (used < sizeof(resp->message) - 1)
    {
        rc = read(fd, resp->message + used, sizeof(resp->message) - used - 1);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            close(fd);
            response_set(resp, 1, "failed to read logs for %s", container_id);
            return;
        }
        if (rc == 0)
            break;
        used += (size_t)rc;
    }
    resp->message[used] = '\0';
    close(fd);

    if (used == 0)
        response_set(resp, 0, "(no logs yet for %s)", container_id);
    else
        resp->status = 0;
}

static void wait_for_exit(container_record_t *rec)
{
    pthread_mutex_lock(&rec->lock);
    while (!rec->finished)
        pthread_cond_wait(&rec->exited_cv, &rec->lock);
    pthread_mutex_unlock(&rec->lock);
}

static void handle_request(supervisor_ctx_t *ctx,
                           const control_request_t *req,
                           control_response_t *resp)
{
    container_record_t *rec;

    switch (req->kind)
    {
    case CMD_START:
    case CMD_RUN:
        if (start_container(ctx, req, &rec) != 0)
        {
            response_set(resp, 1, "failed to start %s: %s", req->container_id, strerror(errno));
            return;
        }

        if (req->kind == CMD_START)
        {
            response_set(resp,
                         0,
                         "started id=%s pid=%d log=%s",
                         rec->id,
                         (int)rec->host_pid,
                         rec->log_path);
            return;
        }

        wait_for_exit(rec);
        pthread_mutex_lock(&rec->lock);
        response_set(resp,
                     rec->exit_code,
                     "run complete id=%s state=%s status=%d",
                     rec->id,
                     state_to_string(rec->state),
                     rec->exit_code);
        pthread_mutex_unlock(&rec->lock);
        return;

    case CMD_PS:
        build_ps_output(ctx, resp);
        return;

    case CMD_LOGS:
        build_logs_output(ctx, req->container_id, resp);
        return;

    case CMD_STOP:
        pthread_mutex_lock(&ctx->metadata_lock);
        rec = find_container_by_id_locked(ctx, req->container_id);
        pthread_mutex_unlock(&ctx->metadata_lock);

        if (!rec)
        {
            response_set(resp, 1, "container not found: %s", req->container_id);
            return;
        }

        if (ensure_stopped(ctx, rec) != 0)
            response_set(resp, 1, "failed to stop %s", req->container_id);
        else
            response_set(resp, 0, "stopped %s", req->container_id);
        return;

    default:
        response_set(resp, 1, "unsupported command");
        return;
    }
}

static void *client_thread(void *arg)
{
    client_job_t *job = (client_job_t *)arg;
    supervisor_ctx_t *ctx = job->ctx;
    int fd = job->client_fd;
    control_request_t req;
    control_response_t resp;

    free(job);

    memset(&req, 0, sizeof(req));
    memset(&resp, 0, sizeof(resp));

    if (readn(fd, &req, sizeof(req)) != 0)
    {
        close(fd);
        return NULL;
    }

    handle_request(ctx, &req, &resp);
    (void)writen(fd, &resp, sizeof(resp));
    close(fd);
    return NULL;
}

static int setup_control_socket(void)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    unlink(CONTROL_PATH);

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    if (listen(fd, 32) < 0)
    {
        close(fd);
        return -1;
    }

    return fd;
}

static void supervisor_signal_handler(int sig)
{
    if (sig == SIGCHLD)
    {
        g_supervisor_reap = 1;
        return;
    }

    if (sig == SIGINT || sig == SIGTERM)
        g_supervisor_stop = 1;
}

static int install_supervisor_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = supervisor_signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGCHLD, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGINT, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;

    return 0;
}

static int ensure_log_dir(void)
{
    struct stat st;
    if (stat(LOG_DIR, &st) == 0)
    {
        if (!S_ISDIR(st.st_mode))
        {
            errno = ENOTDIR;
            return -1;
        }
        return 0;
    }

    if (mkdir(LOG_DIR, 0755) < 0)
        return -1;

    return 0;
}

static void shutdown_containers(supervisor_ctx_t *ctx)
{
    container_record_t *it;

    pthread_mutex_lock(&ctx->metadata_lock);
    it = ctx->containers;
    while (it)
    {
        pthread_mutex_lock(&it->lock);
        if (!it->finished)
        {
            it->stop_requested = 1;
            (void)kill(it->host_pid, SIGTERM);
        }
        pthread_mutex_unlock(&it->lock);
        it = it->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    usleep(400000);
    reap_children(ctx);

    pthread_mutex_lock(&ctx->metadata_lock);
    it = ctx->containers;
    while (it)
    {
        pthread_mutex_lock(&it->lock);
        if (!it->finished)
            (void)kill(it->host_pid, SIGKILL);
        pthread_mutex_unlock(&it->lock);
        it = it->next;
    }
    pthread_mutex_unlock(&ctx->metadata_lock);

    usleep(400000);
    reap_children(ctx);
}

static void cleanup_supervisor(supervisor_ctx_t *ctx)
{
    container_record_t *it;
    container_record_t *next;

    shutdown_containers(ctx);
    bounded_buffer_begin_shutdown(&ctx->log_buffer);
    if (ctx->logger_started)
        pthread_join(ctx->logger_thread, NULL);

    pthread_mutex_lock(&ctx->metadata_lock);
    it = ctx->containers;
    ctx->containers = NULL;
    pthread_mutex_unlock(&ctx->metadata_lock);

    while (it)
    {
        next = it->next;
        free_container_record(it);
        it = next;
    }

    if (ctx->monitor_fd >= 0)
        close(ctx->monitor_fd);
    if (ctx->server_fd >= 0)
        close(ctx->server_fd);

    unlink(CONTROL_PATH);
    bounded_buffer_destroy(&ctx->log_buffer);
    pthread_mutex_destroy(&ctx->metadata_lock);
}

static int run_supervisor(const char *rootfs)
{
    supervisor_ctx_t ctx;
    int rc;

    (void)rootfs;
    memset(&ctx, 0, sizeof(ctx));
    ctx.server_fd = -1;
    ctx.monitor_fd = -1;

    rc = pthread_mutex_init(&ctx.metadata_lock, NULL);
    if (rc != 0)
    {
        errno = rc;
        perror("pthread_mutex_init");
        return 1;
    }

    rc = bounded_buffer_init(&ctx.log_buffer);
    if (rc != 0)
    {
        errno = rc;
        perror("bounded_buffer_init");
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    if (ensure_log_dir() != 0)
    {
        perror("mkdir logs");
        bounded_buffer_destroy(&ctx.log_buffer);
        pthread_mutex_destroy(&ctx.metadata_lock);
        return 1;
    }

    ctx.monitor_fd = open("/dev/container_monitor", O_RDWR);
    if (ctx.monitor_fd < 0)
        fprintf(stderr, "warning: /dev/container_monitor unavailable: %s\n", strerror(errno));

    ctx.server_fd = setup_control_socket();
    if (ctx.server_fd < 0)
    {
        perror("setup control socket");
        cleanup_supervisor(&ctx);
        return 1;
    }

    if (install_supervisor_signals() != 0)
    {
        perror("install signals");
        cleanup_supervisor(&ctx);
        return 1;
    }

    if (pthread_create(&ctx.logger_thread, NULL, logging_thread, &ctx) != 0)
    {
        perror("pthread_create logger");
        cleanup_supervisor(&ctx);
        return 1;
    }
    ctx.logger_started = 1;

    fprintf(stderr, "supervisor started control=%s\n", CONTROL_PATH);

    while (!g_supervisor_stop)
    {
        struct pollfd pfd;
        int poll_rc;

        if (g_supervisor_reap)
        {
            g_supervisor_reap = 0;
            reap_children(&ctx);
        }

        pfd.fd = ctx.server_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        poll_rc = poll(&pfd, 1, 500);
        if (poll_rc < 0)
        {
            if (errno == EINTR)
                continue;
            perror("poll");
            break;
        }

        if (poll_rc == 0)
            continue;

        if (pfd.revents & POLLIN)
        {
            int client_fd = accept(ctx.server_fd, NULL, NULL);
            if (client_fd >= 0)
            {
                pthread_t tid;
                client_job_t *job = calloc(1, sizeof(*job));
                if (!job)
                {
                    close(client_fd);
                    continue;
                }
                job->ctx = &ctx;
                job->client_fd = client_fd;

                if (pthread_create(&tid, NULL, client_thread, job) == 0)
                    pthread_detach(tid);
                else
                {
                    close(client_fd);
                    free(job);
                }
            }
        }
    }

    cleanup_supervisor(&ctx);
    return 0;
}

static void run_client_signal_handler(int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
        g_run_client_stop = 1;
}

static int install_run_client_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = run_client_signal_handler;
    sigemptyset(&sa.sa_mask);

    if (sigaction(SIGINT, &sa, NULL) < 0)
        return -1;
    if (sigaction(SIGTERM, &sa, NULL) < 0)
        return -1;
    return 0;
}

static int raw_send_request(const control_request_t *req, control_response_t *resp)
{
    int fd;
    struct sockaddr_un addr;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        close(fd);
        return -1;
    }

    if (writen(fd, req, sizeof(*req)) != 0)
    {
        close(fd);
        return -1;
    }

    if (readn(fd, resp, sizeof(*resp)) != 0)
    {
        close(fd);
        return -1;
    }

    close(fd);
    return 0;
}

static void forward_stop_for_run(const char *container_id)
{
    control_request_t stop_req;
    control_response_t stop_resp;

    memset(&stop_req, 0, sizeof(stop_req));
    memset(&stop_resp, 0, sizeof(stop_resp));
    stop_req.kind = CMD_STOP;
    strncpy(stop_req.container_id, container_id, sizeof(stop_req.container_id) - 1);

    (void)raw_send_request(&stop_req, &stop_resp);
}

static int send_control_request(const control_request_t *req)
{
    int fd;
    struct sockaddr_un addr;
    control_response_t resp;
    int stop_forwarded = 0;

    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
    {
        perror("socket");
        return 1;
    }

    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, CONTROL_PATH, sizeof(addr.sun_path) - 1);

    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0)
    {
        perror("connect");
        close(fd);
        return 1;
    }

    if (writen(fd, req, sizeof(*req)) != 0)
    {
        perror("write request");
        close(fd);
        return 1;
    }

    if (req->kind == CMD_RUN)
    {
        struct pollfd pfd;

        g_run_client_stop = 0;
        if (install_run_client_signals() != 0)
            perror("install run signals");

        for (;;)
        {
            if (g_run_client_stop && !stop_forwarded)
            {
                forward_stop_for_run(req->container_id);
                stop_forwarded = 1;
            }

            pfd.fd = fd;
            pfd.events = POLLIN;
            pfd.revents = 0;

            if (poll(&pfd, 1, 200) < 0)
            {
                if (errno == EINTR)
                    continue;
                perror("poll");
                close(fd);
                return 1;
            }

            if ((pfd.revents & POLLIN) && readn(fd, &resp, sizeof(resp)) == 0)
                break;

            if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL))
            {
                fprintf(stderr, "run response channel closed unexpectedly\n");
                close(fd);
                return 1;
            }
        }
    }
    else
    {
        if (readn(fd, &resp, sizeof(resp)) != 0)
        {
            perror("read response");
            close(fd);
            return 1;
        }
    }

    close(fd);

    if (resp.message[0] != '\0')
        printf("%s\n", resp.message);

    return resp.status;
}

static int cmd_start(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5)
    {
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

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

    return send_control_request(&req);
}

static int cmd_run(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 5)
    {
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

    if (parse_optional_flags(&req, argc, argv, 5) != 0)
        return 1;

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

    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s logs <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_LOGS;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

static int cmd_stop(int argc, char *argv[])
{
    control_request_t req;

    if (argc < 3)
    {
        fprintf(stderr, "Usage: %s stop <id>\n", argv[0]);
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.kind = CMD_STOP;
    strncpy(req.container_id, argv[2], sizeof(req.container_id) - 1);

    return send_control_request(&req);
}

int main(int argc, char *argv[])
{
    if (argc < 2)
    {
        usage(argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "supervisor") == 0)
    {
        if (argc < 3)
        {
            fprintf(stderr, "Usage: %s supervisor <base-rootfs>\n", argv[0]);
            return 1;
        }
        return run_supervisor(argv[2]);
    }

    if (strcmp(argv[1], "start") == 0)
        return cmd_start(argc, argv);

    if (strcmp(argv[1], "run") == 0)
        return cmd_run(argc, argv);

    if (strcmp(argv[1], "ps") == 0)
        return cmd_ps();

    if (strcmp(argv[1], "logs") == 0)
        return cmd_logs(argc, argv);

    if (strcmp(argv[1], "stop") == 0)
        return cmd_stop(argc, argv);

    usage(argv[0]);
    return 1;
}
