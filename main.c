#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

#include "resources.h"
#include "scheduler.h"
#include "logger.h"
#include "stats.h"

/* ── Configuration ──────────────────────────────────────────────────────── */
#define NUM_SESSIONS        12      /* total class sessions to simulate      */
#define MAX_USE_TIME_SEC     4      /* max seconds a session holds a resource */
#define LOG_FILE            "classroom_log.txt"

/* Pool capacities */
#define PROJECTOR_UNITS     2
#define COMP_LAB_UNITS      1
#define MULTIMEDIA_UNITS    2

/* ── Shared globals ─────────────────────────────────────────────────────── */
static ResourcePool  pool;
static WaitQueue     wait_queue;
static Stats         stats;

/* ── Session thread argument ────────────────────────────────────────────── */
typedef struct {
    int session_id;
    int resource_type;   /* which resource this session needs */
    int priority;        /* 1 (low) … 10 (high)               */
} SessionArgs;

/* ── Session thread function ────────────────────────────────────────────── */
static void *session_thread(void *arg) {
    SessionArgs *sa = (SessionArgs *)arg;
    int id    = sa->session_id;
    int rtype = sa->resource_type;
    int prio  = sa->priority;

    printf("[SESSION %2d] Requesting %-12s  (priority %d)\n",
           id, resource_names[rtype], prio);
    log_event("REQUEST", id, resource_names[rtype], "Session requesting resource");
    stats_record_request(&stats, rtype);

    time_t t_request = time(NULL);

    /* Try to acquire immediately (fast path — no waiting). */
    if (!resource_try_acquire(&pool, rtype)) {
        /* Resource busy — enter wait queue and block until granted.
         *
         * FIX: removed the extra pool.available_units-- / pool.in_use++
         * block that used to follow scheduler_wait().  That block caused
         * available_units to go negative on every queued session because
         * scheduler_notify() now does the bookkeeping itself before waking
         * this thread.  When scheduler_wait() returns the resource is already
         * fully accounted for in the pool. */
        time_t t_wait_start = time(NULL);
        scheduler_wait(&wait_queue, &pool, id, rtype, prio);

        double waited = difftime(time(NULL), t_wait_start);
        stats_record_wait(&stats, rtype, waited);
    }

    time_t t_acquired = time(NULL);
    double wait_time  = difftime(t_acquired, t_request);

    printf("[SESSION %2d] Acquired   %-12s  (waited %.0fs)\n",
           id, resource_names[rtype], wait_time);
    log_event("ACQUIRED", id, resource_names[rtype], "Resource acquired");
    stats_record_granted(&stats, rtype);

    /* Simulate using the resource. */
    int use_duration = 1 + rand() % MAX_USE_TIME_SEC;
    sleep(use_duration);

    /* Release resource back to the pool. */
    resource_release(&pool, rtype);
    printf("[SESSION %2d] Released   %-12s  (used %ds)\n",
           id, resource_names[rtype], use_duration);
    log_event("RELEASED", id, resource_names[rtype], "Resource released");

    /* Notify the scheduler — pass pool so it can do bookkeeping for the
     * next waiter (FIX: scheduler_notify now takes ResourcePool *pool). */
    scheduler_notify(&wait_queue, &pool, rtype);

    free(sa);
    return NULL;
}

/* ── Main ───────────────────────────────────────────────────────────────── */
int main(void) {
    srand((unsigned)time(NULL));

    printf("\033[1;33m╔══════════════════════════════════════════════╗\033[0m\n");
    printf("\033[1;33m║   Smart Classroom Resource Manager           ║\033[0m\n");
    printf("\033[1;33m║   OS Simulation — pthreads / mutex / sem     ║\033[0m\n");
    printf("\033[1;33m╚══════════════════════════════════════════════╝\033[0m\n\n");

    /* Initialise subsystems */
    logger_init(LOG_FILE);
    resource_pool_init(&pool, PROJECTOR_UNITS, COMP_LAB_UNITS, MULTIMEDIA_UNITS);
    stats_init(&stats);
    /* FIX: pass &stats so scheduler can record conflict alerts. */
    scheduler_init(&wait_queue, &stats);

    printf("Resource pool initialised:\n");
    for (int i = 0; i < NUM_RESOURCE_TYPES; i++)
        printf("  %-15s : %d unit(s)\n",
               resource_names[i], pool.total_units[i]);
    printf("\nLaunching %d session threads...\n\n", NUM_SESSIONS);

    /* Launch session threads */
    pthread_t threads[NUM_SESSIONS];
    int resource_cycle[] = {
        RESOURCE_PROJECTOR, RESOURCE_COMP_LAB,   RESOURCE_MULTIMEDIA,
        RESOURCE_PROJECTOR, RESOURCE_PROJECTOR,  RESOURCE_COMP_LAB,
        RESOURCE_MULTIMEDIA, RESOURCE_COMP_LAB,  RESOURCE_PROJECTOR,
        RESOURCE_MULTIMEDIA, RESOURCE_PROJECTOR, RESOURCE_COMP_LAB
    };

    for (int i = 0; i < NUM_SESSIONS; i++) {
        SessionArgs *sa = malloc(sizeof(SessionArgs));
        if (!sa) { perror("malloc SessionArgs"); continue; }
        sa->session_id    = i + 1;
        sa->resource_type = resource_cycle[i];
        sa->priority      = (rand() % MAX_PRIORITY) + MIN_PRIORITY;

        if (pthread_create(&threads[i], NULL, session_thread, sa) != 0) {
            perror("pthread_create");
            free(sa);
        }
        /* Small stagger so not all threads hit at exactly t=0 */
        usleep(150000);  /* 150 ms */
    }

    /* Wait for all sessions to complete. */
    for (int i = 0; i < NUM_SESSIONS; i++)
        pthread_join(threads[i], NULL);

    /* Shutdown */
    scheduler_destroy(&wait_queue);
    stats_print_summary(&stats, &pool);
    stats_destroy(&stats);
    resource_pool_destroy(&pool);
    logger_close();

    printf("Log written to: %s\n\n", LOG_FILE);
    return 0;
}
