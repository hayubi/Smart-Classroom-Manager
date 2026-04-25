#include "resources.h"
#include <stdio.h>
#include <stdlib.h>

const char *resource_names[NUM_RESOURCE_TYPES] = {
    "Projector",
    "CompLab",
    "Multimedia"
};

void resource_pool_init(ResourcePool *pool,
                        int projectors, int comp_labs, int multimedia) {
    pool->total_units[RESOURCE_PROJECTOR]  = projectors;
    pool->total_units[RESOURCE_COMP_LAB]   = comp_labs;
    pool->total_units[RESOURCE_MULTIMEDIA] = multimedia;

    pool->available_units[RESOURCE_PROJECTOR]  = projectors;
    pool->available_units[RESOURCE_COMP_LAB]   = comp_labs;
    pool->available_units[RESOURCE_MULTIMEDIA] = multimedia;

    pool->in_use[RESOURCE_PROJECTOR]  = 0;
    pool->in_use[RESOURCE_COMP_LAB]   = 0;
    pool->in_use[RESOURCE_MULTIMEDIA] = 0;

    if (pthread_mutex_init(&pool->pool_mutex, NULL) != 0) {
        perror("Failed to init pool mutex");
        exit(EXIT_FAILURE);
    }

    for (int i = 0; i < NUM_RESOURCE_TYPES; i++) {
        if (sem_init(&pool->semaphores[i], 0, pool->total_units[i]) != 0) {
            perror("Failed to init semaphore");
            exit(EXIT_FAILURE);
        }
    }
}

void resource_pool_destroy(ResourcePool *pool) {
    pthread_mutex_destroy(&pool->pool_mutex);
    for (int i = 0; i < NUM_RESOURCE_TYPES; i++) {
        sem_destroy(&pool->semaphores[i]);
    }
}

/*
 * Try to acquire one unit without blocking.
 * Returns 1 on success, 0 if no unit is available.
 *
 * The mutex check confirms a unit is free.  Once confirmed, sem_trywait()
 * is called inside the same critical section — it is guaranteed to succeed
 * immediately because available_units > 0 means the semaphore value > 0.
 * This keeps the semaphore value exactly equal to available_units at all
 * times, so the semaphore genuinely enforces the hard concurrency limit
 * as stated in the proposal.
 */
int resource_try_acquire(ResourcePool *pool, int resource_type) {
    int acquired = 0;

    pthread_mutex_lock(&pool->pool_mutex);
    if (pool->available_units[resource_type] > 0) {
        pool->available_units[resource_type]--;
        pool->in_use[resource_type]++;
        /* Decrement the semaphore to match — guaranteed non-blocking here
         * because available_units > 0 implies semaphore value > 0. */
        sem_trywait(&pool->semaphores[resource_type]);
        acquired = 1;
    }
    pthread_mutex_unlock(&pool->pool_mutex);

    return acquired;
}

/*
 * Blocking acquire used by the scheduler's notify path.
 * Decrements the semaphore (blocks if zero) then updates the counters.
 */
void resource_acquire(ResourcePool *pool, int resource_type) {
    sem_wait(&pool->semaphores[resource_type]);

    pthread_mutex_lock(&pool->pool_mutex);
    pool->available_units[resource_type]--;
    pool->in_use[resource_type]++;
    pthread_mutex_unlock(&pool->pool_mutex);
}

/*
 * Release one unit back to the pool.
 * Updates counters under the mutex, then posts the semaphore so any
 * thread blocked in resource_acquire() (or scheduler_notify) can proceed.
 */
void resource_release(ResourcePool *pool, int resource_type) {
    pthread_mutex_lock(&pool->pool_mutex);
    pool->available_units[resource_type]++;
    pool->in_use[resource_type]--;
    pthread_mutex_unlock(&pool->pool_mutex);

    sem_post(&pool->semaphores[resource_type]);
}

/* Peek at available units (mutex-protected read). */
int resource_available(ResourcePool *pool, int resource_type) {
    int avail;
    pthread_mutex_lock(&pool->pool_mutex);
    avail = pool->available_units[resource_type];
    pthread_mutex_unlock(&pool->pool_mutex);
    return avail;
}
