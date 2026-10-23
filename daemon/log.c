#include "log.h"

#include <stdio.h>
#include <time.h>
#include <pthread.h>

static log_sink_fn sink;
static int verbose;
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;

void log_set_sink(log_sink_fn fn) { sink = fn; }
void log_set_verbose(int v) { verbose = v; }

void log_msg(int level, const char *fmt, ...)
{
    if (level == LOG_LVL_DEBUG && !verbose) return;
    static const char *tags[] = { "debug", "info", "warn", "error" };
    char msg[1024];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);

    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    pthread_mutex_lock(&lock);
    fprintf(stderr, "%02d:%02d:%02d [%s] %s\n", tm.tm_hour, tm.tm_min, tm.tm_sec, tags[level], msg);
    pthread_mutex_unlock(&lock);
    if (sink && level >= LOG_LVL_INFO) sink(level, msg);
}
