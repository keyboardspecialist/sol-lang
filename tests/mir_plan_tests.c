#include "sol/mir_plan.h"

#include "sol/effects.h"
#include "sol/lexer.h"
#include "sol/ownership.h"
#include "sol/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

SolMirPlanBuildOutcome sol_mir_plan_test_recursion_classification(int mode);
bool sol_mir_plan_test_effect_normalization(void);

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

static bool compile_text(Compilation *compilation, const char *text) {
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
    return sol_source_from_text(&compilation->source, "mir_plan.sol", text)
        && sol_lex(&compilation->source, &compilation->tokens,
            &compilation->diagnostics)
        && sol_parse(&compilation->source, &compilation->tokens,
            &compilation->syntax, &compilation->diagnostics)
        && sol_hir_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->diagnostics)
        && sol_type_check(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->diagnostics)
        && sol_effect_check(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->diagnostics)
        && sol_contract_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics)
        && sol_ir_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->ir,
            &compilation->diagnostics);
}

static void free_compilation(Compilation *compilation) {
    sol_ir_free(&compilation->ir);
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_syntax_tree_free(&compilation->syntax);
    sol_tokens_free(&compilation->tokens);
    sol_source_free(&compilation->source);
    sol_diagnostics_free(&compilation->diagnostics);
}

static SolIrCallableId callable(const SolIr *ir, const char *name) {
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (strcmp(ir->callables[id].name, name) == 0) return id;
    }
    return SOL_IR_NONE;
}

static SolIrCallableId callable_kind(const SolIr *ir, const char *name,
    SolIrCallableKind kind) {
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == kind
            && strcmp(ir->callables[id].name, name) == 0) return id;
    }
    return SOL_IR_NONE;
}

static bool compile_e6(Compilation *compilation, SolPackage *package) {
    memset(compilation, 0, sizeof(*compilation));
    sol_package_init(package);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
    char error[256];
    if (!sol_package_load_directory(package,
            SOL_TEST_SOURCE_DIR "/tests/conformance/e6",
            &compilation->diagnostics, error, sizeof(error))) return false;
    SolHirFileScope *scopes = malloc(package->file_count * sizeof(*scopes));
    if (scopes == NULL) return false;
    for (size_t index = 0; index < package->file_count; ++index) {
        scopes[index] = (SolHirFileScope){package->files[index].module_name,
            package->files[index].import_start, package->files[index].import_count,
            package->files[index].item_start, package->files[index].item_count};
    }
    bool valid = sol_hir_lower_scoped(&package->source, &package->syntax,
            scopes, package->file_count, &compilation->hir,
            &compilation->diagnostics)
        && sol_type_check(&package->source, &package->syntax, &compilation->hir,
            &compilation->types, &compilation->diagnostics)
        && sol_effect_check(&package->source, &package->syntax, &compilation->hir,
            &compilation->types, &compilation->effects,
            &compilation->diagnostics)
        && sol_contract_lower(&package->source, &package->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics)
        && sol_ir_lower_scoped(&package->source, &package->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, package->files, package->file_count,
            &compilation->ir, &compilation->diagnostics);
    free(scopes);
    return valid;
}

static void free_e6(Compilation *compilation, SolPackage *package) {
    sol_ir_free(&compilation->ir);
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_diagnostics_free(&compilation->diagnostics);
    sol_package_free(package);
}

static SolMirProgramBuildOutcome build_program(const SolIr *ir,
    SolIrCallableId root, SolMirProgram *program, SolDiagnostics *diagnostics) {
    SolMirProgramRoot item = {root, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgramBuildRequest request = {ir, &item, 1, NULL, 0, NULL};
    return sol_mir_program_build(&request, program, diagnostics);
}

static SolMirPlanBuildOutcome build_plan(const SolMirProgram *program,
    const SolMirPlanLimits *limits, SolMirPlan *plan,
    SolDiagnostics *diagnostics) {
    SolMirPlanBuildRequest request = {program, limits};
    return sol_mir_plan_build(&request, plan, diagnostics);
}

static const SolMirPlanInstance *find_instance(const SolMirPlan *plan,
    SolIrCallableId callable_id, size_t ordinal) {
    for (size_t index = 0; index < plan->instance_count; ++index) {
        if (plan->instances[index].callable == callable_id) {
            if (ordinal == 0) return &plan->instances[index];
            --ordinal;
        }
    }
    return NULL;
}

static bool row_has(const SolMirPlan *plan, SolMirPlanEffectRowId row_id,
    const char *name, SolMirPlanEffectAuthority authority, size_t ordinal) {
    if (row_id >= plan->effect_row_count) return false;
    const SolMirPlanEffectRow *row = &plan->effect_rows[row_id];
    for (size_t index = 0; index < row->atom_count; ++index) {
        const SolMirPlanEffectAtom *atom = &plan->effect_atoms[
            plan->effect_row_atoms[row->atom_offset + index]];
        if (atom->authority == authority && atom->ordinal == ordinal
            && strlen(name) == atom->length
            && memcmp(name, atom->name, atom->length) == 0) return true;
    }
    return false;
}

static void test_instances_types_cycles_and_limits(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module plan\n"
        "function identity<T>(value: T) -> T effects { pure } { return value }\n"
        "function direct(value: Int64) -> Int64 effects { pure } { "
        "if value > 0 { return direct(value - 1) } else { return 0 } }\n"
        "function root(flag: Bool) -> Int64 effects { pure } { "
        "if flag { return identity(1) } else { "
        "if identity(true) { return direct(2) } else { return 0 } } }\n"));
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program;
    SolMirPlan plan;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    CHECK(build_program(&compilation.ir, callable(&compilation.ir, "root"),
        &program, &diagnostics) == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    SolMirPlanBuildOutcome first_outcome = build_plan(&program, NULL, &plan,
        &diagnostics);
    if (first_outcome != SOL_MIR_PLAN_BUILD_SUCCEEDED) {
        sol_diagnostics_render_human(stderr, &compilation.source, &diagnostics);
    }
    CHECK(first_outcome == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    CHECK(sol_mir_plan_validate(&plan, NULL));
    CHECK(plan.instance_count == 4);
    size_t identity_instances = 0;
    size_t cycles = 0;
    for (size_t index = 0; index < plan.instance_count; ++index) {
        identity_instances += plan.instances[index].callable
            == callable(&compilation.ir, "identity");
    }
    for (size_t index = 0; index < plan.demand_count; ++index) {
        const SolMirPlanDemand *demand = &plan.demands[index];
        cycles += demand->parent != SOL_MIR_PLAN_NONE
            && demand->instance == demand->parent;
    }
    CHECK(identity_instances == 2);
    CHECK(cycles == 1);
    CHECK(plan.usage.instances == plan.instance_count);
    CHECK(plan.usage.concrete_types == plan.type_count);
    CHECK(plan.usage.typed_uses == plan.typed_use_count);

    SolMirPlanLimits exact = {plan.usage.instances, plan.usage.concrete_types,
        plan.usage.demands, plan.usage.typed_uses, plan.usage.contexts,
        plan.usage.planning_work, plan.usage.substitution_depth};
    SolMirPlan limited;
    sol_mir_plan_init(&limited);
    CHECK(build_plan(&program, &exact, &limited, &diagnostics)
        == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    sol_mir_plan_free(&limited);
    SolMirPlanLimits less = exact;
    --less.max_instances;
    CHECK(build_plan(&program, &less, &limited, &diagnostics)
        == SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED);
    CHECK(limited.program == NULL);
    less = exact;
    --less.max_concrete_types;
    CHECK(build_plan(&program, &less, &limited, &diagnostics)
        == SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED);
    less = exact;
    --less.max_demands;
    CHECK(build_plan(&program, &less, &limited, &diagnostics)
        == SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED);
    less = exact;
    --less.max_typed_uses;
    CHECK(build_plan(&program, &less, &limited, &diagnostics)
        == SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED);
    less = exact;
    --less.max_contexts;
    CHECK(build_plan(&program, &less, &limited, &diagnostics)
        == SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED);
    less = exact;
    --less.max_planning_work;
    CHECK(build_plan(&program, &less, &limited, &diagnostics)
        == SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED);
    less = exact;
    --less.max_substitution_depth;
    CHECK(build_plan(&program, &less, &limited, &diagnostics)
        != SOL_MIR_PLAN_BUILD_SUCCEEDED);

    SolMirPlanBuildRequest occupied_request = {&program, NULL};
    limited.program = &program;
    CHECK(sol_mir_plan_build(&occupied_request, &limited, &diagnostics)
        == SOL_MIR_PLAN_BUILD_INVALID_ARGUMENT);
    limited.program = NULL;

    SolMirPlanTypeId saved = plan.typed_uses[0].type;
    plan.typed_uses[0].type = SOL_MIR_PLAN_NONE;
    CHECK(!sol_mir_plan_validate(&plan, NULL));
    plan.typed_uses[0].type = saved;
    CHECK(sol_mir_plan_validate(&plan, NULL));
    SolMirPlanContextId saved_use_context = plan.typed_uses[0].context;
    plan.typed_uses[0].context = SOL_MIR_PLAN_NONE;
    CHECK(!sol_mir_plan_validate(&plan, NULL));
    plan.typed_uses[0].context = saved_use_context;
    if (plan.context_count > 1) {
        SolMirPlanContext first_context = plan.contexts[0];
        plan.contexts[0] = plan.contexts[1];
        plan.contexts[1] = first_context;
        CHECK(!sol_mir_plan_validate(&plan, NULL));
        plan.contexts[1] = plan.contexts[0];
        plan.contexts[0] = first_context;
    }
    SolMirPlanSlice saved_uses = plan.instances[0].typed_uses;
    --plan.instances[0].typed_uses.count;
    CHECK(!sol_mir_plan_validate(&plan, NULL));
    plan.instances[0].typed_uses = saved_uses;
    SolMirPlanType *types = plan.types;
    plan.types = (SolMirPlanType *)(void *)plan.instances;
    CHECK(!sol_mir_plan_validate(&plan, NULL));
    plan.types = types;
    CHECK(sol_mir_plan_validate(&plan, NULL));
#define CHECK_COUNT_BOUND(count_field, capacity_field) do { \
    size_t saved_count = plan.count_field; \
    plan.count_field = plan.capacity_field + 1; \
    CHECK(!sol_mir_plan_validate(&plan, NULL)); \
    plan.count_field = saved_count; \
} while (0)
    CHECK_COUNT_BOUND(type_count, type_capacity);
    CHECK_COUNT_BOUND(type_component_count, type_component_capacity);
    CHECK_COUNT_BOUND(type_parameter_access_count,
        type_parameter_access_capacity);
    CHECK_COUNT_BOUND(effect_atom_count, effect_atom_capacity);
    CHECK_COUNT_BOUND(effect_row_count, effect_row_capacity);
    CHECK_COUNT_BOUND(effect_row_atom_count, effect_row_atom_capacity);
    CHECK_COUNT_BOUND(instance_count, instance_capacity);
    CHECK_COUNT_BOUND(instance_type_id_count, instance_type_id_capacity);
    CHECK_COUNT_BOUND(instance_access_count, instance_access_capacity);
    CHECK_COUNT_BOUND(dictionary_entry_count, dictionary_entry_capacity);
    CHECK_COUNT_BOUND(import_count, import_capacity);
    CHECK_COUNT_BOUND(typed_use_count, typed_use_capacity);
    CHECK_COUNT_BOUND(context_count, context_capacity);
    CHECK_COUNT_BOUND(demand_count, demand_capacity);
#undef CHECK_COUNT_BOUND
    size_t type_capacity = plan.type_capacity;
    plan.type_capacity = plan.type_count - 1;
    CHECK(!sol_mir_plan_validate(&plan, NULL));
    plan.type_capacity = type_capacity;
    types = plan.types;
    plan.types = NULL;
    CHECK(!sol_mir_plan_validate(&plan, NULL));
    plan.types = types;
    CHECK(sol_mir_plan_validate(&plan, NULL));
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_evidence_self_and_generic_root_policy(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module dictionary\n"
        "trait Score { function score(self: Self) -> Int64 effects { pure } }\n"
        "implementation Score for Int64 { function score(self: Self) -> Int64 "
        "effects { pure } { return self } }\n"
        "function scored<T: Score>(value: T) -> Int64 effects { pure } "
        "{ return value.score() }\n"
        "function outer<U: Score>(value: U) -> Int64 effects { pure } "
        "{ return scored(value) }\n"
        "function root() -> Int64 effects { pure } { return outer(4) }\n"));
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program;
    SolMirPlan plan;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    CHECK(build_program(&compilation.ir, callable(&compilation.ir, "root"),
        &program, &diagnostics) == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(build_plan(&program, NULL, &plan, &diagnostics)
        == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    CHECK(plan.dictionary_entry_count == 2);
    for (size_t index = 0; index < plan.dictionary_entry_count; ++index) {
        CHECK(plan.dictionary_entries[index].method < compilation.ir.callable_count
            && compilation.ir.callables[plan.dictionary_entries[index].method].body
                != SOL_IR_NONE);
    }
    bool receiver_is_int = false;
    for (size_t index = 0; index < plan.instance_count; ++index) {
        if (plan.instances[index].receiver == SOL_MIR_PLAN_NONE) continue;
        receiver_is_int = plan.types[plan.instances[index].receiver].kind
            == SOL_IR_TYPE_INT64;
    }
    CHECK(receiver_is_int);
    SolIrCallableId saved_method = plan.dictionary_entries[0].method;
    plan.dictionary_entries[0].method = SOL_IR_NONE;
    CHECK(!sol_mir_plan_validate(&plan, NULL));
    plan.dictionary_entries[0].method = saved_method;
    SolIrCallableId saved_callable = plan.instances[0].callable;
    plan.instances[0].callable = SOL_IR_NONE;
    CHECK(!sol_mir_plan_validate(&plan, NULL));
    plan.instances[0].callable = saved_callable;
    SolMirPlanInstanceId saved_child = plan.demands[0].instance;
    plan.demands[0].instance = SOL_MIR_PLAN_NONE;
    CHECK(!sol_mir_plan_validate(&plan, NULL));
    plan.demands[0].instance = saved_child;
    SolIrTypeKind saved_kind = plan.types[0].kind;
    plan.types[0].kind = SOL_IR_TYPE_PARAMETER;
    CHECK(!sol_mir_plan_validate(&plan, NULL));
    plan.types[0].kind = saved_kind;
    CHECK(sol_mir_plan_validate(&plan, NULL));
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);

    sol_mir_program_init(&program);
    CHECK(build_program(&compilation.ir, callable(&compilation.ir, "scored"),
        &program, &diagnostics) == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_e6_entry_census(void) {
    Compilation compilation;
    SolPackage package;
    CHECK(compile_e6(&compilation, &package));
    const SolIr *ir = &compilation.ir;
    const char *names[] = {"write", "get", "count", "read"};
    SolIrCallableId approvals[4];
    for (size_t index = 0; index < 4; ++index) {
        approvals[index] = callable_kind(ir, names[index],
            SOL_IR_CALLABLE_CAPABILITY);
    }
    SolMirProgramRoot root = {callable_kind(ir, "launch",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_ENTRY};
    SolMirProgramBuildRequest program_request
        = {ir, &root, 1, approvals, 4, NULL};
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program;
    SolMirPlan plan;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    CHECK(sol_mir_program_build(&program_request, &program, &diagnostics)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    SolMirPlanBuildOutcome outcome = build_plan(&program, NULL, &plan,
        &diagnostics);
    if (outcome != SOL_MIR_PLAN_BUILD_SUCCEEDED) {
        fprintf(stderr, "E6 plan outcome: %d allocation_failed=%d\n",
            (int)outcome, diagnostics.allocation_failed ? 1 : 0);
        for (size_t index = 0; index < diagnostics.count; ++index) {
            fprintf(stderr, "E6 plan diagnostic: %s\n",
                diagnostics.items[index].message);
        }
    }
    CHECK(outcome == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    CHECK(plan.instance_count == 8);
    CHECK(plan.import_count == 4);
    CHECK(plan.dictionary_entry_count == 1);
    SolIrCallableId expected_callables[] = {
        callable_kind(ir, "launch", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "fail_runtime", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "compute", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "identity", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "score", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "score", SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION),
        callable_kind(ir, "increment", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "extract", SOL_IR_CALLABLE_FUNCTION),
    };
    for (size_t expected = 0;
        expected < sizeof(expected_callables) / sizeof(expected_callables[0]);
        ++expected) {
        CHECK(find_instance(&plan, expected_callables[expected], 0) != NULL);
        CHECK(find_instance(&plan, expected_callables[expected], 1) == NULL);
    }
    const SolMirPlanInstance *identity = find_instance(&plan,
        callable_kind(ir, "identity", SOL_IR_CALLABLE_FUNCTION), 0);
    const SolMirPlanInstance *generic_score = find_instance(&plan,
        callable_kind(ir, "score", SOL_IR_CALLABLE_FUNCTION), 0);
    const SolMirPlanInstance *method = find_instance(&plan,
        callable_kind(ir, "score", SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION), 0);
    CHECK(identity != NULL && identity->type_arguments.count == 1
        && plan.types[plan.instance_type_ids[identity->type_arguments.offset]].kind
            == SOL_IR_TYPE_INT64);
    CHECK(generic_score != NULL && generic_score->type_arguments.count == 1
        && plan.types[plan.instance_type_ids[
            generic_score->type_arguments.offset]].kind == SOL_IR_TYPE_INT64);
    CHECK(plan.types[plan.dictionary_entries[0].type].kind
        == SOL_IR_TYPE_INT64);
    CHECK(method != NULL && method->receiver != SOL_MIR_PLAN_NONE
        && plan.types[method->receiver].kind == SOL_IR_TYPE_INT64
        && plan.dictionary_entries[0].method == method->callable);
    const SolMirPlanInstance *launch = find_instance(&plan,
        callable_kind(ir, "launch", SOL_IR_CALLABLE_FUNCTION), 0);
    CHECK(launch != NULL
        && row_has(&plan, launch->effects, "configuration.read",
            SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER, 2)
        && row_has(&plan, launch->effects, "console.write",
            SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER, 0)
        && row_has(&plan, launch->effects, "process.arguments.count",
            SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER, 1)
        && row_has(&plan, launch->effects, "process.arguments.get",
            SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER, 1)
        && row_has(&plan, launch->effects, "panic",
            SOL_MIR_PLAN_EFFECT_AUTHORITY_NONE, 0));
    for (size_t index = 0; index < plan.import_count; ++index) {
        const SolMirPlanEffectRow *row
            = &plan.effect_rows[plan.imports[index].effects];
        CHECK(row->atom_count == 1);
        if (row->atom_count == 1) {
            CHECK(plan.effect_atoms[plan.effect_row_atoms[row->atom_offset]].authority
                == SOL_MIR_PLAN_EFFECT_AUTHORITY_RECEIVER);
        }
    }
    bool use_kinds[SOL_MIR_PLAN_USE_UNREACHABLE_PROOF + 1];
    memset(use_kinds, 0, sizeof(use_kinds));
    for (size_t index = 0; index < plan.typed_use_count; ++index) {
        use_kinds[plan.typed_uses[index].kind] = true;
    }
    for (size_t kind = SOL_MIR_PLAN_USE_RECEIVER;
        kind <= SOL_MIR_PLAN_USE_OBLIGATION_PREDICATE; ++kind) {
        CHECK(use_kinds[kind]);
    }
    for (size_t child = 0; child < plan.instance_count; ++child) {
        bool incoming = false;
        for (size_t demand = 0; demand < plan.demand_count; ++demand) {
            incoming = incoming || plan.demands[demand].instance == child;
        }
        CHECK(incoming);
    }
    for (size_t imported = 0; imported < plan.import_count; ++imported) {
        bool incoming = false;
        for (size_t demand = 0; demand < plan.demand_count; ++demand) {
            incoming = incoming || plan.demands[demand].import == imported;
        }
        CHECK(incoming);
    }
    for (size_t reference = 0; reference < program.reference_count; ++reference) {
        const SolMirProgramReference *item = &program.references[reference];
        if (item->kind != SOL_MIR_PROGRAM_REFERENCE_FUNCTION_VALUE
            && item->kind != SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION
            && item->kind
                != SOL_MIR_PROGRAM_REFERENCE_PREDICATE_FUNCTION_VALUE) continue;
        bool connected = false;
        for (size_t demand = 0; demand < plan.demand_count; ++demand) {
            const SolMirPlanDemand *edge = &plan.demands[demand];
            connected = connected || (edge->source.callable == item->source.callable
                && edge->source.expression == item->source.expression
                && edge->source.file == item->source.file
                && edge->source.start == item->source.start
                && edge->source.end == item->source.end
                && edge->symbolic_target == item->target);
        }
        CHECK(connected);
    }
    CHECK(sol_mir_plan_validate(&plan, NULL));
    plan.effect_atom_count = plan.effect_atom_capacity + 1;
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);

    SolMirProgramRoot roots[5];
    roots[0] = root;
    size_t root_count = 1;
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == SOL_IR_CALLABLE_TEST) {
            roots[root_count++]
                = (SolMirProgramRoot){id, SOL_MIR_PROGRAM_ROOT_TEST};
        }
    }
    CHECK(root_count == 5);
    program_request.roots = roots;
    program_request.root_count = root_count;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    CHECK(sol_mir_program_build(&program_request, &program, &diagnostics)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(build_plan(&program, NULL, &plan, &diagnostics)
        == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    CHECK(plan.instance_count == 14);
    CHECK(plan.import_count == 4);
    CHECK(plan.dictionary_entry_count == 1);
    SolIrCallableId expected_all[14] = {
        callable_kind(ir, "launch", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "fail_runtime", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "compute", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "identity", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "score", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "score", SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION),
        callable_kind(ir, "increment", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "extract", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "increment_option", SOL_IR_CALLABLE_FUNCTION),
        callable_kind(ir, "increment_result", SOL_IR_CALLABLE_FUNCTION),
    };
    size_t expected_all_count = 10;
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == SOL_IR_CALLABLE_TEST) {
            expected_all[expected_all_count++] = id;
        }
    }
    CHECK(expected_all_count == 14);
    for (size_t expected = 0; expected < expected_all_count; ++expected) {
        CHECK(find_instance(&plan, expected_all[expected], 0) != NULL);
        CHECK(find_instance(&plan, expected_all[expected], 1) == NULL);
    }
    identity = find_instance(&plan,
        callable_kind(ir, "identity", SOL_IR_CALLABLE_FUNCTION), 0);
    generic_score = find_instance(&plan,
        callable_kind(ir, "score", SOL_IR_CALLABLE_FUNCTION), 0);
    method = find_instance(&plan,
        callable_kind(ir, "score", SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION), 0);
    CHECK(identity != NULL && identity->type_arguments.count == 1
        && plan.types[plan.instance_type_ids[identity->type_arguments.offset]].kind
            == SOL_IR_TYPE_INT64);
    CHECK(generic_score != NULL && generic_score->dictionary.count == 1
        && generic_score->type_arguments.count == 1
        && plan.types[plan.instance_type_ids[
            generic_score->type_arguments.offset]].kind == SOL_IR_TYPE_INT64);
    CHECK(method != NULL && method->receiver != SOL_MIR_PLAN_NONE
        && plan.types[method->receiver].kind == SOL_IR_TYPE_INT64
        && plan.dictionary_entries[generic_score->dictionary.offset].method
            == method->callable);
    for (size_t child = 0; child < plan.instance_count; ++child) {
        bool incoming = false;
        for (size_t demand = 0; demand < plan.demand_count; ++demand) {
            incoming = incoming || plan.demands[demand].instance == child;
        }
        CHECK(incoming);
    }
    for (size_t imported = 0; imported < plan.import_count; ++imported) {
        bool incoming = false;
        for (size_t demand = 0; demand < plan.demand_count; ++demand) {
            incoming = incoming || plan.demands[demand].import == imported;
        }
        CHECK(incoming);
    }
    CHECK(sol_mir_plan_validate(&plan, NULL));
    SolMirPlan repeated;
    sol_mir_plan_init(&repeated);
    CHECK(build_plan(&program, NULL, &repeated, &diagnostics)
        == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    CHECK(repeated.instance_count == plan.instance_count);
    CHECK(repeated.type_count == plan.type_count);
    CHECK(repeated.demand_count == plan.demand_count);
    CHECK(repeated.typed_use_count == plan.typed_use_count);
    for (size_t index = 0; index < plan.instance_count; ++index) {
        CHECK(repeated.instances[index].callable == plan.instances[index].callable);
        CHECK(repeated.instances[index].receiver == plan.instances[index].receiver);
        CHECK(repeated.instances[index].result == plan.instances[index].result);
        CHECK(repeated.instances[index].effect_tail
            == plan.instances[index].effect_tail);
        CHECK(repeated.instances[index].effects == plan.instances[index].effects);
    }
    for (size_t index = 0; index < plan.type_count; ++index) {
        CHECK(repeated.types[index].kind == plan.types[index].kind);
        CHECK(repeated.types[index].definition == plan.types[index].definition);
        CHECK(repeated.types[index].result == plan.types[index].result);
        CHECK(repeated.types[index].effects == plan.types[index].effects);
    }
    SolMirProgramRoot reverse_roots[5];
    SolIrCallableId reverse_approvals[4];
    for (size_t index = 0; index < root_count; ++index) {
        reverse_roots[index] = roots[root_count - index - 1];
    }
    for (size_t index = 0; index < 4; ++index) {
        reverse_approvals[index] = approvals[3 - index];
    }
    SolMirProgram permuted_program;
    SolMirPlan permuted_plan;
    sol_mir_program_init(&permuted_program);
    sol_mir_plan_init(&permuted_plan);
    SolMirProgramBuildRequest permuted_request = {ir, reverse_roots,
        root_count, reverse_approvals, 4, NULL};
    CHECK(sol_mir_program_build(&permuted_request, &permuted_program,
        &diagnostics) == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(build_plan(&permuted_program, NULL, &permuted_plan, &diagnostics)
        == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    CHECK(permuted_plan.instance_count == plan.instance_count);
    CHECK(permuted_plan.type_count == plan.type_count);
    CHECK(permuted_plan.demand_count == plan.demand_count);
    for (size_t index = 0; index < plan.instance_count; ++index) {
        CHECK(permuted_plan.instances[index].callable
            == plan.instances[index].callable);
        CHECK(permuted_plan.instances[index].receiver
            == plan.instances[index].receiver);
        CHECK(permuted_plan.instances[index].effect_tail
            == plan.instances[index].effect_tail);
        CHECK(permuted_plan.instances[index].effects
            == plan.instances[index].effects);
    }
    for (size_t index = 0; index < plan.demand_count; ++index) {
        CHECK(permuted_plan.demands[index].kind == plan.demands[index].kind);
        CHECK(permuted_plan.demands[index].source.callable
            == plan.demands[index].source.callable);
        CHECK(permuted_plan.demands[index].source.expression
            == plan.demands[index].source.expression);
        CHECK(permuted_plan.demands[index].symbolic_target
            == plan.demands[index].symbolic_target);
        CHECK(permuted_plan.demands[index].instance
            == plan.demands[index].instance);
        CHECK(permuted_plan.demands[index].import
            == plan.demands[index].import);
    }
    sol_mir_plan_free(&permuted_plan);
    sol_mir_program_free(&permuted_program);
    sol_mir_plan_free(&repeated);
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_e6(&compilation, &package);
}

static void test_nested_types_effect_closure_and_root_policy(void) {
    Compilation compilation;
    bool compiled = compile_text(&compilation,
        "module substitutions\n"
        "record Box<T> { value: Option<(T, Bool)> }\n"
        "function root(callback: function() -> Int64 effects { pure }) "
        "-> Int64 effects { pure } { "
        "let value = Box<Int64> { value = some((1, true)) } "
        "while false invariant { true } decreases { 1 } {} "
        "if false { unreachable because { true } } else { return 1 } }\n"
        "function effect_root<effects G>(callback: function() -> Int64 effects G) "
        "-> Int64 effects { G } { return 0 }\n"
        "function generic_root<T>(value: T) -> T effects { pure } { return value }\n");
    if (!compiled) sol_diagnostics_render_human(stderr, &compilation.source,
        &compilation.diagnostics);
    CHECK(compiled);
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program;
    SolMirPlan plan;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    CHECK(build_program(&compilation.ir, callable(&compilation.ir, "root"),
        &program, &diagnostics) == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    SolMirPlanBuildOutcome outcome = build_plan(&program, NULL, &plan,
        &diagnostics);
    CHECK(outcome == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    CHECK(plan.instance_count == 1);
    bool saw_option = false;
    bool saw_tuple = false;
    bool saw_function = false;
    bool saw_box = false;
    for (size_t index = 0; index < plan.type_count; ++index) {
        CHECK(plan.types[index].kind != SOL_IR_TYPE_PARAMETER);
        CHECK(plan.types[index].kind != SOL_IR_TYPE_SELF);
        saw_option = saw_option || plan.types[index].kind == SOL_IR_TYPE_OPTION;
        saw_tuple = saw_tuple || plan.types[index].kind == SOL_IR_TYPE_TUPLE;
        saw_function = saw_function || plan.types[index].kind
            == SOL_IR_TYPE_FUNCTION;
        saw_box = saw_box || (plan.types[index].kind == SOL_IR_TYPE_NOMINAL
            && plan.types[index].argument_count == 1);
    }
    CHECK(saw_option && saw_tuple && saw_function && saw_box);
    bool saw_loop = false;
    bool saw_unreachable = false;
    for (size_t index = 0; index < plan.typed_use_count; ++index) {
        saw_loop = saw_loop || plan.typed_uses[index].kind
            == SOL_MIR_PLAN_USE_LOOP_OBLIGATION;
        saw_unreachable = saw_unreachable || plan.typed_uses[index].kind
            == SOL_MIR_PLAN_USE_UNREACHABLE_PROOF;
    }
    CHECK(saw_loop && saw_unreachable);
    CHECK(plan.context_count >= plan.instance_count);
    for (size_t index = 0; index < plan.instance_count; ++index) {
        CHECK(plan.instances[index].contexts.count >= 1);
        CHECK(plan.contexts[plan.instances[index].contexts.offset].kind
            == SOL_MIR_PLAN_CONTEXT_BODY);
    }
    CHECK(sol_mir_plan_validate(&plan, NULL));
    SolMirPlanLimits exact = {plan.usage.instances, plan.usage.concrete_types,
        plan.usage.demands, plan.usage.typed_uses, plan.usage.contexts,
        plan.usage.planning_work, plan.usage.substitution_depth};
    CHECK(exact.max_substitution_depth > 1);
    SolMirPlanLimits shallow = exact;
    --shallow.max_substitution_depth;
    SolMirPlan limited;
    sol_mir_plan_init(&limited);
    CHECK(build_plan(&program, &shallow, &limited, &diagnostics)
        == SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED);
    CHECK(limited.program == NULL);
    SolMirPlanLimits fewer_types = exact;
    --fewer_types.max_concrete_types;
    CHECK(build_plan(&program, &fewer_types, &limited, &diagnostics)
        == SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED);
    CHECK(limited.program == NULL);
    sol_mir_plan_free(&limited);
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);

    SolIrCallableId rejected[] = {
        callable(&compilation.ir, "generic_root"),
        callable(&compilation.ir, "effect_root"),
    };
    for (size_t index = 0; index < 2; ++index) {
        sol_mir_program_init(&program);
        sol_mir_plan_init(&plan);
        CHECK(build_program(&compilation.ir, rejected[index], &program,
            &diagnostics) == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
        CHECK(build_plan(&program, NULL, &plan, &diagnostics)
            == SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED);
        CHECK(plan.program == NULL);
        sol_mir_plan_free(&plan);
        sol_mir_program_free(&program);
    }
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_callee_effect_coordinates(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module coordinates\n"
        "capability Host { function read() -> Int64 "
        "effects { service.read<Self> } }\n"
        "function helper(host: borrow capability Host) -> Int64 "
        "effects { service.read<host> } { return host.read() }\n"
        "function left(first: borrow capability Host, "
        "second: borrow capability Host) -> Int64 "
        "effects { service.read<first> } { return helper(first) }\n"
        "function right(first: borrow capability Host, "
        "second: borrow capability Host) -> Int64 "
        "effects { service.read<second> } { return helper(second) }\n"
        "function root(first: capability Host, second: capability Host) -> Int64 "
        "effects { service.read<first> service.read<second> } "
        "{ return left(first, second) + right(first, second) }\n"));
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program;
    SolMirPlan plan;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    SolIrCallableId read_id = callable_kind(&compilation.ir, "read",
        SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root = {callable(&compilation.ir, "root"),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgramBuildRequest request
        = {&compilation.ir, &root, 1, &read_id, 1, NULL};
    CHECK(sol_mir_program_build(&request, &program, &diagnostics)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(build_plan(&program, NULL, &plan, &diagnostics)
        == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    CHECK(plan.instance_count == 4);
    CHECK(plan.import_count == 1);
    CHECK(find_instance(&plan, callable(&compilation.ir, "helper"), 0) != NULL);
    CHECK(find_instance(&plan, callable(&compilation.ir, "helper"), 1) == NULL);
    const SolMirPlanInstance *helper = find_instance(&plan,
        callable(&compilation.ir, "helper"), 0);
    const SolMirPlanInstance *left = find_instance(&plan,
        callable(&compilation.ir, "left"), 0);
    const SolMirPlanInstance *right = find_instance(&plan,
        callable(&compilation.ir, "right"), 0);
    const SolMirPlanInstance *root_instance = find_instance(&plan,
        callable(&compilation.ir, "root"), 0);
    CHECK(helper != NULL && row_has(&plan, helper->effects, "service.read",
        SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER, 0));
    CHECK(left != NULL && row_has(&plan, left->effects, "service.read",
        SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER, 0));
    CHECK(right != NULL && row_has(&plan, right->effects, "service.read",
        SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER, 1));
    CHECK(root_instance != NULL
        && row_has(&plan, root_instance->effects, "service.read",
            SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER, 0)
        && row_has(&plan, root_instance->effects, "service.read",
            SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER, 1));
    CHECK(row_has(&plan, plan.imports[0].effects, "service.read",
        SOL_MIR_PLAN_EFFECT_AUTHORITY_RECEIVER, 0));
    if (helper != NULL) {
        CHECK(plan.effect_rows[helper->effect_tail].atom_count == 0);
    }
    CHECK(sol_mir_plan_validate(&plan, NULL));
    CHECK(plan.effect_atom_count >= 2);
    if (plan.effect_atom_count >= 2) {
        size_t first_length = plan.effect_atoms[0].length;
        plan.effect_atoms[0].length = SIZE_MAX;
        CHECK(!sol_mir_plan_validate(&plan, NULL));
        plan.effect_atoms[0].length = first_length;
        char terminal = plan.effect_atoms[0].name[plan.effect_atoms[0].length];
        plan.effect_atoms[0].name[plan.effect_atoms[0].length] = 'x';
        CHECK(!sol_mir_plan_validate(&plan, NULL));
        plan.effect_atoms[0].name[plan.effect_atoms[0].length] = terminal;
        char *second_name = plan.effect_atoms[1].name;
        size_t second_length = plan.effect_atoms[1].length;
        plan.effect_atoms[1].name = plan.effect_atoms[0].name;
        plan.effect_atoms[1].length = plan.effect_atoms[0].length;
        CHECK(!sol_mir_plan_validate(&plan, NULL));
        plan.effect_atoms[1].name = second_name;
        plan.effect_atoms[1].length = second_length;
    }
    CHECK(sol_mir_plan_validate(&plan, NULL));
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_predicate_function_value_demand(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module handles\n"
        "function predicate() -> Bool effects { pure } { return true }\n"
        "function accepts(callback: function() -> Bool effects { pure }) "
        "-> Bool effects { pure } { return true }\n"
        "function root() -> Int64 effects { pure } "
        "requires { accepts(predicate) } { return 1 }\n"));
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgram program;
    SolMirPlan plan;
    sol_mir_program_init(&program);
    sol_mir_plan_init(&plan);
    CHECK(build_program(&compilation.ir, callable(&compilation.ir, "root"),
        &program, &diagnostics) == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(build_plan(&program, NULL, &plan, &diagnostics)
        == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    const SolMirPlanInstance *accepts = find_instance(&plan,
        callable(&compilation.ir, "accepts"), 0);
    CHECK(accepts != NULL);
    CHECK(plan.effect_rows[accepts->effect_tail].atom_count == 0);
    CHECK(plan.effect_rows[accepts->effects].atom_count == 0);
    bool function_value = false;
    for (size_t index = 0; index < plan.demand_count; ++index) {
        const SolMirPlanDemand *demand = &plan.demands[index];
        function_value = function_value
            || (demand->kind
                    == SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE
                && demand->symbolic_target
                    == callable(&compilation.ir, "predicate")
                && demand->instance != SOL_MIR_PLAN_NONE);
    }
    CHECK(function_value);
    CHECK(find_instance(&plan, callable(&compilation.ir, "predicate"), 0)
        != NULL);
    CHECK(sol_mir_plan_validate(&plan, NULL));
    sol_mir_plan_free(&plan);
    sol_mir_program_free(&program);
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static void test_recursion_classification(void) {
    CHECK(sol_mir_plan_test_recursion_classification(0)
        == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    CHECK(sol_mir_plan_test_recursion_classification(1)
        == SOL_MIR_PLAN_BUILD_EXPANDING_RECURSION);
    CHECK(sol_mir_plan_test_recursion_classification(2)
        == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    CHECK(sol_mir_plan_test_recursion_classification(3)
        == SOL_MIR_PLAN_BUILD_EXPANDING_RECURSION);
    CHECK(sol_mir_plan_test_recursion_classification(4)
        == SOL_MIR_PLAN_BUILD_EXPANDING_RECURSION);
    CHECK(sol_mir_plan_test_recursion_classification(5)
        == SOL_MIR_PLAN_BUILD_SUCCEEDED);
    CHECK(sol_mir_plan_test_effect_normalization());
}

int main(void) {
    test_instances_types_cycles_and_limits();
    test_evidence_self_and_generic_root_policy();
    test_e6_entry_census();
    test_nested_types_effect_closure_and_root_policy();
    test_callee_effect_coordinates();
    test_predicate_function_value_demand();
    test_recursion_classification();
    if (failures != 0) {
        fprintf(stderr, "%d MIR plan test(s) failed\n", failures);
        return 1;
    }
    printf("MIR plan tests passed\n");
    return 0;
}
