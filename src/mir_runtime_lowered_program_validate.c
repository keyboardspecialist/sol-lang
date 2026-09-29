#include "sol/mir_runtime_lowered_program.h"
#include "mir_runtime_lowered_program_internal.h"
#include "mir_runtime_arena_internal.h"

#include <stdlib.h>
#include <string.h>

/* Validation has its own meter: construction may invoke the validator to
 * measure validation work, but that replay must not perturb the approved
 * builder meter.  The scope is installed only after the raw owner pointers
 * have been checked and is restored by the single exit path below. */
static _Thread_local SolMirRuntimeLoweredWorkMeter *active_validation_meter;
static bool validation_tick(void) {
    return !active_validation_meter
        || sol_mir_runtime_lowered_tick(active_validation_meter);
}

static bool bad(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics) sol_diagnostics_add(diagnostics, "SOL-MIR-RUNTIME-LOWERED-002",
        SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}
static bool add_size(size_t *left, size_t right) {
    if (!validation_tick()) return false;
    if (right > SIZE_MAX - *left) return false;
    *left += right;
    return true;
}
static bool mul_size(size_t left, size_t right, size_t *out) {
    if (!validation_tick()) return false;
    if (left && right > SIZE_MAX / left) return false;
    *out = left * right;
    return true;
}
static bool present(SolMirRuntimeLoweredState state) {
    if (!validation_tick()) return false;
    return state == SOL_MIR_RUNTIME_LOWERED_PRESENT;
}
static SolMirRuntimeLoweredExecution execution_for(SolMirRuntimeLoweredRuntimeClass runtime_class) {
    return runtime_class == SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE
        ? SOL_MIR_RUNTIME_LOWERED_EXECUTABLE
        : SOL_MIR_RUNTIME_LOWERED_CONTROL_OR_MARKER;
}
#define VD(runtime_class, family, mask) do { if (out) *out = (SolMirRuntimeLoweredDemandDescriptor){ runtime_class, family, mask }; return true; } while (0)
/* This switch is deliberately independent of the builder's descriptor tables. */
static bool image_instruction_descriptor(SolMirInstructionKind kind, SolMirRuntimeLoweredDemandDescriptor *out) {
    switch (kind) {
    case SOL_MIR_INST_CONST_INT64: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE | SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_INST_CONST_BOOL: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE | SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_INST_CONST_TEXT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE | SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION);
    case SOL_MIR_INST_CONST_UNIT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_INST_LOAD_COPY: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY | SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_INST_LOAD_MOVE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP | SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_INST_LOAD_UPDATE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_INST_STORE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP);
    case SOL_MIR_INST_UNARY: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_INST_BINARY: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT);
    case SOL_MIR_INST_COMPOUND_UPDATE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_INST_PATTERN_TEST: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_INST_PATTERN_VALUE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_INST_CONSTRUCT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT, SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE | SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_INST_CAPTURE_SNAPSHOT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_SNAPSHOT, SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY | SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_INST_PARAMETER_LIVE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_INST_STORAGE_LIVE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_INST_DROP_IF_INITIALIZED: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL, SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP);
    case SOL_MIR_INST_STORAGE_DEAD: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_INST_REGION_ENTER: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, 0);
    case SOL_MIR_INST_REGION_EXIT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL, SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_INST_TEMPORARY_INIT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, 0);
    case SOL_MIR_INST_TEMPORARY_DROP: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL, SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP);
    case SOL_MIR_INST_EXPRESSION_RESULT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, 0);
    case SOL_MIR_INST_MATCH_ARM: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, 0);
    case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL, SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP);
    case SOL_MIR_INST_HANDLER_ENTER: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER, SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER, SOL_MIR_RUNTIME_LOWERED_FACILITY_HANDLER_FRAME);
    case SOL_MIR_INST_HANDLER_EXIT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER, SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER, SOL_MIR_RUNTIME_LOWERED_FACILITY_HANDLER_FRAME | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_INST_SCOPE_ENTER: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, 0);
    case SOL_MIR_INST_SCOPE_EXIT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL, SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    }
    return false;
}
static bool image_terminator_descriptor(SolMirTerminatorKind kind, SolMirRuntimeLoweredDemandDescriptor *out) {
    switch (kind) {
    case SOL_MIR_TERM_GOTO: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, 0);
    case SOL_MIR_TERM_BRANCH: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, 0);
    case SOL_MIR_TERM_RETURN: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_TERM_PANIC: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE);
    case SOL_MIR_TERM_INVOKE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE, SOL_MIR_RUNTIME_LOWERED_FACILITY_CALL | SOL_MIR_RUNTIME_LOWERED_FACILITY_SIGNATURE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE);
    case SOL_MIR_TERM_RESUME_FAILURE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE);
    case SOL_MIR_TERM_UNREACHABLE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE);
    case SOL_MIR_TERM_BREAK: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, 0);
    case SOL_MIR_TERM_CONTINUE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, 0);
    case SOL_MIR_TERM_CHECK_REFINED: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE);
    case SOL_MIR_TERM_MATCH_FAILURE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE);
    case SOL_MIR_TERM_PROPAGATE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_TERM_CHECK_CONTRACT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_EVENT | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE);
    case SOL_MIR_TERM_CONTRACT_VIOLATION: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE);
    default: return false;
    }
}
static bool predicate_instruction_descriptor(SolMirPredicateInstructionKind kind, SolMirRuntimeLoweredDemandDescriptor *out) {
    switch (kind) {
    case SOL_MIR_PREDICATE_INST_I64: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_PREDICATE_INST_BOOL: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_PREDICATE_INST_TEXT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION);
    case SOL_MIR_PREDICATE_INST_UNIT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_PREDICATE_INST_UNARY: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_PREDICATE_INST_BINARY: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_PREDICATE_INST_PROJECT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_PREDICATE_INST_FUNCTION: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE);
    case SOL_MIR_PREDICATE_INST_BOUND_OPERATION: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE);
    case SOL_MIR_PREDICATE_INST_CONSTRUCT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE | SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION);
    case SOL_MIR_PREDICATE_INST_PATTERN_TEST: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    }
    return false;
}
static bool predicate_terminator_descriptor(SolMirPredicateTerminatorKind kind, SolMirRuntimeLoweredDemandDescriptor *out) {
    switch (kind) {
    case SOL_MIR_PREDICATE_TERM_RETURN: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_PREDICATE_TERM_JUMP: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, 0);
    case SOL_MIR_PREDICATE_TERM_BRANCH: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_CONTROL, SOL_MIR_RUNTIME_LOWERED_PLAN_CONTROL, 0);
    case SOL_MIR_PREDICATE_TERM_INVOKE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE, SOL_MIR_RUNTIME_LOWERED_FACILITY_CALL | SOL_MIR_RUNTIME_LOWERED_FACILITY_SIGNATURE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE);
    case SOL_MIR_PREDICATE_TERM_PROPAGATE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_PREDICATE_TERM_CHECK_REFINED: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE);
    case SOL_MIR_PREDICATE_TERM_FAILURE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CLEANUP, SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP | SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE);
    }
    return false;
}
static bool provenance_descriptor(SolMirOperationProvenanceKind kind, SolMirRuntimeLoweredDemandDescriptor *out) {
    switch (kind) {
    case SOL_MIR_OPERATION_PROVENANCE_CONSTRUCT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT, SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE | SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_OPERATION_PROVENANCE_PATTERN_TEST: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_OPERATION_PROVENANCE_PATTERN_EXTRACTION: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_OPERATION_PROVENANCE_ARITHMETIC: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_OPERATION_PROVENANCE_PROPAGATION: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_SNAPSHOT, SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY | SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_OPERATION_PROVENANCE_PREDICATE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    case SOL_MIR_OPERATION_PROVENANCE_HANDLER: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_MARKER, SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER, SOL_MIR_RUNTIME_LOWERED_FACILITY_HANDLER_FRAME);
    case SOL_MIR_OPERATION_PROVENANCE_CALLABLE: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE, SOL_MIR_RUNTIME_LOWERED_FACILITY_CALL | SOL_MIR_RUNTIME_LOWERED_FACILITY_SIGNATURE | SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_OPERATION_PROVENANCE_IMPORT_SNAPSHOT: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_IMPORT_SNAPSHOT, SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY | SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE);
    case SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY: VD(SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE, SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP);
    }
    return false;
}
#undef VD
static size_t image_for_block(const SolMirMaterialization *materialization, size_t block) {
    for (size_t image = 0; image < materialization->image_count && validation_tick(); ++image) {
        SolMirPlanSlice slice = materialization->images[image].blocks;
        if (block >= slice.offset && block - slice.offset < slice.count) return image;
    }
    return SOL_MIR_RUNTIME_NONE;
}
static size_t image_for_instruction(const SolMirMaterialization *materialization, size_t instruction) {
    for (size_t image = 0; image < materialization->image_count && validation_tick(); ++image) {
        SolMirPlanSlice slice = materialization->images[image].instructions;
        if (instruction >= slice.offset && instruction - slice.offset < slice.count) return image;
    }
    return SOL_MIR_RUNTIME_NONE;
}
static uint32_t value_facilities(const SolMirRuntimeValues *,size_t);static bool copy_requires_runtime(SolMirCopyKind);
static uint32_t image_instruction_facilities(const SolMirOperations *operations,const SolMirRuntimeValues *values,size_t instruction,SolMirInstructionKind kind,uint32_t facilities){if(kind==SOL_MIR_INST_BINARY||kind==SOL_MIR_INST_COMPOUND_UPDATE)for (size_t i=0;i<operations->arithmetic_count && validation_tick();i++)if(operations->arithmetic[i].instruction==instruction&&operations->arithmetic[i].failures)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE;if(kind==SOL_MIR_INST_CONSTRUCT)for (size_t i=0;i<operations->constructor_count && validation_tick();i++)if(operations->constructors[i].instruction==instruction)facilities|=value_facilities(values,operations->constructors[i].result_recipe)&(SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION|SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP);if(kind==SOL_MIR_INST_CAPTURE_SNAPSHOT){bool copy=false;for (size_t i=0;i<operations->snapshot_count && validation_tick();i++)if(operations->snapshots[i].instruction==instruction&&copy_requires_runtime(operations->snapshots[i].copy_kind))copy=true;if(!copy)facilities&=~(uint32_t)SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;}if(kind==SOL_MIR_INST_PATTERN_VALUE)for (size_t i=0;i<operations->pattern_extraction_count && validation_tick();i++)if(operations->pattern_extractions[i].instruction==instruction&&copy_requires_runtime(operations->pattern_extractions[i].copy_kind))facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;if(kind==SOL_MIR_INST_LOAD_COPY&&!copy_requires_runtime(operations->layout->representation->recipes[operations->layout->representation->materialization->instructions[instruction].type].copy_kind))facilities&=~(uint32_t)SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;return facilities;}
static size_t import_for_call(const SolMirRuntimeConventions *conventions, const SolMirRuntimeCall *call) { if (call->target_kind != SOL_MIR_RUNTIME_TARGET_DIRECT_HOST) return SOL_MIR_RUNTIME_NONE; for (size_t i = 0; i < conventions->import_count && validation_tick(); ++i) if (conventions->imports[i].kind == SOL_MIR_RUNTIME_IMPORT_HOST && conventions->imports[i].host == call->host) return i; return SOL_MIR_RUNTIME_NONE; }
static size_t bound_environment_import_for_call(const SolMirRuntimeConventions *conventions, const SolMirRuntimeCall *call) { if (call->target_kind != SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE || call->signature >= conventions->signature_count) return SOL_MIR_RUNTIME_NONE; SolMirRecipeId recipe = conventions->signatures[call->signature].function_recipe; if (recipe == SOL_MIR_RECIPE_NONE) return SOL_MIR_RUNTIME_NONE; for (size_t i = 0; i < conventions->import_count && validation_tick(); ++i) if (conventions->imports[i].kind == SOL_MIR_RUNTIME_IMPORT_RECIPE_BOUND_ENVIRONMENT && conventions->imports[i].recipe == recipe) return i; return SOL_MIR_RUNTIME_NONE; }
static size_t entry_for_call(const SolMirRuntimeConventions *conventions, const SolMirRuntimeCall *call) { if (call->target_kind != SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL) return SOL_MIR_RUNTIME_NONE; for (size_t i = 0; i < conventions->entry_count && validation_tick(); ++i) if (conventions->entries[i].callable == call->internal) return i; return SOL_MIR_RUNTIME_NONE; }
static size_t image_call_for_block(const SolMirRuntimeConventions *conventions, size_t image, size_t block) { for (size_t i = 0; i < conventions->call_count && validation_tick(); ++i) { const SolMirRuntimeCall *call = &conventions->calls[i]; if (call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_IMAGE && call->image == image && call->block == block) return i; } return SOL_MIR_RUNTIME_NONE; }
static size_t predicate_call_for_block(const SolMirRuntimeConventions *conventions, size_t body, size_t block) { for (size_t i = 0; i < conventions->call_count && validation_tick(); ++i) { const SolMirRuntimeCall *call = &conventions->calls[i]; if (call->owner_kind == SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE && call->predicate == body && call->block == block) return i; } return SOL_MIR_RUNTIME_NONE; }
static bool image_edge_source(const SolMirMaterialization *materialization, size_t edge, size_t *source, size_t *ordinal) { for (size_t block = 0; block < materialization->block_count && validation_tick(); ++block) { const SolMirMaterializedTerminator *term = &materialization->blocks[block].terminator; size_t candidates[3] = { SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE }; size_t count = 0; switch (term->kind) { case SOL_MIR_TERM_GOTO: case SOL_MIR_TERM_BREAK: case SOL_MIR_TERM_CONTINUE: candidates[count++] = term->edge; break; case SOL_MIR_TERM_BRANCH: candidates[count++] = term->true_edge; candidates[count++] = term->false_edge; break; case SOL_MIR_TERM_INVOKE: case SOL_MIR_TERM_CHECK_REFINED: candidates[count++] = term->normal_edge; candidates[count++] = term->failure_edge; break; case SOL_MIR_TERM_PROPAGATE: candidates[count++] = term->value_edge; candidates[count++] = term->residual_edge; break; case SOL_MIR_TERM_CHECK_CONTRACT: candidates[count++] = term->satisfied_edge; candidates[count++] = term->violation_edge; candidates[count++] = term->failure_edge; break; default: break; } for (size_t i = 0; i < count && validation_tick(); ++i) if (candidates[i] == edge) { *source = block; *ordinal = i; return true; } } return false; }
static bool predicate_edge_source(const SolMirOperations *operations, size_t edge, size_t *source, size_t *ordinal) { for (size_t block = 0; block < operations->predicate_block_count && validation_tick(); ++block) { const SolMirPredicateTerminator *term = &operations->predicate_blocks[block].terminator; size_t candidates[2] = { SOL_MIR_RUNTIME_NONE, SOL_MIR_RUNTIME_NONE }; size_t count = 0; switch (term->kind) { case SOL_MIR_PREDICATE_TERM_JUMP: candidates[count++] = term->edge; break; case SOL_MIR_PREDICATE_TERM_BRANCH: candidates[count++] = term->true_edge; candidates[count++] = term->false_edge; break; case SOL_MIR_PREDICATE_TERM_INVOKE: case SOL_MIR_PREDICATE_TERM_PROPAGATE: case SOL_MIR_PREDICATE_TERM_CHECK_REFINED: candidates[count++] = term->normal_edge; candidates[count++] = term->failure_edge; break; default: break; } for (size_t i = 0; i < count && validation_tick(); ++i) if (candidates[i] == edge) { *source = block; *ordinal = i; return true; } } return false; }
static uint32_t recipe_facilities(uint32_t demand) { uint32_t facilities=0; if(demand) facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE; if(demand&SOL_MIR_LINKAGE_RUNTIME_CREATE) facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION; if(demand&SOL_MIR_LINKAGE_RUNTIME_COPY) facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY; if(demand&SOL_MIR_LINKAGE_RUNTIME_DROP) facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP; if(demand&SOL_MIR_LINKAGE_RUNTIME_EQUAL) facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY; return facilities; }
static uint32_t value_facilities(const SolMirRuntimeValues *values, size_t recipe) { uint32_t facilities=0; if(values->allocation_plans[recipe].kind!=SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE) facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION; if(values->copy_plans[recipe].classification!=SOL_MIR_RUNTIME_COPY_UNREACHABLE&&values->copy_plans[recipe].classification!=SOL_MIR_RUNTIME_COPY_FORBIDDEN) facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY; if(values->equality_plans[recipe].classification!=SOL_MIR_RUNTIME_EQUALITY_UNREACHABLE&&values->equality_plans[recipe].classification!=SOL_MIR_RUNTIME_EQUALITY_FORBIDDEN) facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY; if(values->ownership_plans[recipe].classification!=SOL_MIR_RUNTIME_OWNERSHIP_UNREACHABLE) facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP; if(values->host_result_plans[recipe].classification!=SOL_MIR_RUNTIME_HOST_RESULT_UNREACHABLE&&values->host_result_plans[recipe].classification!=SOL_MIR_RUNTIME_HOST_RESULT_FORBIDDEN) facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT; for (size_t i=0;i<values->host_result_requirement_count && validation_tick();i++)if(values->host_result_requirements[i].result==recipe)facilities|=SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT; return facilities?facilities|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE:0; }
static bool copy_requires_runtime(SolMirCopyKind kind){return kind==SOL_MIR_COPY_TEXT||kind==SOL_MIR_COPY_AGGREGATE||kind==SOL_MIR_COPY_WRAPPER;}
static uint32_t value_demand(const SolMirRuntimeConventions*c,const SolMirRuntimeValues*v,const SolMirRuntimeCleanup*cleanup,const SolMirOperations*o,size_t recipe){uint32_t demand=0;for (size_t i=0;i<c->import_count && validation_tick();i++)if(c->imports[i].recipe==recipe){switch(c->imports[i].kind){case SOL_MIR_RUNTIME_IMPORT_RECIPE_CREATE:demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION;break;case SOL_MIR_RUNTIME_IMPORT_RECIPE_COPY:demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;break;case SOL_MIR_RUNTIME_IMPORT_RECIPE_DROP:demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP;break;case SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL:demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY;break;default:break;}}for (size_t i=0;i<o->constructor_count && validation_tick();i++)if(o->constructors[i].result_recipe==recipe)demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION|SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP;for (size_t i=0;i<o->pattern_extraction_count && validation_tick();i++)if(o->pattern_extractions[i].result_recipe==recipe&&copy_requires_runtime(o->pattern_extractions[i].copy_kind))demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;for (size_t i=0;i<o->snapshot_count && validation_tick();i++)if(o->snapshots[i].recipe==recipe&&copy_requires_runtime(o->snapshots[i].copy_kind))demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;for (size_t i=0;i<o->arithmetic_count && validation_tick();i++)if((o->arithmetic[i].operand_recipe==recipe||o->arithmetic[i].result_recipe==recipe)&&o->arithmetic[i].equality.count)demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY;for (size_t i=0;i<cleanup->action_count && validation_tick();i++)if(cleanup->actions[i].recipe==recipe&&(cleanup->actions[i].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_TEMPORARY||cleanup->actions[i].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PLACE||cleanup->actions[i].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_SNAPSHOT||cleanup->actions[i].kind==SOL_MIR_RUNTIME_CLEANUP_ACTION_DROP_PARAMETER))demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP;for (size_t i=0;i<v->host_result_requirement_count && validation_tick();i++)if(v->host_result_requirements[i].result==recipe)demand|=SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT;return demand?demand|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE:0;}
static size_t cleanup_event_for(const SolMirRuntimeCleanup *, SolMirRuntimeCleanupEventKind, size_t, size_t, size_t, bool, SolMirRuntimeCleanupProducerKind);
typedef struct { const SolMirRuntimeLoweredProgram *owner; size_t count; } DemandReader;
static bool expected_value_plan_for_recipe(const SolMirRuntimeValues *values, SolMirRecipeId recipe,
    size_t *result) {
    for (size_t i = 0; i < values->recipe_operation_count && validation_tick(); ++i)
        if (values->recipe_operations[i].recipe == recipe
            && values->allocation_plans[i].recipe == recipe
            && values->copy_plans[i].recipe == recipe
            && values->equality_plans[i].recipe == recipe
            && values->ownership_plans[i].recipe == recipe
            && values->host_result_plans[i].recipe == recipe) {
            *result = i; return true;
        }
    return false;
}
static bool expected_demand_add(DemandReader *reader, const SolMirRuntimeValues *values,
    SolMirRecipeId recipe, uint32_t wanted) {
    if (recipe == SOL_MIR_RECIPE_NONE) return true;
    size_t value_plan;
    if (!expected_value_plan_for_recipe(values, recipe, &value_plan)
        || (reader->owner && reader->count >= reader->owner->recipe_demand_count)) return false;
    if (reader->owner) {
        const SolMirRuntimeLoweredRecipeDemand *row = &reader->owner->recipe_demands[reader->count];
        if (!present(row->state) || row->recipe != recipe || row->value_plan != value_plan
            || row->facilities != (value_facilities(values, value_plan) & wanted)
            || row->ordinal != reader->count) return false;
    }
    ++reader->count;
    return true;
}
static bool expected_signature_demands(DemandReader *writer,
    const SolMirRuntimeConventions *conventions, const SolMirRuntimeValues *values,
    size_t signature) {
    if (signature >= conventions->signature_count) return false;
    const SolMirRuntimeSignature *input = &conventions->signatures[signature];
    if (!expected_demand_add(writer, values, input->function_recipe,
            SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
        || input->slots.offset > conventions->signature_slot_count
        || input->slots.count > conventions->signature_slot_count - input->slots.offset)
        return false;
    for (size_t i = 0; i < input->slots.count && validation_tick(); ++i)
        if (!expected_demand_add(writer, values,
                conventions->signature_slots[input->slots.offset + i].recipe,
                SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)) return false;
    return expected_demand_add(writer, values, input->result,
        SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
        | (input->origin == SOL_MIR_RUNTIME_SIGNATURE_HOST
            ? SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT : 0));
}
static bool expected_image_instruction_demands(DemandReader *writer,
    const SolMirMaterialization *materialization, const SolMirOperations *operations,
    const SolMirRuntimeValues *values, const SolMirRuntimeCleanup *cleanup, size_t instruction) {
    const SolMirMaterializedInstruction *input = &materialization->instructions[instruction];
    SolMirRuntimeLoweredDemandDescriptor descriptor_value;
    if (!image_instruction_descriptor(input->kind, &descriptor_value)) return false;
    if (descriptor_value.runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE) {
        switch (input->kind) {
        case SOL_MIR_INST_TEMPORARY_DROP: case SOL_MIR_INST_DROP_IF_INITIALIZED:
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            break;
        default: return true;
        }
        size_t event = cleanup_event_for(cleanup,
            SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,
            image_for_instruction(materialization, instruction), input->block, instruction,
            true, SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL);
        if (event == SOL_MIR_RUNTIME_LOWERED_NONE) return true;
        const SolMirRuntimeCleanupEvent *source = &cleanup->events[event];
        if (source->actions.offset > cleanup->action_count
            || source->actions.count > cleanup->action_count - source->actions.offset) return false;
        for (size_t i = 0; i < source->actions.count && validation_tick(); ++i)
            if (!expected_demand_add(writer, values,
                    cleanup->actions[source->actions.offset + i].recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP)) return false;
        return true;
    }
    uint32_t wanted = descriptor_value.facilities &
        (SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION
        | SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY | SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY
        | SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP | SOL_MIR_RUNTIME_LOWERED_FACILITY_HOST_RESULT);
    if (input->kind == SOL_MIR_INST_LOAD_COPY && !copy_requires_runtime(operations->layout->representation->recipes[input->type].copy_kind)) wanted &= ~(uint32_t)SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;
    if (!expected_demand_add(writer, values, input->type, wanted)) return false;
    for (size_t i = 0; i < operations->constructor_count && validation_tick(); ++i) {
        const SolMirOperationConstructPlan *plan = &operations->constructors[i];
        if (plan->instruction != instruction) continue;
        if (!expected_demand_add(writer, values, plan->result_recipe,
                SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION
                | SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP)) return false;
        for (size_t q = 0; q < plan->operands.count && validation_tick(); ++q)
            if (!expected_demand_add(writer, values,
                    operations->construct_operands[plan->operands.offset + q].recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)) return false;
    }
    for (size_t i = 0; i < operations->pattern_extraction_count && validation_tick(); ++i) {
        const SolMirOperationPatternExtraction *plan = &operations->pattern_extractions[i];
        if (plan->instruction == instruction
            && (!expected_demand_add(writer, values, plan->scrutinee_recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
                || !expected_demand_add(writer, values, plan->result_recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
                    | (copy_requires_runtime(plan->copy_kind)
                        ? SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY : 0)))) return false;
    }
    for (size_t i = 0; i < operations->arithmetic_count && validation_tick(); ++i) {
        const SolMirOperationArithmeticPlan *plan = &operations->arithmetic[i];
        if (plan->instruction == instruction
            && (!expected_demand_add(writer, values, plan->operand_recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
                    | (plan->equality.count ? SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY : 0))
                || !expected_demand_add(writer, values, plan->result_recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE))) return false;
    }
    for (size_t i = 0; i < operations->snapshot_count && validation_tick(); ++i) {
        const SolMirOperationSnapshotPlan *plan = &operations->snapshots[i];
        if (plan->instruction == instruction
            && (!expected_demand_add(writer, values, plan->root_recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
                || !expected_demand_add(writer, values, plan->recipe,
                    SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
                    | (copy_requires_runtime(plan->copy_kind)
                        ? SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY : 0)))) return false;
    }
    return true;
}
static bool expected_image_terminator_demands(DemandReader *writer,
    const SolMirRuntimeConventions *conventions, const SolMirRuntimeValues *values,
    const SolMirMaterialization *materialization, const SolMirOperations *operations,
    size_t image, size_t block) {
    const SolMirMaterializedTerminator *term = &materialization->blocks[block].terminator;
    SolMirRuntimeLoweredDemandDescriptor descriptor_value;
    if (!image_terminator_descriptor(term->kind, &descriptor_value)) return false;
    if (descriptor_value.runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE) return true;
    if (term->kind == SOL_MIR_TERM_INVOKE) {
        size_t call = image_call_for_block(conventions, image, block);
        if (call == SOL_MIR_RUNTIME_NONE || !expected_signature_demands(writer, conventions,
                values, conventions->calls[call].signature)) return false;
    }
    for (size_t i = 0; i < operations->propagation_count && validation_tick(); ++i) {
        const SolMirOperationPropagationPlan *plan = &operations->propagations[i];
        if (plan->block == block
            && (!expected_demand_add(writer, values, plan->source_recipe, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
                || !expected_demand_add(writer, values, plan->success_recipe, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
                || !expected_demand_add(writer, values, plan->residual_recipe, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE))) return false;
    }
    for (size_t i = 0; i < operations->predicate_count && validation_tick(); ++i) {
        const SolMirOperationPredicatePlan *plan = &operations->predicates[i];
        if (plan->block == block
            && (!expected_demand_add(writer, values, plan->input_recipe, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
                || !expected_demand_add(writer, values, plan->result_recipe, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)
                || !expected_demand_add(writer, values, plan->output_recipe, SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE))) return false;
    }
    return true;
}
static bool expected_predicate_instruction_demands(DemandReader *writer,
    const SolMirOperations *operations, const SolMirRuntimeValues *values, size_t instruction) {
    const SolMirPredicateInstruction *input = &operations->predicate_instructions[instruction];
    SolMirRuntimeLoweredDemandDescriptor descriptor_value;
    if (!predicate_instruction_descriptor(input->kind, &descriptor_value)) return false;
    if (descriptor_value.runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE) return true;
    uint32_t wanted = descriptor_value.facilities &
        (SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE | SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION
        | SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY | SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY
        | SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP);
    if (input->kind == SOL_MIR_PREDICATE_INST_CONSTRUCT
        && input->recipe < values->allocation_plan_count
        && values->allocation_plans[input->recipe].kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE)
        wanted |= SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION;
    if (input->kind == SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT
        && copy_requires_runtime(operations->layout->representation->recipes[input->recipe].copy_kind))
        wanted |= SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY;
    if (!expected_demand_add(writer, values, input->recipe, wanted)) return false;
    if ((input->kind == SOL_MIR_PREDICATE_INST_BINARY || input->kind == SOL_MIR_PREDICATE_INST_UNARY)
        && !expected_demand_add(writer, values, operations->predicate_values[input->left].recipe,
            SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE
            | ((input->opcode == SOL_MIR_OPERATION_VALUE_EQ || input->opcode == SOL_MIR_OPERATION_VALUE_NE)
                ? SOL_MIR_RUNTIME_LOWERED_FACILITY_EQUALITY : 0))) return false;
    if (input->kind == SOL_MIR_PREDICATE_INST_BINARY
        && !expected_demand_add(writer, values, operations->predicate_values[input->right].recipe,
            SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)) return false;
    for (size_t i = 0; i < input->operands.count && validation_tick(); ++i)
        if (!expected_demand_add(writer, values,
                operations->predicate_values[operations->predicate_operands[input->operands.offset + i].value].recipe,
                SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)) return false;
    return true;
}
static bool expected_predicate_terminator_demands(DemandReader *writer,
    const SolMirRuntimeConventions *conventions, const SolMirRuntimeValues *values,
    const SolMirOperations *operations, size_t body, size_t block) {
    const SolMirPredicateTerminator *term = &operations->predicate_blocks[block].terminator;
    SolMirRuntimeLoweredDemandDescriptor descriptor_value;
    if (!predicate_terminator_descriptor(term->kind, &descriptor_value)) return false;
    if (descriptor_value.runtime_class != SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE) return true;
    if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE) {
        size_t call = predicate_call_for_block(conventions, body, block);
        if (call == SOL_MIR_RUNTIME_NONE || !expected_signature_demands(writer, conventions,
                values, conventions->calls[call].signature)) return false;
    }
    if (!expected_demand_add(writer, values, term->result_recipe,
            SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)) return false;
    if (term->value < operations->predicate_value_count
        && !expected_demand_add(writer, values, operations->predicate_values[term->value].recipe,
            SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE)) return false;
    return true;
}
static size_t semantic_offset(const SolMirOperations *operations,
    SolMirOperationProvenanceKind kind) {
    size_t offset = operations->access_plan_count;
    switch (kind) {
    case SOL_MIR_OPERATION_PROVENANCE_CONSTRUCT: return offset;
    case SOL_MIR_OPERATION_PROVENANCE_PATTERN_TEST: return offset + operations->constructor_count;
    case SOL_MIR_OPERATION_PROVENANCE_PATTERN_EXTRACTION: return offset + operations->constructor_count + operations->pattern_test_count;
    case SOL_MIR_OPERATION_PROVENANCE_PROPAGATION: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count;
    case SOL_MIR_OPERATION_PROVENANCE_ARITHMETIC: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count + operations->propagation_count;
    case SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count + operations->propagation_count + operations->arithmetic_count;
    case SOL_MIR_OPERATION_PROVENANCE_CALLABLE: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count + operations->propagation_count + operations->arithmetic_count + operations->snapshot_count;
    case SOL_MIR_OPERATION_PROVENANCE_HANDLER: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count + operations->propagation_count + operations->arithmetic_count + operations->snapshot_count + operations->callable_count;
    case SOL_MIR_OPERATION_PROVENANCE_PREDICATE: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count + operations->propagation_count + operations->arithmetic_count + operations->snapshot_count + operations->callable_count + operations->handler_count;
    case SOL_MIR_OPERATION_PROVENANCE_IMPORT_SNAPSHOT: return offset + operations->constructor_count + operations->pattern_test_count + operations->pattern_extraction_count + operations->propagation_count + operations->arithmetic_count + operations->snapshot_count + operations->callable_count + operations->handler_count + operations->predicate_count;
    case SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY: return SOL_MIR_RUNTIME_LOWERED_NONE;
    }
    return SOL_MIR_RUNTIME_LOWERED_NONE;
}
static bool semantic_for_provenance(const SolMirOperations *operations,
    SolMirOperationProvenanceKind kind, size_t executable, size_t *result) {
    size_t matches = 0;
    for (size_t i = 0; i < operations->provenance_count && validation_tick(); ++i)
        if (operations->provenance[i].kind == kind
            && operations->provenance[i].executable == executable) ++matches;
    size_t offset = semantic_offset(operations, kind);
    if (matches != 1 || offset == SOL_MIR_RUNTIME_LOWERED_NONE) return false;
    *result = offset + executable;
    return true;
}
static size_t callable_semantic_for_binding(const SolMirMaterialization *materialization,
    const SolMirOperations *operations, size_t binding) {
    size_t result = SOL_MIR_RUNTIME_LOWERED_NONE;
    for (size_t i = 0; i < operations->callable_count && validation_tick(); ++i) {
        const SolMirOperationCallablePlan *plan = &operations->callables[i];
        if (plan->semantic_site >= materialization->semantic_site_count
            || materialization->semantic_sites[plan->semantic_site].binding != binding) continue;
        size_t semantic;
        if (!semantic_for_provenance(operations,
                SOL_MIR_OPERATION_PROVENANCE_CALLABLE, i, &semantic)
            || result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE;
        result = semantic;
    }
    return result;
}
static size_t image_instruction_semantic(const SolMirOperations *operations,
    size_t instruction) {
    size_t result = SOL_MIR_RUNTIME_LOWERED_NONE, semantic;
#define MATCH(member, count, field, provenance_kind) \
    for (size_t i = 0; i < operations->count && validation_tick(); ++i) if (operations->member[i].field == instruction) { \
        if (!semantic_for_provenance(operations, provenance_kind, i, &semantic) || result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE; result = semantic; }
    MATCH(constructors, constructor_count, instruction, SOL_MIR_OPERATION_PROVENANCE_CONSTRUCT)
    MATCH(pattern_tests, pattern_test_count, instruction, SOL_MIR_OPERATION_PROVENANCE_PATTERN_TEST)
    MATCH(pattern_extractions, pattern_extraction_count, instruction, SOL_MIR_OPERATION_PROVENANCE_PATTERN_EXTRACTION)
    MATCH(arithmetic, arithmetic_count, instruction, SOL_MIR_OPERATION_PROVENANCE_ARITHMETIC)
    MATCH(snapshots, snapshot_count, instruction, SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT)
#undef MATCH
    return result;
}
static size_t image_terminator_semantic(const SolMirMaterialization *materialization,
    const SolMirOperations *operations, size_t block) {
    const SolMirMaterializedTerminator *term = &materialization->blocks[block].terminator;
    size_t result = SOL_MIR_RUNTIME_LOWERED_NONE, semantic;
    for (size_t i = 0; i < operations->propagation_count && validation_tick(); ++i)
        if (operations->propagations[i].block == block) {
            if (!semantic_for_provenance(operations,
                    SOL_MIR_OPERATION_PROVENANCE_PROPAGATION, i, &semantic)
                || result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE;
            result = semantic;
        }
    for (size_t i = 0; i < operations->predicate_count && validation_tick(); ++i)
        if (operations->predicates[i].block == block) {
            if (!semantic_for_provenance(operations,
                    SOL_MIR_OPERATION_PROVENANCE_PREDICATE, i, &semantic)
                || result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE;
            result = semantic;
        }
    if (term->kind == SOL_MIR_TERM_INVOKE && term->callable_site != SOL_MIR_MATERIALIZED_NONE) {
        size_t callable = callable_semantic_for_binding(materialization, operations,
            materialization->semantic_sites[term->callable_site].binding);
        if (callable == SOL_MIR_RUNTIME_LOWERED_NONE || result != SOL_MIR_RUNTIME_LOWERED_NONE)
            return SOL_MIR_RUNTIME_LOWERED_NONE;
        result = callable;
    }
    return result;
}
static size_t predicate_instruction_semantic(const SolMirMaterialization *materialization,
    const SolMirOperations *operations, size_t instruction) {
    const SolMirPredicateInstruction *input = &operations->predicate_instructions[instruction];
    if (input->kind != SOL_MIR_PREDICATE_INST_FUNCTION
        && input->kind != SOL_MIR_PREDICATE_INST_BOUND_OPERATION)
        return SOL_MIR_RUNTIME_LOWERED_NONE;
    return callable_semantic_for_binding(materialization, operations, input->binding);
}
static size_t predicate_terminator_semantic(const SolMirMaterialization *materialization,
    const SolMirOperations *operations, size_t block) {
    const SolMirPredicateTerminator *term = &operations->predicate_blocks[block].terminator;
    if (term->kind != SOL_MIR_PREDICATE_TERM_INVOKE) return SOL_MIR_RUNTIME_LOWERED_NONE;
    size_t semantic = callable_semantic_for_binding(materialization, operations,
        term->binding);
    if (semantic != SOL_MIR_RUNTIME_LOWERED_NONE) return semantic;
    if (term->callee >= operations->predicate_value_count) return SOL_MIR_RUNTIME_LOWERED_NONE;
    const SolMirPredicateValue *callee = &operations->predicate_values[term->callee];
    return callee->kind == SOL_MIR_PREDICATE_VALUE_INSTRUCTION
        && callee->definition < operations->predicate_instruction_count
        ? callable_semantic_for_binding(materialization, operations,
            operations->predicate_instructions[callee->definition].binding)
        : SOL_MIR_RUNTIME_LOWERED_NONE;
}
static size_t cleanup_event_for(const SolMirRuntimeCleanup *cleanup,
    SolMirRuntimeCleanupEventKind kind, size_t owner, size_t block, size_t operation,
    bool require_producer, SolMirRuntimeCleanupProducerKind producer) {
    size_t result = SOL_MIR_RUNTIME_LOWERED_NONE;
    for (size_t i = 0; i < cleanup->event_count && validation_tick(); ++i) {
        const SolMirRuntimeCleanupEvent *event = &cleanup->events[i];
        if (event->kind != kind || event->owner != owner || event->block != block
            || event->operation != operation || (require_producer && event->producer != producer)) continue;
        if (result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE;
        result = i;
    }
    return result;
}
static size_t cleanup_failure_for_event(const SolMirRuntimeCleanup *cleanup, size_t event) {
    return event == SOL_MIR_RUNTIME_LOWERED_NONE ? SOL_MIR_RUNTIME_LOWERED_NONE
        : cleanup->events[event].inherited_failure_site;
}
static size_t handler_frame_for_cleanup_action(const SolMirRuntimeHandlerAbi *handlers,
    SolMirRuntimeCleanupActionId action) {
    size_t result = SOL_MIR_RUNTIME_LOWERED_NONE;
    for (size_t i = 0; i < handlers->cleanup_exit_count && validation_tick(); ++i) {
        const SolMirRuntimeHandlerCleanupExit *exit = &handlers->cleanup_exits[i];
        if (exit->action != action) continue;
        if (result != SOL_MIR_RUNTIME_LOWERED_NONE) return SOL_MIR_RUNTIME_LOWERED_NONE;
        result = exit->frame;
    }
    return result;
}
static bool image_instruction_event_producer(const SolMirMaterialization *materialization,
    const SolMirOperations *operations, const SolMirRuntimeValues *values,
    size_t instruction, SolMirRuntimeCleanupProducerKind *producer) {
    const SolMirMaterializedInstruction *input = &materialization->instructions[instruction];
    for (size_t i = 0; i < operations->arithmetic_count && validation_tick(); ++i)
        if (operations->arithmetic[i].instruction == instruction
            && operations->arithmetic[i].failures) {
            *producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_ARITHMETIC; return true;
        }
    SolMirRecipeId recipe = SOL_MIR_RECIPE_NONE;
    if (input->kind == SOL_MIR_INST_CONST_TEXT || input->kind == SOL_MIR_INST_CONSTRUCT)
        recipe = input->type;
    else if (input->kind == SOL_MIR_INST_LOAD_COPY && input->place < materialization->place_count)
        recipe = materialization->places[input->place].final_type;
    if (recipe < values->allocation_plan_count
        && values->allocation_plans[recipe].kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE) {
        *producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION; return true;
    }
    switch (input->kind) {
    case SOL_MIR_INST_TEMPORARY_DROP: case SOL_MIR_INST_DROP_IF_INITIALIZED:
    case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED: case SOL_MIR_INST_SCOPE_EXIT:
    case SOL_MIR_INST_REGION_EXIT:
        *producer = SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL; return true;
    default: return false;
    }
}
static SolMirRuntimeCleanupProducerKind image_terminator_event_producer(
    SolMirTerminatorKind kind) {
    switch (kind) {
    case SOL_MIR_TERM_INVOKE: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_INVOKE;
    case SOL_MIR_TERM_PANIC: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_PANIC;
    case SOL_MIR_TERM_MATCH_FAILURE: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_NO_MATCH;
    case SOL_MIR_TERM_UNREACHABLE: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_IMAGE_UNREACHABLE;
    default: return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL;
    }
}
static SolMirRuntimeCleanupProducerKind predicate_instruction_event_producer(
    const SolMirPredicateInstruction *input, const SolMirRuntimeValues *values) {
    if (input->kind == SOL_MIR_PREDICATE_INST_CONSTRUCT
        && input->recipe < values->allocation_plan_count
        && values->allocation_plans[input->recipe].kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE)
        return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_SUPPLEMENTAL_ALLOCATION;
    return input->failures ? SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_ARITHMETIC
        : SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL;
}
static SolMirRuntimeCleanupProducerKind predicate_terminator_event_producer(
    const SolMirPredicateTerminator *term) {
    if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE)
        return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_INVOKE;
    if (term->kind == SOL_MIR_PREDICATE_TERM_FAILURE
        && term->failure_kind == SOL_MIR_PREDICATE_FAILURE_NO_MATCH)
        return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_NO_MATCH;
    if (term->kind == SOL_MIR_PREDICATE_TERM_RETURN)
        return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_PREDICATE_RESULT;
    return SOL_MIR_RUNTIME_CLEANUP_PRODUCER_CONTROL;
}

static bool semantic_expected(const SolMirOperations *operations, const SolMirMaterialization *materialization, size_t index, SolMirRuntimeLoweredSemanticPlan *out) {
#define SET(arena_value, class_value, family_value, plan_value, facility_value, producer_kind_value, producer_value) do { *out = (SolMirRuntimeLoweredSemanticPlan){ SOL_MIR_RUNTIME_LOWERED_PRESENT, arena_value, class_value, family_value, plan_value, facility_value, producer_kind_value, producer_value }; return true; } while (0)
    if (index < operations->access_plan_count) SET(SOL_MIR_RUNTIME_LOWERED_SEMANTIC_ACCESS,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_VALUE,index,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE,SOL_MIR_MATERIALIZED_PRODUCER_ROOT,operations->access_plans[index].place); index -= operations->access_plan_count;
    if (index < operations->constructor_count) SET(SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CONSTRUCT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CONSTRUCT,index,SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION|SOL_MIR_RUNTIME_LOWERED_FACILITY_OWNERSHIP,SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION,operations->constructors[index].instruction); index -= operations->constructor_count;
    if (index < operations->pattern_test_count) SET(SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PATTERN_TEST,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,index,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE,SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION,operations->pattern_tests[index].instruction); index -= operations->pattern_test_count;
    if (index < operations->pattern_extraction_count) SET(SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PATTERN_EXTRACTION,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PATTERN,index,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY,SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION,operations->pattern_extractions[index].instruction); index -= operations->pattern_extraction_count;
    if (index < operations->propagation_count) SET(SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PROPAGATION,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PROPAGATION,index,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP,SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR,operations->propagations[index].block); index -= operations->propagation_count;
    if (index < operations->arithmetic_count) SET(SOL_MIR_RUNTIME_LOWERED_SEMANTIC_ARITHMETIC,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_ARITHMETIC,index,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP|SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE,SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION,operations->arithmetic[index].instruction); index -= operations->arithmetic_count;
    if (index < operations->snapshot_count) SET(SOL_MIR_RUNTIME_LOWERED_SEMANTIC_SNAPSHOT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_SNAPSHOT,index,SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE,SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION,operations->snapshots[index].instruction); index -= operations->snapshot_count;
    if (index < operations->callable_count) SET(SOL_MIR_RUNTIME_LOWERED_SEMANTIC_CALLABLE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_CALLABLE,index,SOL_MIR_RUNTIME_LOWERED_FACILITY_RECIPE|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE,materialization->semantic_sites[operations->callables[index].semantic_site].producer_kind,operations->callables[index].semantic_site); index -= operations->callable_count;
    if (index < operations->handler_count) SET(SOL_MIR_RUNTIME_LOWERED_SEMANTIC_HANDLER,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_HANDLER,index,SOL_MIR_RUNTIME_LOWERED_FACILITY_HANDLER_FRAME,SOL_MIR_MATERIALIZED_PRODUCER_HANDLER,operations->handlers[index].handler); index -= operations->handler_count;
    if (index < operations->predicate_count) SET(SOL_MIR_RUNTIME_LOWERED_SEMANTIC_PREDICATE,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_PREDICATE,index,SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE|SOL_MIR_RUNTIME_LOWERED_FACILITY_CLEANUP,SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE,operations->predicates[index].body); index -= operations->predicate_count;
    if (index < operations->import_snapshot_count) SET(SOL_MIR_RUNTIME_LOWERED_SEMANTIC_IMPORT_SNAPSHOT,SOL_MIR_RUNTIME_LOWERED_CLASS_EXECUTABLE,SOL_MIR_RUNTIME_LOWERED_PLAN_IMPORT_SNAPSHOT,index,SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY|SOL_MIR_RUNTIME_LOWERED_FACILITY_VALUE,SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE,operations->import_snapshots[index].provenance);
#undef SET
    return false;
}
static bool range(const void *pointer, size_t count, size_t element_size);
uint64_t sol_mir_runtime_lowered_program_internal_seal(
    const SolMirRuntimeLoweredProgram *owner, SolMirRuntimeLoweredWorkMeter *meter) {
    /* Builder sealing supplies its approved build meter explicitly.  A
     * validator seal uses the scoped meter installed by internal_validate. */
    if (!meter) meter = active_validation_meter;
    uint64_t hash = UINT64_C(0x50332e362d736c61);
#define MIX(value) do { if (meter && !sol_mir_runtime_lowered_tick(meter)) return 0; hash = (hash ^ (uint64_t)(value)) + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2); } while (0)
#define RAW(member,type,singular) \
    MIX((uintptr_t)owner->member); MIX(owner->singular##_count); MIX(owner->singular##_capacity); \
    if (owner->singular##_count != owner->singular##_capacity \
        || !range(owner->member, owner->singular##_capacity, sizeof(type))) return hash ? hash : 1;
    SOL_MIR_RUNTIME_LOWERED_TABLES(RAW)
#undef RAW
#define SEAL(member,type,singular) \
    for (size_t i = 0; i < owner->singular##_count; ++i) { const unsigned char *bytes = (const unsigned char *)&owner->member[i]; for (size_t byte = 0; byte < sizeof(type); ++byte) { if (meter && !sol_mir_runtime_lowered_tick(meter)) return 0; hash = (hash ^ bytes[byte]) + UINT64_C(0x9e3779b97f4a7c15) + (hash << 6) + (hash >> 2); } }
    SOL_MIR_RUNTIME_LOWERED_TABLES(SEAL)
#undef SEAL
#define USAGE(member,type,singular) MIX(owner->usage.member);
    SOL_MIR_RUNTIME_LOWERED_TABLES(USAGE)
#undef USAGE
    MIX(owner->usage.erased_loops); MIX(owner->usage.provenance_records);
    MIX(owner->usage.demanded_semantic_plans); MIX(owner->usage.owned_bytes);
    MIX(owner->usage.build_scratch_bytes); MIX(owner->usage.build_work);
    MIX(owner->usage.validation_scratch_bytes); MIX(owner->usage.validation_work);
    MIX(owner->usage.render_bytes); MIX(owner->usage.render_scratch_bytes);
    MIX((uintptr_t)owner->conventions); MIX((uintptr_t)owner->values);
    MIX((uintptr_t)owner->cleanup); MIX((uintptr_t)owner->host_abi);
    MIX((uintptr_t)owner->handler_abi);
#define LIMIT(member,type,singular) MIX(owner->limits.max_##member);
    SOL_MIR_RUNTIME_LOWERED_TABLES(LIMIT)
#undef LIMIT
    MIX(owner->limits.max_owned_bytes); MIX(owner->limits.max_build_scratch_bytes);
    MIX(owner->limits.max_build_work); MIX(owner->limits.max_validation_scratch_bytes);
    MIX(owner->limits.max_validation_work);
    MIX(owner->limits.max_render_bytes); MIX(owner->limits.max_render_scratch_bytes);
#undef MIX
    return hash ? hash : 1;
}
#ifdef SOL_MIR_PLAN_TEST_HOOKS
uint64_t sol_mir_runtime_lowered_program_test_seal(const SolMirRuntimeLoweredProgram *owner) {
    return sol_mir_runtime_lowered_program_internal_seal(owner, NULL);
}
#endif
static bool range(const void *pointer, size_t count, size_t element_size) {
    if (!validation_tick()) return false;
    size_t bytes;
    return !count ? pointer == NULL : pointer && mul_size(count, element_size, &bytes) && (uintptr_t)pointer <= UINTPTR_MAX - bytes;
}
typedef struct { const void *pointer; size_t count, size; } RawRange;
static bool ranges_overlap(const RawRange *left, const RawRange *right) {
    if (!validation_tick()) return true;
    size_t left_bytes, right_bytes;
    if (!left->count || !right->count) return false;
    if (!mul_size(left->count, left->size, &left_bytes)
        || !mul_size(right->count, right->size, &right_bytes)) return true;
    uintptr_t left_start = (uintptr_t)left->pointer;
    uintptr_t right_start = (uintptr_t)right->pointer;
    if (left_start > UINTPTR_MAX - left_bytes || right_start > UINTPTR_MAX - right_bytes)
        return true;
    return left_start < right_start + right_bytes && right_start < left_start + left_bytes;
}
typedef struct { const RawRange *local; size_t count; bool exhausted; } LoweredAliasContext;
static bool lowered_alias_visit(const void *pointer, size_t count, size_t size, void *opaque) {
    LoweredAliasContext *context = opaque;
    if (!validation_tick()) { context->exhausted = true; return false; }
    RawRange candidate = {pointer, count, size};
    if (!sol_mir_runtime_arena_range(pointer, count, size)) return false;
    for (size_t i = 0; i < context->count; ++i) {
        if (!validation_tick()) { context->exhausted = true; return false; }
        if (ranges_overlap(&context->local[i], &candidate)) {
            if (active_validation_meter && active_validation_meter->exhausted)
                context->exhausted = true;
            return false;
        }
    }
    return true;
}
static SolMirRuntimeTextGuardResult lowered_text_guard(uintptr_t address,
    uintptr_t *boundary, void *opaque) {
    LoweredAliasContext *context = opaque;
    uintptr_t next = UINTPTR_MAX;
    for (size_t i = 0; i < context->count; ++i) {
        if (!validation_tick()) {
            context->exhausted = true;
            return SOL_MIR_RUNTIME_TEXT_EXHAUSTED;
        }
        const RawRange *range_value = &context->local[i];
        size_t bytes;
        if (!mul_size(range_value->count, range_value->size, &bytes))
            return active_validation_meter && active_validation_meter->exhausted
                ? (context->exhausted = true, SOL_MIR_RUNTIME_TEXT_EXHAUSTED)
                : SOL_MIR_RUNTIME_TEXT_MALFORMED;
        if (active_validation_meter && active_validation_meter->exhausted)
            return (context->exhausted = true, SOL_MIR_RUNTIME_TEXT_EXHAUSTED);
        if (!range_value->count) continue;
        uintptr_t start = (uintptr_t)range_value->pointer;
        if (start > UINTPTR_MAX - bytes) return SOL_MIR_RUNTIME_TEXT_MALFORMED;
        if (address >= start && address < start + bytes)
            return SOL_MIR_RUNTIME_TEXT_OVERLAP;
        if (start > address && start < next) next = start;
    }
    *boundary = next;
    return SOL_MIR_RUNTIME_TEXT_SAFE;
}
static bool raw_local_tables(RawRange *local, size_t local_count) {
    for (size_t i = 0; i < local_count; ++i) {
        if (!validation_tick()) return false;
        if (!range(local[i].pointer, local[i].count, local[i].size)) return false;
        for (size_t j = i + 1; j < local_count; ++j) {
            if (!validation_tick()) return false;
            if (ranges_overlap(&local[i], &local[j])) return false;
        }
    }
    return true;
}
static bool raw_slice(SolMirRuntimeSlice slice, size_t count) {
    if (!validation_tick()) return false;
    return slice.offset <= count && slice.count <= count - slice.offset;
}
/* This intentionally checks only local range topology, not semantic row values.
 * It is safe after raw table-range authentication and precedes the seal. */
static bool raw_owner_slices(const SolMirRuntimeLoweredProgram *owner) {
    for (size_t i = 0; i < owner->image_instruction_count && validation_tick(); ++i)
        if (!raw_slice(owner->image_instructions[i].demanded_recipes,
                owner->recipe_demand_count)) return false;
    for (size_t i = 0; i < owner->image_block_count && validation_tick(); ++i)
        if (!raw_slice(owner->image_blocks[i].incoming_edges, owner->image_incoming_edge_count)
            || !raw_slice(owner->image_blocks[i].outgoing_edges, owner->image_outgoing_edge_count))
            return false;
    for (size_t i = 0; i < owner->image_terminator_count && validation_tick(); ++i)
        if (!raw_slice(owner->image_terminators[i].demanded_recipes,
                owner->recipe_demand_count)) return false;
    for (size_t i = 0; i < owner->predicate_instruction_count && validation_tick(); ++i)
        if (!raw_slice(owner->predicate_instructions[i].demanded_recipes,
                owner->recipe_demand_count)) return false;
    for (size_t i = 0; i < owner->predicate_block_count && validation_tick(); ++i)
        if (!raw_slice(owner->predicate_blocks[i].incoming_edges,
                owner->predicate_incoming_edge_count)
            || !raw_slice(owner->predicate_blocks[i].outgoing_edges,
                owner->predicate_outgoing_edge_count)) return false;
    for (size_t i = 0; i < owner->predicate_terminator_count && validation_tick(); ++i)
        if (!raw_slice(owner->predicate_terminators[i].demanded_recipes,
                owner->recipe_demand_count)) return false;
    for (size_t i = 0; i < owner->host_grant_count && validation_tick(); ++i)
        if (!raw_slice(owner->host_grants[i].incidences, owner->host_incidence_count))
            return false;
    for (size_t i = 0; i < owner->cleanup_failure_count && validation_tick(); ++i) {
        const SolMirRuntimeLoweredCleanupFailure *row = &owner->cleanup_failures[i];
        if (row->kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_EVENT
            && (!raw_slice(row->actions, owner->cleanup->action_count)
                || !raw_slice(row->transitions, owner->cleanup->transition_count))) return false;
        if (row->kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION
            && !raw_slice(row->actions, owner->cleanup->action_count)) return false;
        if (row->kind == SOL_MIR_RUNTIME_LOWERED_CLEANUP_DROP_PATH
            && !raw_slice(row->drop_holes, owner->cleanup->drop_path_count)) return false;
    }
    return true;
}
static bool expected_totals(const SolMirRuntimeCleanup *cleanup,
    const SolMirOperations *operations, size_t *cleanup_total, size_t *semantic_total) {
    if (!validation_tick()) return false;
    size_t cleanup_count = 0, semantic_count = 0;
    if (!add_size(&cleanup_count, cleanup->event_count)
        || !add_size(&cleanup_count, cleanup->action_count)
        || !add_size(&cleanup_count, cleanup->transition_count)
        || !add_size(&cleanup_count, cleanup->supplemental_site_count)
        || !add_size(&cleanup_count, cleanup->drop_path_count)
        || !add_size(&semantic_count, operations->access_plan_count)
        || !add_size(&semantic_count, operations->constructor_count)
        || !add_size(&semantic_count, operations->pattern_test_count)
        || !add_size(&semantic_count, operations->pattern_extraction_count)
        || !add_size(&semantic_count, operations->propagation_count)
        || !add_size(&semantic_count, operations->arithmetic_count)
        || !add_size(&semantic_count, operations->snapshot_count)
        || !add_size(&semantic_count, operations->callable_count)
        || !add_size(&semantic_count, operations->handler_count)
        || !add_size(&semantic_count, operations->predicate_count)
        || !add_size(&semantic_count, operations->import_snapshot_count)) return false;
    *cleanup_total = cleanup_count;
    *semantic_total = semantic_count;
    return true;
}
typedef struct {
#define EXPECTED_MEMBER(member,type,singular) size_t member;
    SOL_MIR_RUNTIME_LOWERED_TABLES(EXPECTED_MEMBER)
#undef EXPECTED_MEMBER
    size_t owned_bytes;
} LoweredExpectedCardinalities;
static bool expected_cardinalities(const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeValues *values, const SolMirRuntimeCleanup *cleanup,
    const SolMirRuntimeHostAbi *host_abi, const SolMirRuntimeHandlerAbi *handler_abi,
    LoweredExpectedCardinalities *expected) {
    const SolMirMaterialization *materialization = &conventions->concrete->materialization;
    const SolMirOperations *operations = &conventions->concrete->operations;
    size_t cleanup_total, semantic_total, grants = 0, bytes = 0, table_bytes;
    if (!expected_totals(cleanup, operations, &cleanup_total, &semantic_total)) return false;
    for (size_t i = 0; i < host_abi->requirement_count && validation_tick(); ++i) {
        bool prior = false;
        for (size_t j = 0; j < i && validation_tick(); ++j) {
            const SolMirRuntimeHostRequirement *left = &host_abi->requirements[j];
            const SolMirRuntimeHostRequirement *right = &host_abi->requirements[i];
            if (left->entry == right->entry && left->root == right->root
                && left->operation == right->operation) { prior = true; break; }
        }
        if (!prior && !add_size(&grants, 1)) return false;
    }
    DemandReader demands = {NULL, 0};
    for (size_t i = 0; i < materialization->instruction_count && validation_tick(); ++i)
        if (!expected_image_instruction_demands(&demands, materialization, operations,
                values, cleanup, i)) return false;
    for (size_t i = 0; i < materialization->block_count && validation_tick(); ++i) {
        size_t image = image_for_block(materialization, i);
        if (image == SOL_MIR_RUNTIME_LOWERED_NONE
            || !expected_image_terminator_demands(&demands, conventions, values,
                materialization, operations, image, i)) return false;
    }
    for (size_t i = 0; i < operations->predicate_instruction_count && validation_tick(); ++i)
        if (!expected_predicate_instruction_demands(&demands, operations, values, i)) return false;
    for (size_t i = 0; i < operations->predicate_block_count && validation_tick(); ++i)
        if (!expected_predicate_terminator_demands(&demands, conventions, values,
                operations, operations->predicate_blocks[i].body, i)) return false;
    *expected = (LoweredExpectedCardinalities){
        .image_instructions = materialization->instruction_count,
        .image_blocks = materialization->block_count,
        .image_edges = materialization->edge_count,
        .image_incoming_edges = materialization->edge_count,
        .image_outgoing_edges = materialization->edge_count,
        .image_terminators = materialization->block_count,
        .predicate_bodies = operations->predicate_body_count,
        .predicate_instructions = operations->predicate_instruction_count,
        .predicate_blocks = operations->predicate_block_count,
        .predicate_edges = operations->predicate_edge_count,
        .predicate_incoming_edges = operations->predicate_edge_count,
        .predicate_outgoing_edges = operations->predicate_edge_count,
        .predicate_terminators = operations->predicate_block_count,
        .semantic_plans = semantic_total, .calls = conventions->call_count,
        .signatures = conventions->signature_count, .imports = conventions->import_count,
        .recipes = values->recipe_operation_count,
        .value_plans = values->recipe_operation_count, .recipe_demands = demands.count,
        .cleanup_failures = cleanup_total, .host_requirements = host_abi->requirement_count,
        .host_grants = grants, .host_incidences = host_abi->requirement_count,
        .handler_frames = handler_abi->frame_count,
        .handler_markers = handler_abi->exit_marker_count,
        .handler_exits = handler_abi->cleanup_exit_count,
    };
#define EXPECTED_BYTES(member,type,singular) \
    if (!validation_tick() || !mul_size(expected->member, sizeof(type), &table_bytes) || !add_size(&bytes, table_bytes)) return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(EXPECTED_BYTES)
#undef EXPECTED_BYTES
    expected->owned_bytes = bytes;
    return true;
}
static bool raw_declared_tables(const SolMirRuntimeLoweredProgram *owner) {
#define RANGE(member,type,singular) \
    if (!validation_tick() || !range(owner->member, owner->singular##_count, sizeof(type)) \
        || !range(owner->member, owner->singular##_capacity, sizeof(type))) return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(RANGE)
#undef RANGE
    return true;
}
static bool expected_owner_header(const SolMirRuntimeLoweredProgram *owner,
    const LoweredExpectedCardinalities *expected) {
#define MATCH(member,type,singular) \
    if (!validation_tick() || owner->singular##_count != expected->member || owner->singular##_capacity != expected->member \
        || owner->usage.member != expected->member || expected->member > owner->limits.max_##member) return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(MATCH)
#undef MATCH
    return owner->usage.owned_bytes == expected->owned_bytes
        && expected->owned_bytes <= owner->limits.max_owned_bytes;
}
static SolMirRuntimeArenaVisit raw_owner_preflight(const SolMirRuntimeLoweredProgram *owner) {
    RawRange local[] = {
        {owner, 1, sizeof *owner},
#define LOCAL(member,type,singular) {owner->member, owner->singular##_capacity, sizeof(type)},
        SOL_MIR_RUNTIME_LOWERED_TABLES(LOCAL)
#undef LOCAL
    };
    if (!raw_local_tables(local, sizeof local / sizeof *local))
        return active_validation_meter && active_validation_meter->exhausted
            ? SOL_MIR_RUNTIME_ARENA_VISIT_EXHAUSTED
            : SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED;
    LoweredAliasContext context = {local, sizeof local / sizeof *local, false};
#define AGAINST(pointer,count,type) \
    do { if (!validation_tick()) return SOL_MIR_RUNTIME_ARENA_VISIT_EXHAUSTED; \
        if (!lowered_alias_visit((pointer), (count), sizeof(type), &context)) \
            return context.exhausted ? SOL_MIR_RUNTIME_ARENA_VISIT_EXHAUSTED : SOL_MIR_RUNTIME_ARENA_VISIT_MALFORMED; } while (0)
    AGAINST(owner->conventions, 1, SolMirRuntimeConventions);
    AGAINST(owner->values, 1, SolMirRuntimeValues);
    AGAINST(owner->cleanup, 1, SolMirRuntimeCleanup);
    AGAINST(owner->host_abi, 1, SolMirRuntimeHostAbi);
    AGAINST(owner->handler_abi, 1, SolMirRuntimeHandlerAbi);
#define CONVENTION(member,type,singular) AGAINST(owner->conventions->member, owner->conventions->singular##_capacity, type);
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(CONVENTION)
#undef CONVENTION
    AGAINST(owner->values->recipe_operations, owner->values->recipe_operation_capacity, SolMirRuntimeRecipeOperations);
    AGAINST(owner->values->allocation_plans, owner->values->allocation_plan_capacity, SolMirRuntimeAllocationPlan);
    AGAINST(owner->values->copy_plans, owner->values->copy_plan_capacity, SolMirRuntimeCopyPlan);
    AGAINST(owner->values->equality_plans, owner->values->equality_plan_capacity, SolMirRuntimeEqualityPlan);
    AGAINST(owner->values->host_result_plans, owner->values->host_result_plan_capacity, SolMirRuntimeHostResultPlan);
    AGAINST(owner->values->host_result_requirements, owner->values->host_result_requirement_capacity, SolMirRuntimeHostResultRequirement);
    AGAINST(owner->values->ownership_plans, owner->values->ownership_plan_capacity, SolMirRuntimeOwnershipPlan);
    AGAINST(owner->values->ownership_variants, owner->values->ownership_variant_capacity, SolMirRuntimeOwnershipVariant);
    AGAINST(owner->values->owned_edges, owner->values->owned_edge_capacity, SolMirRuntimeOwnedEdge);
    AGAINST(owner->cleanup->events, owner->cleanup->event_capacity, SolMirRuntimeCleanupEvent);
    AGAINST(owner->cleanup->actions, owner->cleanup->action_capacity, SolMirRuntimeCleanupAction);
    AGAINST(owner->cleanup->transitions, owner->cleanup->transition_capacity, SolMirRuntimeCleanupTransition);
    AGAINST(owner->cleanup->supplemental_sites, owner->cleanup->supplemental_site_capacity, SolMirRuntimeCleanupSupplementalSite);
    AGAINST(owner->cleanup->drop_paths, owner->cleanup->drop_path_capacity, SolMirRuntimeCleanupDropPath);
    AGAINST(owner->host_abi->capabilities, owner->host_abi->capability_capacity, SolMirRuntimeHostCapabilityPlan);
    AGAINST(owner->host_abi->entry_roots, owner->host_abi->entry_root_capacity, SolMirRuntimeHostEntryRoot);
    AGAINST(owner->host_abi->operations, owner->host_abi->operation_capacity, SolMirRuntimeHostOperation);
    AGAINST(owner->host_abi->arguments, owner->host_abi->argument_capacity, SolMirRuntimeHostArgument);
    AGAINST(owner->host_abi->formals, owner->host_abi->formal_capacity, SolMirRuntimeHostFormal);
    AGAINST(owner->host_abi->shapes, owner->host_abi->shape_capacity, SolMirRuntimeHostShape);
    AGAINST(owner->host_abi->shape_cases, owner->host_abi->shape_case_capacity, SolMirRuntimeHostShapeCase);
    AGAINST(owner->host_abi->requirements, owner->host_abi->requirement_capacity, SolMirRuntimeHostRequirement);
    AGAINST(owner->handler_abi->frames, owner->handler_abi->frame_capacity, SolMirRuntimeHandlerFramePlan);
    AGAINST(owner->handler_abi->exit_markers, owner->handler_abi->exit_marker_capacity, SolMirRuntimeHandlerExitMarker);
    AGAINST(owner->handler_abi->cleanup_exits, owner->handler_abi->cleanup_exit_capacity, SolMirRuntimeHandlerCleanupExit);
    SolMirRuntimeTextGuard saved_guard = sol_mir_runtime_text_guard;
    void *saved_context = sol_mir_runtime_text_guard_context;
    sol_mir_runtime_text_guard = lowered_text_guard;
    sol_mir_runtime_text_guard_context = &context;
    SolMirRuntimeArenaVisit visited = sol_mir_runtime_visit_concrete_arenas(
        owner->conventions->concrete, lowered_alias_visit, &context);
    sol_mir_runtime_text_guard = saved_guard;
    sol_mir_runtime_text_guard_context = saved_context;
#undef AGAINST
    return context.exhausted ? SOL_MIR_RUNTIME_ARENA_VISIT_EXHAUSTED : visited;
}
static bool limits_ok(const SolMirRuntimeLoweredProgram *owner) {
#define LIMIT(member,type,singular) if (!validation_tick() || owner->singular##_count > owner->limits.max_##member) return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(LIMIT)
#undef LIMIT
    return owner->usage.owned_bytes <= owner->limits.max_owned_bytes && owner->usage.build_scratch_bytes <= owner->limits.max_build_scratch_bytes && owner->usage.build_work <= owner->limits.max_build_work && owner->usage.validation_scratch_bytes <= owner->limits.max_validation_scratch_bytes && owner->usage.validation_work <= owner->limits.max_validation_work && owner->usage.render_bytes <= owner->limits.max_render_bytes && owner->usage.render_scratch_bytes <= owner->limits.max_render_scratch_bytes;
}
static bool render_bounds(const SolMirRuntimeLoweredProgram *owner, size_t *bytes,
    size_t *scratch) {
    enum { render_line_capacity = 4096, render_header_capacity = 1024 };
    size_t rows = 0, relation_bytes, line_bytes;
#define ADD_ROWS(member,type,singular) if (!add_size(&rows, owner->singular##_count)) return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(ADD_ROWS)
#undef ADD_ROWS
    if (!mul_size(rows, render_line_capacity, &relation_bytes)
        || !add_size(&relation_bytes, render_header_capacity)
        || !mul_size(rows, render_line_capacity + sizeof(const char *), &line_bytes)) return false;
    *bytes = relation_bytes; *scratch = line_bytes;
    return true;
}
static bool validation_scratch_size(const SolMirRuntimeLoweredProgram *owner, size_t *out) {
    if (!validation_tick()) return false;
    size_t rows = 0, bytes;
#define ADD_ROWS(member,type,singular) if (!validation_tick() || !add_size(&rows, owner->singular##_count)) return false;
    SOL_MIR_RUNTIME_LOWERED_TABLES(ADD_ROWS)
#undef ADD_ROWS
    if (!mul_size(rows, 2, &bytes)) return false;
    *out = bytes; return true;
}
SolMirRuntimeLoweredProgramBuildOutcome sol_mir_runtime_lowered_program_internal_validate(const SolMirRuntimeLoweredProgram *owner, SolDiagnostics *diagnostics, bool measuring, size_t *out_work) {
    if (out_work) *out_work = 0;
    unsigned char *seen = NULL, *covered = NULL;
    if (!owner || !owner->authentication || !owner->conventions || !owner->values
        || !owner->cleanup || !owner->host_abi || !owner->handler_abi) {
        bad(diagnostics, "runtime lowered typed relation is malformed");
        return SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_INVALID_PREDECESSOR;
    }
    SolMirRuntimeLoweredWorkMeter meter = {owner->limits.max_validation_work, 0, false};
    SolMirRuntimeLoweredWorkMeter *previous_validation_meter = active_validation_meter;
    active_validation_meter = &meter;
    if (!raw_declared_tables(owner)) goto malformed;
    /* Each authenticated predecessor call is one validator operation.  Its
     * internal work remains owned and metered by that predecessor. */
    if (!validation_tick() || !sol_mir_runtime_conventions_validate(owner->conventions, diagnostics)
        || !validation_tick() || !sol_mir_runtime_values_validate(owner->values, diagnostics)
        || !validation_tick() || !sol_mir_runtime_cleanup_validate(owner->cleanup, diagnostics)
        || !validation_tick() || !sol_mir_runtime_host_abi_validate(owner->host_abi, diagnostics)
        || !validation_tick() || !sol_mir_runtime_handler_abi_validate(owner->handler_abi, diagnostics)) goto malformed;
    size_t validation_scratch_bytes;
    LoweredExpectedCardinalities cardinalities;
    if (!validation_tick() || owner->values->conventions != owner->conventions || owner->cleanup->conventions != owner->conventions || owner->cleanup->values != owner->values || owner->host_abi->conventions != owner->conventions || owner->host_abi->values != owner->values || owner->host_abi->cleanup != owner->cleanup || owner->handler_abi->conventions != owner->conventions || owner->handler_abi->values != owner->values || owner->handler_abi->cleanup != owner->cleanup || owner->handler_abi->host_abi != owner->host_abi || !expected_cardinalities(owner->conventions, owner->values, owner->cleanup, owner->host_abi, owner->handler_abi, &cardinalities) || !expected_owner_header(owner, &cardinalities)) goto malformed;
    SolMirRuntimeArenaVisit alias_visit = raw_owner_preflight(owner);
    if (alias_visit == SOL_MIR_RUNTIME_ARENA_VISIT_EXHAUSTED) goto resource_exhausted;
    size_t render_bytes, render_scratch_bytes;
    if (alias_visit != SOL_MIR_RUNTIME_ARENA_VISIT_OK || !raw_owner_slices(owner)
        || !validation_scratch_size(owner, &validation_scratch_bytes)
        || owner->usage.validation_scratch_bytes != validation_scratch_bytes
        || validation_scratch_bytes > owner->limits.max_validation_scratch_bytes
        || !render_bounds(owner, &render_bytes, &render_scratch_bytes)
        || owner->usage.render_bytes != render_bytes
        || owner->usage.render_scratch_bytes != render_scratch_bytes
        || render_bytes > owner->limits.max_render_bytes
        || render_scratch_bytes > owner->limits.max_render_scratch_bytes) goto malformed;
    uint64_t authentication = sol_mir_runtime_lowered_program_internal_seal(owner, NULL);
    if (!authentication || (!measuring && owner->authentication != authentication)) goto malformed;
    if (validation_scratch_bytes) {
        size_t rows = validation_scratch_bytes / 2;
        if (!validation_tick()) goto malformed;
        seen = sol_mir_runtime_lowered_program_internal_validation_scratch(rows, sizeof *seen);
        if (!validation_tick()) goto malformed;
        covered = sol_mir_runtime_lowered_program_internal_validation_scratch(rows, sizeof *covered);
        if (!seen || !covered) goto allocation_failed;
        if (!validation_tick()) goto malformed;
        memset(seen, 0, rows);
        if (!validation_tick()) goto malformed;
        memset(covered, 0, rows);
    }
    const SolMirMaterialization *materialization = &owner->conventions->concrete->materialization;
    const SolMirOperations *operations = &owner->conventions->concrete->operations;
    size_t cleanup_count, semantic_count;
    if (!expected_totals(owner->cleanup, operations, &cleanup_count, &semantic_count)) goto malformed;
    size_t grant_count = 0;
    for (size_t i = 0; i < owner->host_abi->requirement_count && validation_tick(); ++i) { bool prior = false; for (size_t j = 0; j < i && validation_tick(); ++j) if (owner->host_abi->requirements[j].entry == owner->host_abi->requirements[i].entry && owner->host_abi->requirements[j].root == owner->host_abi->requirements[i].root && owner->host_abi->requirements[j].operation == owner->host_abi->requirements[i].operation) prior = true; if (!prior) ++grant_count; }
    if (owner->image_instruction_count != materialization->instruction_count || owner->image_block_count != materialization->block_count || owner->image_edge_count != materialization->edge_count || owner->image_incoming_edge_count != materialization->edge_count || owner->image_outgoing_edge_count != materialization->edge_count || owner->image_terminator_count != materialization->block_count || owner->predicate_body_count != operations->predicate_body_count || owner->predicate_instruction_count != operations->predicate_instruction_count || owner->predicate_block_count != operations->predicate_block_count || owner->predicate_edge_count != operations->predicate_edge_count || owner->predicate_incoming_edge_count != operations->predicate_edge_count || owner->predicate_outgoing_edge_count != operations->predicate_edge_count || owner->predicate_terminator_count != operations->predicate_block_count || owner->semantic_plan_count != semantic_count || owner->call_count != owner->conventions->call_count || owner->signature_count != owner->conventions->signature_count || owner->import_count != owner->conventions->import_count || owner->recipe_count != owner->values->recipe_operation_count || owner->value_plan_count != owner->values->recipe_operation_count || owner->cleanup_failure_count != cleanup_count || owner->host_requirement_count != owner->host_abi->requirement_count || owner->host_grant_count != grant_count || owner->host_grant_count != owner->host_abi->usage.grants || owner->host_incidence_count != owner->host_abi->requirement_count || owner->handler_frame_count != owner->handler_abi->frame_count || owner->handler_marker_count != owner->handler_abi->exit_marker_count || owner->handler_exit_count != owner->handler_abi->cleanup_exit_count || owner->usage.erased_loops != materialization->loop_count || owner->usage.provenance_records != operations->provenance_count || owner->usage.demanded_semantic_plans != owner->semantic_plan_count) goto malformed;
    for (size_t i = 0; i < materialization->instruction_count && validation_tick(); ++i) {
        SolMirRuntimeLoweredDemandDescriptor expected;
        const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[i];
        const SolMirMaterializedInstruction *input = &materialization->instructions[i];
        if (!image_instruction_descriptor(input->kind, &expected)) goto malformed;
        SolMirRuntimeCleanupProducerKind producer;
        size_t event = image_instruction_event_producer(materialization, operations,
            owner->values, i, &producer) ? cleanup_event_for(owner->cleanup,
                SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_INSTRUCTION,
                image_for_instruction(materialization, i), input->block, i, true, producer)
            : SOL_MIR_RUNTIME_LOWERED_NONE;
        if (!present(row->state) || row->execution != execution_for(expected.runtime_class)
            || row->image != image_for_instruction(materialization, i) || row->instruction != i
            || row->block != input->block || row->kind != input->kind
            || row->runtime_class != expected.runtime_class || row->plan_family != expected.plan_family
            || row->facilities != image_instruction_facilities(operations, owner->values, i,
                input->kind, expected.facilities) || row->plan != image_instruction_semantic(operations, i)
            || row->cleanup_event != event || row->failure_site != cleanup_failure_for_event(owner->cleanup, event)) goto malformed;
    }
    for (size_t i = 0; i < materialization->block_count && validation_tick(); ++i) {
        SolMirRuntimeLoweredDemandDescriptor expected;
        const SolMirRuntimeLoweredImageBlock *block = &owner->image_blocks[i];
        const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[i];
        const SolMirMaterializedTerminator *input = &materialization->blocks[i].terminator;
        size_t call = SOL_MIR_RUNTIME_LOWERED_NONE;
        if (!image_terminator_descriptor(input->kind, &expected) || !present(block->state)
            || block->image != image_for_block(materialization, i) || block->block != i) goto malformed;
        if (input->kind == SOL_MIR_TERM_INVOKE) {
            call = image_call_for_block(owner->conventions, block->image, i);
            if (call == SOL_MIR_RUNTIME_LOWERED_NONE) goto malformed;
            expected.facilities |= import_for_call(owner->conventions, &owner->conventions->calls[call]) != SOL_MIR_RUNTIME_LOWERED_NONE ? SOL_MIR_RUNTIME_LOWERED_FACILITY_IMPORT : 0;
        }
        size_t event = cleanup_event_for(owner->cleanup,
            SOL_MIR_RUNTIME_CLEANUP_EVENT_IMAGE_TERMINATOR, block->image, i,
            SOL_MIR_RUNTIME_LOWERED_NONE, true, image_terminator_event_producer(input->kind));
        size_t failure = cleanup_failure_for_event(owner->cleanup, event);
        if (!present(row->state) || row->execution != execution_for(expected.runtime_class)
            || row->image != block->image || row->block != i || row->kind != input->kind
            || row->runtime_class != expected.runtime_class || row->plan_family != expected.plan_family
            || row->facilities != expected.facilities || row->plan != image_terminator_semantic(materialization, operations, i)
            || row->call != call || row->cleanup_event != event || row->failure_site != failure
            || (call != SOL_MIR_RUNTIME_LOWERED_NONE && (event == SOL_MIR_RUNTIME_LOWERED_NONE
                || failure != owner->conventions->calls[call].failure_site))) goto malformed;
    }
    for (size_t i = 0; i < operations->predicate_instruction_count && validation_tick(); ++i) {
        const SolMirPredicateInstruction *input = &operations->predicate_instructions[i];
        const SolMirRuntimeLoweredPredicateInstruction *row = &owner->predicate_instructions[i];
        SolMirRuntimeLoweredDemandDescriptor expected;
        if (input->block >= operations->predicate_block_count
            || !predicate_instruction_descriptor(input->kind, &expected)) goto malformed;
        size_t event = cleanup_event_for(owner->cleanup,
            SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_INSTRUCTION,
            operations->predicate_blocks[input->block].body, input->block, i, true,
            predicate_instruction_event_producer(input, owner->values));
        if (!present(row->state) || row->body != operations->predicate_blocks[input->block].body
            || row->instruction != i || row->block != input->block || row->kind != input->kind
            || row->runtime_class != expected.runtime_class || row->plan_family != expected.plan_family
            || row->facilities != (expected.facilities
                | (input->kind == SOL_MIR_PREDICATE_INST_BINARY && input->failures ? SOL_MIR_RUNTIME_LOWERED_FACILITY_FAILURE : 0)
                | (input->kind == SOL_MIR_PREDICATE_INST_CONSTRUCT && input->recipe < owner->values->allocation_plan_count && owner->values->allocation_plans[input->recipe].kind != SOL_MIR_RUNTIME_ALLOCATION_PLAN_NONE ? SOL_MIR_RUNTIME_LOWERED_FACILITY_ALLOCATION : 0)
                | (input->kind == SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT && copy_requires_runtime(operations->layout->representation->recipes[input->recipe].copy_kind) ? SOL_MIR_RUNTIME_LOWERED_FACILITY_COPY : 0))
            || row->plan != predicate_instruction_semantic(materialization, operations, i)
            || row->cleanup_event != event || row->failure_site != cleanup_failure_for_event(owner->cleanup, event)) goto malformed;
    }
    for (size_t i = 0; i < operations->predicate_block_count && validation_tick(); ++i) {
        const SolMirPredicateBlock *input = &operations->predicate_blocks[i];
        const SolMirRuntimeLoweredPredicateTerminator *row = &owner->predicate_terminators[i];
        SolMirRuntimeLoweredDemandDescriptor expected;
        size_t call = SOL_MIR_RUNTIME_LOWERED_NONE;
        if (!predicate_terminator_descriptor(input->terminator.kind, &expected)
            || !present(owner->predicate_blocks[i].state)
            || owner->predicate_blocks[i].body != input->body || owner->predicate_blocks[i].block != i) goto malformed;
        if (input->terminator.kind == SOL_MIR_PREDICATE_TERM_INVOKE) {
            call = predicate_call_for_block(owner->conventions, input->body, i);
            if (call == SOL_MIR_RUNTIME_LOWERED_NONE) goto malformed;
            expected.facilities |= import_for_call(owner->conventions, &owner->conventions->calls[call]) != SOL_MIR_RUNTIME_LOWERED_NONE ? SOL_MIR_RUNTIME_LOWERED_FACILITY_IMPORT : 0;
        }
        size_t event = cleanup_event_for(owner->cleanup,
            SOL_MIR_RUNTIME_CLEANUP_EVENT_PREDICATE_TERMINATOR, input->body, i,
            SOL_MIR_RUNTIME_LOWERED_NONE, true,
            predicate_terminator_event_producer(&input->terminator));
        size_t failure = cleanup_failure_for_event(owner->cleanup, event);
        if (!present(row->state) || row->body != input->body || row->block != i
            || row->kind != input->terminator.kind || row->runtime_class != expected.runtime_class
            || row->plan_family != expected.plan_family || row->facilities != expected.facilities
            || row->plan != predicate_terminator_semantic(materialization, operations, i)
            || row->call != call || row->cleanup_event != event || row->failure_site != failure
            || (call != SOL_MIR_RUNTIME_LOWERED_NONE && (event == SOL_MIR_RUNTIME_LOWERED_NONE
                || failure != owner->conventions->calls[call].failure_site))) goto malformed;
    }
    DemandReader demand_reader = {owner, 0};
    for (size_t i = 0; i < materialization->instruction_count && validation_tick(); ++i) { const SolMirRuntimeLoweredImageInstruction *row = &owner->image_instructions[i]; if (row->demanded_recipes.offset != demand_reader.count || !expected_image_instruction_demands(&demand_reader, materialization, operations, owner->values, owner->cleanup, i) || row->demanded_recipes.count != demand_reader.count - row->demanded_recipes.offset) goto malformed; }
    for (size_t i = 0; i < materialization->block_count && validation_tick(); ++i) { const SolMirRuntimeLoweredImageTerminator *row = &owner->image_terminators[i]; if (row->demanded_recipes.offset != demand_reader.count || !expected_image_terminator_demands(&demand_reader, owner->conventions, owner->values, materialization, operations, row->image, i) || row->demanded_recipes.count != demand_reader.count - row->demanded_recipes.offset) goto malformed; }
    for (size_t i = 0; i < operations->predicate_instruction_count && validation_tick(); ++i) { const SolMirRuntimeLoweredPredicateInstruction *row = &owner->predicate_instructions[i]; if (row->demanded_recipes.offset != demand_reader.count || !expected_predicate_instruction_demands(&demand_reader, operations, owner->values, i) || row->demanded_recipes.count != demand_reader.count - row->demanded_recipes.offset) goto malformed; }
    for (size_t i = 0; i < operations->predicate_block_count && validation_tick(); ++i) { const SolMirRuntimeLoweredPredicateTerminator *row = &owner->predicate_terminators[i]; if (row->demanded_recipes.offset != demand_reader.count || !expected_predicate_terminator_demands(&demand_reader, owner->conventions, owner->values, operations, row->body, i) || row->demanded_recipes.count != demand_reader.count - row->demanded_recipes.offset) goto malformed; }
    if (demand_reader.count != owner->recipe_demand_count) goto malformed;
    for (size_t i = 0; i < operations->provenance_count && validation_tick(); ++i) if (!provenance_descriptor(operations->provenance[i].kind, NULL)) goto malformed;
    for (size_t i = 0; i < owner->semantic_plan_count && validation_tick(); ++i) { SolMirRuntimeLoweredSemanticPlan expected; if (!semantic_expected(operations, materialization, i, &expected) || owner->semantic_plans[i].state != expected.state || owner->semantic_plans[i].arena != expected.arena || owner->semantic_plans[i].runtime_class != expected.runtime_class || owner->semantic_plans[i].plan_family != expected.plan_family || owner->semantic_plans[i].plan != expected.plan || owner->semantic_plans[i].facilities != expected.facilities || owner->semantic_plans[i].producer_kind != expected.producer_kind || owner->semantic_plans[i].producer != expected.producer) goto malformed; }
    for (size_t i = 0; i < materialization->edge_count && validation_tick(); ++i) { size_t source, ordinal; const SolMirRuntimeLoweredImageEdge *row = &owner->image_edges[i]; if (!image_edge_source(materialization, i, &source, &ordinal) || !present(row->state) || row->image != image_for_block(materialization, materialization->edges[i].block) || row->edge != i || row->target != materialization->edges[i].block || row->source != source || row->ordinal != ordinal) goto malformed; }
    size_t image_incoming = 0, image_outgoing = 0; for (size_t block = 0; block < materialization->block_count && validation_tick(); ++block) { const SolMirRuntimeLoweredImageBlock *row = &owner->image_blocks[block]; if (row->incoming_edges.offset != image_incoming || row->outgoing_edges.offset != image_outgoing) goto malformed; for (size_t edge = 0; edge < materialization->edge_count && validation_tick(); ++edge) { const SolMirRuntimeLoweredImageEdge *relation = &owner->image_edges[edge]; if (relation->target == block) { const SolMirRuntimeLoweredImageBlockEdge *incidence = &owner->image_incoming_edges[image_incoming++]; if (!present(incidence->state) || incidence->image != relation->image || incidence->block != block || incidence->edge != edge || incidence->ordinal != relation->ordinal) goto malformed; } if (relation->source == block) { const SolMirRuntimeLoweredImageBlockEdge *incidence = &owner->image_outgoing_edges[image_outgoing++]; if (!present(incidence->state) || incidence->image != relation->image || incidence->block != block || incidence->edge != edge || incidence->ordinal != relation->ordinal) goto malformed; } } if (row->incoming_edges.count != image_incoming - row->incoming_edges.offset || row->outgoing_edges.count != image_outgoing - row->outgoing_edges.offset) goto malformed; } if (image_incoming != owner->image_incoming_edge_count || image_outgoing != owner->image_outgoing_edge_count) goto malformed;
    for (size_t i = 0; i < operations->predicate_body_count && validation_tick(); ++i) { const SolMirPredicateBody *input = &operations->predicate_bodies[i]; const SolMirRuntimeLoweredPredicateBody *row = &owner->predicate_bodies[i]; if (!present(row->state) || row->body != i || row->owner_kind != input->owner_kind || row->image != input->instance || row->import_id != input->import || row->context != input->context || row->phase != input->phase || row->outcome != input->outcome || row->blocks.offset != input->blocks.offset || row->blocks.count != input->blocks.count || row->entry != input->entry || row->output_recipe != input->output_recipe || row->refinement_self_recipe != input->refinement_self_recipe) goto malformed; }
    for (size_t i = 0; i < operations->predicate_edge_count && validation_tick(); ++i) { size_t source, ordinal; const SolMirPredicateEdge *input = &operations->predicate_edges[i]; const SolMirRuntimeLoweredPredicateEdge *row = &owner->predicate_edges[i]; if (input->source >= operations->predicate_block_count || input->target >= operations->predicate_block_count || !predicate_edge_source(operations, i, &source, &ordinal) || source != input->source || !present(row->state) || row->body != operations->predicate_blocks[source].body || row->edge != i || row->target != input->target || row->source != source || row->ordinal != ordinal) goto malformed; }
    size_t predicate_incoming = 0, predicate_outgoing = 0; for (size_t block = 0; block < operations->predicate_block_count && validation_tick(); ++block) { const SolMirRuntimeLoweredPredicateBlock *row = &owner->predicate_blocks[block]; if (row->incoming_edges.offset != predicate_incoming || row->outgoing_edges.offset != predicate_outgoing) goto malformed; for (size_t edge = 0; edge < operations->predicate_edge_count && validation_tick(); ++edge) { const SolMirRuntimeLoweredPredicateEdge *relation = &owner->predicate_edges[edge]; if (relation->target == block) { const SolMirRuntimeLoweredPredicateBlockEdge *incidence = &owner->predicate_incoming_edges[predicate_incoming++]; if (!present(incidence->state) || incidence->body != relation->body || incidence->block != block || incidence->edge != edge || incidence->ordinal != relation->ordinal) goto malformed; } if (relation->source == block) { const SolMirRuntimeLoweredPredicateBlockEdge *incidence = &owner->predicate_outgoing_edges[predicate_outgoing++]; if (!present(incidence->state) || incidence->body != relation->body || incidence->block != block || incidence->edge != edge || incidence->ordinal != relation->ordinal) goto malformed; } } if (row->incoming_edges.count != predicate_incoming - row->incoming_edges.offset || row->outgoing_edges.count != predicate_outgoing - row->outgoing_edges.offset) goto malformed; } if (predicate_incoming != owner->predicate_incoming_edge_count || predicate_outgoing != owner->predicate_outgoing_edge_count) goto malformed;
    for (size_t i = 0; i < owner->call_count && validation_tick(); ++i) { const SolMirRuntimeCall *input = &owner->conventions->calls[i]; const SolMirRuntimeLoweredCall *row = &owner->calls[i]; if (!present(row->state) || row->call != i || row->signature != input->signature || row->owner_kind != input->owner_kind || row->image != input->image || row->body != input->predicate || row->block != input->block || row->call_kind != input->call_kind || row->target_kind != input->target_kind || row->internal != input->internal || row->host != input->host || row->table != input->table || row->callee.kind != input->callee.kind || row->callee.id != input->callee.id || row->operands.offset != input->operands.offset || row->operands.count != input->operands.count || row->result.kind != input->result.kind || row->result.id != input->result.id || row->normal_edge != input->normal_edge || row->failure_edge != input->failure_edge || row->writebacks.offset != input->writebacks.offset || row->writebacks.count != input->writebacks.count || row->import_id != import_for_call(owner->conventions, input) || row->bound_environment_import != bound_environment_import_for_call(owner->conventions, input) || row->entry != entry_for_call(owner->conventions, input) || row->failure_site != input->failure_site) goto malformed; }
    for (size_t i = 0; i < owner->signature_count && validation_tick(); ++i) if (!present(owner->signatures[i].state) || owner->signatures[i].signature != i) goto malformed;
    for (size_t i = 0; i < owner->import_count && validation_tick(); ++i) if (!present(owner->imports[i].state) || owner->imports[i].import_id != i) goto malformed;
    for (size_t i = 0; i < owner->recipe_count && validation_tick(); ++i) { const SolMirRuntimeRecipeOperations *input = &owner->values->recipe_operations[i]; SolMirRuntimeLoweredState expected_recipe_state = input->demanded_operations ? SOL_MIR_RUNTIME_LOWERED_PRESENT : SOL_MIR_RUNTIME_LOWERED_NOT_DEMANDED; uint32_t expected_value_facilities=value_facilities(owner->values,i); uint32_t expected_value_demand=value_demand(owner->conventions,owner->values,owner->cleanup,operations,i)&expected_value_facilities; SolMirRuntimeLoweredState expected_value_state = expected_value_demand ? SOL_MIR_RUNTIME_LOWERED_PRESENT : SOL_MIR_RUNTIME_LOWERED_NOT_DEMANDED; const SolMirRuntimeLoweredRecipe *recipe = &owner->recipes[i]; const SolMirRuntimeLoweredValuePlan *value = &owner->value_plans[i]; if (recipe->state != expected_recipe_state || recipe->recipe != input->recipe || recipe->demanded_operations != input->demanded_operations || recipe->operations != i || recipe->create_import != input->create_import || recipe->copy_import != input->copy_import || recipe->drop_import != input->drop_import || recipe->equal_import != input->equal_import || recipe->facilities != recipe_facilities(input->demanded_operations) || recipe->allocation != owner->values->allocation_plans[i].kind || recipe->copy != owner->values->copy_plans[i].classification || recipe->equality != owner->values->equality_plans[i].classification || recipe->ownership != owner->values->ownership_plans[i].classification || recipe->host_result != owner->values->host_result_plans[i].classification || value->state != expected_value_state || value->recipe != input->recipe || value->operations != i || value->facilities != expected_value_facilities || value->demanded_facilities != expected_value_demand || value->allocation != owner->values->allocation_plans[i].kind || value->copy != owner->values->copy_plans[i].classification || value->equality != owner->values->equality_plans[i].classification || value->ownership != owner->values->ownership_plans[i].classification || value->host_result != owner->values->host_result_plans[i].classification) goto malformed; }
    for (size_t i = 0; i < owner->host_requirement_count && validation_tick(); ++i) {
        const SolMirRuntimeHostRequirement *input = &owner->host_abi->requirements[i];
        const SolMirRuntimeLoweredHostRequirement *row = &owner->host_requirements[i];
        if (input->call >= owner->conventions->call_count || input->operation >= owner->host_abi->operation_count)
            goto malformed;
        const SolMirRuntimeHostOperation *operation = &owner->host_abi->operations[input->operation];
        const SolMirRuntimeCall *call = &owner->conventions->calls[input->call];
        if (!present(row->state) || row->requirement != i || row->entry != input->entry
            || row->root != input->root || row->operation != input->operation
            || row->call != input->call || row->event != input->event
            || row->transition != input->failure_transition || row->import_id != operation->import_id
            || row->signature != operation->signature || call->signature != operation->signature)
            goto malformed;
    }
    size_t expected_grant = 0, expected_incidence = 0;
    for (size_t requirement = 0; requirement < owner->host_requirement_count && validation_tick(); ++requirement) {
        const SolMirRuntimeHostRequirement *input = &owner->host_abi->requirements[requirement];
        bool prior = false;
        for (size_t i = 0; i < requirement && validation_tick(); ++i) {
            const SolMirRuntimeHostRequirement *earlier = &owner->host_abi->requirements[i];
            if (earlier->entry == input->entry && earlier->root == input->root
                && earlier->operation == input->operation) { prior = true; break; }
        }
        if (prior || expected_grant >= owner->host_grant_count) continue;
        const SolMirRuntimeLoweredHostGrant *grant = &owner->host_grants[expected_grant];
        if (!present(grant->state) || grant->entry != input->entry || grant->root != input->root
            || grant->operation != input->operation || grant->incidences.offset != expected_incidence)
            goto malformed;
        size_t count = 0;
        for (size_t i = 0; i < owner->host_requirement_count && validation_tick(); ++i) {
            const SolMirRuntimeHostRequirement *member = &owner->host_abi->requirements[i];
            if (member->entry != input->entry || member->root != input->root
                || member->operation != input->operation) continue;
            if (expected_incidence + count >= owner->host_incidence_count) goto malformed;
            const SolMirRuntimeLoweredHostIncidence *relation =
                &owner->host_incidences[expected_incidence + count];
            if (!present(relation->state) || relation->requirement != i
                || relation->grant != expected_grant) goto malformed;
            ++count;
        }
        if (grant->incidences.count != count) goto malformed;
        expected_incidence += count;
        ++expected_grant;
    }
    if (expected_grant != owner->host_grant_count
        || expected_incidence != owner->host_incidence_count) goto malformed;
    size_t cleanup_row = 0;
    for (size_t i = 0; i < owner->cleanup->event_count && validation_tick(); ++i, ++cleanup_row) {
        const SolMirRuntimeCleanupEvent *input = &owner->cleanup->events[i];
        const SolMirRuntimeLoweredCleanupFailure *row = &owner->cleanup_failures[cleanup_row];
        if (!present(row->state) || row->kind != SOL_MIR_RUNTIME_LOWERED_CLEANUP_EVENT
            || row->record != i || row->event != i || row->event_kind != input->kind
            || row->origin != input->origin || row->owner != input->owner || row->block != input->block
            || row->operation != input->operation || row->producer != input->producer
            || row->inherited_failure_site != input->inherited_failure_site
            || row->supplemental_site != input->supplemental_site
            || row->actions.offset != input->actions.offset || row->actions.count != input->actions.count
            || row->transitions.offset != input->transitions.offset || row->transitions.count != input->transitions.count
            || row->captures_failure_detail != input->captures_failure_detail
            || row->capture_detail_kind != input->capture_detail_kind) goto malformed;
    }
    for (size_t i = 0; i < owner->cleanup->action_count && validation_tick(); ++i, ++cleanup_row) {
        const SolMirRuntimeCleanupAction *input = &owner->cleanup->actions[i];
        const SolMirRuntimeLoweredCleanupFailure *row = &owner->cleanup_failures[cleanup_row];
        if (!present(row->state) || row->kind != SOL_MIR_RUNTIME_LOWERED_CLEANUP_ACTION
            || row->record != i || row->action != i || row->action_kind != input->kind
            || row->action_flags != input->flags || row->target != input->target
            || row->recipe != input->recipe || row->drop_path != input->drop_path
            || row->frame != handler_frame_for_cleanup_action(owner->handler_abi, i)) goto malformed;
    }
    for (size_t i = 0; i < owner->cleanup->transition_count && validation_tick(); ++i, ++cleanup_row) {
        const SolMirRuntimeCleanupTransition *input = &owner->cleanup->transitions[i];
        const SolMirRuntimeLoweredCleanupFailure *row = &owner->cleanup_failures[cleanup_row];
        if (!present(row->state) || row->kind != SOL_MIR_RUNTIME_LOWERED_CLEANUP_TRANSITION
            || row->record != i || row->event != input->event || row->transition != i
            || row->continuation != input->continuation || row->actions.offset != input->actions.offset
            || row->actions.count != input->actions.count || row->source_edge != input->source_edge
            || row->destination != input->destination || row->primary_failure_wins != input->primary_failure_wins
            || row->failure_source != input->failure_source || row->failure_site != input->failure_site
            || row->failure_mask != input->failure_mask || row->edge_role != input->edge_role
            || row->outcome != input->outcome || row->contract_phase != input->contract_phase
            || row->contract_outcome != input->contract_outcome) goto malformed;
    }
    for (size_t i = 0; i < owner->cleanup->supplemental_site_count && validation_tick(); ++i, ++cleanup_row) {
        const SolMirRuntimeCleanupSupplementalSite *input = &owner->cleanup->supplemental_sites[i];
        const SolMirRuntimeLoweredCleanupFailure *row = &owner->cleanup_failures[cleanup_row];
        if (!present(row->state) || row->kind != SOL_MIR_RUNTIME_LOWERED_CLEANUP_SUPPLEMENTAL_SITE
            || row->record != i || row->event != input->event
            || row->failure_source != SOL_MIR_RUNTIME_CLEANUP_FAILURE_SOURCE_SUPPLEMENTAL_P33
            || row->failure_site != i || row->failure_mask != input->allowed_codes) goto malformed;
    }
    for (size_t i = 0; i < owner->cleanup->drop_path_count && validation_tick(); ++i, ++cleanup_row) {
        const SolMirRuntimeCleanupDropPath *input = &owner->cleanup->drop_paths[i];
        const SolMirRuntimeLoweredCleanupFailure *row = &owner->cleanup_failures[cleanup_row];
        if (!present(row->state) || row->kind != SOL_MIR_RUNTIME_LOWERED_CLEANUP_DROP_PATH
            || row->record != i || row->drop_path != i || row->drop_root != input->root
            || row->drop_place != input->place || row->drop_holes.offset != input->holes.offset
            || row->drop_holes.count != input->holes.count || row->recipe != input->recipe
            || row->drop_liveness != input->liveness) goto malformed;
    }
    if (cleanup_row != owner->cleanup_failure_count) goto malformed;
    for (size_t i = 0; i < owner->handler_frame_count && validation_tick(); ++i) {
        const SolMirRuntimeHandlerFramePlan *input = &owner->handler_abi->frames[i];
        const SolMirRuntimeLoweredHandlerFrame *row = &owner->handler_frames[i];
        if (!present(row->state) || row->frame != i || row->handler != input->handler
            || row->parent != input->parent || row->source_binding != input->source_binding
            || row->source_operation.target_kind != input->source_operation.target_kind
            || row->source_operation.instance != input->source_operation.instance
            || row->source_operation.import != input->source_operation.import
            || row->source_operation.receiver != input->source_operation.receiver
            || row->source_operation.root != input->source_operation.root
            || row->source_operation.effects != input->source_operation.effects
            || row->source_signature != input->source_signature || row->root_match != input->root_match
            || row->root != input->authority_root || row->effects != input->effects
            || row->provider_binding != input->provider_binding || row->provider_place != input->provider_place
            || row->provider_recipe != input->provider_recipe || row->provider_access != input->provider_access
            || row->provider != input->provider_internal || row->signature != input->provider_signature
            || row->enter != input->enter_marker) goto malformed;
    }
    for (size_t i = 0; i < owner->handler_marker_count && validation_tick(); ++i) {
        const SolMirRuntimeHandlerExitMarker *input = &owner->handler_abi->exit_markers[i];
        const SolMirRuntimeLoweredHandlerMarker *row = &owner->handler_markers[i];
        if (!present(row->state) || row->frame != input->frame || row->marker != input->instruction
            || row->image != input->image || row->block != input->block) goto malformed;
    }
    for (size_t i = 0; i < owner->handler_exit_count && validation_tick(); ++i) {
        const SolMirRuntimeHandlerCleanupExit *input = &owner->handler_abi->cleanup_exits[i];
        const SolMirRuntimeLoweredHandlerExit *row = &owner->handler_exits[i];
        if (!present(row->state) || row->frame != input->frame || row->action != input->action
            || row->transition != input->transition || !row->alternative_one_pop
            || handler_frame_for_cleanup_action(owner->handler_abi, row->action) != row->frame) goto malformed;
    }
    size_t owned_bytes = 0, bytes = 0;
#define USAGE_COUNT(member,type,singular) if (owner->usage.member != owner->singular##_count) goto malformed;
    SOL_MIR_RUNTIME_LOWERED_TABLES(USAGE_COUNT)
#undef USAGE_COUNT
#define BYTES(member,type,singular) if (!mul_size(owner->singular##_count, sizeof(type), &bytes) || !add_size(&owned_bytes, bytes)) goto malformed;
    SOL_MIR_RUNTIME_LOWERED_TABLES(BYTES)
#undef BYTES
    if (!validation_tick() || owner->usage.owned_bytes != owned_bytes || !limits_ok(owner)
        || meter.exhausted) goto malformed;
    if (!validation_tick() || (!measuring && owner->usage.validation_work != meter.used)
        || meter.exhausted) goto malformed;
    free(seen); free(covered);
    if (out_work) *out_work = meter.used;
    active_validation_meter = previous_validation_meter;
    return SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_SUCCEEDED;
allocation_failed:
    free(seen); free(covered);
    active_validation_meter = previous_validation_meter;
    return SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_ALLOCATION_FAILED;
malformed:
    if (meter.exhausted) goto resource_exhausted;
    free(seen); free(covered);
    active_validation_meter = previous_validation_meter;
    bad(diagnostics, "runtime lowered typed relation is malformed");
    return SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_INVALID_PREDECESSOR;
resource_exhausted:
    free(seen); free(covered);
    active_validation_meter = previous_validation_meter;
    return SOL_MIR_RUNTIME_LOWERED_PROGRAM_BUILD_RESOURCE_EXHAUSTED;
}
