# c-thread-pools-lib

Minimal C thread-pool library built on pthreads. It provides a small task queue, worker threads, and simple wait APIs for running work asynchronously.

## Build

```sh
make
make test
```

## Usage

```c
#include "ctp.h"
#include <stdatomic.h>
#include <stdio.h>

static void inc_task(void *arg)
{
    atomic_int *counter = arg;
    (*counter)++;
}

int main(void)
{
    ctp_pool_t *pool = ctp_pool_create(4, 32);
    if (!pool)
        return 1;

    atomic_int counter = 0;
    ctp_task_t *task = ctp_task_create(inc_task, &counter);
    if (!task)
        return 1;

    if (ctp_submit_task(pool, task) != 0)
        return 1;

    ctp_wait_task(pool, task);   // task is released after completion
    ctp_pool_destroy(pool);

    printf("counter=%d\n", counter);
    return 0;
}
```

## Common API

- `ctp_pool_create(num_workers, queue_size)`
- `ctp_submit_task(pool, task)`
- `ctp_submit_task_n(pool, task, n)`
- `ctp_wait_task(pool, task)`
- `ctp_wait_all(pool)`
- `ctp_pool_destroy(pool)`

## Notes

- Tasks are created with `ctp_task_create(function, arg)`.
- `ctp_wait_task()` frees the task after it finishes.
- `ctp_submit_task_n()` submits the same task multiple times in one call.
