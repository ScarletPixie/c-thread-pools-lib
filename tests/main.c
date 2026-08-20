#include "greatest.h"
#include "../ctp.h"

#include <stdatomic.h>

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
SUITE(ctp_pool_suite)
{
    RUN_TEST(test_ctp_pool_create_destroy);
    RUN_TEST(test_ctp_pool_basic);
}

static void incrementor_task(void *arg)
{
    atomic_int *counter = arg;
    (*counter)++;
}
TEST test_ctp_pool_basic(void)
{
    ctp_pool_t* pool = ctp_pool_create(2, 2); ASSERT(pool != NULL);

    atomic_int counter = 0;
    ctp_task_t* task = ctp_task_create(incrementor_task, &counter); ASSERT(task != NULL);

    ASSERT_EQ(0, ctp_submit_task(pool, task));


    ctp_pool_destroy(pool);


    ASSERT_EQ(1, counter);
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