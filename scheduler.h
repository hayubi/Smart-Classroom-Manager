#ifndef SCHEDULER_H
#define SCHEDULER_H

#include <pthread.h>
#include <time.h>
#include "resources.h"
#include "stats.h"

#define MAX_QUEUE_SIZE     64
#define AGING_INTERVAL_SEC  3   /* priority boost every N seconds of waiting */
#define AGING_BOOST         1   /* how much priority increases per interval  */
#define MAX_PRIORITY       10
#define MIN_PRIORITY        1

typedef struct {
    int session_id;
    int resource_type;
    int priority;               /* higher = served first                      */
    time_t enqueue_time;        /* when session entered the queue             */
    time_t last_boosted_time;   /* FIX: track last boost to avoid re-counting */
    pthread_cond_t  cond;       /* signalled when resource is ready           */
    pthread_mutex_t cond_mutex;
    int granted;                /* set to 1 when resource is granted          */
} WaitEntry;

typedef struct {
    WaitEntry *entries[MAX_QUEUE_SIZE];
    int size;
    pthread_mutex_t queue_mutex;
    pthread_t aging_thread;
    int running;                /* set to 0 to stop aging thread              */
    Stats *stats;               /* FIX: pointer so conflict count is recorded */
} WaitQueue;

void scheduler_init(WaitQueue *wq, Stats *stats);
void scheduler_destroy(WaitQueue *wq);

/* Enqueue a session waiting for resource_type; blocks until granted.
   On return the caller owns the resource — bookkeeping is done inside. */
void scheduler_wait(WaitQueue *wq, ResourcePool *pool,
                    int session_id, int resource_type, int priority);

/* Notify the highest-priority waiter that a resource of type is free.
   Also does the pool bookkeeping (available_units / in_use) for the
   granted session so main.c does not double-decrement. */
void scheduler_notify(WaitQueue *wq, ResourcePool *pool, int resource_type);

/* Return number of waiters for a given resource type */
int scheduler_queue_depth(WaitQueue *wq, int resource_type);

#endif
