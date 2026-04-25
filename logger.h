#ifndef LOGGER_H
#define LOGGER_H

#include <stdio.h>
#include <pthread.h>

void logger_init(const char *filename);
void logger_close(void);
void log_event(const char *event_type, int session_id, const char *resource, const char *message);
void log_conflict(int session_id1, int session_id2, const char *resource);

#endif
