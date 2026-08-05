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

static void check_ast_links(const SolSyntaxTree *tree) {
    for (size_t index = 0; index < tree->expression_count; ++index) {
        const SolExpr *expression = &tree->expressions[index];
        switch (expression->kind) {
            case SOL_EXPR_UNARY:
                CHECK(expression->as.unary.operand < tree->expression_count);
                break;
            case SOL_EXPR_BINARY:
                CHECK(expression->as.binary.left < tree->expression_count);
                CHECK(expression->as.binary.right < tree->expression_count);
                break;
            case SOL_EXPR_CALL:
                CHECK(expression->as.call.callee < tree->expression_count);
                CHECK(expression->as.call.first_argument == SOL_AST_NONE
                    || expression->as.call.first_argument < tree->argument_count);
                break;
            case SOL_EXPR_FIELD:
                CHECK(expression->as.field.base < tree->expression_count);
                break;
            case SOL_EXPR_RECORD:
                CHECK(expression->as.record.type < tree->expression_count);
                CHECK(expression->as.record.first_field == SOL_AST_NONE
                    || expression->as.record.first_field < tree->argument_count);
                break;
            case SOL_EXPR_IF:
                CHECK(expression->as.if_expr.condition < tree->expression_count);
                CHECK(expression->as.if_expr.then_branch < tree->expression_count);
                CHECK(expression->as.if_expr.else_branch < tree->expression_count);
                break;
            case SOL_EXPR_BLOCK:
                CHECK(expression->as.block.first_statement == SOL_AST_NONE
                    || expression->as.block.first_statement < tree->statement_count);
                break;
            case SOL_EXPR_PROPAGATE:
                CHECK(expression->as.propagated < tree->expression_count);
                break;
            default:
                break;
        }
    }
    for (size_t index = 0; index < tree->argument_count; ++index) {
        CHECK(tree->arguments[index].value < tree->expression_count);
        CHECK(tree->arguments[index].next == SOL_AST_NONE
            || tree->arguments[index].next < tree->argument_count);
    }
    for (size_t index = 0; index < tree->statement_count; ++index) {
        const SolStatement *statement = &tree->statements[index];
        CHECK(statement->next == SOL_AST_NONE || statement->next < tree->statement_count);
        if (statement->kind == SOL_STATEMENT_LET) {
            CHECK(statement->as.let_statement.value < tree->expression_count);
        } else {
            CHECK(statement->as.expression < tree->expression_count);
        }
    }
}

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

static void test_expression_ast(void) {
    static const char source_text[] =
        "module expressions\n"
        "record Pair { left: Int64, right: Int64, }\n"
        "function choose(a: Int64, b: Int64, ready: Bool) -> Result<Pair, Error>\n"
        "effects { pure } {\n"
        "    let pair = Pair { left = a + b * 2, right = b, }\n"
        "    return if ready {\n"
        "        ok(value = pair)?\n"
        "    } else {\n"
        "        Error.invalid(field = \"ready\")\n"
        "    }\n"
        "}\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "expressions.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 2);
    check_ast_links(&tree);

    if (tree.item_count == 2) {
        SolExprId body_id = tree.items[1].body;
        CHECK(body_id != SOL_AST_NONE);
        if (body_id != SOL_AST_NONE) {
            SolExpr *body = &tree.expressions[body_id];
            CHECK(body->kind == SOL_EXPR_BLOCK);
            SolStatementId let_id = body->as.block.first_statement;
            CHECK(let_id != SOL_AST_NONE);
            if (let_id != SOL_AST_NONE) {
                SolStatement *let_statement = &tree.statements[let_id];
                CHECK(let_statement->kind == SOL_STATEMENT_LET);
                SolExpr *record = &tree.expressions[let_statement->as.let_statement.value];
                CHECK(record->kind == SOL_EXPR_RECORD);
                SolArgumentId left_field = record->as.record.first_field;
                CHECK(left_field != SOL_AST_NONE);
                if (left_field != SOL_AST_NONE) {
                    SolExpr *sum = &tree.expressions[tree.arguments[left_field].value];
                    CHECK(sum->kind == SOL_EXPR_BINARY);
                    CHECK(sum->as.binary.operator_kind == SOL_TOKEN_PLUS);
                    SolExpr *product = &tree.expressions[sum->as.binary.right];
                    CHECK(product->kind == SOL_EXPR_BINARY);
                    CHECK(product->as.binary.operator_kind == SOL_TOKEN_STAR);
                }

                SolStatementId return_id = let_statement->next;
                CHECK(return_id != SOL_AST_NONE);
                if (return_id != SOL_AST_NONE) {
                    SolStatement *return_statement = &tree.statements[return_id];
                    CHECK(return_statement->kind == SOL_STATEMENT_RETURN);
                    SolExpr *if_expression = &tree.expressions[return_statement->as.expression];
                    CHECK(if_expression->kind == SOL_EXPR_IF);
                    SolExpr *then_block = &tree.expressions[if_expression->as.if_expr.then_branch];
                    SolStatementId then_statement_id = then_block->as.block.first_statement;
                    CHECK(then_statement_id != SOL_AST_NONE);
                    if (then_statement_id != SOL_AST_NONE) {
                        SolExprId propagated_id = tree.statements[then_statement_id].as.expression;
                        CHECK(tree.expressions[propagated_id].kind == SOL_EXPR_PROPAGATE);
                    }
                }
            }
        }
    }

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_expression_recovery(void) {
    static const char source_text[] =
        "module recovery.expression\n"
        "function broken() -> Int64 { return 1 + }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "expression_recovery.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 1);
    check_ast_links(&tree);
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-PARSE-011") == 0;
    }
    CHECK(found);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_nested_record_condition(void) {
    static const char source_text[] =
        "module condition\n"
        "record Pair { left: Int64, right: Int64, }\n"
        "function compare() -> Int64 {\n"
        "    return if equal(Pair { left = 1, right = 2, }, Pair { left = 1, right = 2, }) {\n"
        "        1\n"
        "    } else {\n"
        "        0\n"
        "    }\n"
        "}\n"
        "function block_condition() -> Int64 {\n"
        "    return if { Pair { left = 1, right = 2, } } {\n"
        "        1\n"
        "    } else {\n"
        "        0\n"
        "    }\n"
        "}\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "condition.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    check_ast_links(&tree);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_expression_depth_limit(void) {
    char source_text[16384];
    size_t used = (size_t)snprintf(
        source_text,
        sizeof(source_text),
        "module expression_depth\nfunction deep(value: Bool) -> Int64 { return "
    );
    for (size_t index = 0; index < 300; ++index) {
        used += (size_t)snprintf(source_text + used, sizeof(source_text) - used, "if value { 1 } else ");
    }
    snprintf(source_text + used, sizeof(source_text) - used, "1 }\n");

    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "expression_depth.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-PARSE-013") == 0;
    }
    CHECK(found);
    check_ast_links(&tree);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_malformed_expression_delimiters(void) {
    static const char source_text[] =
        "module malformed\n"
        "function broken() -> Int64 { return call(1, }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "malformed.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.item_count == 1);
    check_ast_links(&tree);

    sol_syntax_tree_free(&tree);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    sol_source_free(&source);
}

static void test_capability_member_body_rejected(void) {
    static const char source_text[] =
        "module capability_body\n"
        "capability Clock { function now() -> Int64 { return 1 } }\n";
    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree tree;
    CHECK(sol_source_from_text(&source, "capability_body.sol", source_text));
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&tree);
    CHECK(sol_lex(&source, &tokens, &diagnostics));
    CHECK(sol_parse(&source, &tokens, &tree, &diagnostics));
    CHECK(sol_diagnostics_has_errors(&diagnostics));
    CHECK(tree.expression_count == 0);
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-PARSE-014") == 0;
    }
    CHECK(found);

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
    test_expression_ast();
    test_expression_recovery();
    test_nested_record_condition();
    test_expression_depth_limit();
    test_malformed_expression_delimiters();
    test_capability_member_body_rejected();
    if (failures != 0) {
        fprintf(stderr, "%d frontend test failure(s)\n", failures);
        return 1;
    }
    puts("frontend tests passed");
    return 0;
}
