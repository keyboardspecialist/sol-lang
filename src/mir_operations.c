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
        && REQUIRED(max_predicate_instructions) && REQUIRED(max_import_envelopes)
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
    size_t import_envelopes, import_contract_references;
    size_t import_snapshots;
    size_t callables, handlers, recipe_ids, roots, provenance;
} Counts;

static bool count_predicate_expression(Builder *b, const SolIr *ir, size_t id,
    size_t depth, Counts *c) {
    if (!charge(b, SOL_MIR_OPERATIONS_WORK_RECURSE) || id >= ir->expression_count
        || depth > ir->expression_count) return false;
    const SolIrExpression *e = &ir->expressions[id];
    switch (e->kind) {
        case SOL_IR_EXPR_INTEGER: case SOL_IR_EXPR_BOOL: case SOL_IR_EXPR_UNIT:
            ++c->predicate_values; ++c->predicate_instructions; return true;
        case SOL_IR_EXPR_STRING: {
            size_t n = strlen(e->as.string);
            if (c->predicate_values == SIZE_MAX
                || c->predicate_instructions == SIZE_MAX
                || !add_size(&c->literal_bytes, n)) return false;
            ++c->predicate_values; ++c->predicate_instructions; return true;
        }
        case SOL_IR_EXPR_PLACE: case SOL_IR_EXPR_REFINEMENT_SELF:
        case SOL_IR_EXPR_RESULT: case SOL_IR_EXPR_SNAPSHOT_READ:
            if (e->kind == SOL_IR_EXPR_PLACE) {
                if (e->as.place >= ir->place_count
                    || ir->places[e->as.place].root_kind
                        != SOL_IR_PLACE_ROOT_LOCAL
                    || ir->places[e->as.place].projections.count != 0) {
                    return fail(b, SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
                        "projected or computed predicate place is outside P2.6b1");
                }
            }
            if (e->kind == SOL_IR_EXPR_SNAPSHOT_READ) {
                if (e->as.snapshot >= ir->snapshot_count
                    || ir->snapshots[e->as.snapshot].operand
                        >= ir->expression_count) return false;
                const SolIrExpression *operand
                    = &ir->expressions[ir->snapshots[e->as.snapshot].operand];
                if (operand->kind != SOL_IR_EXPR_PLACE
                    || operand->as.place >= ir->place_count
                    || ir->places[operand->as.place].root_kind
                        != SOL_IR_PLACE_ROOT_LOCAL
                    || ir->places[operand->as.place].projections.count != 0)
                    return fail(b, SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
                        "projected or computed predicate snapshot is outside P2.6b1");
            }
            ++c->predicate_inputs; ++c->predicate_values; return true;
        case SOL_IR_EXPR_UNARY:
            if (!count_predicate_expression(b, ir, e->as.unary.operand,
                    depth + 1, c)) return false;
            ++c->predicate_values; ++c->predicate_instructions; return true;
        case SOL_IR_EXPR_BINARY:
            if (e->as.binary.operator_kind == SOL_TOKEN_AMP_AMP
                || e->as.binary.operator_kind == SOL_TOKEN_PIPE_PIPE)
                return fail(b, SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
                    "short-circuit predicate operator is outside P2.6b1");
            if (!count_predicate_expression(b, ir, e->as.binary.left, depth + 1, c)
                || !count_predicate_expression(b, ir, e->as.binary.right,
                    depth + 1, c)) return false;
            ++c->predicate_values; ++c->predicate_instructions; return true;
        default:
            return fail(b, SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED,
                "predicate expression requires synthetic CFG support not available");
    }
}

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
            ++c->predicates; ++c->provenance; ++c->predicate_bodies;
            ++c->predicate_blocks;
            SolObligationId obligation = m->blocks[i].terminator.source_obligation;
            if (obligation >= ir->obligation_count
                || !count_predicate_expression(b, ir,
                    ir->obligations[obligation].predicate, 0, c)) return false;
        }
    }
    c->import_envelopes = m->import_count;
    for (size_t i = 0; i < m->import_count; ++i) {
        for (size_t q = 0; q < m->imports[i].contexts.count; ++q) {
            size_t context = m->imports[i].contexts.offset + q;
            if (context >= m->context_count) return false;
            size_t obligation = m->contexts[context].obligation;
            if (obligation >= ir->obligation_count) return false;
            ++c->predicate_bodies; ++c->predicate_blocks;
            ++c->import_contract_references;
            if (!add_size(&c->import_snapshots,
                    ir->obligations[obligation].snapshots.count)) return false;
            if (!add_size(&c->provenance,
                    ir->obligations[obligation].snapshots.count)) return false;
            if (!count_predicate_expression(b, ir,
                    ir->obligations[obligation].predicate, 0, c)) return false;
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
    return true;
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
} PredicateLowerer;

static bool predicate_snapshot_slot(const SolIrObligation *obligation,
    SolIrSnapshotId source, size_t *slot) {
    for (size_t i = 0; i < obligation->snapshots.count; ++i)
        if (obligation->snapshots.offset + i == source) {
            *slot = i; return true;
        }
    return false;
}

static bool lower_predicate_expression(PredicateLowerer *l, size_t expression,
    SolMirPredicateValueId *result) {
    SolMirOperations *o = l->builder->out;
    const SolMirMaterialization *m = o->layout->representation->materialization;
    const SolIr *ir = m->plan->program->ir;
    if (!charge(l->builder, SOL_MIR_OPERATIONS_WORK_RECURSE)
        || expression >= ir->expression_count) return false;
    const SolIrExpression *e = &ir->expressions[expression];
    SolMirRecipeId recipe = predicate_expression_recipe(o->layout->representation,
        l->image,
        l->context, expression);
    if (recipe == SOL_MIR_RECIPE_NONE) return false;
    if (e->kind == SOL_IR_EXPR_PLACE || e->kind == SOL_IR_EXPR_RESULT
        || e->kind == SOL_IR_EXPR_SNAPSHOT_READ
        || e->kind == SOL_IR_EXPR_REFINEMENT_SELF) {
        SolMirPredicateInputKind kind;
        size_t ordinal = 0;
        SolAccessMode access = SOL_ACCESS_OWNED;
        if (e->kind == SOL_IR_EXPR_RESULT) {
            const SolIrObligation *ob = &ir->obligations[l->obligation];
            kind = ob->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
                ? SOL_MIR_PREDICATE_INPUT_SUCCESS_RESULT
                : SOL_MIR_PREDICATE_INPUT_COMPLETE_RESULT;
        } else if (e->kind == SOL_IR_EXPR_SNAPSHOT_READ) {
            kind = SOL_MIR_PREDICATE_INPUT_SNAPSHOT;
            if (!predicate_snapshot_slot(&ir->obligations[l->obligation],
                    e->as.snapshot, &ordinal)) return false;
            if (!add_size(&ordinal, l->snapshot_base)) return false;
        } else if (e->kind == SOL_IR_EXPR_REFINEMENT_SELF) {
            kind = SOL_MIR_PREDICATE_INPUT_REFINEMENT_SELF;
        } else {
            if (e->as.place >= ir->place_count) return false;
            if (ir->places[e->as.place].root_kind != SOL_IR_PLACE_ROOT_LOCAL
                || ir->places[e->as.place].projections.count != 0) return false;
            SolIrLocalId local = ir->places[e->as.place].local;
            SolIrCallableId callable_id = l->image == SOL_MIR_OPERATION_NONE
                ? m->imports[l->import].source_callable
                : m->images[l->image].source_callable;
            const SolIrCallable *callable = &ir->callables[callable_id];
            if (local == callable->receiver) {
                kind = SOL_MIR_PREDICATE_INPUT_RECEIVER;
                access = callable->receiver_access;
            } else if (local == callable->capability_source) {
                return false;
            } else {
                kind = SOL_MIR_PREDICATE_INPUT_PARAMETER;
                bool found = false;
                for (size_t i = 0; i < callable->parameters.count; ++i) {
                    if (ir->roots[callable->parameters.offset + i] == local) {
                        ordinal = i; access = ir->locals[local].access;
                        found = true; break;
                    }
                }
                if (!found) return false;
            }
        }
        size_t input = o->predicate_input_count++;
        o->predicate_inputs[input] = (SolMirPredicateInput){kind, ordinal,
            recipe, access};
        *result = o->predicate_value_count++;
        o->predicate_values[*result] = (SolMirPredicateValue){
            SOL_MIR_PREDICATE_VALUE_INPUT, recipe, l->block, input};
        return true;
    }
    SolMirPredicateValueId left = SOL_MIR_OPERATION_NONE;
    SolMirPredicateValueId right = SOL_MIR_OPERATION_NONE;
    SolMirPredicateInstruction instruction;
    memset(&instruction, 0, sizeof(instruction));
    instruction.block = l->block; instruction.recipe = recipe;
    instruction.left = SOL_MIR_OPERATION_NONE;
    instruction.right = SOL_MIR_OPERATION_NONE;
    instruction.bytes = (SolMirPlanSlice){0, 0};
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
        instruction.bytes = (SolMirPlanSlice){o->literal_byte_count, length};
        memcpy(o->literal_bytes + o->literal_byte_count, e->as.string, length);
        o->literal_byte_count += length;
    } else if (e->kind == SOL_IR_EXPR_UNARY) {
        instruction.kind = SOL_MIR_PREDICATE_INST_UNARY;
        if (!lower_predicate_expression(l, e->as.unary.operand, &left)) return false;
        instruction.left = left;
        instruction.opcode = opcode(SOL_MIR_INST_UNARY,
            e->as.unary.operator_kind, &instruction.failures);
        if ((int)instruction.opcode < 0) return false;
    } else if (e->kind == SOL_IR_EXPR_BINARY) {
        if (e->as.binary.operator_kind == SOL_TOKEN_AMP_AMP
            || e->as.binary.operator_kind == SOL_TOKEN_PIPE_PIPE) return false;
        instruction.kind = SOL_MIR_PREDICATE_INST_BINARY;
        if (!lower_predicate_expression(l, e->as.binary.left, &left)
            || !lower_predicate_expression(l, e->as.binary.right, &right)) return false;
        instruction.left = left; instruction.right = right;
        instruction.opcode = opcode(SOL_MIR_INST_BINARY,
            e->as.binary.operator_kind, &instruction.failures);
        if ((int)instruction.opcode < 0) return false;
    } else return false;
    size_t id = o->predicate_instruction_count++;
    *result = o->predicate_value_count++;
    instruction.result = *result;
    o->predicate_instructions[id] = instruction;
    o->predicate_values[*result] = (SolMirPredicateValue){
        SOL_MIR_PREDICATE_VALUE_INSTRUCTION, recipe, l->block, id};
    return true;
}

static bool lower_predicate_body(Builder *b, size_t image, size_t context,
    SolObligationId obligation, SolMirPredicateBodyId *result) {
    SolMirOperations *o = b->out;
    const SolMirMaterialization *m
        = o->layout->representation->materialization;
    const SolIr *ir = m->plan->program->ir;
    if (obligation >= ir->obligation_count) return false;
    const SolIrObligation *source = &ir->obligations[obligation];
    size_t body = o->predicate_body_count++;
    size_t block = o->predicate_block_count++;
    size_t input_start = o->predicate_input_count;
    size_t value_start = o->predicate_value_count;
    size_t instruction_start = o->predicate_instruction_count;
    size_t import = m->contexts[context].target_kind == SOL_MIR_PLAN_TARGET_IMPORT
        ? m->contexts[context].import : SOL_MIR_OPERATION_NONE;
    SolMirPlanSlice owner_contexts = import == SOL_MIR_OPERATION_NONE
        ? m->images[image].contexts : m->imports[import].contexts;
    size_t snapshot_base = 0;
    for (size_t i = 0; i < owner_contexts.count; ++i) {
        size_t candidate = owner_contexts.offset + i;
        if (candidate == context) break;
        if (m->contexts[candidate].kind != SOL_MIR_PLAN_CONTEXT_CONTRACT) continue;
        size_t prior = m->contexts[candidate].obligation;
        if (prior >= ir->obligation_count
            || !add_size(&snapshot_base, ir->obligations[prior].snapshots.count))
            return false;
    }
    PredicateLowerer lowerer = {b, image, import, context, body, block,
        snapshot_base, obligation};
    SolMirPredicateValueId value;
    if (!lower_predicate_expression(&lowerer, source->predicate, &value)) return false;
    o->predicate_blocks[block] = (SolMirPredicateBlock){body,
        {instruction_start, o->predicate_instruction_count - instruction_start},
        {SOL_MIR_PREDICATE_TERM_RETURN, value}};
    SolMirRecipeId output = predicate_expression_recipe(
        o->layout->representation, image, context,
        source->predicate);
    o->predicate_bodies[body] = (SolMirPredicateBody){
        import == SOL_MIR_OPERATION_NONE ? SOL_MIR_PREDICATE_OWNER_INSTANCE
            : SOL_MIR_PREDICATE_OWNER_IMPORT,
        image, import,
        context, source->kind, source->outcome,
        {input_start, o->predicate_input_count - input_start}, {block, 1},
        {value_start, o->predicate_value_count - value_start}, block, output};
    *result = body;
    return true;
}

static bool populate(Builder *b) {
    SolMirOperations *o = b->out;
    const SolMirRepresentation *r = o->layout->representation;
    const SolMirMaterialization *m = r->materialization;
    const SolIr *ir = m->plan->program->ir;
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
                    x->construct_variant, SOL_IR_NONE, SOL_IR_NONE})) return false;
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
                    SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE})) return false;
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
                    SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE})) return false;
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
                    SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE})) return false;
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
                    ir->snapshots[x->source_snapshot].obligation, x->source_snapshot})) return false;
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
                    SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE})) return false;
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
                    SOL_IR_NONE, t->source_obligation, SOL_IR_NONE})) return false;
            SolMirPredicateBodyId body;
            if (!lower_predicate_body(b, image, context, t->source_obligation,
                    &body))
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
                SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE})) return false;
    }
    for (size_t i = 0; i < m->import_count; ++i) {
        const SolMirMaterializedImport *source = &m->imports[i];
        size_t requires = o->import_contract_reference_count;
        size_t snapshots = o->import_snapshot_count;
        size_t import_slot = 0;
        for (size_t q = 0; q < source->contexts.count; ++q) {
            size_t context = source->contexts.offset + q;
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
                        ir->snapshots[snapshot].obligation, snapshot})) return false;
                o->import_snapshots[o->import_snapshot_count++]
                    = (SolMirImportSnapshotCapture){i, context,
                        import_slot++, kind, ordinal, recipe, access, provenance};
            }
        }
        for (size_t q = 0; q < source->contexts.count; ++q) {
            size_t context = source->contexts.offset + q;
            size_t obligation = m->contexts[context].obligation;
            if (ir->obligations[obligation].kind != SOL_CONTRACT_REQUIRES)
                continue;
            SolMirPredicateBodyId body;
            if (!lower_predicate_body(b, SOL_MIR_OPERATION_NONE, context,
                    obligation, &body)) return false;
            o->import_contract_references[o->import_contract_reference_count++]
                = body;
        }
        size_t require_count = o->import_contract_reference_count - requires;
        size_t ensures = o->import_contract_reference_count;
        for (size_t q = 0; q < source->contexts.count; ++q) {
            size_t context = source->contexts.offset + q;
            size_t obligation = m->contexts[context].obligation;
            if (ir->obligations[obligation].kind != SOL_CONTRACT_ENSURES)
                continue;
            SolMirPredicateBodyId body;
            if (!lower_predicate_body(b, SOL_MIR_OPERATION_NONE, context,
                    obligation, &body)) return false;
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
                SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE})) return false;
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
    if (!mul_size(ir->pattern_count, sizeof(SolMirOperationPathStep),
            &scratch.usage.build_scratch_bytes)
        || !add_size(&scratch.usage.build_scratch_bytes, r->recipe_count)
        || scratch.usage.build_scratch_bytes
            > scratch.limits.max_build_scratch_bytes) {
        fail(&b, SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED,
            "operations build scratch limit exceeded"); goto failed;
    }
    size_t local_validation_scratch = c.provenance > r->recipe_count
        ? c.provenance : r->recipe_count;
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
    free(b.path_stack); free(b.equality_state);
    b.path_stack = NULL; b.equality_state = NULL;
    *output = scratch;
    if (!sol_mir_operations_validate(output, diagnostics)) {
        sol_mir_operations_free(output);
        return diagnostics != NULL && diagnostics->allocation_failed
            ? SOL_MIR_OPERATIONS_BUILD_ALLOCATION_FAILED
            : SOL_MIR_OPERATIONS_BUILD_INTERNAL_FAILED;
    }
    return SOL_MIR_OPERATIONS_BUILD_SUCCEEDED;
failed:
    free(b.path_stack); free(b.equality_state);
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
    format(&b, "predicate_usage bodies=%zu blocks=%zu inputs=%zu values=%zu instructions=%zu imports=%zu references=%zu import_snapshots=%zu literal_bytes=%zu\n",
        o->predicate_body_count, o->predicate_block_count,
        o->predicate_input_count, o->predicate_value_count,
        o->predicate_instruction_count, o->import_envelope_count,
        o->import_contract_reference_count, o->import_snapshot_count,
        o->literal_byte_count);
    format(&b, "predicate_limits bodies=%zu blocks=%zu inputs=%zu values=%zu instructions=%zu imports=%zu references=%zu import_snapshots=%zu literal_bytes=%zu\n",
        o->limits.max_predicate_bodies, o->limits.max_predicate_blocks,
        o->limits.max_predicate_inputs, o->limits.max_predicate_values,
        o->limits.max_predicate_instructions, o->limits.max_import_envelopes,
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
        format(&b, "predicate_body %zu owner=%d:%zu:%zu context=%zu phase=%d outcome=%d inputs=%zu:%zu blocks=%zu:%zu values=%zu:%zu entry=%zu output=r%zu\n",
            i, (int)p->owner_kind, p->instance, p->import, p->context,
            (int)p->phase, (int)p->outcome, p->inputs.offset, p->inputs.count,
            p->blocks.offset, p->blocks.count, p->values.offset,
            p->values.count, p->entry, p->output_recipe);
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
        format(&b, "predicate_instruction %zu kind=%d block=%zu result=%zu recipe=r%zu opcode=%d operands=%zu/%zu integer=%" PRId64 " boolean=%d bytes=%zu:%zu failures=%u\n",
            i, (int)p->kind, p->block, p->result, p->recipe, (int)p->opcode,
            p->left, p->right, p->integer, p->boolean, p->bytes.offset,
            p->bytes.count, p->failures);
    }
    for (size_t i = 0; i < o->predicate_block_count; ++i)
        format(&b, "predicate_block %zu body=%zu instructions=%zu:%zu return=%zu\n",
            i, o->predicate_blocks[i].body,
            o->predicate_blocks[i].instructions.offset,
            o->predicate_blocks[i].instructions.count,
            o->predicate_blocks[i].terminator.value);
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
        format(&b, "provenance %zu kind=%d executable=%zu expression=%zu pattern=%zu field=%zu variant=%zu obligation=%zu snapshot=%zu\n",
            i, (int)o->provenance[i].kind, o->provenance[i].executable,
            o->provenance[i].source_expression,
            o->provenance[i].source_pattern, o->provenance[i].source_field,
            o->provenance[i].source_variant,
            o->provenance[i].source_obligation,
            o->provenance[i].source_snapshot);
    bool ok = !b.failed && (b.length == 0
        || fwrite(b.data, b.length, 1, stream) == 1);
    free(b.data); return ok;
}
