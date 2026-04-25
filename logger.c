#include "logger.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <pthread.h>

static FILE *log_file = NULL;
static pthread_mutex_t log_mutex = PTHREAD_MUTEX_INITIALIZER;

void logger_init(const char *filename) {
    log_file = fopen(filename, "w");
    if (!log_file) {
        perror("Failed to open log file");
        exit(EXIT_FAILURE);
    }
    fprintf(log_file, "=== Smart Classroom Resource Manager Log ===\n");
    fprintf(log_file, "%-25s %-15s %-6s %-15s %s\n",
            "Timestamp", "Event", "SessID", "Resource", "Message");
    fprintf(log_file, "----------------------------------------------------------------------\n");
    fflush(log_file);
}

void logger_close(void) {
    if (log_file) {
        fprintf(log_file, "=== End of Log ===\n");
        fclose(log_file);
        log_file = NULL;
    }
}

static void get_timestamp(char *buf, size_t size) {
    time_t t = time(NULL);
    struct tm *tm_info = localtime(&t);
    strftime(buf, size, "%Y-%m-%d %H:%M:%S", tm_info);
}

void log_event(const char *event_type, int session_id, const char *resource, const char *message) {
    char ts[32];
    get_timestamp(ts, sizeof(ts));

    pthread_mutex_lock(&log_mutex);
    if (log_file) {
        fprintf(log_file, "%-25s %-15s %-6d %-15s %s\n",
                ts, event_type, session_id, resource ? resource : "-", message ? message : "");
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_mutex);
}

void log_conflict(int session_id1, int session_id2, const char *resource) {
    char ts[32];
    char msg[128];
    get_timestamp(ts, sizeof(ts));
    snprintf(msg, sizeof(msg), "Sessions %d and %d compete for %s", session_id1, session_id2, resource);

    pthread_mutex_lock(&log_mutex);
    printf("\033[1;31m[CONFLICT ALERT] %s\033[0m\n", msg);
    if (log_file) {
        fprintf(log_file, "%-25s %-15s %-6s %-15s %s\n",
                ts, "CONFLICT", "-", resource, msg);
        fflush(log_file);
    }
    pthread_mutex_unlock(&log_mutex);
}
