#ifndef WFC_CONTROL_H
#define WFC_CONTROL_H

#include <stddef.h>
#include <sys/types.h>

/* Line-delimited JSON over a Unix socket between the root daemon and the GUI. */
typedef void (*ctl_cmd_fn)(const char *line);

int  ctl_start(const char *path, uid_t owner, ctl_cmd_fn on_cmd);
void ctl_stop(void);
void ctl_emit(const char *json);          /* one JSON object, no newline */
int  ctl_has_client(void);

/* Tiny JSON helpers for flat objects produced by our own GUI. */
int  json_str(const char *json, const char *key, char *out, size_t outsize);
int  json_int(const char *json, const char *key, int *out);
int  json_bool(const char *json, const char *key, int *out);
/* Calls fn for each string in an array value; returns number of items. */
int  json_str_array(const char *json, const char *key, void (*fn)(void *ctx, const char *s), void *ctx);
/* Escapes `in` as a JSON string body into out. */
void json_escape(char *out, size_t outsize, const char *in);

#endif
