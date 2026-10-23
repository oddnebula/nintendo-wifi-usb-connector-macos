#ifndef WFC_LOG_H
#define WFC_LOG_H

#include <stdarg.h>

enum { LOG_LVL_DEBUG, LOG_LVL_INFO, LOG_LVL_WARN, LOG_LVL_ERR };

/* Optional sink so log lines can also be forwarded to the GUI. */
typedef void (*log_sink_fn)(int level, const char *msg);
void log_set_sink(log_sink_fn fn);
void log_set_verbose(int verbose);

void log_msg(int level, const char *fmt, ...) __attribute__((format(printf, 2, 3)));

#define log_debug(...) log_msg(LOG_LVL_DEBUG, __VA_ARGS__)
#define log_info(...)  log_msg(LOG_LVL_INFO, __VA_ARGS__)
#define log_warn(...)  log_msg(LOG_LVL_WARN, __VA_ARGS__)
#define log_err(...)   log_msg(LOG_LVL_ERR, __VA_ARGS__)

#endif
