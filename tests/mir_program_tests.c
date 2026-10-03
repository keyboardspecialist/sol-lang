#include "sol/mir_program.h"
#include "sol/effects.h"
#include "sol/lexer.h"
#include "sol/ownership.h"
#include "sol/package.h"

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
    return sol_source_from_text(&compilation->source, "mir_program.sol", text)
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

static SolIrCallableId callable(const SolIr *ir, const char *name,
    SolIrCallableKind kind) {
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == kind
            && strcmp(ir->callables[id].name, name) == 0) return id;
    }
    return SOL_IR_NONE;
}

static bool has_template(const SolMirProgram *program, SolIrCallableId id) {
    for (size_t index = 0; index < program->template_count; ++index) {
        if (program->templates[index].callable == id) return true;
    }
    return false;
}

static bool has_import(const SolMirProgram *program, SolIrCallableId id) {
    for (size_t index = 0; index < program->import_count; ++index) {
        if (program->imports[index].callable == id) return true;
    }
    return false;
}

static SolMirProgramBuildOutcome build(const SolIr *ir,
    const SolMirProgramRoot *roots, size_t root_count,
    const SolIrCallableId *approvals, size_t approval_count,
    const SolMirProgramLimits *limits, SolMirProgram *program) {
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgramBuildRequest request = {
        ir, roots, root_count, approvals, approval_count, limits,
    };
    SolMirProgramBuildOutcome outcome
        = sol_mir_program_build(&request, program, &diagnostics);
    if (outcome == SOL_MIR_PROGRAM_BUILD_SUCCEEDED) {
        CHECK(sol_mir_program_validate(program, NULL));
    }
    if (outcome != SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && outcome != SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED
        && outcome != SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE
        && outcome != SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT
        && outcome != SOL_MIR_PROGRAM_BUILD_INVALID_IR) {
        for (size_t index = 0; index < diagnostics.count; ++index) {
            fprintf(stderr, "program diagnostic: %s\n",
                diagnostics.items[index].message);
        }
    }
    sol_diagnostics_free(&diagnostics);
    return outcome;
}

static bool program_fields_equal(const SolMirProgram *a,
    const SolMirProgram *b) {
#define ARRAY_EQUAL(left, right, count, size) \
    ((count) == 0 || memcmp((left), (right), (count) * (size)) == 0)
    return a->ir == b->ir && a->root_count == b->root_count
        && a->approved_import_count == b->approved_import_count
        && a->template_count == b->template_count
        && a->import_count == b->import_count
        && a->specialization_count == b->specialization_count
        && a->reference_count == b->reference_count
        && memcmp(&a->usage, &b->usage, sizeof(a->usage)) == 0
        && ARRAY_EQUAL(a->roots, b->roots, a->root_count, sizeof(*a->roots))
        && ARRAY_EQUAL(a->approved_imports, b->approved_imports,
            a->approved_import_count, sizeof(*a->approved_imports))
        && ARRAY_EQUAL(a->imports, b->imports, a->import_count,
            sizeof(*a->imports))
        && ARRAY_EQUAL(a->specializations, b->specializations,
            a->specialization_count, sizeof(*a->specializations))
        && ARRAY_EQUAL(a->references, b->references, a->reference_count,
            sizeof(*a->references));
#undef ARRAY_EQUAL
}

/* Independently reconstruct the bounded hostile proof's work census: one tick
 * per method invoke, every candidate reference comparison, and every source
 * file range considered by matching invoke references. */
static size_t direct_method_validation_work(const SolMirProgram *program) {
    const SolIr *ir = program->ir;
    size_t work = 0;
    for (size_t template_id = 0; template_id < program->template_count; ++template_id) {
        const SolMir *mir = &program->templates[template_id].mir;
        for (size_t block = 0; block < mir->block_count; ++block) {
            const SolMirTerminator *term = &mir->blocks[block].terminator;
            if (term->kind != SOL_MIR_TERM_INVOKE
                || term->as.invoke.kind != SOL_IR_CALL_METHOD) continue;
            ++work;
            const SolIrExpression *call = term->as.invoke.source_expression
                    < ir->expression_count
                ? &ir->expressions[term->as.invoke.source_expression] : NULL;
            if (call == NULL || call->kind != SOL_IR_EXPR_CALL
                || call->as.call.evidence.count != 1
                || call->as.call.evidence.offset >= ir->evidence_count
                || ir->evidence[call->as.call.evidence.offset].forwarded
                || call->as.call.type_arguments.count != 0) continue;
            for (size_t reference = 0; reference < program->reference_count;
                ++reference) {
                ++work;
                if (program->references[reference].kind
                    != SOL_MIR_PROGRAM_REFERENCE_INVOKE) continue;
                for (size_t file = 0; file < ir->file_count; ++file) {
                    ++work;
                    const SolIrSourceFile *item = &ir->files[file];
                    if (term->span.start >= item->aggregate_start
                        && term->span.end <= item->aggregate_end) break;
                }
            }
        }
    }
    return work;
}

static size_t capability_validation_work(const SolMirProgram *program) {
    const SolIr *ir = program->ir;
    size_t work = 0;
    for (size_t template_id = 0; template_id < program->template_count; ++template_id) {
        const SolMir *mir = &program->templates[template_id].mir;
        for (size_t block = 0; block < mir->block_count; ++block) {
            const SolMirTerminator *term = &mir->blocks[block].terminator;
            if (term->kind != SOL_MIR_TERM_INVOKE
                || term->as.invoke.kind != SOL_IR_CALL_CAPABILITY) continue;
            ++work;
            const SolIrExpression *call = term->as.invoke.source_expression
                    < ir->expression_count
                ? &ir->expressions[term->as.invoke.source_expression] : NULL;
            if (call == NULL || call->kind != SOL_IR_EXPR_CALL
                || call->as.call.callee >= ir->expression_count) continue;
            const SolIrExpression *callee = &ir->expressions[call->as.call.callee];
            for (size_t reference = 0; reference < program->reference_count;
                ++reference) {
                ++work;
                SolSpan span;
                if (program->references[reference].kind
                    == SOL_MIR_PROGRAM_REFERENCE_INVOKE) span = term->span;
                else if (program->references[reference].kind
                    == SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION) span = callee->span;
                else continue;
                for (size_t file = 0; file < ir->file_count; ++file) {
                    ++work;
                    const SolIrSourceFile *item = &ir->files[file];
                    if (span.start >= item->aggregate_start
                        && span.end <= item->aggregate_end) break;
                }
            }
        }
    }
    return work;
}

static void check_invalid_owner(SolMirProgram *program,
    SolDiagnostics *diagnostics) {
    size_t before = diagnostics->count;
    CHECK(!sol_mir_program_validate(program, diagnostics));
    CHECK(diagnostics->allocation_failed || diagnostics->count > before);
}

static bool compile_package_at(Compilation *compilation, SolPackage *package,
    const char *directory) {
    memset(compilation, 0, sizeof(*compilation));
    sol_package_init(package);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
    char error[256];
    if (!sol_package_load_directory(package, directory,
            &compilation->diagnostics, error, sizeof(error))) return false;
    SolHirFileScope *scopes = malloc(package->file_count * sizeof(*scopes));
    if (scopes == NULL) return false;
    for (size_t index = 0; index < package->file_count; ++index) {
        scopes[index] = (SolHirFileScope){
            package->files[index].module_name,
            package->files[index].import_start,
            package->files[index].import_count,
            package->files[index].item_start,
            package->files[index].item_count,
        };
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

static bool compile_e6(Compilation *compilation, SolPackage *package) {
    return compile_package_at(compilation, package,
        SOL_TEST_SOURCE_DIR "/tests/conformance/e6");
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

static void test_e6_closures_and_determinism(void) {
    Compilation compilation;
    SolPackage package;
    CHECK(compile_e6(&compilation, &package));
    const SolIr *ir = &compilation.ir;
    SolIrCallableId launch = callable(ir, "launch", SOL_IR_CALLABLE_FUNCTION);
    const char *import_names[] = {"write", "get", "count", "read"};
    SolIrCallableId approvals[4];
    for (size_t index = 0; index < 4; ++index) {
        approvals[index] = callable(ir, import_names[index],
            SOL_IR_CALLABLE_CAPABILITY);
        CHECK(approvals[index] != SOL_IR_NONE);
    }
    SolMirProgramRoot roots[5];
    roots[0] = (SolMirProgramRoot){launch, SOL_MIR_PROGRAM_ROOT_ENTRY};
    size_t root_count = 1;
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == SOL_IR_CALLABLE_TEST) {
            roots[root_count++] = (SolMirProgramRoot){id,
                SOL_MIR_PROGRAM_ROOT_TEST};
        }
    }
    CHECK(root_count == 5);

    SolMirProgram entry;
    sol_mir_program_init(&entry);
    CHECK(build(ir, roots, 1, approvals, 4, NULL, &entry)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(entry.usage.discovery_work == 1166
        && capability_validation_work(&entry) == 345);
    CHECK(entry.template_count == 8);
    CHECK(entry.import_count == 4);
    CHECK(entry.specialization_count == 1);
    size_t invokes = 0;
    size_t static_function_invokes = 0;
    size_t bound_operation_invokes = 0;
    size_t authenticated_bound_references = 0;
    for (size_t index = 0; index < entry.reference_count; ++index) {
        const SolMirProgramReference *reference = &entry.references[index];
        if (reference->kind != SOL_MIR_PROGRAM_REFERENCE_INVOKE) continue;
        ++invokes;
        const SolIrExpression *call
            = &ir->expressions[reference->source.expression];
        CHECK(call->kind == SOL_IR_EXPR_CALL);
        if (call->as.call.callee < ir->expression_count) {
            SolIrExpressionKind callee_kind
                = ir->expressions[call->as.call.callee].kind;
            static_function_invokes += callee_kind == SOL_IR_EXPR_DEFINITION;
            bound_operation_invokes += callee_kind
                == SOL_IR_EXPR_BOUND_OPERATION;
            if (call->as.call.kind == SOL_IR_CALL_CAPABILITY
                && callee_kind == SOL_IR_EXPR_BOUND_OPERATION) {
                size_t matches = 0;
                for (size_t other = 0; other < entry.reference_count; ++other) {
                    const SolMirProgramReference *bound = &entry.references[other];
                    matches += bound->kind == SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION
                        && bound->source.callable == reference->source.callable
                        && bound->source.expression == call->as.call.callee
                        && bound->target == call->as.call.callable;
                }
                CHECK(matches == 1); authenticated_bound_references += matches;
            }
        }
    }
    CHECK(invokes == 12);
    CHECK(static_function_invokes != 0);
    CHECK(bound_operation_invokes == 5);
    CHECK(authenticated_bound_references == 5 && entry.reference_count == 17);
    CHECK(entry.specializations[0].source_count != 0);
    CHECK(has_template(&entry, launch));
    for (size_t index = 0; index < 4; ++index) {
        CHECK(has_import(&entry, approvals[index]));
    }
    SolDiagnostics validation;
    sol_diagnostics_init(&validation);
    CHECK(sol_mir_program_validate(&entry, &validation));
    size_t bound_reference = SIZE_MAX;
    for (size_t i = 0; i < entry.reference_count; ++i)
        if (entry.references[i].kind == SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION) {
            bound_reference = i; break;
        }
    CHECK(bound_reference != SIZE_MAX);
    if (bound_reference != SIZE_MAX) {
        SolMirProgramReference saved = entry.references[bound_reference];
        entry.references[bound_reference].target = SOL_IR_NONE;
        CHECK(!sol_mir_program_validate(&entry, NULL));
        entry.references[bound_reference] = saved;
        entry.references[bound_reference].source.expression = SOL_IR_NONE;
        CHECK(!sol_mir_program_validate(&entry, NULL));
        entry.references[bound_reference] = saved;
        entry.references[bound_reference].source.file = SIZE_MAX;
        CHECK(!sol_mir_program_validate(&entry, NULL));
        entry.references[bound_reference] = saved;
        ++entry.references[bound_reference].source.start;
        CHECK(!sol_mir_program_validate(&entry, NULL));
        entry.references[bound_reference] = saved;
        SolMirProgramReference displaced = entry.references[0];
        entry.references[0] = saved;
        CHECK(!sol_mir_program_validate(&entry, NULL));
        entry.references[0] = displaced;
        CHECK(sol_mir_program_validate(&entry, NULL));
    }
    SolMirProgramRootKind root_kind = entry.roots[0].kind;
    entry.roots[0].kind = SOL_MIR_PROGRAM_ROOT_TEST;
    CHECK(!sol_mir_program_validate(&entry, &validation));
    entry.roots[0].kind = root_kind;
    SolIrCallableId approved_first = entry.approved_imports[0];
    entry.approved_imports[0] = entry.approved_imports[1];
    CHECK(!sol_mir_program_validate(&entry, &validation));
    entry.approved_imports[0] = approved_first;
    SolIrCallableId imported = entry.imports[0].callable;
    entry.imports[0].callable = SOL_IR_NONE;
    CHECK(!sol_mir_program_validate(&entry, &validation));
    entry.imports[0].callable = imported;
    SolIrCallableId method = entry.specializations[0].method;
    entry.specializations[0].method = SOL_IR_NONE;
    CHECK(!sol_mir_program_validate(&entry, &validation));
    entry.specializations[0].method = method;
    SolIrExpressionId expression = entry.references[0].source.expression;
    entry.references[0].source.expression = SOL_IR_NONE;
    CHECK(!sol_mir_program_validate(&entry, &validation));
    entry.references[0].source.expression = expression;
    size_t source_file = entry.references[0].source.file;
    size_t diagnostic_count = validation.count;
    entry.references[0].source.file = SIZE_MAX;
    CHECK(!sol_mir_program_validate(&entry, &validation));
    CHECK(validation.allocation_failed || validation.count > diagnostic_count);
    entry.references[0].source.file = source_file;
    SolMirProgramReference first_reference = entry.references[0];
    entry.references[0] = entry.references[1];
    entry.references[1] = first_reference;
    CHECK(!sol_mir_program_validate(&entry, &validation));
    entry.references[1] = entry.references[0];
    entry.references[0] = first_reference;
    CHECK(sol_mir_program_validate(&entry, &validation));
    CHECK(sol_mir_program_validate(&entry, NULL));
    sol_diagnostics_free(&validation);

    SolMirProgram all;
    SolMirProgram permuted;
    sol_mir_program_init(&all);
    sol_mir_program_init(&permuted);
    CHECK(build(ir, roots, root_count, approvals, 4, NULL, &all)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    SolMirProgramRoot reverse_roots[5];
    SolIrCallableId reverse_approvals[4];
    for (size_t index = 0; index < root_count; ++index) {
        reverse_roots[index] = roots[root_count - index - 1];
    }
    for (size_t index = 0; index < 4; ++index) {
        reverse_approvals[index] = approvals[3 - index];
    }
    CHECK(build(ir, reverse_roots, root_count, reverse_approvals, 4,
        NULL, &permuted) == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(all.template_count == 14);
    CHECK(all.import_count == 4);
    CHECK(all.specialization_count == 1);
    invokes = 0;
    for (size_t index = 0; index < all.reference_count; ++index) {
        invokes += all.references[index].kind
            == SOL_MIR_PROGRAM_REFERENCE_INVOKE;
    }
    CHECK(invokes == 18);
    CHECK(all.reference_count == 23);
    CHECK(program_fields_equal(&all, &permuted));
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].body != SOL_IR_NONE) CHECK(has_template(&all, id));
    }

    SolMirProgramLimits exact = all.usage.callable_classifications == 0
        ? sol_mir_program_default_limits()
        : (SolMirProgramLimits){all.usage.callable_classifications,
            all.usage.references, all.usage.discovery_work};
    SolMirProgram limited;
    sol_mir_program_init(&limited);
    CHECK(build(ir, roots, root_count, approvals, 4, &exact, &limited)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    sol_mir_program_free(&limited);
    SolMirProgramLimits less = exact;
    --less.max_callable_classifications;
    CHECK(build(ir, roots, root_count, approvals, 4, &less, &limited)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED);
    CHECK(limited.ir == NULL && limited.templates == NULL);
    less = exact;
    --less.max_references;
    CHECK(build(ir, roots, root_count, approvals, 4, &less, &limited)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED);
    less = exact;
    --less.max_discovery_work;
    CHECK(build(ir, roots, root_count, approvals, 4, &less, &limited)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED);

    sol_mir_program_free(&entry);
    sol_mir_program_free(&all);
    sol_mir_program_free(&permuted);
    sol_mir_program_free(&limited);
    sol_mir_program_free(&limited);
    free_e6(&compilation, &package);
}

static void test_recursion_references_and_unreachable(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module closure\n"
        "function absent() -> Int64 { return 99 }\n"
        "function leaf() -> Int64 { return 1 }\n"
        "function direct(value: Int64) -> Int64 { if value > 0 { "
        "return direct(value - 1) } else { return 0 } }\n"
        "function left(flag: Bool) -> Int64 { if flag { return right(false) } "
        "else { return leaf() + leaf() } }\n"
        "function right(flag: Bool) -> Int64 { if flag { return left(false) } "
        "else { return 0 } }\n"));
    SolIrCallableId left = callable(&compilation.ir, "left",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId leaf = callable(&compilation.ir, "leaf",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId absent = callable(&compilation.ir, "absent",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirProgramRoot root = {left, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program;
    sol_mir_program_init(&program);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.template_count == 3);
    CHECK(has_template(&program, leaf));
    CHECK(!has_template(&program, absent));
    size_t leaf_references = 0;
    for (size_t index = 0; index < program.reference_count; ++index) {
        leaf_references += program.references[index].target == leaf;
    }
    CHECK(leaf_references == 2);
    sol_mir_program_free(&program);
    root.callable = callable(&compilation.ir, "direct",
        SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.template_count == 1);
    CHECK(program.reference_count == 1);
    sol_mir_program_free(&program);
    SolMirProgramLimits limits = sol_mir_program_default_limits();
    limits.max_callable_classifications = 1;
    limits.max_references = 1;
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, &limits, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    sol_mir_program_free(&program);
    root.callable = left;
    limits = sol_mir_program_default_limits();
    limits.max_callable_classifications = 2;
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, &limits, &program)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED);
    limits = sol_mir_program_default_limits();
    limits.max_references = 1;
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, &limits, &program)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED);
    limits = sol_mir_program_default_limits();
    limits.max_discovery_work = 1;
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, &limits, &program)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED);
    free_compilation(&compilation);
}

static void test_evidence_and_callbacks(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module evidence\n"
        "trait Value { function value(self: Self) -> Int64 effects { pure } }\n"
        "implementation Value for Int64 { function value(self: Self) -> Int64 "
        "effects { pure } { return self } }\n"
        "implementation Value for Bool { function value(self: Self) -> Int64 "
        "effects { pure } { if self { return 1 } else { return 0 } } }\n"
        "function generic<T: Value>(item: T) -> Int64 effects { pure } "
        "{ return item.value() }\n"
        "function unbounded<T>(item: T) -> T effects { pure } { return item }\n"
        "function concrete() -> Int64 effects { pure } { return generic(1) }\n"
        "function inner<U: Value>(item: U) -> Int64 effects { pure } "
        "{ return item.value() }\n"
        "function outer<T: Value>(item: T) -> Int64 effects { pure } "
        "{ return inner(item) }\n"
        "function int_nested() -> Int64 effects { pure } { return outer(1) }\n"
        "function bool_nested() -> Int64 effects { pure } { return outer(true) }\n"
        "function plain(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function apply(callback: function(Int64) -> Int64 effects { pure }) "
        "-> Int64 effects { pure } { return callback(1) }\n"
        "function callback_root() -> Int64 effects { pure } { return apply(plain) }\n"));
    SolMirProgram program;
    sol_mir_program_init(&program);
    SolMirProgramRoot root = {callable(&compilation.ir, "concrete",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.specialization_count == 1);
    CHECK(program.template_count == 3);
    sol_mir_program_free(&program);

    SolIrCallableId generic = callable(&compilation.ir, "generic",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirProgramRoot evidence_roots[] = {
        {generic, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
        {root.callable, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
    };
    SolMirProgram ordered;
    sol_mir_program_init(&ordered);
    CHECK(build(&compilation.ir, evidence_roots, 2, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    SolMirProgramRoot swapped[] = {evidence_roots[1], evidence_roots[0]};
    CHECK(build(&compilation.ir, swapped, 2, NULL, 0, NULL, &ordered)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    sol_mir_program_free(&program);
    sol_mir_program_free(&ordered);

    root.callable = callable(&compilation.ir, "unbounded",
        SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.template_count == 1);
    CHECK(sol_mir_program_validate(&program, NULL));
    sol_mir_program_free(&program);

    SolMirProgramRoot nested_roots[] = {
        {callable(&compilation.ir, "int_nested", SOL_IR_CALLABLE_FUNCTION),
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
        {callable(&compilation.ir, "bool_nested", SOL_IR_CALLABLE_FUNCTION),
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
    };
    CHECK(build(&compilation.ir, nested_roots, 2, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.specialization_count == 2);
    CHECK(program.specializations[0].type != program.specializations[1].type);
    CHECK(program.specializations[0].method != program.specializations[1].method);
    CHECK(program.specializations[0].source_count >= 3);
    CHECK(program.specializations[1].source_count >= 3);
    CHECK(has_template(&program, callable(&compilation.ir, "outer",
        SOL_IR_CALLABLE_FUNCTION)));
    CHECK(has_template(&program, callable(&compilation.ir, "inner",
        SOL_IR_CALLABLE_FUNCTION)));
    CHECK(has_template(&program, program.specializations[0].method));
    CHECK(has_template(&program, program.specializations[1].method));
    CHECK(sol_mir_program_validate(&program, NULL));
    sol_mir_program_free(&program);

    root.callable = callable(&compilation.ir, "outer",
        SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    SolMirProgramRoot unrelated_roots[] = {
        root,
        {callable(&compilation.ir, "concrete", SOL_IR_CALLABLE_FUNCTION),
            SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE},
    };
    CHECK(build(&compilation.ir, unrelated_roots, 2, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    SolMirProgramRoot leaking_roots[] = {root, nested_roots[0]};
    CHECK(build(&compilation.ir, leaking_roots, 2, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);

    root.callable = generic;
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    CHECK(program.ir == NULL);

    root.callable = callable(&compilation.ir, "callback_root",
        SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    root.callable = callable(&compilation.ir, "apply", SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    sol_mir_program_free(&program);
    free_compilation(&compilation);

    /* Method evidence is an immediate-call proof, never a callable value. */
    Compilation method_value;
    CHECK(!compile_text(&method_value,
        "module method_value\n"
        "trait Value { function value(self: borrow Self) -> Int64 effects { pure } }\n"
        "implementation Value for Int64 { function value(self: borrow Self) -> Int64 "
        "effects { pure } { return self } }\n"
        "function rejected() -> Int64 effects { pure } { let value = 1 "
        "let method = value.value return method() }\n"));
    free_compilation(&method_value);
}

static void test_callback_source_limitations(void) {
    Compilation compilation;
    CHECK(!compile_text(&compilation,
        "module conditional_callback\n"
        "function plain(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function rejected(flag: Bool, callback: function(Int64) -> Int64 "
        "effects { pure }) -> Int64 effects { pure } { "
        "let selected = if flag { plain } else { callback } "
        "return selected(1) }\n"));
    free_compilation(&compilation);
    CHECK(compile_text(&compilation,
        "module mixed_callbacks\n"
        "function plain(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function rejected(callback: function(Int64) -> Int64 effects { pure }) "
        "-> Int64 effects { pure } { let known = plain "
        "let first = known(1) return callback(first) }\n"));
    SolMirProgramRoot root = {callable(&compilation.ir, "rejected",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program; sol_mir_program_init(&program);
    CHECK(root.callable != SOL_IR_NONE && build(&compilation.ir, &root, 1,
        NULL, 0, NULL, &program) == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    sol_mir_program_free(&program);
    free_compilation(&compilation);
    CHECK(!compile_text(&compilation,
        "module callback_parameter_mismatch\n"
        "function plain(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function bad() -> function(Text) -> Int64 effects { pure } effects { pure } { "
        "return plain }\n"));
    free_compilation(&compilation);
    CHECK(!compile_text(&compilation,
        "module callback_access_mismatch\n"
        "function plain(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function bad() -> function(borrow Int64) -> Int64 effects { pure } effects { pure } { "
        "return plain }\n"));
    free_compilation(&compilation);
    CHECK(!compile_text(&compilation,
        "module callback_result_mismatch\n"
        "function plain(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function bad() -> function(Int64) -> Bool effects { pure } effects { pure } { "
        "return plain }\n"));
    free_compilation(&compilation);
}

static void test_callback_producer_discovery_limits(void) {
    static const char source[] =
        "module callback_producer_limits\n"
        "record Wide { left: function() -> Int64 effects { pure }, a: Text, b: Text, "
        "c: Text, d: Text, e: Text }\n"
        "function answer() -> Int64 effects { pure } { return 42 }\n"
        "function root() -> Int64 effects { pure } { let f0 = answer let f1 = f0 "
        "let f2 = f1 let f3 = f2 let f4 = f3 let f5 = f4 let f6 = f5 let f7 = f6 "
        "let f8 = f7 let f9 = f8 let wide = Wide { left = f9, a = \"a\", b = \"b\", "
        "c = \"c\", d = \"d\", e = \"e\" } let moved = wide.left return moved() }\n";
    Compilation compilation; CHECK(compile_text(&compilation, source));
    SolMirProgramRoot root = {callable(&compilation.ir, "root",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program, exact, limited;
    sol_mir_program_init(&program); sol_mir_program_init(&exact); sol_mir_program_init(&limited);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    if (program.ir != NULL) {
        SolMirProgramLimits limits = sol_mir_program_default_limits();
        limits.max_discovery_work = program.usage.discovery_work;
        CHECK(build(&compilation.ir, &root, 1, NULL, 0, &limits, &exact)
            == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
            && exact.usage.discovery_work == limits.max_discovery_work);
        CHECK(limits.max_discovery_work != 0);
        --limits.max_discovery_work;
        CHECK(build(&compilation.ir, &root, 1, NULL, 0, &limits, &limited)
            == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED && limited.ir == NULL);

        SolIrLocalId f0 = SOL_IR_NONE, f9 = SOL_IR_NONE, wide = SOL_IR_NONE;
        for (size_t i = 0; i < compilation.ir.local_count; ++i) {
            if (strcmp(compilation.ir.locals[i].name, "f0") == 0) f0 = i;
            if (strcmp(compilation.ir.locals[i].name, "f9") == 0) f9 = i;
            if (strcmp(compilation.ir.locals[i].name, "wide") == 0) wide = i;
        }
        size_t f0_statement = SOL_IR_NONE, f9_place = SOL_IR_NONE;
        for (size_t i = 0; i < compilation.ir.statement_count; ++i)
            if (compilation.ir.statements[i].kind == SOL_IR_STATEMENT_LET
                && compilation.ir.statements[i].local == f0) f0_statement = i;
        for (size_t i = 0; i < compilation.ir.expression_count; ++i) {
            const SolIrExpression *expression = &compilation.ir.expressions[i];
            if (expression->kind != SOL_IR_EXPR_PLACE
                || expression->as.place >= compilation.ir.place_count) continue;
            const SolIrPlace *place = &compilation.ir.places[expression->as.place];
            if (place->root_kind == SOL_IR_PLACE_ROOT_LOCAL && place->local == f9
                && place->projections.count == 0) f9_place = i;
        }
        CHECK(f0 != SOL_IR_NONE && f9 != SOL_IR_NONE && wide != SOL_IR_NONE
            && f0_statement != SOL_IR_NONE && f9_place != SOL_IR_NONE);
        if (f0_statement != SOL_IR_NONE && f9_place != SOL_IR_NONE) {
            SolMirProgram rejected; sol_mir_program_init(&rejected);
            SolIrExpressionId saved = compilation.ir.statements[f0_statement].expression;
            compilation.ir.statements[f0_statement].expression = f9_place;
            CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &rejected)
                != SOL_MIR_PROGRAM_BUILD_SUCCEEDED && rejected.ir == NULL);
            compilation.ir.statements[f0_statement].expression = saved;
            sol_mir_program_free(&rejected);
            sol_mir_program_init(&rejected);
            SolIrLocalId saved_local = compilation.ir.statements[f0_statement].local;
            compilation.ir.statements[f0_statement].local = wide;
            CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &rejected)
                != SOL_MIR_PROGRAM_BUILD_SUCCEEDED && rejected.ir == NULL);
            compilation.ir.statements[f0_statement].local = saved_local;
            sol_mir_program_free(&rejected);
        }
    }
    sol_mir_program_free(&limited); sol_mir_program_free(&exact); sol_mir_program_free(&program);
    free_compilation(&compilation);
}

static void test_import_policy_and_lifecycle(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module imports\n"
        "type Callback = distinct function() -> Int64 effects { pure }\n"
        "record Deep1 { value: Callback }\n"
        "record Deep2 { value: Deep1 }\n"
        "record Deep3 { value: Deep2 }\n"
        "record Deep4 { value: Deep3 }\n"
        "record Deep5 { value: Deep4 }\n"
        "record Deep6 { value: Deep5 }\n"
        "record Deep7 { value: Deep6 }\n"
        "record Deep8 { value: Deep7 }\n"
        "capability Host { "
        "function good(value: Text) -> Option<Text> effects { host.good<Self> } "
        "function exclusive(value: inout Int64) -> () effects { host.bad<Self> } "
        "function callback() -> Callback effects { host.callback<Self> } "
        "function deep_callback() -> Deep8 effects { host.deep<Self> } "
        "function extra() -> Int64 effects { pure } }\n"
        "capability Clock { function now() -> Int64 "
        "effects { clock.read<Self> } }\n"
        "capability Wrapped derives_from private_source: capability Clock { "
        "function now() -> Int64 effects { clock.read<Self> } "
        "{ return private_source.now() } }\n"
        "function use_wrapped(value: capability Wrapped) -> Int64 "
        "effects { clock.read<value> } { return value.now() }\n"
        "function consume(host: capability Host) -> Option<Text> "
        "effects { host.good<host> } { return host.good(\"x\") }\n"));
    const SolIr *ir = &compilation.ir;
    SolIrCallableId good = callable(ir, "good", SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root = {callable(ir, "consume", SOL_IR_CALLABLE_FUNCTION),
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program;
    sol_mir_program_init(&program);
    CHECK(build(ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    CHECK(program.ir == NULL);
    CHECK(build(ir, &root, 1, &good, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.import_count == 1 && program.imports[0].callable == good);

    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolMirProgramBuildRequest request = {ir, &root, 1, &good, 1, NULL};
    CHECK(sol_mir_program_build(&request, &program, &diagnostics)
        == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
    sol_diagnostics_free(&diagnostics);
    sol_mir_program_free(&program);

    SolIrCallableId extra = callable(ir, "extra", SOL_IR_CALLABLE_CAPABILITY);
    SolIrCallableId extra_approvals[] = {extra, good, extra};
    CHECK(build(ir, &root, 1, extra_approvals, 3, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.approved_import_count == 2);
    CHECK(program.import_count == 1 && program.imports[0].callable == good);
    CHECK(sol_mir_program_validate(&program, NULL));
    SolMirProgramUsage duplicate_approval_usage = program.usage;
    sol_mir_program_free(&program);
    SolIrCallableId unique_extra_approvals[] = {good, extra};
    CHECK(build(ir, &root, 1, unique_extra_approvals, 2, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(memcmp(&program.usage, &duplicate_approval_usage,
        sizeof(program.usage)) == 0);
    CHECK(sol_mir_program_validate(&program, NULL));
    sol_mir_program_free(&program);
    SolIrCallableId permuted_duplicate_approvals[] = {good, extra, good};
    CHECK(build(ir, &root, 1, permuted_duplicate_approvals, 3, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(memcmp(&program.usage, &duplicate_approval_usage,
        sizeof(program.usage)) == 0);
    CHECK(sol_mir_program_validate(&program, NULL));
    sol_mir_program_free(&program);

    const char *invalid_names[] = {"exclusive", "callback", "deep_callback"};
    for (size_t index = 0; index < 3; ++index) {
        SolIrCallableId invalid = callable(ir, invalid_names[index],
            SOL_IR_CALLABLE_CAPABILITY);
        CHECK(build(ir, &root, 1, &invalid, 1, NULL, &program)
            == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
        CHECK(program.ir == NULL);
    }
    SolIrCallableId wrapped_now = SOL_IR_NONE;
    for (SolIrCallableId id = 0; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == SOL_IR_CALLABLE_CAPABILITY
            && ir->callables[id].body != SOL_IR_NONE
            && strcmp(ir->callables[id].name, "now") == 0) wrapped_now = id;
    }
    CHECK(wrapped_now != SOL_IR_NONE);
    CHECK(build(ir, &root, 1, &wrapped_now, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
    SolMirProgramRoot wrapped_root = {callable(ir, "use_wrapped",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    CHECK(build(ir, &wrapped_root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    SolMirProgramLimits zero = {0};
    CHECK(build(ir, &root, 1, &good, 1, &zero, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    sol_mir_program_free(&program);
    SolMirProgramRoot duplicate_roots[] = {root, root};
    CHECK(build(ir, duplicate_roots, 2, &good, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.root_count == 1);
    CHECK(sol_mir_program_validate(&program, NULL));
    SolMirProgramUsage duplicate_root_usage = program.usage;
    sol_mir_program_free(&program);
    CHECK(build(ir, &root, 1, &good, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(memcmp(&program.usage, &duplicate_root_usage,
        sizeof(program.usage)) == 0);
    CHECK(sol_mir_program_validate(&program, NULL));
    sol_mir_program_free(&program);
    duplicate_roots[1].kind = SOL_MIR_PROGRAM_ROOT_TEST;
    CHECK(build(ir, duplicate_roots, 2, &good, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
    SolMirProgramRoot bodyless = {good,
        SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    CHECK(build(ir, &bodyless, 1, &good, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
    SolMirProgramLimits partial = {1, 0, 1};
    CHECK(build(ir, &root, 1, &good, 1, &partial, &program)
        == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
    CHECK(build(ir, &root, SIZE_MAX, &good, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT);
    free_compilation(&compilation);
}

static void test_predicate_proof_and_handler_relations(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module provenance\n"
        "function predicate_dependency(value: Int64) -> Bool effects { pure } "
        "{ return value > 0 }\n"
        "function proof_dependency(value: Int64) -> Bool effects { pure } "
        "{ return value >= 0 }\n"
        "type Positive = refined Int64 where predicate_dependency(self)\n"
        "function dynamic_contract(callback: function(Int64) -> Bool "
        "effects { pure }) -> Int64 effects { pure } "
        "requires { callback(1) } { return 1 }\n"
        "function checked() -> Positive effects { pure } { "
        "while false invariant { proof_dependency(1) } decreases { 1 } {} "
        "return Positive(1) }\n"
        "capability Source { function read(value: Int64) -> Positive "
        "effects { service.read<Self> } }\n"
        "capability Provider { function read(value: Int64) -> Positive effects { pure } "
        "requires { Positive(value) == Positive(value) } "
        "ensures { result == Positive(old(value)) } { return Positive(value) } }\n"
        "function direct(provider: capability Provider) -> Positive "
        "{ return provider.read(1) }\n"
        "function handled(source: capability Source, provider: capability Provider) "
        "-> Positive { handle service.read<source> with provider { source.read(1) } }\n"));
    const SolIr *ir = &compilation.ir;
    SolIrCallableId checked = callable(ir, "checked", SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId predicate = callable(ir, "predicate_dependency",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId proof = callable(ir, "proof_dependency",
        SOL_IR_CALLABLE_FUNCTION);
    SolMirProgramRoot root = {checked, SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program;
    sol_mir_program_init(&program);
    CHECK(build(ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(has_template(&program, predicate));
    CHECK(!has_template(&program, proof));
    size_t predicate_references = 0;
    for (size_t index = 0; index < program.reference_count; ++index) {
        predicate_references += program.references[index].kind
            == SOL_MIR_PROGRAM_REFERENCE_PREDICATE_CALL
            && program.references[index].target == predicate;
        CHECK(program.references[index].target != proof);
    }
    CHECK(predicate_references == 1);
    sol_mir_program_free(&program);

    root.callable = callable(ir, "dynamic_contract", SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    CHECK(program.ir == NULL);

    SolIrCallableId source = callable(ir, "read", SOL_IR_CALLABLE_CAPABILITY);
    CHECK(ir->callables[source].body == SOL_IR_NONE);
    CHECK(ir->callables[source].receiver == SOL_IR_NONE);
    CHECK(ir->callables[source].receiver_access == SOL_ACCESS_OWNED);
    SolIrCallableId provider = SOL_IR_NONE;
    for (SolIrCallableId id = source + 1; id < ir->callable_count; ++id) {
        if (ir->callables[id].kind == SOL_IR_CALLABLE_CAPABILITY
            && strcmp(ir->callables[id].name, "read") == 0) {
            provider = id;
            break;
        }
    }
    root.callable = callable(ir, "direct", SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(has_template(&program, provider));
    CHECK(has_template(&program, predicate));
    CHECK(!has_import(&program, provider));
    sol_mir_program_free(&program);

    SolIrCallable *provider_metadata = &compilation.ir.callables[provider];
    SolIrLocalId saved_receiver = provider_metadata->receiver;
    provider_metadata->receiver = SOL_IR_NONE;
    CHECK(!sol_ir_validate(&compilation.ir, NULL));
    provider_metadata->receiver = saved_receiver;
    SolIrExpressionId saved_body = provider_metadata->body;
    provider_metadata->body = SOL_IR_NONE;
    CHECK(!sol_ir_validate(&compilation.ir, NULL));
    provider_metadata->body = saved_body;
    CHECK(sol_ir_validate(&compilation.ir, NULL));

    SolIrCallableId approvals[] = {source};
    root.callable = callable(ir, "handled", SOL_IR_CALLABLE_FUNCTION);
    CHECK(build(ir, &root, 1, approvals, 1, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    CHECK(program.import_count == 1);
    CHECK(has_template(&program, provider));
    CHECK(has_template(&program, predicate));
    CHECK(!has_import(&program, provider));
    size_t handler_sources = 0;
    size_t handler_providers = 0;
    for (size_t index = 0; index < program.reference_count; ++index) {
        handler_sources += program.references[index].kind
            == SOL_MIR_PROGRAM_REFERENCE_HANDLER_SOURCE;
        handler_providers += program.references[index].kind
            == SOL_MIR_PROGRAM_REFERENCE_HANDLER_PROVIDER;
    }
    CHECK(handler_sources == 1);
    CHECK(handler_providers == 1);
    sol_mir_program_free(&program);
    free_compilation(&compilation);
}

static void test_derived_bodyful_capability_is_transactionally_unsupported(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module derived_bodyful\n"
        "capability Source { function read() -> Int64 effects { source.read<Self> } }\n"
        "capability Provider derives_from private_source: capability Source { "
        "function read() -> Int64 effects { pure } { return 1 } }\n"
        "function direct(provider: capability Provider) -> Int64 "
        "{ return provider.read() }\n"));
    SolMirProgram program;
    sol_mir_program_init(&program);
    SolMirProgramRoot root = {callable(&compilation.ir, "direct",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE);
    CHECK(program.ir == NULL && program.templates == NULL && program.imports == NULL);
    free_compilation(&compilation);
}

static void test_source_backed_method_meter(void) {
    Compilation compilation;
    SolPackage package;
    CHECK(compile_package_at(&compilation, &package,
        SOL_TEST_SOURCE_DIR "/tests/conformance/p43_method_meter"));
    SolMirProgramRoot root = {callable(&compilation.ir, "launch",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_ENTRY};
    SolMirProgram program;
    sol_mir_program_init(&program);
    CHECK(root.callable != SOL_IR_NONE && build(&compilation.ir, &root, 1,
        NULL, 0, NULL, &program) == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    size_t direct_methods = 0;
    for (size_t index = 0; index < program.reference_count; ++index) {
        const SolMirProgramReference *reference = &program.references[index];
        if (reference->kind != SOL_MIR_PROGRAM_REFERENCE_INVOKE
            || reference->source.expression >= compilation.ir.expression_count) continue;
        const SolIrExpression *source
            = &compilation.ir.expressions[reference->source.expression];
        if (source->kind == SOL_IR_EXPR_CALL
            && source->as.call.kind == SOL_IR_CALL_METHOD) {
            ++direct_methods;
            CHECK(reference->source.file < compilation.ir.file_count
                && reference->source.start < reference->source.end);
        }
    }
    CHECK(direct_methods == 4 && direct_method_validation_work(&program) == 76
        && program.usage.discovery_work == 203
        && sol_mir_program_validate(&program, NULL));
    SolMirProgramLimits exact = {program.usage.callable_classifications,
        program.usage.references, program.usage.discovery_work};
    SolMirProgram repeated;
    sol_mir_program_init(&repeated);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, &exact, &repeated)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && repeated.usage.discovery_work == exact.max_discovery_work
        && sol_mir_program_validate(&repeated, NULL));
    sol_mir_program_free(&repeated);
    SolMirProgramLimits less = exact;
    --less.max_discovery_work;
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, &less, &repeated)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED
        && repeated.ir == NULL && repeated.references == NULL
        && repeated.usage.discovery_work == 0);
    sol_mir_program_free(&repeated);
    sol_mir_program_free(&program);
    free_e6(&compilation, &package);
}

static void test_source_backed_capability_meter(void) {
    Compilation compilation;
    SolPackage package;
    CHECK(compile_package_at(&compilation, &package,
        SOL_TEST_SOURCE_DIR "/tests/conformance/p43_capability_meter"));
    SolIrCallableId launch = callable(&compilation.ir, "launch",
        SOL_IR_CALLABLE_FUNCTION);
    SolIrCallableId tick = callable(&compilation.ir, "tick",
        SOL_IR_CALLABLE_CAPABILITY);
    SolMirProgramRoot root = {launch, SOL_MIR_PROGRAM_ROOT_ENTRY};
    SolMirProgram program;
    sol_mir_program_init(&program);
    CHECK(launch != SOL_IR_NONE && tick != SOL_IR_NONE
        && build(&compilation.ir, &root, 1, &tick, 1, NULL, &program)
            == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    size_t invokes = 0, bound = 0;
    for (size_t index = 0; index < program.reference_count; ++index) {
        invokes += program.references[index].kind == SOL_MIR_PROGRAM_REFERENCE_INVOKE;
        bound += program.references[index].kind
            == SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION;
    }
    CHECK(invokes == 3 && bound == 3 && program.import_count == 1
        && capability_validation_work(&program) == 57
        && program.usage.discovery_work == 133
        && sol_mir_program_validate(&program, NULL));
    SolMirProgramLimits exact = {program.usage.callable_classifications,
        program.usage.references, program.usage.discovery_work};
    SolMirProgram repeated;
    sol_mir_program_init(&repeated);
    CHECK(build(&compilation.ir, &root, 1, &tick, 1, &exact, &repeated)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED
        && repeated.usage.discovery_work == exact.max_discovery_work
        && sol_mir_program_validate(&repeated, NULL));
    sol_mir_program_free(&repeated);
    SolMirProgramLimits less = exact;
    --less.max_discovery_work;
    CHECK(build(&compilation.ir, &root, 1, &tick, 1, &less, &repeated)
        == SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED
        && repeated.ir == NULL && repeated.references == NULL
        && repeated.usage.discovery_work == 0);
    sol_mir_program_free(&repeated);
    sol_mir_program_free(&program);
    free_e6(&compilation, &package);
}

static void test_malformed_owner(void) {
    Compilation compilation;
    CHECK(compile_text(&compilation,
        "module malformed\nfunction leaf() -> Int64 { return 1 }\n"
        "function root() -> Int64 { return leaf() }\n"));
    SolMirProgramRoot root = {callable(&compilation.ir, "root",
        SOL_IR_CALLABLE_FUNCTION), SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE};
    SolMirProgram program;
    sol_mir_program_init(&program);
    CHECK(build(&compilation.ir, &root, 1, NULL, 0, NULL, &program)
        == SOL_MIR_PROGRAM_BUILD_SUCCEEDED);
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    size_t count = program.template_count;
    program.template_count = compilation.ir.callable_count + 1;
    check_invalid_owner(&program, &diagnostics);
    program.template_count = count;
    SolMirProgramRoot *roots = program.roots;
    program.roots = NULL;
    check_invalid_owner(&program, &diagnostics);
    program.roots = roots;
    SolMirProgramTemplate *templates = program.templates;
    program.templates = NULL;
    check_invalid_owner(&program, &diagnostics);
    program.templates = templates;
    SolMirProgramSpecialization *specializations = program.specializations;
    CHECK(program.specialization_count == 0 && specializations == NULL);
    program.specializations = (SolMirProgramSpecialization *)program.roots;
    check_invalid_owner(&program, &diagnostics);
    program.specializations = specializations;

    roots = program.roots;
    program.roots = (SolMirProgramRoot *)program.templates;
    check_invalid_owner(&program, &diagnostics);
    program.roots = roots;
    SolMirProgramReference *references = program.references;
    program.references = (SolMirProgramReference *)(void *)(
        (unsigned char *)program.templates + 1);
    check_invalid_owner(&program, &diagnostics);
    program.references = references;

    if (program.template_count > 1) {
        SolIrCallableId second = program.templates[1].callable;
        program.templates[1].callable = program.templates[0].callable;
        check_invalid_owner(&program, &diagnostics);
        program.templates[1].callable = second;

        SolMir *first_mir = &program.templates[0].mir;
        SolMir *second_mir = &program.templates[1].mir;
        SolMirBlock *second_blocks = second_mir->blocks;
        size_t second_block_count = second_mir->block_count;
        size_t second_block_capacity = second_mir->block_capacity;
        second_mir->blocks = first_mir->blocks;
        second_mir->block_count = first_mir->block_count;
        second_mir->block_capacity = first_mir->block_capacity;
        check_invalid_owner(&program, &diagnostics);
        second_mir->blocks = second_blocks;
        second_mir->block_count = second_block_count;
        second_mir->block_capacity = second_block_capacity;
    }
    SolIrCallableId target = program.references[0].target;
    program.references[0].target = SOL_IR_NONE;
    check_invalid_owner(&program, &diagnostics);
    program.references[0].target = target;
    SolIrCallableId callable_id = program.templates[0].mir.callable;
    program.templates[0].mir.callable = SOL_IR_NONE;
    check_invalid_owner(&program, &diagnostics);
    program.templates[0].mir.callable = callable_id;

    SolMir *mir = &program.templates[0].mir;
    SolMirInstruction *instructions = mir->instructions;
    size_t instruction_count = mir->instruction_count;
    size_t instruction_capacity = mir->instruction_capacity;
    mir->instructions = (SolMirInstruction *)(void *)mir->blocks;
    mir->instruction_count = 1;
    mir->instruction_capacity = 1;
    check_invalid_owner(&program, &diagnostics);
    mir->instructions = instructions;
    mir->instruction_count = instruction_count;
    mir->instruction_capacity = instruction_capacity;

    SolMirValueId *edge_values = mir->edge_values;
    size_t edge_value_count = mir->edge_value_count;
    size_t edge_value_capacity = mir->edge_value_capacity;
    mir->edge_values = (SolMirValueId *)(void *)((unsigned char *)mir->values + 1);
    mir->edge_value_count = 1;
    mir->edge_value_capacity = 1;
    check_invalid_owner(&program, &diagnostics);
    mir->edge_values = edge_values;
    mir->edge_value_count = edge_value_count;
    mir->edge_value_capacity = edge_value_capacity;

    SolMirLoop *loops = mir->loops;
    size_t loop_count = mir->loop_count;
    size_t loop_capacity = mir->loop_capacity;
    CHECK(loop_count == 0 && loop_capacity == 0 && loops == NULL);
    mir->loops = (SolMirLoop *)(void *)program.roots;
    check_invalid_owner(&program, &diagnostics);
    mir->loops = loops;
    mir->loops = (SolMirLoop *)(void *)program.templates;
    mir->loop_count = 1;
    mir->loop_capacity = 1;
    check_invalid_owner(&program, &diagnostics);
    mir->loops = loops;
    mir->loop_count = loop_count;
    mir->loop_capacity = loop_capacity;

    size_t block_capacity = mir->block_capacity;
    mir->block_capacity = SIZE_MAX;
    check_invalid_owner(&program, &diagnostics);
    mir->block_capacity = block_capacity;
    CHECK(sol_mir_program_validate(&program, &diagnostics));
    sol_diagnostics_free(&diagnostics);
    sol_mir_program_free(&program);

    CHECK(compilation.ir.file_count == 1);
    size_t file_start = compilation.ir.files[0].aggregate_start;
    size_t file_end = compilation.ir.files[0].aggregate_end;
    compilation.ir.files[0].aggregate_start = compilation.ir.source_length;
    compilation.ir.files[0].aggregate_end = compilation.ir.source_length;
    CHECK(sol_ir_validate(&compilation.ir, NULL));
    sol_diagnostics_init(&diagnostics);
    SolMirProgramBuildRequest request = {
        &compilation.ir, &root, 1, NULL, 0, NULL,
    };
    CHECK(sol_mir_program_build(&request, &program, &diagnostics)
        == SOL_MIR_PROGRAM_BUILD_INVALID_IR);
    CHECK(diagnostics.count != 0 || diagnostics.allocation_failed);
    CHECK(program.ir == NULL && program.templates == NULL);
    sol_diagnostics_free(&diagnostics);
    compilation.ir.files[0].aggregate_start = file_start;
    compilation.ir.files[0].aggregate_end = file_end;
    CHECK(sol_ir_validate(&compilation.ir, NULL));
    free_compilation(&compilation);
}

int main(void) {
    test_e6_closures_and_determinism();
    test_recursion_references_and_unreachable();
    test_evidence_and_callbacks();
    test_callback_source_limitations();
    test_callback_producer_discovery_limits();
    test_import_policy_and_lifecycle();
    test_predicate_proof_and_handler_relations();
    test_derived_bodyful_capability_is_transactionally_unsupported();
    test_source_backed_method_meter();
    test_source_backed_capability_meter();
    test_malformed_owner();
    if (failures != 0) {
        fprintf(stderr, "%d MIR program test(s) failed\n", failures);
        return 1;
    }
    printf("MIR program tests passed\n");
    return 0;
}
