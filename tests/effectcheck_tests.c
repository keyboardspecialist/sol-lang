#include "sol/diagnostic.h"
#include "sol/effectcheck.h"
#include "sol/hir.h"
#include "sol/lexer.h"
#include "sol/parser.h"
#include "sol/source.h"
#include "sol/typecheck.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
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
} TestCompilation;

static bool compile_source(TestCompilation *compilation, const char *text) {
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    if (!sol_source_from_text(&compilation->source, "effects.sol", text)
        || !sol_lex(&compilation->source, &compilation->tokens, &compilation->diagnostics)
        || !sol_parse(
            &compilation->source,
            &compilation->tokens,
            &compilation->syntax,
            &compilation->diagnostics
        )) {
        return false;
    }
    if (sol_diagnostics_has_errors(&compilation->diagnostics)) return true;
    if (!sol_hir_lower(
        &compilation->source,
        &compilation->syntax,
        &compilation->hir,
        &compilation->diagnostics
    ) || sol_diagnostics_has_errors(&compilation->diagnostics)) {
        return false;
    }
    if (!sol_type_check(
        &compilation->source,
        &compilation->syntax,
        &compilation->hir,
        &compilation->types,
        &compilation->diagnostics
    ) || sol_diagnostics_has_errors(&compilation->diagnostics)) {
        return false;
    }
    return sol_effect_check(
        &compilation->source,
        &compilation->syntax,
        &compilation->hir,
        &compilation->types,
        &compilation->effects,
        &compilation->diagnostics
    );
}

static void free_compilation(TestCompilation *compilation) {
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_syntax_tree_free(&compilation->syntax);
    sol_diagnostics_free(&compilation->diagnostics);
    sol_tokens_free(&compilation->tokens);
    sol_source_free(&compilation->source);
}

static bool has_diagnostic(const TestCompilation *compilation, const char *code) {
    for (size_t index = 0; index < compilation->diagnostics.count; ++index) {
        if (strcmp(compilation->diagnostics.items[index].code, code) == 0) return true;
    }
    return false;
}

static size_t diagnostic_count(const TestCompilation *compilation, const char *code) {
    size_t count = 0;
    for (size_t index = 0; index < compilation->diagnostics.count; ++index) {
        if (strcmp(compilation->diagnostics.items[index].code, code) == 0) ++count;
    }
    return count;
}

static bool span_equals(const SolSource *source, SolSpan span, const char *text) {
    size_t length = span.end - span.start;
    return strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static bool row_has_effect(
    const TestCompilation *compilation,
    const SolEffectRow *row,
    const char *name
) {
    for (size_t index = 0; index < row->count; ++index) {
        if (span_equals(&compilation->source, row->atoms[index].name, name)) return true;
    }
    return false;
}

static void test_private_pure_inference(void) {
    static const char text[] =
        "module private_pure\n"
        "function value() -> Int64 { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.function_count == compilation.syntax.item_count);
    CHECK(compilation.effects.functions[0].inferred);
    CHECK(compilation.effects.functions[0].count == 0);
    free_compilation(&compilation);
}

static void test_forward_transitive_inference(void) {
    static const char text[] =
        "module forward_transitive\n"
        "function outer() -> Int64 { return middle() }\n"
        "function middle() -> Int64 { return source() }\n"
        "function source() -> Int64 effects { clock.read } { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.functions[0].inferred);
    CHECK(compilation.effects.functions[0].count == 1);
    CHECK(row_has_effect(&compilation, &compilation.effects.functions[0], "clock.read"));
    CHECK(compilation.effects.functions[1].inferred);
    CHECK(compilation.effects.functions[1].count == 1);
    CHECK(!compilation.effects.functions[2].inferred);
    CHECK(compilation.effects.functions[2].atoms[0].argument_kind
        == SOL_EFFECT_ATOM_NO_ARGUMENT);
    free_compilation(&compilation);
}

static void test_capability_self_inference_identity(void) {
    static const char text[] =
        "module capability_self_inference\n"
        "capability Clock {\n"
        "    function now() -> Int64 effects { clock.read<Self> }\n"
        "}\n"
        "function read(clock: capability Clock) -> Int64 { return clock.now() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.capability_members[0].count == 1);
    CHECK(compilation.effects.capability_members[0].atoms[0].argument_kind
        == SOL_EFFECT_ATOM_SELF);
    const SolEffectRow *row = &compilation.effects.functions[1];
    CHECK(row->inferred);
    CHECK(row->count == 1);
    CHECK(row->atoms[0].argument_kind == SOL_EFFECT_ATOM_PARAMETER);
    CHECK(row->atoms[0].parameter == compilation.syntax.items[1].first_parameter);
    free_compilation(&compilation);
}

static void test_branch_argument_union_and_deduplication(void) {
    static const char text[] =
        "module branch_argument_union\n"
        "function first() -> Int64 effects { service.first } { return 1 }\n"
        "function second() -> Int64 effects { service.second } { return 2 }\n"
        "function add(left: Int64, right: Int64) -> Int64 effects { pure } {\n"
        "    return left + right\n"
        "}\n"
        "function combined(flag: Bool) -> Int64 {\n"
        "    return if flag { add(first(), first()) } else { second() }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    const SolEffectRow *row = &compilation.effects.functions[3];
    CHECK(row->inferred);
    CHECK(row->count == 2);
    CHECK(row_has_effect(&compilation, row, "service.first"));
    CHECK(row_has_effect(&compilation, row, "service.second"));
    free_compilation(&compilation);
}

static void test_explicit_effect_boundaries(void) {
    static const char text[] =
        "module explicit_boundaries\n"
        "capability Clock { function now() -> Int64 }\n"
        "public function exposed() -> Int64 { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-005") == 2);
    free_compilation(&compilation);
}

static void test_recursive_omitted_effect_boundaries(void) {
    static const char text[] =
        "module recursive_boundaries\n"
        "function direct(value: Int64) -> Int64 {\n"
        "    return if value == 0 { 0 } else { direct(value - 1) }\n"
        "}\n"
        "function left(value: Int64) -> Int64 { return right(value) }\n"
        "function right(value: Int64) -> Int64 { return left(value) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-005") == 3);
    CHECK(!compilation.effects.functions[0].inferred);
    CHECK(!compilation.effects.functions[1].inferred);
    CHECK(!compilation.effects.functions[2].inferred);
    free_compilation(&compilation);
}

static void test_explicit_recursive_effect_rows(void) {
    static const char text[] =
        "module explicit_recursive\n"
        "function direct(value: Int64) -> Int64 effects { pure } {\n"
        "    return if value == 0 { 0 } else { direct(value - 1) }\n"
        "}\n"
        "function left(value: Int64) -> Int64 effects { pure } { return right(value) }\n"
        "function right(value: Int64) -> Int64 effects { pure } { return left(value) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);
}

static void test_explicit_caller_checks_inferred_helper(void) {
    static const char text[] =
        "module explicit_calls_inferred\n"
        "function source() -> Int64 effects { clock.read } { return 1 }\n"
        "function helper() -> Int64 { return source() }\n"
        "function caller() -> Int64 effects { pure } { return helper() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(compilation.effects.functions[1].inferred);
    CHECK(compilation.effects.functions[1].count == 1);
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-002") == 1);
    free_compilation(&compilation);
}

static void test_valid_effect_propagation(void) {
    static const char text[] =
        "module valid_effects\n"
        "function read() -> Int64 effects { clock./* stable */read } { return 1 }\n"
        "function caller() -> Int64 effects { clock.read } { return read() }\n"
        "function pure_value() -> Int64 effects { pure } { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);
}

static void test_undeclared_effect(void) {
    static const char text[] =
        "module undeclared_effect\n"
        "function read() -> Int64 effects { clock.read } { return 1 }\n"
        "function missing() -> Int64 { return read() }\n"
        "function pure_caller() -> Int64 effects { pure } { return read() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-002") == 1);
    CHECK(compilation.effects.functions[1].inferred);
    CHECK(compilation.effects.functions[1].count == 1);
    free_compilation(&compilation);
}

static void test_invalid_effect_rows(void) {
    static const char text[] =
        "module invalid_rows\n"
        "function duplicate() -> Int64 effects { clock.read clock.read } { return 1 }\n"
        "function mixed() -> Int64 effects { pure clock.read } { return 1 }\n"
        "function parameterized_pure() -> Int64 effects { pure<Value> } { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-001"));
    CHECK(compilation.effects.functions[0].count == 1);
    CHECK(compilation.effects.functions[1].count == 1);
    CHECK(compilation.effects.functions[2].count == 0);
    free_compilation(&compilation);
}

static void test_parameterized_effects(void) {
    static const char text[] =
        "module parameterized_effects\n"
        "function read() -> Int64 effects { database.read<Primary> } { return 1 }\n"
        "function valid() -> Int64 effects { database.read<Primary> } { return read() }\n"
        "function wrong() -> Int64 effects { database.read<Secondary> } { return read() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-002") == 1);
    free_compilation(&compilation);
}

static void test_static_effect_argument_name_collision(void) {
    static const char text[] =
        "module static_effect_argument\n"
        "function source(tag: Int64) -> Int64 effects { database.read<tag> } { return tag }\n"
        "function caller(actual: Int64) -> Int64 effects { database.read<tag> } {\n"
        "    return source(actual)\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.functions[0].atoms[0].argument_kind
        == SOL_EFFECT_ATOM_STATIC_PATH);
    free_compilation(&compilation);
}

static void test_capability_operation_effects(void) {
    static const char text[] =
        "module capability_effects\n"
        "capability Clock {\n"
        "    function now() -> Int64 effects { clock.read<Self> }\n"
        "}\n"
        "function valid(clock: capability Clock) -> Int64 effects { clock.read<clock> } {\n"
        "    return clock.now()\n"
        "}\n"
        "function helper(clock: capability Clock, count: Int64) -> Int64\n"
        "effects { clock.read<clock> } { return clock.now() + count }\n"
        "function transitive(actual: capability Clock) -> Int64\n"
        "effects { clock.read<actual> } { return helper(count = 1, clock = actual) }\n"
        "function aliased(actual: capability Clock) -> Int64 effects { clock.read<actual> } {\n"
        "    let callable = helper\n"
        "    return callable(actual, 1)\n"
        "}\n"
        "function inferred(actual: capability Clock) -> Int64 {\n"
        "    return helper(count = 1, clock = actual)\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.functions[5].inferred);
    CHECK(compilation.effects.functions[5].count == 1);
    CHECK(compilation.effects.functions[5].atoms[0].argument_kind
        == SOL_EFFECT_ATOM_PARAMETER);
    CHECK(compilation.effects.functions[5].atoms[0].parameter
        == compilation.syntax.items[5].first_parameter);
    free_compilation(&compilation);
}

static void test_function_effect_parameter_substitution(void) {
    static const char text[] =
        "module function_effect_substitution\n"
        "capability Clock { function now() -> Int64 effects { clock.read<Self> } }\n"
        "function helper(clock: capability Clock) -> Int64\n"
        "effects { clock.read<clock> } { return clock.now() }\n"
        "function wrong(actual: capability Clock, other: capability Clock) -> Int64\n"
        "effects { clock.read<other> } { return helper(actual) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-002") == 1);
    free_compilation(&compilation);
}

static void test_exact_function_alias_effects(void) {
    static const char text[] =
        "module function_alias_effects\n"
        "function read() -> Int64 effects { clock.read } { return 1 }\n"
        "function pure_value() -> Int64 effects { pure } { return 1 }\n"
        "function bad() -> Int64 effects { pure } {\n"
        "    let callable = read\n"
        "    return callable()\n"
        "}\n"
        "function valid() -> Int64 effects { pure } {\n"
        "    let callable = pure_value\n"
        "    return callable()\n"
        "}\n"
        "function inferred() -> Int64 {\n"
        "    let callable = read\n"
        "    return callable()\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-002") == 1);
    CHECK(compilation.effects.functions[4].inferred);
    CHECK(compilation.effects.functions[4].count == 1);
    CHECK(row_has_effect(&compilation, &compilation.effects.functions[4], "clock.read"));
    free_compilation(&compilation);
}

static void test_unrepresentable_effect_argument(void) {
    static const char text[] =
        "module unrepresentable_effect_argument\n"
        "capability Clock { function now() -> Int64 effects { clock.read<Self> } }\n"
        "function helper(clock: capability Clock) -> Int64\n"
        "effects { clock.read<clock> clock.observe<clock> } { return clock.now() }\n"
        "function alias(clock: capability Clock) -> Int64\n"
        "effects { clock.read<clock> clock.observe<clock> } {\n"
        "    let authority = clock\n"
        "    return helper(authority)\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-003") == 1);
    free_compilation(&compilation);
}

static void test_missing_capability_operation_effects(void) {
    static const char text[] =
        "module missing_capability_effects\n"
        "capability Clock {\n"
        "    function now() -> Int64 effects { clock.read<Self> }\n"
        "}\n"
        "function missing(clock: capability Clock) -> Int64 effects { pure } {\n"
        "    return clock.now()\n"
        "}\n"
        "function wrong(clock: capability Clock, other: capability Clock) -> Int64\n"
        "effects { clock.read<other> } { return clock.now() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-002") == 2);
    free_compilation(&compilation);
}

static void test_invalid_capability_effect_row(void) {
    static const char text[] =
        "module invalid_capability_effects\n"
        "capability Clock {\n"
        "    function now() -> Int64 effects { clock.read<Self> clock.read<Self> }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-001"));
    free_compilation(&compilation);
}

static void test_malformed_capability_arena_rejected(void) {
    static const char text[] =
        "module malformed_capability_arena\n"
        "capability Clock { function now(value: Int64) -> Int64 effects { pure } }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolCapabilityMember *members = compilation.syntax.capability_members;
    compilation.syntax.capability_members = NULL;
    sol_effect_table_free(&compilation.effects);
    CHECK(!sol_effect_check(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.types,
        &compilation.effects,
        &compilation.diagnostics
    ));
    compilation.syntax.capability_members = members;
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    SolParameter *parameters = compilation.syntax.parameters;
    compilation.syntax.parameters = NULL;
    CHECK(!sol_effect_check(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.types,
        &compilation.effects,
        &compilation.diagnostics
    ));
    compilation.syntax.parameters = parameters;
    free_compilation(&compilation);
}

static void test_nonempty_effect_table_rejected(void) {
    static const char text[] =
        "module nonempty_effect_table\n"
        "function value() -> Int64 { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_effect_check(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.types,
        &compilation.effects,
        &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    free_compilation(&compilation);
}

int main(void) {
    test_private_pure_inference();
    test_forward_transitive_inference();
    test_capability_self_inference_identity();
    test_branch_argument_union_and_deduplication();
    test_explicit_effect_boundaries();
    test_recursive_omitted_effect_boundaries();
    test_explicit_recursive_effect_rows();
    test_explicit_caller_checks_inferred_helper();
    test_valid_effect_propagation();
    test_undeclared_effect();
    test_invalid_effect_rows();
    test_parameterized_effects();
    test_static_effect_argument_name_collision();
    test_capability_operation_effects();
    test_function_effect_parameter_substitution();
    test_exact_function_alias_effects();
    test_unrepresentable_effect_argument();
    test_missing_capability_operation_effects();
    test_invalid_capability_effect_row();
    test_malformed_capability_arena_rejected();
    test_nonempty_effect_table_rejected();
    if (failures != 0) {
        fprintf(stderr, "%d effect-checking test failure(s)\n", failures);
        return 1;
    }
    puts("effect-checking tests passed");
    return 0;
}
