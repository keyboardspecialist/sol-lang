#ifndef SOL_TEST_PROGRESS_H
#define SOL_TEST_PROGRESS_H

/* Test-only, single-threaded C17 reporting. Compiler output is never a channel. */
#include <stddef.h>

void sol_test_progress_begin(const char *name);
void sol_test_progress_case(const char *name);
void sol_test_progress_case_end(void);
void sol_test_progress_phase(const char *name, const char *unit, size_t total);
/* Call AFTER real work. A zero total means unknown, not an empty percentage. */
void sol_test_progress_completed(size_t completed);
void sol_test_progress_phase_end(void);
void sol_test_progress_operation_begin(const char *name);
void sol_test_progress_operation_at(const char *group, const char *name);
void sol_test_progress_operation_end(void);
void sol_test_progress_fixture(const char *name);
void sol_test_progress_ordinal(const char *kind, size_t ordinal, size_t total);
void sol_test_progress_hook(const char *kind, size_t after);
int sol_test_progress_finish(int status);

/* Preserve the exact result type and evaluate the original operation once. */
#define SOL_TEST_VALUE_HOOK(type, name) \
    static inline type sol_test_return_##name(type result) { \
        sol_test_progress_operation_end(); return result; \
    }
#define SOL_TEST_VALUE(name, ...) \
    (sol_test_progress_operation_at(__func__, #name), \
     sol_test_return_##name(name(__VA_ARGS__)))

/* Keep the original main (including early returns/cleanup) as sol_test_body. */
#define SOL_TEST_ENTRY(name) \
    int main(void) { sol_test_progress_begin(name); \
        return sol_test_progress_finish(sol_test_body()); }
#define SOL_TEST_ARGV_ENTRY(name) \
    int main(int argc, char **argv) { sol_test_progress_begin(name); \
        return sol_test_progress_finish(sol_test_body(argc, argv)); }

/* Does not inspect failures or short-circuit following cases. */
#define SOL_TEST_RUN(call) do { \
    sol_test_progress_case(#call); \
    call; \
    sol_test_progress_case_end(); \
} while (0)

#endif
