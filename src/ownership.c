#include "sol/ownership.h"

#include <stdio.h>
#include <stdlib.h>
#include "resource_internal.h"
#include <string.h>

typedef struct OwnershipLoop OwnershipLoop;

struct OwnershipLoop {
    OwnershipLoop *parent;
    bool *entry_introduced;
    bool *break_unavailable;
    bool *break_initialized;
    bool *back_unavailable;
    bool *back_initialized;
    bool has_break;
    bool has_back;
};

typedef struct {
    const SolIr *ir;
    SolDiagnostics *diagnostics;
    SolIrLocalUse *uses;
    bool *copy_states;
    SolIrExpressionId *loan_places;
    SolAccessMode *loan_modes;
    size_t loan_count;
    bool *introduced;
    bool *initialized;
    size_t *modify_depths;
    size_t *introduction_depths;
    SolIrDefinitionId owner;
    size_t region_depth;
    bool validating;
    OwnershipLoop *loop;
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
    return id < analysis->ir->type_count && analysis->copy_states[id];
}

static bool copy_dependencies_hold(Ownership *analysis, SolIrTypeId id) {
    const SolIrType *type = &analysis->ir->types[id];
    if (type->kind == SOL_IR_TYPE_OPTION || type->kind == SOL_IR_TYPE_RESULT
        || type->kind == SOL_IR_TYPE_TUPLE) {
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

bool sol_ir_compute_copyability(const SolIr *ir, bool *copyable, size_t count) {
    if (ir == NULL || count != ir->type_count
        || (count != 0 && copyable == NULL)) return false;
    Ownership analysis = {.ir = ir, .copy_states = copyable};
    for (size_t id = 0; id < ir->type_count; ++id) {
        const SolIrType *type = &ir->types[id];
        bool candidate = type->kind == SOL_IR_TYPE_INT64
            || type->kind == SOL_IR_TYPE_BOOL || type->kind == SOL_IR_TYPE_TEXT
            || type->kind == SOL_IR_TYPE_UNIT || type->kind == SOL_IR_TYPE_NEVER
            || type->kind == SOL_IR_TYPE_OPTION || type->kind == SOL_IR_TYPE_RESULT
            || type->kind == SOL_IR_TYPE_TUPLE;
        if (type->kind == SOL_IR_TYPE_NOMINAL
            && type->definition < ir->definition_count) {
            const SolIrDefinition *definition
                = &ir->definitions[type->definition];
            candidate = definition->generic_parameters.count == 0
                && (definition->kind == SOL_IR_DEFINITION_RECORD
                    || definition->kind == SOL_IR_DEFINITION_ENUM
                    || definition->kind == SOL_IR_DEFINITION_DISTINCT
                    || definition->kind == SOL_IR_DEFINITION_REFINED);
        }
        copyable[id] = candidate;
    }
    bool changed;
    do {
        changed = false;
        for (size_t id = 0; id < ir->type_count; ++id) {
            if (copyable[id] && !copy_dependencies_hold(&analysis, id)) {
                copyable[id] = false;
                changed = true;
            }
        }
    } while (changed);
    return true;
}

static bool analyze_expression_inner(Ownership *analysis, SolIrExpressionId id,
    bool *unavailable, SolAccessMode access, bool *reachable);

static bool analyze_expression(Ownership *analysis, SolIrExpressionId id,
    bool *unavailable, SolAccessMode access, bool *reachable) {
    bool valid = analyze_expression_inner(
        analysis, id, unavailable, access, reachable);
    if (valid && *reachable && id < analysis->ir->expression_count
        && analysis->ir->expressions[id].type < analysis->ir->type_count
        && analysis->ir->types[analysis->ir->expressions[id].type].kind
            == SOL_IR_TYPE_NEVER) {
        *reachable = false;
    }
    return valid;
}

static bool analyze_child(Ownership *analysis, SolIrExpressionId id,
    bool *unavailable, SolAccessMode access, bool *reachable) {
    return analyze_expression(analysis, id, unavailable, access, reachable);
}

static const SolIrPlace *expression_place(
    const SolIr *ir, SolIrExpressionId id
) {
    if (id >= ir->expression_count || ir->expressions[id].kind != SOL_IR_EXPR_PLACE
        || ir->expressions[id].as.place >= ir->place_count) return NULL;
    return &ir->places[ir->expressions[id].as.place];
}

static bool local_place(
    const SolIr *ir, SolIrExpressionId id, SolIrLocalId *local
) {
    const SolIrPlace *place = expression_place(ir, id);
    if (place == NULL || place->root_kind != SOL_IR_PLACE_ROOT_LOCAL
        || place->local >= ir->local_count) return false;
    if (local != NULL) *local = place->local;
    return true;
}

static bool path_prefix(const SolIr *ir, const SolIrPlace *prefix,
    const SolIrPlace *path) {
    if (prefix->local != path->local
        || prefix->projections.count > path->projections.count) return false;
    for (size_t index = 0; index < prefix->projections.count; ++index) {
        const SolIrProjection *left
            = &ir->projections[prefix->projections.offset + index];
        const SolIrProjection *right
            = &ir->projections[path->projections.offset + index];
        if (left->kind != right->kind
            || (left->kind == SOL_IR_PROJECTION_FIELD && left->field != right->field)
            || (left->kind == SOL_IR_PROJECTION_TUPLE_FIELD
                && left->ordinal != right->ordinal)) return false;
    }
    return true;
}

static bool paths_overlap(const SolIr *ir, SolIrExpressionId left,
    SolIrExpressionId right) {
    const SolIrPlace *a = expression_place(ir, left);
    const SolIrPlace *b = expression_place(ir, right);
    return a != NULL && b != NULL && a->root_kind == SOL_IR_PLACE_ROOT_LOCAL
        && b->root_kind == SOL_IR_PLACE_ROOT_LOCAL
        && (path_prefix(ir, a, b) || path_prefix(ir, b, a));
}

static bool path_available(Ownership *analysis, SolIrExpressionId id,
    const bool *unavailable) {
    for (size_t place = 0; place < analysis->ir->place_count; ++place) {
        if (!unavailable[place]) continue;
        for (size_t expression = 0; expression < analysis->ir->expression_count;
            ++expression) {
            if (analysis->ir->expressions[expression].kind == SOL_IR_EXPR_PLACE
                && analysis->ir->expressions[expression].as.place == place
                && paths_overlap(analysis->ir, id, expression)) return false;
        }
    }
    return true;
}

static void move_path(Ownership *analysis, SolIrExpressionId id,
    bool *unavailable) {
    const SolIrPlace *moved = expression_place(analysis->ir, id);
    for (size_t place = 0; place < analysis->ir->place_count; ++place) {
        const SolIrPlace *candidate = &analysis->ir->places[place];
        if (candidate->root_kind == SOL_IR_PLACE_ROOT_LOCAL
            && path_prefix(analysis->ir, moved, candidate)) unavailable[place] = false;
    }
    unavailable[analysis->ir->expressions[id].as.place] = true;
}

static bool restore_path(Ownership *analysis, SolIrExpressionId id,
    bool *unavailable) {
    const SolIrPlace *target = expression_place(analysis->ir, id);
    for (size_t place = 0; place < analysis->ir->place_count; ++place) {
        if (!unavailable[place]) continue;
        const SolIrPlace *frontier = &analysis->ir->places[place];
        if (path_prefix(analysis->ir, frontier, target)
            && frontier->projections.count < target->projections.count) return false;
    }
    for (size_t place = 0; place < analysis->ir->place_count; ++place) {
        const SolIrPlace *frontier = &analysis->ir->places[place];
        if (unavailable[place] && path_prefix(analysis->ir, target, frontier)) {
            unavailable[place] = false;
        }
    }
    return true;
}

static bool loan_conflicts(Ownership *analysis, SolIrExpressionId id,
    SolAccessMode access) {
    for (size_t index = 0; index < analysis->loan_count; ++index) {
        if (paths_overlap(analysis->ir, id, analysis->loan_places[index])
            && (access == SOL_ACCESS_EXCLUSIVE
                || analysis->loan_modes[index] == SOL_ACCESS_EXCLUSIVE)) return true;
    }
    return false;
}

static void clear_local_paths(Ownership *analysis, bool *unavailable,
    SolIrLocalId local) {
    for (size_t place = 0; place < analysis->ir->place_count; ++place) {
        if (analysis->ir->places[place].root_kind == SOL_IR_PLACE_ROOT_LOCAL
            && analysis->ir->places[place].local == local) unavailable[place] = false;
    }
}

static bool analyze_assignment(Ownership *analysis, const SolIrStatement *statement,
    bool *unavailable, bool *reachable) {
    SolIrLocalId target_local = SOL_IR_NONE;
    if (!local_place(analysis->ir, statement->target, &target_local)) {
        return ownership_internal(analysis, statement->span,
            "assignment target metadata is out of range");
    }
    const SolIrExpression *target = &analysis->ir->expressions[statement->target];
    const SolIrLocal *local = &analysis->ir->locals[target_local];
    bool writable_binding = local->kind == SOL_IR_LOCAL_BINDING
        && local->access == SOL_ACCESS_OWNED
        && (local->mutable || analysis->modify_depths[target_local] != 0);
    bool writable_parameter = local->kind == SOL_IR_LOCAL_PARAMETER
        && (local->access == SOL_ACCESS_EXCLUSIVE
            || (local->access == SOL_ACCESS_OWNED
                && analysis->modify_depths[target_local] != 0));
    if (target->kind != SOL_IR_EXPR_PLACE || local->owner != analysis->owner
        || (!writable_binding && !writable_parameter)) {
        return ownership_error(analysis, statement->span,
            "assignment target is not a writable local place");
    }
    if (!analysis->introduced[target_local]) {
        return ownership_internal(analysis, statement->span,
            "assignment target has not been introduced in this scope");
    }
    const SolIrPlace *place = expression_place(analysis->ir, statement->target);
    bool compound = statement->operator_kind != SOL_TOKEN_EQUAL;
    if ((compound || place->projections.count != 0)
        && !analysis->initialized[target_local]) {
        return ownership_error_code(analysis, statement->span,
            "SOL-INITIALIZATION-001", "local is used before it is initialized");
    }
    if (compound && !path_available(analysis, statement->target, unavailable)) {
        return ownership_error(analysis, target->span,
            "compound assignment target is used after part of it was moved");
    }
    if (!compound && !analyze_expression(analysis, statement->expression, unavailable,
            SOL_ACCESS_OWNED, reachable)) return false;
    if (!*reachable) return true;
    if (compound && (!analyze_expression(analysis, statement->expression, unavailable,
            SOL_ACCESS_OWNED, reachable) || !*reachable)) return *reachable == false;
    if (loan_conflicts(analysis, statement->target, SOL_ACCESS_EXCLUSIVE)) {
        return ownership_error_code(analysis, statement->span, "SOL-OWNERSHIP-003",
            "local place cannot be updated while it is borrowed");
    }
    analysis->uses[statement->target] = SOL_IR_LOCAL_USE_UPDATE;
    if (analysis->region_depth > analysis->introduction_depths[target_local]
        && !type_is_copy(analysis,
            target->type)
        && !type_is_copy(analysis,
            analysis->ir->expressions[statement->expression].type)) {
        return ownership_error_code(analysis, statement->span, "SOL-REGION-001",
            "an affine value from a deeper explicit region cannot update an outer local");
    }
    if (!restore_path(analysis, statement->target, unavailable)) {
        return ownership_error(analysis, target->span,
            "assignment target is below a moved parent place");
    }
    if (place->projections.count == 0) analysis->initialized[target_local] = true;
    return true;
}

static bool analyze_modify(Ownership *analysis, const SolIrStatement *statement,
    bool *unavailable, bool *reachable) {
    SolIrLocalId local = SOL_IR_NONE;
    const SolIrPlace *place = expression_place(analysis->ir, statement->target);
    if (!local_place(analysis->ir, statement->target, &local) || place == NULL
        || place->projections.count != 0 || !analysis->introduced[local]) {
        return ownership_internal(analysis, statement->span,
            "modify target is not an introduced whole local");
    }
    const SolIrLocal *metadata = &analysis->ir->locals[local];
    if (metadata->access != SOL_ACCESS_OWNED
        || !((metadata->kind == SOL_IR_LOCAL_BINDING && !metadata->mutable)
            || metadata->kind == SOL_IR_LOCAL_PARAMETER)) {
        return ownership_internal(analysis, statement->span,
            "modify target is not an immutable owned local");
    }
    if (!analysis->initialized[local]) return ownership_error_code(analysis,
        statement->span, "SOL-INITIALIZATION-001",
        "local is used before it is initialized");
    if (!path_available(analysis, statement->target, unavailable)) {
        return ownership_error(analysis, statement->span,
            "modify target is used after part of it was moved");
    }
    if (loan_conflicts(analysis, statement->target, SOL_ACCESS_EXCLUSIVE)) {
        return ownership_error_code(analysis, statement->span, "SOL-OWNERSHIP-003",
            "local cannot be modified while it is borrowed");
    }
    ++analysis->modify_depths[local];
    bool valid = analyze_expression(analysis, statement->expression, unavailable,
        SOL_ACCESS_OWNED, reachable);
    --analysis->modify_depths[local];
    return valid;
}

static bool begin_loan(Ownership *analysis, SolIrExpressionId id,
    bool *unavailable, SolAccessMode access) {
    SolIrLocalId local = SOL_IR_NONE;
    if (!local_place(analysis->ir, id, &local)) {
        return ownership_error_code(analysis, id < analysis->ir->expression_count
            ? analysis->ir->expressions[id].span : (SolSpan){0},
            "SOL-OWNERSHIP-004", "borrowed arguments must be local field places");
    }
    const SolIrExpression *expression = &analysis->ir->expressions[id];
    if (local >= analysis->ir->local_count
        || !analysis->introduced[local]
        || !analysis->initialized[local]
        || !path_available(analysis, id, unavailable)) {
        if (local < analysis->ir->local_count && analysis->introduced[local]
            && !analysis->initialized[local]) return ownership_error_code(analysis,
                expression->span, "SOL-INITIALIZATION-001",
                "local is borrowed before it is initialized");
        return ownership_error(analysis, expression->span,
            "borrowed place is used after it or a parent was moved");
    }
    SolAccessMode source = analysis->ir->locals[local].access;
    if ((source == SOL_ACCESS_SHARED && access == SOL_ACCESS_EXCLUSIVE)
        || (source != SOL_ACCESS_OWNED && access == SOL_ACCESS_OWNED)) {
        return ownership_error_code(analysis, expression->span,
            "SOL-OWNERSHIP-004", "invalid reborrow of borrowed parameter");
    }
    if (access == SOL_ACCESS_EXCLUSIVE) {
        const SolIrLocal *root = &analysis->ir->locals[local];
        bool writable = (root->kind == SOL_IR_LOCAL_BINDING && root->mutable
                && root->access == SOL_ACCESS_OWNED)
            || (root->kind == SOL_IR_LOCAL_PARAMETER
                && root->access == SOL_ACCESS_EXCLUSIVE)
            || (root->access == SOL_ACCESS_OWNED
                && analysis->modify_depths[local] != 0);
        if (!writable) return ownership_error_code(analysis, expression->span,
            "SOL-OWNERSHIP-004",
            "exclusive arguments must be mutable local or inout parameter places");
        if (analysis->region_depth > analysis->introduction_depths[local]
            && !type_is_copy(analysis, expression->type)) {
            return ownership_error_code(analysis, expression->span,
                "SOL-REGION-001",
                "an affine inout place cannot write back across an explicit region");
        }
    }
    if (loan_conflicts(analysis, id, access)) {
        return ownership_error_code(analysis, expression->span,
            "SOL-OWNERSHIP-002", "borrow conflicts with an overlapping loan");
    }
    analysis->uses[id] = access == SOL_ACCESS_SHARED
        ? SOL_IR_LOCAL_USE_SHARED : SOL_IR_LOCAL_USE_EXCLUSIVE;
    if (analysis->loan_count >= analysis->ir->expression_count) {
        return ownership_internal(analysis, expression->span,
            "ownership loan metadata capacity was exceeded");
    }
    analysis->loan_places[analysis->loan_count] = id;
    analysis->loan_modes[analysis->loan_count++] = access;
    return true;
}

static bool validate_borrow_places(Ownership *analysis) {
    for (size_t index = 0; index < analysis->ir->expression_count; ++index) {
        const SolIrExpression *expression = &analysis->ir->expressions[index];
        if (expression->kind == SOL_IR_EXPR_CALL) {
            if (expression->as.call.kind == SOL_IR_CALL_METHOD
                && expression->as.call.receiver_access != SOL_ACCESS_OWNED
                && !local_place(analysis->ir,
                    expression->as.call.receiver, NULL)) {
                return ownership_error_code(analysis, expression->span,
                    "SOL-OWNERSHIP-004", "borrowed receiver must be a direct local place");
            }
            if ((expression->as.call.kind == SOL_IR_CALL_CAPABILITY
                    || expression->as.call.kind == SOL_IR_CALL_CALLBACK)
                && analysis->ir->expressions[expression->as.call.callee].kind
                    == SOL_IR_EXPR_BOUND_OPERATION) {
                SolIrExpressionId receiver = analysis->ir->expressions[
                    expression->as.call.callee].as.operation.receiver;
                if (!local_place(analysis->ir, receiver, NULL)) {
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
                    && !local_place(analysis->ir, entry->value, NULL)) {
                    return ownership_error_code(analysis, expression->span,
                        "SOL-OWNERSHIP-004",
                        "borrowed arguments must be direct local places");
                }
            }
        } else if (expression->kind == SOL_IR_EXPR_HANDLE
            && (!local_place(analysis->ir,
                    expression->as.handler.authority, NULL)
                || !local_place(analysis->ir,
                    expression->as.handler.provider, NULL))) {
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
        destination[index] = left[index] || right[index];
    }
}

static void intersect_states(bool *destination, const bool *left,
    const bool *right, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        destination[index] = left[index] && right[index];
    }
}

static void join_edge(Ownership *analysis, bool *joined_unavailable,
    bool *joined_initialized, bool *has_edge, const bool *unavailable) {
    size_t place_count = analysis->ir->place_count;
    size_t local_count = analysis->ir->local_count;
    for (size_t place = 0; place < place_count; ++place) {
        const SolIrPlace *metadata = &analysis->ir->places[place];
        bool lexical = metadata->root_kind == SOL_IR_PLACE_ROOT_LOCAL
            && analysis->introduced[metadata->local]
            && !analysis->loop->entry_introduced[metadata->local];
        bool edge = lexical ? false : unavailable[place];
        joined_unavailable[place] = *has_edge
            ? joined_unavailable[place] || edge : edge;
    }
    for (size_t local = 0; local < local_count; ++local) {
        bool lexical = analysis->introduced[local]
            && !analysis->loop->entry_introduced[local];
        bool edge = lexical ? false : analysis->initialized[local];
        joined_initialized[local] = *has_edge
            ? joined_initialized[local] && edge : edge;
    }
    *has_edge = true;
}

static bool states_equal(const bool *left, const bool *right, size_t count) {
    return count == 0 || memcmp(left, right, count * sizeof(*left)) == 0;
}

static bool analyze_loop(Ownership *analysis, const SolIrStatement *statement,
    bool *unavailable, bool *reachable) {
    size_t place_count = analysis->ir->place_count;
    size_t local_count = analysis->ir->local_count;
#define LOOP_ALLOC(name, count) \
    bool *name = (count) == 0 ? NULL : malloc((count) * sizeof(*name))
    LOOP_ALLOC(entry_unavailable, place_count);
    LOOP_ALLOC(header_unavailable, place_count);
    LOOP_ALLOC(next_unavailable, place_count);
    LOOP_ALLOC(break_unavailable, place_count);
    LOOP_ALLOC(back_unavailable, place_count);
    LOOP_ALLOC(false_unavailable, place_count);
    LOOP_ALLOC(entry_initialized, local_count);
    LOOP_ALLOC(header_initialized, local_count);
    LOOP_ALLOC(next_initialized, local_count);
    LOOP_ALLOC(break_initialized, local_count);
    LOOP_ALLOC(back_initialized, local_count);
    LOOP_ALLOC(false_initialized, local_count);
    LOOP_ALLOC(entry_introduced, local_count);
    size_t *entry_depths = local_count == 0 ? NULL
        : malloc(local_count * sizeof(*entry_depths));
#undef LOOP_ALLOC
    if ((place_count != 0 && (entry_unavailable == NULL
            || header_unavailable == NULL || next_unavailable == NULL
            || break_unavailable == NULL || back_unavailable == NULL
            || false_unavailable == NULL))
        || (local_count != 0 && (entry_initialized == NULL
            || header_initialized == NULL || next_initialized == NULL
            || break_initialized == NULL || back_initialized == NULL
            || false_initialized == NULL || entry_introduced == NULL
            || entry_depths == NULL))) {
        free(entry_unavailable); free(header_unavailable); free(next_unavailable);
        free(break_unavailable); free(back_unavailable); free(false_unavailable);
        free(entry_initialized); free(header_initialized); free(next_initialized);
        free(break_initialized); free(back_initialized); free(false_initialized);
        free(entry_introduced); free(entry_depths);
        return ownership_internal(analysis, statement->span,
            "ownership loop allocation failed");
    }
    if (place_count != 0) {
        memcpy(entry_unavailable, unavailable, place_count * sizeof(*unavailable));
        memcpy(header_unavailable, unavailable, place_count * sizeof(*unavailable));
    }
    if (local_count != 0) {
        memcpy(entry_initialized, analysis->initialized,
            local_count * sizeof(*entry_initialized));
        memcpy(header_initialized, analysis->initialized,
            local_count * sizeof(*header_initialized));
        memcpy(entry_introduced, analysis->introduced,
            local_count * sizeof(*entry_introduced));
        memcpy(entry_depths, analysis->introduction_depths,
            local_count * sizeof(*entry_depths));
    }
    SolDiagnostics *diagnostics = analysis->diagnostics;
    bool valid = true;
    bool converged = false;
    size_t limit = place_count + local_count + 1;
    for (size_t iteration = 0; valid && iteration < limit; ++iteration) {
        if (place_count != 0) memcpy(unavailable, header_unavailable,
            place_count * sizeof(*unavailable));
        if (local_count != 0) {
            memcpy(analysis->initialized, header_initialized,
                local_count * sizeof(*header_initialized));
            memcpy(analysis->introduced, entry_introduced,
                local_count * sizeof(*entry_introduced));
            memcpy(analysis->introduction_depths, entry_depths,
                local_count * sizeof(*entry_depths));
        }
        OwnershipLoop loop = {.parent = analysis->loop,
            .entry_introduced = entry_introduced,
            .break_unavailable = break_unavailable,
            .break_initialized = break_initialized,
            .back_unavailable = back_unavailable,
            .back_initialized = back_initialized};
        analysis->diagnostics = NULL;
        analysis->loop = &loop;
        bool pass_reachable = true;
        if (statement->kind == SOL_IR_STATEMENT_WHILE) {
            valid = analyze_expression(analysis, statement->condition, unavailable,
                SOL_ACCESS_OWNED, &pass_reachable);
        }
        if (valid && pass_reachable) {
            valid = analyze_expression(analysis, statement->expression, unavailable,
                SOL_ACCESS_OWNED, &pass_reachable);
        }
        if (valid && pass_reachable) join_edge(analysis, back_unavailable,
            back_initialized, &loop.has_back, unavailable);
        analysis->loop = loop.parent;
        analysis->diagnostics = diagnostics;
        if (!valid) {
            if (place_count != 0) memcpy(unavailable, header_unavailable,
                place_count * sizeof(*unavailable));
            if (local_count != 0) {
                memcpy(analysis->initialized, header_initialized,
                    local_count * sizeof(*header_initialized));
                memcpy(analysis->introduced, entry_introduced,
                    local_count * sizeof(*entry_introduced));
            }
            OwnershipLoop report = {.parent = analysis->loop,
                .entry_introduced = entry_introduced,
                .break_unavailable = break_unavailable,
                .break_initialized = break_initialized,
                .back_unavailable = back_unavailable,
                .back_initialized = back_initialized};
            bool report_reachable = true;
            analysis->loop = &report;
            if (statement->kind != SOL_IR_STATEMENT_WHILE
                || analyze_expression(analysis, statement->condition, unavailable,
                    SOL_ACCESS_OWNED, &report_reachable)) {
                if (report_reachable) {
                    analyze_expression(analysis, statement->expression,
                        unavailable, SOL_ACCESS_OWNED, &report_reachable);
                }
            }
            analysis->loop = report.parent;
            break;
        }
        if (place_count != 0) memcpy(next_unavailable, entry_unavailable,
            place_count * sizeof(*next_unavailable));
        if (local_count != 0) memcpy(next_initialized, entry_initialized,
            local_count * sizeof(*next_initialized));
        if (loop.has_back) {
            join_states(next_unavailable, next_unavailable, back_unavailable,
                place_count);
            intersect_states(next_initialized, next_initialized, back_initialized,
                local_count);
        }
        converged = states_equal(header_unavailable, next_unavailable, place_count)
            && states_equal(header_initialized, next_initialized, local_count);
        if (place_count != 0) memcpy(header_unavailable, next_unavailable,
            place_count * sizeof(*header_unavailable));
        if (local_count != 0) memcpy(header_initialized, next_initialized,
            local_count * sizeof(*header_initialized));
        if (converged) break;
    }
    if (valid && !converged) valid = ownership_internal(analysis, statement->span,
        "ownership loop fixed point did not converge");
    bool has_exit = false;
    if (valid) {
        if (place_count != 0) memcpy(unavailable, header_unavailable,
            place_count * sizeof(*unavailable));
        if (local_count != 0) {
            memcpy(analysis->initialized, header_initialized,
                local_count * sizeof(*header_initialized));
            memcpy(analysis->introduced, entry_introduced,
                local_count * sizeof(*entry_introduced));
        }
        OwnershipLoop loop = {.parent = analysis->loop,
            .entry_introduced = entry_introduced,
            .break_unavailable = break_unavailable,
            .break_initialized = break_initialized,
            .back_unavailable = back_unavailable,
            .back_initialized = back_initialized};
        bool pass_reachable = true;
        analysis->loop = &loop;
        if (statement->kind == SOL_IR_STATEMENT_WHILE) {
            valid = analyze_expression(analysis, statement->condition, unavailable,
                SOL_ACCESS_OWNED, &pass_reachable);
            if (valid && pass_reachable) {
                if (place_count != 0) memcpy(false_unavailable, unavailable,
                    place_count * sizeof(*false_unavailable));
                if (local_count != 0) memcpy(false_initialized,
                    analysis->initialized,
                    local_count * sizeof(*false_initialized));
                has_exit = true;
            }
        }
        if (valid && pass_reachable) {
            valid = analyze_expression(analysis, statement->expression,
                unavailable, SOL_ACCESS_OWNED, &pass_reachable);
        }
        analysis->loop = loop.parent;
        if (valid && loop.has_break) {
            if (!has_exit) {
                if (place_count != 0) memcpy(false_unavailable, break_unavailable,
                    place_count * sizeof(*false_unavailable));
                if (local_count != 0) memcpy(false_initialized, break_initialized,
                    local_count * sizeof(*false_initialized));
            } else {
                join_states(false_unavailable, false_unavailable, break_unavailable,
                    place_count);
                intersect_states(false_initialized, false_initialized,
                    break_initialized, local_count);
            }
            has_exit = true;
        }
        if (valid && has_exit) {
            if (place_count != 0) memcpy(unavailable, false_unavailable,
                place_count * sizeof(*unavailable));
            if (local_count != 0) memcpy(analysis->initialized, false_initialized,
                local_count * sizeof(*false_initialized));
        }
        *reachable = valid && has_exit;
    }
    analysis->diagnostics = diagnostics;
    if (local_count != 0) {
        memcpy(analysis->introduced, entry_introduced,
            local_count * sizeof(*entry_introduced));
        memcpy(analysis->introduction_depths, entry_depths,
            local_count * sizeof(*entry_depths));
    }
    free(entry_unavailable); free(header_unavailable); free(next_unavailable);
    free(break_unavailable); free(back_unavailable); free(false_unavailable);
    free(entry_initialized); free(header_initialized); free(next_initialized);
    free(break_initialized); free(back_initialized); free(false_initialized);
    free(entry_introduced); free(entry_depths);
    return valid;
}

static bool analyze_branches(Ownership *analysis, SolIrExpressionId left_id,
    SolIrExpressionId right_id, bool *available, bool *reachable) {
    size_t count = analysis->ir->place_count;
    size_t local_count = analysis->ir->local_count;
    bool *left = count == 0 ? NULL : malloc(count * sizeof(*left));
    bool *right = count == 0 ? NULL : malloc(count * sizeof(*right));
    bool *left_initialized = local_count == 0 ? NULL
        : malloc(local_count * sizeof(*left_initialized));
    bool *right_initialized = local_count == 0 ? NULL
        : malloc(local_count * sizeof(*right_initialized));
    if ((count != 0 && (left == NULL || right == NULL))
        || (local_count != 0 && (left_initialized == NULL
            || right_initialized == NULL))) {
        free(left);
        free(right);
        free(left_initialized); free(right_initialized);
        return ownership_internal(analysis, (SolSpan){0},
            "ownership branch allocation failed");
    }
    if (local_count != 0) memcpy(left_initialized, analysis->initialized,
        local_count * sizeof(*left_initialized));
    if (count != 0) {
        memcpy(left, available, count * sizeof(*left));
        memcpy(right, available, count * sizeof(*right));
    }
    bool left_reachable = true;
    bool right_reachable = true;
    bool valid = analyze_expression(analysis, left_id, left, SOL_ACCESS_OWNED,
        &left_reachable);
    if (local_count != 0) {
        memcpy(right_initialized, analysis->initialized,
            local_count * sizeof(*right_initialized));
        memcpy(analysis->initialized, left_initialized,
            local_count * sizeof(*left_initialized));
    }
    valid = valid && analyze_expression(analysis, right_id, right,
        SOL_ACCESS_OWNED, &right_reachable);
    if (valid && local_count != 0) {
        if (left_reachable && right_reachable) intersect_states(analysis->initialized,
            right_initialized, analysis->initialized, local_count);
        else if (left_reachable) memcpy(analysis->initialized, right_initialized,
            local_count * sizeof(*right_initialized));
        else if (!right_reachable) memcpy(analysis->initialized, left_initialized,
            local_count * sizeof(*left_initialized));
    }
    if (valid) {
        if (left_reachable && right_reachable) join_states(available, left, right, count);
        else if (left_reachable && count != 0) memcpy(available, left, count * sizeof(*left));
        else if (right_reachable && count != 0) memcpy(available, right, count * sizeof(*right));
        *reachable = left_reachable || right_reachable;
    }
    free(left);
    free(right);
    free(left_initialized); free(right_initialized);
    return valid;
}

static bool analyze_match(Ownership *analysis, const SolIrExpression *expression,
    bool *available, bool *reachable) {
    if (!analyze_expression(analysis, expression->as.match_expr.scrutinee,
        available, SOL_ACCESS_OWNED, reachable) || !*reachable) return *reachable == false;
    size_t count = analysis->ir->place_count;
    size_t local_count = analysis->ir->local_count;
    bool *baseline = count == 0 ? NULL : malloc(count * sizeof(*baseline));
    bool *branch = count == 0 ? NULL : malloc(count * sizeof(*branch));
    bool *joined = count == 0 ? NULL : malloc(count * sizeof(*joined));
    bool *baseline_initialized = local_count == 0 ? NULL
        : malloc(local_count * sizeof(*baseline_initialized));
    bool *joined_initialized = local_count == 0 ? NULL
        : malloc(local_count * sizeof(*joined_initialized));
    if ((count != 0 && (baseline == NULL || branch == NULL || joined == NULL))
        || (local_count != 0 && (baseline_initialized == NULL
            || joined_initialized == NULL))) {
        free(baseline); free(branch); free(joined);
        free(baseline_initialized); free(joined_initialized);
        return ownership_internal(analysis, expression->span,
            "ownership match allocation failed");
    }
    if (count != 0) memcpy(baseline, available, count * sizeof(*baseline));
    if (local_count != 0) memcpy(baseline_initialized, analysis->initialized,
        local_count * sizeof(*baseline_initialized));
    bool has_join = false;
    bool valid = true;
    for (size_t index = 0; valid && index < expression->as.match_expr.arms.count;
        ++index) {
        const SolIrArm *arm = &analysis->ir->arms[analysis->ir->arm_ids[
            expression->as.match_expr.arms.offset + index]];
        if (count != 0) memcpy(branch, baseline, count * sizeof(*branch));
        if (local_count != 0) memcpy(analysis->initialized, baseline_initialized,
            local_count * sizeof(*baseline_initialized));
        for (size_t binding = 0; binding < arm->bindings.count; ++binding) {
            SolIrLocalId local = analysis->ir->roots[
                arm->bindings.offset + binding];
            clear_local_paths(analysis, branch, local);
            analysis->introduced[local] = true;
            analysis->initialized[local] = true;
            analysis->introduction_depths[local] = analysis->region_depth;
        }
        bool branch_reachable = true;
        if (arm->guard != SOL_IR_NONE) {
            valid = analyze_expression(analysis, arm->guard, branch,
                SOL_ACCESS_OWNED, &branch_reachable);
            for (size_t place = 0; valid && place < count; ++place) {
                const SolIrPlace *entry = &analysis->ir->places[place];
                if (branch[place] && !baseline[place]
                    && entry->root_kind == SOL_IR_PLACE_ROOT_LOCAL
                    && !type_is_copy(analysis,
                        analysis->ir->locals[entry->local].type)) {
                    valid = ownership_error_code(analysis,
                        analysis->ir->expressions[arm->guard].span,
                        "SOL-OWNERSHIP-005",
                        "an affine local cannot be consumed in a match guard");
                }
            }
            for (size_t local = 0; valid && local < local_count; ++local) {
                if (baseline_initialized[local] && !analysis->initialized[local]
                    && !type_is_copy(analysis, analysis->ir->locals[local].type)) {
                    valid = ownership_error_code(analysis,
                        analysis->ir->expressions[arm->guard].span,
                        "SOL-OWNERSHIP-005",
                        "an affine local cannot be consumed in a match guard");
                }
            }
            if (count != 0) memcpy(branch, baseline,
                count * sizeof(*branch));
            if (local_count != 0) memcpy(analysis->initialized,
                baseline_initialized, local_count * sizeof(*baseline_initialized));
            for (size_t binding = 0; binding < arm->bindings.count; ++binding) {
                SolIrLocalId local = analysis->ir->roots[
                    arm->bindings.offset + binding];
                clear_local_paths(analysis, branch, local);
                analysis->initialized[local] = true;
            }
            branch_reachable = true;
        }
        valid = valid && analyze_expression(analysis, arm->body, branch, SOL_ACCESS_OWNED,
            &branch_reachable);
        for (size_t binding = 0; binding < arm->bindings.count; ++binding) {
            SolIrLocalId local = analysis->ir->roots[arm->bindings.offset + binding];
            clear_local_paths(analysis, branch, local);
            analysis->introduced[local] = false;
            analysis->initialized[local] = false;
        }
        if (!valid || !branch_reachable) continue;
        if (!has_join && count != 0) memcpy(joined, branch, count * sizeof(*joined));
        else if (has_join) join_states(joined, joined, branch, count);
        if (!has_join && local_count != 0) memcpy(joined_initialized,
            analysis->initialized, local_count * sizeof(*joined_initialized));
        else if (has_join) intersect_states(joined_initialized, joined_initialized,
            analysis->initialized, local_count);
        has_join = true;
    }
    if (valid && has_join && count != 0) memcpy(available, joined, count * sizeof(*joined));
    if (valid && has_join && local_count != 0) memcpy(analysis->initialized,
        joined_initialized, local_count * sizeof(*joined_initialized));
    *reachable = valid && has_join;
    free(baseline); free(branch); free(joined);
    free(baseline_initialized); free(joined_initialized);
    return valid;
}

static bool analyze_expression_inner(Ownership *analysis, SolIrExpressionId id,
    bool *unavailable, SolAccessMode access, bool *reachable) {
    if (id >= analysis->ir->expression_count) {
        return ownership_internal(analysis, (SolSpan){0},
            "ownership expression is out of range");
    }
    if (!*reachable) return true;
    const SolIrExpression *expression = &analysis->ir->expressions[id];
    switch (expression->kind) {
        case SOL_IR_EXPR_PLACE: {
            const SolIrPlace *place = expression_place(analysis->ir, id);
            if (place == NULL) return ownership_internal(analysis, expression->span,
                "place ownership metadata is malformed");
            if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY) {
                return analyze_expression(analysis, place->temporary, unavailable,
                    SOL_ACCESS_OWNED, reachable);
            }
            SolIrLocalId local = place->local;
            if (local >= analysis->ir->local_count
                || analysis->ir->locals[local].owner != analysis->owner) {
                return ownership_internal(analysis, place->root_span,
                    "local ownership is inconsistent with its callable");
            }
            if (!analysis->introduced[local]) {
                return ownership_internal(analysis, place->root_span,
                    "local is used outside its introduction scope");
            }
            if (!analysis->initialized[local]) return ownership_error_code(analysis,
                place->root_span, "SOL-INITIALIZATION-001",
                "local is read before it is initialized");
            if (access != SOL_ACCESS_OWNED) {
                return begin_loan(analysis, id, unavailable, access);
            }
            SolIrLocalUse use = type_is_copy(analysis, expression->type)
                    ? SOL_IR_LOCAL_USE_COPY : SOL_IR_LOCAL_USE_MOVE;
            analysis->uses[id] = use;
            if (!path_available(analysis, id, unavailable)) {
                char message[256];
                const char *name = analysis->ir->locals[local].name;
                int written = snprintf(message, sizeof(message),
                    "place rooted at '%s' is used after part of it was moved", name);
                if (written < 0) return false;
                return ownership_error(analysis, place->root_span, message);
            }
            if (loan_conflicts(analysis, id, use == SOL_IR_LOCAL_USE_MOVE
                    ? SOL_ACCESS_EXCLUSIVE : SOL_ACCESS_SHARED)) {
                return ownership_error_code(analysis, place->root_span,
                    "SOL-OWNERSHIP-003", "owned use or move occurs while local is borrowed");
            }
            if (analysis->ir->locals[local].access != SOL_ACCESS_OWNED
                && use == SOL_IR_LOCAL_USE_MOVE) {
                return ownership_error_code(analysis, place->root_span,
                    "SOL-OWNERSHIP-004", "borrowed affine parameter cannot escape or be consumed");
            }
            if (use == SOL_IR_LOCAL_USE_MOVE) move_path(analysis, id, unavailable);
            return true;
        }
        case SOL_IR_EXPR_BOUND_OPERATION:
            return analyze_child(analysis, expression->as.operation.receiver,
                unavailable, SOL_ACCESS_OWNED, reachable);
        case SOL_IR_EXPR_UNARY:
            return analyze_expression(analysis, expression->as.unary.operand,
                unavailable, SOL_ACCESS_OWNED, reachable);
        case SOL_IR_EXPR_BINARY:
            if (!analyze_expression(analysis, expression->as.binary.left,
                unavailable, SOL_ACCESS_OWNED, reachable) || !*reachable) return *reachable == false;
            if (expression->as.binary.operator_kind == SOL_TOKEN_AMP_AMP
                || expression->as.binary.operator_kind == SOL_TOKEN_PIPE_PIPE) {
                size_t count = analysis->ir->place_count;
                size_t local_count = analysis->ir->local_count;
                bool *skipped = count == 0 ? NULL : malloc(count * sizeof(*skipped));
                bool *skipped_initialized = local_count == 0 ? NULL
                    : malloc(local_count * sizeof(*skipped_initialized));
                if ((count != 0 && skipped == NULL)
                    || (local_count != 0 && skipped_initialized == NULL)) {
                    free(skipped); free(skipped_initialized);
                    return ownership_internal(
                    analysis, expression->span, "ownership join allocation failed");
                }
                if (count != 0) memcpy(skipped, unavailable, count * sizeof(*skipped));
                if (local_count != 0) memcpy(skipped_initialized,
                    analysis->initialized,
                    local_count * sizeof(*skipped_initialized));
                bool right_reachable = true;
                bool valid = analyze_expression(analysis, expression->as.binary.right,
                    unavailable, SOL_ACCESS_OWNED, &right_reachable);
                if (valid && right_reachable) join_states(unavailable, skipped, unavailable, count);
                else if (valid && count != 0) memcpy(unavailable, skipped,
                    count * sizeof(*skipped));
                if (valid && right_reachable) intersect_states(analysis->initialized,
                    skipped_initialized, analysis->initialized, local_count);
                else if (valid && local_count != 0) memcpy(analysis->initialized,
                    skipped_initialized, local_count * sizeof(*skipped_initialized));
                *reachable = valid;
                free(skipped);
                free(skipped_initialized);
                return valid;
            }
            return analyze_expression(analysis, expression->as.binary.right,
                unavailable, SOL_ACCESS_OWNED, reachable);
        case SOL_IR_EXPR_CALL:
            {
            size_t loan_base = analysis->loan_count;
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
                if (!begin_loan(analysis, receiver, unavailable, receiver_access)) valid = false;
                if (!valid) goto call_complete;
            } else if ((expression->as.call.kind == SOL_IR_CALL_CALLBACK
                    || expression->as.call.kind == SOL_IR_CALL_CAPABILITY)
                && !analyze_expression(analysis, expression->as.call.callee,
                    unavailable, SOL_ACCESS_OWNED, reachable)) {
                valid = false;
                goto call_complete;
            }
            if (expression->as.call.kind == SOL_IR_CALL_METHOD) {
                if (expression->as.call.receiver_access == SOL_ACCESS_OWNED) {
                    if (!analyze_expression(analysis, expression->as.call.receiver,
                        unavailable, SOL_ACCESS_OWNED, reachable)) {
                        valid = false;
                        goto call_complete;
                    }
                } else {
                    if (!begin_loan(analysis, expression->as.call.receiver, unavailable,
                        expression->as.call.receiver_access)) {
                        valid = false;
                        goto call_complete;
                    }
                }
            }
            for (size_t index = 0; *reachable
                && index < expression->as.call.operands.count; ++index) {
                const SolIrOperand *operand = &analysis->ir->operands[
                    expression->as.call.operands.offset + index];
                if (operand->access == SOL_ACCESS_OWNED) {
                    if (!analyze_expression(analysis, operand->value, unavailable,
                        SOL_ACCESS_OWNED, reachable)) {
                        valid = false;
                        goto call_complete;
                    }
                } else {
                    if (!begin_loan(analysis, operand->value,
                        unavailable, operand->access)) {
                        valid = false;
                        goto call_complete;
                    }
                }
            }
call_complete:
            analysis->loan_count = loan_base;
            return valid;
            }
        case SOL_IR_EXPR_RECORD:
            for (size_t index = 0; *reachable
                && index < expression->as.record.fields.count; ++index) {
                if (!analyze_expression(analysis, analysis->ir->operands[
                    expression->as.record.fields.offset + index].value,
                    unavailable, SOL_ACCESS_OWNED, reachable)) return false;
            }
            return true;
        case SOL_IR_EXPR_TUPLE:
            for (size_t index = 0; *reachable
                && index < expression->as.tuple.operands.count; ++index) {
                if (!analyze_expression(analysis, analysis->ir->operands[
                    expression->as.tuple.operands.offset + index].value,
                    unavailable, SOL_ACCESS_OWNED, reachable)) return false;
            }
            return true;
        case SOL_IR_EXPR_IF:
            if (!analyze_expression(analysis, expression->as.if_expr.condition,
                unavailable, SOL_ACCESS_OWNED, reachable) || !*reachable) return *reachable == false;
            return analyze_branches(analysis, expression->as.if_expr.then_branch,
                expression->as.if_expr.else_branch, unavailable, reachable);
        case SOL_IR_EXPR_MATCH:
            return analyze_match(analysis, expression, unavailable, reachable);
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
                bool valid_statement;
                if (statement->kind == SOL_IR_STATEMENT_DECLARE) {
                    valid_statement = true;
                } else if (statement->kind == SOL_IR_STATEMENT_ASSIGNMENT) {
                    valid_statement = analyze_assignment(analysis, statement,
                        unavailable, reachable);
                } else if (statement->kind == SOL_IR_STATEMENT_MODIFY) {
                    valid_statement = analyze_modify(analysis, statement,
                        unavailable, reachable);
                } else if (statement->kind == SOL_IR_STATEMENT_LOOP
                    || statement->kind == SOL_IR_STATEMENT_WHILE) {
                    valid_statement = analyze_loop(analysis, statement,
                        unavailable, reachable);
                } else if (statement->kind == SOL_IR_STATEMENT_UNREACHABLE) {
                    *reachable = false;
                    valid_statement = true;
                } else if (statement->kind == SOL_IR_STATEMENT_PANIC) {
                    valid_statement = analyze_expression(analysis,
                        statement->expression, unavailable, SOL_ACCESS_OWNED,
                        reachable);
                    if (valid_statement) *reachable = false;
                } else if (statement->kind == SOL_IR_STATEMENT_REQUIRE) {
                    valid_statement = analyze_expression(analysis,
                        statement->condition, unavailable, SOL_ACCESS_OWNED,
                        reachable);
                    if (valid_statement && *reachable) {
                        size_t place_count = analysis->ir->place_count;
                        size_t local_count = analysis->ir->local_count;
                        bool *fallback_unavailable = place_count == 0 ? NULL
                            : malloc(place_count * sizeof(*fallback_unavailable));
                        bool *saved_initialized = local_count == 0 ? NULL
                            : malloc(local_count * sizeof(*saved_initialized));
                        bool *saved_introduced = local_count == 0 ? NULL
                            : malloc(local_count * sizeof(*saved_introduced));
                        size_t *saved_depths = local_count == 0 ? NULL
                            : malloc(local_count * sizeof(*saved_depths));
                        if ((place_count != 0 && fallback_unavailable == NULL)
                            || (local_count != 0 && (saved_initialized == NULL
                                || saved_introduced == NULL
                                || saved_depths == NULL))) {
                            free(fallback_unavailable);
                            free(saved_initialized);
                            free(saved_introduced);
                            free(saved_depths);
                            return ownership_internal(analysis, statement->span,
                                "ownership require branch allocation failed");
                        }
                        if (place_count != 0) memcpy(fallback_unavailable,
                            unavailable, place_count * sizeof(*unavailable));
                        if (local_count != 0) {
                            memcpy(saved_initialized, analysis->initialized,
                                local_count * sizeof(*saved_initialized));
                            memcpy(saved_introduced, analysis->introduced,
                                local_count * sizeof(*saved_introduced));
                            memcpy(saved_depths, analysis->introduction_depths,
                                local_count * sizeof(*saved_depths));
                        }
                        bool fallback_reachable = true;
                        valid_statement = analyze_expression(analysis,
                            statement->expression, fallback_unavailable,
                            SOL_ACCESS_OWNED, &fallback_reachable);
                        if (valid_statement && fallback_reachable) {
                            valid_statement = ownership_internal(analysis,
                                statement->span,
                                "require fallback does not terminate");
                        }
                        if (local_count != 0) {
                            memcpy(analysis->initialized, saved_initialized,
                                local_count * sizeof(*saved_initialized));
                            memcpy(analysis->introduced, saved_introduced,
                                local_count * sizeof(*saved_introduced));
                            memcpy(analysis->introduction_depths, saved_depths,
                                local_count * sizeof(*saved_depths));
                        }
                        free(fallback_unavailable);
                        free(saved_initialized);
                        free(saved_introduced);
                        free(saved_depths);
                    }
                } else if (statement->kind == SOL_IR_STATEMENT_BREAK
                    || statement->kind == SOL_IR_STATEMENT_CONTINUE) {
                    if (analysis->loop == NULL) {
                        valid_statement = ownership_internal(analysis,
                            statement->span,
                            "loop exit appears outside a loop");
                    } else {
                        if (statement->kind == SOL_IR_STATEMENT_BREAK) {
                            join_edge(analysis,
                                analysis->loop->break_unavailable,
                                analysis->loop->break_initialized,
                                &analysis->loop->has_break, unavailable);
                        } else {
                            join_edge(analysis,
                                analysis->loop->back_unavailable,
                                analysis->loop->back_initialized,
                                &analysis->loop->has_back, unavailable);
                        }
                        *reachable = false;
                        valid_statement = true;
                    }
                } else {
                    valid_statement = analyze_expression(analysis,
                        statement->expression, unavailable, SOL_ACCESS_OWNED, reachable);
                }
                if (!valid_statement) {
                    if (statement->kind == SOL_IR_STATEMENT_REGION) {
                        --analysis->region_depth;
                    }
                    return false;
                }
                if (statement->kind == SOL_IR_STATEMENT_REGION) {
                    --analysis->region_depth;
                }
                if (*reachable && (statement->kind == SOL_IR_STATEMENT_LET
                        || statement->kind == SOL_IR_STATEMENT_DECLARE)) {
                    clear_local_paths(analysis, unavailable, statement->local);
                    analysis->introduced[statement->local] = true;
                    analysis->initialized[statement->local]
                        = statement->kind == SOL_IR_STATEMENT_LET;
                    analysis->introduction_depths[statement->local]
                        = analysis->region_depth;
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
                SolIrLocalId local = analysis->ir->cleanup_locals[
                    expression->as.block.cleanup.offset + index];
                clear_local_paths(analysis, unavailable, local);
                analysis->introduced[local] = false;
                analysis->initialized[local] = false;
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
                unavailable, SOL_ACCESS_OWNED, reachable);
        case SOL_IR_EXPR_HANDLE:
            {
            size_t loan_base = analysis->loan_count;
            if (!begin_loan(analysis, expression->as.handler.authority, unavailable,
                    SOL_ACCESS_SHARED)
                || !begin_loan(analysis, expression->as.handler.provider, unavailable,
                    SOL_ACCESS_SHARED)) {
                analysis->loan_count = loan_base;
                return false;
            }
            bool valid = analyze_expression(analysis, expression->as.handler.body,
                unavailable, SOL_ACCESS_OWNED, reachable);
            analysis->loan_count = loan_base;
            return valid;
            }
        default:
            return true;
    }
}

static bool run_ownership(Ownership *analysis) {
    size_t place_count = analysis->ir->place_count;
    bool *unavailable = place_count == 0 ? NULL
        : calloc(place_count, sizeof(*unavailable));
    if (place_count != 0 && unavailable == NULL) return ownership_internal(
        analysis, (SolSpan){0}, "ownership state allocation failed");
    bool valid = validate_borrow_places(analysis);
    for (size_t index = 0; valid && index < analysis->ir->callable_count; ++index) {
        const SolIrCallable *callable = &analysis->ir->callables[index];
        if (callable->body == SOL_IR_NONE) continue;
        if (place_count != 0) memset(unavailable, 0,
            place_count * sizeof(*unavailable));
        if (analysis->ir->local_count != 0) memset(analysis->introduced, 0,
            analysis->ir->local_count * sizeof(*analysis->introduced));
        if (analysis->ir->local_count != 0) memset(analysis->initialized, 0,
            analysis->ir->local_count * sizeof(*analysis->initialized));
        analysis->loan_count = 0;
        analysis->owner = callable->owner;
        analysis->region_depth = 0;
        for (size_t parameter = 0; parameter < callable->parameters.count; ++parameter) {
            SolIrLocalId local = analysis->ir->roots[
                callable->parameters.offset + parameter];
            analysis->introduced[local] = true;
            analysis->initialized[local] = true;
        }
        if (callable->receiver != SOL_IR_NONE) {
            analysis->introduced[callable->receiver] = true;
            analysis->initialized[callable->receiver] = true;
        }
        if (callable->capability_source != SOL_IR_NONE) {
            analysis->introduced[callable->capability_source] = true;
            analysis->initialized[callable->capability_source] = true;
        }
        bool reachable = true;
        valid = analyze_expression(analysis, callable->body, unavailable, SOL_ACCESS_OWNED,
            &reachable);
    }
    free(unavailable);
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
    analysis->loan_places = ir->expression_count == 0 ? NULL
        : calloc(ir->expression_count, sizeof(*analysis->loan_places));
    analysis->loan_modes = ir->expression_count == 0 ? NULL
        : calloc(ir->expression_count, sizeof(*analysis->loan_modes));
    analysis->introduced = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*analysis->introduced));
    analysis->initialized = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*analysis->initialized));
    analysis->modify_depths = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*analysis->modify_depths));
    analysis->introduction_depths = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*analysis->introduction_depths));
    if ((ir->expression_count != 0 && analysis->uses == NULL)
        || (ir->type_count != 0 && analysis->copy_states == NULL)
        || (ir->expression_count != 0
            && (analysis->loan_places == NULL || analysis->loan_modes == NULL))
        || (ir->local_count != 0
            && (analysis->introduced == NULL || analysis->initialized == NULL
                || analysis->modify_depths == NULL
                || analysis->introduction_depths == NULL))) {
        free(analysis->uses);
        free(analysis->copy_states);
        free(analysis->loan_places);
        free(analysis->loan_modes);
        free(analysis->introduced);
        free(analysis->initialized);
        free(analysis->modify_depths);
        free(analysis->introduction_depths);
        return ownership_internal(analysis, (SolSpan){0},
            "ownership metadata allocation failed");
    }
    if (!sol_ir_compute_copyability(ir, analysis->copy_states,
        ir->type_count)) return ownership_internal(analysis, (SolSpan){0},
            "ownership copyability computation failed");
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
    free(analysis.loan_places);
    free(analysis.loan_modes);
    free(analysis.introduced);
    free(analysis.initialized);
    free(analysis.modify_depths);
    free(analysis.introduction_depths);
    return valid;
}

bool sol_ir_validate_ownership(const SolIr *ir, SolDiagnostics *diagnostics) {
    if (ir == NULL) return false;
    Ownership analysis;
    if (!ownership_prepare(ir, diagnostics, true, &analysis)) return false;
    bool valid = run_ownership(&analysis);
    for (size_t index = 0; valid && index < ir->expression_count; ++index) {
        if ((int)ir->expressions[index].local_use < 0
            || ir->expressions[index].local_use > SOL_IR_LOCAL_USE_UPDATE
            || ir->expressions[index].local_use != analysis.uses[index]) {
            valid = ownership_error(&analysis, ir->expressions[index].span,
                "IR local-use ownership metadata is inconsistent");
        }
    }
    free(analysis.uses);
    free(analysis.copy_states);
    free(analysis.loan_places);
    free(analysis.loan_modes);
    free(analysis.introduced);
    free(analysis.initialized);
    free(analysis.modify_depths);
    free(analysis.introduction_depths);
    return valid;
}
