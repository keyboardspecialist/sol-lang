#ifndef SOL_MIR_H
#define SOL_MIR_H

#include "sol/ir.h"

#include <stdio.h>

/* Unstable callable-scoped CFG MIR checkpoint; not a serialized or target ABI. */
typedef size_t SolMirBlockId;
typedef size_t SolMirInstructionId;
typedef size_t SolMirValueId;
typedef size_t SolMirLoopId;
typedef size_t SolMirTemporaryId;

#define SOL_MIR_NONE SIZE_MAX

typedef struct {
    SolIrLocalId local;
    /* SOL_IR_NONE denotes a synthetic whole-local place. */
    SolIrPlaceId source_place;
} SolMirPlace;

typedef struct {
    size_t offset;
    size_t count;
} SolMirSlice;

typedef enum {
    SOL_MIR_LOWER_SUCCEEDED,
    SOL_MIR_LOWER_UNSUPPORTED,
    SOL_MIR_LOWER_FAILED,
} SolMirLowerOutcome;

typedef enum {
    SOL_MIR_VALUE_BLOCK_PARAMETER,
    SOL_MIR_VALUE_INSTRUCTION,
    /* Available only on its designated edge from the defining terminator. */
    SOL_MIR_VALUE_TERMINATOR,
} SolMirValueKind;

typedef struct {
    SolMirValueKind kind;
    SolIrTypeId type;
    SolMirBlockId block;
    size_t definition;
    SolIrExpressionId source_expression;
    SolSpan span;
} SolMirValue;

typedef enum {
    SOL_MIR_INST_CONST_INT64,
    SOL_MIR_INST_CONST_BOOL,
    SOL_MIR_INST_CONST_TEXT,
    SOL_MIR_INST_CONST_UNIT,
    SOL_MIR_INST_PARAMETER_LIVE,
    SOL_MIR_INST_STORAGE_LIVE,
    SOL_MIR_INST_DROP_IF_INITIALIZED,
    SOL_MIR_INST_STORAGE_DEAD,
    SOL_MIR_INST_LOAD_COPY,
    SOL_MIR_INST_LOAD_MOVE,
    /* Reads an initialized assignment target before evaluating the RHS. */
    SOL_MIR_INST_LOAD_UPDATE,
    SOL_MIR_INST_STORE,
    SOL_MIR_INST_UNARY,
    SOL_MIR_INST_BINARY,
    SOL_MIR_INST_COMPOUND_UPDATE,
    SOL_MIR_INST_REGION_ENTER,
    SOL_MIR_INST_REGION_EXIT,
    SOL_MIR_INST_TEMPORARY_INIT,
    SOL_MIR_INST_TEMPORARY_DROP,
    SOL_MIR_INST_EXPRESSION_RESULT,
    SOL_MIR_INST_PATTERN_TEST,
    SOL_MIR_INST_PATTERN_VALUE,
    SOL_MIR_INST_MATCH_ARM,
    SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED,
    SOL_MIR_INST_HANDLER_ENTER,
    SOL_MIR_INST_HANDLER_EXIT,
    SOL_MIR_INST_CONSTRUCT,
    /* Captures one infallible logical copy in the callable contract envelope.
       Every terminal exit destroys all captured snapshots implicitly. */
    SOL_MIR_INST_CAPTURE_SNAPSHOT,
} SolMirInstructionKind;

typedef enum {
    SOL_MIR_CONSTRUCT_RECORD,
    SOL_MIR_CONSTRUCT_CAPABILITY,
    SOL_MIR_CONSTRUCT_TUPLE,
    SOL_MIR_CONSTRUCT_ENUM,
    SOL_MIR_CONSTRUCT_OPTION_NONE,
    SOL_MIR_CONSTRUCT_OPTION_SOME,
    SOL_MIR_CONSTRUCT_RESULT_OK,
    SOL_MIR_CONSTRUCT_RESULT_ERR,
    SOL_MIR_CONSTRUCT_DISTINCT,
} SolMirConstructKind;

typedef struct {
    SolMirInstructionKind kind;
    SolMirBlockId block;
    SolMirValueId result;
    SolIrTypeId type;
    SolIrExpressionId source_expression;
    SolSpan span;
    union {
        int64_t integer;
        bool boolean;
        SolIrLocalId local;
        SolMirPlace place;
        struct {
            SolIrStatementId statement;
            SolMirPlace place;
        } update_load;
        struct {
            SolTokenKind operator_kind;
            SolMirValueId operand;
        } unary;
        struct {
            SolTokenKind operator_kind;
            SolMirValueId left;
            SolMirValueId right;
        } binary;
        struct {
            SolIrStatementId statement;
            SolTokenKind operator_kind;
            SolMirPlace place;
            SolMirTemporaryId previous;
            SolMirValueId right;
        } compound_update;
        struct {
            SolMirPlace place;
            SolMirValueId value;
        } store;
        SolMirValueId operand;
        SolIrStatementId region;
        struct {
            SolMirTemporaryId temporary;
            SolMirValueId value;
        } temporary_init;
        struct {
            SolMirTemporaryId temporary;
            size_t preserve_depth;
        } temporary_drop;
        struct {
            SolIrExpressionId match_expression;
            SolIrArmId arm;
            size_t arm_ordinal;
            SolIrPatternId pattern;
            SolMirTemporaryId scrutinee;
        } pattern;
        struct {
            SolMirConstructKind kind;
            SolIrDefinitionId definition;
            SolIrVariantId variant;
            SolMirSlice operands;
            /* Exact authority provenance remains owned by the source IR. */
            SolIrSlice capability_roots;
            SolIrSlice operation_roots;
        } construct;
        SolIrSnapshotId snapshot;
    } as;
} SolMirInstruction;

typedef struct {
    SolIrTypeId type;
    SolIrExpressionId source_expression;
    SolSpan span;
} SolMirTemporary;

typedef struct {
    size_t formal;
    SolIrExpressionId source_expression;
    SolMirTemporaryId temporary;
} SolMirConstructOperand;

typedef struct {
    size_t formal;
    SolAccessMode access;
    SolIrExpressionId source_expression;
    /* Owned operands carry a temporary; borrows carry a local-rooted place.
       Exclusive places write back only when the invoke takes its normal edge. */
    SolMirTemporaryId temporary;
    SolIrPlaceId place;
} SolMirCallArgument;

typedef struct {
    SolIrStatementId statement;
    SolMirLoopId parent;
    SolMirBlockId preheader;
    SolMirBlockId header;
    SolMirBlockId condition;
    SolMirBlockId body;
    SolMirBlockId backedge;
    SolMirBlockId exit;
    /* Proof-only owning-IR metadata; expressions are not executed by MIR. */
    SolIrSlice obligations;
    SolSpan span;
} SolMirLoop;

typedef struct {
    SolMirBlockId block;
    SolMirSlice arguments;
} SolMirEdge;

typedef enum {
    SOL_MIR_TERM_INVALID,
    SOL_MIR_TERM_GOTO,
    SOL_MIR_TERM_BRANCH,
    SOL_MIR_TERM_RETURN,
    SOL_MIR_TERM_PANIC,
    SOL_MIR_TERM_INVOKE,
    SOL_MIR_TERM_RESUME_FAILURE,
    SOL_MIR_TERM_UNREACHABLE,
    SOL_MIR_TERM_BREAK,
    SOL_MIR_TERM_CONTINUE,
    SOL_MIR_TERM_CHECK_REFINED,
    SOL_MIR_TERM_MATCH_FAILURE,
    SOL_MIR_TERM_PROPAGATE,
    SOL_MIR_TERM_CHECK_CONTRACT,
    SOL_MIR_TERM_CONTRACT_VIOLATION,
} SolMirTerminatorKind;

typedef struct {
    SolMirTerminatorKind kind;
    SolSpan span;
    union {
        SolMirEdge go_to;
        struct {
            SolMirValueId condition;
            SolMirEdge true_edge;
            SolMirEdge false_edge;
        } branch;
        struct {
            SolIrExpressionId source_expression;
            SolIrCallKind kind;
            SolIrCallableId callable;
            /* Exact instantiated metadata remains owned by the source IR. */
            SolIrSlice type_arguments;
            SolIrSlice effects;
            SolIrEffectParameterId effect_parameter;
            SolIrSlice evidence;
            SolMirTemporaryId callee;
            /* Dynamic callee or receiver acquisition precedes arguments. */
            SolMirCallArgument receiver;
            SolMirSlice arguments;
            SolMirValueId result;
            SolMirEdge normal_edge;
            SolMirEdge failure_edge;
        } invoke;
        SolMirValueId value;
        struct {
            SolIrStatementId statement;
            size_t obligation;
        } unreachable;
        struct {
            SolIrStatementId statement;
            SolMirLoopId loop;
            SolMirEdge edge;
        } transfer;
        struct {
            /* The representation is consumed on both edges; only the normal
               edge transports the checked nominal result. */
            SolIrExpressionId source_expression;
            SolIrDefinitionId definition;
            SolObligationId obligation;
            SolMirTemporaryId representation;
            SolMirValueId result;
            SolMirEdge normal_edge;
            SolMirEdge failure_edge;
        } check_refined;
        SolIrExpressionId match_failure;
        struct {
            SolIrExpressionId source_expression;
            SolIrPropagationKind kind;
            SolMirTemporaryId operand;
            SolMirValueId value_result;
            SolMirValueId residual_result;
            SolMirEdge value_edge;
            SolMirEdge residual_edge;
        } propagate;
        struct {
            SolObligationId obligation;
            SolContractClauseKind phase;
            SolContractOutcomeKind outcome;
            /* SOL_MIR_NONE for requires; the complete callable result for ensures.
               Ensures consume it on violation/failure and forward it only when
               satisfied. Contract terminals also destroy captured snapshots. */
            SolMirValueId result;
            SolMirEdge satisfied_edge;
            SolMirEdge violation_edge;
            SolMirEdge failure_edge;
        } check_contract;
        SolObligationId contract_violation;
    } as;
} SolMirTerminator;

typedef struct {
    SolMirBlockId id;
    size_t order;
    SolMirSlice parameters;
    SolMirSlice instructions;
    SolMirTerminator terminator;
    SolSpan span;
    bool started;
} SolMirBlock;

typedef struct {
    SolIrCallableId callable;
    /* Symbolic callable metadata remains owned by the source IR. */
    SolIrSlice generic_parameters;
    SolIrSlice effect_parameters;
    SolMirBlockId entry;
    /* SOL_MIR_NONE when the callable has no semantic contract envelope. */
    SolMirBlockId contract_body;
    SolMirBlockId contract_epilogue;
    SolMirBlock *blocks;
    size_t block_count;
    size_t block_capacity;
    SolMirInstruction *instructions;
    size_t instruction_count;
    size_t instruction_capacity;
    SolMirValue *values;
    size_t value_count;
    size_t value_capacity;
    SolMirValueId *parameter_values;
    size_t parameter_value_count;
    size_t parameter_value_capacity;
    SolMirValueId *edge_values;
    size_t edge_value_count;
    size_t edge_value_capacity;
    SolMirCallArgument *call_arguments;
    size_t call_argument_count;
    size_t call_argument_capacity;
    SolMirLoop *loops;
    size_t loop_count;
    size_t loop_capacity;
    SolMirConstructOperand *construct_operands;
    size_t construct_operand_count;
    size_t construct_operand_capacity;
    SolMirTemporary *temporaries;
    size_t temporary_count;
    size_t temporary_capacity;
} SolMir;

void sol_mir_init(SolMir *mir);
void sol_mir_free(SolMir *mir);
SolMirLowerOutcome sol_mir_lower_callable(
    const SolIr *ir,
    SolIrCallableId callable,
    SolMir *mir,
    SolDiagnostics *diagnostics
);
/* Validation requires the originating validated owning IR; MIR is not standalone. */
bool sol_mir_validate(
    const SolIr *ir,
    const SolMir *mir,
    SolDiagnostics *diagnostics
);
bool sol_mir_render(FILE *stream, const SolIr *ir, const SolMir *mir);

#endif
