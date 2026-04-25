#ifndef STATS_H
#define STATS_H

#include <pthread.h>
#include "resources.h"

typedef struct {
    int total_requests[NUM_RESOURCE_TYPES];
    int total_granted[NUM_RESOURCE_TYPES];
    int total_waited[NUM_RESOURCE_TYPES];    /* sessions that had to wait  */
    double total_wait_time[NUM_RESOURCE_TYPES]; /* seconds total wait time */
    int conflicts_detected;
    pthread_mutex_t stats_mutex;
} Stats;

void stats_init(Stats *s);
void stats_destroy(Stats *s);
void stats_record_request(Stats *s, int resource_type);
void stats_record_granted(Stats *s, int resource_type);
void stats_record_wait(Stats *s, int resource_type, double wait_seconds);
void stats_record_conflict(Stats *s);
void stats_print_summary(Stats *s, ResourcePool *pool);

#endif
