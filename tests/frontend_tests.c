#include "sol/diagnostic.h"
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

static void test_valid_declarations(void) {
    static const char source_text[] =
        "module demo.main\n"
        "edition 2027\n"
        "use core.Text\n"
        "@stable(\"demo.User.v1\")\n"
        "public record User { name: Text, }\n"
        "enum GreetError { invalid(field: Text), }\n"
        "capability Clock {\n"
        "    function now() -> Int64\n"
        "    effects { clock.read<Self> }\n"
        "}\n"
        "public function greet(user: User, clock: capability Clock) -> Result<Text, GreetError>\n"
        "effects { clock.read<clock> }\n"
        "requires { true }\n"
        "ensures { true } {\n"
        "    return ok(user.name)\n"
        "}\n";

    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "valid.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.edition == 2027);
    CHECK(tree.item_count == 4);

    size_t covered = 0;
    for (size_t index = 0; index + 1 < tokens.count; ++index) {
        CHECK(tokens.items[index].span.start == covered);
        covered = tokens.items[index].span.end;
    }
    CHECK(covered == source.length);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_missing_module(void) {
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "missing.sol", "record Value { value: Int64, }\n"));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(diagnostics.count >= 1);
    if (diagnostics.count >= 1) {
        CHECK(strcmp(diagnostics.items[0].code, "SOL-PARSE-004") == 0);
    }

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_nested_and_unterminated_comments(void) {
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    CHECK(sol_source_from_text(&source, "comments.sol", "/* outer /* inner */"));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(diagnostics.count == 1);
    if (diagnostics.count == 1) {
        CHECK(strcmp(diagnostics.items[0].code, "SOL-LEX-003") == 0);
    }

    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_clause_order(void) {
    static const char source_text[] =
        "module ordering\n"
        "function value() -> Int64\n"
        "ensures { true }\n"
        "effects { pure } { return 1 }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "ordering.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-PARSE-009") == 0;
    }
    CHECK(found);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_carriage_return_positions(void) {
    SolSource source;
    CHECK(sol_source_from_text(&source, "cr.sol", "module first\rrecord Value {}\r"));
    SolPosition position = sol_source_position(&source, 13);
    CHECK(position.line == 2);
    CHECK(position.column == 1);
    sol_source_free(&source);
}

static void test_type_depth_limit(void) {
    char source_text[4096];
    size_t used = (size_t)snprintf(
        source_text,
        sizeof(source_text),
        "module depth\nrecord Deep { value: "
    );
    for (size_t index = 0; index < 300 && used + 2 < sizeof(source_text); ++index) {
        source_text[used++] = 'A';
        source_text[used++] = '<';
    }
    source_text[used++] = 'Z';
    for (size_t index = 0; index < 300 && used + 1 < sizeof(source_text); ++index) {
        source_text[used++] = '>';
    }
    source_text[used++] = ',';
    source_text[used++] = '}';
    source_text[used] = '\0';

    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "depth.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-PARSE-010") == 0;
    }
    CHECK(found);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_declaration_recovery(void) {
    static const char source_text[] =
        "module recovery\n"
        "record Broken { value: Int64\n"
        "function recovered() -> Int64 { return 1 }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "recovery.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 1);
    if (tree.item_count == 1) {
        CHECK(tree.items[0].kind == SOL_ITEM_FUNCTION);
        SolToken name = {.span = tree.items[0].name};
        CHECK(sol_token_text_equal(&source, name, "recovered"));
    }

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

int main(void) {
    test_valid_declarations();
    test_missing_module();
    test_nested_and_unterminated_comments();
    test_clause_order();
    test_carriage_return_positions();
    test_type_depth_limit();
    test_declaration_recovery();
    if (failures != 0) {
        fprintf(stderr, "%d frontend test failure(s)\n", failures);
        return 1;
    }
    puts("frontend tests passed");
    return 0;
}
