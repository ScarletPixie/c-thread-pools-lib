#ifndef CTP_H
#define CTP_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>

/// @name Task Management
/// Functions for creating and destroying tasks.
/// @{

typedef struct ctp_task ctp_task_t;

/// @brief Create a new task for execution.
/// @param function The function to be executed.
/// @param arg The argument to be passed to the function.
/// @return A pointer to the newly created task, or NULL if creation failed.
ctp_task_t *ctp_task_create(void (*function)(void *), void *arg);

/// @brief Destroy a task and free its resources.
/// @param task A pointer to the task to be destroyed.
/// @note Should not be called on a submitted task.
void ctp_task_destroy(ctp_task_t *task);

/// @}


/// @name Thread Pool
/// Functions for creating and destroying thread pools.
/// @{

typedef struct ctp_pool ctp_pool_t;

///@brief Create a new thread pool with the specified number of worker threads.
///@param num_workers The number of worker threads to create in the pool.
///@param queue_size The size of the task queue.
///@return A pointer to the newly created thread pool, or NULL if creation failed.
ctp_pool_t *ctp_pool_create(size_t num_workers, size_t queue_size);

///@brief Submit a task to the thread pool for execution.
///@param pool A pointer to the thread pool.
void ctp_pool_destroy(ctp_pool_t *pool);

/// @}


/// @name Pool Tasks
/// Functions for managing tasks inside the pool.
/// @{

///@brief Submit a task to the thread pool for execution.
///@param pool A pointer to the thread pool.
///@param task A pointer to the task to be submitted.
///@return 0 if the task was successfully submitted, or a non-zero value if submission failed.
int ctp_submit_task(ctp_pool_t *pool, ctp_task_t* task);

/// @brief Wait for a task to complete.
/// @param pool A pointer to the thread pool.
/// @param task A pointer to the task to wait for.
void ctp_wait_task(ctp_pool_t *pool, ctp_task_t *task);

/// @}


#ifdef __cplusplus
}
#endif

#endif