#include "sol/ownership.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const SolIr *ir;
    SolDiagnostics *diagnostics;
    SolIrLocalUse *uses;
    unsigned char *copy_states;
    size_t *shared_loans;
    bool *exclusive_loans;
    SolIrDefinitionId owner;
    size_t region_depth;
    bool validating;
} Ownership;

static bool ownership_error_code(Ownership *analysis, SolSpan span,
    const char *code, const char *message) {
    if (analysis->diagnostics != NULL) {
        sol_diagnostics_add(analysis->diagnostics,
            analysis->validating ? "SOL-INTERNAL-006" : code,
            SOL_SEVERITY_ERROR, span, "%s", message);
    }
    return false;
}

static bool ownership_error(Ownership *analysis, SolSpan span,
    const char *message) {
    return ownership_error_code(analysis, span, "SOL-OWNERSHIP-001", message);
}

static bool ownership_internal(Ownership *analysis, SolSpan span,
    const char *message) {
    if (analysis->diagnostics != NULL) {
        sol_diagnostics_add(analysis->diagnostics, "SOL-INTERNAL-006",
            SOL_SEVERITY_ERROR, span, "%s", message);
    }
    return false;
}

static bool type_is_copy(Ownership *analysis, SolIrTypeId id) {
    return id < analysis->ir->type_count && analysis->copy_states[id] == 2;
}

static bool copy_dependencies_hold(Ownership *analysis, SolIrTypeId id) {
    const SolIrType *type = &analysis->ir->types[id];
    if (type->kind == SOL_IR_TYPE_OPTION || type->kind == SOL_IR_TYPE_RESULT) {
        for (size_t index = 0; index < type->argument_count; ++index) {
            if (!type_is_copy(analysis,
                analysis->ir->type_ids[type->argument_offset + index])) return false;
        }
        return true;
    }
    if (type->kind != SOL_IR_TYPE_NOMINAL
        || type->definition >= analysis->ir->definition_count) return true;
    const SolIrDefinition *definition = &analysis->ir->definitions[type->definition];
    if (definition->kind == SOL_IR_DEFINITION_RECORD) {
        for (size_t index = 0; index < definition->fields.count; ++index) {
            if (!type_is_copy(analysis,
                analysis->ir->fields[definition->fields.offset + index].type)) return false;
        }
    } else if (definition->kind == SOL_IR_DEFINITION_ENUM) {
        for (size_t variant = 0; variant < definition->variants.count; ++variant) {
            SolIrSlice fields = analysis->ir->variants[
                definition->variants.offset + variant].fields;
            for (size_t field = 0; field < fields.count; ++field) {
                if (!type_is_copy(analysis,
                    analysis->ir->fields[fields.offset + field].type)) return false;
            }
        }
    } else if (definition->representation >= analysis->ir->type_count
        || !type_is_copy(analysis, definition->representation)) return false;
    return true;
}

static void compute_copy_states(Ownership *analysis) {
    for (size_t id = 0; id < analysis->ir->type_count; ++id) {
        const SolIrType *type = &analysis->ir->types[id];
        bool candidate = type->kind == SOL_IR_TYPE_INT64
            || type->kind == SOL_IR_TYPE_BOOL || type->kind == SOL_IR_TYPE_TEXT
            || type->kind == SOL_IR_TYPE_UNIT || type->kind == SOL_IR_TYPE_NEVER
            || type->kind == SOL_IR_TYPE_OPTION || type->kind == SOL_IR_TYPE_RESULT;
        if (type->kind == SOL_IR_TYPE_NOMINAL
            && type->definition < analysis->ir->definition_count) {
            const SolIrDefinition *definition
                = &analysis->ir->definitions[type->definition];
            candidate = definition->generic_parameters.count == 0
                && (definition->kind == SOL_IR_DEFINITION_RECORD
                    || definition->kind == SOL_IR_DEFINITION_ENUM
                    || definition->kind == SOL_IR_DEFINITION_DISTINCT
                    || definition->kind == SOL_IR_DEFINITION_REFINED);
        }
        analysis->copy_states[id] = candidate ? 2 : 3;
    }
    bool changed;
    do {
        changed = false;
        for (size_t id = 0; id < analysis->ir->type_count; ++id) {
            if (analysis->copy_states[id] == 2
                && !copy_dependencies_hold(analysis, id)) {
                analysis->copy_states[id] = 3;
                changed = true;
            }
        }
    } while (changed);
}

static bool analyze_expression(Ownership *analysis, SolIrExpressionId id,
    bool *available, SolAccessMode access, bool *reachable);

static bool analyze_child(Ownership *analysis, SolIrExpressionId id,
    bool *available, SolAccessMode access, bool *reachable) {
    return analyze_expression(analysis, id, available, access, reachable);
}

static bool begin_loan(Ownership *analysis, SolIrExpressionId id,
    bool *available, SolAccessMode access, SolIrLocalId *local_out) {
    if (id >= analysis->ir->expression_count
        || analysis->ir->expressions[id].kind != SOL_IR_EXPR_LOCAL) {
        return ownership_error_code(analysis, id < analysis->ir->expression_count
            ? analysis->ir->expressions[id].span : (SolSpan){0},
            "SOL-OWNERSHIP-004", "borrowed arguments must be direct local places");
    }
    const SolIrExpression *expression = &analysis->ir->expressions[id];
    SolIrLocalId local = expression->as.local;
    if (local >= analysis->ir->local_count || !available[local]) {
        return ownership_error(analysis, expression->span,
            "borrowed local is used after it was moved");
    }
    SolAccessMode source = analysis->ir->locals[local].access;
    if ((source == SOL_ACCESS_SHARED && access == SOL_ACCESS_EXCLUSIVE)
        || (source != SOL_ACCESS_OWNED && access == SOL_ACCESS_OWNED)) {
        return ownership_error_code(analysis, expression->span,
            "SOL-OWNERSHIP-004", "invalid reborrow of borrowed parameter");
    }
    if (access == SOL_ACCESS_EXCLUSIVE
        ? analysis->exclusive_loans[local] || analysis->shared_loans[local] != 0
        : analysis->exclusive_loans[local]) {
        return ownership_error_code(analysis, expression->span,
            "SOL-OWNERSHIP-002", "borrow conflicts with an overlapping loan");
    }
    analysis->uses[id] = access == SOL_ACCESS_SHARED
        ? SOL_IR_LOCAL_USE_SHARED : SOL_IR_LOCAL_USE_EXCLUSIVE;
    if (access == SOL_ACCESS_SHARED) ++analysis->shared_loans[local];
    else analysis->exclusive_loans[local] = true;
    *local_out = local;
    return true;
}

static void end_loan(Ownership *analysis, SolIrLocalId local, SolAccessMode access) {
    if (access == SOL_ACCESS_SHARED) --analysis->shared_loans[local];
    else analysis->exclusive_loans[local] = false;
}

static bool validate_borrow_places(Ownership *analysis) {
    for (size_t index = 0; index < analysis->ir->expression_count; ++index) {
        const SolIrExpression *expression = &analysis->ir->expressions[index];
        if (expression->kind == SOL_IR_EXPR_CALL) {
            if (expression->as.call.kind == SOL_IR_CALL_METHOD
                && expression->as.call.receiver_access != SOL_ACCESS_OWNED
                && analysis->ir->expressions[expression->as.call.receiver].kind
                    != SOL_IR_EXPR_LOCAL) {
                return ownership_error_code(analysis, expression->span,
                    "SOL-OWNERSHIP-004", "borrowed receiver must be a direct local place");
            }
            if ((expression->as.call.kind == SOL_IR_CALL_CAPABILITY
                    || expression->as.call.kind == SOL_IR_CALL_CALLBACK)
                && analysis->ir->expressions[expression->as.call.callee].kind
                    == SOL_IR_EXPR_BOUND_OPERATION) {
                SolIrExpressionId receiver = analysis->ir->expressions[
                    expression->as.call.callee].as.operation.receiver;
                if (analysis->ir->expressions[receiver].kind != SOL_IR_EXPR_LOCAL) {
                    return ownership_error_code(analysis, expression->span,
                        "SOL-OWNERSHIP-004",
                        "borrowed capability receiver must be a direct local place");
                }
            }
            for (size_t operand = 0; operand < expression->as.call.operands.count;
                ++operand) {
                const SolIrOperand *entry = &analysis->ir->operands[
                    expression->as.call.operands.offset + operand];
                if (entry->access != SOL_ACCESS_OWNED
                    && analysis->ir->expressions[entry->value].kind
                        != SOL_IR_EXPR_LOCAL) {
                    return ownership_error_code(analysis, expression->span,
                        "SOL-OWNERSHIP-004",
                        "borrowed arguments must be direct local places");
                }
            }
        } else if (expression->kind == SOL_IR_EXPR_HANDLE
            && (analysis->ir->expressions[expression->as.handler.authority].kind
                    != SOL_IR_EXPR_LOCAL
                || analysis->ir->expressions[expression->as.handler.provider].kind
                    != SOL_IR_EXPR_LOCAL)) {
            return ownership_error_code(analysis, expression->span,
                "SOL-OWNERSHIP-004",
                "handler authority and provider must be direct local places");
        }
    }
    return true;
}

static void join_states(bool *destination, const bool *left, const bool *right,
    size_t count) {
    for (size_t index = 0; index < count; ++index) {
        destination[index] = left[index] && right[index];
    }
}

static bool analyze_branches(Ownership *analysis, SolIrExpressionId left_id,
    SolIrExpressionId right_id, bool *available, bool *reachable) {
    size_t count = analysis->ir->local_count;
    bool *left = count == 0 ? NULL : malloc(count * sizeof(*left));
    bool *right = count == 0 ? NULL : malloc(count * sizeof(*right));
    if (count != 0 && (left == NULL || right == NULL)) {
        free(left);
        free(right);
        return ownership_internal(analysis, (SolSpan){0},
            "ownership branch allocation failed");
    }
    if (count != 0) {
        memcpy(left, available, count * sizeof(*left));
        memcpy(right, available, count * sizeof(*right));
    }
    bool left_reachable = true;
    bool right_reachable = true;
    bool valid = analyze_expression(analysis, left_id, left, SOL_ACCESS_OWNED, &left_reachable)
        && analyze_expression(analysis, right_id, right, SOL_ACCESS_OWNED, &right_reachable);
    if (valid) {
        if (left_reachable && right_reachable) join_states(available, left, right, count);
        else if (left_reachable && count != 0) memcpy(available, left, count * sizeof(*left));
        else if (right_reachable && count != 0) memcpy(available, right, count * sizeof(*right));
        *reachable = left_reachable || right_reachable;
    }
    free(left);
    free(right);
    return valid;
}

static bool analyze_match(Ownership *analysis, const SolIrExpression *expression,
    bool *available, bool *reachable) {
    if (!analyze_expression(analysis, expression->as.match_expr.scrutinee,
        available, SOL_ACCESS_OWNED, reachable) || !*reachable) return *reachable == false;
    size_t count = analysis->ir->local_count;
    bool *baseline = count == 0 ? NULL : malloc(count * sizeof(*baseline));
    bool *branch = count == 0 ? NULL : malloc(count * sizeof(*branch));
    bool *joined = count == 0 ? NULL : malloc(count * sizeof(*joined));
    if (count != 0 && (baseline == NULL || branch == NULL || joined == NULL)) {
        free(baseline); free(branch); free(joined);
        return ownership_internal(analysis, expression->span,
            "ownership match allocation failed");
    }
    if (count != 0) memcpy(baseline, available, count * sizeof(*baseline));
    bool has_join = false;
    bool valid = true;
    for (size_t index = 0; valid && index < expression->as.match_expr.arms.count;
        ++index) {
        const SolIrArm *arm = &analysis->ir->arms[analysis->ir->arm_ids[
            expression->as.match_expr.arms.offset + index]];
        if (count != 0) memcpy(branch, baseline, count * sizeof(*branch));
        for (size_t binding = 0; binding < arm->bindings.count; ++binding) {
            branch[analysis->ir->roots[arm->bindings.offset + binding]] = true;
        }
        bool branch_reachable = true;
        valid = analyze_expression(analysis, arm->value, branch, SOL_ACCESS_OWNED,
            &branch_reachable);
        for (size_t binding = 0; binding < arm->bindings.count; ++binding) {
            branch[analysis->ir->roots[arm->bindings.offset + binding]] = false;
        }
        if (!valid || !branch_reachable) continue;
        if (!has_join && count != 0) memcpy(joined, branch, count * sizeof(*joined));
        else if (has_join) join_states(joined, joined, branch, count);
        has_join = true;
    }
    if (valid && has_join && count != 0) memcpy(available, joined, count * sizeof(*joined));
    *reachable = valid && has_join;
    free(baseline); free(branch); free(joined);
    return valid;
}

static bool analyze_expression(Ownership *analysis, SolIrExpressionId id,
    bool *available, SolAccessMode access, bool *reachable) {
    if (id >= analysis->ir->expression_count) {
        return ownership_internal(analysis, (SolSpan){0},
            "ownership expression is out of range");
    }
    if (!*reachable) return true;
    const SolIrExpression *expression = &analysis->ir->expressions[id];
    switch (expression->kind) {
        case SOL_IR_EXPR_LOCAL: {
            SolIrLocalId local = expression->as.local;
            if (local >= analysis->ir->local_count
                || analysis->ir->locals[local].owner != analysis->owner) {
                return ownership_internal(analysis, expression->span,
                    "local ownership is inconsistent with its callable");
            }
            if (access != SOL_ACCESS_OWNED) {
                SolIrLocalId borrowed;
                return begin_loan(analysis, id, available, access, &borrowed);
            }
            SolIrLocalUse use = type_is_copy(analysis, analysis->ir->locals[local].type)
                    ? SOL_IR_LOCAL_USE_COPY : SOL_IR_LOCAL_USE_MOVE;
            analysis->uses[id] = use;
            if (!available[local]) {
                char message[256];
                const char *name = analysis->ir->locals[local].name;
                int written = snprintf(message, sizeof(message),
                    "local '%s' is used after it was moved", name);
                if (written < 0) return false;
                return ownership_error(analysis, expression->span, message);
            }
            if (analysis->exclusive_loans[local]
                || (analysis->shared_loans[local] != 0
                    && use == SOL_IR_LOCAL_USE_MOVE)) {
                return ownership_error_code(analysis, expression->span,
                    "SOL-OWNERSHIP-003", "owned use or move occurs while local is borrowed");
            }
            if (analysis->ir->locals[local].access != SOL_ACCESS_OWNED
                && use == SOL_IR_LOCAL_USE_MOVE) {
                return ownership_error_code(analysis, expression->span,
                    "SOL-OWNERSHIP-004", "borrowed affine parameter cannot escape or be consumed");
            }
            if (use == SOL_IR_LOCAL_USE_MOVE) available[local] = false;
            return true;
        }
        case SOL_IR_EXPR_BOUND_OPERATION:
            return analyze_child(analysis, expression->as.operation.receiver,
                available, SOL_ACCESS_OWNED, reachable);
        case SOL_IR_EXPR_UNARY:
            return analyze_expression(analysis, expression->as.unary.operand,
                available, SOL_ACCESS_OWNED, reachable);
        case SOL_IR_EXPR_BINARY:
            if (!analyze_expression(analysis, expression->as.binary.left,
                available, SOL_ACCESS_OWNED, reachable) || !*reachable) return *reachable == false;
            if (expression->as.binary.operator_kind == SOL_TOKEN_AMP_AMP
                || expression->as.binary.operator_kind == SOL_TOKEN_PIPE_PIPE) {
                size_t count = analysis->ir->local_count;
                bool *skipped = count == 0 ? NULL : malloc(count * sizeof(*skipped));
                if (count != 0 && skipped == NULL) return ownership_internal(
                    analysis, expression->span, "ownership join allocation failed");
                if (count != 0) memcpy(skipped, available, count * sizeof(*skipped));
                bool right_reachable = true;
                bool valid = analyze_expression(analysis, expression->as.binary.right,
                    available, SOL_ACCESS_OWNED, &right_reachable);
                if (valid && right_reachable) join_states(available, skipped, available, count);
                else if (valid && count != 0) memcpy(available, skipped,
                    count * sizeof(*skipped));
                *reachable = valid;
                free(skipped);
                return valid;
            }
            return analyze_expression(analysis, expression->as.binary.right,
                available, SOL_ACCESS_OWNED, reachable);
        case SOL_IR_EXPR_CALL:
            {
            size_t loan_capacity = expression->as.call.operands.count + 2;
            if (loan_capacity < expression->as.call.operands.count
                || loan_capacity > SIZE_MAX / sizeof(SolIrLocalId)
                || loan_capacity > SIZE_MAX / sizeof(SolAccessMode)) {
                return ownership_internal(analysis, expression->span,
                    "ownership call loan allocation is too large");
            }
            SolIrLocalId *loaned = malloc(loan_capacity * sizeof(*loaned));
            SolAccessMode *modes = malloc(loan_capacity * sizeof(*modes));
            if (loaned == NULL || modes == NULL) {
                free(loaned);
                free(modes);
                return ownership_internal(analysis, expression->span,
                    "ownership call loan allocation failed");
            }
            size_t loan_count = 0;
            bool valid = true;
            bool direct_operation = (expression->as.call.kind == SOL_IR_CALL_CAPABILITY
                    || expression->as.call.kind == SOL_IR_CALL_CALLBACK)
                && expression->as.call.callee < analysis->ir->expression_count
                && analysis->ir->expressions[expression->as.call.callee].kind
                    == SOL_IR_EXPR_BOUND_OPERATION;
            if (direct_operation) {
                SolIrExpressionId receiver = analysis->ir->expressions[
                    expression->as.call.callee].as.operation.receiver;
                SolAccessMode receiver_access
                    = expression->as.call.kind == SOL_IR_CALL_CAPABILITY
                    ? expression->as.call.receiver_access : SOL_ACCESS_SHARED;
                if (!begin_loan(analysis, receiver, available, receiver_access,
                    &loaned[loan_count])) valid = false;
                if (!valid) goto call_complete;
                modes[loan_count++] = receiver_access;
            } else if ((expression->as.call.kind == SOL_IR_CALL_CALLBACK
                    || expression->as.call.kind == SOL_IR_CALL_CAPABILITY)
                && !analyze_expression(analysis, expression->as.call.callee,
                    available, SOL_ACCESS_OWNED, reachable)) {
                valid = false;
                goto call_complete;
            }
            if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
                if (expression->as.call.receiver_access == SOL_ACCESS_OWNED) {
                    if (!analyze_expression(analysis, expression->as.call.receiver,
                        available, SOL_ACCESS_OWNED, reachable)) {
                        valid = false;
                        goto call_complete;
                    }
                } else {
                    if (!begin_loan(analysis, expression->as.call.receiver, available,
                        expression->as.call.receiver_access,
                        &loaned[loan_count])) {
                        valid = false;
                        goto call_complete;
                    }
                    modes[loan_count++] = expression->as.call.receiver_access;
                }
            }
            for (size_t index = 0; *reachable
                && index < expression->as.call.operands.count; ++index) {
                const SolIrOperand *operand = &analysis->ir->operands[
                    expression->as.call.operands.offset + index];
                if (operand->access == SOL_ACCESS_OWNED) {
                    if (!analyze_expression(analysis, operand->value, available,
                        SOL_ACCESS_OWNED, reachable)) {
                        valid = false;
                        goto call_complete;
                    }
                } else {
                    if (!begin_loan(analysis, operand->value,
                        available, operand->access, &loaned[loan_count])) {
                        valid = false;
                        goto call_complete;
                    }
                    modes[loan_count++] = operand->access;
                }
            }
call_complete:
            while (loan_count != 0) {
                --loan_count;
                end_loan(analysis, loaned[loan_count], modes[loan_count]);
            }
            free(loaned);
            free(modes);
            return valid;
            }
        case SOL_IR_EXPR_RECORD:
            for (size_t index = 0; *reachable
                && index < expression->as.record.fields.count; ++index) {
                if (!analyze_expression(analysis, analysis->ir->operands[
                    expression->as.record.fields.offset + index].value,
                    available, SOL_ACCESS_OWNED, reachable)) return false;
            }
            return true;
        case SOL_IR_EXPR_FIELD:
            return analyze_expression(analysis, expression->as.field.base,
                available, SOL_ACCESS_OWNED, reachable);
        case SOL_IR_EXPR_IF:
            if (!analyze_expression(analysis, expression->as.if_expr.condition,
                available, SOL_ACCESS_OWNED, reachable) || !*reachable) return *reachable == false;
            return analyze_branches(analysis, expression->as.if_expr.then_branch,
                expression->as.if_expr.else_branch, available, reachable);
        case SOL_IR_EXPR_MATCH:
            return analyze_match(analysis, expression, available, reachable);
        case SOL_IR_EXPR_BLOCK:
            for (size_t index = 0; *reachable
                && index < expression->as.block.statements.count;
                ++index) {
                const SolIrStatement *statement = &analysis->ir->statements[
                    analysis->ir->statement_ids[
                        expression->as.block.statements.offset + index]];
                if (statement->kind == SOL_IR_STATEMENT_REGION) {
                    ++analysis->region_depth;
                }
                if (!analyze_expression(analysis, statement->expression,
                    available, SOL_ACCESS_OWNED, reachable)) {
                    if (statement->kind == SOL_IR_STATEMENT_REGION) {
                        --analysis->region_depth;
                    }
                    return false;
                }
                if (statement->kind == SOL_IR_STATEMENT_REGION) {
                    --analysis->region_depth;
                }
                if (*reachable && statement->kind == SOL_IR_STATEMENT_LET) {
                    available[statement->local] = true;
                } else if (*reachable && statement->kind == SOL_IR_STATEMENT_RETURN) {
                    if (analysis->region_depth != 0
                        && !type_is_copy(analysis,
                            analysis->ir->expressions[statement->expression].type)) {
                        return ownership_error_code(analysis, statement->span,
                            "SOL-REGION-001",
                            "an affine value cannot return from an explicit region");
                    }
                    *reachable = false;
                }
            }
            for (size_t index = 0; index < expression->as.block.cleanup.count; ++index) {
                available[analysis->ir->cleanup_locals[
                    expression->as.block.cleanup.offset + index]] = false;
            }
            return true;
        case SOL_IR_EXPR_PROPAGATE:
            if (analysis->region_depth != 0
                && expression->as.propagate.kind == SOL_IR_PROPAGATE_RESULT
                && !type_is_copy(analysis, expression->as.propagate.residual)) {
                return ownership_error_code(analysis, expression->span,
                    "SOL-REGION-001",
                    "an affine propagation residual cannot leave an explicit region");
            }
            return analyze_expression(analysis, expression->as.propagate.operand,
                available, SOL_ACCESS_OWNED, reachable);
        case SOL_IR_EXPR_HANDLE:
            {
            SolIrLocalId authority, provider;
            if (!begin_loan(analysis, expression->as.handler.authority, available,
                    SOL_ACCESS_SHARED, &authority)
                || !begin_loan(analysis, expression->as.handler.provider, available,
                    SOL_ACCESS_SHARED, &provider)) return false;
            bool valid = analyze_expression(analysis, expression->as.handler.body,
                available, SOL_ACCESS_OWNED, reachable);
            end_loan(analysis, provider, SOL_ACCESS_SHARED);
            end_loan(analysis, authority, SOL_ACCESS_SHARED);
            return valid;
            }
        default:
            return true;
    }
}

static bool run_ownership(Ownership *analysis) {
    size_t local_count = analysis->ir->local_count;
    bool *available = local_count == 0 ? NULL
        : calloc(local_count, sizeof(*available));
    if (local_count != 0 && available == NULL) return ownership_internal(
        analysis, (SolSpan){0}, "ownership state allocation failed");
    bool valid = validate_borrow_places(analysis);
    for (size_t index = 0; valid && index < analysis->ir->callable_count; ++index) {
        const SolIrCallable *callable = &analysis->ir->callables[index];
        if (callable->body == SOL_IR_NONE) continue;
        if (local_count != 0) memset(available, 0,
            local_count * sizeof(*available));
        analysis->owner = callable->owner;
        analysis->region_depth = 0;
        for (size_t parameter = 0; parameter < callable->parameters.count; ++parameter) {
            available[analysis->ir->roots[callable->parameters.offset + parameter]] = true;
        }
        if (callable->receiver != SOL_IR_NONE) available[callable->receiver] = true;
        if (callable->capability_source != SOL_IR_NONE) {
            available[callable->capability_source] = true;
        }
        bool reachable = true;
        valid = analyze_expression(analysis, callable->body, available, SOL_ACCESS_OWNED,
            &reachable);
    }
    free(available);
    return valid;
}

static bool ownership_prepare(const SolIr *ir, SolDiagnostics *diagnostics,
    bool validating, Ownership *analysis) {
    memset(analysis, 0, sizeof(*analysis));
    analysis->ir = ir;
    analysis->diagnostics = diagnostics;
    analysis->validating = validating;
    analysis->uses = ir->expression_count == 0 ? NULL
        : calloc(ir->expression_count, sizeof(*analysis->uses));
    analysis->copy_states = ir->type_count == 0 ? NULL
        : calloc(ir->type_count, sizeof(*analysis->copy_states));
    analysis->shared_loans = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*analysis->shared_loans));
    analysis->exclusive_loans = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*analysis->exclusive_loans));
    if ((ir->expression_count != 0 && analysis->uses == NULL)
        || (ir->type_count != 0 && analysis->copy_states == NULL)
        || (ir->local_count != 0
            && (analysis->shared_loans == NULL || analysis->exclusive_loans == NULL))) {
        free(analysis->uses);
        free(analysis->copy_states);
        free(analysis->shared_loans);
        free(analysis->exclusive_loans);
        return ownership_internal(analysis, (SolSpan){0},
            "ownership metadata allocation failed");
    }
    compute_copy_states(analysis);
    return true;
}

bool sol_ir_analyze_ownership(SolIr *ir, SolDiagnostics *diagnostics) {
    if (ir == NULL) return false;
    Ownership analysis;
    if (!ownership_prepare(ir, diagnostics, false, &analysis)) return false;
    bool valid = run_ownership(&analysis);
    if (valid) {
        for (size_t index = 0; index < ir->expression_count; ++index) {
            ir->expressions[index].local_use = analysis.uses[index];
        }
    }
    free(analysis.uses);
    free(analysis.copy_states);
    free(analysis.shared_loans);
    free(analysis.exclusive_loans);
    return valid;
}

bool sol_ir_validate_ownership(const SolIr *ir, SolDiagnostics *diagnostics) {
    if (ir == NULL) return false;
    Ownership analysis;
    if (!ownership_prepare(ir, diagnostics, true, &analysis)) return false;
    bool valid = run_ownership(&analysis);
    for (size_t index = 0; valid && index < ir->expression_count; ++index) {
        if (ir->expressions[index].local_use > SOL_IR_LOCAL_USE_EXCLUSIVE
            || ir->expressions[index].local_use != analysis.uses[index]) {
            valid = ownership_error(&analysis, ir->expressions[index].span,
                "IR local-use ownership metadata is inconsistent");
        }
    }
    free(analysis.uses);
    free(analysis.copy_states);
    free(analysis.shared_loans);
    free(analysis.exclusive_loans);
    return valid;
}
