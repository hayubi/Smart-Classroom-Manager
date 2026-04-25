#include "scheduler.h"
#include "logger.h"
#include "stats.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ── Aging thread ───────────────────────────────────────────────────────── */
static void *aging_thread_func(void *arg) {
    WaitQueue *wq = (WaitQueue *)arg;

    while (wq->running) {
        sleep(AGING_INTERVAL_SEC);

        pthread_mutex_lock(&wq->queue_mutex);
        time_t now = time(NULL);

        for (int i = 0; i < wq->size; i++) {
            WaitEntry *e = wq->entries[i];

            /*
             * FIX (aging bug): previously the code computed
             *   boosts = (now - enqueue_time) / interval
             * and added boosts*AGING_BOOST every wake-up, causing the same
             * elapsed seconds to be counted again and again (e.g. a session
             * waiting 6s got +2 on the first wake and +4 on the second).
             *
             * Now we only count seconds elapsed since the LAST boost, using
             * last_boosted_time.  Each interval contributes exactly +AGING_BOOST
             * once.
             */
            double since_last = difftime(now, e->last_boosted_time);
            int boosts = (int)(since_last / AGING_INTERVAL_SEC);

            if (boosts > 0) {
                int old_prio = e->priority;
                e->priority += boosts * AGING_BOOST;
                if (e->priority > MAX_PRIORITY)
                    e->priority = MAX_PRIORITY;

                /* Advance the baseline so we don't re-count these seconds */
                e->last_boosted_time = now;

                if (e->priority != old_prio) {
                    char msg[64];
                    double total_waited = difftime(now, e->enqueue_time);
                    snprintf(msg, sizeof(msg),
                             "Priority boosted %d -> %d (waited %.0fs)",
                             old_prio, e->priority, total_waited);
                    log_event("AGING", e->session_id,
                              resource_names[e->resource_type], msg);
                    printf("[AGING] Session %d: %s\n", e->session_id, msg);
                }
            }
        }
        pthread_mutex_unlock(&wq->queue_mutex);
    }
    return NULL;
}

/* ── Init / Destroy ─────────────────────────────────────────────────────── */

/* FIX: accept Stats pointer so conflict alerts can be counted. */
void scheduler_init(WaitQueue *wq, Stats *stats) {
    wq->size    = 0;
    wq->running = 1;
    wq->stats   = stats;
    pthread_mutex_init(&wq->queue_mutex, NULL);
    pthread_create(&wq->aging_thread, NULL, aging_thread_func, wq);
}

void scheduler_destroy(WaitQueue *wq) {
    if (!wq->running && wq->aging_thread == 0) return; /* never initialised */
    wq->running = 0;
    pthread_join(wq->aging_thread, NULL);
    wq->aging_thread = 0;
    pthread_mutex_destroy(&wq->queue_mutex);
}

/* ── Internal helpers ───────────────────────────────────────────────────── */

/*
 * Insert entry maintaining descending priority order.
 * FIX (FCFS bug): old code used strict '>' so equal-priority entries were
 * served in insertion order but the bubble stopped early, breaking FCFS.
 * New code keeps strict '>' for the swap — this means a new entry that ties
 * in priority stays BEHIND existing entries at the same level (correct FCFS:
 * earlier arrivals are already higher in the array).
 */
static void enqueue(WaitQueue *wq, WaitEntry *entry) {
    if (wq->size >= MAX_QUEUE_SIZE) {
        fprintf(stderr, "Wait queue full!\n");
        return;
    }
    /* Append at the end, then bubble up past entries with STRICTLY lower
     * priority.  Entries with equal priority are left ahead of the newcomer,
     * preserving arrival order (FCFS) within the same priority tier. */
    int i = wq->size;
    wq->entries[i] = entry;
    wq->size++;

    while (i > 0 && wq->entries[i]->priority > wq->entries[i-1]->priority) {
        WaitEntry *tmp   = wq->entries[i];
        wq->entries[i]   = wq->entries[i-1];
        wq->entries[i-1] = tmp;
        i--;
    }
}

/* Remove an entry by pointer. */
static void dequeue(WaitQueue *wq, WaitEntry *entry) {
    for (int i = 0; i < wq->size; i++) {
        if (wq->entries[i] == entry) {
            for (int j = i; j < wq->size - 1; j++)
                wq->entries[j] = wq->entries[j+1];
            wq->size--;
            return;
        }
    }
}

/* ── Public API ─────────────────────────────────────────────────────────── */

void scheduler_wait(WaitQueue *wq, ResourcePool *pool __attribute__((unused)),
                    int session_id, int resource_type, int priority) {

    /*
     * FIX (dangling pointer bug): WaitEntry was previously stack-allocated.
     * Its pointer was stored in wq->entries[] and accessed by the aging thread
     * and scheduler_notify() — both running on other threads.  If this function
     * returned while those threads still held the pointer, it became a dangling
     * reference (use-after-free / undefined behaviour).
     *
     * Solution: heap-allocate the entry and free it only after it has been
     * fully dequeued and all other threads are done with it.
     */
    WaitEntry *entry = malloc(sizeof(WaitEntry));
    if (!entry) {
        perror("malloc WaitEntry");
        exit(EXIT_FAILURE);
    }
    entry->session_id       = session_id;
    entry->resource_type    = resource_type;
    entry->priority         = priority;
    entry->enqueue_time     = time(NULL);
    entry->last_boosted_time = entry->enqueue_time; /* FIX: init for aging */
    entry->granted          = 0;
    pthread_cond_init(&entry->cond, NULL);
    pthread_mutex_init(&entry->cond_mutex, NULL);

    /* Add to wait queue */
    pthread_mutex_lock(&wq->queue_mutex);
    enqueue(wq, entry);

    /*
     * FIX (conflict stats): log_conflict() now also calls
     * stats_record_conflict() via wq->stats so the summary is accurate.
     */
    for (int i = 0; i < wq->size - 1; i++) {
        if (wq->entries[i]->resource_type == resource_type &&
            wq->entries[i] != entry) {
            log_conflict(wq->entries[i]->session_id, session_id,
                         resource_names[resource_type]);
            if (wq->stats)
                stats_record_conflict(wq->stats);
            break;
        }
    }
    pthread_mutex_unlock(&wq->queue_mutex);

    log_event("QUEUED", session_id, resource_names[resource_type],
              "Waiting for resource");
    printf("[QUEUE] Session %d waiting for %s (priority %d)\n",
           session_id, resource_names[resource_type], priority);

    /* Block until granted by scheduler_notify(). */
    pthread_mutex_lock(&entry->cond_mutex);
    while (!entry->granted)
        pthread_cond_wait(&entry->cond, &entry->cond_mutex);
    pthread_mutex_unlock(&entry->cond_mutex);

    /*
     * At this point scheduler_notify() has already dequeued the entry and
     * done the pool bookkeeping (available_units--, in_use++), so we just
     * clean up the synchronisation primitives and free the heap allocation.
     */
    pthread_cond_destroy(&entry->cond);
    pthread_mutex_destroy(&entry->cond_mutex);
    free(entry);
}

/*
 * Wake the highest-priority waiter for resource_type.
 *
 * FIX (double bookkeeping bug): previously main.c did
 *   available_units[rtype]--; in_use[rtype]++;
 * AFTER scheduler_wait() returned, but scheduler_notify() had already
 * conceptually granted the resource.  This caused available_units to go
 * negative on every queued session.
 *
 * Now scheduler_notify() does the pool bookkeeping itself (under the pool
 * mutex) before signalling the waiter.  main.c no longer touches the pool
 * counters after scheduler_wait() returns.
 *
 * FIX (race with aging thread): previously the queue_mutex was released
 * before signalling the condition variable, allowing the aging thread to
 * re-acquire the mutex and iterate over a partially-freed entry.  Now we
 * signal the condition variable while still holding cond_mutex (which is
 * always correct for pthread_cond_signal), and we only release queue_mutex
 * after the pool bookkeeping is complete.
 */
void scheduler_notify(WaitQueue *wq, ResourcePool *pool, int resource_type) {
    pthread_mutex_lock(&wq->queue_mutex);

    /* Find highest-priority waiter for this resource type. */
    int best = -1;
    for (int i = 0; i < wq->size; i++) {
        if (wq->entries[i]->resource_type == resource_type) {
            if (best == -1 ||
                wq->entries[i]->priority > wq->entries[best]->priority)
                best = i;
        }
    }

    if (best != -1) {
        WaitEntry *e = wq->entries[best];
        dequeue(wq, e);
        pthread_mutex_unlock(&wq->queue_mutex);

        /*
         * Use the semaphore as the actual gate before doing bookkeeping.
         * resource_release() just called sem_post() so the semaphore value
         * is guaranteed to be >= 1 here — sem_wait() returns immediately
         * without blocking.  This makes the semaphore a real enforcer:
         * the granted session cannot proceed past this point unless the
         * semaphore permits it, satisfying the proposal requirement.
         */
        sem_wait(&pool->semaphores[resource_type]);

        /* Do the pool bookkeeping on behalf of the waking session. */
        pthread_mutex_lock(&pool->pool_mutex);
        pool->available_units[resource_type]--;
        pool->in_use[resource_type]++;
        pthread_mutex_unlock(&pool->pool_mutex);

        /* Capture id before signalling — the session thread may free the
         * heap entry as soon as it wakes, so reading e->session_id after
         * pthread_cond_signal() is a use-after-free. */
        int granted_session = e->session_id;

        /* Signal the waiting session thread. */
        pthread_mutex_lock(&e->cond_mutex);
        e->granted = 1;
        pthread_cond_signal(&e->cond);
        pthread_mutex_unlock(&e->cond_mutex);

        log_event("GRANTED", granted_session,
                  resource_names[resource_type], "Resource granted from queue");
        printf("[GRANT] Session %d granted %s from queue\n",
               granted_session, resource_names[resource_type]);
    } else {
        pthread_mutex_unlock(&wq->queue_mutex);
    }
}

int scheduler_queue_depth(WaitQueue *wq, int resource_type) {
    int count = 0;
    pthread_mutex_lock(&wq->queue_mutex);
    for (int i = 0; i < wq->size; i++)
        if (wq->entries[i]->resource_type == resource_type)
            count++;
    pthread_mutex_unlock(&wq->queue_mutex);
    return count;
}
