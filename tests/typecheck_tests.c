#include "sol/diagnostic.h"
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
    if (!sol_source_from_text(&compilation->source, "types.sol", text)
        || !sol_lex(&compilation->source, &compilation->tokens, &compilation->diagnostics)
        || !sol_parse(
            &compilation->source,
            &compilation->tokens,
            &compilation->syntax,
            &compilation->diagnostics
        )) {
        return false;
    }
    if (sol_diagnostics_has_errors(&compilation->diagnostics)) {
        return true;
    }
    if (!sol_hir_lower(
        &compilation->source,
        &compilation->syntax,
        &compilation->hir,
        &compilation->diagnostics
    )) {
        return false;
    }
    if (sol_diagnostics_has_errors(&compilation->diagnostics)) {
        return true;
    }
    return sol_type_check(
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
        if (strcmp(compilation->diagnostics.items[index].code, code) == 0) {
            return true;
        }
    }
    return false;
}

static void test_valid_types(void) {
    static const char text[] =
        "module valid_types\n"
        "record Pair {}\n"
        "function add(a: Int64, b: Int64) -> Int64 {\n"
        "    let sum = a + b\n"
        "    return sum\n"
        "}\n"
        "function choose(flag: Bool) -> Int64 {\n"
        "    return if flag { 1 } else { 2 }\n"
        "}\n"
        "function call() -> Int64 { return add(1, 2) }\n"
        "function labeled(left: Int64, right: Bool) -> Int64 { return left }\n"
        "function named_call() -> Int64 { return labeled(right = true, left = 1) }\n"
        "function make() -> Pair { return Pair {} }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    bool found_binary = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind == SOL_EXPR_BINARY) {
            CHECK(compilation.types.expressions[index].kind == SOL_TYPE_INT64);
            found_binary = true;
        }
    }
    CHECK(found_binary);
    free_compilation(&compilation);
}

static void test_invalid_operator(void) {
    static const char text[] =
        "module invalid_operator\n"
        "function bad() -> Int64 { return true + 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-002"));
    free_compilation(&compilation);
}

static void test_invalid_return_and_condition(void) {
    static const char text[] =
        "module invalid_return\n"
        "function bad() -> Bool { return 1 }\n"
        "function condition() -> Int64 { return if 1 { 1 } else { 2 } }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-004"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-003"));
    free_compilation(&compilation);
}

static void test_invalid_call(void) {
    static const char text[] =
        "module invalid_call\n"
        "function add(a: Int64, b: Int64) -> Int64 { return a + b }\n"
        "function wrong_type() -> Int64 { return add(true, 2) }\n"
        "function wrong_count() -> Int64 { return add(1) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-005"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-006"));
    free_compilation(&compilation);
}

static void test_mismatched_if_branches(void) {
    static const char text[] =
        "module branch_types\n"
        "function bad(flag: Bool) -> Int64 {\n"
        "    return if flag { 1 } else { \"text\" }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-008"));
    free_compilation(&compilation);
}

static void test_unresolved_declared_type(void) {
    static const char text[] =
        "module missing_type\n"
        "function bad(value: Missing) -> Missing { return value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-009"));
    free_compilation(&compilation);
}

static void test_body_fallthrough_type(void) {
    static const char text[] =
        "module fallthrough\n"
        "function wrong() -> Int64 { true }\n"
        "function empty() -> Int64 {}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-004"));
    free_compilation(&compilation);
}

static void test_function_value_and_noncallable(void) {
    static const char text[] =
        "module callability\n"
        "function value() -> Int64 { return 1 }\n"
        "function function_value() -> Int64 { return value }\n"
        "function bad_call(number: Int64) -> Int64 { return number() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-004"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-010"));
    free_compilation(&compilation);
}

static void test_forward_and_recursive_calls(void) {
    static const char text[] =
        "module recursion\n"
        "function first(value: Int64) -> Int64 { return second(value) }\n"
        "function second(value: Int64) -> Int64 {\n"
        "    return if value == 0 { 0 } else { second(value - 1) }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);
}

static void test_local_function_call_and_unreachable(void) {
    static const char text[] =
        "module local_function\n"
        "function target() -> Int64 { return 1 }\n"
        "function indirect() -> Int64 { let callable = target return callable() }\n"
        "function unreachable() -> Int64 { return 1 true }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);
}

static void test_invalid_named_arguments(void) {
    static const char text[] =
        "module named_arguments\n"
        "function target(left: Int64, right: Bool) -> Int64 { return left }\n"
        "function unknown() -> Int64 { return target(missing = 1, right = true) }\n"
        "function duplicate() -> Int64 { return target(left = 1, left = 2) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-012"));
    free_compilation(&compilation);
}

static void test_malformed_hir_rejected(void) {
    static const char text[] =
        "module malformed_types\n"
        "function value() -> Int64 { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolResolution *resolutions = compilation.hir.resolutions;
    compilation.hir.resolutions = NULL;
    sol_type_table_free(&compilation.types);
    sol_type_table_init(&compilation.types);
    sol_diagnostics_free(&compilation.diagnostics);
    sol_diagnostics_init(&compilation.diagnostics);
    CHECK(!sol_type_check(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.types,
        &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-003"));
    compilation.hir.resolutions = resolutions;
    free_compilation(&compilation);
}

int main(void) {
    test_valid_types();
    test_invalid_operator();
    test_invalid_return_and_condition();
    test_invalid_call();
    test_mismatched_if_branches();
    test_unresolved_declared_type();
    test_body_fallthrough_type();
    test_function_value_and_noncallable();
    test_forward_and_recursive_calls();
    test_local_function_call_and_unreachable();
    test_invalid_named_arguments();
    test_malformed_hir_rejected();
    if (failures != 0) {
        fprintf(stderr, "%d type-checking test failure(s)\n", failures);
        return 1;
    }
    puts("type-checking tests passed");
    return 0;
}
