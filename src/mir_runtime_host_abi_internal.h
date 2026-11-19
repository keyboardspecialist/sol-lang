#ifndef SOL_MIR_RUNTIME_HOST_ABI_INTERNAL_H
#define SOL_MIR_RUNTIME_HOST_ABI_INTERNAL_H
#include "sol/mir_runtime_host_abi.h"

/* This meter is deliberately operation based.  A caller must tick before it
 * examines a queue item, follows an edge, compares a record, or emits one; a
 * rejected tick performs no persistent-owner allocation. */
typedef struct {
    size_t limit;
    size_t used;
} SolMirRuntimeHostAbiWorkMeter;
#ifdef SOL_MIR_PLAN_TEST_HOOKS
#include <stdio.h>
typedef struct {
    const char *name;
    const char *availability;
    const char *units;
    size_t value;
    bool value_available;
} SolMirRuntimeHostAbiObservationValue;

/* Temporary, test-hook-only observation state.  A build wrapper owns this
 * object on its stack; nested validator calls borrow it only for the duration
 * of the original call.  The bounded buffers are diagnostic storage, never
 * algorithm state. */
typedef struct {
    const char *first_failure_id;
    const char *first_failure_phase;
    const char *first_failure_branch;
    const char *first_failure_message;
    const char *producer_message;
    const char *propagation_id;
    const char *propagation_branch;
    const char *validator_rejection_id;
    const char *validator_rejection_message;
    const char *comparison_id;
    size_t comparison_left_index;
    size_t comparison_right_index;
    size_t comparison_length;
    int comparison_result;
    unsigned char comparison_left[4096];
    unsigned char comparison_right[4096];
    bool comparison_available;
    bool comparison_left_available;
    bool comparison_right_available;
    bool comparison_result_available;
    bool comparison_is_fieldwise;
    bool comparison_is_duplicate;
    const char *comparison_operation;
    bool comparison_equal_available;
    bool comparison_equal;
    const char *fieldwise_id;
    size_t fieldwise_index;
    size_t fieldwise_length;
    unsigned char fieldwise_bytes[4096];
    bool fieldwise_available;
    bool capture_gap;
    const char *capture_gap_reason;
    SolMirRuntimeHostAbiObservationValue values[96];
    size_t value_count;
    const char *measurement_scope;
    SolMirRuntimeHostAbiBuildOutcome measurement_outcome;
    const char *measurement_rejection_id;
    bool measurement_recorded;
    bool measurement_active;
    bool validator_active;
    const char *final_validation_scope;
    SolMirRuntimeHostAbiBuildOutcome final_validation_outcome;
    const char *final_validation_rejection_id;
    bool final_validation_recorded;
    SolMirRuntimeHostAbiBuildOutcome producer_outcome;
    const char *current_index_names[8];
    size_t current_index_values[8];
    bool current_index_available[8];
} SolMirRuntimeHostAbiObservationContext;

extern _Thread_local SolMirRuntimeHostAbiObservationContext
    *sol_mir_runtime_host_abi_observation_context;
void sol_mir_runtime_host_abi_observe_failure(const char *, const char *,
    const char *, const char *);
void sol_mir_runtime_host_abi_observe_outcome_message(const char *);
void sol_mir_runtime_host_abi_observe_shared_failure(const char *, const char *,
    const char *, const char *, const char *, const char *);
void sol_mir_runtime_host_abi_observe_propagation(const char *, const char *);
void sol_mir_runtime_host_abi_observe_validator_rejection(const char *,
    const char *);
bool sol_mir_runtime_host_abi_observe_check(const char *, bool);
bool sol_mir_runtime_host_abi_observe_reject(const char *, bool);
void sol_mir_runtime_host_abi_observe_value(const char *, const char *,
    const char *, size_t);
void sol_mir_runtime_host_abi_observe_unavailable_value(const char *,
    const char *, const char *);
void sol_mir_runtime_host_abi_observe_index(size_t, const char *, size_t);
void sol_mir_runtime_host_abi_observe_meter_failure(
    const SolMirRuntimeHostAbiWorkMeter *, const char *, bool, bool);
void sol_mir_runtime_host_abi_observe_validator_result(const char *,
    SolMirRuntimeHostAbiBuildOutcome);
int sol_mir_runtime_host_abi_observe_comparison(const char *, const void *,
    const void *, size_t, int, size_t, size_t, bool);
int sol_mir_runtime_host_abi_observe_compare_once(const char *, const void *,
    const void *, size_t, size_t, size_t, bool);
int sol_mir_runtime_host_abi_observe_compare_once_shared(const char *,
    const char *, const void *, const void *, size_t, size_t, size_t, bool);
void sol_mir_runtime_host_abi_observe_capability_comparison(const char *,
    const void *, const void *, size_t, size_t, size_t, bool);
void sol_mir_runtime_host_abi_test_emit_comparison(FILE *,
    const SolMirRuntimeHostAbiObservationContext *);
/* Both non-null operands must point to readable, properly aligned capability
 * rows for the duration of the call.  Null operands are rejected. */
bool sol_mir_runtime_host_abi_test_capability_fields_equal(
    const SolMirRuntimeHostCapabilityPlan *,
    const SolMirRuntimeHostCapabilityPlan *);
typedef enum {
    SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_SUCCEEDED,
    SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_REJECTED,
    SOL_MIR_RUNTIME_HOST_ABI_LINEAGE_TEST_EXHAUSTED
} SolMirRuntimeHostAbiLineageTestOutcome;
SolMirRuntimeHostAbiLineageTestOutcome
sol_mir_runtime_host_abi_test_resolve_base_parent(
    const SolMirRuntimeHostAbi *, size_t, size_t, size_t *, size_t *, size_t *);
SolMirRuntimeHostAbiLineageTestOutcome
sol_mir_runtime_host_abi_test_validate_base_parent(
    const SolMirRuntimeHostAbi *, size_t, size_t, size_t *, size_t *, size_t *);
bool sol_mir_runtime_host_abi_observe_ne(const char *, const char *, size_t,
    const char *, size_t);
bool sol_mir_runtime_host_abi_observe_eq(const char *, const char *, size_t,
    const char *, size_t);
bool sol_mir_runtime_host_abi_observe_ge(const char *, const char *, size_t,
    const char *, size_t);
bool sol_mir_runtime_host_abi_observe_gt(const char *, const char *, size_t,
    const char *, size_t);
bool sol_mir_runtime_host_abi_observe_le(const char *, const char *, size_t,
    const char *, size_t);
void sol_mir_runtime_host_abi_observe_fieldwise_row(const char *, const void *,
    size_t, size_t);
void sol_mir_runtime_host_abi_observe_capture_gap(const char *);
#endif
static inline bool sol_mir_runtime_host_abi_work_tick(
    SolMirRuntimeHostAbiWorkMeter *meter) {
#ifdef SOL_MIR_PLAN_TEST_HOOKS
    if (!meter) {
        sol_mir_runtime_host_abi_observe_meter_failure(NULL,
            "H37.P.sol_mir_runtime_host_abi_work_tick.14.meter_null",false,false);
        return false;
    }
    if (meter->used == SIZE_MAX) {
        sol_mir_runtime_host_abi_observe_meter_failure(meter,
            "H37.P.sol_mir_runtime_host_abi_work_tick.14.used_overflow",true,false);
        return false;
    }
    if (meter->used >= meter->limit) {
        sol_mir_runtime_host_abi_observe_meter_failure(meter,
            "H37.P.sol_mir_runtime_host_abi_work_tick.14.limit",true,true);
        return false;
    }
#else
    if (!meter || meter->used == SIZE_MAX || meter->used >= meter->limit)
        return false;
#endif
    ++meter->used;
    return true;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
extern _Thread_local size_t sol_mir_runtime_host_abi_internal_allocation_attempts;
extern _Thread_local SolMirRuntimeHostAbiWorkCensus
    sol_mir_runtime_host_abi_internal_work_census;
#endif
SolMirRuntimeHostAbiBuildOutcome sol_mir_runtime_host_abi_internal_validate(
    const SolMirRuntimeHostAbi *, SolDiagnostics *);
bool sol_mir_runtime_host_abi_internal_measure_validation_work(
    const SolMirRuntimeHostAbi *, size_t *);
#ifdef SOL_MIR_PLAN_TEST_HOOKS
void sol_mir_runtime_host_abi_internal_record_validation_work(size_t audit,
    size_t validation);
#endif
#endif
