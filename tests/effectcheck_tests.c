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
} TestCompilation;

static bool compile_source(TestCompilation *compilation, const char *text) {
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
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
        &compilation->diagnostics
    );
}

static void free_compilation(TestCompilation *compilation) {
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
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-002"));
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
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-002"));
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
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
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
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-002"));
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
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-002"));
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
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-002"));
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
    CHECK(!sol_effect_check(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.types,
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
        &compilation.diagnostics
    ));
    compilation.syntax.parameters = parameters;
    free_compilation(&compilation);
}

int main(void) {
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
    if (failures != 0) {
        fprintf(stderr, "%d effect-checking test failure(s)\n", failures);
        return 1;
    }
    puts("effect-checking tests passed");
    return 0;
}
