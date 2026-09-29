#include "sol/mir_operations.h"

#include "sol/effects.h"
#include "sol/lexer.h"
#include "sol/ownership.h"
#include "sol/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "%s:%d: check failed: %s\n", \
    __FILE__, __LINE__, #x); ++failures; } } while (0)

bool sol_mir_operations_internal_expected_build_work(const SolMirOperations *,
    size_t *);

typedef struct {
    SolSource source;
    SolTokens tokens;
    SolSyntaxTree syntax;
    SolDiagnostics diagnostics;
    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    SolContractTable contracts;
    SolIr ir;
} Compilation;

typedef struct {
    SolMirProgram program;
    SolMirPlan plan;
    SolMirMaterialization materialization;
    SolMirRepresentation representation;
    SolMirLayout layout;
    SolMirOperations operations;
    SolDiagnostics diagnostics;
} Pipeline;

static SolIrCallableId callable(const SolIr *ir, const char *name,
    SolIrCallableKind kind) {
    for (size_t i = 0; i < ir->callable_count; ++i)
        if (ir->callables[i].kind == kind && strcmp(ir->callables[i].name, name) == 0)
            return i;
    return SOL_IR_NONE;
}

static void pipeline_init(Pipeline *p) {
    memset(p, 0, sizeof(*p));
    sol_mir_program_init(&p->program); sol_mir_plan_init(&p->plan);
    sol_mir_materialization_init(&p->materialization);
    sol_mir_representation_init(&p->representation);
    sol_mir_layout_init(&p->layout); sol_mir_operations_init(&p->operations);
    sol_diagnostics_init(&p->diagnostics);
}

static void pipeline_free(Pipeline *p) {
    sol_mir_operations_free(&p->operations); sol_mir_layout_free(&p->layout);
    sol_mir_representation_free(&p->representation);
    sol_mir_materialization_free(&p->materialization);
    sol_mir_plan_free(&p->plan); sol_mir_program_free(&p->program);
    sol_diagnostics_free(&p->diagnostics);
}

static bool build_pipeline(const SolIr *ir, const SolMirProgramRoot *roots,
    size_t root_count, const SolIrCallableId *imports, size_t import_count,
    Pipeline *p, const SolMirOperationsLimits *limits) {
    SolMirProgramBuildRequest a = {ir, roots, root_count, imports, import_count, NULL};
    SolMirPlanBuildRequest b = {&p->program, NULL};
    SolMirMaterializeBuildRequest c = {&p->plan, NULL};
    SolMirRepresentationBuildRequest d = {&p->materialization, NULL};
    SolMirTargetDescriptor wasm = sol_mir_target_wasm32();
    SolMirLayoutBuildRequest e = {&p->representation, &wasm, NULL};
    SolMirOperationsBuildRequest f = {&p->layout, limits};
    bool layout = sol_mir_program_build(&a, &p->program, &p->diagnostics)
            == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && sol_mir_plan_build(&b, &p->plan, &p->diagnostics)
            == SOL_MIR_PLAN_BUILD_SUCCEEDED
        && sol_mir_materialize_build(&c, &p->materialization, &p->diagnostics)
            == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED
        && sol_mir_representation_build(&d, &p->representation, &p->diagnostics)
            == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED
        && sol_mir_layout_build(&e, &p->layout, &p->diagnostics)
            == SOL_MIR_LAYOUT_BUILD_SUCCEEDED;
    return layout && sol_mir_operations_build(&f, &p->operations, &p->diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED;
}

static bool compile_text(Compilation *c, const char *text) {
    memset(c, 0, sizeof(*c)); sol_tokens_init(&c->tokens);
    sol_diagnostics_init(&c->diagnostics); sol_syntax_tree_init(&c->syntax);
    sol_hir_module_init(&c->hir); sol_type_table_init(&c->types);
    sol_effect_table_init(&c->effects); sol_contract_table_init(&c->contracts);
    sol_ir_init(&c->ir);
    return sol_source_from_text(&c->source, "operations.sol", text)
        && sol_lex(&c->source, &c->tokens, &c->diagnostics)
        && sol_parse(&c->source, &c->tokens, &c->syntax, &c->diagnostics)
        && sol_hir_lower(&c->source, &c->syntax, &c->hir, &c->diagnostics)
        && sol_type_check(&c->source, &c->syntax, &c->hir, &c->types, &c->diagnostics)
        && sol_effect_check(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->diagnostics)
        && sol_contract_lower(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->contracts, &c->diagnostics)
        && sol_ir_lower(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->contracts, &c->ir, &c->diagnostics);
}

static void free_text(Compilation *c) {
    sol_ir_free(&c->ir); sol_contract_table_free(&c->contracts);
    sol_effect_table_free(&c->effects); sol_type_table_free(&c->types);
    sol_hir_module_free(&c->hir); sol_syntax_tree_free(&c->syntax);
    sol_tokens_free(&c->tokens); sol_source_free(&c->source);
    sol_diagnostics_free(&c->diagnostics);
}

static bool compile_e6(Compilation *c, SolPackage *package) {
    memset(c, 0, sizeof(*c)); sol_package_init(package);
    sol_diagnostics_init(&c->diagnostics); sol_hir_module_init(&c->hir);
    sol_type_table_init(&c->types); sol_effect_table_init(&c->effects);
    sol_contract_table_init(&c->contracts); sol_ir_init(&c->ir);
    char message[256];
    if (!sol_package_load_directory(package,
            SOL_TEST_SOURCE_DIR "/tests/conformance/e6", &c->diagnostics,
            message, sizeof(message))) return false;
    SolHirFileScope *scopes = package->file_count == 0 ? NULL
        : malloc(package->file_count * sizeof(*scopes));
    if (package->file_count != 0 && scopes == NULL) return false;
    for (size_t i = 0; i < package->file_count; ++i)
        scopes[i] = (SolHirFileScope){package->files[i].module_name,
            package->files[i].import_start, package->files[i].import_count,
            package->files[i].item_start, package->files[i].item_count};
    bool ok = sol_hir_lower_scoped(&package->source, &package->syntax, scopes,
            package->file_count, &c->hir, &c->diagnostics)
        && sol_type_check(&package->source, &package->syntax, &c->hir,
            &c->types, &c->diagnostics)
        && sol_effect_check(&package->source, &package->syntax, &c->hir,
            &c->types, &c->effects, &c->diagnostics)
        && sol_contract_lower(&package->source, &package->syntax, &c->hir,
            &c->types, &c->effects, &c->contracts, &c->diagnostics)
        && sol_ir_lower_scoped(&package->source, &package->syntax, &c->hir,
            &c->types, &c->effects, &c->contracts, package->files,
            package->file_count, &c->ir, &c->diagnostics);
    free(scopes); return ok;
}

static void free_e6(Compilation *c, SolPackage *package) {
    sol_ir_free(&c->ir); sol_contract_table_free(&c->contracts);
    sol_effect_table_free(&c->effects); sol_type_table_free(&c->types);
    sol_hir_module_free(&c->hir); sol_diagnostics_free(&c->diagnostics);
    sol_package_free(package);
}

static char *render(const SolMirOperations *o, size_t *length) {
    FILE *f = tmpfile();
    if (f == NULL || !sol_mir_operations_render(f, o) || fflush(f) != 0
        || fseek(f, 0, SEEK_END) != 0) { if (f != NULL) fclose(f); return NULL; }
    long end = ftell(f);
    if (end < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return NULL; }
    char *text = malloc((size_t)end + 1);
    if (text == NULL || fread(text, 1, (size_t)end, f) != (size_t)end) {
        free(text); fclose(f); return NULL;
    }
    fclose(f); text[end] = '\0'; *length = (size_t)end; return text;
}

static SolMirOperationsLimits exact_limits(const SolMirOperations *o) {
#define NONZERO(value) ((value) == 0 ? 1 : (value))
    return (SolMirOperationsLimits){
        .max_access_plans = NONZERO(o->usage.access_plans),
        .max_access_steps = NONZERO(o->usage.access_steps),
        .max_constructors = NONZERO(o->usage.constructors),
        .max_construct_operands = NONZERO(o->usage.construct_operands),
        .max_pattern_tests = NONZERO(o->usage.pattern_tests),
        .max_pattern_extractions = NONZERO(o->usage.pattern_extractions),
        .max_pattern_nodes = NONZERO(o->usage.pattern_nodes),
        .max_path_steps = NONZERO(o->usage.path_steps),
        .max_propagations = NONZERO(o->usage.propagations),
        .max_arithmetic = NONZERO(o->usage.arithmetic),
        .max_equality_nodes = NONZERO(o->usage.equality_nodes),
        .max_equality_children = NONZERO(o->usage.equality_children),
        .max_snapshots = NONZERO(o->usage.snapshots),
        .max_callables = NONZERO(o->usage.callables),
        .max_handlers = NONZERO(o->usage.handlers),
        .max_predicates = NONZERO(o->usage.predicates),
        .max_predicate_bodies = NONZERO(o->usage.predicate_bodies),
        .max_predicate_blocks = NONZERO(o->usage.predicate_blocks),
        .max_predicate_inputs = NONZERO(o->usage.predicate_inputs),
        .max_predicate_values = NONZERO(o->usage.predicate_values),
        .max_predicate_instructions = NONZERO(o->usage.predicate_instructions),
        .max_predicate_edges = NONZERO(o->usage.predicate_edges),
        .max_predicate_edge_values = NONZERO(o->usage.predicate_edge_values),
        .max_predicate_operands = NONZERO(o->usage.predicate_operands),
        .max_predicate_path_steps = NONZERO(o->usage.predicate_path_steps),
        .max_predicate_pattern_nodes
            = NONZERO(o->usage.predicate_pattern_nodes),
        .max_import_envelopes = NONZERO(o->usage.import_envelopes),
        .max_import_contract_references
            = NONZERO(o->usage.import_contract_references),
        .max_import_snapshots = NONZERO(o->usage.import_snapshots),
        .max_literal_bytes = NONZERO(o->usage.literal_bytes),
        .max_recipe_ids = NONZERO(o->usage.recipe_ids),
        .max_roots = NONZERO(o->usage.roots),
        .max_provenance = NONZERO(o->usage.provenance),
        .max_owned_bytes = NONZERO(o->usage.owned_bytes),
        .max_build_scratch_bytes = NONZERO(o->usage.build_scratch_bytes),
        .max_build_work = NONZERO(o->usage.build_work),
        .max_validation_scratch_bytes
            = NONZERO(o->usage.validation_scratch_bytes),
        .max_validation_work = NONZERO(o->usage.validation_work)};
#undef NONZERO
}

static void reject_arena_header_mutations(SolMirOperations *o) {
#define REJECT_HEADER(member, type, singular) do { \
    if (o->singular##_count != 0) { \
        size_t saved_count = o->singular##_count; --o->singular##_count; \
        CHECK(!sol_mir_operations_validate(o, NULL)); \
        o->singular##_count = saved_count; \
        size_t saved_capacity = o->singular##_capacity; \
        ++o->singular##_capacity; \
        CHECK(!sol_mir_operations_validate(o, NULL)); \
        o->singular##_capacity = saved_capacity; \
        size_t saved_usage = o->usage.member; ++o->usage.member; \
        CHECK(!sol_mir_operations_validate(o, NULL)); \
        o->usage.member = saved_usage; \
    } \
} while (0);
    SOL_MIR_OPERATIONS_ARENAS(REJECT_HEADER)
#undef REJECT_HEADER
    CHECK(sol_mir_operations_validate(o, NULL));
}

static SolMirPlanSlice noncanonical_slice(size_t available) {
    return available == 0 ? (SolMirPlanSlice){1, 0}
        : (SolMirPlanSlice){0, 1};
}

typedef struct {
    size_t instructions[SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT + 1];
    size_t constructs[SOL_MIR_PREDICATE_CONSTRUCT_WRAPPER + 1];
    size_t terminators[SOL_MIR_PREDICATE_TERM_FAILURE + 1];
    size_t calls[SOL_IR_CALL_METHOD + 1];
    size_t option_constructs;
    size_t result_constructs;
    size_t bound_invocations;
} PredicateCensus;

static void reject_inactive_predicate_fields(SolMirOperations *o,
    PredicateCensus *census) {
#define REJECT_INSTRUCTION(member, value) do { \
    SolMirPredicateInstruction saved = *instruction; \
    instruction->member = (value); \
    CHECK(!sol_mir_operations_validate(o, NULL)); \
    *instruction = saved; \
} while (0)
    for (size_t i = 0; i < o->predicate_instruction_count; ++i) {
        SolMirPredicateInstruction *instruction = &o->predicate_instructions[i];
        if (census != NULL) {
            switch (instruction->kind) {
                case SOL_MIR_PREDICATE_INST_I64:
                case SOL_MIR_PREDICATE_INST_BOOL:
                case SOL_MIR_PREDICATE_INST_TEXT:
                case SOL_MIR_PREDICATE_INST_UNIT:
                case SOL_MIR_PREDICATE_INST_UNARY:
                case SOL_MIR_PREDICATE_INST_BINARY:
                case SOL_MIR_PREDICATE_INST_PROJECT:
                case SOL_MIR_PREDICATE_INST_FUNCTION:
                case SOL_MIR_PREDICATE_INST_BOUND_OPERATION:
                case SOL_MIR_PREDICATE_INST_PATTERN_TEST:
                case SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT:
                    ++census->instructions[instruction->kind];
                    break;
                case SOL_MIR_PREDICATE_INST_CONSTRUCT: {
                    ++census->instructions[instruction->kind];
                    switch (instruction->construct_kind) {
                        case SOL_MIR_PREDICATE_CONSTRUCT_RECORD:
                        case SOL_MIR_PREDICATE_CONSTRUCT_TUPLE:
                        case SOL_MIR_PREDICATE_CONSTRUCT_SUM:
                        case SOL_MIR_PREDICATE_CONSTRUCT_WRAPPER:
                            ++census->constructs[instruction->construct_kind];
                            break;
                        default: CHECK(false); break;
                    }
                    if (instruction->construct_kind
                            == SOL_MIR_PREDICATE_CONSTRUCT_SUM
                        && instruction->recipe
                            < o->layout->representation->recipe_count) {
                        SolMirRecipeKind recipe_kind = o->layout->representation
                            ->recipes[instruction->recipe].kind;
                        census->option_constructs
                            += recipe_kind == SOL_MIR_RECIPE_OPTION;
                        census->result_constructs
                            += recipe_kind == SOL_MIR_RECIPE_RESULT;
                    }
                    break;
                }
                default: CHECK(false); break;
            }
        }
        bool unary = instruction->kind == SOL_MIR_PREDICATE_INST_UNARY;
        bool binary = instruction->kind == SOL_MIR_PREDICATE_INST_BINARY;
        bool project = instruction->kind == SOL_MIR_PREDICATE_INST_PROJECT;
        bool function = instruction->kind == SOL_MIR_PREDICATE_INST_FUNCTION;
        bool bound = instruction->kind == SOL_MIR_PREDICATE_INST_BOUND_OPERATION;
        bool construct = instruction->kind == SOL_MIR_PREDICATE_INST_CONSTRUCT;
        bool pattern_test
            = instruction->kind == SOL_MIR_PREDICATE_INST_PATTERN_TEST;
        bool pattern_extract
            = instruction->kind == SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT;
        if (!unary && !binary && !project && !bound && !pattern_test
            && !pattern_extract) REJECT_INSTRUCTION(left, 0);
        if (!binary) REJECT_INSTRUCTION(right, 0);
        if (instruction->kind != SOL_MIR_PREDICATE_INST_I64)
            REJECT_INSTRUCTION(integer, 1);
        if (instruction->kind != SOL_MIR_PREDICATE_INST_BOOL)
            REJECT_INSTRUCTION(boolean, true);
        if (instruction->kind != SOL_MIR_PREDICATE_INST_TEXT)
            REJECT_INSTRUCTION(bytes,
                noncanonical_slice(o->literal_byte_count));
        if (!unary && !binary) {
            REJECT_INSTRUCTION(opcode, SOL_MIR_OPERATION_I64_NEG);
            REJECT_INSTRUCTION(failures, SOL_MIR_OPERATION_FAILURE_OVERFLOW);
        }
        if (!construct) REJECT_INSTRUCTION(operands,
            noncanonical_slice(o->predicate_operand_count));
        if (!project && !pattern_extract) REJECT_INSTRUCTION(path,
            noncanonical_slice(o->predicate_path_step_count));
        if (!pattern_test) REJECT_INSTRUCTION(pattern,
            noncanonical_slice(o->predicate_pattern_node_count));
        if (!construct) {
            REJECT_INSTRUCTION(construct_kind,
                SOL_MIR_PREDICATE_CONSTRUCT_TUPLE);
            REJECT_INSTRUCTION(variant_layout, 0);
            REJECT_INSTRUCTION(semantic_tag, 1);
        } else if (instruction->construct_kind
                != SOL_MIR_PREDICATE_CONSTRUCT_SUM) {
            REJECT_INSTRUCTION(variant_layout, 0);
            REJECT_INSTRUCTION(semantic_tag, 1);
        }
        if (!function && !bound) REJECT_INSTRUCTION(binding, 0);
    }
#undef REJECT_INSTRUCTION

#define REJECT_TERMINATOR(member, value) do { \
    SolMirPredicateTerminator saved = *term; term->member = (value); \
    CHECK(!sol_mir_operations_validate(o, NULL)); *term = saved; \
} while (0)
    for (size_t i = 0; i < o->predicate_block_count; ++i) {
        SolMirPredicateTerminator *term = &o->predicate_blocks[i].terminator;
        if (census != NULL) {
            switch (term->kind) {
                case SOL_MIR_PREDICATE_TERM_RETURN:
                case SOL_MIR_PREDICATE_TERM_JUMP:
                case SOL_MIR_PREDICATE_TERM_BRANCH:
                case SOL_MIR_PREDICATE_TERM_PROPAGATE:
                case SOL_MIR_PREDICATE_TERM_CHECK_REFINED:
                case SOL_MIR_PREDICATE_TERM_FAILURE:
                    ++census->terminators[term->kind];
                    break;
                case SOL_MIR_PREDICATE_TERM_INVOKE:
                    ++census->terminators[term->kind];
                    switch (term->call_kind) {
                        case SOL_IR_CALL_FUNCTION:
                        case SOL_IR_CALL_CALLBACK:
                        case SOL_IR_CALL_CAPABILITY:
                        case SOL_IR_CALL_METHOD:
                            ++census->calls[term->call_kind];
                            break;
                        default: CHECK(false); break;
                    }
                    if (term->callee < o->predicate_value_count) {
                        const SolMirPredicateValue *callee
                            = &o->predicate_values[term->callee];
                        if (callee->kind == SOL_MIR_PREDICATE_VALUE_INSTRUCTION
                            && callee->definition
                                < o->predicate_instruction_count
                            && o->predicate_instructions[callee->definition].kind
                                == SOL_MIR_PREDICATE_INST_BOUND_OPERATION)
                            ++census->bound_invocations;
                    }
                    break;
                default: CHECK(false); break;
            }
        }
        bool returning = term->kind == SOL_MIR_PREDICATE_TERM_RETURN;
        bool jump = term->kind == SOL_MIR_PREDICATE_TERM_JUMP;
        bool branch = term->kind == SOL_MIR_PREDICATE_TERM_BRANCH;
        bool invoke = term->kind == SOL_MIR_PREDICATE_TERM_INVOKE;
        bool propagate = term->kind == SOL_MIR_PREDICATE_TERM_PROPAGATE;
        bool refined = term->kind == SOL_MIR_PREDICATE_TERM_CHECK_REFINED;
        bool failure = term->kind == SOL_MIR_PREDICATE_TERM_FAILURE;
        if (!returning && !propagate && !refined)
            REJECT_TERMINATOR(value, 0);
        if (!branch) REJECT_TERMINATOR(condition, 0);
        if (!invoke) {
            REJECT_TERMINATOR(callee, 0); REJECT_TERMINATOR(receiver, 0);
            REJECT_TERMINATOR(receiver_access, SOL_ACCESS_SHARED);
            REJECT_TERMINATOR(arguments,
                noncanonical_slice(o->predicate_operand_count));
            REJECT_TERMINATOR(call_kind, SOL_IR_CALL_CALLBACK);
            REJECT_TERMINATOR(binding, 0); REJECT_TERMINATOR(effects, 0);
        }
        if (!invoke && !propagate && !refined)
            REJECT_TERMINATOR(result, 0);
        if (!jump) REJECT_TERMINATOR(edge, 0);
        if (!branch) {
            REJECT_TERMINATOR(true_edge, 0);
            REJECT_TERMINATOR(false_edge, 0);
        }
        if (!invoke && !propagate && !refined) {
            REJECT_TERMINATOR(normal_edge, 0);
            REJECT_TERMINATOR(failure_edge, 0);
            REJECT_TERMINATOR(result_recipe, 0);
        }
        if (!propagate) {
            REJECT_TERMINATOR(propagation_kind, SOL_IR_PROPAGATE_RESULT);
            REJECT_TERMINATOR(success_variant_layout, 0);
            REJECT_TERMINATOR(residual_variant_layout, 0);
            REJECT_TERMINATOR(success_field_layout, 0);
        }
        if (!refined) REJECT_TERMINATOR(nested_body, 0);
        if (!failure)
            REJECT_TERMINATOR(failure_kind,
                SOL_MIR_PREDICATE_FAILURE_NO_MATCH);
    }
#undef REJECT_TERMINATOR
    CHECK(sol_mir_operations_validate(o, NULL));
}

static void check_predicate_census(const PredicateCensus *census) {
    CHECK(census->instructions[SOL_MIR_PREDICATE_INST_I64] != 0);
    CHECK(census->instructions[SOL_MIR_PREDICATE_INST_BOOL] != 0);
    CHECK(census->instructions[SOL_MIR_PREDICATE_INST_TEXT] != 0);
    CHECK(census->instructions[SOL_MIR_PREDICATE_INST_UNIT] != 0);
    CHECK(census->instructions[SOL_MIR_PREDICATE_INST_UNARY] != 0);
    CHECK(census->instructions[SOL_MIR_PREDICATE_INST_BINARY] != 0);
    CHECK(census->instructions[SOL_MIR_PREDICATE_INST_PROJECT] != 0);
    CHECK(census->instructions[SOL_MIR_PREDICATE_INST_FUNCTION] != 0);
    CHECK(census->instructions[SOL_MIR_PREDICATE_INST_BOUND_OPERATION] != 0);
    CHECK(census->instructions[SOL_MIR_PREDICATE_INST_CONSTRUCT] != 0);
    CHECK(census->instructions[SOL_MIR_PREDICATE_INST_PATTERN_TEST] != 0);
    CHECK(census->instructions[SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT] != 0);
    CHECK(census->constructs[SOL_MIR_PREDICATE_CONSTRUCT_RECORD] != 0);
    CHECK(census->constructs[SOL_MIR_PREDICATE_CONSTRUCT_TUPLE] != 0);
    CHECK(census->constructs[SOL_MIR_PREDICATE_CONSTRUCT_SUM] != 0);
    CHECK(census->constructs[SOL_MIR_PREDICATE_CONSTRUCT_WRAPPER] != 0);
    CHECK(census->option_constructs != 0 && census->result_constructs != 0);
    CHECK(census->terminators[SOL_MIR_PREDICATE_TERM_RETURN] != 0);
    CHECK(census->terminators[SOL_MIR_PREDICATE_TERM_JUMP] != 0);
    CHECK(census->terminators[SOL_MIR_PREDICATE_TERM_BRANCH] != 0);
    CHECK(census->terminators[SOL_MIR_PREDICATE_TERM_INVOKE] != 0);
    CHECK(census->terminators[SOL_MIR_PREDICATE_TERM_PROPAGATE] == 0);
    CHECK(census->terminators[SOL_MIR_PREDICATE_TERM_CHECK_REFINED] != 0);
    CHECK(census->terminators[SOL_MIR_PREDICATE_TERM_FAILURE] != 0);
    CHECK(census->calls[SOL_IR_CALL_FUNCTION] != 0);
    CHECK(census->calls[SOL_IR_CALL_CALLBACK] == 0);
    CHECK(census->calls[SOL_IR_CALL_CAPABILITY] != 0);
    CHECK(census->calls[SOL_IR_CALL_METHOD] != 0);
    CHECK(census->bound_invocations != 0);
}

static void reject_forged_predicate_propagation(SolMirOperations *o) {
    size_t instruction_id = 0;
    while (instruction_id < o->predicate_instruction_count) {
        const SolMirPredicateInstruction *instruction
            = &o->predicate_instructions[instruction_id];
        if (instruction->kind == SOL_MIR_PREDICATE_INST_CONSTRUCT
            && instruction->recipe < o->layout->representation->recipe_count
            && o->layout->representation->recipes[instruction->recipe].kind
                == SOL_MIR_RECIPE_OPTION) break;
        ++instruction_id;
    }
    CHECK(instruction_id < o->predicate_instruction_count);
    if (instruction_id >= o->predicate_instruction_count) return;
    const SolMirPredicateInstruction *instruction
        = &o->predicate_instructions[instruction_id];
    SolMirPredicateTerminator *term
        = &o->predicate_blocks[instruction->block].terminator;
    SolMirPredicateTerminator saved = *term;
    SolMirPredicateTerminator forged;
    memset(&forged, 0, sizeof(forged));
    forged.kind = SOL_MIR_PREDICATE_TERM_PROPAGATE;
    forged.value = instruction->result;
    forged.condition = forged.callee = forged.receiver = SOL_MIR_OPERATION_NONE;
    forged.binding = forged.effects = SOL_MIR_MATERIALIZED_NONE;
    forged.result = instruction->result;
    forged.edge = forged.true_edge = forged.false_edge = SOL_MIR_OPERATION_NONE;
    forged.normal_edge = forged.failure_edge = SOL_MIR_OPERATION_NONE;
    forged.propagation_kind = SOL_IR_PROPAGATE_OPTION;
    forged.success_variant_layout = o->layout->representation->variant_count;
    forged.residual_variant_layout = SOL_MIR_OPERATION_NONE;
    forged.success_field_layout = SOL_MIR_OPERATION_NONE;
    forged.nested_body = SOL_MIR_OPERATION_NONE;
    forged.result_recipe = instruction->recipe;
    forged.failure_kind = SOL_MIR_PREDICATE_FAILURE_CALL;
    *term = forged;
    CHECK(!sol_mir_operations_validate(o, NULL));
    /* PROPAGATE owns normal_edge/failure_edge; the legacy jump edge is inactive. */
    term->edge = 0;
    CHECK(!sol_mir_operations_validate(o, NULL));
    *term = saved;
    CHECK(sol_mir_operations_validate(o, NULL));
}

static void test_predicate_body_count_wrap_rejection(void) {
    static const char source[] =
        "module body_count_wrap\n"
        "function root(value: Int64) -> Bool effects { pure } "
        "requires { value > 0 value == value !false } { return true }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, NULL, 0, &p, NULL);
    CHECK(built && p.operations.predicate_body_count >= 2);
    if (built && p.operations.predicate_body_count >= 2) {
        SolMirOperations *o = &p.operations;
        SolMirPredicateBody *saved_bodies
            = malloc(o->predicate_body_count * sizeof(*saved_bodies));
        SolMirPredicateBodyId *saved_owners
            = malloc(o->predicate_block_count * sizeof(*saved_owners));
        CHECK(saved_bodies != NULL
            && (o->predicate_block_count == 0 || saved_owners != NULL));
        if (saved_bodies != NULL
            && (o->predicate_block_count == 0 || saved_owners != NULL)) {
            memcpy(saved_bodies, o->predicate_bodies,
                o->predicate_body_count * sizeof(*saved_bodies));
            for (size_t i = 0; i < o->predicate_block_count; ++i)
                saved_owners[i] = o->predicate_blocks[i].body;
#define WRAP_BODY_SLICES(member, total) do { \
    size_t wrapped = 0; \
    for (size_t i = 0; i < o->predicate_body_count; ++i) { \
        o->predicate_bodies[i].member.offset = wrapped; \
        o->predicate_bodies[i].member.count = i == 0 \
            ? SIZE_MAX - (o->predicate_body_count - 2) \
            : i + 1 == o->predicate_body_count ? (total) + 1 : 1; \
        wrapped += o->predicate_bodies[i].member.count; \
    } \
    CHECK(wrapped == (total)); \
} while (0)
            WRAP_BODY_SLICES(inputs, o->predicate_input_count);
            CHECK(!sol_mir_operations_validate(o, NULL));
            memcpy(o->predicate_bodies, saved_bodies,
                o->predicate_body_count * sizeof(*saved_bodies));
            WRAP_BODY_SLICES(values, o->predicate_value_count);
            CHECK(!sol_mir_operations_validate(o, NULL));
            memcpy(o->predicate_bodies, saved_bodies,
                o->predicate_body_count * sizeof(*saved_bodies));
            for (size_t i = 0; i < o->predicate_block_count; ++i)
                o->predicate_blocks[i].body = 0;
            WRAP_BODY_SLICES(blocks, o->predicate_block_count);
            for (size_t i = 0; i < o->predicate_body_count; ++i)
                o->predicate_bodies[i].entry = o->predicate_bodies[i].blocks.offset;
            CHECK(!sol_mir_operations_validate(o, NULL));
#undef WRAP_BODY_SLICES
            memcpy(o->predicate_bodies, saved_bodies,
                o->predicate_body_count * sizeof(*saved_bodies));
            for (size_t i = 0; i < o->predicate_block_count; ++i)
                o->predicate_blocks[i].body = saved_owners[i];
            CHECK(sol_mir_operations_validate(o, NULL));
        }
        free(saved_owners); free(saved_bodies);
    }
    pipeline_free(&p); free_text(&c);
}

static void test_contract_propagation_rejection(void) {
    static const char source[] =
        "module contract_propagation\n"
        "function root(value: Option<Bool>) -> Option<Bool> effects { pure } "
        "requires { value? } { return value }\n";
    Compilation c; bool compiled = compile_text(&c, source);
    bool rejected = false;
    for (size_t i = 0; i < c.diagnostics.count; ++i)
        rejected = rejected
            || strcmp(c.diagnostics.items[i].code, "SOL-CONTRACT-002") == 0;
    CHECK(!compiled && rejected);
    free_text(&c);
}

static void test_dynamic_predicate_callback_rejection(void) {
    static const char source[] =
        "module dynamic_predicate_callback\n"
        "function root(callback: function(Int64) -> Bool effects { pure }) "
        "-> Bool effects { pure } requires { callback(1) } { return true }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program; sol_mir_program_init(&program);
    SolMirProgramBuildRequest request = {&c.ir, &root, 1, NULL, 0, NULL};
    CHECK(sol_mir_program_build(&request, &program, &c.diagnostics)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    CHECK(program.ir == NULL);
    sol_mir_program_free(&program); free_text(&c);
}

static bool collect_pattern_bindings(const SolIr *ir, size_t pattern_id,
    size_t depth, size_t ids[2], size_t *count) {
    if (pattern_id >= ir->pattern_count || depth > ir->pattern_count) return false;
    const SolIrPattern *pattern = &ir->patterns[pattern_id];
    if (pattern->kind == SOL_IR_PATTERN_BINDING && *count < 2)
        ids[(*count)++] = pattern_id;
    if (pattern->children.offset > ir->pattern_child_count
        || pattern->children.count
            > ir->pattern_child_count - pattern->children.offset) return false;
    for (size_t i = 0; i < pattern->children.count && *count < 2; ++i)
        if (!collect_pattern_bindings(ir,
                ir->pattern_children[pattern->children.offset + i].pattern,
                depth + 1, ids, count)) return false;
    return true;
}

static void swap_pattern_ids(SolIr *ir, size_t left, size_t right) {
    SolIrPattern saved = ir->patterns[left];
    ir->patterns[left] = ir->patterns[right];
    ir->patterns[right] = saved;
    for (size_t i = 0; i < ir->pattern_child_count; ++i) {
        if (ir->pattern_children[i].pattern == left)
            ir->pattern_children[i].pattern = right;
        else if (ir->pattern_children[i].pattern == right)
            ir->pattern_children[i].pattern = left;
    }
    for (size_t i = 0; i < ir->arm_count; ++i) {
        if (ir->arms[i].pattern == left) ir->arms[i].pattern = right;
        else if (ir->arms[i].pattern == right) ir->arms[i].pattern = left;
    }
}

static void test_match_binding_dfs_authentication(void) {
    static const char source[] =
        "module match_binding_order\n"
        "enum Pair { pair(first: Int64, second: Int64) }\n"
        "function root(value: Pair) -> Bool effects { pure } requires { "
        "match value { pair(first, second) => first < second } } "
        "{ return true }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) {
        sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
        free_text(&c); return;
    }
    size_t arm_id = SOL_IR_NONE;
    for (size_t i = 0; i < c.ir.arm_count; ++i)
        if (c.ir.arms[i].bindings.count >= 2) { arm_id = i; break; }
    CHECK(arm_id < c.ir.arm_count);
    if (arm_id >= c.ir.arm_count) { free_text(&c); return; }
    size_t bindings[2], binding_count = 0;
    CHECK(collect_pattern_bindings(&c.ir, c.ir.arms[arm_id].pattern, 0,
        bindings, &binding_count));
    CHECK(binding_count == 2);
    if (binding_count == 2 && bindings[0] < bindings[1])
        swap_pattern_ids(&c.ir, bindings[0], bindings[1]);
    binding_count = 0;
    CHECK(collect_pattern_bindings(&c.ir, c.ir.arms[arm_id].pattern, 0,
        bindings, &binding_count));
    CHECK(binding_count == 2 && bindings[0] > bindings[1]);
    CHECK(sol_ir_validate(&c.ir, NULL));
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, NULL, 0, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built && sol_mir_operations_validate(&p.operations, NULL));
    if (built) {
        size_t extracts[2], extract_count = 0;
        for (size_t i = 0; i < p.operations.predicate_instruction_count
            && extract_count < 2; ++i)
            if (p.operations.predicate_instructions[i].kind
                    == SOL_MIR_PREDICATE_INST_PATTERN_EXTRACT)
                extracts[extract_count++] = i;
        CHECK(extract_count == 2);
        if (extract_count == 2) {
            SolMirPlanSlice first
                = p.operations.predicate_instructions[extracts[0]].path;
            p.operations.predicate_instructions[extracts[0]].path
                = p.operations.predicate_instructions[extracts[1]].path;
            p.operations.predicate_instructions[extracts[1]].path = first;
            CHECK(!sol_mir_operations_validate(&p.operations, NULL));
            p.operations.predicate_instructions[extracts[1]].path
                = p.operations.predicate_instructions[extracts[0]].path;
            p.operations.predicate_instructions[extracts[0]].path = first;
            CHECK(sol_mir_operations_validate(&p.operations, NULL));
        }
    }
    pipeline_free(&p); free_text(&c);
}

static void test_e6_operations(void) {
    Compilation c; SolPackage package;
    bool compiled = compile_e6(&c, &package); CHECK(compiled);
    if (!compiled) { free_e6(&c, &package); return; }
    const char *names[] = {"write", "get", "count", "read"};
    SolIrCallableId imports[4];
    for (size_t i = 0; i < 4; ++i)
        imports[i] = callable(&c.ir, names[i], SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot roots[5] = {{callable(&c.ir, "launch",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_ENTRY}};
    size_t root_count = 1;
    for (size_t i = 0; i < c.ir.callable_count; ++i)
        if (c.ir.callables[i].kind == SOL_IR_CALLABLE_TEST)
            roots[root_count++] = (SolMirProgramRoot){i, SOL_MIR_PROGRAM_ROOT_TEST};
    Pipeline p; pipeline_init(&p);
    bool built = root_count == 5 && build_pipeline(&c.ir, roots, root_count,
        imports, 4, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &package.source, &p.diagnostics);
    CHECK(built);
    if (!built) { pipeline_free(&p); free_e6(&c, &package); return; }
    SolMirOperations *o = &p.operations;
    CHECK(sol_mir_operations_validate(o, NULL));
    CHECK(o->access_plan_count == p.materialization.place_count
        && o->access_step_count == 5);
    CHECK(o->constructor_count == 28 && o->arithmetic_count == 17
        && o->pattern_test_count == 3 && o->pattern_extraction_count == 3
        && o->propagation_count == 2 && o->snapshot_count == 1
        && o->predicate_count == 4 && o->handler_count == 0
        && o->callable_count == 0);
    size_t checked = 0, comparison = 0, equality = 0, compound = 0;
    for (size_t i = 0; i < o->arithmetic_count; ++i) {
        checked += o->arithmetic[i].failures != SOL_MIR_OPERATION_FAILURE_NONE;
        comparison += o->arithmetic[i].opcode >= SOL_MIR_OPERATION_I64_LT
            && o->arithmetic[i].opcode <= SOL_MIR_OPERATION_I64_GE;
        equality += o->arithmetic[i].opcode == SOL_MIR_OPERATION_VALUE_EQ
            || o->arithmetic[i].opcode == SOL_MIR_OPERATION_VALUE_NE;
        compound += o->arithmetic[i].compound;
    }
    /* The current E6 closure has four checked binary operations plus its one
       checked compound update; its remaining twelve operations compare. */
    CHECK(checked == 5 && comparison + equality == 12 && compound == 1);
    size_t contract = 0, refinement = 0;
    for (size_t i = 0; i < o->predicate_count; ++i) {
        contract += o->predicates[i].kind == SOL_MIR_OPERATION_PREDICATE_CONTRACT;
        refinement += o->predicates[i].kind == SOL_MIR_OPERATION_PREDICATE_REFINEMENT;
        CHECK(o->predicates[i].body < o->predicate_body_count);
    }
    CHECK(contract == 3 && refinement == 1);
    CHECK(o->predicate_body_count == 4 && o->predicate_block_count == 4
        && o->predicate_input_count == 4 && o->predicate_value_count == 10
        && o->predicate_instruction_count == 6
        && o->import_envelope_count == 4);
    size_t predicate_constants = 0, predicate_comparisons = 0;
    for (size_t i = 0; i < o->predicate_instruction_count; ++i) {
        predicate_constants += o->predicate_instructions[i].kind
                == SOL_MIR_PREDICATE_INST_I64
            || o->predicate_instructions[i].kind == SOL_MIR_PREDICATE_INST_BOOL;
        predicate_comparisons += o->predicate_instructions[i].kind
                == SOL_MIR_PREDICATE_INST_BINARY
            && o->predicate_instructions[i].opcode >= SOL_MIR_OPERATION_I64_LT
            && o->predicate_instructions[i].opcode <= SOL_MIR_OPERATION_VALUE_NE;
    }
    CHECK(predicate_constants == 3 && predicate_comparisons == 3);
    for (size_t i = 0; i < o->propagation_count; ++i) {
        const SolMirOperationPropagationPlan *plan = &o->propagations[i];
        if (plan->source_residual_field_layout == SOL_MIR_OPERATION_NONE) {
            CHECK(plan->destination_residual_field_layout == SOL_MIR_OPERATION_NONE
                && plan->source_residual_field_offset == SOL_MIR_LAYOUT_OFFSET_NONE
                && plan->destination_residual_field_offset
                    == SOL_MIR_LAYOUT_OFFSET_NONE);
            continue;
        }
        CHECK(p.representation.fields[plan->source_residual_field_layout].type
            == p.representation.fields[plan->destination_residual_field_layout].type);
    }
    size_t first_length = 0, second_length = 0;
    char *first = render(o, &first_length);
    SolMirOperations repeated; sol_mir_operations_init(&repeated);
    SolMirOperationsBuildRequest request = {&p.layout, NULL};
    CHECK(sol_mir_operations_build(&request, &repeated, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED);
    char *second = render(&repeated, &second_length);
    CHECK(first != NULL && second != NULL && first_length == second_length
        && memcmp(first, second, first_length) == 0);
    CHECK(first != NULL && strstr(first, "access_step ") != NULL
        && strstr(first, "construct_write ") != NULL
        && strstr(first, "pattern_node ") != NULL
        && strstr(first, "path_step ") != NULL
        && strstr(first, "equality_node ") != NULL
        && strstr(first, "equality_child ") != NULL
        && strstr(first, "provenance ") != NULL);
    free(first); free(second); sol_mir_operations_free(&repeated);

    SolMirOperationsLimits exact = exact_limits(o);
    size_t prerequisite_work = p.layout.usage.validation_work;
    size_t local_work = o->usage.validation_work - prerequisite_work;
    size_t local_scratch = o->provenance_count > p.representation.recipe_count
        ? o->provenance_count : p.representation.recipe_count;
    CHECK(o->usage.validation_work > prerequisite_work && local_work != 0);
    CHECK(p.layout.usage.validation_scratch_bytes > local_scratch
        && o->usage.validation_scratch_bytes
            == p.layout.usage.validation_scratch_bytes);
    request.limits = &exact;
    CHECK(sol_mir_operations_build(&request, &repeated, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED);
    CHECK(sol_mir_operations_validate(&repeated, NULL));
    sol_mir_operations_free(&repeated);
    SolMirOperationsLimits local_only = exact;
    local_only.max_validation_work = local_work;
    request.limits = &local_only;
    CHECK(sol_mir_operations_build(&request, &repeated, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED
        && repeated.layout == NULL);
    local_only = exact;
    local_only.max_validation_scratch_bytes = local_scratch;
    request.limits = &local_only;
    CHECK(sol_mir_operations_build(&request, &repeated, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED
        && repeated.layout == NULL);
    request.limits = &exact;
#define LESS(member) do { SolMirOperationsLimits less = exact; --less.member; \
    request.limits = &less; CHECK(sol_mir_operations_build(&request, &repeated, \
        &p.diagnostics) == (exact.member == 1 \
            ? SOL_MIR_OPERATIONS_BUILD_INVALID_ARGUMENT \
            : SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED)); \
    CHECK(repeated.layout == NULL); } while (0)
    LESS(max_access_plans); LESS(max_access_steps); LESS(max_constructors);
    LESS(max_construct_operands); LESS(max_pattern_tests);
    LESS(max_pattern_extractions); LESS(max_pattern_nodes); LESS(max_path_steps);
    LESS(max_propagations); LESS(max_arithmetic); LESS(max_equality_nodes);
    LESS(max_equality_children);
    LESS(max_snapshots); LESS(max_predicates); LESS(max_recipe_ids);
    LESS(max_predicate_bodies); LESS(max_predicate_blocks);
    LESS(max_predicate_inputs); LESS(max_predicate_values);
    LESS(max_predicate_instructions); LESS(max_predicate_edges);
    LESS(max_predicate_edge_values); LESS(max_predicate_operands);
    LESS(max_predicate_path_steps); LESS(max_predicate_pattern_nodes);
    LESS(max_import_envelopes);
    LESS(max_import_contract_references); LESS(max_import_snapshots);
    LESS(max_literal_bytes);
    LESS(max_provenance); LESS(max_owned_bytes); LESS(max_build_scratch_bytes);
    LESS(max_build_work); LESS(max_validation_scratch_bytes);
    LESS(max_validation_work);
#undef LESS
    request.limits = NULL;

#define MUTATE(arena, index, member, value) do { \
    unsigned char saved[sizeof(o->arena[index])]; \
    memcpy(saved, &o->arena[index], sizeof(saved)); \
    o->arena[index].member = (value); CHECK(!sol_mir_operations_validate(o, NULL)); \
    memcpy(&o->arena[index], saved, sizeof(saved)); } while (0)
    MUTATE(access_plans, 0, place, SOL_MIR_OPERATION_NONE);
    MUTATE(access_plans, 0, image, SOL_MIR_OPERATION_NONE);
    MUTATE(access_plans, 0, local, SOL_MIR_OPERATION_NONE);
    MUTATE(access_plans, 0, steps.count, o->access_plans[0].steps.count + 1);
    MUTATE(access_steps, 0, object_offset, o->access_steps[0].object_offset ^ 1u);
    MUTATE(constructors, 0, kind, (SolMirOperationConstructKind)99);
    MUTATE(constructors, 0, result_recipe, SOL_MIR_RECIPE_NONE);
    MUTATE(construct_operands, 0, layout_field, SOL_MIR_OPERATION_NONE);
    MUTATE(construct_operands, 0, temporary, SOL_MIR_OPERATION_NONE);
    MUTATE(pattern_tests, 0, nodes.offset, SOL_MIR_OPERATION_NONE);
    MUTATE(pattern_nodes, 0, kind, (SolMirOperationPatternKind)99);
    MUTATE(pattern_nodes, 0, boolean, !o->pattern_nodes[0].boolean);
    MUTATE(path_steps, 0, field_layout, SOL_MIR_OPERATION_NONE);
    MUTATE(pattern_extractions, 0, copy_kind, SOL_MIR_COPY_FORBIDDEN);
    MUTATE(pattern_extractions, 0, path.offset, SOL_MIR_OPERATION_NONE);
    MUTATE(propagations, 0, success_variant_layout, SOL_MIR_OPERATION_NONE);
    MUTATE(propagations, 0, success_tag, o->propagations[0].success_tag ^ 1u);
    MUTATE(propagations, 0, success_field_layout, SOL_MIR_OPERATION_NONE);
    MUTATE(propagations, 0, success_field_offset,
        o->propagations[0].success_field_offset ^ 1u);
    MUTATE(propagations, 0, source_residual_variant_layout,
        SOL_MIR_OPERATION_NONE);
    MUTATE(propagations, 0, source_residual_tag,
        o->propagations[0].source_residual_tag ^ 1u);
    MUTATE(propagations, 0, destination_residual_variant_layout,
        SOL_MIR_OPERATION_NONE);
    MUTATE(propagations, 0, destination_residual_tag,
        o->propagations[0].destination_residual_tag ^ 1u);
    size_t result_propagation = 0;
    while (result_propagation < o->propagation_count
        && o->propagations[result_propagation].source_residual_field_layout
            == SOL_MIR_OPERATION_NONE) ++result_propagation;
    CHECK(result_propagation < o->propagation_count);
    if (result_propagation < o->propagation_count) {
        MUTATE(propagations, result_propagation, source_residual_field_layout,
            SOL_MIR_OPERATION_NONE);
        MUTATE(propagations, result_propagation, source_residual_field_recipe,
            SOL_MIR_RECIPE_NONE);
        MUTATE(propagations, result_propagation, source_residual_field_offset,
            o->propagations[result_propagation].source_residual_field_offset ^ 1u);
        MUTATE(propagations, result_propagation, destination_residual_field_layout,
            SOL_MIR_OPERATION_NONE);
        MUTATE(propagations, result_propagation,
            destination_residual_field_recipe, SOL_MIR_RECIPE_NONE);
        MUTATE(propagations, result_propagation,
            destination_residual_field_offset,
            o->propagations[result_propagation].destination_residual_field_offset ^ 1u);
    }
    MUTATE(propagations, 0, residual_edge, SOL_MIR_OPERATION_NONE);
    MUTATE(arithmetic, 0, opcode, (SolMirOperationOpcode)99);
    MUTATE(arithmetic, 0, instruction, SOL_MIR_OPERATION_NONE);
    size_t binary_operation = 0;
    while (binary_operation < o->arithmetic_count
        && o->arithmetic[binary_operation].left == SOL_MIR_MATERIALIZED_NONE)
        ++binary_operation;
    CHECK(binary_operation < o->arithmetic_count);
    if (binary_operation < o->arithmetic_count)
        MUTATE(arithmetic, binary_operation, left, SOL_MIR_OPERATION_NONE);
    MUTATE(arithmetic, 0, failures, o->arithmetic[0].failures ^ 1u);
    MUTATE(equality_nodes, 0, kind, (SolMirOperationEqualityKind)99);
    MUTATE(equality_children, 0, recipe, SOL_MIR_RECIPE_NONE);
    MUTATE(equality_children, 0, field_layout, SOL_MIR_OPERATION_NONE);
    MUTATE(snapshots, 0, slot, 2);
    MUTATE(snapshots, 0, context, SOL_MIR_OPERATION_NONE);
    MUTATE(snapshots, 0, path.count, o->snapshots[0].path.count + 1);
    MUTATE(predicates, 0, body, SOL_MIR_OPERATION_NONE);
    MUTATE(predicates, 0, context, SOL_MIR_OPERATION_NONE);
    MUTATE(predicates, 0, output_recipe, SOL_MIR_RECIPE_NONE);
    MUTATE(predicate_bodies, 0, entry, SOL_MIR_OPERATION_NONE);
    MUTATE(predicate_blocks, 0, terminator.value, SOL_MIR_OPERATION_NONE);
    MUTATE(predicate_values, 0, recipe, SOL_MIR_RECIPE_NONE);
    MUTATE(predicate_instructions, 0, result, SOL_MIR_OPERATION_NONE);
    MUTATE(import_envelopes, 0, import, SOL_MIR_OPERATION_NONE);
    MUTATE(import_envelopes, 0, host_invoke, false);
    size_t integer_instruction = 0;
    while (integer_instruction < o->predicate_instruction_count
        && o->predicate_instructions[integer_instruction].kind
            != SOL_MIR_PREDICATE_INST_I64) ++integer_instruction;
    CHECK(integer_instruction < o->predicate_instruction_count);
    if (integer_instruction < o->predicate_instruction_count)
        MUTATE(predicate_instructions, integer_instruction, integer, 42);
    MUTATE(predicate_inputs, 0, ordinal, o->predicate_inputs[0].ordinal + 1);
    MUTATE(predicate_inputs, 0, kind, SOL_MIR_PREDICATE_INPUT_SNAPSHOT);
    MUTATE(predicate_inputs, 0, access, SOL_ACCESS_SHARED);
    size_t binary_predicate = 0;
    while (binary_predicate < o->predicate_instruction_count
        && o->predicate_instructions[binary_predicate].kind
            != SOL_MIR_PREDICATE_INST_BINARY) ++binary_predicate;
    CHECK(binary_predicate < o->predicate_instruction_count);
    if (binary_predicate < o->predicate_instruction_count) {
        MUTATE(predicate_instructions, binary_predicate, opcode,
            SOL_MIR_OPERATION_I64_ADD);
        MUTATE(predicate_instructions, binary_predicate, failures,
            SOL_MIR_OPERATION_FAILURE_OVERFLOW);
    }
    MUTATE(predicate_bodies, 0, phase, SOL_CONTRACT_ENSURES);
    MUTATE(predicate_bodies, 0, outcome, SOL_CONTRACT_OUTCOME_FAILURE);
    MUTATE(predicate_bodies, 0, context, o->predicate_bodies[1].context);
    MUTATE(predicate_bodies, 0, owner_kind, SOL_MIR_PREDICATE_OWNER_IMPORT);
    if (o->predicate_body_count > 1) {
        SolMirPredicateBody first_body = o->predicate_bodies[0];
        o->predicate_bodies[0] = o->predicate_bodies[1];
        o->predicate_bodies[1] = first_body;
        CHECK(!sol_mir_operations_validate(o, NULL));
        o->predicate_bodies[1] = o->predicate_bodies[0];
        o->predicate_bodies[0] = first_body;
    }
    MUTATE(provenance, 0, source_expression,
        o->provenance[0].source_expression ^ 1u);
    size_t body_provenance = 0;
    while (body_provenance < o->provenance_count
        && o->provenance[body_provenance].kind
            != SOL_MIR_OPERATION_PROVENANCE_PREDICATE_BODY) ++body_provenance;
    CHECK(body_provenance < o->provenance_count);
    if (body_provenance < o->provenance_count) {
        MUTATE(provenance, body_provenance, source_obligation, SOL_IR_NONE);
        MUTATE(provenance, body_provenance, source_definition, 0);
    }
    size_t capability = 0;
    while (capability < o->constructor_count
        && o->constructors[capability].kind
            != SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY) ++capability;
    if (capability < o->constructor_count) {
        MUTATE(constructors, capability, capability_rule,
            SOL_MIR_OPERATION_CAPABILITY_NONE);
        MUTATE(constructors, capability, capability_source_operand,
            SOL_MIR_OPERATION_NONE);
        MUTATE(constructors, capability, inherited_root,
            SOL_MIR_MATERIALIZED_NONE);
    }
#undef MUTATE
    size_t saved_usage = o->usage.constructors; ++o->usage.constructors;
    CHECK(!sol_mir_operations_validate(o, NULL)); o->usage.constructors = saved_usage;
    saved_usage = o->usage.validation_work; ++o->usage.validation_work;
    CHECK(!sol_mir_operations_validate(o, NULL));
    o->usage.validation_work = saved_usage;
    if (saved_usage != 0) {
        --o->usage.validation_work;
        CHECK(!sol_mir_operations_validate(o, NULL));
        o->usage.validation_work = saved_usage;
    }
    size_t saved_validation_limit = o->limits.max_validation_work;
    o->limits.max_validation_work = local_work;
    CHECK(!sol_mir_operations_validate(o, NULL));
    o->limits.max_validation_work = saved_validation_limit;
    size_t saved_validation_scratch_limit
        = o->limits.max_validation_scratch_bytes;
    o->limits.max_validation_scratch_bytes = local_scratch;
    CHECK(!sol_mir_operations_validate(o, NULL));
    o->limits.max_validation_scratch_bytes
        = saved_validation_scratch_limit;
    saved_usage = o->usage.build_work;
    CHECK(saved_usage < o->limits.max_build_work);
    if (saved_usage < o->limits.max_build_work) {
        ++o->usage.build_work;
        CHECK(!sol_mir_operations_validate(o, NULL));
        o->usage.build_work = saved_usage;
    }
    CHECK(saved_usage != 0);
    if (saved_usage != 0) {
        --o->usage.build_work;
        CHECK(!sol_mir_operations_validate(o, NULL));
        o->usage.build_work = saved_usage;
    }
    size_t independently_expected = 0;
    CHECK(sol_mir_operations_internal_expected_build_work(o,
        &independently_expected) && independently_expected == saved_usage);
    SolMirOperationConstructPlan saved_constructor = o->constructors[0];
    o->constructors[0].result_recipe = SOL_MIR_RECIPE_NONE;
    size_t expected_after_output_mutation = 0;
    CHECK(sol_mir_operations_internal_expected_build_work(o,
        &expected_after_output_mutation)
        && expected_after_output_mutation == independently_expected);
    o->constructors[0] = saved_constructor;
    size_t saved_capacity = o->constructor_capacity; ++o->constructor_capacity;
    CHECK(!sol_mir_operations_validate(o, NULL)); o->constructor_capacity = saved_capacity;
    SolMirOperationConstructPlan *saved_pointer = o->constructors;
    o->constructors = (SolMirOperationConstructPlan *)(void *)o->access_plans;
    CHECK(!sol_mir_operations_validate(o, NULL)); o->constructors = saved_pointer;
    reject_arena_header_mutations(o);
#define ALIAS_ACCESS(pointer) do { \
    SolMirOperationAccessPlan *saved_alias = o->access_plans; \
    o->access_plans = (SolMirOperationAccessPlan *)(void *)(pointer); \
    CHECK(!sol_mir_operations_validate(o, NULL)); o->access_plans = saved_alias; \
} while (0)
    ALIAS_ACCESS(p.layout.types); ALIAS_ACCESS(p.representation.recipes);
    ALIAS_ACCESS(p.materialization.type_ids); ALIAS_ACCESS(p.materialization.edges);
    ALIAS_ACCESS(p.materialization.semantic_sites); ALIAS_ACCESS(p.plan.types);
    ALIAS_ACCESS(p.program.templates); ALIAS_ACCESS(p.program.templates[0].mir.values);
    ALIAS_ACCESS(c.ir.patterns); ALIAS_ACCESS(c.ir.source_bytes);
#undef ALIAS_ACCESS
    char *saved_source_path = c.ir.source_path;
    c.ir.source_path = (char *)(void *)o->access_plans;
    CHECK(!sol_mir_operations_validate(o, NULL));
    c.ir.source_path = saved_source_path;
    FILE *stream = tmpfile(); CHECK(stream != NULL);
    if (stream != NULL) {
        SolMirRecipeId saved = o->constructors[0].result_recipe;
        o->constructors[0].result_recipe = SOL_MIR_RECIPE_NONE;
        CHECK(!sol_mir_operations_render(stream, o) && ftell(stream) == 0);
        o->constructors[0].result_recipe = saved; fclose(stream);
    }
    CHECK(sol_mir_operations_validate(o, NULL));
    SolMirOperationsLimits partial = exact;
    partial.max_roots = 0;
    request.limits = &partial;
    CHECK(sol_mir_operations_build(&request, &repeated, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_INVALID_ARGUMENT && repeated.layout == NULL);
    size_t saved_limit = o->limits.max_roots; o->limits.max_roots = 0;
    CHECK(!sol_mir_operations_validate(o, NULL)); o->limits.max_roots = saved_limit;
    pipeline_free(&p); free_e6(&c, &package);
}

static void test_cross_recipe_result_propagation(void) {
    static const char source[] =
        "module propagation\n"
        "function adapt(value: Result<Int64, Text>) -> Result<Bool, Text> { "
        "let number = value? return ok(number > 0) }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolMirProgramRoot root = {callable(&c.ir, "adapt", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, NULL, 0, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built && p.operations.propagation_count == 1);
    if (built) {
        SolMirOperationPropagationPlan *plan = &p.operations.propagations[0];
        CHECK(plan->source_recipe != plan->residual_recipe
            && plan->source_residual_field_layout
                != plan->destination_residual_field_layout
            && plan->source_residual_field_offset
                == p.layout.fields[plan->source_residual_field_layout].offset
            && plan->destination_residual_field_offset
                == p.layout.fields[plan->destination_residual_field_layout].offset
            && p.representation.fields[plan->source_residual_field_layout].type
                == p.representation.fields[
                    plan->destination_residual_field_layout].type);
        size_t rendered_length = 0;
        char *rendered = render(&p.operations, &rendered_length);
        CHECK(rendered != NULL && rendered_length != 0
            && strstr(rendered, "source_residual=") != NULL
            && strstr(rendered, "destination_residual=") != NULL);
        free(rendered);
#define MUTATE_PROP(member, value) do { \
    SolMirOperationPropagationPlan saved = *plan; plan->member = (value); \
    CHECK(!sol_mir_operations_validate(&p.operations, NULL)); *plan = saved; \
} while (0)
        MUTATE_PROP(source_residual_field_layout, SOL_MIR_OPERATION_NONE);
        MUTATE_PROP(source_residual_field_offset,
            plan->source_residual_field_offset ^ UINT64_C(1));
        MUTATE_PROP(destination_residual_field_layout, SOL_MIR_OPERATION_NONE);
        MUTATE_PROP(destination_residual_field_offset,
            plan->destination_residual_field_offset ^ UINT64_C(1));
#undef MUTATE_PROP
    }
    pipeline_free(&p); free_text(&c);
}

static void test_capability_constructor_plan(void) {
    static const char source[] =
        "module capability_constructor\n"
        "capability Clock { function now() -> Int64 effects { clock.read<Self> } }\n"
        "capability Wrapped derives_from private_source: capability Clock {}\n"
        "function wrap(clock: capability Clock) -> capability Wrapped effects { pure } "
        "{ return Wrapped { private_source = clock } }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolMirProgramRoot root = {callable(&c.ir, "wrap", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, NULL, 0, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built && p.operations.constructor_count == 1);
    if (built) {
        SolMirOperationConstructPlan *plan = &p.operations.constructors[0];
        CHECK(plan->kind == SOL_MIR_OPERATION_CONSTRUCT_CAPABILITY
            && plan->capability_rule != SOL_MIR_OPERATION_CAPABILITY_NONE
            && plan->capability_source_operand == 0
            && plan->inherited_root < p.materialization.local_count);
#define MUTATE_CAP(member, value) do { SolMirOperationConstructPlan saved = *plan; \
    plan->member = (value); CHECK(!sol_mir_operations_validate(&p.operations, NULL)); \
    *plan = saved; } while (0)
        MUTATE_CAP(capability_rule, SOL_MIR_OPERATION_CAPABILITY_NONE);
        MUTATE_CAP(capability_source_operand, SOL_MIR_OPERATION_NONE);
        MUTATE_CAP(inherited_root, SOL_MIR_MATERIALIZED_NONE);
#undef MUTATE_CAP
    }
    pipeline_free(&p); free_text(&c);
}

static void test_recursive_equality_graph(void) {
    static const char source[] =
        "module recursive_equality\n"
        "record Node { next: Option<Node> }\n"
        "function same(left: Node, right: Node) -> Bool { return left == right }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolMirProgramRoot root = {callable(&c.ir, "same", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, NULL, 0, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built && p.operations.arithmetic_count == 1
        && p.operations.equality_node_count >= 2
        && p.operations.equality_node_count < p.representation.recipe_count);
    if (built) {
        CHECK(sol_mir_operations_validate(&p.operations, NULL));
        SolMirOperationEqualityChild saved = p.operations.equality_children[0];
        p.operations.equality_children[0].variant_layout = 0;
        CHECK(!sol_mir_operations_validate(&p.operations, NULL));
        p.operations.equality_children[0] = saved;
    }
    pipeline_free(&p); free_text(&c);
}

static void test_source_search_work_is_not_an_arena_count(void) {
    static const char *sources[] = {
        "module first_variant\n"
        "enum Choice { first, second }\n"
        "function make() -> Choice { return Choice.first }\n",
        "module second_variant\n"
        "enum Choice { first, second }\n"
        "function make() -> Choice { return Choice.second }\n",
    };
    Compilation compilations[2]; Pipeline pipelines[2];
    bool built[2] = {false, false};
    for (size_t i = 0; i < 2; ++i) {
        bool compiled = compile_text(&compilations[i], sources[i]);
        CHECK(compiled); pipeline_init(&pipelines[i]);
        if (!compiled) continue;
        SolMirProgramRoot root = {callable(&compilations[i].ir, "make",
            SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
        built[i] = build_pipeline(&compilations[i].ir, &root, 1, NULL, 0,
            &pipelines[i], NULL);
        CHECK(built[i]);
    }
    if (built[0] && built[1]) {
#define SAME_ARENA(member, type, singular) \
        CHECK(pipelines[0].operations.singular##_count \
            == pipelines[1].operations.singular##_count);
        SOL_MIR_OPERATIONS_ARENAS(SAME_ARENA)
#undef SAME_ARENA
        CHECK(pipelines[0].operations.usage.build_work
            != pipelines[1].operations.usage.build_work);
    }
    for (size_t i = 0; i < 2; ++i) {
        pipeline_free(&pipelines[i]);
        free_text(&compilations[i]);
    }
}

static void test_handlers_and_unresolved_callable_rejection(void) {
    static const char handlers[] =
        "module handlers\n"
        "capability Source { function read(value: Int64) -> Int64 effects { service.read<Self> } }\n"
        "capability Provider { function read(value: Int64) -> Int64 effects { pure } }\n"
        "function root(source: capability Source, first: capability Provider, "
        "second: capability Provider) -> Int64 { return handle service.read<source> "
        "with first { handle service.read<source> with second { source.read(1) } } }\n";
    Compilation c; CHECK(compile_text(&c, handlers));
    SolIrCallableId imports[2] = {SOL_IR_NONE, SOL_IR_NONE};
    for (size_t i = 0; i < c.ir.callable_count; ++i) {
        if (c.ir.callables[i].kind != SOL_IR_CALLABLE_CAPABILITY
            || strcmp(c.ir.callables[i].name, "read") != 0) continue;
        imports[imports[0] == SOL_IR_NONE ? 0 : 1] = i;
    }
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, imports, 2, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built && p.operations.handler_count == 2);
    if (built) {
        size_t roots = 0, nested = 0;
        for (size_t i = 0; i < p.operations.handler_count; ++i) {
            roots += p.operations.handlers[i].frame_parent == SOL_MIR_OPERATION_NONE;
            nested += p.operations.handlers[i].frame_parent < p.operations.handler_count;
            CHECK(p.operations.handlers[i].root_match
                == SOL_MIR_OPERATION_ROOT_TOKEN_EQUAL);
        }
        CHECK(roots == 1 && nested == 1);
        reject_arena_header_mutations(&p.operations);
        SolMirOperationRootMatchRule saved = p.operations.handlers[0].root_match;
        p.operations.handlers[0].root_match = (SolMirOperationRootMatchRule)99;
        CHECK(!sol_mir_operations_validate(&p.operations, NULL));
        p.operations.handlers[0].root_match = saved;
        SolMirOperationsLimits exact = exact_limits(&p.operations);
        SolMirOperations limited; sol_mir_operations_init(&limited);
        SolMirOperationsBuildRequest request = {&p.layout, &exact};
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED);
        sol_mir_operations_free(&limited);
        --exact.max_handlers;
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED
            && limited.layout == NULL);
        exact = exact_limits(&p.operations);
        --exact.max_recipe_ids;
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED
            && limited.layout == NULL);
    }
    pipeline_free(&p); free_text(&c);

    static const char exact_callable[] =
        "module exact_callable\n"
        "capability Base { function choose(value: Int64) -> Bool effects { pure } }\n"
        "function callback(value: Int64) -> Bool effects { pure } { return true }\n"
        "function apply(callback: function(Int64) -> Bool effects { pure }) -> Bool "
        "effects { pure } { return true }\n"
        "function root(base: capability Base) -> Bool effects { pure } "
        "requires { { let exact = callback let bound = base.choose true } } "
        "{ return base.choose(1) }\n";
    CHECK(compile_text(&c, exact_callable));
    root = (SolMirProgramRoot){callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    pipeline_init(&p);
    SolIrCallableId choose = callable(&c.ir, "choose", SOL_IR_CALLABLE_CAPABILITY);
    built = build_pipeline(&c.ir, &root, 1, &choose, 1, &p, NULL);
    CHECK(built && p.operations.layout != NULL
        && p.representation.callable_producer_count == 2);
    if (built) {
        size_t exact = SOL_MIR_OPERATION_NONE, bound = SOL_MIR_OPERATION_NONE;
        for (size_t i = 0; i < p.operations.callable_count; ++i) {
            if (p.operations.callables[i].kind
                    == SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION) exact = i;
            else if (p.operations.callables[i].kind
                    == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION) bound = i;
        }
        CHECK(exact < p.operations.callable_count
            && p.operations.callables[exact].capture_kind
                == SOL_MIR_OPERATION_CAPTURE_NONE
            && p.operations.callables[exact].capture_access == SOL_MIR_OPERATION_NONE
            && p.operations.callables[exact].capture_recipe == SOL_MIR_RECIPE_NONE
            && p.operations.callables[exact].roots.count == 0);
        CHECK(bound < p.operations.callable_count
            && p.operations.callables[bound].capture_kind
                == SOL_MIR_OPERATION_CAPTURE_PLACE
            && p.operations.callables[bound].roots.count == 1);
        reject_arena_header_mutations(&p.operations);
        reject_inactive_predicate_fields(&p.operations, NULL);
        const SolMirOperationCallablePlan *producer = &p.operations.callables[0];
        SolMirMaterializedTargetKind saved = producer->target_kind;
        p.operations.callables[0].target_kind = (SolMirMaterializedTargetKind)99;
        CHECK(!sol_mir_operations_validate(&p.operations, NULL));
        p.operations.callables[0].target_kind = saved;
        SolMirOperationsLimits exact_limits_value = exact_limits(&p.operations);
        SolMirOperations limited; sol_mir_operations_init(&limited);
        SolMirOperationsBuildRequest limits_request
            = {&p.layout, &exact_limits_value};
        CHECK(sol_mir_operations_build(&limits_request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED);
        sol_mir_operations_free(&limited);
        --exact_limits_value.max_callables;
        CHECK(sol_mir_operations_build(&limits_request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED
            && limited.layout == NULL);
        exact_limits_value = exact_limits(&p.operations);
        exact_limits_value.max_roots = 0;
        CHECK(sol_mir_operations_build(&limits_request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_INVALID_ARGUMENT
            && limited.layout == NULL);
    }
    pipeline_free(&p); free_text(&c);

    static const char unresolved[] =
        "module unresolved\n"
        "capability Base { function choose(value: Int64) -> Bool effects { pure } }\n"
        "function preserve(value: capability Base) -> capability Base effects { pure } "
        "authority { result derives_from value } { return value }\n"
        "function apply(callback: function(Int64) -> Bool effects { pure }) -> Bool "
        "effects { pure } { return true }\n"
        "function root(base: capability Base) -> Bool effects { pure } "
        "requires { apply(preserve(base).choose) } { return true }\n";
    CHECK(compile_text(&c, unresolved));
    SolIrCallableId import = callable(&c.ir, "choose", SOL_IR_CALLABLE_CAPABILITY);
    root = (SolMirProgramRoot){callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    pipeline_init(&p);
    SolMirProgramBuildRequest a = {&c.ir, &root, 1, &import, 1, NULL};
    SolMirPlanBuildRequest b = {&p.program, NULL};
    SolMirMaterializeBuildRequest d = {&p.plan, NULL};
    SolMirRepresentationBuildRequest e = {&p.materialization, NULL};
    SolMirTargetDescriptor wasm = sol_mir_target_wasm32();
    SolMirLayoutBuildRequest f = {&p.representation, &wasm, NULL};
    bool layout = sol_mir_program_build(&a, &p.program, &p.diagnostics)
            == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && sol_mir_plan_build(&b, &p.plan, &p.diagnostics) == SOL_MIR_PLAN_BUILD_SUCCEEDED
        && sol_mir_materialize_build(&d, &p.materialization, &p.diagnostics)
            == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED
        && sol_mir_representation_build(&e, &p.representation, &p.diagnostics)
            == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED
        && sol_mir_layout_build(&f, &p.layout, &p.diagnostics)
            == SOL_MIR_LAYOUT_BUILD_SUCCEEDED;
    CHECK(layout);
    SolMirOperationsBuildRequest request = {&p.layout, NULL};
    CHECK(layout && sol_mir_operations_build(&request, &p.operations, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_UNSUPPORTED);
    CHECK(p.operations.layout == NULL);
    pipeline_free(&p); free_text(&c);
}

static void test_bodyless_import_contract(void) {
    static const char source[] =
        "module import_contract\n"
        "type Positive = refined Int64 where self > 0\n"
        "capability ContractHost { function echo(value: Int64) -> Int64 "
        "effects { pure } requires { Positive(value) == Positive(value) } "
        "ensures { result >= old(value) result >= old(value) } }\n"
        "function root(host: capability ContractHost) -> Int64 effects { pure } "
        "{ return host.echo(7) }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) {
        sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
        free_text(&c); return;
    }
    SolIrCallableId echo = callable(&c.ir, "echo", SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, &echo, 1, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built);
    if (built) {
        CHECK(p.plan.import_count == 1 && p.plan.imports[0].contexts.count == 5);
        CHECK(p.materialization.imports[0].contexts.count == 5
            && p.materialization.imports[0].overlays.count != 0);
        CHECK(p.operations.import_envelope_count == 1
            && p.operations.import_envelopes[0].requires.count == 1
            && p.operations.import_envelopes[0].ensures.count == 2
            && p.operations.import_envelopes[0].snapshots.count == 2
            && p.operations.predicate_body_count == 5);
        size_t import_refinements = 0;
        for (size_t context = 0; context < p.plan.context_count; ++context)
            import_refinements += p.plan.contexts[context].kind
                    == SOL_MIR_PLAN_CONTEXT_REFINEMENT
                && p.plan.contexts[context].target_kind
                    == SOL_MIR_PLAN_TARGET_IMPORT;
        CHECK(import_refinements == 2);
        CHECK(p.operations.import_snapshots[0].slot == 0
            && p.operations.import_snapshots[1].slot == 1);
        CHECK(sol_mir_operations_validate(&p.operations, NULL));
        reject_arena_header_mutations(&p.operations);
        reject_inactive_predicate_fields(&p.operations, NULL);
        SolMirOperationsLimits exact = exact_limits(&p.operations);
        SolMirOperations limited; sol_mir_operations_init(&limited);
        SolMirOperationsBuildRequest request = {&p.layout, &exact};
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED);
        sol_mir_operations_free(&limited);
        --exact.max_import_snapshots;
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED);
        exact = exact_limits(&p.operations);
        --exact.max_import_contract_references;
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED);
        SolMirImportSnapshotCapture saved = p.operations.import_snapshots[0];
        p.operations.import_snapshots[0].slot = 1;
        CHECK(!sol_mir_operations_validate(&p.operations, NULL));
        p.operations.import_snapshots[0] = saved;
        SolMirOperationProvenance provenance
            = p.operations.provenance[saved.provenance];
        p.operations.provenance[saved.provenance].source_snapshot
            = p.operations.provenance[
                p.operations.import_snapshots[1].provenance].source_snapshot;
        CHECK(!sol_mir_operations_validate(&p.operations, NULL));
        p.operations.provenance[saved.provenance] = provenance;
    }
    pipeline_free(&p); free_text(&c);
}

static void test_p2_6b2_rich_predicates(void) {
    PredicateCensus census = {0};
    static const char *sources[] = {
        "module short_circuit\nfunction root(value: Int64) -> Bool effects { pure } "
            "requires { false && (1 / 0 > value) } { return true }\n",
        "module short_circuit_or\nfunction root(value: Int64) -> Bool effects { pure } "
            "requires { true || (1 / 0 > value) } { return true }\n",
        "module projected\nrecord Box { value: Int64 }\n"
            "function root(box: Box) -> Bool effects { pure } "
            "requires { box.value > 0 } { return true }\n",
        "module snapshot\nfunction root(value: Int64) -> Int64 effects { pure } "
            "ensures { result >= old(value) } { return 0 }\n",
        "module direct_call\nfunction helper(value: Int64) -> Bool effects { pure } "
            "{ return true }\nfunction root(value: Int64) -> Bool effects { pure } "
            "requires { helper(value) } { return true }\n",
        "module function_value\nfunction helper(value: Int64) -> Bool effects { pure } "
            "{ return true }\nfunction root() -> Bool effects { pure } "
            "requires { { let callback = helper callback(1) } } { return true }\n",
        "module aggregate\nrecord Box { value: Int64 }\n"
            "function root() -> Bool effects { pure } "
            "requires { Box { value = 1 } == Box { value = 1 } } { return true }\n",
        "module tuple_aggregate\nfunction root() -> Bool effects { pure } "
            "requires { (1, true) == (1, true) } { return true }\n",
        "module distinct_wrapper\ntype Meter = distinct Int64\n"
            "function root() -> Bool effects { pure } "
            "requires { Meter(1) == Meter(1) } { return true }\n",
        "module conditional\nfunction root() -> Bool effects { pure } "
            "requires { if true { true } else { false } } { return true }\n",
        "module matching\nfunction root(value: Bool) -> Bool effects { pure } "
            "requires { match value { true => true false => false } } { return true }\n",
        "module guarded_match\nenum Choice { yes(value: Int64), no }\n"
            "function root(value: Int64) -> Bool effects { pure } requires { "
            "match Choice.yes(value) { yes(item) if item > 0 => true _ => false } "
            "} { return true }\n",
        "module local_block\nfunction root() -> Bool effects { pure } "
            "requires { { let value = true value } } { return true }\n",
        "module nested_refined\ntype Positive = refined Int64 where self > 0\n"
            "function root(value: Int64) -> Bool effects { pure } "
            "requires { Positive(value) == Positive(value) } { return true }\n",
        "module generic_refined\ntype Identity<T> = refined T where self == self\n"
            "function root(value: Int64) -> Bool effects { pure } "
            "requires { Identity<Int64>(value) == Identity<Int64>(value) } "
            "{ return true }\n",
        "module cyclic_refined\n"
            "type First<T> = refined T where Second<T>(self) == Second<T>(self)\n"
            "type Second<T> = refined T where First<T>(self) == First<T>(self)\n"
            "function root(value: Int64) -> Bool effects { pure } "
            "requires { First<Int64>(value) == First<Int64>(value) } "
            "{ return true }\n",
        "module canonical_payload\n"
            "record Box { value: Int64 }\n"
            "enum Choice { yes(value: Int64), no }\n"
            "function helper(value: Int64) -> Bool effects { pure } "
            "{ return true }\n"
            "function root(box: Box) -> Bool effects { pure } requires { { "
            "let callback = helper callback(box.value) } match "
            "Choice.yes(box.value) { yes(item) => item > 0 _ => false } "
            "} { return true }\n",
        "module scalar_variants\n"
            "function root(value: Int64) -> Bool effects { pure } requires { { "
            "let unit = () let text = \"x\" !false && text == \"x\" && "
            "unit == () && -value <= 0 } } { return true }\n",
        "module option_result_construction\n"
            "function root(option: Option<Int64>, value: Result<Int64, Text>) "
            "-> Bool effects { pure } requires { option == some(1) && "
            "value == ok(1) } { return true }\n",
        "module invocation_variants\n"
            "capability Guard { function check(value: Int64) -> Bool "
            "effects { pure } }\n"
            "function root(guard: capability Guard) -> Bool effects { pure } "
            "requires { { let bound = guard.check bound(1) } } "
            "{ return guard.check(1) }\n",
        "module method_invocation\n"
            "trait Positive { function positive(self: Self) -> Bool "
            "effects { pure } }\n"
            "implementation Positive for Int64 { function positive(self: Self) "
            "-> Bool effects { pure } { return self > 0 } }\n"
            "function root(value: Int64) -> Bool effects { pure } "
            "requires { value.positive() } { return true }\n",
    };
    static const char *import_names[sizeof(sources) / sizeof(*sources)] = {
        [19] = "check",
    };
    for (size_t i = 0; i < sizeof(sources) / sizeof(*sources); ++i) {
        Compilation c; bool compiled = compile_text(&c, sources[i]); CHECK(compiled);
        if (!compiled) {
            fprintf(stderr, "rich predicate fixture %zu did not compile\n", i);
            sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
            free_text(&c); continue;
        }
        SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
        Pipeline p; pipeline_init(&p);
        SolIrCallableId import = import_names[i] == NULL ? SOL_IR_NONE
            : callable(&c.ir, import_names[i], SOL_IR_CALLABLE_CAPABILITY);
        bool built = build_pipeline(&c.ir, &root, 1,
            import_names[i] == NULL ? NULL : &import,
            import_names[i] == NULL ? 0 : 1, &p, NULL);
        if (!built) {
            fprintf(stderr, "rich predicate fixture %zu failed\n", i);
            sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
        }
        CHECK(built && p.operations.layout != NULL);
        if (built) {
            size_t rendered_length = 0;
            char *rendered = render(&p.operations, &rendered_length);
            CHECK(rendered != NULL && rendered_length != 0);
            free(rendered);
            if (p.operations.predicate_edge_count != 0) {
                size_t saved = p.operations.predicate_edges[0].target;
                p.operations.predicate_edges[0].target
                    = p.operations.predicate_edges[0].source;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                p.operations.predicate_edges[0].target = saved;
                SolMirPlanSlice arguments
                    = p.operations.predicate_edges[0].arguments;
                p.operations.predicate_edges[0].arguments.offset = SIZE_MAX;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                p.operations.predicate_edges[0].arguments = arguments;
            }
            if (p.operations.predicate_instruction_count != 0) {
                SolMirRecipeId saved
                    = p.operations.predicate_instructions[0].recipe;
                p.operations.predicate_instructions[0].recipe
                    = p.representation.recipe_count;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                p.operations.predicate_instructions[0].recipe = saved;
                SolMirPlanSlice path
                    = p.operations.predicate_instructions[0].path;
                p.operations.predicate_instructions[0].path.offset = SIZE_MAX;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                p.operations.predicate_instructions[0].path = path;
            }
            if (p.operations.predicate_value_count != 0) {
                size_t saved = p.operations.predicate_values[0].block;
                p.operations.predicate_values[0].block
                    = p.operations.predicate_block_count;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                p.operations.predicate_values[0].block = saved;
            }
            size_t branch = 0;
            while (branch < p.operations.predicate_block_count
                && p.operations.predicate_blocks[branch].terminator.kind
                    != SOL_MIR_PREDICATE_TERM_BRANCH) ++branch;
            if (branch < p.operations.predicate_block_count) {
                size_t saved
                    = p.operations.predicate_blocks[branch].terminator.condition;
                p.operations.predicate_blocks[branch].terminator.condition
                    = p.operations.predicate_value_count;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                p.operations.predicate_blocks[branch].terminator.condition = saved;
            }
            size_t semantic = 0;
            while (semantic < p.operations.predicate_instruction_count
                && p.operations.predicate_instructions[semantic].kind
                    != SOL_MIR_PREDICATE_INST_BOOL
                && p.operations.predicate_instructions[semantic].kind
                    != SOL_MIR_PREDICATE_INST_I64) ++semantic;
            if (semantic < p.operations.predicate_instruction_count) {
                SolMirPredicateInstruction *instruction
                    = &p.operations.predicate_instructions[semantic];
                if (instruction->kind == SOL_MIR_PREDICATE_INST_BOOL) {
                    instruction->boolean = !instruction->boolean;
                    CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                    instruction->boolean = !instruction->boolean;
                } else {
                    int64_t saved = instruction->integer;
                    instruction->integer ^= 1;
                    CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                    instruction->integer = saved;
                }
            }
            if (p.operations.predicate_instruction_count != 0) {
                SolMirPredicateInstruction *instruction
                    = &p.operations.predicate_instructions[0];
                size_t saved = instruction->bytes.offset;
                instruction->bytes.offset = 1;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                instruction->bytes.offset = saved;
            }
            size_t constant = 0;
            while (constant < p.operations.predicate_instruction_count
                && p.operations.predicate_instructions[constant].kind
                    != SOL_MIR_PREDICATE_INST_BOOL) ++constant;
            if (constant < p.operations.predicate_instruction_count) {
                SolMirPredicateValueId saved
                    = p.operations.predicate_instructions[constant].left;
                p.operations.predicate_instructions[constant].left = 0;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                p.operations.predicate_instructions[constant].left = saved;
            }
            if (p.operations.predicate_body_count != 0) {
                SolMirPlanSlice saved = p.operations.predicate_bodies[0].blocks;
                p.operations.predicate_bodies[0].blocks.offset = SIZE_MAX;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                p.operations.predicate_bodies[0].blocks = saved;
            }
            if (p.operations.predicate_block_count != 0) {
                SolMirPlanSlice saved
                    = p.operations.predicate_blocks[0].instructions;
                p.operations.predicate_blocks[0].instructions.offset = SIZE_MAX;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                p.operations.predicate_blocks[0].instructions = saved;
            }
            if (p.operations.predicate_pattern_node_count != 0) {
                SolMirPredicatePatternNode *node
                    = &p.operations.predicate_pattern_nodes[0];
                SolMirPlanSlice saved = node->path;
                node->path = (SolMirPlanSlice){SIZE_MAX, 1};
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                node->path = saved;
                SolMirOperationPatternKind kind = node->kind;
                node->kind = (SolMirOperationPatternKind)99;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                node->kind = kind;
                SolMirRecipeId recipe = node->recipe;
                node->recipe = p.representation.recipe_count;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                node->recipe = recipe;
            }
            if (p.operations.predicate_operand_count != 0) {
                SolMirPredicateOperand *operand = &p.operations.predicate_operands[0];
                SolMirPredicateOperand saved = *operand;
                operand->value = p.operations.predicate_value_count;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                *operand = saved;
                operand->access = SOL_ACCESS_EXCLUSIVE;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                *operand = saved;
                operand->field_layout = p.representation.field_count;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                *operand = saved;
                operand->formal_ordinal ^= 1u;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                *operand = saved;
            }
            if (p.operations.predicate_edge_value_count != 0) {
                SolMirPredicateValueId saved = p.operations.predicate_edge_values[0];
                p.operations.predicate_edge_values[0]
                    = p.operations.predicate_value_count;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                p.operations.predicate_edge_values[0] = saved;
            }
            if (p.operations.predicate_path_step_count != 0) {
                SolMirPredicatePathStep *step = &p.operations.predicate_path_steps[0];
                SolMirPredicatePathStep saved = *step;
                step->base_recipe = p.representation.recipe_count;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                *step = saved;
                step->field_layout = p.representation.field_count;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                *step = saved;
                step->result_recipe = p.representation.recipe_count;
                CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                *step = saved;
            }
            for (size_t block = 0; block < p.operations.predicate_block_count;
                ++block) {
                SolMirPredicateTerminator *term
                    = &p.operations.predicate_blocks[block].terminator;
                SolMirPredicateTerminator saved = *term;
                if (term->kind == SOL_MIR_PREDICATE_TERM_INVOKE) {
                    term->binding = p.materialization.binding_count;
                    CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                    *term = saved;
                    term->arguments.offset = SIZE_MAX;
                    CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                    *term = saved;
                } else if (term->kind == SOL_MIR_PREDICATE_TERM_PROPAGATE) {
                    term->success_variant_layout = p.representation.variant_count;
                    CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                    *term = saved;
                } else if (term->kind == SOL_MIR_PREDICATE_TERM_CHECK_REFINED) {
                    term->nested_body = p.operations.predicate_body_count;
                    CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                    *term = saved;
                } else if (term->kind == SOL_MIR_PREDICATE_TERM_RETURN) {
                    term->condition = 0;
                    CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                    *term = saved;
                    term->kind = SOL_MIR_PREDICATE_TERM_INVOKE;
                    CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                    *term = saved;
                    term->kind = SOL_MIR_PREDICATE_TERM_PROPAGATE;
                    CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                    *term = saved;
                    term->kind = SOL_MIR_PREDICATE_TERM_CHECK_REFINED;
                    CHECK(!sol_mir_operations_validate(&p.operations, NULL));
                    *term = saved;
                }
            }
            CHECK(sol_mir_operations_validate(&p.operations, NULL));
            reject_arena_header_mutations(&p.operations);
            reject_inactive_predicate_fields(&p.operations, &census);
            if (i == 18)
                reject_forged_predicate_propagation(&p.operations);
            if (i == 16) {
                size_t constructs = 0, functions = 0;
                for (size_t instruction = 0;
                    instruction < p.operations.predicate_instruction_count;
                    ++instruction) {
                    constructs += p.operations.predicate_instructions[instruction].kind
                        == SOL_MIR_PREDICATE_INST_CONSTRUCT;
                    functions += p.operations.predicate_instructions[instruction].kind
                        == SOL_MIR_PREDICATE_INST_FUNCTION;
                }
                CHECK(constructs != 0 && functions != 0
                    && p.operations.predicate_path_step_count != 0
                    && p.operations.predicate_pattern_node_count != 0);
            }
            SolMirOperationsLimits exact = exact_limits(&p.operations);
            SolMirOperations limited; sol_mir_operations_init(&limited);
            SolMirOperationsBuildRequest request = {&p.layout, &exact};
            CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics)
                == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED);
            sol_mir_operations_free(&limited);
#define LESS_RICH(member, usage_member) do { \
    if (p.operations.usage.usage_member != 0) { \
        SolMirOperationsLimits less = exact; --less.member; \
        request.limits = &less; \
        CHECK(sol_mir_operations_build(&request, &limited, &p.diagnostics) \
            == (exact.member == 1 \
                ? SOL_MIR_OPERATIONS_BUILD_INVALID_ARGUMENT \
                : SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED)); \
        CHECK(limited.layout == NULL); \
    } \
} while (0)
            LESS_RICH(max_predicate_bodies, predicate_bodies);
            LESS_RICH(max_predicate_blocks, predicate_blocks);
            LESS_RICH(max_predicate_inputs, predicate_inputs);
            LESS_RICH(max_predicate_values, predicate_values);
            LESS_RICH(max_predicate_instructions, predicate_instructions);
            LESS_RICH(max_predicate_edges, predicate_edges);
            LESS_RICH(max_predicate_edge_values, predicate_edge_values);
            LESS_RICH(max_predicate_operands, predicate_operands);
            LESS_RICH(max_predicate_path_steps, predicate_path_steps);
            LESS_RICH(max_predicate_pattern_nodes, predicate_pattern_nodes);
#undef LESS_RICH
            request.limits = &exact;

            size_t refinement_context = 0;
            while (refinement_context < p.plan.context_count
                && p.plan.contexts[refinement_context].kind
                    != SOL_MIR_PLAN_CONTEXT_REFINEMENT) ++refinement_context;
            if (refinement_context < p.plan.context_count) {
                SolMirPlanTypeId saved
                    = p.plan.contexts[refinement_context].refinement_type;
                CHECK(saved < p.plan.type_count);
                p.plan.contexts[refinement_context].refinement_type
                    = SOL_MIR_PLAN_NONE;
                CHECK(!sol_mir_plan_validate(&p.plan, NULL));
                p.plan.contexts[refinement_context].refinement_type = saved;
            }
        }
        pipeline_free(&p); free_text(&c);
    }
    check_predicate_census(&census);
}

static void test_exact_nested_refinement_context(void) {
    static const char source[] =
        "module exact_nested_refinement\n"
        "type Inner<T> = refined T where self == self\n"
        "type Outer<T> = refined T where Inner<T>(self) == Inner<T>(self)\n"
        "function root(number: Int64, flag: Bool) -> Bool effects { pure } "
        "requires { Outer<Int64>(number) == Outer<Int64>(number) && "
        "Outer<Bool>(flag) == Outer<Bool>(flag) } { return true }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolIrDefinitionId inner = SOL_IR_NONE;
    for (size_t i = 0; i < c.ir.definition_count; ++i)
        if (strcmp(c.ir.definitions[i].name, "Inner") == 0) inner = i;
    CHECK(inner != SOL_IR_NONE);
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = inner != SOL_IR_NONE
        && build_pipeline(&c.ir, &root, 1, NULL, 0, &p, NULL);
    if (!built) sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(built);
    if (built) {
        size_t first = SOL_MIR_OPERATION_NONE, second = SOL_MIR_OPERATION_NONE;
        for (size_t i = 0; i < p.materialization.context_count; ++i) {
            const SolMirPlanContext *left = &p.materialization.contexts[i];
            if (left->kind != SOL_MIR_PLAN_CONTEXT_REFINEMENT
                || left->definition != inner) continue;
            for (size_t j = i + 1; j < p.materialization.context_count; ++j) {
                const SolMirPlanContext *right = &p.materialization.contexts[j];
                if (right->kind == SOL_MIR_PLAN_CONTEXT_REFINEMENT
                    && right->definition == inner
                    && right->source.expression == left->source.expression
                    && right->refinement_type != left->refinement_type) {
                    first = i; second = j; break;
                }
            }
            if (first != SOL_MIR_OPERATION_NONE) break;
        }
        CHECK(first != SOL_MIR_OPERATION_NONE && second != SOL_MIR_OPERATION_NONE);
        if (first != SOL_MIR_OPERATION_NONE && second != SOL_MIR_OPERATION_NONE) {
            SolMirRecipeId recipes[2] = {
                p.materialization.contexts[first].refinement_type,
                p.materialization.contexts[second].refinement_type};
            CHECK(recipes[0] < p.representation.recipe_count
                && recipes[1] < p.representation.recipe_count
                && p.representation.recipes[recipes[0]].kind
                    == SOL_MIR_RECIPE_REFINED
                && p.representation.recipes[recipes[1]].kind
                    == SOL_MIR_RECIPE_REFINED
                && p.representation.recipes[recipes[0]].backing
                    != p.representation.recipes[recipes[1]].backing);
            size_t references[2] = {0, 0};
            for (size_t block = 0; block < p.operations.predicate_block_count;
                ++block) {
                const SolMirPredicateTerminator *term
                    = &p.operations.predicate_blocks[block].terminator;
                if (term->kind != SOL_MIR_PREDICATE_TERM_CHECK_REFINED
                    || term->nested_body >= p.operations.predicate_body_count)
                    continue;
                size_t context
                    = p.operations.predicate_bodies[term->nested_body].context;
                for (size_t q = 0; q < 2; ++q)
                    if (context == (q == 0 ? first : second)) {
                        ++references[q];
                        CHECK(term->result_recipe == recipes[q]);
                    }
            }
            CHECK(references[0] != 0 && references[1] != 0);
        }
        CHECK(sol_mir_operations_validate(&p.operations, NULL));
    }
    pipeline_free(&p); free_text(&c);
}

static void test_predicate_literal_authentication_and_rendering(void) {
    static const char *sources[] = {
        "module text_a\nfunction root() -> Bool effects { pure } "
            "requires { \"a\" == \"a\" } { return true }\n",
        "module text_b\nfunction root() -> Bool effects { pure } "
            "requires { \"b\" == \"b\" } { return true }\n",
    };
    Compilation c[2]; Pipeline p[2]; char *text[2] = {NULL, NULL};
    size_t length[2] = {0, 0};
    for (size_t i = 0; i < 2; ++i) {
        bool compiled = compile_text(&c[i], sources[i]); CHECK(compiled);
        pipeline_init(&p[i]);
        if (!compiled) continue;
        SolMirProgramRoot root = {callable(&c[i].ir, "root",
            SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
        bool built = build_pipeline(&c[i].ir, &root, 1, NULL, 0, &p[i], NULL);
        CHECK(built && p[i].operations.literal_byte_count == 2);
        if (built) text[i] = render(&p[i].operations, &length[i]);
    }
    CHECK(text[0] != NULL && text[1] != NULL
        && (length[0] != length[1] || memcmp(text[0], text[1], length[0]) != 0)
        && strstr(text[0], "predicate_literal_bytes=6161") != NULL);
    if (p[0].operations.literal_byte_count != 0) {
        reject_arena_header_mutations(&p[0].operations);
        reject_inactive_predicate_fields(&p[0].operations, NULL);
        char saved = p[0].operations.literal_bytes[0];
        p[0].operations.literal_bytes[0] = 'z';
        CHECK(!sol_mir_operations_validate(&p[0].operations, NULL));
        p[0].operations.literal_bytes[0] = saved;
        SolMirOperationsLimits exact = exact_limits(&p[0].operations);
        SolMirOperations limited; sol_mir_operations_init(&limited);
        SolMirOperationsBuildRequest request = {&p[0].layout, &exact};
        --exact.max_literal_bytes;
        CHECK(sol_mir_operations_build(&request, &limited, &p[0].diagnostics)
            == SOL_MIR_OPERATIONS_BUILD_RESOURCE_EXHAUSTED);
    }
    for (size_t i = 0; i < 2; ++i) {
        free(text[i]); pipeline_free(&p[i]); free_text(&c[i]);
    }
}

static void test_import_contract_helper_is_retained_and_lowered(void) {
    static const char source[] =
        "module import_helper\n"
        "function positive(value: Int64) -> Bool effects { pure } "
        "{ return value > 0 }\n"
        "capability ContractHost { function echo(value: Int64) -> Int64 "
        "effects { pure } requires { positive(value) } }\n"
        "function root(host: capability ContractHost) -> Int64 effects { pure } "
        "{ return host.echo(7) }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolIrCallableId echo = callable(&c.ir, "echo", SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    SolMirProgramBuildRequest a = {&c.ir, &root, 1, &echo, 1, NULL};
    SolMirPlanBuildRequest b = {&p.program, NULL};
    SolMirMaterializeBuildRequest d = {&p.plan, NULL};
    CHECK(sol_mir_program_build(&a, &p.program, &p.diagnostics)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(sol_mir_plan_build(&b, &p.plan, &p.diagnostics)
        == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    SolMirMaterializeBuildOutcome materialized
        = sol_mir_materialize_build(&d, &p.materialization, &p.diagnostics);
    if (materialized != SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED)
        sol_diagnostics_render_human(stderr, &c.source, &p.diagnostics);
    CHECK(materialized == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED);
    CHECK(sol_mir_plan_validate(&p.plan, NULL)
        && sol_mir_materialization_validate(&p.materialization, NULL));
    size_t import_owned = 0;
    for (size_t i = 0; i < p.materialization.binding_count; ++i) {
        const SolMirMaterializedBinding *binding = &p.materialization.bindings[i];
        if (binding->owner_kind != SOL_MIR_PLAN_DEMAND_OWNER_IMPORT) continue;
        ++import_owned;
        CHECK(binding->parent == SOL_MIR_PLAN_NONE
            && binding->parent_import < p.materialization.import_count
            && p.materialization.semantic_sites[binding->site].block
                == SOL_MIR_MATERIALIZED_NONE);
    }
    CHECK(import_owned == 1);
    SolMirRepresentationBuildRequest e = {&p.materialization, NULL};
    SolMirTargetDescriptor wasm = sol_mir_target_wasm32();
    SolMirLayoutBuildRequest f = {&p.representation, &wasm, NULL};
    CHECK(sol_mir_representation_build(&e, &p.representation, &p.diagnostics)
        == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED);
    CHECK(sol_mir_layout_build(&f, &p.layout, &p.diagnostics)
        == SOL_MIR_LAYOUT_BUILD_SUCCEEDED);
    SolMirOperationsBuildRequest g = {&p.layout, NULL};
    CHECK(sol_mir_operations_build(&g, &p.operations, &p.diagnostics)
        == SOL_MIR_OPERATIONS_BUILD_SUCCEEDED
        && p.operations.layout == &p.layout
        && p.operations.import_envelope_count == 1
        && p.operations.predicate_body_count != 0
        && sol_mir_operations_validate(&p.operations, NULL));
    size_t owned_demand = SOL_MIR_OPERATION_NONE;
    for (size_t i = 0; i < p.plan.demand_count; ++i)
        if (p.plan.demands[i].owner_kind == SOL_MIR_PLAN_DEMAND_OWNER_IMPORT)
            owned_demand = i;
    CHECK(owned_demand < p.plan.demand_count);
    if (owned_demand < p.plan.demand_count) {
        SolMirPlanDemand saved = p.plan.demands[owned_demand];
        p.plan.demands[owned_demand].owner_kind
            = SOL_MIR_PLAN_DEMAND_OWNER_INSTANCE;
        CHECK(!sol_mir_plan_validate(&p.plan, NULL));
        p.plan.demands[owned_demand] = saved;
        size_t material_binding = 0;
        while (material_binding < p.materialization.binding_count
            && p.materialization.bindings[material_binding].source_demand
                != owned_demand) ++material_binding;
        CHECK(material_binding < p.materialization.binding_count);
        SolMirMaterializedBinding binding
            = p.materialization.bindings[material_binding];
        p.materialization.bindings[material_binding].owner_kind
            = SOL_MIR_PLAN_DEMAND_OWNER_ROOT;
        CHECK(!sol_mir_materialization_validate(&p.materialization, NULL));
        p.materialization.bindings[material_binding] = binding;
        p.plan.demands[owned_demand].parent_import = SOL_MIR_PLAN_NONE;
        CHECK(!sol_mir_plan_validate(&p.plan, NULL));
        p.plan.demands[owned_demand] = saved;
    }
    pipeline_free(&p); free_text(&c);
}

static void test_multiple_import_context_canonicalization(void) {
    static const char source[] =
        "module import_order\n"
        "capability Alpha { function first(value: Int64) -> Int64 effects { pure } "
            "requires { value > 0 } }\n"
        "capability Zeta { function second(value: Int64) -> Int64 effects { pure } "
            "requires { value >= 0 } }\n"
        "function root(alpha: capability Alpha, zeta: capability Zeta) -> Int64 "
            "effects { pure } { return zeta.second(2) + alpha.first(1) }\n";
    Compilation c; bool compiled = compile_text(&c, source); CHECK(compiled);
    if (!compiled) { free_text(&c); return; }
    SolIrCallableId first = callable(&c.ir, "first", SOL_IR_CALLABLE_CAPABILITY);
    SolIrCallableId second = callable(&c.ir, "second", SOL_IR_CALLABLE_CAPABILITY);
    SolIrCallableId approved[2] = {second, first};
    SolMirProgramRoot root = {callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    Pipeline p; pipeline_init(&p);
    bool built = build_pipeline(&c.ir, &root, 1, approved, 2, &p, NULL);
    CHECK(built && p.plan.import_count == 2);
    if (built) {
        CHECK(p.plan.imports[0].callable < p.plan.imports[1].callable
            && p.plan.imports[0].contexts.count == 1
            && p.plan.imports[1].contexts.count == 1
            && p.plan.imports[0].contexts.offset
                + p.plan.imports[0].contexts.count
                == p.plan.imports[1].contexts.offset
            && p.plan.imports[0].typed_uses.offset
                + p.plan.imports[0].typed_uses.count
                == p.plan.imports[1].typed_uses.offset);
        SolMirPlanSlice uses = p.plan.imports[0].typed_uses;
        --p.plan.imports[0].typed_uses.count;
        CHECK(!sol_mir_plan_validate(&p.plan, NULL));
        p.plan.imports[0].typed_uses = uses;
        SolMirPlanContext context
            = p.plan.contexts[p.plan.imports[0].contexts.offset];
        p.plan.contexts[p.plan.imports[0].contexts.offset].import = 1;
        CHECK(!sol_mir_plan_validate(&p.plan, NULL));
        p.plan.contexts[p.plan.imports[0].contexts.offset] = context;
        p.plan.contexts[p.plan.imports[0].contexts.offset].target_kind
            = SOL_MIR_PLAN_TARGET_INSTANCE;
        CHECK(!sol_mir_plan_validate(&p.plan, NULL));
        p.plan.contexts[p.plan.imports[0].contexts.offset] = context;
        SolMirPlanTypedUse use
            = p.plan.typed_uses[p.plan.imports[0].typed_uses.offset];
        p.plan.typed_uses[p.plan.imports[0].typed_uses.offset].context
            = p.plan.imports[1].contexts.offset;
        CHECK(!sol_mir_plan_validate(&p.plan, NULL));
        p.plan.typed_uses[p.plan.imports[0].typed_uses.offset] = use;
        SolMirMaterializedTypeOverlay overlay
            = p.materialization.overlays[p.materialization.imports[0].overlays.offset];
        p.materialization.overlays[p.materialization.imports[0].overlays.offset].context
            = p.materialization.imports[1].contexts.offset;
        CHECK(!sol_mir_materialization_validate(&p.materialization, NULL));
        p.materialization.overlays[p.materialization.imports[0].overlays.offset]
            = overlay;
        p.materialization.overlays[p.materialization.imports[0].overlays.offset].type
            = (overlay.type + 1) % p.materialization.type_count;
        CHECK(!sol_mir_materialization_validate(&p.materialization, NULL));
        p.materialization.overlays[p.materialization.imports[0].overlays.offset]
            = overlay;
        SolMirPlanSlice contexts = p.materialization.imports[0].contexts;
        p.materialization.imports[0].contexts = p.materialization.imports[1].contexts;
        CHECK(!sol_mir_materialization_validate(&p.materialization, NULL));
        p.materialization.imports[0].contexts = contexts;
        size_t context_id = p.materialization.imports[0].contexts.offset;
        SolMirPlanContext material_context = p.materialization.contexts[context_id];
        p.materialization.contexts[context_id].import = 1;
        CHECK(!sol_mir_materialization_validate(&p.materialization, NULL));
        p.materialization.contexts[context_id] = material_context;
    }
    pipeline_free(&p); free_text(&c);
}

int main(void) {
    SolMirOperations empty;
    memset(&empty, 0xa5, sizeof(empty)); sol_mir_operations_init(&empty);
    CHECK(empty.layout == NULL && !sol_mir_operations_validate(&empty, NULL));
    SolMirOperationsLimits defaults = sol_mir_operations_default_limits();
    CHECK(defaults.max_access_plans != 0 && defaults.max_validation_work != 0);
    sol_mir_operations_free(&empty);
    test_e6_operations();
    test_cross_recipe_result_propagation();
    test_capability_constructor_plan();
    test_recursive_equality_graph();
    test_source_search_work_is_not_an_arena_count();
    test_handlers_and_unresolved_callable_rejection();
    test_bodyless_import_contract();
    test_import_contract_helper_is_retained_and_lowered();
    test_multiple_import_context_canonicalization();
    test_predicate_body_count_wrap_rejection();
    test_contract_propagation_rejection();
    test_dynamic_predicate_callback_rejection();
    test_match_binding_dfs_authentication();
    test_p2_6b2_rich_predicates();
    test_exact_nested_refinement_context();
    test_predicate_literal_authentication_and_rendering();
    if (failures != 0) {
        fprintf(stderr, "%d MIR operations test(s) failed\n", failures); return 1;
    }
    printf("MIR operations tests passed\n"); return 0;
}
