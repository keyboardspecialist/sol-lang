#ifndef SOL_MIR_EVAL_H
#define SOL_MIR_EVAL_H

#include "sol/interpreter.h"
#include "sol/mir.h"

typedef enum {
    SOL_MIR_CALLEE_FOUND,
    SOL_MIR_CALLEE_NO_BODY,
    SOL_MIR_CALLEE_UNAVAILABLE,
} SolMirCalleeStatus;

/*
 * Trusted borrowed-MIR provider. Every SOL_MIR_CALLEE_FOUND pointer, the
 * SolMir object, and all storage reachable from it must remain immutable and
 * valid until the enclosing sol_mir_evaluate call returns. Found pointers for
 * different callables must not alias mutable scratch storage. The evaluator
 * validates and caches the borrowed pointer, but does not deep-copy it.
 */
typedef SolMirCalleeStatus (*SolMirCalleeProvider)(
    void *context,
    SolIrCallableId callable,
    const SolMir **mir
);

typedef enum {
    SOL_MIR_TRACE_CALL,
    SOL_MIR_TRACE_BLOCK,
    SOL_MIR_TRACE_INSTRUCTION,
    SOL_MIR_TRACE_TERMINATOR,
    SOL_MIR_TRACE_EDGE,
    SOL_MIR_TRACE_HOST,
    SOL_MIR_TRACE_DROP,
    SOL_MIR_TRACE_RETURN,
    SOL_MIR_TRACE_FAILURE,
} SolMirTraceEventKind;

#define SOL_MIR_TRACE_INSTRUCTION_KIND_NONE ((SolMirInstructionKind)-1)

typedef struct {
    size_t ordinal;
    SolMirTraceEventKind kind;
    size_t call_depth;
    SolIrCallableId callable;
    SolMirBlockId block;
    SolMirInstructionId instruction;
    SolMirInstructionKind instruction_kind;
    SolMirTerminatorKind terminator_kind;
    SolMirBlockId edge_target;
    SolIrExpressionId source_expression;
    SolIrCallableId operation;
    SolIrLocalId local;
    SolInterpreterCode code;
    SolSpan span;
} SolMirTraceEvent;

typedef void (*SolMirTraceObserver)(
    void *context,
    const SolMirTraceEvent *event
);

typedef struct {
    size_t calls;
    size_t blocks;
    size_t instructions;
    size_t terminators;
    size_t edges;
    size_t host_dispatches;
    size_t temporary_drops;
} SolMirExecutionCounts;

typedef struct {
    const SolIr *ir;
    const SolMir *entry;
    const SolInterpreterValue *arguments;
    size_t argument_count;
    const SolIrTypeId *type_arguments;
    size_t type_argument_count;
    SolInterpreterEvidence evidence;
    SolInterpreterContractPolicy contracts;
    /*
     * Deterministic MIR-local units. Differential callers should use generous
     * limits. Unstable raw-request safety ceilings, including bounded value
     * inspection metadata, are also reported as resource-limit diagnostics.
     */
    SolInterpreterLimits limits;
    size_t trace_events;
    SolInterpreterHostOperation host_operation;
    SolInterpreterHostAllows host_allows;
    void *host_context;
    SolInterpreterCleanupObserver cleanup_observer;
    void *cleanup_context;
    SolMirCalleeProvider callee_provider;
    void *callee_context;
    SolMirTraceObserver trace_observer;
    void *trace_context;
} SolMirEvaluateRequest;

typedef struct {
    SolInterpreterValue value;
    SolInterpreterDiagnostic diagnostic;
    /* MIR-engine-local usage; not numerically comparable across evaluators. */
    SolInterpreterLimits used;
    size_t cleanup_actions;
    SolMirExecutionCounts executed;
    size_t trace_count;
    bool trace_truncated;
} SolMirEvaluateResult;

void sol_mir_evaluate_result_init(SolMirEvaluateResult *result);
void sol_mir_evaluate_result_free(SolMirEvaluateResult *result);
bool sol_mir_evaluate(
    const SolMirEvaluateRequest *request,
    SolMirEvaluateResult *result
);

#endif
