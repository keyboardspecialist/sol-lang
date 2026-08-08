#include "sol/diagnostic.h"
#include "sol/hir.h"
#include "sol/lexer.h"
#include "sol/parser.h"
#include "sol/source.h"

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
} TestCompilation;

static bool compile_source(TestCompilation *compilation, const char *text) {
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    if (!sol_source_from_text(&compilation->source, "sema.sol", text)) {
        return false;
    }
    if (!sol_lex(&compilation->source, &compilation->tokens, &compilation->diagnostics)) {
        return false;
    }
    if (!sol_parse(
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
    return sol_hir_lower(
        &compilation->source,
        &compilation->syntax,
        &compilation->hir,
        &compilation->diagnostics
    );
}

static void free_compilation(TestCompilation *compilation) {
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

static bool span_text_equal(const SolSource *source, SolSpan span, const char *text) {
    size_t length = span.end - span.start;
    return strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static void test_successful_resolution(void) {
    static const char text[] =
        "module resolution\n"
        "record Pair { left: Int64, right: Int64, }\n"
        "function make(a: Int64 /* input */, b: Int64) -> Pair {\n"
        "    let sum = a + b\n"
        "    return Pair { left = sum, right = b, }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.hir.definition_count == 2);
    CHECK(compilation.syntax.parameter_count == 2);
    CHECK(compilation.hir.local_count == 3);
    CHECK(span_text_equal(
        &compilation.source,
        compilation.syntax.parameters[0].type,
        "Int64"
    ));

    size_t resolved_paths = 0;
    bool pair_resolved = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        const SolExpr *expression = &compilation.syntax.expressions[index];
        if (expression->kind != SOL_EXPR_PATH) {
            continue;
        }
        ++resolved_paths;
        SolResolution resolution = compilation.hir.resolutions[index];
        CHECK(resolution.kind == SOL_RESOLUTION_LOCAL
            || resolution.kind == SOL_RESOLUTION_DEFINITION);
        if (span_text_equal(&compilation.source, expression->as.name, "Pair")) {
            CHECK(resolution.kind == SOL_RESOLUTION_DEFINITION);
            CHECK(resolution.target == 0);
            pair_resolved = true;
        }
    }
    CHECK(resolved_paths == 5);
    CHECK(pair_resolved);
    free_compilation(&compilation);
}

static void test_unresolved_name(void) {
    static const char text[] =
        "module unresolved\n"
        "function bad() -> Int64 { return missing }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-002"));
    free_compilation(&compilation);
}

static void test_duplicate_declaration(void) {
    static const char text[] =
        "module duplicate\n"
        "record Value {}\n"
        "record Value {}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-001"));
    free_compilation(&compilation);
}

static void test_scope_rules(void) {
    static const char text[] =
        "module scopes\n"
        "function bad(value: Int64) -> Int64 {\n"
        "    let repeated = value\n"
        "    let repeated = value\n"
        "    if true { let hidden = value hidden } else { 0 }\n"
        "    return hidden\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-003"));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-002"));
    free_compilation(&compilation);
}

static void test_qualified_function_resolution(void) {
    static const char text[] =
        "module qualified\n"
        "function service.client.value() -> Int64 { return 1 }\n"
        "function call() -> Int64 { return service /* lookup */ . client . value() }\n"
        "function shadow(service: Int64) -> Int64 { return service.client.value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    bool found_definition = false;
    bool found_shadow = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        const SolExpr *expression = &compilation.syntax.expressions[index];
        if (expression->kind == SOL_EXPR_FIELD
            && compilation.hir.resolutions[index].kind == SOL_RESOLUTION_DEFINITION) {
            CHECK(compilation.hir.resolutions[index].target == 0);
            found_definition = true;
        } else if (expression->kind == SOL_EXPR_FIELD) {
            SolExprId base = expression->as.field.base;
            if (compilation.syntax.expressions[base].kind == SOL_EXPR_PATH
                && compilation.hir.resolutions[base].kind == SOL_RESOLUTION_LOCAL) {
                found_shadow = true;
            }
        }
    }
    CHECK(found_definition);
    CHECK(found_shadow);
    free_compilation(&compilation);
}

static void test_qualified_duplicate_normalization(void) {
    static const char text[] =
        "module qualified_duplicate\n"
        "function service.value() -> Int64 { return 1 }\n"
        "function service /* same path */ . value() -> Int64 { return 2 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-001"));
    free_compilation(&compilation);
}

static void test_semantic_depth_limit(void) {
    char text[32768];
    size_t used = (size_t)snprintf(
        text,
        sizeof(text),
        "module deep\nfunction sum(value: Int64) -> Int64 { return value"
    );
    for (size_t index = 0; index < 600; ++index) {
        used += (size_t)snprintf(text + used, sizeof(text) - used, "+value");
    }
    snprintf(text + used, sizeof(text) - used, " }\n");

    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-004"));
    free_compilation(&compilation);
}

static void test_malformed_ast_rejected(void) {
    static const char text[] =
        "module malformed_ast\n"
        "function value() -> Int64 { return 1 + 2 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolExprId binary = SOL_AST_NONE;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind == SOL_EXPR_BINARY) {
            binary = index;
            break;
        }
    }
    CHECK(binary != SOL_AST_NONE);
    if (binary != SOL_AST_NONE) {
        compilation.syntax.expressions[binary].as.binary.left = SOL_AST_NONE;
    }

    sol_hir_module_free(&compilation.hir);
    sol_hir_module_init(&compilation.hir);
    sol_diagnostics_free(&compilation.diagnostics);
    sol_diagnostics_init(&compilation.diagnostics);
    CHECK(!sol_hir_lower(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    free_compilation(&compilation);
}

static void test_malformed_arena_metadata_rejected(void) {
    SolSource source;
    SolSyntaxTree syntax;
    SolHirModule hir;
    SolDiagnostics diagnostics;
    CHECK(sol_source_from_text(&source, "metadata.sol", "module metadata\n"));
    sol_syntax_tree_init(&syntax);
    sol_hir_module_init(&hir);
    sol_diagnostics_init(&diagnostics);
    syntax.item_count = 1;
    syntax.item_capacity = 1;
    CHECK(!sol_hir_lower(&source, &syntax, &hir, &diagnostics));
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-INTERNAL-002") == 0;
    }
    CHECK(found);

    sol_diagnostics_free(&diagnostics);
    sol_hir_module_free(&hir);
    sol_syntax_tree_free(&syntax);
    sol_source_free(&source);
}

static void test_derived_capability_resolution_and_malformed_body(void) {
    static const char text[] =
        "module derived_resolution\n"
        "capability Root {}\n"
        "capability Wrapper derives_from source: capability Root {\n"
        "    function invalid() -> capability Root effects { pure } { return Self }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-002"));
    free_compilation(&compilation);

    static const char valid_text[] =
        "module malformed_derived_body\n"
        "capability Root {}\n"
        "capability Wrapper derives_from source: capability Root {\n"
        "    function root() -> capability Root\n"
        "    authority { result derives_from Self }\n"
        "    effects { pure } { return source }\n"
        "}\n";
    CHECK(compile_source(&compilation, valid_text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolExprId body = compilation.syntax.capability_members[0].body;
    compilation.syntax.capability_members[0].body = compilation.syntax.expression_count;
    sol_hir_module_free(&compilation.hir);
    sol_hir_module_init(&compilation.hir);
    sol_diagnostics_free(&compilation.diagnostics);
    sol_diagnostics_init(&compilation.diagnostics);
    CHECK(!sol_hir_lower(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    compilation.syntax.capability_members[0].body = body;
    free_compilation(&compilation);
}

int main(void) {
    test_successful_resolution();
    test_unresolved_name();
    test_duplicate_declaration();
    test_scope_rules();
    test_qualified_function_resolution();
    test_qualified_duplicate_normalization();
    test_semantic_depth_limit();
    test_malformed_ast_rejected();
    test_malformed_arena_metadata_rejected();
    test_derived_capability_resolution_and_malformed_body();
    if (failures != 0) {
        fprintf(stderr, "%d semantic test failure(s)\n", failures);
        return 1;
    }
    puts("semantic tests passed");
    return 0;
}
