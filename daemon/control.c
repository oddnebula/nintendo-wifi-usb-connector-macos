#include "control.h"
#include "log.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

static int listen_fd = -1;
static int client_fd = -1;
static char sock_path[256];
static ctl_cmd_fn handler;
static pthread_t thread;
static pthread_mutex_t wlock = PTHREAD_MUTEX_INITIALIZER;
static volatile int stopping;

int ctl_has_client(void) { return client_fd >= 0; }

void ctl_emit(const char *json)
{
    pthread_mutex_lock(&wlock);
    if (client_fd >= 0) {
        size_t n = strlen(json);
        if (write(client_fd, json, n) != (ssize_t)n || write(client_fd, "\n", 1) != 1) {
            close(client_fd);
            client_fd = -1;
        }
    }
    pthread_mutex_unlock(&wlock);
}

static void *serve(void *arg)
{
    (void)arg;
    while (!stopping) {
        int fd = accept(listen_fd, NULL, NULL);
        if (fd < 0) {
            if (stopping) break;
            if (errno == EINTR) continue;
            usleep(100000);
            continue;
        }
        pthread_mutex_lock(&wlock);
        if (client_fd >= 0) close(client_fd);   /* newest GUI wins */
        client_fd = fd;
        pthread_mutex_unlock(&wlock);
        handler("{\"cmd\":\"hello\"}");

        char buf[16384];
        size_t have = 0;
        for (;;) {
            ssize_t r = read(fd, buf + have, sizeof(buf) - 1 - have);
            if (r <= 0) break;
            have += (size_t)r;
            buf[have] = 0;
            char *nl;
            while ((nl = memchr(buf, '\n', have))) {
                *nl = 0;
                if (nl > buf) handler(buf);
                size_t used = (size_t)(nl - buf) + 1;
                memmove(buf, nl + 1, have - used);
                have -= used;
                buf[have] = 0;
            }
            if (have == sizeof(buf) - 1) have = 0;  /* oversized line: drop */
        }
        pthread_mutex_lock(&wlock);
        if (client_fd == fd) { close(fd); client_fd = -1; }
        pthread_mutex_unlock(&wlock);
        handler("{\"cmd\":\"goodbye\"}");
    }
    return NULL;
}

int ctl_start(const char *path, uid_t owner, ctl_cmd_fn on_cmd)
{
    handler = on_cmd;
    strncpy(sock_path, path, sizeof(sock_path) - 1);
    unlink(path);
    listen_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    struct sockaddr_un sa = { .sun_family = AF_UNIX };
    strncpy(sa.sun_path, path, sizeof(sa.sun_path) - 1);
    if (bind(listen_fd, (struct sockaddr *)&sa, sizeof sa) || listen(listen_fd, 2)) {
        log_err("control socket %s: %s", path, strerror(errno));
        return -1;
    }
    /* Only the user who launched the GUI may talk to the root daemon. */
    if (owner != (uid_t)-1) chown(path, owner, (gid_t)-1);
    chmod(path, 0600);
    pthread_create(&thread, NULL, serve, NULL);
    return 0;
}

void ctl_stop(void)
{
    stopping = 1;
    if (listen_fd >= 0) { shutdown(listen_fd, SHUT_RDWR); close(listen_fd); listen_fd = -1; }
    pthread_mutex_lock(&wlock);
    if (client_fd >= 0) { shutdown(client_fd, SHUT_RDWR); }
    pthread_mutex_unlock(&wlock);
    unlink(sock_path);
}

/* ---- JSON helpers -------------------------------------------------------- */

static const char *find_value(const char *json, const char *key)
{
    char pat[96];
    snprintf(pat, sizeof pat, "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat))) {
        p += strlen(pat);
        while (*p == ' ' || *p == '\t') p++;
        if (*p == ':') {
            p++;
            while (*p == ' ' || *p == '\t') p++;
            return p;
        }
    }
    return NULL;
}

static const char *parse_string(const char *p, char *out, size_t outsize)
{
    if (*p != '"') return NULL;
    p++;
    size_t o = 0;
    while (*p && *p != '"') {
        char c = *p++;
        if (c == '\\' && *p) {
            char e = *p++;
            switch (e) {
            case 'n': c = '\n'; break;
            case 't': c = '\t'; break;
            case 'u': c = '?'; for (int i = 0; i < 4 && *p; i++) p++; break;
            default: c = e; break;
            }
        }
        if (o + 1 < outsize) out[o++] = c;
    }
    if (outsize) out[o] = 0;
    return *p == '"' ? p + 1 : NULL;
}

int json_str(const char *json, const char *key, char *out, size_t outsize)
{
    const char *p = find_value(json, key);
    return p && parse_string(p, out, outsize) != NULL;
}

int json_int(const char *json, const char *key, int *out)
{
    const char *p = find_value(json, key);
    if (!p || !(*p == '-' || (*p >= '0' && *p <= '9'))) return 0;
    *out = atoi(p);
    return 1;
}

int json_bool(const char *json, const char *key, int *out)
{
    const char *p = find_value(json, key);
    if (!p) return 0;
    if (!strncmp(p, "true", 4)) { *out = 1; return 1; }
    if (!strncmp(p, "false", 5)) { *out = 0; return 1; }
    return 0;
}

int json_str_array(const char *json, const char *key, void (*fn)(void *, const char *), void *ctx)
{
    const char *p = find_value(json, key);
    if (!p || *p != '[') return 0;
    p++;
    int n = 0;
    for (;;) {
        while (*p == ' ' || *p == ',' || *p == '\t') p++;
        if (*p != '"') break;
        char item[256];
        p = parse_string(p, item, sizeof item);
        if (!p) break;
        fn(ctx, item);
        n++;
    }
    return n;
}

void json_escape(char *out, size_t outsize, const char *in)
{
    size_t o = 0;
    for (; *in && o + 7 < outsize; in++) {
        unsigned char c = (unsigned char)*in;
        if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = (char)c; }
        else if (c == '\n') { out[o++] = '\\'; out[o++] = 'n'; }
        else if (c < 0x20) { o += (size_t)snprintf(out + o, outsize - o, "\\u%04x", c); }
        else out[o++] = (char)c;
    }
    out[o] = 0;
}
