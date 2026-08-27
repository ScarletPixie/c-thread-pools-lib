#include "ctp.h"

#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>


/// @name Worker Thread - Summary
/// Functions for initializing thread workers and the worker routine.
/// @{

typedef struct
{
    pthread_t thread;
    ctp_pool_t *pool;
} _worker;

/// @brief Initializes the `_worker` object.
/// @param worker A pointer to a worker object.
/// @param pool The pool the worker belongs to. Must not be `NULL`.
/// @return Returns `worker` on success, or `NULL` on failure.
static _worker* _worker_init(_worker* worker, ctp_pool_t* pool);

/// @brief The routine of each worker thread in the thread pool.
/// @param worker A pointer to a `_worker` object. Must not be of any other type.
/// @return Always returns NULL.
static void* _worker_routine(void* worker);

/// @}


/// @name Task Queue - Summary
/// Functions for initializing and managing the task queue.
/// @{

typedef struct _task_queue
{
    size_t size;
    size_t current;
    ctp_task_t* tasks[];
} _task_queue_t;

/// @brief Creates a new task queue object.
/// @param size The fixed number of slots available in the queue.
/// @return Returns a New task queue object.
static _task_queue_t*   _task_queue_create(size_t size);

/// @brief Destroys a task queue object and every task inside it.
static void             _task_queue_destroy(_task_queue_t *queue);

/// @brief Inserts a new task into the queue.
/// @return Returns 0 on success, -1 on failure like if the queue is full for example.
static int              _task_queue_push(_task_queue_t *queue, ctp_task_t *task);

/// @brief Removes the next task present inside the queue.
/// @return Returns the removed task or NULL if there are no elements to be removed.
static ctp_task_t*      _task_queue_pop(_task_queue_t *queue);

/// @brief Returns whether the queue is full of tasks.
static bool             _task_queue_full(_task_queue_t *queue);

/// @brief Searches the queue for the input task.
/// @param task The task to be searched inside the queue.
/// @return Returns `task` if it's found, `NULL` otherwise.
static ctp_task_t*      _task_queue_search(_task_queue_t *queue, ctp_task_t* task);

/// @brief Returns whether there are no tasks stored in the queue.
static bool             _task_queue_is_empty(_task_queue_t *queue);

/// @brief Gets the remaining number of free task slots inside the queue.
static size_t           _task_queue_remaining_slots(_task_queue_t *queue);

/// @}


/// @name Task Management - Summary
/// Functions for creating and destroying tasks.
/// @{

struct ctp_task
{
    void (*function)(void *);
    void *arg;

    pthread_mutex_t mutex;
    pthread_cond_t cond;

    atomic_uint remaining;
    atomic_uint ref_count;
};

/// @brief Releases a reference to a task destroying it when the reference count reaches 0.
static void     _ctp_task_release(ctp_pool_t* pool, ctp_task_t *task);

/// @}


/// @name Thread Pool - Summary
/// Functions for creating and destroying thread pools.
/// @{

typedef struct ctp_pool
{
    size_t num_workers;
    _worker *workers;

    _task_queue_t *queue;
    _task_queue_t *active;

    bool stopping;
    pthread_mutex_t mutex;
    pthread_cond_t cond;

} ctp_pool_t;

/// @}


/// @name Thread Pool - Implementation
/// Functions for creating and destroying thread pools.
/// @{

ctp_pool_t *ctp_pool_create(size_t num_workers, size_t queue_size)
{
    ctp_pool_t *pool = malloc(sizeof(*pool));
    if (!pool)
        return NULL;

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->cond, NULL);

    pool->num_workers = num_workers;
    pool->stopping = false;

    pool->queue = _task_queue_create(queue_size);
    pool->active = _task_queue_create(queue_size);
    if (!pool->queue || ! pool->active)
    {
        free(pool->queue);
        free(pool->active);
        free(pool);
        return NULL;
    }

    pool->workers = malloc(sizeof(_worker) * num_workers);
    if (!pool->workers)
    {
        _task_queue_destroy(pool->queue);
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < num_workers; ++i)
    {
        if (!_worker_init(pool->workers + i, pool))
        {
            for (size_t j = 0; j < i; ++j)
                pthread_join(pool->workers[j].thread, NULL);
            _task_queue_destroy(pool->queue);
            _task_queue_destroy(pool->active);
            free(pool->workers);
            free(pool);
            return NULL;
        }
    }

    return pool;
}

int ctp_submit_task(ctp_pool_t *pool, ctp_task_t* task)
{
    assert(pool != NULL && task != NULL);

    pthread_mutex_lock(&pool->mutex);
    if (_task_queue_full(pool->queue) || _task_queue_full(pool->active))
        return -1;
    if (_task_queue_push(pool->queue, task) != 0)
    {
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }
    atomic_fetch_add(&task->remaining, 1);
    pthread_cond_signal(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);

    return 0;
}

int ctp_submit_task_n(ctp_pool_t* pool, ctp_task_t* task, size_t n)
{
    assert(pool != NULL && task != NULL);

    pthread_mutex_lock(&pool->mutex);
    if (n == 0 || _task_queue_remaining_slots(pool->queue) < n || _task_queue_full(pool->queue) || _task_queue_full(pool->active))
        return -1;

    for (size_t i = 0; i < n; ++i)
    {
        if (_task_queue_push(pool->queue, task) != 0)
        {
            pthread_mutex_unlock(&pool->mutex);
            return -1;
        }
        atomic_fetch_add(&task->remaining, 1);
    }
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
    return 0;
}


void ctp_wait_task(ctp_pool_t *pool, ctp_task_t *task)
{
    if (!pool || !task)
        return;

    pthread_mutex_lock(&pool->mutex);
    if (!_task_queue_search(pool->queue, task) && !_task_queue_search(pool->active, task))
    {
        pthread_mutex_unlock(&pool->mutex);
        return;
    }
    pthread_mutex_unlock(&pool->mutex);

    pthread_mutex_lock(&task->mutex);
    while (task->remaining != 0)
        pthread_cond_wait(&task->cond, &task->mutex);
    pthread_mutex_unlock(&task->mutex);
    _ctp_task_release(pool, task);
}
void ctp_wait_all(ctp_pool_t* pool)
{
    if (!pool)
        return;

    while (true)
    {
        ctp_task_t* next_active = NULL;
        ctp_task_t* next_pending = NULL;

        pthread_mutex_lock(&pool->mutex);
        next_active = pool->active->tasks[pool->active->current - pool->active->current != 0];
        next_pending = pool->queue->tasks[pool->queue->current - pool->active->current != 0];
        pthread_mutex_unlock(&pool->mutex);

        if (!next_active && !next_pending)
            break;

        ctp_wait_task(pool, next_active);
        ctp_wait_task(pool, next_pending);
    }
}


void ctp_pool_destroy(ctp_pool_t *pool)
{
    if (!pool)
        return;

    ctp_wait_all(pool);

    pthread_mutex_lock(&pool->mutex);
    pool->stopping = true;
    pthread_cond_broadcast(&pool->cond);
    pthread_mutex_unlock(&pool->mutex);
    for (size_t i = 0; i < pool->num_workers; ++i)
        pthread_join(pool->workers[i].thread, NULL);

    _task_queue_destroy(pool->queue);
    _task_queue_destroy(pool->active);
    free(pool->workers);
    free(pool);
}

/// @}


/// @name Task Management - Implementation
/// Functions for creating and destroying tasks.
/// @{

ctp_task_t *ctp_task_create(void (*function)(void *), void *arg)
{
    assert(function != NULL);

    ctp_task_t *task = malloc(sizeof(*task));
    if (!task)
        return NULL;

    task->arg = arg;
    task->function = function;
    task->remaining = 0;

    pthread_mutex_init(&task->mutex, NULL);
    pthread_cond_init(&task->cond, NULL);

    task->ref_count = 1;

    return task;
}

static void _ctp_task_release(ctp_pool_t* pool, ctp_task_t *task)
{
    if (!task)
        return;

    if (atomic_fetch_sub(&task->ref_count, 1) == 1)
    {
        pthread_mutex_lock(&pool->mutex);
        for (size_t i = 0; i < pool->active->size; i++)
        {
            if (pool->active->tasks[i] == task)
                pool->active->tasks[i] = NULL;
        }
        ctp_task_destroy(task);
        pthread_mutex_unlock(&pool->mutex);
    }
}

void ctp_task_destroy(ctp_task_t *task)
{
    if (!task)
        return;

    pthread_mutex_destroy(&task->mutex);
    pthread_cond_destroy(&task->cond);
    free(task);
}

/// @}


/// @name Task Queue - Implementation
/// Functions for initializing and managing the task queue.
/// @{

static _task_queue_t* _task_queue_create(size_t size)
{
    _task_queue_t *queue = calloc(1, sizeof(*queue) + size * sizeof(ctp_task_t));
    if (!queue)
        return NULL;

    queue->size = size;
    return queue;
}

static int _task_queue_push(_task_queue_t *queue, ctp_task_t *task)
{
    assert(queue != NULL && task != NULL);

    if (queue->current >= queue->size)
        return -1; // Queue is full

    queue->tasks[queue->current++] = task;

    atomic_fetch_add(&task->ref_count, 1);

    return 0;
}

static ctp_task_t* _task_queue_pop(_task_queue_t *queue)
{
    assert(queue != NULL);

    if (queue->current == 0)
        return NULL;

    ctp_task_t *current_task = queue->tasks[--queue->current];
    queue->tasks[queue->current] = NULL;

    return current_task;
}

static void _task_queue_destroy(_task_queue_t *queue)
{
    if (!queue)
        return;

    for (size_t i = 0; i < queue->current; ++i)
        ctp_task_destroy(queue->tasks[i]);

    free(queue);
}
static bool _task_queue_is_empty(_task_queue_t *queue)
{
    assert(queue != NULL);

    return queue->current == 0;
}

static bool _task_queue_full(_task_queue_t *queue)
{
    return queue->current >= queue->size;
}
static ctp_task_t* _task_queue_search(_task_queue_t *queue, ctp_task_t* task)
{
    for (size_t i = 0; i < queue->size; ++i)
    {
        if (queue->tasks[i] == task)
            return queue->tasks[i];
    }
    return NULL;
}
static size_t _task_queue_remaining_slots(_task_queue_t *queue)
{
    return queue->size - queue->current;
}

/// @}


/// @name Worker Thread - Implementation
/// Functions for initializing thread workers and the worker routine.
/// @{

static _worker* _worker_init(_worker* worker, ctp_pool_t* pool)
{
    assert(pool);

    worker->pool = pool;

    if (pthread_create(&worker->thread, NULL, _worker_routine, worker) != 0)
        return NULL;

    return worker;
}

static void* _worker_routine(void* arg)
{
    _worker* const worker = (_worker *)arg;
    ctp_pool_t* const pool = worker->pool;

    while (true)
    {
        pthread_mutex_lock(&pool->mutex);
        while (_task_queue_is_empty(pool->queue) && !pool->stopping)
            pthread_cond_wait(&pool->cond, &pool->mutex);

        if (pool->stopping && _task_queue_is_empty(pool->queue))
        {
            pthread_mutex_unlock(&pool->mutex);
            break;
        }

        if (_task_queue_full(pool->active))
        {
            pthread_mutex_unlock(&pool->mutex);
            continue;
        }
        
        ctp_task_t* task = _task_queue_pop(pool->queue);
        assert(task != NULL);
        _task_queue_push(pool->active, task);
        pthread_mutex_unlock(&pool->mutex);

        task->function(task->arg);

        pthread_mutex_lock(&task->mutex);
        if (atomic_fetch_sub(&task->remaining, 1) == 1)
            pthread_cond_signal(&task->cond);
        pthread_mutex_unlock(&task->mutex);

        _ctp_task_release(pool, task);
    }

    return NULL;
}

/// @}