#include "sol/mir_representation.h"

#include "sol/effects.h"
#include "sol/lexer.h"
#include "sol/ownership.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, \
            #condition); \
        ++failures; \
    } \
} while (0)

typedef struct {
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree syntax;
    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    SolContractTable contracts;
    SolIr ir;
} Compilation;

static bool compile_text(Compilation *c, const char *text) {
    memset(c, 0, sizeof(*c));
    sol_tokens_init(&c->tokens); sol_diagnostics_init(&c->diagnostics);
    sol_syntax_tree_init(&c->syntax); sol_hir_module_init(&c->hir);
    sol_type_table_init(&c->types); sol_effect_table_init(&c->effects);
    sol_contract_table_init(&c->contracts); sol_ir_init(&c->ir);
    return sol_source_from_text(&c->source, "representation.sol", text)
        && sol_lex(&c->source, &c->tokens, &c->diagnostics)
        && sol_parse(&c->source, &c->tokens, &c->syntax, &c->diagnostics)
        && sol_hir_lower(&c->source, &c->syntax, &c->hir, &c->diagnostics)
        && sol_type_check(&c->source, &c->syntax, &c->hir, &c->types,
            &c->diagnostics)
        && sol_effect_check(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->diagnostics)
        && sol_contract_lower(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->contracts, &c->diagnostics)
        && sol_ir_lower(&c->source, &c->syntax, &c->hir, &c->types,
            &c->effects, &c->contracts, &c->ir, &c->diagnostics);
}

static void free_compilation(Compilation *c) {
    sol_ir_free(&c->ir); sol_contract_table_free(&c->contracts);
    sol_effect_table_free(&c->effects); sol_type_table_free(&c->types);
    sol_hir_module_free(&c->hir); sol_syntax_tree_free(&c->syntax);
    sol_tokens_free(&c->tokens); sol_source_free(&c->source);
    sol_diagnostics_free(&c->diagnostics);
}

static SolIrCallableId callable(const SolIr *ir, const char *name,
    SolIrCallableKind kind) {
    for (size_t i = 0; i < ir->callable_count; ++i)
        if (ir->callables[i].kind == kind
            && strcmp(ir->callables[i].name, name) == 0) return i;
    return SOL_IR_NONE;
}

static bool build_materialization(const SolIr *ir, SolIrCallableId root,
    const SolIrCallableId *imports, size_t import_count, SolMirProgram *program,
    SolMirPlan *plan, SolMirMaterialization *materialization,
    SolDiagnostics *diagnostics) {
    SolMirProgramRoot program_root = {root, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgramBuildRequest program_request
        = {ir, &program_root, 1, imports, import_count, NULL};
    SolMirPlanBuildRequest plan_request = {program, NULL};
    SolMirMaterializeBuildRequest materialize_request = {plan, NULL};
    return sol_mir_program_build(&program_request, program, diagnostics)
            == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && sol_mir_plan_build(&plan_request, plan, diagnostics)
            == SOL_MIR_PLAN_BUILD_SUCCEEDED
        && sol_mir_materialize_build(&materialize_request, materialization,
            diagnostics) == SOL_MIR_MATERIALIZE_BUILD_SUCCEEDED;
}

static char *render(const SolMirRepresentation *representation, size_t *length) {
    FILE *stream = tmpfile();
    if (stream == NULL || !sol_mir_representation_render(stream, representation)
        || fflush(stream) != 0 || fseek(stream, 0, SEEK_END) != 0) {
        if (stream != NULL) fclose(stream);
        return NULL;
    }
    long end = ftell(stream);
    if (end < 0 || fseek(stream, 0, SEEK_SET) != 0) { fclose(stream); return NULL; }
    char *text = malloc((size_t)end + 1);
    if (text == NULL) { fclose(stream); return NULL; }
    size_t got = fread(text, 1, (size_t)end, stream); fclose(stream);
    if (got != (size_t)end) { free(text); return NULL; }
    text[got] = '\0'; *length = got; return text;
}

static const SolMirRecipe *named_recipe(const SolMirRepresentation *r,
    const char *name) {
    const SolIr *ir = r->materialization->plan->program->ir;
    for (size_t i = 0; i < r->recipe_count; ++i) {
        if (r->recipes[i].concrete_definition != SOL_IR_NONE
            && strcmp(ir->definitions[r->recipes[i].concrete_definition].name,
                name) == 0) return &r->recipes[i];
    }
    return NULL;
}

static void test_complete_graph(void) {
    static const char source[] =
        "module representation\n"
        "capability Base { function choose(value: Int64) -> Bool effects { pure } }\n"
        "capability Derived derives_from source: capability Base { "
        "function choose(value: Int64) -> Bool effects { pure } { return true } }\n"
        "capability Token {}\n"
        "record Empty {}\n"
        "record Ordered { zeta: Int64, alpha: Bool, text: Text, unit: (), empty: Empty, "
        "tuple: (Int64, Bool), option: Option<Int64>, "
        "outcome: Result<Int64, Bool>, wrapped: Distinct, checked: Refined }\n"
        "record Node { next: Option<Node> }\n"
        "record Loop { next: Loop }\n"
        "enum Void {}\n"
        "enum Choice { zeta(value: Int64), alpha, middle(flag: Bool) }\n"
        "enum Chain<T> { end, next(value: Chain<T>), item(value: T) }\n"
        "type Distinct = distinct Int64\n"
        "type Refined = refined Int64 where true\n"
        "function exact(value: Int64) -> Bool effects { pure } { return value > 0 }\n"
        "function apply(callback: function(Int64) -> Bool effects { pure }) -> Bool "
        "effects { pure } { return true }\n"
        "function fails() -> Bool effects { panic } { panic \"failure\" }\n"
        "function root(base: capability Base, second: capability Base, derived: capability Derived, "
        "token: capability Token, ordered: Ordered, node: Node, "
        "looped: Loop, voided: Void, choice: Choice, chain: Chain<Int64>, "
        "owned_chain: Chain<capability Token>) -> Bool effects { panic } "
        "requires { apply(exact) && apply(base.choose) && apply(second.choose) } { "
        "if base.choose(1) { return true } else { return fails() } }\n";
    Compilation c;
    bool compiled = compile_text(&c, source);
    if (!compiled) sol_diagnostics_render_human(stderr, &c.source, &c.diagnostics);
    CHECK(compiled);
    if (!compiled) { free_compilation(&c); return; }
    SolDiagnostics diagnostics; sol_diagnostics_init(&diagnostics);
    SolMirProgram program; SolMirPlan plan; SolMirMaterialization materialization;
    sol_mir_program_init(&program); sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    SolIrCallableId import = callable(&c.ir, "choose", SOL_IR_CALLABLE_CAPABILITY);
    bool built = build_materialization(&c.ir,
        callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION), &import, 1,
        &program, &plan, &materialization, &diagnostics);
    CHECK(built);
    if (!built) {
        sol_diagnostics_render_human(stderr, &c.source, &diagnostics);
        sol_mir_materialization_free(&materialization); sol_mir_plan_free(&plan);
        sol_mir_program_free(&program); sol_diagnostics_free(&diagnostics);
        free_compilation(&c); return;
    }
    SolMirRepresentation representation; sol_mir_representation_init(&representation);
    SolMirRepresentationBuildRequest request = {&materialization, NULL};
    SolMirRepresentationBuildOutcome outcome = sol_mir_representation_build(
        &request, &representation, &diagnostics);
    if (outcome != SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED)
        sol_diagnostics_render_human(stderr, &c.source, &diagnostics);
    CHECK(outcome == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED);
    CHECK(representation.recipe_count == materialization.type_count);
    CHECK(sol_mir_representation_validate(&representation, NULL));

    bool kinds[SOL_MIR_RECIPE_CAPABILITY + 1] = {0};
    bool option_tags = false, result_tags = false;
    for (size_t i = 0; i < representation.recipe_count; ++i) {
        const SolMirRecipe *recipe = &representation.recipes[i];
        kinds[recipe->kind] = true;
        CHECK(recipe->kind != SOL_MIR_RECIPE_INT64
            || recipe->storage == SOL_MIR_STORAGE_SCALAR);
        CHECK(recipe->kind != SOL_MIR_RECIPE_BOOL
            || recipe->storage == SOL_MIR_STORAGE_SCALAR);
        CHECK(recipe->kind != SOL_MIR_RECIPE_TEXT
            || recipe->storage == SOL_MIR_STORAGE_TEXT_HANDLE);
        CHECK(recipe->kind != SOL_MIR_RECIPE_FUNCTION
            || recipe->storage == SOL_MIR_STORAGE_CALLABLE_HANDLE);
        CHECK(recipe->kind != SOL_MIR_RECIPE_CAPABILITY
            || recipe->storage == SOL_MIR_STORAGE_CAPABILITY_HANDLE);
        if (recipe->kind == SOL_MIR_RECIPE_OPTION) {
            const SolMirRecipeVariant *v
                = &representation.variants[recipe->variants.offset];
            option_tags = v[0].semantic_tag == 0 && v[0].fields.count == 0
                && v[1].semantic_tag == 1 && v[1].fields.count == 1;
        }
        if (recipe->kind == SOL_MIR_RECIPE_RESULT) {
            const SolMirRecipeVariant *v
                = &representation.variants[recipe->variants.offset];
            result_tags = v[0].semantic_tag == 0 && v[0].fields.count == 1
                && v[1].semantic_tag == 1 && v[1].fields.count == 1;
        }
    }
    for (size_t i = 0; i <= SOL_MIR_RECIPE_CAPABILITY; ++i) CHECK(kinds[i]);
    CHECK(option_tags && result_tags);

    const SolMirRecipe *empty = named_recipe(&representation, "Empty");
    const SolMirRecipe *ordered = named_recipe(&representation, "Ordered");
    const SolMirRecipe *node = named_recipe(&representation, "Node");
    const SolMirRecipe *loop = named_recipe(&representation, "Loop");
    const SolMirRecipe *voided = named_recipe(&representation, "Void");
    const SolMirRecipe *choice = named_recipe(&representation, "Choice");
    const SolMirRecipe *distinct = named_recipe(&representation, "Distinct");
    const SolMirRecipe *refined = named_recipe(&representation, "Refined");
    const SolMirRecipe *derived = named_recipe(&representation, "Derived");
    CHECK(empty != NULL && empty->inhabited && empty->zero_sized
        && empty->storage == SOL_MIR_STORAGE_NONE);
    CHECK(node != NULL && node->inhabited
        && node->storage == SOL_MIR_STORAGE_AGGREGATE_VALUE);
    CHECK(loop != NULL && !loop->inhabited
        && loop->storage == SOL_MIR_STORAGE_NONE
        && loop->copy_kind == SOL_MIR_COPY_UNREACHABLE);
    CHECK(voided != NULL && !voided->inhabited);
    CHECK(distinct != NULL && refined != NULL
        && distinct->backing != SOL_MIR_RECIPE_NONE
        && refined->backing != SOL_MIR_RECIPE_NONE
        && distinct->copy_kind == SOL_MIR_COPY_WRAPPER
        && refined->drop_kind == SOL_MIR_DROP_WRAPPER);
    CHECK(derived != NULL && derived->capability_source != SOL_MIR_RECIPE_NONE);
    CHECK(ordered != NULL && ordered->fields.count == 10);
    if (ordered != NULL && ordered->fields.count == 10) {
        const SolIrDefinition *definition = &c.ir.definitions[
            ordered->concrete_definition];
        const SolMirRecipeField *fields
            = &representation.fields[ordered->fields.offset];
        CHECK(fields[0].source_field == definition->fields.offset
            && fields[0].ordinal == 0
            && fields[1].source_field == definition->fields.offset + 1
            && fields[1].ordinal == 1);
    }
    CHECK(choice != NULL && choice->variants.count == 3);
    if (choice != NULL) {
        const SolIrDefinition *definition = &c.ir.definitions[
            choice->concrete_definition];
        const SolMirRecipeVariant *variants
            = &representation.variants[choice->variants.offset];
        CHECK(variants[0].source_variant == definition->variants.offset
            && variants[1].source_variant == definition->variants.offset + 1
            && variants[2].source_variant == definition->variants.offset + 2);
    }

    size_t exact_producers = 0, bound_producers = 0;
    SolMirRecipeId shared = SOL_MIR_RECIPE_NONE;
    for (size_t i = 0; i < representation.callable_producer_count; ++i) {
        const SolMirCallableProducer *producer
            = &representation.callable_producers[i];
        if (producer->kind == SOL_MIR_CALLABLE_PRODUCER_EXACT_FUNCTION) {
            ++exact_producers; shared = producer->function_recipe;
            CHECK(producer->captured_receiver_type == SOL_MIR_RECIPE_NONE);
        } else {
            ++bound_producers;
            CHECK(producer->captured_receiver_type != SOL_MIR_RECIPE_NONE
                && producer->captured_receiver_kind
                    != SOL_MIR_MATERIALIZED_RECEIVER_NONE
                && producer->captured_receiver_roots.count != 0);
            if (shared != SOL_MIR_RECIPE_NONE)
                CHECK(producer->function_recipe == shared);
        }
    }
    CHECK(exact_producers != 0 && bound_producers != 0);

    bool saw_copy_chain = false, saw_noncopy_chain = false;
    for (size_t i = 0; i < representation.recipe_count; ++i) {
        const SolMirRecipe *recipe = &representation.recipes[i];
        if (recipe->concrete_definition == SOL_IR_NONE
            || strcmp(c.ir.definitions[recipe->concrete_definition].name,
                "Chain") != 0) continue;
        saw_copy_chain |= recipe->is_copy;
        saw_noncopy_chain |= !recipe->is_copy;
    }
    CHECK(saw_copy_chain && saw_noncopy_chain);

    size_t first_length = 0, second_length = 0;
    char *first_text = render(&representation, &first_length);
    CHECK(first_text != NULL && strstr(first_text, "recipe r") != NULL
        && strstr(first_text, "producer p") != NULL);
    SolMirRepresentation repeated; sol_mir_representation_init(&repeated);
    CHECK(sol_mir_representation_build(&request, &repeated, &diagnostics)
        == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED);
    char *second_text = render(&repeated, &second_length);
    CHECK(first_text != NULL && second_text != NULL
        && first_length == second_length
        && memcmp(first_text, second_text, first_length) == 0);
    free(first_text); free(second_text);

    SolMirRepresentationLimits exact = {
        .max_recipes = representation.usage.recipes,
        .max_fields = representation.usage.fields,
        .max_variants = representation.usage.variants,
        .max_recipe_ids = representation.usage.recipe_ids,
        .max_callable_producers = representation.usage.callable_producers,
        .max_receiver_roots = representation.usage.receiver_roots,
        .max_owned_bytes = representation.usage.owned_bytes,
        .max_build_scratch_bytes = representation.usage.build_scratch_bytes,
        .max_build_work = representation.usage.build_work,
        .max_validation_work = representation.usage.validation_work,
        .max_validation_scratch_bytes
            = representation.usage.validation_scratch_bytes,
    };
    SolMirRepresentation limited; sol_mir_representation_init(&limited);
    SolMirRepresentationBuildRequest limited_request = {&materialization, &exact};
    CHECK(sol_mir_representation_build(&limited_request, &limited, &diagnostics)
        == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED);
    sol_mir_representation_free(&limited);
#define LIMIT_FAIL(field) do { \
    SolMirRepresentationLimits less = exact; --less.field; \
    limited_request.limits = &less; \
    SolMirRepresentationBuildOutcome limited_outcome = sol_mir_representation_build( \
        &limited_request, &limited, &diagnostics); \
    if (limited_outcome != SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED) \
        fprintf(stderr, "limit %s returned %d\n", #field, (int)limited_outcome); \
    CHECK(limited_outcome == SOL_MIR_REPRESENTATION_BUILD_RESOURCE_EXHAUSTED); \
    CHECK(limited.materialization == NULL); \
} while (0)
    LIMIT_FAIL(max_recipes); LIMIT_FAIL(max_fields); LIMIT_FAIL(max_variants);
    LIMIT_FAIL(max_recipe_ids); LIMIT_FAIL(max_callable_producers);
    LIMIT_FAIL(max_receiver_roots); LIMIT_FAIL(max_owned_bytes);
    LIMIT_FAIL(max_build_scratch_bytes); LIMIT_FAIL(max_build_work);
    LIMIT_FAIL(max_validation_work); LIMIT_FAIL(max_validation_scratch_bytes);
#undef LIMIT_FAIL

    if (representation.variant_count != 0) {
        size_t saved = representation.variants[0].semantic_tag;
        ++representation.variants[0].semantic_tag;
        CHECK(!sol_mir_representation_validate(&representation, NULL));
        FILE *stream = tmpfile(); CHECK(stream != NULL);
        if (stream != NULL) {
            CHECK(!sol_mir_representation_render(stream, &representation));
            CHECK(ftell(stream) == 0); fclose(stream);
        }
        representation.variants[0].semantic_tag = saved;
        SolMirPlanSlice fields = representation.variants[0].fields;
        representation.variants[0].fields.offset = SIZE_MAX;
        CHECK(!sol_mir_representation_validate(&representation, NULL));
        representation.variants[0].fields = fields;
    }
    size_t capacity = representation.recipe_capacity;
    ++representation.recipe_capacity;
    CHECK(!sol_mir_representation_validate(&representation, NULL));
    representation.recipe_capacity = capacity;
    size_t field_count = representation.field_count;
    size_t field_capacity = representation.field_capacity;
    size_t usage_fields = representation.usage.fields;
    SolMirRepresentationLimits saved_limits = representation.limits;
    representation.field_count = SIZE_MAX;
    representation.field_capacity = SIZE_MAX;
    representation.usage.fields = SIZE_MAX;
    representation.limits.max_fields = SIZE_MAX;
    representation.limits.max_owned_bytes = SIZE_MAX;
    CHECK(!sol_mir_representation_validate(&representation, NULL));
    representation.field_count = field_count;
    representation.field_capacity = field_capacity;
    representation.usage.fields = usage_fields;
    representation.limits = saved_limits;
    if (representation.field_count != 0 && materialization.shape_field_count != 0) {
        SolMirRecipeField *saved = representation.fields;
        representation.fields = (SolMirRecipeField *)(void *)materialization.shape_fields;
        CHECK(!sol_mir_representation_validate(&representation, NULL));
        representation.fields = saved;
    }
    CHECK(sol_mir_representation_validate(&representation, NULL));

    for (size_t i = 0; i < representation.callable_producer_count; ++i) {
        SolMirCallableProducer *producer = &representation.callable_producers[i];
        if (producer->kind != SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION) continue;
        SolMirMaterializedReceiverKind kind = producer->captured_receiver_kind;
        producer->captured_receiver_kind = SOL_MIR_MATERIALIZED_RECEIVER_NONE;
        CHECK(!sol_mir_representation_validate(&representation, NULL));
        producer->captured_receiver_kind = kind;
        break;
    }

    SolMirRecipe *saved_recipes = representation.recipes;
    representation.recipes = NULL;
    CHECK(!sol_mir_representation_validate(&representation, NULL));
    representation.recipes = saved_recipes;

    SolMirMaterializedTypeId saved_type = materialization.shape_fields[0].type;
    materialization.shape_fields[0].type = materialization.type_count;
    limited_request.limits = NULL;
    CHECK(sol_mir_representation_build(&limited_request, &limited, &diagnostics)
        == SOL_MIR_REPRESENTATION_BUILD_INVALID_MATERIALIZATION);
    CHECK(limited.materialization == NULL);
    materialization.shape_fields[0].type = saved_type;

    limited.materialization = &materialization;
    CHECK(sol_mir_representation_build(&request, &limited, &diagnostics)
        == SOL_MIR_REPRESENTATION_BUILD_INVALID_ARGUMENT);
    limited.materialization = NULL;

    sol_mir_representation_free(&limited);
    sol_mir_representation_free(&repeated);
    sol_mir_representation_free(&representation);
    sol_mir_materialization_free(&materialization); sol_mir_plan_free(&plan);
    sol_mir_program_free(&program); sol_diagnostics_free(&diagnostics);
    free_compilation(&c);
}

static void test_open_enum_rejected_transactionally(void) {
    Compilation c;
    CHECK(compile_text(&c,
        "module open_recipe\n"
        "open enum Open { first, second(value: Int64) }\n"
        "function root(value: Open) -> Int64 { return 0 }\n"));
    SolDiagnostics diagnostics; sol_diagnostics_init(&diagnostics);
    SolMirProgram program; SolMirPlan plan; SolMirMaterialization materialization;
    sol_mir_program_init(&program); sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    CHECK(build_materialization(&c.ir,
        callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION), NULL, 0,
        &program, &plan, &materialization, &diagnostics));
    SolMirRepresentation representation; sol_mir_representation_init(&representation);
    SolMirRepresentationBuildRequest request = {&materialization, NULL};
    CHECK(sol_mir_representation_build(&request, &representation, &diagnostics)
        == SOL_MIR_REPRESENTATION_BUILD_UNSUPPORTED);
    CHECK(representation.materialization == NULL);
    sol_mir_representation_free(&representation);
    sol_mir_materialization_free(&materialization); sol_mir_plan_free(&plan);
    sol_mir_program_free(&program); sol_diagnostics_free(&diagnostics);
    free_compilation(&c);
}

static void test_derived_capability_source_closure(void) {
    Compilation c;
    bool compiled = compile_text(&c,
        "module derived_closure\n"
        "capability Base {}\n"
        "capability C01 derives_from source: capability Base {}\n"
        "capability C02 derives_from source: capability C01 {}\n"
        "capability C03 derives_from source: capability C02 {}\n"
        "capability C04 derives_from source: capability C03 {}\n"
        "capability C05 derives_from source: capability C04 {}\n"
        "capability C06 derives_from source: capability C05 {}\n"
        "capability C07 derives_from source: capability C06 {}\n"
        "capability C08 derives_from source: capability C07 {}\n"
        "capability C09 derives_from source: capability C08 {}\n"
        "capability C10 derives_from source: capability C09 {}\n"
        "capability C11 derives_from source: capability C10 {}\n"
        "capability C12 derives_from source: capability C11 {}\n"
        "function root(top: capability C12) -> Int64 { return 0 }\n");
    CHECK(compiled);
    if (!compiled) { free_compilation(&c); return; }
    SolDiagnostics diagnostics; sol_diagnostics_init(&diagnostics);
    SolMirProgram program; SolMirPlan plan; SolMirMaterialization materialization;
    sol_mir_program_init(&program); sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    CHECK(build_materialization(&c.ir,
        callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION), NULL, 0,
        &program, &plan, &materialization, &diagnostics));
    SolMirMaterializedTypeId top = SOL_MIR_MATERIALIZED_NONE;
    for (size_t i = 0; i < materialization.type_count; ++i) {
        const SolMirMaterializedType *type = &materialization.types[i];
        if (type->nominal_category != SOL_MIR_MATERIALIZED_NOMINAL_CAPABILITY)
            continue;
        const char *name = c.ir.definitions[type->definition].name;
        if (strcmp(name, "C12") == 0) top = i;
    }
    CHECK(top != SOL_MIR_MATERIALIZED_NONE);
    static const char *chain[] = {"C12", "C11", "C10", "C09", "C08",
        "C07", "C06", "C05", "C04", "C03", "C02", "C01", "Base"};
    SolMirMaterializedTypeId current = top;
    for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); ++i) {
        CHECK(current < materialization.type_count);
        if (current >= materialization.type_count) break;
        const SolMirMaterializedType *type = &materialization.types[current];
        CHECK(strcmp(c.ir.definitions[type->definition].name, chain[i]) == 0);
        if (i + 1 == sizeof(chain) / sizeof(chain[0])) {
            CHECK(type->capability_source == SOL_MIR_MATERIALIZED_NONE);
        } else {
            CHECK(type->capability_source < materialization.type_count);
            current = type->capability_source;
        }
    }
    SolMirRepresentation representation; sol_mir_representation_init(&representation);
    SolMirRepresentationBuildRequest request = {&materialization, NULL};
    CHECK(sol_mir_representation_build(&request, &representation, &diagnostics)
        == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED);
    for (size_t i = 0; i < sizeof(chain) / sizeof(chain[0]); ++i)
        CHECK(named_recipe(&representation, chain[i]) != NULL);
    sol_mir_representation_free(&representation);
    sol_mir_materialization_free(&materialization); sol_mir_plan_free(&plan);
    sol_mir_program_free(&program); sol_diagnostics_free(&diagnostics);
    free_compilation(&c);
}

static void test_computed_bound_receiver(void) {
    Compilation c;
    bool compiled = compile_text(&c,
        "module computed_receiver\n"
        "capability Base { function choose(value: Int64) -> Bool effects { pure } }\n"
        "function preserve(value: capability Base) -> capability Base "
        "effects { pure } authority { result derives_from value } { return value }\n"
        "function apply(callback: function(Int64) -> Bool effects { pure }) -> Bool "
        "effects { pure } { return true }\n"
        "function root(flag: Bool, base: capability Base, other: capability Base) "
        "-> Bool effects { pure } requires { apply(preserve(base).choose) && "
        "apply((if flag { base } else { other }).choose) } { return true }\n");
    CHECK(compiled);
    if (!compiled) { free_compilation(&c); return; }
    SolDiagnostics diagnostics; sol_diagnostics_init(&diagnostics);
    SolMirProgram program; SolMirPlan plan; SolMirMaterialization materialization;
    sol_mir_program_init(&program); sol_mir_plan_init(&plan);
    sol_mir_materialization_init(&materialization);
    SolIrCallableId import = callable(&c.ir, "choose", SOL_IR_CALLABLE_CAPABILITY);
    bool built = build_materialization(&c.ir,
        callable(&c.ir, "root", SOL_IR_CALLABLE_FUNCTION), &import, 1,
        &program, &plan, &materialization, &diagnostics);
    CHECK(built);
    if (!built) {
        sol_diagnostics_render_human(stderr, &c.source, &diagnostics);
        sol_mir_materialization_free(&materialization); sol_mir_plan_free(&plan);
        sol_mir_program_free(&program); sol_diagnostics_free(&diagnostics);
        free_compilation(&c); return;
    }
    size_t computed = 0;
    for (size_t i = 0; i < materialization.semantic_site_count; ++i) {
        const SolMirMaterializedSemanticSite *site
            = &materialization.semantic_sites[i];
        if (site->kind != SOL_MIR_PLAN_DEMAND_BOUND_OPERATION) continue;
        computed += site->captured_receiver_kind
                == SOL_MIR_MATERIALIZED_RECEIVER_SOURCE_EXPRESSION
            && site->captured_receiver_place == SOL_MIR_MATERIALIZED_NONE
            && (site->captured_receiver_roots.count == 1
                || site->captured_receiver_roots.count == 2)
            && site->operation.root == SOL_MIR_MATERIALIZED_NONE;
    }
    CHECK(computed == 2);
    SolMirRepresentation representation; sol_mir_representation_init(&representation);
    SolMirRepresentationBuildRequest request = {&materialization, NULL};
    CHECK(sol_mir_representation_build(&request, &representation, &diagnostics)
        == SOL_MIR_REPRESENTATION_BUILD_SUCCEEDED);
    size_t producer = 0;
    for (size_t i = 0; i < representation.callable_producer_count; ++i) {
        const SolMirCallableProducer *item
            = &representation.callable_producers[i];
        producer += item->kind == SOL_MIR_CALLABLE_PRODUCER_BOUND_OPERATION
            && item->captured_receiver_kind
                == SOL_MIR_MATERIALIZED_RECEIVER_SOURCE_EXPRESSION
            && item->captured_receiver_place == SOL_MIR_MATERIALIZED_NONE
            && (item->captured_receiver_roots.count == 1
                || item->captured_receiver_roots.count == 2);
    }
    CHECK(producer == 2
        && sol_mir_representation_validate(&representation, NULL));
    sol_mir_representation_free(&representation);
    sol_mir_materialization_free(&materialization); sol_mir_plan_free(&plan);
    sol_mir_program_free(&program); sol_diagnostics_free(&diagnostics);
    free_compilation(&c);
}

int main(void) {
    test_complete_graph();
    test_derived_capability_source_closure();
    test_computed_bound_receiver();
    test_open_enum_rejected_transactionally();
    if (failures != 0) {
        fprintf(stderr, "%d MIR representation test(s) failed\n", failures);
        return 1;
    }
    printf("MIR representation tests passed\n");
    return 0;
}
