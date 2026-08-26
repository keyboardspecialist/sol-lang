#include "sol/mir.h"

#include <stdlib.h>
#include <string.h>

typedef struct Scope Scope;
typedef struct LoopContext LoopContext;

struct Scope {
    const Scope *parent;
    SolIrSlice cleanup;
    SolIrStatementId region;
};

struct LoopContext {
    const LoopContext *parent;
    SolMirLoopId id;
    const Scope *boundary;
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
    free(mir->call_arguments);
    free(mir->loops);
    free(mir->construct_operands);
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
        && mir->edge_value_count == 0 && mir->edge_value_capacity == 0
        && mir->call_arguments == NULL && mir->call_argument_count == 0
        && mir->call_argument_capacity == 0 && mir->loops == NULL
        && mir->loop_count == 0 && mir->loop_capacity == 0
        && mir->construct_operands == NULL
        && mir->construct_operand_count == 0
        && mir->construct_operand_capacity == 0;
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
        .order = SOL_MIR_NONE,
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
    mir->values[id] = value;
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
    mir->instructions[id] = instruction;
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
    mir->call_arguments[mir->call_argument_count++] = argument;
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

static bool mir_emit_exit_cleanup(MirLowerer *lowerer,
    const Scope *scope, SolSpan span) {
    for (const Scope *current = scope; current != NULL;
        current = current->parent) {
        if (!mir_emit_cleanup_slice(lowerer, current->cleanup, span)) return false;
        if (current->region != SOL_IR_NONE
            && mir_append_instruction(lowerer, (SolMirInstruction){
                .kind = SOL_MIR_INST_REGION_EXIT,
                .type = SOL_IR_NONE,
                .source_expression = SOL_IR_NONE,
                .span = span,
                .as.region = current->region,
            }, false) == SOL_MIR_NONE) return false;
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

static bool mir_emit_cleanup_to(MirLowerer *lowerer,
    const Scope *scope, const Scope *boundary, SolSpan span) {
    const Scope *current = scope;
    while (current != boundary) {
        if (current == NULL
            || !mir_emit_cleanup_slice(lowerer, current->cleanup, span)) {
            return false;
        }
        if (current->region != SOL_IR_NONE
            && mir_append_instruction(lowerer, (SolMirInstruction){
                .kind = SOL_MIR_INST_REGION_EXIT,
                .type = SOL_IR_NONE,
                .source_expression = SOL_IR_NONE,
                .span = span,
                .as.region = current->region,
            }, false) == SOL_MIR_NONE) return false;
        current = current->parent;
    }
    return true;
}

static LoweredValue mir_lower_expression(MirLowerer *lowerer,
    SolIrExpressionId id, const Scope *scope);
static bool mir_type_is(const SolIr *ir, SolIrTypeId id,
    SolIrTypeKind kind);

static LoweredValue mir_unreachable(void) {
    return (LoweredValue){.reachable = false, .value = SOL_MIR_NONE};
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
    if (right.reachable && !mir_set_goto(lowerer, lowerer->current, join,
        right.value, true, expression->span)) return mir_failure(lowerer);
    if (!mir_start_block(lowerer, join)) return mir_failure(lowerer);
    return (LoweredValue){.reachable = true, .value = parameter};
}

static LoweredValue mir_lower_direct_call(MirLowerer *lowerer,
    SolIrExpressionId id, const SolIrExpression *expression,
    const Scope *scope) {
    if (expression->as.call.kind != SOL_IR_CALL_FUNCTION
        || expression->as.call.callable >= lowerer->ir->callable_count
        || expression->as.call.type_arguments.count != 0
        || expression->as.call.evidence.count != 0
        || expression->as.call.effect_parameter != SOL_IR_NONE) {
        mir_unsupported(lowerer, expression->span,
            "P1a.2 MIR supports only nongeneric direct function calls");
        return mir_unreachable();
    }
    const SolIrCallable *target
        = &lowerer->ir->callables[expression->as.call.callable];
    if (target->kind != SOL_IR_CALLABLE_FUNCTION
        || target->generic_parameters.count != 0
        || target->effect_parameters.count != 0
        || target->receiver != SOL_IR_NONE
        || target->capability_source != SOL_IR_NONE
        || target->result != expression->type) {
        mir_unsupported(lowerer, expression->span,
            "P1a.2 MIR direct call target is outside the nongeneric free-function subset");
        return mir_unreachable();
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
            .value = SOL_MIR_NONE,
            .place = SOL_IR_NONE,
        };
        if (operand->access == SOL_ACCESS_OWNED) {
            LoweredValue value = mir_lower_expression(lowerer,
                operand->value, scope);
            if (!value.reachable) {
                free(arguments);
                return value;
            }
            if (value.value >= lowerer->mir->value_count
                || lowerer->mir->values[value.value].source_expression
                    != operand->value) {
                free(arguments);
                mir_unsupported(lowerer,
                    lowerer->ir->expressions[operand->value].span,
                    "P1a.2 MIR does not lower block-valued direct-call operands");
                return mir_unreachable();
            }
            argument->value = mir_instruction_result(lowerer,
                (SolMirInstruction){
                    .kind = SOL_MIR_INST_CALL_ARGUMENT,
                    .type = lowerer->ir->expressions[operand->value].type,
                    .source_expression = operand->value,
                    .span = lowerer->ir->expressions[operand->value].span,
                    .as.operand = value.value,
                });
            if (argument->value == SOL_MIR_NONE) {
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
        if (place->root_kind != SOL_IR_PLACE_ROOT_LOCAL
            || place->projections.count != 0) {
            free(arguments);
            mir_unsupported(lowerer, source->span,
                "P1a.2 MIR does not lower projected call borrows");
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
            .callable = expression->as.call.callable,
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

static LoweredValue mir_lower_construct(MirLowerer *lowerer,
    SolIrExpressionId id, const SolIrExpression *expression,
    const Scope *scope) {
    SolMirConstructKind kind;
    SolIrDefinitionId definition = SOL_IR_NONE;
    SolIrVariantId variant = SOL_IR_NONE;
    SolIrSlice operands = {0};
    if (expression->capability_roots.count != 0
        || expression->operation_roots.count != 0) {
        mir_unsupported(lowerer, expression->span,
            "P1a.3b MIR construction does not yet retain authority roots");
        return mir_unreachable();
    }
    if (expression->kind == SOL_IR_EXPR_RECORD) {
        definition = expression->as.record.definition;
        operands = expression->as.record.fields;
        if (definition >= lowerer->ir->definition_count
            || lowerer->ir->definitions[definition].kind
                != SOL_IR_DEFINITION_RECORD) {
            mir_unsupported(lowerer, expression->span,
                "P1a.3b MIR does not lower capability record wrappers");
            return mir_unreachable();
        }
        kind = SOL_MIR_CONSTRUCT_RECORD;
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
                    mir_unsupported(lowerer, expression->span,
                        "checked refined construction requires a later P1a.3b failure edge");
                    return mir_unreachable();
                }
                if (lowerer->ir->definitions[definition].kind
                    != SOL_IR_DEFINITION_DISTINCT) return mir_failure(lowerer);
                kind = SOL_MIR_CONSTRUCT_DISTINCT;
                break;
            default:
                return mir_lower_direct_call(lowerer, id, expression, scope);
        }
    } else {
        mir_unsupported(lowerer, expression->span,
            "multi-operand aggregate construction requires owned temporary cleanup");
        return mir_unreachable();
    }
    if (operands.count > 1) {
        mir_unsupported(lowerer, expression->span,
            "multi-operand aggregate construction requires owned temporary cleanup");
        return mir_unreachable();
    }
    SolMirSlice construct_operands = {
        .offset = lowerer->mir->construct_operand_count,
        .count = 0,
    };
    if (operands.count == 1) {
        const SolIrOperand *operand
            = &lowerer->ir->operands[operands.offset];
        if (operand->access != SOL_ACCESS_OWNED) return mir_failure(lowerer);
        LoweredValue lowered = mir_lower_expression(lowerer,
            operand->value, scope);
        if (!lowered.reachable) return lowered;
        if (lowered.value >= lowerer->mir->value_count
            || lowerer->mir->values[lowered.value].source_expression
                != operand->value) {
            mir_unsupported(lowerer,
                lowerer->ir->expressions[operand->value].span,
                "block-valued construction operands require explicit value-origin transport");
            return mir_unreachable();
        }
        SolMirValueId wrapped = mir_instruction_result(lowerer,
            (SolMirInstruction){
                .kind = SOL_MIR_INST_CONSTRUCT_ARGUMENT,
                .type = lowerer->ir->expressions[operand->value].type,
                .source_expression = operand->value,
                .span = lowerer->ir->expressions[operand->value].span,
                .as.operand = lowered.value,
            });
        if (wrapped == SOL_MIR_NONE
            || !mir_append_construct_operand(lowerer,
                (SolMirConstructOperand){
                    .formal = operand->formal,
                    .source_expression = operand->value,
                    .value = wrapped,
                })) return mir_failure(lowerer);
        construct_operands.count = 1;
    }
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
            },
        });
    return result == SOL_MIR_NONE ? mir_failure(lowerer)
        : (LoweredValue){.reachable = true, .value = result};
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
    };
    const LoopContext *saved_loop = lowerer->loop;
    lowerer->loop = &context;
    if (is_while) {
        LoweredValue condition = mir_lower_expression(lowerer,
            statement->condition, scope);
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

static bool mir_span_valid(const SolIr *ir, SolSpan span) {
    return span.start <= span.end && span.end <= ir->source_length;
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
        case SOL_IR_EXPR_BINARY:
            return mir_expression_contains_statement(ir,
                    expression->as.binary.left, sought, depth + 1)
                || mir_expression_contains_statement(ir,
                    expression->as.binary.right, sought, depth + 1);
        case SOL_IR_EXPR_CALL:
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
        case SOL_IR_EXPR_BINARY:
            return mir_find_statement_loop_parent(ir,
                    expression->as.binary.left, sought, active_loop, parent,
                    depth + 1)
                || mir_find_statement_loop_parent(ir,
                    expression->as.binary.right, sought, active_loop, parent,
                    depth + 1);
        case SOL_IR_EXPR_CALL:
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
        SolMirSlice slices[2];
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
            if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
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
                || term->kind == SOL_MIR_TERM_UNREACHABLE)) {
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
            } else if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
                if (term->as.invoke.normal_edge.block != SOL_MIR_NONE) {
                    targets[target_count++] = term->as.invoke.normal_edge.block;
                }
                targets[target_count++] = term->as.invoke.failure_edge.block;
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
                || term->kind == SOL_MIR_TERM_UNREACHABLE)) valid = false;
        SolMirBlockId targets[2];
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
                            && next->as.store.local == instruction->as.local);
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
                || term->kind == SOL_MIR_TERM_UNREACHABLE)) valid = false;
        SolMirBlockId targets[2];
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
    free(statements);
    free(orders);
    return valid || mir_error(diagnostics,
        ir->callables[mir->callable].span,
        "MIR source control events are missing or reordered");
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
        || mir->block_count > mir->block_capacity
        || mir->instruction_count > mir->instruction_capacity
        || mir->value_count > mir->value_capacity
        || mir->parameter_value_count > mir->parameter_value_capacity
        || mir->edge_value_count > mir->edge_value_capacity
        || mir->call_argument_count > mir->call_argument_capacity
        || mir->loop_count > mir->loop_capacity
        || mir->construct_operand_count > mir->construct_operand_capacity
        || (mir->block_count != 0 && mir->blocks == NULL)
        || (mir->instruction_count != 0 && mir->instructions == NULL)
        || (mir->value_count != 0 && mir->values == NULL)
        || (mir->parameter_value_count != 0 && mir->parameter_values == NULL)
        || (mir->edge_value_count != 0 && mir->edge_values == NULL)
        || (mir->call_argument_count != 0 && mir->call_arguments == NULL)
        || (mir->loop_count != 0 && mir->loops == NULL)
        || (mir->construct_operand_count != 0
            && mir->construct_operands == NULL)) {
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
            || block->terminator.kind > SOL_MIR_TERM_CONTINUE
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
                || mir->values[id].source_expression >= ir->expression_count) {
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
                bool invoke_valid = source != NULL
                    && source->kind == SOL_IR_EXPR_CALL
                    && source->as.call.kind == SOL_IR_CALL_FUNCTION
                    && source->as.call.callable == term->as.invoke.callable
                    && term->as.invoke.callable < ir->callable_count
                    && ir->callables[term->as.invoke.callable].kind
                        == SOL_IR_CALLABLE_FUNCTION
                    && ir->callables[term->as.invoke.callable].generic_parameters.count == 0
                    && ir->callables[term->as.invoke.callable].effect_parameters.count == 0
                    && ir->callables[term->as.invoke.callable].receiver == SOL_IR_NONE
                    && ir->callables[term->as.invoke.callable].capability_source
                        == SOL_IR_NONE
                    && source->type
                        == ir->callables[term->as.invoke.callable].result
                    && source->as.call.type_arguments.count == 0
                    && source->as.call.evidence.count == 0
                    && source->as.call.effect_parameter == SOL_IR_NONE
                    && mir_slice_valid(term->as.invoke.arguments,
                        mir->call_argument_count)
                    && term->as.invoke.arguments.count
                        == source->as.call.operands.count
                    && term->as.invoke.arguments.count
                        == ir->callables[term->as.invoke.callable].parameters.count
                    && term->as.invoke.failure_edge.arguments.count == 0
                    && mir_validate_edge(mir, block_id,
                        &term->as.invoke.failure_edge, SOL_MIR_NONE)
                    && mir->blocks[term->as.invoke.failure_edge.block]
                        .terminator.kind == SOL_MIR_TERM_RESUME_FAILURE;
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
                        && source->type
                            == ir->callables[term->as.invoke.callable].result
                        && mir_validate_edge(mir, block_id,
                            &term->as.invoke.normal_edge, result)
                        && term->as.invoke.normal_edge.arguments.count == 1
                        && mir->edge_values[
                            term->as.invoke.normal_edge.arguments.offset]
                            == result;
                }
                for (size_t index = 0; invoke_valid
                    && index < term->as.invoke.arguments.count; ++index) {
                    const SolMirCallArgument *argument = &mir->call_arguments[
                        term->as.invoke.arguments.offset + index];
                    const SolIrOperand *operand = &ir->operands[
                        source->as.call.operands.offset + index];
                    SolIrLocalId formal = ir->roots[
                        ir->callables[term->as.invoke.callable].parameters.offset
                            + index];
                    invoke_valid = argument->formal == operand->formal
                        && argument->formal == index
                        && argument->access == operand->access
                        && argument->access == ir->locals[formal].access
                        && argument->source_expression == operand->value
                        && operand->value < ir->expression_count
                        && ir->expressions[operand->value].type
                            == ir->locals[formal].type;
                    if (!invoke_valid) break;
                    if (argument->access == SOL_ACCESS_OWNED) {
                        invoke_valid = argument->place == SOL_IR_NONE
                            && mir_value_available(mir, argument->value,
                                block_id, block->instructions.offset
                                    + block->instructions.count)
                            && mir->values[argument->value].type
                                == ir->expressions[operand->value].type
                            && mir->values[argument->value].kind
                                == SOL_MIR_VALUE_INSTRUCTION
                            && mir->instructions[mir->values[
                                argument->value].definition].kind
                                == SOL_MIR_INST_CALL_ARGUMENT
                            && mir->instructions[mir->values[
                                argument->value].definition].source_expression
                                == operand->value
                            && mir->instructions[mir->values[
                                argument->value].definition].as.operand
                                < mir->value_count
                            && mir->values[mir->instructions[mir->values[
                                argument->value].definition].as.operand]
                                .source_expression == operand->value;
                    } else {
                        SolIrLocalUse use = argument->access == SOL_ACCESS_SHARED
                            ? SOL_IR_LOCAL_USE_SHARED
                            : SOL_IR_LOCAL_USE_EXCLUSIVE;
                        const SolIrExpression *actual
                            = &ir->expressions[operand->value];
                        invoke_valid = (argument->access == SOL_ACCESS_SHARED
                                || argument->access == SOL_ACCESS_EXCLUSIVE)
                            && argument->value == SOL_MIR_NONE
                            && argument->place < ir->place_count
                            && actual->kind == SOL_IR_EXPR_PLACE
                            && actual->local_use == use
                            && actual->as.place == argument->place
                            && ir->places[argument->place].root_kind
                                == SOL_IR_PLACE_ROOT_LOCAL
                            && ir->places[argument->place].projections.count == 0
                            && ir->locals[ir->places[argument->place].local].owner
                                == callable->owner;
                    }
                }
                for (size_t left = 0; invoke_valid
                    && left < term->as.invoke.arguments.count; ++left) {
                    const SolMirCallArgument *a = &mir->call_arguments[
                        term->as.invoke.arguments.offset + left];
                    if (a->access == SOL_ACCESS_OWNED) continue;
                    for (size_t right = left + 1;
                        right < term->as.invoke.arguments.count; ++right) {
                        const SolMirCallArgument *b = &mir->call_arguments[
                            term->as.invoke.arguments.offset + right];
                        if (b->access != SOL_ACCESS_OWNED
                            && ir->places[a->place].local
                                == ir->places[b->place].local
                            && (a->access == SOL_ACCESS_EXCLUSIVE
                                || b->access == SOL_ACCESS_EXCLUSIVE)) {
                            invoke_valid = false;
                            break;
                        }
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
            || instruction->kind > SOL_MIR_INST_CONSTRUCT
            || !mir_span_valid(ir, instruction->span)) {
            return mir_error(diagnostics, instruction->span,
                "malformed MIR instruction");
        }
        bool result_kind = instruction->kind <= SOL_MIR_INST_CONST_UNIT
            || instruction->kind == SOL_MIR_INST_LOAD_COPY
            || instruction->kind == SOL_MIR_INST_LOAD_MOVE
            || instruction->kind == SOL_MIR_INST_UNARY
            || instruction->kind == SOL_MIR_INST_BINARY
            || instruction->kind == SOL_MIR_INST_CALL_ARGUMENT
            || instruction->kind == SOL_MIR_INST_CONSTRUCT_ARGUMENT
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
        } else if (instruction->kind == SOL_MIR_INST_CALL_ARGUMENT) {
            if (source == NULL
                || !mir_value_available(mir, instruction->as.operand,
                    instruction->block, id)
                || mir->values[instruction->as.operand].type
                    != source->type
                || mir->values[instruction->as.operand].source_expression
                    != instruction->source_expression) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR owned call argument");
            }
        } else if (instruction->kind == SOL_MIR_INST_CONSTRUCT_ARGUMENT) {
            if (source == NULL
                || !mir_value_available(mir, instruction->as.operand,
                    instruction->block, id)
                || mir->values[instruction->as.operand].type
                    != source->type
                || mir->values[instruction->as.operand].source_expression
                    != instruction->source_expression
                || instruction->span.start != source->span.start
                || instruction->span.end != source->span.end
                || instruction->result >= mir->value_count
                || mir->values[instruction->result].span.start
                    != instruction->span.start
                || mir->values[instruction->result].span.end
                    != instruction->span.end) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR owned construction argument");
            }
        } else if (instruction->kind == SOL_MIR_INST_CONSTRUCT) {
            SolMirSlice operands = instruction->as.construct.operands;
            SolIrSlice source_operands = {0};
            SolMirConstructKind expected_kind = instruction->as.construct.kind;
            SolIrDefinitionId expected_definition = SOL_IR_NONE;
            SolIrVariantId expected_variant = SOL_IR_NONE;
            bool shape = source != NULL
                && instruction->span.start == source->span.start
                && instruction->span.end == source->span.end
                && source->capability_roots.count == 0
                && source->operation_roots.count == 0
                && mir_slice_valid(operands, mir->construct_operand_count)
                && operands.count <= 1;
            if (shape && source->kind == SOL_IR_EXPR_RECORD) {
                expected_kind = SOL_MIR_CONSTRUCT_RECORD;
                expected_definition = source->as.record.definition;
                source_operands = source->as.record.fields;
                shape = expected_definition < ir->definition_count
                    && ir->definitions[expected_definition].kind
                        == SOL_IR_DEFINITION_RECORD;
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
                    && operand->value < mir->value_count
                    && mir_value_available(mir, operand->value,
                        instruction->block, id)
                    && mir->values[operand->value].kind
                        == SOL_MIR_VALUE_INSTRUCTION
                    && mir->values[operand->value].source_expression
                        == source_operand->value
                    && mir->values[operand->value].type
                        == ir->expressions[source_operand->value].type
                    && mir->instructions[mir->values[operand->value].definition]
                        .kind == SOL_MIR_INST_CONSTRUCT_ARGUMENT;
            }
            if (!shape) {
                return mir_error(diagnostics, instruction->span,
                    "malformed MIR semantic construction");
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
        }
    }
    for (size_t id = 0; id < mir->value_count; ++id) {
        const SolMirValue *value = &mir->values[id];
        if ((int)value->kind < 0 || value->kind > SOL_MIR_VALUE_TERMINATOR
            || value->type >= ir->type_count || value->block >= mir->block_count
            || (value->source_expression != SOL_IR_NONE
                && (value->source_expression >= ir->expression_count
                    || ir->expressions[value->source_expression].type
                        != value->type))
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
        } else if (term->kind == SOL_MIR_TERM_INVOKE) {
            if (term->as.invoke.normal_edge.block != SOL_MIR_NONE) {
                ++predecessors[term->as.invoke.normal_edge.block];
            }
            ++predecessors[term->as.invoke.failure_edge.block];
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
        SolMirBlockId targets[2];
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
    return mir_validate_arena_ownership(mir, diagnostics, callable->span)
        && mir_validate_storage(ir, mir, diagnostics)
        && mir_validate_regions(ir, mir, diagnostics)
        && mir_validate_storage_order(ir, mir, diagnostics)
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
