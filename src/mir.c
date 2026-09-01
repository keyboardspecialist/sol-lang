#include "sol/mir.h"

#include <stdlib.h>
#include <string.h>

typedef struct Scope Scope;
typedef struct LoopContext LoopContext;

struct Scope {
    const Scope *parent;
    SolIrSlice cleanup;
    SolIrStatementId region;
    bool cleanup_forward;
    bool cleanup_before_temporaries;
    size_t temporary_cleanup_boundary;
    size_t temporary_parent_boundary;
    bool exits_handler;
    SolIrExpressionId handler;
};

struct LoopContext {
    const LoopContext *parent;
    SolMirLoopId id;
    const Scope *boundary;
    size_t temporary_entry_depth;
};

typedef struct {
    bool reachable;
    SolMirValueId value;
} LoweredValue;

typedef struct {
    const SolIr *ir;
    const SolIrCallable *callable;
    SolMir *mir;
    SolDiagnostics *diagnostics;
    SolMirBlockId current;
    size_t next_block_order;
    const LoopContext *loop;
    SolMirTemporaryId *pending_temporaries;
    size_t pending_temporary_count;
    size_t pending_temporary_capacity;
    bool unsupported;
    bool failed;
    bool contracted;
    SolMirBlockId contract_epilogue;
} MirLowerer;

static bool mir_error(SolDiagnostics *diagnostics, SolSpan span,
    const char *message) {
    if (diagnostics != NULL) {
        sol_diagnostics_add(diagnostics, "SOL-MIR-002", SOL_SEVERITY_ERROR,
            span, "%s", message);
    }
    return false;
}

static bool mir_unsupported(MirLowerer *lowerer, SolSpan span,
    const char *message) {
    if (!lowerer->unsupported && !lowerer->failed && lowerer->diagnostics != NULL) {
        sol_diagnostics_add(lowerer->diagnostics, "SOL-MIR-001",
            SOL_SEVERITY_ERROR, span, "%s", message);
    }
    lowerer->unsupported = true;
    return false;
}

static bool mir_grow(void **items, size_t *capacity, size_t count,
    size_t item_size) {
    if (count < *capacity) return true;
    size_t next = *capacity == 0 ? 8 : *capacity * 2;
    if (next < *capacity || next > SIZE_MAX / item_size) return false;
    void *grown = realloc(*items, next * item_size);
    if (grown == NULL) return false;
    memset((char *)grown + *capacity * item_size, 0,
        (next - *capacity) * item_size);
    *items = grown;
    *capacity = next;
    return true;
}

void sol_mir_init(SolMir *mir) {
    if (mir == NULL) return;
    memset(mir, 0, sizeof(*mir));
    mir->callable = SOL_IR_NONE;
    mir->generic_parameters = (SolIrSlice){0};
    mir->effect_parameters = (SolIrSlice){0};
    mir->entry = SOL_MIR_NONE;
    mir->contract_body = SOL_MIR_NONE;
    mir->contract_epilogue = SOL_MIR_NONE;
}

void sol_mir_free(SolMir *mir) {
    if (mir == NULL) return;
    free(mir->blocks);
    free(mir->instructions);
    free(mir->values);
    free(mir->parameter_values);
    free(mir->edge_values);
    free(mir->call_arguments);
    free(mir->loops);
    free(mir->construct_operands);
    free(mir->temporaries);
    sol_mir_init(mir);
}

static bool mir_empty(const SolMir *mir) {
    return mir != NULL && mir->callable == SOL_IR_NONE
        && mir->generic_parameters.count == 0
        && mir->effect_parameters.count == 0
        && mir->entry == SOL_MIR_NONE && mir->blocks == NULL
        && mir->contract_body == SOL_MIR_NONE
        && mir->contract_epilogue == SOL_MIR_NONE
        && mir->block_count == 0 && mir->block_capacity == 0
        && mir->instructions == NULL && mir->instruction_count == 0
        && mir->instruction_capacity == 0 && mir->values == NULL
        && mir->value_count == 0 && mir->value_capacity == 0
        && mir->parameter_values == NULL && mir->parameter_value_count == 0
        && mir->parameter_value_capacity == 0 && mir->edge_values == NULL
        && mir->edge_value_count == 0 && mir->edge_value_capacity == 0
        && mir->call_arguments == NULL && mir->call_argument_count == 0
        && mir->call_argument_capacity == 0 && mir->loops == NULL
        && mir->loop_count == 0 && mir->loop_capacity == 0
        && mir->construct_operands == NULL
        && mir->construct_operand_count == 0
        && mir->construct_operand_capacity == 0 && mir->temporaries == NULL
        && mir->temporary_count == 0 && mir->temporary_capacity == 0;
}

static void mir_canonicalize_terminator(SolMirTerminator *terminator) {
    SolMirTerminator source = *terminator;
    memset(terminator, 0, sizeof(*terminator));
    terminator->kind = source.kind;
    terminator->span = source.span;
    switch (source.kind) {
        case SOL_MIR_TERM_GOTO:
            terminator->as.go_to.block = source.as.go_to.block;
            terminator->as.go_to.arguments = source.as.go_to.arguments;
            break;
        case SOL_MIR_TERM_BRANCH:
            terminator->as.branch.condition = source.as.branch.condition;
            terminator->as.branch.true_edge = source.as.branch.true_edge;
            terminator->as.branch.false_edge = source.as.branch.false_edge;
            break;
        case SOL_MIR_TERM_RETURN:
        case SOL_MIR_TERM_PANIC:
            terminator->as.value = source.as.value;
            break;
        case SOL_MIR_TERM_INVOKE:
            terminator->as.invoke.source_expression
                = source.as.invoke.source_expression;
            terminator->as.invoke.kind = source.as.invoke.kind;
            terminator->as.invoke.callable = source.as.invoke.callable;
            terminator->as.invoke.type_arguments
                = source.as.invoke.type_arguments;
            terminator->as.invoke.effects = source.as.invoke.effects;
            terminator->as.invoke.effect_parameter
                = source.as.invoke.effect_parameter;
            terminator->as.invoke.evidence = source.as.invoke.evidence;
            terminator->as.invoke.callee = source.as.invoke.callee;
            terminator->as.invoke.receiver.formal
                = source.as.invoke.receiver.formal;
            terminator->as.invoke.receiver.access
                = source.as.invoke.receiver.access;
            terminator->as.invoke.receiver.source_expression
                = source.as.invoke.receiver.source_expression;
            terminator->as.invoke.receiver.temporary
                = source.as.invoke.receiver.temporary;
            terminator->as.invoke.receiver.place
                = source.as.invoke.receiver.place;
            terminator->as.invoke.arguments = source.as.invoke.arguments;
            terminator->as.invoke.result = source.as.invoke.result;
            terminator->as.invoke.normal_edge = source.as.invoke.normal_edge;
            terminator->as.invoke.failure_edge = source.as.invoke.failure_edge;
            break;
        case SOL_MIR_TERM_UNREACHABLE:
            terminator->as.unreachable.statement
                = source.as.unreachable.statement;
            terminator->as.unreachable.obligation
                = source.as.unreachable.obligation;
            break;
        case SOL_MIR_TERM_BREAK:
        case SOL_MIR_TERM_CONTINUE:
            terminator->as.transfer.statement = source.as.transfer.statement;
            terminator->as.transfer.loop = source.as.transfer.loop;
            terminator->as.transfer.edge = source.as.transfer.edge;
            break;
        case SOL_MIR_TERM_CHECK_REFINED:
            terminator->as.check_refined.source_expression
                = source.as.check_refined.source_expression;
            terminator->as.check_refined.definition
                = source.as.check_refined.definition;
            terminator->as.check_refined.obligation
                = source.as.check_refined.obligation;
            terminator->as.check_refined.representation
                = source.as.check_refined.representation;
            terminator->as.check_refined.result
                = source.as.check_refined.result;
            terminator->as.check_refined.normal_edge
                = source.as.check_refined.normal_edge;
            terminator->as.check_refined.failure_edge
                = source.as.check_refined.failure_edge;
            break;
        case SOL_MIR_TERM_MATCH_FAILURE:
            terminator->as.match_failure = source.as.match_failure;
            break;
        case SOL_MIR_TERM_PROPAGATE:
            terminator->as.propagate.source_expression
                = source.as.propagate.source_expression;
            terminator->as.propagate.kind = source.as.propagate.kind;
            terminator->as.propagate.operand = source.as.propagate.operand;
            terminator->as.propagate.value_result
                = source.as.propagate.value_result;
            terminator->as.propagate.residual_result
                = source.as.propagate.residual_result;
            terminator->as.propagate.value_edge
                = source.as.propagate.value_edge;
            terminator->as.propagate.residual_edge
                = source.as.propagate.residual_edge;
            break;
        case SOL_MIR_TERM_CHECK_CONTRACT:
            terminator->as.check_contract.obligation
                = source.as.check_contract.obligation;
            terminator->as.check_contract.phase
                = source.as.check_contract.phase;
            terminator->as.check_contract.outcome
                = source.as.check_contract.outcome;
            terminator->as.check_contract.result
                = source.as.check_contract.result;
            terminator->as.check_contract.satisfied_edge
                = source.as.check_contract.satisfied_edge;
            terminator->as.check_contract.violation_edge
                = source.as.check_contract.violation_edge;
            terminator->as.check_contract.failure_edge
                = source.as.check_contract.failure_edge;
            break;
        case SOL_MIR_TERM_CONTRACT_VIOLATION:
            terminator->as.contract_violation = source.as.contract_violation;
            break;
        default: break;
    }
}

static SolMirBlockId mir_append_block(MirLowerer *lowerer, SolSpan span) {
    SolMir *mir = lowerer->mir;
    if (!mir_grow((void **)&mir->blocks, &mir->block_capacity,
        mir->block_count, sizeof(*mir->blocks))) {
        lowerer->failed = true;
        return SOL_MIR_NONE;
    }
    SolMirBlockId id = mir->block_count++;
    SolMirBlock *block = &mir->blocks[id];
    memset(block, 0, sizeof(*block));
    block->id = id;
    block->order = SOL_MIR_NONE;
    block->parameters.offset = mir->parameter_value_count;
    block->instructions.offset = mir->instruction_count;
    block->terminator.kind = SOL_MIR_TERM_INVALID;
    block->span = span;
    return id;
}

static bool mir_start_block(MirLowerer *lowerer, SolMirBlockId block) {
    if (block >= lowerer->mir->block_count
        || lowerer->mir->blocks[block].started) {
        lowerer->failed = true;
        return false;
    }
    lowerer->current = block;
    lowerer->mir->blocks[block].order = lowerer->next_block_order++;
    lowerer->mir->blocks[block].started = true;
    lowerer->mir->blocks[block].instructions.offset
        = lowerer->mir->instruction_count;
    return true;
}

static SolMirValueId mir_append_value(MirLowerer *lowerer,
    SolMirValue value) {
    SolMir *mir = lowerer->mir;
    if (!mir_grow((void **)&mir->values, &mir->value_capacity,
        mir->value_count, sizeof(*mir->values))) {
        lowerer->failed = true;
        return SOL_MIR_NONE;
    }
    SolMirValueId id = mir->value_count++;
    SolMirValue *stored = &mir->values[id];
    memset(stored, 0, sizeof(*stored));
    stored->kind = value.kind;
    stored->type = value.type;
    stored->block = value.block;
    stored->definition = value.definition;
    stored->source_expression = value.source_expression;
    stored->span = value.span;
    return id;
}

static SolMirValueId mir_append_parameter(MirLowerer *lowerer,
    SolMirBlockId block, SolIrTypeId type,
    SolIrExpressionId source_expression, SolSpan span) {
    SolMir *mir = lowerer->mir;
    if (block >= mir->block_count
        || !mir_grow((void **)&mir->parameter_values,
            &mir->parameter_value_capacity, mir->parameter_value_count,
            sizeof(*mir->parameter_values))) {
        lowerer->failed = true;
        return SOL_MIR_NONE;
    }
    SolMirValueId value = mir_append_value(lowerer, (SolMirValue){
        .kind = SOL_MIR_VALUE_BLOCK_PARAMETER,
        .type = type,
        .block = block,
        .definition = mir->blocks[block].parameters.count,
        .source_expression = source_expression,
        .span = span,
    });
    if (value == SOL_MIR_NONE) return value;
    mir->parameter_values[mir->parameter_value_count++] = value;
    ++mir->blocks[block].parameters.count;
    return value;
}

static SolMirInstructionId mir_append_instruction(MirLowerer *lowerer,
    SolMirInstruction instruction, bool has_result) {
    SolMir *mir = lowerer->mir;
    if (lowerer->current >= mir->block_count
        || mir->blocks[lowerer->current].terminator.kind
            != SOL_MIR_TERM_INVALID
        || !mir_grow((void **)&mir->instructions,
            &mir->instruction_capacity, mir->instruction_count,
            sizeof(*mir->instructions))) {
        lowerer->failed = true;
        return SOL_MIR_NONE;
    }
    SolMirInstructionId id = mir->instruction_count++;
    instruction.block = lowerer->current;
    instruction.result = SOL_MIR_NONE;
    SolMirInstruction *stored = &mir->instructions[id];
    memset(stored, 0, sizeof(*stored));
    stored->kind = instruction.kind;
    stored->block = instruction.block;
    stored->result = instruction.result;
    stored->type = instruction.type;
    stored->source_expression = instruction.source_expression;
    stored->span = instruction.span;
    switch (instruction.kind) {
        case SOL_MIR_INST_CONST_INT64:
            stored->as.integer = instruction.as.integer; break;
        case SOL_MIR_INST_CONST_BOOL:
            stored->as.boolean = instruction.as.boolean; break;
        case SOL_MIR_INST_PARAMETER_LIVE:
        case SOL_MIR_INST_STORAGE_LIVE:
        case SOL_MIR_INST_DROP_IF_INITIALIZED:
        case SOL_MIR_INST_STORAGE_DEAD:
            stored->as.local = instruction.as.local; break;
        case SOL_MIR_INST_LOAD_COPY:
        case SOL_MIR_INST_LOAD_MOVE:
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            stored->as.place.local = instruction.as.place.local;
            stored->as.place.source_place = instruction.as.place.source_place;
            break;
        case SOL_MIR_INST_LOAD_UPDATE:
            stored->as.update_load.statement
                = instruction.as.update_load.statement;
            stored->as.update_load.place.local
                = instruction.as.update_load.place.local;
            stored->as.update_load.place.source_place
                = instruction.as.update_load.place.source_place;
            break;
        case SOL_MIR_INST_STORE:
            stored->as.store.place.local = instruction.as.store.place.local;
            stored->as.store.place.source_place
                = instruction.as.store.place.source_place;
            stored->as.store.value = instruction.as.store.value;
            break;
        case SOL_MIR_INST_UNARY:
            stored->as.unary.operator_kind = instruction.as.unary.operator_kind;
            stored->as.unary.operand = instruction.as.unary.operand;
            break;
        case SOL_MIR_INST_BINARY:
            stored->as.binary.operator_kind = instruction.as.binary.operator_kind;
            stored->as.binary.left = instruction.as.binary.left;
            stored->as.binary.right = instruction.as.binary.right;
            break;
        case SOL_MIR_INST_COMPOUND_UPDATE:
            stored->as.compound_update.statement
                = instruction.as.compound_update.statement;
            stored->as.compound_update.operator_kind
                = instruction.as.compound_update.operator_kind;
            stored->as.compound_update.place.local
                = instruction.as.compound_update.place.local;
            stored->as.compound_update.place.source_place
                = instruction.as.compound_update.place.source_place;
            stored->as.compound_update.previous
                = instruction.as.compound_update.previous;
            stored->as.compound_update.right
                = instruction.as.compound_update.right;
            break;
        case SOL_MIR_INST_REGION_ENTER:
        case SOL_MIR_INST_REGION_EXIT:
            stored->as.region = instruction.as.region; break;
        case SOL_MIR_INST_TEMPORARY_INIT:
            stored->as.temporary_init.temporary
                = instruction.as.temporary_init.temporary;
            stored->as.temporary_init.value = instruction.as.temporary_init.value;
            break;
        case SOL_MIR_INST_TEMPORARY_DROP:
            stored->as.temporary_drop.temporary
                = instruction.as.temporary_drop.temporary;
            stored->as.temporary_drop.preserve_depth
                = instruction.as.temporary_drop.preserve_depth;
            break;
        case SOL_MIR_INST_EXPRESSION_RESULT:
            stored->as.operand = instruction.as.operand; break;
        case SOL_MIR_INST_PATTERN_TEST:
        case SOL_MIR_INST_PATTERN_VALUE:
        case SOL_MIR_INST_MATCH_ARM:
            stored->as.pattern.match_expression
                = instruction.as.pattern.match_expression;
            stored->as.pattern.arm = instruction.as.pattern.arm;
            stored->as.pattern.arm_ordinal
                = instruction.as.pattern.arm_ordinal;
            stored->as.pattern.pattern = instruction.as.pattern.pattern;
            stored->as.pattern.scrutinee = instruction.as.pattern.scrutinee;
            break;
        case SOL_MIR_INST_CONSTRUCT:
            stored->as.construct.kind = instruction.as.construct.kind;
            stored->as.construct.definition = instruction.as.construct.definition;
            stored->as.construct.variant = instruction.as.construct.variant;
            stored->as.construct.operands = instruction.as.construct.operands;
            stored->as.construct.capability_roots
                = instruction.as.construct.capability_roots;
            stored->as.construct.operation_roots
                = instruction.as.construct.operation_roots;
            break;
        case SOL_MIR_INST_CAPTURE_SNAPSHOT:
            stored->as.snapshot = instruction.as.snapshot; break;
        default: break;
    }
    ++mir->blocks[lowerer->current].instructions.count;
    if (has_result) {
        SolMirValueId result = mir_append_value(lowerer, (SolMirValue){
            .kind = SOL_MIR_VALUE_INSTRUCTION,
            .type = instruction.type,
            .block = lowerer->current,
            .definition = id,
            .source_expression = instruction.source_expression,
            .span = instruction.span,
        });
        if (result == SOL_MIR_NONE) return SOL_MIR_NONE;
        mir->instructions[id].result = result;
    }
    return id;
}

static SolMirValueId mir_instruction_result(MirLowerer *lowerer,
    SolMirInstruction instruction) {
    SolMirInstructionId id = mir_append_instruction(lowerer, instruction, true);
    return id == SOL_MIR_NONE ? SOL_MIR_NONE
        : lowerer->mir->instructions[id].result;
}

static SolMirSlice mir_append_edge_arguments(MirLowerer *lowerer,
    const SolMirValueId *values, size_t count) {
    SolMir *mir = lowerer->mir;
    SolMirSlice slice = {mir->edge_value_count, 0};
    for (size_t index = 0; index < count; ++index) {
        if (!mir_grow((void **)&mir->edge_values,
            &mir->edge_value_capacity, mir->edge_value_count,
            sizeof(*mir->edge_values))) {
            lowerer->failed = true;
            return (SolMirSlice){0};
        }
        mir->edge_values[mir->edge_value_count++] = values[index];
        ++slice.count;
    }
    return slice;
}

static bool mir_append_call_argument(MirLowerer *lowerer,
    SolMirCallArgument argument) {
    SolMir *mir = lowerer->mir;
    if (!mir_grow((void **)&mir->call_arguments,
        &mir->call_argument_capacity, mir->call_argument_count,
        sizeof(*mir->call_arguments))) {
        lowerer->failed = true;
        return false;
    }
    SolMirCallArgument *stored
        = &mir->call_arguments[mir->call_argument_count++];
    memset(stored, 0, sizeof(*stored));
    stored->formal = argument.formal;
    stored->access = argument.access;
    stored->source_expression = argument.source_expression;
    stored->temporary = argument.temporary;
    stored->place = argument.place;
    return true;
}

static bool mir_append_construct_operand(MirLowerer *lowerer,
    SolMirConstructOperand operand) {
    SolMir *mir = lowerer->mir;
    if (!mir_grow((void **)&mir->construct_operands,
        &mir->construct_operand_capacity, mir->construct_operand_count,
        sizeof(*mir->construct_operands))) {
        lowerer->failed = true;
        return false;
    }
    mir->construct_operands[mir->construct_operand_count++] = operand;
    return true;
}

static SolMirTemporaryId mir_append_temporary(MirLowerer *lowerer,
    SolIrExpressionId source_expression) {
    SolMir *mir = lowerer->mir;
    if (source_expression >= lowerer->ir->expression_count
        || !mir_grow((void **)&mir->temporaries, &mir->temporary_capacity,
            mir->temporary_count, sizeof(*mir->temporaries))) {
        lowerer->failed = true;
        return SOL_MIR_NONE;
    }
    SolMirTemporaryId id = mir->temporary_count++;
    mir->temporaries[id] = (SolMirTemporary){
        .type = lowerer->ir->expressions[source_expression].type,
        .source_expression = source_expression,
        .span = lowerer->ir->expressions[source_expression].span,
    };
    return id;
}

static bool mir_push_pending_temporary(MirLowerer *lowerer,
    SolMirTemporaryId temporary) {
    if (!mir_grow((void **)&lowerer->pending_temporaries,
        &lowerer->pending_temporary_capacity,
        lowerer->pending_temporary_count,
        sizeof(*lowerer->pending_temporaries))) {
        lowerer->failed = true;
        return false;
    }
    lowerer->pending_temporaries[lowerer->pending_temporary_count++] = temporary;
    return true;
}

static bool mir_stage_temporary(MirLowerer *lowerer,
    SolIrExpressionId source_expression, SolMirValueId value,
    SolMirTemporaryId *temporary) {
    SolMirTemporaryId id = mir_append_temporary(lowerer, source_expression);
    if (id == SOL_MIR_NONE
        || mir_append_instruction(lowerer, (SolMirInstruction){
            .kind = SOL_MIR_INST_TEMPORARY_INIT,
            .type = SOL_IR_NONE,
            .source_expression = source_expression,
            .span = lowerer->ir->expressions[source_expression].span,
            .as.temporary_init = {.temporary = id, .value = value},
        }, false) == SOL_MIR_NONE
        || !mir_push_pending_temporary(lowerer, id)) return false;
    *temporary = id;
    return true;
}

static bool mir_emit_temporary_cleanup_to(MirLowerer *lowerer,
    size_t depth, SolSpan span) {
    if (depth > lowerer->pending_temporary_count) return false;
    for (size_t index = depth;
        index < lowerer->pending_temporary_count; ++index) {
        SolMirTemporaryId temporary = lowerer->pending_temporaries[index];
        if (mir_append_instruction(lowerer, (SolMirInstruction){
            .kind = SOL_MIR_INST_TEMPORARY_DROP,
            .type = SOL_IR_NONE,
            .source_expression = SOL_IR_NONE,
            .span = span,
            .as.temporary_drop = {
                .temporary = temporary,
                .preserve_depth = depth,
            },
        }, false) == SOL_MIR_NONE) return false;
    }
    return true;
}

static bool mir_emit_temporary_cleanup_range(MirLowerer *lowerer,
    size_t begin, size_t end, size_t preserve_depth, SolSpan span) {
    if (begin > end || end > lowerer->pending_temporary_count) return false;
    for (size_t index = begin; index < end; ++index) {
        if (mir_append_instruction(lowerer, (SolMirInstruction){
            .kind = SOL_MIR_INST_TEMPORARY_DROP,
            .type = SOL_IR_NONE,
            .source_expression = SOL_IR_NONE,
            .span = span,
            .as.temporary_drop = {
                .temporary = lowerer->pending_temporaries[index],
                .preserve_depth = preserve_depth,
            },
        }, false) == SOL_MIR_NONE) return false;
    }
    return true;
}

static SolMirLoopId mir_append_loop(MirLowerer *lowerer,
    SolMirLoop loop) {
    SolMir *mir = lowerer->mir;
    if (!mir_grow((void **)&mir->loops, &mir->loop_capacity,
        mir->loop_count, sizeof(*mir->loops))) {
        lowerer->failed = true;
        return SOL_MIR_NONE;
    }
    SolMirLoopId id = mir->loop_count++;
    mir->loops[id] = loop;
    return id;
}

static bool mir_set_goto(MirLowerer *lowerer, SolMirBlockId source,
    SolMirBlockId target, SolMirValueId value, bool has_value, SolSpan span) {
    if (source >= lowerer->mir->block_count
        || target >= lowerer->mir->block_count
        || lowerer->mir->blocks[source].terminator.kind
            != SOL_MIR_TERM_INVALID) {
        lowerer->failed = true;
        return false;
    }
    SolMirSlice arguments = {lowerer->mir->edge_value_count, 0};
    if (has_value) arguments = mir_append_edge_arguments(lowerer, &value, 1);
    if (lowerer->failed) return false;
    lowerer->mir->blocks[source].terminator = (SolMirTerminator){
        .kind = SOL_MIR_TERM_GOTO,
        .span = span,
        .as.go_to = {.block = target, .arguments = arguments},
    };
    return true;
}

static bool mir_set_branch(MirLowerer *lowerer, SolMirBlockId source,
    SolMirValueId condition, SolMirBlockId true_block,
    SolMirBlockId false_block, SolSpan span) {
    if (source >= lowerer->mir->block_count
        || true_block >= lowerer->mir->block_count
        || false_block >= lowerer->mir->block_count
        || lowerer->mir->blocks[source].terminator.kind
            != SOL_MIR_TERM_INVALID) {
        lowerer->failed = true;
        return false;
    }
    SolMirSlice empty = {lowerer->mir->edge_value_count, 0};
    lowerer->mir->blocks[source].terminator = (SolMirTerminator){
        .kind = SOL_MIR_TERM_BRANCH,
        .span = span,
        .as.branch = {
            .condition = condition,
            .true_edge = {.block = true_block, .arguments = empty},
            .false_edge = {.block = false_block, .arguments = empty},
        },
    };
    return true;
}

static bool mir_set_branch_values(MirLowerer *lowerer, SolMirBlockId source,
    SolMirValueId condition, SolMirBlockId true_block,
    SolMirValueId true_value, bool true_has_value,
    SolMirBlockId false_block, SolMirValueId false_value,
    bool false_has_value, SolSpan span) {
    if (source >= lowerer->mir->block_count
        || true_block >= lowerer->mir->block_count
        || false_block >= lowerer->mir->block_count
        || lowerer->mir->blocks[source].terminator.kind
            != SOL_MIR_TERM_INVALID) {
        lowerer->failed = true;
        return false;
    }
    SolMirSlice true_arguments = {lowerer->mir->edge_value_count, 0};
    if (true_has_value) {
        true_arguments = mir_append_edge_arguments(lowerer, &true_value, 1);
    }
    SolMirSlice false_arguments = {lowerer->mir->edge_value_count, 0};
    if (false_has_value) {
        false_arguments = mir_append_edge_arguments(lowerer, &false_value, 1);
    }
    if (lowerer->failed) return false;
    lowerer->mir->blocks[source].terminator = (SolMirTerminator){
        .kind = SOL_MIR_TERM_BRANCH,
        .span = span,
        .as.branch = {
            .condition = condition,
            .true_edge = {.block = true_block, .arguments = true_arguments},
            .false_edge = {.block = false_block, .arguments = false_arguments},
        },
    };
    return true;
}

static bool mir_emit_local(MirLowerer *lowerer,
    SolMirInstructionKind kind, SolIrLocalId local, SolSpan span) {
    return mir_append_instruction(lowerer, (SolMirInstruction){
        .kind = kind,
        .type = SOL_IR_NONE,
        .source_expression = SOL_IR_NONE,
        .span = span,
        .as.local = local,
    }, false) != SOL_MIR_NONE;
}

static bool mir_emit_cleanup_slice(MirLowerer *lowerer,
    SolIrSlice cleanup, SolSpan span) {
    while (cleanup.count != 0) {
        --cleanup.count;
        SolIrLocalId local = lowerer->ir->cleanup_locals[
            cleanup.offset + cleanup.count];
        if (!mir_emit_local(lowerer, SOL_MIR_INST_DROP_IF_INITIALIZED,
                local, span)
            || !mir_emit_local(lowerer, SOL_MIR_INST_STORAGE_DEAD,
                local, span)) return false;
    }
    return true;
}

static bool mir_emit_scope_cleanup(MirLowerer *lowerer,
    const Scope *scope, SolSpan span) {
    if (!scope->cleanup_forward) {
        return mir_emit_cleanup_slice(lowerer, scope->cleanup, span);
    }
    for (size_t index = 0; index < scope->cleanup.count; ++index) {
        SolIrLocalId local = lowerer->ir->cleanup_locals[
            scope->cleanup.offset + index];
        if (!mir_emit_local(lowerer, SOL_MIR_INST_DROP_IF_INITIALIZED,
                local, span)
            || !mir_emit_local(lowerer, SOL_MIR_INST_STORAGE_DEAD,
                local, span)) return false;
    }
    return true;
}

static bool mir_emit_scope_exit(MirLowerer *lowerer,
    const Scope *scope, SolSpan span) {
    if (!mir_emit_scope_cleanup(lowerer, scope, span)) return false;
    if (scope->region != SOL_IR_NONE
        && mir_append_instruction(lowerer, (SolMirInstruction){
                .kind = SOL_MIR_INST_REGION_EXIT,
                .type = SOL_IR_NONE,
                .source_expression = SOL_IR_NONE,
                .span = span,
                .as.region = scope->region,
            }, false) == SOL_MIR_NONE) return false;
    return !scope->exits_handler
        || mir_append_instruction(lowerer, (SolMirInstruction){
            .kind = SOL_MIR_INST_HANDLER_EXIT,
            .type = SOL_IR_NONE,
            .source_expression = scope->handler,
            .span = lowerer->ir->expressions[scope->handler].span,
        }, false) != SOL_MIR_NONE;
}

static bool mir_emit_exit_cleanup_mode(MirLowerer *lowerer,
    const Scope *scope, SolSpan span, bool cleanup_parameters) {
    const Scope *current = scope;
    size_t temporary_end = lowerer->pending_temporary_count;
    while (current != NULL) {
        const Scope *boundary = current;
        while (boundary != NULL && !boundary->cleanup_before_temporaries) {
            boundary = boundary->parent;
        }
        if (boundary == NULL) break;
        size_t temporary_boundary = boundary->temporary_cleanup_boundary;
        if (!mir_emit_temporary_cleanup_range(lowerer, temporary_boundary,
            temporary_end, temporary_boundary, span)) {
            return false;
        }
        while (current != boundary->parent) {
            if (!mir_emit_scope_exit(lowerer, current, span)) return false;
            current = current->parent;
        }
        if (!mir_emit_temporary_cleanup_range(lowerer,
            boundary->temporary_parent_boundary, temporary_boundary,
            boundary->temporary_parent_boundary, span)) return false;
        temporary_end = boundary->temporary_parent_boundary;
    }
    if (!mir_emit_temporary_cleanup_range(lowerer, 0,
        temporary_end, 0, span)) {
        return false;
    }
    for (; current != NULL; current = current->parent) {
        if (!mir_emit_scope_exit(lowerer, current, span)) return false;
    }
    SolIrSlice parameters = cleanup_parameters
        ? lowerer->callable->parameters : (SolIrSlice){0};
    while (parameters.count != 0) {
        --parameters.count;
        SolIrLocalId local = lowerer->ir->roots[
            parameters.offset + parameters.count];
        if (!mir_emit_local(lowerer, SOL_MIR_INST_DROP_IF_INITIALIZED,
                local, span)
            || !mir_emit_local(lowerer, SOL_MIR_INST_STORAGE_DEAD,
                local, span)) return false;
    }
    if (cleanup_parameters && lowerer->callable->receiver != SOL_IR_NONE) {
        SolIrLocalId local = lowerer->callable->receiver;
        if (!mir_emit_local(lowerer, SOL_MIR_INST_DROP_IF_INITIALIZED,
                local, span)
            || !mir_emit_local(lowerer, SOL_MIR_INST_STORAGE_DEAD,
                local, span)) return false;
    }
    return true;
}

static bool mir_emit_exit_cleanup(MirLowerer *lowerer,
    const Scope *scope, SolSpan span) {
    return mir_emit_exit_cleanup_mode(lowerer, scope, span, true);
}

static bool mir_emit_cleanup_to(MirLowerer *lowerer,
    const Scope *scope, const Scope *boundary, SolSpan span) {
    const Scope *current = scope;
    while (current != boundary) {
        if (current == NULL || !mir_emit_scope_exit(lowerer, current, span)) {
            return false;
        }
        current = current->parent;
    }
    return true;
}

static LoweredValue mir_lower_expression(MirLowerer *lowerer,
    SolIrExpressionId id, const Scope *scope);
static bool mir_type_is(const SolIr *ir, SolIrTypeId id,
    SolIrTypeKind kind);
static bool mir_type_matches_call(const SolIr *ir,
    const SolIrCallable *target, SolIrSlice arguments,
    SolIrTypeId symbolic, SolIrTypeId actual);
static bool mir_set_return(MirLowerer *lowerer, SolMirValueId value,
    SolSpan span);

static bool mir_finish_success(MirLowerer *lowerer, const Scope *scope,
    SolMirValueId value, SolSpan span) {
    if (!lowerer->contracted) {
        return mir_emit_exit_cleanup(lowerer, scope, span)
            && mir_set_return(lowerer, value, span);
    }
    return mir_emit_exit_cleanup_mode(lowerer, scope, span, false)
        && mir_set_goto(lowerer, lowerer->current,
            lowerer->contract_epilogue, value, true, span);
}

static LoweredValue mir_unreachable(void) {
    return (LoweredValue){.reachable = false, .value = SOL_MIR_NONE};
}

static SolIrExpressionId mir_block_result_source(const SolIr *ir,
    const SolIrExpression *block) {
    if (block->kind != SOL_IR_EXPR_BLOCK
        || block->as.block.statements.count == 0) return SOL_IR_NONE;
    SolIrStatementId statement_id = ir->statement_ids[
        block->as.block.statements.offset + block->as.block.statements.count - 1];
    const SolIrStatement *statement = &ir->statements[statement_id];
    return statement->kind == SOL_IR_STATEMENT_EXPRESSION
            || statement->kind == SOL_IR_STATEMENT_REGION
        ? statement->expression : SOL_IR_NONE;
}

static LoweredValue mir_failure(MirLowerer *lowerer) {
    lowerer->failed = lowerer->failed || !lowerer->unsupported;
    return mir_unreachable();
}

static SolMirValueId mir_append_terminator_result(MirLowerer *lowerer,
    SolMirBlockId block, SolIrTypeId type,
    SolIrExpressionId source_expression, SolSpan span) {
    return mir_append_value(lowerer, (SolMirValue){
        .kind = SOL_MIR_VALUE_TERMINATOR,
        .type = type,
        .block = block,
        .definition = block,
        .source_expression = source_expression,
        .span = span,
    });
}

static bool mir_set_failure_terminator(MirLowerer *lowerer,
    SolMirTerminatorKind kind, SolSpan span) {
    if (lowerer->current >= lowerer->mir->block_count
        || lowerer->mir->blocks[lowerer->current].terminator.kind
            != SOL_MIR_TERM_INVALID
        || (kind != SOL_MIR_TERM_RESUME_FAILURE
            && kind != SOL_MIR_TERM_UNREACHABLE)) return false;
    lowerer->mir->blocks[lowerer->current].terminator = (SolMirTerminator){
        .kind = kind,
        .span = span,
    };
    return true;
}

static LoweredValue mir_lower_short_circuit(MirLowerer *lowerer,
    const SolIrExpression *expression, const Scope *scope) {
    size_t temporary_depth = lowerer->pending_temporary_count;
    LoweredValue left = mir_lower_expression(lowerer,
        expression->as.binary.left, scope);
    if (!left.reachable) return left;
    SolMirBlockId branch = lowerer->current;
    SolMirBlockId right_block = mir_append_block(lowerer,
        lowerer->ir->expressions[expression->as.binary.right].span);
    SolMirBlockId join = mir_append_block(lowerer, expression->span);
    SolMirValueId parameter = mir_append_parameter(lowerer, join,
        expression->type,
        (SolIrExpressionId)(expression - lowerer->ir->expressions),
        expression->span);
    bool conjunction = expression->as.binary.operator_kind == SOL_TOKEN_AMP_AMP;
    if (right_block == SOL_MIR_NONE || join == SOL_MIR_NONE
        || parameter == SOL_MIR_NONE
        || !mir_set_branch_values(lowerer, branch, left.value,
            conjunction ? right_block : join,
            conjunction ? SOL_MIR_NONE : left.value, !conjunction,
            conjunction ? join : right_block,
            conjunction ? left.value : SOL_MIR_NONE, conjunction,
            expression->span)
        || !mir_start_block(lowerer, right_block)) return mir_failure(lowerer);
    LoweredValue right = mir_lower_expression(lowerer,
        expression->as.binary.right, scope);
    if (lowerer->pending_temporary_count != temporary_depth) {
        return mir_failure(lowerer);
    }
    if (right.reachable && !mir_set_goto(lowerer, lowerer->current, join,
        right.value, true, expression->span)) return mir_failure(lowerer);
    if (!mir_start_block(lowerer, join)) return mir_failure(lowerer);
    return (LoweredValue){.reachable = true, .value = parameter};
}

static LoweredValue mir_lower_direct_call(MirLowerer *lowerer,
    SolIrExpressionId id, const SolIrExpression *expression,
    const Scope *scope) {
    bool callback = expression->as.call.kind == SOL_IR_CALL_CALLBACK;
    bool method = expression->as.call.kind == SOL_IR_CALL_METHOD;
    bool capability = expression->as.call.kind == SOL_IR_CALL_CAPABILITY;
    bool forwarded_method = false;
    if ((!callback && !method && !capability
            && expression->as.call.kind != SOL_IR_CALL_FUNCTION)
        || (method ? expression->as.call.evidence.count != 1 : false)) {
        mir_unsupported(lowerer, expression->span,
            "P1a MIR supports only validated direct and callback calls");
        return mir_unreachable();
    }
    SolIrCallableId selected_callable = expression->as.call.callable;
    if (method) {
        size_t evidence_id = expression->as.call.evidence.offset;
        if (evidence_id >= lowerer->ir->evidence_count) {
            return mir_failure(lowerer);
        }
        const SolIrDispatchEvidence *evidence
            = &lowerer->ir->evidence[evidence_id];
        if (evidence->binding != SOL_IR_NONE
            || evidence->requirement != expression->as.call.callable
            || (evidence->forwarded
                ? (evidence->parameter == SOL_IR_NONE
                    || evidence->method != SOL_IR_NONE
                    || evidence->implementation != SOL_IR_NONE
                    || evidence->type != SOL_IR_NONE)
                : (evidence->parameter != SOL_IR_NONE
                    || evidence->type != lowerer->ir->expressions[
                        expression->as.call.receiver].type
                    || evidence->method >= lowerer->ir->callable_count))) {
            mir_unsupported(lowerer, expression->span,
                "P1a MIR method evidence is outside the bounded trait subset");
            return mir_unreachable();
        }
        forwarded_method = evidence->forwarded;
        if (!forwarded_method) selected_callable = evidence->method;
    }
    if (!callback) {
        if (selected_callable >= lowerer->ir->callable_count) {
            return mir_failure(lowerer);
        }
        const SolIrCallable *target
            = &lowerer->ir->callables[selected_callable];
        if (capability) {
            for (size_t index = 0; index < target->effects.count; ++index) {
                if (lowerer->ir->effects[target->effects.offset + index]
                    .authority_kind == SOL_IR_AUTHORITY_LOCAL) {
                    mir_unsupported(lowerer, expression->span,
                        "P1a MIR capability calls defer operand-dependent effects");
                    return mir_unreachable();
                }
            }
        }
        if ((!method && !capability
                && target->kind != SOL_IR_CALLABLE_FUNCTION)
            || (method && target->kind != (forwarded_method
                ? SOL_IR_CALLABLE_TRAIT_REQUIREMENT
                : SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION))
            || (capability && target->kind != SOL_IR_CALLABLE_CAPABILITY)
            || (!method && target->receiver != SOL_IR_NONE)
            || (method && target->receiver == SOL_IR_NONE)
            || (!capability && target->capability_source != SOL_IR_NONE)
            || (target->generic_parameters.count == 0
                && target->result != expression->type)) {
            mir_unsupported(lowerer, expression->span,
                "P1a MIR direct call target is outside the nongeneric free-function subset");
            return mir_unreachable();
        }
        bool source_never = mir_type_is(lowerer->ir, expression->type,
            SOL_IR_TYPE_NEVER);
        if (!source_never && !mir_type_matches_call(lowerer->ir, target,
                expression->as.call.type_arguments, target->result,
                expression->type)) {
            mir_unsupported(lowerer, expression->span,
                "P1a MIR call result requires broader generic substitution");
            return mir_unreachable();
        }
        for (size_t index = 0; !source_never
            && index < expression->as.call.operands.count;
            ++index) {
            SolIrLocalId formal = lowerer->ir->roots[
                target->parameters.offset + index];
            SolIrExpressionId value = lowerer->ir->operands[
                expression->as.call.operands.offset + index].value;
            if (mir_type_is(lowerer->ir,
                lowerer->ir->expressions[value].type, SOL_IR_TYPE_NEVER)) break;
            if (!mir_type_matches_call(lowerer->ir, target,
                    expression->as.call.type_arguments,
                    lowerer->ir->locals[formal].type,
                    lowerer->ir->expressions[value].type)) {
                mir_unsupported(lowerer, expression->span,
                    "P1a MIR call operands require broader generic substitution");
                return mir_unreachable();
            }
        }
    } else if (expression->as.call.callee >= lowerer->ir->expression_count
        || lowerer->ir->expressions[expression->as.call.callee].type
            >= lowerer->ir->type_count
        || lowerer->ir->types[lowerer->ir->expressions[
            expression->as.call.callee].type].kind != SOL_IR_TYPE_FUNCTION) {
        return mir_failure(lowerer);
    }
    size_t operation_depth = lowerer->pending_temporary_count;
    SolMirTemporaryId callee = SOL_MIR_NONE;
    SolMirCallArgument receiver = {
        .formal = SOL_IR_NONE,
        .source_expression = SOL_IR_NONE,
        .temporary = SOL_MIR_NONE,
        .place = SOL_IR_NONE,
    };
    if (callback) {
        LoweredValue value = mir_lower_expression(lowerer,
            expression->as.call.callee, scope);
        if (!value.reachable) return value;
        if (!mir_stage_temporary(lowerer, expression->as.call.callee,
            value.value, &callee)) return mir_failure(lowerer);
    } else if (method || capability) {
        receiver.access = expression->as.call.receiver_access;
        SolIrExpressionId receiver_id = expression->as.call.receiver;
        if (capability) {
            if (expression->as.call.callee >= lowerer->ir->expression_count
                || lowerer->ir->expressions[expression->as.call.callee].kind
                    != SOL_IR_EXPR_BOUND_OPERATION) return mir_failure(lowerer);
            receiver_id = lowerer->ir->expressions[
                expression->as.call.callee].as.operation.receiver;
        }
        receiver.source_expression = receiver_id;
        const SolIrExpression *source
            = &lowerer->ir->expressions[receiver_id];
        if (receiver.access == SOL_ACCESS_OWNED) {
            LoweredValue value = mir_lower_expression(lowerer,
                receiver_id, scope);
            if (!value.reachable) return value;
            if (!mir_stage_temporary(lowerer, receiver_id,
                value.value, &receiver.temporary)) return mir_failure(lowerer);
        } else if ((receiver.access == SOL_ACCESS_SHARED
                || receiver.access == SOL_ACCESS_EXCLUSIVE)
            && source->kind == SOL_IR_EXPR_PLACE
            && source->as.place < lowerer->ir->place_count
            && lowerer->ir->places[source->as.place].root_kind
                == SOL_IR_PLACE_ROOT_LOCAL) {
            receiver.place = source->as.place;
        } else {
            mir_unsupported(lowerer, source->span,
                "P1a MIR supports only owned or local-rooted method receivers");
            return mir_unreachable();
        }
    }
    size_t count = expression->as.call.operands.count;
    SolMirCallArgument *arguments = count == 0 ? NULL
        : calloc(count, sizeof(*arguments));
    if (count != 0 && arguments == NULL) return mir_failure(lowerer);
    for (size_t index = 0; index < count; ++index) {
        const SolIrOperand *operand = &lowerer->ir->operands[
            expression->as.call.operands.offset + index];
        SolMirCallArgument *argument = &arguments[index];
        *argument = (SolMirCallArgument){
            .formal = operand->formal,
            .access = operand->access,
            .source_expression = operand->value,
            .temporary = SOL_MIR_NONE,
            .place = SOL_IR_NONE,
        };
        if (operand->access == SOL_ACCESS_OWNED) {
            LoweredValue value = mir_lower_expression(lowerer,
                operand->value, scope);
            if (!value.reachable) {
                free(arguments);
                lowerer->pending_temporary_count = operation_depth;
                return value;
            }
            if (!mir_stage_temporary(lowerer, operand->value, value.value,
                &argument->temporary)) {
                free(arguments);
                return mir_failure(lowerer);
            }
            continue;
        }
        const SolIrExpression *source
            = &lowerer->ir->expressions[operand->value];
        if ((operand->access != SOL_ACCESS_SHARED
                && operand->access != SOL_ACCESS_EXCLUSIVE)
            || source->kind != SOL_IR_EXPR_PLACE
            || source->as.place >= lowerer->ir->place_count) {
            free(arguments);
            mir_unsupported(lowerer, source->span,
                "P1a.2 MIR supports only whole-local call borrows");
            return mir_unreachable();
        }
        const SolIrPlace *place = &lowerer->ir->places[source->as.place];
        if (place->root_kind != SOL_IR_PLACE_ROOT_LOCAL) {
            free(arguments);
            mir_unsupported(lowerer, source->span,
                "P1a MIR supports only local-rooted call borrows");
            return mir_unreachable();
        }
        argument->place = source->as.place;
    }
    SolMirSlice argument_slice = {lowerer->mir->call_argument_count, 0};
    for (size_t index = 0; index < count; ++index) {
        if (!mir_append_call_argument(lowerer, arguments[index])) break;
        ++argument_slice.count;
    }
    free(arguments);
    if (lowerer->failed) return mir_failure(lowerer);
    lowerer->pending_temporary_count = operation_depth;

    SolMirBlockId invoke_block = lowerer->current;
    SolMirBlockId failure = mir_append_block(lowerer, expression->span);
    bool never = mir_type_is(lowerer->ir, expression->type, SOL_IR_TYPE_NEVER);
    SolMirBlockId normal = never ? SOL_MIR_NONE
        : mir_append_block(lowerer, expression->span);
    SolMirValueId result = never ? SOL_MIR_NONE
        : mir_append_terminator_result(lowerer, invoke_block,
            expression->type, id, expression->span);
    SolMirValueId parameter = never ? SOL_MIR_NONE
        : mir_append_parameter(lowerer, normal, expression->type,
            id, expression->span);
    if (failure == SOL_MIR_NONE || (!never
            && (normal == SOL_MIR_NONE || result == SOL_MIR_NONE
                || parameter == SOL_MIR_NONE))) return mir_failure(lowerer);
    SolMirSlice no_arguments = {lowerer->mir->edge_value_count, 0};
    SolMirSlice normal_arguments = no_arguments;
    if (!never) normal_arguments = mir_append_edge_arguments(lowerer, &result, 1);
    if (lowerer->failed) return mir_failure(lowerer);
    lowerer->mir->blocks[invoke_block].terminator = (SolMirTerminator){
        .kind = SOL_MIR_TERM_INVOKE,
        .span = expression->span,
        .as.invoke = {
            .source_expression = id,
            .kind = expression->as.call.kind,
            .callable = selected_callable,
            .type_arguments = expression->as.call.type_arguments,
            .effects = expression->as.call.effects,
            .effect_parameter = expression->as.call.effect_parameter,
            .evidence = expression->as.call.evidence,
            .callee = callee,
            .receiver = receiver,
            .arguments = argument_slice,
            .result = result,
            .normal_edge = {.block = normal, .arguments = normal_arguments},
            .failure_edge = {.block = failure, .arguments = no_arguments},
        },
    };
    if (!mir_start_block(lowerer, failure)
        || !mir_emit_exit_cleanup(lowerer, scope, expression->span)
        || !mir_set_failure_terminator(lowerer,
            SOL_MIR_TERM_RESUME_FAILURE, expression->span)) {
        return mir_failure(lowerer);
    }
    if (never) {
        lowerer->current = SOL_MIR_NONE;
        return mir_unreachable();
    }
    if (!mir_start_block(lowerer, normal)) return mir_failure(lowerer);
    return (LoweredValue){.reachable = true, .value = parameter};
}

static LoweredValue mir_lower_checked_refined(MirLowerer *lowerer,
    SolIrExpressionId id, const SolIrExpression *expression,
    const Scope *scope) {
    SolIrDefinitionId definition = expression->as.call.definition;
    SolIrSlice operands = expression->as.call.operands;
    if (definition >= lowerer->ir->definition_count
        || lowerer->ir->definitions[definition].kind
            != SOL_IR_DEFINITION_REFINED
        || operands.count != 1) return mir_failure(lowerer);
    SolObligationId obligation = SOL_IR_NONE;
    for (size_t index = 0; index < lowerer->ir->obligation_count; ++index) {
        if (lowerer->ir->obligations[index].owner_kind
                == SOL_CONTRACT_OWNER_TYPE
            && lowerer->ir->obligations[index].owner == definition) {
            obligation = index;
            break;
        }
    }
    if (obligation == SOL_IR_NONE) return mir_failure(lowerer);
    size_t operation_depth = lowerer->pending_temporary_count;
    const SolIrOperand *operand = &lowerer->ir->operands[operands.offset];
    LoweredValue representation = mir_lower_expression(lowerer,
        operand->value, scope);
    if (!representation.reachable) {
        lowerer->pending_temporary_count = operation_depth;
        return representation;
    }
    SolMirTemporaryId temporary = SOL_MIR_NONE;
    if (!mir_stage_temporary(lowerer, operand->value,
        representation.value, &temporary)) return mir_failure(lowerer);
    SolMirBlockId check_block = lowerer->current;
    SolMirBlockId failure = mir_append_block(lowerer, expression->span);
    SolMirBlockId normal = mir_append_block(lowerer, expression->span);
    SolMirValueId result = mir_append_terminator_result(lowerer, check_block,
        expression->type, id, expression->span);
    SolMirValueId parameter = mir_append_parameter(lowerer, normal,
        expression->type, id, expression->span);
    if (failure == SOL_MIR_NONE || normal == SOL_MIR_NONE
        || result == SOL_MIR_NONE || parameter == SOL_MIR_NONE) {
        return mir_failure(lowerer);
    }
    SolMirSlice no_arguments = {lowerer->mir->edge_value_count, 0};
    SolMirSlice normal_arguments
        = mir_append_edge_arguments(lowerer, &result, 1);
    if (lowerer->failed) return mir_failure(lowerer);
    lowerer->pending_temporary_count = operation_depth;
    lowerer->mir->blocks[check_block].terminator = (SolMirTerminator){
        .kind = SOL_MIR_TERM_CHECK_REFINED,
        .span = expression->span,
        .as.check_refined = {
            .source_expression = id,
            .definition = definition,
            .obligation = obligation,
            .representation = temporary,
            .result = result,
            .normal_edge = {.block = normal, .arguments = normal_arguments},
            .failure_edge = {.block = failure, .arguments = no_arguments},
        },
    };
    if (!mir_start_block(lowerer, failure)
        || !mir_emit_exit_cleanup(lowerer, scope, expression->span)
        || !mir_set_failure_terminator(lowerer,
            SOL_MIR_TERM_RESUME_FAILURE, expression->span)) {
        return mir_failure(lowerer);
    }
    if (!mir_start_block(lowerer, normal)) return mir_failure(lowerer);
    return (LoweredValue){.reachable = true, .value = parameter};
}

static LoweredValue mir_lower_propagate(MirLowerer *lowerer,
    SolIrExpressionId id, const SolIrExpression *expression,
    const Scope *scope) {
    size_t operation_depth = lowerer->pending_temporary_count;
    SolIrExpressionId operand_id = expression->as.propagate.operand;
    LoweredValue operand = mir_lower_expression(lowerer, operand_id, scope);
    if (!operand.reachable) return operand;
    SolMirTemporaryId temporary = SOL_MIR_NONE;
    if (!mir_stage_temporary(lowerer, operand_id, operand.value, &temporary)) {
        return mir_failure(lowerer);
    }
    SolMirBlockId source = lowerer->current;
    SolMirBlockId value_block = mir_append_block(lowerer, expression->span);
    SolMirValueId value_parameter = mir_append_parameter(lowerer, value_block,
        expression->type, id, expression->span);
    SolMirBlockId residual_block = mir_append_block(lowerer, expression->span);
    SolMirValueId residual_parameter = mir_append_parameter(lowerer,
        residual_block, lowerer->callable->result, id,
        expression->span);
    SolMirValueId value_result = mir_append_terminator_result(lowerer, source,
        expression->type, id, expression->span);
    SolMirValueId residual_result = mir_append_terminator_result(lowerer,
        source, lowerer->callable->result, id, expression->span);
    if (value_block == SOL_MIR_NONE || residual_block == SOL_MIR_NONE
        || value_result == SOL_MIR_NONE || residual_result == SOL_MIR_NONE
        || value_parameter == SOL_MIR_NONE
        || residual_parameter == SOL_MIR_NONE) return mir_failure(lowerer);
    SolMirSlice value_arguments
        = mir_append_edge_arguments(lowerer, &value_result, 1);
    SolMirSlice residual_arguments
        = mir_append_edge_arguments(lowerer, &residual_result, 1);
    if (lowerer->failed) return mir_failure(lowerer);
    lowerer->pending_temporary_count = operation_depth;
    lowerer->mir->blocks[source].terminator = (SolMirTerminator){
        .kind = SOL_MIR_TERM_PROPAGATE,
        .span = expression->span,
        .as.propagate = {
            .source_expression = id,
            .kind = expression->as.propagate.kind,
            .operand = temporary,
            .value_result = value_result,
            .residual_result = residual_result,
            .value_edge = {.block = value_block, .arguments = value_arguments},
            .residual_edge = {
                .block = residual_block,
                .arguments = residual_arguments,
            },
        },
    };
    if (!mir_start_block(lowerer, residual_block)
        || !mir_finish_success(lowerer, scope, residual_parameter,
            expression->span)
        || !mir_start_block(lowerer, value_block)) return mir_failure(lowerer);
    return (LoweredValue){.reachable = true, .value = value_parameter};
}

static LoweredValue mir_lower_construct(MirLowerer *lowerer,
    SolIrExpressionId id, const SolIrExpression *expression,
    const Scope *scope) {
    if (expression->kind == SOL_IR_EXPR_CALL
        && (expression->as.call.kind == SOL_IR_CALL_FUNCTION
            || expression->as.call.kind == SOL_IR_CALL_CALLBACK
            || expression->as.call.kind == SOL_IR_CALL_METHOD
            || expression->as.call.kind == SOL_IR_CALL_CAPABILITY)) {
        return mir_lower_direct_call(lowerer, id, expression, scope);
    }
    SolMirConstructKind kind;
    SolIrDefinitionId definition = SOL_IR_NONE;
    SolIrVariantId variant = SOL_IR_NONE;
    SolIrSlice operands = {0};
    if (expression->kind == SOL_IR_EXPR_RECORD) {
        definition = expression->as.record.definition;
        operands = expression->as.record.fields;
        if (definition >= lowerer->ir->definition_count) {
            return mir_failure(lowerer);
        }
        SolIrDefinitionKind definition_kind
            = lowerer->ir->definitions[definition].kind;
        if (definition_kind != SOL_IR_DEFINITION_RECORD
            && definition_kind != SOL_IR_DEFINITION_CAPABILITY) {
            return mir_failure(lowerer);
        }
        kind = definition_kind == SOL_IR_DEFINITION_CAPABILITY
            ? SOL_MIR_CONSTRUCT_CAPABILITY : SOL_MIR_CONSTRUCT_RECORD;
    } else if (expression->kind == SOL_IR_EXPR_TUPLE) {
        operands = expression->as.tuple.operands;
        kind = SOL_MIR_CONSTRUCT_TUPLE;
    } else if (expression->kind == SOL_IR_EXPR_VARIANT) {
        variant = expression->as.variant.variant;
        if (variant >= lowerer->ir->variant_count) return mir_failure(lowerer);
        definition = lowerer->ir->variants[variant].owner;
        kind = SOL_MIR_CONSTRUCT_ENUM;
    } else if (expression->kind == SOL_IR_EXPR_CALL) {
        operands = expression->as.call.operands;
        switch (expression->as.call.kind) {
            case SOL_IR_CALL_BUILTIN_NONE:
                kind = SOL_MIR_CONSTRUCT_OPTION_NONE;
                break;
            case SOL_IR_CALL_BUILTIN_SOME:
                kind = SOL_MIR_CONSTRUCT_OPTION_SOME;
                break;
            case SOL_IR_CALL_BUILTIN_OK:
                kind = SOL_MIR_CONSTRUCT_RESULT_OK;
                break;
            case SOL_IR_CALL_BUILTIN_ERR:
                kind = SOL_MIR_CONSTRUCT_RESULT_ERR;
                break;
            case SOL_IR_CALL_ENUM_CONSTRUCTOR:
                kind = SOL_MIR_CONSTRUCT_ENUM;
                variant = expression->as.call.variant;
                if (variant >= lowerer->ir->variant_count) {
                    return mir_failure(lowerer);
                }
                definition = lowerer->ir->variants[variant].owner;
                break;
            case SOL_IR_CALL_DISTINCT_CONSTRUCTOR:
                definition = expression->as.call.definition;
                if (definition >= lowerer->ir->definition_count) {
                    return mir_failure(lowerer);
                }
                if (lowerer->ir->definitions[definition].kind
                    == SOL_IR_DEFINITION_REFINED) {
                    return mir_lower_checked_refined(lowerer, id,
                        expression, scope);
                }
                if (lowerer->ir->definitions[definition].kind
                    != SOL_IR_DEFINITION_DISTINCT) return mir_failure(lowerer);
                kind = SOL_MIR_CONSTRUCT_DISTINCT;
                break;
            default:
                mir_unsupported(lowerer, expression->span,
                    "call kind is outside the current P1a MIR checkpoint");
                return mir_unreachable();
        }
    } else {
        mir_unsupported(lowerer, expression->span,
            "expression is not a semantic MIR construction");
        return mir_unreachable();
    }
    size_t operation_depth = lowerer->pending_temporary_count;
    SolMirConstructOperand *staged = operands.count == 0 ? NULL
        : calloc(operands.count, sizeof(*staged));
    if (operands.count != 0 && staged == NULL) return mir_failure(lowerer);
    SolMirSlice construct_operands = {0};
    for (size_t index = 0; index < operands.count; ++index) {
        const SolIrOperand *operand
            = &lowerer->ir->operands[operands.offset + index];
        if (operand->access != SOL_ACCESS_OWNED) {
            free(staged);
            return mir_failure(lowerer);
        }
        LoweredValue lowered = mir_lower_expression(lowerer,
            operand->value, scope);
        if (!lowered.reachable) {
            free(staged);
            lowerer->pending_temporary_count = operation_depth;
            return lowered;
        }
        SolMirTemporaryId temporary = SOL_MIR_NONE;
        if (!mir_stage_temporary(lowerer, operand->value, lowered.value,
            &temporary)) {
            free(staged);
            return mir_failure(lowerer);
        }
        staged[index] = (SolMirConstructOperand){
            .formal = operand->formal,
            .source_expression = operand->value,
            .temporary = temporary,
        };
    }
    construct_operands.offset = lowerer->mir->construct_operand_count;
    for (size_t index = 0; index < operands.count; ++index) {
        if (!mir_append_construct_operand(lowerer, staged[index])) break;
        ++construct_operands.count;
    }
    free(staged);
    if (lowerer->failed) return mir_failure(lowerer);
    SolMirValueId result = mir_instruction_result(lowerer,
        (SolMirInstruction){
            .kind = SOL_MIR_INST_CONSTRUCT,
            .type = expression->type,
            .source_expression = id,
            .span = expression->span,
            .as.construct = {
                .kind = kind,
                .definition = definition,
                .variant = variant,
                .operands = construct_operands,
                .capability_roots = expression->capability_roots,
                .operation_roots = expression->operation_roots,
            },
        });
    lowerer->pending_temporary_count = operation_depth;
    return result == SOL_MIR_NONE ? mir_failure(lowerer)
        : (LoweredValue){.reachable = true, .value = result};
}

static LoweredValue mir_lower_if(MirLowerer *lowerer,
    const SolIrExpression *expression, const Scope *scope) {
    size_t temporary_depth = lowerer->pending_temporary_count;
    LoweredValue condition = mir_lower_expression(lowerer,
        expression->as.if_expr.condition, scope);
    if (!condition.reachable) return condition;
    SolMirBlockId branch = lowerer->current;
    SolMirBlockId then_block = mir_append_block(lowerer,
        lowerer->ir->expressions[expression->as.if_expr.then_branch].span);
    SolMirBlockId else_block = mir_append_block(lowerer,
        lowerer->ir->expressions[expression->as.if_expr.else_branch].span);
    if (then_block == SOL_MIR_NONE || else_block == SOL_MIR_NONE
        || !mir_set_branch(lowerer, branch, condition.value,
            then_block, else_block, expression->span)
        || !mir_start_block(lowerer, then_block)) return mir_failure(lowerer);
    LoweredValue then_value = mir_lower_expression(lowerer,
        expression->as.if_expr.then_branch, scope);
    if (lowerer->pending_temporary_count != temporary_depth) {
        return mir_failure(lowerer);
    }
    SolMirBlockId then_end = lowerer->current;
    if (!mir_start_block(lowerer, else_block)) return mir_failure(lowerer);
    LoweredValue else_value = mir_lower_expression(lowerer,
        expression->as.if_expr.else_branch, scope);
    if (lowerer->pending_temporary_count != temporary_depth) {
        return mir_failure(lowerer);
    }
    SolMirBlockId else_end = lowerer->current;
    if (!then_value.reachable && !else_value.reachable) {
        lowerer->current = SOL_MIR_NONE;
        return mir_unreachable();
    }
    SolMirBlockId join = mir_append_block(lowerer, expression->span);
    SolMirValueId parameter = mir_append_parameter(lowerer, join,
        expression->type,
        (SolIrExpressionId)(expression - lowerer->ir->expressions),
        expression->span);
    if (join == SOL_MIR_NONE || parameter == SOL_MIR_NONE) {
        return mir_failure(lowerer);
    }
    if (then_value.reachable && !mir_set_goto(lowerer, then_end, join,
        then_value.value, true, expression->span)) return mir_failure(lowerer);
    if (else_value.reachable && !mir_set_goto(lowerer, else_end, join,
        else_value.value, true, expression->span)) return mir_failure(lowerer);
    if (!mir_start_block(lowerer, join)) return mir_failure(lowerer);
    return (LoweredValue){.reachable = true, .value = parameter};
}

static bool mir_emit_pattern_bindings(MirLowerer *lowerer,
    SolIrExpressionId match_expression, SolIrArmId arm_id,
    size_t arm_ordinal, SolIrPatternId pattern_id,
    SolMirTemporaryId scrutinee, size_t depth) {
    if (depth > 64 || pattern_id >= lowerer->ir->pattern_count) return false;
    const SolIrPattern *pattern = &lowerer->ir->patterns[pattern_id];
    if (pattern->kind == SOL_IR_PATTERN_BINDING) {
        SolMirValueId value = mir_instruction_result(lowerer,
            (SolMirInstruction){
                .kind = SOL_MIR_INST_PATTERN_VALUE,
                .type = pattern->type,
                .source_expression = SOL_IR_NONE,
                .span = pattern->span,
                .as.pattern = {
                    .match_expression = match_expression,
                    .arm = arm_id,
                    .arm_ordinal = arm_ordinal,
                    .pattern = pattern_id,
                    .scrutinee = scrutinee,
                },
            });
        return value != SOL_MIR_NONE
            && mir_emit_local(lowerer, SOL_MIR_INST_STORAGE_LIVE,
                pattern->binding, pattern->span)
            && mir_append_instruction(lowerer, (SolMirInstruction){
                .kind = SOL_MIR_INST_STORE,
                .type = SOL_IR_NONE,
                .source_expression = SOL_IR_NONE,
                .span = pattern->span,
                .as.store = {
                    .place = {
                        .local = pattern->binding,
                        .source_place = SOL_IR_NONE,
                    },
                    .value = value,
                },
            }, false) != SOL_MIR_NONE;
    }
    for (size_t index = 0; index < pattern->children.count; ++index) {
        SolIrPatternId child = lowerer->ir->pattern_children[
            pattern->children.offset + index].pattern;
        if (!mir_emit_pattern_bindings(lowerer, match_expression, arm_id,
            arm_ordinal, child, scrutinee, depth + 1)) return false;
    }
    return true;
}

static LoweredValue mir_lower_match(MirLowerer *lowerer,
    SolIrExpressionId id, const SolIrExpression *expression,
    const Scope *scope) {
    size_t operation_depth = lowerer->pending_temporary_count;
    LoweredValue scrutinee = mir_lower_expression(lowerer,
        expression->as.match_expr.scrutinee, scope);
    if (!scrutinee.reachable) return scrutinee;
    SolMirTemporaryId temporary = SOL_MIR_NONE;
    if (!mir_stage_temporary(lowerer, expression->as.match_expr.scrutinee,
        scrutinee.value, &temporary)) return mir_failure(lowerer);
    size_t match_depth = lowerer->pending_temporary_count;
    SolIrTypeId bool_type = SOL_IR_NONE;
    for (size_t type = 0; type < lowerer->ir->type_count; ++type) {
        if (lowerer->ir->types[type].kind == SOL_IR_TYPE_BOOL) {
            bool_type = type;
            break;
        }
    }
    if (bool_type == SOL_IR_NONE) return mir_failure(lowerer);
    SolMirBlockId join = SOL_MIR_NONE;
    SolMirValueId parameter = SOL_MIR_NONE;
    for (size_t index = 0; index < expression->as.match_expr.arms.count;
        ++index) {
        SolIrArmId arm_id = lowerer->ir->arm_ids[
            expression->as.match_expr.arms.offset + index];
        const SolIrArm *arm = &lowerer->ir->arms[arm_id];
        SolMirValueId matched = mir_instruction_result(lowerer,
            (SolMirInstruction){
                .kind = SOL_MIR_INST_PATTERN_TEST,
                .type = bool_type,
                .source_expression = SOL_IR_NONE,
                .span = arm->span,
                .as.pattern = {
                    .match_expression = id,
                    .arm = arm_id,
                    .arm_ordinal = index,
                    .pattern = arm->pattern,
                    .scrutinee = temporary,
                },
            });
        SolMirBlockId selected = mir_append_block(lowerer, arm->span);
        SolMirBlockId next = mir_append_block(lowerer, expression->span);
        if (matched == SOL_MIR_NONE || selected == SOL_MIR_NONE
            || next == SOL_MIR_NONE
            || !mir_set_branch(lowerer, lowerer->current, matched,
                selected, next, arm->span)
            || !mir_start_block(lowerer, selected)) return mir_failure(lowerer);
        if (mir_append_instruction(lowerer, (SolMirInstruction){
                .kind = SOL_MIR_INST_MATCH_ARM,
                .type = SOL_IR_NONE,
                .source_expression = arm->body,
                .span = arm->span,
                .as.pattern = {
                    .match_expression = id,
                    .arm = arm_id,
                    .arm_ordinal = index,
                    .pattern = arm->pattern,
                    .scrutinee = temporary,
                },
            }, false) == SOL_MIR_NONE
            || !mir_emit_pattern_bindings(lowerer, id, arm_id, index,
                arm->pattern, temporary, 0)) return mir_failure(lowerer);
        Scope arm_scope = {
            .parent = scope,
            .cleanup = arm->cleanup,
            .region = SOL_IR_NONE,
            .cleanup_forward = true,
        };
        if (arm->guard != SOL_IR_NONE) {
            Scope guard_scope = arm_scope;
            guard_scope.cleanup_before_temporaries = true;
            guard_scope.temporary_cleanup_boundary = match_depth;
            guard_scope.temporary_parent_boundary = operation_depth;
            LoweredValue guard = mir_lower_expression(lowerer,
                arm->guard, &guard_scope);
            SolMirBlockId accepted = mir_append_block(lowerer, arm->span);
            SolMirBlockId rejected = mir_append_block(lowerer, arm->span);
            if (!guard.reachable || accepted == SOL_MIR_NONE
                || rejected == SOL_MIR_NONE
                || !mir_set_branch(lowerer, lowerer->current, guard.value,
                    accepted, rejected, arm->span)
                || !mir_start_block(lowerer, rejected)
                || !mir_emit_scope_cleanup(lowerer, &arm_scope, arm->span)
                || !mir_set_goto(lowerer, lowerer->current, next,
                    SOL_MIR_NONE, false, arm->span)
                || !mir_start_block(lowerer, accepted)) {
                return mir_failure(lowerer);
            }
        }
        if (!mir_emit_temporary_cleanup_to(lowerer, operation_depth, arm->span)) {
            return mir_failure(lowerer);
        }
        lowerer->pending_temporary_count = operation_depth;
        LoweredValue body = mir_lower_expression(lowerer, arm->body, &arm_scope);
        if (body.reachable) {
            if (!mir_emit_scope_cleanup(lowerer, &arm_scope, arm->span)) {
                return mir_failure(lowerer);
            }
            if (join == SOL_MIR_NONE) {
                join = mir_append_block(lowerer, expression->span);
                parameter = mir_append_parameter(lowerer, join,
                    expression->type, id, expression->span);
                if (join == SOL_MIR_NONE || parameter == SOL_MIR_NONE) {
                    return mir_failure(lowerer);
                }
            }
            if (!mir_set_goto(lowerer, lowerer->current, join,
                body.value, true, arm->span)) return mir_failure(lowerer);
        }
        lowerer->pending_temporary_count = match_depth;
        if (!mir_start_block(lowerer, next)) return mir_failure(lowerer);
    }
    if (!mir_emit_exit_cleanup(lowerer, scope, expression->span)) {
        return mir_failure(lowerer);
    }
    lowerer->mir->blocks[lowerer->current].terminator = (SolMirTerminator){
        .kind = SOL_MIR_TERM_MATCH_FAILURE,
        .span = expression->span,
        .as.match_failure = id,
    };
    lowerer->pending_temporary_count = operation_depth;
    if (join == SOL_MIR_NONE) {
        lowerer->current = SOL_MIR_NONE;
        return mir_unreachable();
    }
    if (!mir_start_block(lowerer, join)) return mir_failure(lowerer);
    return (LoweredValue){.reachable = true, .value = parameter};
}

static LoweredValue mir_lower_handler(MirLowerer *lowerer,
    SolIrExpressionId id, const SolIrExpression *expression,
    const Scope *scope) {
    const SolIrExpression *authority
        = &lowerer->ir->expressions[expression->as.handler.authority];
    const SolIrExpression *provider
        = &lowerer->ir->expressions[expression->as.handler.provider];
    if (authority->kind != SOL_IR_EXPR_PLACE
        || provider->kind != SOL_IR_EXPR_PLACE
        || authority->local_use != SOL_IR_LOCAL_USE_SHARED
        || provider->local_use != SOL_IR_LOCAL_USE_SHARED
        || lowerer->ir->places[authority->as.place].root_kind
            != SOL_IR_PLACE_ROOT_LOCAL
        || lowerer->ir->places[provider->as.place].root_kind
            != SOL_IR_PLACE_ROOT_LOCAL
        || lowerer->ir->places[authority->as.place].projections.count != 0
        || lowerer->ir->places[provider->as.place].projections.count != 0) {
        mir_unsupported(lowerer, expression->span,
            "P1a MIR handlers require direct shared local capabilities");
        return mir_unreachable();
    }
    if (mir_append_instruction(lowerer, (SolMirInstruction){
            .kind = SOL_MIR_INST_HANDLER_ENTER,
            .type = SOL_IR_NONE,
            .source_expression = id,
            .span = expression->span,
        }, false) == SOL_MIR_NONE) return mir_failure(lowerer);
    Scope handler_scope = {
        .parent = scope,
        .region = SOL_IR_NONE,
        .exits_handler = true,
        .handler = id,
    };
    LoweredValue body = mir_lower_expression(lowerer,
        expression->as.handler.body, &handler_scope);
    if (!body.reachable) return body;
    if (!mir_emit_scope_exit(lowerer, &handler_scope, expression->span)) {
        return mir_failure(lowerer);
    }
    body.value = mir_instruction_result(lowerer, (SolMirInstruction){
        .kind = SOL_MIR_INST_EXPRESSION_RESULT,
        .type = expression->type,
        .source_expression = id,
        .span = expression->span,
        .as.operand = body.value,
    });
    if (body.value == SOL_MIR_NONE) return mir_failure(lowerer);
    return body;
}

static bool mir_set_return(MirLowerer *lowerer, SolMirValueId value,
    SolSpan span) {
    if (lowerer->current >= lowerer->mir->block_count
        || lowerer->mir->blocks[lowerer->current].terminator.kind
            != SOL_MIR_TERM_INVALID) return false;
    lowerer->mir->blocks[lowerer->current].terminator = (SolMirTerminator){
        .kind = SOL_MIR_TERM_RETURN,
        .span = span,
        .as.value = value,
    };
    return true;
}

static bool mir_set_panic(MirLowerer *lowerer, SolMirValueId value,
    SolSpan span) {
    if (lowerer->current >= lowerer->mir->block_count
        || lowerer->mir->blocks[lowerer->current].terminator.kind
            != SOL_MIR_TERM_INVALID) return false;
    lowerer->mir->blocks[lowerer->current].terminator = (SolMirTerminator){
        .kind = SOL_MIR_TERM_PANIC,
        .span = span,
        .as.value = value,
    };
    return true;
}

static bool mir_set_contract_violation(MirLowerer *lowerer,
    SolObligationId obligation, SolSpan span) {
    if (lowerer->current >= lowerer->mir->block_count
        || lowerer->mir->blocks[lowerer->current].terminator.kind
            != SOL_MIR_TERM_INVALID) return false;
    lowerer->mir->blocks[lowerer->current].terminator = (SolMirTerminator){
        .kind = SOL_MIR_TERM_CONTRACT_VIOLATION,
        .span = span,
        .as.contract_violation = obligation,
    };
    return true;
}

static bool mir_emit_contract_check(MirLowerer *lowerer,
    SolObligationId obligation_id, SolMirValueId result,
    SolMirBlockId satisfied, SolSpan span) {
    SolMirBlockId source = lowerer->current;
    SolMirBlockId violation = mir_append_block(lowerer, span);
    SolMirBlockId failure = mir_append_block(lowerer, span);
    const SolIrObligation *obligation
        = &lowerer->ir->obligations[obligation_id];
    if (violation == SOL_MIR_NONE || failure == SOL_MIR_NONE) return false;
    SolMirSlice empty = {lowerer->mir->edge_value_count, 0};
    SolMirSlice satisfied_arguments = empty;
    if (result != SOL_MIR_NONE) {
        satisfied_arguments = mir_append_edge_arguments(lowerer, &result, 1);
    }
    if (lowerer->failed) return false;
    lowerer->mir->blocks[source].terminator = (SolMirTerminator){
        .kind = SOL_MIR_TERM_CHECK_CONTRACT,
        .span = span,
        .as.check_contract = {
            .obligation = obligation_id,
            .phase = obligation->kind,
            .outcome = obligation->outcome,
            .result = result,
            .satisfied_edge = {
                .block = satisfied,
                .arguments = satisfied_arguments,
            },
            .violation_edge = {.block = violation, .arguments = empty},
            .failure_edge = {.block = failure, .arguments = empty},
        },
    };
    if (!mir_start_block(lowerer, violation)
        || !mir_emit_exit_cleanup(lowerer, NULL, span)
        || !mir_set_contract_violation(lowerer, obligation_id, span)
        || !mir_start_block(lowerer, failure)
        || !mir_emit_exit_cleanup(lowerer, NULL, span)
        || !mir_set_failure_terminator(lowerer,
            SOL_MIR_TERM_RESUME_FAILURE, span)) return false;
    return true;
}

static bool mir_obligation_owned_by_callable(const SolIrObligation *obligation,
    const SolIrCallable *callable) {
    return obligation->owner_kind == SOL_CONTRACT_OWNER_ITEM
        && obligation->owner == callable->owner;
}

static bool mir_lower_contract_entry(MirLowerer *lowerer) {
    for (size_t id = 0; id < lowerer->ir->obligation_count; ++id) {
        const SolIrObligation *obligation = &lowerer->ir->obligations[id];
        if (!mir_obligation_owned_by_callable(obligation, lowerer->callable)
            || obligation->kind != SOL_CONTRACT_REQUIRES) continue;
        SolMirBlockId satisfied = mir_append_block(lowerer,
            lowerer->ir->expressions[obligation->predicate].span);
        if (satisfied == SOL_MIR_NONE
            || !mir_emit_contract_check(lowerer, id, SOL_MIR_NONE,
                satisfied, lowerer->ir->expressions[obligation->predicate].span)
            || !mir_start_block(lowerer, satisfied)) return false;
    }
    for (size_t id = 0; id < lowerer->ir->snapshot_count; ++id) {
        const SolIrSnapshot *snapshot = &lowerer->ir->snapshots[id];
        if (snapshot->obligation >= lowerer->ir->obligation_count
            || !mir_obligation_owned_by_callable(
                &lowerer->ir->obligations[snapshot->obligation],
                lowerer->callable)) continue;
        const SolIrExpression *read = &lowerer->ir->expressions[snapshot->read];
        if (mir_append_instruction(lowerer, (SolMirInstruction){
                .kind = SOL_MIR_INST_CAPTURE_SNAPSHOT,
                .type = SOL_IR_NONE,
                .source_expression = snapshot->read,
                .span = read->span,
                .as.snapshot = id,
            }, false) == SOL_MIR_NONE) return false;
    }
    lowerer->mir->contract_body = lowerer->current;
    return true;
}

static bool mir_lower_contract_epilogue(MirLowerer *lowerer,
    SolMirValueId result) {
    for (size_t id = 0; id < lowerer->ir->obligation_count; ++id) {
        const SolIrObligation *obligation = &lowerer->ir->obligations[id];
        if (!mir_obligation_owned_by_callable(obligation, lowerer->callable)
            || obligation->kind != SOL_CONTRACT_ENSURES) continue;
        SolSpan span = lowerer->ir->expressions[obligation->predicate].span;
        SolMirBlockId satisfied = mir_append_block(lowerer, span);
        SolMirValueId parameter = mir_append_parameter(lowerer, satisfied,
            lowerer->callable->result, SOL_IR_NONE, span);
        if (satisfied == SOL_MIR_NONE || parameter == SOL_MIR_NONE
            || !mir_emit_contract_check(lowerer, id, result, satisfied, span)
            || !mir_start_block(lowerer, satisfied)) return false;
        result = parameter;
    }
    return mir_emit_exit_cleanup(lowerer, NULL, lowerer->callable->span)
        && mir_set_return(lowerer, result, lowerer->callable->span);
}

static SolMirBlockId mir_ensure_loop_exit(MirLowerer *lowerer,
    SolMirLoopId loop, SolSpan span) {
    if (loop >= lowerer->mir->loop_count) {
        lowerer->failed = true;
        return SOL_MIR_NONE;
    }
    if (lowerer->mir->loops[loop].exit == SOL_MIR_NONE) {
        lowerer->mir->loops[loop].exit = mir_append_block(lowerer, span);
    }
    return lowerer->mir->loops[loop].exit;
}

static bool mir_set_loop_transfer(MirLowerer *lowerer,
    SolIrStatementId statement, bool is_break, const Scope *scope,
    SolSpan span) {
    if (lowerer->loop == NULL || lowerer->current >= lowerer->mir->block_count
        || lowerer->mir->blocks[lowerer->current].terminator.kind
            != SOL_MIR_TERM_INVALID
        || !mir_emit_temporary_cleanup_to(lowerer,
            lowerer->loop->temporary_entry_depth, span)
        || !mir_emit_cleanup_to(lowerer, scope,
            lowerer->loop->boundary, span)) return false;
    SolMirBlockId target = is_break
        ? mir_ensure_loop_exit(lowerer, lowerer->loop->id, span)
        : lowerer->mir->loops[lowerer->loop->id].header;
    if (target == SOL_MIR_NONE) return false;
    SolMirSlice arguments = {lowerer->mir->edge_value_count, 0};
    lowerer->mir->blocks[lowerer->current].terminator = (SolMirTerminator){
        .kind = is_break ? SOL_MIR_TERM_BREAK : SOL_MIR_TERM_CONTINUE,
        .span = span,
        .as.transfer = {
            .statement = statement,
            .loop = lowerer->loop->id,
            .edge = {.block = target, .arguments = arguments},
        },
    };
    return true;
}

static LoweredValue mir_lower_loop_statement(MirLowerer *lowerer,
    SolIrStatementId statement_id, const SolIrStatement *statement,
    const Scope *scope) {
    bool is_while = statement->kind == SOL_IR_STATEMENT_WHILE;
    SolMirBlockId preheader = lowerer->current;
    SolMirBlockId header = mir_append_block(lowerer, statement->span);
    SolMirLoopId loop = mir_append_loop(lowerer, (SolMirLoop){
        .statement = statement_id,
        .parent = lowerer->loop == NULL ? SOL_MIR_NONE : lowerer->loop->id,
        .preheader = preheader,
        .header = header,
        .condition = is_while ? SOL_MIR_NONE : header,
        .body = SOL_MIR_NONE,
        .backedge = SOL_MIR_NONE,
        .exit = SOL_MIR_NONE,
        .obligations = statement->loop_obligations,
        .span = statement->span,
    });
    if (header == SOL_MIR_NONE || loop == SOL_MIR_NONE
        || !mir_set_goto(lowerer, preheader, header, SOL_MIR_NONE, false,
            statement->span)
        || !mir_start_block(lowerer, header)) return mir_failure(lowerer);
    LoopContext context = {
        .parent = lowerer->loop,
        .id = loop,
        .boundary = scope,
        .temporary_entry_depth = lowerer->pending_temporary_count,
    };
    const LoopContext *saved_loop = lowerer->loop;
    lowerer->loop = &context;
    if (is_while) {
        LoweredValue condition = mir_lower_expression(lowerer,
            statement->condition, scope);
        if (lowerer->pending_temporary_count
            != context.temporary_entry_depth) {
            lowerer->loop = saved_loop;
            return mir_failure(lowerer);
        }
        if (condition.reachable) {
            SolMirBlockId condition_end = lowerer->current;
            lowerer->mir->loops[loop].condition = condition_end;
            SolMirBlockId body = mir_append_block(lowerer,
                lowerer->ir->expressions[statement->expression].span);
            SolMirBlockId exit = mir_ensure_loop_exit(lowerer, loop,
                statement->span);
            lowerer->mir->loops[loop].body = body;
            if (body == SOL_MIR_NONE || exit == SOL_MIR_NONE
                || !mir_set_branch(lowerer, condition_end, condition.value,
                    body, exit, statement->span)
                || !mir_start_block(lowerer, body)) {
                lowerer->loop = saved_loop;
                return mir_failure(lowerer);
            }
            LoweredValue body_value = mir_lower_expression(lowerer,
                statement->expression, scope);
            if (lowerer->pending_temporary_count
                != context.temporary_entry_depth) {
                lowerer->loop = saved_loop;
                return mir_failure(lowerer);
            }
            if (body_value.reachable) {
                lowerer->mir->loops[loop].backedge = lowerer->current;
                if (!mir_set_goto(lowerer, lowerer->current, header,
                    SOL_MIR_NONE, false, statement->span)) {
                    lowerer->loop = saved_loop;
                    return mir_failure(lowerer);
                }
            }
        }
    } else {
        SolMirBlockId body = mir_append_block(lowerer,
            lowerer->ir->expressions[statement->expression].span);
        lowerer->mir->loops[loop].body = body;
        if (body == SOL_MIR_NONE
            || !mir_set_goto(lowerer, header, body, SOL_MIR_NONE, false,
                statement->span)
            || !mir_start_block(lowerer, body)) {
            lowerer->loop = saved_loop;
            return mir_failure(lowerer);
        }
        LoweredValue body_value = mir_lower_expression(lowerer,
            statement->expression, scope);
        if (lowerer->pending_temporary_count
            != context.temporary_entry_depth) {
            lowerer->loop = saved_loop;
            return mir_failure(lowerer);
        }
        if (body_value.reachable) {
            lowerer->mir->loops[loop].backedge = lowerer->current;
            if (!mir_set_goto(lowerer, lowerer->current, header,
                SOL_MIR_NONE, false, statement->span)) {
                lowerer->loop = saved_loop;
                return mir_failure(lowerer);
            }
        }
    }
    lowerer->loop = saved_loop;
    SolMirBlockId exit = lowerer->mir->loops[loop].exit;
    if (exit == SOL_MIR_NONE) {
        lowerer->current = SOL_MIR_NONE;
        return mir_unreachable();
    }
    if (!mir_start_block(lowerer, exit)) return mir_failure(lowerer);
    SolIrTypeId unit = SOL_IR_NONE;
    for (size_t type = 0; type < lowerer->ir->type_count; ++type) {
        if (lowerer->ir->types[type].kind == SOL_IR_TYPE_UNIT) {
            unit = type;
            break;
        }
    }
    if (unit == SOL_IR_NONE) return mir_failure(lowerer);
    SolMirValueId value = mir_instruction_result(lowerer, (SolMirInstruction){
        .kind = SOL_MIR_INST_CONST_UNIT,
        .type = unit,
        .source_expression = SOL_IR_NONE,
        .span = statement->span,
    });
    return value == SOL_MIR_NONE ? mir_failure(lowerer)
        : (LoweredValue){.reachable = true, .value = value};
}

static LoweredValue mir_lower_block(MirLowerer *lowerer,
    const SolIrExpression *expression, const Scope *parent) {
    Scope scope = {
        .parent = parent,
        .cleanup = {.offset = expression->as.block.cleanup.offset, .count = 0},
        .region = SOL_IR_NONE,
    };
    LoweredValue last = {.reachable = true, .value = SOL_MIR_NONE};
    for (size_t index = 0; index < expression->as.block.statements.count;
        ++index) {
        SolIrStatementId statement_id = lowerer->ir->statement_ids[
            expression->as.block.statements.offset + index];
        const SolIrStatement *statement = &lowerer->ir->statements[statement_id];
        if (!last.reachable) break;
        switch (statement->kind) {
            case SOL_IR_STATEMENT_LET: {
                LoweredValue value = mir_lower_expression(lowerer,
                    statement->expression, &scope);
                if (!value.reachable) return value;
                if (!mir_emit_local(lowerer, SOL_MIR_INST_STORAGE_LIVE,
                        statement->local, statement->span)
                    || mir_append_instruction(lowerer, (SolMirInstruction){
                        .kind = SOL_MIR_INST_STORE,
                        .type = SOL_IR_NONE,
                        .source_expression = statement->expression,
                        .span = statement->span,
                        .as.store = {
                            .place = {
                                .local = statement->local,
                                .source_place = SOL_IR_NONE,
                            },
                            .value = value.value,
                        },
                    }, false) == SOL_MIR_NONE) return mir_failure(lowerer);
                ++scope.cleanup.count;
                last.value = SOL_MIR_NONE;
                break;
            }
            case SOL_IR_STATEMENT_DECLARE:
                if (!mir_emit_local(lowerer, SOL_MIR_INST_STORAGE_LIVE,
                    statement->local, statement->span)) return mir_failure(lowerer);
                ++scope.cleanup.count;
                last.value = SOL_MIR_NONE;
                break;
            case SOL_IR_STATEMENT_ASSIGNMENT: {
                if (statement->target >= lowerer->ir->expression_count) {
                    return mir_failure(lowerer);
                }
                const SolIrExpression *target
                    = &lowerer->ir->expressions[statement->target];
                if (target->kind != SOL_IR_EXPR_PLACE
                    || target->as.place >= lowerer->ir->place_count) {
                    return mir_failure(lowerer);
                }
                const SolIrPlace *place = &lowerer->ir->places[target->as.place];
                if (place->root_kind != SOL_IR_PLACE_ROOT_LOCAL) {
                    mir_unsupported(lowerer, statement->span,
                        "P1a MIR supports only local-rooted assignment");
                    return mir_unreachable();
                }
                SolMirTemporaryId previous = SOL_MIR_NONE;
                if (statement->operator_kind != SOL_TOKEN_EQUAL) {
                    SolMirValueId old_value = mir_instruction_result(lowerer,
                        (SolMirInstruction){
                            .kind = SOL_MIR_INST_LOAD_UPDATE,
                            .type = target->type,
                            .source_expression = statement->target,
                            .span = target->span,
                            .as.update_load = {
                                .statement = statement_id,
                                .place = {
                                    .local = place->local,
                                    .source_place = target->as.place,
                                },
                            },
                        });
                    if (old_value == SOL_MIR_NONE
                        || !mir_stage_temporary(lowerer, statement->target,
                            old_value, &previous)) return mir_failure(lowerer);
                }
                LoweredValue value = mir_lower_expression(lowerer,
                    statement->expression, &scope);
                if (!value.reachable) return value;
                if (statement->operator_kind != SOL_TOKEN_EQUAL) {
                    SolMirValueId updated = mir_instruction_result(lowerer,
                        (SolMirInstruction){
                            .kind = SOL_MIR_INST_COMPOUND_UPDATE,
                            .type = target->type,
                            .source_expression = statement->expression,
                            .span = statement->span,
                            .as.compound_update = {
                                .statement = statement_id,
                                .operator_kind = statement->operator_kind,
                                .place = {
                                    .local = place->local,
                                    .source_place = target->as.place,
                                },
                                .previous = previous,
                                .right = value.value,
                            },
                        });
                    if (updated == SOL_MIR_NONE
                        || lowerer->pending_temporary_count == 0
                        || lowerer->pending_temporaries[
                            lowerer->pending_temporary_count - 1] != previous) {
                        return mir_failure(lowerer);
                    }
                    --lowerer->pending_temporary_count;
                    value.value = updated;
                }
                if (mir_append_instruction(lowerer, (SolMirInstruction){
                        .kind = SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED,
                        .type = SOL_IR_NONE,
                        .source_expression = statement->target,
                        .span = statement->span,
                        .as.place = {
                            .local = place->local,
                            .source_place = target->as.place,
                        },
                    }, false) == SOL_MIR_NONE
                    || mir_append_instruction(lowerer, (SolMirInstruction){
                        .kind = SOL_MIR_INST_STORE,
                        .type = SOL_IR_NONE,
                        .source_expression = statement->expression,
                        .span = statement->span,
                        .as.store = {
                            .place = {
                                .local = place->local,
                                .source_place = target->as.place,
                            },
                            .value = value.value,
                        },
                    }, false) == SOL_MIR_NONE) return mir_failure(lowerer);
                last.value = SOL_MIR_NONE;
                break;
            }
            case SOL_IR_STATEMENT_EXPRESSION:
                last = mir_lower_expression(lowerer, statement->expression, &scope);
                break;
            case SOL_IR_STATEMENT_RETURN: {
                LoweredValue value = mir_lower_expression(lowerer,
                    statement->expression, &scope);
                if (!value.reachable) return value;
                if (!mir_finish_success(lowerer, &scope, value.value,
                    statement->span)) {
                    return mir_failure(lowerer);
                }
                return mir_unreachable();
            }
            case SOL_IR_STATEMENT_PANIC: {
                LoweredValue value = mir_lower_expression(lowerer,
                    statement->expression, &scope);
                if (!value.reachable) return value;
                if (!mir_emit_exit_cleanup(lowerer, &scope, statement->span)
                    || !mir_set_panic(lowerer, value.value, statement->span)) {
                    return mir_failure(lowerer);
                }
                return mir_unreachable();
            }
            case SOL_IR_STATEMENT_LOOP:
            case SOL_IR_STATEMENT_WHILE:
                last = mir_lower_loop_statement(lowerer, statement_id,
                    statement, &scope);
                break;
            case SOL_IR_STATEMENT_BREAK:
            case SOL_IR_STATEMENT_CONTINUE:
                if (!mir_set_loop_transfer(lowerer, statement_id,
                    statement->kind == SOL_IR_STATEMENT_BREAK,
                    &scope, statement->span)) return mir_failure(lowerer);
                return mir_unreachable();
            case SOL_IR_STATEMENT_REGION: {
                if (mir_append_instruction(lowerer, (SolMirInstruction){
                    .kind = SOL_MIR_INST_REGION_ENTER,
                    .type = SOL_IR_NONE,
                    .source_expression = SOL_IR_NONE,
                    .span = statement->span,
                    .as.region = statement_id,
                }, false) == SOL_MIR_NONE) return mir_failure(lowerer);
                Scope region_scope = {
                    .parent = &scope,
                    .cleanup = {0},
                    .region = statement_id,
                };
                LoweredValue body = mir_lower_expression(lowerer,
                    statement->expression, &region_scope);
                if (!body.reachable) return body;
                if (mir_append_instruction(lowerer, (SolMirInstruction){
                    .kind = SOL_MIR_INST_REGION_EXIT,
                    .type = SOL_IR_NONE,
                    .source_expression = SOL_IR_NONE,
                    .span = statement->span,
                    .as.region = statement_id,
                }, false) == SOL_MIR_NONE) return mir_failure(lowerer);
                last = body;
                break;
            }
            case SOL_IR_STATEMENT_REQUIRE: {
                size_t temporary_depth = lowerer->pending_temporary_count;
                LoweredValue condition = mir_lower_expression(lowerer,
                    statement->condition, &scope);
                if (!condition.reachable) return condition;
                SolMirBlockId branch = lowerer->current;
                SolMirBlockId required = mir_append_block(lowerer, statement->span);
                SolMirBlockId fallback = mir_append_block(lowerer,
                    lowerer->ir->expressions[statement->expression].span);
                if (required == SOL_MIR_NONE || fallback == SOL_MIR_NONE
                    || !mir_set_branch(lowerer, branch, condition.value,
                        required, fallback, statement->span)
                    || !mir_start_block(lowerer, fallback)) {
                    return mir_failure(lowerer);
                }
                LoweredValue failed = mir_lower_expression(lowerer,
                    statement->expression, &scope);
                if (lowerer->pending_temporary_count
                    != temporary_depth) return mir_failure(lowerer);
                if (failed.reachable) {
                    lowerer->failed = true;
                    return mir_unreachable();
                }
                if (!mir_start_block(lowerer, required)) {
                    return mir_failure(lowerer);
                }
                SolIrTypeId unit = SOL_IR_NONE;
                for (size_t type = 0; type < lowerer->ir->type_count; ++type) {
                    if (lowerer->ir->types[type].kind == SOL_IR_TYPE_UNIT) {
                        unit = type;
                        break;
                    }
                }
                if (unit == SOL_IR_NONE) return mir_failure(lowerer);
                last = (LoweredValue){
                    .reachable = true,
                    .value = mir_instruction_result(lowerer, (SolMirInstruction){
                        .kind = SOL_MIR_INST_CONST_UNIT,
                        .type = unit,
                        .source_expression = SOL_IR_NONE,
                        .span = statement->span,
                    }),
                };
                if (last.value == SOL_MIR_NONE) return mir_failure(lowerer);
                break;
            }
            case SOL_IR_STATEMENT_UNREACHABLE:
                if (!mir_emit_exit_cleanup(lowerer, &scope, statement->span)) {
                    return mir_failure(lowerer);
                }
                if (!mir_set_failure_terminator(lowerer,
                    SOL_MIR_TERM_UNREACHABLE, statement->span)) {
                    return mir_failure(lowerer);
                }
                lowerer->mir->blocks[lowerer->current].terminator
                    .as.unreachable.statement = statement_id;
                lowerer->mir->blocks[lowerer->current].terminator
                    .as.unreachable.obligation
                    = statement->unreachable_obligations.offset;
                return mir_unreachable();
            default:
                mir_unsupported(lowerer, statement->span,
                    "statement is outside the initial P1a MIR checkpoint");
                return mir_unreachable();
        }
    }
    if (!last.reachable) return last;
    if (last.value == SOL_MIR_NONE) {
        last.value = mir_instruction_result(lowerer, (SolMirInstruction){
            .kind = SOL_MIR_INST_CONST_UNIT,
            .type = expression->type,
            .source_expression = SOL_IR_NONE,
            .span = expression->span,
        });
        if (last.value == SOL_MIR_NONE) return mir_failure(lowerer);
    }
    SolIrExpressionId block_id
        = (SolIrExpressionId)(expression - lowerer->ir->expressions);
    last.value = mir_instruction_result(lowerer, (SolMirInstruction){
        .kind = SOL_MIR_INST_EXPRESSION_RESULT,
        .type = expression->type,
        .source_expression = block_id,
        .span = expression->span,
        .as.operand = last.value,
    });
    if (last.value == SOL_MIR_NONE) return mir_failure(lowerer);
    if (!mir_emit_cleanup_slice(lowerer, scope.cleanup, expression->span)) {
        return mir_failure(lowerer);
    }
    return last;
}

static LoweredValue mir_lower_expression(MirLowerer *lowerer,
    SolIrExpressionId id, const Scope *scope) {
    if (id >= lowerer->ir->expression_count
        || lowerer->current >= lowerer->mir->block_count) {
        return mir_failure(lowerer);
    }
    const SolIrExpression *expression = &lowerer->ir->expressions[id];
    SolMirInstruction instruction = {
        .type = expression->type,
        .source_expression = id,
        .span = expression->span,
    };
    switch (expression->kind) {
        case SOL_IR_EXPR_INTEGER:
            instruction.kind = SOL_MIR_INST_CONST_INT64;
            instruction.as.integer = expression->as.integer;
            break;
        case SOL_IR_EXPR_BOOL:
            instruction.kind = SOL_MIR_INST_CONST_BOOL;
            instruction.as.boolean = expression->as.boolean;
            break;
        case SOL_IR_EXPR_STRING:
            instruction.kind = SOL_MIR_INST_CONST_TEXT;
            break;
        case SOL_IR_EXPR_UNIT:
            instruction.kind = SOL_MIR_INST_CONST_UNIT;
            break;
        case SOL_IR_EXPR_PLACE: {
            if (expression->as.place >= lowerer->ir->place_count) {
                return mir_failure(lowerer);
            }
            const SolIrPlace *place = &lowerer->ir->places[expression->as.place];
            if (place->root_kind != SOL_IR_PLACE_ROOT_LOCAL
                || (expression->local_use != SOL_IR_LOCAL_USE_COPY
                    && expression->local_use != SOL_IR_LOCAL_USE_MOVE)) {
                mir_unsupported(lowerer, expression->span,
                    "P1a MIR checkpoint supports only copied or moved whole locals");
                return mir_unreachable();
            }
            instruction.kind = expression->local_use == SOL_IR_LOCAL_USE_COPY
                ? SOL_MIR_INST_LOAD_COPY : SOL_MIR_INST_LOAD_MOVE;
            instruction.as.place = (SolMirPlace){
                .local = place->local,
                .source_place = expression->as.place,
            };
            break;
        }
        case SOL_IR_EXPR_UNARY: {
            LoweredValue operand = mir_lower_expression(lowerer,
                expression->as.unary.operand, scope);
            if (!operand.reachable) return operand;
            instruction.kind = SOL_MIR_INST_UNARY;
            instruction.as.unary.operator_kind
                = expression->as.unary.operator_kind;
            instruction.as.unary.operand = operand.value;
            break;
        }
        case SOL_IR_EXPR_BINARY: {
            if (expression->as.binary.operator_kind == SOL_TOKEN_AMP_AMP
                || expression->as.binary.operator_kind == SOL_TOKEN_PIPE_PIPE) {
                return mir_lower_short_circuit(lowerer, expression, scope);
            }
            LoweredValue left = mir_lower_expression(lowerer,
                expression->as.binary.left, scope);
            if (!left.reachable) return left;
            LoweredValue right = mir_lower_expression(lowerer,
                expression->as.binary.right, scope);
            if (!right.reachable) return right;
            instruction.kind = SOL_MIR_INST_BINARY;
            instruction.as.binary.operator_kind
                = expression->as.binary.operator_kind;
            instruction.as.binary.left = left.value;
            instruction.as.binary.right = right.value;
            break;
        }
        case SOL_IR_EXPR_IF:
            return mir_lower_if(lowerer, expression, scope);
        case SOL_IR_EXPR_MATCH:
            return mir_lower_match(lowerer, id, expression, scope);
        case SOL_IR_EXPR_PROPAGATE:
            return mir_lower_propagate(lowerer, id, expression, scope);
        case SOL_IR_EXPR_HANDLE:
            return mir_lower_handler(lowerer, id, expression, scope);
        case SOL_IR_EXPR_BLOCK:
            return mir_lower_block(lowerer, expression, scope);
        case SOL_IR_EXPR_CALL:
        case SOL_IR_EXPR_RECORD:
        case SOL_IR_EXPR_TUPLE:
        case SOL_IR_EXPR_VARIANT:
            return mir_lower_construct(lowerer, id, expression, scope);
        default:
            mir_unsupported(lowerer, expression->span,
                "expression is outside the initial P1a MIR checkpoint");
            return mir_unreachable();
    }
    SolMirValueId value = mir_instruction_result(lowerer, instruction);
    return value == SOL_MIR_NONE ? mir_failure(lowerer)
        : (LoweredValue){.reachable = true, .value = value};
}

static bool mir_slice_valid(SolMirSlice slice, size_t count) {
    return slice.offset <= count && slice.count <= count - slice.offset;
}

static bool mir_ir_slice_equal(SolIrSlice left, SolIrSlice right) {
    return left.offset == right.offset && left.count == right.count;
}

static bool mir_type_matches_call(const SolIr *ir,
    const SolIrCallable *target, SolIrSlice arguments,
    SolIrTypeId symbolic, SolIrTypeId actual) {
    if (symbolic >= ir->type_count || actual >= ir->type_count) return false;
    const SolIrType *type = &ir->types[symbolic];
    if (type->kind != SOL_IR_TYPE_PARAMETER) return symbolic == actual;
    if (type->definition < target->generic_parameters.offset
        || type->definition - target->generic_parameters.offset
            >= target->generic_parameters.count) return symbolic == actual;
    size_t ordinal = type->definition - target->generic_parameters.offset;
    return ordinal < arguments.count
        && ir->type_ids[arguments.offset + ordinal] == actual;
}

static bool mir_span_valid(const SolIr *ir, SolSpan span) {
    return span.start <= span.end && span.end <= ir->source_length;
}

static bool mir_pattern_contains(const SolIr *ir, SolIrPatternId root,
    SolIrPatternId target, size_t depth) {
    if (depth > 64 || root >= ir->pattern_count) return false;
    if (root == target) return true;
    const SolIrPattern *pattern = &ir->patterns[root];
    for (size_t index = 0; index < pattern->children.count; ++index) {
        if (mir_pattern_contains(ir, ir->pattern_children[
                pattern->children.offset + index].pattern, target,
            depth + 1)) return true;
    }
    return false;
}

static bool mir_match_has_arm(const SolIr *ir,
    const SolIrExpression *match, SolIrArmId arm) {
    for (size_t index = 0; index < match->as.match_expr.arms.count; ++index) {
        if (ir->arm_ids[match->as.match_expr.arms.offset + index] == arm) {
            return true;
        }
    }
    return false;
}

static bool mir_place_valid(const SolIr *ir, const SolIrCallable *callable,
    SolMirPlace place, bool synthetic, SolIrTypeId *type) {
    if (place.local >= ir->local_count
        || ir->locals[place.local].owner != callable->owner) return false;
    if (place.source_place == SOL_IR_NONE) {
        if (!synthetic) return false;
        *type = ir->locals[place.local].type;
        return true;
    }
    if (place.source_place >= ir->place_count) return false;
    const SolIrPlace *source = &ir->places[place.source_place];
    if (source->root_kind != SOL_IR_PLACE_ROOT_LOCAL
        || source->local != place.local) return false;
    for (size_t index = 0; index < source->projections.count; ++index) {
        SolIrProjectionKind kind = ir->projections[
            source->projections.offset + index].kind;
        if (kind != SOL_IR_PROJECTION_FIELD
            && kind != SOL_IR_PROJECTION_TUPLE_FIELD) return false;
    }
    *type = source->type;
    return true;
}

static bool mir_place_prefix(const SolIr *ir, SolIrPlaceId prefix_id,
    SolIrPlaceId place_id) {
    const SolIrPlace *prefix = &ir->places[prefix_id];
    const SolIrPlace *place = &ir->places[place_id];
    if (prefix->local != place->local
        || prefix->projections.count > place->projections.count) return false;
    for (size_t index = 0; index < prefix->projections.count; ++index) {
        const SolIrProjection *a
            = &ir->projections[prefix->projections.offset + index];
        const SolIrProjection *b
            = &ir->projections[place->projections.offset + index];
        if (a->kind != b->kind) return false;
        if (a->kind == SOL_IR_PROJECTION_FIELD && a->field != b->field) {
            return false;
        }
        if (a->kind == SOL_IR_PROJECTION_TUPLE_FIELD
            && a->ordinal != b->ordinal) return false;
    }
    return true;
}

static bool mir_places_overlap(const SolIr *ir, SolIrPlaceId left,
    SolIrPlaceId right) {
    return mir_place_prefix(ir, left, right)
        || mir_place_prefix(ir, right, left);
}

static bool mir_capability_effects_match(const SolIr *ir,
    const SolIrExpression *call, const SolIrCallable *target,
    const SolIrExpression *receiver) {
    size_t actual = 0;
    for (size_t index = 0; index < target->effects.count; ++index) {
        const SolIrEffect *expected
            = &ir->effects[target->effects.offset + index];
        size_t copies = expected->authority_kind == SOL_IR_AUTHORITY_SELF
            ? receiver->capability_roots.count : 1;
        if (expected->authority_kind == SOL_IR_AUTHORITY_LOCAL) return false;
        for (size_t copy = 0; copy < copies; ++copy) {
            if (actual >= call->as.call.effects.count) return false;
            const SolIrEffect *effect
                = &ir->effects[call->as.call.effects.offset + actual++];
            if (strcmp(effect->name, expected->name) != 0) return false;
            if (expected->authority_kind == SOL_IR_AUTHORITY_SELF) {
                if (effect->authority_kind != SOL_IR_AUTHORITY_LOCAL
                    || effect->authority != ir->roots[
                        receiver->capability_roots.offset + copy]) return false;
            } else if (effect->authority_kind != expected->authority_kind
                || effect->authority != expected->authority) {
                return false;
            }
        }
    }
    return actual == call->as.call.effects.count;
}

static bool mir_source_reaches_match(const SolIr *ir,
    SolIrExpressionId expression_id, SolIrExpressionId sought, size_t depth) {
    if (expression_id >= ir->expression_count || depth > ir->expression_count) {
        return false;
    }
    const SolIrExpression *expression = &ir->expressions[expression_id];
    if (expression_id == sought) {
        if (expression->kind == SOL_IR_EXPR_CALL) {
            SolIrExpressionId callee = expression->as.call.kind
                    == SOL_IR_CALL_METHOD
                ? expression->as.call.receiver
                : (expression->as.call.kind == SOL_IR_CALL_CALLBACK
                        || expression->as.call.kind == SOL_IR_CALL_CAPABILITY)
                    ? expression->as.call.callee : SOL_IR_NONE;
            if (callee != SOL_IR_NONE && mir_type_is(ir,
                ir->expressions[callee].type, SOL_IR_TYPE_NEVER)) return false;
            for (size_t index = 0; index < expression->as.call.operands.count;
                ++index) {
                SolIrExpressionId operand = ir->operands[
                    expression->as.call.operands.offset + index].value;
                if (mir_type_is(ir, ir->expressions[operand].type,
                    SOL_IR_TYPE_NEVER)) return false;
            }
        }
        SolIrExpressionId prerequisite = expression->kind == SOL_IR_EXPR_MATCH
            ? expression->as.match_expr.scrutinee
            : expression->kind == SOL_IR_EXPR_PROPAGATE
            ? expression->as.propagate.operand : SOL_IR_NONE;
        return prerequisite == SOL_IR_NONE
            || !mir_type_is(ir, ir->expressions[prerequisite].type,
                SOL_IR_TYPE_NEVER);
    }
    switch (expression->kind) {
        case SOL_IR_EXPR_UNARY:
            return mir_source_reaches_match(ir, expression->as.unary.operand,
                sought, depth + 1);
        case SOL_IR_EXPR_PROPAGATE:
            return mir_source_reaches_match(ir,
                expression->as.propagate.operand, sought, depth + 1);
        case SOL_IR_EXPR_BOUND_OPERATION:
            return mir_source_reaches_match(ir,
                expression->as.operation.receiver, sought, depth + 1);
        case SOL_IR_EXPR_HANDLE:
            return mir_source_reaches_match(ir, expression->as.handler.authority,
                    sought, depth + 1)
                || (!mir_type_is(ir, ir->expressions[
                        expression->as.handler.authority].type,
                        SOL_IR_TYPE_NEVER)
                    && (mir_source_reaches_match(ir,
                            expression->as.handler.provider, sought, depth + 1)
                        || (!mir_type_is(ir, ir->expressions[
                                expression->as.handler.provider].type,
                                SOL_IR_TYPE_NEVER)
                            && mir_source_reaches_match(ir,
                                expression->as.handler.body, sought,
                                depth + 1))));
        case SOL_IR_EXPR_BINARY:
            if (mir_source_reaches_match(ir, expression->as.binary.left,
                sought, depth + 1)) return true;
            return !mir_type_is(ir,
                    ir->expressions[expression->as.binary.left].type,
                    SOL_IR_TYPE_NEVER)
                && mir_source_reaches_match(ir, expression->as.binary.right,
                    sought, depth + 1);
        case SOL_IR_EXPR_CALL:
            if (expression->as.call.kind == SOL_IR_CALL_METHOD
                && expression->as.call.receiver < ir->expression_count
                && mir_source_reaches_match(ir, expression->as.call.receiver,
                    sought, depth + 1)) return true;
            if (expression->as.call.kind == SOL_IR_CALL_METHOD
                && expression->as.call.receiver < ir->expression_count
                && mir_type_is(ir, ir->expressions[
                    expression->as.call.receiver].type, SOL_IR_TYPE_NEVER)) {
                return false;
            }
            if ((expression->as.call.kind == SOL_IR_CALL_CALLBACK
                    || expression->as.call.kind == SOL_IR_CALL_CAPABILITY)
                && expression->as.call.callee < ir->expression_count
                && mir_source_reaches_match(ir, expression->as.call.callee,
                    sought, depth + 1)) return true;
            if ((expression->as.call.kind == SOL_IR_CALL_CALLBACK
                    || expression->as.call.kind == SOL_IR_CALL_CAPABILITY)
                && expression->as.call.callee < ir->expression_count
                && mir_type_is(ir, ir->expressions[
                    expression->as.call.callee].type, SOL_IR_TYPE_NEVER)) {
                return false;
            }
            for (size_t index = 0; index < expression->as.call.operands.count;
                ++index) {
                SolIrExpressionId operand = ir->operands[
                    expression->as.call.operands.offset + index].value;
                if (mir_source_reaches_match(ir, operand, sought, depth + 1)) {
                    return true;
                }
                if (mir_type_is(ir, ir->expressions[operand].type,
                    SOL_IR_TYPE_NEVER)) return false;
            }
            return false;
        case SOL_IR_EXPR_RECORD:
        case SOL_IR_EXPR_TUPLE: {
            SolIrSlice operands = expression->kind == SOL_IR_EXPR_RECORD
                ? expression->as.record.fields : expression->as.tuple.operands;
            for (size_t index = 0; index < operands.count; ++index) {
                SolIrExpressionId operand
                    = ir->operands[operands.offset + index].value;
                if (mir_source_reaches_match(ir, operand, sought, depth + 1)) {
                    return true;
                }
                if (mir_type_is(ir, ir->expressions[operand].type,
                    SOL_IR_TYPE_NEVER)) return false;
            }
            return false;
        }
        case SOL_IR_EXPR_IF:
            if (mir_source_reaches_match(ir,
                expression->as.if_expr.condition, sought, depth + 1)) {
                return true;
            }
            return !mir_type_is(ir, ir->expressions[
                    expression->as.if_expr.condition].type, SOL_IR_TYPE_NEVER)
                && (mir_source_reaches_match(ir,
                        expression->as.if_expr.then_branch, sought, depth + 1)
                    || mir_source_reaches_match(ir,
                        expression->as.if_expr.else_branch, sought, depth + 1));
        case SOL_IR_EXPR_MATCH:
            if (mir_source_reaches_match(ir,
                expression->as.match_expr.scrutinee, sought, depth + 1)) {
                return true;
            }
            if (mir_type_is(ir, ir->expressions[
                expression->as.match_expr.scrutinee].type,
                SOL_IR_TYPE_NEVER)) return false;
            for (size_t index = 0; index < expression->as.match_expr.arms.count;
                ++index) {
                const SolIrArm *arm = &ir->arms[ir->arm_ids[
                    expression->as.match_expr.arms.offset + index]];
                if ((arm->guard != SOL_IR_NONE
                        && mir_source_reaches_match(ir, arm->guard, sought,
                            depth + 1))
                    || mir_source_reaches_match(ir, arm->body, sought,
                        depth + 1)) return true;
            }
            return false;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0; index < expression->as.block.statements.count;
                ++index) {
                const SolIrStatement *statement = &ir->statements[
                    ir->statement_ids[expression->as.block.statements.offset
                        + index]];
                if (statement->target != SOL_IR_NONE
                    && mir_source_reaches_match(ir, statement->target, sought,
                        depth + 1)) return true;
                if (statement->condition != SOL_IR_NONE
                    && mir_source_reaches_match(ir, statement->condition,
                        sought, depth + 1)) return true;
                if (statement->condition != SOL_IR_NONE
                    && mir_type_is(ir,
                        ir->expressions[statement->condition].type,
                        SOL_IR_TYPE_NEVER)) return false;
                if (statement->expression != SOL_IR_NONE
                    && mir_source_reaches_match(ir, statement->expression,
                        sought, depth + 1)) return true;
                if (statement->kind == SOL_IR_STATEMENT_RETURN
                    || statement->kind == SOL_IR_STATEMENT_PANIC
                    || statement->kind == SOL_IR_STATEMENT_BREAK
                    || statement->kind == SOL_IR_STATEMENT_CONTINUE
                    || statement->kind == SOL_IR_STATEMENT_UNREACHABLE
                    || (statement->kind != SOL_IR_STATEMENT_REQUIRE
                        && statement->expression != SOL_IR_NONE
                        && mir_type_is(ir,
                            ir->expressions[statement->expression].type,
                            SOL_IR_TYPE_NEVER))) return false;
            }
            return false;
        default:
            return false;
    }
}

static bool mir_type_is(const SolIr *ir, SolIrTypeId id,
    SolIrTypeKind kind) {
    return id < ir->type_count && ir->types[id].kind == kind;
}

static bool mir_expression_contains_statement(const SolIr *ir,
    SolIrExpressionId expression_id, SolIrStatementId sought, size_t depth) {
    if (expression_id >= ir->expression_count || depth > ir->expression_count) {
        return false;
    }
    const SolIrExpression *expression = &ir->expressions[expression_id];
    switch (expression->kind) {
        case SOL_IR_EXPR_UNARY:
            return mir_expression_contains_statement(ir,
                expression->as.unary.operand, sought, depth + 1);
        case SOL_IR_EXPR_PROPAGATE:
            return mir_expression_contains_statement(ir,
                expression->as.propagate.operand, sought, depth + 1);
        case SOL_IR_EXPR_BOUND_OPERATION:
            return mir_expression_contains_statement(ir,
                expression->as.operation.receiver, sought, depth + 1);
        case SOL_IR_EXPR_HANDLE:
            return mir_expression_contains_statement(ir,
                    expression->as.handler.authority, sought, depth + 1)
                || mir_expression_contains_statement(ir,
                    expression->as.handler.provider, sought, depth + 1)
                || mir_expression_contains_statement(ir,
                    expression->as.handler.body, sought, depth + 1);
        case SOL_IR_EXPR_BINARY:
            return mir_expression_contains_statement(ir,
                    expression->as.binary.left, sought, depth + 1)
                || mir_expression_contains_statement(ir,
                    expression->as.binary.right, sought, depth + 1);
        case SOL_IR_EXPR_CALL:
            if (expression->as.call.kind == SOL_IR_CALL_METHOD
                && expression->as.call.receiver < ir->expression_count
                && mir_expression_contains_statement(ir,
                    expression->as.call.receiver, sought, depth + 1)) return true;
            if ((expression->as.call.kind == SOL_IR_CALL_CALLBACK
                    || expression->as.call.kind == SOL_IR_CALL_CAPABILITY)
                && expression->as.call.callee < ir->expression_count
                && mir_expression_contains_statement(ir,
                    expression->as.call.callee, sought, depth + 1)) return true;
            for (size_t index = 0;
                index < expression->as.call.operands.count; ++index) {
                if (mir_expression_contains_statement(ir, ir->operands[
                        expression->as.call.operands.offset + index].value,
                    sought, depth + 1)) return true;
            }
            return false;
        case SOL_IR_EXPR_RECORD:
        case SOL_IR_EXPR_TUPLE: {
            SolIrSlice operands = expression->kind == SOL_IR_EXPR_RECORD
                ? expression->as.record.fields : expression->as.tuple.operands;
            for (size_t index = 0; index < operands.count; ++index) {
                if (mir_expression_contains_statement(ir,
                    ir->operands[operands.offset + index].value,
                    sought, depth + 1)) return true;
            }
            return false;
        }
        case SOL_IR_EXPR_IF:
            return mir_expression_contains_statement(ir,
                    expression->as.if_expr.condition, sought, depth + 1)
                || mir_expression_contains_statement(ir,
                    expression->as.if_expr.then_branch, sought, depth + 1)
                || mir_expression_contains_statement(ir,
                    expression->as.if_expr.else_branch, sought, depth + 1);
        case SOL_IR_EXPR_MATCH:
            if (mir_expression_contains_statement(ir,
                expression->as.match_expr.scrutinee, sought, depth + 1)) {
                return true;
            }
            for (size_t index = 0; index < expression->as.match_expr.arms.count;
                ++index) {
                const SolIrArm *arm = &ir->arms[ir->arm_ids[
                    expression->as.match_expr.arms.offset + index]];
                if ((arm->guard != SOL_IR_NONE
                        && mir_expression_contains_statement(ir, arm->guard,
                            sought, depth + 1))
                    || mir_expression_contains_statement(ir, arm->body,
                        sought, depth + 1)) return true;
            }
            return false;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0;
                index < expression->as.block.statements.count; ++index) {
                SolIrStatementId statement_id = ir->statement_ids[
                    expression->as.block.statements.offset + index];
                if (statement_id == sought) return true;
                const SolIrStatement *statement = &ir->statements[statement_id];
                if (statement->target != SOL_IR_NONE
                    && mir_expression_contains_statement(ir, statement->target,
                        sought, depth + 1)) return true;
                if (statement->condition != SOL_IR_NONE
                    && mir_expression_contains_statement(ir,
                        statement->condition, sought, depth + 1)) return true;
                if (statement->expression != SOL_IR_NONE
                    && mir_expression_contains_statement(ir,
                        statement->expression, sought, depth + 1)) return true;
            }
            return false;
        default:
            return false;
    }
}

static bool mir_find_statement_loop_parent(const SolIr *ir,
    SolIrExpressionId expression_id, SolIrStatementId sought,
    SolIrStatementId active_loop, SolIrStatementId *parent, size_t depth) {
    if (expression_id >= ir->expression_count || depth > ir->expression_count) {
        return false;
    }
    const SolIrExpression *expression = &ir->expressions[expression_id];
    switch (expression->kind) {
        case SOL_IR_EXPR_UNARY:
            return mir_find_statement_loop_parent(ir,
                expression->as.unary.operand, sought, active_loop, parent,
                depth + 1);
        case SOL_IR_EXPR_PROPAGATE:
            return mir_find_statement_loop_parent(ir,
                expression->as.propagate.operand, sought, active_loop, parent,
                depth + 1);
        case SOL_IR_EXPR_BOUND_OPERATION:
            return mir_find_statement_loop_parent(ir,
                expression->as.operation.receiver, sought, active_loop, parent,
                depth + 1);
        case SOL_IR_EXPR_HANDLE:
            return mir_find_statement_loop_parent(ir,
                    expression->as.handler.authority, sought, active_loop,
                    parent, depth + 1)
                || mir_find_statement_loop_parent(ir,
                    expression->as.handler.provider, sought, active_loop,
                    parent, depth + 1)
                || mir_find_statement_loop_parent(ir,
                    expression->as.handler.body, sought, active_loop, parent,
                    depth + 1);
        case SOL_IR_EXPR_BINARY:
            return mir_find_statement_loop_parent(ir,
                    expression->as.binary.left, sought, active_loop, parent,
                    depth + 1)
                || mir_find_statement_loop_parent(ir,
                    expression->as.binary.right, sought, active_loop, parent,
                    depth + 1);
        case SOL_IR_EXPR_CALL:
            if (expression->as.call.kind == SOL_IR_CALL_METHOD
                && expression->as.call.receiver < ir->expression_count
                && mir_find_statement_loop_parent(ir,
                    expression->as.call.receiver, sought, active_loop, parent,
                    depth + 1)) return true;
            if ((expression->as.call.kind == SOL_IR_CALL_CALLBACK
                    || expression->as.call.kind == SOL_IR_CALL_CAPABILITY)
                && expression->as.call.callee < ir->expression_count
                && mir_find_statement_loop_parent(ir,
                    expression->as.call.callee, sought, active_loop, parent,
                    depth + 1)) return true;
            for (size_t index = 0;
                index < expression->as.call.operands.count; ++index) {
                if (mir_find_statement_loop_parent(ir, ir->operands[
                        expression->as.call.operands.offset + index].value,
                    sought, active_loop, parent, depth + 1)) return true;
            }
            return false;
        case SOL_IR_EXPR_RECORD:
        case SOL_IR_EXPR_TUPLE: {
            SolIrSlice operands = expression->kind == SOL_IR_EXPR_RECORD
                ? expression->as.record.fields : expression->as.tuple.operands;
            for (size_t index = 0; index < operands.count; ++index) {
                if (mir_find_statement_loop_parent(ir,
                    ir->operands[operands.offset + index].value,
                    sought, active_loop, parent, depth + 1)) return true;
            }
            return false;
        }
        case SOL_IR_EXPR_IF:
            return mir_find_statement_loop_parent(ir,
                    expression->as.if_expr.condition, sought, active_loop,
                    parent, depth + 1)
                || mir_find_statement_loop_parent(ir,
                    expression->as.if_expr.then_branch, sought, active_loop,
                    parent, depth + 1)
                || mir_find_statement_loop_parent(ir,
                    expression->as.if_expr.else_branch, sought, active_loop,
                    parent, depth + 1);
        case SOL_IR_EXPR_MATCH:
            if (mir_find_statement_loop_parent(ir,
                expression->as.match_expr.scrutinee, sought, active_loop,
                parent, depth + 1)) return true;
            for (size_t index = 0; index < expression->as.match_expr.arms.count;
                ++index) {
                const SolIrArm *arm = &ir->arms[ir->arm_ids[
                    expression->as.match_expr.arms.offset + index]];
                if ((arm->guard != SOL_IR_NONE
                        && mir_find_statement_loop_parent(ir, arm->guard,
                            sought, active_loop, parent, depth + 1))
                    || mir_find_statement_loop_parent(ir, arm->body, sought,
                        active_loop, parent, depth + 1)) return true;
            }
            return false;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0;
                index < expression->as.block.statements.count; ++index) {
                SolIrStatementId statement_id = ir->statement_ids[
                    expression->as.block.statements.offset + index];
                if (statement_id == sought) {
                    *parent = active_loop;
                    return true;
                }
                const SolIrStatement *statement = &ir->statements[statement_id];
                SolIrStatementId nested_loop
                    = statement->kind == SOL_IR_STATEMENT_LOOP
                        || statement->kind == SOL_IR_STATEMENT_WHILE
                    ? statement_id : active_loop;
                if (statement->condition != SOL_IR_NONE
                    && mir_find_statement_loop_parent(ir,
                        statement->condition, sought, nested_loop, parent,
                        depth + 1)) return true;
                if (statement->expression != SOL_IR_NONE
                    && mir_find_statement_loop_parent(ir,
                        statement->expression, sought, nested_loop, parent,
                        depth + 1)) return true;
            }
            return false;
        default:
            return false;
    }
}

static bool mir_source_loop_falls_through(const SolIr *ir,
    const SolMir *mir, SolIrStatementId statement) {
    if (ir->statements[statement].kind == SOL_IR_STATEMENT_WHILE
        && mir_type_is(ir,
            ir->expressions[ir->statements[statement].condition].type,
            SOL_IR_TYPE_BOOL)) return true;
    for (size_t loop = 0; loop < mir->loop_count; ++loop) {
        if (mir->loops[loop].statement == statement) {
            return mir->loops[loop].exit != SOL_MIR_NONE;
        }
    }
    return false;
}

static bool mir_collect_source_statements(const SolIr *ir, const SolMir *mir,
    SolIrExpressionId expression_id, SolIrStatementId *statements,
    size_t *count, size_t depth) {
    if (expression_id >= ir->expression_count || depth > ir->expression_count) {
        return false;
    }
    const SolIrExpression *expression = &ir->expressions[expression_id];
    switch (expression->kind) {
        case SOL_IR_EXPR_UNARY:
            return mir_collect_source_statements(ir, mir,
                expression->as.unary.operand, statements, count, depth + 1);
        case SOL_IR_EXPR_PROPAGATE:
            return mir_collect_source_statements(ir, mir,
                expression->as.propagate.operand, statements, count,
                depth + 1);
        case SOL_IR_EXPR_BOUND_OPERATION:
            return mir_collect_source_statements(ir, mir,
                expression->as.operation.receiver, statements, count,
                depth + 1);
        case SOL_IR_EXPR_HANDLE:
            if (!mir_collect_source_statements(ir, mir,
                expression->as.handler.authority, statements, count,
                depth + 1)) return false;
            if (mir_type_is(ir, ir->expressions[
                expression->as.handler.authority].type,
                SOL_IR_TYPE_NEVER)) return true;
            if (!mir_collect_source_statements(ir, mir,
                expression->as.handler.provider, statements, count,
                depth + 1)) return false;
            return mir_type_is(ir, ir->expressions[
                    expression->as.handler.provider].type, SOL_IR_TYPE_NEVER)
                || mir_collect_source_statements(ir, mir,
                    expression->as.handler.body, statements, count,
                    depth + 1);
        case SOL_IR_EXPR_BINARY:
            if (!mir_collect_source_statements(ir, mir,
                expression->as.binary.left, statements, count, depth + 1)) {
                return false;
            }
            return mir_type_is(ir,
                    ir->expressions[expression->as.binary.left].type,
                    SOL_IR_TYPE_NEVER)
                || mir_collect_source_statements(ir, mir,
                    expression->as.binary.right, statements, count, depth + 1);
        case SOL_IR_EXPR_CALL:
            if (expression->as.call.kind == SOL_IR_CALL_METHOD
                && expression->as.call.receiver < ir->expression_count) {
                if (!mir_collect_source_statements(ir, mir,
                    expression->as.call.receiver, statements, count,
                    depth + 1)) return false;
                if (mir_type_is(ir, ir->expressions[
                    expression->as.call.receiver].type, SOL_IR_TYPE_NEVER)) {
                    return true;
                }
            }
            if ((expression->as.call.kind == SOL_IR_CALL_CALLBACK
                    || expression->as.call.kind == SOL_IR_CALL_CAPABILITY)
                && expression->as.call.callee < ir->expression_count) {
                if (!mir_collect_source_statements(ir, mir,
                    expression->as.call.callee, statements, count,
                    depth + 1)) return false;
                if (mir_type_is(ir, ir->expressions[
                    expression->as.call.callee].type, SOL_IR_TYPE_NEVER)) {
                    return true;
                }
            }
            for (size_t index = 0;
                index < expression->as.call.operands.count; ++index) {
                SolIrExpressionId operand = ir->operands[
                    expression->as.call.operands.offset + index].value;
                if (!mir_collect_source_statements(ir, mir, operand,
                    statements, count, depth + 1)) return false;
                if (mir_type_is(ir, ir->expressions[operand].type,
                    SOL_IR_TYPE_NEVER)) return true;
            }
            return true;
        case SOL_IR_EXPR_RECORD:
        case SOL_IR_EXPR_TUPLE: {
            SolIrSlice operands = expression->kind == SOL_IR_EXPR_RECORD
                ? expression->as.record.fields : expression->as.tuple.operands;
            for (size_t index = 0; index < operands.count; ++index) {
                SolIrExpressionId operand
                    = ir->operands[operands.offset + index].value;
                if (!mir_collect_source_statements(ir, mir, operand,
                    statements, count, depth + 1)) return false;
                if (mir_type_is(ir, ir->expressions[operand].type,
                    SOL_IR_TYPE_NEVER)) return true;
            }
            return true;
        }
        case SOL_IR_EXPR_IF:
            if (!mir_collect_source_statements(ir, mir,
                    expression->as.if_expr.condition, statements, count,
                    depth + 1)) return false;
            if (mir_type_is(ir,
                ir->expressions[expression->as.if_expr.condition].type,
                SOL_IR_TYPE_NEVER)) return true;
            return mir_collect_source_statements(ir, mir,
                    expression->as.if_expr.then_branch, statements, count,
                    depth + 1)
                && mir_collect_source_statements(ir, mir,
                    expression->as.if_expr.else_branch, statements, count,
                    depth + 1);
        case SOL_IR_EXPR_MATCH:
            if (!mir_collect_source_statements(ir, mir,
                expression->as.match_expr.scrutinee, statements, count,
                depth + 1)) return false;
            for (size_t index = 0; index < expression->as.match_expr.arms.count;
                ++index) {
                const SolIrArm *arm = &ir->arms[ir->arm_ids[
                    expression->as.match_expr.arms.offset + index]];
                if (arm->guard != SOL_IR_NONE
                    && !mir_collect_source_statements(ir, mir, arm->guard,
                        statements, count, depth + 1)) return false;
                if (!mir_collect_source_statements(ir, mir, arm->body,
                    statements, count, depth + 1)) return false;
            }
            return true;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0;
                index < expression->as.block.statements.count; ++index) {
                SolIrStatementId statement_id = ir->statement_ids[
                    expression->as.block.statements.offset + index];
                if (*count >= ir->statement_count) return false;
                statements[(*count)++] = statement_id;
                const SolIrStatement *statement = &ir->statements[statement_id];
                if (statement->target != SOL_IR_NONE
                    && !mir_collect_source_statements(ir, mir,
                        statement->target,
                        statements, count, depth + 1)) return false;
                if (statement->condition != SOL_IR_NONE
                    && !mir_collect_source_statements(ir, mir,
                        statement->condition,
                        statements, count, depth + 1)) return false;
                bool condition_terminates = statement->condition != SOL_IR_NONE
                    && mir_type_is(ir,
                        ir->expressions[statement->condition].type,
                        SOL_IR_TYPE_NEVER);
                if (!condition_terminates
                    && statement->expression != SOL_IR_NONE
                    && !mir_collect_source_statements(ir, mir,
                        statement->expression, statements, count,
                        depth + 1)) return false;
                bool terminal = statement->kind == SOL_IR_STATEMENT_RETURN
                    || statement->kind == SOL_IR_STATEMENT_PANIC
                    || statement->kind == SOL_IR_STATEMENT_UNREACHABLE
                    || statement->kind == SOL_IR_STATEMENT_BREAK
                    || statement->kind == SOL_IR_STATEMENT_CONTINUE;
                if (statement->kind == SOL_IR_STATEMENT_LOOP
                    || statement->kind == SOL_IR_STATEMENT_WHILE) {
                    terminal = !mir_source_loop_falls_through(ir, mir,
                        statement_id);
                } else if (statement->kind != SOL_IR_STATEMENT_REQUIRE
                    && statement->expression != SOL_IR_NONE
                    && mir_type_is(ir,
                        ir->expressions[statement->expression].type,
                        SOL_IR_TYPE_NEVER)) {
                    terminal = true;
                }
                if (terminal) break;
            }
            return true;
        default:
            return true;
    }
}

static SolIrStatementId mir_next_source_statement(const SolIr *ir,
    const SolIrStatementId *statements, size_t count, size_t *cursor,
    SolIrStatementKind kind) {
    while (*cursor < count) {
        SolIrStatementId statement = statements[(*cursor)++];
        if (ir->statements[statement].kind == kind) return statement;
    }
    return SOL_IR_NONE;
}

static SolIrStatementId mir_next_source_loop(const SolIr *ir,
    const SolIrStatementId *statements, size_t count, size_t *cursor) {
    while (*cursor < count) {
        SolIrStatementId statement = statements[(*cursor)++];
        if (ir->statements[statement].kind == SOL_IR_STATEMENT_LOOP
            || ir->statements[statement].kind == SOL_IR_STATEMENT_WHILE) {
            return statement;
        }
    }
    return SOL_IR_NONE;
}

static SolIrStatementId mir_next_source_transfer(const SolIr *ir,
    const SolIrStatementId *statements, size_t count, size_t *cursor) {
    while (*cursor < count) {
        SolIrStatementId statement = statements[(*cursor)++];
        if (ir->statements[statement].kind == SOL_IR_STATEMENT_BREAK
            || ir->statements[statement].kind == SOL_IR_STATEMENT_CONTINUE) {
            return statement;
        }
    }
    return SOL_IR_NONE;
}

static bool mir_value_available(const SolMir *mir, SolMirValueId id,
    SolMirBlockId block, size_t before_instruction) {
    if (id >= mir->value_count) return false;
    const SolMirValue *value = &mir->values[id];
    return value->block == block
        && (value->kind == SOL_MIR_VALUE_BLOCK_PARAMETER
            || (value->kind == SOL_MIR_VALUE_INSTRUCTION
                && value->definition < before_instruction));
}

static SolMirInstructionId mir_temporary_initializer(const SolMir *mir,
    SolMirTemporaryId temporary) {
    for (size_t instruction = 0; instruction < mir->instruction_count;
        ++instruction) {
        if (mir->instructions[instruction].kind == SOL_MIR_INST_TEMPORARY_INIT
            && mir->instructions[instruction].as.temporary_init.temporary
                == temporary) return instruction;
    }
    return SOL_MIR_NONE;
}

static SolIrExpressionId mir_instruction_event_source(
    const SolMirInstruction *instruction) {
    if (instruction->kind == SOL_MIR_INST_CAPTURE_SNAPSHOT) return SOL_IR_NONE;
    if (instruction->source_expression != SOL_IR_NONE) {
        return instruction->source_expression;
    }
    if (instruction->kind == SOL_MIR_INST_PATTERN_TEST
        || instruction->kind == SOL_MIR_INST_PATTERN_VALUE
        || instruction->kind == SOL_MIR_INST_MATCH_ARM) {
        return instruction->as.pattern.match_expression;
    }
    return SOL_IR_NONE;
}

static SolIrExpressionId mir_terminator_event_source(
    const SolMirTerminator *term) {
    switch (term->kind) {
        case SOL_MIR_TERM_INVOKE: return term->as.invoke.source_expression;
        case SOL_MIR_TERM_CHECK_REFINED:
            return term->as.check_refined.source_expression;
        case SOL_MIR_TERM_MATCH_FAILURE: return term->as.match_failure;
        case SOL_MIR_TERM_PROPAGATE:
            return term->as.propagate.source_expression;
        default: return SOL_IR_NONE;
    }
}

static bool mir_expression_events_after(const SolIr *ir, const SolMir *mir,
    SolIrExpressionId expression, SolMirInstructionId initializer) {
    if (initializer >= mir->instruction_count
        || mir->instructions[initializer].block >= mir->block_count) return false;
    size_t initial_order
        = mir->blocks[mir->instructions[initializer].block].order;
    for (size_t instruction = 0; instruction < mir->instruction_count;
        ++instruction) {
        SolIrExpressionId event
            = mir_instruction_event_source(&mir->instructions[instruction]);
        if (event == SOL_IR_NONE
            || !mir_source_reaches_match(ir, expression, event, 0)) continue;
        if (mir->instructions[instruction].block >= mir->block_count) return false;
        size_t order = mir->blocks[mir->instructions[instruction].block].order;
        if (order < initial_order
            || (order == initial_order && instruction <= initializer)) return false;
    }
    for (size_t block = 0; block < mir->block_count; ++block) {
        SolIrExpressionId event
            = mir_terminator_event_source(&mir->blocks[block].terminator);
        if (event != SOL_IR_NONE
            && mir_source_reaches_match(ir, expression, event, 0)
            && mir->blocks[block].order < initial_order) return false;
    }
    return true;
}

static bool mir_validate_edge(const SolMir *mir, SolMirBlockId source,
    const SolMirEdge *edge, SolMirValueId terminator_result) {
    if (edge->block >= mir->block_count
        || !mir_slice_valid(edge->arguments, mir->edge_value_count)
        || !mir_slice_valid(mir->blocks[edge->block].parameters,
            mir->parameter_value_count)
        || edge->arguments.count != mir->blocks[edge->block].parameters.count) {
        return false;
    }
    for (size_t index = 0; index < edge->arguments.count; ++index) {
        SolMirValueId argument = mir->edge_values[edge->arguments.offset + index];
        SolMirValueId parameter = mir->parameter_values[
            mir->blocks[edge->block].parameters.offset + index];
        if (argument >= mir->value_count || parameter >= mir->value_count
            || (!mir_value_available(mir, argument, source,
                    mir->blocks[source].instructions.offset
                        + mir->blocks[source].instructions.count)
                && !(argument == terminator_result
                    && mir->values[argument].kind
                        == SOL_MIR_VALUE_TERMINATOR
                    && mir->values[argument].block == source))
            || mir->values[argument].type != mir->values[parameter].type) return false;
    }
    return true;
}

static bool mir_validate_arena_ownership(const SolMir *mir,
    SolDiagnostics *diagnostics, SolSpan span) {
    unsigned char *values = mir->value_count == 0 ? NULL
        : calloc(mir->value_count, 1);
    unsigned char *edges = mir->edge_value_count == 0 ? NULL
        : calloc(mir->edge_value_count, 1);
    unsigned char *arguments = mir->call_argument_count == 0 ? NULL
        : calloc(mir->call_argument_count, 1);
    unsigned char *constructs = mir->construct_operand_count == 0 ? NULL
        : calloc(mir->construct_operand_count, 1);
    if ((mir->value_count != 0 && values == NULL)
        || (mir->edge_value_count != 0 && edges == NULL)
        || (mir->call_argument_count != 0 && arguments == NULL)
        || (mir->construct_operand_count != 0 && constructs == NULL)) {
        free(values);
        free(edges);
        free(arguments);
        free(constructs);
        return mir_error(diagnostics, span,
            "MIR ownership validation allocation failed");
    }
    bool valid = true;
    for (size_t block = 0; valid && block < mir->block_count; ++block) {
        SolMirSlice parameters = mir->blocks[block].parameters;
        for (size_t index = 0; valid && index < parameters.count; ++index) {
            SolMirValueId value = mir->parameter_values[parameters.offset + index];
            valid = value < mir->value_count && ++values[value] == 1;
        }
        const SolMirTerminator *terminator = &mir->blocks[block].terminator;
        SolMirSlice slices[3];
        size_t count = 0;
        if (terminator->kind == SOL_MIR_TERM_GOTO) {
            slices[count++] = terminator->as.go_to.arguments;
        } else if (terminator->kind == SOL_MIR_TERM_BRANCH) {
            slices[count++] = terminator->as.branch.true_edge.arguments;
            slices[count++] = terminator->as.branch.false_edge.arguments;
        } else if (terminator->kind == SOL_MIR_TERM_INVOKE) {
            slices[count++] = terminator->as.invoke.normal_edge.arguments;
            slices[count++] = terminator->as.invoke.failure_edge.arguments;
            SolMirSlice call = terminator->as.invoke.arguments;
            for (size_t index = 0; valid && index < call.count; ++index) {
                size_t slot = call.offset + index;
                valid = slot < mir->call_argument_count
                    && ++arguments[slot] == 1;
            }
            if (terminator->as.invoke.result != SOL_MIR_NONE) {
                SolMirValueId result = terminator->as.invoke.result;
                valid = result < mir->value_count && ++values[result] == 1;
            }
        } else if (terminator->kind == SOL_MIR_TERM_CHECK_REFINED) {
            slices[count++]
                = terminator->as.check_refined.normal_edge.arguments;
            slices[count++]
                = terminator->as.check_refined.failure_edge.arguments;
            SolMirValueId result = terminator->as.check_refined.result;
            valid = result < mir->value_count && ++values[result] == 1;
        } else if (terminator->kind == SOL_MIR_TERM_PROPAGATE) {
            slices[count++] = terminator->as.propagate.value_edge.arguments;
            slices[count++] = terminator->as.propagate.residual_edge.arguments;
            SolMirValueId value = terminator->as.propagate.value_result;
            SolMirValueId residual = terminator->as.propagate.residual_result;
            valid = value < mir->value_count && ++values[value] == 1
                && residual < mir->value_count && ++values[residual] == 1;
        } else if (terminator->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
            slices[count++]
                = terminator->as.check_contract.satisfied_edge.arguments;
            slices[count++]
                = terminator->as.check_contract.violation_edge.arguments;
            slices[count++]
                = terminator->as.check_contract.failure_edge.arguments;
        } else if (terminator->kind == SOL_MIR_TERM_BREAK
            || terminator->kind == SOL_MIR_TERM_CONTINUE) {
            slices[count++] = terminator->as.transfer.edge.arguments;
        }
        for (size_t slice = 0; valid && slice < count; ++slice) {
            for (size_t index = 0; valid && index < slices[slice].count; ++index) {
                size_t slot = slices[slice].offset + index;
                valid = slot < mir->edge_value_count && ++edges[slot] == 1;
            }
        }
    }
    for (size_t id = 0; valid && id < mir->instruction_count; ++id) {
        if (mir->instructions[id].kind == SOL_MIR_INST_CONSTRUCT) {
            SolMirSlice operands = mir->instructions[id].as.construct.operands;
            for (size_t index = 0; valid && index < operands.count; ++index) {
                size_t slot = operands.offset + index;
                valid = slot < mir->construct_operand_count
                    && ++constructs[slot] == 1;
            }
        }
        SolMirValueId result = mir->instructions[id].result;
        if (result != SOL_MIR_NONE) {
            valid = result < mir->value_count && ++values[result] == 1;
        }
    }
    for (size_t id = 0; valid && id < mir->value_count; ++id) {
        valid = values[id] == 1;
    }
    for (size_t id = 0; valid && id < mir->edge_value_count; ++id) {
        valid = edges[id] == 1;
    }
    for (size_t id = 0; valid && id < mir->call_argument_count; ++id) {
        valid = arguments[id] == 1;
    }
    for (size_t id = 0; valid && id < mir->construct_operand_count; ++id) {
        valid = constructs[id] == 1;
    }
    free(values);
    free(edges);
    free(arguments);
    free(constructs);
    return valid || mir_error(diagnostics, span,
        "MIR value or edge arena is shared or orphaned");
}

typedef enum {
    MIR_STORAGE_DEAD,
    MIR_STORAGE_UNINITIALIZED,
    MIR_STORAGE_INITIALIZED,
    MIR_STORAGE_MAYBE_INITIALIZED,
} MirStorageState;

static bool mir_merge_storage(MirStorageState *target,
    const MirStorageState *source, size_t count, bool *changed) {
    for (size_t index = 0; index < count; ++index) {
        MirStorageState merged = target[index];
        if (target[index] != source[index]) {
            if ((target[index] == MIR_STORAGE_INITIALIZED
                    || target[index] == MIR_STORAGE_UNINITIALIZED
                    || target[index] == MIR_STORAGE_MAYBE_INITIALIZED)
                && (source[index] == MIR_STORAGE_INITIALIZED
                    || source[index] == MIR_STORAGE_UNINITIALIZED
                    || source[index] == MIR_STORAGE_MAYBE_INITIALIZED)) {
                merged = MIR_STORAGE_MAYBE_INITIALIZED;
            } else {
                return false;
            }
        }
        if (merged != target[index]) {
            target[index] = merged;
            *changed = true;
        }
    }
    return true;
}

static bool mir_validate_storage(const SolIr *ir, const SolMir *mir,
    SolDiagnostics *diagnostics) {
    size_t state_count = mir->block_count * ir->local_count;
    if (ir->local_count != 0
        && mir->block_count > SIZE_MAX / ir->local_count) {
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR storage validation domain is too large");
    }
    MirStorageState *incoming = state_count == 0 ? NULL
        : calloc(state_count, sizeof(*incoming));
    bool *known = mir->block_count == 0 ? NULL
        : calloc(mir->block_count, sizeof(*known));
    MirStorageState *state = ir->local_count == 0 ? NULL
        : malloc(ir->local_count * sizeof(*state));
    if ((state_count != 0 && incoming == NULL)
        || (mir->block_count != 0 && known == NULL)
        || (ir->local_count != 0 && state == NULL)) {
        free(incoming);
        free(known);
        free(state);
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR storage validation allocation failed");
    }
    known[mir->entry] = true;
    bool valid = true;
    bool changed = true;
    size_t passes = 0;
    size_t pass_limit = mir->block_count > SIZE_MAX / 5
        ? SIZE_MAX : mir->block_count * 5 + 1;
    while (valid && changed && passes++ < pass_limit) {
        changed = false;
        for (size_t block = 0; valid && block < mir->block_count; ++block) {
            if (!known[block]) continue;
            if (ir->local_count != 0) memcpy(state,
                &incoming[block * ir->local_count],
                ir->local_count * sizeof(*state));
            SolMirSlice instructions = mir->blocks[block].instructions;
            for (size_t index = 0; valid && index < instructions.count; ++index) {
                const SolMirInstruction *instruction
                    = &mir->instructions[instructions.offset + index];
                SolIrLocalId local = SOL_IR_NONE;
                if (instruction->kind == SOL_MIR_INST_STORE) {
                    local = instruction->as.store.place.local;
                } else if (instruction->kind
                    == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED) {
                    local = instruction->as.place.local;
                } else if (instruction->kind == SOL_MIR_INST_LOAD_COPY
                    || instruction->kind == SOL_MIR_INST_LOAD_MOVE) {
                    local = instruction->as.place.local;
                } else if (instruction->kind == SOL_MIR_INST_LOAD_UPDATE) {
                    local = instruction->as.update_load.place.local;
                } else if (instruction->kind >= SOL_MIR_INST_PARAMETER_LIVE
                    && instruction->kind <= SOL_MIR_INST_STORAGE_DEAD) {
                    local = instruction->as.local;
                }
                if (local == SOL_IR_NONE) continue;
                if (local >= ir->local_count) {
                    valid = false;
                    break;
                }
                switch (instruction->kind) {
                    case SOL_MIR_INST_PARAMETER_LIVE:
                        valid = ir->locals[local].kind == SOL_IR_LOCAL_PARAMETER
                            && state[local] == MIR_STORAGE_DEAD;
                        state[local] = MIR_STORAGE_INITIALIZED;
                        break;
                    case SOL_MIR_INST_STORAGE_LIVE:
                        valid = ir->locals[local].kind != SOL_IR_LOCAL_PARAMETER
                            && state[local] == MIR_STORAGE_DEAD;
                        state[local] = MIR_STORAGE_UNINITIALIZED;
                        break;
                    case SOL_MIR_INST_DROP_IF_INITIALIZED:
                        valid = state[local] != MIR_STORAGE_DEAD;
                        state[local] = MIR_STORAGE_UNINITIALIZED;
                        break;
                    case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED: {
                        SolIrPlaceId place = instruction->as.place.source_place;
                        valid = state[local] == MIR_STORAGE_INITIALIZED
                            && place < ir->place_count;
                        if (valid && ir->places[place].projections.count == 0) {
                            state[local] = MIR_STORAGE_UNINITIALIZED;
                        }
                        break;
                    }
                    case SOL_MIR_INST_STORAGE_DEAD:
                        valid = state[local] == MIR_STORAGE_UNINITIALIZED;
                        state[local] = MIR_STORAGE_DEAD;
                        break;
                    case SOL_MIR_INST_LOAD_COPY:
                    case SOL_MIR_INST_LOAD_UPDATE:
                        valid = state[local] == MIR_STORAGE_INITIALIZED;
                        break;
                    case SOL_MIR_INST_LOAD_MOVE:
                        valid = state[local] == MIR_STORAGE_INITIALIZED;
                        if (valid && ir->places[instruction->as.place.source_place]
                            .projections.count == 0) {
                            state[local] = MIR_STORAGE_UNINITIALIZED;
                        }
                        break;
                    case SOL_MIR_INST_STORE:
                        if (instruction->as.store.place.source_place != SOL_IR_NONE
                            && ir->places[instruction->as.store.place.source_place]
                                .projections.count != 0) {
                            valid = state[local] == MIR_STORAGE_INITIALIZED;
                        } else {
                            valid = state[local] == MIR_STORAGE_UNINITIALIZED;
                            state[local] = MIR_STORAGE_INITIALIZED;
                        }
                        break;
                    default: break;
                }
            }
            const SolMirTerminator *term = &mir->blocks[block].terminator;
            if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
                if ((term->as.invoke.kind == SOL_IR_CALL_METHOD
                        || term->as.invoke.kind == SOL_IR_CALL_CAPABILITY)
                    && term->as.invoke.receiver.access != SOL_ACCESS_OWNED) {
                    SolIrPlaceId receiver = term->as.invoke.receiver.place;
                    valid = receiver < ir->place_count
                        && state[ir->places[receiver].local]
                            == MIR_STORAGE_INITIALIZED;
                }
                SolMirSlice arguments = term->as.invoke.arguments;
                for (size_t index = 0; valid && index < arguments.count; ++index) {
                    const SolMirCallArgument *argument
                        = &mir->call_arguments[arguments.offset + index];
                    if (argument->access == SOL_ACCESS_OWNED) continue;
                    const SolIrPlace *place = &ir->places[argument->place];
                    valid = state[place->local] == MIR_STORAGE_INITIALIZED;
                }
            }
            if (valid && (term->kind == SOL_MIR_TERM_RETURN
                || term->kind == SOL_MIR_TERM_PANIC
                || term->kind == SOL_MIR_TERM_RESUME_FAILURE
                || term->kind == SOL_MIR_TERM_MATCH_FAILURE
                || term->kind == SOL_MIR_TERM_UNREACHABLE
                || term->kind == SOL_MIR_TERM_CONTRACT_VIOLATION)) {
                SolIrDefinitionId owner = ir->callables[mir->callable].owner;
                for (size_t local = 0; valid && local < ir->local_count; ++local) {
                    if (ir->locals[local].owner == owner) {
                        valid = state[local] == MIR_STORAGE_DEAD;
                    }
                }
            }
            SolMirBlockId targets[3];
            size_t target_count = 0;
            if (valid && term->kind == SOL_MIR_TERM_GOTO) {
                targets[target_count++] = term->as.go_to.block;
            } else if (valid && term->kind == SOL_MIR_TERM_BRANCH) {
                targets[target_count++] = term->as.branch.true_edge.block;
                targets[target_count++] = term->as.branch.false_edge.block;
            } else if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
                if (term->as.invoke.normal_edge.block != SOL_MIR_NONE) {
                    targets[target_count++] = term->as.invoke.normal_edge.block;
                }
                targets[target_count++] = term->as.invoke.failure_edge.block;
            } else if (valid && term->kind == SOL_MIR_TERM_CHECK_REFINED) {
                targets[target_count++]
                    = term->as.check_refined.normal_edge.block;
                targets[target_count++]
                    = term->as.check_refined.failure_edge.block;
            } else if (valid && term->kind == SOL_MIR_TERM_PROPAGATE) {
                targets[target_count++] = term->as.propagate.value_edge.block;
                targets[target_count++] = term->as.propagate.residual_edge.block;
            } else if (valid && term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
                targets[target_count++]
                    = term->as.check_contract.satisfied_edge.block;
                targets[target_count++]
                    = term->as.check_contract.violation_edge.block;
                targets[target_count++]
                    = term->as.check_contract.failure_edge.block;
            } else if (valid && (term->kind == SOL_MIR_TERM_BREAK
                || term->kind == SOL_MIR_TERM_CONTINUE)) {
                targets[target_count++] = term->as.transfer.edge.block;
            }
            for (size_t index = 0; valid && index < target_count; ++index) {
                SolMirBlockId target = targets[index];
                if (!known[target]) {
                    known[target] = true;
                    if (ir->local_count != 0) memcpy(
                        &incoming[target * ir->local_count], state,
                        ir->local_count * sizeof(*state));
                    changed = true;
                } else if (ir->local_count != 0) {
                    valid = mir_merge_storage(
                        &incoming[target * ir->local_count], state,
                        ir->local_count, &changed);
                }
            }
        }
    }
    valid = valid && !changed;
    free(incoming);
    free(known);
    free(state);
    return valid || mir_error(diagnostics,
        ir->callables[mir->callable].span,
        "MIR local storage transitions are inconsistent");
}

static void mir_clear_local_paths(const SolIr *ir, unsigned char *state,
    SolIrLocalId local) {
    for (size_t place = 0; place < ir->place_count; ++place) {
        if (ir->places[place].root_kind == SOL_IR_PLACE_ROOT_LOCAL
            && ir->places[place].local == local) state[place] = 0;
    }
}

static bool mir_path_available(const SolIr *ir, const unsigned char *state,
    SolIrPlaceId place) {
    for (size_t hole = 0; hole < ir->place_count; ++hole) {
        if (state[hole] && mir_places_overlap(ir, hole, place)) return false;
    }
    return true;
}

static bool mir_validate_paths(const SolIr *ir, const SolMir *mir,
    SolDiagnostics *diagnostics) {
    if (ir->place_count != 0
        && mir->block_count > SIZE_MAX / ir->place_count) {
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR path-state validation domain is too large");
    }
    size_t state_count = mir->block_count * ir->place_count;
    unsigned char *incoming = state_count == 0 ? NULL
        : calloc(state_count, 1);
    unsigned char *state = ir->place_count == 0 ? NULL
        : malloc(ir->place_count);
    bool *known = calloc(mir->block_count, sizeof(*known));
    if ((state_count != 0 && incoming == NULL)
        || (ir->place_count != 0 && state == NULL) || known == NULL) {
        free(incoming);
        free(state);
        free(known);
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR path-state validation allocation failed");
    }
    known[mir->entry] = true;
    bool valid = true;
    bool changed = true;
    size_t passes = 0;
    size_t facts = ir->place_count > SIZE_MAX - ir->local_count - 1
        ? SIZE_MAX : ir->place_count + ir->local_count + 1;
    size_t limit = facts != 0 && mir->block_count > SIZE_MAX / facts
        ? SIZE_MAX : mir->block_count * facts + 1;
    while (valid && changed && passes++ < limit) {
        changed = false;
        for (size_t block = 0; valid && block < mir->block_count; ++block) {
            if (!known[block]) continue;
            if (ir->place_count != 0) memcpy(state,
                &incoming[block * ir->place_count], ir->place_count);
            SolMirSlice instructions = mir->blocks[block].instructions;
            for (size_t index = 0; valid && index < instructions.count; ++index) {
                const SolMirInstruction *instruction
                    = &mir->instructions[instructions.offset + index];
                if (instruction->kind == SOL_MIR_INST_PARAMETER_LIVE
                    || instruction->kind == SOL_MIR_INST_STORAGE_LIVE
                    || instruction->kind == SOL_MIR_INST_DROP_IF_INITIALIZED
                    || instruction->kind == SOL_MIR_INST_STORAGE_DEAD) {
                    mir_clear_local_paths(ir, state, instruction->as.local);
                } else if (instruction->kind == SOL_MIR_INST_LOAD_COPY
                    || instruction->kind == SOL_MIR_INST_LOAD_MOVE) {
                    SolIrPlaceId place = instruction->as.place.source_place;
                    valid = mir_path_available(ir, state, place);
                    if (valid && instruction->kind == SOL_MIR_INST_LOAD_MOVE
                        && ir->places[place].projections.count != 0) {
                        for (size_t hole = 0; hole < ir->place_count; ++hole) {
                            if (state[hole]
                                && mir_place_prefix(ir, place, hole)) {
                                state[hole] = 0;
                            }
                        }
                        state[place] = 1;
                    }
                } else if (instruction->kind == SOL_MIR_INST_LOAD_UPDATE) {
                    valid = mir_path_available(ir, state,
                        instruction->as.update_load.place.source_place);
                } else if (instruction->kind
                    == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED) {
                    SolIrPlaceId place = instruction->as.place.source_place;
                    for (size_t hole = 0; valid && hole < ir->place_count;
                        ++hole) {
                        if (!state[hole]) continue;
                        if (mir_place_prefix(ir, hole, place)
                            && !mir_place_prefix(ir, place, hole)) {
                            valid = false;
                        } else if (mir_place_prefix(ir, place, hole)) {
                            state[hole] = 0;
                        }
                    }
                    if (valid) state[place] = 1;
                } else if (instruction->kind == SOL_MIR_INST_STORE) {
                    SolIrPlaceId place
                        = instruction->as.store.place.source_place;
                    if (place == SOL_IR_NONE
                        || ir->places[place].projections.count == 0) {
                        mir_clear_local_paths(ir, state,
                            instruction->as.store.place.local);
                    } else {
                        bool found = false;
                        for (size_t hole = 0; hole < ir->place_count; ++hole) {
                            if (state[hole] && mir_place_prefix(ir, hole, place)
                                && mir_place_prefix(ir, place, hole)) {
                                state[hole] = 0;
                                found = true;
                            }
                        }
                        valid = found;
                    }
                }
            }
            const SolMirTerminator *term = &mir->blocks[block].terminator;
            if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
                if ((term->as.invoke.kind == SOL_IR_CALL_METHOD
                        || term->as.invoke.kind == SOL_IR_CALL_CAPABILITY)
                    && term->as.invoke.receiver.access != SOL_ACCESS_OWNED) {
                    valid = term->as.invoke.receiver.place < ir->place_count
                        && mir_path_available(ir, state,
                            term->as.invoke.receiver.place);
                }
                SolMirSlice arguments = term->as.invoke.arguments;
                for (size_t index = 0; valid && index < arguments.count;
                    ++index) {
                    const SolMirCallArgument *argument
                        = &mir->call_arguments[arguments.offset + index];
                    if (argument->access != SOL_ACCESS_OWNED) {
                        valid = mir_path_available(ir, state, argument->place);
                    }
                }
            }
            SolMirBlockId targets[3];
            size_t target_count = 0;
            if (valid && term->kind == SOL_MIR_TERM_GOTO) {
                targets[target_count++] = term->as.go_to.block;
            } else if (valid && term->kind == SOL_MIR_TERM_BRANCH) {
                targets[target_count++] = term->as.branch.true_edge.block;
                targets[target_count++] = term->as.branch.false_edge.block;
            } else if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
                if (term->as.invoke.normal_edge.block != SOL_MIR_NONE) {
                    targets[target_count++] = term->as.invoke.normal_edge.block;
                }
                targets[target_count++] = term->as.invoke.failure_edge.block;
            } else if (valid && term->kind == SOL_MIR_TERM_CHECK_REFINED) {
                targets[target_count++] = term->as.check_refined.normal_edge.block;
                targets[target_count++] = term->as.check_refined.failure_edge.block;
            } else if (valid && term->kind == SOL_MIR_TERM_PROPAGATE) {
                targets[target_count++] = term->as.propagate.value_edge.block;
                targets[target_count++] = term->as.propagate.residual_edge.block;
            } else if (valid && term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
                targets[target_count++]
                    = term->as.check_contract.satisfied_edge.block;
                targets[target_count++]
                    = term->as.check_contract.violation_edge.block;
                targets[target_count++]
                    = term->as.check_contract.failure_edge.block;
            } else if (valid && (term->kind == SOL_MIR_TERM_BREAK
                || term->kind == SOL_MIR_TERM_CONTINUE)) {
                targets[target_count++] = term->as.transfer.edge.block;
            }
            for (size_t index = 0; valid && index < target_count; ++index) {
                SolMirBlockId target = targets[index];
                unsigned char *destination = ir->place_count == 0 ? NULL
                    : &incoming[target * ir->place_count];
                if (!known[target]) {
                    known[target] = true;
                    if (ir->place_count != 0) memcpy(destination, state,
                        ir->place_count);
                    changed = true;
                } else {
                    for (size_t place = 0; place < ir->place_count; ++place) {
                        if (state[place] && !destination[place]) {
                            destination[place] = 1;
                            changed = true;
                        }
                    }
                }
            }
        }
    }
    valid = valid && !changed;
    free(incoming);
    free(state);
    free(known);
    return valid || mir_error(diagnostics,
        ir->callables[mir->callable].span,
        "MIR partial-move path transitions are inconsistent");
}

static bool mir_validate_regions(const SolIr *ir, const SolMir *mir,
    SolDiagnostics *diagnostics) {
    if (ir->statement_count != 0
        && mir->block_count > SIZE_MAX / ir->statement_count) {
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR region validation domain is too large");
    }
    size_t stack_count = mir->block_count * ir->statement_count;
    SolIrStatementId *incoming = stack_count == 0 ? NULL
        : malloc(stack_count * sizeof(*incoming));
    SolIrStatementId *working = ir->statement_count == 0 ? NULL
        : malloc(ir->statement_count * sizeof(*working));
    size_t *depths = calloc(mir->block_count, sizeof(*depths));
    bool *known = calloc(mir->block_count, sizeof(*known));
    SolMirBlockId *queue = malloc(mir->block_count * sizeof(*queue));
    if ((stack_count != 0 && incoming == NULL)
        || (ir->statement_count != 0 && working == NULL)
        || depths == NULL || known == NULL || queue == NULL) {
        free(incoming);
        free(working);
        free(depths);
        free(known);
        free(queue);
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR region validation allocation failed");
    }
    size_t first = 0;
    size_t count = 1;
    queue[0] = mir->entry;
    known[mir->entry] = true;
    bool valid = true;
    while (valid && first < count) {
        SolMirBlockId block = queue[first++];
        size_t depth = depths[block];
        if (depth != 0) memcpy(working,
            &incoming[block * ir->statement_count],
            depth * sizeof(*working));
        SolMirSlice instructions = mir->blocks[block].instructions;
        for (size_t index = 0; valid && index < instructions.count; ++index) {
            const SolMirInstruction *instruction
                = &mir->instructions[instructions.offset + index];
            if (instruction->kind == SOL_MIR_INST_REGION_ENTER) {
                if (depth == ir->statement_count) valid = false;
                else working[depth++] = instruction->as.region;
            } else if (instruction->kind == SOL_MIR_INST_REGION_EXIT) {
                if (depth == 0
                    || working[depth - 1] != instruction->as.region) {
                    valid = false;
                } else {
                    --depth;
                }
            }
        }
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        if (valid && depth != 0
            && (term->kind == SOL_MIR_TERM_RETURN
                || term->kind == SOL_MIR_TERM_PANIC
                || term->kind == SOL_MIR_TERM_RESUME_FAILURE
                || term->kind == SOL_MIR_TERM_MATCH_FAILURE
                || term->kind == SOL_MIR_TERM_UNREACHABLE
                || term->kind == SOL_MIR_TERM_CONTRACT_VIOLATION)) valid = false;
        SolMirBlockId targets[3];
        size_t target_count = 0;
        if (valid && term->kind == SOL_MIR_TERM_GOTO) {
            targets[target_count++] = term->as.go_to.block;
        } else if (valid && term->kind == SOL_MIR_TERM_BRANCH) {
            targets[target_count++] = term->as.branch.true_edge.block;
            targets[target_count++] = term->as.branch.false_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
            if (term->as.invoke.normal_edge.block != SOL_MIR_NONE) {
                targets[target_count++] = term->as.invoke.normal_edge.block;
            }
            targets[target_count++] = term->as.invoke.failure_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_CHECK_REFINED) {
            targets[target_count++] = term->as.check_refined.normal_edge.block;
            targets[target_count++] = term->as.check_refined.failure_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_PROPAGATE) {
            targets[target_count++] = term->as.propagate.value_edge.block;
            targets[target_count++] = term->as.propagate.residual_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
            targets[target_count++]
                = term->as.check_contract.satisfied_edge.block;
            targets[target_count++]
                = term->as.check_contract.violation_edge.block;
            targets[target_count++]
                = term->as.check_contract.failure_edge.block;
        } else if (valid && (term->kind == SOL_MIR_TERM_BREAK
            || term->kind == SOL_MIR_TERM_CONTINUE)) {
            targets[target_count++] = term->as.transfer.edge.block;
        }
        for (size_t index = 0; valid && index < target_count; ++index) {
            SolMirBlockId target = targets[index];
            if (!known[target]) {
                known[target] = true;
                depths[target] = depth;
                if (depth != 0) memcpy(
                    &incoming[target * ir->statement_count], working,
                    depth * sizeof(*working));
                queue[count++] = target;
            } else {
                valid = depths[target] == depth
                    && (depth == 0 || memcmp(
                        &incoming[target * ir->statement_count], working,
                        depth * sizeof(*working)) == 0);
            }
        }
    }
    free(incoming);
    free(working);
    free(depths);
    free(known);
    free(queue);
    return valid || mir_error(diagnostics,
        ir->callables[mir->callable].span,
        "MIR lexical region stack is inconsistent");
}

static bool mir_collect_handler_parents(const SolIr *ir,
    SolIrExpressionId id, SolIrExpressionId active,
    SolIrExpressionId *parents, bool *seen, size_t depth) {
    if (id >= ir->expression_count || depth > ir->expression_count) return false;
    if (seen[id]) return parents[id] == active;
    seen[id] = true;
    parents[id] = active;
    const SolIrExpression *expression = &ir->expressions[id];
#define MIR_HANDLER_CHILD(child, handler) \
    do { if (!mir_collect_handler_parents(ir, (child), (handler), parents, \
        seen, depth + 1)) return false; } while (0)
    switch (expression->kind) {
        case SOL_IR_EXPR_UNARY:
            MIR_HANDLER_CHILD(expression->as.unary.operand, active); break;
        case SOL_IR_EXPR_PROPAGATE:
            MIR_HANDLER_CHILD(expression->as.propagate.operand, active); break;
        case SOL_IR_EXPR_BOUND_OPERATION:
            MIR_HANDLER_CHILD(expression->as.operation.receiver, active); break;
        case SOL_IR_EXPR_HANDLE:
            MIR_HANDLER_CHILD(expression->as.handler.authority, active);
            MIR_HANDLER_CHILD(expression->as.handler.provider, active);
            MIR_HANDLER_CHILD(expression->as.handler.body, id);
            break;
        case SOL_IR_EXPR_BINARY:
            MIR_HANDLER_CHILD(expression->as.binary.left, active);
            MIR_HANDLER_CHILD(expression->as.binary.right, active);
            break;
        case SOL_IR_EXPR_CALL:
            if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
                MIR_HANDLER_CHILD(expression->as.call.receiver, active);
            } else if (expression->as.call.kind == SOL_IR_CALL_CALLBACK
                || expression->as.call.kind == SOL_IR_CALL_CAPABILITY) {
                MIR_HANDLER_CHILD(expression->as.call.callee, active);
            }
            for (size_t index = 0; index < expression->as.call.operands.count;
                ++index) {
                MIR_HANDLER_CHILD(ir->operands[
                    expression->as.call.operands.offset + index].value, active);
            }
            break;
        case SOL_IR_EXPR_RECORD:
        case SOL_IR_EXPR_TUPLE: {
            SolIrSlice operands = expression->kind == SOL_IR_EXPR_RECORD
                ? expression->as.record.fields : expression->as.tuple.operands;
            for (size_t index = 0; index < operands.count; ++index) {
                MIR_HANDLER_CHILD(ir->operands[operands.offset + index].value,
                    active);
            }
            break;
        }
        case SOL_IR_EXPR_IF:
            MIR_HANDLER_CHILD(expression->as.if_expr.condition, active);
            MIR_HANDLER_CHILD(expression->as.if_expr.then_branch, active);
            MIR_HANDLER_CHILD(expression->as.if_expr.else_branch, active);
            break;
        case SOL_IR_EXPR_MATCH:
            MIR_HANDLER_CHILD(expression->as.match_expr.scrutinee, active);
            for (size_t index = 0; index < expression->as.match_expr.arms.count;
                ++index) {
                const SolIrArm *arm = &ir->arms[ir->arm_ids[
                    expression->as.match_expr.arms.offset + index]];
                if (arm->guard != SOL_IR_NONE) {
                    MIR_HANDLER_CHILD(arm->guard, active);
                }
                MIR_HANDLER_CHILD(arm->body, active);
            }
            break;
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0; index < expression->as.block.statements.count;
                ++index) {
                const SolIrStatement *statement = &ir->statements[
                    ir->statement_ids[expression->as.block.statements.offset
                        + index]];
                if (statement->target != SOL_IR_NONE) {
                    MIR_HANDLER_CHILD(statement->target, active);
                }
                if (statement->condition != SOL_IR_NONE) {
                    MIR_HANDLER_CHILD(statement->condition, active);
                }
                if (statement->expression != SOL_IR_NONE) {
                    MIR_HANDLER_CHILD(statement->expression, active);
                }
            }
            break;
        default:
            break;
    }
#undef MIR_HANDLER_CHILD
    return true;
}

static size_t mir_expected_handlers(SolIrExpressionId target,
    const SolIrExpressionId *parents, SolIrExpressionId *expected,
    size_t expression_count) {
    size_t count = 0;
    for (SolIrExpressionId handler = parents[target]; handler != SOL_IR_NONE;
        handler = parents[handler]) {
        if (count == expression_count) return SIZE_MAX;
        expected[count++] = handler;
    }
    for (size_t left = 0; left < count / 2; ++left) {
        SolIrExpressionId swap = expected[left];
        expected[left] = expected[count - left - 1];
        expected[count - left - 1] = swap;
    }
    return count;
}

static bool mir_validate_handlers(const SolIr *ir, const SolMir *mir,
    SolDiagnostics *diagnostics) {
    if (ir->expression_count > SIZE_MAX / sizeof(SolIrExpressionId)
        || (ir->expression_count != 0
            && mir->block_count > SIZE_MAX / ir->expression_count)
        || mir->block_count * ir->expression_count
            > SIZE_MAX / sizeof(SolIrExpressionId)) {
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR handler validation domain is too large");
    }
    size_t stack_count = mir->block_count * ir->expression_count;
    SolIrExpressionId *incoming = stack_count == 0 ? NULL
        : malloc(stack_count * sizeof(*incoming));
    SolIrExpressionId *working = ir->expression_count == 0 ? NULL
        : malloc(ir->expression_count * sizeof(*working));
    SolIrExpressionId *expected = ir->expression_count == 0 ? NULL
        : malloc(ir->expression_count * sizeof(*expected));
    SolIrExpressionId *parents = ir->expression_count == 0 ? NULL
        : malloc(ir->expression_count * sizeof(*parents));
    bool *seen_expressions = calloc(ir->expression_count,
        sizeof(*seen_expressions));
    size_t *depths = calloc(mir->block_count, sizeof(*depths));
    bool *known = calloc(mir->block_count, sizeof(*known));
    SolMirBlockId *queue = malloc(mir->block_count * sizeof(*queue));
    if ((stack_count != 0 && incoming == NULL)
        || (ir->expression_count != 0
            && (working == NULL || expected == NULL || parents == NULL
                || seen_expressions == NULL))
        || depths == NULL || known == NULL || queue == NULL) {
        free(incoming); free(working); free(expected); free(parents);
        free(seen_expressions); free(depths); free(known); free(queue);
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR handler validation allocation failed");
    }
    if (!mir_collect_handler_parents(ir, ir->callables[mir->callable].body,
        SOL_IR_NONE, parents, seen_expressions, 0)) {
        free(incoming); free(working); free(expected); free(parents);
        free(seen_expressions); free(depths); free(known); free(queue);
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR handler source ancestry is inconsistent");
    }
    size_t first = 0;
    size_t count = 1;
    queue[0] = mir->entry;
    known[mir->entry] = true;
    bool valid = true;
    while (valid && first < count) {
        SolMirBlockId block = queue[first++];
        size_t depth = depths[block];
        if (depth != 0) memcpy(working,
            &incoming[block * ir->expression_count],
            depth * sizeof(*working));
        SolMirSlice instructions = mir->blocks[block].instructions;
        for (size_t index = 0; valid && index < instructions.count; ++index) {
            const SolMirInstruction *instruction
                = &mir->instructions[instructions.offset + index];
            bool semantic_snapshot = instruction->kind
                == SOL_MIR_INST_CAPTURE_SNAPSHOT;
            if (!semantic_snapshot
                && instruction->source_expression < ir->expression_count
                && !seen_expressions[instruction->source_expression]) {
                valid = false;
                break;
            }
            size_t expected_depth = !semantic_snapshot
                    && instruction->source_expression
                    < ir->expression_count
                ? mir_expected_handlers(instruction->source_expression,
                    parents, expected, ir->expression_count) : 0;
            if (instruction->kind == SOL_MIR_INST_HANDLER_ENTER) {
                valid = depth == expected_depth
                    && (depth == 0 || memcmp(working, expected,
                        depth * sizeof(*working)) == 0);
                if (valid) {
                    if (depth == ir->expression_count) valid = false;
                    else working[depth++] = instruction->source_expression;
                }
            } else if (instruction->kind == SOL_MIR_INST_HANDLER_EXIT) {
                if (depth != expected_depth + 1
                    || working[depth - 1] != instruction->source_expression
                    || (expected_depth != 0 && memcmp(working, expected,
                        expected_depth * sizeof(*working)) != 0)) {
                    valid = false;
                } else {
                    --depth;
                }
            }
        }
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
            if (term->as.invoke.source_expression >= ir->expression_count
                || !seen_expressions[term->as.invoke.source_expression]) {
                valid = false;
            }
        }
        if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
            size_t expected_depth = mir_expected_handlers(
                term->as.invoke.source_expression, parents, expected,
                ir->expression_count);
            valid = depth == expected_depth
                && (depth == 0 || memcmp(working, expected,
                    depth * sizeof(*working)) == 0);
        }
        if (valid && depth != 0
            && (term->kind == SOL_MIR_TERM_RETURN
                || term->kind == SOL_MIR_TERM_PANIC
                || term->kind == SOL_MIR_TERM_RESUME_FAILURE
                || term->kind == SOL_MIR_TERM_MATCH_FAILURE
                || term->kind == SOL_MIR_TERM_UNREACHABLE
                || term->kind == SOL_MIR_TERM_CONTRACT_VIOLATION)) valid = false;
        SolMirBlockId targets[3];
        size_t target_count = 0;
        if (valid && term->kind == SOL_MIR_TERM_GOTO) {
            targets[target_count++] = term->as.go_to.block;
        } else if (valid && term->kind == SOL_MIR_TERM_BRANCH) {
            targets[target_count++] = term->as.branch.true_edge.block;
            targets[target_count++] = term->as.branch.false_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
            if (term->as.invoke.normal_edge.block != SOL_MIR_NONE) {
                targets[target_count++] = term->as.invoke.normal_edge.block;
            }
            targets[target_count++] = term->as.invoke.failure_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_CHECK_REFINED) {
            targets[target_count++] = term->as.check_refined.normal_edge.block;
            targets[target_count++] = term->as.check_refined.failure_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_PROPAGATE) {
            targets[target_count++] = term->as.propagate.value_edge.block;
            targets[target_count++] = term->as.propagate.residual_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
            targets[target_count++]
                = term->as.check_contract.satisfied_edge.block;
            targets[target_count++]
                = term->as.check_contract.violation_edge.block;
            targets[target_count++]
                = term->as.check_contract.failure_edge.block;
        } else if (valid && (term->kind == SOL_MIR_TERM_BREAK
            || term->kind == SOL_MIR_TERM_CONTINUE)) {
            targets[target_count++] = term->as.transfer.edge.block;
        }
        for (size_t index = 0; valid && index < target_count; ++index) {
            SolMirBlockId target = targets[index];
            if (!known[target]) {
                known[target] = true;
                depths[target] = depth;
                if (depth != 0) memcpy(
                    &incoming[target * ir->expression_count], working,
                    depth * sizeof(*working));
                queue[count++] = target;
            } else {
                valid = depths[target] == depth
                    && (depth == 0 || memcmp(
                        &incoming[target * ir->expression_count], working,
                        depth * sizeof(*working)) == 0);
            }
        }
    }
    free(incoming); free(working); free(expected); free(parents);
    free(seen_expressions); free(depths); free(known); free(queue);
    return valid || mir_error(diagnostics,
        ir->callables[mir->callable].span,
        "MIR handler stack is inconsistent");
}

static bool mir_validate_storage_order(const SolIr *ir, const SolMir *mir,
    SolDiagnostics *diagnostics) {
    if (ir->local_count != 0
        && mir->block_count > SIZE_MAX / ir->local_count) {
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR lifetime-order validation domain is too large");
    }
    size_t stack_count = mir->block_count * ir->local_count;
    SolIrLocalId *incoming = stack_count == 0 ? NULL
        : malloc(stack_count * sizeof(*incoming));
    SolIrLocalId *working = ir->local_count == 0 ? NULL
        : malloc(ir->local_count * sizeof(*working));
    size_t *depths = calloc(mir->block_count, sizeof(*depths));
    bool *known = calloc(mir->block_count, sizeof(*known));
    SolMirBlockId *queue = malloc(mir->block_count * sizeof(*queue));
    if ((stack_count != 0 && incoming == NULL)
        || (ir->local_count != 0 && working == NULL)
        || depths == NULL || known == NULL || queue == NULL) {
        free(incoming);
        free(working);
        free(depths);
        free(known);
        free(queue);
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR lifetime-order validation allocation failed");
    }
    size_t first = 0;
    size_t count = 1;
    queue[0] = mir->entry;
    known[mir->entry] = true;
    bool valid = true;
    while (valid && first < count) {
        SolMirBlockId block = queue[first++];
        size_t depth = depths[block];
        if (depth != 0) memcpy(working,
            &incoming[block * ir->local_count],
            depth * sizeof(*working));
        SolMirSlice instructions = mir->blocks[block].instructions;
        for (size_t index = 0; valid && index < instructions.count; ++index) {
            size_t instruction_id = instructions.offset + index;
            const SolMirInstruction *instruction
                = &mir->instructions[instruction_id];
            if (instruction->kind == SOL_MIR_INST_PARAMETER_LIVE
                || instruction->kind == SOL_MIR_INST_STORAGE_LIVE) {
                if (depth == ir->local_count) valid = false;
                else working[depth++] = instruction->as.local;
            } else if (instruction->kind
                == SOL_MIR_INST_DROP_IF_INITIALIZED) {
                if (index + 1 >= instructions.count) {
                    valid = false;
                } else {
                    const SolMirInstruction *next
                        = &mir->instructions[instruction_id + 1];
                    valid = (next->kind == SOL_MIR_INST_STORAGE_DEAD
                            && next->as.local == instruction->as.local)
                        || (next->kind == SOL_MIR_INST_STORE
                            && next->as.store.place.local
                                == instruction->as.local);
                }
            } else if (instruction->kind
                == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED) {
                valid = index + 1 < instructions.count;
                if (valid) {
                    const SolMirInstruction *next
                        = &mir->instructions[instruction_id + 1];
                    valid = next->kind == SOL_MIR_INST_STORE
                        && next->as.store.place.local
                            == instruction->as.place.local
                        && next->as.store.place.source_place
                            == instruction->as.place.source_place;
                }
            } else if (instruction->kind == SOL_MIR_INST_STORAGE_DEAD) {
                valid = depth != 0
                    && working[depth - 1] == instruction->as.local
                    && index != 0
                    && mir->instructions[instruction_id - 1].kind
                        == SOL_MIR_INST_DROP_IF_INITIALIZED
                    && mir->instructions[instruction_id - 1].as.local
                        == instruction->as.local;
                if (valid) --depth;
            }
        }
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        if (valid && depth != 0
            && (term->kind == SOL_MIR_TERM_RETURN
                || term->kind == SOL_MIR_TERM_PANIC
                || term->kind == SOL_MIR_TERM_RESUME_FAILURE
                || term->kind == SOL_MIR_TERM_MATCH_FAILURE
                || term->kind == SOL_MIR_TERM_UNREACHABLE
                || term->kind == SOL_MIR_TERM_CONTRACT_VIOLATION)) valid = false;
        SolMirBlockId targets[3];
        size_t target_count = 0;
        if (valid && term->kind == SOL_MIR_TERM_GOTO) {
            targets[target_count++] = term->as.go_to.block;
        } else if (valid && term->kind == SOL_MIR_TERM_BRANCH) {
            targets[target_count++] = term->as.branch.true_edge.block;
            targets[target_count++] = term->as.branch.false_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
            if (term->as.invoke.normal_edge.block != SOL_MIR_NONE) {
                targets[target_count++] = term->as.invoke.normal_edge.block;
            }
            targets[target_count++] = term->as.invoke.failure_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_CHECK_REFINED) {
            targets[target_count++] = term->as.check_refined.normal_edge.block;
            targets[target_count++] = term->as.check_refined.failure_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_PROPAGATE) {
            targets[target_count++] = term->as.propagate.value_edge.block;
            targets[target_count++] = term->as.propagate.residual_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
            targets[target_count++]
                = term->as.check_contract.satisfied_edge.block;
            targets[target_count++]
                = term->as.check_contract.violation_edge.block;
            targets[target_count++]
                = term->as.check_contract.failure_edge.block;
        } else if (valid && (term->kind == SOL_MIR_TERM_BREAK
            || term->kind == SOL_MIR_TERM_CONTINUE)) {
            targets[target_count++] = term->as.transfer.edge.block;
        }
        for (size_t index = 0; valid && index < target_count; ++index) {
            SolMirBlockId target = targets[index];
            if (!known[target]) {
                known[target] = true;
                depths[target] = depth;
                if (depth != 0) memcpy(
                    &incoming[target * ir->local_count], working,
                    depth * sizeof(*working));
                queue[count++] = target;
            } else {
                valid = depths[target] == depth
                    && (depth == 0 || memcmp(
                        &incoming[target * ir->local_count], working,
                        depth * sizeof(*working)) == 0);
            }
        }
    }
    free(incoming);
    free(working);
    free(depths);
    free(known);
    free(queue);
    return valid || mir_error(diagnostics,
        ir->callables[mir->callable].span,
        "MIR lexical storage cleanup order is inconsistent");
}

static bool mir_validate_temporaries(const SolIr *ir, const SolMir *mir,
    SolDiagnostics *diagnostics) {
    if (mir->temporary_count != 0
        && mir->block_count > SIZE_MAX / mir->temporary_count) {
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR temporary validation domain is too large");
    }
    size_t stack_count = mir->block_count * mir->temporary_count;
    SolMirTemporaryId *incoming = stack_count == 0 ? NULL
        : malloc(stack_count * sizeof(*incoming));
    SolMirTemporaryId *working = mir->temporary_count == 0 ? NULL
        : malloc(mir->temporary_count * sizeof(*working));
    size_t *depths = calloc(mir->block_count, sizeof(*depths));
    bool *known = calloc(mir->block_count, sizeof(*known));
    SolMirBlockId *queue = malloc(mir->block_count * sizeof(*queue));
    size_t *initializers = calloc(mir->temporary_count,
        sizeof(*initializers));
    unsigned char *staged_values = mir->value_count == 0 ? NULL
        : calloc(mir->value_count, 1);
    if ((stack_count != 0 && incoming == NULL)
        || (mir->temporary_count != 0
            && (working == NULL || initializers == NULL))
        || (mir->value_count != 0 && staged_values == NULL)
        || depths == NULL || known == NULL || queue == NULL) {
        free(incoming);
        free(working);
        free(depths);
        free(known);
        free(queue);
        free(initializers);
        free(staged_values);
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR temporary validation allocation failed");
    }
    bool valid = true;
    for (size_t instruction = 0; valid && instruction < mir->instruction_count;
        ++instruction) {
        if (mir->instructions[instruction].kind
            == SOL_MIR_INST_TEMPORARY_INIT) {
            SolMirTemporaryId temporary
                = mir->instructions[instruction].as.temporary_init.temporary;
            if (temporary < mir->temporary_count) ++initializers[temporary];
            SolMirValueId value
                = mir->instructions[instruction].as.temporary_init.value;
            valid = value < mir->value_count && ++staged_values[value] == 1;
        }
    }
    for (size_t temporary = 0; valid && temporary < mir->temporary_count;
        ++temporary) {
        const SolMirTemporary *entry = &mir->temporaries[temporary];
        valid = initializers[temporary] == 1
            && entry->type < ir->type_count
            && entry->source_expression < ir->expression_count
            && ir->expressions[entry->source_expression].type == entry->type
            && entry->span.start
                == ir->expressions[entry->source_expression].span.start
            && entry->span.end
                == ir->expressions[entry->source_expression].span.end;
    }
    size_t first = 0;
    size_t count = 1;
    queue[0] = mir->entry;
    known[mir->entry] = true;
    while (valid && first < count) {
        SolMirBlockId block = queue[first++];
        size_t depth = depths[block];
        if (depth != 0) memcpy(working,
            &incoming[block * mir->temporary_count],
            depth * sizeof(*working));
        SolMirSlice instructions = mir->blocks[block].instructions;
        size_t drop_floor = SOL_MIR_NONE;
        for (size_t index = 0; valid && index < instructions.count; ++index) {
            const SolMirInstruction *instruction
                = &mir->instructions[instructions.offset + index];
            if (instruction->kind != SOL_MIR_INST_TEMPORARY_DROP
                && drop_floor != SOL_MIR_NONE) {
                valid = depth == drop_floor;
                drop_floor = SOL_MIR_NONE;
            }
            if (instruction->kind == SOL_MIR_INST_TEMPORARY_INIT) {
                SolMirTemporaryId temporary
                    = instruction->as.temporary_init.temporary;
                valid = temporary < mir->temporary_count
                    && depth < mir->temporary_count;
                for (size_t active = 0; valid && active < depth; ++active) {
                    valid = working[active] != temporary;
                }
                if (valid) working[depth++] = temporary;
            } else if (instruction->kind == SOL_MIR_INST_TEMPORARY_DROP) {
                size_t preserve
                    = instruction->as.temporary_drop.preserve_depth;
                valid = preserve < depth
                    && (drop_floor == SOL_MIR_NONE || drop_floor == preserve)
                    && working[preserve]
                        == instruction->as.temporary_drop.temporary;
                if (valid) {
                    drop_floor = preserve;
                    memmove(&working[preserve], &working[preserve + 1],
                        (depth - preserve - 1) * sizeof(*working));
                    --depth;
                }
            } else if (instruction->kind == SOL_MIR_INST_CONSTRUCT) {
                SolMirSlice operands = instruction->as.construct.operands;
                valid = operands.count <= depth;
                for (size_t operand = 0; valid && operand < operands.count;
                    ++operand) {
                    valid = working[depth - operands.count + operand]
                        == mir->construct_operands[
                            operands.offset + operand].temporary;
                }
                if (valid) depth -= operands.count;
            } else if (instruction->kind == SOL_MIR_INST_COMPOUND_UPDATE) {
                valid = depth != 0 && working[depth - 1]
                    == instruction->as.compound_update.previous;
                if (valid) --depth;
            } else if (instruction->kind == SOL_MIR_INST_PATTERN_TEST
                || instruction->kind == SOL_MIR_INST_PATTERN_VALUE
                || instruction->kind == SOL_MIR_INST_MATCH_ARM) {
                bool active = false;
                for (size_t active_index = 0; active_index < depth;
                    ++active_index) {
                    active = active || working[active_index]
                        == instruction->as.pattern.scrutinee;
                }
                valid = active;
            }
        }
        if (valid && drop_floor != SOL_MIR_NONE && depth != drop_floor) {
            valid = false;
        }
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
            SolMirSlice arguments = term->as.invoke.arguments;
            size_t owned = 0;
            for (size_t index = 0; index < arguments.count; ++index) {
                owned += mir->call_arguments[arguments.offset + index].access
                    == SOL_ACCESS_OWNED;
            }
            bool has_callee = term->as.invoke.kind == SOL_IR_CALL_CALLBACK;
            bool has_receiver = (term->as.invoke.kind == SOL_IR_CALL_METHOD
                    || term->as.invoke.kind == SOL_IR_CALL_CAPABILITY)
                && term->as.invoke.receiver.access == SOL_ACCESS_OWNED;
            size_t consumed = owned + (has_callee ? 1u : 0u)
                + (has_receiver ? 1u : 0u);
            valid = consumed <= depth
                && (!has_callee || working[depth - consumed]
                    == term->as.invoke.callee)
                && (!has_receiver || working[depth - consumed]
                    == term->as.invoke.receiver.temporary);
            size_t ordinal = 0;
            for (size_t index = 0; valid && index < arguments.count; ++index) {
                const SolMirCallArgument *argument
                    = &mir->call_arguments[arguments.offset + index];
                if (argument->access != SOL_ACCESS_OWNED) continue;
                valid = working[depth - owned + ordinal++]
                    == argument->temporary;
            }
            if (valid) depth -= consumed;
        } else if (valid && term->kind == SOL_MIR_TERM_CHECK_REFINED) {
            valid = depth != 0
                && working[depth - 1] == term->as.check_refined.representation;
            if (valid) --depth;
        } else if (valid && term->kind == SOL_MIR_TERM_PROPAGATE) {
            valid = depth != 0
                && working[depth - 1] == term->as.propagate.operand;
            if (valid) --depth;
        }
        if (valid && depth != 0
            && (term->kind == SOL_MIR_TERM_RETURN
                || term->kind == SOL_MIR_TERM_PANIC
                || term->kind == SOL_MIR_TERM_RESUME_FAILURE
                || term->kind == SOL_MIR_TERM_MATCH_FAILURE
                || term->kind == SOL_MIR_TERM_UNREACHABLE
                || term->kind == SOL_MIR_TERM_CONTRACT_VIOLATION)) valid = false;
        SolMirBlockId targets[3];
        size_t target_count = 0;
        if (valid && term->kind == SOL_MIR_TERM_GOTO) {
            targets[target_count++] = term->as.go_to.block;
        } else if (valid && term->kind == SOL_MIR_TERM_BRANCH) {
            targets[target_count++] = term->as.branch.true_edge.block;
            targets[target_count++] = term->as.branch.false_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
            if (term->as.invoke.normal_edge.block != SOL_MIR_NONE) {
                targets[target_count++] = term->as.invoke.normal_edge.block;
            }
            targets[target_count++] = term->as.invoke.failure_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_CHECK_REFINED) {
            targets[target_count++] = term->as.check_refined.normal_edge.block;
            targets[target_count++] = term->as.check_refined.failure_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_PROPAGATE) {
            targets[target_count++] = term->as.propagate.value_edge.block;
            targets[target_count++] = term->as.propagate.residual_edge.block;
        } else if (valid && term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
            targets[target_count++]
                = term->as.check_contract.satisfied_edge.block;
            targets[target_count++]
                = term->as.check_contract.violation_edge.block;
            targets[target_count++]
                = term->as.check_contract.failure_edge.block;
        } else if (valid && (term->kind == SOL_MIR_TERM_BREAK
            || term->kind == SOL_MIR_TERM_CONTINUE)) {
            targets[target_count++] = term->as.transfer.edge.block;
        }
        for (size_t index = 0; valid && index < target_count; ++index) {
            SolMirBlockId target = targets[index];
            if (!known[target]) {
                known[target] = true;
                depths[target] = depth;
                if (depth != 0) memcpy(
                    &incoming[target * mir->temporary_count], working,
                    depth * sizeof(*working));
                queue[count++] = target;
            } else {
                valid = depths[target] == depth
                    && (depth == 0 || memcmp(
                        &incoming[target * mir->temporary_count], working,
                        depth * sizeof(*working)) == 0);
            }
        }
    }
    free(incoming);
    free(working);
    free(depths);
    free(known);
    free(queue);
    free(initializers);
    free(staged_values);
    return valid || mir_error(diagnostics,
        ir->callables[mir->callable].span,
        "MIR owned temporary transitions are inconsistent");
}

static bool mir_validate_source_events(const SolIr *ir, const SolMir *mir,
    SolDiagnostics *diagnostics) {
    SolIrStatementId *statements = ir->statement_count == 0 ? NULL
        : malloc(ir->statement_count * sizeof(*statements));
    bool *orders = calloc(mir->block_count, sizeof(*orders));
    if ((ir->statement_count != 0 && statements == NULL) || orders == NULL) {
        free(statements);
        free(orders);
        return mir_error(diagnostics, ir->callables[mir->callable].span,
            "MIR source-event validation allocation failed");
    }
    size_t statement_count = 0;
    bool valid = mir_collect_source_statements(ir, mir,
        ir->callables[mir->callable].body, statements, &statement_count, 0);
    for (size_t block = 0; valid && block < mir->block_count; ++block) {
        size_t order = mir->blocks[block].order;
        valid = order < mir->block_count && !orders[order];
        if (valid) orders[order] = true;
    }
    size_t previous_offset = 0;
    for (size_t order = 0; valid && order < mir->block_count; ++order) {
        SolMirBlockId block = SOL_MIR_NONE;
        for (size_t candidate = 0; candidate < mir->block_count; ++candidate) {
            if (mir->blocks[candidate].order == order) {
                block = candidate;
                break;
            }
        }
        valid = block != SOL_MIR_NONE
            && mir->blocks[block].instructions.offset >= previous_offset;
        if (valid) previous_offset = mir->blocks[block].instructions.offset;
    }
    size_t cursor = 0;
    for (size_t index = 0; valid && index < mir->instruction_count; ++index) {
        const SolMirInstruction *instruction = &mir->instructions[index];
        if (instruction->kind != SOL_MIR_INST_REGION_ENTER) continue;
        valid = mir_next_source_statement(ir, statements, statement_count,
                &cursor, SOL_IR_STATEMENT_REGION) == instruction->as.region;
    }
    if (valid) {
        valid = mir_next_source_statement(ir, statements, statement_count,
            &cursor, SOL_IR_STATEMENT_REGION) == SOL_IR_NONE;
    }
    cursor = 0;
    for (size_t loop = 0; valid && loop < mir->loop_count; ++loop) {
        valid = mir_next_source_loop(ir, statements, statement_count, &cursor)
            == mir->loops[loop].statement;
    }
    if (valid) {
        valid = mir_next_source_loop(ir, statements, statement_count, &cursor)
            == SOL_IR_NONE;
    }
    cursor = 0;
    for (size_t order = 0; valid && order < mir->block_count; ++order) {
        SolMirBlockId block = SOL_MIR_NONE;
        for (size_t candidate = 0; candidate < mir->block_count; ++candidate) {
            if (mir->blocks[candidate].order == order) {
                block = candidate;
                break;
            }
        }
        if (block == SOL_MIR_NONE
            || mir->blocks[block].terminator.kind
                != SOL_MIR_TERM_UNREACHABLE) continue;
        valid = mir_next_source_statement(ir, statements, statement_count,
                &cursor, SOL_IR_STATEMENT_UNREACHABLE)
            == mir->blocks[block].terminator.as.unreachable.statement;
    }
    if (valid) {
        valid = mir_next_source_statement(ir, statements, statement_count,
            &cursor, SOL_IR_STATEMENT_UNREACHABLE) == SOL_IR_NONE;
    }
    cursor = 0;
    for (size_t order = 0; valid && order < mir->block_count; ++order) {
        SolMirBlockId block = SOL_MIR_NONE;
        for (size_t candidate = 0; candidate < mir->block_count; ++candidate) {
            if (mir->blocks[candidate].order == order) {
                block = candidate;
                break;
            }
        }
        if (block == SOL_MIR_NONE
            || (mir->blocks[block].terminator.kind != SOL_MIR_TERM_BREAK
                && mir->blocks[block].terminator.kind
                    != SOL_MIR_TERM_CONTINUE)) continue;
        valid = mir_next_source_transfer(ir, statements, statement_count,
                &cursor)
            == mir->blocks[block].terminator.as.transfer.statement;
    }
    if (valid) {
        valid = mir_next_source_transfer(ir, statements, statement_count,
            &cursor) == SOL_IR_NONE;
    }
    for (size_t source_index = 0; valid && source_index < statement_count;
        ++source_index) {
        SolIrStatementId statement_id = statements[source_index];
        const SolIrStatement *statement = &ir->statements[statement_id];
        if (statement->kind != SOL_IR_STATEMENT_ASSIGNMENT
            || statement->operator_kind == SOL_TOKEN_EQUAL) continue;
        size_t reads = 0;
        size_t updates = 0;
        for (size_t instruction = 0; instruction < mir->instruction_count;
            ++instruction) {
            reads += mir->instructions[instruction].kind
                    == SOL_MIR_INST_LOAD_UPDATE
                && mir->instructions[instruction].as.update_load.statement
                    == statement_id;
            updates += mir->instructions[instruction].kind
                    == SOL_MIR_INST_COMPOUND_UPDATE
                && mir->instructions[instruction].as.compound_update.statement
                    == statement_id;
        }
        bool rhs_never = mir_type_is(ir,
            ir->expressions[statement->expression].type, SOL_IR_TYPE_NEVER);
        valid = reads == 1 && updates == (rhs_never ? 0u : 1u);
    }
    for (size_t instruction = 0; valid && instruction < mir->instruction_count;
        ++instruction) {
        const SolMirInstruction *item = &mir->instructions[instruction];
        SolIrStatementId statement = item->kind == SOL_MIR_INST_LOAD_UPDATE
            ? item->as.update_load.statement
            : item->kind == SOL_MIR_INST_COMPOUND_UPDATE
                ? item->as.compound_update.statement : SOL_IR_NONE;
        if (statement == SOL_IR_NONE) continue;
        bool found = false;
        for (size_t source_index = 0; source_index < statement_count;
            ++source_index) {
            found = found || statements[source_index] == statement;
        }
        valid = found;
    }
    for (SolIrExpressionId source = 0;
        valid && source < ir->expression_count; ++source) {
        const SolIrExpression *expression = &ir->expressions[source];
        if (expression->kind != SOL_IR_EXPR_CALL
            || (expression->as.call.kind != SOL_IR_CALL_FUNCTION
                && expression->as.call.kind != SOL_IR_CALL_CALLBACK
                && expression->as.call.kind != SOL_IR_CALL_CAPABILITY
                && expression->as.call.kind != SOL_IR_CALL_METHOD)) continue;
        size_t invokes = 0;
        for (SolMirBlockId block = 0; block < mir->block_count; ++block) {
            invokes += mir->blocks[block].terminator.kind == SOL_MIR_TERM_INVOKE
                && mir->blocks[block].terminator.as.invoke.source_expression
                    == source;
        }
        bool required = mir_source_reaches_match(ir,
            ir->callables[mir->callable].body, source, 0);
        if (invokes != (required ? 1u : 0u)) {
            free(statements);
            free(orders);
            return mir_error(diagnostics, expression->span,
                !required ? "MIR source call is foreign"
                : invokes == 0 ? "MIR source call is missing"
                : "MIR source call is duplicated");
        }
    }
    free(statements);
    free(orders);
    return valid || mir_error(diagnostics,
        ir->callables[mir->callable].span,
        "MIR source control events are missing or reordered");
}

static size_t mir_predecessor_count(const SolMir *mir, SolMirBlockId target) {
    size_t count = 0;
    for (size_t block = 0; block < mir->block_count; ++block) {
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        if (term->kind == SOL_MIR_TERM_GOTO) {
            count += term->as.go_to.block == target;
        } else if (term->kind == SOL_MIR_TERM_BRANCH) {
            count += term->as.branch.true_edge.block == target;
            count += term->as.branch.false_edge.block == target;
        } else if (term->kind == SOL_MIR_TERM_INVOKE) {
            count += term->as.invoke.normal_edge.block == target;
            count += term->as.invoke.failure_edge.block == target;
        } else if (term->kind == SOL_MIR_TERM_CHECK_REFINED) {
            count += term->as.check_refined.normal_edge.block == target;
            count += term->as.check_refined.failure_edge.block == target;
        } else if (term->kind == SOL_MIR_TERM_PROPAGATE) {
            count += term->as.propagate.value_edge.block == target;
            count += term->as.propagate.residual_edge.block == target;
        } else if (term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
            count += term->as.check_contract.satisfied_edge.block == target;
            count += term->as.check_contract.violation_edge.block == target;
            count += term->as.check_contract.failure_edge.block == target;
        } else if (term->kind == SOL_MIR_TERM_BREAK
            || term->kind == SOL_MIR_TERM_CONTINUE) {
            count += term->as.transfer.edge.block == target;
        }
    }
    return count;
}

static bool mir_validate_contract_envelope(const SolIr *ir, const SolMir *mir,
    SolDiagnostics *diagnostics) {
    const SolIrCallable *callable = &ir->callables[mir->callable];
    if (!mir_ir_slice_equal(mir->generic_parameters,
            callable->generic_parameters)
        || !mir_ir_slice_equal(mir->effect_parameters,
            callable->effect_parameters)) {
        return mir_error(diagnostics, callable->span,
            "MIR callable generic metadata is inconsistent");
    }
    size_t obligation_count = 0;
    size_t require_count = 0;
    size_t ensure_count = 0;
    size_t snapshot_count = 0;
    for (size_t id = 0; id < ir->obligation_count; ++id) {
        const SolIrObligation *obligation = &ir->obligations[id];
        if (!mir_obligation_owned_by_callable(obligation, callable)) continue;
        ++obligation_count;
        require_count += obligation->kind == SOL_CONTRACT_REQUIRES;
        ensure_count += obligation->kind == SOL_CONTRACT_ENSURES;
        if (!mir_slice_valid((SolMirSlice){obligation->snapshots.offset,
                obligation->snapshots.count}, ir->snapshot_count)) {
            return mir_error(diagnostics, callable->span,
                "malformed owning contract snapshot slice");
        }
        for (size_t index = 0; index < obligation->snapshots.count; ++index) {
            SolIrSnapshotId snapshot = obligation->snapshots.offset + index;
            if (ir->snapshots[snapshot].id != snapshot
                || ir->snapshots[snapshot].obligation != id) {
                return mir_error(diagnostics, callable->span,
                    "malformed owning contract snapshot order");
            }
            ++snapshot_count;
        }
    }
    if (obligation_count == 0) {
        if (mir->contract_body != SOL_MIR_NONE
            || mir->contract_epilogue != SOL_MIR_NONE) {
            return mir_error(diagnostics, callable->span,
                "uncontracted MIR has a contract envelope");
        }
        for (size_t block = 0; block < mir->block_count; ++block) {
            if (mir->blocks[block].terminator.kind
                    == SOL_MIR_TERM_CHECK_CONTRACT
                || mir->blocks[block].terminator.kind
                    == SOL_MIR_TERM_CONTRACT_VIOLATION) {
                return mir_error(diagnostics, callable->span,
                    "uncontracted MIR has contract control flow");
            }
        }
        return true;
    }
    if (mir->contract_body >= mir->block_count
        || mir->contract_epilogue >= mir->block_count
        || mir->contract_body == mir->contract_epilogue
        || mir->blocks[mir->contract_epilogue].parameters.count != 1) {
        return mir_error(diagnostics, callable->span,
            "malformed MIR contract envelope anchors");
    }
    size_t seen_checks = 0;
    size_t seen_violations = 0;
    size_t returns = 0;
    for (size_t block = 0; block < mir->block_count; ++block) {
        seen_checks += mir->blocks[block].terminator.kind
            == SOL_MIR_TERM_CHECK_CONTRACT;
        seen_violations += mir->blocks[block].terminator.kind
            == SOL_MIR_TERM_CONTRACT_VIOLATION;
        returns += mir->blocks[block].terminator.kind == SOL_MIR_TERM_RETURN;
    }
    if (seen_checks != require_count + ensure_count
        || seen_violations != seen_checks || returns != 1) {
        return mir_error(diagnostics, callable->span,
            "MIR contract checks are missing, duplicated, or bypassed");
    }
    SolMirBlockId current = mir->entry;
    for (size_t id = 0; id < ir->obligation_count; ++id) {
        const SolIrObligation *obligation = &ir->obligations[id];
        if (!mir_obligation_owned_by_callable(obligation, callable)
            || obligation->kind != SOL_CONTRACT_REQUIRES) continue;
        const SolMirTerminator *term = &mir->blocks[current].terminator;
        if (term->kind != SOL_MIR_TERM_CHECK_CONTRACT
            || term->as.check_contract.obligation != id
            || term->as.check_contract.result != SOL_MIR_NONE) {
            return mir_error(diagnostics, callable->span,
                "MIR requires checks are missing or reordered");
        }
        current = term->as.check_contract.satisfied_edge.block;
        if (mir_predecessor_count(mir, current) != 1) {
            return mir_error(diagnostics, callable->span,
                "MIR requires continuation has a bypass edge");
        }
    }
    if (current != mir->contract_body) {
        return mir_error(diagnostics, callable->span,
            "MIR contract body bypasses requires checks");
    }
    size_t seen_snapshots = 0;
    bool body_started = false;
    SolMirSlice body_instructions = mir->blocks[mir->contract_body].instructions;
    for (size_t index = 0; index < body_instructions.count; ++index) {
        const SolMirInstruction *instruction
            = &mir->instructions[body_instructions.offset + index];
        if (instruction->kind != SOL_MIR_INST_CAPTURE_SNAPSHOT) {
            body_started = body_started
                || instruction->kind != SOL_MIR_INST_PARAMETER_LIVE;
            continue;
        }
        if (body_started) {
            return mir_error(diagnostics, instruction->span,
                "MIR snapshot capture occurs after body execution");
        }
        SolIrSnapshotId expected = SOL_IR_NONE;
        size_t ordinal = 0;
        for (size_t id = 0; id < ir->snapshot_count; ++id) {
            if (ir->snapshots[id].obligation < ir->obligation_count
                && mir_obligation_owned_by_callable(
                    &ir->obligations[ir->snapshots[id].obligation], callable)) {
                if (ordinal++ == seen_snapshots) expected = id;
            }
        }
        if (instruction->as.snapshot != expected) {
            return mir_error(diagnostics, instruction->span,
                "MIR snapshots are missing or reordered");
        }
        ++seen_snapshots;
    }
    for (size_t instruction = 0; instruction < mir->instruction_count;
        ++instruction) {
        if (mir->instructions[instruction].kind == SOL_MIR_INST_CAPTURE_SNAPSHOT
            && mir->instructions[instruction].block != mir->contract_body) {
            return mir_error(diagnostics, mir->instructions[instruction].span,
                "MIR snapshot is outside the contract entry boundary");
        }
    }
    if (seen_snapshots != snapshot_count) {
        return mir_error(diagnostics, callable->span,
            "MIR snapshots are incomplete");
    }
    current = mir->contract_epilogue;
    SolMirValueId result = mir->parameter_values[
        mir->blocks[current].parameters.offset];
    for (size_t id = 0; id < ir->obligation_count; ++id) {
        const SolIrObligation *obligation = &ir->obligations[id];
        if (!mir_obligation_owned_by_callable(obligation, callable)
            || obligation->kind != SOL_CONTRACT_ENSURES) continue;
        const SolMirTerminator *term = &mir->blocks[current].terminator;
        if (term->kind != SOL_MIR_TERM_CHECK_CONTRACT
            || term->as.check_contract.obligation != id
            || term->as.check_contract.result != result) {
            return mir_error(diagnostics, callable->span,
                "MIR ensures checks are missing or reordered");
        }
        current = term->as.check_contract.satisfied_edge.block;
        if (mir->blocks[current].parameters.count != 1
            || mir_predecessor_count(mir, current) != 1) {
            return mir_error(diagnostics, callable->span,
                "MIR ensures result transport is incomplete");
        }
        result = mir->parameter_values[mir->blocks[current].parameters.offset];
    }
    if (mir->blocks[current].terminator.kind != SOL_MIR_TERM_RETURN
        || mir->blocks[current].terminator.as.value != result) {
        return mir_error(diagnostics, callable->span,
            "MIR contract epilogue does not preserve the complete result");
    }
    for (size_t block = 0; block < mir->block_count; ++block) {
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        if (term->kind != SOL_MIR_TERM_GOTO
            || term->as.go_to.block != mir->contract_epilogue) continue;
        if (term->as.go_to.arguments.count != 1
            || mir->values[mir->edge_values[term->as.go_to.arguments.offset]].type
                != callable->result) {
            return mir_error(diagnostics, term->span,
                "MIR successful result bypasses the contract epilogue");
        }
    }
    return true;
}

bool sol_mir_validate(const SolIr *ir, const SolMir *mir,
    SolDiagnostics *diagnostics) {
    if (ir == NULL || mir == NULL || !sol_ir_validate(ir, diagnostics)
        || mir->callable >= ir->callable_count
        || mir->entry >= mir->block_count
        || mir->block_count > SIZE_MAX / sizeof(SolMirBlockId)
        || mir->loop_count > SIZE_MAX / sizeof(SolMirLoop)
        || mir->construct_operand_count
            > SIZE_MAX / sizeof(SolMirConstructOperand)
        || mir->temporary_count > SIZE_MAX / sizeof(SolMirTemporary)
        || mir->block_count > mir->block_capacity
        || mir->instruction_count > mir->instruction_capacity
        || mir->value_count > mir->value_capacity
        || mir->parameter_value_count > mir->parameter_value_capacity
        || mir->edge_value_count > mir->edge_value_capacity
        || mir->call_argument_count > mir->call_argument_capacity
        || mir->loop_count > mir->loop_capacity
        || mir->construct_operand_count > mir->construct_operand_capacity
        || mir->temporary_count > mir->temporary_capacity
        || (mir->block_count != 0 && mir->blocks == NULL)
        || (mir->instruction_count != 0 && mir->instructions == NULL)
        || (mir->value_count != 0 && mir->values == NULL)
        || (mir->parameter_value_count != 0 && mir->parameter_values == NULL)
        || (mir->edge_value_count != 0 && mir->edge_values == NULL)
        || (mir->call_argument_count != 0 && mir->call_arguments == NULL)
        || (mir->loop_count != 0 && mir->loops == NULL)
        || (mir->construct_operand_count != 0
            && mir->construct_operands == NULL)
        || (mir->temporary_count != 0 && mir->temporaries == NULL)) {
        return mir_error(diagnostics, (SolSpan){0}, "malformed MIR arena header");
    }
    const SolIrCallable *callable = &ir->callables[mir->callable];
    if (mir->blocks[mir->entry].parameters.count != 0) {
        return mir_error(diagnostics, callable->span,
            "MIR entry block cannot have SSA parameters");
    }
    for (size_t loop_id = 0; loop_id < mir->loop_count; ++loop_id) {
        const SolMirLoop *loop = &mir->loops[loop_id];
        const SolIrStatement *source_loop = loop->statement < ir->statement_count
            ? &ir->statements[loop->statement] : NULL;
        SolIrStatementId parent_statement = SOL_IR_NONE;
        bool found_loop = mir_find_statement_loop_parent(ir, callable->body,
            loop->statement, SOL_IR_NONE, &parent_statement, 0);
        SolMirLoopId expected_parent = SOL_MIR_NONE;
        if (parent_statement != SOL_IR_NONE) {
            for (size_t previous = 0; previous < loop_id; ++previous) {
                if (mir->loops[previous].statement == parent_statement) {
                    expected_parent = previous;
                    break;
                }
            }
        }
        bool is_while = source_loop != NULL
            && source_loop->kind == SOL_IR_STATEMENT_WHILE;
        if (source_loop == NULL
            || (source_loop->kind != SOL_IR_STATEMENT_LOOP && !is_while)
            || !mir_expression_contains_statement(ir, callable->body,
                loop->statement, 0)
            || !found_loop
            || (parent_statement != SOL_IR_NONE
                && expected_parent == SOL_MIR_NONE)
            || loop->parent != expected_parent
            || loop->preheader >= mir->block_count
            || loop->header >= mir->block_count
            || loop->header == mir->entry
            || loop->preheader == loop->header
            || mir->blocks[loop->preheader].terminator.kind
                != SOL_MIR_TERM_GOTO
            || mir->blocks[loop->preheader].terminator.as.go_to.block
                != loop->header
            || mir->blocks[loop->preheader].terminator.as.go_to.arguments.count
                != 0
            || mir->blocks[loop->header].parameters.count != 0
            || (loop->body != SOL_MIR_NONE
                && (loop->body >= mir->block_count
                    || loop->body == loop->preheader
                    || loop->body == loop->header
                    || mir->blocks[loop->body].parameters.count != 0))
            || (loop->body == SOL_MIR_NONE
                ? loop->backedge != SOL_MIR_NONE
                : (mir_type_is(ir,
                        ir->expressions[source_loop->expression].type,
                        SOL_IR_TYPE_UNIT)
                    ? loop->backedge >= mir->block_count
                        || loop->backedge == loop->preheader
                        || loop->backedge == loop->header
                        || loop->backedge == loop->exit
                        || mir->blocks[loop->backedge].terminator.kind
                            != SOL_MIR_TERM_GOTO
                        || mir->blocks[loop->backedge].terminator.as.go_to.block
                            != loop->header
                        || mir->blocks[loop->backedge].terminator.as.go_to
                            .arguments.count != 0
                    : loop->backedge != SOL_MIR_NONE))
            || (loop->exit != SOL_MIR_NONE
                && (loop->exit >= mir->block_count
                    || loop->exit == loop->preheader
                    || loop->exit == loop->header
                    || loop->exit == loop->body
                    || mir->blocks[loop->exit].parameters.count != 0))
            || loop->obligations.offset
                != source_loop->loop_obligations.offset
            || loop->obligations.count
                != source_loop->loop_obligations.count
            || loop->span.start != source_loop->span.start
            || loop->span.end != source_loop->span.end
            || (!is_while
                && loop->body == SOL_MIR_NONE)
            || (!is_while
                && (loop->condition != loop->header
                    || mir->blocks[loop->header].terminator.kind
                        != SOL_MIR_TERM_GOTO
                    || mir->blocks[loop->header].terminator.as.go_to.block
                        != loop->body
                    || mir->blocks[loop->header].terminator.as.go_to.arguments.count
                        != 0))
            || (is_while
                && mir_type_is(ir,
                    ir->expressions[source_loop->condition].type,
                    SOL_IR_TYPE_BOOL)
                && (loop->condition >= mir->block_count
                    || loop->body == SOL_MIR_NONE
                    || loop->exit == SOL_MIR_NONE
                    || loop->condition == loop->body
                    || loop->condition == loop->exit
                    || loop->body == loop->exit
                    || mir->blocks[loop->condition].terminator.kind
                        != SOL_MIR_TERM_BRANCH
                    || mir->blocks[loop->condition].terminator.as.branch
                        .true_edge.block != loop->body
                    || mir->blocks[loop->condition].terminator.as.branch
                        .false_edge.block != loop->exit
                    || mir->blocks[loop->condition].terminator.as.branch
                        .true_edge.arguments.count != 0
                    || mir->blocks[loop->condition].terminator.as.branch
                        .false_edge.arguments.count != 0))
            || (is_while
                && mir_type_is(ir, ir->expressions[source_loop->condition].type,
                    SOL_IR_TYPE_NEVER)
                && (loop->condition != SOL_MIR_NONE
                    || loop->body != SOL_MIR_NONE))) {
            return mir_error(diagnostics, callable->span,
                "malformed MIR loop metadata");
        }
        for (size_t previous = 0; previous < loop_id; ++previous) {
            if (mir->loops[previous].statement == loop->statement) {
                return mir_error(diagnostics, callable->span,
                    "MIR loop source statement is duplicated");
            }
        }
        for (size_t index = 0; index < loop->obligations.count; ++index) {
            const SolIrLoopObligation *obligation = &ir->loop_obligations[
                loop->obligations.offset + index];
            if (obligation->loop_statement != loop->statement
                || obligation->callable != mir->callable) {
                return mir_error(diagnostics, obligation->span,
                    "MIR loop obligation metadata is inconsistent");
            }
        }
    }
    unsigned char *instruction_owners = mir->instruction_count == 0 ? NULL
        : calloc(mir->instruction_count, 1);
    unsigned char *parameter_owners = mir->parameter_value_count == 0 ? NULL
        : calloc(mir->parameter_value_count, 1);
    if ((mir->instruction_count != 0 && instruction_owners == NULL)
        || (mir->parameter_value_count != 0 && parameter_owners == NULL)) {
        free(instruction_owners);
        free(parameter_owners);
        return mir_error(diagnostics, callable->span,
            "MIR validation allocation failed");
    }
    for (size_t block_id = 0; block_id < mir->block_count; ++block_id) {
        const SolMirBlock *block = &mir->blocks[block_id];
        if (block->id != block_id || !block->started
            || !mir_span_valid(ir, block->span)
            || !mir_slice_valid(block->parameters, mir->parameter_value_count)
            || !mir_slice_valid(block->instructions, mir->instruction_count)
            || block->terminator.kind <= SOL_MIR_TERM_INVALID
            || block->terminator.kind > SOL_MIR_TERM_CONTRACT_VIOLATION
            || !mir_span_valid(ir, block->terminator.span)) {
            free(instruction_owners);
            free(parameter_owners);
            return mir_error(diagnostics, block->span, "malformed MIR block");
        }
        for (size_t index = 0; index < block->instructions.count; ++index) {
            size_t slot = block->instructions.offset + index;
            if (++instruction_owners[slot] != 1) {
                free(instruction_owners);
                free(parameter_owners);
                return mir_error(diagnostics, block->span,
                    "MIR instruction slice is shared");
            }
        }
        for (size_t index = 0; index < block->parameters.count; ++index) {
            size_t slot = block->parameters.offset + index;
            SolMirValueId id = mir->parameter_values[slot];
            if (id >= mir->value_count
                || ++parameter_owners[slot] != 1
                || mir->values[id].kind != SOL_MIR_VALUE_BLOCK_PARAMETER
                || mir->values[id].block != block_id
                || mir->values[id].definition != index
                || (mir->values[id].source_expression != SOL_IR_NONE
                    && mir->values[id].source_expression
                        >= ir->expression_count)) {
                free(instruction_owners);
                free(parameter_owners);
                return mir_error(diagnostics, block->span,
                    "malformed MIR block parameter");
            }
        }
        switch (block->terminator.kind) {
            case SOL_MIR_TERM_GOTO:
                if (!mir_validate_edge(mir, block_id,
                    &block->terminator.as.go_to, SOL_MIR_NONE)) {
                    free(instruction_owners);
                    free(parameter_owners);
                    return mir_error(diagnostics, block->terminator.span,
                        "malformed MIR goto edge");
                }
                break;
            case SOL_MIR_TERM_BRANCH:
                if (block->terminator.as.branch.condition >= mir->value_count
                    || !mir_type_is(ir, mir->values[
                        block->terminator.as.branch.condition].type,
                        SOL_IR_TYPE_BOOL)
                    || !mir_value_available(mir,
                        block->terminator.as.branch.condition, block_id,
                        block->instructions.offset + block->instructions.count)
                    || !mir_validate_edge(mir, block_id,
                        &block->terminator.as.branch.true_edge, SOL_MIR_NONE)
                    || !mir_validate_edge(mir, block_id,
                        &block->terminator.as.branch.false_edge, SOL_MIR_NONE)) {
                    free(instruction_owners);
                    free(parameter_owners);
                    return mir_error(diagnostics, block->terminator.span,
                        "malformed MIR branch");
                }
                break;
            case SOL_MIR_TERM_RETURN:
                if (block->terminator.as.value >= mir->value_count
                    || !mir_value_available(mir, block->terminator.as.value,
                        block_id, block->instructions.offset
                            + block->instructions.count)
                    || mir->values[block->terminator.as.value].type
                        != callable->result) {
                    free(instruction_owners);
                    free(parameter_owners);
                    return mir_error(diagnostics, block->terminator.span,
                        "malformed MIR return");
                }
                break;
            case SOL_MIR_TERM_PANIC:
                if (block->terminator.as.value >= mir->value_count
                    || !mir_value_available(mir, block->terminator.as.value,
                        block_id, block->instructions.offset
                            + block->instructions.count)
                    || !mir_type_is(ir, mir->values[
                        block->terminator.as.value].type, SOL_IR_TYPE_TEXT)) {
                    free(instruction_owners);
                    free(parameter_owners);
                    return mir_error(diagnostics, block->terminator.span,
                        "malformed MIR panic");
                }
                break;
            case SOL_MIR_TERM_INVOKE: {
                const SolMirTerminator *term = &block->terminator;
                const SolIrExpression *source
                    = term->as.invoke.source_expression < ir->expression_count
                    ? &ir->expressions[term->as.invoke.source_expression] : NULL;
                bool never = source != NULL
                    && mir_type_is(ir, source->type, SOL_IR_TYPE_NEVER);
                bool callback = source != NULL
                    && source->kind == SOL_IR_EXPR_CALL
                    && source->as.call.kind == SOL_IR_CALL_CALLBACK;
                bool method = source != NULL
                    && source->kind == SOL_IR_EXPR_CALL
                    && source->as.call.kind == SOL_IR_CALL_METHOD;
                bool capability = source != NULL
                    && source->kind == SOL_IR_EXPR_CALL
                    && source->as.call.kind == SOL_IR_CALL_CAPABILITY;
                const SolIrType *callback_type = callback
                        && source->as.call.callee < ir->expression_count
                        && ir->expressions[source->as.call.callee].type
                            < ir->type_count
                    ? &ir->types[ir->expressions[
                        source->as.call.callee].type] : NULL;
                bool invoke_valid = source != NULL
                    && source->kind == SOL_IR_EXPR_CALL
                    && source->as.call.kind == term->as.invoke.kind
                    && ((method || capability)
                        || (term->as.invoke.receiver.source_expression
                                == SOL_IR_NONE
                            && term->as.invoke.receiver.temporary == SOL_MIR_NONE
                            && term->as.invoke.receiver.place == SOL_IR_NONE))
                    && mir_ir_slice_equal(term->as.invoke.type_arguments,
                        source->as.call.type_arguments)
                    && mir_ir_slice_equal(term->as.invoke.effects,
                        source->as.call.effects)
                    && term->as.invoke.effect_parameter
                        == source->as.call.effect_parameter
                    && mir_ir_slice_equal(term->as.invoke.evidence,
                        source->as.call.evidence)
                    && mir_slice_valid((SolMirSlice){
                            term->as.invoke.type_arguments.offset,
                            term->as.invoke.type_arguments.count},
                        ir->type_id_count)
                    && mir_slice_valid((SolMirSlice){
                            term->as.invoke.effects.offset,
                            term->as.invoke.effects.count}, ir->effect_count)
                    && mir_slice_valid((SolMirSlice){
                            term->as.invoke.evidence.offset,
                            term->as.invoke.evidence.count}, ir->evidence_count)
                    && (term->as.invoke.effect_parameter == SOL_IR_NONE
                        || term->as.invoke.effect_parameter
                            < ir->effect_parameter_count)
                    && (!method || source->as.call.evidence.count == 1)
                    && ((!callback && !capability)
                        || source->as.call.evidence.count == 0)
                    && mir_slice_valid(term->as.invoke.arguments,
                        mir->call_argument_count)
                    && term->as.invoke.arguments.count
                        == source->as.call.operands.count
                    && term->as.invoke.failure_edge.arguments.count == 0
                    && mir_validate_edge(mir, block_id,
                        &term->as.invoke.failure_edge, SOL_MIR_NONE)
                    && mir->blocks[term->as.invoke.failure_edge.block]
                        .terminator.kind == SOL_MIR_TERM_RESUME_FAILURE;
                if (invoke_valid && capability) {
                    const SolIrExpression *callee
                        = source->as.call.callee < ir->expression_count
                        ? &ir->expressions[source->as.call.callee] : NULL;
                    const SolIrCallable *target
                        = term->as.invoke.callable < ir->callable_count
                        ? &ir->callables[term->as.invoke.callable] : NULL;
                    const SolIrExpression *receiver = callee != NULL
                            && callee->kind == SOL_IR_EXPR_BOUND_OPERATION
                            && callee->as.operation.receiver < ir->expression_count
                        ? &ir->expressions[callee->as.operation.receiver] : NULL;
                    invoke_valid = callee != NULL && receiver != NULL
                        && target != NULL
                        && callee->kind == SOL_IR_EXPR_BOUND_OPERATION
                        && callee->as.operation.callable
                            == source->as.call.callable
                        && source->as.call.callable == term->as.invoke.callable
                        && target->kind == SOL_IR_CALLABLE_CAPABILITY
                        && target->receiver == SOL_IR_NONE
                        && source->as.call.receiver_access == SOL_ACCESS_SHARED
                        && receiver->type < ir->type_count
                        && ir->types[receiver->type].kind == SOL_IR_TYPE_NOMINAL
                        && ir->types[receiver->type].definition == target->owner
                        && receiver->capability_roots.count != 0
                        && mir_capability_effects_match(ir, source, target,
                            receiver)
                        && target->generic_parameters.count == 0
                        && target->effect_parameters.count == 0
                        && target->result == source->type
                        && target->parameters.count
                            == term->as.invoke.arguments.count
                        && term->as.invoke.callee == SOL_MIR_NONE
                        && term->as.invoke.receiver.formal == SOL_IR_NONE
                        && term->as.invoke.receiver.access
                            == source->as.call.receiver_access
                        && term->as.invoke.receiver.source_expression
                            == callee->as.operation.receiver;
                    if (invoke_valid
                        && term->as.invoke.receiver.access == SOL_ACCESS_OWNED) {
                        invoke_valid = term->as.invoke.receiver.place == SOL_IR_NONE
                            && term->as.invoke.receiver.temporary
                                < mir->temporary_count
                            && mir->temporaries[term->as.invoke.receiver.temporary]
                                .source_expression == callee->as.operation.receiver
                            && mir->temporaries[term->as.invoke.receiver.temporary]
                                .type == receiver->type;
                    } else if (invoke_valid) {
                        SolIrLocalUse use = term->as.invoke.receiver.access
                                == SOL_ACCESS_SHARED
                            ? SOL_IR_LOCAL_USE_SHARED
                            : SOL_IR_LOCAL_USE_EXCLUSIVE;
                        invoke_valid = (term->as.invoke.receiver.access
                                == SOL_ACCESS_SHARED
                                || term->as.invoke.receiver.access
                                    == SOL_ACCESS_EXCLUSIVE)
                            && term->as.invoke.receiver.temporary == SOL_MIR_NONE
                            && term->as.invoke.receiver.place < ir->place_count
                            && receiver->kind == SOL_IR_EXPR_PLACE
                            && receiver->local_use == use
                            && receiver->as.place
                                == term->as.invoke.receiver.place
                            && ir->places[receiver->as.place].root_kind
                                == SOL_IR_PLACE_ROOT_LOCAL
                            && ir->locals[ir->places[receiver->as.place].local]
                                .owner == callable->owner;
                    }
                } else if (invoke_valid && method) {
                    size_t evidence_id = source->as.call.evidence.offset;
                    const SolIrDispatchEvidence *evidence
                        = evidence_id < ir->evidence_count
                        ? &ir->evidence[evidence_id] : NULL;
                    const SolIrCallable *target
                        = term->as.invoke.callable < ir->callable_count
                        ? &ir->callables[term->as.invoke.callable] : NULL;
                    const SolIrExpression *receiver
                        = source->as.call.receiver < ir->expression_count
                        ? &ir->expressions[source->as.call.receiver] : NULL;
                    bool forwarded = evidence != NULL && evidence->forwarded;
                    invoke_valid = evidence != NULL && target != NULL
                        && receiver != NULL
                        && evidence->binding == SOL_IR_NONE
                        && evidence->requirement == source->as.call.callable
                        && term->as.invoke.callable == (forwarded
                            ? evidence->requirement : evidence->method)
                        && (forwarded
                            ? (evidence->implementation == SOL_IR_NONE
                                && evidence->method == SOL_IR_NONE
                                && evidence->type == SOL_IR_NONE
                                && evidence->parameter
                                    < ir->generic_parameter_count
                                && receiver->type < ir->type_count
                                && ir->types[receiver->type].kind
                                    == SOL_IR_TYPE_PARAMETER
                                && ir->types[receiver->type].definition
                                    == evidence->parameter
                                && target->kind
                                    == SOL_IR_CALLABLE_TRAIT_REQUIREMENT
                                && target->owner == evidence->trait)
                            : (evidence->parameter == SOL_IR_NONE
                                && evidence->type == receiver->type
                                && target->kind
                                    == SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION
                                && target->owner
                                    == evidence->implementation))
                        && target->receiver != SOL_IR_NONE
                        && target->receiver_access
                            == source->as.call.receiver_access
                        && target->generic_parameters.count == 0
                        && target->effect_parameters.count == 0
                        && target->capability_source == SOL_IR_NONE
                        && target->result == source->type
                        && target->parameters.count
                            == term->as.invoke.arguments.count
                        && term->as.invoke.callee == SOL_MIR_NONE
                        && term->as.invoke.receiver.formal == SOL_IR_NONE
                        && term->as.invoke.receiver.access
                            == source->as.call.receiver_access
                        && term->as.invoke.receiver.source_expression
                            == source->as.call.receiver;
                    if (invoke_valid
                        && term->as.invoke.receiver.access == SOL_ACCESS_OWNED) {
                        SolMirInstructionId receiver_initializer
                            = mir_temporary_initializer(mir,
                                term->as.invoke.receiver.temporary);
                        invoke_valid = term->as.invoke.receiver.place == SOL_IR_NONE
                            && term->as.invoke.receiver.temporary
                                < mir->temporary_count
                            && receiver_initializer != SOL_MIR_NONE
                            && mir->temporaries[term->as.invoke.receiver.temporary]
                                .source_expression == source->as.call.receiver
                            && mir->temporaries[term->as.invoke.receiver.temporary]
                                .type == receiver->type;
                        for (size_t index = 0; invoke_valid
                            && index < source->as.call.operands.count; ++index) {
                            invoke_valid = mir_expression_events_after(ir, mir,
                                ir->operands[source->as.call.operands.offset
                                    + index].value, receiver_initializer);
                        }
                    } else if (invoke_valid) {
                        SolIrLocalUse use = term->as.invoke.receiver.access
                                == SOL_ACCESS_SHARED
                            ? SOL_IR_LOCAL_USE_SHARED
                            : SOL_IR_LOCAL_USE_EXCLUSIVE;
                        invoke_valid = (term->as.invoke.receiver.access
                                == SOL_ACCESS_SHARED
                                || term->as.invoke.receiver.access
                                    == SOL_ACCESS_EXCLUSIVE)
                            && term->as.invoke.receiver.temporary == SOL_MIR_NONE
                            && term->as.invoke.receiver.place < ir->place_count
                            && receiver->kind == SOL_IR_EXPR_PLACE
                            && receiver->local_use == use
                            && receiver->as.place
                                == term->as.invoke.receiver.place
                            && ir->places[receiver->as.place].root_kind
                                == SOL_IR_PLACE_ROOT_LOCAL
                            && ir->locals[ir->places[receiver->as.place].local]
                                .owner == callable->owner;
                    }
                } else if (invoke_valid && callback) {
                    SolMirInstructionId callee_initializer
                        = mir_temporary_initializer(mir,
                            term->as.invoke.callee);
                    invoke_valid = source->as.call.callable == SOL_IR_NONE
                        && term->as.invoke.callable == SOL_IR_NONE
                        && source->as.call.callee < ir->expression_count
                        && callback_type != NULL
                        && callback_type->kind == SOL_IR_TYPE_FUNCTION
                        && callback_type->result == source->type
                        && callback_type->parameter_count
                            == term->as.invoke.arguments.count
                        && term->as.invoke.callee < mir->temporary_count
                        && callee_initializer != SOL_MIR_NONE
                        && mir->instructions[callee_initializer].block
                            < mir->block_count
                        && mir->temporaries[term->as.invoke.callee]
                            .source_expression == source->as.call.callee
                        && mir->temporaries[term->as.invoke.callee].type
                            == ir->expressions[source->as.call.callee].type;
                    for (size_t operand_index = 0; invoke_valid
                        && operand_index < source->as.call.operands.count;
                        ++operand_index) {
                        SolIrExpressionId operand = ir->operands[
                            source->as.call.operands.offset
                                + operand_index].value;
                        const SolMirInstruction *callee_init
                            = &mir->instructions[callee_initializer];
                        for (size_t instruction = 0; invoke_valid
                            && instruction < mir->instruction_count;
                            ++instruction) {
                            SolIrExpressionId event = mir_instruction_event_source(
                                &mir->instructions[instruction]);
                            if (event == SOL_IR_NONE
                                || !mir_source_reaches_match(ir, operand,
                                    event, 0)) continue;
                            if (mir->instructions[instruction].block
                                >= mir->block_count) {
                                invoke_valid = false;
                                break;
                            }
                            size_t event_order = mir->blocks[
                                mir->instructions[instruction].block].order;
                            size_t callee_order
                                = mir->blocks[callee_init->block].order;
                            invoke_valid = event_order > callee_order
                                || (event_order == callee_order
                                    && instruction > callee_initializer);
                        }
                        for (size_t event_block = 0; invoke_valid
                            && event_block < mir->block_count; ++event_block) {
                            SolIrExpressionId event = mir_terminator_event_source(
                                &mir->blocks[event_block].terminator);
                            if (event != SOL_IR_NONE
                                && mir_source_reaches_match(ir, operand,
                                    event, 0)) {
                                invoke_valid = mir->blocks[event_block].order
                                    >= mir->blocks[callee_init->block].order;
                            }
                        }
                    }
                } else if (invoke_valid) {
                    const SolIrCallable *target
                        = term->as.invoke.callable < ir->callable_count
                        ? &ir->callables[term->as.invoke.callable] : NULL;
                    invoke_valid = source->as.call.kind == SOL_IR_CALL_FUNCTION
                        && term->as.invoke.callee == SOL_MIR_NONE
                        && source->as.call.callable == term->as.invoke.callable
                        && term->as.invoke.callable < ir->callable_count
                        && ir->callables[term->as.invoke.callable].kind
                            == SOL_IR_CALLABLE_FUNCTION
                        && target->generic_parameters.count
                            == term->as.invoke.type_arguments.count
                        && target->effect_parameters.count <= 1
                        && ir->callables[term->as.invoke.callable].receiver
                            == SOL_IR_NONE
                        && ir->callables[term->as.invoke.callable]
                            .capability_source == SOL_IR_NONE
                        && mir_type_matches_call(ir, target,
                            term->as.invoke.type_arguments, target->result,
                            source->type)
                        && term->as.invoke.arguments.count
                            == ir->callables[term->as.invoke.callable]
                                .parameters.count;
                }
                if (invoke_valid && never) {
                    invoke_valid = term->as.invoke.result == SOL_MIR_NONE
                        && term->as.invoke.normal_edge.block == SOL_MIR_NONE
                        && mir_slice_valid(term->as.invoke.normal_edge.arguments,
                            mir->edge_value_count)
                        && term->as.invoke.normal_edge.arguments.count == 0;
                } else if (invoke_valid) {
                    SolMirValueId result = term->as.invoke.result;
                    invoke_valid = result < mir->value_count
                        && mir->values[result].kind
                            == SOL_MIR_VALUE_TERMINATOR
                        && mir->values[result].block == block_id
                        && mir->values[result].definition == block_id
                        && mir->values[result].source_expression
                            == term->as.invoke.source_expression
                        && mir->values[result].type == source->type
                        && (!callback || callback_type->result == source->type)
                        && mir_validate_edge(mir, block_id,
                            &term->as.invoke.normal_edge, result)
                        && term->as.invoke.normal_edge.arguments.count == 1
                        && mir->edge_values[
                            term->as.invoke.normal_edge.arguments.offset]
                            == result
                        && mir->blocks[term->as.invoke.normal_edge.block]
                            .parameters.count == 1
                        && mir->values[mir->parameter_values[
                            mir->blocks[term->as.invoke.normal_edge.block]
                                .parameters.offset]].source_expression
                            == term->as.invoke.source_expression;
                }
                for (size_t left = 0; invoke_valid
                    && left < term->as.invoke.arguments.count; ++left) {
                    const SolMirCallArgument *a = &mir->call_arguments[
                        term->as.invoke.arguments.offset + left];
                    if ((method || capability)
                        && term->as.invoke.receiver.access != SOL_ACCESS_OWNED
                        && a->access != SOL_ACCESS_OWNED) {
                        invoke_valid = term->as.invoke.receiver.place
                                < ir->place_count
                            && a->place < ir->place_count
                            && (!(term->as.invoke.receiver.access
                                    == SOL_ACCESS_EXCLUSIVE
                                    || a->access == SOL_ACCESS_EXCLUSIVE)
                                || !mir_places_overlap(ir,
                                    term->as.invoke.receiver.place, a->place));
                    }
                    if (a->access == SOL_ACCESS_OWNED) continue;
                    invoke_valid = a->place < ir->place_count;
                    for (size_t right = left + 1; invoke_valid
                        && right < term->as.invoke.arguments.count; ++right) {
                        const SolMirCallArgument *b = &mir->call_arguments[
                            term->as.invoke.arguments.offset + right];
                        if (b->access == SOL_ACCESS_OWNED) continue;
                        invoke_valid = b->place < ir->place_count;
                        if (invoke_valid
                            && (a->access == SOL_ACCESS_EXCLUSIVE
                                || b->access == SOL_ACCESS_EXCLUSIVE)
                            && mir_places_overlap(ir, a->place, b->place)) {
                            invoke_valid = false;
                        }
                    }
                }
                for (size_t index = 0; invoke_valid
                    && index < term->as.invoke.arguments.count; ++index) {
                    const SolMirCallArgument *argument = &mir->call_arguments[
                        term->as.invoke.arguments.offset + index];
                    const SolIrOperand *operand = &ir->operands[
                        source->as.call.operands.offset + index];
                    SolAccessMode expected_access = SOL_ACCESS_OWNED;
                    SolIrTypeId expected_type = SOL_IR_NONE;
                    if (callback) {
                        expected_access = ir->accesses[
                            callback_type->parameter_access_offset + index];
                        expected_type = ir->type_ids[
                            callback_type->parameter_offset + index];
                    } else {
                        SolIrLocalId formal = ir->roots[
                            ir->callables[term->as.invoke.callable]
                                .parameters.offset + index];
                        expected_access = ir->locals[formal].access;
                        expected_type = ir->locals[formal].type;
                    }
                    invoke_valid = argument->formal == operand->formal
                        && argument->formal == index
                        && argument->access == operand->access
                        && argument->access == expected_access
                        && argument->source_expression == operand->value
                        && operand->value < ir->expression_count
                        && (callback
                            ? ir->expressions[operand->value].type
                                == expected_type
                            : mir_type_matches_call(ir,
                                &ir->callables[term->as.invoke.callable],
                                term->as.invoke.type_arguments, expected_type,
                                ir->expressions[operand->value].type));
                    if (!invoke_valid) break;
                    if (argument->access == SOL_ACCESS_OWNED) {
                        invoke_valid = argument->place == SOL_IR_NONE
                            && argument->temporary < mir->temporary_count
                            && mir->temporaries[argument->temporary]
                                .source_expression == operand->value
                            && mir->temporaries[argument->temporary].type
                                == ir->expressions[operand->value].type
                            && (!callback || mir_temporary_initializer(mir,
                                    argument->temporary)
                                > mir_temporary_initializer(mir,
                                    term->as.invoke.callee));
                    } else {
                        SolIrLocalUse use = argument->access == SOL_ACCESS_SHARED
                            ? SOL_IR_LOCAL_USE_SHARED
                            : SOL_IR_LOCAL_USE_EXCLUSIVE;
                        const SolIrExpression *actual
                            = &ir->expressions[operand->value];
                        invoke_valid = (argument->access == SOL_ACCESS_SHARED
                                || argument->access == SOL_ACCESS_EXCLUSIVE)
                            && argument->temporary == SOL_MIR_NONE
                            && argument->place < ir->place_count
                            && actual->kind == SOL_IR_EXPR_PLACE
                            && actual->local_use == use
                            && actual->as.place == argument->place
                            && ir->places[argument->place].root_kind
                                == SOL_IR_PLACE_ROOT_LOCAL
                            && ir->locals[ir->places[argument->place].local].owner
                                == callable->owner;
                    }
                }
                if (!invoke_valid) {
                    free(instruction_owners);
                    free(parameter_owners);
                    return mir_error(diagnostics, block->terminator.span,
                        "malformed MIR direct invocation");
                }
                break;
            }
            case SOL_MIR_TERM_RESUME_FAILURE:
                break;
            case SOL_MIR_TERM_UNREACHABLE: {
                size_t obligation
                    = block->terminator.as.unreachable.obligation;
                SolIrStatementId statement
                    = block->terminator.as.unreachable.statement;
                if (obligation
                        >= ir->unreachable_obligation_count
                    || ir->unreachable_obligations[
                        obligation].callable != mir->callable
                    || statement >= ir->statement_count
                    || ir->unreachable_obligations[obligation].statement
                        != statement
                    || ir->statements[statement].kind
                        != SOL_IR_STATEMENT_UNREACHABLE
                    || ir->statements[statement].unreachable_obligations.offset
                        != obligation
                    || block->terminator.span.start
                        != ir->statements[statement].span.start
                    || block->terminator.span.end
                        != ir->statements[statement].span.end
                    || !mir_expression_contains_statement(ir, callable->body,
                        statement, 0)) {
                    free(instruction_owners);
                    free(parameter_owners);
                    return mir_error(diagnostics, block->terminator.span,
                        "malformed MIR unreachable proof edge");
                }
                break;
            }
            case SOL_MIR_TERM_BREAK:
            case SOL_MIR_TERM_CONTINUE: {
                const SolMirTerminator *term = &block->terminator;
                bool is_break = term->kind == SOL_MIR_TERM_BREAK;
                SolMirLoopId loop = term->as.transfer.loop;
                SolIrStatementId statement = term->as.transfer.statement;
                SolIrStatementId parent_statement = SOL_IR_NONE;
                bool found_transfer = mir_find_statement_loop_parent(ir,
                    callable->body, statement, SOL_IR_NONE,
                    &parent_statement, 0);
                bool valid = loop < mir->loop_count
                    && statement < ir->statement_count
                    && ir->statements[statement].kind == (is_break
                        ? SOL_IR_STATEMENT_BREAK : SOL_IR_STATEMENT_CONTINUE)
                    && term->span.start == ir->statements[statement].span.start
                    && term->span.end == ir->statements[statement].span.end
                    && mir_expression_contains_statement(ir, callable->body,
                        statement, 0)
                    && found_transfer
                    && mir->loops[loop].statement == parent_statement
                    && term->as.transfer.edge.arguments.count == 0
                    && mir_validate_edge(mir, block_id,
                        &term->as.transfer.edge, SOL_MIR_NONE)
                    && term->as.transfer.edge.block == (is_break
                        ? mir->loops[loop].exit : mir->loops[loop].header);
                if (!valid) {
                    free(instruction_owners);
                    free(parameter_owners);
                    return mir_error(diagnostics, term->span,
                        "malformed MIR loop transfer");
                }
                break;
            }
            case SOL_MIR_TERM_CHECK_REFINED: {
                const SolMirTerminator *term = &block->terminator;
                const SolIrExpression *source
                    = term->as.check_refined.source_expression
                            < ir->expression_count
                    ? &ir->expressions[
                        term->as.check_refined.source_expression] : NULL;
                SolIrDefinitionId definition
                    = term->as.check_refined.definition;
                SolObligationId obligation
                    = term->as.check_refined.obligation;
                SolMirTemporaryId representation
                    = term->as.check_refined.representation;
                SolMirValueId result = term->as.check_refined.result;
                bool valid = source != NULL
                    && source->kind == SOL_IR_EXPR_CALL
                    && source->as.call.kind
                        == SOL_IR_CALL_DISTINCT_CONSTRUCTOR
                    && source->as.call.definition == definition
                    && source->as.call.operands.count == 1
                    && source->capability_roots.count == 0
                    && source->operation_roots.count == 0
                    && definition < ir->definition_count
                    && ir->definitions[definition].kind
                        == SOL_IR_DEFINITION_REFINED
                    && source->type < ir->type_count
                    && ir->types[source->type].kind == SOL_IR_TYPE_NOMINAL
                    && ir->types[source->type].definition == definition
                    && obligation < ir->obligation_count
                    && ir->obligations[obligation].id == obligation
                    && ir->obligations[obligation].owner_kind
                        == SOL_CONTRACT_OWNER_TYPE
                    && ir->obligations[obligation].owner == definition
                    && ir->obligations[obligation].kind
                        == SOL_CONTRACT_REQUIRES
                    && ir->obligations[obligation].outcome
                        == SOL_CONTRACT_OUTCOME_ALWAYS
                    && !ir->obligations[obligation].result_available
                    && ir->obligations[obligation].snapshots.count == 0
                    && representation < mir->temporary_count
                    && mir->temporaries[representation].source_expression
                        == ir->operands[source->as.call.operands.offset].value
                    && mir->temporaries[representation].type
                        == ir->expressions[ir->operands[
                            source->as.call.operands.offset].value].type
                    && ir->operands[source->as.call.operands.offset].formal == 0
                    && ir->operands[source->as.call.operands.offset].access
                        == SOL_ACCESS_OWNED
                    && result < mir->value_count
                    && mir->values[result].kind == SOL_MIR_VALUE_TERMINATOR
                    && mir->values[result].block == block_id
                    && mir->values[result].definition == block_id
                    && mir->values[result].type == source->type
                    && mir->values[result].source_expression
                        == term->as.check_refined.source_expression
                    && term->span.start == source->span.start
                    && term->span.end == source->span.end
                    && mir_validate_edge(mir, block_id,
                        &term->as.check_refined.normal_edge, result)
                    && term->as.check_refined.normal_edge.arguments.count == 1
                    && mir->edge_values[term->as.check_refined.normal_edge
                        .arguments.offset] == result
                    && term->as.check_refined.failure_edge.arguments.count == 0
                    && mir_validate_edge(mir, block_id,
                        &term->as.check_refined.failure_edge, SOL_MIR_NONE)
                    && mir->blocks[term->as.check_refined.failure_edge.block]
                        .terminator.kind == SOL_MIR_TERM_RESUME_FAILURE;
                if (!valid) {
                    free(instruction_owners);
                    free(parameter_owners);
                    return mir_error(diagnostics, term->span,
                        "malformed MIR checked refinement");
                }
                break;
            }
            case SOL_MIR_TERM_MATCH_FAILURE: {
                SolIrExpressionId source
                    = block->terminator.as.match_failure;
                size_t tests = 0;
                if (source < ir->expression_count) {
                    for (size_t instruction = 0;
                        instruction < mir->instruction_count; ++instruction) {
                        tests += mir->instructions[instruction].kind
                                == SOL_MIR_INST_PATTERN_TEST
                            && mir->instructions[instruction].as.pattern
                                .match_expression == source;
                    }
                }
                if (source >= ir->expression_count
                    || ir->expressions[source].kind != SOL_IR_EXPR_MATCH
                    || tests != ir->expressions[source]
                        .as.match_expr.arms.count
                    || block->terminator.span.start
                        != ir->expressions[source].span.start
                    || block->terminator.span.end
                        != ir->expressions[source].span.end) {
                    free(instruction_owners);
                    free(parameter_owners);
                    return mir_error(diagnostics, block->terminator.span,
                        "malformed MIR match failure");
                }
                break;
            }
            case SOL_MIR_TERM_PROPAGATE: {
                const SolMirTerminator *term = &block->terminator;
                SolIrExpressionId source_id
                    = term->as.propagate.source_expression;
                const SolIrExpression *source = source_id < ir->expression_count
                    ? &ir->expressions[source_id] : NULL;
                SolMirTemporaryId operand = term->as.propagate.operand;
                SolMirValueId value = term->as.propagate.value_result;
                SolMirValueId residual = term->as.propagate.residual_result;
                SolIrTypeKind expected_type
                    = term->as.propagate.kind == SOL_IR_PROPAGATE_OPTION
                    ? SOL_IR_TYPE_OPTION : SOL_IR_TYPE_RESULT;
                bool valid = source != NULL
                    && source->kind == SOL_IR_EXPR_PROPAGATE
                    && source->as.propagate.kind == term->as.propagate.kind
                    && (term->as.propagate.kind == SOL_IR_PROPAGATE_OPTION
                        || term->as.propagate.kind == SOL_IR_PROPAGATE_RESULT)
                    && source->as.propagate.operand < ir->expression_count
                    && ir->expressions[source->as.propagate.operand].type
                        < ir->type_count
                    && ir->types[ir->expressions[
                        source->as.propagate.operand].type].kind == expected_type
                    && operand < mir->temporary_count
                    && mir->temporaries[operand].source_expression
                        == source->as.propagate.operand
                    && mir->temporaries[operand].type
                        == ir->expressions[source->as.propagate.operand].type
                    && value < mir->value_count
                    && mir->values[value].kind == SOL_MIR_VALUE_TERMINATOR
                    && mir->values[value].block == block_id
                    && mir->values[value].definition == block_id
                    && mir->values[value].type == source->type
                    && mir->values[value].source_expression == source_id
                    && residual < mir->value_count
                    && mir->values[residual].kind == SOL_MIR_VALUE_TERMINATOR
                    && mir->values[residual].block == block_id
                    && mir->values[residual].definition == block_id
                    && mir->values[residual].type == callable->result
                    && mir->values[residual].source_expression == source_id
                    && mir_validate_edge(mir, block_id,
                        &term->as.propagate.value_edge, value)
                    && term->as.propagate.value_edge.arguments.count == 1
                    && mir->edge_values[
                        term->as.propagate.value_edge.arguments.offset] == value
                    && mir_validate_edge(mir, block_id,
                        &term->as.propagate.residual_edge, residual)
                    && term->as.propagate.residual_edge.arguments.count == 1
                    && mir->edge_values[
                        term->as.propagate.residual_edge.arguments.offset]
                        == residual
                    && mir->blocks[term->as.propagate.residual_edge.block]
                        .parameters.count == 1
                    && mir->values[mir->parameter_values[
                        mir->blocks[term->as.propagate.residual_edge.block]
                            .parameters.offset]].source_expression == source_id
                    && (mir->contract_epilogue == SOL_MIR_NONE
                        ? (mir->blocks[term->as.propagate.residual_edge.block]
                                .terminator.kind == SOL_MIR_TERM_RETURN
                            && mir->blocks[term->as.propagate.residual_edge.block]
                                .terminator.as.value == mir->parameter_values[
                                    mir->blocks[term->as.propagate.residual_edge.block]
                                        .parameters.offset])
                        : (mir->blocks[term->as.propagate.residual_edge.block]
                                .terminator.kind == SOL_MIR_TERM_GOTO
                            && mir->blocks[term->as.propagate.residual_edge.block]
                                .terminator.as.go_to.block
                                    == mir->contract_epilogue
                            && mir->blocks[term->as.propagate.residual_edge.block]
                                .terminator.as.go_to.arguments.count == 1
                            && mir->edge_values[mir->blocks[
                                term->as.propagate.residual_edge.block]
                                    .terminator.as.go_to.arguments.offset]
                                == mir->parameter_values[mir->blocks[
                                    term->as.propagate.residual_edge.block]
                                        .parameters.offset]))
                    && term->span.start == source->span.start
                    && term->span.end == source->span.end;
                if (!valid) {
                    free(instruction_owners);
                    free(parameter_owners);
                    return mir_error(diagnostics, term->span,
                        "malformed MIR propagation");
                }
                break;
            }
            case SOL_MIR_TERM_CHECK_CONTRACT: {
                const SolMirTerminator *term = &block->terminator;
                SolObligationId id = term->as.check_contract.obligation;
                const SolIrObligation *obligation = id < ir->obligation_count
                    ? &ir->obligations[id] : NULL;
                const SolIrExpression *predicate = obligation != NULL
                        && obligation->predicate < ir->expression_count
                    ? &ir->expressions[obligation->predicate] : NULL;
                bool requires = obligation != NULL
                    && obligation->kind == SOL_CONTRACT_REQUIRES;
                SolMirValueId result = term->as.check_contract.result;
                bool valid = obligation != NULL && predicate != NULL
                    && obligation->id == id
                    && mir_obligation_owned_by_callable(obligation, callable)
                    && term->as.check_contract.phase == obligation->kind
                    && term->as.check_contract.outcome == obligation->outcome
                    && predicate->type < ir->type_count
                    && mir_type_is(ir, predicate->type, SOL_IR_TYPE_BOOL)
                    && term->span.start == predicate->span.start
                    && term->span.end == predicate->span.end
                    && term->as.check_contract.violation_edge.arguments.count == 0
                    && term->as.check_contract.failure_edge.arguments.count == 0
                    && mir_validate_edge(mir, block_id,
                        &term->as.check_contract.violation_edge, SOL_MIR_NONE)
                    && mir_validate_edge(mir, block_id,
                        &term->as.check_contract.failure_edge, SOL_MIR_NONE)
                    && mir->blocks[term->as.check_contract.violation_edge.block]
                        .terminator.kind == SOL_MIR_TERM_CONTRACT_VIOLATION
                    && mir->blocks[term->as.check_contract.violation_edge.block]
                        .terminator.as.contract_violation == id
                    && mir->blocks[term->as.check_contract.failure_edge.block]
                        .terminator.kind == SOL_MIR_TERM_RESUME_FAILURE;
                if (valid && requires) {
                    valid = obligation->outcome == SOL_CONTRACT_OUTCOME_ALWAYS
                        && !obligation->result_available
                        && result == SOL_MIR_NONE
                        && term->as.check_contract.satisfied_edge.arguments.count
                            == 0
                        && mir_validate_edge(mir, block_id,
                            &term->as.check_contract.satisfied_edge,
                            SOL_MIR_NONE);
                } else if (valid) {
                    valid = obligation->kind == SOL_CONTRACT_ENSURES
                        && result < mir->value_count
                        && mir_value_available(mir, result, block_id,
                            block->instructions.offset
                                + block->instructions.count)
                        && mir->values[result].type == callable->result
                        && term->as.check_contract.satisfied_edge.arguments.count
                            == 1
                        && mir->edge_values[term->as.check_contract
                            .satisfied_edge.arguments.offset] == result
                        && mir_validate_edge(mir, block_id,
                            &term->as.check_contract.satisfied_edge,
                            SOL_MIR_NONE);
                }
                if (!valid) {
                    free(instruction_owners);
                    free(parameter_owners);
                    return mir_error(diagnostics, term->span,
                        "malformed MIR callable contract check");
                }
                break;
            }
            case SOL_MIR_TERM_CONTRACT_VIOLATION: {
                SolObligationId id = block->terminator.as.contract_violation;
                if (id >= ir->obligation_count
                    || ir->obligations[id].id != id
                    || !mir_obligation_owned_by_callable(&ir->obligations[id],
                        callable)
                    || block->terminator.span.start
                        != ir->expressions[ir->obligations[id].predicate].span.start
                    || block->terminator.span.end
                        != ir->expressions[ir->obligations[id].predicate].span.end) {
                    free(instruction_owners);
                    free(parameter_owners);
                    return mir_error(diagnostics, block->terminator.span,
                        "malformed MIR contract violation");
                }
                break;
            }
            case SOL_MIR_TERM_INVALID: break;
        }
    }
    bool arenas_owned = true;
    for (size_t index = 0; arenas_owned && index < mir->instruction_count; ++index) {
        arenas_owned = instruction_owners[index] == 1;
    }
    for (size_t index = 0; arenas_owned && index < mir->parameter_value_count; ++index) {
        arenas_owned = parameter_owners[index] == 1;
    }
    free(instruction_owners);
    free(parameter_owners);
    if (!arenas_owned) {
        return mir_error(diagnostics, callable->span,
            "MIR block arenas are shared or orphaned");
    }
    for (size_t id = 0; id < mir->instruction_count; ++id) {
        const SolMirInstruction *instruction = &mir->instructions[id];
        if (instruction->block >= mir->block_count
            || id < mir->blocks[instruction->block].instructions.offset
            || id - mir->blocks[instruction->block].instructions.offset
                >= mir->blocks[instruction->block].instructions.count
            || (int)instruction->kind < 0
            || instruction->kind > SOL_MIR_INST_CAPTURE_SNAPSHOT
            || !mir_span_valid(ir, instruction->span)) {
            return mir_error(diagnostics, instruction->span,
                "malformed MIR instruction");
        }
        bool result_kind = instruction->kind <= SOL_MIR_INST_CONST_UNIT
            || instruction->kind == SOL_MIR_INST_LOAD_COPY
            || instruction->kind == SOL_MIR_INST_LOAD_MOVE
            || instruction->kind == SOL_MIR_INST_LOAD_UPDATE
            || instruction->kind == SOL_MIR_INST_UNARY
            || instruction->kind == SOL_MIR_INST_BINARY
            || instruction->kind == SOL_MIR_INST_COMPOUND_UPDATE
            || instruction->kind == SOL_MIR_INST_EXPRESSION_RESULT
            || instruction->kind == SOL_MIR_INST_PATTERN_TEST
            || instruction->kind == SOL_MIR_INST_PATTERN_VALUE
            || instruction->kind == SOL_MIR_INST_CONSTRUCT;
        if (result_kind && (instruction->type >= ir->type_count
            || (instruction->source_expression != SOL_IR_NONE
                && (instruction->source_expression >= ir->expression_count
                    || ir->expressions[instruction->source_expression].type
                        != instruction->type)))) {
            return mir_error(diagnostics, instruction->span,
                "MIR instruction result type is inconsistent with source IR");
        }
        const SolIrExpression *source = instruction->source_expression
                < ir->expression_count
            ? &ir->expressions[instruction->source_expression] : NULL;
        if ((instruction->kind == SOL_MIR_INST_CONST_INT64
                && (source == NULL || source->kind != SOL_IR_EXPR_INTEGER
                    || source->as.integer != instruction->as.integer))
            || (instruction->kind == SOL_MIR_INST_CONST_BOOL
                && (source == NULL || source->kind != SOL_IR_EXPR_BOOL
                    || source->as.boolean != instruction->as.boolean))
            || (instruction->kind == SOL_MIR_INST_CONST_TEXT
                && (source == NULL || source->kind != SOL_IR_EXPR_STRING))
            || (instruction->kind == SOL_MIR_INST_CONST_UNIT
                && source != NULL && source->kind != SOL_IR_EXPR_UNIT)) {
            return mir_error(diagnostics, instruction->span,
                "MIR literal is inconsistent with source IR");
        }
        if ((instruction->kind == SOL_MIR_INST_CONST_INT64
                && !mir_type_is(ir, instruction->type, SOL_IR_TYPE_INT64))
            || (instruction->kind == SOL_MIR_INST_CONST_BOOL
                && !mir_type_is(ir, instruction->type, SOL_IR_TYPE_BOOL))
            || (instruction->kind == SOL_MIR_INST_CONST_TEXT
                && !mir_type_is(ir, instruction->type, SOL_IR_TYPE_TEXT))
            || (instruction->kind == SOL_MIR_INST_CONST_UNIT
                && !mir_type_is(ir, instruction->type, SOL_IR_TYPE_UNIT))) {
            return mir_error(diagnostics, instruction->span,
                "MIR literal has the wrong primitive type");
        }
        if (result_kind) {
            if (instruction->result >= mir->value_count
                || mir->values[instruction->result].kind
                    != SOL_MIR_VALUE_INSTRUCTION
                || mir->values[instruction->result].definition != id
                || mir->values[instruction->result].block != instruction->block
                || mir->values[instruction->result].type != instruction->type
                || mir->values[instruction->result].source_expression
                    != instruction->source_expression) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR instruction result");
            }
        } else if (instruction->result != SOL_MIR_NONE
            || instruction->type != SOL_IR_NONE) {
            return mir_error(diagnostics, instruction->span,
                "effect-only MIR instruction has a result");
        }
        if (instruction->kind == SOL_MIR_INST_PARAMETER_LIVE
            || instruction->kind == SOL_MIR_INST_STORAGE_LIVE
            || instruction->kind == SOL_MIR_INST_DROP_IF_INITIALIZED
            || instruction->kind == SOL_MIR_INST_STORAGE_DEAD) {
            if (instruction->as.local >= ir->local_count
                || ir->locals[instruction->as.local].owner != callable->owner) {
                return mir_error(diagnostics, instruction->span,
                    "MIR instruction references a foreign local");
            }
        } else if (instruction->kind == SOL_MIR_INST_LOAD_COPY
            || instruction->kind == SOL_MIR_INST_LOAD_MOVE) {
            SolIrTypeId place_type = SOL_IR_NONE;
            if (!mir_place_valid(ir, callable, instruction->as.place, false,
                    &place_type)
                || source == NULL || source->kind != SOL_IR_EXPR_PLACE
                || source->as.place != instruction->as.place.source_place
                || source->local_use != (instruction->kind
                    == SOL_MIR_INST_LOAD_COPY ? SOL_IR_LOCAL_USE_COPY
                    : SOL_IR_LOCAL_USE_MOVE)
                || instruction->type != place_type) {
                return mir_error(diagnostics, instruction->span,
                    "MIR local load is inconsistent with source ownership");
            }
        } else if (instruction->kind == SOL_MIR_INST_LOAD_UPDATE) {
            SolIrStatementId statement_id
                = instruction->as.update_load.statement;
            const SolIrStatement *statement = statement_id < ir->statement_count
                ? &ir->statements[statement_id] : NULL;
            SolIrTypeId place_type = SOL_IR_NONE;
            bool place_valid = mir_place_valid(ir, callable,
                instruction->as.update_load.place, false, &place_type);
            bool terminating_rhs = statement != NULL
                && statement->expression < ir->expression_count
                && mir_type_is(ir, ir->expressions[statement->expression].type,
                    SOL_IR_TYPE_NEVER);
            SolMirInstructionId initializer = SOL_MIR_NONE;
            size_t initializer_count = 0;
            for (size_t candidate = 0; candidate < mir->instruction_count;
                ++candidate) {
                if (mir->instructions[candidate].kind
                        == SOL_MIR_INST_TEMPORARY_INIT
                    && mir->instructions[candidate].as.temporary_init.value
                        == instruction->result) {
                    initializer = candidate;
                    ++initializer_count;
                }
            }
            if (statement == NULL
                || statement->kind != SOL_IR_STATEMENT_ASSIGNMENT
                || statement->operator_kind == SOL_TOKEN_EQUAL
                || statement->target != instruction->source_expression
                || source == NULL || source->kind != SOL_IR_EXPR_PLACE
                || source->as.place
                    != instruction->as.update_load.place.source_place
                || (source->local_use != SOL_IR_LOCAL_USE_UPDATE
                    && !(terminating_rhs
                        && source->local_use == SOL_IR_LOCAL_USE_NONE))
                || !place_valid
                || instruction->type != place_type
                || initializer_count != 1
                || !mir_expression_events_after(ir, mir,
                    statement->expression, initializer)) {
                return mir_error(diagnostics, instruction->span,
                    "MIR update load is inconsistent with source assignment");
            }
        } else if (instruction->kind == SOL_MIR_INST_STORE) {
            SolIrTypeId place_type = SOL_IR_NONE;
            if (!mir_place_valid(ir, callable, instruction->as.store.place,
                    true, &place_type)
                || instruction->as.store.value >= mir->value_count
                || !mir_value_available(mir, instruction->as.store.value,
                    instruction->block, id)
                || mir->values[instruction->as.store.value].source_expression
                    != instruction->source_expression
                || mir->values[instruction->as.store.value].type
                    != place_type
                || (instruction->as.store.place.source_place != SOL_IR_NONE
                    && (id == 0
                        || mir->instructions[id - 1].block
                            != instruction->block
                        || mir->instructions[id - 1].kind
                            != SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED
                        || mir->instructions[id - 1].as.place.local
                            != instruction->as.store.place.local
                        || mir->instructions[id - 1].as.place.source_place
                            != instruction->as.store.place.source_place))) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR store");
            }
        } else if (instruction->kind
            == SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED) {
            SolIrTypeId place_type = SOL_IR_NONE;
            if (!mir_place_valid(ir, callable, instruction->as.place, false,
                    &place_type)
                || source == NULL || source->kind != SOL_IR_EXPR_PLACE
                || source->as.place != instruction->as.place.source_place) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR projected replacement drop");
            }
        } else if (instruction->kind == SOL_MIR_INST_UNARY) {
            if (!mir_value_available(mir, instruction->as.unary.operand,
                    instruction->block, id)
                || source == NULL || source->kind != SOL_IR_EXPR_UNARY
                || source->as.unary.operator_kind
                    != instruction->as.unary.operator_kind
                || mir->values[instruction->as.unary.operand].type
                    != ir->expressions[source->as.unary.operand].type) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR unary operand");
            }
        } else if (instruction->kind == SOL_MIR_INST_BINARY) {
            if (!mir_value_available(mir, instruction->as.binary.left,
                    instruction->block, id)
                || !mir_value_available(mir, instruction->as.binary.right,
                    instruction->block, id)
                || source == NULL || source->kind != SOL_IR_EXPR_BINARY
                || source->as.binary.operator_kind
                    != instruction->as.binary.operator_kind
                || mir->values[instruction->as.binary.left].type
                    != ir->expressions[source->as.binary.left].type
                || mir->values[instruction->as.binary.right].type
                    != ir->expressions[source->as.binary.right].type) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR binary operand");
            }
        } else if (instruction->kind == SOL_MIR_INST_COMPOUND_UPDATE) {
            SolIrStatementId statement_id
                = instruction->as.compound_update.statement;
            const SolIrStatement *statement = statement_id < ir->statement_count
                ? &ir->statements[statement_id] : NULL;
            SolMirTemporaryId previous
                = instruction->as.compound_update.previous;
            SolMirInstructionId initializer
                = mir_temporary_initializer(mir, previous);
            SolMirInstructionId update_load = SOL_MIR_NONE;
            for (size_t candidate = 0; candidate < mir->instruction_count;
                ++candidate) {
                if (mir->instructions[candidate].kind == SOL_MIR_INST_LOAD_UPDATE
                    && mir->instructions[candidate].as.update_load.statement
                        == statement_id) update_load = candidate;
            }
            SolIrTypeId place_type = SOL_IR_NONE;
            bool compound_operator = instruction->as.compound_update.operator_kind
                    == SOL_TOKEN_PLUS_EQUAL
                || instruction->as.compound_update.operator_kind
                    == SOL_TOKEN_MINUS_EQUAL
                || instruction->as.compound_update.operator_kind
                    == SOL_TOKEN_STAR_EQUAL
                || instruction->as.compound_update.operator_kind
                    == SOL_TOKEN_SLASH_EQUAL
                || instruction->as.compound_update.operator_kind
                    == SOL_TOKEN_PERCENT_EQUAL;
            if (statement == NULL
                || statement->kind != SOL_IR_STATEMENT_ASSIGNMENT
                || statement->operator_kind
                    != instruction->as.compound_update.operator_kind
                || !compound_operator
                || statement->target >= ir->expression_count
                || statement->expression != instruction->source_expression
                || ir->expressions[statement->target].kind != SOL_IR_EXPR_PLACE
                || ir->expressions[statement->target].as.place
                    != instruction->as.compound_update.place.source_place
                || !mir_place_valid(ir, callable,
                    instruction->as.compound_update.place, false, &place_type)
                || !mir_type_is(ir, place_type, SOL_IR_TYPE_INT64)
                || instruction->type != place_type
                || previous >= mir->temporary_count
                || mir->temporaries[previous].source_expression
                    != statement->target
                || mir->temporaries[previous].type != place_type
                || initializer == SOL_MIR_NONE
                || update_load == SOL_MIR_NONE
                || mir->instructions[initializer].as.temporary_init.value
                    != mir->instructions[update_load].result
                || !mir_expression_events_after(ir, mir,
                    statement->expression, initializer)
                || !mir_value_available(mir,
                    instruction->as.compound_update.right,
                    instruction->block, id)
                || mir->values[instruction->as.compound_update.right].type
                    != place_type
                || mir->values[instruction->as.compound_update.right]
                    .source_expression != statement->expression
                || instruction->span.start != statement->span.start
                || instruction->span.end != statement->span.end) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR compound update");
            }
        } else if (instruction->kind == SOL_MIR_INST_TEMPORARY_INIT) {
            SolMirTemporaryId temporary
                = instruction->as.temporary_init.temporary;
            if (source == NULL || temporary >= mir->temporary_count
                || !mir_value_available(mir,
                    instruction->as.temporary_init.value,
                    instruction->block, id)
                || mir->values[instruction->as.temporary_init.value].type
                    != mir->temporaries[temporary].type
                || mir->values[instruction->as.temporary_init.value]
                    .source_expression != instruction->source_expression
                || mir->temporaries[temporary].type != source->type
                || mir->temporaries[temporary].source_expression
                    != instruction->source_expression
                || instruction->span.start != source->span.start
                || instruction->span.end != source->span.end
                || mir->temporaries[temporary].span.start != source->span.start
                || mir->temporaries[temporary].span.end != source->span.end) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR temporary initialization");
            }
        } else if (instruction->kind == SOL_MIR_INST_TEMPORARY_DROP) {
            if (instruction->source_expression != SOL_IR_NONE
                || instruction->as.temporary_drop.temporary
                    >= mir->temporary_count
                || instruction->as.temporary_drop.preserve_depth
                    >= mir->temporary_count) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR temporary drop");
            }
        } else if (instruction->kind == SOL_MIR_INST_EXPRESSION_RESULT) {
            SolIrExpressionId operand_source = source != NULL
                    && source->kind == SOL_IR_EXPR_BLOCK
                ? mir_block_result_source(ir, source)
                : source != NULL && source->kind == SOL_IR_EXPR_HANDLE
                    ? source->as.handler.body : SOL_IR_NONE;
            if (source == NULL || (source->kind != SOL_IR_EXPR_BLOCK
                    && source->kind != SOL_IR_EXPR_HANDLE)
                || !mir_value_available(mir, instruction->as.operand,
                    instruction->block, id)
                || mir->values[instruction->as.operand].type
                    != instruction->type
                || mir->values[instruction->as.operand].source_expression
                    != operand_source
                || instruction->span.start != source->span.start
                || instruction->span.end != source->span.end) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR block expression result");
            }
        } else if (instruction->kind == SOL_MIR_INST_PATTERN_TEST
            || instruction->kind == SOL_MIR_INST_PATTERN_VALUE
            || instruction->kind == SOL_MIR_INST_MATCH_ARM) {
            SolIrExpressionId match_id
                = instruction->as.pattern.match_expression;
            SolIrArmId arm_id = instruction->as.pattern.arm;
            SolIrPatternId pattern_id = instruction->as.pattern.pattern;
            SolMirTemporaryId scrutinee
                = instruction->as.pattern.scrutinee;
            const SolIrExpression *match = match_id < ir->expression_count
                ? &ir->expressions[match_id] : NULL;
            const SolIrArm *arm = arm_id < ir->arm_count
                ? &ir->arms[arm_id] : NULL;
            const SolIrPattern *pattern = pattern_id < ir->pattern_count
                ? &ir->patterns[pattern_id] : NULL;
            bool test = instruction->kind == SOL_MIR_INST_PATTERN_TEST;
            bool marker = instruction->kind == SOL_MIR_INST_MATCH_ARM;
            bool valid = (marker
                    ? instruction->source_expression
                        == (arm == NULL ? SOL_IR_NONE : arm->body)
                    : instruction->source_expression == SOL_IR_NONE)
                && match != NULL && match->kind == SOL_IR_EXPR_MATCH
                && arm != NULL && pattern != NULL
                && instruction->as.pattern.arm_ordinal
                    < match->as.match_expr.arms.count
                && ir->arm_ids[match->as.match_expr.arms.offset
                    + instruction->as.pattern.arm_ordinal] == arm_id
                && mir_match_has_arm(ir, match, arm_id)
                && mir_pattern_contains(ir, arm->pattern, pattern_id, 0)
                && scrutinee < mir->temporary_count
                && mir->temporaries[scrutinee].source_expression
                    == match->as.match_expr.scrutinee
                && mir->temporaries[scrutinee].type
                    == ir->expressions[match->as.match_expr.scrutinee].type;
            if (test) {
                valid = valid && pattern_id == arm->pattern
                    && mir_type_is(ir, instruction->type, SOL_IR_TYPE_BOOL)
                    && instruction->span.start == arm->span.start
                    && instruction->span.end == arm->span.end
                    && mir->blocks[instruction->block].terminator.kind
                        == SOL_MIR_TERM_BRANCH
                    && mir->blocks[instruction->block].terminator
                        .as.branch.condition == instruction->result;
                if (valid) {
                    const SolMirTerminator *branch
                        = &mir->blocks[instruction->block].terminator;
                    const SolMirBlock *selected
                        = &mir->blocks[branch->as.branch.true_edge.block];
                    valid = selected->instructions.count != 0;
                    if (valid) {
                        const SolMirInstruction *arm_marker
                            = &mir->instructions[selected->instructions.offset];
                        valid = arm_marker->kind == SOL_MIR_INST_MATCH_ARM
                            && arm_marker->as.pattern.match_expression == match_id
                            && arm_marker->as.pattern.arm_ordinal
                                == instruction->as.pattern.arm_ordinal
                            && arm_marker->as.pattern.arm == arm_id;
                    }
                    const SolMirBlock *otherwise
                        = &mir->blocks[branch->as.branch.false_edge.block];
                    if (valid && instruction->as.pattern.arm_ordinal + 1
                        < match->as.match_expr.arms.count) {
                        valid = otherwise->instructions.count != 0;
                        if (valid) {
                            const SolMirInstruction *next_test
                                = &mir->instructions[
                                    otherwise->instructions.offset];
                            valid = next_test->kind == SOL_MIR_INST_PATTERN_TEST
                                && next_test->as.pattern.match_expression
                                    == match_id
                                && next_test->as.pattern.arm_ordinal
                                    == instruction->as.pattern.arm_ordinal + 1;
                        }
                    } else if (valid) {
                        valid = otherwise->terminator.kind
                                == SOL_MIR_TERM_MATCH_FAILURE
                            && otherwise->terminator.as.match_failure == match_id;
                    }
                }
            } else if (marker) {
                valid = valid && pattern_id == arm->pattern
                    && instruction->span.start == arm->span.start
                    && instruction->span.end == arm->span.end;
            } else {
                valid = valid && pattern->kind == SOL_IR_PATTERN_BINDING
                    && pattern->binding < ir->local_count
                    && instruction->type == pattern->type
                    && ir->locals[pattern->binding].type == pattern->type
                    && instruction->span.start == pattern->span.start
                    && instruction->span.end == pattern->span.end
                    && id + 2 < mir->instruction_count
                    && mir->instructions[id + 1].block == instruction->block
                    && mir->instructions[id + 1].kind
                        == SOL_MIR_INST_STORAGE_LIVE
                    && mir->instructions[id + 1].as.local == pattern->binding
                    && mir->instructions[id + 2].block == instruction->block
                    && mir->instructions[id + 2].kind == SOL_MIR_INST_STORE
                    && mir->instructions[id + 2].as.store.place.local
                        == pattern->binding
                    && mir->instructions[id + 2].as.store.place.source_place
                        == SOL_IR_NONE
                    && mir->instructions[id + 2].as.store.value
                        == instruction->result;
            }
            if (!valid) return mir_error(diagnostics, instruction->span,
                "malformed MIR pattern operation");
        } else if (instruction->kind == SOL_MIR_INST_CONSTRUCT) {
            SolMirSlice operands = instruction->as.construct.operands;
            SolIrSlice source_operands = {0};
            SolMirConstructKind expected_kind = instruction->as.construct.kind;
            SolIrDefinitionId expected_definition = SOL_IR_NONE;
            SolIrVariantId expected_variant = SOL_IR_NONE;
            bool shape = source != NULL
                && instruction->span.start == source->span.start
                && instruction->span.end == source->span.end
                && mir_ir_slice_equal(
                    instruction->as.construct.capability_roots,
                    source->capability_roots)
                && mir_ir_slice_equal(
                    instruction->as.construct.operation_roots,
                    source->operation_roots)
                && mir_slice_valid(operands, mir->construct_operand_count);
            if (shape && source->kind == SOL_IR_EXPR_RECORD) {
                expected_definition = source->as.record.definition;
                source_operands = source->as.record.fields;
                shape = expected_definition < ir->definition_count
                    && (ir->definitions[expected_definition].kind
                            == SOL_IR_DEFINITION_RECORD
                        || ir->definitions[expected_definition].kind
                            == SOL_IR_DEFINITION_CAPABILITY);
                if (shape) expected_kind
                    = ir->definitions[expected_definition].kind
                            == SOL_IR_DEFINITION_CAPABILITY
                        ? SOL_MIR_CONSTRUCT_CAPABILITY
                        : SOL_MIR_CONSTRUCT_RECORD;
            } else if (shape && source->kind == SOL_IR_EXPR_TUPLE) {
                expected_kind = SOL_MIR_CONSTRUCT_TUPLE;
                source_operands = source->as.tuple.operands;
            } else if (shape && source->kind == SOL_IR_EXPR_VARIANT) {
                expected_kind = SOL_MIR_CONSTRUCT_ENUM;
                expected_variant = source->as.variant.variant;
                shape = expected_variant < ir->variant_count;
                if (shape) expected_definition
                    = ir->variants[expected_variant].owner;
            } else if (shape && source->kind == SOL_IR_EXPR_CALL) {
                source_operands = source->as.call.operands;
                switch (source->as.call.kind) {
                    case SOL_IR_CALL_BUILTIN_NONE:
                        expected_kind = SOL_MIR_CONSTRUCT_OPTION_NONE;
                        break;
                    case SOL_IR_CALL_BUILTIN_SOME:
                        expected_kind = SOL_MIR_CONSTRUCT_OPTION_SOME;
                        break;
                    case SOL_IR_CALL_BUILTIN_OK:
                        expected_kind = SOL_MIR_CONSTRUCT_RESULT_OK;
                        break;
                    case SOL_IR_CALL_BUILTIN_ERR:
                        expected_kind = SOL_MIR_CONSTRUCT_RESULT_ERR;
                        break;
                    case SOL_IR_CALL_ENUM_CONSTRUCTOR:
                        expected_kind = SOL_MIR_CONSTRUCT_ENUM;
                        expected_variant = source->as.call.variant;
                        shape = expected_variant < ir->variant_count;
                        if (shape) expected_definition
                            = ir->variants[expected_variant].owner;
                        break;
                    case SOL_IR_CALL_DISTINCT_CONSTRUCTOR:
                        expected_kind = SOL_MIR_CONSTRUCT_DISTINCT;
                        expected_definition = source->as.call.definition;
                        shape = expected_definition < ir->definition_count
                            && ir->definitions[expected_definition].kind
                                == SOL_IR_DEFINITION_DISTINCT;
                        break;
                    default:
                        shape = false;
                        break;
                }
            } else {
                shape = false;
            }
            shape = shape && instruction->as.construct.kind == expected_kind
                && instruction->as.construct.definition == expected_definition
                && instruction->as.construct.variant == expected_variant
                && operands.count == source_operands.count;
            for (size_t index = 0; shape && index < operands.count; ++index) {
                const SolMirConstructOperand *operand
                    = &mir->construct_operands[operands.offset + index];
                const SolIrOperand *source_operand
                    = &ir->operands[source_operands.offset + index];
                shape = source_operand->access == SOL_ACCESS_OWNED
                    && operand->formal == source_operand->formal
                    && operand->source_expression == source_operand->value
                    && operand->temporary < mir->temporary_count
                    && mir->temporaries[operand->temporary].source_expression
                        == source_operand->value
                    && mir->temporaries[operand->temporary].type
                        == ir->expressions[source_operand->value].type;
            }
            if (!shape) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR semantic construction");
            }
        } else if (instruction->kind == SOL_MIR_INST_CAPTURE_SNAPSHOT) {
            SolIrSnapshotId snapshot_id = instruction->as.snapshot;
            const SolIrSnapshot *snapshot = snapshot_id < ir->snapshot_count
                ? &ir->snapshots[snapshot_id] : NULL;
            const SolIrExpression *operand = snapshot != NULL
                    && snapshot->operand < ir->expression_count
                ? &ir->expressions[snapshot->operand] : NULL;
            const SolIrPlace *place = operand != NULL
                    && operand->kind == SOL_IR_EXPR_PLACE
                    && operand->as.place < ir->place_count
                ? &ir->places[operand->as.place] : NULL;
            if (snapshot == NULL || snapshot->id != snapshot_id
                || snapshot->obligation >= ir->obligation_count
                || !mir_obligation_owned_by_callable(
                    &ir->obligations[snapshot->obligation], callable)
                || snapshot->read != instruction->source_expression
                || snapshot->read >= ir->expression_count
                || snapshot->operand >= ir->expression_count
                || snapshot->type >= ir->type_count
                || ir->expressions[snapshot->operand].type != snapshot->type
                || operand == NULL
                || (operand->local_use != SOL_IR_LOCAL_USE_NONE
                    && operand->local_use != SOL_IR_LOCAL_USE_COPY
                    && operand->local_use != SOL_IR_LOCAL_USE_SHARED)
                || place == NULL || place->root_kind != SOL_IR_PLACE_ROOT_LOCAL
                || place->projections.count != 0
                || (!mir_type_is(ir, snapshot->type, SOL_IR_TYPE_INT64)
                    && !mir_type_is(ir, snapshot->type, SOL_IR_TYPE_BOOL)
                    && !mir_type_is(ir, snapshot->type, SOL_IR_TYPE_UNIT))
                || instruction->span.start
                    != ir->expressions[snapshot->read].span.start
                || instruction->span.end
                    != ir->expressions[snapshot->read].span.end) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR callable snapshot capture");
            }
        } else if (instruction->kind == SOL_MIR_INST_REGION_ENTER
            || instruction->kind == SOL_MIR_INST_REGION_EXIT) {
            if (instruction->source_expression != SOL_IR_NONE
                || instruction->as.region >= ir->statement_count
                || ir->statements[instruction->as.region].kind
                    != SOL_IR_STATEMENT_REGION
                || !mir_expression_contains_statement(ir, callable->body,
                    instruction->as.region, 0)
                || (instruction->kind == SOL_MIR_INST_REGION_ENTER
                    && (instruction->span.start
                            != ir->statements[instruction->as.region].span.start
                        || instruction->span.end
                            != ir->statements[instruction->as.region].span.end))) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR lexical region marker");
            }
        } else if (instruction->kind == SOL_MIR_INST_HANDLER_ENTER
            || instruction->kind == SOL_MIR_INST_HANDLER_EXIT) {
            const SolIrExpression *handler = instruction->source_expression
                    < ir->expression_count
                ? &ir->expressions[instruction->source_expression] : NULL;
            const SolIrExpression *authority = handler != NULL
                    && handler->kind == SOL_IR_EXPR_HANDLE
                    && handler->as.handler.authority < ir->expression_count
                ? &ir->expressions[handler->as.handler.authority] : NULL;
            const SolIrExpression *provider = handler != NULL
                    && handler->kind == SOL_IR_EXPR_HANDLE
                    && handler->as.handler.provider < ir->expression_count
                ? &ir->expressions[handler->as.handler.provider] : NULL;
            bool valid = handler != NULL && authority != NULL && provider != NULL
                && instruction->type == SOL_IR_NONE
                && authority->kind == SOL_IR_EXPR_PLACE
                && authority->local_use == SOL_IR_LOCAL_USE_SHARED
                && authority->as.place < ir->place_count
                && ir->places[authority->as.place].root_kind
                    == SOL_IR_PLACE_ROOT_LOCAL
                && ir->places[authority->as.place].projections.count == 0
                && provider->kind == SOL_IR_EXPR_PLACE
                && provider->local_use == SOL_IR_LOCAL_USE_SHARED
                && provider->as.place < ir->place_count
                && ir->places[provider->as.place].root_kind
                    == SOL_IR_PLACE_ROOT_LOCAL
                && ir->places[provider->as.place].projections.count == 0
                && instruction->span.start == handler->span.start
                && instruction->span.end == handler->span.end
                && mir_source_reaches_match(ir, callable->body,
                    instruction->source_expression, 0);
            if (!valid) return mir_error(diagnostics, instruction->span,
                "malformed MIR handler scope marker");
        }
    }
    for (size_t match_id = 0; match_id < ir->expression_count; ++match_id) {
        const SolIrExpression *match = &ir->expressions[match_id];
        if (match->kind != SOL_IR_EXPR_MATCH) continue;
        size_t expected = 0;
        size_t failures = 0;
        bool represented = mir_source_reaches_match(ir, callable->body,
            match_id, 0);
        for (size_t block = 0; block < mir->block_count; ++block) {
            failures += mir->blocks[block].terminator.kind
                    == SOL_MIR_TERM_MATCH_FAILURE
                && mir->blocks[block].terminator.as.match_failure == match_id;
        }
        for (size_t id = 0; id < mir->instruction_count; ++id) {
            const SolMirInstruction *instruction = &mir->instructions[id];
            if (instruction->kind != SOL_MIR_INST_PATTERN_TEST
                || instruction->as.pattern.match_expression != match_id) {
                continue;
            }
            if (expected >= match->as.match_expr.arms.count
                || instruction->as.pattern.arm_ordinal != expected) {
                return mir_error(diagnostics, instruction->span,
                    "MIR match arms are missing or reordered");
            }
            ++expected;
        }
        if ((represented && (expected != match->as.match_expr.arms.count
                || failures != 1))
            || (!represented && (expected != 0 || failures != 0))) {
            return mir_error(diagnostics, match->span,
                "MIR match arms are missing or reordered");
        }
    }
    for (size_t source = 0; source < ir->expression_count; ++source) {
        if (ir->expressions[source].kind == SOL_IR_EXPR_HANDLE) {
            size_t enters = 0;
            for (size_t id = 0; id < mir->instruction_count; ++id) {
                enters += mir->instructions[id].kind
                        == SOL_MIR_INST_HANDLER_ENTER
                    && mir->instructions[id].source_expression == source;
            }
            bool required = mir_source_reaches_match(ir, callable->body,
                source, 0);
            if (enters != (required ? 1u : 0u)) {
                return mir_error(diagnostics, ir->expressions[source].span,
                    "MIR handler scope is missing, duplicated, or foreign");
            }
        }
        if (ir->expressions[source].kind != SOL_IR_EXPR_PROPAGATE) continue;
        size_t count = 0;
        for (size_t block = 0; block < mir->block_count; ++block) {
            count += mir->blocks[block].terminator.kind
                    == SOL_MIR_TERM_PROPAGATE
                && mir->blocks[block].terminator.as.propagate.source_expression
                    == source;
        }
        bool required = mir_source_reaches_match(ir, callable->body, source, 0);
        if (count != (required ? 1u : 0u)) {
            return mir_error(diagnostics, ir->expressions[source].span,
                "MIR propagation is missing, duplicated, or foreign");
        }
    }
    for (size_t id = 0; id < mir->value_count; ++id) {
        const SolMirValue *value = &mir->values[id];
        bool propagation_residual = false;
        for (size_t block = 0; block < mir->block_count; ++block) {
            const SolMirTerminator *term = &mir->blocks[block].terminator;
            if (term->kind != SOL_MIR_TERM_PROPAGATE) continue;
            propagation_residual = propagation_residual
                || (id == term->as.propagate.residual_result
                    && value->source_expression
                        == term->as.propagate.source_expression);
            SolMirBlockId residual_block
                = term->as.propagate.residual_edge.block;
            if (residual_block < mir->block_count
                && mir->blocks[residual_block].parameters.count == 1) {
                propagation_residual = propagation_residual
                    || (id == mir->parameter_values[
                            mir->blocks[residual_block].parameters.offset]
                        && value->source_expression
                            == term->as.propagate.source_expression);
            }
        }
        propagation_residual = propagation_residual
            && value->source_expression < ir->expression_count
            && ir->expressions[value->source_expression].kind
                == SOL_IR_EXPR_PROPAGATE
            && value->type == callable->result;
        if ((int)value->kind < 0 || value->kind > SOL_MIR_VALUE_TERMINATOR
            || value->type >= ir->type_count || value->block >= mir->block_count
            || (value->source_expression != SOL_IR_NONE
                && (value->source_expression >= ir->expression_count
                    || ir->expressions[value->source_expression].type
                        != value->type) && !propagation_residual)
            || !mir_span_valid(ir, value->span)) {
            return mir_error(diagnostics, value->span,
                "malformed MIR value metadata");
        }
    }
    SolMirSlice entry_instructions = mir->blocks[mir->entry].instructions;
    size_t receiver_count = callable->receiver == SOL_IR_NONE ? 0u : 1u;
    size_t entry_parameter_count = callable->parameters.count + receiver_count;
    if (entry_instructions.count < entry_parameter_count) {
        return mir_error(diagnostics, callable->span,
            "MIR entry parameter activation is incomplete");
    }
    for (size_t index = 0; index < mir->instruction_count; ++index) {
        const SolMirInstruction *instruction = &mir->instructions[index];
        if (instruction->kind != SOL_MIR_INST_PARAMETER_LIVE) continue;
        size_t ordinal = index - entry_instructions.offset;
        SolIrLocalId expected = ordinal < receiver_count
            ? callable->receiver : ordinal - receiver_count
                    < callable->parameters.count
                ? ir->roots[callable->parameters.offset + ordinal
                    - receiver_count]
                : SOL_IR_NONE;
        if (instruction->block != mir->entry
            || index < entry_instructions.offset
            || ordinal >= entry_parameter_count
            || instruction->as.local != expected) {
            return mir_error(diagnostics, instruction->span,
                "MIR parameter activation is misplaced or reordered");
        }
    }
    for (size_t ordinal = 0; ordinal < entry_parameter_count; ++ordinal) {
        const SolMirInstruction *instruction = &mir->instructions[
            entry_instructions.offset + ordinal];
        SolIrLocalId expected = ordinal < receiver_count
            ? callable->receiver : ir->roots[callable->parameters.offset
                + ordinal - receiver_count];
        if (instruction->kind != SOL_MIR_INST_PARAMETER_LIVE
            || instruction->as.local != expected) {
            return mir_error(diagnostics, instruction->span,
                "MIR entry parameter activation is incomplete");
        }
    }
    size_t *predecessors = calloc(mir->block_count, sizeof(*predecessors));
    bool *reachable = calloc(mir->block_count, sizeof(*reachable));
    SolMirBlockId *queue = malloc(mir->block_count * sizeof(*queue));
    if (predecessors == NULL || reachable == NULL || queue == NULL) {
        free(predecessors);
        free(reachable);
        free(queue);
        return mir_error(diagnostics, callable->span,
            "MIR validation allocation failed");
    }
    for (size_t block = 0; block < mir->block_count; ++block) {
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        if (term->kind == SOL_MIR_TERM_GOTO) {
            ++predecessors[term->as.go_to.block];
        } else if (term->kind == SOL_MIR_TERM_BRANCH) {
            ++predecessors[term->as.branch.true_edge.block];
            ++predecessors[term->as.branch.false_edge.block];
        } else if (term->kind == SOL_MIR_TERM_INVOKE) {
            if (term->as.invoke.normal_edge.block != SOL_MIR_NONE) {
                ++predecessors[term->as.invoke.normal_edge.block];
            }
            ++predecessors[term->as.invoke.failure_edge.block];
        } else if (term->kind == SOL_MIR_TERM_CHECK_REFINED) {
            ++predecessors[term->as.check_refined.normal_edge.block];
            ++predecessors[term->as.check_refined.failure_edge.block];
        } else if (term->kind == SOL_MIR_TERM_PROPAGATE) {
            ++predecessors[term->as.propagate.value_edge.block];
            ++predecessors[term->as.propagate.residual_edge.block];
        } else if (term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
            ++predecessors[term->as.check_contract.satisfied_edge.block];
            ++predecessors[term->as.check_contract.violation_edge.block];
            ++predecessors[term->as.check_contract.failure_edge.block];
        } else if (term->kind == SOL_MIR_TERM_BREAK
            || term->kind == SOL_MIR_TERM_CONTINUE) {
            ++predecessors[term->as.transfer.edge.block];
        }
    }
    size_t first = 0;
    size_t count = 1;
    queue[0] = mir->entry;
    reachable[mir->entry] = true;
    while (first < count) {
        SolMirBlockId block = queue[first++];
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        SolMirBlockId targets[3];
        size_t target_count = 0;
        if (term->kind == SOL_MIR_TERM_GOTO) targets[target_count++] = term->as.go_to.block;
        else if (term->kind == SOL_MIR_TERM_BRANCH) {
            targets[target_count++] = term->as.branch.true_edge.block;
            targets[target_count++] = term->as.branch.false_edge.block;
        } else if (term->kind == SOL_MIR_TERM_INVOKE) {
            if (term->as.invoke.normal_edge.block != SOL_MIR_NONE) {
                targets[target_count++] = term->as.invoke.normal_edge.block;
            }
            targets[target_count++] = term->as.invoke.failure_edge.block;
        } else if (term->kind == SOL_MIR_TERM_CHECK_REFINED) {
            targets[target_count++] = term->as.check_refined.normal_edge.block;
            targets[target_count++] = term->as.check_refined.failure_edge.block;
        } else if (term->kind == SOL_MIR_TERM_PROPAGATE) {
            targets[target_count++] = term->as.propagate.value_edge.block;
            targets[target_count++] = term->as.propagate.residual_edge.block;
        } else if (term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
            targets[target_count++] = term->as.check_contract.satisfied_edge.block;
            targets[target_count++] = term->as.check_contract.violation_edge.block;
            targets[target_count++] = term->as.check_contract.failure_edge.block;
        } else if (term->kind == SOL_MIR_TERM_BREAK
            || term->kind == SOL_MIR_TERM_CONTINUE) {
            targets[target_count++] = term->as.transfer.edge.block;
        }
        for (size_t index = 0; index < target_count; ++index) {
            if (!reachable[targets[index]]) {
                reachable[targets[index]] = true;
                queue[count++] = targets[index];
            }
        }
    }
    bool valid = predecessors[mir->entry] == 0;
    for (size_t block = 0; valid && block < mir->block_count; ++block) {
        valid = reachable[block];
    }
    free(predecessors);
    free(reachable);
    free(queue);
    if (!valid) return mir_error(diagnostics, callable->span,
        "MIR contains an unreachable block or entry predecessor");
    return mir_validate_contract_envelope(ir, mir, diagnostics)
        && mir_validate_arena_ownership(mir, diagnostics, callable->span)
        && mir_validate_storage(ir, mir, diagnostics)
        && mir_validate_paths(ir, mir, diagnostics)
        && mir_validate_regions(ir, mir, diagnostics)
        && mir_validate_handlers(ir, mir, diagnostics)
        && mir_validate_storage_order(ir, mir, diagnostics)
        && mir_validate_temporaries(ir, mir, diagnostics)
        && mir_validate_source_events(ir, mir, diagnostics);
}

SolMirLowerOutcome sol_mir_lower_callable(const SolIr *ir,
    SolIrCallableId callable_id, SolMir *mir, SolDiagnostics *diagnostics) {
    if (ir == NULL || mir == NULL || diagnostics == NULL
        || !mir_empty(mir) || callable_id >= ir->callable_count
        || !sol_ir_validate(ir, diagnostics)) {
        mir_error(diagnostics, (SolSpan){0}, "invalid input passed to MIR lowering");
        return SOL_MIR_LOWER_FAILED;
    }
    const SolIrCallable *callable = &ir->callables[callable_id];
    bool contracted = false;
    for (size_t index = 0; index < ir->obligation_count; ++index) {
        contracted = contracted
            || (ir->obligations[index].owner_kind == SOL_CONTRACT_OWNER_ITEM
                && ir->obligations[index].owner == callable->owner);
    }
    bool exclusive_contract_parameter = false;
    bool fallible_contract_snapshot = false;
    for (size_t index = 0; index < callable->parameters.count; ++index) {
        SolIrLocalId local = ir->roots[callable->parameters.offset + index];
        exclusive_contract_parameter = exclusive_contract_parameter
            || ir->locals[local].access == SOL_ACCESS_EXCLUSIVE;
    }
    exclusive_contract_parameter = exclusive_contract_parameter
        || (callable->receiver != SOL_IR_NONE
            && callable->receiver_access == SOL_ACCESS_EXCLUSIVE);
    for (size_t index = 0; index < ir->snapshot_count; ++index) {
        const SolIrSnapshot *snapshot = &ir->snapshots[index];
        if (snapshot->obligation >= ir->obligation_count
            || !mir_obligation_owned_by_callable(
                &ir->obligations[snapshot->obligation], callable)) continue;
        const SolIrExpression *operand = &ir->expressions[snapshot->operand];
        fallible_contract_snapshot = fallible_contract_snapshot
            || operand->kind != SOL_IR_EXPR_PLACE
            || (operand->local_use != SOL_IR_LOCAL_USE_NONE
                && operand->local_use != SOL_IR_LOCAL_USE_COPY
                && operand->local_use != SOL_IR_LOCAL_USE_SHARED)
            || operand->as.place >= ir->place_count
            || ir->places[operand->as.place].root_kind
                != SOL_IR_PLACE_ROOT_LOCAL
            || ir->places[operand->as.place].projections.count != 0
            || (!mir_type_is(ir, snapshot->type, SOL_IR_TYPE_INT64)
                && !mir_type_is(ir, snapshot->type, SOL_IR_TYPE_BOOL)
                && !mir_type_is(ir, snapshot->type, SOL_IR_TYPE_UNIT));
    }
    bool implementation
        = callable->kind == SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION;
    if ((callable->kind != SOL_IR_CALLABLE_FUNCTION
            && callable->kind != SOL_IR_CALLABLE_TEST && !implementation)
        || callable->body == SOL_IR_NONE
        || (implementation
            ? callable->receiver == SOL_IR_NONE
            : callable->receiver != SOL_IR_NONE)
        || callable->capability_source != SOL_IR_NONE
        || (contracted
            && (callable->generic_parameters.count != 0
                || callable->effect_parameters.count != 0
                || exclusive_contract_parameter
                || fallible_contract_snapshot))) {
        sol_diagnostics_add(diagnostics, "SOL-MIR-001", SOL_SEVERITY_ERROR,
            callable->span,
            "callable is outside the initial P1a MIR checkpoint");
        return SOL_MIR_LOWER_UNSUPPORTED;
    }
    SolMir lowered;
    sol_mir_init(&lowered);
    lowered.callable = callable_id;
    lowered.generic_parameters = callable->generic_parameters;
    lowered.effect_parameters = callable->effect_parameters;
    MirLowerer lowerer = {
        .ir = ir,
        .callable = callable,
        .mir = &lowered,
        .diagnostics = diagnostics,
        .current = SOL_MIR_NONE,
        .contracted = contracted,
        .contract_epilogue = SOL_MIR_NONE,
    };
    SolMirBlockId entry = mir_append_block(&lowerer, callable->span);
    lowered.entry = entry;
    if (entry != SOL_MIR_NONE && mir_start_block(&lowerer, entry)) {
        if (callable->receiver != SOL_IR_NONE
            && !mir_emit_local(&lowerer, SOL_MIR_INST_PARAMETER_LIVE,
                callable->receiver, callable->span)) lowerer.failed = true;
        for (size_t index = 0; !lowerer.failed
            && index < callable->parameters.count; ++index) {
            SolIrLocalId local = ir->roots[callable->parameters.offset + index];
            if (!mir_emit_local(&lowerer, SOL_MIR_INST_PARAMETER_LIVE,
                local, callable->span)) break;
        }
    }
    SolMirValueId contract_result = SOL_MIR_NONE;
    if (contracted && !lowerer.failed) {
        lowerer.contract_epilogue = mir_append_block(&lowerer, callable->span);
        lowered.contract_epilogue = lowerer.contract_epilogue;
        contract_result = mir_append_parameter(&lowerer,
            lowerer.contract_epilogue, callable->result,
            SOL_IR_NONE, callable->span);
        if (lowerer.contract_epilogue == SOL_MIR_NONE
            || contract_result == SOL_MIR_NONE
            || !mir_lower_contract_entry(&lowerer)) lowerer.failed = true;
    }
    LoweredValue body = lowerer.failed || lowerer.unsupported
        ? mir_unreachable()
        : mir_lower_expression(&lowerer, callable->body, NULL);
    if (body.reachable && !lowerer.failed && !lowerer.unsupported) {
        if (!mir_finish_success(&lowerer, NULL, body.value, callable->span)) {
            lowerer.failed = true;
        }
    }
    if (contracted && !lowerer.failed && !lowerer.unsupported) {
        bool has_success = false;
        for (size_t block = 0; block < lowered.block_count; ++block) {
            const SolMirTerminator *term = &lowered.blocks[block].terminator;
            has_success = has_success || (term->kind == SOL_MIR_TERM_GOTO
                && term->as.go_to.block == lowerer.contract_epilogue);
        }
        if (!has_success) {
            mir_unsupported(&lowerer, callable->span,
                "contracted MIR requires a reachable successful result");
        } else if (!mir_start_block(&lowerer, lowerer.contract_epilogue)
            || !mir_lower_contract_epilogue(&lowerer, contract_result)) {
            lowerer.failed = true;
        }
    }
    free(lowerer.pending_temporaries);
    for (size_t block = 0; block < lowered.block_count; ++block) {
        mir_canonicalize_terminator(&lowered.blocks[block].terminator);
    }
    if (lowerer.unsupported) {
        sol_mir_free(&lowered);
        return SOL_MIR_LOWER_UNSUPPORTED;
    }
    if (lowerer.failed || !sol_mir_validate(ir, &lowered, diagnostics)) {
        sol_mir_free(&lowered);
        return SOL_MIR_LOWER_FAILED;
    }
    *mir = lowered;
    return SOL_MIR_LOWER_SUCCEEDED;
}
