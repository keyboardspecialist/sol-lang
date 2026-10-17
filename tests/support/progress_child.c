#include "test_progress.h"

#include <stdio.h>
#include <string.h>

static int failures;
static int calls;
static int one_step(int value) { ++calls; return value; }
SOL_TEST_VALUE_HOOK(int, one_step)

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "check failed: %s\n", #condition); \
        ++failures; \
    } \
} while (0)

static void following_case(void) {
    CHECK(1);
    fputs("FOLLOWING_CASE_EXECUTED\n", stdout);
    fflush(stdout);
}

int main(int argc, char **argv) {
    sol_test_progress_begin("support-continuation");
    sol_test_progress_case("intentional-check");
    CHECK(!(argc > 1 && strcmp(argv[1], "--fail") == 0));
    sol_test_progress_completed(1);
    sol_test_progress_phase_end();
    if (argc > 1 && strcmp(argv[1], "--operations") == 0) {
        sol_test_progress_fixture("fixture with spaces");
        sol_test_progress_phase("mutate-restore", "mutation", 2);
        sol_test_progress_ordinal("allocation", 3, 7);
        CHECK(SOL_TEST_VALUE(one_step, -7) == -7 && calls == 1);
        sol_test_progress_completed(1);
        CHECK(SOL_TEST_VALUE(one_step, 9) == 9 && calls == 2);
        sol_test_progress_completed(2);
        sol_test_progress_phase_end();
    }
    if (argc > 1 && strcmp(argv[1], "--wait") == 0) {
        sol_test_progress_phase("wait-for-input", "input", 1);
        (void)getchar();
        sol_test_progress_completed(1);
        sol_test_progress_phase_end();
    }
    SOL_TEST_RUN(following_case());
    return sol_test_progress_finish(failures != 0);
}
