#include "greatest.h"
#include "../ctp.h"

GREATEST_MAIN_DEFS();

SUITE(ctp_task_suite);


int main(int argc, char **argv)
{
    GREATEST_MAIN_BEGIN();

    RUN_SUITE(ctp_task_suite);

    GREATEST_MAIN_END();
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