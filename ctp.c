#include "ctp.h"

#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>


typedef struct _task_queue _task_queue_t;
static _task_queue_t* _task_queue_create(size_t size);
static void _task_queue_destroy(_task_queue_t *queue);
static int _task_queue_push(_task_queue_t *queue, ctp_task_t *task);
static ctp_task_t* _task_queue_pop(_task_queue_t *queue);
static bool _task_queue_is_empty(_task_queue_t *queue);
static bool _task_queue_full(_task_queue_t *queue);
static ctp_task_t* _task_queue_search(_task_queue_t *queue, ctp_task_t* task);
static size_t _task_queue_remaining_slots(_task_queue_t *queue);


typedef struct
{
    pthread_t thread;
    ctp_task_t *task;
    ctp_pool_t *pool;
} _worker;

typedef struct _task_queue
{
    ctp_task_t **tasks;
    size_t size;
    size_t current;

    bool has_work;
} _task_queue_t;


struct ctp_task
{
    void (*function)(void *);
    void *arg;

    pthread_mutex_t mutex;
    pthread_cond_t task_completed_cond;

    atomic_uint remaining;
    atomic_uint ref_count;
};
static void _ctp_task_release(ctp_pool_t* pool, ctp_task_t *task);

typedef struct ctp_pool
{
    size_t num_workers;
    _worker *workers;

    _task_queue_t *queue;
    _task_queue_t *active;

    bool stopping;
    pthread_mutex_t mutex;
    pthread_cond_t task_cond;

} ctp_pool_t;


static void* _routine(void* arg)
{
    _worker* const worker = (_worker *)arg;

    while (true)
    {
        pthread_mutex_lock(&worker->pool->mutex);
        while (_task_queue_is_empty(worker->pool->queue) && !worker->pool->stopping)
            pthread_cond_wait(&worker->pool->task_cond, &worker->pool->mutex);

        if (worker->pool->stopping && _task_queue_is_empty(worker->pool->queue))
        {
            pthread_mutex_unlock(&worker->pool->mutex);
            break;
        }

        if (_task_queue_full(worker->pool->active))
        {
            pthread_mutex_unlock(&worker->pool->mutex);
            continue;
        }
        
        worker->task = _task_queue_pop(worker->pool->queue);
        assert(worker->task != NULL);
        _task_queue_push(worker->pool->active, worker->task);
        pthread_mutex_unlock(&worker->pool->mutex);

        worker->task->function(worker->task->arg);

        pthread_mutex_lock(&worker->task->mutex);
        if (atomic_fetch_sub(&worker->task->remaining, 1) == 1)
            pthread_cond_signal(&worker->task->task_completed_cond);
        pthread_mutex_unlock(&worker->task->mutex);
        _ctp_task_release(worker->pool, worker->task);
        worker->task = NULL;
    }

    return NULL;
}


ctp_pool_t *ctp_pool_create(size_t num_workers, size_t queue_size)
{
    ctp_pool_t *pool = malloc(sizeof(*pool));
    if (!pool)
        return NULL;

    pthread_mutex_init(&pool->mutex, NULL);
    pthread_cond_init(&pool->task_cond, NULL);

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
        pool->workers[i].pool = pool;
        pool->workers[i].task = NULL;

        if (pthread_create(&pool->workers[i].thread, NULL, _routine, &pool->workers[i]) != 0)
        {
            for (size_t j = 0; j < i; ++j)
                pthread_join(pool->workers[j].thread, NULL);
            _task_queue_destroy(pool->queue);
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
    pthread_cond_signal(&pool->task_cond);
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
    pthread_cond_broadcast(&pool->task_cond);
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
        pthread_cond_wait(&task->task_completed_cond, &task->mutex);
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
    pthread_cond_broadcast(&pool->task_cond);
    pthread_mutex_unlock(&pool->mutex);
    for (size_t i = 0; i < pool->num_workers; ++i)
        pthread_join(pool->workers[i].thread, NULL);

    _task_queue_destroy(pool->queue);
    _task_queue_destroy(pool->active);
    free(pool->workers);
    free(pool);
}

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
    pthread_cond_init(&task->task_completed_cond, NULL);

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
    pthread_cond_destroy(&task->task_completed_cond);
    free(task);
}


static int _task_queue_push(_task_queue_t *queue, ctp_task_t *task)
{
    assert(queue != NULL && task != NULL);

    if (queue->current >= queue->size)
        return -1; // Queue is full

    queue->tasks[queue->current++] = task;
    queue->has_work = true;

    atomic_fetch_add(&task->ref_count, 1);

    return 0;
}

static ctp_task_t* _task_queue_pop(_task_queue_t *queue)
{
    assert(queue != NULL);

    if (queue->current == 0 && !queue->has_work)
        return NULL;

    ctp_task_t *current_task = queue->tasks[--queue->current];

    if (queue->current == 0)
        queue->has_work = false;

    queue->tasks[queue->current] = NULL;

    return current_task;
}

static _task_queue_t* _task_queue_create(size_t size)
{
    _task_queue_t *queue = malloc(sizeof(*queue));
    if (!queue)
        return NULL;

    queue->tasks = calloc(size, sizeof(ctp_task_t *));
    if (!queue->tasks)
    {
        free(queue);
        return NULL;
    }

    queue->size = size;
    queue->current = 0;
    queue->has_work = false;

    return queue;
}

static void _task_queue_destroy(_task_queue_t *queue)
{
    if (!queue)
        return;

    for (size_t i = 0; i < queue->current; ++i)
        ctp_task_destroy(queue->tasks[i]);

    free(queue->tasks);
    free(queue);
}
static bool _task_queue_is_empty(_task_queue_t *queue)
{
    assert(queue != NULL);

    bool is_empty = (queue->current == 0 && !queue->has_work);

    return is_empty;
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
