#ifndef SOL_MIR_H
#define SOL_MIR_H

#include "sol/ir.h"

/* Unstable callable-scoped CFG MIR checkpoint; not a serialized or target ABI. */
typedef size_t SolMirBlockId;
typedef size_t SolMirInstructionId;
typedef size_t SolMirValueId;

#define SOL_MIR_NONE SIZE_MAX

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
} SolMirValueKind;

typedef struct {
    SolMirValueKind kind;
    SolIrTypeId type;
    SolMirBlockId block;
    size_t definition;
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
    SOL_MIR_INST_STORE,
    SOL_MIR_INST_UNARY,
    SOL_MIR_INST_BINARY,
} SolMirInstructionKind;

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
            SolIrLocalId local;
            SolMirValueId value;
        } store;
    } as;
} SolMirInstruction;

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
        SolMirValueId value;
    } as;
} SolMirTerminator;

typedef struct {
    SolMirBlockId id;
    SolMirSlice parameters;
    SolMirSlice instructions;
    SolMirTerminator terminator;
    SolSpan span;
    bool started;
} SolMirBlock;

typedef struct {
    SolIrCallableId callable;
    SolMirBlockId entry;
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

#endif
