#ifndef SOL_IR_TEST_INTERNAL_H
#define SOL_IR_TEST_INTERNAL_H

#ifdef SOL_MIR_PLAN_TEST_HOOKS

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const void *base;
    size_t bytes;
} SolIrTestAllocationRequest;

/* Caller-owned, bounded observation of successful requests in the IR lowering
 * allocation funnels. This is not an allocation registry and retains no owner. */
typedef struct {
    SolIrTestAllocationRequest *requests;
    size_t capacity;
    size_t count;
    bool active;
    bool overflow;
    bool invalid;
} SolIrTestAllocationCapture;

bool sol_ir_test_allocation_capture_begin(SolIrTestAllocationCapture *capture,
    SolIrTestAllocationRequest *requests, size_t capacity);
void sol_ir_test_allocation_capture_end(SolIrTestAllocationCapture *capture);

#endif

#endif
