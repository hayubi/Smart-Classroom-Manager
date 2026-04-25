#ifndef RESOURCES_H
#define RESOURCES_H

#include <pthread.h>
#include <semaphore.h>

#define NUM_RESOURCE_TYPES 3
#define RESOURCE_PROJECTOR  0
#define RESOURCE_COMP_LAB   1
#define RESOURCE_MULTIMEDIA 2

extern const char *resource_names[NUM_RESOURCE_TYPES];

typedef struct {
    int total_units[NUM_RESOURCE_TYPES];      /* max units per resource type */
    int available_units[NUM_RESOURCE_TYPES];  /* currently available units   */
    pthread_mutex_t pool_mutex;               /* protects available_units    */
    sem_t semaphores[NUM_RESOURCE_TYPES];     /* hard concurrency limits     */
    int in_use[NUM_RESOURCE_TYPES];           /* sessions currently using    */
} ResourcePool;

void resource_pool_init(ResourcePool *pool,
                        int projectors, int comp_labs, int multimedia);
void resource_pool_destroy(ResourcePool *pool);

/* Returns 1 if successfully acquired without waiting, 0 if not available */
int resource_try_acquire(ResourcePool *pool, int resource_type);

/* Blocking acquire — waits until the resource becomes available */
void resource_acquire(ResourcePool *pool, int resource_type);

/* Release a previously acquired resource unit */
void resource_release(ResourcePool *pool, int resource_type);

/* Peek at available units (mutex-protected read) */
int resource_available(ResourcePool *pool, int resource_type);

#endif
