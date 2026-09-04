#include "sol/mir_materialize.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    STORAGE_DEAD,
    STORAGE_UNINITIALIZED,
    STORAGE_INITIALIZED,
    STORAGE_MAYBE_INITIALIZED,
} Storage;

typedef struct {
    const SolMirMaterialization *owner;
    const SolMirMaterializedImage *image;
    size_t image_id;
    size_t block_count;
    size_t value_count;
    size_t local_count;
    size_t place_count;
} View;

static const SolMirMaterializedTerminator *cleanup_terminal(const View *v,
    size_t block);
static bool signatures_equal(const SolMirMaterialization *o,
    const SolMirMaterializedBinding *left,
    const SolMirMaterializedBinding *right);

static bool operation_key_matches(const SolMirMaterializedOperationKey *key,
    const SolMirMaterializedBinding *binding, SolMirMaterializedTypeId receiver,
    SolMirMaterializedPlaceId root, SolMirMaterializedEffectRowId effects) {
    return key->target_kind == binding->target_kind
        && key->instance == binding->instance && key->import == binding->import
        && key->receiver == receiver && key->root == root
        && key->effects == effects;
}

static bool targets_equal(const SolMirMaterializedBinding *left,
    const SolMirMaterializedBinding *right) {
    return left->target_kind == right->target_kind
        && left->instance == right->instance && left->import == right->import;
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_materialization_test_callable_site_target_equality(void) {
    SolMirMaterializedBinding invoke = {.target_kind
            = SOL_MIR_MATERIALIZED_TARGET_INSTANCE,
        .instance = 1, .import = SOL_MIR_PLAN_NONE};
    SolMirMaterializedBinding function = invoke;
    if (!targets_equal(&function, &invoke)) return false;
    function.instance = 2;
    if (targets_equal(&function, &invoke)) return false;
    invoke = (SolMirMaterializedBinding){.target_kind
            = SOL_MIR_MATERIALIZED_TARGET_IMPORT,
        .instance = SOL_MIR_PLAN_NONE, .import = 3};
    SolMirMaterializedBinding operation = invoke;
    SolMirMaterializedOperationKey key = {operation.target_kind,
        operation.instance, operation.import, 4, 5, 6};
    if (!targets_equal(&operation, &invoke)
        || !operation_key_matches(&key, &invoke, 4, 5, 6)) return false;
    key.root = 8;
    if (operation_key_matches(&key, &invoke, 4, 5, 6)) return false;
    key.root = 5; key.effects = 9;
    if (operation_key_matches(&key, &invoke, 4, 5, 6)) return false;
    key.effects = 6;
    operation.import = 7;
    key.import = operation.import;
    return !targets_equal(&operation, &invoke)
        && !operation_key_matches(&key, &invoke, 4, 5, 6);
}
#endif

static bool work_add(size_t *total, size_t value) {
    if (value > SIZE_MAX - *total) return false;
    *total += value;
    return true;
}

static bool work_mul(size_t left, size_t right, size_t *result) {
    if (left != 0 && right > SIZE_MAX / left) return false;
    *result = left * right;
    return true;
}

bool sol_mir_materialization_validation_work(
    const SolMirMaterialization *owner, size_t *work) {
    if (owner == NULL || work == NULL) return false;
    size_t total = owner->usage.concrete_records;
    size_t global = owner->usage.concrete_records;
    if (!work_add(&global, owner->image_count)
        || !work_add(&global, owner->binding_count)
        || !work_add(&global, owner->block_count)
        || !work_add(&global, owner->type_count)
        || !work_add(&global, 1)) return false;
    size_t global_scans;
    if (!work_mul(global, global, &global_scans)
        || !work_add(&total, global_scans)) return false;
    for (size_t i = 0; i < owner->image_count; ++i) {
        const SolMirMaterializedImage *im = &owner->images[i];
        size_t projections = 0;
        for (size_t p = 0; p < im->places.count; ++p) {
            const SolMirMaterializedPlace *place
                = &owner->places[im->places.offset + p];
            if (!work_add(&projections, place->projections.count)) return false;
        }
        size_t facts = im->values.count;
        if (!work_add(&facts, im->locals.count)
            || !work_add(&facts, im->places.count)
            || !work_add(&facts, im->temporaries.count)
            || !work_add(&facts, im->instructions.count)
            || !work_add(&facts, im->handlers.count)
            || !work_add(&facts, 1)) return false;
        size_t square, cube, cfg_work, flow;
        if (!work_mul(im->blocks.count, im->blocks.count, &square)
            || !work_mul(square, im->blocks.count, &cube)
            || !work_mul(cube, im->blocks.count, &cfg_work)
            || !work_mul(square, facts, &flow)
            || !work_mul(flow, facts, &flow)
            || !work_add(&total, cfg_work) || !work_add(&total, flow)) return false;
        size_t storage_facts = im->locals.count;
        size_t storage_passes, path_scan = projections, storage_scan = im->blocks.count;
        if (!work_add(&storage_facts, im->places.count)
            || !work_add(&storage_facts, 1)
            || !work_mul(im->blocks.count, storage_facts, &storage_passes)
            || !work_add(&storage_passes, 1)
            || !work_add(&path_scan, 1)
            || !work_mul(im->places.count, path_scan, &path_scan)) return false;
        size_t instruction_scan, argument_scan, merge_scan;
        if (!work_add(&path_scan, 1)
            || !work_mul(im->instructions.count, path_scan, &instruction_scan)
            || !work_mul(im->call_arguments.count, path_scan, &argument_scan)
            || !work_mul(im->blocks.count, storage_facts, &merge_scan)
            || !work_mul(merge_scan, 3, &merge_scan)
            || !work_add(&storage_scan, instruction_scan)
            || !work_add(&storage_scan, argument_scan)
            || !work_add(&storage_scan, merge_scan)) return false;
        size_t storage_work;
        if (!work_mul(storage_passes, storage_scan, &storage_work)
            || !work_add(&total, storage_work)) return false;
    }
    size_t closure;
    if (owner->image_count == SIZE_MAX || owner->binding_count == SIZE_MAX
        || !work_mul(owner->image_count + 1, owner->binding_count + 1, &closure)
        || !work_add(&total, closure)) return false;
    *work = total;
    return true;
}

static bool error(SolDiagnostics *diagnostics, const char *message) {
    if (diagnostics != NULL) sol_diagnostics_add(diagnostics,
        "SOL-MIR-MATERIALIZE-003", SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool slice(SolMirPlanSlice value, size_t count) {
    return value.offset <= count && value.count <= count - value.offset;
}

static bool in(SolMirPlanSlice range, size_t id) {
    return id >= range.offset && id - range.offset < range.count;
}

static bool access_valid(SolAccessMode access) {
    return access >= SOL_ACCESS_OWNED && access <= SOL_ACCESS_EXCLUSIVE;
}

static bool target_valid(const SolMirMaterialization *o,
    SolMirMaterializedTargetKind kind, size_t instance, size_t import) {
    return (kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
            && instance < o->image_count && import == SOL_MIR_PLAN_NONE)
        || (kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT
            && import < o->import_count && instance == SOL_MIR_PLAN_NONE);
}

static bool shallow_ranges(const SolMirMaterialization *o) {
    for (size_t i = 0; i < o->access_count; ++i)
        if (!access_valid(o->accesses[i])) return false;
    for (size_t i = 0; i < o->image_count; ++i) {
        const SolMirMaterializedImage *im = &o->images[i];
        if (!access_valid(im->receiver_access)
            || !slice(im->type_arguments, o->type_id_count)
            || !slice(im->parameter_types, o->type_id_count)
            || !slice(im->parameter_accesses, o->access_count)
            || !slice(im->overlays, o->overlay_count)
            || !slice(im->contexts, o->context_count)
            || !slice(im->locals, o->local_count)
            || !slice(im->places, o->place_count)
            || !slice(im->values, o->value_count)
            || !slice(im->instructions, o->instruction_count)
            || !slice(im->temporaries, o->temporary_count)
            || !slice(im->construct_operands, o->construct_operand_count)
            || !slice(im->call_arguments, o->call_argument_count)
            || !slice(im->blocks, o->block_count)
            || !slice(im->loops, o->loop_count)
            || !slice(im->handlers, o->handler_count)
            || !slice(im->bindings, o->binding_count)) return false;
    }
    for (size_t i = 0; i < o->local_count; ++i)
        if (!access_valid(o->locals[i].access)) return false;
    for (size_t i = 0; i < o->overlay_count; ++i)
        if (!access_valid(o->overlays[i].access)
            || o->overlays[i].type >= o->type_count) return false;
    for (size_t i = 0; i < o->call_argument_count; ++i) {
        const SolMirMaterializedCallArgument *a = &o->call_arguments[i];
        if (!access_valid(a->access) || a->type >= o->type_count
            || (a->temporary != SOL_MIR_MATERIALIZED_NONE
                && a->temporary >= o->temporary_count)
            || (a->place != SOL_MIR_MATERIALIZED_NONE
                && a->place >= o->place_count)) return false;
    }
    for (size_t i = 0; i < o->temporary_count; ++i)
        if (o->temporaries[i].type >= o->type_count) return false;
    for (size_t i = 0; i < o->value_count; ++i) {
        const SolMirMaterializedValue *value = &o->values[i];
        if (value->kind < SOL_MIR_VALUE_BLOCK_PARAMETER
            || value->kind > SOL_MIR_VALUE_TERMINATOR || value->type >= o->type_count
            || value->block >= o->block_count
            || (value->instruction != SOL_MIR_MATERIALIZED_NONE
                && value->instruction >= o->instruction_count)) return false;
    }
    for (size_t i = 0; i < o->construct_operand_count; ++i)
        if (o->construct_operands[i].type >= o->type_count
            || o->construct_operands[i].temporary >= o->temporary_count) return false;
    for (size_t i = 0; i < o->writeback_count; ++i)
        if (o->writebacks[i].type >= o->type_count
            || o->writebacks[i].place >= o->place_count) return false;
    for (size_t i = 0; i < o->binding_count; ++i) {
        const SolMirMaterializedBinding *b = &o->bindings[i];
        if (!target_valid(o, b->target_kind, b->instance, b->import)
            || b->site >= o->semantic_site_count
            || (b->parent != SOL_MIR_PLAN_NONE && b->parent >= o->image_count)) return false;
    }
    for (size_t i = 0; i < o->import_count; ++i) {
        const SolMirMaterializedImport *im = &o->imports[i];
        if (!access_valid(im->receiver_access)) return false;
    }
    for (size_t i = 0; i < o->edge_count; ++i) {
        const SolMirMaterializedEdge *edge = &o->edges[i];
        if (edge->block >= o->block_count
            || !slice(edge->arguments, o->edge_value_count)) return false;
        for (size_t a = 0; a < edge->arguments.count; ++a)
            if (o->edge_values[edge->arguments.offset + a] >= o->value_count) return false;
    }
    for (size_t i = 0; i < o->parameter_value_count; ++i)
        if (o->parameter_values[i] >= o->value_count) return false;
    for (size_t i = 0; i < o->block_count; ++i) {
        const SolMirMaterializedBlock *block = &o->blocks[i];
        const SolMirMaterializedTerminator *term = &block->terminator;
        if (term->kind <= SOL_MIR_TERM_INVALID
            || term->kind > SOL_MIR_TERM_CONTRACT_VIOLATION
            || !slice(block->parameters, o->parameter_value_count)
            || !slice(block->instructions, o->instruction_count)
            || !slice(term->arguments, o->call_argument_count)
            || !slice(term->writebacks, o->writeback_count)
            || !access_valid(term->receiver.access)) return false;
#define OPTIONAL(field, count) \
        do { if (term->field != SOL_MIR_MATERIALIZED_NONE \
            && term->field >= (count)) return false; } while (0)
        OPTIONAL(binding, o->binding_count); OPTIONAL(callee, o->temporary_count);
        OPTIONAL(result, o->value_count); OPTIONAL(value, o->value_count);
        OPTIONAL(condition, o->value_count); OPTIONAL(value_result, o->value_count);
        OPTIONAL(residual_result, o->value_count);
        OPTIONAL(representation, o->temporary_count); OPTIONAL(operand, o->temporary_count);
        OPTIONAL(edge, o->edge_count); OPTIONAL(true_edge, o->edge_count);
        OPTIONAL(false_edge, o->edge_count); OPTIONAL(normal_edge, o->edge_count);
        OPTIONAL(failure_edge, o->edge_count); OPTIONAL(value_edge, o->edge_count);
        OPTIONAL(residual_edge, o->edge_count); OPTIONAL(satisfied_edge, o->edge_count);
        OPTIONAL(violation_edge, o->edge_count); OPTIONAL(loop, o->loop_count);
        OPTIONAL(callable_site, o->semantic_site_count);
        OPTIONAL(receiver.temporary, o->temporary_count);
        OPTIONAL(receiver.place, o->place_count);
#undef OPTIONAL
    }
    for (size_t i = 0; i < o->handler_count; ++i) {
        const SolMirMaterializedHandler *h = &o->handlers[i];
        if (h->parent >= o->image_count || h->source_binding >= o->binding_count
            || h->provider_binding >= o->binding_count || h->authority >= o->place_count
            || h->provider >= o->place_count || h->context >= o->context_count
            || !target_valid(o, h->operation.target_kind, h->operation.instance,
                h->operation.import)
            || h->operation.receiver >= o->type_count
            || h->operation.root >= o->place_count
            || h->operation.effects >= o->effect_row_count) return false;
    }
    for (size_t i = 0; i < o->semantic_site_count; ++i) {
        const SolMirMaterializedSemanticSite *site = &o->semantic_sites[i];
        if (site->binding >= o->binding_count
            || (site->parent != SOL_MIR_PLAN_NONE && site->parent >= o->image_count)
            || (site->block != SOL_MIR_MATERIALIZED_NONE && site->block >= o->block_count)
            || (site->instruction != SOL_MIR_MATERIALIZED_NONE
                && site->instruction >= o->instruction_count)
            || (site->handler != SOL_MIR_MATERIALIZED_NONE
                && site->handler >= o->handler_count)
            || (site->produced_function_type != SOL_MIR_MATERIALIZED_NONE
                && site->produced_function_type >= o->type_count)
            || (site->captured_receiver_type != SOL_MIR_MATERIALIZED_NONE
                && site->captured_receiver_type >= o->type_count)) return false;
        if (site->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
            && (!target_valid(o, site->operation.target_kind,
                    site->operation.instance, site->operation.import)
                || site->operation.receiver >= o->type_count
                || (site->operation.root != SOL_MIR_MATERIALIZED_NONE
                    && site->operation.root >= o->place_count)
                || site->operation.effects >= o->effect_row_count
                || site->captured_receiver_kind
                    <= SOL_MIR_MATERIALIZED_RECEIVER_NONE
                || site->captured_receiver_kind
                    > SOL_MIR_MATERIALIZED_RECEIVER_VALUE
                || site->captured_receiver_expression >= o->plan->program->ir->expression_count
                || !slice(site->captured_receiver_roots, o->receiver_root_count)
                || site->captured_receiver_roots.count == 0)) return false;
        for (size_t r = 0; r < site->captured_receiver_roots.count; ++r)
            if (o->receiver_roots[site->captured_receiver_roots.offset + r]
                >= o->local_count) return false;
    }
    return true;
}

static bool terminal(SolMirTerminatorKind kind) {
    return kind == SOL_MIR_TERM_RETURN || kind == SOL_MIR_TERM_PANIC
        || kind == SOL_MIR_TERM_RESUME_FAILURE
        || kind == SOL_MIR_TERM_UNREACHABLE
        || kind == SOL_MIR_TERM_MATCH_FAILURE
        || kind == SOL_MIR_TERM_CONTRACT_VIOLATION;
}

static size_t term_edges(const SolMirMaterializedTerminator *term,
    SolMirMaterializedEdgeId result[3]) {
    size_t count = 0;
#define EDGE(field) do { if (term->field != SOL_MIR_MATERIALIZED_NONE) \
    result[count++] = term->field; } while (0)
    switch (term->kind) {
        case SOL_MIR_TERM_GOTO: case SOL_MIR_TERM_BREAK:
        case SOL_MIR_TERM_CONTINUE: EDGE(edge); break;
        case SOL_MIR_TERM_BRANCH: EDGE(true_edge); EDGE(false_edge); break;
        case SOL_MIR_TERM_INVOKE: EDGE(normal_edge); EDGE(failure_edge); break;
        case SOL_MIR_TERM_CHECK_REFINED: EDGE(normal_edge); EDGE(failure_edge); break;
        case SOL_MIR_TERM_PROPAGATE: EDGE(value_edge); EDGE(residual_edge); break;
        case SOL_MIR_TERM_CHECK_CONTRACT:
            EDGE(satisfied_edge); EDGE(violation_edge); EDGE(failure_edge); break;
        default: break;
    }
#undef EDGE
    return count;
}

static bool block_edges(const View *view, size_t block,
    SolMirMaterializedEdgeId edges[3], size_t *count) {
    if (block >= view->block_count) return false;
    const SolMirMaterializedTerminator *term = &view->owner->blocks[
        view->image->blocks.offset + block].terminator;
    if (term->kind <= SOL_MIR_TERM_INVALID
        || term->kind > SOL_MIR_TERM_CONTRACT_VIOLATION) return false;
#define NO_EDGE(field) (term->field == SOL_MIR_MATERIALIZED_NONE)
    bool canonical_edges = false;
    switch (term->kind) {
        case SOL_MIR_TERM_GOTO: case SOL_MIR_TERM_BREAK:
        case SOL_MIR_TERM_CONTINUE:
            canonical_edges = NO_EDGE(true_edge) && NO_EDGE(false_edge)
                && NO_EDGE(normal_edge) && NO_EDGE(failure_edge)
                && NO_EDGE(value_edge) && NO_EDGE(residual_edge)
                && NO_EDGE(satisfied_edge) && NO_EDGE(violation_edge); break;
        case SOL_MIR_TERM_BRANCH:
            canonical_edges = NO_EDGE(edge) && NO_EDGE(normal_edge)
                && NO_EDGE(failure_edge) && NO_EDGE(value_edge)
                && NO_EDGE(residual_edge) && NO_EDGE(satisfied_edge)
                && NO_EDGE(violation_edge); break;
        case SOL_MIR_TERM_INVOKE: case SOL_MIR_TERM_CHECK_REFINED:
            canonical_edges = NO_EDGE(edge) && NO_EDGE(true_edge)
                && NO_EDGE(false_edge) && NO_EDGE(value_edge)
                && NO_EDGE(residual_edge) && NO_EDGE(satisfied_edge)
                && NO_EDGE(violation_edge); break;
        case SOL_MIR_TERM_PROPAGATE:
            canonical_edges = NO_EDGE(edge) && NO_EDGE(true_edge)
                && NO_EDGE(false_edge) && NO_EDGE(normal_edge)
                && NO_EDGE(failure_edge) && NO_EDGE(satisfied_edge)
                && NO_EDGE(violation_edge); break;
        case SOL_MIR_TERM_CHECK_CONTRACT:
            canonical_edges = NO_EDGE(edge) && NO_EDGE(true_edge)
                && NO_EDGE(false_edge) && NO_EDGE(normal_edge)
                && NO_EDGE(value_edge) && NO_EDGE(residual_edge); break;
        default:
            canonical_edges = NO_EDGE(edge) && NO_EDGE(true_edge)
                && NO_EDGE(false_edge) && NO_EDGE(normal_edge)
                && NO_EDGE(failure_edge) && NO_EDGE(value_edge)
                && NO_EDGE(residual_edge) && NO_EDGE(satisfied_edge)
                && NO_EDGE(violation_edge); break;
    }
#undef NO_EDGE
    if (!canonical_edges) return false;
    *count = term_edges(term, edges);
    size_t expected = 0;
    switch (term->kind) {
        case SOL_MIR_TERM_GOTO: case SOL_MIR_TERM_BREAK:
        case SOL_MIR_TERM_CONTINUE: expected = 1; break;
        case SOL_MIR_TERM_BRANCH: case SOL_MIR_TERM_CHECK_REFINED:
        case SOL_MIR_TERM_PROPAGATE: expected = 2; break;
        case SOL_MIR_TERM_INVOKE:
            if (term->failure_edge == SOL_MIR_MATERIALIZED_NONE) return false;
            expected = term->normal_edge == SOL_MIR_MATERIALIZED_NONE ? 1 : 2;
            break;
        case SOL_MIR_TERM_CHECK_CONTRACT: expected = 3; break;
        default: expected = 0; break;
    }
    if (*count != expected) return false;
    for (size_t i = 0; i < *count; ++i) {
        if (edges[i] >= view->owner->edge_count
            || !in(view->image->blocks, view->owner->edges[edges[i]].block)) {
            return false;
        }
    }
    return true;
}

static bool dominates(const View *view, const size_t *idom, size_t definition,
    size_t use) {
    while (use != definition && use != view->image->entry - view->image->blocks.offset) {
        use = idom[use];
    }
    return use == definition;
}

static bool value_available(const View *view, const size_t *idom, size_t value_id,
    size_t block_id, size_t before) {
    if (!in(view->image->values, value_id)) return false;
    const SolMirMaterializedValue *value = &view->owner->values[value_id];
    if (!in(view->image->blocks, value->block)
        || value->kind == SOL_MIR_VALUE_TERMINATOR) return false;
    size_t definition_block = value->block - view->image->blocks.offset;
    if (definition_block != block_id) return dominates(view, idom,
        definition_block, block_id);
    return value->kind == SOL_MIR_VALUE_BLOCK_PARAMETER
        || (value->kind == SOL_MIR_VALUE_INSTRUCTION
            && value->instruction < before);
}

static bool edge_allows_result(const SolMirMaterializedTerminator *term,
    SolMirMaterializedEdgeId edge, SolMirMaterializedValueId value) {
    if (term->kind == SOL_MIR_TERM_INVOKE) {
        return edge == term->normal_edge && value == term->result;
    }
    if (term->kind == SOL_MIR_TERM_CHECK_REFINED) {
        return edge == term->normal_edge && value == term->result;
    }
    if (term->kind == SOL_MIR_TERM_PROPAGATE) {
        return (edge == term->value_edge && value == term->value_result)
            || (edge == term->residual_edge && value == term->residual_result);
    }
    return false;
}

static size_t edge_value_occurrences(const SolMirMaterialization *o,
    SolMirMaterializedEdgeId edge_id, SolMirMaterializedValueId value) {
    if (edge_id >= o->edge_count) return 0;
    const SolMirMaterializedEdge *edge = &o->edges[edge_id];
    size_t count = 0;
    for (size_t i = 0; i < edge->arguments.count; ++i) {
        count += o->edge_values[edge->arguments.offset + i] == value;
    }
    return count;
}

static bool concrete_types(const View *view) {
    const SolMirMaterialization *o = view->owner;
    const SolMirMaterializedImage *im = view->image;
    if (im->result >= o->type_count
        || (im->receiver != SOL_MIR_MATERIALIZED_NONE && im->receiver >= o->type_count)
        || !slice(im->parameter_types, o->type_id_count)
        || !slice(im->parameter_accesses, o->access_count)
        || im->parameter_types.count != im->parameter_accesses.count) return false;
    for (size_t i = 0; i < im->parameter_types.count; ++i) {
        if (o->type_ids[im->parameter_types.offset + i] >= o->type_count) return false;
    }
    for (size_t i = 0; i < im->locals.count; ++i) {
        const SolMirMaterializedLocal *local = &o->locals[im->locals.offset + i];
        if (local->instance != view->image_id || local->type >= o->type_count) return false;
    }
    for (size_t i = 0; i < im->places.count; ++i) {
        const SolMirMaterializedPlace *place = &o->places[im->places.offset + i];
        if (place->instance != view->image_id || !in(im->locals, place->local)
            || place->root_type != o->locals[place->local].type
            || place->final_type >= o->type_count
            || !slice(place->projections, o->projection_count)) return false;
        SolMirMaterializedTypeId type = place->root_type;
        for (size_t p = 0; p < place->projections.count; ++p) {
            const SolMirMaterializedProjection *projection
                = &o->projections[place->projections.offset + p];
            if (projection->type >= o->type_count) return false;
            type = projection->type;
        }
        if (type != place->final_type) return false;
    }
    return true;
}

static bool validate_type_and_effect_arenas(const SolMirMaterialization *o) {
    unsigned char *copy = o->type_count == 0 ? NULL : malloc(o->type_count);
    unsigned char *fields = o->shape_field_count == 0 ? NULL
        : calloc(o->shape_field_count, 1);
    unsigned char *variants = o->shape_variant_count == 0 ? NULL
        : calloc(o->shape_variant_count, 1);
    if ((o->type_count != 0 && copy == NULL)
        || (o->shape_field_count != 0 && fields == NULL)
        || (o->shape_variant_count != 0 && variants == NULL)) {
        free(copy); free(fields); free(variants); return false;
    }
    bool valid = true;
    for (size_t i = 0; i < o->type_count; ++i) {
        const SolMirMaterializedType *type = &o->types[i];
        if (type->kind < SOL_IR_TYPE_INT64 || type->kind > SOL_IR_TYPE_FUNCTION
            || !slice(type->arguments, o->type_id_count)
            || !slice(type->parameters, o->type_id_count)
            || !slice(type->parameter_accesses, o->access_count)
            || !slice(type->fields, o->shape_field_count)
            || !slice(type->variants, o->shape_variant_count)
            || !slice(type->ownership_components, o->type_id_count)
            || type->parameters.count != type->parameter_accesses.count) {
            valid = false; break;
        }
        bool nominal = type->kind == SOL_IR_TYPE_NOMINAL;
        if ((!nominal && (type->fields.count != 0 || type->variants.count != 0
                    || type->backing != SOL_MIR_MATERIALIZED_NONE
                    || type->capability_source != SOL_MIR_MATERIALIZED_NONE
                    || type->nominal_open))
            || (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_RECORD
                && (type->variants.count != 0
                    || type->backing != SOL_MIR_MATERIALIZED_NONE
                    || type->capability_source != SOL_MIR_MATERIALIZED_NONE))
            || (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_ENUM
                && (type->fields.count != 0
                    || type->backing != SOL_MIR_MATERIALIZED_NONE
                    || type->capability_source != SOL_MIR_MATERIALIZED_NONE))
            || ((type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT
                    || type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_REFINED)
                && (type->fields.count != 0 || type->variants.count != 0
                    || type->backing >= o->type_count
                    || type->capability_source != SOL_MIR_MATERIALIZED_NONE))
            || (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY
                && (type->fields.count != 0 || type->variants.count != 0
                    || type->backing != SOL_MIR_MATERIALIZED_NONE
                    || (type->capability_source != SOL_MIR_MATERIALIZED_NONE
                        && type->capability_source >= o->type_count)))) {
            valid = false; break;
        }
        if (nominal) {
            const SolIr *ir = o->plan->program->ir;
            if (type->definition >= ir->definition_count
                || type->nominal_open != ir->definitions[type->definition].open) {
                valid = false; break;
            }
            const SolIrDefinition *definition = &ir->definitions[type->definition];
            if ((type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_RECORD
                    && (definition->kind != SOL_IR_DEFINITION_RECORD
                        || type->fields.count != definition->fields.count))
                || (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_ENUM
                    && (definition->kind != SOL_IR_DEFINITION_ENUM
                        || type->variants.count != definition->variants.count))
                || (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT
                    && definition->kind != SOL_IR_DEFINITION_DISTINCT)
                || (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_REFINED
                    && definition->kind != SOL_IR_DEFINITION_REFINED)
                || (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY
                    && definition->kind != SOL_IR_DEFINITION_CAPABILITY)) {
                valid = false; break;
            }
            if (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY) {
                SolMirMaterializedTypeId expected = o->plan->types[i].capability_source;
                if (type->capability_source != expected
                    || (definition->capability_source == SOL_IR_NONE)
                        != (expected == SOL_MIR_MATERIALIZED_NONE)) {
                    valid = false; break;
                }
            }
            if ((type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_DISTINCT
                    || type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_REFINED)
                && (type->ownership_components.count != 1
                    || o->type_ids[type->ownership_components.offset]
                        != type->backing)) { valid = false; break; }
        }
        for (size_t f = 0; f < type->fields.count; ++f) {
            size_t slot = type->fields.offset + f;
            const SolMirMaterializedShapeField *field = &o->shape_fields[slot];
            if (fields[slot] || field->ordinal != f || field->type >= o->type_count) {
                valid = false; break;
            }
            if (type->nominal_category == SOL_MIR_MATERIALIZED_NOMINAL_RECORD
                && field->source_field != o->plan->program->ir->definitions[
                    type->definition].fields.offset + f) { valid = false; break; }
            fields[slot] = 1;
        }
        for (size_t v = 0; valid && v < type->variants.count; ++v) {
            size_t slot = type->variants.offset + v;
            const SolMirMaterializedShapeVariant *variant
                = &o->shape_variants[slot];
            if (variants[slot] || variant->ordinal != v
                || !slice(variant->fields, o->shape_field_count)) {
                valid = false; break;
            }
            const SolIrDefinition *definition = &o->plan->program->ir->definitions[
                type->definition];
            if (variant->source_variant != definition->variants.offset + v
                || variant->fields.count != o->plan->program->ir->variants[
                    variant->source_variant].fields.count) { valid = false; break; }
            variants[slot] = 1;
            for (size_t f = 0; f < variant->fields.count; ++f) {
                size_t field_slot = variant->fields.offset + f;
                const SolMirMaterializedShapeField *field
                    = &o->shape_fields[field_slot];
                if (fields[field_slot] || field->ordinal != f
                    || field->type >= o->type_count) { valid = false; break; }
                if (field->source_field != o->plan->program->ir->variants[
                        variant->source_variant].fields.offset + f) {
                    valid = false; break;
                }
                fields[field_slot] = 1;
            }
        }
        if ((type->kind == SOL_IR_TYPE_NOMINAL
                && (type->nominal_category < SOL_MIR_MATERIALIZED_NOMINAL_RECORD
                    || type->nominal_category
                        > SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY))
            || (type->kind != SOL_IR_TYPE_NOMINAL
                && type->nominal_category != SOL_MIR_MATERIALIZED_NOMINAL_NONE)) {
            valid = false; break;
        }
        for (size_t c = 0; c < type->arguments.count; ++c)
            if (o->type_ids[type->arguments.offset + c] >= o->type_count) valid = false;
        for (size_t c = 0; c < type->parameters.count; ++c)
            if (o->type_ids[type->parameters.offset + c] >= o->type_count) valid = false;
        for (size_t c = 0; c < type->ownership_components.count; ++c) {
            size_t child = o->type_ids[type->ownership_components.offset + c];
            if (child >= o->type_count) valid = false;
        }
        if (type->kind == SOL_IR_TYPE_FUNCTION
            && (type->result >= o->type_count || type->effects >= o->effect_row_count))
            valid = false;
        copy[i] = (unsigned char)(type->kind == SOL_IR_TYPE_INT64
            || type->kind == SOL_IR_TYPE_BOOL || type->kind == SOL_IR_TYPE_TEXT
            || type->kind == SOL_IR_TYPE_UNIT || type->kind == SOL_IR_TYPE_NEVER
            || type->kind == SOL_IR_TYPE_OPTION || type->kind == SOL_IR_TYPE_RESULT
            || type->kind == SOL_IR_TYPE_TUPLE
            || (type->kind == SOL_IR_TYPE_NOMINAL
                && type->nominal_category != SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY));
    }
    bool changed = valid;
    size_t passes = 0;
    while (valid && changed && passes++ <= o->type_count) {
        changed = false;
        for (size_t i = 0; i < o->type_count; ++i) {
            if (!copy[i]) continue;
            const SolMirMaterializedType *type = &o->types[i];
            for (size_t c = 0; c < type->ownership_components.count; ++c) {
                size_t child = o->type_ids[type->ownership_components.offset + c];
                if (!copy[child]) { copy[i] = 0; changed = true; break; }
            }
        }
    }
    valid = valid && !changed;
    for (size_t i = 0; valid && i < o->type_count; ++i)
        valid = o->types[i].is_copy == (copy[i] != 0);
    for (size_t i = 0; valid && i < o->shape_field_count; ++i)
        valid = fields[i] == 1;
    for (size_t i = 0; valid && i < o->shape_variant_count; ++i)
        valid = variants[i] == 1;
    free(copy); free(fields); free(variants);
    for (size_t i = 0; valid && i < o->effect_atom_count; ++i) {
        const SolMirMaterializedEffectAtom *atom = &o->effect_atoms[i];
        if (!slice(atom->name, o->effect_name_count)
            || atom->name.offset + atom->name.count >= o->effect_name_count
            || o->effect_names[atom->name.offset + atom->name.count] != '\0') valid = false;
    }
    for (size_t i = 0; valid && i < o->effect_row_count; ++i) {
        const SolMirMaterializedEffectRow *row = &o->effect_rows[i];
        if (!slice(row->atoms, o->effect_row_atom_count)) { valid = false; break; }
        for (size_t a = 0; a < row->atoms.count; ++a)
            if (o->effect_row_atoms[row->atoms.offset + a] >= o->effect_atom_count)
                valid = false;
    }
    for (size_t i = 0; valid && i < o->import_count; ++i) {
        const SolMirMaterializedImport *import = &o->imports[i];
        if ((import->receiver != SOL_MIR_MATERIALIZED_NONE
                && import->receiver >= o->type_count)
            || import->result >= o->type_count || import->effects >= o->effect_row_count
            || !slice(import->parameter_types, o->type_id_count)
            || !slice(import->parameter_accesses, o->access_count)
            || import->parameter_types.count != import->parameter_accesses.count)
            valid = false;
        for (size_t p = 0; valid && p < import->parameter_types.count; ++p)
            if (o->type_ids[import->parameter_types.offset + p] >= o->type_count)
                valid = false;
    }
    return valid;
}

static bool argument_signature(const SolMirMaterialization *o,
    const SolMirMaterializedBinding *binding, SolMirMaterializedTypeId *receiver,
    SolMirPlanSlice *parameters, SolMirPlanSlice *accesses,
    SolMirMaterializedTypeId *result, SolMirMaterializedEffectRowId *effects) {
    if (binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
        if (binding->instance >= o->image_count) return false;
        const SolMirMaterializedImage *target = &o->images[binding->instance];
        *receiver = target->receiver; *parameters = target->parameter_types;
        *accesses = target->parameter_accesses; *result = target->result;
        *effects = target->effects;
    } else if (binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
        if (binding->import >= o->import_count) return false;
        const SolMirMaterializedImport *target = &o->imports[binding->import];
        *receiver = target->receiver; *parameters = target->parameter_types;
        *accesses = target->parameter_accesses; *result = target->result;
        *effects = target->effects;
    } else return false;
    return true;
}

static SolAccessMode binding_receiver_access(const SolMirMaterialization *o,
    const SolMirMaterializedBinding *binding) {
    return binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
        ? o->images[binding->instance].receiver_access
        : o->imports[binding->import].receiver_access;
}

static bool places_overlap(const SolMirMaterialization *o, size_t left,
    size_t right) {
    const SolMirMaterializedPlace *a = &o->places[left], *b = &o->places[right];
    if (a->local != b->local) return false;
    size_t common = a->projections.count < b->projections.count
        ? a->projections.count : b->projections.count;
    for (size_t i = 0; i < common; ++i) {
        const SolMirMaterializedProjection *x
            = &o->projections[a->projections.offset + i];
        const SolMirMaterializedProjection *y
            = &o->projections[b->projections.offset + i];
        if (x->kind != y->kind || x->source_field != y->source_field
            || x->tuple_ordinal != y->tuple_ordinal) return false;
    }
    return true;
}

static bool instruction_semantics(const View *view,
    const SolMirMaterializedInstruction *ins) {
    const SolMirMaterialization *o = view->owner;
    if (!in(view->image->blocks, ins->block)) return false;
    if (ins->result != SOL_MIR_MATERIALIZED_NONE) {
        if (!in(view->image->values, ins->result)
            || o->values[ins->result].kind != SOL_MIR_VALUE_INSTRUCTION
            || o->values[ins->result].instruction != (size_t)(ins - o->instructions)
            || o->values[ins->result].type != ins->type) return false;
    }
    switch (ins->kind) {
        case SOL_MIR_INST_CONST_INT64:
            return ins->type < o->type_count
                && o->types[ins->type].kind == SOL_IR_TYPE_INT64;
        case SOL_MIR_INST_CONST_BOOL:
            return ins->type < o->type_count
                && o->types[ins->type].kind == SOL_IR_TYPE_BOOL;
        case SOL_MIR_INST_CONST_TEXT:
            return ins->type < o->type_count
                && o->types[ins->type].kind == SOL_IR_TYPE_TEXT
                && slice(ins->text, o->literal_byte_count);
        case SOL_MIR_INST_CONST_UNIT:
            return ins->type < o->type_count
                && o->types[ins->type].kind == SOL_IR_TYPE_UNIT;
        case SOL_MIR_INST_PARAMETER_LIVE: case SOL_MIR_INST_STORAGE_LIVE:
        case SOL_MIR_INST_DROP_IF_INITIALIZED: case SOL_MIR_INST_STORAGE_DEAD:
            return in(view->image->locals, ins->local);
        case SOL_MIR_INST_LOAD_COPY:
            return in(view->image->places, ins->place)
                && ins->type == o->places[ins->place].final_type
                && o->types[ins->type].is_copy;
        case SOL_MIR_INST_LOAD_MOVE:
            return in(view->image->places, ins->place)
                && ins->type == o->places[ins->place].final_type
                && !o->types[ins->type].is_copy;
        case SOL_MIR_INST_LOAD_UPDATE:
            return in(view->image->places, ins->place)
                && ins->type == o->places[ins->place].final_type;
        case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
            return in(view->image->places, ins->place);
        case SOL_MIR_INST_STORE:
            return in(view->image->places, ins->place)
                && in(view->image->values, ins->left)
                && o->values[ins->left].type == o->places[ins->place].final_type;
        case SOL_MIR_INST_UNARY:
            return in(view->image->values, ins->left)
                && o->values[ins->left].type == ins->type;
        case SOL_MIR_INST_BINARY:
            return in(view->image->values, ins->left)
                && in(view->image->values, ins->right)
                && o->values[ins->left].type == o->values[ins->right].type
                && (o->values[ins->left].type == ins->type
                    || (ins->type < o->type_count
                        && o->types[ins->type].kind == SOL_IR_TYPE_BOOL));
        case SOL_MIR_INST_COMPOUND_UPDATE:
            return in(view->image->places, ins->place)
                && in(view->image->temporaries, ins->previous)
                && in(view->image->values, ins->right)
                && o->temporaries[ins->previous].type
                    == o->places[ins->place].final_type
                && o->values[ins->right].type == o->places[ins->place].final_type;
        case SOL_MIR_INST_TEMPORARY_INIT:
            return in(view->image->temporaries, ins->temporary)
                && in(view->image->values, ins->left)
                && o->temporaries[ins->temporary].type == o->values[ins->left].type;
        case SOL_MIR_INST_TEMPORARY_DROP:
            return in(view->image->temporaries, ins->temporary);
        case SOL_MIR_INST_EXPRESSION_RESULT:
            return in(view->image->values, ins->left)
                && o->values[ins->left].type == ins->type;
        case SOL_MIR_INST_PATTERN_TEST: case SOL_MIR_INST_PATTERN_VALUE:
        case SOL_MIR_INST_MATCH_ARM:
            return in(view->image->temporaries, ins->pattern_scrutinee);
        case SOL_MIR_INST_HANDLER_ENTER: case SOL_MIR_INST_HANDLER_EXIT:
            return in(view->image->handlers, ins->handler);
        case SOL_MIR_INST_CONSTRUCT:
            if (!slice(ins->construct_operands, o->construct_operand_count)) return false;
            for (size_t i = 0; i < ins->construct_operands.count; ++i) {
                const SolMirMaterializedConstructOperand *operand
                    = &o->construct_operands[ins->construct_operands.offset + i];
                if (!in(view->image->temporaries, operand->temporary)
                    || operand->type != o->temporaries[operand->temporary].type) return false;
            }
            return ins->type < o->type_count;
        case SOL_MIR_INST_CAPTURE_SNAPSHOT:
            for (size_t i = 0; i < view->image->overlays.count; ++i) {
                const SolMirMaterializedTypeOverlay *overlay
                    = &o->overlays[view->image->overlays.offset + i];
                if (overlay->kind == SOL_MIR_PLAN_USE_SNAPSHOT
                    && overlay->source == ins->source_snapshot) {
                    return overlay->type == ins->type
                        && overlay->type < o->type_count
                        && o->types[overlay->type].is_copy;
                }
            }
            return false;
        case SOL_MIR_INST_REGION_ENTER: case SOL_MIR_INST_REGION_EXIT:
            return true;
    }
    return false;
}

static bool terminator_semantics(const View *view, size_t block_id) {
    const SolMirMaterialization *o = view->owner;
    const SolMirMaterializedBlock *block
        = &o->blocks[view->image->blocks.offset + block_id];
    const SolMirMaterializedTerminator *term = &block->terminator;
    bool predicate_check = term->kind == SOL_MIR_TERM_CHECK_REFINED
        || term->kind == SOL_MIR_TERM_CHECK_CONTRACT;
    if (!predicate_check && term->predicate_inline) return false;
    if (term->kind != SOL_MIR_TERM_INVOKE
        && term->callable_site != SOL_MIR_MATERIALIZED_NONE) return false;
    if (term->kind == SOL_MIR_TERM_BRANCH) {
        return in(view->image->values, term->condition)
            && o->types[o->values[term->condition].type].kind == SOL_IR_TYPE_BOOL;
    }
    if (term->kind == SOL_MIR_TERM_RETURN) {
        return in(view->image->values, term->value)
            && o->values[term->value].type == view->image->result;
    }
    if (term->kind == SOL_MIR_TERM_PANIC) {
        return in(view->image->values, term->value)
            && o->types[o->values[term->value].type].kind == SOL_IR_TYPE_TEXT;
    }
    if (term->kind == SOL_MIR_TERM_BREAK || term->kind == SOL_MIR_TERM_CONTINUE) {
        return in(view->image->loops, term->loop)
            && o->edges[term->edge].arguments.count == 0;
    }
    if (term->kind == SOL_MIR_TERM_INVOKE) {
        if (term->binding >= o->binding_count
            || o->bindings[term->binding].parent != view->image_id
            || term->effects >= o->effect_row_count
            || !slice(term->arguments, o->call_argument_count)
            || !slice(term->writebacks, o->writeback_count)) return false;
        const SolMirMaterializedBinding *invoke_binding
            = &o->bindings[term->binding];
        if ((term->call_kind == SOL_IR_CALL_CALLBACK
                && (invoke_binding->kind != SOL_MIR_PLAN_DEMAND_CALLBACK
                    || !in(view->image->temporaries, term->callee)
                    || term->receiver.source_expression != SOL_IR_NONE))
            || (term->call_kind != SOL_IR_CALL_CALLBACK
                && (invoke_binding->kind != SOL_MIR_PLAN_DEMAND_INVOKE
                    || term->callee != SOL_MIR_MATERIALIZED_NONE))
            || (term->call_kind == SOL_IR_CALL_FUNCTION
                && term->receiver.source_expression != SOL_IR_NONE)
            || ((term->call_kind == SOL_IR_CALL_METHOD
                    || term->call_kind == SOL_IR_CALL_CAPABILITY)
                && term->receiver.source_expression == SOL_IR_NONE)
            || (term->call_kind != SOL_IR_CALL_FUNCTION
                && term->call_kind != SOL_IR_CALL_CALLBACK
                && term->call_kind != SOL_IR_CALL_METHOD
                && term->call_kind != SOL_IR_CALL_CAPABILITY)) return false;
        bool callable_producer = term->callable_site != SOL_MIR_MATERIALIZED_NONE;
        if (term->call_kind == SOL_IR_CALL_CALLBACK && !callable_producer) return false;
        if (callable_producer) {
            if (term->callable_site >= o->semantic_site_count) return false;
            const SolMirMaterializedSemanticSite *site
                = &o->semantic_sites[term->callable_site];
            if (site->producer_kind != SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR
                || site->block != block->id || site->parent != view->image_id
                || (term->call_kind == SOL_IR_CALL_CALLBACK
                    ? site->kind != SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE
                        && site->kind != SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
                    : site->kind != SOL_MIR_PLAN_DEMAND_BOUND_OPERATION)
                || !targets_equal(&o->bindings[site->binding], invoke_binding)) return false;
            if (site->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
                && !operation_key_matches(&site->operation, invoke_binding,
                    term->receiver.type, term->receiver.place, term->effects)) return false;
        }
        SolMirMaterializedTypeId receiver, result;
        SolMirMaterializedEffectRowId effects;
        SolMirPlanSlice parameters, accesses;
        if (!argument_signature(o, &o->bindings[term->binding], &receiver,
                &parameters, &accesses, &result, &effects)
            || effects != term->effects || parameters.count != term->arguments.count
            || accesses.count != parameters.count) return false;
        if ((receiver == SOL_MIR_MATERIALIZED_NONE)
                != (term->receiver.source_expression == SOL_IR_NONE)) return false;
        if (receiver != SOL_MIR_MATERIALIZED_NONE) {
            if (term->receiver.type != receiver
                || term->receiver.access
                    != binding_receiver_access(o, invoke_binding)) return false;
            if (term->receiver.access == SOL_ACCESS_OWNED) {
                if (!in(view->image->temporaries, term->receiver.temporary)
                    || o->temporaries[term->receiver.temporary].type != receiver
                    || term->receiver.place != SOL_MIR_MATERIALIZED_NONE) return false;
            } else if ((term->receiver.access != SOL_ACCESS_SHARED
                    && term->receiver.access != SOL_ACCESS_EXCLUSIVE)
                || !in(view->image->places, term->receiver.place)
                || o->places[term->receiver.place].final_type != receiver
                || term->receiver.temporary != SOL_MIR_MATERIALIZED_NONE) return false;
        }
        if ((term->call_kind == SOL_IR_CALL_FUNCTION
                || term->call_kind == SOL_IR_CALL_METHOD)
            && invoke_binding->target_kind != SOL_MIR_MATERIALIZED_TARGET_INSTANCE)
            return false;
        if (term->call_kind == SOL_IR_CALL_CALLBACK) {
            SolMirMaterializedTypeId callable_type
                = o->temporaries[term->callee].type;
            if (callable_type >= o->type_count
                || o->types[callable_type].kind != SOL_IR_TYPE_FUNCTION) return false;
            const SolMirMaterializedType *function = &o->types[callable_type];
            if (function->result != result || function->effects != effects
                || function->parameters.count != parameters.count
                || function->parameter_accesses.count != accesses.count) return false;
            for (size_t i = 0; i < parameters.count; ++i) {
                if (o->type_ids[function->parameters.offset + i]
                        != o->type_ids[parameters.offset + i]
                    || o->accesses[function->parameter_accesses.offset + i]
                        != o->accesses[accesses.offset + i]) return false;
            }
        }
        size_t exclusive = receiver != SOL_MIR_MATERIALIZED_NONE
            && term->receiver.access == SOL_ACCESS_EXCLUSIVE;
        for (size_t i = 0; i < term->arguments.count; ++i) {
            const SolMirMaterializedCallArgument *argument
                = &o->call_arguments[term->arguments.offset + i];
            SolMirMaterializedTypeId expected = o->type_ids[parameters.offset + i];
            if (argument->formal != i || argument->type != expected
                || argument->access != o->accesses[accesses.offset + i]) return false;
            if (argument->access == SOL_ACCESS_OWNED) {
                if (!in(view->image->temporaries, argument->temporary)
                    || o->temporaries[argument->temporary].type != expected) return false;
            } else if (!in(view->image->places, argument->place)
                || o->places[argument->place].final_type != expected) return false;
            if (argument->access != SOL_ACCESS_OWNED) {
                for (size_t j = 0; j < i; ++j) {
                    const SolMirMaterializedCallArgument *other
                        = &o->call_arguments[term->arguments.offset + j];
                    if (other->access != SOL_ACCESS_OWNED
                        && (argument->access == SOL_ACCESS_EXCLUSIVE
                            || other->access == SOL_ACCESS_EXCLUSIVE)
                        && places_overlap(o, argument->place, other->place)) return false;
                }
                if (receiver != SOL_MIR_MATERIALIZED_NONE
                    && term->receiver.access != SOL_ACCESS_OWNED
                    && (argument->access == SOL_ACCESS_EXCLUSIVE
                        || term->receiver.access == SOL_ACCESS_EXCLUSIVE)
                    && places_overlap(o, argument->place, term->receiver.place)) return false;
            }
            if (argument->access == SOL_ACCESS_EXCLUSIVE) {
                ++exclusive;
            }
        }
        if ((term->result == SOL_MIR_MATERIALIZED_NONE)
                != (term->normal_edge == SOL_MIR_MATERIALIZED_NONE)
            || (term->result != SOL_MIR_MATERIALIZED_NONE
                && (!in(view->image->values, term->result)
                    || o->values[term->result].type != result
                    || o->types[result].kind == SOL_IR_TYPE_NEVER))
            || (term->result == SOL_MIR_MATERIALIZED_NONE
                && o->types[result].kind != SOL_IR_TYPE_NEVER)) return false;
        if (term->result != SOL_MIR_MATERIALIZED_NONE
            && edge_value_occurrences(o, term->normal_edge, term->result) != 1) {
            return false;
        }
        if (o->edges[term->failure_edge].arguments.count != 0
            || (term->normal_edge != SOL_MIR_MATERIALIZED_NONE
                && o->edges[term->normal_edge].arguments.count != 1)) return false;
        if (term->writebacks.count != (term->normal_edge == SOL_MIR_MATERIALIZED_NONE
                ? 0 : exclusive)) return false;
        size_t at = 0;
        if (term->normal_edge != SOL_MIR_MATERIALIZED_NONE
            && receiver != SOL_MIR_MATERIALIZED_NONE
            && term->receiver.access == SOL_ACCESS_EXCLUSIVE) {
            const SolMirMaterializedWriteback *w
                = &o->writebacks[term->writebacks.offset + at++];
            if (!w->receiver || w->place != term->receiver.place
                || w->type != receiver) return false;
        }
        for (size_t i = 0; i < term->arguments.count; ++i) {
            const SolMirMaterializedCallArgument *a
                = &o->call_arguments[term->arguments.offset + i];
            if (term->normal_edge == SOL_MIR_MATERIALIZED_NONE
                || a->access != SOL_ACCESS_EXCLUSIVE) continue;
            const SolMirMaterializedWriteback *w
                = &o->writebacks[term->writebacks.offset + at++];
            if (w->receiver || w->formal != i || w->place != a->place
                || w->type != a->type) return false;
        }
    } else if (term->kind == SOL_MIR_TERM_CHECK_REFINED) {
        if (!in(view->image->temporaries, term->representation)
            || !in(view->image->values, term->result)
            || o->values[term->result].kind != SOL_MIR_VALUE_TERMINATOR) return false;
        const SolMirMaterializedType *result = &o->types[o->values[term->result].type];
        if (result->kind != SOL_IR_TYPE_NOMINAL
            || result->nominal_category != SOL_MIR_MATERIALIZED_NOMINAL_REFINED
            || result->ownership_components.count != 1
            || o->type_ids[result->ownership_components.offset]
                != o->temporaries[term->representation].type) return false;
        size_t contexts = 0, predicates = 0;
        size_t matched_context = SOL_MIR_MATERIALIZED_NONE;
        for (size_t i = 0; i < view->image->contexts.count; ++i) {
            size_t id = view->image->contexts.offset + i;
            const SolMirPlanContext *context = &o->contexts[id];
            if (context->kind == SOL_MIR_PLAN_CONTEXT_REFINEMENT
                && context->instance == view->image_id
                && context->definition == term->source_definition
                && context->obligation == term->source_obligation
                && context->source.expression == term->source_expression) {
                ++contexts;
                matched_context = id;
                size_t predicate_overlays = 0;
                for (size_t u = 0; u < view->image->overlays.count; ++u) {
                    const SolMirMaterializedTypeOverlay *overlay
                        = &o->overlays[view->image->overlays.offset + u];
                    if (overlay->kind == SOL_MIR_PLAN_USE_OBLIGATION_PREDICATE
                        && overlay->context == id
                        && overlay->source == term->source_obligation) {
                        ++predicate_overlays;
                        if (overlay->type >= o->type_count
                            || o->types[overlay->type].kind != SOL_IR_TYPE_BOOL) {
                            return false;
                        }
                    }
                }
                if (predicate_overlays != 1) return false;
                for (size_t b = 0; b < view->image->bindings.count; ++b) {
                    const SolMirMaterializedBinding *binding
                        = &o->bindings[view->image->bindings.offset + b];
                    predicates += (binding->kind == SOL_MIR_PLAN_DEMAND_PREDICATE
                            || binding->kind == SOL_MIR_PLAN_DEMAND_CALLBACK)
                        && binding->context == id;
                }
            }
        }
        if (contexts != 1 || term->predicate_inline != (predicates == 0)) return false;
        for (size_t s = 0; s < o->semantic_site_count; ++s) {
            const SolMirMaterializedSemanticSite *site = &o->semantic_sites[s];
            if (site->parent == view->image_id && site->context == matched_context
                && site->source_obligation == term->source_obligation
                && site->producer_kind == SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE
                && site->block != block->id) return false;
        }
        if (edge_value_occurrences(o, term->normal_edge, term->result) != 1
            || edge_value_occurrences(o, term->failure_edge, term->result) != 0) {
            return false;
        }
        if (o->edges[term->normal_edge].arguments.count != 1
            || o->edges[term->failure_edge].arguments.count != 0) return false;
        const SolMirMaterializedTerminator *failure = cleanup_terminal(view,
            o->edges[term->failure_edge].block);
        if (failure == NULL || failure->kind != SOL_MIR_TERM_RESUME_FAILURE) {
            return false;
        }
    } else if (term->kind == SOL_MIR_TERM_PROPAGATE) {
        if (!in(view->image->temporaries, term->operand)
            || !in(view->image->values, term->value_result)
            || !in(view->image->values, term->residual_result)
            || o->values[term->value_result].kind != SOL_MIR_VALUE_TERMINATOR
            || o->values[term->residual_result].kind != SOL_MIR_VALUE_TERMINATOR
            || edge_value_occurrences(o, term->value_edge, term->value_result) != 1
            || edge_value_occurrences(o, term->residual_edge,
                term->residual_result) != 1
            || edge_value_occurrences(o, term->value_edge,
                term->residual_result) != 0
            || edge_value_occurrences(o, term->residual_edge,
                term->value_result) != 0) return false;
        SolMirMaterializedTypeId operand_type = o->temporaries[term->operand].type;
        if (operand_type >= o->type_count) return false;
        const SolMirMaterializedType *operand = &o->types[operand_type];
        SolIrTypeKind aggregate_kind;
        size_t argument_count;
        if (term->propagation_kind == SOL_IR_PROPAGATE_OPTION) {
            aggregate_kind = SOL_IR_TYPE_OPTION; argument_count = 1;
        } else if (term->propagation_kind == SOL_IR_PROPAGATE_RESULT) {
            aggregate_kind = SOL_IR_TYPE_RESULT; argument_count = 2;
        } else return false;
        const SolMirMaterializedType *callable_result
            = &o->types[view->image->result];
        if (operand->kind != aggregate_kind || operand->arguments.count != argument_count
            || callable_result->kind != aggregate_kind
            || callable_result->arguments.count != argument_count
            || o->values[term->value_result].type
                != o->type_ids[operand->arguments.offset]
            || o->values[term->residual_result].type != view->image->result
            || (aggregate_kind == SOL_IR_TYPE_RESULT
                && o->type_ids[operand->arguments.offset + 1]
                    != o->type_ids[callable_result->arguments.offset + 1])
            || o->edges[term->value_edge].arguments.count != 1
            || o->edges[term->residual_edge].arguments.count != 1) return false;
        const SolMirMaterializedBlock *residual
            = &o->blocks[o->edges[term->residual_edge].block];
        if (residual->parameters.count != 1) return false;
        size_t residual_parameter
            = o->parameter_values[residual->parameters.offset];
        if (view->image->contract_epilogue == SOL_MIR_MATERIALIZED_NONE) {
            if (residual->terminator.kind != SOL_MIR_TERM_RETURN
                || residual->terminator.value != residual_parameter) return false;
        } else if (residual->terminator.kind != SOL_MIR_TERM_GOTO
            || o->edges[residual->terminator.edge].block
                != view->image->contract_epilogue
            || o->edges[residual->terminator.edge].arguments.count != 1
            || o->edge_values[o->edges[residual->terminator.edge].arguments.offset]
                != residual_parameter) return false;
    } else if (term->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
        if (term->result != SOL_MIR_MATERIALIZED_NONE
            && (!in(view->image->values, term->result)
                || o->values[term->result].type != view->image->result)) return false;
        if (o->edges[term->violation_edge].arguments.count != 0
            || o->edges[term->failure_edge].arguments.count != 0
            || o->edges[term->satisfied_edge].arguments.count
                != (term->result == SOL_MIR_MATERIALIZED_NONE ? 0u : 1u)
            || (term->result != SOL_MIR_MATERIALIZED_NONE
                && edge_value_occurrences(o, term->satisfied_edge,
                    term->result) != 1)) return false;
    }
    return true;
}

static size_t *compute_idom(const View *view) {
    size_t n = view->block_count;
    if (n == 0 || n > SIZE_MAX / sizeof(size_t)) return NULL;
    size_t *pred_count = calloc(n, sizeof(*pred_count));
    size_t *offset = calloc(n + 1, sizeof(*offset));
    size_t *cursor = calloc(n, sizeof(*cursor));
    size_t *stack = malloc(n * sizeof(*stack));
    size_t *next = calloc(n, sizeof(*next));
    size_t *post = malloc(n * sizeof(*post));
    size_t *rpo = malloc(n * sizeof(*rpo));
    size_t *idom = malloc(n * sizeof(*idom));
    unsigned char *seen = calloc(n, 1);
    bool valid = pred_count && offset && cursor && stack && next && post && rpo
        && idom && seen;
    for (size_t b = 0; valid && b < n; ++b) {
        SolMirMaterializedEdgeId edges[3]; size_t count;
        valid = block_edges(view, b, edges, &count);
        for (size_t i = 0; valid && i < count; ++i) {
            size_t target = view->owner->edges[edges[i]].block
                - view->image->blocks.offset;
            valid = pred_count[target] != SIZE_MAX;
            if (valid) ++pred_count[target];
        }
    }
    for (size_t b = 0; valid && b < n; ++b) {
        valid = offset[b] <= SIZE_MAX - pred_count[b];
        if (valid) offset[b + 1] = offset[b] + pred_count[b];
    }
    size_t total = valid ? offset[n] : 0;
    if (valid && total > SIZE_MAX / sizeof(size_t)) valid = false;
    size_t *pred = !valid || total == 0 ? NULL : malloc(total * sizeof(*pred));
    valid = valid && (total == 0 || pred != NULL);
    for (size_t b = 0; valid && b < n; ++b) {
        SolMirMaterializedEdgeId edges[3]; size_t count;
        valid = block_edges(view, b, edges, &count);
        for (size_t i = 0; valid && i < count; ++i) {
            size_t target = view->owner->edges[edges[i]].block
                - view->image->blocks.offset;
            pred[offset[target] + cursor[target]++] = b;
        }
    }
    size_t entry = view->image->entry - view->image->blocks.offset;
    size_t depth = 0, post_count = 0;
    if (valid && entry < n) { stack[depth++] = entry; seen[entry] = 1; }
    else valid = false;
    while (valid && depth != 0) {
        size_t b = stack[depth - 1]; SolMirMaterializedEdgeId edges[3]; size_t count;
        valid = block_edges(view, b, edges, &count);
        if (!valid) break;
        if (next[b] < count) {
            size_t target = view->owner->edges[edges[next[b]++]].block
                - view->image->blocks.offset;
            if (!seen[target]) { seen[target] = 1; stack[depth++] = target; }
        } else { post[post_count++] = b; --depth; }
    }
    valid = valid && post_count == n && pred_count[entry] == 0;
    for (size_t order = 0; valid && order < n; ++order) {
        size_t b = post[n - order - 1]; rpo[b] = order; idom[b] = SIZE_MAX;
    }
    if (valid) idom[entry] = entry;
    bool changed = valid;
    size_t passes = 0, limit = n > SIZE_MAX / n ? SIZE_MAX : n * n + 1;
    while (valid && changed && passes++ < limit) {
        changed = false;
        for (size_t order = 1; order < n; ++order) {
            size_t b = post[n - order - 1], candidate = SIZE_MAX;
            for (size_t p = offset[b]; p < offset[b + 1]; ++p) {
                size_t other = pred[p]; if (idom[other] == SIZE_MAX) continue;
                if (candidate == SIZE_MAX) candidate = other;
                else while (candidate != other) {
                    while (rpo[candidate] > rpo[other]) candidate = idom[candidate];
                    while (rpo[other] > rpo[candidate]) other = idom[other];
                }
            }
            if (candidate != SIZE_MAX && idom[b] != candidate) {
                idom[b] = candidate; changed = true;
            }
        }
    }
    valid = valid && !changed;
    for (size_t b = 0; valid && b < n; ++b) valid = idom[b] != SIZE_MAX;
    free(pred_count); free(offset); free(cursor); free(stack); free(next);
    free(post); free(rpo); free(seen); free(pred);
    if (!valid) { free(idom); return NULL; }
    return idom;
}

static bool validate_ssa(const View *view) {
    const SolMirMaterialization *o = view->owner;
    size_t *idom = compute_idom(view);
    if (idom == NULL) return false;
    bool valid = true;
    unsigned char *defined = calloc(view->value_count, 1);
    if (view->value_count != 0 && defined == NULL) valid = false;
    for (size_t value = 0; valid && value < view->value_count; ++value) {
        valid = o->values[view->image->values.offset + value].type < o->type_count;
    }
    for (size_t b = 0; valid && b < view->block_count; ++b) {
        const SolMirMaterializedBlock *block
            = &o->blocks[view->image->blocks.offset + b];
        if (block->id != view->image->blocks.offset + b
            || !slice(block->parameters, o->parameter_value_count)
            || !slice(block->instructions, o->instruction_count)
            || block->instructions.offset < view->image->instructions.offset
            || block->instructions.offset - view->image->instructions.offset
                    > view->image->instructions.count
            || block->instructions.count > view->image->instructions.count
                    - (block->instructions.offset
                        - view->image->instructions.offset)) {
            valid = false; break;
        }
        for (size_t p = 0; p < block->parameters.count; ++p) {
            size_t value = o->parameter_values[block->parameters.offset + p];
            if (!in(view->image->values, value)
                || o->values[value].kind != SOL_MIR_VALUE_BLOCK_PARAMETER
                || o->values[value].block != block->id
                || defined[value - view->image->values.offset]++) valid = false;
        }
        for (size_t i = 0; valid && i < block->instructions.count; ++i) {
            size_t iid = block->instructions.offset + i;
            const SolMirMaterializedInstruction *ins = &o->instructions[iid];
            if (ins->block != block->id || !instruction_semantics(view, ins)) {
                valid = false; break;
            }
            size_t uses[2], count = 0;
            if (ins->kind == SOL_MIR_INST_STORE || ins->kind == SOL_MIR_INST_UNARY
                || ins->kind == SOL_MIR_INST_TEMPORARY_INIT
                || ins->kind == SOL_MIR_INST_EXPRESSION_RESULT) uses[count++] = ins->left;
            else if (ins->kind == SOL_MIR_INST_BINARY) {
                uses[count++] = ins->left; uses[count++] = ins->right;
            } else if (ins->kind == SOL_MIR_INST_COMPOUND_UPDATE) uses[count++] = ins->right;
            for (size_t u = 0; valid && u < count; ++u) valid = value_available(
                view, idom, uses[u], b, iid);
            if (ins->result != SOL_MIR_MATERIALIZED_NONE) {
                size_t relative = ins->result - view->image->values.offset;
                if (defined[relative]++) valid = false;
            }
        }
        if (!valid || !terminator_semantics(view, b)) { valid = false; break; }
        const SolMirMaterializedTerminator *term = &block->terminator;
        size_t direct[2], direct_count = 0;
        if (term->kind == SOL_MIR_TERM_BRANCH) direct[direct_count++] = term->condition;
        else if (term->kind == SOL_MIR_TERM_RETURN || term->kind == SOL_MIR_TERM_PANIC)
            direct[direct_count++] = term->value;
        else if (term->kind == SOL_MIR_TERM_CHECK_CONTRACT
            && term->result != SOL_MIR_MATERIALIZED_NONE) direct[direct_count++] = term->result;
        size_t before = block->instructions.offset + block->instructions.count;
        for (size_t i = 0; valid && i < direct_count; ++i) valid = value_available(
            view, idom, direct[i], b, before);
        SolMirMaterializedEdgeId edges[3]; size_t edge_count;
        valid = valid && block_edges(view, b, edges, &edge_count);
        for (size_t e = 0; valid && e < edge_count; ++e) {
            const SolMirMaterializedEdge *edge = &o->edges[edges[e]];
            const SolMirMaterializedBlock *target = &o->blocks[edge->block];
            if (!slice(edge->arguments, o->edge_value_count)
                || edge->arguments.count != target->parameters.count) {
                valid = false; break;
            }
            for (size_t a = 0; valid && a < edge->arguments.count; ++a) {
                size_t value = o->edge_values[edge->arguments.offset + a];
                size_t parameter = o->parameter_values[target->parameters.offset + a];
                if (!in(view->image->values, value)
                    || o->values[value].type != o->values[parameter].type) valid = false;
                else if (o->values[value].kind == SOL_MIR_VALUE_TERMINATOR) {
                    valid = o->values[value].block == block->id
                        && edge_allows_result(term, edges[e], value);
                } else valid = value_available(view, idom, value, b, before);
            }
        }
        size_t produced[2], produced_count = 0;
        if (term->kind == SOL_MIR_TERM_INVOKE || term->kind == SOL_MIR_TERM_CHECK_REFINED) {
            if (term->result != SOL_MIR_MATERIALIZED_NONE) produced[produced_count++] = term->result;
        } else if (term->kind == SOL_MIR_TERM_PROPAGATE) {
            produced[produced_count++] = term->value_result;
            produced[produced_count++] = term->residual_result;
        }
        for (size_t i = 0; valid && i < produced_count; ++i) {
            size_t value = produced[i];
            if (!in(view->image->values, value)
                || o->values[value].kind != SOL_MIR_VALUE_TERMINATOR
                || o->values[value].block != block->id
                || defined[value - view->image->values.offset]++) valid = false;
        }
    }
    for (size_t v = 0; valid && v < view->value_count; ++v) valid = defined[v] == 1;
    free(defined); free(idom); return valid;
}

static bool path_available(const View *v, const unsigned char *holes, size_t place) {
    for (size_t i = 0; i < v->place_count; ++i) {
        if (holes[i] && places_overlap(v->owner,
                v->image->places.offset + i, place)) return false;
    }
    return true;
}

static bool path_prefix(const SolMirMaterialization *o, size_t prefix,
    size_t place) {
    const SolMirMaterializedPlace *a = &o->places[prefix];
    const SolMirMaterializedPlace *b = &o->places[place];
    if (a->local != b->local || a->projections.count > b->projections.count) {
        return false;
    }
    for (size_t i = 0; i < a->projections.count; ++i) {
        const SolMirMaterializedProjection *x
            = &o->projections[a->projections.offset + i];
        const SolMirMaterializedProjection *y
            = &o->projections[b->projections.offset + i];
        if (x->kind != y->kind || x->source_field != y->source_field
            || x->tuple_ordinal != y->tuple_ordinal) return false;
    }
    return true;
}

static void clear_covered_holes(const View *v, unsigned char *holes,
    size_t place) {
    for (size_t p = 0; p < v->place_count; ++p) {
        size_t hole = v->image->places.offset + p;
        if (holes[p] && path_prefix(v->owner, place, hole)) holes[p] = 0;
    }
}

#ifdef SOL_MIR_PLAN_TEST_HOOKS
bool sol_mir_materialization_test_path_frontier(void) {
    SolMirMaterializedProjection projections[4] = {
        {.kind = SOL_IR_PROJECTION_FIELD, .source_field = 1},
        {.kind = SOL_IR_PROJECTION_FIELD, .source_field = 1},
        {.kind = SOL_IR_PROJECTION_FIELD, .source_field = 2},
        {.kind = SOL_IR_PROJECTION_FIELD, .source_field = 3},
    };
    SolMirMaterializedPlace places[4] = {
        {.local = 0, .projections = {0, 0}},
        {.local = 0, .projections = {0, 1}},
        {.local = 0, .projections = {1, 2}},
        {.local = 0, .projections = {3, 1}},
    };
    SolMirMaterializedImage image = {.places = {0, 4}};
    SolMirMaterialization owner = {.places = places, .place_count = 4,
        .projections = projections, .projection_count = 4};
    View view = {.owner = &owner, .image = &image, .place_count = 4};
    unsigned char left[4] = {0, 0, 1, 0};
    unsigned char right[4] = {0, 1, 0, 0};
    if (path_available(&view, left, 0) || path_available(&view, left, 1)
        || path_available(&view, left, 2) || !path_available(&view, left, 3)) {
        return false;
    }
    clear_covered_holes(&view, left, 1);
    if (!path_available(&view, left, 2)) return false;
    for (size_t i = 0; i < 4; ++i) left[i] |= right[i];
    if (path_available(&view, left, 2) || !path_available(&view, left, 3)) {
        return false;
    }
    clear_covered_holes(&view, left, 0);
    return path_available(&view, left, 0)
        && path_available(&view, left, 2)
        && path_available(&view, left, 3);
}
#endif

static void clear_holes(const View *v, unsigned char *holes, size_t local) {
    for (size_t p = 0; p < v->place_count; ++p) {
        if (v->owner->places[v->image->places.offset + p].local == local) holes[p] = 0;
    }
}

static bool merge_storage(Storage *target, const Storage *source, size_t count,
    bool *changed) {
    for (size_t i = 0; i < count; ++i) {
        Storage merged = target[i];
        if (target[i] != source[i]) {
            if (target[i] == STORAGE_DEAD || source[i] == STORAGE_DEAD) return false;
            merged = STORAGE_MAYBE_INITIALIZED;
        }
        if (merged != target[i]) { target[i] = merged; *changed = true; }
    }
    return true;
}

static bool validate_storage_paths(const View *v) {
    const SolMirMaterialization *o = v->owner;
    if ((v->local_count && v->block_count > SIZE_MAX / v->local_count)
        || (v->place_count && v->block_count > SIZE_MAX / v->place_count)) return false;
    size_t sc = v->block_count * v->local_count, hc = v->block_count * v->place_count;
    if (sc > SIZE_MAX / sizeof(Storage)) return false;
    Storage *incoming = calloc(sc, sizeof(*incoming));
    unsigned char *holes = calloc(hc, 1), *work_holes = malloc(v->place_count);
    Storage *work = malloc(v->local_count * sizeof(*work));
    unsigned char *known = calloc(v->block_count, 1);
    bool valid = (sc == 0 || incoming) && (hc == 0 || holes)
        && (v->place_count == 0 || work_holes) && (v->local_count == 0 || work)
        && known;
    size_t entry = v->image->entry - v->image->blocks.offset;
    if (valid) known[entry] = 1;
    bool changed = valid; size_t passes = 0;
    size_t facts = v->local_count <= SIZE_MAX - v->place_count - 1
        ? v->local_count + v->place_count + 1 : SIZE_MAX;
    size_t limit = facts && v->block_count > SIZE_MAX / facts
        ? SIZE_MAX : v->block_count * facts + 1;
    while (valid && changed && passes++ < limit) {
        changed = false;
        for (size_t b = 0; valid && b < v->block_count; ++b) {
            if (!known[b]) continue;
            if (v->local_count) memcpy(work, &incoming[b * v->local_count],
                v->local_count * sizeof(*work));
            if (v->place_count) memcpy(work_holes, &holes[b * v->place_count],
                v->place_count);
            const SolMirMaterializedBlock *block = &o->blocks[v->image->blocks.offset + b];
            for (size_t i = 0; valid && i < block->instructions.count; ++i) {
                const SolMirMaterializedInstruction *ins
                    = &o->instructions[block->instructions.offset + i];
                size_t local = SOL_MIR_MATERIALIZED_NONE, place = ins->place;
                if (in(v->image->places, place)) local = o->places[place].local;
                else if (in(v->image->locals, ins->local)) local = ins->local;
                size_t lr = local == SOL_MIR_MATERIALIZED_NONE ? 0
                    : local - v->image->locals.offset;
                switch (ins->kind) {
                    case SOL_MIR_INST_PARAMETER_LIVE:
                        valid = work[lr] == STORAGE_DEAD
                            && o->locals[local].kind != SOL_MIR_MATERIALIZED_LOCAL_BODY;
                        if (valid) work[lr] = STORAGE_INITIALIZED; break;
                    case SOL_MIR_INST_STORAGE_LIVE:
                        valid = work[lr] == STORAGE_DEAD
                            && o->locals[local].kind == SOL_MIR_MATERIALIZED_LOCAL_BODY;
                        if (valid) work[lr] = STORAGE_UNINITIALIZED; break;
                    case SOL_MIR_INST_DROP_IF_INITIALIZED:
                        valid = work[lr] != STORAGE_DEAD;
                        if (valid) { work[lr] = STORAGE_UNINITIALIZED;
                            clear_holes(v, work_holes, local); } break;
                    case SOL_MIR_INST_STORAGE_DEAD:
                        valid = work[lr] == STORAGE_UNINITIALIZED;
                        if (valid) work[lr] = STORAGE_DEAD; break;
                    case SOL_MIR_INST_LOAD_COPY:
                        valid = work[lr] == STORAGE_INITIALIZED
                            && o->types[o->places[place].final_type].is_copy
                            && path_available(v, work_holes, place); break;
                    case SOL_MIR_INST_LOAD_UPDATE:
                        valid = work[lr] == STORAGE_INITIALIZED
                            && path_available(v, work_holes, place); break;
                    case SOL_MIR_INST_LOAD_MOVE:
                        valid = work[lr] == STORAGE_INITIALIZED
                            && path_available(v, work_holes, place);
                        if (valid && o->places[place].projections.count == 0) {
                            work[lr] = STORAGE_UNINITIALIZED; clear_holes(v, work_holes, local);
                        } else if (valid) {
                            clear_covered_holes(v, work_holes, place);
                            work_holes[place - v->image->places.offset] = 1;
                        }
                        break;
                    case SOL_MIR_INST_DROP_PLACE_IF_INITIALIZED:
                        valid = work[lr] == STORAGE_INITIALIZED;
                        if (valid && o->places[place].projections.count == 0) {
                            work[lr] = STORAGE_UNINITIALIZED; clear_holes(v, work_holes, local);
                        } else if (valid) {
                            clear_covered_holes(v, work_holes, place);
                            work_holes[place - v->image->places.offset] = 1;
                        }
                        break;
                    case SOL_MIR_INST_STORE:
                        if (o->places[place].projections.count == 0) {
                            valid = work[lr] == STORAGE_UNINITIALIZED;
                            if (valid) { work[lr] = STORAGE_INITIALIZED;
                                clear_holes(v, work_holes, local); }
                        } else {
                            bool covered = false;
                            valid = work[lr] == STORAGE_INITIALIZED;
                            for (size_t p = 0; valid && p < v->place_count; ++p) {
                                if (!work_holes[p]) continue;
                                size_t hole = v->image->places.offset + p;
                                if (path_prefix(o, place, hole)) covered = true;
                                else if (path_prefix(o, hole, place)) valid = false;
                            }
                            valid = valid && covered;
                            if (valid) clear_covered_holes(v, work_holes, place);
                        }
                        break;
                    default: break;
                }
            }
            const SolMirMaterializedTerminator *term = &block->terminator;
            if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
                const SolMirMaterializedCallArgument *all[64]; size_t count = 0;
                if (term->receiver.source_expression != SOL_IR_NONE
                    && term->receiver.access != SOL_ACCESS_OWNED) all[count++] = &term->receiver;
                if (term->arguments.count > 63) valid = false;
                for (size_t i = 0; valid && i < term->arguments.count; ++i) {
                    const SolMirMaterializedCallArgument *a
                        = &o->call_arguments[term->arguments.offset + i];
                    if (a->access != SOL_ACCESS_OWNED) all[count++] = a;
                }
                for (size_t i = 0; valid && i < count; ++i) {
                    size_t p = all[i]->place, l = o->places[p].local
                        - v->image->locals.offset;
                    valid = work[l] == STORAGE_INITIALIZED
                        && path_available(v, work_holes, p);
                }
            }
            if (valid && terminal(term->kind)) {
                for (size_t l = 0; valid && l < v->local_count; ++l)
                    valid = work[l] == STORAGE_DEAD;
                for (size_t p = 0; valid && p < v->place_count; ++p)
                    valid = work_holes[p] == 0;
            }
            SolMirMaterializedEdgeId edges[3]; size_t edge_count;
            valid = valid && block_edges(v, b, edges, &edge_count);
            for (size_t e = 0; valid && e < edge_count; ++e) {
                size_t target = o->edges[edges[e]].block - v->image->blocks.offset;
                if (!known[target]) {
                    known[target] = 1;
                    if (v->local_count) memcpy(&incoming[target * v->local_count], work,
                        v->local_count * sizeof(*work));
                    if (v->place_count) memcpy(&holes[target * v->place_count], work_holes,
                        v->place_count);
                    changed = true;
                } else {
                    Storage *target_storage = v->local_count == 0 ? NULL
                        : &incoming[target * v->local_count];
                    valid = merge_storage(target_storage, work,
                        v->local_count, &changed);
                    for (size_t p = 0; valid && p < v->place_count; ++p) {
                        unsigned char merged = holes[target * v->place_count + p]
                            || work_holes[p];
                        changed = changed || merged != holes[target * v->place_count + p];
                        holes[target * v->place_count + p] = merged;
                    }
                }
            }
        }
    }
    valid = valid && !changed;
    free(incoming); free(holes); free(work_holes); free(work); free(known);
    return valid;
}

static bool stack_flow(const View *v, int kind) {
    const SolMirMaterialization *o = v->owner;
    size_t width = kind == 0 ? v->image->temporaries.count
        : kind == 1 ? v->image->instructions.count : v->image->handlers.count;
    if (width && (v->block_count > SIZE_MAX / width
            || v->block_count * width > SIZE_MAX / sizeof(size_t))) return false;
    size_t total = width * v->block_count;
    size_t *incoming = total ? malloc(total * sizeof(*incoming)) : NULL;
    size_t *working = width ? malloc(width * sizeof(*working)) : NULL;
    size_t *depths = calloc(v->block_count, sizeof(*depths));
    unsigned char *known = calloc(v->block_count, 1);
    size_t *queue = malloc(v->block_count * sizeof(*queue));
    bool valid = (total == 0 || incoming) && (width == 0 || working)
        && depths && known && queue;
    size_t first = 0, count = valid ? 1 : 0;
    if (valid) { queue[0] = v->image->entry - v->image->blocks.offset;
        known[queue[0]] = 1; }
    while (valid && first < count) {
        size_t b = queue[first++], depth = depths[b];
        if (depth) memcpy(working, &incoming[b * width], depth * sizeof(*working));
        const SolMirMaterializedBlock *block = &o->blocks[v->image->blocks.offset + b];
        size_t drop_floor = SIZE_MAX;
        for (size_t i = 0; valid && i < block->instructions.count; ++i) {
            const SolMirMaterializedInstruction *ins
                = &o->instructions[block->instructions.offset + i];
            if (kind == 0) {
                if (ins->kind != SOL_MIR_INST_TEMPORARY_DROP && drop_floor != SIZE_MAX) {
                    valid = depth == drop_floor; drop_floor = SIZE_MAX;
                }
                if (ins->kind == SOL_MIR_INST_TEMPORARY_INIT) {
                    valid = depth < width;
                    for (size_t x = 0; valid && x < depth; ++x)
                        valid = working[x] != ins->temporary;
                    if (valid) working[depth++] = ins->temporary;
                } else if (ins->kind == SOL_MIR_INST_TEMPORARY_DROP) {
                    valid = ins->preserve_depth < depth
                        && (drop_floor == SIZE_MAX || drop_floor == ins->preserve_depth)
                        && working[ins->preserve_depth] == ins->temporary;
                    if (valid) { drop_floor = ins->preserve_depth;
                        memmove(&working[ins->preserve_depth],
                            &working[ins->preserve_depth + 1],
                            (depth - ins->preserve_depth - 1) * sizeof(*working)); --depth; }
                } else if (ins->kind == SOL_MIR_INST_CONSTRUCT) {
                    valid = ins->construct_operands.count <= depth;
                    for (size_t x = 0; valid && x < ins->construct_operands.count; ++x)
                        valid = working[depth - ins->construct_operands.count + x]
                            == o->construct_operands[ins->construct_operands.offset + x].temporary;
                    if (valid) depth -= ins->construct_operands.count;
                } else if (ins->kind == SOL_MIR_INST_COMPOUND_UPDATE) {
                    valid = depth && working[depth - 1] == ins->previous;
                    if (valid) --depth;
                } else if (ins->kind == SOL_MIR_INST_PATTERN_TEST
                    || ins->kind == SOL_MIR_INST_PATTERN_VALUE
                    || ins->kind == SOL_MIR_INST_MATCH_ARM) {
                    bool active = false;
                    for (size_t x = 0; x < depth; ++x) active |= working[x] == ins->pattern_scrutinee;
                    valid = active;
                }
            } else if (kind == 1) {
                if (ins->kind == SOL_MIR_INST_REGION_ENTER) {
                    valid = depth < width; if (valid) working[depth++] = ins->source_statement;
                } else if (ins->kind == SOL_MIR_INST_REGION_EXIT) {
                    valid = depth && working[depth - 1] == ins->source_statement;
                    if (valid) --depth;
                }
            } else {
                if (ins->kind == SOL_MIR_INST_HANDLER_ENTER) {
                    valid = depth < width; if (valid) working[depth++] = ins->handler;
                } else if (ins->kind == SOL_MIR_INST_HANDLER_EXIT) {
                    valid = depth && working[depth - 1] == ins->handler;
                    if (valid) --depth;
                }
            }
        }
        if (valid && kind == 0 && drop_floor != SIZE_MAX && depth != drop_floor) {
            valid = false;
        }
        const SolMirMaterializedTerminator *term = &block->terminator;
        if (kind == 2 && valid && term->kind == SOL_MIR_TERM_INVOKE
            && term->call_kind == SOL_IR_CALL_CAPABILITY
            && term->receiver.access != SOL_ACCESS_OWNED) {
            const SolMirMaterializedBinding *binding = &o->bindings[term->binding];
            for (size_t active = depth; active != 0; --active) {
                const SolMirMaterializedHandler *handler
                    = &o->handlers[working[active - 1]];
                if (!operation_key_matches(&handler->operation, binding,
                        term->receiver.type, term->receiver.place,
                        term->effects)) continue;
                valid = signatures_equal(o, binding,
                    &o->bindings[handler->source_binding]);
                break;
            }
        }
        if (kind == 0 && valid) {
            size_t consumed = 0;
            if (term->kind == SOL_MIR_TERM_INVOKE) {
                bool callee = term->call_kind == SOL_IR_CALL_CALLBACK;
                bool receiver = term->receiver.source_expression != SOL_IR_NONE
                    && term->receiver.access == SOL_ACCESS_OWNED;
                for (size_t i = 0; i < term->arguments.count; ++i)
                    consumed += o->call_arguments[term->arguments.offset + i].access
                        == SOL_ACCESS_OWNED;
                consumed += callee + receiver; valid = consumed <= depth;
                size_t at = depth - consumed;
                if (valid && callee) valid = working[at++] == term->callee;
                if (valid && receiver) valid = working[at++] == term->receiver.temporary;
                for (size_t i = 0; valid && i < term->arguments.count; ++i) {
                    const SolMirMaterializedCallArgument *a
                        = &o->call_arguments[term->arguments.offset + i];
                    if (a->access == SOL_ACCESS_OWNED) valid = working[at++] == a->temporary;
                }
            } else if (term->kind == SOL_MIR_TERM_CHECK_REFINED) {
                consumed = 1; valid = depth && working[depth - 1] == term->representation;
            } else if (term->kind == SOL_MIR_TERM_PROPAGATE) {
                consumed = 1; valid = depth && working[depth - 1] == term->operand;
            }
            if (valid) depth -= consumed;
        }
        if (valid && terminal(term->kind) && depth != 0) valid = false;
        SolMirMaterializedEdgeId edges[3]; size_t edge_count;
        valid = valid && block_edges(v, b, edges, &edge_count);
        for (size_t e = 0; valid && e < edge_count; ++e) {
            size_t target = o->edges[edges[e]].block - v->image->blocks.offset;
            if (!known[target]) {
                known[target] = 1; depths[target] = depth;
                if (depth) memcpy(&incoming[target * width], working,
                    depth * sizeof(*working));
                queue[count++] = target;
            } else valid = depths[target] == depth && (!depth || memcmp(
                &incoming[target * width], working, depth * sizeof(*working)) == 0);
        }
    }
    free(incoming); free(working); free(depths); free(known); free(queue);
    return valid;
}

static bool take_value(const View *v, unsigned char *unavailable, size_t id,
    bool checking) {
    if (!in(v->image->values, id)) return false;
    size_t relative = id - v->image->values.offset;
    if (v->owner->types[v->owner->values[id].type].is_copy) return true;
    if (checking && unavailable[relative]) return false;
    unavailable[relative] = 1; return true;
}

static bool affine_block(const View *v, size_t b, unsigned char *state,
    bool checking) {
    const SolMirMaterialization *o = v->owner;
    const SolMirMaterializedBlock *block = &o->blocks[v->image->blocks.offset + b];
    for (size_t i = 0; i < block->instructions.count; ++i) {
        const SolMirMaterializedInstruction *ins
            = &o->instructions[block->instructions.offset + i];
        size_t uses[2], count = 0;
        if (ins->kind == SOL_MIR_INST_STORE || ins->kind == SOL_MIR_INST_UNARY
            || ins->kind == SOL_MIR_INST_TEMPORARY_INIT
            || ins->kind == SOL_MIR_INST_EXPRESSION_RESULT) uses[count++] = ins->left;
        else if (ins->kind == SOL_MIR_INST_BINARY) {
            uses[count++] = ins->left; uses[count++] = ins->right;
        } else if (ins->kind == SOL_MIR_INST_COMPOUND_UPDATE) uses[count++] = ins->right;
        for (size_t u = 0; u < count; ++u)
            if (!take_value(v, state, uses[u], checking)) return false;
        if (ins->result != SOL_MIR_MATERIALIZED_NONE
            && !o->types[o->values[ins->result].type].is_copy)
            state[ins->result - v->image->values.offset] = 0;
    }
    const SolMirMaterializedTerminator *term = &block->terminator;
    if (term->kind == SOL_MIR_TERM_RETURN || term->kind == SOL_MIR_TERM_PANIC)
        return take_value(v, state, term->value, checking);
    if (term->kind == SOL_MIR_TERM_CHECK_CONTRACT
        && term->result != SOL_MIR_MATERIALIZED_NONE
        && !o->types[o->values[term->result].type].is_copy)
        return !checking || !state[term->result - v->image->values.offset];
    return true;
}

static bool validate_affine(const View *v) {
    if (v->value_count && v->block_count > SIZE_MAX / v->value_count) return false;
    size_t total = v->value_count * v->block_count;
    unsigned char *incoming = total ? malloc(total) : NULL;
    unsigned char *work = v->value_count ? malloc(v->value_count) : NULL;
    unsigned char *successor = v->value_count ? malloc(v->value_count) : NULL;
    unsigned char *edge_seen = v->value_count ? malloc(v->value_count) : NULL;
    unsigned char *known = calloc(v->block_count, 1), *queued = calloc(v->block_count, 1);
    size_t *queue = malloc(v->block_count * sizeof(*queue));
    bool valid = (total == 0 || incoming) && (v->value_count == 0
        || (work && successor && edge_seen)) && known && queued && queue;
    if (valid && total) memset(incoming, 1, total);
    size_t count = valid ? 1 : 0, entry = v->image->entry - v->image->blocks.offset;
    if (valid) { queue[0] = entry; known[entry] = queued[entry] = 1; }
    while (valid && count) {
        size_t b = queue[--count]; queued[b] = 0;
        if (v->value_count) memcpy(work, &incoming[b * v->value_count], v->value_count);
        valid = affine_block(v, b, work, false);
        SolMirMaterializedEdgeId edges[3]; size_t edge_count;
        valid = valid && block_edges(v, b, edges, &edge_count);
        const SolMirMaterializedTerminator *term
            = &v->owner->blocks[v->image->blocks.offset + b].terminator;
        for (size_t e = 0; valid && e < edge_count; ++e) {
            if (v->value_count) { memcpy(successor, work, v->value_count);
                memset(edge_seen, 0, v->value_count); }
            const SolMirMaterializedEdge *edge = &v->owner->edges[edges[e]];
            if (term->kind == SOL_MIR_TERM_CHECK_CONTRACT
                && term->result != SOL_MIR_MATERIALIZED_NONE
                && edges[e] != term->satisfied_edge)
                valid = take_value(v, successor, term->result, false);
            for (size_t a = 0; valid && a < edge->arguments.count; ++a) {
                size_t value = v->owner->edge_values[edge->arguments.offset + a];
                if (v->owner->values[value].kind == SOL_MIR_VALUE_TERMINATOR
                    || v->owner->types[v->owner->values[value].type].is_copy) continue;
                size_t r = value - v->image->values.offset;
                valid = !edge_seen[r]; edge_seen[r] = 1; successor[r] = 1;
            }
            const SolMirMaterializedBlock *target = &v->owner->blocks[edge->block];
            for (size_t p = 0; p < target->parameters.count; ++p) {
                size_t value = v->owner->parameter_values[target->parameters.offset + p];
                if (!v->owner->types[v->owner->values[value].type].is_copy)
                    successor[value - v->image->values.offset] = 0;
            }
            size_t target_id = edge->block - v->image->blocks.offset;
            bool changed = !known[target_id];
            for (size_t x = 0; x < v->value_count; ++x) {
                unsigned char merged = known[target_id]
                    ? incoming[target_id * v->value_count + x] || successor[x]
                    : successor[x];
                changed |= merged != incoming[target_id * v->value_count + x];
                incoming[target_id * v->value_count + x] = merged;
            }
            if (changed && !queued[target_id]) { known[target_id] = queued[target_id] = 1;
                queue[count++] = target_id; }
        }
    }
    for (size_t b = 0; valid && b < v->block_count; ++b) {
        valid = known[b]; if (!valid) break;
        if (v->value_count) memcpy(work, &incoming[b * v->value_count], v->value_count);
        valid = affine_block(v, b, work, true);
        SolMirMaterializedEdgeId edges[3]; size_t edge_count;
        valid = valid && block_edges(v, b, edges, &edge_count);
        for (size_t e = 0; valid && e < edge_count; ++e) {
            if (v->value_count) { memcpy(successor, work, v->value_count);
                memset(edge_seen, 0, v->value_count); }
            const SolMirMaterializedEdge *edge = &v->owner->edges[edges[e]];
            for (size_t a = 0; valid && a < edge->arguments.count; ++a) {
                size_t value = v->owner->edge_values[edge->arguments.offset + a];
                if (v->owner->values[value].kind == SOL_MIR_VALUE_TERMINATOR
                    || v->owner->types[v->owner->values[value].type].is_copy) continue;
                size_t r = value - v->image->values.offset;
                valid = !successor[r] && !edge_seen[r]; edge_seen[r] = 1; successor[r] = 1;
            }
        }
    }
    free(incoming); free(work); free(successor); free(edge_seen);
    free(known); free(queued); free(queue); return valid;
}

static const SolMirMaterializedTerminator *cleanup_terminal(const View *v,
    size_t block) {
    for (size_t steps = 0; steps <= v->block_count; ++steps) {
        if (!in(v->image->blocks, block)) return NULL;
        const SolMirMaterializedTerminator *term
            = &v->owner->blocks[block].terminator;
        if (terminal(term->kind)) return term;
        if (term->kind != SOL_MIR_TERM_GOTO) return NULL;
        block = v->owner->edges[term->edge].block;
    }
    return NULL;
}

static bool validate_contracts(const View *v) {
    const SolMirMaterialization *o = v->owner;
    size_t requires = 0, ensures = 0, violations = 0, snapshots = 0;
    for (size_t b = 0; b < v->block_count; ++b) {
        const SolMirMaterializedBlock *block = &o->blocks[v->image->blocks.offset + b];
        const SolMirMaterializedTerminator *t = &block->terminator;
        if (t->kind == SOL_MIR_TERM_CHECK_CONTRACT) {
            if (t->contract_phase == SOL_CONTRACT_REQUIRES) ++requires;
            else if (t->contract_phase == SOL_CONTRACT_ENSURES) ++ensures;
            else return false;
            if ((t->contract_phase == SOL_CONTRACT_REQUIRES
                    && t->contract_outcome != SOL_CONTRACT_OUTCOME_ALWAYS)
                || (int)t->contract_outcome < 0
                || t->contract_outcome > SOL_CONTRACT_OUTCOME_FAILURE
                || (t->contract_phase == SOL_CONTRACT_ENSURES
                    && t->contract_outcome != SOL_CONTRACT_OUTCOME_ALWAYS
                    && o->types[v->image->result].kind != SOL_IR_TYPE_RESULT)) {
                return false;
            }
            size_t predicate_overlays = 0;
            for (size_t u = 0; u < v->image->overlays.count; ++u) {
                const SolMirMaterializedTypeOverlay *overlay
                    = &o->overlays[v->image->overlays.offset + u];
                if (overlay->kind == SOL_MIR_PLAN_USE_OBLIGATION_PREDICATE
                    && overlay->source == t->source_obligation) {
                    ++predicate_overlays;
                    if (overlay->type >= o->type_count
                        || o->types[overlay->type].kind != SOL_IR_TYPE_BOOL) {
                        return false;
                    }
                }
            }
            if (predicate_overlays != 1) return false;
            size_t predicate_sites = 0;
            for (size_t s = 0; s < o->semantic_site_count; ++s) {
                const SolMirMaterializedSemanticSite *site = &o->semantic_sites[s];
                if (site->parent != v->image_id
                    || site->source_obligation != t->source_obligation) continue;
                if (site->kind == SOL_MIR_PLAN_DEMAND_PREDICATE
                    || site->kind == SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE
                    || site->kind == SOL_MIR_PLAN_DEMAND_CALLBACK
                    || site->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION) {
                    if (site->kind == SOL_MIR_PLAN_DEMAND_PREDICATE
                        || site->kind == SOL_MIR_PLAN_DEMAND_CALLBACK)
                        ++predicate_sites;
                    SolMirMaterializedTypeId receiver, site_result;
                    SolMirMaterializedEffectRowId effects;
                    SolMirPlanSlice parameters, accesses;
                    if (!argument_signature(o, &o->bindings[site->binding],
                            &receiver, &parameters, &accesses, &site_result,
                            &effects)
                        || site_result >= o->type_count
                        || site->producer_kind != SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE
                        || site->block != block->id) {
                        return false;
                    }
                }
            }
            if (t->predicate_inline != (predicate_sites == 0)) return false;
            const SolMirMaterializedTerminator *violation = cleanup_terminal(v,
                o->edges[t->violation_edge].block);
            const SolMirMaterializedTerminator *failure = cleanup_terminal(v,
                o->edges[t->failure_edge].block);
            if (violation == NULL || failure == NULL
                || violation->kind != SOL_MIR_TERM_CONTRACT_VIOLATION
                || violation->source_obligation != t->source_obligation
                || failure->kind != SOL_MIR_TERM_RESUME_FAILURE) return false;
        } else if (t->kind == SOL_MIR_TERM_CONTRACT_VIOLATION) ++violations;
        for (size_t i = 0; i < block->instructions.count; ++i)
            snapshots += o->instructions[block->instructions.offset + i].kind
                == SOL_MIR_INST_CAPTURE_SNAPSHOT;
    }
    if (requires + ensures == 0) return v->image->contract_body == SOL_MIR_MATERIALIZED_NONE
        && v->image->contract_epilogue == SOL_MIR_MATERIALIZED_NONE && snapshots == 0;
    if (violations != requires + ensures || !in(v->image->blocks,
            v->image->contract_body) || !in(v->image->blocks, v->image->contract_epilogue))
        return false;
    size_t current = v->image->entry;
    for (size_t i = 0; i < requires; ++i) {
        const SolMirMaterializedTerminator *t = &o->blocks[current].terminator;
        if (t->kind != SOL_MIR_TERM_CHECK_CONTRACT
            || t->contract_phase != SOL_CONTRACT_REQUIRES
            || t->result != SOL_MIR_MATERIALIZED_NONE) return false;
        current = o->edges[t->satisfied_edge].block;
        size_t predecessors = 0;
        for (size_t b = 0; b < v->block_count; ++b) {
            SolMirMaterializedEdgeId edges[3]; size_t count;
            if (!block_edges(v, b, edges, &count)) return false;
            for (size_t e = 0; e < count; ++e)
                predecessors += o->edges[edges[e]].block == current;
        }
        if (predecessors != 1) return false;
    }
    if (current != v->image->contract_body) return false;
    const SolMirMaterializedBlock *body = &o->blocks[current]; bool started = false;
    size_t snapshot_overlays = 0;
    for (size_t u = 0; u < v->image->overlays.count; ++u)
        snapshot_overlays += o->overlays[v->image->overlays.offset + u].kind
            == SOL_MIR_PLAN_USE_SNAPSHOT;
    for (size_t i = 0; i < body->instructions.count; ++i) {
        const SolMirMaterializedInstruction *ins
            = &o->instructions[body->instructions.offset + i];
        if (ins->kind == SOL_MIR_INST_CAPTURE_SNAPSHOT) {
            if (started || !instruction_semantics(v, ins)) return false;
        } else if (ins->kind != SOL_MIR_INST_PARAMETER_LIVE) started = true;
    }
    if (snapshots != snapshot_overlays) return false;
    current = v->image->contract_epilogue;
    const SolMirMaterializedBlock *epilogue = &o->blocks[current];
    if (epilogue->parameters.count != 1) return false;
    size_t result = o->parameter_values[epilogue->parameters.offset];
    for (size_t i = 0; i < ensures; ++i) {
        const SolMirMaterializedTerminator *t = &o->blocks[current].terminator;
        if (t->kind != SOL_MIR_TERM_CHECK_CONTRACT
            || t->contract_phase != SOL_CONTRACT_ENSURES || t->result != result)
            return false;
        current = o->edges[t->satisfied_edge].block;
        const SolMirMaterializedBlock *next = &o->blocks[current];
        size_t predecessors = 0;
        for (size_t b = 0; b < v->block_count; ++b) {
            SolMirMaterializedEdgeId edges[3]; size_t count;
            if (!block_edges(v, b, edges, &count)) return false;
            for (size_t e = 0; e < count; ++e)
                predecessors += o->edges[edges[e]].block == current;
        }
        if (next->parameters.count != 1 || predecessors != 1) return false;
        result = o->parameter_values[next->parameters.offset];
    }
    return o->blocks[current].terminator.kind == SOL_MIR_TERM_RETURN
        && o->blocks[current].terminator.value == result;
}

static bool validate_activation(const View *v) {
    const SolMirMaterialization *o = v->owner;
    size_t receiver = 0, parameters = 0, seen = 0;
    for (size_t l = 0; l < v->local_count; ++l) {
        const SolMirMaterializedLocal *local = &o->locals[v->image->locals.offset + l];
        if (local->kind < SOL_MIR_MATERIALIZED_LOCAL_RECEIVER
            || local->kind > SOL_MIR_MATERIALIZED_LOCAL_BODY) return false;
        receiver += local->kind == SOL_MIR_MATERIALIZED_LOCAL_RECEIVER;
        parameters += local->kind == SOL_MIR_MATERIALIZED_LOCAL_PARAMETER;
        if (local->kind == SOL_MIR_MATERIALIZED_LOCAL_RECEIVER) {
            if (local->ordinal != 0
                || local->type != v->image->receiver
                || local->access != v->image->receiver_access) return false;
        } else if (local->kind == SOL_MIR_MATERIALIZED_LOCAL_PARAMETER) {
            if (local->ordinal >= v->image->parameter_types.count
                || local->type != o->type_ids[
                    v->image->parameter_types.offset + local->ordinal]
                || local->access != o->accesses[
                    v->image->parameter_accesses.offset + local->ordinal]) return false;
        } else if (local->ordinal != SOL_MIR_MATERIALIZED_NONE) return false;
    }
    for (size_t i = 0; i < v->image->instructions.count; ++i) {
        const SolMirMaterializedInstruction *ins
            = &o->instructions[v->image->instructions.offset + i];
        if (ins->kind != SOL_MIR_INST_PARAMETER_LIVE) continue;
        const SolMirMaterializedLocal *local = &o->locals[ins->local];
        if (seen < receiver) {
            if (local->kind != SOL_MIR_MATERIALIZED_LOCAL_RECEIVER || local->ordinal != seen)
                return false;
        } else if (local->kind != SOL_MIR_MATERIALIZED_LOCAL_PARAMETER
            || local->ordinal != seen - receiver) return false;
        ++seen;
    }
    return receiver == (v->image->receiver == SOL_MIR_MATERIALIZED_NONE ? 0u : 1u)
        && parameters == v->image->parameter_types.count
        && seen == receiver + parameters;
}

static bool validate_loops(const View *v) {
    const SolMirMaterialization *o = v->owner;
    for (size_t i = 0; i < v->image->loops.count; ++i) {
        size_t id = v->image->loops.offset + i;
        const SolMirMaterializedLoop *loop = &o->loops[id];
        if (loop->source_loop != i || loop->source_statement == SOL_IR_NONE
            || loop->span.end < loop->span.start
            || loop->source_obligations.offset
                > SIZE_MAX - loop->source_obligations.count
            || (loop->parent != SOL_MIR_MATERIALIZED_NONE
                && (!in(v->image->loops, loop->parent) || loop->parent >= id))
            || !in(v->image->blocks, loop->preheader)
            || !in(v->image->blocks, loop->header)
            || loop->preheader == loop->header || loop->header == v->image->entry
            || (loop->condition != SOL_MIR_MATERIALIZED_NONE
                && !in(v->image->blocks, loop->condition))
            || (loop->body != SOL_MIR_MATERIALIZED_NONE
                && !in(v->image->blocks, loop->body))
            || (loop->backedge != SOL_MIR_MATERIALIZED_NONE
                && !in(v->image->blocks, loop->backedge))
            || (loop->exit != SOL_MIR_MATERIALIZED_NONE
                && !in(v->image->blocks, loop->exit))) return false;
        for (size_t previous = 0; previous < i; ++previous) {
            if (o->loops[v->image->loops.offset + previous].source_statement
                == loop->source_statement) return false;
        }
        const SolMirMaterializedTerminator *pre
            = &o->blocks[loop->preheader].terminator;
        if (pre->kind != SOL_MIR_TERM_GOTO
            || o->edges[pre->edge].block != loop->header
            || o->edges[pre->edge].arguments.count != 0
            || o->blocks[loop->header].parameters.count != 0) return false;
        if (loop->condition == loop->header) {
            const SolMirMaterializedTerminator *header
                = &o->blocks[loop->header].terminator;
            if (loop->body == SOL_MIR_MATERIALIZED_NONE) return false;
            if (header->kind == SOL_MIR_TERM_GOTO) {
                if (o->edges[header->edge].block != loop->body
                    || o->edges[header->edge].arguments.count != 0) return false;
            } else if (header->kind == SOL_MIR_TERM_BRANCH) {
                if (loop->exit == SOL_MIR_MATERIALIZED_NONE
                    || o->edges[header->true_edge].block != loop->body
                    || o->edges[header->false_edge].block != loop->exit
                    || o->edges[header->true_edge].arguments.count != 0
                    || o->edges[header->false_edge].arguments.count != 0) return false;
            } else return false;
        } else if (loop->condition != SOL_MIR_MATERIALIZED_NONE) {
            const SolMirMaterializedTerminator *condition
                = &o->blocks[loop->condition].terminator;
            if (loop->body == SOL_MIR_MATERIALIZED_NONE
                || loop->exit == SOL_MIR_MATERIALIZED_NONE
                || condition->kind != SOL_MIR_TERM_BRANCH
                || o->edges[condition->true_edge].block != loop->body
                || o->edges[condition->false_edge].block != loop->exit
                || o->edges[condition->true_edge].arguments.count != 0
                || o->edges[condition->false_edge].arguments.count != 0) return false;
        } else if (loop->body != SOL_MIR_MATERIALIZED_NONE) return false;
        if (loop->backedge != SOL_MIR_MATERIALIZED_NONE) {
            const SolMirMaterializedTerminator *back
                = &o->blocks[loop->backedge].terminator;
            if (back->kind != SOL_MIR_TERM_GOTO
                || o->edges[back->edge].block != loop->header
                || o->edges[back->edge].arguments.count != 0) return false;
        }
        for (size_t b = 0; b < v->block_count; ++b) {
            const SolMirMaterializedTerminator *term
                = &o->blocks[v->image->blocks.offset + b].terminator;
            if ((term->kind != SOL_MIR_TERM_BREAK
                    && term->kind != SOL_MIR_TERM_CONTINUE)
                || term->loop != id) continue;
            size_t target = o->edges[term->edge].block;
            if (o->edges[term->edge].arguments.count != 0
                || (term->kind == SOL_MIR_TERM_BREAK && target != loop->exit)
                || (term->kind == SOL_MIR_TERM_CONTINUE
                    && target != loop->header)) return false;
        }
    }
    return true;
}

static bool validate_closure(const SolMirMaterialization *o) {
    unsigned char *images = calloc(o->image_count, 1), *imports = calloc(o->import_count, 1);
    unsigned char *bindings = calloc(o->binding_count, 1), *queued = calloc(o->image_count, 1);
    unsigned char *demands = calloc(o->binding_count, 1);
    size_t *queue = malloc(o->image_count * sizeof(*queue));
    bool valid = (o->image_count == 0 || (images && queued && queue))
        && (o->import_count == 0 || imports)
        && (o->binding_count == 0 || (bindings && demands));
    size_t first = 0, count = 0;
    for (size_t b = 0; valid && b < o->binding_count; ++b) {
        const SolMirMaterializedBinding *binding = &o->bindings[b];
        if (binding->source_demand >= o->binding_count
            || demands[binding->source_demand]) valid = false;
        else demands[binding->source_demand] = 1;
        SolMirMaterializedTypeId receiver, result;
        SolMirMaterializedEffectRowId effects;
        SolMirPlanSlice parameters, accesses;
        if (valid && !argument_signature(o, binding, &receiver, &parameters,
                &accesses, &result, &effects)) valid = false;
        if (valid && binding->kind == SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE
            && result >= o->type_count) valid = false;
        (void)receiver; (void)parameters; (void)accesses; (void)effects;
        if (binding->parent == SOL_MIR_PLAN_NONE) {
            if (binding->kind != SOL_MIR_PLAN_DEMAND_ROOT
                || binding->target_kind != SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                || binding->instance >= o->image_count) valid = false;
            else { bindings[b] = 1; if (!queued[binding->instance]) {
                queued[binding->instance] = 1; queue[count++] = binding->instance; } }
        }
    }
    while (valid && first < count) {
        size_t image = queue[first++]; images[image] = 1;
        const SolMirMaterializedImage *im = &o->images[image];
        for (size_t b = 0; b < im->blocks.count; ++b) {
            const SolMirMaterializedTerminator *t
                = &o->blocks[im->blocks.offset + b].terminator;
            if (t->kind == SOL_MIR_TERM_INVOKE) bindings[t->binding] = 1;
        }
        for (size_t h = 0; h < im->handlers.count; ++h) {
            const SolMirMaterializedHandler *handler = &o->handlers[im->handlers.offset + h];
            bindings[handler->source_binding] = bindings[handler->provider_binding] = 1;
        }
        for (size_t s = 0; s < o->semantic_site_count; ++s) {
            const SolMirMaterializedSemanticSite *site = &o->semantic_sites[s];
            if (site->parent == image
                && site->kind != SOL_MIR_PLAN_DEMAND_INVOKE
                && !(site->kind == SOL_MIR_PLAN_DEMAND_CALLBACK
                    && site->producer_kind
                        == SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR)
                && site->kind != SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE
                && site->kind != SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER) {
                bindings[site->binding] = 1;
            }
        }
        for (size_t d = 0; d < o->binding_count; ++d) {
            if (!bindings[d] || o->bindings[d].parent != image) continue;
            const SolMirMaterializedBinding *binding = &o->bindings[d];
            if (binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
                if (binding->instance >= o->image_count) { valid = false; break; }
                if (!queued[binding->instance]) { queued[binding->instance] = 1;
                    queue[count++] = binding->instance; }
            } else if (binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_IMPORT) {
                if (binding->import >= o->import_count) { valid = false; break; }
                imports[binding->import] = 1;
            } else { valid = false; break; }
        }
    }
    for (size_t i = 0; valid && i < o->image_count; ++i) valid = images[i];
    for (size_t i = 0; valid && i < o->import_count; ++i) valid = imports[i];
    for (size_t i = 0; valid && i < o->binding_count; ++i) valid = bindings[i];
    for (size_t i = 0; valid && i < o->binding_count; ++i) valid = demands[i];
    free(images); free(imports); free(bindings); free(demands); free(queued);
    free(queue); return valid;
}

static bool validate_semantic_sites(const SolMirMaterialization *o) {
    if (o->semantic_site_count != o->binding_count) return false;
    unsigned char *seen = o->binding_count == 0 ? NULL
        : calloc(o->binding_count, 1);
    if (o->binding_count != 0 && seen == NULL) return false;
    bool valid = true;
    for (size_t i = 0; valid && i < o->semantic_site_count; ++i) {
        const SolMirMaterializedSemanticSite *site = &o->semantic_sites[i];
        if (site->binding >= o->binding_count || seen[site->binding]) {
            valid = false; break;
        }
        seen[site->binding] = 1;
        const SolMirMaterializedBinding *binding = &o->bindings[site->binding];
        valid = binding->site == i && site->kind == binding->kind
            && site->parent == binding->parent && site->context == binding->context
            && site->source.callable == binding->source.callable
            && site->source.expression == binding->source.expression
            && site->source.file == binding->source.file
            && site->source.start == binding->source.start
            && site->source.end == binding->source.end;
        if (!valid) break;
        bool callable_value = site->kind == SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE
            || site->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
            || site->kind == SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE;
        if (callable_value) {
            if (site->produced_function_type >= o->type_count
                || o->types[site->produced_function_type].kind
                    != SOL_IR_TYPE_FUNCTION) { valid = false; break; }
            const SolMirMaterializedType *function
                = &o->types[site->produced_function_type];
            SolMirMaterializedTypeId receiver, result;
            SolMirMaterializedEffectRowId effects;
            SolMirPlanSlice parameters, accesses;
            if (!argument_signature(o, binding, &receiver, &parameters,
                    &accesses, &result, &effects)
                || function->result != result || function->effects != effects
                || function->parameters.count != parameters.count
                || function->parameter_accesses.count != accesses.count) {
                valid = false; break;
            }
            for (size_t p = 0; p < parameters.count; ++p) {
                if (o->type_ids[function->parameters.offset + p]
                        != o->type_ids[parameters.offset + p]
                    || o->accesses[function->parameter_accesses.offset + p]
                        != o->accesses[accesses.offset + p]) {
                    valid = false; break;
                }
            }
            if (!valid) break;
            if ((site->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
                    && (receiver >= o->type_count
                        || site->captured_receiver_type != receiver))
                || (site->kind != SOL_MIR_PLAN_DEMAND_BOUND_OPERATION
                    && site->captured_receiver_type != SOL_MIR_MATERIALIZED_NONE)) {
                valid = false; break;
            }
            if (site->kind == SOL_MIR_PLAN_DEMAND_BOUND_OPERATION) {
                const SolMirMaterializedImage *image = &o->images[site->parent];
                bool capture = false;
                if (site->captured_receiver_kind
                        == SOL_MIR_MATERIALIZED_RECEIVER_PLACE) {
                    capture = in(image->places, site->captured_receiver_place)
                        && o->places[site->captured_receiver_place].final_type
                            == site->captured_receiver_type
                        && site->captured_receiver_temporary
                            == SOL_MIR_MATERIALIZED_NONE
                        && site->captured_receiver_value
                            == SOL_MIR_MATERIALIZED_NONE;
                } else if (site->captured_receiver_kind
                        == SOL_MIR_MATERIALIZED_RECEIVER_TEMPORARY) {
                    capture = in(image->temporaries,
                            site->captured_receiver_temporary)
                        && o->temporaries[site->captured_receiver_temporary].type
                            == site->captured_receiver_type
                        && site->captured_receiver_place
                            == SOL_MIR_MATERIALIZED_NONE
                        && site->captured_receiver_value
                            == SOL_MIR_MATERIALIZED_NONE;
                } else if (site->captured_receiver_kind
                        == SOL_MIR_MATERIALIZED_RECEIVER_VALUE) {
                    capture = in(image->values, site->captured_receiver_value)
                        && o->values[site->captured_receiver_value].type
                            == site->captured_receiver_type
                        && site->captured_receiver_place
                            == SOL_MIR_MATERIALIZED_NONE
                        && site->captured_receiver_temporary
                            == SOL_MIR_MATERIALIZED_NONE
                        && o->values[site->captured_receiver_value].instruction
                            == site->captured_receiver_instruction;
                } else if (site->captured_receiver_kind
                        == SOL_MIR_MATERIALIZED_RECEIVER_SOURCE_EXPRESSION) {
                    capture = site->captured_receiver_place
                            == SOL_MIR_MATERIALIZED_NONE
                        && site->captured_receiver_temporary
                            == SOL_MIR_MATERIALIZED_NONE
                        && site->captured_receiver_value
                            == SOL_MIR_MATERIALIZED_NONE
                        && site->captured_receiver_instruction
                            == SOL_MIR_MATERIALIZED_NONE;
                }
                if (!capture
                    || (site->operation.root != SOL_MIR_MATERIALIZED_NONE
                        && site->operation.root != site->captured_receiver_place)) {
                    valid = false; break;
                }
                for (size_t r = 0; r < site->captured_receiver_roots.count; ++r)
                    if (!in(image->locals, o->receiver_roots[
                            site->captured_receiver_roots.offset + r])) {
                        valid = false; break;
                    }
                if (!valid) break;
            }
        } else if (site->produced_function_type != SOL_MIR_MATERIALIZED_NONE
            || site->captured_receiver_type != SOL_MIR_MATERIALIZED_NONE) {
            valid = false; break;
        }
        if (site->kind != SOL_MIR_PLAN_DEMAND_ROOT) {
            valid = site->parent < o->image_count
                && site->context < o->context_count
                && o->contexts[site->context].instance == site->parent;
            if (!valid) break;
        }
        if (site->kind == SOL_MIR_PLAN_DEMAND_ROOT) {
            valid = site->parent == SOL_MIR_PLAN_NONE
                && site->producer_kind == SOL_MIR_MATERIALIZED_PRODUCER_ROOT
                && site->block < o->block_count
                && binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                && site->block == o->images[binding->instance].entry
                && site->instruction == SOL_MIR_MATERIALIZED_NONE
                && site->handler == SOL_MIR_MATERIALIZED_NONE;
        } else if (site->kind == SOL_MIR_PLAN_DEMAND_INVOKE
            || (site->kind == SOL_MIR_PLAN_DEMAND_CALLBACK
                && site->producer_kind
                    == SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR)) {
            valid = site->parent < o->image_count
                && site->producer_kind == SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR
                && in(o->images[site->parent].blocks, site->block)
                && o->blocks[site->block].terminator.kind == SOL_MIR_TERM_INVOKE
                && o->blocks[site->block].terminator.binding == site->binding
                && site->instruction == SOL_MIR_MATERIALIZED_NONE
                && site->handler == SOL_MIR_MATERIALIZED_NONE;
        } else if (site->kind == SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE
            || site->kind == SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER) {
            valid = site->parent < o->image_count
                && site->producer_kind == SOL_MIR_MATERIALIZED_PRODUCER_HANDLER
                && in(o->images[site->parent].handlers, site->handler)
                && site->block == SOL_MIR_MATERIALIZED_NONE
                && site->instruction == SOL_MIR_MATERIALIZED_NONE;
            if (valid) {
                const SolMirMaterializedHandler *handler = &o->handlers[site->handler];
                valid = site->kind == SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE
                    ? handler->source_binding == site->binding
                    : handler->provider_binding == site->binding;
            }
        } else {
            valid = site->parent < o->image_count
                && site->handler == SOL_MIR_MATERIALIZED_NONE;
            if (valid && site->producer_kind
                    == SOL_MIR_MATERIALIZED_PRODUCER_INSTRUCTION) {
                valid = in(o->images[site->parent].instructions, site->instruction)
                    && site->block == o->instructions[site->instruction].block
                    && o->instructions[site->instruction].source_expression
                        == site->source.expression;
            } else if (valid && site->producer_kind
                    == SOL_MIR_MATERIALIZED_PRODUCER_TERMINATOR) {
                valid = in(o->images[site->parent].blocks, site->block)
                    && o->blocks[site->block].terminator.kind == SOL_MIR_TERM_INVOKE
                    && o->blocks[site->block].terminator.callable_site == i
                    && site->instruction == SOL_MIR_MATERIALIZED_NONE;
            } else if (valid && site->producer_kind
                    == SOL_MIR_MATERIALIZED_PRODUCER_PREDICATE) {
                valid = in(o->images[site->parent].blocks, site->block)
                    && site->instruction == SOL_MIR_MATERIALIZED_NONE
                    && site->source_obligation != SOL_IR_NONE;
                if (valid) {
                    const SolMirMaterializedTerminator *term
                        = &o->blocks[site->block].terminator;
                    valid = (term->kind == SOL_MIR_TERM_CHECK_CONTRACT
                            && term->source_obligation == site->source_obligation)
                        || (term->kind == SOL_MIR_TERM_CHECK_REFINED
                            && site->context < o->context_count
                            && o->contexts[site->context].kind
                                == SOL_MIR_PLAN_CONTEXT_REFINEMENT
                            && term->source_expression
                                == o->contexts[site->context].source.expression);
                }
            } else valid = false;
        }
        if (valid && site->kind != SOL_MIR_PLAN_DEMAND_ROOT
            && o->contexts[site->context].kind
                == SOL_MIR_PLAN_CONTEXT_REFINEMENT) {
            valid = site->source_definition == o->contexts[site->context].definition
                && site->source_obligation == o->contexts[site->context].obligation;
        } else if (valid && site->kind != SOL_MIR_PLAN_DEMAND_ROOT
            && site->source_obligation != SOL_IR_NONE) {
            bool found = false;
            if (o->contexts[site->context].kind
                    != SOL_MIR_PLAN_CONTEXT_CONTRACT
                || o->contexts[site->context].obligation
                    != site->source_obligation) valid = false;
            else for (size_t b = 0; b < o->images[site->parent].blocks.count; ++b) {
                const SolMirMaterializedTerminator *term = &o->blocks[
                    o->images[site->parent].blocks.offset + b].terminator;
                found = found || (term->kind == SOL_MIR_TERM_CHECK_CONTRACT
                    && term->source_obligation == site->source_obligation);
            }
            valid = valid && found;
        } else if (valid && site->kind != SOL_MIR_PLAN_DEMAND_ROOT
            && o->contexts[site->context].kind
                != SOL_MIR_PLAN_CONTEXT_BODY) {
            valid = false;
        }
    }
    for (size_t i = 0; valid && i < o->binding_count; ++i) valid = seen[i] == 1;
    free(seen); return valid;
}

static bool signatures_equal(const SolMirMaterialization *o,
    const SolMirMaterializedBinding *left,
    const SolMirMaterializedBinding *right) {
    SolMirMaterializedTypeId lr, rr, lresult, rresult;
    SolMirMaterializedEffectRowId le, re;
    SolMirPlanSlice lp, rp, la, ra;
    if (!argument_signature(o, left, &lr, &lp, &la, &lresult, &le)
        || !argument_signature(o, right, &rr, &rp, &ra, &rresult, &re)
        || lr != rr || le != re || lresult != rresult
        || lp.count != rp.count || la.count != ra.count)
        return false;
    for (size_t i = 0; i < lp.count; ++i) {
        if (o->type_ids[lp.offset + i] != o->type_ids[rp.offset + i]
            || o->accesses[la.offset + i] != o->accesses[ra.offset + i]) return false;
    }
    return true;
}

static bool operation_payloads_equal(const SolMirMaterialization *o,
    const SolMirMaterializedBinding *left,
    const SolMirMaterializedBinding *right) {
    SolMirMaterializedTypeId lr, rr, lresult, rresult;
    SolMirMaterializedEffectRowId le, re;
    SolMirPlanSlice lp, rp, la, ra;
    if (!argument_signature(o, left, &lr, &lp, &la, &lresult, &le)
        || !argument_signature(o, right, &rr, &rp, &ra, &rresult, &re)
        || lresult != rresult || lp.count != rp.count || la.count != ra.count)
        return false;
    for (size_t i = 0; i < lp.count; ++i) {
        if (o->type_ids[lp.offset + i] != o->type_ids[rp.offset + i]
            || o->accesses[la.offset + i] != o->accesses[ra.offset + i]) return false;
    }
    (void)lr; (void)rr; (void)le; (void)re;
    return true;
}

static bool validate_handlers(const SolMirMaterialization *o) {
    for (size_t i = 0; i < o->handler_count; ++i) {
        const SolMirMaterializedHandler *handler = &o->handlers[i];
        if (handler->parent >= o->image_count
            || !in(o->images[handler->parent].handlers, i)
            || handler->source_binding >= o->binding_count
            || handler->provider_binding >= o->binding_count
            || !in(o->images[handler->parent].places, handler->authority)
            || !in(o->images[handler->parent].places, handler->provider)
            || handler->context >= o->context_count) return false;
        const SolMirMaterializedBinding *source = &o->bindings[handler->source_binding];
        const SolMirMaterializedBinding *provider = &o->bindings[handler->provider_binding];
        SolMirMaterializedTypeId sr, pr, source_result, provider_result;
        SolMirMaterializedEffectRowId source_effects, provider_effects;
        SolMirPlanSlice source_parameters, source_accesses;
        SolMirPlanSlice provider_parameters, provider_accesses;
        if (source->kind != SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE
            || provider->kind != SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER
            || source->parent != handler->parent || provider->parent != handler->parent
            || source->context != handler->context || provider->context != handler->context
            || !argument_signature(o, source, &sr, &source_parameters,
                &source_accesses, &source_result, &source_effects)
            || source_effects >= o->effect_row_count
            || sr != o->places[handler->authority].final_type
            || !argument_signature(o, provider, &pr, &provider_parameters,
                &provider_accesses, &provider_result, &provider_effects)
            || provider_effects >= o->effect_row_count
            || pr != o->places[handler->provider].final_type
            || !operation_payloads_equal(o, source, provider)
            || !operation_key_matches(&handler->operation, source, sr,
                handler->authority, source_effects)) return false;
        (void)source_parameters; (void)source_accesses; (void)source_result;
        (void)provider_parameters; (void)provider_accesses; (void)provider_result;
        (void)provider_effects;
    }
    return true;
}

static bool validate_arena_closure(const SolMirMaterialization *o) {
#define COUNTERS(name, count) unsigned char *name = (count) == 0 ? NULL : calloc((count), 1)
    COUNTERS(edges, o->edge_count); COUNTERS(edge_values, o->edge_value_count);
    COUNTERS(parameters, o->parameter_value_count);
    COUNTERS(arguments, o->call_argument_count);
    COUNTERS(operands, o->construct_operand_count);
    COUNTERS(projections, o->projection_count);
    COUNTERS(writebacks, o->writeback_count); COUNTERS(handler_enters, o->handler_count);
    COUNTERS(handler_exits, o->handler_count); COUNTERS(temp_initializers, o->temporary_count);
    COUNTERS(receiver_roots, o->receiver_root_count);
#undef COUNTERS
    bool valid = (o->edge_count == 0 || edges) && (o->edge_value_count == 0 || edge_values)
        && (o->parameter_value_count == 0 || parameters)
        && (o->call_argument_count == 0 || arguments)
        && (o->construct_operand_count == 0 || operands)
        && (o->projection_count == 0 || projections)
        && (o->writeback_count == 0 || writebacks)
        && (o->receiver_root_count == 0 || receiver_roots)
        && (o->handler_count == 0 || (handler_enters && handler_exits))
        && (o->temporary_count == 0 || temp_initializers);
    for (size_t b = 0; valid && b < o->block_count; ++b) {
        const SolMirMaterializedBlock *block = &o->blocks[b];
        for (size_t p = 0; p < block->parameters.count; ++p) {
            size_t slot = block->parameters.offset + p;
            valid = slot < o->parameter_value_count && !parameters[slot];
            if (valid) parameters[slot] = 1;
        }
        SolMirMaterializedEdgeId outgoing[3];
        size_t count = term_edges(&block->terminator, outgoing);
        for (size_t e = 0; valid && e < count; ++e) {
            size_t id = outgoing[e];
            valid = id < o->edge_count && !edges[id];
            if (!valid) break;
            edges[id] = 1;
            const SolMirMaterializedEdge *edge = &o->edges[id];
            for (size_t a = 0; a < edge->arguments.count; ++a) {
                size_t slot = edge->arguments.offset + a;
                valid = slot < o->edge_value_count && !edge_values[slot];
                if (!valid) break;
                edge_values[slot] = 1;
            }
        }
        const SolMirMaterializedTerminator *term = &block->terminator;
        if (valid && term->kind == SOL_MIR_TERM_INVOKE) {
            for (size_t a = 0; a < term->arguments.count; ++a) {
                size_t slot = term->arguments.offset + a;
                valid = slot < o->call_argument_count && !arguments[slot];
                if (!valid) break;
                arguments[slot] = 1;
            }
            for (size_t w = 0; valid && w < term->writebacks.count; ++w) {
                size_t slot = term->writebacks.offset + w;
                valid = slot < o->writeback_count && !writebacks[slot];
                if (valid) writebacks[slot] = 1;
            }
        }
    }
    for (size_t i = 0; valid && i < o->instruction_count; ++i) {
        const SolMirMaterializedInstruction *ins = &o->instructions[i];
        if (ins->kind == SOL_MIR_INST_CONSTRUCT) {
            for (size_t p = 0; p < ins->construct_operands.count; ++p) {
                size_t slot = ins->construct_operands.offset + p;
                valid = slot < o->construct_operand_count && !operands[slot];
                if (!valid) break;
                operands[slot] = 1;
            }
        } else if (ins->kind == SOL_MIR_INST_HANDLER_ENTER) {
            valid = ins->handler < o->handler_count && !handler_enters[ins->handler];
            if (valid) handler_enters[ins->handler] = 1;
        } else if (ins->kind == SOL_MIR_INST_HANDLER_EXIT) {
            valid = ins->handler < o->handler_count;
            if (valid) handler_exits[ins->handler] = 1;
        } else if (ins->kind == SOL_MIR_INST_TEMPORARY_INIT) {
            valid = ins->temporary < o->temporary_count
                && !temp_initializers[ins->temporary];
            if (valid) temp_initializers[ins->temporary] = 1;
        }
    }
    for (size_t p = 0; valid && p < o->place_count; ++p) {
        const SolMirMaterializedPlace *place = &o->places[p];
        for (size_t i = 0; i < place->projections.count; ++i) {
            size_t slot = place->projections.offset + i;
            valid = slot < o->projection_count && !projections[slot];
            if (!valid) break;
            projections[slot] = 1;
        }
    }
    for (size_t s = 0; valid && s < o->semantic_site_count; ++s) {
        const SolMirMaterializedSemanticSite *site = &o->semantic_sites[s];
        for (size_t r = 0; r < site->captured_receiver_roots.count; ++r) {
            size_t slot = site->captured_receiver_roots.offset + r;
            valid = slot < o->receiver_root_count && !receiver_roots[slot];
            if (!valid) break;
            receiver_roots[slot] = 1;
        }
    }
#define ALL(counter, count) for (size_t i = 0; valid && i < (count); ++i) \
    valid = (counter)[i] == 1
    ALL(edges, o->edge_count); ALL(edge_values, o->edge_value_count);
    ALL(parameters, o->parameter_value_count); ALL(arguments, o->call_argument_count);
    ALL(operands, o->construct_operand_count); ALL(projections, o->projection_count);
    ALL(writebacks, o->writeback_count);
    ALL(receiver_roots, o->receiver_root_count);
    ALL(handler_enters, o->handler_count); ALL(handler_exits, o->handler_count);
    ALL(temp_initializers, o->temporary_count);
#undef ALL
    free(edges); free(edge_values); free(parameters); free(arguments); free(operands);
    free(projections);
    free(writebacks); free(handler_enters); free(handler_exits); free(temp_initializers);
    free(receiver_roots);
    return valid;
}

bool sol_mir_materialization_validate_concrete(
    const SolMirMaterialization *owner, SolDiagnostics *diagnostics) {
    if (owner == NULL || owner->image_count == 0 || owner->type_count == 0) {
        return error(diagnostics, "concrete MIR has no executable closure");
    }
    if (!shallow_ranges(owner)) return error(diagnostics,
        "concrete MIR discriminant, access, or reference range is malformed");
    size_t validation_work = 0;
    if (!sol_mir_materialization_validation_work(owner, &validation_work)
        || validation_work != owner->usage.validation_work
        || validation_work > owner->limits.max_validation_work) {
        return error(diagnostics, "concrete MIR validation work is inconsistent");
    }
    if (!validate_type_and_effect_arenas(owner)) return error(diagnostics,
        "concrete MIR type or closed effect arena is malformed");
    bool valid = true;
    size_t blocks = 0, values = 0, instructions = 0, locals = 0, places = 0;
    size_t temporaries = 0, operands = 0, arguments = 0, loops = 0;
    size_t handlers = 0, overlays = 0, contexts = 0;
    for (size_t i = 0; valid && i < owner->image_count; ++i) {
        const SolMirMaterializedImage *im = &owner->images[i];
        valid = im->instance == i && im->blocks.offset == blocks
            && im->values.offset == values && im->instructions.offset == instructions
            && im->locals.offset == locals && im->places.offset == places
            && im->temporaries.offset == temporaries
            && im->construct_operands.offset == operands
            && im->call_arguments.offset == arguments && im->loops.offset == loops
            && im->handlers.offset == handlers && im->overlays.offset == overlays
            && im->contexts.offset == contexts
            && im->entry >= im->blocks.offset && im->entry - im->blocks.offset < im->blocks.count;
        if (!valid) break;
        View view = {owner, im, i, im->blocks.count, im->values.count,
            im->locals.count, im->places.count};
        if (!concrete_types(&view)) return error(diagnostics,
            "concrete MIR type/place validation failed");
        if (!validate_ssa(&view)) return error(diagnostics,
            "concrete MIR SSA dominance validation failed");
        if (!validate_affine(&view)) return error(diagnostics,
            "concrete MIR affine value validation failed");
        if (!validate_activation(&view)) return error(diagnostics,
            "concrete MIR parameter activation validation failed");
        if (!validate_storage_paths(&view)) return error(diagnostics,
            "concrete MIR storage/path validation failed");
        if (!stack_flow(&view, 0)) return error(diagnostics,
            "concrete MIR temporary validation failed");
        if (!stack_flow(&view, 1)) return error(diagnostics,
            "concrete MIR region validation failed");
        if (!stack_flow(&view, 2)) return error(diagnostics,
            "concrete MIR handler validation failed");
        if (!validate_contracts(&view)) return error(diagnostics,
            "concrete MIR contract validation failed");
        if (!validate_loops(&view)) return error(diagnostics,
            "concrete MIR loop validation failed");
        blocks += im->blocks.count; values += im->values.count;
        instructions += im->instructions.count; locals += im->locals.count;
        places += im->places.count;
        temporaries += im->temporaries.count;
        operands += im->construct_operands.count;
        arguments += im->call_arguments.count; loops += im->loops.count;
        handlers += im->handlers.count; overlays += im->overlays.count;
        contexts += im->contexts.count;
    }
    valid = valid && blocks == owner->block_count && values == owner->value_count
        && instructions == owner->instruction_count && locals == owner->local_count
        && places == owner->place_count && temporaries == owner->temporary_count
        && operands == owner->construct_operand_count
        && arguments == owner->call_argument_count && loops == owner->loop_count
        && handlers == owner->handler_count && overlays == owner->overlay_count
        && contexts == owner->context_count && validate_arena_closure(owner)
        && validate_semantic_sites(owner) && validate_handlers(owner)
        && validate_closure(owner);
    return valid || error(diagnostics,
        "independent concrete MIR dataflow or closure validation failed");
}
