#include "greatest.h"
#include "../ctp.h"

#include <stdio.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>

static uint64_t monotonic_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);

    return (uint64_t)ts.tv_sec * 1000000000UL + (uint64_t)ts.tv_nsec;
}

GREATEST_MAIN_DEFS();

SUITE(ctp_task_suite);
SUITE(ctp_pool_suite);


int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();

    RUN_SUITE(ctp_task_suite);
    RUN_SUITE(ctp_pool_suite);

    GREATEST_MAIN_END();
}

TEST test_ctp_pool_create_destroy(void);
TEST test_ctp_pool_basic(void);
TEST test_ctp_pool_concurency(void);
SUITE(ctp_pool_suite)
{
    RUN_TEST(test_ctp_pool_create_destroy);
    RUN_TEST(test_ctp_pool_basic);
    RUN_TEST(test_ctp_pool_concurency);
}

static void incrementor_task(void *arg)
{
    atomic_int *counter = arg;
    (*counter)++;
}

TEST test_ctp_pool_basic(void)
{
    ctp_pool_t* pool = ctp_pool_create(2, 10); ASSERT(pool != NULL);

    atomic_int counter = 0;
    ctp_task_t* task = ctp_task_create(incrementor_task, &counter); ASSERT(task != NULL);

    ASSERT_EQ(0, ctp_submit_task(pool, task));
    ASSERT_EQ(0, ctp_submit_task(pool, ctp_task_create(incrementor_task, &counter)));


    ctp_wait_task(pool, task);
    ctp_pool_destroy(pool);

    ASSERT_EQ(2, counter);
    PASS();
}

static void sleep_task(void *arg)
{
    const size_t usecs = *(size_t*)arg;

    usleep(usecs);
}
TEST test_ctp_pool_concurency(void)
{
    const size_t task_count = 64;
    const size_t workers = task_count;
    const size_t sleep_us = 1000;

    ctp_pool_t* pool = ctp_pool_create(workers, task_count);

    const size_t batches = (task_count + workers - 1) / workers;
    const size_t expected_ns = batches * sleep_us * 1000;
    const size_t slack_ns = expected_ns * 1.20;

    const uint64_t start = monotonic_ns();
    ctp_task_t* task = ctp_task_create(sleep_task, (size_t*)&sleep_us);
    ASSERT_EQ(0, ctp_submit_task_n(pool, task, task_count));
    const uint64_t end = monotonic_ns();


    ctp_wait_all(pool);
    const uint64_t elapsed = end - start;

    printf("elapsed: %zu, maximum: %zu", elapsed, slack_ns);

    //ASSERT_LTE(elapsed, slack_ns);

    ctp_pool_destroy(pool);
    PASS();
}

TEST test_ctp_pool_create_destroy(void)
{
    ctp_pool_t* pool = ctp_pool_create(2, 10);
    ASSERT(pool != NULL);

    ctp_pool_destroy(pool);

    PASS();
}


TEST test_ctp_task_create_destroy(void);
SUITE(ctp_task_suite)
{
    RUN_TEST(test_ctp_task_create_destroy);
}

TEST test_ctp_task_create_destroy(void)
{
    ctp_task_t* task = ctp_task_create((void (*)(void *))1, NULL);
    ASSERT(task != NULL);

    ctp_task_destroy(task);

    PASS();
}