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
    bool completed;

    atomic_uint ref_count;
};
static void _ctp_task_release(ctp_task_t *task);

typedef struct ctp_pool
{
    size_t num_workers;
    _worker *threads;
    _task_queue_t *queue;

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

        worker->task = _task_queue_pop(worker->pool->queue);
        pthread_mutex_unlock(&worker->pool->mutex);

        assert(worker->task != NULL);

        worker->task->function(worker->task->arg);

        pthread_mutex_lock(&worker->task->mutex);
        worker->task->completed = true;
        pthread_cond_signal(&worker->task->task_completed_cond);
        pthread_mutex_unlock(&worker->task->mutex);
        _ctp_task_release(worker->task);
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
    if (!pool->queue)
    {
        free(pool);
        return NULL;
    }

    pool->threads = malloc(sizeof(_worker) * num_workers);
    if (!pool->threads)
    {
        _task_queue_destroy(pool->queue);
        free(pool);
        return NULL;
    }

    for (size_t i = 0; i < num_workers; ++i)
    {
        pool->threads[i].pool = pool;
        pool->threads[i].task = NULL;

        if (pthread_create(&pool->threads[i].thread, NULL, _routine, &pool->threads[i]) != 0)
        {
            for (size_t j = 0; j < i; ++j)
                pthread_join(pool->threads[j].thread, NULL);
            _task_queue_destroy(pool->queue);
            free(pool->threads);
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
    if (_task_queue_push(pool->queue, task) != 0)
    {
        pthread_mutex_unlock(&pool->mutex);
        return -1;
    }
    pthread_cond_signal(&pool->task_cond);
    pthread_mutex_unlock(&pool->mutex);

    return 0;
}

void ctp_wait_task(ctp_pool_t *pool, ctp_task_t *task)
{
    if (!pool || !task)
        return;

    ctp_task_t** task_slot = NULL;
    for (size_t i = 0; i < pool->queue->size; ++i)
    {
        if (pool->queue->tasks[i] == task)
        {
            task_slot = &pool->queue->tasks[i];
            break;
        }
    }
    if (!task_slot)
        return;


    pthread_mutex_lock(&task->mutex);
    while (!task->completed)
        pthread_cond_wait(&task->task_completed_cond, &task->mutex);
    *task_slot = NULL;
    pthread_mutex_unlock(&task->mutex);
    _ctp_task_release(task);
}

void ctp_pool_destroy(ctp_pool_t *pool)
{
    if (!pool)
        return;

    pthread_mutex_lock(&pool->mutex);
    pool->stopping = true;
    pthread_cond_broadcast(&pool->task_cond);
    pthread_mutex_unlock(&pool->mutex);

    for (size_t i = 0; i < pool->queue->size; ++i)
    {
        if (pool->queue->tasks[i])
            ctp_wait_task(pool, pool->queue->tasks[i]);
    }

    for (size_t i = 0; i < pool->num_workers; ++i)
        pthread_join(pool->threads[i].thread, NULL);

    _task_queue_destroy(pool->queue);
    free(pool->threads);
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
    task->completed = false;

    pthread_mutex_init(&task->mutex, NULL);
    pthread_cond_init(&task->task_completed_cond, NULL);

    task->ref_count = 2; // One for the worker and one for the waiter

    return task;
}

static void _ctp_task_release(ctp_task_t *task)
{
    if (!task)
        return;

    if (task->ref_count-- == 0)
        ctp_task_destroy(task);
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

    return 0;
}

static ctp_task_t* _task_queue_pop(_task_queue_t *queue)
{
    assert(queue != NULL);

    if (queue->current == 0 && !queue->has_work)
        return NULL; // Queue is empty

    ctp_task_t *current_task = queue->tasks[--queue->current];

    if (queue->current == 0)
        queue->has_work = false;

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