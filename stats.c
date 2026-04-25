#include "stats.h"
#include <stdio.h>
#include <string.h>

void stats_init(Stats *s) {
    memset(s, 0, sizeof(Stats));
    pthread_mutex_init(&s->stats_mutex, NULL);
}

void stats_destroy(Stats *s) {
    pthread_mutex_destroy(&s->stats_mutex);
}

void stats_record_request(Stats *s, int resource_type) {
    pthread_mutex_lock(&s->stats_mutex);
    s->total_requests[resource_type]++;
    pthread_mutex_unlock(&s->stats_mutex);
}

void stats_record_granted(Stats *s, int resource_type) {
    pthread_mutex_lock(&s->stats_mutex);
    s->total_granted[resource_type]++;
    pthread_mutex_unlock(&s->stats_mutex);
}

void stats_record_wait(Stats *s, int resource_type, double wait_seconds) {
    pthread_mutex_lock(&s->stats_mutex);
    s->total_waited[resource_type]++;
    s->total_wait_time[resource_type] += wait_seconds;
    pthread_mutex_unlock(&s->stats_mutex);
}

void stats_record_conflict(Stats *s) {
    pthread_mutex_lock(&s->stats_mutex);
    s->conflicts_detected++;
    pthread_mutex_unlock(&s->stats_mutex);
}

void stats_print_summary(Stats *s, ResourcePool *pool) {
    printf("\n\033[1;36m╔══════════════════════════════════════════════════╗\033[0m\n");
    printf("\033[1;36m║       SIMULATION SUMMARY STATISTICS              ║\033[0m\n");
    printf("\033[1;36m╚══════════════════════════════════════════════════╝\033[0m\n\n");

    printf("%-20s %10s %10s %10s %12s\n",
           "Resource", "Requests", "Granted", "Waited", "Avg Wait(s)");
    printf("%-20s %10s %10s %10s %12s\n",
           "────────────────────", "─────────", "────────",
           "──────", "──────────");

    for (int i = 0; i < NUM_RESOURCE_TYPES; i++) {
        double avg_wait = 0.0;
        if (s->total_waited[i] > 0)
            avg_wait = s->total_wait_time[i] / s->total_waited[i];

        printf("%-20s %10d %10d %10d %12.2f\n",
               resource_names[i],
               s->total_requests[i],
               s->total_granted[i],
               s->total_waited[i],
               avg_wait);
    }

    printf("\n  Conflict alerts detected : %d\n", s->conflicts_detected);

    printf("\n  Resource utilisation at end:\n");
    for (int i = 0; i < NUM_RESOURCE_TYPES; i++) {
        int total = pool->total_units[i];
        int avail = pool->available_units[i];
        int used  = total - avail;
        printf("    %-15s  total=%-3d  in-use=%-3d  available=%d\n",
               resource_names[i], total, used, avail);
    }
    printf("\n");
}
