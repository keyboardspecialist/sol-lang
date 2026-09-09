#include "sol/mir_operations.h"
#include "mir_operations_work.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

bool sol_mir_operations_internal_validation_requirements(
    const SolMirOperations *o, size_t *work, size_t *scratch);

typedef struct {
    SolMirOperations *out;
    SolDiagnostics *diagnostics;
    SolMirOperationsBuildOutcome outcome;
    SolMirOperationPathStep *path_stack;
    SolMirPredicateValueId *predicate_locals;
    unsigned char *predicate_local_bound;
    unsigned char *equality_state;
    size_t actual_work;
} Builder;

typedef struct { char *data; size_t length, capacity; bool failed; } Buffer;

static bool error(SolDiagnostics *d, const char *message) {
    if (d != NULL) sol_diagnostics_add(d, "SOL-MIR-OPERATIONS-001",
        SOL_SEVERITY_ERROR, (SolSpan){0}, message);
    return false;
}

static bool fail(Builder *b, SolMirOperationsBuildOutcome outcome,
    const char *message) {
    b->outcome = outcome;
    return error(b->diagnostics, message);
}

static bool add_size(size_t *value, size_t amount) {
    if (amount > SIZE_MAX - *value) return false;
    *value += amount; return true;
}

static bool charge(Builder *b, size_t amount) {
    if (!add_size(&b->actual_work, amount)
        || b->actual_work > b->out->limits.max_build_work) {
        b->outcome = SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED;
        return false;
    }
    return true;
}

#define CHARGE(builder) charge((builder), SOL_MIR_OPERATIONS_WORK_SCAN)

static bool mul_size(size_t a, size_t b, size_t *result) {
    if (a != 0 && b > SIZE_MAX / a) return false;
    *result = a * b; return true;
}

void sol_mir_operations_init(SolMirOperations *o) {
    if (o != NULL) memset(o, 0, sizeof(*o));
}

void sol_mir_operations_free(SolMirOperations *o) {
    if (o == NULL) return;
#define FREE(member, type, singular) free(o->member);
    SOL_MIR_OPERATIONS_ARENAS(FREE)
#undef FREE
    sol_mir_operations_init(o);
}

SolMirOperationsLimits sol_mir_operations_default_limits(void) {
    return (SolMirOperationsLimits){
        .max_access_plans = 12000000, .max_access_steps = 24000000,
        .max_constructors = 12000000, .max_construct_operands = 24000000,
        .max_pattern_tests = 12000000, .max_pattern_extractions = 12000000,
        .max_pattern_nodes = 48000000, .max_path_steps = 96000000,
        .max_propagations = 12000000, .max_arithmetic = 24000000,
        .max_equality_nodes = 240000000, .max_equality_children = 480000000,
        .max_snapshots = 12000000, .max_callables = 12000000,
        .max_handlers = 12000000, .max_predicates = 12000000,
        .max_predicate_bodies = 12000000, .max_predicate_blocks = 48000000,
        .max_predicate_inputs = 48000000, .max_predicate_values = 96000000,
        .max_predicate_instructions = 96000000,
        .max_predicate_edges = 96000000,
        .max_predicate_edge_values = 96000000,
        .max_predicate_operands = 192000000,
        .max_predicate_path_steps = 192000000,
        .max_predicate_pattern_nodes = 192000000,
        .max_import_envelopes = 12000000,
        .max_import_contract_references = 48000000,
        .max_import_snapshots = 48000000,
        .max_literal_bytes = 512u * 1024u * 1024u,
        .max_recipe_ids = 240000000, .max_roots = 24000000,
        .max_provenance = 48000000, .max_owned_bytes = 1536u * 1024u * 1024u,
        .max_build_scratch_bytes = 256u * 1024u * 1024u,
        .max_build_work = 1000000000,
        .max_validation_scratch_bytes = 1536u * 1024u * 1024u,
        .max_validation_work = 2000000000,
    };
}

static bool limits_zero(SolMirOperationsLimits v) {
    const unsigned char *p = (const unsigned char *)&v;
    for (size_t i = 0; i < sizeof(v); ++i) if (p[i] != 0) return false;
    return true;
}

static bool limits_complete(SolMirOperationsLimits v) {
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

static bool owner_empty(const SolMirOperations *o) {
    if (o == NULL) return false;
    const unsigned char *p = (const unsigned char *)o;
    for (size_t i = 0; i < sizeof(*o); ++i) if (p[i] != 0) return false;
    return true;
}

static SolMirPlanInstanceId image_for_place(Builder *b,
    const SolMirMaterialization *m, size_t place) {
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!CHARGE(b)) return SOL_MIR_OPERATION_NONE;
        SolMirPlanSlice s = m->images[i].places;
        if (place >= s.offset && place - s.offset < s.count) return i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static SolMirPlanInstanceId image_for_instruction(Builder *b,
    const SolMirMaterialization *m, size_t instruction) {
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!CHARGE(b)) return SOL_MIR_OPERATION_NONE;
        SolMirPlanSlice s = m->images[i].instructions;
        if (instruction >= s.offset && instruction - s.offset < s.count) return i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static SolMirPlanInstanceId image_for_block(Builder *b,
    const SolMirMaterialization *m, size_t block) {
    for (size_t i = 0; i < m->image_count; ++i) {
        if (!CHARGE(b)) return SOL_MIR_OPERATION_NONE;
        SolMirPlanSlice s = m->images[i].blocks;
        if (block >= s.offset && block - s.offset < s.count) return i;
    }
    return SOL_MIR_OPERATION_NONE;
}

static bool expression_contains(Builder *b, const SolIr *ir, size_t root,
    size_t target, size_t depth) {
    if (!charge(b, SOL_MIR_OPERATIONS_WORK_RECURSE) || root >= ir->expression_count
        || depth > ir->expression_count) return false;
    if (root == target) return true;
    const SolIrExpression *e = &ir->expressions[root];
#define HAS(id) expression_contains(b, ir, (id), target, depth + 1)
    switch (e->kind) {
        case SOL_IR_EXPR_UNARY: return HAS(e->as.unary.operand);
        case SOL_IR_EXPR_BINARY:
            return HAS(e->as.binary.left) || HAS(e->as.binary.right);
        case SOL_IR_EXPR_CALL:
            if ((e->as.call.callee != SOL_IR_NONE && HAS(e->as.call.callee))
                || (e->as.call.receiver != SOL_IR_NONE && HAS(e->as.call.receiver))) return true;
            for (size_t i = 0; i < e->as.call.operands.count; ++i) {
                if (!charge(b, 1)) return false;
                if (HAS(ir->operands[e->as.call.operands.offset + i].value)) return true;
            }
            return false;
        case SOL_IR_EXPR_RECORD:
            for (size_t i = 0; i < e->as.record.fields.count; ++i) {
                if (!charge(b, 1)) return false;
                if (HAS(ir->operands[e->as.record.fields.offset + i].value)) return true;
            }
            return false;
        case SOL_IR_EXPR_TUPLE:
            for (size_t i = 0; i < e->as.tuple.operands.count; ++i) {
                if (!charge(b, 1)) return false;
                if (HAS(ir->operands[e->as.tuple.operands.offset + i].value)) return true;
            }
            return false;
        case SOL_IR_EXPR_IF:
            return HAS(e->as.if_expr.condition) || HAS(e->as.if_expr.then_branch)
                || HAS(e->as.if_expr.else_branch);
        case SOL_IR_EXPR_MATCH:
            if (HAS(e->as.match_expr.scrutinee)) return true;
            for (size_t i = 0; i < e->as.match_expr.arms.count; ++i) {
                if (!charge(b, 1)) return false;
                const SolIrArm *arm = &ir->arms[ir->arm_ids[e->as.match_expr.arms.offset + i]];
                if ((arm->guard != SOL_IR_NONE && HAS(arm->guard)) || HAS(arm->body)) return true;
            }
            return false;
        case SOL_IR_EXPR_BLOCK:
            for (size_t i = 0; i < e->as.block.statements.count; ++i) {
                if (!charge(b, 1)) return false;
                const SolIrStatement *s
                    = &ir->statements[ir->statement_ids[e->as.block.statements.offset + i]];
                if ((s->target != SOL_IR_NONE && HAS(s->target))
                    || (s->expression != SOL_IR_NONE && HAS(s->expression))
                    || (s->condition != SOL_IR_NONE && HAS(s->condition))) return true;
            }
            return false;
        case SOL_IR_EXPR_PROPAGATE: return HAS(e->as.propagate.operand);
        case SOL_IR_EXPR_HANDLE:
            return HAS(e->as.handler.authority) || HAS(e->as.handler.provider)
                || HAS(e->as.handler.body);
        case SOL_IR_EXPR_BOUND_OPERATION: return HAS(e->as.operation.receiver);
        case SOL_IR_EXPR_INTEGER: case SOL_IR_EXPR_STRING: case SOL_IR_EXPR_BOOL:
        case SOL_IR_EXPR_UNIT: case SOL_IR_EXPR_PLACE: case SOL_IR_EXPR_DEFINITION:
        case SOL_IR_EXPR_REFINEMENT_SELF: case SOL_IR_EXPR_VARIANT:
        case SOL_IR_EXPR_RESULT: case SOL_IR_EXPR_SNAPSHOT_READ:
        case SOL_IR_EXPR_COMPILE_TIME_HEAD:
            return false;
    }
#undef HAS
    return false;
}

static size_t handler_parent(Builder *b, const SolMirMaterialization *m,
    size_t id) {
    const SolIr *ir = m->plan->program->ir;
    const SolMirMaterializedHandler *target = &m->handlers[id];
    size_t result = SOL_MIR_OPERATION_NONE;
    for (size_t i = 0; i < m->handler_count; ++i) {
        if (!charge(b, 1)) return SOL_MIR_OPERATION_NONE;
        if (i == id || m->handlers[i].parent != target->parent) continue;
        size_t source = m->handlers[i].source_expression;
        if (source >= ir->expression_count
            || ir->expressions[source].kind != SOL_IR_EXPR_HANDLE
            || !expression_contains(b, ir, ir->expressions[source].as.handler.body,
                target->source_expression, 0)) continue;
        if (result == SOL_MIR_OPERATION_NONE) result = i;
        else {
            size_t selected = m->handlers[result].source_expression;
            if (expression_contains(b, ir,
                    ir->expressions[selected].as.handler.body, source, 0)) result = i;
        }
    }
    return result;
}

static size_t field_by_ordinal(Builder *b, const SolMirRepresentation *r,
    size_t recipe, size_t variant, size_t ordinal) {
    SolMirPlanSlice fields = variant == SOL_MIR_OPERATION_NONE
        ? r->recipes[recipe].fields : r->variants[variant].fields;
    size_t found = SOL_MIR_OPERATION_NONE;
    for (size_t i = 0; i < fields.count; ++i) {
        if (!charge(b, 1)) return SOL_MIR_OPERATION_NONE;
        size_t id = fields.offset + i;
        if (r->fields[id].ordinal == ordinal) {
            if (found != SOL_MIR_OPERATION_NONE) return SOL_MIR_OPERATION_NONE;
            found = id;
        }
    }
    return found;
}

static size_t variant_by_ordinal(Builder *b, const SolMirRepresentation *r,
    size_t recipe, size_t ordinal) {
    SolMirPlanSlice variants = r->recipes[recipe].variants;
    for (size_t i = 0; i < variants.count; ++i) {
        if (!charge(b, 1)) return SOL_MIR_OPERATION_NONE;
        size_t id = variants.offset + i;
        if (r->variants[id].ordinal == ordinal) return id;
    }
    return SOL_MIR_OPERATION_NONE;
}

static size_t variant_by_source(Builder *b, const SolMirRepresentation *r,
    size_t recipe, size_t source) {
    SolMirPlanSlice variants = r->recipes[recipe].variants;
    for (size_t i = 0; i < variants.count; ++i) {
        if (!charge(b, 1)) return SOL_MIR_OPERATION_NONE;
        size_t id = variants.offset + i;
        if (r->variants[id].source_variant == source) return id;
    }
    return SOL_MIR_OPERATION_NONE;
}

static SolMirOperationOpcode opcode(SolMirInstructionKind instruction,
    SolTokenKind token, unsigned *failures) {
    *failures = SOL_MIR_OPERATION_FAILURE_NONE;
    if (instruction == SOL_MIR_INST_COMPOUND_UPDATE) {
        if (token == SOL_TOKEN_PLUS_EQUAL) token = SOL_TOKEN_PLUS;
        else if (token == SOL_TOKEN_MINUS_EQUAL) token = SOL_TOKEN_MINUS;
        else if (token == SOL_TOKEN_STAR_EQUAL) token = SOL_TOKEN_STAR;
        else if (token == SOL_TOKEN_SLASH_EQUAL) token = SOL_TOKEN_SLASH;
        else if (token == SOL_TOKEN_PERCENT_EQUAL) token = SOL_TOKEN_PERCENT;
    }
    switch (token) {
        case SOL_TOKEN_BANG: return SOL_MIR_OPERATION_BOOL_NOT;
        case SOL_TOKEN_MINUS:
            *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW;
            return instruction == SOL_MIR_INST_UNARY
                ? SOL_MIR_OPERATION_I64_NEG : SOL_MIR_OPERATION_I64_SUB;
        case SOL_TOKEN_PLUS: *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW;
            return SOL_MIR_OPERATION_I64_ADD;
        case SOL_TOKEN_STAR: *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW;
            return SOL_MIR_OPERATION_I64_MUL;
        case SOL_TOKEN_SLASH: *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW
                | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO;
            return SOL_MIR_OPERATION_I64_DIV;
        case SOL_TOKEN_PERCENT: *failures = SOL_MIR_OPERATION_FAILURE_OVERFLOW
                | SOL_MIR_OPERATION_FAILURE_DIVISION_BY_ZERO;
            return SOL_MIR_OPERATION_I64_REM;
        case SOL_TOKEN_LESS: return SOL_MIR_OPERATION_I64_LT;
        case SOL_TOKEN_LESS_EQUAL: return SOL_MIR_OPERATION_I64_LE;
        case SOL_TOKEN_GREATER: return SOL_MIR_OPERATION_I64_GT;
        case SOL_TOKEN_GREATER_EQUAL: return SOL_MIR_OPERATION_I64_GE;
        case SOL_TOKEN_AMP_AMP: return SOL_MIR_OPERATION_BOOL_AND;
        case SOL_TOKEN_PIPE_PIPE: return SOL_MIR_OPERATION_BOOL_OR;
        case SOL_TOKEN_EQUAL_EQUAL: return SOL_MIR_OPERATION_VALUE_EQ;
        case SOL_TOKEN_BANG_EQUAL: return SOL_MIR_OPERATION_VALUE_NE;
        default: return (SolMirOperationOpcode)-1;
    }
}

static bool pattern_totals(Builder *b, const SolIr *ir, size_t id,
    size_t depth, size_t *nodes, size_t *steps) {
    if (!charge(b, SOL_MIR_OPERATIONS_WORK_RECURSE) || id >= ir->pattern_count || depth > ir->pattern_count
        || !add_size(nodes, 1) || !add_size(steps, depth)) return false;
    const SolIrPattern *p = &ir->patterns[id];
    if (p->kind < SOL_IR_PATTERN_WILDCARD || p->kind > SOL_IR_PATTERN_TUPLE
        || p->children.offset > ir->pattern_child_count
        || p->children.count > ir->pattern_child_count - p->children.offset) return false;
    for (size_t i = 0; i < p->children.count; ++i) {
        if (!CHARGE(b)) return false;
        if (!pattern_totals(b, ir,
                ir->pattern_children[p->children.offset + i].pattern,
                depth + 1, nodes, steps)) return false;
    }
    return true;
}

static bool path_to(Builder *b, const SolIr *ir, size_t root, size_t target,
    size_t depth, size_t *length) {
    if (!charge(b, SOL_MIR_OPERATIONS_WORK_RECURSE) || root >= ir->pattern_count
        || depth > ir->pattern_count) return false;
    if (root == target) { *length = depth; return true; }
    const SolIrPattern *p = &ir->patterns[root];
    for (size_t i = 0; i < p->children.count; ++i) {
        if (!charge(b, 1)) return false;
        if (path_to(b, ir, ir->pattern_children[p->children.offset + i].pattern,
                target, depth + 1, length)) return true;
        if (b->outcome == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED) return false;
    }
    return false;
}

typedef struct {
    size_t constructors, construct_operands, tests, extractions, nodes, paths;
    size_t propagations, arithmetic, equality_nodes, equality_children;
    size_t snapshots, predicates;
    size_t predicate_bodies, predicate_blocks, predicate_inputs;
    size_t predicate_values, predicate_instructions, literal_bytes;
    size_t predicate_edges, predicate_edge_values, predicate_operands;
    size_t predicate_path_steps, predicate_pattern_nodes;
    size_t import_envelopes, import_contract_references;
    size_t import_snapshots;
    size_t callables, handlers, recipe_ids, roots, provenance;
} Counts;

static bool count_predicate_graph(const SolMirLayout *layout, Counts *counts,
    Builder *builder);

static bool recipe_reachable(Builder *b, const SolMirRepresentation *r,
    size_t recipe, size_t target, size_t depth) {
    if (!charge(b, SOL_MIR_OPERATIONS_WORK_RECURSE) || recipe >= r->recipe_count || depth > r->recipe_count)
        return false;
    if (recipe == target) return true;
    const SolMirRecipe *value = &r->recipes[recipe];
    for (size_t f = 0; f < value->fields.count; ++f) {
        if (!charge(b, 1)) return false;
        size_t field = value->fields.offset + f;
        if (recipe_reachable(b, r, r->fields[field].type, target, depth + 1))
            return true;
        if (b->outcome == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED) return false;
    }
    for (size_t v = 0; v < value->variants.count; ++v) {
        if (!charge(b, 1)) return false;
        SolMirPlanSlice fields = r->variants[value->variants.offset + v].fields;
        for (size_t f = 0; f < fields.count; ++f) {
            if (!charge(b, 1)) return false;
            size_t field = fields.offset + f;
            if (recipe_reachable(b, r, r->fields[field].type, target, depth + 1))
                return true;
            if (b->outcome == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED)
                return false;
        }
    }
    if (value->kind == SOL_MIR_RECIPE_DISTINCT
        || value->kind == SOL_MIR_RECIPE_REFINED) {
        if (!charge(b, 1)) return false;
        return recipe_reachable(b, r, value->backing, target, depth + 1);
    }
    return false;
}

static bool count_equality_graph(Builder *b, const SolMirRepresentation *r,
    size_t root, Counts *c) {
    for (size_t q = 0; q < r->recipe_count; ++q) {
        if (!charge(b, 1)) return false;
        bool reachable = recipe_reachable(b, r, root, q, 0);
        if (b->outcome == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED) return false;
        if (!reachable) continue;
        if (!add_size(&c->equality_nodes, 1)) return false;
        const SolMirRecipe *recipe = &r->recipes[q];
        for (size_t f = 0; f < recipe->fields.count; ++f) {
            if (!charge(b, 1) || !add_size(&c->equality_children, 1)) return false;
        }
        for (size_t v = 0; v < recipe->variants.count; ++v) {
            if (!charge(b, 1)) return false;
            SolMirPlanSlice fields = r->variants[recipe->variants.offset + v].fields;
            for (size_t f = 0; f < fields.count; ++f)
                if (!charge(b, 1)
                    || !add_size(&c->equality_children, 1)) return false;
        }
        if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
            || recipe->kind == SOL_MIR_RECIPE_REFINED) {
            if (!charge(b, 1) || !add_size(&c->equality_children, 1)) return false;
        }
    }
    return true;
}

static bool count_all(const SolMirLayout *layout, Counts *c, Builder *b) {
    memset(c, 0, sizeof(*c));
    const SolMirRepresentation *r = layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    for (size_t i = 0; i < m->instruction_count; ++i) {
        if (!charge(b, 1)) return false;
        const SolMirMaterializedInstruction *x = &m->instructions[i];
        if (x->kind == SOL_MIR_INST_CONSTRUCT) {
            ++c->constructors; ++c->provenance;
            if (!add_size(&c->construct_operands,
                    x->construct_operands.count)) return false;
        } else if (x->kind == SOL_MIR_INST_PATTERN_TEST) {
            ++c->tests; ++c->provenance;
            if (!pattern_totals(b, ir, x->source_pattern, 0,
                    &c->nodes, &c->paths)) return false;
        } else if (x->kind == SOL_MIR_INST_PATTERN_VALUE) {
            ++c->extractions; ++c->provenance;
            if (x->source_arm >= ir->arm_count) return false;
            size_t length;
            if (!path_to(b, ir, ir->arms[x->source_arm].pattern,
                    x->source_pattern, 0, &length) || !add_size(&c->paths, length)) return false;
        } else if (x->kind == SOL_MIR_INST_UNARY
            || x->kind == SOL_MIR_INST_BINARY
            || x->kind == SOL_MIR_INST_COMPOUND_UPDATE) {
            unsigned f;
            SolMirOperationOpcode op = opcode(x->kind, x->operator_kind, &f);
            if ((int)op < 0) return false;
            ++c->arithmetic; ++c->provenance;
            if (op == SOL_MIR_OPERATION_VALUE_EQ || op == SOL_MIR_OPERATION_VALUE_NE) {
                SolMirRecipeId root = m->values[x->left].type;
                if (!count_equality_graph(b, r, root, c)) return false;
            }
        } else if (x->kind == SOL_MIR_INST_CAPTURE_SNAPSHOT) {
            ++c->snapshots; ++c->provenance;
            if (x->source_snapshot >= ir->snapshot_count
                || ir->snapshots[x->source_snapshot].operand >= ir->expression_count)
                return false;
            const SolIrExpression *operand
                = &ir->expressions[ir->snapshots[x->source_snapshot].operand];
            if (operand->kind != SOL_IR_EXPR_PLACE || operand->as.place >= ir->place_count
                || !add_size(&c->paths,
                    ir->places[operand->as.place].projections.count))
                return false;
        }
    }
    for (size_t i = 0; i < m->block_count; ++i) {
        if (!charge(b, 1)) return false;
        SolMirTerminatorKind kind = m->blocks[i].terminator.kind;
        if (kind == SOL_MIR_TERM_PROPAGATE) { ++c->propagations; ++c->provenance; }
        else if (kind == SOL_MIR_TERM_CHECK_CONTRACT
            || kind == SOL_MIR_TERM_CHECK_REFINED) {
            ++c->predicates; ++c->provenance;
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
            ++c->import_contract_references;
            if (!add_size(&c->import_snapshots,
                    ir->obligations[obligation].snapshots.count)) return false;
            if (!add_size(&c->provenance,
                    ir->obligations[obligation].snapshots.count)) return false;
        }
    }
    c->callables = r->callable_producer_count;
    c->handlers = m->handler_count;
    if (!add_size(&c->provenance, c->callables)
        || !add_size(&c->provenance, c->handlers)) return false;
    for (size_t i = 0; i < r->callable_producer_count; ++i) {
        if (!charge(b, 1)) return false;
        size_t roots = r->callable_producers[i].captured_receiver_roots.count;
        if (!add_size(&c->roots, roots)) return false;
    }
    for (size_t i = 0; i < m->handler_count; ++i) {
        if (!charge(b, 1)) return false;
        const SolMirMaterializedHandler *h = &m->handlers[i];
        const SolMirMaterializedBinding *binding
            = &m->bindings[h->provider_binding];
        SolMirPlanSlice params
            = binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
            ? m->images[binding->instance].parameter_types
            : m->imports[binding->import].parameter_types;
        if (!add_size(&c->recipe_ids, params.count))
            return false;
    }
    if (!count_predicate_graph(layout, c, b)) return false;
    return add_size(&c->provenance, c->predicate_bodies);
}

static void *allocate(Builder *b, size_t count, size_t item_size) {
    if (count == 0) return NULL;
    if (!charge(b, SOL_MIR_OPERATIONS_WORK_ALLOCATE)) return NULL;
    size_t bytes;
    if (!mul_size(count, item_size, &bytes)
        || !add_size(&b->out->usage.owned_bytes, bytes)
        || b->out->usage.owned_bytes > b->out->limits.max_owned_bytes) {
        b->outcome = SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED; return NULL;
    }
    void *p = calloc(count, item_size);
    if (p == NULL) { b->outcome = SOL_MIR_OPERATIONS_BUILD_ALLOCATION_FAILED; }
    return p;
}

static bool add_provenance(Builder *b, SolMirOperationProvenance value) {
    if (!charge(b, SOL_MIR_OPERATIONS_WORK_POPULATE)
        || b->out->provenance_count >= b->out->provenance_capacity) return false;
    b->out->provenance[b->out->provenance_count++] = value; return true;
}

static SolMirRecipeId pattern_recipe(Builder *b,
    const SolMirMaterialization *m, size_t image, size_t source_pattern) {
    const SolMirMaterializedImage *im = &m->images[image];
    for (size_t i = 0; i < im->overlays.count; ++i) {
        if (!charge(b, 1)) return SOL_MIR_RECIPE_NONE;
        const SolMirMaterializedTypeOverlay *o = &m->overlays[im->overlays.offset + i];
        if (o->kind == SOL_MIR_PLAN_USE_PATTERN && o->source == source_pattern)
            return o->type;
    }
    return SOL_MIR_RECIPE_NONE;
}

static bool append_pattern(Builder *b, size_t image, size_t source,
    const SolMirOperationPathStep *path, size_t depth) {
    SolMirOperations *o = b->out;
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    if (!charge(b, SOL_MIR_OPERATIONS_WORK_RECURSE) || source >= ir->pattern_count
        || o->pattern_node_count >= o->pattern_node_capacity)
        return false;
    const SolIrPattern *p = &ir->patterns[source];
    SolMirRecipeId recipe = pattern_recipe(b, m, image, source);
    if (recipe >= r->recipe_count || depth > o->path_step_capacity - o->path_step_count)
        return false;
    size_t path_at = o->path_step_count;
    for (size_t i = 0; i < depth; ++i) {
        if (!charge(b, 1)) return false;
        o->path_steps[o->path_step_count++] = path[i];
    }
    SolMirOperationPatternNode node = {.recipe = recipe,
        .path = {path_at, depth}, .semantic_tag = 0, .boolean = p->boolean};
    if (p->kind == SOL_IR_PATTERN_WILDCARD) node.kind = SOL_MIR_OPERATION_PATTERN_WILDCARD;
    else if (p->kind == SOL_IR_PATTERN_BINDING) node.kind = SOL_MIR_OPERATION_PATTERN_BINDING;
    else if (p->kind == SOL_IR_PATTERN_BOOL) node.kind = SOL_MIR_OPERATION_PATTERN_BOOL;
    else if (p->kind == SOL_IR_PATTERN_VARIANT) {
        node.kind = SOL_MIR_OPERATION_PATTERN_SUM_TAG;
        size_t variant = variant_by_source(b, r, recipe, p->variant);
        if (variant >= r->variant_count) return false;
        node.semantic_tag = o->layout->variants[variant].tag;
    } else if (p->kind == SOL_IR_PATTERN_RECORD || p->kind == SOL_IR_PATTERN_TUPLE) {
        node.kind = SOL_MIR_OPERATION_PATTERN_PRODUCT;
    } else return false;
    o->pattern_nodes[o->pattern_node_count++] = node;
    for (size_t i = 0; i < p->children.count; ++i) {
        if (!charge(b, 1)) return false;
        const SolIrPatternChild *child = &ir->pattern_children[p->children.offset + i];
        size_t ordinal = child->ordinal;
        if (child->field != SOL_IR_NONE) {
            if (p->definition >= ir->definition_count
                || child->field < ir->definitions[p->definition].fields.offset) return false;
            ordinal = child->field - ir->definitions[p->definition].fields.offset;
        }
        size_t variant = p->kind == SOL_IR_PATTERN_VARIANT
            ? variant_by_source(b, r, recipe, p->variant) : SOL_MIR_OPERATION_NONE;
        size_t field = field_by_ordinal(b, r, recipe, variant, ordinal);
        if (field >= r->field_count || depth >= ir->pattern_count) return false;
        if (b->path_stack == NULL) return false;
        b->path_stack[depth] = (SolMirOperationPathStep){recipe, r->fields[field].type,
            field, o->layout->fields[field].offset};
        if (!append_pattern(b, image, child->pattern, b->path_stack, depth + 1))
            return false;
    }
    return true;
}

static bool append_extraction_path(Builder *b, size_t image, size_t root,
    size_t target, SolMirRecipeId recipe, size_t depth, size_t *start,
    size_t *count) {
    const SolMirRepresentation *r = b->out->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    if (!charge(b, SOL_MIR_OPERATIONS_WORK_RECURSE)) return false;
    if (root == target) { *count = depth; return true; }
    if (root >= ir->pattern_count || depth > ir->pattern_count) return false;
    const SolIrPattern *p = &ir->patterns[root];
    for (size_t i = 0; i < p->children.count; ++i) {
        if (!charge(b, 1)) return false;
        const SolIrPatternChild *child = &ir->pattern_children[p->children.offset + i];
        size_t ordinal = child->ordinal;
        if (child->field != SOL_IR_NONE) {
            if (p->definition >= ir->definition_count
                || child->field < ir->definitions[p->definition].fields.offset) return false;
            ordinal = child->field - ir->definitions[p->definition].fields.offset;
        }
        size_t variant = p->kind == SOL_IR_PATTERN_VARIANT
            ? variant_by_source(b, r, recipe, p->variant) : SOL_MIR_OPERATION_NONE;
        size_t field = field_by_ordinal(b, r, recipe, variant, ordinal);
        if (field >= r->field_count) return false;
        size_t mark = b->out->path_step_count;
        if (mark >= b->out->path_step_capacity) return false;
        if (depth == 0) *start = mark;
        b->out->path_steps[b->out->path_step_count++] = (SolMirOperationPathStep){
            recipe, r->fields[field].type, field,
            b->out->layout->fields[field].offset};
        if (append_extraction_path(b, image, child->pattern, target,
                r->fields[field].type, depth + 1, start, count)) return true;
        b->out->path_step_count = mark;
    }
    return false;
}

static SolMirOperationEqualityKind equality_kind(const SolMirRecipe *r) {
    switch (r->kind) {
        case SOL_MIR_RECIPE_TEXT: return SOL_MIR_OPERATION_EQUAL_TEXT;
        case SOL_MIR_RECIPE_TUPLE: case SOL_MIR_RECIPE_RECORD:
            return SOL_MIR_OPERATION_EQUAL_PRODUCT;
        case SOL_MIR_RECIPE_ENUM: case SOL_MIR_RECIPE_OPTION:
        case SOL_MIR_RECIPE_RESULT: return SOL_MIR_OPERATION_EQUAL_SUM;
        case SOL_MIR_RECIPE_DISTINCT: case SOL_MIR_RECIPE_REFINED:
            return SOL_MIR_OPERATION_EQUAL_WRAPPER;
        default: return SOL_MIR_OPERATION_EQUAL_SCALAR;
    }
}

static bool append_equality_recipe(Builder *b, size_t q) {
    SolMirOperations *o = b->out;
    const SolMirRepresentation *r = o->layout->representation;
    if (!charge(b, SOL_MIR_OPERATIONS_WORK_RECURSE) || q >= r->recipe_count) return false;
    if (b->equality_state[q] != 0) return true;
    b->equality_state[q] = 1;
    const SolMirRecipe *recipe = &r->recipes[q];
    size_t node = o->equality_node_count++;
    size_t at = o->equality_child_count, count = 0;
        for (size_t f = 0; f < recipe->fields.count; ++f) {
            if (!charge(b, 1)) return false;
            size_t field = recipe->fields.offset + f;
            o->equality_children[o->equality_child_count++]
                = (SolMirOperationEqualityChild){r->fields[field].type, field,
                    SOL_MIR_OPERATION_NONE, 0};
            ++count;
        }
        for (size_t v = 0; v < recipe->variants.count; ++v) {
            if (!charge(b, 1)) return false;
            size_t variant = recipe->variants.offset + v;
            SolMirPlanSlice fields = r->variants[variant].fields;
            for (size_t f = 0; f < fields.count; ++f) {
                if (!charge(b, 1)) return false;
                size_t field = fields.offset + f;
                o->equality_children[o->equality_child_count++]
                    = (SolMirOperationEqualityChild){r->fields[field].type,
                        field, variant, o->layout->variants[variant].tag};
                ++count;
            }
        }
        if (recipe->kind == SOL_MIR_RECIPE_DISTINCT
            || recipe->kind == SOL_MIR_RECIPE_REFINED) {
            if (!charge(b, 1)) return false;
            o->equality_children[o->equality_child_count++]
                = (SolMirOperationEqualityChild){recipe->backing,
                    SOL_MIR_OPERATION_NONE, SOL_MIR_OPERATION_NONE, 0};
            ++count;
        }
    o->equality_nodes[node] = (SolMirOperationEqualityNode){
        q, equality_kind(recipe), {at, count}};
    for (size_t i = 0; i < count; ++i) {
        if (!charge(b, 1)) return false;
        if (!append_equality_recipe(b, o->equality_children[at + i].recipe)) return false;
    }
    return true;
}

static bool append_equality(Builder *b, SolMirRecipeId root,
    SolMirPlanSlice *slice) {
    const SolMirRepresentation *r = b->out->layout->representation;
    if (r->recipe_count != 0 && b->equality_state == NULL) return false;
    for (size_t i = 0; i < r->recipe_count; ++i) {
        if (!charge(b, 1)) return false;
        b->equality_state[i] = 0;
    }
    slice->offset = b->out->equality_node_count;
    if (!append_equality_recipe(b, root)) return false;
    slice->count = b->out->equality_node_count - slice->offset;
    return true;
}

static SolMirRecipeId predicate_expression_recipe(const SolMirRepresentation *r,
    size_t image, size_t context, size_t expression) {
    const SolMirMaterialization *m = r->materialization;
    if (context >= m->context_count) return SOL_MIR_RECIPE_NONE;
    const SolMirPlanContext *owner = &m->contexts[context];
    SolMirPlanSlice overlays = owner->target_kind == SOL_MIR_PLAN_TARGET_IMPORT
        ? m->imports[owner->import].overlays : m->images[image].overlays;
    for (size_t i = 0; i < overlays.count; ++i) {
        const SolMirMaterializedTypeOverlay *use
            = &m->overlays[overlays.offset + i];
        if (use->kind == SOL_MIR_PLAN_USE_EXPRESSION
            && use->context == context && use->source == expression)
            return use->type;
    }
    return SOL_MIR_RECIPE_NONE;
}

typedef struct {
    Builder *builder;
    size_t image, import, context, body, block;
    size_t snapshot_base;
    SolObligationId obligation;
    SolMirPredicateValueId *locals;
    unsigned char *local_bound;
} PredicateLowerer;

static bool predicate_snapshot_slot(const SolIrObligation *obligation,
    SolIrSnapshotId source, size_t *slot) {
    for (size_t i = 0; i < obligation->snapshots.count; ++i)
        if (obligation->snapshots.offset + i == source) {
            *slot = i; return true;
        }
    return false;
}

static SolMirPredicateTerminator predicate_terminator(
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

static SolMirRecipeId predicate_use_recipe(const SolMirRepresentation *r,
    size_t image, size_t import, size_t context, SolMirPlanTypedUseKind kind,
    size_t source, size_t ordinal) {
    const SolMirMaterialization *m = r->materialization;
    SolMirPlanSlice overlays = import != SOL_MIR_OPERATION_NONE
        ? m->imports[import].overlays : m->images[image].overlays;
    SolMirRecipeId found = SOL_MIR_RECIPE_NONE;
    for (size_t i = 0; i < overlays.count; ++i) {
        const SolMirMaterializedTypeOverlay *use = &m->overlays[overlays.offset + i];
        if (use->kind == kind && use->context == context && use->source == source
            && use->ordinal == ordinal) {
            if (found != SOL_MIR_RECIPE_NONE) return SOL_MIR_RECIPE_NONE;
            found = use->type;
        }
    }
    return found;
}

static bool predicate_value(PredicateLowerer *l, SolMirPredicateValueKind kind,
    SolMirRecipeId recipe, size_t block, size_t definition,
    SolMirPredicateValueId *result) {
    SolMirOperations *o = l->builder->out;
    if (o->predicate_value_count == SIZE_MAX) return false;
    *result = o->predicate_value_count++;
    if (o->predicate_values != NULL)
        o->predicate_values[*result] = (SolMirPredicateValue){kind, recipe,
            block, definition};
    return true;
}

static SolMirPredicateBlockId predicate_new_block(PredicateLowerer *l,
    SolMirRecipeId parameter_recipe, SolMirPredicateValueId *parameter) {
    SolMirOperations *o = l->builder->out;
    if (o->predicate_block_count == SIZE_MAX) return SOL_MIR_OPERATION_NONE;
    size_t id = o->predicate_block_count++;
    SolMirPlanSlice parameters = {o->predicate_value_count, 0};
    if (parameter_recipe != SOL_MIR_RECIPE_NONE) {
        if (!predicate_value(l, SOL_MIR_PREDICATE_VALUE_BLOCK_PARAMETER,
                parameter_recipe, id, 0, parameter)) return SOL_MIR_OPERATION_NONE;
        parameters.count = 1;
    }
    if (o->predicate_blocks != NULL) {
        o->predicate_blocks[id] = (SolMirPredicateBlock){l->body, parameters,
            {SOL_MIR_OPERATION_NONE, 0},
            predicate_terminator(SOL_MIR_PREDICATE_TERM_FAILURE)};
        o->predicate_blocks[id].terminator.failure_kind
            = SOL_MIR_PREDICATE_FAILURE_CALL;
    }
    return id;
}

static void predicate_start_block(PredicateLowerer *l, size_t block) {
    l->block = block;
    if (l->builder->out->predicate_blocks != NULL)
        l->builder->out->predicate_blocks[block].instructions.offset
            = l->builder->out->predicate_instruction_count;
}

static bool predicate_end_block(PredicateLowerer *l,
    SolMirPredicateTerminator term) {
    SolMirOperations *o = l->builder->out;
    if (o->predicate_blocks != NULL) {
        SolMirPredicateBlock *block = &o->predicate_blocks[l->block];
        if (block->instructions.offset == SOL_MIR_OPERATION_NONE) return false;
        block->instructions.count = o->predicate_instruction_count
            - block->instructions.offset;
        block->terminator = term;
    }
    return true;
}

static SolMirPredicateEdgeId predicate_edge(PredicateLowerer *l, size_t source,
    size_t target, SolMirPredicateValueId argument) {
    SolMirOperations *o = l->builder->out;
    if (o->predicate_edge_count == SIZE_MAX) return SOL_MIR_OPERATION_NONE;
    size_t id = o->predicate_edge_count++;
    SolMirPlanSlice arguments = {o->predicate_edge_value_count, 0};
    if (argument != SOL_MIR_OPERATION_NONE) {
        if (o->predicate_edge_value_count == SIZE_MAX) return SOL_MIR_OPERATION_NONE;
        if (o->predicate_edge_values != NULL)
            o->predicate_edge_values[o->predicate_edge_value_count] = argument;
        ++o->predicate_edge_value_count; arguments.count = 1;
    }
    if (o->predicate_edges != NULL)
        o->predicate_edges[id] = (SolMirPredicateEdge){source, target, arguments};
    return id;
}

static bool predicate_failure_block(PredicateLowerer *l,
    SolMirPredicateFailureKind kind, SolMirPredicateBlockId *result) {
    *result = predicate_new_block(l, SOL_MIR_RECIPE_NONE, NULL);
    if (*result == SOL_MIR_OPERATION_NONE) return false;
    size_t saved = l->block;
    predicate_start_block(l, *result);
    SolMirPredicateTerminator term = predicate_terminator(
        SOL_MIR_PREDICATE_TERM_FAILURE);
    term.failure_kind = kind;
    if (!predicate_end_block(l, term)) return false;
    l->block = saved;
    return true;
}

static bool predicate_instruction(PredicateLowerer *l,
    SolMirPredicateInstruction instruction, SolMirPredicateValueId *result) {
    SolMirOperations *o = l->builder->out;
    if (o->predicate_instruction_count == SIZE_MAX) return false;
    size_t id = o->predicate_instruction_count++;
    if (!predicate_value(l, SOL_MIR_PREDICATE_VALUE_INSTRUCTION,
            instruction.recipe, l->block, id, result)) return false;
    instruction.block = l->block; instruction.result = *result;
    if (o->predicate_instructions != NULL) o->predicate_instructions[id] = instruction;
    return true;
}

static SolMirPredicateInstruction predicate_instruction_init(
    SolMirPredicateInstructionKind kind, SolMirRecipeId recipe) {
    SolMirPredicateInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.kind = kind; instruction.recipe = recipe;
    instruction.left = instruction.right = SOL_MIR_OPERATION_NONE;
    instruction.binding = SOL_MIR_MATERIALIZED_NONE;
    instruction.variant_layout = SOL_MIR_OPERATION_NONE;
    return instruction;
}

static SolMirMaterializedBindingId predicate_binding(PredicateLowerer *l,
    size_t expression, SolMirPlanDemandKind first, SolMirPlanDemandKind second) {
    const SolMirMaterialization *m = l->builder->out->layout->representation
        ->materialization;
    size_t found = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = 0; i < m->binding_count; ++i) {
        if (!CHARGE(l->builder)) return SOL_MIR_MATERIALIZED_NONE;
        const SolMirMaterializedBinding *binding = &m->bindings[i];
        if (binding->context == l->context
            && binding->source.expression == expression
            && (binding->kind == first || binding->kind == second)) {
            if (found != SOL_MIR_MATERIALIZED_NONE) return SOL_MIR_MATERIALIZED_NONE;
            found = i;
        }
    }
    return found;
}

static bool predicate_append_path(PredicateLowerer *l, size_t place_id,
    SolMirRecipeId root_recipe, SolMirPlanSlice *path) {
    SolMirOperations *o = l->builder->out;
    const SolMirRepresentation *r = o->layout->representation;
    const SolIr *ir = r->materialization->plan->program->ir;
    if (place_id >= ir->place_count) return false;
    const SolIrPlace *place = &ir->places[place_id];
    path->offset = o->predicate_path_step_count;
    SolMirRecipeId current = root_recipe;
    for (size_t i = 0; i < place->projections.count; ++i) {
        if (!CHARGE(l->builder)) return false;
        const SolIrProjection *projection
            = &ir->projections[place->projections.offset + i];
        size_t field = SOL_MIR_OPERATION_NONE;
        if (projection->kind == SOL_IR_PROJECTION_FIELD) {
            SolMirPlanSlice fields = r->recipes[current].fields;
            for (size_t f = 0; f < fields.count; ++f) {
                if (!CHARGE(l->builder)) return false;
                size_t candidate = fields.offset + f;
                if (r->fields[candidate].source_field == projection->field) {
                    if (field != SOL_MIR_OPERATION_NONE) return false;
                    field = candidate;
                }
            }
        } else if (projection->kind == SOL_IR_PROJECTION_TUPLE_FIELD) {
            field = field_by_ordinal(l->builder, r, current,
                SOL_MIR_OPERATION_NONE, projection->ordinal);
        } else return false;
        if (field >= r->field_count || o->predicate_path_step_count == SIZE_MAX)
            return false;
        SolMirRecipeId next = r->fields[field].type;
        if (o->predicate_path_steps != NULL)
            o->predicate_path_steps[o->predicate_path_step_count]
                = (SolMirPredicatePathStep){current, next, field};
        ++o->predicate_path_step_count; current = next;
    }
    path->count = o->predicate_path_step_count - path->offset;
    return true;
}

static bool lower_predicate_expression(PredicateLowerer *l, size_t expression,
    size_t depth, SolMirPredicateValueId *result);

static bool lower_predicate_place(PredicateLowerer *l, size_t expression,
    size_t depth, SolMirPredicateValueId *result) {
    SolMirOperations *o = l->builder->out;
    const SolMirMaterialization *m = o->layout->representation->materialization;
    const SolIr *ir = m->plan->program->ir;
    const SolIrExpression *e = &ir->expressions[expression];
    const SolIrPlace *place = &ir->places[e->as.place];
    SolMirPredicateValueId root;
    SolMirRecipeId root_recipe;
    if (place->root_kind == SOL_IR_PLACE_ROOT_TEMPORARY) {
        if (!lower_predicate_expression(l, place->temporary, depth + 1, &root))
            return false;
        root_recipe = o->predicate_values != NULL
            ? o->predicate_values[root].recipe
            : predicate_expression_recipe(o->layout->representation, l->image,
                l->context, place->temporary);
    } else if (place->local < ir->local_count && l->local_bound[place->local]) {
        root = l->locals[place->local];
        root_recipe = predicate_use_recipe(o->layout->representation, l->image,
            l->import, l->context, SOL_MIR_PLAN_USE_PLACE_ROOT, e->as.place, 0);
    } else {
        SolMirPredicateInputKind kind;
        size_t ordinal = 0;
        SolAccessMode access = SOL_ACCESS_OWNED;
        SolIrLocalId local = place->local;
        SolIrCallableId callable_id = l->image == SOL_MIR_OPERATION_NONE
            ? m->imports[l->import].source_callable
            : m->images[l->image].source_callable;
        const SolIrCallable *callable = &ir->callables[callable_id];
        if (local == callable->receiver) {
            kind = SOL_MIR_PREDICATE_INPUT_RECEIVER;
            access = callable->receiver_access;
        } else if (local == callable->capability_source) {
            kind = SOL_MIR_PREDICATE_INPUT_PRIVATE_SOURCE;
            access = ir->locals[local].access;
        } else {
            kind = SOL_MIR_PREDICATE_INPUT_PARAMETER;
            bool found = false;
            for (size_t i = 0; i < callable->parameters.count; ++i) {
                if (!CHARGE(l->builder)) return false;
                if (ir->roots[callable->parameters.offset + i] == local) {
                    ordinal = i; access = ir->locals[local].access;
                    found = true; break;
                }
            }
            if (!found) return false;
        }
        root_recipe = predicate_use_recipe(o->layout->representation, l->image,
            l->import, l->context, SOL_MIR_PLAN_USE_PLACE_ROOT, e->as.place, 0);
        if (root_recipe == SOL_MIR_RECIPE_NONE || o->predicate_input_count == SIZE_MAX)
            return false;
        size_t input = o->predicate_input_count++;
        if (o->predicate_inputs != NULL)
            o->predicate_inputs[input] = (SolMirPredicateInput){kind, ordinal,
                root_recipe, access};
        if (!predicate_value(l, SOL_MIR_PREDICATE_VALUE_INPUT, root_recipe,
                l->block, input, &root)) return false;
    }
    if (place->projections.count == 0) { *result = root; return true; }
    SolMirRecipeId recipe = predicate_expression_recipe(o->layout->representation,
        l->image, l->context, expression);
    SolMirPredicateInstruction instruction = predicate_instruction_init(
        SOL_MIR_PREDICATE_INST_PROJECT, recipe);
    instruction.left = root;
    if (!predicate_append_path(l, e->as.place, root_recipe, &instruction.path))
        return false;
    return predicate_instruction(l, instruction, result);
}

static bool predicate_input_expression(PredicateLowerer *l, size_t expression,
    SolMirPredicateInputKind kind, size_t ordinal, SolAccessMode access,
    SolMirPredicateValueId *result) {
    SolMirOperations *o = l->builder->out;
    SolMirRecipeId recipe = predicate_expression_recipe(o->layout->representation,
        l->image, l->context, expression);
    if (recipe == SOL_MIR_RECIPE_NONE || o->predicate_input_count == SIZE_MAX)
        return false;
    size_t input = o->predicate_input_count++;
    if (o->predicate_inputs != NULL)
        o->predicate_inputs[input] = (SolMirPredicateInput){kind, ordinal,
            recipe, access};
    return predicate_value(l, SOL_MIR_PREDICATE_VALUE_INPUT, recipe, l->block,
        input, result);
}

static bool predicate_jump(PredicateLowerer *l, size_t target,
    SolMirPredicateValueId argument) {
    SolMirPredicateTerminator jump = predicate_terminator(
        SOL_MIR_PREDICATE_TERM_JUMP);
    jump.edge = predicate_edge(l, l->block, target, argument);
    return jump.edge != SOL_MIR_OPERATION_NONE && predicate_end_block(l, jump);
}

static bool lower_predicate_short_circuit(PredicateLowerer *l,
    size_t expression, const SolIrExpression *e, size_t depth,
    SolMirPredicateValueId *result) {
    SolMirOperations *o = l->builder->out;
    SolMirPredicateValueId left;
    if (!lower_predicate_expression(l, e->as.binary.left, depth + 1, &left))
        return false;
    size_t branch_block = l->block;
    size_t rhs = predicate_new_block(l, SOL_MIR_RECIPE_NONE, NULL);
    size_t shortcut = predicate_new_block(l, SOL_MIR_RECIPE_NONE, NULL);
    SolMirRecipeId recipe = predicate_expression_recipe(o->layout->representation,
        l->image, l->context, expression);
    SolMirPredicateValueId parameter;
    size_t join = predicate_new_block(l, recipe, &parameter);
    if (rhs == SOL_MIR_OPERATION_NONE || shortcut == SOL_MIR_OPERATION_NONE
        || join == SOL_MIR_OPERATION_NONE)
        return false;
    SolMirPredicateTerminator branch = predicate_terminator(
        SOL_MIR_PREDICATE_TERM_BRANCH);
    branch.condition = left;
    bool is_and = e->as.binary.operator_kind == SOL_TOKEN_AMP_AMP;
    branch.true_edge = predicate_edge(l, branch_block, is_and ? rhs : shortcut,
        SOL_MIR_OPERATION_NONE);
    branch.false_edge = predicate_edge(l, branch_block, is_and ? shortcut : rhs,
        SOL_MIR_OPERATION_NONE);
    if (branch.true_edge == SOL_MIR_OPERATION_NONE
        || branch.false_edge == SOL_MIR_OPERATION_NONE
        || !predicate_end_block(l, branch)) return false;
    predicate_start_block(l, rhs);
    SolMirPredicateValueId right;
    if (!lower_predicate_expression(l, e->as.binary.right, depth + 1, &right))
        return false;
    if (!predicate_jump(l, join, right)) return false;
    predicate_start_block(l, shortcut);
    SolMirPredicateInstruction constant = predicate_instruction_init(
        SOL_MIR_PREDICATE_INST_BOOL, recipe);
    constant.boolean = !is_and;
    SolMirPredicateValueId short_value;
    if (!predicate_instruction(l, constant, &short_value)) return false;
    if (!predicate_jump(l, join, short_value)) return false;
    predicate_start_block(l, join); *result = parameter; return true;
}

static bool lower_predicate_if(PredicateLowerer *l, size_t expression,
    const SolIrExpression *e, size_t depth, SolMirPredicateValueId *result) {
    SolMirPredicateValueId condition;
    if (!lower_predicate_expression(l, e->as.if_expr.condition, depth + 1,
            &condition)) return false;
    size_t branch_block = l->block;
    size_t then_block = predicate_new_block(l, SOL_MIR_RECIPE_NONE, NULL);
    size_t else_block = predicate_new_block(l, SOL_MIR_RECIPE_NONE, NULL);
    SolMirRecipeId recipe = predicate_expression_recipe(
        l->builder->out->layout->representation, l->image, l->context,
        expression);
    SolMirPredicateValueId parameter;
    size_t join = predicate_new_block(l, recipe, &parameter);
    if (then_block == SOL_MIR_OPERATION_NONE || else_block == SOL_MIR_OPERATION_NONE
        || join == SOL_MIR_OPERATION_NONE)
        return false;
    SolMirPredicateTerminator branch = predicate_terminator(
        SOL_MIR_PREDICATE_TERM_BRANCH);
    branch.condition = condition;
    branch.true_edge = predicate_edge(l, branch_block, then_block,
        SOL_MIR_OPERATION_NONE);
    branch.false_edge = predicate_edge(l, branch_block, else_block,
        SOL_MIR_OPERATION_NONE);
    if (branch.true_edge == SOL_MIR_OPERATION_NONE
        || branch.false_edge == SOL_MIR_OPERATION_NONE
        || !predicate_end_block(l, branch)) return false;
    predicate_start_block(l, then_block);
    SolMirPredicateValueId then_value;
    if (!lower_predicate_expression(l, e->as.if_expr.then_branch, depth + 1,
            &then_value)) return false;
    if (!predicate_jump(l, join, then_value)) return false;
    predicate_start_block(l, else_block);
    SolMirPredicateValueId else_value;
    if (!lower_predicate_expression(l, e->as.if_expr.else_branch, depth + 1,
            &else_value)) return false;
    if (!predicate_jump(l, join, else_value)) return false;
    predicate_start_block(l, join); *result = parameter; return true;
}

static bool predicate_pattern_nodes(PredicateLowerer *l, size_t pattern_id,
    SolMirRecipeId recipe, SolMirPredicatePathStep *path, size_t depth,
    size_t *binding_count) {
    SolMirOperations *o = l->builder->out;
    const SolMirRepresentation *r = o->layout->representation;
    const SolIr *ir = r->materialization->plan->program->ir;
    if (!charge(l->builder, SOL_MIR_OPERATIONS_WORK_RECURSE)
        || pattern_id >= ir->pattern_count || depth > ir->pattern_count
        || o->predicate_pattern_node_count == SIZE_MAX) return false;
    const SolIrPattern *pattern_source = &ir->patterns[pattern_id];
    SolMirPredicatePatternNode node = {.recipe = recipe,
        .path = {o->predicate_path_step_count, depth},
        .semantic_tag = 0, .boolean = pattern_source->boolean};
    for (size_t i = 0; i < depth; ++i) {
        if (!CHARGE(l->builder)) return false;
        if (o->predicate_path_step_count == SIZE_MAX) return false;
        if (o->predicate_path_steps != NULL)
            o->predicate_path_steps[o->predicate_path_step_count] = path[i];
        ++o->predicate_path_step_count;
    }
    if (pattern_source->kind == SOL_IR_PATTERN_WILDCARD)
        node.kind = SOL_MIR_OPERATION_PATTERN_WILDCARD;
    else if (pattern_source->kind == SOL_IR_PATTERN_BINDING) {
        node.kind = SOL_MIR_OPERATION_PATTERN_BINDING; ++*binding_count;
    } else if (pattern_source->kind == SOL_IR_PATTERN_BOOL)
        node.kind = SOL_MIR_OPERATION_PATTERN_BOOL;
    else if (pattern_source->kind == SOL_IR_PATTERN_VARIANT) {
        node.kind = SOL_MIR_OPERATION_PATTERN_SUM_TAG;
        size_t variant = variant_by_source(l->builder, r, recipe,
            pattern_source->variant);
        if (variant >= r->variant_count) return false;
        node.semantic_tag = o->layout->variants[variant].tag;
    } else if (pattern_source->kind == SOL_IR_PATTERN_RECORD
        || pattern_source->kind == SOL_IR_PATTERN_TUPLE)
        node.kind = SOL_MIR_OPERATION_PATTERN_PRODUCT;
    else return false;
    if (o->predicate_pattern_nodes != NULL)
        o->predicate_pattern_nodes[o->predicate_pattern_node_count] = node;
    ++o->predicate_pattern_node_count;
    for (size_t i = 0; i < pattern_source->children.count; ++i) {
        if (!CHARGE(l->builder)) return false;
        const SolIrPatternChild *child
            = &ir->pattern_children[pattern_source->children.offset + i];
        size_t ordinal = child->ordinal;
        if (child->field != SOL_IR_NONE)
            ordinal = child->field
                - ir->definitions[pattern_source->definition].fields.offset;
        size_t variant = pattern_source->kind == SOL_IR_PATTERN_VARIANT
            ? variant_by_source(l->builder, r, recipe, pattern_source->variant)
            : SOL_MIR_OPERATION_NONE;
        size_t field = field_by_ordinal(l->builder, r, recipe, variant, ordinal);
        if (field >= r->field_count || depth >= ir->pattern_count) return false;
        path[depth] = (SolMirPredicatePathStep){recipe, r->fields[field].type,
            field};
        if (!predicate_pattern_nodes(l, child->pattern, r->fields[field].type,
                path, depth + 1, binding_count)) return false;
    }
    return true;
}

static bool predicate_extract_bindings(PredicateLowerer *l, size_t root,
    SolMirPredicateValueId scrutinee, SolMirRecipeId recipe,
    SolMirPredicatePathStep *path, size_t depth, const SolIrArm *arm,
    size_t *leaf) {
    SolMirOperations *o = l->builder->out;
    const SolMirRepresentation *r = o->layout->representation;
    const SolIr *ir = r->materialization->plan->program->ir;
    if (!charge(l->builder, SOL_MIR_OPERATIONS_WORK_RECURSE)
        || root >= ir->pattern_count || depth > ir->pattern_count) return false;
    const SolIrPattern *pattern = &ir->patterns[root];
    if (pattern->kind == SOL_IR_PATTERN_BINDING) {
        if (*leaf >= arm->bindings.count) return false;
        SolMirPlanSlice result = {o->predicate_path_step_count, depth};
        for (size_t i = 0; i < depth; ++i) {
            if (!CHARGE(l->builder) || o->predicate_path_step_count == SIZE_MAX)
                return false;
            if (o->predicate_path_steps != NULL)
                o->predicate_path_steps[o->predicate_path_step_count] = path[i];
            ++o->predicate_path_step_count;
        }
        SolMirPredicateInstruction extraction = predicate_instruction_init(
            SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT,
            predicate_use_recipe(r, l->image, l->import, l->context,
                SOL_MIR_PLAN_USE_PATTERN, root, 0));
        extraction.left = scrutinee;
        extraction.path = result;
        SolMirPredicateValueId value;
        size_t local = ir->roots[arm->bindings.offset + *leaf];
        if (local >= ir->local_count || l->local_bound[local]
            || !predicate_instruction(l, extraction, &value)) return false;
        l->local_bound[local] = 1;
        l->locals[local] = value;
        ++*leaf;
    }
    for (size_t i = 0; i < pattern->children.count; ++i) {
        if (!CHARGE(l->builder)) return false;
        const SolIrPatternChild *child
            = &ir->pattern_children[pattern->children.offset + i];
        size_t ordinal = child->ordinal;
        if (child->field != SOL_IR_NONE)
            ordinal = child->field - ir->definitions[pattern->definition].fields.offset;
        size_t variant = pattern->kind == SOL_IR_PATTERN_VARIANT
            ? variant_by_source(l->builder, r, recipe, pattern->variant)
            : SOL_MIR_OPERATION_NONE;
        size_t field = field_by_ordinal(l->builder, r, recipe, variant, ordinal);
        if (field >= r->field_count || depth >= ir->pattern_count) return false;
        path[depth] = (SolMirPredicatePathStep){recipe, r->fields[field].type,
            field};
        if (!predicate_extract_bindings(l, child->pattern, scrutinee,
                r->fields[field].type, path, depth + 1, arm, leaf)) return false;
    }
    return true;
}

static bool lower_predicate_match(PredicateLowerer *l, size_t expression,
    const SolIrExpression *e, size_t depth, SolMirPredicateValueId *result) {
    SolMirOperations *o = l->builder->out;
    const SolIr *ir = o->layout->representation->materialization->plan->program->ir;
    SolMirPredicateValueId scrutinee;
    if (!lower_predicate_expression(l, e->as.match_expr.scrutinee, depth + 1,
            &scrutinee)) return false;
    SolMirRecipeId scrutinee_recipe = predicate_expression_recipe(
        o->layout->representation, l->image, l->context,
        e->as.match_expr.scrutinee);
    size_t join = predicate_new_block(l,
        predicate_expression_recipe(o->layout->representation, l->image,
            l->context, expression), result);
    if (join == SOL_MIR_OPERATION_NONE) return false;
    size_t next = l->block;
    for (size_t arm_ordinal = 0; arm_ordinal < e->as.match_expr.arms.count;
        ++arm_ordinal) {
        if (!CHARGE(l->builder)) return false;
        if (arm_ordinal != 0) predicate_start_block(l, next);
        size_t arm_id = ir->arm_ids[e->as.match_expr.arms.offset + arm_ordinal];
        const SolIrArm *arm = &ir->arms[arm_id];
        SolMirRecipeId bool_recipe = SOL_MIR_RECIPE_NONE;
        for (size_t q = 0; q < o->layout->representation->recipe_count; ++q) {
            if (!CHARGE(l->builder)) return false;
            if (o->layout->representation->recipes[q].kind
                    == SOL_MIR_RECIPE_BOOL) bool_recipe = q;
        }
        if (bool_recipe == SOL_MIR_RECIPE_NONE) return false;
        SolMirPredicateInstruction test = predicate_instruction_init(
            SOL_MIR_PREDICATE_INST_PATTERN_TEST, bool_recipe);
        test.left = scrutinee;
        test.pattern.offset = o->predicate_pattern_node_count;
        SolMirPredicatePathStep *stack = l->builder->path_stack == NULL ? NULL
            : (SolMirPredicatePathStep *)(void *)l->builder->path_stack;
        size_t bindings = 0;
        if (!predicate_pattern_nodes(l, arm->pattern, scrutinee_recipe, stack, 0,
                &bindings)) return false;
        test.pattern.count = o->predicate_pattern_node_count - test.pattern.offset;
        SolMirPredicateValueId matched;
        if (!predicate_instruction(l, test, &matched)) return false;
        size_t arm_block = predicate_new_block(l, SOL_MIR_RECIPE_NONE, NULL);
        size_t miss_block = predicate_new_block(l, SOL_MIR_RECIPE_NONE, NULL);
        if (arm_block == SOL_MIR_OPERATION_NONE || miss_block == SOL_MIR_OPERATION_NONE)
            return false;
        SolMirPredicateTerminator branch = predicate_terminator(
            SOL_MIR_PREDICATE_TERM_BRANCH);
        branch.condition = matched;
        branch.true_edge = predicate_edge(l, next, arm_block, SOL_MIR_OPERATION_NONE);
        branch.false_edge = predicate_edge(l, next, miss_block, SOL_MIR_OPERATION_NONE);
        if (!predicate_end_block(l, branch)) return false;
        predicate_start_block(l, arm_block);
        size_t leaf = 0;
        if (!predicate_extract_bindings(l, arm->pattern, scrutinee,
                scrutinee_recipe, stack, 0, arm, &leaf)) return false;
        if (leaf != arm->bindings.count) return false;
        if (arm->guard != SOL_IR_NONE) {
            SolMirPredicateValueId guard;
            if (!lower_predicate_expression(l, arm->guard, depth + 1, &guard))
                return false;
            size_t body_block = predicate_new_block(l, SOL_MIR_RECIPE_NONE, NULL);
            if (body_block == SOL_MIR_OPERATION_NONE) return false;
            branch = predicate_terminator(SOL_MIR_PREDICATE_TERM_BRANCH);
            branch.condition = guard;
            branch.true_edge = predicate_edge(l, l->block, body_block,
                SOL_MIR_OPERATION_NONE);
            branch.false_edge = predicate_edge(l, l->block, miss_block,
                SOL_MIR_OPERATION_NONE);
            if (!predicate_end_block(l, branch)) return false;
            predicate_start_block(l, body_block);
        }
        SolMirPredicateValueId arm_value;
        if (!lower_predicate_expression(l, arm->body, depth + 1, &arm_value))
            return false;
        SolMirPredicateTerminator jump = predicate_terminator(
            SOL_MIR_PREDICATE_TERM_JUMP);
        jump.edge = predicate_edge(l, l->block, join, arm_value);
        if (!predicate_end_block(l, jump)) return false;
        for (size_t i = 0; i < arm->bindings.count; ++i) {
            if (!CHARGE(l->builder)) return false;
            size_t local = ir->roots[arm->bindings.offset + i];
            l->local_bound[local] = 0;
        }
        next = miss_block;
    }
    predicate_start_block(l, next);
    SolMirPredicateTerminator failure = predicate_terminator(
        SOL_MIR_PREDICATE_TERM_FAILURE);
    failure.failure_kind = SOL_MIR_PREDICATE_FAILURE_NO_MATCH;
    if (!predicate_end_block(l, failure)) return false;
    predicate_start_block(l, join);
    return true;
}

static bool lower_predicate_expression(PredicateLowerer *l, size_t expression,
    size_t depth, SolMirPredicateValueId *result) {
    SolMirOperations *o = l->builder->out;
    const SolMirMaterialization *m = o->layout->representation->materialization;
    const SolMirRepresentation *r = o->layout->representation;
    const SolIr *ir = m->plan->program->ir;
    if (!charge(l->builder, SOL_MIR_OPERATIONS_WORK_RECURSE)
        || expression >= ir->expression_count || depth > ir->expression_count)
        return false;
    const SolIrExpression *e = &ir->expressions[expression];
    SolMirRecipeId recipe = predicate_expression_recipe(r, l->image, l->context,
        expression);
    if (recipe == SOL_MIR_RECIPE_NONE) return false;
    if (e->kind == SOL_IR_EXPR_PLACE) return lower_predicate_place(l, expression,
        depth, result);
    if (e->kind == SOL_IR_EXPR_RESULT) {
        const SolIrObligation *ob = &ir->obligations[l->obligation];
        return predicate_input_expression(l, expression,
            ob->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                ? SOL_MIR_PREDICATE_INPUT_SUCCESS_RESULT
                : SOL_MIR_PREDICATE_INPUT_COMPLETE_RESULT,
            0, SOL_ACCESS_OWNED, result);
    }
    if (e->kind == SOL_IR_EXPR_SNAPSHOT_READ) {
        size_t ordinal;
        if (!predicate_snapshot_slot(&ir->obligations[l->obligation],
                e->as.snapshot, &ordinal) || !add_size(&ordinal, l->snapshot_base))
            return false;
        return predicate_input_expression(l, expression,
            SOL_MIR_PREDICATE_INPUT_SNAPSHOT, ordinal, SOL_ACCESS_OWNED, result);
    }
    if (e->kind == SOL_IR_EXPR_REFINEMENT_SELF)
        return predicate_input_expression(l, expression,
            SOL_MIR_PREDICATE_INPUT_REFINEMENT_SELF, 0, SOL_ACCESS_OWNED, result);
    if (e->kind == SOL_IR_EXPR_BINARY
        && (e->as.binary.operator_kind == SOL_TOKEN_AMP_AMP
            || e->as.binary.operator_kind == SOL_TOKEN_PIPE_PIPE))
        return lower_predicate_short_circuit(l, expression, e, depth, result);
    if (e->kind == SOL_IR_EXPR_IF)
        return lower_predicate_if(l, expression, e, depth, result);
    if (e->kind == SOL_IR_EXPR_MATCH)
        return lower_predicate_match(l, expression, e, depth, result);
    if (e->kind == SOL_IR_EXPR_BLOCK) {
        size_t count = e->as.block.statements.count;
        size_t completed = 0;
        if (count == 0) {
            SolMirPredicateInstruction unit = predicate_instruction_init(
                SOL_MIR_PREDICATE_INST_UNIT, recipe);
            if (!predicate_instruction(l, unit, result)) goto block_failed;
        }
        for (size_t i = 0; i < count; ++i) {
            if (!CHARGE(l->builder)) goto block_failed;
            const SolIrStatement *statement = &ir->statements[
                ir->statement_ids[e->as.block.statements.offset + i]];
            if (statement->kind != SOL_IR_STATEMENT_LET
                && statement->kind != SOL_IR_STATEMENT_EXPRESSION
                && statement->kind != SOL_IR_STATEMENT_RETURN) goto block_failed;
            if (!lower_predicate_expression(l, statement->expression, depth + 1,
                    result)) goto block_failed;
            if (statement->kind == SOL_IR_STATEMENT_LET) {
                if (statement->local >= ir->local_count
                    || l->local_bound[statement->local]) goto block_failed;
                l->locals[statement->local] = *result;
                l->local_bound[statement->local] = 1;
            }
            completed = i + 1;
            if (statement->kind == SOL_IR_STATEMENT_RETURN) break;
        }
        for (size_t i = 0; i < completed; ++i) {
            if (!CHARGE(l->builder)) goto block_failed;
            const SolIrStatement *statement = &ir->statements[
                ir->statement_ids[e->as.block.statements.offset + i]];
            if (statement->kind == SOL_IR_STATEMENT_LET)
                l->local_bound[statement->local] = 0;
        }
        return true;
block_failed:
        for (size_t i = 0; i < completed; ++i) {
            if (!CHARGE(l->builder)) return false;
            const SolIrStatement *statement = &ir->statements[
                ir->statement_ids[e->as.block.statements.offset + i]];
            if (statement->kind == SOL_IR_STATEMENT_LET)
                l->local_bound[statement->local] = 0;
        }
        return false;
    }
    if (e->kind == SOL_IR_EXPR_DEFINITION
        || e->kind == SOL_IR_EXPR_BOUND_OPERATION) {
        SolMirPredicateInstruction instruction = predicate_instruction_init(
            e->kind == SOL_IR_EXPR_DEFINITION ? SOL_MIR_PREDICATE_INST_FUNCTION
                : SOL_MIR_PREDICATE_INST_BOUND_OPERATION, recipe);
        instruction.binding = predicate_binding(l, expression,
            e->kind == SOL_IR_EXPR_DEFINITION
                ? SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE
                : SOL_MIR_PLAN_DEMAND_BOUND_OPERATION,
            e->kind == SOL_IR_EXPR_DEFINITION
                ? SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE
                : SOL_MIR_PLAN_DEMAND_BOUND_OPERATION);
        if (instruction.binding == SOL_MIR_MATERIALIZED_NONE) return false;
        if (e->kind == SOL_IR_EXPR_BOUND_OPERATION
            && !lower_predicate_expression(l, e->as.operation.receiver,
                depth + 1, &instruction.left)) return false;
        return predicate_instruction(l, instruction, result);
    }
    if (e->kind == SOL_IR_EXPR_CALL && e->as.call.kind <= SOL_IR_CALL_METHOD) {
        SolMirPredicateValueId callee = SOL_MIR_OPERATION_NONE;
        SolMirPredicateValueId receiver = SOL_MIR_OPERATION_NONE;
        if ((e->as.call.kind == SOL_IR_CALL_CALLBACK
                || e->as.call.kind == SOL_IR_CALL_CAPABILITY)
            && !lower_predicate_expression(l, e->as.call.callee, depth + 1,
                &callee)) return false;
        if (e->as.call.kind == SOL_IR_CALL_METHOD
            && !lower_predicate_expression(l, e->as.call.receiver, depth + 1,
                &receiver)) return false;
        size_t arguments = o->predicate_operand_count;
        for (size_t i = 0; i < e->as.call.operands.count; ++i) {
            if (!CHARGE(l->builder)) return false;
            const SolIrOperand *operand
                = &ir->operands[e->as.call.operands.offset + i];
            if (operand->access == SOL_ACCESS_EXCLUSIVE) return false;
            SolMirPredicateValueId value;
            if (!lower_predicate_expression(l, operand->value, depth + 1, &value)
                || o->predicate_operand_count == SIZE_MAX) return false;
            if (o->predicate_operands != NULL)
                o->predicate_operands[o->predicate_operand_count]
                    = (SolMirPredicateOperand){value, operand->formal,
                        operand->access, SOL_MIR_OPERATION_NONE};
            ++o->predicate_operand_count;
        }
        SolMirMaterializedBindingId binding = predicate_binding(l, expression,
            SOL_MIR_PLAN_DEMAND_PREDICATE, SOL_MIR_PLAN_DEMAND_CALLBACK);
        if (binding == SOL_MIR_MATERIALIZED_NONE
            || m->effect_rows[m->bindings[binding].target_kind
                    == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
                ? m->images[m->bindings[binding].instance].effects
                : m->imports[m->bindings[binding].import].effects].atoms.count != 0)
            return false;
        size_t source_block = l->block;
        SolMirPredicateValueId produced;
        if (!predicate_value(l, SOL_MIR_PREDICATE_VALUE_TERMINATOR, recipe,
                source_block, source_block, &produced)) return false;
        SolMirPredicateValueId parameter;
        size_t normal = predicate_new_block(l, recipe, &parameter);
        size_t failure;
        if (normal == SOL_MIR_OPERATION_NONE
            || !predicate_failure_block(l, SOL_MIR_PREDICATE_FAILURE_CALL,
                &failure)) return false;
        SolMirPredicateTerminator invoke = predicate_terminator(
            SOL_MIR_PREDICATE_TERM_INVOKE);
        invoke.call_kind = e->as.call.kind; invoke.binding = binding;
        invoke.effects = m->bindings[binding].target_kind
            == SOL_MIR_MATERIALIZED_TARGET_INSTANCE
            ? m->images[m->bindings[binding].instance].effects
            : m->imports[m->bindings[binding].import].effects;
        invoke.callee = callee; invoke.receiver = receiver;
        invoke.receiver_access = e->as.call.receiver_access;
        invoke.arguments = (SolMirPlanSlice){arguments, e->as.call.operands.count};
        invoke.result = produced; invoke.result_recipe = recipe;
        invoke.normal_edge = predicate_edge(l, source_block, normal, produced);
        invoke.failure_edge = predicate_edge(l, source_block, failure,
            SOL_MIR_OPERATION_NONE);
        if (!predicate_end_block(l, invoke)) return false;
        predicate_start_block(l, normal); *result = parameter; return true;
    }
    if (e->kind == SOL_IR_EXPR_PROPAGATE) {
        SolMirPredicateValueId operand;
        if (!lower_predicate_expression(l, e->as.propagate.operand, depth + 1,
                &operand)) return false;
        SolMirRecipeId source_recipe = predicate_expression_recipe(r, l->image,
            l->context, e->as.propagate.operand);
        size_t success = variant_by_ordinal(l->builder, r, source_recipe,
            e->as.propagate.kind == SOL_IR_PROPAGATE_OPTION ? 1 : 0);
        size_t residual = variant_by_ordinal(l->builder, r, source_recipe,
            e->as.propagate.kind == SOL_IR_PROPAGATE_OPTION ? 0 : 1);
        size_t field = field_by_ordinal(l->builder, r, source_recipe, success, 0);
        if (success >= r->variant_count || residual >= r->variant_count
            || field >= r->field_count) return false;
        size_t source_block = l->block;
        SolMirPredicateValueId produced;
        if (!predicate_value(l, SOL_MIR_PREDICATE_VALUE_TERMINATOR, recipe,
                source_block, source_block, &produced)) return false;
        SolMirPredicateValueId parameter;
        size_t normal = predicate_new_block(l, recipe, &parameter), failure;
        if (normal == SOL_MIR_OPERATION_NONE || !predicate_failure_block(l,
                SOL_MIR_PREDICATE_FAILURE_PROPAGATION, &failure)) return false;
        SolMirPredicateTerminator term = predicate_terminator(
            SOL_MIR_PREDICATE_TERM_PROPAGATE);
        term.value = operand; term.result = produced;
        term.propagation_kind = e->as.propagate.kind;
        term.success_variant_layout = success;
        term.residual_variant_layout = residual;
        term.success_field_layout = field; term.result_recipe = recipe;
        term.normal_edge = predicate_edge(l, source_block, normal, produced);
        term.failure_edge = predicate_edge(l, source_block, failure,
            SOL_MIR_OPERATION_NONE);
        if (!predicate_end_block(l, term)) return false;
        predicate_start_block(l, normal); *result = parameter; return true;
    }
    SolMirPredicateValueId left = SOL_MIR_OPERATION_NONE;
    SolMirPredicateValueId right = SOL_MIR_OPERATION_NONE;
    SolMirPredicateInstruction instruction = predicate_instruction_init(
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
        size_t offset = o->literal_byte_count;
        if (!add_size(&o->literal_byte_count, length)) return false;
        instruction.bytes = (SolMirPlanSlice){offset, length};
        if (o->literal_bytes != NULL) {
            if (offset > o->literal_byte_capacity
                || length > o->literal_byte_capacity - offset) return false;
            memcpy(o->literal_bytes + offset, e->as.string, length);
        }
    } else if (e->kind == SOL_IR_EXPR_UNARY) {
        instruction.kind = SOL_MIR_PREDICATE_INST_UNARY;
        if (!lower_predicate_expression(l, e->as.unary.operand, depth + 1, &left)) return false;
        instruction.left = left;
        instruction.opcode = opcode(SOL_MIR_INST_UNARY,
            e->as.unary.operator_kind, &instruction.failures);
        if ((int)instruction.opcode < 0) return false;
    } else if (e->kind == SOL_IR_EXPR_BINARY) {
        instruction.kind = SOL_MIR_PREDICATE_INST_BINARY;
        if (!lower_predicate_expression(l, e->as.binary.left, depth + 1, &left)
            || !lower_predicate_expression(l, e->as.binary.right, depth + 1, &right)) return false;
        instruction.left = left; instruction.right = right;
        instruction.opcode = opcode(SOL_MIR_INST_BINARY,
            e->as.binary.operator_kind, &instruction.failures);
        if ((int)instruction.opcode < 0) return false;
    } else if (e->kind == SOL_IR_EXPR_RECORD || e->kind == SOL_IR_EXPR_TUPLE
        || e->kind == SOL_IR_EXPR_VARIANT
        || (e->kind == SOL_IR_EXPR_CALL && e->as.call.kind
            >= SOL_IR_CALL_BUILTIN_OK)) {
        SolIrSlice source_slice = e->kind == SOL_IR_EXPR_RECORD
            ? e->as.record.fields : e->kind == SOL_IR_EXPR_TUPLE
            ? e->as.tuple.operands : e->kind == SOL_IR_EXPR_VARIANT
            ? (SolIrSlice){0, 0} : e->as.call.operands;
        SolMirPlanSlice source_operands = {source_slice.offset,
            source_slice.count};
        instruction.kind = SOL_MIR_PREDICATE_INST_CONSTRUCT;
        instruction.construct_kind = e->kind == SOL_IR_EXPR_RECORD
            ? SOL_MIR_PREDICATE_CONSTRUCT_RECORD : e->kind == SOL_IR_EXPR_TUPLE
            ? SOL_MIR_PREDICATE_CONSTRUCT_TUPLE
            : SOL_MIR_PREDICATE_CONSTRUCT_SUM;
        if (e->kind == SOL_IR_EXPR_CALL
            && e->as.call.kind == SOL_IR_CALL_DISTINCT_CONSTRUCTOR) {
            SolIrDefinitionId definition = e->as.call.definition;
            if (definition >= ir->definition_count) return false;
            if (ir->definitions[definition].kind == SOL_IR_DEFINITION_REFINED) {
                SolMirPredicateValueId backing;
                if (source_operands.count != 1
                    || !lower_predicate_expression(l,
                        ir->operands[source_operands.offset].value, depth + 1,
                        &backing)) return false;
                SolObligationId nested_obligation = SOL_IR_NONE;
                for (size_t i = 0; i < ir->obligation_count; ++i) {
                    if (!CHARGE(l->builder)) return false;
                    if (ir->obligations[i].owner_kind == SOL_CONTRACT_OWNER_TYPE
                        && ir->obligations[i].owner == definition)
                        nested_obligation = i;
                }
                size_t nested_context = SOL_MIR_OPERATION_NONE;
                for (size_t i = 0; i < m->context_count; ++i) {
                    if (!CHARGE(l->builder)) return false;
                    if (m->contexts[i].kind == SOL_MIR_PLAN_CONTEXT_REFINEMENT
                        && m->contexts[i].definition == definition
                        && m->contexts[i].obligation == nested_obligation
                        && m->contexts[i].refinement_type == recipe
                        && m->contexts[i].target_kind
                            == (l->import == SOL_MIR_OPERATION_NONE
                                ? SOL_MIR_PLAN_TARGET_INSTANCE
                                : SOL_MIR_PLAN_TARGET_IMPORT)
                        && (l->import == SOL_MIR_OPERATION_NONE
                            ? m->contexts[i].instance == l->image
                            : m->contexts[i].import == l->import)) {
                        if (nested_context != SOL_MIR_OPERATION_NONE
                            && m->contexts[i].source.expression == expression)
                            return false;
                        if (m->contexts[i].source.expression == expression)
                            nested_context = i;
                    }
                }
                if (nested_obligation == SOL_IR_NONE
                    || nested_context == SOL_MIR_OPERATION_NONE) return false;
                SolMirPredicateBodyId nested_body = SOL_MIR_OPERATION_NONE;
                for (size_t i = 0; i < o->predicate_body_count; ++i) {
                    if (!CHARGE(l->builder)) return false;
                    const SolMirPredicateBody *body = &o->predicate_bodies[i];
                    if (body->context == nested_context
                        && body->refinement_self_recipe
                            == r->recipes[recipe].backing) nested_body = i;
                }
                if (nested_body == SOL_MIR_OPERATION_NONE) return false;
                size_t source_block = l->block;
                SolMirPredicateValueId produced;
                if (!predicate_value(l, SOL_MIR_PREDICATE_VALUE_TERMINATOR,
                        recipe, source_block, source_block, &produced)) return false;
                SolMirPredicateValueId parameter;
                size_t normal = predicate_new_block(l, recipe, &parameter), failure;
                if (normal == SOL_MIR_OPERATION_NONE || !predicate_failure_block(l,
                        SOL_MIR_PREDICATE_FAILURE_REFINEMENT, &failure)) return false;
                SolMirPredicateTerminator check = predicate_terminator(
                    SOL_MIR_PREDICATE_TERM_CHECK_REFINED);
                check.value = backing; check.result = produced;
                check.nested_body = nested_body; check.result_recipe = recipe;
                check.normal_edge = predicate_edge(l, source_block, normal, produced);
                check.failure_edge = predicate_edge(l, source_block, failure,
                    SOL_MIR_OPERATION_NONE);
                if (!predicate_end_block(l, check)) return false;
                predicate_start_block(l, normal); *result = parameter; return true;
            }
            instruction.construct_kind = SOL_MIR_PREDICATE_CONSTRUCT_WRAPPER;
        }
        if (e->kind == SOL_IR_EXPR_VARIANT
            || (e->kind == SOL_IR_EXPR_CALL && e->as.call.kind
                == SOL_IR_CALL_ENUM_CONSTRUCTOR)) {
            size_t source_variant = e->kind == SOL_IR_EXPR_VARIANT
                ? e->as.variant.variant : e->as.call.variant;
            instruction.variant_layout = variant_by_source(l->builder, r, recipe,
                source_variant);
        } else if (e->kind == SOL_IR_EXPR_CALL
            && (e->as.call.kind == SOL_IR_CALL_BUILTIN_NONE
                || e->as.call.kind == SOL_IR_CALL_BUILTIN_SOME
                || e->as.call.kind == SOL_IR_CALL_BUILTIN_OK
                || e->as.call.kind == SOL_IR_CALL_BUILTIN_ERR)) {
            size_t ordinal = e->as.call.kind == SOL_IR_CALL_BUILTIN_NONE ? 0
                : e->as.call.kind == SOL_IR_CALL_BUILTIN_SOME ? 1
                : e->as.call.kind == SOL_IR_CALL_BUILTIN_OK ? 0 : 1;
            instruction.variant_layout = variant_by_ordinal(l->builder, r, recipe,
                ordinal);
        }
        if (instruction.variant_layout != SOL_MIR_OPERATION_NONE)
            instruction.semantic_tag
                = o->layout->variants[instruction.variant_layout].tag;
        instruction.operands.offset = o->predicate_operand_count;
        for (size_t i = 0; i < source_operands.count; ++i) {
            if (!CHARGE(l->builder)) return false;
            const SolIrOperand *operand = &ir->operands[source_operands.offset + i];
            SolMirPredicateValueId value;
            if (!lower_predicate_expression(l, operand->value, depth + 1, &value))
                return false;
            size_t field = SOL_MIR_OPERATION_NONE;
            if (instruction.construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_RECORD) {
                for (size_t f = 0; f < r->recipes[recipe].fields.count; ++f) {
                    if (!CHARGE(l->builder)) return false;
                    size_t candidate = r->recipes[recipe].fields.offset + f;
                    if (r->fields[candidate].source_field == operand->formal)
                        field = candidate;
                }
            } else if (instruction.construct_kind
                    == SOL_MIR_PREDICATE_CONSTRUCT_TUPLE)
                field = field_by_ordinal(l->builder, r, recipe,
                    SOL_MIR_OPERATION_NONE, operand->formal);
            else if (instruction.construct_kind == SOL_MIR_PREDICATE_CONSTRUCT_SUM)
                field = field_by_ordinal(l->builder, r, recipe,
                    instruction.variant_layout, i);
            if (instruction.construct_kind != SOL_MIR_PREDICATE_CONSTRUCT_WRAPPER
                && source_operands.count != 0 && field >= r->field_count) return false;
            if (o->predicate_operands != NULL)
                o->predicate_operands[o->predicate_operand_count]
                    = (SolMirPredicateOperand){value, operand->formal,
                        operand->access, field};
            ++o->predicate_operand_count;
        }
        instruction.operands.count = o->predicate_operand_count
            - instruction.operands.offset;
    } else return false;
    return predicate_instruction(l, instruction, result);
}

static bool reserve_predicate_body(Builder *b, size_t image, size_t context,
    SolObligationId obligation, SolMirRecipeId refinement_self_recipe,
    SolMirPredicateBodyId *result) {
    SolMirOperations *o = b->out;
    const SolMirMaterialization *m
        = o->layout->representation->materialization;
    const SolIr *ir = m->plan->program->ir;
    if (obligation >= ir->obligation_count) return false;
    const SolIrObligation *source = &ir->obligations[obligation];
    size_t import = m->contexts[context].target_kind == SOL_MIR_PLAN_TARGET_IMPORT
        ? m->contexts[context].import : SOL_MIR_OPERATION_NONE;
    for (size_t i = 0; i < o->predicate_body_count; ++i) {
        if (!CHARGE(b)) return false;
        const SolMirPredicateBody *body = &o->predicate_bodies[i];
        if (body->owner_kind == (import == SOL_MIR_OPERATION_NONE
                ? SOL_MIR_PREDICATE_OWNER_INSTANCE
                : SOL_MIR_PREDICATE_OWNER_IMPORT)
            && body->instance == image && body->import == import
            && body->context == context
            && body->phase == source->kind && body->outcome == source->outcome
            && body->refinement_self_recipe == refinement_self_recipe) {
            *result = i; return true;
        }
    }
    if (o->predicate_body_count == SIZE_MAX) return false;
    size_t body = o->predicate_body_count++;
    SolMirPredicateBody value = {
        .owner_kind = import == SOL_MIR_OPERATION_NONE
            ? SOL_MIR_PREDICATE_OWNER_INSTANCE : SOL_MIR_PREDICATE_OWNER_IMPORT,
        .instance = image, .import = import, .context = context,
        .phase = source->kind, .outcome = source->outcome,
        .entry = SOL_MIR_OPERATION_NONE,
        .output_recipe = predicate_expression_recipe(o->layout->representation,
            image, context, source->predicate),
        .refinement_self_recipe = refinement_self_recipe};
    if (o->predicate_bodies != NULL) o->predicate_bodies[body] = value;
    *result = body; return true;
}

static bool lower_reserved_predicate_body(Builder *b, size_t body) {
    SolMirOperations *o = b->out;
    const SolMirMaterialization *m = o->layout->representation->materialization;
    const SolIr *ir = m->plan->program->ir;
    SolMirPredicateBody key = o->predicate_bodies != NULL
        ? o->predicate_bodies[body] : (SolMirPredicateBody){0};
    if (o->predicate_bodies == NULL) return false;
    size_t image = key.instance, context = key.context, import = key.import;
    SolObligationId obligation = m->contexts[context].obligation;
    const SolIrObligation *source = &ir->obligations[obligation];
    size_t input_start = o->predicate_input_count;
    size_t value_start = o->predicate_value_count;
    size_t block_start = o->predicate_block_count;
    SolMirPlanSlice owner_contexts = import == SOL_MIR_OPERATION_NONE
        ? m->images[image].contexts : m->imports[import].contexts;
    size_t snapshot_base = 0;
    for (size_t i = 0; i < owner_contexts.count; ++i) {
        if (!CHARGE(b)) return false;
        size_t candidate = owner_contexts.offset + i;
        if (candidate == context) break;
        if (m->contexts[candidate].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT) continue;
        size_t prior = m->contexts[candidate].obligation;
        if (prior >= ir->obligation_count
            || !add_size(&snapshot_base, ir->obligations[prior].snapshots.count))
            return false;
    }
    if (ir->local_count != 0
        && (b->predicate_locals == NULL || b->predicate_local_bound == NULL))
        return false;
    if (ir->local_count != 0) {
        memset(b->predicate_locals, 0,
            ir->local_count * sizeof(*b->predicate_locals));
        memset(b->predicate_local_bound, 0, ir->local_count);
    }
    PredicateLowerer lowerer = {b, image, import, context, body,
        SOL_MIR_OPERATION_NONE, snapshot_base, obligation,
        b->predicate_locals, b->predicate_local_bound};
    size_t block = predicate_new_block(&lowerer, SOL_MIR_RECIPE_NONE, NULL);
    if (block == SOL_MIR_OPERATION_NONE) return false;
    predicate_start_block(&lowerer, block);
    SolMirPredicateValueId value;
    bool ok = lower_predicate_expression(&lowerer, source->predicate, 0, &value);
    if (!ok) return false;
    SolMirPredicateTerminator term = predicate_terminator(
        SOL_MIR_PREDICATE_TERM_RETURN);
    term.value = value;
    if (!predicate_end_block(&lowerer, term)) return false;
    SolMirRecipeId output = predicate_expression_recipe(
        o->layout->representation, image, context, source->predicate);
    o->predicate_bodies[body].inputs = (SolMirPlanSlice){input_start,
        o->predicate_input_count - input_start};
    o->predicate_bodies[body].blocks = (SolMirPlanSlice){block_start,
        o->predicate_block_count - block_start};
    o->predicate_bodies[body].values = (SolMirPlanSlice){value_start,
        o->predicate_value_count - value_start};
    o->predicate_bodies[body].entry = block;
    o->predicate_bodies[body].output_recipe = output;
    return output < o->layout->representation->recipe_count
        && o->layout->representation->recipes[output].kind == SOL_MIR_RECIPE_BOOL;
}

static bool prepare_predicate_bodies(Builder *b) {
    SolMirOperations *o = b->out;
    const SolMirMaterialization *m = o->layout->representation->materialization;
    const SolMirRepresentation *r = o->layout->representation;
    const SolIr *ir = m->plan->program->ir;
    for (size_t context = 0; context < m->context_count; ++context) {
        if (!CHARGE(b)) return false;
        const SolMirPlanContext *source = &m->contexts[context];
        if (source->kind != SOL_MIR_PLAN_CONTEXT_CONTRACT
            && source->kind != SOL_MIR_PLAN_CONTEXT_REFINEMENT) continue;
        size_t image = source->target_kind == SOL_MIR_PLAN_TARGET_INSTANCE
            ? source->instance : SOL_MIR_OPERATION_NONE;
        SolIrDefinitionId definition = source->kind == SOL_MIR_PLAN_CONTEXT_REFINEMENT
            ? source->definition : SOL_IR_NONE;
        SolMirRecipeId self_recipe = SOL_MIR_RECIPE_NONE;
        if (definition != SOL_IR_NONE) {
            SolMirRecipeId result_recipe = source->refinement_type;
            if (result_recipe >= r->recipe_count
                || r->recipes[result_recipe].kind != SOL_MIR_RECIPE_REFINED)
                return false;
            self_recipe = r->recipes[result_recipe].backing;
        }
        SolMirPredicateBodyId body;
        if (!reserve_predicate_body(b, image, context, source->obligation,
                self_recipe, &body)) return false;
    }
    for (size_t body = 0; body < o->predicate_body_count; ++body) {
        if (!CHARGE(b) || !lower_reserved_predicate_body(b, body)) return false;
    }
    (void)ir;
    return true;
}

static SolMirPredicateBodyId predicate_body_for_context(
    const SolMirOperations *o, size_t context) {
    for (size_t i = 0; i < o->predicate_body_count; ++i)
        if (o->predicate_bodies[i].context == context) return i;
    return SOL_MIR_OPERATION_NONE;
}

static bool count_predicate_graph(const SolMirLayout *layout, Counts *counts,
    Builder *builder) {
    const SolMirMaterialization *m = layout->representation->materialization;
    SolMirOperations temporary;
    sol_mir_operations_init(&temporary);
    temporary.layout = layout;
    temporary.limits = builder->out->limits;
    temporary.predicate_body_capacity = m->context_count;
    if (m->context_count != 0
        && !charge(builder, SOL_MIR_OPERATIONS_WORK_ALLOCATE)) return false;
    temporary.predicate_bodies = m->context_count == 0 ? NULL
        : calloc(m->context_count, sizeof(*temporary.predicate_bodies));
    if (m->context_count != 0 && temporary.predicate_bodies == NULL) return false;
    const SolIr *ir = m->plan->program->ir;
    if (ir->pattern_count != 0
        && !charge(builder, SOL_MIR_OPERATIONS_WORK_ALLOCATE)) {
        free(temporary.predicate_bodies); return false;
    }
    SolMirOperationPathStep *path_stack = ir->pattern_count == 0 ? NULL
        : calloc(ir->pattern_count, sizeof(*path_stack));
    if (ir->pattern_count != 0 && path_stack == NULL) {
        free(temporary.predicate_bodies); return false;
    }
    SolMirPredicateValueId *locals = NULL;
    unsigned char *bound = NULL;
    if (ir->local_count != 0) {
        if (!charge(builder, 2 * SOL_MIR_OPERATIONS_WORK_ALLOCATE)) {
            free(path_stack); free(temporary.predicate_bodies); return false;
        }
        locals = calloc(ir->local_count, sizeof(*locals));
        bound = calloc(ir->local_count, 1);
        if (locals == NULL || bound == NULL) {
            free(locals); free(bound); free(path_stack);
            free(temporary.predicate_bodies); return false;
        }
    }
    Builder dry = {.out = &temporary, .diagnostics = builder->diagnostics,
        .outcome = builder->outcome, .path_stack = path_stack,
        .predicate_locals = locals, .predicate_local_bound = bound,
        .actual_work = builder->actual_work};
    if (!prepare_predicate_bodies(&dry)) {
        builder->outcome = dry.outcome;
        builder->actual_work = dry.actual_work;
        free(locals); free(bound); free(path_stack);
        free(temporary.predicate_bodies); return false;
    }
    builder->actual_work = dry.actual_work;
    counts->predicate_bodies = temporary.predicate_body_count;
    counts->predicate_blocks = temporary.predicate_block_count;
    counts->predicate_inputs = temporary.predicate_input_count;
    counts->predicate_values = temporary.predicate_value_count;
    counts->predicate_instructions = temporary.predicate_instruction_count;
    counts->predicate_edges = temporary.predicate_edge_count;
    counts->predicate_edge_values = temporary.predicate_edge_value_count;
    counts->predicate_operands = temporary.predicate_operand_count;
    counts->predicate_path_steps = temporary.predicate_path_step_count;
    counts->predicate_pattern_nodes = temporary.predicate_pattern_node_count;
    counts->literal_bytes = temporary.literal_byte_count;
    free(locals); free(bound); free(path_stack); free(temporary.predicate_bodies);
    return true;
}

static bool populate(Builder *b) {
    SolMirOperations *o = b->out;
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    if (!prepare_predicate_bodies(b)) return false;
    for (size_t body = 0; body < o->predicate_body_count; ++body) {
        const SolMirPlanContext *context
            = &m->contexts[o->predicate_bodies[body].context];
        if (!add_provenance(b, (SolMirOperationProvenance){
                .kind = SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY,
                .executable = body,
                .source_expression = SOL_IR_NONE,
                .source_pattern = SOL_IR_NONE,
                .source_field = SOL_IR_NONE,
                .source_variant = SOL_IR_NONE,
                .source_obligation = context->obligation,
                .source_snapshot = SOL_IR_NONE,
                .source_definition = context->kind
                        == SOL_MIR_PLAN_CONTEXT_REFINEMENT
                    ? context->definition : SOL_IR_NONE})) return false;
    }
    for (size_t p = 0; p < m->place_count; ++p) {
        if (!charge(b, 1)) return false;
        const SolMirMaterializedPlace *place = &m->places[p];
        size_t at = o->access_step_count;
        for (size_t i = 0; i < place->projections.count; ++i) {
            if (!charge(b, 1)) return false;
            const SolMirProjectionMap *map
                = &o->layout->projections[place->projections.offset + i];
            o->access_steps[o->access_step_count++] = (SolMirOperationAccessStep){
                map->projection, map->base_recipe, map->result_recipe,
                map->field_layout, map->object_offset};
        }
        o->access_plans[o->access_plan_count++] = (SolMirOperationAccessPlan){
            p, image_for_place(b, m, p), place->local, place->root_type,
            place->final_type, {at, place->projections.count}};
    }
    for (size_t i = 0; i < m->instruction_count; ++i) {
        if (!charge(b, 1)) return false;
        const SolMirMaterializedInstruction *x = &m->instructions[i];
        size_t image = image_for_instruction(b, m, i);
        if (image == SOL_MIR_OPERATION_NONE) return false;
        if (x->kind == SOL_MIR_INST_CONSTRUCT) {
            size_t recipe = x->type, variant = SOL_MIR_OPERATION_NONE;
            SolMirOperationConstructKind kind;
            size_t ordinal = 0;
            switch (x->construct_kind) {
                case SOL_MIR_CONSTRUCT_RECORD: kind = SOL_MIR_OPERATION_CONSTRUCT_RECORD; break;
                case SOL_MIR_CONSTRUCT_CAPABILITY: kind = SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY; break;
                case SOL_MIR_CONSTRUCT_TUPLE: kind = SOL_MIR_OPERATION_CONSTRUCT_TUPLE; break;
                case SOL_MIR_CONSTRUCT_ENUM: kind = SOL_MIR_OPERATION_CONSTRUCT_SUM;
                    variant = variant_by_source(b, r, recipe, x->construct_variant); break;
                case SOL_MIR_CONSTRUCT_OPTION_NONE: kind = SOL_MIR_OPERATION_CONSTRUCT_SUM; ordinal = 0;
                    variant = variant_by_ordinal(b, r, recipe, ordinal); break;
                case SOL_MIR_CONSTRUCT_OPTION_SOME: kind = SOL_MIR_OPERATION_CONSTRUCT_SUM; ordinal = 1;
                    variant = variant_by_ordinal(b, r, recipe, ordinal); break;
                case SOL_MIR_CONSTRUCT_RESULT_OK: kind = SOL_MIR_OPERATION_CONSTRUCT_SUM; ordinal = 0;
                    variant = variant_by_ordinal(b, r, recipe, ordinal); break;
                case SOL_MIR_CONSTRUCT_RESULT_ERR: kind = SOL_MIR_OPERATION_CONSTRUCT_SUM; ordinal = 1;
                    variant = variant_by_ordinal(b, r, recipe, ordinal); break;
                case SOL_MIR_CONSTRUCT_DISTINCT: kind = SOL_MIR_OPERATION_CONSTRUCT_WRAPPER; break;
                default: return false;
            }
            if (recipe >= r->recipe_count || (kind == SOL_MIR_OPERATION_CONSTRUCT_SUM
                    && variant >= r->variant_count)) return false;
            size_t operand_at = o->construct_operand_count;
            SolMirOperationCapabilityRule rule = SOL_MIR_OPERATION_CAPABILITY_NONE;
            size_t source_operand = SOL_MIR_OPERATION_NONE;
            size_t inherited_root = SOL_MIR_MATERIALIZED_NONE;
            for (size_t j = 0; j < x->construct_operands.count; ++j) {
                if (!charge(b, 1)) return false;
                const SolMirMaterializedConstructOperand *operand
                    = &m->construct_operands[x->construct_operands.offset + j];
                size_t field = SOL_MIR_OPERATION_NONE;
                if (kind == SOL_MIR_OPERATION_CONSTRUCT_RECORD)
                    for (size_t f = 0; f < r->recipes[recipe].fields.count; ++f) {
                        if (!charge(b, 1)) return false;
                        size_t id = r->recipes[recipe].fields.offset + f;
                        if (r->fields[id].source_field == operand->formal) field = id;
                    }
                else if (kind == SOL_MIR_OPERATION_CONSTRUCT_TUPLE)
                    field = field_by_ordinal(b, r, recipe,
                        SOL_MIR_OPERATION_NONE, operand->formal);
                else if (kind == SOL_MIR_OPERATION_CONSTRUCT_SUM
                    && x->construct_kind == SOL_MIR_CONSTRUCT_ENUM) {
                    SolMirPlanSlice fields = r->variants[variant].fields;
                    for (size_t f = 0; f < fields.count; ++f) {
                        if (!charge(b, 1)) return false;
                        size_t id = fields.offset + f;
                        if (r->fields[id].source_field == operand->formal) field = id;
                    }
                } else if (kind == SOL_MIR_OPERATION_CONSTRUCT_SUM)
                    field = field_by_ordinal(b, r, recipe, variant, j);
                uint64_t offset = 0;
                if (field != SOL_MIR_OPERATION_NONE) offset = o->layout->fields[field].offset;
                else if (kind == SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY)
                    offset = o->layout->types[recipe].private_source_handle_offset;
                size_t formal_ordinal = field == SOL_MIR_OPERATION_NONE
                    ? operand->formal : r->fields[field].ordinal;
                o->construct_operands[o->construct_operand_count++]
                    = (SolMirOperationConstructOperand){formal_ordinal, j,
                        operand->temporary, operand->type, field, field, offset};
                if (kind == SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY) {
                    source_operand = j;
                    const SolMirRecipe *source_recipe = &r->recipes[operand->type];
                    rule = source_recipe->capability_source == SOL_MIR_RECIPE_NONE
                        ? SOL_MIR_OPERATION_CAPABILITY_ROOT_SOURCE
                        : operand->type == r->recipes[recipe].capability_source
                            ? SOL_MIR_OPERATION_CAPABILITY_BASE_SOURCE
                            : SOL_MIR_OPERATION_CAPABILITY_PRIVATE_SOURCE;
                }
            }
            if (kind == SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
                && x->source_capability_roots.count == 1) {
                SolIrLocalId source_root = ir->roots[x->source_capability_roots.offset];
                for (size_t q = 0; q < m->images[image].locals.count; ++q) {
                    if (!charge(b, 1)) return false;
                    size_t id = m->images[image].locals.offset + q;
                    if (m->locals[id].source_local == source_root) inherited_root = id;
                }
            }
            if (kind == SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
                && (x->construct_operands.count != 1
                    || inherited_root == SOL_MIR_MATERIALIZED_NONE
                    || source_operand == SOL_MIR_OPERATION_NONE)) return false;
            SolMirRecipeId backing = kind == SOL_MIR_OPERATION_CONSTRUCT_WRAPPER
                ? r->recipes[recipe].backing : SOL_MIR_RECIPE_NONE;
            uint32_t tag = variant == SOL_MIR_OPERATION_NONE ? 0
                : o->layout->variants[variant].tag;
            size_t executable = o->constructor_count;
            o->constructors[o->constructor_count++] = (SolMirOperationConstructPlan){
                image, i, x->result, kind, recipe, o->layout->types[recipe].object_kind,
                variant, tag, {operand_at, x->construct_operands.count}, backing,
                rule, source_operand, inherited_root};
            if (!add_provenance(b, (SolMirOperationProvenance){
                    SOL_MIR_OPERATION_PROVENANCE_CONSTRUCT, executable,
                    x->source_expression, SOL_IR_NONE, SOL_IR_NONE,
                    x->construct_variant, SOL_IR_NONE, SOL_IR_NONE,
                    SOL_IR_NONE})) return false;
        } else if (x->kind == SOL_MIR_INST_PATTERN_TEST) {
            size_t node_at = o->pattern_node_count;
            if (!append_pattern(b, image, x->source_pattern, NULL, 0))
                return fail(b, SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
                    "pattern test could not be flattened");
            size_t executable = o->pattern_test_count;
            o->pattern_tests[o->pattern_test_count++] = (SolMirOperationPatternTest){
                image, i, x->pattern_scrutinee,
                m->temporaries[x->pattern_scrutinee].type, x->result,
                {node_at, o->pattern_node_count - node_at}};
            if (!add_provenance(b, (SolMirOperationProvenance){
                    SOL_MIR_OPERATION_PROVENANCE_PATTERN_TEST, executable,
                    x->match_expression, x->source_pattern, SOL_IR_NONE,
                    SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE,
                    SOL_IR_NONE})) return false;
        } else if (x->kind == SOL_MIR_INST_PATTERN_VALUE) {
            if (x->source_arm >= ir->arm_count) return false;
            size_t at = o->path_step_count, count = 0;
            SolMirRecipeId root = m->temporaries[x->pattern_scrutinee].type;
            if (!append_extraction_path(b, image, ir->arms[x->source_arm].pattern,
                    x->source_pattern, root, 0, &at, &count))
                return fail(b, SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
                    "pattern extraction path could not be flattened");
            size_t executable = o->pattern_extraction_count;
            o->pattern_extractions[o->pattern_extraction_count++]
                = (SolMirOperationPatternExtraction){image, i, x->pattern_scrutinee,
                    root, x->result, x->type, {at, count}, r->recipes[x->type].copy_kind};
            if (!add_provenance(b, (SolMirOperationProvenance){
                    SOL_MIR_OPERATION_PROVENANCE_PATTERN_EXTRACTION, executable,
                    x->match_expression, x->source_pattern, SOL_IR_NONE,
                    SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE,
                    SOL_IR_NONE})) return false;
        } else if (x->kind == SOL_MIR_INST_UNARY || x->kind == SOL_MIR_INST_BINARY
            || x->kind == SOL_MIR_INST_COMPOUND_UPDATE) {
            unsigned failures;
            SolMirOperationOpcode op = opcode(x->kind, x->operator_kind, &failures);
            if ((int)op < 0) return false;
            SolMirMaterializedValueId left = x->left;
            SolMirRecipeId operand_recipe = x->kind == SOL_MIR_INST_COMPOUND_UPDATE
                ? m->temporaries[x->previous].type : m->values[left].type;
            SolMirPlanSlice equality = {0};
            if ((op == SOL_MIR_OPERATION_VALUE_EQ || op == SOL_MIR_OPERATION_VALUE_NE)
                && !append_equality(b, operand_recipe, &equality)) return false;
            size_t executable = o->arithmetic_count;
            o->arithmetic[o->arithmetic_count++] = (SolMirOperationArithmeticPlan){
                image, i, op, left, x->right, x->previous, operand_recipe,
                x->result, x->type, failures,
                x->kind == SOL_MIR_INST_COMPOUND_UPDATE, equality};
            if (!add_provenance(b, (SolMirOperationProvenance){
                    SOL_MIR_OPERATION_PROVENANCE_ARITHMETIC, executable,
                    x->source_expression, SOL_IR_NONE, SOL_IR_NONE,
                    SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE,
                    SOL_IR_NONE})) return false;
        } else if (x->kind == SOL_MIR_INST_CAPTURE_SNAPSHOT) {
            if (x->source_snapshot >= ir->snapshot_count) return false;
            const SolIrExpression *operand
                = &ir->expressions[ir->snapshots[x->source_snapshot].operand];
            if (operand->kind != SOL_IR_EXPR_PLACE) return false;
            size_t access = SOL_MIR_OPERATION_NONE;
            for (size_t p = m->images[image].places.offset;
                p < m->images[image].places.offset + m->images[image].places.count; ++p) {
                if (!charge(b, 1)) return false;
                if (m->places[p].source_place == operand->as.place) { access = p; break; }
            }
            const SolIrPlace *source_place = &ir->places[operand->as.place];
            size_t local = SOL_MIR_MATERIALIZED_NONE;
            for (size_t q = 0; q < m->images[image].locals.count; ++q) {
                if (!charge(b, 1)) return false;
                size_t id = m->images[image].locals.offset + q;
                if (m->locals[id].source_local == source_place->local) { local = id; break; }
            }
            SolMirRecipeId current = local == SOL_MIR_MATERIALIZED_NONE
                ? SOL_MIR_RECIPE_NONE : m->locals[local].type;
            size_t path_at = o->path_step_count;
            for (size_t q = 0; q < source_place->projections.count; ++q) {
                if (!charge(b, 1)) return false;
                const SolIrProjection *projection
                    = &ir->projections[source_place->projections.offset + q];
                size_t field = SOL_MIR_OPERATION_NONE;
                if (projection->kind == SOL_IR_PROJECTION_FIELD) {
                    SolMirPlanSlice fields = r->recipes[current].fields;
                    for (size_t f = 0; f < fields.count; ++f) {
                        if (!charge(b, 1)) return false;
                        size_t id = fields.offset + f;
                        if (r->fields[id].source_field == projection->field) field = id;
                    }
                } else if (projection->kind == SOL_IR_PROJECTION_TUPLE_FIELD) {
                    field = field_by_ordinal(b, r, current, SOL_MIR_OPERATION_NONE,
                        projection->ordinal);
                }
                if (field >= r->field_count) return false;
                o->path_steps[o->path_step_count++] = (SolMirOperationPathStep){
                    current, r->fields[field].type, field,
                    o->layout->fields[field].offset};
                current = r->fields[field].type;
            }
            if (local == SOL_MIR_MATERIALIZED_NONE || current != x->type
                || !r->recipes[x->type].is_copy)
                return fail(b, SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
                    "snapshot operand has no concrete Copy access");
            size_t provenance = o->provenance_count;
            size_t executable = o->snapshot_count;
            size_t context = SOL_MIR_OPERATION_NONE;
            for (size_t q = 0; q < m->images[image].contexts.count; ++q) {
                if (!charge(b, 1)) return false;
                size_t id = m->images[image].contexts.offset + q;
                if (m->contexts[id].kind == SOL_MIR_PLAN_CONTEXT_CONTRACT
                    && m->contexts[id].obligation
                        == ir->snapshots[x->source_snapshot].obligation) {
                    if (context != SOL_MIR_OPERATION_NONE) return false;
                    context = id;
                }
            }
            if (context == SOL_MIR_OPERATION_NONE) return false;
            if (!add_provenance(b, (SolMirOperationProvenance){
                    SOL_MIR_OPERATION_PROVENANCE_SNAPSHOT, executable,
                    ir->snapshots[x->source_snapshot].operand, SOL_IR_NONE,
                    SOL_IR_NONE, SOL_IR_NONE,
                    ir->snapshots[x->source_snapshot].obligation,
                    x->source_snapshot, SOL_IR_NONE})) return false;
            size_t local_slot = 0;
            for (size_t q = 0; q < o->snapshot_count; ++q) {
                if (!charge(b, 1)) return false;
                local_slot += o->snapshots[q].image == image;
            }
            o->snapshots[o->snapshot_count++] = (SolMirOperationSnapshotPlan){
                image, i, local_slot, context, access, local,
                m->locals[local].type,
                {path_at, source_place->projections.count}, x->type,
                r->recipes[x->type].copy_kind, provenance};
        }
    }
    for (size_t i = 0; i < m->block_count; ++i) {
        if (!charge(b, 1)) return false;
        const SolMirMaterializedTerminator *t = &m->blocks[i].terminator;
        size_t image = image_for_block(b, m, i);
        if (t->kind == SOL_MIR_TERM_PROPAGATE) {
            size_t source_recipe = m->temporaries[t->operand].type;
            size_t success_variant = variant_by_ordinal(b, r, source_recipe,
                t->propagation_kind == SOL_IR_PROPAGATE_OPTION ? 1 : 0);
            size_t residual_source_variant = variant_by_ordinal(b, r, source_recipe,
                t->propagation_kind == SOL_IR_PROPAGATE_OPTION ? 0 : 1);
            size_t residual_recipe = m->values[t->residual_result].type;
            size_t residual_variant = variant_by_ordinal(b, r, residual_recipe,
                t->propagation_kind == SOL_IR_PROPAGATE_OPTION ? 0 : 1);
            if (success_variant >= r->variant_count
                || residual_source_variant >= r->variant_count
                || residual_variant >= r->variant_count) return false;
            size_t sf = field_by_ordinal(b, r, source_recipe, success_variant, 0);
            size_t source_rf = t->propagation_kind == SOL_IR_PROPAGATE_OPTION
                ? SOL_MIR_OPERATION_NONE
                : field_by_ordinal(b, r, source_recipe,
                    residual_source_variant, 0);
            size_t destination_rf = t->propagation_kind == SOL_IR_PROPAGATE_OPTION
                ? SOL_MIR_OPERATION_NONE
                : field_by_ordinal(b, r, residual_recipe, residual_variant, 0);
            if (sf >= r->field_count
                || (t->propagation_kind == SOL_IR_PROPAGATE_RESULT
                    && (source_rf >= r->field_count
                        || destination_rf >= r->field_count))) return false;
            size_t executable = o->propagation_count;
            o->propagations[o->propagation_count++] = (SolMirOperationPropagationPlan){
                .image = image, .block = i, .source = t->operand,
                .source_recipe = source_recipe, .success_result = t->value_result,
                .success_recipe = m->values[t->value_result].type,
                .residual_result = t->residual_result,
                .residual_recipe = residual_recipe,
                .success_variant_layout = success_variant,
                .success_tag = o->layout->variants[success_variant].tag,
                .source_residual_variant_layout = residual_source_variant,
                .source_residual_tag
                    = o->layout->variants[residual_source_variant].tag,
                .destination_residual_variant_layout = residual_variant,
                .destination_residual_tag = o->layout->variants[residual_variant].tag,
                .success_field_layout = sf,
                .success_field_offset = o->layout->fields[sf].offset,
                .source_residual_field_layout = source_rf,
                .source_residual_field_recipe = source_rf == SOL_MIR_OPERATION_NONE
                    ? SOL_MIR_RECIPE_NONE : r->fields[source_rf].type,
                .source_residual_field_offset = source_rf == SOL_MIR_OPERATION_NONE
                    ? SOL_MIR_LAYOUT_OFFSET_NONE : o->layout->fields[source_rf].offset,
                .destination_residual_field_layout = destination_rf,
                .destination_residual_field_recipe
                    = destination_rf == SOL_MIR_OPERATION_NONE
                    ? SOL_MIR_RECIPE_NONE : r->fields[destination_rf].type,
                .destination_residual_field_offset
                    = destination_rf == SOL_MIR_OPERATION_NONE
                    ? SOL_MIR_LAYOUT_OFFSET_NONE : o->layout->fields[destination_rf].offset,
                .success_edge = t->value_edge, .residual_edge = t->residual_edge};
            if (!add_provenance(b, (SolMirOperationProvenance){
                    SOL_MIR_OPERATION_PROVENANCE_PROPAGATION, executable,
                    t->source_expression, SOL_IR_NONE, SOL_IR_NONE,
                    SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE,
                    SOL_IR_NONE})) return false;
        } else if (t->kind == SOL_MIR_TERM_CHECK_CONTRACT
            || t->kind == SOL_MIR_TERM_CHECK_REFINED) {
            size_t context = SOL_MIR_OPERATION_NONE;
            for (size_t q = 0; q < m->images[image].contexts.count; ++q) {
                if (!charge(b, 1)) return false;
                size_t id = m->images[image].contexts.offset + q;
                const SolMirPlanContext *cx = &m->contexts[id];
                if (cx->obligation == t->source_obligation
                    && ((t->kind == SOL_MIR_TERM_CHECK_CONTRACT
                            && cx->kind == SOL_MIR_PLAN_CONTEXT_CONTRACT)
                        || (t->kind == SOL_MIR_TERM_CHECK_REFINED
                            && cx->kind == SOL_MIR_PLAN_CONTEXT_REFINEMENT))) {
                    if (context != SOL_MIR_OPERATION_NONE) return false;
                    context = id;
                }
            }
            size_t bool_recipe = SOL_MIR_RECIPE_NONE;
            for (size_t q = 0; q < r->recipe_count; ++q) {
                if (!charge(b, 1)) return false;
                if (r->recipes[q].kind == SOL_MIR_RECIPE_BOOL) bool_recipe = q;
            }
            if (context == SOL_MIR_OPERATION_NONE || bool_recipe == SOL_MIR_RECIPE_NONE)
                return fail(b, SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
                    "predicate has no exact context or Bool output recipe");
            size_t provenance = o->provenance_count, executable = o->predicate_count;
            if (!add_provenance(b, (SolMirOperationProvenance){
                    SOL_MIR_OPERATION_PROVENANCE_PREDICATE, executable,
                    t->source_expression, SOL_IR_NONE, SOL_IR_NONE,
                    SOL_IR_NONE, t->source_obligation, SOL_IR_NONE,
                    SOL_IR_NONE})) return false;
            SolMirPredicateBodyId body = predicate_body_for_context(o, context);
            if (body == SOL_MIR_OPERATION_NONE)
                return fail(b, SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
                    "predicate cannot be lowered to a concrete monomorphic body");
            SolMirOperationPredicatePlan plan = {
                .kind = t->kind == SOL_MIR_TERM_CHECK_CONTRACT
                    ? SOL_MIR_OPERATION_PREDICATE_CONTRACT
                    : SOL_MIR_OPERATION_PREDICATE_REFINEMENT,
                .body = body,
                .image = image, .block = i, .context = context,
                .representation = t->representation,
                .input_recipe = SOL_MIR_RECIPE_NONE, .result = t->result,
                .result_recipe = t->result == SOL_MIR_MATERIALIZED_NONE
                    ? SOL_MIR_RECIPE_NONE
                    : m->values[t->result].type,
                .output_recipe = bool_recipe,
                .contract_phase = t->kind == SOL_MIR_TERM_CHECK_CONTRACT
                    ? t->contract_phase : (SolContractClauseKind)0,
                .contract_outcome = t->kind == SOL_MIR_TERM_CHECK_CONTRACT
                    ? t->contract_outcome : (SolContractOutcomeKind)0,
                .provenance = provenance};
            if (t->kind == SOL_MIR_TERM_CHECK_REFINED)
                plan.input_recipe = m->temporaries[t->representation].type;
            o->predicates[o->predicate_count++] = plan;
        }
    }
    for (size_t i = 0; i < r->callable_producer_count; ++i) {
        if (!charge(b, 1)) return false;
        const SolMirCallableProducer *p = &r->callable_producers[i];
        if (p->captured_receiver_kind == SOL_MIR_MATERIALIZED_RECEIVER_SOURCE_EXPRESSION)
            return fail(b, SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
                "callable producer receiver has no concrete materialized capture");
        SolMirOperationCaptureKind capture = SOL_MIR_OPERATION_CAPTURE_NONE;
        size_t access = SOL_MIR_OPERATION_NONE;
        if (p->captured_receiver_kind == SOL_MIR_MATERIALIZED_RECEIVER_PLACE) {
            capture = SOL_MIR_OPERATION_CAPTURE_PLACE; access = p->captured_receiver_place;
        } else if (p->captured_receiver_kind == SOL_MIR_MATERIALIZED_RECEIVER_TEMPORARY)
            capture = SOL_MIR_OPERATION_CAPTURE_TEMPORARY;
        else if (p->captured_receiver_kind == SOL_MIR_MATERIALIZED_RECEIVER_VALUE)
            capture = SOL_MIR_OPERATION_CAPTURE_VALUE;
        size_t roots = o->root_count;
        for (size_t q = 0; q < p->captured_receiver_roots.count; ++q) {
            if (!charge(b, 1)) return false;
            o->roots[o->root_count++] = r->receiver_roots[p->captured_receiver_roots.offset + q];
        }
        size_t executable = o->callable_count;
        o->callables[o->callable_count++] = (SolMirOperationCallablePlan){
            p->semantic_site, p->kind, p->function_recipe, p->target_kind,
            p->instance, p->import, capture, access, p->captured_receiver_temporary,
            p->captured_receiver_value, p->captured_receiver_instruction,
            p->captured_receiver_type, p->effects, {roots, p->captured_receiver_roots.count}};
        if (!add_provenance(b, (SolMirOperationProvenance){
                SOL_MIR_OPERATION_PROVENANCE_CALLABLE, executable,
                p->captured_receiver_expression, SOL_IR_NONE, SOL_IR_NONE,
                SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE,
                SOL_IR_NONE})) return false;
    }
    for (size_t i = 0; i < m->import_count; ++i) {
        const SolMirMaterializedImport *source = &m->imports[i];
        size_t requires = o->import_contract_reference_count;
        size_t snapshots = o->import_snapshot_count;
        size_t import_slot = 0;
        for (size_t q = 0; q < source->contexts.count; ++q) {
            size_t context = source->contexts.offset + q;
            if (m->contexts[context].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT)
                continue;
            const SolIrObligation *obligation
                = &ir->obligations[m->contexts[context].obligation];
            for (size_t s = 0; s < obligation->snapshots.count; ++s) {
                size_t snapshot = obligation->snapshots.offset + s;
                const SolIrExpression *operand
                    = &ir->expressions[ir->snapshots[snapshot].operand];
                if (operand->kind != SOL_IR_EXPR_PLACE
                    || operand->as.place >= ir->place_count) return false;
                if (ir->places[operand->as.place].root_kind
                        != SOL_IR_PLACE_ROOT_LOCAL
                    || ir->places[operand->as.place].projections.count != 0)
                    return false;
                SolIrLocalId local = ir->places[operand->as.place].local;
                const SolIrCallable *callable
                    = &ir->callables[source->source_callable];
                SolMirPredicateInputKind kind;
                size_t ordinal = 0; SolAccessMode access = SOL_ACCESS_OWNED;
                if (local == callable->receiver) {
                    kind = SOL_MIR_PREDICATE_INPUT_RECEIVER;
                    access = callable->receiver_access;
                } else {
                    kind = SOL_MIR_PREDICATE_INPUT_PARAMETER;
                    bool found = false;
                    for (size_t p = 0; p < callable->parameters.count; ++p)
                        if (ir->roots[callable->parameters.offset + p] == local) {
                            ordinal = p; access = ir->locals[local].access;
                            found = true; break;
                        }
                    if (!found) return false;
                }
                SolMirRecipeId recipe = predicate_expression_recipe(r,
                    SOL_MIR_OPERATION_NONE, context,
                    ir->snapshots[snapshot].operand);
                if (recipe >= r->recipe_count) return false;
                size_t executable = o->import_snapshot_count;
                size_t provenance = o->provenance_count;
                if (!add_provenance(b, (SolMirOperationProvenance){
                        SOL_MIR_OPERATION_PROVENANCE_IMPORT_SNAPSHOT,
                        executable, ir->snapshots[snapshot].operand,
                        SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE,
                        ir->snapshots[snapshot].obligation, snapshot,
                        SOL_IR_NONE})) return false;
                o->import_snapshots[o->import_snapshot_count++]
                    = (SolMirImportSnapshotCapture){i, context,
                        import_slot++, kind, ordinal, recipe, access, provenance};
            }
        }
        for (size_t q = 0; q < source->contexts.count; ++q) {
            size_t context = source->contexts.offset + q;
            if (m->contexts[context].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT)
                continue;
            size_t obligation = m->contexts[context].obligation;
            if (ir->obligations[obligation].kind != SOL_CONTRACT_REQUIRES)
                continue;
            SolMirPredicateBodyId body = predicate_body_for_context(o, context);
            if (body == SOL_MIR_OPERATION_NONE) return false;
            o->import_contract_references[o->import_contract_reference_count++]
                = body;
        }
        size_t require_count = o->import_contract_reference_count - requires;
        size_t ensures = o->import_contract_reference_count;
        for (size_t q = 0; q < source->contexts.count; ++q) {
            size_t context = source->contexts.offset + q;
            if (m->contexts[context].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT)
                continue;
            size_t obligation = m->contexts[context].obligation;
            if (ir->obligations[obligation].kind != SOL_CONTRACT_ENSURES)
                continue;
            SolMirPredicateBodyId body = predicate_body_for_context(o, context);
            if (body == SOL_MIR_OPERATION_NONE) return false;
            o->import_contract_references[o->import_contract_reference_count++]
                = body;
        }
        o->import_envelopes[o->import_envelope_count++]
            = (SolMirImportContractEnvelope){
                .import = i, .receiver = source->receiver,
                .receiver_access = source->receiver_access,
                .parameters = source->parameter_types,
                .parameter_accesses = source->parameter_accesses,
                .result = source->result, .effects = source->effects,
                .requires = {requires, require_count},
                .snapshots = {snapshots,
                    o->import_snapshot_count - snapshots},
                .ensures = {ensures,
                    o->import_contract_reference_count - ensures},
                .host_invoke = true};
    }
    for (size_t i = 0; i < m->handler_count; ++i) {
        if (!charge(b, 1)) return false;
        const SolMirMaterializedHandler *h = &m->handlers[i];
        const SolMirMaterializedBinding *binding = &m->bindings[h->provider_binding];
        SolMirRecipeId receiver, result; SolMirPlanSlice parameters;
        if (binding->target_kind == SOL_MIR_MATERIALIZED_TARGET_INSTANCE) {
            const SolMirMaterializedImage *target = &m->images[binding->instance];
            receiver = target->receiver; result = target->result;
            parameters = target->parameter_types;
        } else {
            const SolMirMaterializedImport *target = &m->imports[binding->import];
            receiver = target->receiver; result = target->result;
            parameters = target->parameter_types;
        }
        size_t at = o->recipe_id_count;
        for (size_t q = 0; q < parameters.count; ++q) {
            if (!charge(b, 1)) return false;
            o->recipe_ids[o->recipe_id_count++] = m->type_ids[parameters.offset + q];
        }
        size_t executable = o->handler_count;
        o->handlers[o->handler_count++] = (SolMirOperationHandlerPlan){
            i, h->parent, handler_parent(b, m, i), h->source_binding,
            h->provider_binding, h->authority, h->provider, h->operation,
            receiver, {at, parameters.count}, result,
            SOL_MIR_OPERATION_ROOT_TOKEN_EQUAL, h->operation.effects};
        if (!add_provenance(b, (SolMirOperationProvenance){
                SOL_MIR_OPERATION_PROVENANCE_HANDLER, executable,
                h->source_expression, SOL_IR_NONE, SOL_IR_NONE,
                SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE,
                SOL_IR_NONE})) return false;
    }
    return true;
}

static bool within_limits(const SolMirOperationsLimits *l, const Counts *c,
    size_t places, size_t projections) {
    return places <= l->max_access_plans && projections <= l->max_access_steps
        && c->constructors <= l->max_constructors
        && c->construct_operands <= l->max_construct_operands
        && c->tests <= l->max_pattern_tests
        && c->extractions <= l->max_pattern_extractions
        && c->nodes <= l->max_pattern_nodes && c->paths <= l->max_path_steps
        && c->propagations <= l->max_propagations
        && c->arithmetic <= l->max_arithmetic
        && c->equality_nodes <= l->max_equality_nodes
        && c->equality_children <= l->max_equality_children
        && c->snapshots <= l->max_snapshots && c->callables <= l->max_callables
        && c->handlers <= l->max_handlers && c->predicates <= l->max_predicates
        && c->predicate_bodies <= l->max_predicate_bodies
        && c->predicate_blocks <= l->max_predicate_blocks
        && c->predicate_inputs <= l->max_predicate_inputs
        && c->predicate_values <= l->max_predicate_values
        && c->predicate_instructions <= l->max_predicate_instructions
        && c->predicate_edges <= l->max_predicate_edges
        && c->predicate_edge_values <= l->max_predicate_edge_values
        && c->predicate_operands <= l->max_predicate_operands
        && c->predicate_path_steps <= l->max_predicate_path_steps
        && c->predicate_pattern_nodes <= l->max_predicate_pattern_nodes
        && c->import_envelopes <= l->max_import_envelopes
        && c->import_contract_references <= l->max_import_contract_references
        && c->import_snapshots <= l->max_import_snapshots
        && c->literal_bytes <= l->max_literal_bytes
        && c->recipe_ids <= l->max_recipe_ids && c->roots <= l->max_roots
        && c->provenance <= l->max_provenance;
}

SolMirOperationsBuildOutcome sol_mir_operations_build(
    const SolMirOperationsBuildRequest *request, SolMirOperations *output,
    SolDiagnostics *diagnostics) {
    if (request == NULL || output == NULL || !owner_empty(output)
        || request->layout == NULL || (request->limits != NULL
            && !limits_zero(*request->limits)
            && !limits_complete(*request->limits))) {
        error(diagnostics, "invalid operations build request or destination");
        return SOL_MIR_OPERATIONS_BUILD_INVALID_ARGUMENT;
    }
    if (!sol_mir_layout_validate(request->layout, diagnostics))
        return SOL_MIR_OPERATIONS_BUILD_INVALID_LAYOUT;
    SolMirOperations scratch; sol_mir_operations_init(&scratch);
    scratch.layout = request->layout;
    scratch.limits = request->limits == NULL || limits_zero(*request->limits)
        ? sol_mir_operations_default_limits() : *request->limits;
    Builder b = {.out = &scratch, .diagnostics = diagnostics,
        .outcome = SOL_MIR_OPERATIONS_BUILD_INTERNAL_FAILED};
    if (request->layout->usage.validation_work
            > scratch.limits.max_validation_work
        || request->layout->usage.validation_scratch_bytes
            > scratch.limits.max_validation_scratch_bytes) {
        fail(&b, SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED,
            "operations prerequisite validation resource limit exceeded");
        goto failed;
    }
    const SolMirRepresentation *r = request->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
    Counts c;
    if (!count_all(request->layout, &c, &b)) {
        if (b.outcome == SOL_MIR_OPERATIONS_BUILD_INTERNAL_FAILED)
            fail(&b, SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
                "unsupported source-semantic operation cannot be flattened");
        goto failed;
    }
    if (!within_limits(&scratch.limits, &c, m->place_count, m->projection_count)) {
        fail(&b, SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED,
            "operations arena limit exceeded"); goto failed;
    }
    size_t path_scratch, local_values_scratch, local_scratch, dry_scratch;
    if (!mul_size(ir->pattern_count, sizeof(SolMirOperationPathStep),
            &path_scratch)
        || !mul_size(ir->local_count, sizeof(SolMirPredicateValueId),
            &local_values_scratch)
        || !add_size(&local_values_scratch, ir->local_count)) {
        fail(&b, SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED,
            "operations build scratch limit exceeded"); goto failed;
    }
    local_scratch = path_scratch;
    if (!add_size(&local_scratch, local_values_scratch)
        || !add_size(&local_scratch, r->recipe_count)) {
        fail(&b, SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED,
            "operations build scratch limit exceeded"); goto failed;
    }
    dry_scratch = path_scratch;
    if (!add_size(&dry_scratch, local_values_scratch)
        || m->context_count > SIZE_MAX / sizeof(SolMirPredicateBody)
        || !add_size(&dry_scratch,
            m->context_count * sizeof(SolMirPredicateBody))) {
        fail(&b, SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED,
            "operations build scratch limit exceeded"); goto failed;
    }
    scratch.usage.build_scratch_bytes = local_scratch > dry_scratch
        ? local_scratch : dry_scratch;
    if (scratch.usage.build_scratch_bytes
            > scratch.limits.max_build_scratch_bytes) {
        fail(&b, SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED,
            "operations build scratch limit exceeded"); goto failed;
    }
    size_t local_validation_scratch = c.provenance > r->recipe_count
        ? c.provenance : r->recipe_count;
    size_t predicate_validation_scratch = c.predicate_instructions;
    if (!add_size(&predicate_validation_scratch, c.predicate_values)
        || !add_size(&predicate_validation_scratch, c.predicate_edges)
        || !add_size(&predicate_validation_scratch, c.predicate_blocks)
        || !add_size(&predicate_validation_scratch, c.predicate_operands)
        || !add_size(&predicate_validation_scratch, c.predicate_path_steps)
        || !add_size(&predicate_validation_scratch, c.predicate_pattern_nodes)
        || c.predicate_blocks > SIZE_MAX / (2 * sizeof(size_t))
        || !add_size(&predicate_validation_scratch,
            c.predicate_blocks * 2 * sizeof(size_t))) {
        fail(&b, SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED,
            "operations validation resource limit exceeded"); goto failed;
    }
    if (predicate_validation_scratch > local_validation_scratch)
        local_validation_scratch = predicate_validation_scratch;
    size_t authentication_validation_scratch;
    if (!mul_size(ir->local_count, sizeof(SolMirPredicateValueId),
            &authentication_validation_scratch)
        || !add_size(&authentication_validation_scratch, ir->local_count)
        || ir->pattern_count > SIZE_MAX / sizeof(SolMirPredicatePathStep)
        || !add_size(&authentication_validation_scratch,
            ir->pattern_count * sizeof(SolMirPredicatePathStep))) {
        fail(&b, SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED,
            "operations validation resource limit exceeded"); goto failed;
    }
    if (authentication_validation_scratch > local_validation_scratch)
        local_validation_scratch = authentication_validation_scratch;
    scratch.usage.validation_scratch_bytes
        = request->layout->usage.validation_scratch_bytes
            > local_validation_scratch
        ? request->layout->usage.validation_scratch_bytes
        : local_validation_scratch;
    if (scratch.usage.validation_scratch_bytes
            > scratch.limits.max_validation_scratch_bytes) {
        fail(&b, SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED,
            "operations validation resource limit exceeded"); goto failed;
    }
    if (ir->pattern_count != 0) {
        if (!charge(&b, SOL_MIR_OPERATIONS_WORK_ALLOCATE)) goto failed;
        b.path_stack = calloc(ir->pattern_count, sizeof(*b.path_stack));
        if (b.path_stack == NULL) {
            b.outcome = SOL_MIR_OPERATIONS_BUILD_ALLOCATION_FAILED; goto failed;
        }
    }
    if (ir->local_count != 0) {
        if (!charge(&b, 2 * SOL_MIR_OPERATIONS_WORK_ALLOCATE)) goto failed;
        b.predicate_locals = calloc(ir->local_count,
            sizeof(*b.predicate_locals));
        b.predicate_local_bound = calloc(ir->local_count, 1);
        if (b.predicate_locals == NULL || b.predicate_local_bound == NULL) {
            b.outcome = SOL_MIR_OPERATIONS_BUILD_ALLOCATION_FAILED; goto failed;
        }
    }
    if (r->recipe_count != 0) {
        if (!charge(&b, SOL_MIR_OPERATIONS_WORK_ALLOCATE)) goto failed;
        b.equality_state = calloc(r->recipe_count, 1);
        if (b.equality_state == NULL) {
            b.outcome = SOL_MIR_OPERATIONS_BUILD_ALLOCATION_FAILED; goto failed;
        }
    }
#define ALLOC(member, type, singular, count_value) do { \
    scratch.member = allocate(&b, (count_value), sizeof(*scratch.member)); \
    scratch.singular##_capacity = (count_value); \
    if ((count_value) != 0 && scratch.member == NULL) goto failed; \
} while (0)
    ALLOC(access_plans, SolMirOperationAccessPlan, access_plan, m->place_count);
    ALLOC(access_steps, SolMirOperationAccessStep, access_step, m->projection_count);
    ALLOC(constructors, SolMirOperationConstructPlan, constructor, c.constructors);
    ALLOC(construct_operands, SolMirOperationConstructOperand, construct_operand, c.construct_operands);
    ALLOC(pattern_tests, SolMirOperationPatternTest, pattern_test, c.tests);
    ALLOC(pattern_extractions, SolMirOperationPatternExtraction, pattern_extraction, c.extractions);
    ALLOC(pattern_nodes, SolMirOperationPatternNode, pattern_node, c.nodes);
    ALLOC(path_steps, SolMirOperationPathStep, path_step, c.paths);
    ALLOC(propagations, SolMirOperationPropagationPlan, propagation, c.propagations);
    ALLOC(arithmetic, SolMirOperationArithmeticPlan, arithmetic, c.arithmetic);
    ALLOC(equality_nodes, SolMirOperationEqualityNode, equality_node, c.equality_nodes);
    ALLOC(equality_children, SolMirOperationEqualityChild, equality_child, c.equality_children);
    ALLOC(snapshots, SolMirOperationSnapshotPlan, snapshot, c.snapshots);
    ALLOC(callables, SolMirOperationCallablePlan, callable, c.callables);
    ALLOC(handlers, SolMirOperationHandlerPlan, handler, c.handlers);
    ALLOC(predicates, SolMirOperationPredicatePlan, predicate, c.predicates);
    ALLOC(predicate_bodies, SolMirPredicateBody, predicate_body, c.predicate_bodies);
    ALLOC(predicate_blocks, SolMirPredicateBlock, predicate_block, c.predicate_blocks);
    ALLOC(predicate_inputs, SolMirPredicateInput, predicate_input, c.predicate_inputs);
    ALLOC(predicate_values, SolMirPredicateValue, predicate_value, c.predicate_values);
    ALLOC(predicate_instructions, SolMirPredicateInstruction, predicate_instruction,
        c.predicate_instructions);
    ALLOC(predicate_edges, SolMirPredicateEdge, predicate_edge, c.predicate_edges);
    ALLOC(predicate_edge_values, SolMirPredicateValueId, predicate_edge_value,
        c.predicate_edge_values);
    ALLOC(predicate_operands, SolMirPredicateOperand, predicate_operand,
        c.predicate_operands);
    ALLOC(predicate_path_steps, SolMirPredicatePathStep, predicate_path_step,
        c.predicate_path_steps);
    ALLOC(predicate_pattern_nodes, SolMirPredicatePatternNode,
        predicate_pattern_node, c.predicate_pattern_nodes);
    ALLOC(import_envelopes, SolMirImportContractEnvelope, import_envelope,
        c.import_envelopes);
    ALLOC(import_contract_references, SolMirPredicateBodyId,
        import_contract_reference, c.import_contract_references);
    ALLOC(import_snapshots, SolMirImportSnapshotCapture, import_snapshot,
        c.import_snapshots);
    ALLOC(literal_bytes, char, literal_byte, c.literal_bytes);
    ALLOC(recipe_ids, SolMirRecipeId, recipe_id, c.recipe_ids);
    ALLOC(roots, SolMirMaterializedLocalId, root, c.roots);
    ALLOC(provenance, SolMirOperationProvenance, provenance, c.provenance);
#undef ALLOC
    if (!populate(&b)) {
        if (b.outcome == SOL_MIR_OPERATIONS_BUILD_INTERNAL_FAILED)
            fail(&b, SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
                "operation plan requires unavailable concrete semantics");
        goto failed;
    }
#define USE(member, type, singular) scratch.usage.member = scratch.singular##_count;
    SOL_MIR_OPERATIONS_ARENAS(USE)
#undef USE
    scratch.usage.build_work = b.actual_work;
    size_t measured_validation_scratch;
    if (!sol_mir_operations_internal_validation_requirements(&scratch,
            &scratch.usage.validation_work, &measured_validation_scratch)
        || measured_validation_scratch
            != scratch.usage.validation_scratch_bytes
        || scratch.usage.validation_work > scratch.limits.max_validation_work) {
        fail(&b, SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED,
            "operations validation resource limit exceeded"); goto failed;
    }
    free(b.path_stack); free(b.predicate_locals);
    free(b.predicate_local_bound); free(b.equality_state);
    b.path_stack = NULL; b.predicate_locals = NULL;
    b.predicate_local_bound = NULL; b.equality_state = NULL;
    *output = scratch;
    if (!sol_mir_operations_validate(output, diagnostics)) {
        sol_mir_operations_free(output);
        return diagnostics != NULL && diagnostics->allocation_failed
            ? SOL_MIR_OPERATIONS_BUILD_ALLOCATION_FAILED
            : SOL_MIR_OPERATIONS_BUILD_INTERNAL_FAILED;
    }
    return SOL_MIR_OPERATIONS_BUILD_SUCCEEDED;
failed:
    free(b.path_stack); free(b.predicate_locals);
    free(b.predicate_local_bound); free(b.equality_state);
    sol_mir_operations_free(&scratch); return b.outcome;
}

static void format(Buffer *b, const char *pattern, ...) {
    if (b->failed) return;
    va_list args; va_start(args, pattern); va_list copy; va_copy(copy, args);
    int n = vsnprintf(NULL, 0, pattern, copy); va_end(copy);
    if (n < 0 || (size_t)n > SIZE_MAX - b->length - 1) {
        b->failed = true; va_end(args); return;
    }
    size_t needed = b->length + (size_t)n + 1;
    if (needed > b->capacity) {
        size_t capacity = b->capacity == 0 ? 4096 : b->capacity;
        while (capacity < needed) {
            if (capacity > SIZE_MAX / 2) { capacity = needed; break; }
            capacity *= 2;
        }
        char *grown = realloc(b->data, capacity);
        if (grown == NULL) { b->failed = true; va_end(args); return; }
        b->data = grown; b->capacity = capacity;
    }
    (void)vsnprintf(b->data + b->length, b->capacity - b->length, pattern, args);
    va_end(args); b->length += (size_t)n;
}

bool sol_mir_operations_render(FILE *stream, const SolMirOperations *o) {
    if (stream == NULL || !sol_mir_operations_validate(o, NULL)) return false;
    Buffer b = {0};
    format(&b, "mir_operations access=%zu/%zu construct=%zu/%zu pattern=%zu/%zu/%zu/%zu propagate=%zu arithmetic=%zu equality=%zu/%zu snapshot=%zu callable=%zu handler=%zu predicate=%zu recipes=%zu roots=%zu provenance=%zu owned=%zu scratch=%zu/%zu work=%zu/%zu\n",
        o->access_plan_count, o->access_step_count, o->constructor_count,
        o->construct_operand_count, o->pattern_test_count,
        o->pattern_extraction_count, o->pattern_node_count, o->path_step_count,
        o->propagation_count, o->arithmetic_count, o->equality_node_count,
        o->equality_child_count,
        o->snapshot_count, o->callable_count, o->handler_count,
        o->predicate_count, o->recipe_id_count, o->root_count,
        o->provenance_count, o->usage.owned_bytes,
        o->usage.build_scratch_bytes, o->usage.validation_scratch_bytes,
        o->usage.build_work, o->usage.validation_work);
    format(&b, "limits access=%zu/%zu construct=%zu/%zu pattern=%zu/%zu/%zu/%zu propagate=%zu arithmetic=%zu equality=%zu/%zu snapshot=%zu callable=%zu handler=%zu predicate=%zu recipes=%zu roots=%zu provenance=%zu owned=%zu scratch=%zu/%zu work=%zu/%zu\n",
        o->limits.max_access_plans, o->limits.max_access_steps,
        o->limits.max_constructors, o->limits.max_construct_operands,
        o->limits.max_pattern_tests, o->limits.max_pattern_extractions,
        o->limits.max_pattern_nodes, o->limits.max_path_steps,
        o->limits.max_propagations, o->limits.max_arithmetic,
        o->limits.max_equality_nodes, o->limits.max_equality_children,
        o->limits.max_snapshots, o->limits.max_callables,
        o->limits.max_handlers, o->limits.max_predicates,
        o->limits.max_recipe_ids, o->limits.max_roots,
        o->limits.max_provenance, o->limits.max_owned_bytes,
        o->limits.max_build_scratch_bytes,
        o->limits.max_validation_scratch_bytes, o->limits.max_build_work,
        o->limits.max_validation_work);
    format(&b, "predicate_usage bodies=%zu blocks=%zu inputs=%zu values=%zu instructions=%zu edges=%zu edge_values=%zu operands=%zu paths=%zu patterns=%zu imports=%zu references=%zu import_snapshots=%zu literal_bytes=%zu\n",
        o->predicate_body_count, o->predicate_block_count,
        o->predicate_input_count, o->predicate_value_count,
        o->predicate_instruction_count, o->predicate_edge_count,
        o->predicate_edge_value_count, o->predicate_operand_count,
        o->predicate_path_step_count, o->predicate_pattern_node_count,
        o->import_envelope_count,
        o->import_contract_reference_count, o->import_snapshot_count,
        o->literal_byte_count);
    format(&b, "predicate_limits bodies=%zu blocks=%zu inputs=%zu values=%zu instructions=%zu edges=%zu edge_values=%zu operands=%zu paths=%zu patterns=%zu imports=%zu references=%zu import_snapshots=%zu literal_bytes=%zu\n",
        o->limits.max_predicate_bodies, o->limits.max_predicate_blocks,
        o->limits.max_predicate_inputs, o->limits.max_predicate_values,
        o->limits.max_predicate_instructions, o->limits.max_predicate_edges,
        o->limits.max_predicate_edge_values, o->limits.max_predicate_operands,
        o->limits.max_predicate_path_steps,
        o->limits.max_predicate_pattern_nodes,
        o->limits.max_import_envelopes,
        o->limits.max_import_contract_references,
        o->limits.max_import_snapshots, o->limits.max_literal_bytes);
    format(&b, "predicate_literal_bytes=");
    for (size_t i = 0; i < o->literal_byte_count; ++i)
        format(&b, "%02x", (unsigned char)o->literal_bytes[i]);
    format(&b, "\n");
    for (size_t i = 0; i < o->access_plan_count; ++i) {
        const SolMirOperationAccessPlan *p = &o->access_plans[i];
        format(&b, "access %zu image=%zu local=%zu r%zu->r%zu steps=%zu:%zu\n",
            p->place, p->image, p->local, p->root_recipe, p->final_recipe,
            p->steps.offset, p->steps.count);
    }
    for (size_t i = 0; i < o->access_step_count; ++i)
        format(&b, "access_step %zu projection=%zu base=r%zu result=r%zu field=%zu offset=%" PRIu64 "\n",
            i, o->access_steps[i].projection, o->access_steps[i].base_recipe,
            o->access_steps[i].result_recipe, o->access_steps[i].field_layout,
            o->access_steps[i].object_offset);
    for (size_t i = 0; i < o->constructor_count; ++i) {
        const SolMirOperationConstructPlan *p = &o->constructors[i];
        format(&b, "construct %zu image=%zu instruction=%zu result=%zu kind=%d recipe=%zu object=%d variant=%zu tag=%" PRIu32 " operands=%zu:%zu backing=%zu capability=%d:%zu\n",
            i, p->image, p->instruction, p->result, (int)p->kind,
            p->result_recipe, (int)p->object_kind, p->variant_layout,
            p->semantic_tag, p->operands.offset, p->operands.count,
            p->wrapper_backing, (int)p->capability_rule,
            p->capability_source_operand);
        format(&b, "construct_root %zu inherited=%zu\n", i, p->inherited_root);
    }
    for (size_t i = 0; i < o->construct_operand_count; ++i)
        format(&b, "construct_write %zu formal=%zu source=%zu temporary=%zu recipe=r%zu recipe_field=%zu layout_field=%zu offset=%" PRIu64 "\n",
            i, o->construct_operands[i].formal_ordinal,
            o->construct_operands[i].source_operand_ordinal,
            o->construct_operands[i].temporary, o->construct_operands[i].recipe,
            o->construct_operands[i].recipe_field,
            o->construct_operands[i].layout_field,
            o->construct_operands[i].absolute_offset);
    for (size_t i = 0; i < o->pattern_test_count; ++i)
        format(&b, "pattern_test %zu image=%zu instruction=%zu temporary=%zu recipe=%zu result=%zu nodes=%zu:%zu\n",
            i, o->pattern_tests[i].image, o->pattern_tests[i].instruction,
            o->pattern_tests[i].scrutinee, o->pattern_tests[i].scrutinee_recipe,
            o->pattern_tests[i].result, o->pattern_tests[i].nodes.offset,
            o->pattern_tests[i].nodes.count);
    for (size_t i = 0; i < o->pattern_extraction_count; ++i)
        format(&b, "pattern_value %zu image=%zu instruction=%zu temporary=%zu recipe=%zu result=%zu:r%zu path=%zu:%zu copy=%d\n",
            i, o->pattern_extractions[i].image, o->pattern_extractions[i].instruction,
            o->pattern_extractions[i].scrutinee, o->pattern_extractions[i].scrutinee_recipe,
            o->pattern_extractions[i].result, o->pattern_extractions[i].result_recipe,
            o->pattern_extractions[i].path.offset, o->pattern_extractions[i].path.count,
            (int)o->pattern_extractions[i].copy_kind);
    for (size_t i = 0; i < o->pattern_node_count; ++i)
        format(&b, "pattern_node %zu kind=%d recipe=r%zu path=%zu:%zu tag=%" PRIu32 " bool=%d\n",
            i, (int)o->pattern_nodes[i].kind, o->pattern_nodes[i].recipe,
            o->pattern_nodes[i].path.offset, o->pattern_nodes[i].path.count,
            o->pattern_nodes[i].semantic_tag, o->pattern_nodes[i].boolean);
    for (size_t i = 0; i < o->path_step_count; ++i)
        format(&b, "path_step %zu base=r%zu result=r%zu field=%zu offset=%" PRIu64 "\n",
            i, o->path_steps[i].base_recipe, o->path_steps[i].result_recipe,
            o->path_steps[i].field_layout, o->path_steps[i].object_offset);
    for (size_t i = 0; i < o->propagation_count; ++i)
        format(&b, "propagate %zu image=%zu block=%zu source=%zu:r%zu success=%zu:r%zu variant=%zu tag=%" PRIu32 " field=%zu@%" PRIu64 " source_residual=%zu:%" PRIu32 ":%zu:r%zu@%" PRIu64 " destination_residual=%zu:r%zu:%" PRIu32 ":%zu:r%zu@%" PRIu64 " edges=%zu/%zu\n",
            i, o->propagations[i].image, o->propagations[i].block,
            o->propagations[i].source, o->propagations[i].source_recipe,
            o->propagations[i].success_result, o->propagations[i].success_recipe,
            o->propagations[i].success_variant_layout,
            o->propagations[i].success_tag, o->propagations[i].success_field_layout,
            o->propagations[i].success_field_offset,
            o->propagations[i].source_residual_variant_layout,
            o->propagations[i].source_residual_tag,
            o->propagations[i].source_residual_field_layout,
            o->propagations[i].source_residual_field_recipe,
            o->propagations[i].source_residual_field_offset,
            o->propagations[i].destination_residual_variant_layout,
            o->propagations[i].residual_result, o->propagations[i].residual_recipe,
            o->propagations[i].destination_residual_tag,
            o->propagations[i].destination_residual_field_layout,
            o->propagations[i].destination_residual_field_recipe,
            o->propagations[i].destination_residual_field_offset,
            o->propagations[i].success_edge, o->propagations[i].residual_edge);
    for (size_t i = 0; i < o->arithmetic_count; ++i)
        format(&b, "operation %zu image=%zu instruction=%zu opcode=%d operands=%zu/%zu previous=%zu recipe=%zu result=%zu:r%zu failures=%u compound=%d equality=%zu:%zu\n",
            i, o->arithmetic[i].image, o->arithmetic[i].instruction,
            (int)o->arithmetic[i].opcode, o->arithmetic[i].left,
            o->arithmetic[i].right, o->arithmetic[i].previous,
            o->arithmetic[i].operand_recipe, o->arithmetic[i].result,
            o->arithmetic[i].result_recipe, o->arithmetic[i].failures,
            o->arithmetic[i].compound, o->arithmetic[i].equality.offset,
            o->arithmetic[i].equality.count);
    for (size_t i = 0; i < o->equality_node_count; ++i)
        format(&b, "equality_node %zu recipe=r%zu kind=%d children=%zu:%zu\n",
            i, o->equality_nodes[i].recipe, (int)o->equality_nodes[i].kind,
            o->equality_nodes[i].children.offset, o->equality_nodes[i].children.count);
    for (size_t i = 0; i < o->equality_child_count; ++i)
        format(&b, "equality_child %zu recipe=r%zu field=%zu variant=%zu tag=%" PRIu32 "\n",
            i, o->equality_children[i].recipe,
            o->equality_children[i].field_layout,
            o->equality_children[i].variant_layout,
            o->equality_children[i].semantic_tag);
    for (size_t i = 0; i < o->snapshot_count; ++i)
        format(&b, "snapshot %zu image=%zu instruction=%zu slot=%zu context=%zu access=%zu local=%zu root=%zu path=%zu:%zu recipe=%zu copy=%d provenance=%zu\n",
            i, o->snapshots[i].image, o->snapshots[i].instruction,
            o->snapshots[i].slot, o->snapshots[i].context,
            o->snapshots[i].access,
            o->snapshots[i].local, o->snapshots[i].root_recipe,
            o->snapshots[i].path.offset, o->snapshots[i].path.count,
            o->snapshots[i].recipe, (int)o->snapshots[i].copy_kind,
            o->snapshots[i].provenance);
    for (size_t i = 0; i < o->callable_count; ++i)
        format(&b, "callable %zu site=%zu kind=%d recipe=%zu target=%d:%zu:%zu capture=%d:%zu:%zu:%zu:%zu:r%zu effects=%zu roots=%zu:%zu\n",
            i, o->callables[i].semantic_site, (int)o->callables[i].kind,
            o->callables[i].function_recipe, (int)o->callables[i].target_kind,
            o->callables[i].target_instance, o->callables[i].target_import,
            (int)o->callables[i].capture_kind, o->callables[i].capture_access,
            o->callables[i].capture_temporary, o->callables[i].capture_value,
            o->callables[i].capture_instruction, o->callables[i].capture_recipe,
            o->callables[i].effects, o->callables[i].roots.offset,
            o->callables[i].roots.count);
    for (size_t i = 0; i < o->handler_count; ++i)
        format(&b, "handler %zu image=%zu parent=%zu bindings=%zu/%zu access=%zu/%zu operation=%d:%zu:%zu:%zu:%zu:%zu signature=%zu:%zu:%zu:%zu effects=%zu root_match=%d\n",
            i, o->handlers[i].image, o->handlers[i].frame_parent,
            o->handlers[i].source_binding, o->handlers[i].provider_binding,
            o->handlers[i].authority_access, o->handlers[i].provider_access,
            (int)o->handlers[i].operation.target_kind,
            o->handlers[i].operation.instance, o->handlers[i].operation.import,
            o->handlers[i].operation.receiver, o->handlers[i].operation.root,
            o->handlers[i].operation.effects, o->handlers[i].receiver_recipe,
            o->handlers[i].parameter_recipes.offset,
            o->handlers[i].parameter_recipes.count, o->handlers[i].result_recipe,
            o->handlers[i].effects, (int)o->handlers[i].root_match);
    for (size_t i = 0; i < o->predicate_count; ++i)
        format(&b, "predicate %zu kind=%d body=%zu image=%zu block=%zu context=%zu input=%zu:r%zu result=%zu:r%zu output=r%zu phase=%d outcome=%d provenance=%zu\n",
            i, (int)o->predicates[i].kind, o->predicates[i].body,
            o->predicates[i].image, o->predicates[i].block,
            o->predicates[i].context, o->predicates[i].representation,
            o->predicates[i].input_recipe, o->predicates[i].result,
            o->predicates[i].result_recipe, o->predicates[i].output_recipe,
            (int)o->predicates[i].contract_phase,
            (int)o->predicates[i].contract_outcome,
            o->predicates[i].provenance);
    for (size_t i = 0; i < o->predicate_body_count; ++i) {
        const SolMirPredicateBody *p = &o->predicate_bodies[i];
        format(&b, "predicate_body %zu owner=%d:%zu:%zu context=%zu phase=%d outcome=%d inputs=%zu:%zu blocks=%zu:%zu values=%zu:%zu entry=%zu output=r%zu refinement_self=r%zu\n",
            i, (int)p->owner_kind, p->instance, p->import, p->context,
            (int)p->phase, (int)p->outcome, p->inputs.offset, p->inputs.count,
            p->blocks.offset, p->blocks.count, p->values.offset,
            p->values.count, p->entry, p->output_recipe,
            p->refinement_self_recipe);
    }
    for (size_t i = 0; i < o->predicate_input_count; ++i)
        format(&b, "predicate_input %zu kind=%d ordinal=%zu recipe=r%zu access=%d\n",
            i, (int)o->predicate_inputs[i].kind, o->predicate_inputs[i].ordinal,
            o->predicate_inputs[i].recipe, (int)o->predicate_inputs[i].access);
    for (size_t i = 0; i < o->predicate_value_count; ++i)
        format(&b, "predicate_value %zu kind=%d recipe=r%zu block=%zu definition=%zu\n",
            i, (int)o->predicate_values[i].kind, o->predicate_values[i].recipe,
            o->predicate_values[i].block, o->predicate_values[i].definition);
    for (size_t i = 0; i < o->predicate_instruction_count; ++i) {
        const SolMirPredicateInstruction *p = &o->predicate_instructions[i];
        format(&b, "predicate_instruction %zu kind=%d block=%zu result=%zu recipe=r%zu opcode=%d values=%zu/%zu integer=%" PRId64 " boolean=%d bytes=%zu:%zu failures=%u operands=%zu:%zu path=%zu:%zu pattern=%zu:%zu construct=%d variant=%zu tag=%" PRIu32 " binding=%zu\n",
            i, (int)p->kind, p->block, p->result, p->recipe, (int)p->opcode,
            p->left, p->right, p->integer, p->boolean, p->bytes.offset,
            p->bytes.count, p->failures, p->operands.offset, p->operands.count,
            p->path.offset, p->path.count, p->pattern.offset, p->pattern.count,
            (int)p->construct_kind, p->variant_layout, p->semantic_tag,
            p->binding);
    }
    for (size_t i = 0; i < o->predicate_block_count; ++i) {
        const SolMirPredicateBlock *p = &o->predicate_blocks[i];
        const SolMirPredicateTerminator *t = &p->terminator;
        format(&b, "predicate_block %zu body=%zu parameters=%zu:%zu instructions=%zu:%zu terminator=%d value=%zu condition=%zu callee=%zu receiver=%zu/%d arguments=%zu:%zu call=%d binding=%zu effects=%zu result=%zu edges=%zu/%zu/%zu/%zu/%zu propagation=%d variants=%zu/%zu field=%zu nested=%zu result_recipe=r%zu failure=%d\n",
            i, p->body, p->parameters.offset, p->parameters.count,
            p->instructions.offset, p->instructions.count, (int)t->kind,
            t->value, t->condition, t->callee, t->receiver,
            (int)t->receiver_access, t->arguments.offset, t->arguments.count,
            (int)t->call_kind, t->binding, t->effects, t->result, t->edge,
            t->true_edge, t->false_edge, t->normal_edge, t->failure_edge,
            (int)t->propagation_kind, t->success_variant_layout,
            t->residual_variant_layout, t->success_field_layout,
            t->nested_body, t->result_recipe, (int)t->failure_kind);
    }
    for (size_t i = 0; i < o->predicate_edge_count; ++i)
        format(&b, "predicate_edge %zu source=%zu target=%zu arguments=%zu:%zu\n",
            i, o->predicate_edges[i].source, o->predicate_edges[i].target,
            o->predicate_edges[i].arguments.offset,
            o->predicate_edges[i].arguments.count);
    for (size_t i = 0; i < o->predicate_edge_value_count; ++i)
        format(&b, "predicate_edge_value %zu value=%zu\n", i,
            o->predicate_edge_values[i]);
    for (size_t i = 0; i < o->predicate_operand_count; ++i) {
        const SolMirPredicateOperand *p = &o->predicate_operands[i];
        format(&b, "predicate_operand %zu value=%zu formal=%zu access=%d field=%zu\n",
            i, p->value, p->formal_ordinal, (int)p->access, p->field_layout);
    }
    for (size_t i = 0; i < o->predicate_path_step_count; ++i) {
        const SolMirPredicatePathStep *p = &o->predicate_path_steps[i];
        format(&b, "predicate_path_step %zu base=r%zu result=r%zu field=%zu\n",
            i, p->base_recipe, p->result_recipe, p->field_layout);
    }
    for (size_t i = 0; i < o->predicate_pattern_node_count; ++i) {
        const SolMirPredicatePatternNode *p = &o->predicate_pattern_nodes[i];
        format(&b, "predicate_pattern_node %zu kind=%d recipe=r%zu path=%zu:%zu tag=%" PRIu32 " bool=%d\n",
            i, (int)p->kind, p->recipe, p->path.offset, p->path.count,
            p->semantic_tag, p->boolean);
    }
    for (size_t i = 0; i < o->import_envelope_count; ++i) {
        const SolMirImportContractEnvelope *p = &o->import_envelopes[i];
        format(&b, "import_contract %zu import=%zu receiver=r%zu/%d parameters=%zu:%zu accesses=%zu:%zu result=r%zu effects=%zu requires=%zu:%zu snapshots=%zu:%zu host_invoke=%d ensures=%zu:%zu\n",
            i, p->import, p->receiver, (int)p->receiver_access,
            p->parameters.offset,
            p->parameters.count, p->parameter_accesses.offset,
            p->parameter_accesses.count, p->result, p->effects,
            p->requires.offset, p->requires.count, p->snapshots.offset,
            p->snapshots.count, p->host_invoke, p->ensures.offset,
            p->ensures.count);
    }
    for (size_t i = 0; i < o->import_snapshot_count; ++i) {
        const SolMirImportSnapshotCapture *p = &o->import_snapshots[i];
        format(&b, "import_snapshot %zu import=%zu context=%zu slot=%zu input=%d:%zu recipe=r%zu access=%d provenance=%zu\n",
            i, p->import, p->context, p->slot, (int)p->input_kind,
            p->ordinal, p->recipe, (int)p->access, p->provenance);
    }
    for (size_t i = 0; i < o->import_contract_reference_count; ++i)
        format(&b, "import_contract_reference %zu body=%zu\n", i,
            o->import_contract_references[i]);
    for (size_t i = 0; i < o->recipe_id_count; ++i)
        format(&b, "recipe_id %zu r%zu\n", i, o->recipe_ids[i]);
    for (size_t i = 0; i < o->root_count; ++i)
        format(&b, "callable_root %zu local=%zu\n", i, o->roots[i]);
    for (size_t i = 0; i < o->provenance_count; ++i)
        format(&b, "provenance %zu kind=%d executable=%zu expression=%zu pattern=%zu field=%zu variant=%zu obligation=%zu snapshot=%zu definition=%zu\n",
            i, (int)o->provenance[i].kind, o->provenance[i].executable,
            o->provenance[i].source_expression,
            o->provenance[i].source_pattern, o->provenance[i].source_field,
            o->provenance[i].source_variant,
            o->provenance[i].source_obligation,
            o->provenance[i].source_snapshot,
            o->provenance[i].source_definition);
    bool ok = !b.failed && (b.length == 0
        || fwrite(b.data, b.length, 1, stream) == 1);
    free(b.data); return ok;
}
