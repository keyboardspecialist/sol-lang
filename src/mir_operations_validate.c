#include "sol/mir_operations.h"
#include "mir_operations_work.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct { uintptr_t start, end; } Range;

static _Thread_local size_t metered_work;
static _Thread_local size_t metered_limit;

static bool invalid(SolDiagnostics *d, const char *message) {
    if (d != NULL) sol_diagnostics_add(d, "SOL-MIR-OPERATIONS-001",
        SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static void mark_allocation_failed(SolDiagnostics *diagnostics) {
    if (diagnostics != NULL) diagnostics->allocation_failed = true;
}

static bool add_size(size_t *v, size_t n) {
    if (n > SIZE_MAX - *v) return false;
    *v += n; return true;
}

static bool tick(size_t amount) {
    return add_size(&metered_work, amount) && metered_work <= metered_limit;
}

static bool mul_size(size_t a, size_t b, size_t *r) {
    if (a != 0 && b > SIZE_MAX / a) return false;
    *r = a * b; return true;
}

/* Do not trust the builder's construct-write record: reconstruct the complete
 * exact unbound callable shape and the one-word Wasm32 layout from the owners
 * it borrows. */
static bool exact_unbound_function_field_compatible(const SolMirLayout *layout,
    SolMirRecipeId operand_recipe, SolMirRecipeId field_recipe, size_t field) {
    const SolMirRepresentation *r = layout->representation;
    if (operand_recipe >= r->recipe_count || field_recipe >= r->recipe_count
        || field >= r->field_count || operand_recipe >= layout->type_count
        || field_recipe >= layout->type_count || field >= layout->field_count) return false;
    const SolMirRecipe *operand = &r->recipes[operand_recipe];
    const SolMirRecipe *destination = &r->recipes[field_recipe];
    const SolMirTypeLayout *operand_layout = &layout->types[operand_recipe];
    const SolMirTypeLayout *destination_layout = &layout->types[field_recipe];
    const SolMirFieldLayout *field_layout = &layout->fields[field];
    if (operand->kind != SOL_MIR_RECIPE_FUNCTION
        || destination->kind != SOL_MIR_RECIPE_FUNCTION
        || operand->parameters.count != destination->parameters.count
        || operand->parameter_accesses.count != destination->parameter_accesses.count
        || operand->result != destination->result || operand->effects != destination->effects
        || operand->storage != SOL_MIR_STORAGE_CALLABLE_HANDLE
        || destination->storage != SOL_MIR_STORAGE_CALLABLE_HANDLE
        || operand->is_copy || destination->is_copy
        || operand->copy_kind != SOL_MIR_COPY_FORBIDDEN
        || destination->copy_kind != SOL_MIR_COPY_FORBIDDEN
        || operand->drop_kind != SOL_MIR_DROP_CALLABLE
        || destination->drop_kind != SOL_MIR_DROP_CALLABLE
        || layout->target.pointer_size != 4 || layout->target.pointer_alignment != 4
        || operand_layout->value_size != 4 || destination_layout->value_size != 4
        || operand_layout->value_alignment != 4 || destination_layout->value_alignment != 4
        || operand_layout->object_kind != SOL_MIR_LAYOUT_OBJECT_CALLABLE
        || destination_layout->object_kind != SOL_MIR_LAYOUT_OBJECT_CALLABLE
        || operand_layout->object_size != 4 || destination_layout->object_size != 4
        || operand_layout->target_token_offset != 0
        || destination_layout->target_token_offset != 0
        || operand_layout->environment_handle_offset != SOL_MIR_LAYOUT_OFFSET_NONE
        || destination_layout->environment_handle_offset != SOL_MIR_LAYOUT_OFFSET_NONE
        || field_layout->owner_recipe == SOL_MIR_RECIPE_NONE
        || field_layout->size != 4 || field_layout->alignment != 4) return false;
    for (size_t i = 0; i < operand->parameters.count; ++i) {
        if (!tick(1)) return false;
        size_t left = operand->parameters.offset + i;
        size_t right = destination->parameters.offset + i;
        size_t left_access = operand->parameter_accesses.offset + i;
        size_t right_access = destination->parameter_accesses.offset + i;
        if (left >= r->recipe_id_count || right >= r->recipe_id_count
            || left_access >= r->access_count || right_access >= r->access_count
            || r->recipe_ids[left] != r->recipe_ids[right]
            || r->accesses[left_access] != r->accesses[right_access]) return false;
    }
    return r->fields[field].type == field_recipe;
}

static bool canonical(size_t count, size_t capacity, const void *p) {
    return count == capacity && ((count == 0) == (p == NULL));
}

static bool complete_limits(SolMirOperationsLimits v) {
#define REQUIRED(name) v.name != 0
    return REQUIRED(max_access_plans) && REQUIRED(max_access_steps)
        && REQUIRED(max_constructors) && REQUIRED(max_construct_operands)
        && REQUIRED(max_pattern_tests) && REQUIRED(max_pattern_extractions)
        && REQUIRED(max_pattern_nodes) && REQUIRED(max_path_steps)
        && REQUIRED(max_propagations) && REQUIRED(max_arithmetic)
        && REQUIRED(max_equality_nodes) && REQUIRED(max_equality_children)
        && REQUIRED(max_snapshots) && REQUIRED(max_callables)
        && REQUIRED(max_handlers) && REQUIRED(max_predicates)
        && REQUIRED(max_predicate_bodies) && REQUIRED(max_predicate_blocks)
        && REQUIRED(max_predicate_inputs) && REQUIRED(max_predicate_values)
        && REQUIRED(max_predicate_instructions) && REQUIRED(max_predicate_edges)
        && REQUIRED(max_predicate_edge_values) && REQUIRED(max_predicate_operands)
        && REQUIRED(max_predicate_path_steps)
        && REQUIRED(max_predicate_pattern_nodes)
        && REQUIRED(max_import_envelopes)
        && REQUIRED(max_import_contract_references) && REQUIRED(max_literal_bytes)
        && REQUIRED(max_import_snapshots)
        && REQUIRED(max_recipe_ids) && REQUIRED(max_roots)
        && REQUIRED(max_provenance) && REQUIRED(max_owned_bytes)
        && REQUIRED(max_build_scratch_bytes) && REQUIRED(max_build_work)
        && REQUIRED(max_validation_scratch_bytes)
        && REQUIRED(max_validation_work);
#undef REQUIRED
}

static bool validation_scratch(const SolMirOperations *o, size_t *result) {
    const SolMirRepresentation *r = o->layout->representation;
    const SolIr *ir = r->materialization->plan->program->ir;
    size_t local = o->provenance_count > r->recipe_count
        ? o->provenance_count : r->recipe_count;
    size_t predicate = o->predicate_instruction_count;
    if (!add_size(&predicate, o->predicate_value_count)
        || !add_size(&predicate, o->predicate_edge_count)
        || !add_size(&predicate, o->predicate_block_count)
        || !add_size(&predicate, o->predicate_operand_count)
        || !add_size(&predicate, o->predicate_path_step_count)
        || !add_size(&predicate, o->predicate_pattern_node_count)
        || o->predicate_block_count > SIZE_MAX / (2 * sizeof(size_t))
        || !add_size(&predicate,
            o->predicate_block_count * 2 * sizeof(size_t))) return false;
    if (predicate > local) local = predicate;
    size_t authentication;
    if (!mul_size(ir->local_count, sizeof(SolMirPredicateValueId),
            &authentication)
        || !add_size(&authentication, ir->local_count)
        || ir->pattern_count > SIZE_MAX / sizeof(SolMirPredicatePathStep)
        || !add_size(&authentication,
            ir->pattern_count * sizeof(SolMirPredicatePathStep))) return false;
    if (authentication > local) local = authentication;
    *result = o->layout->usage.validation_scratch_bytes > local
        ? o->layout->usage.validation_scratch_bytes : local;
    return true;
}

static bool add_range(Range *ranges, size_t *count, const void *p,
    size_t items, size_t item_size) {
    if (items == 0) return p == NULL;
    size_t bytes;
    if (p == NULL || !mul_size(items, item_size, &bytes)) return false;
    uintptr_t start = (uintptr_t)p;
    if (bytes > UINTPTR_MAX - start) return false;
    Range next = {start, start + bytes};
    for (size_t i = 0; i < *count; ++i) {
        if (!tick(1)) return false;
        if (next.start < ranges[i].end && ranges[i].start < next.end) return false;
    }
    ranges[(*count)++] = next; return true;
}

static bool overlaps(const Range *ranges, size_t count, const void *p,
    size_t items, size_t item_size) {
    if (items == 0) return p != NULL;
    size_t bytes;
    if (p == NULL || !mul_size(items, item_size, &bytes)) return true;
    uintptr_t start = (uintptr_t)p;
    if (bytes > UINTPTR_MAX - start) return true;
    uintptr_t end = start + bytes;
    for (size_t i = 0; i < count; ++i) {
        if (!tick(1)) return true;
        if (start < ranges[i].end && ranges[i].start < end) return true;
    }
    return false;
}

static bool text_size(const char *text, size_t *result) {
    size_t length = 0;
    do {
        if (!tick(1) || length == SIZE_MAX) return false;
    } while (text[length++] != '\0');
    *result = length;
    return true;
}

static bool slice(SolMirPlanSlice s, size_t count) {
    return s.offset <= count && s.count <= count - s.offset;
}

static size_t image_for(const SolMirMaterialization *m, SolMirPlanSlice image_member,
    size_t id) {
    (void)m;
    return id >= image_member.offset && id - image_member.offset < image_member.count;
}

static size_t instruction_image(const SolMirMaterialization *m, size_t id) {
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!tick(1)) return SOL_MIR_OPERATION_NONE;
        if (image_for(m, m->images[i].instructions, id)) return i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static size_t block_image(const SolMirMaterialization *m, size_t id) {
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!tick(1)) return SOL_MIR_OPERATION_NONE;
        if (image_for(m, m->images[i].blocks, id)) return i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static size_t place_image(const SolMirMaterialization *m, size_t id) {
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!tick(1)) return SOL_MIR_OPERATION_NONE;
        if (image_for(m, m->images[i].places, id)) return i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static bool expression_contains(const SolIr *ir, size_t root, size_t target,
    size_t depth) {
    if (!tick(1) || root >= ir->expression_count
        || depth > ir->expression_count) return false;
    if (root == target) return true;
    const SolIrExpression *e = &ir->expressions[root];
#define HAS(id) expression_contains(ir, (id), target, depth + 1)
    switch (e->kind) {
        case SOL_IR_EXPR_UNARY: return HAS(e->as.unary.operand);
        case SOL_IR_EXPR_BINARY: return HAS(e->as.binary.left) || HAS(e->as.binary.right);
        case SOL_IR_EXPR_CALL:
            if ((e->as.call.callee != SOL_IR_NONE && HAS(e->as.call.callee))
                || (e->as.call.receiver != SOL_IR_NONE && HAS(e->as.call.receiver))) return true;
            for (size_t i = 0; i < e->as.call.operands.count; ++i)
                if (!tick(1)) return false;
                else
                if (HAS(ir->operands[e->as.call.operands.offset + i].value)) return true;
            return false;
        case SOL_IR_EXPR_RECORD:
            for (size_t i = 0; i < e->as.record.fields.count; ++i)
                if (!tick(1)) return false;
                else
                if (HAS(ir->operands[e->as.record.fields.offset + i].value)) return true;
            return false;
        case SOL_IR_EXPR_TUPLE:
            for (size_t i = 0; i < e->as.tuple.operands.count; ++i)
                if (!tick(1)) return false;
                else
                if (HAS(ir->operands[e->as.tuple.operands.offset + i].value)) return true;
            return false;
        case SOL_IR_EXPR_IF: return HAS(e->as.if_expr.condition)
                || HAS(e->as.if_expr.then_branch) || HAS(e->as.if_expr.else_branch);
        case SOL_IR_EXPR_MATCH:
            if (HAS(e->as.match_expr.scrutinee)) return true;
            for (size_t i = 0; i < e->as.match_expr.arms.count; ++i) {
                if (!tick(1)) return false;
                const SolIrArm *arm = &ir->arms[ir->arm_ids[e->as.match_expr.arms.offset + i]];
                if ((arm->guard != SOL_IR_NONE && HAS(arm->guard)) || HAS(arm->body)) return true;
            }
            return false;
        case SOL_IR_EXPR_BLOCK:
            for (size_t i = 0; i < e->as.block.statements.count; ++i) {
                if (!tick(1)) return false;
                const SolIrStatement *s
                    = &ir->statements[ir->statement_ids[e->as.block.statements.offset + i]];
                if ((s->target != SOL_IR_NONE && HAS(s->target))
                    || (s->expression != SOL_IR_NONE && HAS(s->expression))
                    || (s->condition != SOL_IR_NONE && HAS(s->condition))) return true;
            }
            return false;
        case SOL_IR_EXPR_PROPAGATE: return HAS(e->as.propagate.operand);
        case SOL_IR_EXPR_HANDLE: return HAS(e->as.handler.authority)
                || HAS(e->as.handler.provider) || HAS(e->as.handler.body);
        case SOL_IR_EXPR_BOUND_OPERATION: return HAS(e->as.operation.receiver);
        case SOL_IR_EXPR_INTEGER: case SOL_IR_EXPR_STRING: case SOL_IR_EXPR_BOOL:
        case SOL_IR_EXPR_UNIT: case SOL_IR_EXPR_PLACE: case SOL_IR_EXPR_DEFINITION:
        case SOL_IR_EXPR_REFINEMENT_SELF: case SOL_IR_EXPR_VARIANT:
        case SOL_IR_EXPR_RESULT: case SOL_IR_EXPR_SNAPSHOT_READ:
        case SOL_IR_EXPR_COMPILE_TIME_HEAD: return false;
    }
#undef HAS
    return false;
}

static size_t expected_handler_parent(const SolMirMaterialization *m, size_t id) {
    const SolIr *ir = m->plan->program->ir;
    size_t result = SOL_MIR_OPERATION_NONE;
    for (size_t i = 0; i < m->handler_count; ++i) {
        if (!tick(1)) return SOL_MIR_OPERATION_NONE;
        if (i == id || m->handlers[i].parent != m->handlers[id].parent) continue;
        size_t source = m->handlers[i].source_expression;
        if (source >= ir->expression_count || ir->expressions[source].kind != SOL_IR_EXPR_HANDLE
            || !expression_contains(ir, ir->expressions[source].as.handler.body,
                m->handlers[id].source_expression, 0)) continue;
        if (result == SOL_MIR_OPERATION_NONE) result = i;
        else {
            size_t selected = m->handlers[result].source_expression;
            if (expression_contains(ir, ir->expressions[selected].as.handler.body,
                    source, 0)) result = i;
        }
    }
    return result;
}

static size_t variant_ordinal(const SolMirRepresentation *r, size_t recipe,
    size_t ordinal) {
    if (recipe >= r->recipe_count) return SOL_MIR_OPERATION_NONE;
    SolMirPlanSlice s = r->recipes[recipe].variants;
    for (size_t i = 0; i < s.count; ++i) {
        if (!tick(1)) return SOL_MIR_OPERATION_NONE;
        if (r->variants[s.offset + i].ordinal == ordinal) return s.offset + i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static size_t variant_source(const SolMirRepresentation *r, size_t recipe,
    size_t source) {
    if (recipe >= r->recipe_count) return SOL_MIR_OPERATION_NONE;
    SolMirPlanSlice s = r->recipes[recipe].variants;
    for (size_t i = 0; i < s.count; ++i) {
        if (!tick(1)) return SOL_MIR_OPERATION_NONE;
        if (r->variants[s.offset + i].source_variant == source) return s.offset + i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static size_t field_ordinal(const SolMirRepresentation *r, size_t recipe,
    size_t variant, size_t ordinal) {
    if (recipe >= r->recipe_count) return SOL_MIR_OPERATION_NONE;
    SolMirPlanSlice s = variant == SOL_MIR_OPERATION_NONE
        ? r->recipes[recipe].fields : r->variants[variant].fields;
    size_t result = SOL_MIR_OPERATION_NONE;
    for (size_t i = 0; i < s.count; ++i) {
        if (!tick(1)) return SOL_MIR_OPERATION_NONE;
        size_t id = s.offset + i;
        if (r->fields[id].ordinal == ordinal) {
            if (result != SOL_MIR_OPERATION_NONE) return SOL_MIR_OPERATION_NONE;
            result = id;
        }
    }
    return result;
}

static bool expected_opcode(SolMirInstructionKind kind, SolTokenKind token,
    SolMirOperationOpcode *opcode, unsigned *failures) {
    *failures = 0;
    if (kind == SOL_MIR_INST_COMPOUND_UPDATE) {
        if (token == SOL_TOKEN_PLUS_EQUAL) token = SOL_TOKEN_PLUS;
        else if (token == SOL_TOKEN_MINUS_EQUAL) token = SOL_TOKEN_MINUS;
        else if (token == SOL_TOKEN_STAR_EQUAL) token = SOL_TOKEN_STAR;
        else if (token == SOL_TOKEN_SLASH_EQUAL) token = SOL_TOKEN_SLASH;
        else if (token == SOL_TOKEN_PERCENT_EQUAL) token = SOL_TOKEN_PERCENT;
    }
    switch (token) {
        case SOL_TOKEN_BANG: *opcode = SOL_MIR_OPERATION_BOOL_NOT; return true;
        case SOL_TOKEN_MINUS: *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW;
            *opcode = kind == SOL_MIR_INST_UNARY ? SOL_MIR_OPERATION_I64_NEG
                : SOL_MIR_OPERATION_I64_SUB; return true;
        case SOL_TOKEN_PLUS: *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW;
            *opcode = SOL_MIR_OPERATION_I64_ADD; return true;
        case SOL_TOKEN_STAR: *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW;
            *opcode = SOL_MIR_OPERATION_I64_MUL; return true;
        case SOL_TOKEN_SLASH: *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW
                | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO;
            *opcode = SOL_MIR_OPERATION_I64_DIV; return true;
        case SOL_TOKEN_PERCENT: *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW
                | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO;
            *opcode = SOL_MIR_OPERATION_I64_REM; return true;
        case SOL_TOKEN_LESS: *opcode = SOL_MIR_OPERATION_I64_LT; return true;
        case SOL_TOKEN_LESS_EQUAL: *opcode = SOL_MIR_OPERATION_I64_LE; return true;
        case SOL_TOKEN_GREATER: *opcode = SOL_MIR_OPERATION_I64_GT; return true;
        case SOL_TOKEN_GREATER_EQUAL: *opcode = SOL_MIR_OPERATION_I64_GE; return true;
        case SOL_TOKEN_AMP_AMP: *opcode = SOL_MIR_OPERATION_BOOL_AND; return true;
        case SOL_TOKEN_PIPE_PIPE: *opcode = SOL_MIR_OPERATION_BOOL_OR; return true;
        case SOL_TOKEN_EQUAL_EQUAL: *opcode = SOL_MIR_OPERATION_VALUE_EQ; return true;
        case SOL_TOKEN_BANG_EQUAL: *opcode = SOL_MIR_OPERATION_VALUE_NE; return true;
        default: return false;
    }
}

static SolMirOperationEqualityKind expected_equality(SolMirRecipeKind kind) {
    if (kind == SOL_MIR_RECIPE_TEXT) return SOL_MIR_OPERATION_EQUAL_TEXT;
    if (kind == SOL_MIR_RECIPE_TUPLE || kind == SOL_MIR_RECIPE_RECORD)
        return SOL_MIR_OPERATION_EQUAL_PRODUCT;
    if (kind == SOL_MIR_RECIPE_ENUM || kind == SOL_MIR_RECIPE_OPTION
        || kind == SOL_MIR_RECIPE_RESULT) return SOL_MIR_OPERATION_EQUAL_SUM;
    if (kind == SOL_MIR_RECIPE_DISTINCT || kind == SOL_MIR_RECIPE_REFINED)
        return SOL_MIR_OPERATION_EQUAL_WRAPPER;
    return SOL_MIR_OPERATION_EQUAL_SCALAR;
}

typedef struct {
    size_t constructors, construct_operands, tests, extractions, nodes, paths;
    size_t propagations, arithmetic, equality_nodes, equality_children;
    size_t snapshots, predicates, callables, handlers, recipe_ids, roots;
    size_t predicate_bodies, predicate_blocks, predicate_inputs;
    size_t predicate_values, predicate_instructions, literal_bytes;
    size_t predicate_edges, predicate_edge_values, predicate_operands;
    size_t predicate_path_steps, predicate_pattern_nodes;
    size_t import_envelopes, import_contract_references;
    size_t import_snapshots;
    size_t provenance;
} ExpectedBuildCounts;

typedef struct {
    size_t work;
    unsigned char *equality_seen;
} ExpectedBuild;

static bool build_event(ExpectedBuild *b, size_t weight) {
    return add_size(&b->work, weight) && tick(weight);
}

#define BUILD_SCAN(b) build_event((b), SOL_MIR_OPERATIONS_WORK_SCAN)
#define BUILD_RECURSE(b) build_event((b), SOL_MIR_OPERATIONS_WORK_RECURSE)
#define BUILD_ALLOCATE(b) build_event((b), SOL_MIR_OPERATIONS_WORK_ALLOCATE)
#define BUILD_POPULATE(b) build_event((b), SOL_MIR_OPERATIONS_WORK_POPULATE)

static size_t build_place_image(ExpectedBuild *b, const SolMirMaterialization *m,
    size_t id) {
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!BUILD_SCAN(b)) return SOL_MIR_OPERATION_NONE;
        SolMirPlanSlice s = m->images[i].places;
        if (id >= s.offset && id - s.offset < s.count) return i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static size_t build_instruction_image(ExpectedBuild *b,
    const SolMirMaterialization *m, size_t id) {
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!BUILD_SCAN(b)) return SOL_MIR_OPERATION_NONE;
        SolMirPlanSlice s = m->images[i].instructions;
        if (id >= s.offset && id - s.offset < s.count) return i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static size_t build_block_image(ExpectedBuild *b, const SolMirMaterialization *m,
    size_t id) {
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!BUILD_SCAN(b)) return SOL_MIR_OPERATION_NONE;
        SolMirPlanSlice s = m->images[i].blocks;
        if (id >= s.offset && id - s.offset < s.count) return i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static size_t build_variant_ordinal(ExpectedBuild *b,
    const SolMirRepresentation *r, size_t recipe, size_t ordinal) {
    SolMirPlanSlice s = r->recipes[recipe].variants;
    for (size_t i = 0; i < s.count; ++i) {
        if (!BUILD_SCAN(b)) return SOL_MIR_OPERATION_NONE;
        if (r->variants[s.offset + i].ordinal == ordinal) return s.offset + i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static size_t build_variant_source(ExpectedBuild *b,
    const SolMirRepresentation *r, size_t recipe, size_t source) {
    SolMirPlanSlice s = r->recipes[recipe].variants;
    for (size_t i = 0; i < s.count; ++i) {
        if (!BUILD_SCAN(b)) return SOL_MIR_OPERATION_NONE;
        if (r->variants[s.offset + i].source_variant == source) return s.offset + i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static size_t build_field_ordinal(ExpectedBuild *b,
    const SolMirRepresentation *r, size_t recipe, size_t variant,
    size_t ordinal) {
    SolMirPlanSlice s = variant == SOL_MIR_OPERATION_NONE
        ? r->recipes[recipe].fields : r->variants[variant].fields;
    size_t found = SOL_MIR_OPERATION_NONE;
    for (size_t i = 0; i < s.count; ++i) {
        if (!BUILD_SCAN(b)) return SOL_MIR_OPERATION_NONE;
        size_t field = s.offset + i;
        if (r->fields[field].ordinal == ordinal) {
            if (found != SOL_MIR_OPERATION_NONE) return SOL_MIR_OPERATION_NONE;
            found = field;
        }
    }
    return found;
}

static bool build_expression_contains(ExpectedBuild *b, const SolIr *ir,
    size_t root, size_t target, size_t depth) {
    if (!BUILD_RECURSE(b) || root >= ir->expression_count
        || depth > ir->expression_count) return false;
    if (root == target) return true;
    const SolIrExpression *e = &ir->expressions[root];
#define CONTAINS(id) build_expression_contains(b, ir, (id), target, depth + 1)
    switch (e->kind) {
        case SOL_IR_EXPR_UNARY: return CONTAINS(e->as.unary.operand);
        case SOL_IR_EXPR_BINARY:
            return CONTAINS(e->as.binary.left) || CONTAINS(e->as.binary.right);
        case SOL_IR_EXPR_CALL:
            if ((e->as.call.callee != SOL_IR_NONE && CONTAINS(e->as.call.callee))
                || (e->as.call.receiver != SOL_IR_NONE
                    && CONTAINS(e->as.call.receiver))) return true;
            for (size_t i = 0; i < e->as.call.operands.count; ++i) {
                if (!BUILD_SCAN(b)) return false;
                if (CONTAINS(ir->operands[e->as.call.operands.offset + i].value))
                    return true;
            }
            return false;
        case SOL_IR_EXPR_RECORD:
            for (size_t i = 0; i < e->as.record.fields.count; ++i) {
                if (!BUILD_SCAN(b)) return false;
                if (CONTAINS(ir->operands[e->as.record.fields.offset + i].value))
                    return true;
            }
            return false;
        case SOL_IR_EXPR_TUPLE:
            for (size_t i = 0; i < e->as.tuple.operands.count; ++i) {
                if (!BUILD_SCAN(b)) return false;
                if (CONTAINS(ir->operands[e->as.tuple.operands.offset + i].value))
                    return true;
            }
            return false;
        case SOL_IR_EXPR_IF: return CONTAINS(e->as.if_expr.condition)
                || CONTAINS(e->as.if_expr.then_branch)
                || CONTAINS(e->as.if_expr.else_branch);
        case SOL_IR_EXPR_MATCH:
            if (CONTAINS(e->as.match_expr.scrutinee)) return true;
            for (size_t i = 0; i < e->as.match_expr.arms.count; ++i) {
                if (!BUILD_SCAN(b)) return false;
                const SolIrArm *arm
                    = &ir->arms[ir->arm_ids[e->as.match_expr.arms.offset + i]];
                if ((arm->guard != SOL_IR_NONE && CONTAINS(arm->guard))
                    || CONTAINS(arm->body)) return true;
            }
            return false;
        case SOL_IR_EXPR_BLOCK:
            for (size_t i = 0; i < e->as.block.statements.count; ++i) {
                if (!BUILD_SCAN(b)) return false;
                const SolIrStatement *s = &ir->statements[
                    ir->statement_ids[e->as.block.statements.offset + i]];
                if ((s->target != SOL_IR_NONE && CONTAINS(s->target))
                    || (s->expression != SOL_IR_NONE && CONTAINS(s->expression))
                    || (s->condition != SOL_IR_NONE && CONTAINS(s->condition)))
                    return true;
            }
            return false;
        case SOL_IR_EXPR_PROPAGATE: return CONTAINS(e->as.propagate.operand);
        case SOL_IR_EXPR_HANDLE: return CONTAINS(e->as.handler.authority)
                || CONTAINS(e->as.handler.provider)
                || CONTAINS(e->as.handler.body);
        case SOL_IR_EXPR_BOUND_OPERATION: return CONTAINS(e->as.operation.receiver);
        case SOL_IR_EXPR_INTEGER: case SOL_IR_EXPR_STRING: case SOL_IR_EXPR_BOOL:
        case SOL_IR_EXPR_UNIT: case SOL_IR_EXPR_PLACE: case SOL_IR_EXPR_DEFINITION:
        case SOL_IR_EXPR_REFINEMENT_SELF: case SOL_IR_EXPR_VARIANT:
        case SOL_IR_EXPR_RESULT: case SOL_IR_EXPR_SNAPSHOT_READ:
        case SOL_IR_EXPR_COMPILE_TIME_HEAD: return false;
    }
#undef CONTAINS
    return false;
}

static size_t build_handler_parent(ExpectedBuild *b,
    const SolMirMaterialization *m, size_t id) {
    const SolIr *ir = m->plan->program->ir;
    const SolMirMaterializedHandler *target = &m->handlers[id];
    size_t result = SOL_MIR_OPERATION_NONE;
    for (size_t i = 0; i < m->handler_count; ++i) {
        if (!BUILD_SCAN(b)) return SOL_MIR_OPERATION_NONE;
        if (i == id || m->handlers[i].parent != target->parent) continue;
        size_t source = m->handlers[i].source_expression;
        if (source >= ir->expression_count
            || ir->expressions[source].kind != SOL_IR_EXPR_HANDLE
            || !build_expression_contains(b, ir,
                ir->expressions[source].as.handler.body,
                target->source_expression, 0)) continue;
        if (result == SOL_MIR_OPERATION_NONE) result = i;
        else {
            size_t selected = m->handlers[result].source_expression;
            if (build_expression_contains(b, ir,
                    ir->expressions[selected].as.handler.body, source, 0)) result = i;
        }
    }
    return result;
}

static bool build_pattern_totals(ExpectedBuild *b, const SolIr *ir, size_t id,
    size_t depth, size_t *nodes, size_t *paths) {
    if (!BUILD_RECURSE(b) || id >= ir->pattern_count
        || depth > ir->pattern_count || !add_size(nodes, 1)
        || !add_size(paths, depth)) return false;
    const SolIrPattern *p = &ir->patterns[id];
    for (size_t i = 0; i < p->children.count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        if (!build_pattern_totals(b, ir,
                ir->pattern_children[p->children.offset + i].pattern,
                depth + 1, nodes, paths)) return false;
    }
    return true;
}

static bool build_predicate_extraction_work(ExpectedBuild *b, const SolIr *ir,
    size_t id, size_t depth) {
    if (!BUILD_RECURSE(b) || id >= ir->pattern_count
        || depth > ir->pattern_count) return false;
    const SolIrPattern *pattern = &ir->patterns[id];
    if (pattern->kind == SOL_IR_PATTERN_BINDING)
        for (size_t i = 0; i < depth; ++i)
            if (!BUILD_SCAN(b)) return false;
    for (size_t i = 0; i < pattern->children.count; ++i) {
        if (!BUILD_SCAN(b)
            || !build_predicate_extraction_work(b, ir,
                ir->pattern_children[pattern->children.offset + i].pattern,
                depth + 1)) return false;
    }
    return true;
}

static bool build_pattern_path(ExpectedBuild *b, const SolIr *ir, size_t root,
    size_t target, size_t depth, size_t *length) {
    if (!BUILD_RECURSE(b) || root >= ir->pattern_count
        || depth > ir->pattern_count) return false;
    if (root == target) { *length = depth; return true; }
    const SolIrPattern *p = &ir->patterns[root];
    for (size_t i = 0; i < p->children.count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        if (build_pattern_path(b, ir,
                ir->pattern_children[p->children.offset + i].pattern,
                target, depth + 1, length)) return true;
    }
    return false;
}

static bool build_recipe_reachable(ExpectedBuild *b,
    const SolMirRepresentation *r, size_t recipe, size_t target, size_t depth) {
    if (!BUILD_RECURSE(b) || recipe >= r->recipe_count
        || depth > r->recipe_count) return false;
    if (recipe == target) return true;
    const SolMirRecipe *value = &r->recipes[recipe];
    for (size_t i = 0; i < value->fields.count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        if (build_recipe_reachable(b, r,
                r->fields[value->fields.offset + i].type, target, depth + 1))
            return true;
    }
    for (size_t i = 0; i < value->variants.count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        SolMirPlanSlice fields = r->variants[value->variants.offset + i].fields;
        for (size_t f = 0; f < fields.count; ++f) {
            if (!BUILD_SCAN(b)) return false;
            if (build_recipe_reachable(b, r, r->fields[fields.offset + f].type,
                    target, depth + 1)) return true;
        }
    }
    if (value->kind == SOL_MIR_RECIPE_DISTINCT
        || value->kind == SOL_MIR_RECIPE_REFINED) {
        if (!BUILD_SCAN(b)) return false;
        return build_recipe_reachable(b, r, value->backing, target, depth + 1);
    }
    return false;
}

static bool build_count_equality(ExpectedBuild *b,
    const SolMirRepresentation *r, size_t root, ExpectedBuildCounts *c) {
    for (size_t q = 0; q < r->recipe_count; ++q) {
        if (!BUILD_SCAN(b)) return false;
        if (!build_recipe_reachable(b, r, root, q, 0)) continue;
        if (!add_size(&c->equality_nodes, 1)) return false;
        const SolMirRecipe *recipe = &r->recipes[q];
        for (size_t f = 0; f < recipe->fields.count; ++f)
            if (!BUILD_SCAN(b) || !add_size(&c->equality_children, 1)) return false;
        for (size_t v = 0; v < recipe->variants.count; ++v) {
            if (!BUILD_SCAN(b)) return false;
            SolMirPlanSlice fields = r->variants[recipe->variants.offset + v].fields;
            for (size_t f = 0; f < fields.count; ++f)
                if (!BUILD_SCAN(b) || !add_size(&c->equality_children, 1)) return false;
        }
        if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
            || recipe->kind == SOL_MIR_RECIPE_REFINED)
            if (!BUILD_SCAN(b) || !add_size(&c->equality_children, 1)) return false;
    }
    return true;
}

static bool build_count_predicate(ExpectedBuild *b, const SolIr *ir, size_t id,
    size_t depth, ExpectedBuildCounts *c) {
    if (!BUILD_RECURSE(b) || id >= ir->expression_count
        || depth > ir->expression_count) return false;
    const SolIrExpression *e = &ir->expressions[id];
    switch (e->kind) {
        case SOL_IR_EXPR_INTEGER: case SOL_IR_EXPR_BOOL: case SOL_IR_EXPR_UNIT:
            ++c->predicate_values; ++c->predicate_instructions; return true;
        case SOL_IR_EXPR_STRING:
            if (!add_size(&c->literal_bytes, strlen(e->as.string))) return false;
            ++c->predicate_values; ++c->predicate_instructions; return true;
        case SOL_IR_EXPR_PLACE: case SOL_IR_EXPR_REFINEMENT_SELF:
        case SOL_IR_EXPR_RESULT: case SOL_IR_EXPR_SNAPSHOT_READ:
            if (e->kind == SOL_IR_EXPR_PLACE && e->as.place >= ir->place_count)
                return false;
            if (e->kind == SOL_IR_EXPR_SNAPSHOT_READ
                && (e->as.snapshot >= ir->snapshot_count
                    || ir->snapshots[e->as.snapshot].operand
                        >= ir->expression_count
                    || ir->expressions[ir->snapshots[e->as.snapshot].operand].kind
                        != SOL_IR_EXPR_PLACE
                    || ir->expressions[ir->snapshots[e->as.snapshot].operand]
                        .as.place >= ir->place_count)) return false;
            ++c->predicate_inputs; ++c->predicate_values; return true;
        case SOL_IR_EXPR_UNARY:
            if (!build_count_predicate(b, ir, e->as.unary.operand,
                    depth + 1, c)) return false;
            ++c->predicate_values; ++c->predicate_instructions; return true;
        case SOL_IR_EXPR_BINARY:
            if (!build_count_predicate(b, ir, e->as.binary.left, depth + 1, c)
                || !build_count_predicate(b, ir, e->as.binary.right,
                    depth + 1, c)) return false;
            ++c->predicate_values; ++c->predicate_instructions; return true;
        case SOL_IR_EXPR_CALL:
            if ((e->as.call.kind == SOL_IR_CALL_CALLBACK
                    || e->as.call.kind == SOL_IR_CALL_CAPABILITY)
                && e->as.call.callee != SOL_IR_NONE
                && !build_count_predicate(b, ir, e->as.call.callee, depth + 1, c))
                return false;
            if (e->as.call.kind == SOL_IR_CALL_METHOD
                && e->as.call.receiver != SOL_IR_NONE
                && !build_count_predicate(b, ir, e->as.call.receiver, depth + 1, c))
                return false;
            for (size_t i = 0; i < e->as.call.operands.count; ++i) {
                bool refined = e->as.call.kind
                        == SOL_IR_CALL_DISTINCT_CONSTRUCTOR
                    && e->as.call.definition < ir->definition_count
                    && ir->definitions[e->as.call.definition].kind
                        == SOL_IR_DEFINITION_REFINED;
                if ((!refined && !BUILD_SCAN(b))
                    || !build_count_predicate(b, ir,
                        ir->operands[e->as.call.operands.offset + i].value,
                        depth + 1, c)) return false;
            }
            return true;
        case SOL_IR_EXPR_RECORD:
            for (size_t i = 0; i < e->as.record.fields.count; ++i)
                if (!BUILD_SCAN(b) || !build_count_predicate(b, ir,
                        ir->operands[e->as.record.fields.offset + i].value,
                        depth + 1, c)) return false;
            return true;
        case SOL_IR_EXPR_TUPLE:
            for (size_t i = 0; i < e->as.tuple.operands.count; ++i)
                if (!BUILD_SCAN(b) || !build_count_predicate(b, ir,
                        ir->operands[e->as.tuple.operands.offset + i].value,
                        depth + 1, c)) return false;
            return true;
        case SOL_IR_EXPR_IF:
            return build_count_predicate(b, ir, e->as.if_expr.condition,
                    depth + 1, c)
                && build_count_predicate(b, ir, e->as.if_expr.then_branch,
                    depth + 1, c)
                && build_count_predicate(b, ir, e->as.if_expr.else_branch,
                    depth + 1, c);
        case SOL_IR_EXPR_MATCH:
            if (!build_count_predicate(b, ir, e->as.match_expr.scrutinee,
                    depth + 1, c)) return false;
            for (size_t i = 0; i < e->as.match_expr.arms.count; ++i) {
                if (!BUILD_SCAN(b)) return false;
                const SolIrArm *arm
                    = &ir->arms[ir->arm_ids[e->as.match_expr.arms.offset + i]];
                size_t nodes = 0, paths = 0;
                if (!build_pattern_totals(b, ir, arm->pattern, 0,
                        &nodes, &paths)
                    || !build_predicate_extraction_work(b, ir,
                        arm->pattern, 0)) return false;
                for (size_t q = 0; q < paths; ++q)
                    if (!BUILD_SCAN(b)) return false;
                for (size_t q = 0; q < arm->bindings.count; ++q)
                    if (!BUILD_SCAN(b)) return false;
                if ((arm->guard != SOL_IR_NONE
                        && !build_count_predicate(b, ir, arm->guard, depth + 1, c))
                    || !build_count_predicate(b, ir, arm->body, depth + 1, c))
                    return false;
            }
            return true;
        case SOL_IR_EXPR_BLOCK:
            for (size_t i = 0; i < e->as.block.statements.count; ++i) {
                if (!BUILD_SCAN(b)) return false;
                const SolIrStatement *statement = &ir->statements[
                    ir->statement_ids[e->as.block.statements.offset + i]];
                if (!build_count_predicate(b, ir, statement->expression,
                        depth + 1, c)) return false;
            }
            for (size_t i = 0; i < e->as.block.statements.count; ++i)
                if (!BUILD_SCAN(b)) return false;
            return true;
        case SOL_IR_EXPR_PROPAGATE:
            return build_count_predicate(b, ir, e->as.propagate.operand,
                depth + 1, c);
        case SOL_IR_EXPR_BOUND_OPERATION:
            return build_count_predicate(b, ir, e->as.operation.receiver,
                depth + 1, c);
        case SOL_IR_EXPR_DEFINITION: case SOL_IR_EXPR_VARIANT: return true;
        case SOL_IR_EXPR_HANDLE: case SOL_IR_EXPR_COMPILE_TIME_HEAD: return false;
    }
    return false;
}

static bool build_replay_predicate(ExpectedBuild *b, const SolIr *ir,
    size_t id, size_t depth) {
    if (!BUILD_RECURSE(b) || id >= ir->expression_count
        || depth > ir->expression_count) return false;
    const SolIrExpression *e = &ir->expressions[id];
    if (e->kind == SOL_IR_EXPR_UNARY)
        return build_replay_predicate(b, ir, e->as.unary.operand, depth + 1);
    if (e->kind == SOL_IR_EXPR_BINARY)
        return build_replay_predicate(b, ir, e->as.binary.left, depth + 1)
            && build_replay_predicate(b, ir, e->as.binary.right, depth + 1);
    if (e->kind == SOL_IR_EXPR_CALL) {
        if ((e->as.call.kind == SOL_IR_CALL_CALLBACK
                || e->as.call.kind == SOL_IR_CALL_CAPABILITY)
            && e->as.call.callee != SOL_IR_NONE
            && !build_replay_predicate(b, ir, e->as.call.callee, depth + 1))
            return false;
        if (e->as.call.kind == SOL_IR_CALL_METHOD
            && e->as.call.receiver != SOL_IR_NONE
            && !build_replay_predicate(b, ir, e->as.call.receiver, depth + 1))
            return false;
        for (size_t i = 0; i < e->as.call.operands.count; ++i) {
            bool refined = e->as.call.kind == SOL_IR_CALL_DISTINCT_CONSTRUCTOR
                && e->as.call.definition < ir->definition_count
                && ir->definitions[e->as.call.definition].kind
                    == SOL_IR_DEFINITION_REFINED;
            if ((!refined && !BUILD_SCAN(b))
                || !build_replay_predicate(b, ir,
                    ir->operands[e->as.call.operands.offset + i].value,
                    depth + 1)) return false;
        }
    } else if (e->kind == SOL_IR_EXPR_RECORD) {
        for (size_t i = 0; i < e->as.record.fields.count; ++i)
            if (!BUILD_SCAN(b) || !build_replay_predicate(b, ir,
                    ir->operands[e->as.record.fields.offset + i].value,
                    depth + 1)) return false;
    } else if (e->kind == SOL_IR_EXPR_TUPLE) {
        for (size_t i = 0; i < e->as.tuple.operands.count; ++i)
            if (!BUILD_SCAN(b) || !build_replay_predicate(b, ir,
                    ir->operands[e->as.tuple.operands.offset + i].value,
                    depth + 1)) return false;
    } else if (e->kind == SOL_IR_EXPR_IF) {
        return build_replay_predicate(b, ir, e->as.if_expr.condition, depth + 1)
            && build_replay_predicate(b, ir, e->as.if_expr.then_branch, depth + 1)
            && build_replay_predicate(b, ir, e->as.if_expr.else_branch, depth + 1);
    } else if (e->kind == SOL_IR_EXPR_MATCH) {
        if (!build_replay_predicate(b, ir, e->as.match_expr.scrutinee, depth + 1))
            return false;
        for (size_t i = 0; i < e->as.match_expr.arms.count; ++i) {
            if (!BUILD_SCAN(b)) return false;
            const SolIrArm *arm
                = &ir->arms[ir->arm_ids[e->as.match_expr.arms.offset + i]];
            size_t nodes = 0, paths = 0;
            if (!build_pattern_totals(b, ir, arm->pattern, 0, &nodes, &paths)
                || !build_predicate_extraction_work(b, ir,
                    arm->pattern, 0)) return false;
            for (size_t q = 0; q < paths; ++q)
                if (!BUILD_SCAN(b)) return false;
            for (size_t q = 0; q < arm->bindings.count; ++q)
                if (!BUILD_SCAN(b)) return false;
            if ((arm->guard != SOL_IR_NONE
                    && !build_replay_predicate(b, ir, arm->guard, depth + 1))
                || !build_replay_predicate(b, ir, arm->body, depth + 1))
                return false;
        }
    } else if (e->kind == SOL_IR_EXPR_BLOCK) {
        for (size_t i = 0; i < e->as.block.statements.count; ++i) {
            if (!BUILD_SCAN(b)) return false;
            const SolIrStatement *statement = &ir->statements[
                ir->statement_ids[e->as.block.statements.offset + i]];
            if (!build_replay_predicate(b, ir, statement->expression, depth + 1))
                return false;
        }
        for (size_t i = 0; i < e->as.block.statements.count; ++i)
            if (!BUILD_SCAN(b)) return false;
    } else if (e->kind == SOL_IR_EXPR_PROPAGATE) {
        return build_replay_predicate(b, ir, e->as.propagate.operand, depth + 1);
    } else if (e->kind == SOL_IR_EXPR_BOUND_OPERATION) {
        return build_replay_predicate(b, ir, e->as.operation.receiver, depth + 1);
    }
    return true;
}

static bool build_derive_counts(ExpectedBuild *b, const SolMirLayout *layout,
    ExpectedBuildCounts *c) {
    memset(c, 0, sizeof(*c));
    const SolMirRepresentation *r = layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        const SolMirMaterializedInstruction *x = &m->instructions[i];
        if (x->kind == SOL_MIR_INST_CONSTRUCT) {
            ++c->constructors; ++c->provenance;
            if (!add_size(&c->construct_operands, x->construct_operands.count))
                return false;
        } else if (x->kind == SOL_MIR_INST_PATTERN_TEST) {
            ++c->tests; ++c->provenance;
            if (!build_pattern_totals(b, ir, x->source_pattern, 0,
                    &c->nodes, &c->paths)) return false;
        } else if (x->kind == SOL_MIR_INST_PATTERN_VALUE) {
            ++c->extractions; ++c->provenance;
            size_t length;
            if (!build_pattern_path(b, ir, ir->arms[x->source_arm].pattern,
                    x->source_pattern, 0, &length)
                || !add_size(&c->paths, length)) return false;
        } else if (x->kind == SOL_MIR_INST_UNARY
            || x->kind == SOL_MIR_INST_BINARY
            || x->kind == SOL_MIR_INST_COMPOUND_UPDATE) {
            SolMirOperationOpcode op; unsigned failures;
            if (!expected_opcode(x->kind, x->operator_kind, &op, &failures))
                return false;
            ++c->arithmetic; ++c->provenance;
            if (op == SOL_MIR_OPERATION_VALUE_EQ
                || op == SOL_MIR_OPERATION_VALUE_NE) {
                size_t root = m->values[x->left].type;
                if (!build_count_equality(b, r, root, c)) return false;
            }
        } else if (x->kind == SOL_MIR_INST_CAPTURE_SNAPSHOT) {
            ++c->snapshots; ++c->provenance;
            const SolIrExpression *operand
                = &ir->expressions[ir->snapshots[x->source_snapshot].operand];
            if (!add_size(&c->paths,
                    ir->places[operand->as.place].projections.count)) return false;
        }
    }
    for (size_t i = 0; i < m->block_count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        SolMirTerminatorKind kind = m->blocks[i].terminator.kind;
        if (kind == SOL_MIR_TERM_PROPAGATE) { ++c->propagations; ++c->provenance; }
        else if (kind == SOL_MIR_TERM_CHECK_CONTRACT
            || kind == SOL_MIR_TERM_CHECK_REFINED) {
            ++c->predicates; ++c->provenance; ++c->predicate_bodies;
            ++c->predicate_blocks;
            size_t obligation = m->blocks[i].terminator.source_obligation;
            if (obligation >= ir->obligation_count
                || !build_count_predicate(b, ir,
                    ir->obligations[obligation].predicate, 0, c)) return false;
        }
    }
    c->import_envelopes = m->import_count;
    for (size_t i = 0; i < m->import_count; ++i) {
        for (size_t q = 0; q < m->imports[i].contexts.count; ++q) {
            size_t context = m->imports[i].contexts.offset + q;
            if (context >= m->context_count) return false;
            if (m->contexts[context].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT)
                continue;
            size_t obligation = m->contexts[context].obligation;
            if (obligation >= ir->obligation_count) return false;
            ++c->predicate_bodies; ++c->predicate_blocks;
            ++c->import_contract_references;
            if (!add_size(&c->import_snapshots,
                    ir->obligations[obligation].snapshots.count)) return false;
            if (!add_size(&c->provenance,
                    ir->obligations[obligation].snapshots.count)) return false;
            if (!build_count_predicate(b, ir,
                    ir->obligations[obligation].predicate, 0, c)) return false;
        }
    }
    for (size_t context = 0; context < m->context_count; ++context) {
        if (m->contexts[context].kind != SOL_MIR_PLAN_CONTEXT_REFINEMENT
            || m->contexts[context].source_block != SOL_MIR_NONE) continue;
        size_t obligation = m->contexts[context].obligation;
        if (obligation >= ir->obligation_count) return false;
        if (!build_count_predicate(b, ir, ir->obligations[obligation].predicate,
                0, c)) return false;
    }
    c->callables = r->callable_producer_count;
    c->handlers = m->handler_count;
    if (!add_size(&c->provenance, c->callables)
        || !add_size(&c->provenance, c->handlers)) return false;
    for (size_t i = 0; i < r->callable_producer_count; ++i) {
        if (!BUILD_SCAN(b)
            || !add_size(&c->roots,
                r->callable_producers[i].captured_receiver_roots.count)) return false;
    }
    for (size_t i = 0; i < m->handler_count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        const SolMirMaterializedBinding *binding
            = &m->bindings[m->handlers[i].provider_binding];
        SolMirPlanSlice parameters
            = binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
            ? m->images[binding->instance].parameter_types
            : m->imports[binding->import].parameter_types;
        if (!add_size(&c->recipe_ids, parameters.count)) return false;
    }
    return true;
}

static SolMirRecipeId build_pattern_recipe(ExpectedBuild *b,
    const SolMirMaterialization *m, size_t image, size_t source) {
    SolMirPlanSlice overlays = m->images[image].overlays;
    for (size_t i = 0; i < overlays.count; ++i) {
        if (!BUILD_SCAN(b)) return SOL_MIR_RECIPE_NONE;
        const SolMirMaterializedTypeOverlay *o
            = &m->overlays[overlays.offset + i];
        if (o->kind == SOL_MIR_PLAN_USE_PATTERN && o->source == source)
            return o->type;
    }
    return SOL_MIR_RECIPE_NONE;
}

static bool build_append_pattern(ExpectedBuild *b, const SolMirLayout *layout,
    size_t image, size_t source, size_t depth) {
    const SolMirRepresentation *r = layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    if (!BUILD_RECURSE(b) || source >= ir->pattern_count) return false;
    const SolIrPattern *p = &ir->patterns[source];
    size_t recipe = build_pattern_recipe(b, m, image, source);
    for (size_t i = 0; i < depth; ++i)
        if (!BUILD_SCAN(b)) return false;
    if (p->kind == SOL_IR_PATTERN_VARIANT
        && build_variant_source(b, r, recipe, p->variant) >= r->variant_count)
        return false;
    for (size_t i = 0; i < p->children.count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        const SolIrPatternChild *child
            = &ir->pattern_children[p->children.offset + i];
        size_t ordinal = child->ordinal;
        if (child->field != SOL_IR_NONE)
            ordinal = child->field - ir->definitions[p->definition].fields.offset;
        size_t variant = p->kind == SOL_IR_PATTERN_VARIANT
            ? build_variant_source(b, r, recipe, p->variant)
            : SOL_MIR_OPERATION_NONE;
        size_t field = build_field_ordinal(b, r, recipe, variant, ordinal);
        if (field >= r->field_count || !build_append_pattern(b, layout, image,
                child->pattern, depth + 1)) return false;
    }
    return true;
}

static bool build_append_extraction(ExpectedBuild *b,
    const SolMirRepresentation *r, const SolIr *ir, size_t root, size_t target,
    size_t recipe, size_t depth) {
    if (!BUILD_RECURSE(b)) return false;
    if (root == target) return true;
    if (root >= ir->pattern_count || depth > ir->pattern_count) return false;
    const SolIrPattern *p = &ir->patterns[root];
    for (size_t i = 0; i < p->children.count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        const SolIrPatternChild *child
            = &ir->pattern_children[p->children.offset + i];
        size_t ordinal = child->ordinal;
        if (child->field != SOL_IR_NONE)
            ordinal = child->field - ir->definitions[p->definition].fields.offset;
        size_t variant = p->kind == SOL_IR_PATTERN_VARIANT
            ? build_variant_source(b, r, recipe, p->variant)
            : SOL_MIR_OPERATION_NONE;
        size_t field = build_field_ordinal(b, r, recipe, variant, ordinal);
        if (field >= r->field_count) return false;
        if (build_append_extraction(b, r, ir, child->pattern, target,
                r->fields[field].type, depth + 1)) return true;
    }
    return false;
}

static bool build_append_equality_recipe(ExpectedBuild *b,
    const SolMirRepresentation *r, size_t recipe_id) {
    if (!BUILD_RECURSE(b) || recipe_id >= r->recipe_count) return false;
    if (b->equality_seen[recipe_id] != 0) return true;
    b->equality_seen[recipe_id] = 1;
    const SolMirRecipe *recipe = &r->recipes[recipe_id];
    size_t children = 0;
    for (size_t f = 0; f < recipe->fields.count; ++f) {
        if (!BUILD_SCAN(b)) return false;
        ++children;
    }
    for (size_t v = 0; v < recipe->variants.count; ++v) {
        if (!BUILD_SCAN(b)) return false;
        SolMirPlanSlice fields = r->variants[recipe->variants.offset + v].fields;
        for (size_t f = 0; f < fields.count; ++f) {
            if (!BUILD_SCAN(b)) return false;
            ++children;
        }
    }
    if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
        || recipe->kind == SOL_MIR_RECIPE_REFINED) {
        if (!BUILD_SCAN(b)) return false;
        ++children;
    }
    size_t at = 0;
    for (size_t f = 0; f < recipe->fields.count; ++f) {
        if (!BUILD_SCAN(b)
            || !build_append_equality_recipe(b, r,
                r->fields[recipe->fields.offset + f].type)) return false;
        ++at;
    }
    for (size_t v = 0; v < recipe->variants.count; ++v) {
        SolMirPlanSlice fields = r->variants[recipe->variants.offset + v].fields;
        for (size_t f = 0; f < fields.count; ++f) {
            if (!BUILD_SCAN(b)
                || !build_append_equality_recipe(b, r,
                    r->fields[fields.offset + f].type)) return false;
            ++at;
        }
    }
    if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
        || recipe->kind == SOL_MIR_RECIPE_REFINED) {
        if (!BUILD_SCAN(b)
            || !build_append_equality_recipe(b, r, recipe->backing)) return false;
        ++at;
    }
    return at == children;
}

static bool build_append_equality(ExpectedBuild *b,
    const SolMirRepresentation *r, size_t root) {
    for (size_t i = 0; i < r->recipe_count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        b->equality_seen[i] = 0;
    }
    return build_append_equality_recipe(b, r, root);
}

static bool build_replay_population(ExpectedBuild *b,
    const SolMirLayout *layout) {
    const SolMirRepresentation *r = layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    for (size_t p = 0; p < m->place_count; ++p) {
        if (!BUILD_SCAN(b)) return false;
        for (size_t i = 0; i < m->places[p].projections.count; ++i)
            if (!BUILD_SCAN(b)) return false;
        if (build_place_image(b, m, p) == SOL_MIR_OPERATION_NONE) return false;
    }
    size_t snapshot_count = 0;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        const SolMirMaterializedInstruction *x = &m->instructions[i];
        size_t image = build_instruction_image(b, m, i);
        if (image == SOL_MIR_OPERATION_NONE) return false;
        if (x->kind == SOL_MIR_INST_CONSTRUCT) {
            size_t recipe = x->type, variant = SOL_MIR_OPERATION_NONE;
            if (x->construct_kind == SOL_MIR_CONSTRUCT_ENUM)
                variant = build_variant_source(b, r, recipe, x->construct_variant);
            else if (x->construct_kind == SOL_MIR_CONSTRUCT_OPTION_NONE
                || x->construct_kind == SOL_MIR_CONSTRUCT_RESULT_OK)
                variant = build_variant_ordinal(b, r, recipe, 0);
            else if (x->construct_kind == SOL_MIR_CONSTRUCT_OPTION_SOME
                || x->construct_kind == SOL_MIR_CONSTRUCT_RESULT_ERR)
                variant = build_variant_ordinal(b, r, recipe, 1);
            for (size_t j = 0; j < x->construct_operands.count; ++j) {
                if (!BUILD_SCAN(b)) return false;
                const SolMirMaterializedConstructOperand *operand
                    = &m->construct_operands[x->construct_operands.offset + j];
                if (x->construct_kind == SOL_MIR_CONSTRUCT_RECORD) {
                    SolMirPlanSlice fields = r->recipes[recipe].fields;
                    for (size_t f = 0; f < fields.count; ++f)
                        if (!BUILD_SCAN(b)) return false;
                } else if (x->construct_kind == SOL_MIR_CONSTRUCT_TUPLE) {
                    (void)build_field_ordinal(b, r, recipe,
                        SOL_MIR_OPERATION_NONE, operand->formal);
                } else if (x->construct_kind == SOL_MIR_CONSTRUCT_ENUM) {
                    SolMirPlanSlice fields = r->variants[variant].fields;
                    for (size_t f = 0; f < fields.count; ++f)
                        if (!BUILD_SCAN(b)) return false;
                } else if (x->construct_kind == SOL_MIR_CONSTRUCT_OPTION_NONE
                    || x->construct_kind == SOL_MIR_CONSTRUCT_OPTION_SOME
                    || x->construct_kind == SOL_MIR_CONSTRUCT_RESULT_OK
                    || x->construct_kind == SOL_MIR_CONSTRUCT_RESULT_ERR) {
                    (void)build_field_ordinal(b, r, recipe, variant, j);
                }
            }
            if (x->construct_kind == SOL_MIR_CONSTRUCT_CAPABILITY
                && x->source_capability_roots.count == 1)
                for (size_t q = 0; q < m->images[image].locals.count; ++q)
                    if (!BUILD_SCAN(b)) return false;
            if (!BUILD_POPULATE(b)) return false;
        } else if (x->kind == SOL_MIR_INST_PATTERN_TEST) {
            if (!build_append_pattern(b, layout, image, x->source_pattern, 0)
                || !BUILD_POPULATE(b)) return false;
        } else if (x->kind == SOL_MIR_INST_PATTERN_VALUE) {
            size_t recipe = m->temporaries[x->pattern_scrutinee].type;
            if (!build_append_extraction(b, r, ir,
                    ir->arms[x->source_arm].pattern, x->source_pattern,
                    recipe, 0) || !BUILD_POPULATE(b)) return false;
        } else if (x->kind == SOL_MIR_INST_UNARY
            || x->kind == SOL_MIR_INST_BINARY
            || x->kind == SOL_MIR_INST_COMPOUND_UPDATE) {
            SolMirOperationOpcode op; unsigned failures;
            if (!expected_opcode(x->kind, x->operator_kind, &op, &failures))
                return false;
            size_t operand = x->kind == SOL_MIR_INST_COMPOUND_UPDATE
                ? m->temporaries[x->previous].type : m->values[x->left].type;
            if ((op == SOL_MIR_OPERATION_VALUE_EQ
                    || op == SOL_MIR_OPERATION_VALUE_NE)
                && !build_append_equality(b, r, operand)) return false;
            if (!BUILD_POPULATE(b)) return false;
        } else if (x->kind == SOL_MIR_INST_CAPTURE_SNAPSHOT) {
            const SolIrExpression *operand
                = &ir->expressions[ir->snapshots[x->source_snapshot].operand];
            const SolIrPlace *place = &ir->places[operand->as.place];
            for (size_t q = 0; q < m->images[image].places.count; ++q) {
                if (!BUILD_SCAN(b)) return false;
                size_t id = m->images[image].places.offset + q;
                if (m->places[id].source_place == operand->as.place) break;
            }
            size_t local = SOL_MIR_MATERIALIZED_NONE;
            for (size_t q = 0; q < m->images[image].locals.count; ++q) {
                if (!BUILD_SCAN(b)) return false;
                size_t id = m->images[image].locals.offset + q;
                if (m->locals[id].source_local == place->local) {
                    local = id; break;
                }
            }
            if (local == SOL_MIR_MATERIALIZED_NONE) return false;
            size_t current = m->locals[local].type;
            for (size_t q = 0; q < place->projections.count; ++q) {
                if (!BUILD_SCAN(b)) return false;
                const SolIrProjection *projection
                    = &ir->projections[place->projections.offset + q];
                size_t field = SOL_MIR_OPERATION_NONE;
                if (projection->kind == SOL_IR_PROJECTION_FIELD) {
                    SolMirPlanSlice fields = r->recipes[current].fields;
                    for (size_t f = 0; f < fields.count; ++f) {
                        if (!BUILD_SCAN(b)) return false;
                        size_t candidate = fields.offset + f;
                        if (r->fields[candidate].source_field == projection->field)
                            field = candidate;
                    }
                } else {
                    field = build_field_ordinal(b, r, current,
                        SOL_MIR_OPERATION_NONE, projection->ordinal);
                }
                current = r->fields[field].type;
            }
            for (size_t q = 0; q < m->images[image].contexts.count; ++q)
                if (!BUILD_SCAN(b)) return false;
            for (size_t q = 0; q < snapshot_count; ++q)
                if (!BUILD_SCAN(b)) return false;
            ++snapshot_count;
            if (!BUILD_POPULATE(b)) return false;
        }
    }
    for (size_t i = 0; i < m->block_count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        const SolMirMaterializedTerminator *t = &m->blocks[i].terminator;
        size_t image = build_block_image(b, m, i);
        if (image == SOL_MIR_OPERATION_NONE) return false;
        if (t->kind == SOL_MIR_TERM_PROPAGATE) {
            size_t source = m->temporaries[t->operand].type;
            size_t success = build_variant_ordinal(b, r, source,
                t->propagation_kind == SOL_IR_PROPAGATE_OPTION ? 1 : 0);
            size_t residual = build_variant_ordinal(b, r, source,
                t->propagation_kind == SOL_IR_PROPAGATE_OPTION ? 0 : 1);
            size_t destination = m->values[t->residual_result].type;
            size_t destination_variant = build_variant_ordinal(b, r, destination,
                t->propagation_kind == SOL_IR_PROPAGATE_OPTION ? 0 : 1);
            (void)build_field_ordinal(b, r, source, success, 0);
            if (t->propagation_kind == SOL_IR_PROPAGATE_RESULT) {
                (void)build_field_ordinal(b, r, source, residual, 0);
                (void)build_field_ordinal(b, r, destination,
                    destination_variant, 0);
            }
            if (!BUILD_POPULATE(b)) return false;
        } else if (t->kind == SOL_MIR_TERM_CHECK_CONTRACT
            || t->kind == SOL_MIR_TERM_CHECK_REFINED) {
            for (size_t q = 0; q < m->images[image].contexts.count; ++q)
                if (!BUILD_SCAN(b)) return false;
            for (size_t q = 0; q < r->recipe_count; ++q)
                if (!BUILD_SCAN(b)) return false;
            size_t obligation = t->source_obligation;
            if (obligation >= ir->obligation_count
                || !build_replay_predicate(b, ir,
                    ir->obligations[obligation].predicate, 0)) return false;
            if (!BUILD_POPULATE(b)) return false;
        }
    }
    for (size_t i = 0; i < r->callable_producer_count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        for (size_t q = 0;
            q < r->callable_producers[i].captured_receiver_roots.count; ++q)
            if (!BUILD_SCAN(b)) return false;
        if (!BUILD_POPULATE(b)) return false;
    }
    for (size_t i = 0; i < m->import_count; ++i)
        for (size_t q = 0; q < m->imports[i].contexts.count; ++q) {
            size_t context = m->imports[i].contexts.offset + q;
            if (m->contexts[context].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT)
                continue;
            size_t obligation = m->contexts[context].obligation;
            for (size_t s = 0;
                s < ir->obligations[obligation].snapshots.count; ++s)
                if (!BUILD_POPULATE(b)) return false;
            if (!build_replay_predicate(b, ir,
                    ir->obligations[obligation].predicate, 0)) return false;
        }
    for (size_t context = 0; context < m->context_count; ++context) {
        if (m->contexts[context].kind != SOL_MIR_PLAN_CONTEXT_REFINEMENT
            || m->contexts[context].source_block != SOL_MIR_NONE) continue;
        size_t obligation = m->contexts[context].obligation;
        if (obligation >= ir->obligation_count
            || !build_replay_predicate(b, ir,
                ir->obligations[obligation].predicate, 0)) return false;
    }
    for (size_t i = 0; i < m->handler_count; ++i) {
        if (!BUILD_SCAN(b)) return false;
        const SolMirMaterializedBinding *binding
            = &m->bindings[m->handlers[i].provider_binding];
        SolMirPlanSlice parameters
            = binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
            ? m->images[binding->instance].parameter_types
            : m->imports[binding->import].parameter_types;
        for (size_t q = 0; q < parameters.count; ++q)
            if (!BUILD_SCAN(b)) return false;
        (void)build_handler_parent(b, m, i);
        if (!BUILD_POPULATE(b)) return false;
    }
    return true;
}

static bool predicate_helper_work(const SolMirOperations *o, size_t *result) {
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    size_t work = 0;
    for (size_t i = 0; i < o->predicate_input_count; ++i) {
        if (!tick(1)) return false;
        const SolMirPredicateInput *input = &o->predicate_inputs[i];
        if (input->kind == SOL_MIR_PREDICATE_INPUT_PARAMETER
            && (input->ordinal == SIZE_MAX
                || !add_size(&work, input->ordinal + 1))) return false;
    }
    for (size_t i = 0; i < o->predicate_instruction_count; ++i) {
        if (!tick(1)) return false;
        const SolMirPredicateInstruction *instruction
            = &o->predicate_instructions[i];
        if (!slice(instruction->path, o->predicate_path_step_count)
            || !slice(instruction->operands, o->predicate_operand_count)
            || !slice(instruction->pattern,
                o->predicate_pattern_node_count)) return false;
        if (instruction->kind == SOL_MIR_PREDICATE_INST_PROJECT) {
            if (!add_size(&work, instruction->path.count)) return false;
            for (size_t q = 0; q < instruction->path.count; ++q) {
                if (!tick(1)) return false;
                const SolMirPredicatePathStep *step
                    = &o->predicate_path_steps[instruction->path.offset + q];
                if (step->base_recipe >= r->recipe_count
                    || step->field_layout >= r->field_count) return false;
                if (!add_size(&work,
                        r->recipes[step->base_recipe].fields.count)) return false;
            }
        } else if (instruction->kind == SOL_MIR_PREDICATE_INST_CONSTRUCT) {
            if (instruction->construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_SUM) {
                if (instruction->recipe >= r->recipe_count
                    || instruction->variant_layout >= r->variant_count) return false;
                SolMirPlanSlice variants = r->recipes[instruction->recipe].variants;
                size_t position = 0;
                while (position < variants.count
                    && variants.offset + position != instruction->variant_layout) {
                    if (!tick(1)) return false;
                    ++position;
                }
                if (position < variants.count && !tick(1)) return false;
                if (position == variants.count || !add_size(&work, position + 1))
                    return false;
            }
            for (size_t q = 0; q < instruction->operands.count; ++q) {
                if (!tick(1)) return false;
                if (instruction->construct_kind
                        == SOL_MIR_PREDICATE_CONSTRUCT_TUPLE) {
                    if (instruction->recipe >= r->recipe_count
                        || !add_size(&work,
                            r->recipes[instruction->recipe].fields.count))
                        return false;
                } else if (instruction->construct_kind
                        == SOL_MIR_PREDICATE_CONSTRUCT_SUM) {
                    if (instruction->variant_layout >= r->variant_count
                        || !add_size(&work, r->variants[
                            instruction->variant_layout].fields.count))
                        return false;
                } else if (instruction->construct_kind
                        == SOL_MIR_PREDICATE_CONSTRUCT_RECORD) {
                    if (instruction->recipe >= r->recipe_count
                        || !add_size(&work,
                            r->recipes[instruction->recipe].fields.count))
                        return false;
                }
            }
        }
        if ((instruction->kind == SOL_MIR_PREDICATE_INST_FUNCTION
                || instruction->kind == SOL_MIR_PREDICATE_INST_BOUND_OPERATION)
            && !add_size(&work, m->binding_count)) return false;
        if (instruction->kind == SOL_MIR_PREDICATE_INST_PATTERN_TEST) {
            if (!add_size(&work, r->recipe_count)) return false;
            for (size_t q = 0; q < instruction->pattern.count; ++q) {
                if (!tick(1)) return false;
                const SolMirPredicatePatternNode *node
                    = &o->predicate_pattern_nodes[instruction->pattern.offset + q];
                if (!slice(node->path, o->predicate_path_step_count)
                    || node->recipe >= r->recipe_count) return false;
                if (node->kind == SOL_MIR_OPERATION_PATTERN_SUM_TAG) {
                    SolMirPlanSlice variants = r->recipes[node->recipe].variants;
                    size_t v = 0;
                while (v < variants.count
                    && o->layout->variants[variants.offset + v].tag
                            != node->semantic_tag) {
                    if (!tick(1)) return false;
                    ++v;
                }
                if (v < variants.count && !tick(1)) return false;
                    if (v == variants.count || !add_size(&work, v + 1))
                        return false;
                }
                if (node->path.count == 0) continue;
                const SolMirPredicatePathStep *step
                    = &o->predicate_path_steps[
                        node->path.offset + node->path.count - 1];
                if (step->base_recipe >= r->recipe_count
                    || step->field_layout >= r->field_count) return false;
                SolMirPlanSlice fields = r->recipes[step->base_recipe].fields;
                size_t f = 0;
                while (f < fields.count
                    && fields.offset + f != step->field_layout) {
                    if (!tick(1)) return false;
                    ++f;
                }
                if (f < fields.count && !tick(1)) return false;
                if (f != fields.count) {
                    if (!add_size(&work, fields.count)) return false;
                    continue;
                }
                SolMirPlanSlice variants = r->recipes[step->base_recipe].variants;
                size_t v = 0;
                for (; v < variants.count; ++v) {
                    if (!tick(1)) return false;
                    fields = r->variants[variants.offset + v].fields;
                    for (f = 0; f < fields.count; ++f) {
                        if (!tick(1)) return false;
                        if (fields.offset + f == step->field_layout) break;
                    }
                    if (f != fields.count) break;
                }
                if (v == variants.count || !add_size(&work, v + 1)
                    || !add_size(&work, fields.count)) return false;
            }
        } else if (instruction->kind
                == SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT) {
            for (size_t q = 0; q < instruction->path.count; ++q) {
                if (!tick(1)) return false;
                const SolMirPredicatePathStep *step
                    = &o->predicate_path_steps[instruction->path.offset + q];
                if (step->base_recipe >= r->recipe_count
                    || step->field_layout >= r->field_count) return false;
                SolMirPlanSlice fields = r->recipes[step->base_recipe].fields;
                size_t f = 0;
                while (f < fields.count
                    && fields.offset + f != step->field_layout) {
                    if (!tick(1)) return false;
                    ++f;
                }
                if (f < fields.count && !tick(1)) return false;
                if (f != fields.count) {
                    if (!add_size(&work, fields.count)) return false;
                    continue;
                }
                SolMirPlanSlice variants = r->recipes[step->base_recipe].variants;
                size_t v = 0;
                for (; v < variants.count; ++v) {
                    if (!tick(1)) return false;
                    fields = r->variants[variants.offset + v].fields;
                    for (f = 0; f < fields.count; ++f) {
                        if (!tick(1)) return false;
                        if (fields.offset + f == step->field_layout) break;
                    }
                    if (f != fields.count) break;
                }
                if (v == variants.count || !add_size(&work, v + 1)
                    || !add_size(&work, fields.count)) return false;
            }
        }
    }
    for (size_t i = 0; i < o->predicate_block_count; ++i) {
        if (!tick(1)) return false;
        const SolMirPredicateTerminator *term
            = &o->predicate_blocks[i].terminator;
        if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE
            && !add_size(&work, m->binding_count)) return false;
        if (term->kind == SOL_MIR_PREDICATE_TERM_CHECK_REFINED
            && (!add_size(&work, m->plan->program->ir->obligation_count)
                || !add_size(&work, m->context_count)
                || !add_size(&work, o->predicate_body_count))) return false;
        if (term->kind != SOL_MIR_PREDICATE_TERM_PROPAGATE) continue;
        if (term->success_variant_layout >= r->variant_count
            || term->residual_variant_layout >= r->variant_count
            || term->value >= o->predicate_value_count) return false;
        SolMirRecipeId recipe = o->predicate_values[term->value].recipe;
        if (recipe >= r->recipe_count) return false;
        SolMirPlanSlice variants = r->recipes[recipe].variants;
        size_t success = 0, residual = 0;
        while (success < variants.count
            && variants.offset + success != term->success_variant_layout) {
            if (!tick(1)) return false;
            ++success;
        }
        if (success < variants.count && !tick(1)) return false;
        while (residual < variants.count
            && variants.offset + residual != term->residual_variant_layout) {
            if (!tick(1)) return false;
            ++residual;
        }
        if (residual < variants.count && !tick(1)) return false;
        if (success == variants.count || residual == variants.count
            || !add_size(&work, success + residual + 2)
            || !add_size(&work,
                r->variants[term->success_variant_layout].fields.count))
            return false;
    }
    if (!add_size(&work, m->context_count)
        || !add_size(&work, o->predicate_body_count)) return false;
    for (size_t body_id = 0; body_id < o->predicate_body_count; ++body_id) {
        if (!tick(1)) return false;
        if (!add_size(&work, body_id)) return false;
        const SolMirPredicateBody *body = &o->predicate_bodies[body_id];
        if (body->owner_kind != SOL_MIR_PREDICATE_OWNER_INSTANCE
            && body->owner_kind != SOL_MIR_PREDICATE_OWNER_IMPORT) return false;
        if ((body->owner_kind == SOL_MIR_PREDICATE_OWNER_IMPORT
                && body->import >= m->import_count)
            || (body->owner_kind == SOL_MIR_PREDICATE_OWNER_INSTANCE
                && body->instance >= m->image_count)) return false;
        SolMirPlanSlice contexts = body->owner_kind
                == SOL_MIR_PREDICATE_OWNER_IMPORT
            ? m->imports[body->import].contexts : m->images[body->instance].contexts;
        bool found = false;
        for (size_t q = 0; q < contexts.count; ++q) {
            if (!tick(1)) return false;
            if (!add_size(&work, 1)) return false;
            if (contexts.offset + q == body->context) { found = true; break; }
        }
        if (!found) return false;
    }
    if (work > SIZE_MAX / 2) return false;
    *result = work * 2; return true;
}

static bool expected_build_work(const SolMirOperations *o, size_t *result,
    SolDiagnostics *diagnostics) {
    ExpectedBuild b = {0};
    ExpectedBuildCounts c;
    const SolMirLayout *layout = o->layout;
    const SolMirRepresentation *r = layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    if (!build_derive_counts(&b, layout, &c)) return false;
    c.predicate_bodies = o->predicate_body_count;
    c.predicate_blocks = o->predicate_block_count;
    c.predicate_inputs = o->predicate_input_count;
    c.predicate_values = o->predicate_value_count;
    c.predicate_instructions = o->predicate_instruction_count;
    c.predicate_edges = o->predicate_edge_count;
    c.predicate_edge_values = o->predicate_edge_value_count;
    c.predicate_operands = o->predicate_operand_count;
    c.predicate_path_steps = o->predicate_path_step_count;
    c.predicate_pattern_nodes = o->predicate_pattern_node_count;
    c.literal_bytes = o->literal_byte_count;
    if (!add_size(&c.provenance, c.predicate_bodies)) goto failed;
    if (m->context_count != 0 && !BUILD_ALLOCATE(&b)) return false;
    if (ir->pattern_count != 0
        && (!BUILD_ALLOCATE(&b) || !BUILD_ALLOCATE(&b))) return false;
    if (ir->local_count != 0
        && (!BUILD_ALLOCATE(&b) || !BUILD_ALLOCATE(&b)
            || !BUILD_ALLOCATE(&b) || !BUILD_ALLOCATE(&b))) return false;
    if (r->recipe_count != 0) {
        if (!BUILD_ALLOCATE(&b)) return false;
        b.equality_seen = calloc(r->recipe_count, 1);
        if (b.equality_seen == NULL) {
            mark_allocation_failed(diagnostics); return false;
        }
    }
#define EXPECT_ALLOCATION(count) \
    do { if ((count) != 0 && !BUILD_ALLOCATE(&b)) goto failed; } while (0)
    EXPECT_ALLOCATION(m->place_count); EXPECT_ALLOCATION(m->projection_count);
    EXPECT_ALLOCATION(c.constructors); EXPECT_ALLOCATION(c.construct_operands);
    EXPECT_ALLOCATION(c.tests); EXPECT_ALLOCATION(c.extractions);
    EXPECT_ALLOCATION(c.nodes); EXPECT_ALLOCATION(c.paths);
    EXPECT_ALLOCATION(c.propagations); EXPECT_ALLOCATION(c.arithmetic);
    EXPECT_ALLOCATION(c.equality_nodes); EXPECT_ALLOCATION(c.equality_children);
    EXPECT_ALLOCATION(c.snapshots); EXPECT_ALLOCATION(c.callables);
    EXPECT_ALLOCATION(c.handlers); EXPECT_ALLOCATION(c.predicates);
    EXPECT_ALLOCATION(c.predicate_bodies); EXPECT_ALLOCATION(c.predicate_blocks);
    EXPECT_ALLOCATION(c.predicate_inputs); EXPECT_ALLOCATION(c.predicate_values);
    EXPECT_ALLOCATION(c.predicate_instructions);
    EXPECT_ALLOCATION(c.predicate_edges);
    EXPECT_ALLOCATION(c.predicate_edge_values);
    EXPECT_ALLOCATION(c.predicate_operands);
    EXPECT_ALLOCATION(c.predicate_path_steps);
    EXPECT_ALLOCATION(c.predicate_pattern_nodes);
    EXPECT_ALLOCATION(c.import_envelopes);
    EXPECT_ALLOCATION(c.import_contract_references);
    EXPECT_ALLOCATION(c.import_snapshots);
    EXPECT_ALLOCATION(c.literal_bytes);
    EXPECT_ALLOCATION(c.recipe_ids); EXPECT_ALLOCATION(c.roots);
    EXPECT_ALLOCATION(c.provenance);
#undef EXPECT_ALLOCATION
    for (size_t i = 0; i < c.predicate_bodies; ++i)
        if (!BUILD_POPULATE(&b)) goto failed;
    if (!build_replay_population(&b, layout)) goto failed;
    size_t helper_work;
    if (!predicate_helper_work(o, &helper_work)
        || !add_size(&b.work, helper_work)) goto failed;
    free(b.equality_seen); *result = b.work; return true;
failed:
    free(b.equality_seen); return false;
}

#undef BUILD_POPULATE
#undef BUILD_ALLOCATE
#undef BUILD_RECURSE
#undef BUILD_SCAN

static bool validate_equality_recipe(const SolMirOperations *o, size_t recipe_id,
    unsigned char *seen, size_t node_end, size_t *node_at, size_t *child_at) {
    const SolMirRepresentation *r = o->layout->representation;
    if (!tick(1) || recipe_id >= r->recipe_count) return false;
    if (seen[recipe_id] != 0) return true;
    seen[recipe_id] = 1;
    if (*node_at >= node_end || *node_at >= o->equality_node_count) return false;
    const SolMirRecipe *recipe = &r->recipes[recipe_id];
    const SolMirOperationEqualityNode *node = &o->equality_nodes[(*node_at)++];
    size_t expected_count = recipe->fields.count;
    for (size_t v = 0; v < recipe->variants.count; ++v) {
        if (!tick(1)) return false;
        if (!add_size(&expected_count,
                r->variants[recipe->variants.offset + v].fields.count)) return false;
    }
    if ((recipe->kind == SOL_MIR_RECIPE_DISTINCT
            || recipe->kind == SOL_MIR_RECIPE_REFINED)
        && !add_size(&expected_count, 1)) return false;
    if (node->recipe != recipe_id || node->kind != expected_equality(recipe->kind)
        || node->children.offset != *child_at
        || node->children.count != expected_count
        || !slice(node->children, o->equality_child_count)) return false;
    size_t first_child = *child_at;
    for (size_t f = 0; f < recipe->fields.count; ++f, ++*child_at) {
        if (!tick(1)) return false;
        size_t field = recipe->fields.offset + f;
        const SolMirOperationEqualityChild *child = &o->equality_children[*child_at];
        if (child->recipe != r->fields[field].type || child->field_layout != field
            || child->variant_layout != SOL_MIR_OPERATION_NONE
            || child->semantic_tag != 0) return false;
    }
    for (size_t v = 0; v < recipe->variants.count; ++v) {
        if (!tick(1)) return false;
        size_t variant = recipe->variants.offset + v;
        SolMirPlanSlice fields = r->variants[variant].fields;
        for (size_t f = 0; f < fields.count; ++f, ++*child_at) {
            if (!tick(1)) return false;
            size_t field = fields.offset + f;
            const SolMirOperationEqualityChild *child = &o->equality_children[*child_at];
            if (child->recipe != r->fields[field].type || child->field_layout != field
                || child->variant_layout != variant
                || child->semantic_tag != o->layout->variants[variant].tag) return false;
        }
    }
    if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
        || recipe->kind == SOL_MIR_RECIPE_REFINED) {
        const SolMirOperationEqualityChild *child = &o->equality_children[(*child_at)++];
        if (child->recipe != recipe->backing
            || child->field_layout != SOL_MIR_OPERATION_NONE
            || child->variant_layout != SOL_MIR_OPERATION_NONE
            || child->semantic_tag != 0) return false;
    }
    for (size_t i = 0; i < expected_count; ++i) {
        if (!tick(1)) return false;
        if (!validate_equality_recipe(o, o->equality_children[first_child + i].recipe,
                seen, node_end, node_at, child_at)) return false;
    }
    return true;
}

static size_t provenance_find(const SolMirOperations *o,
    SolMirOperationProvenanceKind kind, size_t executable) {
    size_t found = SOL_MIR_OPERATION_NONE;
    for (size_t i = 0; i < o->provenance_count; ++i) {
        if (!tick(1)) return SOL_MIR_OPERATION_NONE;
        if (o->provenance[i].kind != kind
            || o->provenance[i].executable != executable) continue;
        if (found != SOL_MIR_OPERATION_NONE) return SOL_MIR_OPERATION_NONE;
        found = i;
    }
    return found;
}

static bool validate_provenance_shapes(const SolMirOperations *o) {
    const SolIr *ir = o->layout->representation->materialization->plan->program->ir;
    for (size_t i = 0; i < o->provenance_count; ++i) {
        if (!tick(1)) return false;
        const SolMirOperationProvenance *p = &o->provenance[i];
        if (p->source_expression != SOL_IR_NONE
            && p->source_expression >= ir->expression_count) return false;
        if (p->source_pattern != SOL_IR_NONE
            && p->source_pattern >= ir->pattern_count) return false;
        if (p->source_field != SOL_IR_NONE && p->source_field >= ir->field_count)
            return false;
        if (p->source_variant != SOL_IR_NONE
            && p->source_variant >= ir->variant_count) return false;
        if (p->source_obligation != SOL_IR_NONE
            && p->source_obligation >= ir->obligation_count) return false;
        if (p->source_snapshot != SOL_IR_NONE
            && p->source_snapshot >= ir->snapshot_count) return false;
        if (p->source_definition != SOL_IR_NONE
            && p->source_definition >= ir->definition_count) return false;
        if (p->kind != SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY
            && p->source_definition != SOL_IR_NONE) return false;
        switch (p->kind) {
            case SOL_MIR_OPERATION_PROVENANCE_CONSTRUCT:
                if (p->executable >= o->constructor_count
                    || p->source_pattern != SOL_IR_NONE
                    || p->source_field != SOL_IR_NONE
                    || p->source_obligation != SOL_IR_NONE
                    || p->source_snapshot != SOL_IR_NONE) return false;
                break;
            case SOL_MIR_OPERATION_PROVENANCE_PATTERN_TEST:
                if (p->executable >= o->pattern_test_count
                    || p->source_pattern == SOL_IR_NONE
                    || p->source_field != SOL_IR_NONE
                    || p->source_variant != SOL_IR_NONE
                    || p->source_obligation != SOL_IR_NONE
                    || p->source_snapshot != SOL_IR_NONE) return false;
                break;
            case SOL_MIR_OPERATION_PROVENANCE_PATTERN_EXTRACTION:
                if (p->executable >= o->pattern_extraction_count
                    || p->source_pattern == SOL_IR_NONE
                    || p->source_field != SOL_IR_NONE
                    || p->source_variant != SOL_IR_NONE
                    || p->source_obligation != SOL_IR_NONE
                    || p->source_snapshot != SOL_IR_NONE) return false;
                break;
            case SOL_MIR_OPERATION_PROVENANCE_ARITHMETIC:
                if (p->executable >= o->arithmetic_count
                    || p->source_pattern != SOL_IR_NONE
                    || p->source_field != SOL_IR_NONE
                    || p->source_variant != SOL_IR_NONE
                    || p->source_obligation != SOL_IR_NONE
                    || p->source_snapshot != SOL_IR_NONE) return false;
                break;
            case SOL_MIR_OPERATION_PROVENANCE_PROPAGATION:
                if (p->executable >= o->propagation_count
                    || p->source_pattern != SOL_IR_NONE
                    || p->source_field != SOL_IR_NONE
                    || p->source_variant != SOL_IR_NONE
                    || p->source_obligation != SOL_IR_NONE
                    || p->source_snapshot != SOL_IR_NONE) return false;
                break;
            case SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT:
                if (p->executable >= o->snapshot_count
                    || p->source_pattern != SOL_IR_NONE
                    || p->source_field != SOL_IR_NONE
                    || p->source_variant != SOL_IR_NONE
                    || p->source_obligation == SOL_IR_NONE
                    || p->source_snapshot == SOL_IR_NONE) return false;
                break;
            case SOL_MIR_OPERATION_PROVENANCE_PREDICATE:
                if (p->executable >= o->predicate_count
                    || p->source_pattern != SOL_IR_NONE
                    || p->source_field != SOL_IR_NONE
                    || p->source_variant != SOL_IR_NONE
                    || p->source_obligation == SOL_IR_NONE
                    || p->source_snapshot != SOL_IR_NONE) return false;
                break;
            case SOL_MIR_OPERATION_PROVENANCE_HANDLER:
                if (p->executable >= o->handler_count
                    || p->source_pattern != SOL_IR_NONE
                    || p->source_field != SOL_IR_NONE
                    || p->source_variant != SOL_IR_NONE
                    || p->source_obligation != SOL_IR_NONE
                    || p->source_snapshot != SOL_IR_NONE) return false;
                break;
            case SOL_MIR_OPERATION_PROVENANCE_CALLABLE:
                if (p->executable >= o->callable_count
                    || p->source_pattern != SOL_IR_NONE
                    || p->source_field != SOL_IR_NONE
                    || p->source_variant != SOL_IR_NONE
                    || p->source_obligation != SOL_IR_NONE
                    || p->source_snapshot != SOL_IR_NONE) return false;
                break;
            case SOL_MIR_OPERATION_PROVENANCE_IMPORT_SNAPSHOT:
                if (p->executable >= o->import_snapshot_count
                    || p->source_expression == SOL_IR_NONE
                    || p->source_pattern != SOL_IR_NONE
                    || p->source_field != SOL_IR_NONE
                    || p->source_variant != SOL_IR_NONE
                    || p->source_obligation == SOL_IR_NONE
                    || p->source_snapshot == SOL_IR_NONE) return false;
                break;
            case SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY:
                if (p->executable >= o->predicate_body_count
                    || p->source_expression != SOL_IR_NONE
                    || p->source_pattern != SOL_IR_NONE
                    || p->source_field != SOL_IR_NONE
                    || p->source_variant != SOL_IR_NONE
                    || p->source_obligation == SOL_IR_NONE
                    || p->source_snapshot != SOL_IR_NONE) return false;
                break;
            default: return false;
        }
    }
    return true;
}

static bool validate_provenance_uniqueness(const SolMirOperations *o,
    SolDiagnostics *diagnostics) {
    if (o->provenance_count != 0 && !tick(1)) return false;
    unsigned char *seen = o->provenance_count == 0 ? NULL
        : calloc(o->provenance_count, 1);
    if (o->provenance_count != 0 && seen == NULL) {
        mark_allocation_failed(diagnostics); return false;
    }
    bool valid = true;
    for (size_t i = 0; valid && i < o->provenance_count; ++i) {
        if (!tick(1)) { valid = false; break; }
        if (seen[i] != 0) { valid = false; break; }
        seen[i] = 1;
        for (size_t j = i + 1; j < o->provenance_count; ++j) {
            if (!tick(1)) { valid = false; break; }
            if (o->provenance[i].kind == o->provenance[j].kind
                && o->provenance[i].executable
                    == o->provenance[j].executable) {
                valid = false; break;
            }
        }
    }
    free(seen); return valid;
}

static bool validate_access(const SolMirOperations *o) {
    const SolMirMaterialization *m = o->layout->representation->materialization;
    if (o->access_plan_count != m->place_count
        || o->access_step_count != m->projection_count) return false;
    size_t at = 0;
    for (size_t i = 0; i < m->place_count; ++i) {
        if (!tick(1)) return false;
        const SolMirMaterializedPlace *source = &m->places[i];
        const SolMirOperationAccessPlan *p = &o->access_plans[i];
        if (p->place != i || p->image != place_image(m, i)
            || p->local != source->local || p->root_recipe != source->root_type
            || p->final_recipe != source->final_type || p->steps.offset != at
            || p->steps.count != source->projections.count) return false;
        for (size_t q = 0; q < p->steps.count; ++q, ++at) {
            if (!tick(1)) return false;
            const SolMirProjectionMap *map = &o->layout->projections[at];
            const SolMirOperationAccessStep *s = &o->access_steps[at];
            if (s->projection != map->projection || map->place != i
                || s->base_recipe != map->base_recipe
                || s->result_recipe != map->result_recipe
                || s->field_layout != map->field_layout
                || s->object_offset != map->object_offset) return false;
        }
    }
    return at == o->access_step_count;
}

static bool validate_constructors(const SolMirOperations *o, size_t *prov) {
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    size_t at = 0, operands = 0;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        if (!tick(1)) return false;
        const SolMirMaterializedInstruction *x = &m->instructions[i];
        if (x->kind != SOL_MIR_INST_CONSTRUCT) continue;
        if (at >= o->constructor_count) return false;
        const SolMirOperationConstructPlan *p = &o->constructors[at];
        size_t provenance_id = provenance_find(o,
            SOL_MIR_OPERATION_PROVENANCE_CONSTRUCT, at);
        if (provenance_id == SOL_MIR_OPERATION_NONE) return false;
        const SolMirOperationProvenance *v = &o->provenance[provenance_id];
        ++*prov;
        if (v->kind != SOL_MIR_OPERATION_PROVENANCE_CONSTRUCT
            || v->source_expression != x->source_expression
            || v->source_variant != x->construct_variant
            || p->image != instruction_image(m, i) || p->instruction != i
            || p->result != x->result || p->result_recipe != x->type
            || x->type >= r->recipe_count || p->object_kind != o->layout->types[x->type].object_kind
            || p->operands.offset != operands
            || p->operands.count != x->construct_operands.count) return false;
        SolMirOperationConstructKind kind;
        size_t variant = SOL_MIR_OPERATION_NONE;
        switch (x->construct_kind) {
            case SOL_MIR_CONSTRUCT_RECORD: kind = SOL_MIR_OPERATION_CONSTRUCT_RECORD; break;
            case SOL_MIR_CONSTRUCT_CAPABILITY: kind = SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY; break;
            case SOL_MIR_CONSTRUCT_TUPLE: kind = SOL_MIR_OPERATION_CONSTRUCT_TUPLE; break;
            case SOL_MIR_CONSTRUCT_ENUM: kind = SOL_MIR_OPERATION_CONSTRUCT_SUM;
                variant = variant_source(r, x->type, x->construct_variant); break;
            case SOL_MIR_CONSTRUCT_OPTION_NONE: kind = SOL_MIR_OPERATION_CONSTRUCT_SUM;
                variant = variant_ordinal(r, x->type, 0); break;
            case SOL_MIR_CONSTRUCT_OPTION_SOME: kind = SOL_MIR_OPERATION_CONSTRUCT_SUM;
                variant = variant_ordinal(r, x->type, 1); break;
            case SOL_MIR_CONSTRUCT_RESULT_OK: kind = SOL_MIR_OPERATION_CONSTRUCT_SUM;
                variant = variant_ordinal(r, x->type, 0); break;
            case SOL_MIR_CONSTRUCT_RESULT_ERR: kind = SOL_MIR_OPERATION_CONSTRUCT_SUM;
                variant = variant_ordinal(r, x->type, 1); break;
            case SOL_MIR_CONSTRUCT_DISTINCT: kind = SOL_MIR_OPERATION_CONSTRUCT_WRAPPER; break;
            default: return false;
        }
        if (p->kind != kind || p->variant_layout != variant
            || p->semantic_tag != (variant == SOL_MIR_OPERATION_NONE ? 0
                : o->layout->variants[variant].tag)
            || p->wrapper_backing != (kind == SOL_MIR_OPERATION_CONSTRUCT_WRAPPER
                ? r->recipes[x->type].backing : SOL_MIR_RECIPE_NONE)) return false;
        if (kind != SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
            && (p->capability_rule != SOL_MIR_OPERATION_CAPABILITY_NONE
                || p->capability_source_operand != SOL_MIR_OPERATION_NONE
                || p->inherited_root != SOL_MIR_MATERIALIZED_NONE)) return false;
        if (kind == SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY) {
            size_t inherited = SOL_MIR_MATERIALIZED_NONE;
            const SolIr *ir = m->plan->program->ir;
            if (x->source_capability_roots.count == 1) {
                SolIrLocalId source_root = ir->roots[x->source_capability_roots.offset];
                for (size_t q = 0; q < m->images[p->image].locals.count; ++q) {
                    if (!tick(1)) return false;
                    size_t id = m->images[p->image].locals.offset + q;
                    if (m->locals[id].source_local == source_root) inherited = id;
                }
            }
            if (x->construct_operands.count != 1
                || inherited == SOL_MIR_MATERIALIZED_NONE
                || p->inherited_root != inherited) return false;
        }
        for (size_t q = 0; q < x->construct_operands.count; ++q, ++operands) {
            if (!tick(1)) return false;
            const SolMirMaterializedConstructOperand *source
                = &m->construct_operands[x->construct_operands.offset + q];
            const SolMirOperationConstructOperand *actual = &o->construct_operands[operands];
            size_t expected_field = SOL_MIR_OPERATION_NONE;
            if (kind == SOL_MIR_OPERATION_CONSTRUCT_RECORD) {
                size_t matches = 0;
                SolMirPlanSlice fields = r->recipes[x->type].fields;
                for (size_t f = 0; f < fields.count; ++f) {
                    if (!tick(1)) return false;
                    size_t field = fields.offset + f;
                    if (r->fields[field].source_field == source->formal) {
                        expected_field = field; ++matches;
                    }
                }
                if (matches != 1) return false;
            } else if (kind == SOL_MIR_OPERATION_CONSTRUCT_TUPLE) {
                expected_field = field_ordinal(r, x->type,
                    SOL_MIR_OPERATION_NONE, source->formal);
            } else if (kind == SOL_MIR_OPERATION_CONSTRUCT_SUM
                && x->construct_kind == SOL_MIR_CONSTRUCT_ENUM) {
                size_t matches = 0;
                SolMirPlanSlice fields = r->variants[variant].fields;
                for (size_t f = 0; f < fields.count; ++f) {
                    if (!tick(1)) return false;
                    size_t field = fields.offset + f;
                    if (r->fields[field].source_field == source->formal) {
                        expected_field = field; ++matches;
                    }
                }
                if (matches != 1) return false;
            } else if (kind == SOL_MIR_OPERATION_CONSTRUCT_SUM) {
                expected_field = field_ordinal(r, x->type, variant, q);
            }
            if (actual->source_operand_ordinal != q
                || actual->temporary != source->temporary
                || actual->recipe != source->type
                || actual->recipe_field != expected_field
                || actual->layout_field != expected_field) return false;
            if ((source->type < r->recipe_count
                    && r->recipes[source->type].kind == SOL_MIR_RECIPE_FUNCTION)
                || (expected_field != SOL_MIR_OPERATION_NONE
                    && expected_field < r->field_count
                    && r->recipes[r->fields[expected_field].type].kind
                        == SOL_MIR_RECIPE_FUNCTION)) {
                if (expected_field == SOL_MIR_OPERATION_NONE) return false;
                SolMirRecipeId field_recipe = r->fields[expected_field].type;
                const SolMirTypeLayout *operand_layout = source->type < o->layout->type_count
                    ? &o->layout->types[source->type] : NULL;
                const SolMirTypeLayout *field_type_layout = field_recipe < o->layout->type_count
                    ? &o->layout->types[field_recipe] : NULL;
                if (operand_layout != NULL && field_type_layout != NULL
                    && operand_layout->environment_handle_offset
                        == SOL_MIR_LAYOUT_OFFSET_NONE
                    && field_type_layout->environment_handle_offset
                        == SOL_MIR_LAYOUT_OFFSET_NONE
                    && !exact_unbound_function_field_compatible(o->layout,
                        source->type, field_recipe, expected_field)) return false;
            }
            size_t expected_formal = expected_field == SOL_MIR_OPERATION_NONE
                ? source->formal : r->fields[expected_field].ordinal;
            if (actual->formal_ordinal != expected_formal) return false;
            if (actual->layout_field != SOL_MIR_OPERATION_NONE) {
                if (actual->layout_field >= o->layout->field_count
                    || (r->fields[actual->layout_field].type != source->type
                        && !(source->type < r->recipe_count
                            && r->fields[actual->layout_field].type < r->recipe_count
                            && r->recipes[source->type].kind == SOL_MIR_RECIPE_FUNCTION
                            && r->recipes[r->fields[actual->layout_field].type].kind
                                == SOL_MIR_RECIPE_FUNCTION
                            && exact_unbound_function_field_compatible(o->layout,
                                source->type,
                                r->fields[actual->layout_field].type,
                                actual->layout_field)))
                    || o->layout->fields[actual->layout_field].owner_recipe != x->type
                    || o->layout->fields[actual->layout_field].variant
                        != (kind == SOL_MIR_OPERATION_CONSTRUCT_SUM
                            ? variant : SOL_MIR_OPERATION_NONE)
                    || !o->layout->fields[actual->layout_field].has_storage
                    || actual->absolute_offset
                        != o->layout->fields[actual->layout_field].offset) return false;
            } else if (kind == SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY) {
                if (actual->absolute_offset
                    != o->layout->types[x->type].private_source_handle_offset
                    || p->capability_source_operand != q
                    || source->type >= r->recipe_count) return false;
                SolMirOperationCapabilityRule rule
                    = r->recipes[source->type].capability_source == SOL_MIR_RECIPE_NONE
                    ? SOL_MIR_OPERATION_CAPABILITY_ROOT_SOURCE
                    : source->type == r->recipes[x->type].capability_source
                        ? SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE
                        : SOL_MIR_OPERATION_CAPABILITY_PRIVATE_SOURCE;
                if (p->capability_rule != rule) return false;
            } else if (actual->absolute_offset != 0) return false;
        }
        ++at;
    }
    return at == o->constructor_count && operands == o->construct_operand_count;
}

static SolMirRecipeId source_pattern_recipe(const SolMirMaterialization *m,
    size_t image, size_t source) {
    const SolMirMaterializedImage *im = &m->images[image];
    SolMirRecipeId found = SOL_MIR_RECIPE_NONE;
    for (size_t i = 0; i < im->overlays.count; ++i) {
        if (!tick(1)) return SOL_MIR_RECIPE_NONE;
        const SolMirMaterializedTypeOverlay *overlay
            = &m->overlays[im->overlays.offset + i];
        if (overlay->kind != SOL_MIR_PLAN_USE_PATTERN || overlay->source != source)
            continue;
        if (found != SOL_MIR_RECIPE_NONE) return SOL_MIR_RECIPE_NONE;
        found = overlay->type;
    }
    return found;
}

static bool validate_pattern_source(const SolMirOperations *o, size_t image,
    size_t source, const SolMirOperationPatternNode *parent, size_t child_field,
    size_t *cursor, size_t end, size_t depth) {
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    if (!tick(1) || source >= ir->pattern_count || *cursor >= end
        || depth > ir->pattern_count)
        return false;
    const SolIrPattern *p = &ir->patterns[source];
    const SolMirOperationPatternNode *n = &o->pattern_nodes[(*cursor)++];
    SolMirRecipeId recipe = source_pattern_recipe(m, image, source);
    if (recipe >= r->recipe_count || n->recipe != recipe) return false;
    SolMirOperationPatternKind kind;
    if (p->kind == SOL_IR_PATTERN_WILDCARD) kind = SOL_MIR_OPERATION_PATTERN_WILDCARD;
    else if (p->kind == SOL_IR_PATTERN_BINDING) kind = SOL_MIR_OPERATION_PATTERN_BINDING;
    else if (p->kind == SOL_IR_PATTERN_BOOL) kind = SOL_MIR_OPERATION_PATTERN_BOOL;
    else if (p->kind == SOL_IR_PATTERN_VARIANT) kind = SOL_MIR_OPERATION_PATTERN_SUM_TAG;
    else if (p->kind == SOL_IR_PATTERN_RECORD || p->kind == SOL_IR_PATTERN_TUPLE)
        kind = SOL_MIR_OPERATION_PATTERN_PRODUCT;
    else return false;
    if (n->kind != kind || n->boolean != p->boolean) return false;
    if (parent == NULL) {
        if (n->path.count != 0) return false;
    } else {
        if (child_field >= r->field_count || n->path.count != parent->path.count + 1
            || !slice(n->path, o->path_step_count)) return false;
        for (size_t i = 0; i < parent->path.count; ++i) {
            if (!tick(1)) return false;
            const SolMirOperationPathStep *a = &o->path_steps[parent->path.offset + i];
            const SolMirOperationPathStep *b = &o->path_steps[n->path.offset + i];
            if (a->base_recipe != b->base_recipe || a->result_recipe != b->result_recipe
                || a->field_layout != b->field_layout
                || a->object_offset != b->object_offset) return false;
        }
        const SolMirOperationPathStep *last
            = &o->path_steps[n->path.offset + n->path.count - 1];
        if (last->base_recipe != parent->recipe || last->result_recipe != recipe
            || last->field_layout != child_field
            || last->object_offset != o->layout->fields[child_field].offset) return false;
    }
    if (kind == SOL_MIR_OPERATION_PATTERN_SUM_TAG) {
        size_t variant = variant_source(r, recipe, p->variant);
        if (variant >= r->variant_count
            || n->semantic_tag != o->layout->variants[variant].tag) return false;
    } else if (n->semantic_tag != 0) return false;
    for (size_t i = 0; i < p->children.count; ++i) {
        if (!tick(1)) return false;
        const SolIrPatternChild *child = &ir->pattern_children[p->children.offset + i];
        size_t ordinal = child->ordinal;
        if (child->field != SOL_IR_NONE) {
            if (p->definition >= ir->definition_count
                || child->field < ir->definitions[p->definition].fields.offset) return false;
            ordinal = child->field - ir->definitions[p->definition].fields.offset;
        }
        size_t variant = p->kind == SOL_IR_PATTERN_VARIANT
            ? variant_source(r, recipe, p->variant) : SOL_MIR_OPERATION_NONE;
        size_t field = field_ordinal(r, recipe, variant, ordinal);
        if (field >= r->field_count || !validate_pattern_source(o, image,
                child->pattern, n, field, cursor, end, depth + 1)) return false;
    }
    return true;
}

static bool pattern_contains(const SolIr *ir, size_t root, size_t target,
    size_t depth) {
    if (!tick(1) || root >= ir->pattern_count
        || depth > ir->pattern_count) return false;
    if (root == target) return true;
    const SolIrPattern *p = &ir->patterns[root];
    for (size_t i = 0; i < p->children.count; ++i) {
        if (!tick(1)) return false;
        if (pattern_contains(ir, ir->pattern_children[p->children.offset + i].pattern,
                target, depth + 1)) return true;
    }
    return false;
}

static bool validate_extraction_source(const SolMirOperations *o, size_t root,
    size_t target, SolMirRecipeId recipe, SolMirPlanSlice path, size_t depth) {
    const SolMirRepresentation *r = o->layout->representation;
    const SolIr *ir = r->materialization->plan->program->ir;
    if (!tick(1) || root >= ir->pattern_count
        || depth > ir->pattern_count) return false;
    if (root == target) return depth == path.count;
    const SolIrPattern *p = &ir->patterns[root];
    size_t selected = SOL_MIR_OPERATION_NONE;
    for (size_t i = 0; i < p->children.count; ++i) {
        if (!tick(1)) return false;
        const SolIrPatternChild *child = &ir->pattern_children[p->children.offset + i];
        if (!pattern_contains(ir, child->pattern, target, depth + 1)) continue;
        if (selected != SOL_MIR_OPERATION_NONE) return false;
        selected = i;
    }
    if (selected == SOL_MIR_OPERATION_NONE || depth >= path.count) return false;
    const SolIrPatternChild *child
        = &ir->pattern_children[p->children.offset + selected];
    size_t ordinal = child->ordinal;
    if (child->field != SOL_IR_NONE) {
        if (p->definition >= ir->definition_count
            || child->field < ir->definitions[p->definition].fields.offset) return false;
        ordinal = child->field - ir->definitions[p->definition].fields.offset;
    }
    size_t variant = p->kind == SOL_IR_PATTERN_VARIANT
        ? variant_source(r, recipe, p->variant) : SOL_MIR_OPERATION_NONE;
    size_t field = field_ordinal(r, recipe, variant, ordinal);
    const SolMirOperationPathStep *step = &o->path_steps[path.offset + depth];
    if (field >= r->field_count || step->base_recipe != recipe
        || step->result_recipe != r->fields[field].type
        || step->field_layout != field
        || step->object_offset != o->layout->fields[field].offset) return false;
    return validate_extraction_source(o, child->pattern, target,
        r->fields[field].type, path, depth + 1);
}

static bool validate_patterns(const SolMirOperations *o, size_t *prov) {
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    size_t tests = 0, extracts = 0, node_at = 0;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        if (!tick(1)) return false;
        const SolMirMaterializedInstruction *x = &m->instructions[i];
        if (x->kind != SOL_MIR_INST_PATTERN_TEST
            && x->kind != SOL_MIR_INST_PATTERN_VALUE) continue;
        size_t provenance_id = provenance_find(o,
            x->kind == SOL_MIR_INST_PATTERN_TEST
                ? SOL_MIR_OPERATION_PROVENANCE_PATTERN_TEST
                : SOL_MIR_OPERATION_PROVENANCE_PATTERN_EXTRACTION,
            x->kind == SOL_MIR_INST_PATTERN_TEST ? tests : extracts);
        if (provenance_id == SOL_MIR_OPERATION_NONE) return false;
        const SolMirOperationProvenance *v = &o->provenance[provenance_id];
        ++*prov;
        if (v->kind != (x->kind == SOL_MIR_INST_PATTERN_TEST
                ? SOL_MIR_OPERATION_PROVENANCE_PATTERN_TEST
                : SOL_MIR_OPERATION_PROVENANCE_PATTERN_EXTRACTION)
            || v->source_expression != x->match_expression
            || v->source_pattern != x->source_pattern) return false;
        if (x->kind == SOL_MIR_INST_PATTERN_TEST) {
            if (tests >= o->pattern_test_count || v->executable != tests) return false;
            const SolMirOperationPatternTest *p = &o->pattern_tests[tests++];
            if (p->image != instruction_image(m, i) || p->instruction != i
                || p->scrutinee != x->pattern_scrutinee
                || p->scrutinee < m->images[p->image].temporaries.offset
                || p->scrutinee - m->images[p->image].temporaries.offset
                    >= m->images[p->image].temporaries.count
                || p->scrutinee_recipe != m->temporaries[x->pattern_scrutinee].type
                || p->result != x->result || p->nodes.offset != node_at
                || !slice(p->nodes, o->pattern_node_count) || p->nodes.count == 0) return false;
            size_t source_cursor = node_at;
            if (!validate_pattern_source(o, p->image, x->source_pattern, NULL,
                    SOL_MIR_OPERATION_NONE, &source_cursor,
                    node_at + p->nodes.count, 0)
                || source_cursor != node_at + p->nodes.count) return false;
            for (size_t q = 0; q < p->nodes.count; ++q, ++node_at) {
                if (!tick(1)) return false;
                const SolMirOperationPatternNode *n = &o->pattern_nodes[node_at];
                if (n->kind < SOL_MIR_OPERATION_PATTERN_WILDCARD
                    || n->kind > SOL_MIR_OPERATION_PATTERN_PRODUCT
                    || n->recipe >= r->recipe_count || !slice(n->path, o->path_step_count))
                    return false;
                SolMirRecipeId recipe = p->scrutinee_recipe;
                for (size_t s = 0; s < n->path.count; ++s) {
                    if (!tick(1)) return false;
                    const SolMirOperationPathStep *step
                        = &o->path_steps[n->path.offset + s];
                    if (step->base_recipe != recipe || step->field_layout >= r->field_count
                        || r->fields[step->field_layout].type != step->result_recipe
                        || o->layout->fields[step->field_layout].offset != step->object_offset)
                        return false;
                    recipe = step->result_recipe;
                }
                if (recipe != n->recipe) return false;
                if (n->kind == SOL_MIR_OPERATION_PATTERN_SUM_TAG) {
                    bool found = false;
                    SolMirPlanSlice variants = r->recipes[n->recipe].variants;
                    for (size_t z = 0; z < variants.count; ++z) {
                        if (!tick(1)) return false;
                        found |= o->layout->variants[variants.offset + z].tag
                            == n->semantic_tag;
                    }
                    if (!found) return false;
                } else if (n->semantic_tag != 0) return false;
            }
        } else {
            if (extracts >= o->pattern_extraction_count || v->executable != extracts)
                return false;
            const SolMirOperationPatternExtraction *p
                = &o->pattern_extractions[extracts++];
            if (p->image != instruction_image(m, i) || p->instruction != i
                || p->scrutinee != x->pattern_scrutinee
                || p->scrutinee_recipe != m->temporaries[x->pattern_scrutinee].type
                || p->result != x->result || p->result_recipe != x->type
                || p->copy_kind != r->recipes[x->type].copy_kind
                || !slice(p->path, o->path_step_count)) return false;
            const SolIr *ir = m->plan->program->ir;
            if (x->source_arm >= ir->arm_count
                || !validate_extraction_source(o, ir->arms[x->source_arm].pattern,
                    x->source_pattern, p->scrutinee_recipe, p->path, 0)) return false;
            SolMirRecipeId recipe = p->scrutinee_recipe;
            for (size_t q = 0; q < p->path.count; ++q) {
                if (!tick(1)) return false;
                const SolMirOperationPathStep *step = &o->path_steps[p->path.offset + q];
                if (step->base_recipe != recipe || step->field_layout >= r->field_count
                    || r->fields[step->field_layout].type != step->result_recipe
                    || o->layout->fields[step->field_layout].offset != step->object_offset)
                    return false;
                recipe = step->result_recipe;
            }
            if (recipe != p->result_recipe) return false;
        }
    }
    return tests == o->pattern_test_count && extracts == o->pattern_extraction_count
        && node_at == o->pattern_node_count;
}

static bool validate_arithmetic(const SolMirOperations *o, size_t *prov,
    SolDiagnostics *diagnostics) {
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    size_t at = 0, equality = 0, children = 0;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        if (!tick(1)) return false;
        const SolMirMaterializedInstruction *x = &m->instructions[i];
        if (x->kind != SOL_MIR_INST_UNARY && x->kind != SOL_MIR_INST_BINARY
            && x->kind != SOL_MIR_INST_COMPOUND_UPDATE) continue;
        if (at >= o->arithmetic_count) return false;
        const SolMirOperationArithmeticPlan *p = &o->arithmetic[at];
        size_t provenance_id = provenance_find(o,
            SOL_MIR_OPERATION_PROVENANCE_ARITHMETIC, at);
        if (provenance_id == SOL_MIR_OPERATION_NONE) return false;
        const SolMirOperationProvenance *v = &o->provenance[provenance_id];
        ++*prov;
        SolMirOperationOpcode op; unsigned failures;
        if (!expected_opcode(x->kind, x->operator_kind, &op, &failures)
            || v->kind != SOL_MIR_OPERATION_PROVENANCE_ARITHMETIC
            || v->executable != at || v->source_expression != x->source_expression
            || p->image != instruction_image(m, i) || p->instruction != i
            || p->opcode != op || p->failures != failures || p->left != x->left
            || p->right != x->right || p->previous != x->previous
            || p->result != x->result || p->result_recipe != x->type
            || p->compound != (x->kind == SOL_MIR_INST_COMPOUND_UPDATE)) return false;
        SolMirRecipeId operand = x->kind == SOL_MIR_INST_COMPOUND_UPDATE
            ? m->temporaries[x->previous].type : m->values[x->left].type;
        if (p->operand_recipe != operand) return false;
        bool equal = op == SOL_MIR_OPERATION_VALUE_EQ || op == SOL_MIR_OPERATION_VALUE_NE;
        if (!equal && (p->equality.offset != 0 || p->equality.count != 0)) return false;
        if (equal) {
            if (p->equality.offset != equality
                || !slice(p->equality, o->equality_node_count)) return false;
            if (r->recipe_count != 0 && !tick(1)) return false;
            unsigned char *seen = r->recipe_count == 0 ? NULL
                : calloc(r->recipe_count, 1);
            if (r->recipe_count != 0 && seen == NULL) {
                mark_allocation_failed(diagnostics); return false;
            }
            size_t node_end = equality + p->equality.count;
            bool valid = validate_equality_recipe(o, operand, seen, node_end,
                &equality, &children) && equality == node_end;
            free(seen); if (!valid) return false;
        }
        ++at;
    }
    return at == o->arithmetic_count && equality == o->equality_node_count
        && children == o->equality_child_count;
}

static bool validate_propagations_predicates(const SolMirOperations *o,
    size_t *prov) {
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    size_t propagations = 0, predicates = 0;
    for (size_t i = 0; i < m->block_count; ++i) {
        if (!tick(1)) return false;
        const SolMirMaterializedTerminator *t = &m->blocks[i].terminator;
        if (t->kind == SOL_MIR_TERM_PROPAGATE) {
            if (propagations >= o->propagation_count)
                return false;
            const SolMirOperationPropagationPlan *p = &o->propagations[propagations];
            size_t provenance_id = provenance_find(o,
                SOL_MIR_OPERATION_PROVENANCE_PROPAGATION, propagations);
            if (provenance_id == SOL_MIR_OPERATION_NONE) return false;
            const SolMirOperationProvenance *v = &o->provenance[provenance_id];
            ++*prov;
            size_t source = m->temporaries[t->operand].type;
            size_t success_variant = variant_ordinal(r, source,
                t->propagation_kind == SOL_IR_PROPAGATE_OPTION ? 1 : 0);
            size_t source_residual_variant = variant_ordinal(r, source,
                t->propagation_kind == SOL_IR_PROPAGATE_OPTION ? 0 : 1);
            size_t residual_recipe = m->values[t->residual_result].type;
            size_t destination_residual_variant = variant_ordinal(r, residual_recipe,
                t->propagation_kind == SOL_IR_PROPAGATE_OPTION ? 0 : 1);
            if (v->kind != SOL_MIR_OPERATION_PROVENANCE_PROPAGATION
                || v->executable != propagations || v->source_expression != t->source_expression
                || p->image != block_image(m, i) || p->block != i
                || p->source != t->operand || p->source_recipe != source
                || p->success_result != t->value_result
                || p->success_recipe != m->values[t->value_result].type
                || p->residual_result != t->residual_result
                || p->residual_recipe != residual_recipe
                || p->success_variant_layout != success_variant
                || p->success_tag != o->layout->variants[success_variant].tag
                || p->source_residual_variant_layout != source_residual_variant
                || p->source_residual_tag
                    != o->layout->variants[source_residual_variant].tag
                || p->destination_residual_variant_layout
                    != destination_residual_variant
                || p->destination_residual_tag
                    != o->layout->variants[destination_residual_variant].tag
                || p->success_edge != t->value_edge || p->residual_edge != t->residual_edge)
                return false;
            size_t sf = field_ordinal(r, source, success_variant, 0);
            size_t source_rf = t->propagation_kind == SOL_IR_PROPAGATE_OPTION
                ? SOL_MIR_OPERATION_NONE : field_ordinal(r, source,
                    source_residual_variant, 0);
            size_t destination_rf = t->propagation_kind == SOL_IR_PROPAGATE_OPTION
                ? SOL_MIR_OPERATION_NONE : field_ordinal(r, residual_recipe,
                    destination_residual_variant, 0);
            if (sf >= r->field_count || r->fields[sf].type != p->success_recipe
                || p->success_field_layout != sf
                || p->success_field_offset != o->layout->fields[sf].offset
                || p->source_residual_field_layout != source_rf
                || p->source_residual_field_recipe
                    != (source_rf == SOL_MIR_OPERATION_NONE ? SOL_MIR_RECIPE_NONE
                        : r->fields[source_rf].type)
                || p->source_residual_field_offset != (source_rf == SOL_MIR_OPERATION_NONE
                    ? SOL_MIR_LAYOUT_OFFSET_NONE : o->layout->fields[source_rf].offset)
                || p->destination_residual_field_layout != destination_rf
                || p->destination_residual_field_recipe
                    != (destination_rf == SOL_MIR_OPERATION_NONE
                        ? SOL_MIR_RECIPE_NONE : r->fields[destination_rf].type)
                || p->destination_residual_field_offset
                    != (destination_rf == SOL_MIR_OPERATION_NONE
                        ? SOL_MIR_LAYOUT_OFFSET_NONE
                        : o->layout->fields[destination_rf].offset)
                || (t->propagation_kind == SOL_IR_PROPAGATE_RESULT
                    && (source_rf >= r->field_count
                        || destination_rf >= r->field_count
                        || r->fields[source_rf].type
                            != r->fields[destination_rf].type)))
                return false;
            ++propagations;
        } else if (t->kind == SOL_MIR_TERM_CHECK_CONTRACT
            || t->kind == SOL_MIR_TERM_CHECK_REFINED) {
            if (predicates >= o->predicate_count)
                return false;
            const SolMirOperationPredicatePlan *p = &o->predicates[predicates];
            size_t provenance_id = provenance_find(o,
                SOL_MIR_OPERATION_PROVENANCE_PREDICATE, predicates);
            if (provenance_id == SOL_MIR_OPERATION_NONE) return false;
            const SolMirOperationProvenance *v = &o->provenance[provenance_id];
            ++*prov;
            if (v->kind != SOL_MIR_OPERATION_PROVENANCE_PREDICATE
                || v->executable != predicates || v->source_obligation != t->source_obligation
                || v->source_expression != t->source_expression
                || p->body >= o->predicate_body_count
                || p->image != block_image(m, i) || p->block != i
                || p->context >= m->context_count
                || m->contexts[p->context].obligation != t->source_obligation
                || m->contexts[p->context].instance != p->image
                || m->contexts[p->context].kind
                    != (t->kind == SOL_MIR_TERM_CHECK_CONTRACT
                        ? SOL_MIR_PLAN_CONTEXT_CONTRACT
                        : SOL_MIR_PLAN_CONTEXT_REFINEMENT)
                || p->kind != (t->kind == SOL_MIR_TERM_CHECK_CONTRACT
                    ? SOL_MIR_OPERATION_PREDICATE_CONTRACT
                    : SOL_MIR_OPERATION_PREDICATE_REFINEMENT)
                || p->representation != t->representation || p->result != t->result
                || p->result_recipe != (t->result == SOL_MIR_MATERIALIZED_NONE
                    ? SOL_MIR_RECIPE_NONE : m->values[t->result].type)
                || p->output_recipe >= r->recipe_count
                || r->recipes[p->output_recipe].kind != SOL_MIR_RECIPE_BOOL
                || p->provenance != provenance_id) return false;
            if (t->kind == SOL_MIR_TERM_CHECK_REFINED) {
                if (p->input_recipe != m->temporaries[t->representation].type
                    || p->result_recipe >= r->recipe_count
                    || r->recipes[p->result_recipe].kind != SOL_MIR_RECIPE_REFINED
                    || r->recipes[p->result_recipe].backing != p->input_recipe
                    || m->contexts[p->context].definition != t->source_definition
                    || m->contexts[p->context].source.expression
                        != t->source_expression
                    || p->contract_phase != (SolContractClauseKind)0
                    || p->contract_outcome != (SolContractOutcomeKind)0) return false;
            } else if (p->input_recipe != SOL_MIR_RECIPE_NONE
                || p->contract_phase != t->contract_phase
                || p->contract_outcome != t->contract_outcome) return false;
            ++predicates;
        }
    }
    return propagations == o->propagation_count && predicates == o->predicate_count;
}

static bool predicate_block_dominates(const SolMirOperations *o,
    unsigned char *seen, size_t *queue, size_t definition, size_t use) {
    if (definition == use) return true;
    if (definition >= o->predicate_block_count
        || use >= o->predicate_block_count
        || o->predicate_blocks[definition].body
            != o->predicate_blocks[use].body) return false;
    const SolMirPredicateBody *body
        = &o->predicate_bodies[o->predicate_blocks[use].body];
    if (definition == body->entry) return true;
    for (size_t q = 0; q < body->blocks.count; ++q) {
        if (!tick(1)) return false;
        seen[body->blocks.offset + q] = 0;
    }
    size_t first = 0, count = 0;
    seen[body->entry] = 1;
    queue[count++] = body->entry;
    while (first < count) {
        if (!tick(1)) return false;
        size_t source = queue[first++];
        for (size_t edge_id = 0; edge_id < o->predicate_edge_count; ++edge_id) {
            if (!tick(1)) return false;
            const SolMirPredicateEdge *edge = &o->predicate_edges[edge_id];
            if (edge->source != source || edge->target == definition
                || seen[edge->target] != 0) continue;
            if (edge->target == use) return false;
            seen[edge->target] = 1;
            queue[count++] = edge->target;
        }
    }
    return true;
}

static bool predicate_value_available(const SolMirOperations *o,
    unsigned char *seen, size_t *queue, size_t value_id, size_t use_block,
    size_t before_instruction, bool edge_use) {
    if (value_id >= o->predicate_value_count || use_block >= o->predicate_block_count)
        return false;
    const SolMirPredicateValue *value = &o->predicate_values[value_id];
    if (value->block >= o->predicate_block_count) return false;
    if (o->predicate_blocks[value->block].body
        != o->predicate_blocks[use_block].body) return false;
    if (value->block == use_block) {
        if (value->kind == SOL_MIR_PREDICATE_VALUE_INSTRUCTION)
            return before_instruction == SOL_MIR_OPERATION_NONE
                || value->definition < before_instruction;
        if (value->kind == SOL_MIR_PREDICATE_VALUE_TERMINATOR)
            return edge_use;
        return true;
    }
    if (value->kind == SOL_MIR_PREDICATE_VALUE_TERMINATOR) return false;
    return predicate_block_dominates(o, seen, queue, value->block, use_block);
}

static bool predicate_empty_slice(SolMirPlanSlice value) {
    return value.offset == 0 && value.count == 0;
}

static bool predicate_instruction_shape(
    const SolMirPredicateInstruction *instruction) {
    bool unary = instruction->kind == SOL_MIR_PREDICATE_INST_UNARY;
    bool binary = instruction->kind == SOL_MIR_PREDICATE_INST_BINARY;
    bool project = instruction->kind == SOL_MIR_PREDICATE_INST_PROJECT;
    bool function = instruction->kind == SOL_MIR_PREDICATE_INST_FUNCTION;
    bool bound = instruction->kind == SOL_MIR_PREDICATE_INST_BOUND_OPERATION;
    bool construct = instruction->kind == SOL_MIR_PREDICATE_INST_CONSTRUCT;
    bool pattern_test = instruction->kind == SOL_MIR_PREDICATE_INST_PATTERN_TEST;
    bool pattern_extract
        = instruction->kind == SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT;
    bool has_left = unary || binary || project || bound || pattern_test
        || pattern_extract;
    if ((!has_left && instruction->left != SOL_MIR_OPERATION_NONE)
        || (!binary && instruction->right != SOL_MIR_OPERATION_NONE)
        || (instruction->kind != SOL_MIR_PREDICATE_INST_I64
            && instruction->integer != 0)
        || (instruction->kind != SOL_MIR_PREDICATE_INST_BOOL
            && instruction->boolean)
        || (instruction->kind != SOL_MIR_PREDICATE_INST_TEXT
            && !predicate_empty_slice(instruction->bytes))
        || ((!unary && !binary)
            && (instruction->opcode != SOL_MIR_OPERATION_BOOL_NOT
                || instruction->failures != 0))
        || (!construct && !predicate_empty_slice(instruction->operands))
        || ((!project && !pattern_extract)
            && !predicate_empty_slice(instruction->path))
        || (!pattern_test && !predicate_empty_slice(instruction->pattern))
        || (!construct
            && (instruction->construct_kind
                    != SOL_MIR_PREDICATE_CONSTRUCT_RECORD
                || instruction->variant_layout != SOL_MIR_OPERATION_NONE
                || instruction->semantic_tag != 0))
        || (construct
            && instruction->construct_kind != SOL_MIR_PREDICATE_CONSTRUCT_SUM
            && (instruction->variant_layout != SOL_MIR_OPERATION_NONE
                || instruction->semantic_tag != 0))
        || ((!function && !bound)
            && instruction->binding != SOL_MIR_MATERIALIZED_NONE)) return false;
    return true;
}

static bool predicate_terminator_shape(
    const SolMirPredicateTerminator *term) {
    bool returning = term->kind == SOL_MIR_PREDICATE_TERM_RETURN;
    bool jump = term->kind == SOL_MIR_PREDICATE_TERM_JUMP;
    bool branch = term->kind == SOL_MIR_PREDICATE_TERM_BRANCH;
    bool invoke = term->kind == SOL_MIR_PREDICATE_TERM_INVOKE;
    bool propagate = term->kind == SOL_MIR_PREDICATE_TERM_PROPAGATE;
    bool refined = term->kind == SOL_MIR_PREDICATE_TERM_CHECK_REFINED;
    bool failure = term->kind == SOL_MIR_PREDICATE_TERM_FAILURE;
    bool has_value = returning || propagate || refined;
    bool has_result = invoke || propagate || refined;
    bool has_continuations = invoke || propagate || refined;
    if ((!has_value && term->value != SOL_MIR_OPERATION_NONE)
        || (!branch && term->condition != SOL_MIR_OPERATION_NONE)
        || (!invoke
            && (term->callee != SOL_MIR_OPERATION_NONE
                || term->receiver != SOL_MIR_OPERATION_NONE
                || term->receiver_access != SOL_ACCESS_OWNED
                || !predicate_empty_slice(term->arguments)
                || term->call_kind != (SolIrCallKind)0
                || term->binding != SOL_MIR_MATERIALIZED_NONE
                || term->effects != SOL_MIR_MATERIALIZED_NONE))
        || (!has_result && term->result != SOL_MIR_OPERATION_NONE)
        || (!jump && term->edge != SOL_MIR_OPERATION_NONE)
        || (!branch
            && (term->true_edge != SOL_MIR_OPERATION_NONE
                || term->false_edge != SOL_MIR_OPERATION_NONE))
        || (!has_continuations
            && (term->normal_edge != SOL_MIR_OPERATION_NONE
                || term->failure_edge != SOL_MIR_OPERATION_NONE))
        || (!propagate
            && (term->propagation_kind != (SolIrPropagationKind)0
                || term->success_variant_layout != SOL_MIR_OPERATION_NONE
                || term->residual_variant_layout != SOL_MIR_OPERATION_NONE
                || term->success_field_layout != SOL_MIR_OPERATION_NONE))
        || (!refined && term->nested_body != SOL_MIR_OPERATION_NONE)
        || (!has_result && term->result_recipe != SOL_MIR_RECIPE_NONE)
        || (!failure
            && term->failure_kind != SOL_MIR_PREDICATE_FAILURE_CALL)) return false;
    return true;
}

static bool validate_predicate_cfg(const SolMirOperations *o,
    SolDiagnostics *diagnostics) {
#define PFAIL(tag) do { goto predicate_failed; } while (0)
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    unsigned char *instruction_seen = NULL, *value_seen = NULL;
    unsigned char *edge_seen = NULL, *reachable = NULL;
    unsigned char *operand_seen = NULL, *path_seen = NULL, *pattern_seen = NULL;
    size_t *indegree = NULL, *queue = NULL;
    size_t inputs = 0, blocks = 0, values = 0;
    for (size_t i = 0; i < o->predicate_body_count; ++i) {
        if (!tick(1)) PFAIL("work");
        const SolMirPredicateBody *body = &o->predicate_bodies[i];
        if (!slice(body->inputs, o->predicate_input_count)
            || !slice(body->blocks, o->predicate_block_count)
            || !slice(body->values, o->predicate_value_count))
            PFAIL("body slices");
        bool instance_owner = body->owner_kind
            == SOL_MIR_PREDICATE_OWNER_INSTANCE;
        bool import_owner = body->owner_kind == SOL_MIR_PREDICATE_OWNER_IMPORT;
        if ((!instance_owner && !import_owner)
            || (instance_owner && (body->instance >= m->image_count
                || body->import != SOL_MIR_OPERATION_NONE))
            || (import_owner && (body->instance != SOL_MIR_OPERATION_NONE
                || body->import >= m->import_count))
            || body->context >= m->context_count
            || (instance_owner
                && (m->contexts[body->context].target_kind
                        != SOL_MIR_PLAN_TARGET_INSTANCE
                    || m->contexts[body->context].instance != body->instance))
            || (import_owner
                && (m->contexts[body->context].target_kind
                        != SOL_MIR_PLAN_TARGET_IMPORT
                    || m->contexts[body->context].import != body->import))
            || body->inputs.offset != inputs || body->blocks.offset != blocks
            || body->values.offset != values || body->blocks.count == 0
            || body->entry != body->blocks.offset
            || body->output_recipe >= r->recipe_count
            || r->recipes[body->output_recipe].kind != SOL_MIR_RECIPE_BOOL
            || m->contexts[body->context].obligation
                >= m->plan->program->ir->obligation_count
            || (m->contexts[body->context].kind == SOL_MIR_PLAN_CONTEXT_CONTRACT
                && body->refinement_self_recipe != SOL_MIR_RECIPE_NONE)
            || (m->contexts[body->context].kind == SOL_MIR_PLAN_CONTEXT_REFINEMENT
                && body->refinement_self_recipe >= r->recipe_count)) {
            PFAIL("body");
        }
        if (!add_size(&inputs, body->inputs.count)
            || !add_size(&blocks, body->blocks.count)
            || !add_size(&values, body->values.count)) PFAIL("body totals");
    }
    if (inputs != o->predicate_input_count || blocks != o->predicate_block_count
        || values != o->predicate_value_count) PFAIL("totals");
    for (size_t i = 0; i < o->predicate_input_count; ++i) {
        if (!tick(1)) PFAIL("work");
        const SolMirPredicateInput *input = &o->predicate_inputs[i];
        if (input->kind < SOL_MIR_PREDICATE_INPUT_RECEIVER
            || input->kind > SOL_MIR_PREDICATE_INPUT_REFINEMENT_SELF
            || input->recipe >= r->recipe_count
            || input->access < SOL_ACCESS_OWNED
            || input->access > SOL_ACCESS_EXCLUSIVE) PFAIL("input");
    }
    instruction_seen = o->predicate_instruction_count == 0 ? NULL
        : calloc(o->predicate_instruction_count, 1);
    value_seen = o->predicate_value_count == 0 ? NULL
        : calloc(o->predicate_value_count, 1);
    edge_seen = o->predicate_edge_count == 0 ? NULL
        : calloc(o->predicate_edge_count, 1);
    reachable = o->predicate_block_count == 0 ? NULL
        : calloc(o->predicate_block_count, 1);
    indegree = o->predicate_block_count == 0 ? NULL
        : calloc(o->predicate_block_count, sizeof(*indegree));
    queue = o->predicate_block_count == 0 ? NULL
        : malloc(o->predicate_block_count * sizeof(*queue));
    operand_seen = o->predicate_operand_count == 0 ? NULL
        : calloc(o->predicate_operand_count, 1);
    path_seen = o->predicate_path_step_count == 0 ? NULL
        : calloc(o->predicate_path_step_count, 1);
    pattern_seen = o->predicate_pattern_node_count == 0 ? NULL
        : calloc(o->predicate_pattern_node_count, 1);
    if ((o->predicate_instruction_count != 0 && instruction_seen == NULL)
        || (o->predicate_value_count != 0 && value_seen == NULL)
        || (o->predicate_edge_count != 0 && edge_seen == NULL)
        || (o->predicate_block_count != 0
            && (reachable == NULL || indegree == NULL || queue == NULL))
        || (o->predicate_operand_count != 0 && operand_seen == NULL)
        || (o->predicate_path_step_count != 0 && path_seen == NULL)
        || (o->predicate_pattern_node_count != 0 && pattern_seen == NULL)) {
        mark_allocation_failed(diagnostics);
        PFAIL("allocation");
    }
    for (size_t i = 0; i < o->predicate_block_count; ++i) {
        if (!tick(1)) PFAIL("work");
        const SolMirPredicateBlock *block = &o->predicate_blocks[i];
        if (block->body >= o->predicate_body_count
            || !slice(block->parameters, o->predicate_value_count)
            || !slice(block->instructions, o->predicate_instruction_count)
            || block->terminator.kind < SOL_MIR_PREDICATE_TERM_RETURN
            || block->terminator.kind > SOL_MIR_PREDICATE_TERM_FAILURE)
            PFAIL("block");
        const SolMirPredicateBody *body = &o->predicate_bodies[block->body];
        if (i < body->blocks.offset || i - body->blocks.offset >= body->blocks.count)
            PFAIL("block owner");
        for (size_t q = 0; q < block->parameters.count; ++q) {
            size_t value = block->parameters.offset + q;
            if (value < body->values.offset
                || value - body->values.offset >= body->values.count
                || value_seen[value] != 0
                || o->predicate_values[value].kind
                    != SOL_MIR_PREDICATE_VALUE_BLOCK_PARAMETER
                || o->predicate_values[value].block != i
                || o->predicate_values[value].definition != q) PFAIL("parameter");
            value_seen[value] = 1;
        }
        for (size_t q = 0; q < block->instructions.count; ++q) {
            size_t instruction = block->instructions.offset + q;
            if (instruction_seen[instruction] != 0
                || o->predicate_instructions[instruction].block != i)
                PFAIL("instruction owner");
            instruction_seen[instruction] = 1;
        }
        const SolMirPredicateTerminator *term = &block->terminator;
        if (!slice(term->arguments, o->predicate_operand_count)
            || !predicate_terminator_shape(term)) PFAIL("terminator payload");
        SolMirPredicateEdgeId ids[2] = {SOL_MIR_OPERATION_NONE,
            SOL_MIR_OPERATION_NONE};
        size_t edge_count = 0;
        if (term->kind == SOL_MIR_PREDICATE_TERM_RETURN) {
            if (term->value < body->values.offset
                || term->value - body->values.offset >= body->values.count
                || o->predicate_values[term->value].recipe != body->output_recipe)
                PFAIL("return");
        } else if (term->kind == SOL_MIR_PREDICATE_TERM_JUMP) {
            ids[edge_count++] = term->edge;
        } else if (term->kind == SOL_MIR_PREDICATE_TERM_BRANCH) {
            if (term->condition < body->values.offset
                || term->condition - body->values.offset >= body->values.count
                || o->predicate_values[term->condition].recipe >= r->recipe_count
                || r->recipes[o->predicate_values[term->condition].recipe].kind
                    != SOL_MIR_RECIPE_BOOL) PFAIL("branch");
            ids[edge_count++] = term->true_edge; ids[edge_count++] = term->false_edge;
        } else if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE) {
            if (term->binding >= m->binding_count || term->effects >= m->effect_row_count
                || m->effect_rows[term->effects].atoms.count != 0
                || term->result >= o->predicate_value_count
                || o->predicate_values[term->result].kind
                    != SOL_MIR_PREDICATE_VALUE_TERMINATOR
                || o->predicate_values[term->result].block != i
                || o->predicate_values[term->result].recipe != term->result_recipe)
                PFAIL("invoke");
            const SolMirMaterializedBinding *binding = &m->bindings[term->binding];
            SolMirRecipeId receiver_recipe, result_recipe;
            SolMirMaterializedEffectRowId effects;
            SolMirPlanSlice parameters, accesses;
            if (binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
                if (binding->instance >= m->image_count) PFAIL("invoke target");
                const SolMirMaterializedImage *target = &m->images[binding->instance];
                receiver_recipe = target->receiver; result_recipe = target->result;
                effects = target->effects; parameters = target->parameter_types;
                accesses = target->parameter_accesses;
            } else {
                if (binding->target_kind != SOL_MIR_MATERIALIZED_TARGET_IMPORT
                    || binding->import >= m->import_count) PFAIL("invoke target");
                const SolMirMaterializedImport *target = &m->imports[binding->import];
                receiver_recipe = target->receiver; result_recipe = target->result;
                effects = target->effects; parameters = target->parameter_types;
                accesses = target->parameter_accesses;
            }
            if (term->effects != effects || term->result_recipe != result_recipe
                || term->arguments.count != parameters.count
                || ((term->call_kind == SOL_IR_CALL_CALLBACK
                        || term->call_kind == SOL_IR_CALL_CAPABILITY)
                    != (term->callee != SOL_MIR_OPERATION_NONE))
                || ((term->call_kind == SOL_IR_CALL_METHOD)
                    != (term->receiver != SOL_MIR_OPERATION_NONE))
                || (term->receiver != SOL_MIR_OPERATION_NONE
                    && (term->receiver >= o->predicate_value_count
                        || o->predicate_values[term->receiver].recipe
                            != receiver_recipe))) PFAIL("invoke signature");
            if (term->callee != SOL_MIR_OPERATION_NONE) {
                if (term->callee >= o->predicate_value_count) PFAIL("invoke callee");
                SolMirRecipeId callee_recipe
                    = o->predicate_values[term->callee].recipe;
                if (callee_recipe >= r->recipe_count
                    || r->recipes[callee_recipe].kind != SOL_MIR_RECIPE_FUNCTION
                    || r->recipes[callee_recipe].result != result_recipe
                    || r->recipes[callee_recipe].effects != effects
                    || r->recipes[callee_recipe].parameters.count
                        != parameters.count
                    || r->recipes[callee_recipe].parameter_accesses.count
                        != accesses.count) PFAIL("invoke callee");
                for (size_t q = 0; q < parameters.count; ++q) {
                    if (r->recipe_ids[r->recipes[callee_recipe].parameters.offset + q]
                            != m->type_ids[parameters.offset + q]
                        || r->accesses[r->recipes[callee_recipe]
                                .parameter_accesses.offset + q]
                            != m->accesses[accesses.offset + q])
                        PFAIL("invoke callback signature");
                }
            }
            for (size_t q = 0; q < term->arguments.count; ++q) {
                const SolMirPredicateOperand *argument
                    = &o->predicate_operands[term->arguments.offset + q];
                if (argument->formal_ordinal >= parameters.count
                    || argument->value >= o->predicate_value_count
                    || o->predicate_values[argument->value].recipe != m->type_ids[
                        parameters.offset + argument->formal_ordinal]
                    || argument->access != m->accesses[
                        accesses.offset + argument->formal_ordinal]
                    || argument->field_layout != SOL_MIR_OPERATION_NONE) {
                    PFAIL("invoke argument");
                }
                for (size_t prior = 0; prior < q; ++prior) {
                    if (!tick(1)) PFAIL("work");
                    if (o->predicate_operands[term->arguments.offset + prior]
                            .formal_ordinal == argument->formal_ordinal)
                        PFAIL("duplicate invoke argument");
                }
            }
            for (size_t q = 0; q < term->arguments.count; ++q) {
                size_t operand = term->arguments.offset + q;
                if (operand_seen[operand] != 0) PFAIL("operand owner");
                operand_seen[operand] = 1;
            }
            ids[edge_count++] = term->normal_edge;
            ids[edge_count++] = term->failure_edge;
        } else if (term->kind == SOL_MIR_PREDICATE_TERM_PROPAGATE) {
            if (term->value >= o->predicate_value_count
                || term->result >= o->predicate_value_count
                || term->success_variant_layout >= r->variant_count
                || term->residual_variant_layout >= r->variant_count
                || term->success_field_layout >= r->field_count
                || o->predicate_values[term->result].kind
                    != SOL_MIR_PREDICATE_VALUE_TERMINATOR
                || o->predicate_values[term->result].block != i
                || o->predicate_values[term->result].recipe != term->result_recipe)
                PFAIL("propagate");
            SolMirRecipeId source_recipe = o->predicate_values[term->value].recipe;
            if (source_recipe >= r->recipe_count
                || (r->recipes[source_recipe].kind != SOL_MIR_RECIPE_OPTION
                    && r->recipes[source_recipe].kind != SOL_MIR_RECIPE_RESULT)
                || term->success_variant_layout != variant_ordinal(r,
                    source_recipe, term->propagation_kind
                        == SOL_IR_PROPAGATE_OPTION ? 1 : 0)
                || term->residual_variant_layout != variant_ordinal(r,
                    source_recipe, term->propagation_kind
                        == SOL_IR_PROPAGATE_OPTION ? 0 : 1)
                || term->success_field_layout != field_ordinal(r, source_recipe,
                    term->success_variant_layout, 0)) PFAIL("propagate layout");
            ids[edge_count++] = term->normal_edge;
            ids[edge_count++] = term->failure_edge;
        } else if (term->kind == SOL_MIR_PREDICATE_TERM_CHECK_REFINED) {
            if (term->value >= o->predicate_value_count
                || term->result >= o->predicate_value_count
                || term->nested_body >= o->predicate_body_count
                || term->result_recipe >= r->recipe_count
                || r->recipes[term->result_recipe].kind != SOL_MIR_RECIPE_REFINED
                || o->predicate_values[term->result].kind
                    != SOL_MIR_PREDICATE_VALUE_TERMINATOR
                || o->predicate_values[term->result].block != i
                || o->predicate_values[term->result].recipe != term->result_recipe
                || o->predicate_bodies[term->nested_body].refinement_self_recipe
                    != o->predicate_values[term->value].recipe
                || m->contexts[o->predicate_bodies[term->nested_body].context]
                        .refinement_type != term->result_recipe
                || r->recipes[term->result_recipe].backing
                    != o->predicate_values[term->value].recipe)
                PFAIL("refinement");
            ids[edge_count++] = term->normal_edge;
            ids[edge_count++] = term->failure_edge;
        } else if (term->failure_kind < SOL_MIR_PREDICATE_FAILURE_CALL
            || term->failure_kind > SOL_MIR_PREDICATE_FAILURE_NO_MATCH)
            PFAIL("failure");
        for (size_t q = 0; q < edge_count; ++q) {
            if (!tick(1)) PFAIL("work");
            if (ids[q] >= o->predicate_edge_count || edge_seen[ids[q]] != 0
                || o->predicate_edges[ids[q]].source != i) PFAIL("edge source");
            edge_seen[ids[q]] = 1;
        }
    }
    for (size_t i = 0; i < o->predicate_instruction_count; ++i) {
        if (!tick(1)) PFAIL("work");
        if (instruction_seen[i] == 0) PFAIL("instruction coverage");
    }
    size_t edge_values = 0;
    for (size_t i = 0; i < o->predicate_edge_count; ++i) {
        if (!tick(1)) PFAIL("work");
        const SolMirPredicateEdge *edge = &o->predicate_edges[i];
        if (edge_seen[i] == 0 || edge->source >= o->predicate_block_count
            || edge->target >= o->predicate_block_count
            || o->predicate_blocks[edge->source].body
                != o->predicate_blocks[edge->target].body
            || edge->arguments.offset != edge_values
            || edge->arguments.count
                != o->predicate_blocks[edge->target].parameters.count
            || !slice(edge->arguments, o->predicate_edge_value_count))
            PFAIL("edge");
        for (size_t q = 0; q < edge->arguments.count; ++q) {
            if (!tick(1)) PFAIL("work");
            size_t value = o->predicate_edge_values[edge->arguments.offset + q];
            size_t parameter = o->predicate_blocks[edge->target].parameters.offset + q;
            if (value >= o->predicate_value_count
                || o->predicate_values[value].recipe
                    != o->predicate_values[parameter].recipe)
                PFAIL("edge argument");
        }
        if (!add_size(&edge_values, edge->arguments.count))
            PFAIL("edge total");
    }
    if (edge_values != o->predicate_edge_value_count) PFAIL("edge total");
    for (size_t body_id = 0; body_id < o->predicate_body_count; ++body_id) {
        if (!tick(1)) PFAIL("work");
        const SolMirPredicateBody *body = &o->predicate_bodies[body_id];
        memset(reachable + body->blocks.offset, 0, body->blocks.count);
        memset(indegree + body->blocks.offset, 0,
            body->blocks.count * sizeof(*indegree));
        for (size_t edge_id = 0; edge_id < o->predicate_edge_count; ++edge_id) {
            if (!tick(1)) PFAIL("work");
            const SolMirPredicateEdge *edge = &o->predicate_edges[edge_id];
            if (o->predicate_blocks[edge->source].body == body_id)
                ++indegree[edge->target];
        }
        size_t read_at = 0, write_at = 0;
        reachable[body->entry] = 1; queue[write_at++] = body->entry;
        while (read_at < write_at) {
            if (!tick(1)) PFAIL("work");
            size_t source = queue[read_at++];
            for (size_t edge_id = 0; edge_id < o->predicate_edge_count;
                ++edge_id) {
                if (!tick(1)) PFAIL("work");
                const SolMirPredicateEdge *edge = &o->predicate_edges[edge_id];
                if (edge->source == source && !reachable[edge->target]) {
                    reachable[edge->target] = 1; queue[write_at++] = edge->target;
                }
            }
        }
        if (write_at != body->blocks.count) PFAIL("unreachable block");
        read_at = 0; write_at = 0;
        for (size_t block = body->blocks.offset;
            block < body->blocks.offset + body->blocks.count; ++block) {
            if (!tick(1)) PFAIL("work");
            if (indegree[block] == 0) queue[write_at++] = block;
        }
        size_t visited = 0;
        while (read_at < write_at) {
            if (!tick(1)) PFAIL("work");
            size_t source = queue[read_at++]; ++visited;
            for (size_t edge_id = 0; edge_id < o->predicate_edge_count;
                ++edge_id) {
                if (!tick(1)) PFAIL("work");
                const SolMirPredicateEdge *edge = &o->predicate_edges[edge_id];
                if (edge->source == source && --indegree[edge->target] == 0)
                    queue[write_at++] = edge->target;
            }
        }
        if (visited != body->blocks.count) PFAIL("cyclic body");
    }
    for (size_t edge_id = 0; edge_id < o->predicate_edge_count; ++edge_id) {
        if (!tick(1)) PFAIL("work");
        const SolMirPredicateEdge *edge = &o->predicate_edges[edge_id];
        for (size_t q = 0; q < edge->arguments.count; ++q) {
            if (!tick(1)) PFAIL("work");
            if (!predicate_value_available(o, reachable, queue,
                    o->predicate_edge_values[edge->arguments.offset + q],
                    edge->source, SOL_MIR_OPERATION_NONE, true))
                PFAIL("edge argument dominance");
        }
    }
    for (size_t i = 0; i < o->predicate_value_count; ++i) {
        if (!tick(1)) PFAIL("work");
        const SolMirPredicateValue *value = &o->predicate_values[i];
        if (value->recipe >= r->recipe_count
            || value->block >= o->predicate_block_count) PFAIL("value");
        const SolMirPredicateBody *body
            = &o->predicate_bodies[o->predicate_blocks[value->block].body];
        if (i < body->values.offset || i - body->values.offset >= body->values.count)
            PFAIL("value owner");
        if (value->kind == SOL_MIR_PREDICATE_VALUE_INPUT) {
            if (value->definition >= o->predicate_input_count
                || o->predicate_inputs[value->definition].recipe != value->recipe)
                PFAIL("input value");
        } else if (value->kind == SOL_MIR_PREDICATE_VALUE_INSTRUCTION) {
            if (value->definition >= o->predicate_instruction_count
                || o->predicate_instructions[value->definition].result != i)
                PFAIL("instruction value");
        } else if (value->kind == SOL_MIR_PREDICATE_VALUE_BLOCK_PARAMETER) {
            if (value->definition >= o->predicate_blocks[value->block].parameters.count
                || o->predicate_blocks[value->block].parameters.offset
                    + value->definition != i) PFAIL("parameter value");
        } else if (value->kind == SOL_MIR_PREDICATE_VALUE_TERMINATOR) {
            if (value->definition != value->block) PFAIL("terminator value");
        } else PFAIL("value kind");
    }
    for (size_t i = 0; i < o->predicate_instruction_count; ++i) {
        if (!tick(1)) PFAIL("work");
        const SolMirPredicateInstruction *instruction
            = &o->predicate_instructions[i];
        if (instruction->kind < SOL_MIR_PREDICATE_INST_I64
            || instruction->kind > SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT
            || instruction->block >= o->predicate_block_count
            || instruction->result >= o->predicate_value_count
            || instruction->recipe >= r->recipe_count
            || o->predicate_values[instruction->result].definition != i
            || o->predicate_values[instruction->result].block != instruction->block
            || o->predicate_values[instruction->result].recipe != instruction->recipe
            || instruction->bytes.offset > o->literal_byte_count
            || instruction->bytes.count
                > o->literal_byte_count - instruction->bytes.offset) PFAIL("instruction");
        if ((instruction->left != SOL_MIR_OPERATION_NONE
                && instruction->left >= o->predicate_value_count)
            || (instruction->right != SOL_MIR_OPERATION_NONE
                && instruction->right >= o->predicate_value_count)
            || !slice(instruction->operands, o->predicate_operand_count)
            || !slice(instruction->path, o->predicate_path_step_count)
            || !slice(instruction->pattern, o->predicate_pattern_node_count)
            || !predicate_instruction_shape(instruction))
            PFAIL("instruction payload");
        if ((instruction->left != SOL_MIR_OPERATION_NONE
                && !predicate_value_available(o, reachable, queue, instruction->left,
                    instruction->block, i, false))
            || (instruction->right != SOL_MIR_OPERATION_NONE
                && !predicate_value_available(o, reachable, queue, instruction->right,
                    instruction->block, i, false))) PFAIL("instruction dominance");
        if (instruction->kind == SOL_MIR_PREDICATE_INST_I64) {
            if (instruction->left != SOL_MIR_OPERATION_NONE
                || instruction->right != SOL_MIR_OPERATION_NONE
                || instruction->bytes.count != 0 || instruction->operands.count != 0
                || instruction->path.count != 0 || instruction->pattern.count != 0
                || instruction->failures != 0
                || r->recipes[instruction->recipe].kind != SOL_MIR_RECIPE_INT64)
                PFAIL("integer instruction");
        } else if (instruction->kind == SOL_MIR_PREDICATE_INST_BOOL
            || instruction->kind == SOL_MIR_PREDICATE_INST_UNIT
            || instruction->kind == SOL_MIR_PREDICATE_INST_TEXT) {
            if (instruction->left != SOL_MIR_OPERATION_NONE
                || instruction->right != SOL_MIR_OPERATION_NONE
                || (instruction->kind != SOL_MIR_PREDICATE_INST_TEXT
                    && instruction->bytes.count != 0)
                || instruction->operands.count != 0 || instruction->path.count != 0
                || instruction->pattern.count != 0 || instruction->failures != 0
                || r->recipes[instruction->recipe].kind
                    != (instruction->kind == SOL_MIR_PREDICATE_INST_BOOL
                        ? SOL_MIR_RECIPE_BOOL
                        : instruction->kind == SOL_MIR_PREDICATE_INST_TEXT
                        ? SOL_MIR_RECIPE_TEXT : SOL_MIR_RECIPE_UNIT))
                PFAIL("constant instruction");
        } else if (instruction->kind == SOL_MIR_PREDICATE_INST_UNARY) {
            unsigned expected = instruction->opcode == SOL_MIR_OPERATION_I64_NEG
                ? SOL_MIR_OPERATION_FAILURE_OVERFLOW : 0;
            if (instruction->left == SOL_MIR_OPERATION_NONE
                || instruction->right != SOL_MIR_OPERATION_NONE
                || (instruction->opcode != SOL_MIR_OPERATION_BOOL_NOT
                    && instruction->opcode != SOL_MIR_OPERATION_I64_NEG)
                || instruction->failures != expected
                || o->predicate_values[instruction->left].recipe
                    != instruction->recipe
                || r->recipes[instruction->recipe].kind
                    != (instruction->opcode == SOL_MIR_OPERATION_BOOL_NOT
                        ? SOL_MIR_RECIPE_BOOL : SOL_MIR_RECIPE_INT64))
                PFAIL("unary instruction");
        } else if (instruction->kind == SOL_MIR_PREDICATE_INST_BINARY) {
            unsigned expected = instruction->opcode >= SOL_MIR_OPERATION_I64_ADD
                    && instruction->opcode <= SOL_MIR_OPERATION_I64_MUL
                ? SOL_MIR_OPERATION_FAILURE_OVERFLOW
                : instruction->opcode == SOL_MIR_OPERATION_I64_DIV
                    || instruction->opcode == SOL_MIR_OPERATION_I64_REM
                ? SOL_MIR_OPERATION_FAILURE_OVERFLOW
                    | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO : 0;
            if (instruction->left == SOL_MIR_OPERATION_NONE
                || instruction->right == SOL_MIR_OPERATION_NONE
                || instruction->opcode < SOL_MIR_OPERATION_I64_ADD
                || instruction->opcode > SOL_MIR_OPERATION_VALUE_NE
                || instruction->opcode == SOL_MIR_OPERATION_BOOL_AND
                || instruction->opcode == SOL_MIR_OPERATION_BOOL_OR
                || instruction->failures != expected) PFAIL("binary instruction");
            SolMirRecipeId left_recipe
                = o->predicate_values[instruction->left].recipe;
            SolMirRecipeId right_recipe
                = o->predicate_values[instruction->right].recipe;
            if (left_recipe != right_recipe) PFAIL("binary operands");
            if (instruction->opcode <= SOL_MIR_OPERATION_I64_GE) {
                if (r->recipes[left_recipe].kind != SOL_MIR_RECIPE_INT64
                    || r->recipes[instruction->recipe].kind
                        != (instruction->opcode <= SOL_MIR_OPERATION_I64_REM
                            ? SOL_MIR_RECIPE_INT64 : SOL_MIR_RECIPE_BOOL))
                    PFAIL("integer binary recipe");
            } else if (instruction->opcode == SOL_MIR_OPERATION_VALUE_EQ
                || instruction->opcode == SOL_MIR_OPERATION_VALUE_NE) {
                if (r->recipes[instruction->recipe].kind != SOL_MIR_RECIPE_BOOL)
                    PFAIL("equality recipe");
            }
        } else if (instruction->kind == SOL_MIR_PREDICATE_INST_PROJECT) {
            if (instruction->left == SOL_MIR_OPERATION_NONE
                || instruction->right != SOL_MIR_OPERATION_NONE
                || instruction->path.count == 0) PFAIL("project instruction");
        } else if (instruction->kind == SOL_MIR_PREDICATE_INST_FUNCTION
            || instruction->kind == SOL_MIR_PREDICATE_INST_BOUND_OPERATION) {
            if (instruction->binding >= m->binding_count
                || (instruction->kind == SOL_MIR_PREDICATE_INST_FUNCTION
                    ? instruction->left != SOL_MIR_OPERATION_NONE
                    : instruction->left == SOL_MIR_OPERATION_NONE)
                || r->recipes[instruction->recipe].kind
                    != SOL_MIR_RECIPE_FUNCTION)
                PFAIL("function instruction");
            const SolMirMaterializedBinding *binding
                = &m->bindings[instruction->binding];
            SolMirPlanSlice parameters, accesses;
            SolMirRecipeId receiver, result;
            SolMirMaterializedEffectRowId effects;
            if (binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
                if (binding->instance >= m->image_count) PFAIL("function target");
                const SolMirMaterializedImage *target = &m->images[binding->instance];
                parameters = target->parameter_types;
                accesses = target->parameter_accesses; receiver = target->receiver;
                result = target->result; effects = target->effects;
            } else {
                if (binding->target_kind != SOL_MIR_MATERIALIZED_TARGET_IMPORT
                    || binding->import >= m->import_count) PFAIL("function target");
                const SolMirMaterializedImport *target = &m->imports[binding->import];
                parameters = target->parameter_types;
                accesses = target->parameter_accesses; receiver = target->receiver;
                result = target->result; effects = target->effects;
            }
            const SolMirRecipe *function = &r->recipes[instruction->recipe];
            if (function->parameters.count != parameters.count
                || function->parameter_accesses.count != accesses.count
                || function->result != result || function->effects != effects
                || (instruction->kind == SOL_MIR_PREDICATE_INST_BOUND_OPERATION
                    && o->predicate_values[instruction->left].recipe != receiver))
                PFAIL("function signature");
            for (size_t q = 0; q < parameters.count; ++q) {
                if (!tick(1)) PFAIL("work");
                if (r->recipe_ids[function->parameters.offset + q]
                        != m->type_ids[parameters.offset + q]
                    || r->accesses[function->parameter_accesses.offset + q]
                        != m->accesses[accesses.offset + q])
                    PFAIL("function parameter");
            }
        } else if (instruction->kind == SOL_MIR_PREDICATE_INST_CONSTRUCT) {
            if (instruction->construct_kind < SOL_MIR_PREDICATE_CONSTRUCT_RECORD
                || instruction->construct_kind > SOL_MIR_PREDICATE_CONSTRUCT_WRAPPER
                || (instruction->construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_SUM
                    ? instruction->variant_layout >= r->variant_count
                    : instruction->variant_layout != SOL_MIR_OPERATION_NONE))
                PFAIL("construct instruction");
            SolMirRecipeKind recipe_kind = r->recipes[instruction->recipe].kind;
            if ((instruction->construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_RECORD
                    && recipe_kind != SOL_MIR_RECIPE_RECORD)
                || (instruction->construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_TUPLE
                    && recipe_kind != SOL_MIR_RECIPE_TUPLE)
                || (instruction->construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_SUM
                    && recipe_kind != SOL_MIR_RECIPE_ENUM
                    && recipe_kind != SOL_MIR_RECIPE_OPTION
                    && recipe_kind != SOL_MIR_RECIPE_RESULT)
                || (instruction->construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_WRAPPER
                    && recipe_kind != SOL_MIR_RECIPE_DISTINCT))
                PFAIL("construct recipe");
            if (instruction->construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_SUM
                && instruction->semantic_tag
                    != o->layout->variants[instruction->variant_layout].tag)
                PFAIL("construct tag");
            if (instruction->construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_WRAPPER
                && instruction->operands.count != 1)
                PFAIL("wrapper construct");
            SolMirPlanSlice expected_fields = {0, 0};
            if (instruction->construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_RECORD
                || instruction->construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_TUPLE)
                expected_fields = r->recipes[instruction->recipe].fields;
            else if (instruction->construct_kind
                    == SOL_MIR_PREDICATE_CONSTRUCT_SUM)
                expected_fields = r->variants[instruction->variant_layout].fields;
            if (instruction->construct_kind != SOL_MIR_PREDICATE_CONSTRUCT_WRAPPER
                && instruction->operands.count != expected_fields.count)
                PFAIL("construct completeness");
            for (size_t q = 0; q < instruction->operands.count; ++q) {
                if (!tick(1)) PFAIL("work");
                const SolMirPredicateOperand *operand
                    = &o->predicate_operands[instruction->operands.offset + q];
                if (operand->value >= o->predicate_value_count)
                    PFAIL("construct operand");
                if (instruction->construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_WRAPPER) {
                    if (operand->field_layout != SOL_MIR_OPERATION_NONE
                        || r->recipes[instruction->recipe].backing
                            != o->predicate_values[operand->value].recipe)
                        PFAIL("wrapper field");
                } else if (operand->field_layout >= r->field_count
                    || r->fields[operand->field_layout].type
                        != o->predicate_values[operand->value].recipe
                    || operand->field_layout < expected_fields.offset
                    || operand->field_layout - expected_fields.offset
                        >= expected_fields.count) {
                    PFAIL("construct field");
                }
                for (size_t prior = 0; prior < q; ++prior) {
                    if (!tick(1)) PFAIL("work");
                    if (o->predicate_operands[instruction->operands.offset + prior]
                            .field_layout == operand->field_layout)
                        PFAIL("duplicate construct field");
                }
            }
        } else if (instruction->kind == SOL_MIR_PREDICATE_INST_PATTERN_TEST) {
            if (instruction->left == SOL_MIR_OPERATION_NONE
                || instruction->pattern.count == 0
                || r->recipes[instruction->recipe].kind != SOL_MIR_RECIPE_BOOL)
                PFAIL("pattern test instruction");
        } else if (instruction->left == SOL_MIR_OPERATION_NONE)
            PFAIL("pattern extraction instruction");
        if (instruction->path.count != 0) {
            SolMirRecipeId current = o->predicate_values[instruction->left].recipe;
            for (size_t q = 0; q < instruction->path.count; ++q) {
                if (!tick(1)) PFAIL("work");
                const SolMirPredicatePathStep *step
                    = &o->predicate_path_steps[instruction->path.offset + q];
                if (step->base_recipe != current) PFAIL("path chain");
                current = step->result_recipe;
            }
            if (current != instruction->recipe) PFAIL("path result");
        }
        if (instruction->kind == SOL_MIR_PREDICATE_INST_PATTERN_TEST) {
            SolMirRecipeId root = o->predicate_values[instruction->left].recipe;
            for (size_t q = 0; q < instruction->pattern.count; ++q) {
                if (!tick(1)) PFAIL("work");
                const SolMirPredicatePatternNode *node
                    = &o->predicate_pattern_nodes[instruction->pattern.offset + q];
                if (!slice(node->path, o->predicate_path_step_count))
                    PFAIL("pattern path");
                SolMirRecipeId current = root;
                for (size_t p = 0; p < node->path.count; ++p) {
                    if (!tick(1)) PFAIL("work");
                    const SolMirPredicatePathStep *step
                        = &o->predicate_path_steps[node->path.offset + p];
                    if (step->base_recipe != current) PFAIL("pattern path chain");
                    current = step->result_recipe;
                }
                if (current != node->recipe) PFAIL("pattern path result");
            }
        }
        for (size_t q = 0; q < instruction->operands.count; ++q) {
            if (!tick(1)) PFAIL("work");
            size_t operand = instruction->operands.offset + q;
            if (operand_seen[operand] != 0) PFAIL("operand owner");
            if (!predicate_value_available(o, reachable, queue,
                    o->predicate_operands[operand].value, instruction->block, i,
                    false)) PFAIL("operand dominance");
            operand_seen[operand] = 1;
        }
        for (size_t q = 0; q < instruction->path.count; ++q) {
            if (!tick(1)) PFAIL("work");
            size_t path = instruction->path.offset + q;
            if (path_seen[path] != 0) PFAIL("path owner");
            path_seen[path] = 1;
        }
        for (size_t q = 0; q < instruction->pattern.count; ++q) {
            if (!tick(1)) PFAIL("work");
            size_t pattern = instruction->pattern.offset + q;
            if (pattern_seen[pattern] != 0) PFAIL("pattern owner");
            pattern_seen[pattern] = 1;
        }
    }
    for (size_t i = 0; i < o->predicate_operand_count; ++i) {
        if (!tick(1)) PFAIL("work");
        if (o->predicate_operands[i].value >= o->predicate_value_count
            || o->predicate_operands[i].access < SOL_ACCESS_OWNED
            || o->predicate_operands[i].access > SOL_ACCESS_SHARED)
            PFAIL("operand");
    }
    for (size_t i = 0; i < o->predicate_path_step_count; ++i) {
        if (!tick(1)) PFAIL("work");
        const SolMirPredicatePathStep *step = &o->predicate_path_steps[i];
        if (step->base_recipe >= r->recipe_count
            || step->result_recipe >= r->recipe_count
            || step->field_layout >= r->field_count
            || r->fields[step->field_layout].type != step->result_recipe)
            PFAIL("path");
        size_t matches = 0;
        SolMirPlanSlice fields = r->recipes[step->base_recipe].fields;
        for (size_t q = 0; q < fields.count; ++q) {
            if (!tick(1)) PFAIL("work");
            matches += fields.offset + q == step->field_layout;
        }
        SolMirPlanSlice variants = r->recipes[step->base_recipe].variants;
        for (size_t v = 0; v < variants.count; ++v) {
            if (!tick(1)) PFAIL("work");
            fields = r->variants[variants.offset + v].fields;
            for (size_t q = 0; q < fields.count; ++q) {
                if (!tick(1)) PFAIL("work");
                matches += fields.offset + q == step->field_layout;
            }
        }
        if (matches != 1) PFAIL("path field");
    }
    for (size_t i = 0; i < o->predicate_pattern_node_count; ++i) {
        if (!tick(1)) PFAIL("work");
        const SolMirPredicatePatternNode *node = &o->predicate_pattern_nodes[i];
        if (node->kind < SOL_MIR_OPERATION_PATTERN_WILDCARD
            || node->kind > SOL_MIR_OPERATION_PATTERN_PRODUCT
            || node->recipe >= r->recipe_count
            || !slice(node->path, o->predicate_path_step_count)) PFAIL("pattern");
        if ((node->kind == SOL_MIR_OPERATION_PATTERN_BOOL
                && r->recipes[node->recipe].kind != SOL_MIR_RECIPE_BOOL)
            || (node->kind != SOL_MIR_OPERATION_PATTERN_SUM_TAG
                && node->semantic_tag != 0)) PFAIL("pattern semantics");
        if (node->kind == SOL_MIR_OPERATION_PATTERN_SUM_TAG) {
            size_t matches = 0;
            SolMirPlanSlice variants = r->recipes[node->recipe].variants;
            for (size_t v = 0; v < variants.count; ++v) {
                if (!tick(1)) PFAIL("work");
                matches += o->layout->variants[variants.offset + v].tag
                    == node->semantic_tag;
            }
            if (matches != 1) PFAIL("pattern tag");
        }
        for (size_t q = 0; q < node->path.count; ++q) {
            if (!tick(1)) PFAIL("work");
            size_t path = node->path.offset + q;
            if (path_seen[path] != 0) PFAIL("pattern path owner");
            path_seen[path] = 1;
        }
    }
    for (size_t i = 0; i < o->predicate_operand_count; ++i) {
        if (!tick(1)) PFAIL("work");
        if (operand_seen[i] == 0) PFAIL("operand coverage");
    }
    for (size_t i = 0; i < o->predicate_path_step_count; ++i) {
        if (!tick(1)) PFAIL("work");
        if (path_seen[i] == 0) PFAIL("path coverage");
    }
    for (size_t i = 0; i < o->predicate_pattern_node_count; ++i) {
        if (!tick(1)) PFAIL("work");
        if (pattern_seen[i] == 0) PFAIL("pattern coverage");
    }
    for (size_t block_id = 0; block_id < o->predicate_block_count; ++block_id) {
        if (!tick(1)) PFAIL("work");
        const SolMirPredicateTerminator *term
            = &o->predicate_blocks[block_id].terminator;
#define AVAILABLE(value) ((value) == SOL_MIR_OPERATION_NONE || \
    predicate_value_available(o, reachable, queue, (value), block_id, \
        SOL_MIR_OPERATION_NONE, false))
        if ((term->kind == SOL_MIR_PREDICATE_TERM_RETURN && !AVAILABLE(term->value))
            || (term->kind == SOL_MIR_PREDICATE_TERM_BRANCH
                && !AVAILABLE(term->condition))
            || (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE
                && (!AVAILABLE(term->callee) || !AVAILABLE(term->receiver)))
            || ((term->kind == SOL_MIR_PREDICATE_TERM_PROPAGATE
                    || term->kind == SOL_MIR_PREDICATE_TERM_CHECK_REFINED)
                && !AVAILABLE(term->value))) PFAIL("terminator dominance");
        if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE)
            for (size_t q = 0; q < term->arguments.count; ++q) {
                if (!tick(1)) PFAIL("work");
                if (!AVAILABLE(o->predicate_operands[
                        term->arguments.offset + q].value))
                    PFAIL("argument dominance");
            }
#undef AVAILABLE
    }
    if (o->import_envelope_count != m->import_count) PFAIL("envelope total");
    size_t references = 0, snapshots = 0;
    for (size_t i = 0; i < o->import_envelope_count; ++i) {
        if (!tick(1)) PFAIL("work");
        const SolMirImportContractEnvelope *e = &o->import_envelopes[i];
        const SolMirMaterializedImport *source = &m->imports[i];
        if (!slice(e->requires, o->import_contract_reference_count)
            || !slice(e->ensures, o->import_contract_reference_count)
            || !slice(e->snapshots, o->import_snapshot_count))
            PFAIL("envelope slices");
        size_t ensures_offset = references;
        size_t next_references = references;
        size_t next_snapshots = snapshots;
        if (!add_size(&ensures_offset, e->requires.count)
            || !add_size(&next_references, e->requires.count)
            || !add_size(&next_references, e->ensures.count)
            || !add_size(&next_snapshots, e->snapshots.count))
            PFAIL("envelope totals");
        if (e->import != i || e->receiver != source->receiver
            || e->receiver_access != source->receiver_access
            || e->parameters.offset != source->parameter_types.offset
            || e->parameters.count != source->parameter_types.count
            || e->parameter_accesses.offset != source->parameter_accesses.offset
            || e->parameter_accesses.count != source->parameter_accesses.count
            || e->result != source->result || e->effects != source->effects
            || !e->host_invoke
            || e->requires.offset != references || e->snapshots.offset != snapshots
            || e->ensures.offset != ensures_offset
            || next_references > o->import_contract_reference_count
            || next_snapshots > o->import_snapshot_count) PFAIL("envelope");
        for (size_t q = 0; q < next_references - references; ++q) {
            size_t body = o->import_contract_references[references + q];
            if (body >= o->predicate_body_count
                || o->predicate_bodies[body].owner_kind
                    != SOL_MIR_PREDICATE_OWNER_IMPORT
                || o->predicate_bodies[body].import != i) PFAIL("reference");
        }
        references = next_references;
        size_t capture_at = 0;
        const SolIr *ir = m->plan->program->ir;
        for (size_t c = 0; c < source->contexts.count; ++c) {
            size_t context = source->contexts.offset + c;
            if (m->contexts[context].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT)
                continue;
            size_t obligation_id = m->contexts[context].obligation;
            if (obligation_id >= ir->obligation_count) PFAIL("snapshot obligation");
            const SolIrObligation *obligation = &ir->obligations[obligation_id];
            for (size_t q = 0; q < obligation->snapshots.count; ++q) {
                if (capture_at >= e->snapshots.count) PFAIL("snapshot count");
                size_t source_snapshot = obligation->snapshots.offset + q;
                const SolIrSnapshot *snapshot = &ir->snapshots[source_snapshot];
                const SolMirImportSnapshotCapture *capture
                    = &o->import_snapshots[snapshots + capture_at];
                if (snapshot->operand >= ir->expression_count
                    || ir->expressions[snapshot->operand].kind
                        != SOL_IR_EXPR_PLACE
                    || ir->expressions[snapshot->operand].as.place
                        >= ir->place_count) PFAIL("snapshot operand");
                const SolIrPlace *place = &ir->places[
                    ir->expressions[snapshot->operand].as.place];
                const SolIrCallable *callable
                    = &ir->callables[source->source_callable];
                SolMirPredicateInputKind input_kind;
                size_t ordinal = 0; SolAccessMode access = SOL_ACCESS_OWNED;
                if (place->root_kind != SOL_IR_PLACE_ROOT_LOCAL
                    || place->projections.count != 0) PFAIL("snapshot place");
                if (place->local == callable->receiver) {
                    input_kind = SOL_MIR_PREDICATE_INPUT_RECEIVER;
                    access = callable->receiver_access;
                } else {
                    input_kind = SOL_MIR_PREDICATE_INPUT_PARAMETER;
                    bool found = false;
                    for (size_t p = 0; p < callable->parameters.count; ++p)
                        if (ir->roots[callable->parameters.offset + p]
                                == place->local) {
                            ordinal = p; access = ir->locals[place->local].access;
                            found = true; break;
                        }
                    if (!found) PFAIL("snapshot local");
                }
                SolMirRecipeId recipe = SOL_MIR_RECIPE_NONE;
                for (size_t u = 0; u < source->overlays.count; ++u) {
                    const SolMirMaterializedTypeOverlay *overlay
                        = &m->overlays[source->overlays.offset + u];
                    if (overlay->kind == SOL_MIR_PLAN_USE_EXPRESSION
                        && overlay->context == context
                        && overlay->source == snapshot->operand)
                        recipe = overlay->type;
                }
                size_t provenance = provenance_find(o,
                    SOL_MIR_OPERATION_PROVENANCE_IMPORT_SNAPSHOT,
                    snapshots + capture_at);
                if (capture->import != i || capture->context != context
                    || capture->slot != capture_at
                    || capture->input_kind != input_kind
                    || capture->ordinal != ordinal || capture->access != access
                    || capture->recipe != recipe || recipe >= r->recipe_count
                    || provenance == SOL_MIR_OPERATION_NONE
                    || capture->provenance != provenance
                    || o->provenance[provenance].source_snapshot != source_snapshot
                    || o->provenance[provenance].source_obligation != obligation_id
                    || o->provenance[provenance].source_expression != snapshot->operand)
                    PFAIL("import snapshot");
                ++capture_at;
            }
        }
        if (capture_at != e->snapshots.count) PFAIL("snapshot coverage");
        snapshots = next_snapshots;
    }
    if (references != o->import_contract_reference_count
        || snapshots != o->import_snapshot_count) PFAIL("reference total");
    free(instruction_seen); free(value_seen); free(edge_seen); free(reachable);
    free(operand_seen); free(path_seen); free(pattern_seen);
    free(indegree); free(queue);
#undef PFAIL
    return true;
predicate_failed:
    free(instruction_seen); free(value_seen); free(edge_seen); free(reachable);
    free(operand_seen); free(path_seen); free(pattern_seen);
    free(indegree); free(queue);
#undef PFAIL
    return false;
}

typedef struct {
    const SolMirOperations *operations;
    const SolIr *ir;
    const SolMirPredicateBody *body;
    size_t block;
    size_t input;
    size_t value;
    size_t instruction;
    size_t literal;
    size_t snapshot_base;
} PredicateAuthentication;

static SolMirRecipeId authenticated_expression_recipe(
    const SolMirOperations *o, const SolMirPredicateBody *body,
    size_t expression) {
    const SolMirMaterialization *m = o->layout->representation->materialization;
    SolMirPlanSlice overlays = body->owner_kind == SOL_MIR_PREDICATE_OWNER_IMPORT
        ? m->imports[body->import].overlays : m->images[body->instance].overlays;
    for (size_t i = 0; i < overlays.count; ++i) {
        if (!tick(1)) return SOL_MIR_RECIPE_NONE;
        const SolMirMaterializedTypeOverlay *use
            = &m->overlays[overlays.offset + i];
        if (use->kind == SOL_MIR_PLAN_USE_EXPRESSION
            && use->context == body->context && use->source == expression)
            return use->type;
    }
    return SOL_MIR_RECIPE_NONE;
}

static bool authenticate_predicate_expression(PredicateAuthentication *a,
    size_t expression, size_t depth, SolMirPredicateValueId *result) {
    if (!tick(SOL_MIR_OPERATIONS_WORK_RECURSE)
        || expression >= a->ir->expression_count
        || depth > a->ir->expression_count) return false;
    const SolIrExpression *source = &a->ir->expressions[expression];
    SolMirRecipeId recipe = authenticated_expression_recipe(a->operations,
        a->body, expression);
    if (recipe >= a->operations->layout->representation->recipe_count
        || a->value >= a->body->values.offset + a->body->values.count)
        return false;
    if (source->kind == SOL_IR_EXPR_PLACE
        || source->kind == SOL_IR_EXPR_RESULT
        || source->kind == SOL_IR_EXPR_SNAPSHOT_READ
        || source->kind == SOL_IR_EXPR_REFINEMENT_SELF) {
        if (a->input >= a->body->inputs.offset + a->body->inputs.count)
            return false;
        SolMirPredicateInputKind kind;
        size_t ordinal = 0; SolAccessMode access = SOL_ACCESS_OWNED;
        const SolMirMaterialization *m
            = a->operations->layout->representation->materialization;
        SolIrCallableId callable_id = a->body->owner_kind
                == SOL_MIR_PREDICATE_OWNER_IMPORT
            ? m->imports[a->body->import].source_callable
            : m->images[a->body->instance].source_callable;
        const SolIrCallable *callable = &a->ir->callables[callable_id];
        if (source->kind == SOL_IR_EXPR_RESULT) {
            kind = a->body->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                ? SOL_MIR_PREDICATE_INPUT_SUCCESS_RESULT
                : SOL_MIR_PREDICATE_INPUT_COMPLETE_RESULT;
        } else if (source->kind == SOL_IR_EXPR_SNAPSHOT_READ) {
            kind = SOL_MIR_PREDICATE_INPUT_SNAPSHOT;
            const SolIrObligation *obligation
                = &a->ir->obligations[m->contexts[a->body->context].obligation];
            bool found = false;
            for (size_t i = 0; i < obligation->snapshots.count; ++i)
                if (obligation->snapshots.offset + i == source->as.snapshot) {
                    if (i > SIZE_MAX - a->snapshot_base) return false;
                    ordinal = a->snapshot_base + i; found = true; break;
                }
            if (!found) return false;
            const SolIrSnapshot *snapshot = &a->ir->snapshots[source->as.snapshot];
            if (snapshot->operand >= a->ir->expression_count
                || a->ir->expressions[snapshot->operand].kind
                    != SOL_IR_EXPR_PLACE
                || a->ir->expressions[snapshot->operand].as.place
                    >= a->ir->place_count) return false;
            const SolIrPlace *place = &a->ir->places[
                a->ir->expressions[snapshot->operand].as.place];
            if (place->root_kind != SOL_IR_PLACE_ROOT_LOCAL
                || place->projections.count != 0) return false;
        } else if (source->kind == SOL_IR_EXPR_REFINEMENT_SELF) {
            kind = SOL_MIR_PREDICATE_INPUT_REFINEMENT_SELF;
        } else {
            if (source->as.place >= a->ir->place_count) return false;
            const SolIrPlace *place = &a->ir->places[source->as.place];
            if (place->root_kind != SOL_IR_PLACE_ROOT_LOCAL
                || place->projections.count != 0) return false;
            if (place->local == callable->receiver) {
                kind = SOL_MIR_PREDICATE_INPUT_RECEIVER;
                access = callable->receiver_access;
            } else if (place->local == callable->capability_source) {
                return false;
            } else {
                kind = SOL_MIR_PREDICATE_INPUT_PARAMETER;
                bool found = false;
                for (size_t i = 0; i < callable->parameters.count; ++i)
                    if (a->ir->roots[callable->parameters.offset + i]
                            == place->local) {
                        ordinal = i; access = a->ir->locals[place->local].access;
                        found = true; break;
                    }
                if (!found) return false;
            }
        }
        const SolMirPredicateInput *input
            = &a->operations->predicate_inputs[a->input];
        const SolMirPredicateValue *value
            = &a->operations->predicate_values[a->value];
        if (input->kind != kind || input->ordinal != ordinal
            || input->recipe != recipe || input->access != access
            || value->kind != SOL_MIR_PREDICATE_VALUE_INPUT
            || value->recipe != recipe || value->block != a->block
            || value->definition != a->input) return false;
        *result = a->value; ++a->input; ++a->value; return true;
    }
    SolMirPredicateValueId left = SOL_MIR_OPERATION_NONE;
    SolMirPredicateValueId right = SOL_MIR_OPERATION_NONE;
    SolMirPredicateInstructionKind kind;
    SolMirOperationOpcode opcode_value = (SolMirOperationOpcode)0;
    unsigned failures = 0;
    int64_t integer = 0; bool boolean = false;
    SolMirPlanSlice bytes = {0, 0};
    if (source->kind == SOL_IR_EXPR_INTEGER) {
        kind = SOL_MIR_PREDICATE_INST_I64; integer = source->as.integer;
    } else if (source->kind == SOL_IR_EXPR_BOOL) {
        kind = SOL_MIR_PREDICATE_INST_BOOL; boolean = source->as.boolean;
    } else if (source->kind == SOL_IR_EXPR_STRING) {
        kind = SOL_MIR_PREDICATE_INST_TEXT;
        size_t length = strlen(source->as.string);
        bytes = (SolMirPlanSlice){a->literal, length};
        if (a->literal > a->operations->literal_byte_count
            || length > a->operations->literal_byte_count - a->literal)
            return false;
        for (size_t i = 0; i < length; ++i)
            if (a->operations->literal_bytes[a->literal + i]
                    != source->as.string[i]) return false;
        a->literal += length;
    } else if (source->kind == SOL_IR_EXPR_UNIT) {
        kind = SOL_MIR_PREDICATE_INST_UNIT;
    } else if (source->kind == SOL_IR_EXPR_UNARY) {
        kind = SOL_MIR_PREDICATE_INST_UNARY;
        if (!authenticate_predicate_expression(a, source->as.unary.operand,
                depth + 1, &left)
            || !expected_opcode(SOL_MIR_INST_UNARY,
                source->as.unary.operator_kind, &opcode_value, &failures)) return false;
    } else if (source->kind == SOL_IR_EXPR_BINARY) {
        if (source->as.binary.operator_kind == SOL_TOKEN_AMP_AMP
            || source->as.binary.operator_kind == SOL_TOKEN_PIPE_PIPE) return false;
        kind = SOL_MIR_PREDICATE_INST_BINARY;
        if (!authenticate_predicate_expression(a, source->as.binary.left,
                depth + 1, &left)
            || !authenticate_predicate_expression(a, source->as.binary.right,
                depth + 1, &right)
            || !expected_opcode(SOL_MIR_INST_BINARY,
                source->as.binary.operator_kind, &opcode_value, &failures)) return false;
    } else return false;
    if (a->instruction >= a->operations->predicate_instruction_count
        || a->value >= a->body->values.offset + a->body->values.count)
        return false;
    const SolMirPredicateInstruction *instruction
        = &a->operations->predicate_instructions[a->instruction];
    const SolMirPredicateValue *value
        = &a->operations->predicate_values[a->value];
    if (instruction->kind != kind || instruction->block != a->block
        || instruction->result != a->value || instruction->recipe != recipe
        || instruction->opcode != opcode_value || instruction->left != left
        || instruction->right != right || instruction->integer != integer
        || instruction->boolean != boolean
        || instruction->bytes.offset != bytes.offset
        || instruction->bytes.count != bytes.count
        || instruction->failures != failures
        || value->kind != SOL_MIR_PREDICATE_VALUE_INSTRUCTION
        || value->recipe != recipe || value->block != a->block
        || value->definition != a->instruction) return false;
    *result = a->value; ++a->instruction; ++a->value; return true;
}

static bool authenticate_predicate_body(const SolMirOperations *o,
    size_t body_id, size_t obligation_id, size_t *literal) {
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    if (body_id >= o->predicate_body_count
        || obligation_id >= ir->obligation_count) return false;
    const SolMirPredicateBody *body = &o->predicate_bodies[body_id];
    const SolIrObligation *obligation = &ir->obligations[obligation_id];
    bool applicable = (obligation->kind == SOL_CONTRACT_REQUIRES
            && obligation->outcome == SOL_CONTRACT_OUTCOME_ALWAYS
            && !obligation->result_available)
        || (obligation->kind == SOL_CONTRACT_ENSURES
            && ((obligation->outcome == SOL_CONTRACT_OUTCOME_ALWAYS
                    && obligation->result_available)
                || obligation->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                || (obligation->outcome == SOL_CONTRACT_OUTCOME_FAILURE
                    && !obligation->result_available)));
    SolMirRecipeId owner_result = body->owner_kind
            == SOL_MIR_PREDICATE_OWNER_IMPORT
        ? m->imports[body->import].result : m->images[body->instance].result;
    if ((obligation->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
            || obligation->outcome == SOL_CONTRACT_OUTCOME_FAILURE)
        && (owner_result >= r->recipe_count
            || r->recipes[owner_result].kind != SOL_MIR_RECIPE_RESULT))
        applicable = false;
    if (body->context >= m->context_count
        || m->contexts[body->context].obligation != obligation_id
        || !applicable || body->phase != obligation->kind
        || body->outcome != obligation->outcome
        || body->blocks.count != 1 || body->entry != body->blocks.offset
        || body->output_recipe >= r->recipe_count
        || r->recipes[body->output_recipe].kind != SOL_MIR_RECIPE_BOOL)
        return false;
    const SolMirPredicateBlock *block = &o->predicate_blocks[body->entry];
    SolMirPlanSlice owner_contexts = body->owner_kind
            == SOL_MIR_PREDICATE_OWNER_IMPORT
        ? m->imports[body->import].contexts : m->images[body->instance].contexts;
    size_t snapshot_base = 0;
    for (size_t i = 0; i < owner_contexts.count; ++i) {
        size_t context = owner_contexts.offset + i;
        if (context == body->context) break;
        if (m->contexts[context].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT) continue;
        size_t prior = m->contexts[context].obligation;
        if (prior >= ir->obligation_count
            || ir->obligations[prior].snapshots.count
                > SIZE_MAX - snapshot_base) return false;
        snapshot_base += ir->obligations[prior].snapshots.count;
    }
    PredicateAuthentication a = {o, ir, body, body->entry,
        body->inputs.offset, body->values.offset, block->instructions.offset,
        *literal, snapshot_base};
    SolMirPredicateValueId result;
    if (!authenticate_predicate_expression(&a, obligation->predicate, 0, &result)
        || a.input != body->inputs.offset + body->inputs.count
        || a.value != body->values.offset + body->values.count
        || a.instruction != block->instructions.offset + block->instructions.count
        || block->terminator.kind != SOL_MIR_PREDICATE_TERM_RETURN
        || block->terminator.value != result) return false;
    *literal = a.literal; return true;
}

static bool scalar_predicate_expression(const SolIr *ir, size_t expression,
    size_t depth) {
    if (expression >= ir->expression_count || depth > ir->expression_count)
        return false;
    const SolIrExpression *source = &ir->expressions[expression];
    if (source->kind == SOL_IR_EXPR_INTEGER || source->kind == SOL_IR_EXPR_BOOL
        || source->kind == SOL_IR_EXPR_STRING || source->kind == SOL_IR_EXPR_UNIT)
        return true;
    if (source->kind == SOL_IR_EXPR_UNARY)
        return scalar_predicate_expression(ir, source->as.unary.operand,
            depth + 1);
    return source->kind == SOL_IR_EXPR_BINARY
        && source->as.binary.operator_kind != SOL_TOKEN_AMP_AMP
        && source->as.binary.operator_kind != SOL_TOKEN_PIPE_PIPE
        && scalar_predicate_expression(ir, source->as.binary.left, depth + 1)
        && scalar_predicate_expression(ir, source->as.binary.right, depth + 1);
}

typedef struct {
    size_t block, instruction, edge, edge_value, operand, path, pattern, literal;
} PredicateAuthenticationCursor;

typedef struct {
    const SolMirOperations *operations;
    const SolMirRepresentation *representation;
    const SolMirMaterialization *materialization;
    const SolIr *ir;
    const SolMirPredicateBody *body;
    size_t body_id, image, import, context, obligation, block;
    size_t snapshot_base, input, value;
    PredicateAuthenticationCursor *cursor;
    SolMirPredicateValueId *locals;
    unsigned char *local_bound;
    SolMirPredicatePathStep *path_stack;
} RichPredicateAuthentication;

static SolMirPredicateInstruction authenticated_instruction_init(
    SolMirPredicateInstructionKind kind, SolMirRecipeId recipe) {
    SolMirPredicateInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.kind = kind; instruction.recipe = recipe;
    instruction.left = instruction.right = SOL_MIR_OPERATION_NONE;
    instruction.binding = SOL_MIR_MATERIALIZED_NONE;
    instruction.variant_layout = SOL_MIR_OPERATION_NONE;
    return instruction;
}

static SolMirPredicateTerminator authenticated_terminator(
    SolMirPredicateTerminatorKind kind) {
    SolMirPredicateTerminator term;
    memset(&term, 0, sizeof(term));
    term.kind = kind;
    term.value = term.condition = term.callee = term.receiver = term.result
        = SOL_MIR_OPERATION_NONE;
    term.binding = SOL_MIR_MATERIALIZED_NONE;
    term.effects = SOL_MIR_MATERIALIZED_NONE;
    term.edge = term.true_edge = term.false_edge = term.normal_edge
        = term.failure_edge = SOL_MIR_OPERATION_NONE;
    term.success_variant_layout = term.residual_variant_layout
        = term.success_field_layout = SOL_MIR_OPERATION_NONE;
    term.nested_body = SOL_MIR_OPERATION_NONE;
    term.result_recipe = SOL_MIR_RECIPE_NONE;
    return term;
}

static bool authenticated_instruction_equal(
    const SolMirPredicateInstruction *a,
    const SolMirPredicateInstruction *b) {
    return a->kind == b->kind && a->block == b->block
        && a->result == b->result && a->recipe == b->recipe
        && a->opcode == b->opcode && a->left == b->left
        && a->right == b->right && a->integer == b->integer
        && a->boolean == b->boolean && a->bytes.offset == b->bytes.offset
        && a->bytes.count == b->bytes.count && a->failures == b->failures
        && a->operands.offset == b->operands.offset
        && a->operands.count == b->operands.count
        && a->path.offset == b->path.offset && a->path.count == b->path.count
        && a->pattern.offset == b->pattern.offset
        && a->pattern.count == b->pattern.count
        && a->construct_kind == b->construct_kind
        && a->variant_layout == b->variant_layout
        && a->semantic_tag == b->semantic_tag && a->binding == b->binding;
}

static bool authenticated_terminator_equal(const SolMirPredicateTerminator *a,
    const SolMirPredicateTerminator *b) {
    return a->kind == b->kind && a->value == b->value
        && a->condition == b->condition && a->callee == b->callee
        && a->receiver == b->receiver && a->receiver_access == b->receiver_access
        && a->arguments.offset == b->arguments.offset
        && a->arguments.count == b->arguments.count
        && a->call_kind == b->call_kind && a->binding == b->binding
        && a->effects == b->effects && a->result == b->result
        && a->edge == b->edge && a->true_edge == b->true_edge
        && a->false_edge == b->false_edge && a->normal_edge == b->normal_edge
        && a->failure_edge == b->failure_edge
        && a->propagation_kind == b->propagation_kind
        && a->success_variant_layout == b->success_variant_layout
        && a->residual_variant_layout == b->residual_variant_layout
        && a->success_field_layout == b->success_field_layout
        && a->nested_body == b->nested_body
        && a->result_recipe == b->result_recipe
        && a->failure_kind == b->failure_kind;
}

static SolMirRecipeId authenticated_use_recipe(RichPredicateAuthentication *a,
    SolMirPlanTypedUseKind kind, size_t source, size_t ordinal) {
    SolMirPlanSlice overlays = a->import != SOL_MIR_OPERATION_NONE
        ? a->materialization->imports[a->import].overlays
        : a->materialization->images[a->image].overlays;
    SolMirRecipeId found = SOL_MIR_RECIPE_NONE;
    for (size_t i = 0; i < overlays.count; ++i) {
        if (!tick(1)) return SOL_MIR_RECIPE_NONE;
        const SolMirMaterializedTypeOverlay *use
            = &a->materialization->overlays[overlays.offset + i];
        if (use->kind == kind && use->context == a->context
            && use->source == source && use->ordinal == ordinal) {
            if (found != SOL_MIR_RECIPE_NONE) return SOL_MIR_RECIPE_NONE;
            found = use->type;
        }
    }
    return found;
}

static bool authenticated_value(RichPredicateAuthentication *a,
    SolMirPredicateValueKind kind, SolMirRecipeId recipe, size_t block,
    size_t definition, SolMirPredicateValueId *result) {
    if (a->value >= a->operations->predicate_value_count
        || a->value >= a->body->values.offset + a->body->values.count)
        return false;
    const SolMirPredicateValue *value
        = &a->operations->predicate_values[a->value];
    if (value->kind != kind || value->recipe != recipe || value->block != block
        || value->definition != definition) return false;
    *result = a->value++;
    return true;
}

static SolMirPredicateBlockId authenticated_new_block(
    RichPredicateAuthentication *a, SolMirRecipeId parameter_recipe,
    SolMirPredicateValueId *parameter) {
    size_t id = a->cursor->block++;
    if (id >= a->operations->predicate_block_count
        || id >= a->body->blocks.offset + a->body->blocks.count)
        return SOL_MIR_OPERATION_NONE;
    SolMirPlanSlice parameters = {a->value, 0};
    if (parameter_recipe != SOL_MIR_RECIPE_NONE) {
        if (!authenticated_value(a, SOL_MIR_PREDICATE_VALUE_BLOCK_PARAMETER,
                parameter_recipe, id, 0, parameter))
            return SOL_MIR_OPERATION_NONE;
        parameters.count = 1;
    }
    const SolMirPredicateBlock *block = &a->operations->predicate_blocks[id];
    if (block->body != a->body_id || block->parameters.offset != parameters.offset
        || block->parameters.count != parameters.count)
        return SOL_MIR_OPERATION_NONE;
    return id;
}

static bool authenticated_start_block(RichPredicateAuthentication *a,
    size_t block) {
    if (block >= a->operations->predicate_block_count
        || a->operations->predicate_blocks[block].instructions.offset
            != a->cursor->instruction) return false;
    a->block = block; return true;
}

static bool authenticated_end_block(RichPredicateAuthentication *a,
    SolMirPredicateTerminator expected) {
    if (a->block >= a->operations->predicate_block_count) return false;
    const SolMirPredicateBlock *block
        = &a->operations->predicate_blocks[a->block];
    return block->instructions.offset <= a->cursor->instruction
        && block->instructions.count
            == a->cursor->instruction - block->instructions.offset
        && authenticated_terminator_equal(&block->terminator, &expected);
}

static SolMirPredicateEdgeId authenticated_edge(RichPredicateAuthentication *a,
    size_t source, size_t target, SolMirPredicateValueId argument) {
    size_t id = a->cursor->edge++;
    if (id >= a->operations->predicate_edge_count)
        return SOL_MIR_OPERATION_NONE;
    SolMirPlanSlice arguments = {a->cursor->edge_value, 0};
    if (argument != SOL_MIR_OPERATION_NONE) {
        if (a->cursor->edge_value >= a->operations->predicate_edge_value_count
            || a->operations->predicate_edge_values[a->cursor->edge_value]
                != argument) return SOL_MIR_OPERATION_NONE;
        ++a->cursor->edge_value; arguments.count = 1;
    }
    const SolMirPredicateEdge *edge = &a->operations->predicate_edges[id];
    if (edge->source != source || edge->target != target
        || edge->arguments.offset != arguments.offset
        || edge->arguments.count != arguments.count)
        return SOL_MIR_OPERATION_NONE;
    return id;
}

static bool authenticated_instruction(RichPredicateAuthentication *a,
    SolMirPredicateInstruction expected, SolMirPredicateValueId *result) {
    size_t id = a->cursor->instruction;
    if (id >= a->operations->predicate_instruction_count
        || !authenticated_value(a, SOL_MIR_PREDICATE_VALUE_INSTRUCTION,
            expected.recipe, a->block, id, result)) return false;
    expected.block = a->block; expected.result = *result;
    if (!authenticated_instruction_equal(
            &a->operations->predicate_instructions[id], &expected)) return false;
    ++a->cursor->instruction; return true;
}

static bool authenticated_jump(RichPredicateAuthentication *a, size_t target,
    SolMirPredicateValueId argument) {
    SolMirPredicateTerminator jump = authenticated_terminator(
        SOL_MIR_PREDICATE_TERM_JUMP);
    jump.edge = authenticated_edge(a, a->block, target, argument);
    return jump.edge != SOL_MIR_OPERATION_NONE
        && authenticated_end_block(a, jump);
}

static bool authenticated_failure_block(RichPredicateAuthentication *a,
    SolMirPredicateFailureKind kind, SolMirPredicateBlockId *result) {
    *result = authenticated_new_block(a, SOL_MIR_RECIPE_NONE, NULL);
    if (*result == SOL_MIR_OPERATION_NONE) return false;
    size_t saved = a->block;
    if (!authenticated_start_block(a, *result)) return false;
    SolMirPredicateTerminator term = authenticated_terminator(
        SOL_MIR_PREDICATE_TERM_FAILURE);
    term.failure_kind = kind;
    if (!authenticated_end_block(a, term)) return false;
    a->block = saved; return true;
}

static SolMirMaterializedBindingId authenticated_binding(
    RichPredicateAuthentication *a, size_t expression,
    SolMirPlanDemandKind first, SolMirPlanDemandKind second) {
    size_t found = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = 0; i < a->materialization->binding_count; ++i) {
        if (!tick(1)) return SOL_MIR_MATERIALIZED_NONE;
        const SolMirMaterializedBinding *binding
            = &a->materialization->bindings[i];
        if (binding->context == a->context
            && binding->source.expression == expression
            && (binding->kind == first || binding->kind == second)) {
            if (found != SOL_MIR_MATERIALIZED_NONE)
                return SOL_MIR_MATERIALIZED_NONE;
            found = i;
        }
    }
    return found;
}

static SolMirPredicateBodyId authenticated_body_for_context(
    const SolMirMaterialization *m, size_t context) {
    size_t body = 0;
    for (size_t i = 0; i < m->context_count; ++i) {
        if (!tick(1)) return SOL_MIR_OPERATION_NONE;
        if (m->contexts[i].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT
            && m->contexts[i].kind != SOL_MIR_PLAN_CONTEXT_REFINEMENT) continue;
        if (i == context) return body;
        ++body;
    }
    return SOL_MIR_OPERATION_NONE;
}

static bool authenticated_append_place_path(RichPredicateAuthentication *a,
    size_t place_id, SolMirRecipeId root_recipe, SolMirPlanSlice *path) {
    if (place_id >= a->ir->place_count) return false;
    const SolIrPlace *place = &a->ir->places[place_id];
    path->offset = a->cursor->path;
    SolMirRecipeId current = root_recipe;
    for (size_t i = 0; i < place->projections.count; ++i) {
        if (!tick(1) || current >= a->representation->recipe_count
            || a->cursor->path >= a->operations->predicate_path_step_count)
            return false;
        const SolIrProjection *projection
            = &a->ir->projections[place->projections.offset + i];
        size_t field = SOL_MIR_OPERATION_NONE;
        if (projection->kind == SOL_IR_PROJECTION_FIELD) {
            SolMirPlanSlice fields = a->representation->recipes[current].fields;
            for (size_t f = 0; f < fields.count; ++f) {
                if (!tick(1)) return false;
                size_t candidate = fields.offset + f;
                if (a->representation->fields[candidate].source_field
                        == projection->field) {
                    if (field != SOL_MIR_OPERATION_NONE) return false;
                    field = candidate;
                }
            }
        } else if (projection->kind == SOL_IR_PROJECTION_TUPLE_FIELD) {
            field = field_ordinal(a->representation, current,
                SOL_MIR_OPERATION_NONE, projection->ordinal);
        } else return false;
        if (field >= a->representation->field_count) return false;
        SolMirRecipeId next = a->representation->fields[field].type;
        const SolMirPredicatePathStep *actual
            = &a->operations->predicate_path_steps[a->cursor->path++];
        if (actual->base_recipe != current || actual->result_recipe != next
            || actual->field_layout != field) return false;
        current = next;
    }
    path->count = a->cursor->path - path->offset;
    return true;
}

static bool authenticated_pattern_nodes(RichPredicateAuthentication *a,
    size_t pattern_id, SolMirRecipeId recipe, SolMirPredicatePathStep *path,
    size_t depth, size_t *binding_count) {
    if (!tick(SOL_MIR_OPERATIONS_WORK_RECURSE)
        || pattern_id >= a->ir->pattern_count || depth > a->ir->pattern_count
        || a->cursor->pattern >= a->operations->predicate_pattern_node_count)
        return false;
    const SolIrPattern *source = &a->ir->patterns[pattern_id];
    SolMirPredicatePatternNode expected = {.recipe = recipe,
        .path = {a->cursor->path, depth}, .semantic_tag = 0,
        .boolean = source->boolean};
    for (size_t i = 0; i < depth; ++i) {
        if (!tick(1) || a->cursor->path
                >= a->operations->predicate_path_step_count) return false;
        const SolMirPredicatePathStep *actual
            = &a->operations->predicate_path_steps[a->cursor->path++];
        if (actual->base_recipe != path[i].base_recipe
            || actual->result_recipe != path[i].result_recipe
            || actual->field_layout != path[i].field_layout) return false;
    }
    if (source->kind == SOL_IR_PATTERN_WILDCARD)
        expected.kind = SOL_MIR_OPERATION_PATTERN_WILDCARD;
    else if (source->kind == SOL_IR_PATTERN_BINDING) {
        expected.kind = SOL_MIR_OPERATION_PATTERN_BINDING; ++*binding_count;
    } else if (source->kind == SOL_IR_PATTERN_BOOL)
        expected.kind = SOL_MIR_OPERATION_PATTERN_BOOL;
    else if (source->kind == SOL_IR_PATTERN_VARIANT) {
        expected.kind = SOL_MIR_OPERATION_PATTERN_SUM_TAG;
        size_t variant = variant_source(a->representation, recipe, source->variant);
        if (variant >= a->representation->variant_count) return false;
        expected.semantic_tag = a->operations->layout->variants[variant].tag;
    } else if (source->kind == SOL_IR_PATTERN_RECORD
        || source->kind == SOL_IR_PATTERN_TUPLE)
        expected.kind = SOL_MIR_OPERATION_PATTERN_PRODUCT;
    else return false;
    const SolMirPredicatePatternNode *actual
        = &a->operations->predicate_pattern_nodes[a->cursor->pattern++];
    if (actual->kind != expected.kind || actual->recipe != expected.recipe
        || actual->path.offset != expected.path.offset
        || actual->path.count != expected.path.count
        || actual->semantic_tag != expected.semantic_tag
        || actual->boolean != expected.boolean) return false;
    for (size_t i = 0; i < source->children.count; ++i) {
        if (!tick(1)) return false;
        const SolIrPatternChild *child
            = &a->ir->pattern_children[source->children.offset + i];
        size_t ordinal = child->ordinal;
        if (child->field != SOL_IR_NONE) {
            if (source->definition >= a->ir->definition_count
                || child->field
                    < a->ir->definitions[source->definition].fields.offset)
                return false;
            ordinal = child->field
                - a->ir->definitions[source->definition].fields.offset;
        }
        size_t variant = source->kind == SOL_IR_PATTERN_VARIANT
            ? variant_source(a->representation, recipe, source->variant)
            : SOL_MIR_OPERATION_NONE;
        size_t field = field_ordinal(a->representation, recipe, variant, ordinal);
        if (field >= a->representation->field_count
            || depth >= a->ir->pattern_count) return false;
        path[depth] = (SolMirPredicatePathStep){recipe,
            a->representation->fields[field].type, field};
        if (!authenticated_pattern_nodes(a, child->pattern,
                a->representation->fields[field].type, path, depth + 1,
                binding_count)) return false;
    }
    return true;
}

static bool authenticated_extract_bindings(RichPredicateAuthentication *a,
    size_t root, SolMirPredicateValueId scrutinee, SolMirRecipeId recipe,
    SolMirPredicatePathStep *path, size_t depth, const SolIrArm *arm,
    size_t *leaf) {
    if (!tick(SOL_MIR_OPERATIONS_WORK_RECURSE)
        || root >= a->ir->pattern_count || depth > a->ir->pattern_count
        || arm->bindings.offset > a->ir->root_count
        || arm->bindings.count > a->ir->root_count - arm->bindings.offset)
        return false;
    const SolIrPattern *source = &a->ir->patterns[root];
    if (source->kind == SOL_IR_PATTERN_BINDING) {
        if (*leaf >= arm->bindings.count) return false;
        SolMirPlanSlice result = {a->cursor->path, depth};
        for (size_t i = 0; i < depth; ++i) {
            if (!tick(1) || a->cursor->path
                    >= a->operations->predicate_path_step_count) return false;
            const SolMirPredicatePathStep *actual
                = &a->operations->predicate_path_steps[a->cursor->path++];
            if (actual->base_recipe != path[i].base_recipe
                || actual->result_recipe != path[i].result_recipe
                || actual->field_layout != path[i].field_layout) return false;
        }
        SolMirPredicateInstruction extraction = authenticated_instruction_init(
            SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT,
            authenticated_use_recipe(a, SOL_MIR_PLAN_USE_PATTERN, root, 0));
        extraction.left = scrutinee; extraction.path = result;
        SolMirPredicateValueId value;
        if (!authenticated_instruction(a, extraction, &value)) return false;
        size_t local = a->ir->roots[arm->bindings.offset + *leaf];
        if (local >= a->ir->local_count || a->local_bound[local]) return false;
        a->local_bound[local] = 1; a->locals[local] = value; ++*leaf;
    }
    if (source->children.offset > a->ir->pattern_child_count
        || source->children.count
            > a->ir->pattern_child_count - source->children.offset)
        return false;
    for (size_t i = 0; i < source->children.count; ++i) {
        if (!tick(1)) return false;
        const SolIrPatternChild *child
            = &a->ir->pattern_children[source->children.offset + i];
        size_t ordinal = child->ordinal;
        if (child->field != SOL_IR_NONE) {
            if (source->definition >= a->ir->definition_count
                || child->field < a->ir->definitions[source->definition]
                    .fields.offset
                || child->field - a->ir->definitions[source->definition]
                        .fields.offset
                    >= a->ir->definitions[source->definition].fields.count)
                return false;
            ordinal = child->field
                - a->ir->definitions[source->definition].fields.offset;
        }
        size_t variant = source->kind == SOL_IR_PATTERN_VARIANT
            ? variant_source(a->representation, recipe, source->variant)
            : SOL_MIR_OPERATION_NONE;
        size_t field = field_ordinal(a->representation, recipe, variant, ordinal);
        if (field >= a->representation->field_count
            || depth >= a->ir->pattern_count) return false;
        path[depth] = (SolMirPredicatePathStep){recipe,
            a->representation->fields[field].type, field};
        if (!authenticated_extract_bindings(a, child->pattern, scrutinee,
                a->representation->fields[field].type, path, depth + 1, arm,
                leaf)) return false;
    }
    return true;
}

static bool authenticate_rich_predicate_expression(
    RichPredicateAuthentication *a, size_t expression, size_t depth,
    SolMirPredicateValueId *result);

static bool authenticate_rich_predicate_place(RichPredicateAuthentication *a,
    size_t expression, size_t depth, SolMirPredicateValueId *result) {
    const SolIrExpression *e = &a->ir->expressions[expression];
    if (e->as.place >= a->ir->place_count) return false;
    const SolIrPlace *place = &a->ir->places[e->as.place];
    SolMirPredicateValueId root;
    SolMirRecipeId root_recipe;
    if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY) {
        if (!authenticate_rich_predicate_expression(a, place->temporary,
                depth + 1, &root)) return false;
        root_recipe = a->operations->predicate_values[root].recipe;
    } else if (place->local < a->ir->local_count
        && a->local_bound[place->local]) {
        root = a->locals[place->local];
        root_recipe = authenticated_use_recipe(a,
            SOL_MIR_PLAN_USE_PLACE_ROOT, e->as.place, 0);
    } else {
        SolMirPredicateInputKind kind;
        size_t ordinal = 0; SolAccessMode access = SOL_ACCESS_OWNED;
        SolIrCallableId callable_id = a->import != SOL_MIR_OPERATION_NONE
            ? a->materialization->imports[a->import].source_callable
            : a->materialization->images[a->image].source_callable;
        if (place->local >= a->ir->local_count
            || callable_id >= a->ir->callable_count) return false;
        const SolIrCallable *callable = &a->ir->callables[callable_id];
        if (place->local == callable->receiver) {
            kind = SOL_MIR_PREDICATE_INPUT_RECEIVER;
            access = callable->receiver_access;
        } else if (place->local == callable->capability_source) {
            kind = SOL_MIR_PREDICATE_INPUT_PRIVATE_SOURCE;
            access = a->ir->locals[place->local].access;
        } else {
            kind = SOL_MIR_PREDICATE_INPUT_PARAMETER;
            bool found = false;
            for (size_t i = 0; i < callable->parameters.count; ++i) {
                if (!tick(1)) return false;
                if (a->ir->roots[callable->parameters.offset + i]
                        == place->local) {
                    ordinal = i; access = a->ir->locals[place->local].access;
                    found = true; break;
                }
            }
            if (!found) return false;
        }
        root_recipe = authenticated_use_recipe(a, SOL_MIR_PLAN_USE_PLACE_ROOT,
            e->as.place, 0);
        if (root_recipe >= a->representation->recipe_count
            || a->input >= a->operations->predicate_input_count) return false;
        const SolMirPredicateInput *input
            = &a->operations->predicate_inputs[a->input];
        if (input->kind != kind || input->ordinal != ordinal
            || input->recipe != root_recipe || input->access != access)
            return false;
        if (!authenticated_value(a, SOL_MIR_PREDICATE_VALUE_INPUT, root_recipe,
                a->block, a->input, &root)) return false;
        ++a->input;
    }
    if (place->projections.count == 0) { *result = root; return true; }
    SolMirRecipeId recipe = authenticated_expression_recipe(a->operations,
        a->body, expression);
    SolMirPredicateInstruction instruction = authenticated_instruction_init(
        SOL_MIR_PREDICATE_INST_PROJECT, recipe);
    instruction.left = root;
    if (!authenticated_append_place_path(a, e->as.place, root_recipe,
            &instruction.path)) return false;
    return authenticated_instruction(a, instruction, result);
}

static bool authenticated_input_expression(RichPredicateAuthentication *a,
    size_t expression, SolMirPredicateInputKind kind, size_t ordinal,
    SolAccessMode access, SolMirPredicateValueId *result) {
    SolMirRecipeId recipe = authenticated_expression_recipe(a->operations,
        a->body, expression);
    if (recipe >= a->representation->recipe_count
        || a->input >= a->operations->predicate_input_count) return false;
    const SolMirPredicateInput *input
        = &a->operations->predicate_inputs[a->input];
    if (input->kind != kind || input->ordinal != ordinal
        || input->recipe != recipe || input->access != access) return false;
    if (!authenticated_value(a, SOL_MIR_PREDICATE_VALUE_INPUT, recipe,
            a->block, a->input, result)) return false;
    ++a->input; return true;
}

static bool authenticate_rich_short_circuit(RichPredicateAuthentication *a,
    size_t expression, const SolIrExpression *e, size_t depth,
    SolMirPredicateValueId *result) {
    SolMirPredicateValueId left;
    if (!authenticate_rich_predicate_expression(a, e->as.binary.left,
            depth + 1, &left)) return false;
    size_t branch_block = a->block;
    size_t rhs = authenticated_new_block(a, SOL_MIR_RECIPE_NONE, NULL);
    size_t shortcut = authenticated_new_block(a, SOL_MIR_RECIPE_NONE, NULL);
    SolMirRecipeId recipe = authenticated_expression_recipe(a->operations,
        a->body, expression);
    SolMirPredicateValueId parameter;
    size_t join = authenticated_new_block(a, recipe, &parameter);
    if (rhs == SOL_MIR_OPERATION_NONE || shortcut == SOL_MIR_OPERATION_NONE
        || join == SOL_MIR_OPERATION_NONE) return false;
    SolMirPredicateTerminator branch = authenticated_terminator(
        SOL_MIR_PREDICATE_TERM_BRANCH);
    branch.condition = left;
    bool is_and = e->as.binary.operator_kind == SOL_TOKEN_AMP_AMP;
    branch.true_edge = authenticated_edge(a, branch_block,
        is_and ? rhs : shortcut, SOL_MIR_OPERATION_NONE);
    branch.false_edge = authenticated_edge(a, branch_block,
        is_and ? shortcut : rhs, SOL_MIR_OPERATION_NONE);
    if (branch.true_edge == SOL_MIR_OPERATION_NONE
        || branch.false_edge == SOL_MIR_OPERATION_NONE
        || !authenticated_end_block(a, branch)
        || !authenticated_start_block(a, rhs)) return false;
    SolMirPredicateValueId right;
    if (!authenticate_rich_predicate_expression(a, e->as.binary.right,
            depth + 1, &right)
        || !authenticated_jump(a, join, right)
        || !authenticated_start_block(a, shortcut)) return false;
    SolMirPredicateInstruction constant = authenticated_instruction_init(
        SOL_MIR_PREDICATE_INST_BOOL, recipe);
    constant.boolean = !is_and;
    SolMirPredicateValueId short_value;
    if (!authenticated_instruction(a, constant, &short_value)
        || !authenticated_jump(a, join, short_value)
        || !authenticated_start_block(a, join)) return false;
    *result = parameter; return true;
}

static bool authenticate_rich_if(RichPredicateAuthentication *a,
    size_t expression, const SolIrExpression *e, size_t depth,
    SolMirPredicateValueId *result) {
    SolMirPredicateValueId condition;
    if (!authenticate_rich_predicate_expression(a, e->as.if_expr.condition,
            depth + 1, &condition)) return false;
    size_t branch_block = a->block;
    size_t then_block = authenticated_new_block(a, SOL_MIR_RECIPE_NONE, NULL);
    size_t else_block = authenticated_new_block(a, SOL_MIR_RECIPE_NONE, NULL);
    SolMirRecipeId recipe = authenticated_expression_recipe(a->operations,
        a->body, expression);
    SolMirPredicateValueId parameter;
    size_t join = authenticated_new_block(a, recipe, &parameter);
    if (then_block == SOL_MIR_OPERATION_NONE || else_block == SOL_MIR_OPERATION_NONE
        || join == SOL_MIR_OPERATION_NONE) return false;
    SolMirPredicateTerminator branch = authenticated_terminator(
        SOL_MIR_PREDICATE_TERM_BRANCH);
    branch.condition = condition;
    branch.true_edge = authenticated_edge(a, branch_block, then_block,
        SOL_MIR_OPERATION_NONE);
    branch.false_edge = authenticated_edge(a, branch_block, else_block,
        SOL_MIR_OPERATION_NONE);
    if (branch.true_edge == SOL_MIR_OPERATION_NONE
        || branch.false_edge == SOL_MIR_OPERATION_NONE
        || !authenticated_end_block(a, branch)
        || !authenticated_start_block(a, then_block)) return false;
    SolMirPredicateValueId then_value;
    if (!authenticate_rich_predicate_expression(a, e->as.if_expr.then_branch,
            depth + 1, &then_value)
        || !authenticated_jump(a, join, then_value)
        || !authenticated_start_block(a, else_block)) return false;
    SolMirPredicateValueId else_value;
    if (!authenticate_rich_predicate_expression(a, e->as.if_expr.else_branch,
            depth + 1, &else_value)
        || !authenticated_jump(a, join, else_value)
        || !authenticated_start_block(a, join)) return false;
    *result = parameter; return true;
}

static bool authenticate_rich_match(RichPredicateAuthentication *a,
    size_t expression, const SolIrExpression *e, size_t depth,
    SolMirPredicateValueId *result) {
    SolMirPredicateValueId scrutinee;
    if (!authenticate_rich_predicate_expression(a,
            e->as.match_expr.scrutinee, depth + 1, &scrutinee)) return false;
    SolMirRecipeId scrutinee_recipe = authenticated_expression_recipe(
        a->operations, a->body, e->as.match_expr.scrutinee);
    size_t join = authenticated_new_block(a, authenticated_expression_recipe(
        a->operations, a->body, expression), result);
    if (join == SOL_MIR_OPERATION_NONE) return false;
    size_t next = a->block;
    for (size_t arm_ordinal = 0; arm_ordinal < e->as.match_expr.arms.count;
        ++arm_ordinal) {
        if (!tick(1)
            || (arm_ordinal != 0 && !authenticated_start_block(a, next)))
            return false;
        size_t arm_id = a->ir->arm_ids[
            e->as.match_expr.arms.offset + arm_ordinal];
        if (arm_id >= a->ir->arm_count) return false;
        const SolIrArm *arm = &a->ir->arms[arm_id];
        SolMirRecipeId bool_recipe = SOL_MIR_RECIPE_NONE;
        for (size_t q = 0; q < a->representation->recipe_count; ++q) {
            if (!tick(1)) return false;
            if (a->representation->recipes[q].kind == SOL_MIR_RECIPE_BOOL)
                bool_recipe = q;
        }
        if (bool_recipe == SOL_MIR_RECIPE_NONE) return false;
        SolMirPredicateInstruction test = authenticated_instruction_init(
            SOL_MIR_PREDICATE_INST_PATTERN_TEST, bool_recipe);
        test.left = scrutinee; test.pattern.offset = a->cursor->pattern;
        size_t bindings = 0;
        if (!authenticated_pattern_nodes(a, arm->pattern, scrutinee_recipe,
                a->path_stack, 0, &bindings)) return false;
        test.pattern.count = a->cursor->pattern - test.pattern.offset;
        SolMirPredicateValueId matched;
        if (!authenticated_instruction(a, test, &matched)) return false;
        size_t arm_block = authenticated_new_block(a, SOL_MIR_RECIPE_NONE, NULL);
        size_t miss_block = authenticated_new_block(a, SOL_MIR_RECIPE_NONE, NULL);
        if (arm_block == SOL_MIR_OPERATION_NONE
            || miss_block == SOL_MIR_OPERATION_NONE) return false;
        SolMirPredicateTerminator branch = authenticated_terminator(
            SOL_MIR_PREDICATE_TERM_BRANCH);
        branch.condition = matched;
        branch.true_edge = authenticated_edge(a, next, arm_block,
            SOL_MIR_OPERATION_NONE);
        branch.false_edge = authenticated_edge(a, next, miss_block,
            SOL_MIR_OPERATION_NONE);
        if (branch.true_edge == SOL_MIR_OPERATION_NONE
            || branch.false_edge == SOL_MIR_OPERATION_NONE
            || !authenticated_end_block(a, branch)
            || !authenticated_start_block(a, arm_block)) return false;
        size_t leaf = 0;
        if (!authenticated_extract_bindings(a, arm->pattern, scrutinee,
                scrutinee_recipe, a->path_stack, 0, arm, &leaf)) return false;
        if (leaf != arm->bindings.count || bindings != arm->bindings.count)
            return false;
        if (arm->guard != SOL_IR_NONE) {
            SolMirPredicateValueId guard;
            if (!authenticate_rich_predicate_expression(a, arm->guard,
                    depth + 1, &guard)) return false;
            size_t body_block = authenticated_new_block(a,
                SOL_MIR_RECIPE_NONE, NULL);
            if (body_block == SOL_MIR_OPERATION_NONE) return false;
            branch = authenticated_terminator(SOL_MIR_PREDICATE_TERM_BRANCH);
            branch.condition = guard;
            branch.true_edge = authenticated_edge(a, a->block, body_block,
                SOL_MIR_OPERATION_NONE);
            branch.false_edge = authenticated_edge(a, a->block, miss_block,
                SOL_MIR_OPERATION_NONE);
            if (branch.true_edge == SOL_MIR_OPERATION_NONE
                || branch.false_edge == SOL_MIR_OPERATION_NONE
                || !authenticated_end_block(a, branch)
                || !authenticated_start_block(a, body_block)) return false;
        }
        SolMirPredicateValueId arm_value;
        if (!authenticate_rich_predicate_expression(a, arm->body,
                depth + 1, &arm_value)
            || !authenticated_jump(a, join, arm_value)) return false;
        for (size_t i = 0; i < arm->bindings.count; ++i) {
            size_t local = a->ir->roots[arm->bindings.offset + i];
            a->local_bound[local] = 0;
        }
        next = miss_block;
    }
    if (!authenticated_start_block(a, next)) return false;
    SolMirPredicateTerminator failure = authenticated_terminator(
        SOL_MIR_PREDICATE_TERM_FAILURE);
    failure.failure_kind = SOL_MIR_PREDICATE_FAILURE_NO_MATCH;
    return authenticated_end_block(a, failure)
        && authenticated_start_block(a, join);
}

static bool authenticate_rich_predicate_expression(
    RichPredicateAuthentication *a, size_t expression, size_t depth,
    SolMirPredicateValueId *result) {
    if (!tick(SOL_MIR_OPERATIONS_WORK_RECURSE)
        || expression >= a->ir->expression_count
        || depth > a->ir->expression_count) return false;
    const SolIrExpression *e = &a->ir->expressions[expression];
    SolMirRecipeId recipe = authenticated_expression_recipe(a->operations,
        a->body, expression);
    if (recipe >= a->representation->recipe_count) return false;
    if (e->kind == SOL_IR_EXPR_PLACE)
        return authenticate_rich_predicate_place(a, expression, depth, result);
    if (e->kind == SOL_IR_EXPR_RESULT) {
        const SolIrObligation *ob = &a->ir->obligations[a->obligation];
        return authenticated_input_expression(a, expression,
            ob->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                ? SOL_MIR_PREDICATE_INPUT_SUCCESS_RESULT
                : SOL_MIR_PREDICATE_INPUT_COMPLETE_RESULT,
            0, SOL_ACCESS_OWNED, result);
    }
    if (e->kind == SOL_IR_EXPR_SNAPSHOT_READ) {
        const SolIrObligation *ob = &a->ir->obligations[a->obligation];
        size_t ordinal = SOL_MIR_OPERATION_NONE;
        for (size_t i = 0; i < ob->snapshots.count; ++i) {
            if (!tick(1)) return false;
            if (ob->snapshots.offset + i == e->as.snapshot) ordinal = i;
        }
        if (ordinal == SOL_MIR_OPERATION_NONE
            || ordinal > SIZE_MAX - a->snapshot_base) return false;
        return authenticated_input_expression(a, expression,
            SOL_MIR_PREDICATE_INPUT_SNAPSHOT, a->snapshot_base + ordinal,
            SOL_ACCESS_OWNED, result);
    }
    if (e->kind == SOL_IR_EXPR_REFINEMENT_SELF)
        return authenticated_input_expression(a, expression,
            SOL_MIR_PREDICATE_INPUT_REFINEMENT_SELF, 0, SOL_ACCESS_OWNED,
            result);
    if (e->kind == SOL_IR_EXPR_BINARY
        && (e->as.binary.operator_kind == SOL_TOKEN_AMP_AMP
            || e->as.binary.operator_kind == SOL_TOKEN_PIPE_PIPE))
        return authenticate_rich_short_circuit(a, expression, e, depth, result);
    if (e->kind == SOL_IR_EXPR_IF)
        return authenticate_rich_if(a, expression, e, depth, result);
    if (e->kind == SOL_IR_EXPR_MATCH)
        return authenticate_rich_match(a, expression, e, depth, result);
    if (e->kind == SOL_IR_EXPR_BLOCK) {
        size_t count = e->as.block.statements.count, completed = 0;
        if (count == 0) {
            SolMirPredicateInstruction unit = authenticated_instruction_init(
                SOL_MIR_PREDICATE_INST_UNIT, recipe);
            if (!authenticated_instruction(a, unit, result)) return false;
        }
        bool ok = true;
        for (size_t i = 0; ok && i < count; ++i) {
            if (!tick(1)) { ok = false; break; }
            size_t statement_id
                = a->ir->statement_ids[e->as.block.statements.offset + i];
            if (statement_id >= a->ir->statement_count) { ok = false; break; }
            const SolIrStatement *statement = &a->ir->statements[statement_id];
            if (statement->kind != SOL_IR_STATEMENT_LET
                && statement->kind != SOL_IR_STATEMENT_EXPRESSION
                && statement->kind != SOL_IR_STATEMENT_RETURN) {
                ok = false; break;
            }
            if (!authenticate_rich_predicate_expression(a,
                    statement->expression, depth + 1, result)) {
                ok = false; break;
            }
            if (statement->kind == SOL_IR_STATEMENT_LET) {
                if (statement->local >= a->ir->local_count
                    || a->local_bound[statement->local]) {
                    ok = false; break;
                }
                a->locals[statement->local] = *result;
                a->local_bound[statement->local] = 1;
            }
            completed = i + 1;
            if (statement->kind == SOL_IR_STATEMENT_RETURN) break;
        }
        for (size_t i = 0; i < completed; ++i) {
            const SolIrStatement *statement = &a->ir->statements[
                a->ir->statement_ids[e->as.block.statements.offset + i]];
            if (statement->kind == SOL_IR_STATEMENT_LET)
                a->local_bound[statement->local] = 0;
        }
        return ok;
    }
    if (e->kind == SOL_IR_EXPR_DEFINITION
        || e->kind == SOL_IR_EXPR_BOUND_OPERATION) {
        SolMirPredicateInstruction instruction = authenticated_instruction_init(
            e->kind == SOL_IR_EXPR_DEFINITION ? SOL_MIR_PREDICATE_INST_FUNCTION
                : SOL_MIR_PREDICATE_INST_BOUND_OPERATION, recipe);
        instruction.binding = authenticated_binding(a, expression,
            e->kind == SOL_IR_EXPR_DEFINITION
                ? SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE
                : SOL_MIR_PLAN_DEMAND_BOUND_OPERATION,
            e->kind == SOL_IR_EXPR_DEFINITION
                ? SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE
                : SOL_MIR_PLAN_DEMAND_BOUND_OPERATION);
        if (instruction.binding == SOL_MIR_MATERIALIZED_NONE) return false;
        if (e->kind == SOL_IR_EXPR_BOUND_OPERATION
            && !authenticate_rich_predicate_expression(a,
                e->as.operation.receiver, depth + 1, &instruction.left))
            return false;
        return authenticated_instruction(a, instruction, result);
    }
    if (e->kind == SOL_IR_EXPR_CALL && e->as.call.kind <= SOL_IR_CALL_METHOD) {
        SolMirPredicateValueId callee = SOL_MIR_OPERATION_NONE;
        SolMirPredicateValueId receiver = SOL_MIR_OPERATION_NONE;
        if ((e->as.call.kind == SOL_IR_CALL_CALLBACK
                || e->as.call.kind == SOL_IR_CALL_CAPABILITY)
            && !authenticate_rich_predicate_expression(a, e->as.call.callee,
                depth + 1, &callee)) return false;
        if (e->as.call.kind == SOL_IR_CALL_METHOD
            && !authenticate_rich_predicate_expression(a, e->as.call.receiver,
                depth + 1, &receiver)) return false;
        size_t arguments = a->cursor->operand;
        for (size_t i = 0; i < e->as.call.operands.count; ++i) {
            if (!tick(1) || a->cursor->operand
                    >= a->operations->predicate_operand_count) return false;
            const SolIrOperand *operand
                = &a->ir->operands[e->as.call.operands.offset + i];
            if (operand->access == SOL_ACCESS_EXCLUSIVE) return false;
            SolMirPredicateValueId value;
            if (!authenticate_rich_predicate_expression(a, operand->value,
                    depth + 1, &value)) return false;
            const SolMirPredicateOperand *actual
                = &a->operations->predicate_operands[a->cursor->operand++];
            if (actual->value != value || actual->formal_ordinal != operand->formal
                || actual->access != operand->access
                || actual->field_layout != SOL_MIR_OPERATION_NONE) return false;
        }
        SolMirMaterializedBindingId binding = authenticated_binding(a, expression,
            SOL_MIR_PLAN_DEMAND_PREDICATE, SOL_MIR_PLAN_DEMAND_CALLBACK);
        if (binding >= a->materialization->binding_count) return false;
        const SolMirMaterializedBinding *binding_record
            = &a->materialization->bindings[binding];
        SolMirMaterializedEffectRowId effects
            = binding_record->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
            ? a->materialization->images[binding_record->instance].effects
            : a->materialization->imports[binding_record->import].effects;
        if (effects >= a->materialization->effect_row_count
            || a->materialization->effect_rows[effects].atoms.count != 0)
            return false;
        size_t source_block = a->block;
        SolMirPredicateValueId produced;
        if (!authenticated_value(a, SOL_MIR_PREDICATE_VALUE_TERMINATOR,
                recipe, source_block, source_block, &produced)) return false;
        SolMirPredicateValueId parameter;
        size_t normal = authenticated_new_block(a, recipe, &parameter), failure;
        if (normal == SOL_MIR_OPERATION_NONE
            || !authenticated_failure_block(a, SOL_MIR_PREDICATE_FAILURE_CALL,
                &failure)) return false;
        SolMirPredicateTerminator invoke = authenticated_terminator(
            SOL_MIR_PREDICATE_TERM_INVOKE);
        invoke.call_kind = e->as.call.kind; invoke.binding = binding;
        invoke.effects = effects; invoke.callee = callee;
        invoke.receiver = receiver;
        invoke.receiver_access = e->as.call.receiver_access;
        invoke.arguments = (SolMirPlanSlice){arguments,
            e->as.call.operands.count};
        invoke.result = produced; invoke.result_recipe = recipe;
        invoke.normal_edge = authenticated_edge(a, source_block, normal, produced);
        invoke.failure_edge = authenticated_edge(a, source_block, failure,
            SOL_MIR_OPERATION_NONE);
        if (invoke.normal_edge == SOL_MIR_OPERATION_NONE
            || invoke.failure_edge == SOL_MIR_OPERATION_NONE
            || !authenticated_end_block(a, invoke)
            || !authenticated_start_block(a, normal)) return false;
        *result = parameter; return true;
    }
    if (e->kind == SOL_IR_EXPR_PROPAGATE) {
        SolMirPredicateValueId operand;
        if (!authenticate_rich_predicate_expression(a, e->as.propagate.operand,
                depth + 1, &operand)) return false;
        SolMirRecipeId source_recipe = authenticated_expression_recipe(
            a->operations, a->body, e->as.propagate.operand);
        size_t success = variant_ordinal(a->representation, source_recipe,
            e->as.propagate.kind == SOL_IR_PROPAGATE_OPTION ? 1 : 0);
        size_t residual = variant_ordinal(a->representation, source_recipe,
            e->as.propagate.kind == SOL_IR_PROPAGATE_OPTION ? 0 : 1);
        size_t field = field_ordinal(a->representation, source_recipe, success, 0);
        if (success >= a->representation->variant_count
            || residual >= a->representation->variant_count
            || field >= a->representation->field_count) return false;
        size_t source_block = a->block;
        SolMirPredicateValueId produced;
        if (!authenticated_value(a, SOL_MIR_PREDICATE_VALUE_TERMINATOR,
                recipe, source_block, source_block, &produced)) return false;
        SolMirPredicateValueId parameter;
        size_t normal = authenticated_new_block(a, recipe, &parameter), failure;
        if (normal == SOL_MIR_OPERATION_NONE || !authenticated_failure_block(a,
                SOL_MIR_PREDICATE_FAILURE_PROPAGATION, &failure)) return false;
        SolMirPredicateTerminator term = authenticated_terminator(
            SOL_MIR_PREDICATE_TERM_PROPAGATE);
        term.value = operand; term.result = produced;
        term.propagation_kind = e->as.propagate.kind;
        term.success_variant_layout = success;
        term.residual_variant_layout = residual;
        term.success_field_layout = field; term.result_recipe = recipe;
        term.normal_edge = authenticated_edge(a, source_block, normal, produced);
        term.failure_edge = authenticated_edge(a, source_block, failure,
            SOL_MIR_OPERATION_NONE);
        if (term.normal_edge == SOL_MIR_OPERATION_NONE
            || term.failure_edge == SOL_MIR_OPERATION_NONE
            || !authenticated_end_block(a, term)
            || !authenticated_start_block(a, normal)) return false;
        *result = parameter; return true;
    }
    SolMirPredicateInstruction instruction = authenticated_instruction_init(
        SOL_MIR_PREDICATE_INST_UNIT, recipe);
    if (e->kind == SOL_IR_EXPR_INTEGER) {
        instruction.kind = SOL_MIR_PREDICATE_INST_I64;
        instruction.integer = e->as.integer;
    } else if (e->kind == SOL_IR_EXPR_BOOL) {
        instruction.kind = SOL_MIR_PREDICATE_INST_BOOL;
        instruction.boolean = e->as.boolean;
    } else if (e->kind == SOL_IR_EXPR_UNIT) {
        instruction.kind = SOL_MIR_PREDICATE_INST_UNIT;
    } else if (e->kind == SOL_IR_EXPR_STRING) {
        instruction.kind = SOL_MIR_PREDICATE_INST_TEXT;
        size_t length = strlen(e->as.string);
        instruction.bytes = (SolMirPlanSlice){a->cursor->literal, length};
        if (a->cursor->literal > a->operations->literal_byte_count
            || length > a->operations->literal_byte_count - a->cursor->literal)
            return false;
        for (size_t i = 0; i < length; ++i) {
            if (!tick(1) || a->operations->literal_bytes[
                    a->cursor->literal + i] != e->as.string[i]) return false;
        }
        a->cursor->literal += length;
    } else if (e->kind == SOL_IR_EXPR_UNARY) {
        instruction.kind = SOL_MIR_PREDICATE_INST_UNARY;
        if (!authenticate_rich_predicate_expression(a, e->as.unary.operand,
                depth + 1, &instruction.left)
            || !expected_opcode(SOL_MIR_INST_UNARY,
                e->as.unary.operator_kind, &instruction.opcode,
                &instruction.failures)) return false;
    } else if (e->kind == SOL_IR_EXPR_BINARY) {
        instruction.kind = SOL_MIR_PREDICATE_INST_BINARY;
        if (!authenticate_rich_predicate_expression(a, e->as.binary.left,
                depth + 1, &instruction.left)
            || !authenticate_rich_predicate_expression(a, e->as.binary.right,
                depth + 1, &instruction.right)
            || !expected_opcode(SOL_MIR_INST_BINARY,
                e->as.binary.operator_kind, &instruction.opcode,
                &instruction.failures)) return false;
    } else if (e->kind == SOL_IR_EXPR_RECORD || e->kind == SOL_IR_EXPR_TUPLE
        || e->kind == SOL_IR_EXPR_VARIANT
        || (e->kind == SOL_IR_EXPR_CALL
            && e->as.call.kind >= SOL_IR_CALL_BUILTIN_OK)) {
        SolIrSlice source_slice = e->kind == SOL_IR_EXPR_RECORD
            ? e->as.record.fields : e->kind == SOL_IR_EXPR_TUPLE
            ? e->as.tuple.operands : e->kind == SOL_IR_EXPR_VARIANT
            ? (SolIrSlice){0, 0} : e->as.call.operands;
        instruction.kind = SOL_MIR_PREDICATE_INST_CONSTRUCT;
        instruction.construct_kind = e->kind == SOL_IR_EXPR_RECORD
            ? SOL_MIR_PREDICATE_CONSTRUCT_RECORD : e->kind == SOL_IR_EXPR_TUPLE
            ? SOL_MIR_PREDICATE_CONSTRUCT_TUPLE
            : SOL_MIR_PREDICATE_CONSTRUCT_SUM;
        if (e->kind == SOL_IR_EXPR_CALL
            && e->as.call.kind == SOL_IR_CALL_DISTINCT_CONSTRUCTOR) {
            SolIrDefinitionId definition = e->as.call.definition;
            if (definition >= a->ir->definition_count) return false;
            if (a->ir->definitions[definition].kind == SOL_IR_DEFINITION_REFINED) {
                if (source_slice.count != 1) return false;
                SolMirPredicateValueId backing;
                if (!authenticate_rich_predicate_expression(a,
                        a->ir->operands[source_slice.offset].value, depth + 1,
                        &backing)) return false;
                SolObligationId nested_obligation = SOL_IR_NONE;
                for (size_t i = 0; i < a->ir->obligation_count; ++i) {
                    if (!tick(1)) return false;
                    if (a->ir->obligations[i].owner_kind
                            == SOL_CONTRACT_OWNER_TYPE
                        && a->ir->obligations[i].owner == definition)
                        nested_obligation = i;
                }
                size_t nested_context = SOL_MIR_OPERATION_NONE;
                for (size_t i = 0; i < a->materialization->context_count; ++i) {
                    if (!tick(1)) return false;
                    const SolMirPlanContext *context
                        = &a->materialization->contexts[i];
                    if (context->kind == SOL_MIR_PLAN_CONTEXT_REFINEMENT
                        && context->definition == definition
                        && context->obligation == nested_obligation
                        && context->refinement_type == recipe
                        && context->target_kind == (a->import
                                == SOL_MIR_OPERATION_NONE
                            ? SOL_MIR_PLAN_TARGET_INSTANCE
                            : SOL_MIR_PLAN_TARGET_IMPORT)
                        && (a->import == SOL_MIR_OPERATION_NONE
                            ? context->instance == a->image
                            : context->import == a->import)
                        && context->source.expression == expression) {
                        if (nested_context != SOL_MIR_OPERATION_NONE) return false;
                        nested_context = i;
                    }
                }
                SolMirPredicateBodyId nested_body
                    = authenticated_body_for_context(a->materialization,
                        nested_context);
                if (nested_obligation == SOL_IR_NONE
                    || nested_body >= a->operations->predicate_body_count)
                    return false;
                size_t source_block = a->block;
                SolMirPredicateValueId produced;
                if (!authenticated_value(a,
                        SOL_MIR_PREDICATE_VALUE_TERMINATOR, recipe, source_block,
                        source_block, &produced)) return false;
                SolMirPredicateValueId parameter;
                size_t normal = authenticated_new_block(a, recipe, &parameter);
                size_t failure;
                if (normal == SOL_MIR_OPERATION_NONE
                    || !authenticated_failure_block(a,
                        SOL_MIR_PREDICATE_FAILURE_REFINEMENT, &failure))
                    return false;
                SolMirPredicateTerminator check = authenticated_terminator(
                    SOL_MIR_PREDICATE_TERM_CHECK_REFINED);
                check.value = backing; check.result = produced;
                check.nested_body = nested_body; check.result_recipe = recipe;
                check.normal_edge = authenticated_edge(a, source_block, normal,
                    produced);
                check.failure_edge = authenticated_edge(a, source_block, failure,
                    SOL_MIR_OPERATION_NONE);
                if (check.normal_edge == SOL_MIR_OPERATION_NONE
                    || check.failure_edge == SOL_MIR_OPERATION_NONE
                    || !authenticated_end_block(a, check)
                    || !authenticated_start_block(a, normal)) return false;
                *result = parameter; return true;
            }
            instruction.construct_kind = SOL_MIR_PREDICATE_CONSTRUCT_WRAPPER;
        }
        if (e->kind == SOL_IR_EXPR_VARIANT
            || (e->kind == SOL_IR_EXPR_CALL
                && e->as.call.kind == SOL_IR_CALL_ENUM_CONSTRUCTOR)) {
            size_t source_variant = e->kind == SOL_IR_EXPR_VARIANT
                ? e->as.variant.variant : e->as.call.variant;
            instruction.variant_layout = variant_source(a->representation,
                recipe, source_variant);
        } else if (e->kind == SOL_IR_EXPR_CALL
            && (e->as.call.kind == SOL_IR_CALL_BUILTIN_NONE
                || e->as.call.kind == SOL_IR_CALL_BUILTIN_SOME
                || e->as.call.kind == SOL_IR_CALL_BUILTIN_OK
                || e->as.call.kind == SOL_IR_CALL_BUILTIN_ERR)) {
            size_t ordinal = e->as.call.kind == SOL_IR_CALL_BUILTIN_NONE ? 0
                : e->as.call.kind == SOL_IR_CALL_BUILTIN_SOME ? 1
                : e->as.call.kind == SOL_IR_CALL_BUILTIN_OK ? 0 : 1;
            instruction.variant_layout = variant_ordinal(a->representation,
                recipe, ordinal);
        }
        if (instruction.variant_layout != SOL_MIR_OPERATION_NONE) {
            if (instruction.variant_layout >= a->operations->layout->variant_count)
                return false;
            instruction.semantic_tag = a->operations->layout->variants[
                instruction.variant_layout].tag;
        }
        instruction.operands.offset = a->cursor->operand;
        for (size_t i = 0; i < source_slice.count; ++i) {
            if (!tick(1) || a->cursor->operand
                    >= a->operations->predicate_operand_count) return false;
            const SolIrOperand *operand
                = &a->ir->operands[source_slice.offset + i];
            SolMirPredicateValueId value;
            if (!authenticate_rich_predicate_expression(a, operand->value,
                    depth + 1, &value)) return false;
            size_t field = SOL_MIR_OPERATION_NONE;
            if (instruction.construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_RECORD) {
                SolMirPlanSlice fields = a->representation->recipes[recipe].fields;
                for (size_t f = 0; f < fields.count; ++f) {
                    if (!tick(1)) return false;
                    size_t candidate = fields.offset + f;
                    if (a->representation->fields[candidate].source_field
                            == operand->formal) field = candidate;
                }
            } else if (instruction.construct_kind
                    == SOL_MIR_PREDICATE_CONSTRUCT_TUPLE)
                field = field_ordinal(a->representation, recipe,
                    SOL_MIR_OPERATION_NONE, operand->formal);
            else if (instruction.construct_kind
                    == SOL_MIR_PREDICATE_CONSTRUCT_SUM)
                field = field_ordinal(a->representation, recipe,
                    instruction.variant_layout, i);
            if (instruction.construct_kind != SOL_MIR_PREDICATE_CONSTRUCT_WRAPPER
                && source_slice.count != 0
                && field >= a->representation->field_count) return false;
            const SolMirPredicateOperand *actual
                = &a->operations->predicate_operands[a->cursor->operand++];
            if (actual->value != value || actual->formal_ordinal != operand->formal
                || actual->access != operand->access
                || actual->field_layout != field) return false;
        }
        instruction.operands.count
            = a->cursor->operand - instruction.operands.offset;
    } else return false;
    return authenticated_instruction(a, instruction, result);
}

static bool authenticate_rich_predicate_body(const SolMirOperations *o,
    size_t body_id, PredicateAuthenticationCursor *cursor,
    SolMirPredicateValueId *locals, unsigned char *local_bound,
    SolMirPredicatePathStep *path_stack) {
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    if (body_id >= o->predicate_body_count) return false;
    const SolMirPredicateBody *body = &o->predicate_bodies[body_id];
    size_t literal_start = cursor->literal;
    size_t expected_context = SOL_MIR_OPERATION_NONE, at = 0;
    for (size_t context = 0; context < m->context_count; ++context) {
        if (!tick(1)) return false;
        if (m->contexts[context].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT
            && m->contexts[context].kind != SOL_MIR_PLAN_CONTEXT_REFINEMENT)
            continue;
        if (at++ == body_id) { expected_context = context; break; }
    }
    if (expected_context == SOL_MIR_OPERATION_NONE
        || body->context != expected_context) return false;
    const SolMirPlanContext *context = &m->contexts[expected_context];
    if (context->obligation >= ir->obligation_count) return false;
    const SolIrObligation *obligation = &ir->obligations[context->obligation];
    size_t image = context->target_kind == SOL_MIR_PLAN_TARGET_INSTANCE
        ? context->instance : SOL_MIR_OPERATION_NONE;
    size_t import = context->target_kind == SOL_MIR_PLAN_TARGET_IMPORT
        ? context->import : SOL_MIR_OPERATION_NONE;
    SolMirRecipeId output = authenticated_expression_recipe(o, body,
        obligation->predicate);
    SolIrDefinitionId definition = context->kind
            == SOL_MIR_PLAN_CONTEXT_REFINEMENT
        ? context->definition : SOL_IR_NONE;
    SolMirRecipeId self_recipe = SOL_MIR_RECIPE_NONE;
    if (definition != SOL_IR_NONE) {
        SolMirRecipeId nominal = context->refinement_type;
        if (nominal >= r->recipe_count
            || r->recipes[nominal].kind != SOL_MIR_RECIPE_REFINED) return false;
        self_recipe = r->recipes[nominal].backing;
    }
    size_t provenance_id = provenance_find(o,
        SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY, body_id);
    if (provenance_id == SOL_MIR_OPERATION_NONE) return false;
    const SolMirOperationProvenance *provenance
        = &o->provenance[provenance_id];
    if (body->owner_kind != (import == SOL_MIR_OPERATION_NONE
            ? SOL_MIR_PREDICATE_OWNER_INSTANCE : SOL_MIR_PREDICATE_OWNER_IMPORT)
        || body->instance != image || body->import != import
        || body->phase != obligation->kind || body->outcome != obligation->outcome
        || body->refinement_self_recipe != self_recipe
        || provenance->source_obligation != context->obligation
        || provenance->source_definition != definition
        || body->output_recipe != output
        || body->blocks.offset != cursor->block
        || body->inputs.offset > o->predicate_input_count
        || body->values.offset > o->predicate_value_count) return false;
    SolMirPlanSlice owner_contexts = import == SOL_MIR_OPERATION_NONE
        ? m->images[image].contexts : m->imports[import].contexts;
    size_t snapshot_base = 0;
    for (size_t i = 0; i < owner_contexts.count; ++i) {
        if (!tick(1)) return false;
        size_t candidate = owner_contexts.offset + i;
        if (candidate == expected_context) break;
        if (m->contexts[candidate].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT) continue;
        size_t prior = m->contexts[candidate].obligation;
        if (prior >= ir->obligation_count
            || !add_size(&snapshot_base, ir->obligations[prior].snapshots.count))
            return false;
    }
    if (ir->local_count != 0) {
        memset(locals, 0, ir->local_count * sizeof(*locals));
        memset(local_bound, 0, ir->local_count);
    }
    RichPredicateAuthentication a = {o, r, m, ir, body, body_id, image, import,
        expected_context, context->obligation, SOL_MIR_OPERATION_NONE,
        snapshot_base, body->inputs.offset, body->values.offset, cursor,
        locals, local_bound, path_stack};
    size_t entry = authenticated_new_block(&a, SOL_MIR_RECIPE_NONE, NULL);
    if (entry == SOL_MIR_OPERATION_NONE || body->entry != entry
        || !authenticated_start_block(&a, entry)) return false;
    SolMirPredicateValueId value;
    if (!authenticate_rich_predicate_expression(&a, obligation->predicate, 0,
            &value)) return false;
    SolMirPredicateTerminator term = authenticated_terminator(
        SOL_MIR_PREDICATE_TERM_RETURN);
    term.value = value;
    bool valid = authenticated_end_block(&a, term)
        && a.input == body->inputs.offset + body->inputs.count
        && a.value == body->values.offset + body->values.count
        && cursor->block == body->blocks.offset + body->blocks.count;
    if (valid && scalar_predicate_expression(ir, obligation->predicate, 0)) {
        size_t scalar_literal = literal_start;
        valid = authenticate_predicate_body(o, body_id, context->obligation,
            &scalar_literal) && scalar_literal == cursor->literal;
    }
    return valid;
}

static bool authenticate_predicates(const SolMirOperations *o,
    SolDiagnostics *diagnostics) {
    const SolMirMaterialization *m = o->layout->representation->materialization;
    const SolIr *ir = m->plan->program->ir;
    for (size_t i = 0; i < o->predicate_count; ++i) {
        const SolMirOperationPredicatePlan *predicate = &o->predicates[i];
        if (!tick(1) || predicate->body >= o->predicate_body_count
            || predicate->context >= m->context_count
            || o->predicate_bodies[predicate->body].context != predicate->context)
            return false;
    }
    for (size_t i = 0; i < o->import_envelope_count; ++i) {
        const SolMirImportContractEnvelope *envelope = &o->import_envelopes[i];
        for (size_t q = 0; q < envelope->requires.count; ++q) {
            if (!tick(1)) return false;
            size_t body = o->import_contract_references[envelope->requires.offset + q];
            if (body >= o->predicate_body_count
                || o->predicate_bodies[body].phase != SOL_CONTRACT_REQUIRES)
                return false;
        }
        for (size_t q = 0; q < envelope->ensures.count; ++q) {
            if (!tick(1)) return false;
            size_t body = o->import_contract_references[envelope->ensures.offset + q];
            if (body >= o->predicate_body_count
                || o->predicate_bodies[body].phase != SOL_CONTRACT_ENSURES)
                return false;
        }
    }
    SolMirPredicateValueId *locals = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, sizeof(*locals));
    unsigned char *local_bound = ir->local_count == 0 ? NULL
        : calloc(ir->local_count, 1);
    SolMirPredicatePathStep *path_stack = ir->pattern_count == 0 ? NULL
        : calloc(ir->pattern_count, sizeof(*path_stack));
    if ((ir->local_count != 0 && (locals == NULL || local_bound == NULL))
        || (ir->pattern_count != 0 && path_stack == NULL)) {
        mark_allocation_failed(diagnostics);
        free(locals); free(local_bound); free(path_stack); return false;
    }
    PredicateAuthenticationCursor cursor = {0};
    bool valid = true;
    for (size_t body_id = 0; body_id < o->predicate_body_count; ++body_id) {
        const SolMirPredicateBody *body = &o->predicate_bodies[body_id];
        if (!tick(1) || body->context >= m->context_count
            || m->contexts[body->context].obligation >= ir->obligation_count) {
            valid = false; break;
        }
        const SolMirPlanContext *context = &m->contexts[body->context];
        size_t direct_references = 0, nested_references = 0;
        for (size_t i = 0; i < o->predicate_count; ++i) {
            if (!tick(1)) { valid = false; break; }
            direct_references += o->predicates[i].body == body_id;
        }
        if (!valid) break;
        for (size_t i = 0; i < o->import_contract_reference_count; ++i) {
            if (!tick(1)) { valid = false; break; }
            direct_references += o->import_contract_references[i] == body_id;
        }
        if (!valid) break;
        for (size_t block = 0; block < o->predicate_block_count; ++block) {
            if (!tick(1)) { valid = false; break; }
            const SolMirPredicateTerminator *term
                = &o->predicate_blocks[block].terminator;
            nested_references += term->kind == SOL_MIR_PREDICATE_TERM_CHECK_REFINED
                && term->nested_body == body_id;
        }
        if (!valid || (nested_references != 0
                ? direct_references != 0
                    || context->kind != SOL_MIR_PLAN_CONTEXT_REFINEMENT
                    || context->source_block != SOL_MIR_NONE
                : direct_references == 0)
            || !authenticate_rich_predicate_body(o, body_id, &cursor, locals,
                local_bound, path_stack)) { valid = false; break; }
    }
    valid = valid && cursor.block == o->predicate_block_count
        && cursor.instruction == o->predicate_instruction_count
        && cursor.edge == o->predicate_edge_count
        && cursor.edge_value == o->predicate_edge_value_count
        && cursor.operand == o->predicate_operand_count
        && cursor.path == o->predicate_path_step_count
        && cursor.pattern == o->predicate_pattern_node_count
        && cursor.literal == o->literal_byte_count;
    free(locals); free(local_bound); free(path_stack); return valid;
}

static bool validate_snapshots(const SolMirOperations *o, size_t *prov) {
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    size_t at = 0;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        if (!tick(1)) return false;
        const SolMirMaterializedInstruction *x = &m->instructions[i];
        if (x->kind != SOL_MIR_INST_CAPTURE_SNAPSHOT) continue;
        if (at >= o->snapshot_count
            || x->source_snapshot >= ir->snapshot_count) return false;
        const SolMirOperationSnapshotPlan *p = &o->snapshots[at];
        if (p->image >= m->image_count) return false;
        size_t provenance_id = provenance_find(o,
            SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT, at);
        if (provenance_id == SOL_MIR_OPERATION_NONE) return false;
        const SolMirOperationProvenance *v = &o->provenance[provenance_id];
        ++*prov;
        const SolIrSnapshot *snapshot = &ir->snapshots[x->source_snapshot];
        if (snapshot->operand >= ir->expression_count
            || ir->expressions[snapshot->operand].kind != SOL_IR_EXPR_PLACE
            || ir->expressions[snapshot->operand].as.place >= ir->place_count)
            return false;
        size_t source_place_id = ir->expressions[snapshot->operand].as.place;
        const SolIrPlace *source_place = &ir->places[source_place_id];
        size_t expected_access = SOL_MIR_OPERATION_NONE;
        for (size_t q = 0; q < m->images[p->image].places.count; ++q) {
            if (!tick(1)) return false;
            size_t id = m->images[p->image].places.offset + q;
            if (m->places[id].source_place == source_place_id) {
                if (expected_access != SOL_MIR_OPERATION_NONE) return false;
                expected_access = id;
            }
        }
        size_t expected_local = SOL_MIR_MATERIALIZED_NONE;
        for (size_t q = 0; q < m->images[p->image].locals.count; ++q) {
            if (!tick(1)) return false;
            size_t id = m->images[p->image].locals.offset + q;
            if (m->locals[id].source_local == source_place->local) {
                if (expected_local != SOL_MIR_MATERIALIZED_NONE) return false;
                expected_local = id;
            }
        }
        size_t expected_context = SOL_MIR_OPERATION_NONE;
        for (size_t q = 0; q < m->images[p->image].contexts.count; ++q) {
            if (!tick(1)) return false;
            size_t id = m->images[p->image].contexts.offset + q;
            if (m->contexts[id].kind == SOL_MIR_PLAN_CONTEXT_CONTRACT
                && m->contexts[id].obligation == snapshot->obligation) {
                if (expected_context != SOL_MIR_OPERATION_NONE) return false;
                expected_context = id;
            }
        }
        size_t expected_slot = 0;
        for (size_t q = 0; q < at; ++q) {
            if (!tick(1)) return false;
            expected_slot += o->snapshots[q].image == p->image;
        }
        if (v->kind != SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT || v->executable != at
            || v->source_snapshot != x->source_snapshot
            || v->source_obligation != ir->snapshots[x->source_snapshot].obligation
            || p->image != instruction_image(m, i) || p->instruction != i
            || p->slot != expected_slot || p->context != expected_context
            || p->access != expected_access || p->local != expected_local
            || p->local >= m->local_count || p->root_recipe != m->locals[p->local].type
            || p->path.count != source_place->projections.count
            || !slice(p->path, o->path_step_count)
            || p->recipe != x->type || !r->recipes[x->type].is_copy
            || p->copy_kind != r->recipes[x->type].copy_kind
            || p->provenance != provenance_id
            ) return false;
        SolMirRecipeId recipe = p->root_recipe;
        for (size_t q = 0; q < p->path.count; ++q) {
            if (!tick(1)) return false;
            const SolMirOperationPathStep *step = &o->path_steps[p->path.offset + q];
            const SolIrProjection *source_projection
                = &ir->projections[source_place->projections.offset + q];
            size_t expected_field = SOL_MIR_OPERATION_NONE;
            if (source_projection->kind == SOL_IR_PROJECTION_FIELD) {
                SolMirPlanSlice fields = r->recipes[recipe].fields;
                size_t matches = 0;
                for (size_t f = 0; f < fields.count; ++f) {
                    if (!tick(1)) return false;
                    size_t field = fields.offset + f;
                    if (r->fields[field].source_field == source_projection->field) {
                        expected_field = field; ++matches;
                    }
                }
                if (matches != 1) return false;
            } else if (source_projection->kind == SOL_IR_PROJECTION_TUPLE_FIELD) {
                expected_field = field_ordinal(r, recipe,
                    SOL_MIR_OPERATION_NONE, source_projection->ordinal);
            } else return false;
            if (step->base_recipe != recipe || step->field_layout != expected_field
                || step->field_layout >= r->field_count
                || r->fields[step->field_layout].type != step->result_recipe
                || o->layout->fields[step->field_layout].offset != step->object_offset)
                return false;
            recipe = step->result_recipe;
        }
        if (recipe != p->recipe || (p->access != SOL_MIR_OPERATION_NONE
                && (p->access >= o->access_plan_count
                    || o->access_plans[p->access].image != p->image
                    || o->access_plans[p->access].final_recipe != p->recipe))) return false;
        ++at;
    }
    return at == o->snapshot_count;
}

static bool validate_path_consumption(const SolMirOperations *o) {
    const SolMirMaterialization *m = o->layout->representation->materialization;
    size_t tests = 0, extractions = 0, snapshots = 0, at = 0;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        if (!tick(1)) return false;
        SolMirInstructionKind kind = m->instructions[i].kind;
        if (kind == SOL_MIR_INST_PATTERN_TEST) {
            if (tests >= o->pattern_test_count) return false;
            SolMirPlanSlice nodes = o->pattern_tests[tests++].nodes;
            for (size_t q = 0; q < nodes.count; ++q) {
                if (!tick(1)) return false;
                SolMirPlanSlice path = o->pattern_nodes[nodes.offset + q].path;
                if (path.offset != at || !add_size(&at, path.count)
                    || at > o->path_step_count) return false;
            }
        } else if (kind == SOL_MIR_INST_PATTERN_VALUE) {
            if (extractions >= o->pattern_extraction_count) return false;
            SolMirPlanSlice path = o->pattern_extractions[extractions++].path;
            if (path.offset != at || !add_size(&at, path.count)
                || at > o->path_step_count) return false;
        } else if (kind == SOL_MIR_INST_CAPTURE_SNAPSHOT) {
            if (snapshots >= o->snapshot_count) return false;
            SolMirPlanSlice path = o->snapshots[snapshots++].path;
            if (path.offset != at || !add_size(&at, path.count)
                || at > o->path_step_count) return false;
        }
    }
    return tests == o->pattern_test_count
        && extractions == o->pattern_extraction_count
        && snapshots == o->snapshot_count && at == o->path_step_count;
}

static bool validate_callables_handlers(const SolMirOperations *o, size_t *prov,
    size_t recipe_ids_used) {
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    size_t root = 0;
    if (o->callable_count != r->callable_producer_count
        || o->handler_count != m->handler_count) return false;
    for (size_t i = 0; i < r->callable_producer_count; ++i) {
        if (!tick(1)) return false;
        const SolMirCallableProducer *s = &r->callable_producers[i];
        const SolMirOperationCallablePlan *p = &o->callables[i];
        if (p->semantic_site >= m->semantic_site_count
            || p->function_recipe >= r->recipe_count
            || r->recipes[p->function_recipe].kind != SOL_MIR_RECIPE_FUNCTION)
            return false;
        const SolMirMaterializedSemanticSite *site
            = &m->semantic_sites[p->semantic_site];
        size_t provenance_id = provenance_find(o,
            SOL_MIR_OPERATION_PROVENANCE_CALLABLE, i);
        if (provenance_id == SOL_MIR_OPERATION_NONE) return false;
        const SolMirOperationProvenance *v = &o->provenance[provenance_id];
        ++*prov;
        if (v->kind != SOL_MIR_OPERATION_PROVENANCE_CALLABLE || v->executable != i
            || v->source_expression != s->captured_receiver_expression
            || s->captured_receiver_kind == SOL_MIR_MATERIALIZED_RECEIVER_SOURCE_EXPRESSION
            || p->semantic_site != s->semantic_site || p->kind != s->kind
            || p->function_recipe != s->function_recipe
            || p->target_kind != s->target_kind || p->target_instance != s->instance
            || p->target_import != s->import || p->capture_recipe != s->captured_receiver_type
            || p->effects != s->effects || p->roots.offset != root
            || p->roots.count != s->captured_receiver_roots.count
            || site->produced_function_type != p->function_recipe
            || site->binding != s->binding) return false;
        SolMirOperationCaptureKind capture = SOL_MIR_OPERATION_CAPTURE_NONE;
        if (s->captured_receiver_kind == SOL_MIR_MATERIALIZED_RECEIVER_PLACE)
            capture = SOL_MIR_OPERATION_CAPTURE_PLACE;
        else if (s->captured_receiver_kind == SOL_MIR_MATERIALIZED_RECEIVER_TEMPORARY)
            capture = SOL_MIR_OPERATION_CAPTURE_TEMPORARY;
        else if (s->captured_receiver_kind == SOL_MIR_MATERIALIZED_RECEIVER_VALUE)
            capture = SOL_MIR_OPERATION_CAPTURE_VALUE;
        if (p->capture_kind != capture
            || p->capture_access != (capture == SOL_MIR_OPERATION_CAPTURE_PLACE
                ? s->captured_receiver_place : SOL_MIR_OPERATION_NONE)
            || p->capture_temporary != s->captured_receiver_temporary
            || p->capture_value != s->captured_receiver_value
            || p->capture_instruction != s->captured_receiver_instruction) return false;
        if (p->kind == SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION) {
            if (capture != SOL_MIR_OPERATION_CAPTURE_NONE
                || p->capture_recipe != SOL_MIR_RECIPE_NONE
                || p->roots.count != 0) return false;
        } else if (p->kind == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION) {
            if (capture == SOL_MIR_OPERATION_CAPTURE_NONE
                || p->capture_recipe >= r->recipe_count || p->roots.count == 0
                || site->operation.receiver != p->capture_recipe
                || site->operation.effects != p->effects) return false;
            if (capture == SOL_MIR_OPERATION_CAPTURE_PLACE
                && (p->capture_access >= o->access_plan_count
                    || o->access_plans[p->capture_access].final_recipe
                        != p->capture_recipe)) return false;
            if (capture == SOL_MIR_OPERATION_CAPTURE_TEMPORARY
                && (p->capture_temporary >= m->temporary_count
                    || m->temporaries[p->capture_temporary].type
                        != p->capture_recipe)) return false;
            if (capture == SOL_MIR_OPERATION_CAPTURE_VALUE
                && (p->capture_value >= m->value_count
                    || m->values[p->capture_value].type != p->capture_recipe
                    || m->values[p->capture_value].instruction
                        != p->capture_instruction)) return false;
        } else return false;
        for (size_t q = 0; q < p->roots.count; ++q, ++root) {
            if (!tick(1)) return false;
            if (o->roots[root] != r->receiver_roots[s->captured_receiver_roots.offset + q])
                return false;
        }
    }
    if (root != o->root_count) return false;
    size_t ids = recipe_ids_used;
    for (size_t i = 0; i < m->handler_count; ++i) {
        if (!tick(1)) return false;
        const SolMirMaterializedHandler *s = &m->handlers[i];
        const SolMirOperationHandlerPlan *p = &o->handlers[i];
        if (p->image >= m->image_count || p->authority_access >= o->access_plan_count
            || p->provider_access >= o->access_plan_count
            || p->source_binding >= m->binding_count
            || p->provider_binding >= m->binding_count) return false;
        size_t provenance_id = provenance_find(o,
            SOL_MIR_OPERATION_PROVENANCE_HANDLER, i);
        if (provenance_id == SOL_MIR_OPERATION_NONE) return false;
        const SolMirOperationProvenance *v = &o->provenance[provenance_id];
        ++*prov;
        if (v->kind != SOL_MIR_OPERATION_PROVENANCE_HANDLER || v->executable != i
            || v->source_expression != s->source_expression || p->handler != i
            || p->image != s->parent || p->frame_parent != expected_handler_parent(m, i)
            || p->source_binding != s->source_binding
            || p->provider_binding != s->provider_binding
            || p->authority_access != s->authority || p->provider_access != s->provider
            || p->operation.target_kind != s->operation.target_kind
            || p->operation.instance != s->operation.instance
            || p->operation.import != s->operation.import
            || p->operation.receiver != s->operation.receiver
            || p->operation.root != s->operation.root
            || p->operation.effects != s->operation.effects
            || p->root_match != SOL_MIR_OPERATION_ROOT_TOKEN_EQUAL
            || p->effects != s->operation.effects || !slice(p->parameter_recipes,
                o->recipe_id_count) || p->parameter_recipes.offset != ids) return false;
        const SolMirMaterializedBinding *binding = &m->bindings[s->provider_binding];
        SolMirRecipeId receiver, result; SolMirPlanSlice parameters;
        if (binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
            const SolMirMaterializedImage *target = &m->images[binding->instance];
            receiver = target->receiver; result = target->result; parameters = target->parameter_types;
        } else {
            const SolMirMaterializedImport *target = &m->imports[binding->import];
            receiver = target->receiver; result = target->result; parameters = target->parameter_types;
        }
        if (p->receiver_recipe != receiver || p->result_recipe != result
            || p->parameter_recipes.count != parameters.count
            || o->access_plans[p->authority_access].image != p->image
            || o->access_plans[p->provider_access].image != p->image
            || o->access_plans[p->provider_access].final_recipe != receiver
            || o->access_plans[p->authority_access].final_recipe
                != p->operation.receiver) return false;
        for (size_t q = 0; q < parameters.count; ++q, ++ids)
            if (!tick(1)) return false;
            else
            if (o->recipe_ids[ids] != m->type_ids[parameters.offset + q]) return false;
    }
    return ids == o->recipe_id_count;
}

static bool validate(const SolMirOperations *o, SolDiagnostics *diagnostics,
    bool authenticate_resources, size_t *measured_work,
    size_t *measured_scratch) {
    if (o == NULL || o->layout == NULL || !complete_limits(o->limits))
        return invalid(diagnostics, "operations header is malformed");
    metered_work = 0;
    metered_limit = o->limits.max_validation_work;
#define CHECK_HEADER(member, type, singular) \
    if (!tick(1) \
        || !canonical(o->singular##_count, o->singular##_capacity, o->member) \
        || o->singular##_count > o->limits.max_##member \
        || o->usage.member != o->singular##_count) \
        return invalid(diagnostics, "operations arena is malformed");
    SOL_MIR_OPERATIONS_ARENAS(CHECK_HEADER)
#undef CHECK_HEADER
    if (o->usage.owned_bytes > o->limits.max_owned_bytes
        || o->usage.build_scratch_bytes > o->limits.max_build_scratch_bytes
        || o->usage.build_work > o->limits.max_build_work
        || o->usage.validation_scratch_bytes > o->limits.max_validation_scratch_bytes
        || (authenticate_resources
            && o->usage.validation_work > o->limits.max_validation_work))
        return invalid(diagnostics, "operations resource limit is malformed");
    Range owned[64]; size_t owned_count = 0, persistent = 0, bytes;
#define OWN(member, type, singular) \
    if (!tick(1) \
        || !add_range(owned, &owned_count, o->member, o->singular##_capacity, \
            sizeof(*o->member)) || !mul_size(o->singular##_capacity, \
            sizeof(*o->member), &bytes) || !add_size(&persistent, bytes)) \
        return invalid(diagnostics, "operations arenas overlap or overflow");
    SOL_MIR_OPERATIONS_ARENAS(OWN)
#undef OWN
    if (persistent != o->usage.owned_bytes)
        return invalid(diagnostics, "operations accounting is malformed");
    if (overlaps(owned, owned_count, o->layout, 1, sizeof(*o->layout)))
        return invalid(diagnostics, "operations owner aliases its borrowed chain");
    const SolMirRepresentation *r = o->layout->representation;
    if (r == NULL || overlaps(owned, owned_count, r, 1, sizeof(*r)))
        return invalid(diagnostics, "operations owner aliases representation");
    const SolMirMaterialization *m = r->materialization;
    if (m == NULL || overlaps(owned, owned_count, m, 1, sizeof(*m)))
        return invalid(diagnostics, "operations owner aliases materialization");
    const SolMirPlan *plan = m->plan;
    const SolMirProgram *program = plan == NULL ? NULL : plan->program;
    const SolIr *ir_owner = program == NULL ? NULL : program->ir;
    if (plan == NULL || program == NULL || ir_owner == NULL
        || overlaps(owned, owned_count, plan, 1, sizeof(*plan))
        || overlaps(owned, owned_count, program, 1, sizeof(*program))
        || overlaps(owned, owned_count, ir_owner, 1, sizeof(*ir_owner)))
        return invalid(diagnostics, "operations owner aliases its transitive owners");
#define BORROW(owner, member, capacity) \
    if (!tick(1) \
        || overlaps(owned, owned_count, (owner)->member, (owner)->capacity, \
            sizeof(*(owner)->member))) return invalid(diagnostics, \
            "operations owner aliases a borrowed arena");
    BORROW(o->layout, types, type_capacity); BORROW(o->layout, fields, field_capacity);
    BORROW(o->layout, variants, variant_capacity); BORROW(o->layout, projections, projection_capacity);
    BORROW(r, recipes, recipe_capacity); BORROW(r, fields, field_capacity);
    BORROW(r, variants, variant_capacity); BORROW(r, recipe_ids, recipe_id_capacity);
    BORROW(r, accesses, access_capacity); BORROW(r, receiver_roots, receiver_root_capacity);
    BORROW(r, callable_producers, callable_producer_capacity);
    BORROW(m, images, image_capacity); BORROW(m, types, type_capacity);
    BORROW(m, shape_fields, shape_field_capacity); BORROW(m, shape_variants, shape_variant_capacity);
    BORROW(m, type_ids, type_id_capacity); BORROW(m, accesses, access_capacity);
    BORROW(m, overlays, overlay_capacity); BORROW(m, contexts, context_capacity);
    BORROW(m, locals, local_capacity); BORROW(m, places, place_capacity);
    BORROW(m, projections, projection_capacity); BORROW(m, values, value_capacity);
    BORROW(m, instructions, instruction_capacity); BORROW(m, temporaries, temporary_capacity);
    BORROW(m, construct_operands, construct_operand_capacity);
    BORROW(m, call_arguments, call_argument_capacity); BORROW(m, blocks, block_capacity);
    BORROW(m, edges, edge_capacity); BORROW(m, edge_values, edge_value_capacity);
    BORROW(m, parameter_values, parameter_value_capacity); BORROW(m, loops, loop_capacity);
    BORROW(m, bindings, binding_capacity); BORROW(m, semantic_sites, semantic_site_capacity);
    BORROW(m, receiver_roots, receiver_root_capacity); BORROW(m, imports, import_capacity);
    BORROW(m, handlers, handler_capacity); BORROW(m, writebacks, writeback_capacity);
    BORROW(m, effect_rows, effect_row_capacity); BORROW(m, effect_atoms, effect_atom_capacity);
    BORROW(m, effect_row_atoms, effect_row_atom_capacity); BORROW(m, effect_names, effect_name_capacity);
    BORROW(m, literal_bytes, literal_byte_capacity);
#define PLAN_BORROW(member, capacity) BORROW(plan, member, capacity)
    PLAN_BORROW(types, type_capacity); PLAN_BORROW(type_components, type_component_capacity);
    PLAN_BORROW(type_parameter_accesses, type_parameter_access_capacity);
    PLAN_BORROW(effect_atoms, effect_atom_capacity); PLAN_BORROW(effect_rows, effect_row_capacity);
    PLAN_BORROW(effect_row_atoms, effect_row_atom_capacity); PLAN_BORROW(instances, instance_capacity);
    PLAN_BORROW(instance_type_ids, instance_type_id_capacity);
    PLAN_BORROW(instance_accesses, instance_access_capacity);
    PLAN_BORROW(dictionary_entries, dictionary_entry_capacity); PLAN_BORROW(imports, import_capacity);
    PLAN_BORROW(typed_uses, typed_use_capacity); PLAN_BORROW(contexts, context_capacity);
    PLAN_BORROW(demands, demand_capacity);
#undef PLAN_BORROW
#define COUNT_BORROW(owner, member, count) \
    if (!tick(1) \
        || overlaps(owned, owned_count, (owner)->member, (owner)->count, \
            sizeof(*(owner)->member))) return invalid(diagnostics, \
            "operations owner aliases a borrowed arena");
    COUNT_BORROW(program, roots, root_count);
    COUNT_BORROW(program, approved_imports, approved_import_count);
    COUNT_BORROW(program, templates, template_count);
    COUNT_BORROW(program, imports, import_count);
    COUNT_BORROW(program, specializations, specialization_count);
    COUNT_BORROW(program, references, reference_count);
    COUNT_BORROW(ir_owner, types, type_count); COUNT_BORROW(ir_owner, type_ids, type_id_count);
    COUNT_BORROW(ir_owner, accesses, access_count); COUNT_BORROW(ir_owner, definitions, definition_count);
    COUNT_BORROW(ir_owner, callables, callable_count); COUNT_BORROW(ir_owner, members, member_count);
    COUNT_BORROW(ir_owner, evidence, evidence_count); COUNT_BORROW(ir_owner, locals, local_count);
    COUNT_BORROW(ir_owner, fields, field_count); COUNT_BORROW(ir_owner, variants, variant_count);
    COUNT_BORROW(ir_owner, expressions, expression_count); COUNT_BORROW(ir_owner, places, place_count);
    COUNT_BORROW(ir_owner, projections, projection_count); COUNT_BORROW(ir_owner, statements, statement_count);
    COUNT_BORROW(ir_owner, statement_ids, statement_id_count); COUNT_BORROW(ir_owner, arms, arm_count);
    COUNT_BORROW(ir_owner, arm_ids, arm_id_count); COUNT_BORROW(ir_owner, patterns, pattern_count);
    COUNT_BORROW(ir_owner, pattern_children, pattern_child_count); COUNT_BORROW(ir_owner, operands, operand_count);
    COUNT_BORROW(ir_owner, roots, root_count); COUNT_BORROW(ir_owner, obligations, obligation_count);
    COUNT_BORROW(ir_owner, snapshots, snapshot_count);
    COUNT_BORROW(ir_owner, cleanup_locals, cleanup_local_count);
    COUNT_BORROW(ir_owner, effects, effect_count);
    COUNT_BORROW(ir_owner, generic_parameters, generic_parameter_count);
    COUNT_BORROW(ir_owner, effect_parameters, effect_parameter_count);
    COUNT_BORROW(ir_owner, loop_obligations, loop_obligation_count);
    COUNT_BORROW(ir_owner, unreachable_obligations, unreachable_obligation_count);
    COUNT_BORROW(ir_owner, files, file_count);
#define MIR_BORROW(mir, member, capacity) \
    if (!tick(1) \
        || overlaps(owned, owned_count, (mir)->member, (mir)->capacity, \
            sizeof(*(mir)->member))) return invalid(diagnostics, \
            "operations owner aliases a borrowed MIR arena");
    for (size_t i = 0; i < program->template_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        const SolMir *mir = &program->templates[i].mir;
        if (overlaps(owned, owned_count, mir, 1, sizeof(*mir)))
            return invalid(diagnostics, "operations owner aliases borrowed MIR");
        MIR_BORROW(mir, blocks, block_capacity); MIR_BORROW(mir, instructions, instruction_capacity);
        MIR_BORROW(mir, values, value_capacity); MIR_BORROW(mir, parameter_values, parameter_value_capacity);
        MIR_BORROW(mir, edge_values, edge_value_capacity); MIR_BORROW(mir, call_arguments, call_argument_capacity);
        MIR_BORROW(mir, loops, loop_capacity); MIR_BORROW(mir, construct_operands, construct_operand_capacity);
        MIR_BORROW(mir, temporaries, temporary_capacity);
    }
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        const SolMir *mir = &m->images[i].topology;
        if (overlaps(owned, owned_count, mir, 1, sizeof(*mir)))
            return invalid(diagnostics, "operations owner aliases borrowed image MIR");
        MIR_BORROW(mir, blocks, block_capacity); MIR_BORROW(mir, instructions, instruction_capacity);
        MIR_BORROW(mir, values, value_capacity); MIR_BORROW(mir, parameter_values, parameter_value_capacity);
        MIR_BORROW(mir, edge_values, edge_value_capacity); MIR_BORROW(mir, call_arguments, call_argument_capacity);
        MIR_BORROW(mir, loops, loop_capacity); MIR_BORROW(mir, construct_operands, construct_operand_capacity);
        MIR_BORROW(mir, temporaries, temporary_capacity);
    }
#undef MIR_BORROW
#undef COUNT_BORROW
#undef BORROW
    /* Reject owned aliases before prerequisite validation can scan text. */
#define TEXT_START(pointer) \
    if (overlaps(owned, owned_count, (pointer), 1, sizeof(char))) \
        return invalid(diagnostics, "operations owner aliases borrowed text")
    TEXT_START(ir_owner->source_path);
    TEXT_START(ir_owner->source_bytes);
    for (size_t i = 0; i < plan->effect_atom_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        TEXT_START(plan->effect_atoms[i].name);
    }
#define OPTIONAL_TEXT_START(pointer) \
    if ((pointer) != NULL) { TEXT_START(pointer); }
    for (size_t i = 0; i < ir_owner->definition_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        OPTIONAL_TEXT_START(ir_owner->definitions[i].name);
    }
    for (size_t i = 0; i < ir_owner->callable_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        OPTIONAL_TEXT_START(ir_owner->callables[i].name);
    }
    for (size_t i = 0; i < ir_owner->local_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        OPTIONAL_TEXT_START(ir_owner->locals[i].name);
    }
    for (size_t i = 0; i < ir_owner->field_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        OPTIONAL_TEXT_START(ir_owner->fields[i].name);
    }
    for (size_t i = 0; i < ir_owner->variant_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        OPTIONAL_TEXT_START(ir_owner->variants[i].name);
    }
    for (size_t i = 0; i < ir_owner->effect_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        TEXT_START(ir_owner->effects[i].name);
    }
    for (size_t i = 0; i < ir_owner->expression_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        const SolIrExpression *e = &ir_owner->expressions[i];
        if (e->kind == SOL_IR_EXPR_STRING) {
            TEXT_START(e->as.string);
        } else if (e->kind == SOL_IR_EXPR_HANDLE) {
            TEXT_START(e->as.handler.effect_name);
        }
    }
    for (size_t i = 0; i < ir_owner->statement_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        OPTIONAL_TEXT_START(ir_owner->statements[i].region_label);
    }
    for (size_t i = 0; i < ir_owner->generic_parameter_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        TEXT_START(ir_owner->generic_parameters[i].name);
    }
    for (size_t i = 0; i < ir_owner->effect_parameter_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        TEXT_START(ir_owner->effect_parameters[i].name);
    }
    for (size_t i = 0; i < ir_owner->file_count; ++i) {
        if (!tick(1)) return invalid(diagnostics,
            "operations validation work limit exceeded");
        TEXT_START(ir_owner->files[i].path);
    }
#undef OPTIONAL_TEXT_START
#undef TEXT_START
    size_t required_scratch;
    if (!validation_scratch(o, &required_scratch)
        || required_scratch > o->limits.max_validation_scratch_bytes
        || (authenticate_resources
            && o->usage.validation_scratch_bytes != required_scratch))
        return invalid(diagnostics,
            "operations validation scratch is malformed");
    if (o->layout->usage.validation_work > o->limits.max_validation_work
        || (authenticate_resources
            && o->usage.validation_work < o->layout->usage.validation_work)
        || !tick(o->layout->usage.validation_work))
        return invalid(diagnostics,
            "operations prerequisite validation work limit exceeded");
    if (!sol_mir_layout_validate(o->layout, diagnostics)) return false;
#define TEXT_BORROW(pointer, count) \
    if (overlaps(owned, owned_count, (pointer), (count), sizeof(char))) \
        return invalid(diagnostics, "operations owner aliases borrowed text");
    size_t text_bytes;
    if (!text_size(ir_owner->source_path, &text_bytes)) return false;
    TEXT_BORROW(ir_owner->source_path, text_bytes);
    TEXT_BORROW(ir_owner->source_bytes, ir_owner->source_length + 1);
    for (size_t i = 0; i < plan->effect_atom_count; ++i) {
        if (!tick(1)) return false;
        TEXT_BORROW(plan->effect_atoms[i].name, plan->effect_atoms[i].length + 1);
    }
    for (size_t i = 0; i < ir_owner->definition_count; ++i) {
        if (!tick(1)) return false;
        if (ir_owner->definitions[i].name != NULL) {
            if (!text_size(ir_owner->definitions[i].name, &text_bytes)) return false;
            TEXT_BORROW(ir_owner->definitions[i].name, text_bytes);
        }
    }
    for (size_t i = 0; i < ir_owner->callable_count; ++i) {
        if (!tick(1)) return false;
        if (ir_owner->callables[i].name != NULL) {
            if (!text_size(ir_owner->callables[i].name, &text_bytes)) return false;
            TEXT_BORROW(ir_owner->callables[i].name, text_bytes);
        }
    }
    for (size_t i = 0; i < ir_owner->local_count; ++i) {
        if (!tick(1)) return false;
        if (ir_owner->locals[i].name != NULL) {
            if (!text_size(ir_owner->locals[i].name, &text_bytes)) return false;
            TEXT_BORROW(ir_owner->locals[i].name, text_bytes);
        }
    }
    for (size_t i = 0; i < ir_owner->field_count; ++i) {
        if (!tick(1)) return false;
        if (ir_owner->fields[i].name != NULL) {
            if (!text_size(ir_owner->fields[i].name, &text_bytes)) return false;
            TEXT_BORROW(ir_owner->fields[i].name, text_bytes);
        }
    }
    for (size_t i = 0; i < ir_owner->variant_count; ++i) {
        if (!tick(1)) return false;
        if (ir_owner->variants[i].name != NULL) {
            if (!text_size(ir_owner->variants[i].name, &text_bytes)) return false;
            TEXT_BORROW(ir_owner->variants[i].name, text_bytes);
        }
    }
    for (size_t i = 0; i < ir_owner->effect_count; ++i) {
        if (!tick(1)) return false;
        if (!text_size(ir_owner->effects[i].name, &text_bytes)) return false;
        TEXT_BORROW(ir_owner->effects[i].name, text_bytes);
    }
    for (size_t i = 0; i < ir_owner->expression_count; ++i) {
        if (!tick(1)) return false;
        const SolIrExpression *e = &ir_owner->expressions[i];
        if (e->kind == SOL_IR_EXPR_STRING) {
            if (!text_size(e->as.string, &text_bytes)) return false;
            TEXT_BORROW(e->as.string, text_bytes);
        } else if (e->kind == SOL_IR_EXPR_HANDLE) {
            if (!text_size(e->as.handler.effect_name, &text_bytes)) return false;
            TEXT_BORROW(e->as.handler.effect_name, text_bytes);
        }
    }
    for (size_t i = 0; i < ir_owner->statement_count; ++i) {
        if (!tick(1)) return false;
        if (ir_owner->statements[i].region_label != NULL) {
            if (!text_size(ir_owner->statements[i].region_label, &text_bytes)) return false;
            TEXT_BORROW(ir_owner->statements[i].region_label, text_bytes);
        }
    }
    for (size_t i = 0; i < ir_owner->generic_parameter_count; ++i) {
        if (!tick(1)) return false;
        if (!text_size(ir_owner->generic_parameters[i].name, &text_bytes)) return false;
        TEXT_BORROW(ir_owner->generic_parameters[i].name, text_bytes);
    }
    for (size_t i = 0; i < ir_owner->effect_parameter_count; ++i) {
        if (!tick(1)) return false;
        if (!text_size(ir_owner->effect_parameters[i].name, &text_bytes)) return false;
        TEXT_BORROW(ir_owner->effect_parameters[i].name, text_bytes);
    }
    for (size_t i = 0; i < ir_owner->file_count; ++i) {
        if (!tick(1)) return false;
        if (!text_size(ir_owner->files[i].path, &text_bytes)) return false;
        TEXT_BORROW(ir_owner->files[i].path, text_bytes);
    }
#undef TEXT_BORROW
    size_t scratch_expected, path_scratch, local_values_scratch, dry_scratch;
    const SolIr *ir = m->plan->program->ir;
    if (!mul_size(ir->pattern_count, sizeof(SolMirOperationPathStep),
            &path_scratch)
        || !mul_size(ir->local_count, sizeof(SolMirPredicateValueId),
            &local_values_scratch)
        || !add_size(&local_values_scratch, ir->local_count))
        return invalid(diagnostics, "operations scratch accounting overflows");
    scratch_expected = path_scratch;
    if (!add_size(&scratch_expected, local_values_scratch)
        || !add_size(&scratch_expected,
            o->layout->representation->recipe_count))
        return invalid(diagnostics, "operations scratch accounting overflows");
    dry_scratch = path_scratch;
    if (!add_size(&dry_scratch, local_values_scratch)
        || m->context_count > SIZE_MAX / sizeof(SolMirPredicateBody)
        || !add_size(&dry_scratch,
            m->context_count * sizeof(SolMirPredicateBody)))
        return invalid(diagnostics, "operations scratch accounting overflows");
    if ((dry_scratch > scratch_expected
            ? dry_scratch : scratch_expected) != o->usage.build_scratch_bytes)
        return invalid(diagnostics, "operations scratch accounting is malformed");
    if (!validate_provenance_shapes(o)
        || !validate_provenance_uniqueness(o, diagnostics))
        return invalid(diagnostics, "operation provenance is malformed");
    if (!validate_access(o))
        return invalid(diagnostics, "operation access plans are malformed");
    size_t prov = 0;
    if (!validate_constructors(o, &prov))
        return invalid(diagnostics, "operation constructors are malformed");
    if (!validate_patterns(o, &prov))
        return invalid(diagnostics, "operation patterns are malformed");
    if (!validate_arithmetic(o, &prov, diagnostics))
        return invalid(diagnostics, "operation arithmetic is malformed");
    if (!validate_snapshots(o, &prov) || !validate_path_consumption(o))
        return invalid(diagnostics, "operation snapshots or paths are malformed");
    if (!validate_propagations_predicates(o, &prov))
        return invalid(diagnostics, "operation predicate envelopes are malformed");
    if (!validate_predicate_cfg(o, diagnostics))
        return invalid(diagnostics, "operation predicate CFG is malformed");
    if (!authenticate_predicates(o, diagnostics))
        return invalid(diagnostics, "operation predicate source authentication failed");
    size_t expected_work;
    if (!expected_build_work(o, &expected_work, diagnostics)
        || o->usage.build_work != expected_work)
        return invalid(diagnostics, "operations build work is malformed");
    if (!add_size(&prov, o->predicate_body_count)
        || !add_size(&prov, o->import_snapshot_count))
        return invalid(diagnostics, "operation provenance count overflows");
    if (!validate_callables_handlers(o, &prov, 0)
        || prov != o->provenance_count)
        return invalid(diagnostics, "operation callable, handler, or provenance plans are malformed");
    if (authenticate_resources && o->usage.validation_work != metered_work)
        return invalid(diagnostics, "operations validation work is malformed");
    if (measured_work != NULL) *measured_work = metered_work;
    if (measured_scratch != NULL) *measured_scratch = required_scratch;
    return true;
}

bool sol_mir_operations_internal_validation_requirements(
    const SolMirOperations *o, size_t *work, size_t *scratch,
    SolDiagnostics *diagnostics) {
    return work != NULL && scratch != NULL
        && validate(o, diagnostics, false, work, scratch);
}

bool sol_mir_operations_internal_expected_build_work(const SolMirOperations *o,
    size_t *result) {
    if (o == NULL || o->layout == NULL || result == NULL) return false;
    metered_work = 0;
    metered_limit = SIZE_MAX;
    return expected_build_work(o, result, NULL);
}

bool sol_mir_operations_validate(const SolMirOperations *o,
    SolDiagnostics *diagnostics) {
    return validate(o, diagnostics, true, NULL, NULL);
}
