#include "sol/mir.h"

#include <stdlib.h>
#include <string.h>

typedef struct Scope Scope;

struct Scope {
    const Scope *parent;
    SolIrSlice cleanup;
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
    bool unsupported;
    bool failed;
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
    mir->entry = SOL_MIR_NONE;
}

void sol_mir_free(SolMir *mir) {
    if (mir == NULL) return;
    free(mir->blocks);
    free(mir->instructions);
    free(mir->values);
    free(mir->parameter_values);
    free(mir->edge_values);
    sol_mir_init(mir);
}

static bool mir_empty(const SolMir *mir) {
    return mir != NULL && mir->callable == SOL_IR_NONE
        && mir->entry == SOL_MIR_NONE && mir->blocks == NULL
        && mir->block_count == 0 && mir->block_capacity == 0
        && mir->instructions == NULL && mir->instruction_count == 0
        && mir->instruction_capacity == 0 && mir->values == NULL
        && mir->value_count == 0 && mir->value_capacity == 0
        && mir->parameter_values == NULL && mir->parameter_value_count == 0
        && mir->parameter_value_capacity == 0 && mir->edge_values == NULL
        && mir->edge_value_count == 0 && mir->edge_value_capacity == 0;
}

static SolMirBlockId mir_append_block(MirLowerer *lowerer, SolSpan span) {
    SolMir *mir = lowerer->mir;
    if (!mir_grow((void **)&mir->blocks, &mir->block_capacity,
        mir->block_count, sizeof(*mir->blocks))) {
        lowerer->failed = true;
        return SOL_MIR_NONE;
    }
    SolMirBlockId id = mir->block_count++;
    mir->blocks[id] = (SolMirBlock){
        .id = id,
        .parameters = {mir->parameter_value_count, 0},
        .instructions = {mir->instruction_count, 0},
        .terminator = {.kind = SOL_MIR_TERM_INVALID},
        .span = span,
    };
    return id;
}

static bool mir_start_block(MirLowerer *lowerer, SolMirBlockId block) {
    if (block >= lowerer->mir->block_count
        || lowerer->mir->blocks[block].started) {
        lowerer->failed = true;
        return false;
    }
    lowerer->current = block;
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
    mir->values[id] = value;
    return id;
}

static SolMirValueId mir_append_parameter(MirLowerer *lowerer,
    SolMirBlockId block, SolIrTypeId type, SolSpan span) {
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
    mir->instructions[id] = instruction;
    ++mir->blocks[lowerer->current].instructions.count;
    if (has_result) {
        SolMirValueId result = mir_append_value(lowerer, (SolMirValue){
            .kind = SOL_MIR_VALUE_INSTRUCTION,
            .type = instruction.type,
            .block = lowerer->current,
            .definition = id,
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

static bool mir_emit_exit_cleanup(MirLowerer *lowerer,
    const Scope *scope, SolSpan span) {
    for (const Scope *current = scope; current != NULL;
        current = current->parent) {
        if (!mir_emit_cleanup_slice(lowerer, current->cleanup, span)) return false;
    }
    SolIrSlice parameters = lowerer->callable->parameters;
    while (parameters.count != 0) {
        --parameters.count;
        SolIrLocalId local = lowerer->ir->roots[
            parameters.offset + parameters.count];
        if (!mir_emit_local(lowerer, SOL_MIR_INST_DROP_IF_INITIALIZED,
                local, span)
            || !mir_emit_local(lowerer, SOL_MIR_INST_STORAGE_DEAD,
                local, span)) return false;
    }
    return true;
}

static LoweredValue mir_lower_expression(MirLowerer *lowerer,
    SolIrExpressionId id, const Scope *scope);

static LoweredValue mir_unreachable(void) {
    return (LoweredValue){.reachable = false, .value = SOL_MIR_NONE};
}

static LoweredValue mir_failure(MirLowerer *lowerer) {
    lowerer->failed = lowerer->failed || !lowerer->unsupported;
    return mir_unreachable();
}

static LoweredValue mir_lower_if(MirLowerer *lowerer,
    const SolIrExpression *expression, const Scope *scope) {
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
    SolMirBlockId then_end = lowerer->current;
    if (!mir_start_block(lowerer, else_block)) return mir_failure(lowerer);
    LoweredValue else_value = mir_lower_expression(lowerer,
        expression->as.if_expr.else_branch, scope);
    SolMirBlockId else_end = lowerer->current;
    if (!then_value.reachable && !else_value.reachable) {
        lowerer->current = SOL_MIR_NONE;
        return mir_unreachable();
    }
    SolMirBlockId join = mir_append_block(lowerer, expression->span);
    SolMirValueId parameter = mir_append_parameter(lowerer, join,
        expression->type, expression->span);
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

static LoweredValue mir_lower_block(MirLowerer *lowerer,
    const SolIrExpression *expression, const Scope *parent) {
    Scope scope = {
        .parent = parent,
        .cleanup = {.offset = expression->as.block.cleanup.offset, .count = 0},
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
                            .local = statement->local,
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
                if (statement->operator_kind != SOL_TOKEN_EQUAL
                    || statement->target >= lowerer->ir->expression_count) {
                    mir_unsupported(lowerer, statement->span,
                        "P1a MIR checkpoint supports only plain whole-local assignment");
                    return mir_unreachable();
                }
                const SolIrExpression *target
                    = &lowerer->ir->expressions[statement->target];
                if (target->kind != SOL_IR_EXPR_PLACE
                    || target->as.place >= lowerer->ir->place_count) {
                    return mir_failure(lowerer);
                }
                const SolIrPlace *place = &lowerer->ir->places[target->as.place];
                if (place->root_kind != SOL_IR_PLACE_ROOT_LOCAL
                    || place->projections.count != 0) {
                    mir_unsupported(lowerer, statement->span,
                        "P1a MIR checkpoint does not lower projected assignment");
                    return mir_unreachable();
                }
                LoweredValue value = mir_lower_expression(lowerer,
                    statement->expression, &scope);
                if (!value.reachable) return value;
                if (!mir_emit_local(lowerer,
                        SOL_MIR_INST_DROP_IF_INITIALIZED,
                        place->local, statement->span)
                    || mir_append_instruction(lowerer, (SolMirInstruction){
                        .kind = SOL_MIR_INST_STORE,
                        .type = SOL_IR_NONE,
                        .source_expression = statement->expression,
                        .span = statement->span,
                        .as.store = {.local = place->local, .value = value.value},
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
                if (!mir_emit_exit_cleanup(lowerer, &scope, statement->span)
                    || !mir_set_return(lowerer, value.value, statement->span)) {
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
                || place->projections.count != 0
                || (expression->local_use != SOL_IR_LOCAL_USE_COPY
                    && expression->local_use != SOL_IR_LOCAL_USE_MOVE)) {
                mir_unsupported(lowerer, expression->span,
                    "P1a MIR checkpoint supports only copied or moved whole locals");
                return mir_unreachable();
            }
            instruction.kind = expression->local_use == SOL_IR_LOCAL_USE_COPY
                ? SOL_MIR_INST_LOAD_COPY : SOL_MIR_INST_LOAD_MOVE;
            instruction.as.local = place->local;
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
                mir_unsupported(lowerer, expression->span,
                    "short-circuit CFG lowering is deferred from the initial P1a checkpoint");
                return mir_unreachable();
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
        case SOL_IR_EXPR_BLOCK:
            return mir_lower_block(lowerer, expression, scope);
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

static bool mir_span_valid(const SolIr *ir, SolSpan span) {
    return span.start <= span.end && span.end <= ir->source_length;
}

static bool mir_type_is(const SolIr *ir, SolIrTypeId id,
    SolIrTypeKind kind) {
    return id < ir->type_count && ir->types[id].kind == kind;
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

static bool mir_validate_edge(const SolMir *mir, SolMirBlockId source,
    const SolMirEdge *edge) {
    if (edge->block >= mir->block_count
        || !mir_slice_valid(edge->arguments, mir->edge_value_count)
        || edge->arguments.count != mir->blocks[edge->block].parameters.count) {
        return false;
    }
    for (size_t index = 0; index < edge->arguments.count; ++index) {
        SolMirValueId argument = mir->edge_values[edge->arguments.offset + index];
        SolMirValueId parameter = mir->parameter_values[
            mir->blocks[edge->block].parameters.offset + index];
        if (argument >= mir->value_count || parameter >= mir->value_count
            || !mir_value_available(mir, argument, source,
                mir->blocks[source].instructions.offset
                    + mir->blocks[source].instructions.count)
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
    if ((mir->value_count != 0 && values == NULL)
        || (mir->edge_value_count != 0 && edges == NULL)) {
        free(values);
        free(edges);
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
        SolMirSlice slices[2];
        size_t count = 0;
        if (terminator->kind == SOL_MIR_TERM_GOTO) {
            slices[count++] = terminator->as.go_to.arguments;
        } else if (terminator->kind == SOL_MIR_TERM_BRANCH) {
            slices[count++] = terminator->as.branch.true_edge.arguments;
            slices[count++] = terminator->as.branch.false_edge.arguments;
        }
        for (size_t slice = 0; valid && slice < count; ++slice) {
            for (size_t index = 0; valid && index < slices[slice].count; ++index) {
                size_t slot = slices[slice].offset + index;
                valid = slot < mir->edge_value_count && ++edges[slot] == 1;
            }
        }
    }
    for (size_t id = 0; valid && id < mir->instruction_count; ++id) {
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
    free(values);
    free(edges);
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
                    local = instruction->as.store.local;
                } else if (instruction->kind >= SOL_MIR_INST_PARAMETER_LIVE
                    && instruction->kind <= SOL_MIR_INST_LOAD_MOVE) {
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
                    case SOL_MIR_INST_STORAGE_DEAD:
                        valid = state[local] == MIR_STORAGE_UNINITIALIZED;
                        state[local] = MIR_STORAGE_DEAD;
                        break;
                    case SOL_MIR_INST_LOAD_COPY:
                        valid = state[local] == MIR_STORAGE_INITIALIZED;
                        break;
                    case SOL_MIR_INST_LOAD_MOVE:
                        valid = state[local] == MIR_STORAGE_INITIALIZED;
                        state[local] = MIR_STORAGE_UNINITIALIZED;
                        break;
                    case SOL_MIR_INST_STORE:
                        valid = state[local] == MIR_STORAGE_UNINITIALIZED;
                        state[local] = MIR_STORAGE_INITIALIZED;
                        break;
                    default: break;
                }
            }
            const SolMirTerminator *term = &mir->blocks[block].terminator;
            if (valid && (term->kind == SOL_MIR_TERM_RETURN
                || term->kind == SOL_MIR_TERM_PANIC)) {
                SolIrDefinitionId owner = ir->callables[mir->callable].owner;
                for (size_t local = 0; valid && local < ir->local_count; ++local) {
                    if (ir->locals[local].owner == owner) {
                        valid = state[local] == MIR_STORAGE_DEAD;
                    }
                }
            }
            SolMirBlockId targets[2];
            size_t target_count = 0;
            if (valid && term->kind == SOL_MIR_TERM_GOTO) {
                targets[target_count++] = term->as.go_to.block;
            } else if (valid && term->kind == SOL_MIR_TERM_BRANCH) {
                targets[target_count++] = term->as.branch.true_edge.block;
                targets[target_count++] = term->as.branch.false_edge.block;
            }
            for (size_t index = 0; valid && index < target_count; ++index) {
                SolMirBlockId target = targets[index];
                if (!known[target]) {
                    known[target] = true;
                    if (ir->local_count != 0) memcpy(
                        &incoming[target * ir->local_count], state,
                        ir->local_count * sizeof(*state));
                    changed = true;
                } else {
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

bool sol_mir_validate(const SolIr *ir, const SolMir *mir,
    SolDiagnostics *diagnostics) {
    if (ir == NULL || mir == NULL || !sol_ir_validate(ir, diagnostics)
        || mir->callable >= ir->callable_count
        || mir->entry >= mir->block_count
        || mir->block_count > SIZE_MAX / sizeof(SolMirBlockId)
        || mir->block_count > mir->block_capacity
        || mir->instruction_count > mir->instruction_capacity
        || mir->value_count > mir->value_capacity
        || mir->parameter_value_count > mir->parameter_value_capacity
        || mir->edge_value_count > mir->edge_value_capacity
        || (mir->block_count != 0 && mir->blocks == NULL)
        || (mir->instruction_count != 0 && mir->instructions == NULL)
        || (mir->value_count != 0 && mir->values == NULL)
        || (mir->parameter_value_count != 0 && mir->parameter_values == NULL)
        || (mir->edge_value_count != 0 && mir->edge_values == NULL)) {
        return mir_error(diagnostics, (SolSpan){0}, "malformed MIR arena header");
    }
    const SolIrCallable *callable = &ir->callables[mir->callable];
    if (mir->blocks[mir->entry].parameters.count != 0) {
        return mir_error(diagnostics, callable->span,
            "MIR entry block cannot have SSA parameters");
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
            || block->terminator.kind > SOL_MIR_TERM_PANIC
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
                || mir->values[id].definition != index) {
                free(instruction_owners);
                free(parameter_owners);
                return mir_error(diagnostics, block->span,
                    "malformed MIR block parameter");
            }
        }
        switch (block->terminator.kind) {
            case SOL_MIR_TERM_GOTO:
                if (!mir_validate_edge(mir, block_id,
                    &block->terminator.as.go_to)) {
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
                        &block->terminator.as.branch.true_edge)
                    || !mir_validate_edge(mir, block_id,
                        &block->terminator.as.branch.false_edge)) {
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
            || instruction->kind > SOL_MIR_INST_BINARY
            || !mir_span_valid(ir, instruction->span)) {
            return mir_error(diagnostics, instruction->span,
                "malformed MIR instruction");
        }
        bool result_kind = instruction->kind <= SOL_MIR_INST_CONST_UNIT
            || instruction->kind == SOL_MIR_INST_LOAD_COPY
            || instruction->kind == SOL_MIR_INST_LOAD_MOVE
            || instruction->kind == SOL_MIR_INST_UNARY
            || instruction->kind == SOL_MIR_INST_BINARY;
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
                || mir->values[instruction->result].type != instruction->type) {
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
            || instruction->kind == SOL_MIR_INST_STORAGE_DEAD
            || instruction->kind == SOL_MIR_INST_LOAD_COPY
            || instruction->kind == SOL_MIR_INST_LOAD_MOVE) {
            if (instruction->as.local >= ir->local_count
                || ir->locals[instruction->as.local].owner != callable->owner) {
                return mir_error(diagnostics, instruction->span,
                    "MIR instruction references a foreign local");
            }
            if ((instruction->kind == SOL_MIR_INST_LOAD_COPY
                    || instruction->kind == SOL_MIR_INST_LOAD_MOVE)
                && (source == NULL || source->kind != SOL_IR_EXPR_PLACE
                    || source->as.place >= ir->place_count
                    || ir->places[source->as.place].root_kind
                        != SOL_IR_PLACE_ROOT_LOCAL
                    || ir->places[source->as.place].local != instruction->as.local
                    || ir->places[source->as.place].projections.count != 0
                    || source->local_use != (instruction->kind
                        == SOL_MIR_INST_LOAD_COPY ? SOL_IR_LOCAL_USE_COPY
                        : SOL_IR_LOCAL_USE_MOVE)
                    || instruction->type != ir->locals[instruction->as.local].type)) {
                return mir_error(diagnostics, instruction->span,
                    "MIR local load is inconsistent with source ownership");
            }
        } else if (instruction->kind == SOL_MIR_INST_STORE) {
            if (instruction->as.store.local >= ir->local_count
                || ir->locals[instruction->as.store.local].owner != callable->owner
                || instruction->as.store.value >= mir->value_count
                || !mir_value_available(mir, instruction->as.store.value,
                    instruction->block, id)
                || mir->values[instruction->as.store.value].type
                    != ir->locals[instruction->as.store.local].type) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR store");
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
        }
    }
    for (size_t id = 0; id < mir->value_count; ++id) {
        const SolMirValue *value = &mir->values[id];
        if ((int)value->kind < 0 || value->kind > SOL_MIR_VALUE_INSTRUCTION
            || value->type >= ir->type_count || value->block >= mir->block_count
            || !mir_span_valid(ir, value->span)) {
            return mir_error(diagnostics, value->span,
                "malformed MIR value metadata");
        }
    }
    SolMirSlice entry_instructions = mir->blocks[mir->entry].instructions;
    if (entry_instructions.count < callable->parameters.count) {
        return mir_error(diagnostics, callable->span,
            "MIR entry parameter activation is incomplete");
    }
    for (size_t index = 0; index < mir->instruction_count; ++index) {
        const SolMirInstruction *instruction = &mir->instructions[index];
        if (instruction->kind != SOL_MIR_INST_PARAMETER_LIVE) continue;
        size_t ordinal = index - entry_instructions.offset;
        if (instruction->block != mir->entry
            || index < entry_instructions.offset
            || ordinal >= callable->parameters.count
            || instruction->as.local != ir->roots[
                callable->parameters.offset + ordinal]) {
            return mir_error(diagnostics, instruction->span,
                "MIR parameter activation is misplaced or reordered");
        }
    }
    for (size_t ordinal = 0; ordinal < callable->parameters.count; ++ordinal) {
        const SolMirInstruction *instruction = &mir->instructions[
            entry_instructions.offset + ordinal];
        if (instruction->kind != SOL_MIR_INST_PARAMETER_LIVE
            || instruction->as.local != ir->roots[
                callable->parameters.offset + ordinal]) {
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
        }
    }
    size_t first = 0;
    size_t count = 1;
    queue[0] = mir->entry;
    reachable[mir->entry] = true;
    while (first < count) {
        SolMirBlockId block = queue[first++];
        const SolMirTerminator *term = &mir->blocks[block].terminator;
        SolMirBlockId targets[2];
        size_t target_count = 0;
        if (term->kind == SOL_MIR_TERM_GOTO) targets[target_count++] = term->as.go_to.block;
        else if (term->kind == SOL_MIR_TERM_BRANCH) {
            targets[target_count++] = term->as.branch.true_edge.block;
            targets[target_count++] = term->as.branch.false_edge.block;
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
    return mir_validate_arena_ownership(mir, diagnostics, callable->span)
        && mir_validate_storage(ir, mir, diagnostics);
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
    if ((callable->kind != SOL_IR_CALLABLE_FUNCTION
            && callable->kind != SOL_IR_CALLABLE_TEST)
        || callable->body == SOL_IR_NONE
        || callable->generic_parameters.count != 0
        || callable->effect_parameters.count != 0
        || callable->receiver != SOL_IR_NONE
        || callable->capability_source != SOL_IR_NONE || contracted) {
        sol_diagnostics_add(diagnostics, "SOL-MIR-001", SOL_SEVERITY_ERROR,
            callable->span,
            "callable is outside the initial P1a MIR checkpoint");
        return SOL_MIR_LOWER_UNSUPPORTED;
    }
    SolMir lowered;
    sol_mir_init(&lowered);
    lowered.callable = callable_id;
    MirLowerer lowerer = {
        .ir = ir,
        .callable = callable,
        .mir = &lowered,
        .diagnostics = diagnostics,
        .current = SOL_MIR_NONE,
    };
    SolMirBlockId entry = mir_append_block(&lowerer, callable->span);
    lowered.entry = entry;
    if (entry != SOL_MIR_NONE && mir_start_block(&lowerer, entry)) {
        for (size_t index = 0; index < callable->parameters.count; ++index) {
            SolIrLocalId local = ir->roots[callable->parameters.offset + index];
            if (!mir_emit_local(&lowerer, SOL_MIR_INST_PARAMETER_LIVE,
                local, callable->span)) break;
        }
    }
    LoweredValue body = lowerer.failed || lowerer.unsupported
        ? mir_unreachable()
        : mir_lower_expression(&lowerer, callable->body, NULL);
    if (body.reachable && !lowerer.failed && !lowerer.unsupported) {
        if (!mir_emit_exit_cleanup(&lowerer, NULL, callable->span)
            || !mir_set_return(&lowerer, body.value, callable->span)) {
            lowerer.failed = true;
        }
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
