#include "sol/package.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

#define VALID_DIRECTORY SOL_TEST_SOURCE_DIR "/tests/packages/valid"
#define RELOCATION_DIRECTORY SOL_TEST_SOURCE_DIR "/tests/packages/relocation"
#define RELOCATION_SEED SOL_TEST_SOURCE_DIR "/tests/packages/relocation/a.sol"
#define SINGLE_FILE SOL_TEST_SOURCE_DIR "/tests/valid.sol"

typedef struct {
    const char *relative_path;
    const char *module_name;
    size_t import_start;
    size_t import_count;
    size_t item_start;
    size_t item_count;
} ExpectedFile;

static const ExpectedFile expected_files[] = {
    {"/interfaces/display.sol", "example.interfaces", 0, 0, 0, 1},
    {"/main.sol", "example.main", 0, 8, 1, 1},
    {"/models/core.sol", "example.models", 8, 1, 2, 4},
    {"/models/create.sol", "example.models", 9, 0, 6, 1},
    {"/models/display.sol", "example.models", 9, 1, 7, 2},
    {"/rules/numbers.sol", "example.rules", 10, 0, 9, 1},
    {"/services/read.sol", "example.services", 10, 2, 10, 4},
};

static bool span_text_equal(const SolSource *source, SolSpan span, const char *text) {
    size_t length = span.end - span.start;
    return span.end <= source->length
        && strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static void check_valid_package(const SolPackage *package) {
    CHECK(package->is_directory);
    CHECK(strcmp(package->path, VALID_DIRECTORY) == 0);
    CHECK(package->file_count == sizeof(expected_files) / sizeof(expected_files[0]));
    CHECK(package->syntax.import_count == 12);
    CHECK(package->syntax.item_count == 14);
    CHECK(package->syntax.import_capacity > package->syntax.import_count);
    CHECK(package->syntax.item_capacity > package->syntax.item_count);
#define CHECK_ARENA_CAPACITY(name) \
    CHECK(package->syntax.name##_count <= package->syntax.name##_capacity)
    CHECK_ARENA_CAPACITY(import);
    CHECK_ARENA_CAPACITY(item);
    CHECK_ARENA_CAPACITY(expression);
    CHECK_ARENA_CAPACITY(statement);
    CHECK_ARENA_CAPACITY(loop_invariant);
    CHECK_ARENA_CAPACITY(argument);
    CHECK_ARENA_CAPACITY(parameter);
    CHECK_ARENA_CAPACITY(type);
    CHECK_ARENA_CAPACITY(type_argument);
    CHECK_ARENA_CAPACITY(type_parameter);
    CHECK_ARENA_CAPACITY(effect_parameter);
    CHECK_ARENA_CAPACITY(field);
    CHECK_ARENA_CAPACITY(variant);
    CHECK_ARENA_CAPACITY(pattern);
    CHECK_ARENA_CAPACITY(pattern_binding);
    CHECK_ARENA_CAPACITY(match_arm);
    CHECK_ARENA_CAPACITY(effect);
    CHECK_ARENA_CAPACITY(capability_member);
    CHECK_ARENA_CAPACITY(trait_method);
    CHECK_ARENA_CAPACITY(contract_clause);
    CHECK_ARENA_CAPACITY(contract_condition);
#undef CHECK_ARENA_CAPACITY
    CHECK(sol_syntax_contracts_validate(&package->source, &package->syntax));

    if (package->file_count != sizeof(expected_files) / sizeof(expected_files[0])) return;
    size_t aggregate_offset = 0;
    for (size_t index = 0; index < package->file_count; ++index) {
        const SolPackageFile *file = &package->files[index];
        const ExpectedFile *expected = &expected_files[index];
        char path[sizeof(VALID_DIRECTORY) + 32];
        int written = snprintf(path, sizeof(path), "%s%s", VALID_DIRECTORY, expected->relative_path);
        CHECK(written >= 0 && (size_t)written < sizeof(path));
        CHECK(strcmp(file->path, path) == 0);
        CHECK(file->aggregate_start == aggregate_offset);
        CHECK(file->aggregate_end == aggregate_offset + file->source.length);
        aggregate_offset = file->aggregate_end;
        if (index + 1 < package->file_count) ++aggregate_offset;
        CHECK(span_text_equal(&package->source, file->module_name, expected->module_name));
        CHECK(file->import_start == expected->import_start);
        CHECK(file->import_count == expected->import_count);
        CHECK(file->item_start == expected->item_start);
        CHECK(file->item_count == expected->item_count);
    }
    CHECK(package->source.length == aggregate_offset);

    CHECK(span_text_equal(&package->source, package->syntax.module_name, "example.interfaces"));
    CHECK(expected_files[2].item_start + expected_files[2].item_count
        == expected_files[3].item_start);
    CHECK(expected_files[3].item_start + expected_files[3].item_count
        == expected_files[4].item_start);
}

static void test_directory_load_and_boundaries(void) {
    SolPackage package;
    SolDiagnostics diagnostics;
    char error[256];
    sol_package_init(&package);
    sol_diagnostics_init(&diagnostics);

    CHECK(sol_package_file_at(&package, 0) == NULL);
    CHECK(sol_package_load(&package, VALID_DIRECTORY, &diagnostics, error, sizeof(error)));
    CHECK(error[0] == '\0');
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    check_valid_package(&package);

    for (size_t index = 0; index < package.file_count; ++index) {
        const SolPackageFile *file = &package.files[index];
        CHECK(sol_package_file_at(&package, file->aggregate_start) == file);
        CHECK(sol_package_file_at(&package, file->aggregate_end) == file);
        if (index + 1 < package.file_count) {
            CHECK(sol_package_file_at(&package, file->aggregate_end + 1)
                == &package.files[index + 1]);
        }
    }
    CHECK(sol_package_file_at(&package, package.source.length + 1) == NULL);
    CHECK(sol_package_file_at(&package, SIZE_MAX) == NULL);

    size_t item_count = package.syntax.item_count;
    package.syntax.item_count = package.syntax.item_capacity + 1;
    CHECK(!sol_syntax_contracts_validate(&package.source, &package.syntax));
    package.syntax.item_count = item_count;
    CHECK(sol_syntax_contracts_validate(&package.source, &package.syntax));

    sol_diagnostics_free(&diagnostics);
    sol_package_free(&package);
}

static void test_package_reuse(void) {
    SolPackage package;
    SolDiagnostics diagnostics;
    char error[256];
    sol_package_init(&package);
    sol_diagnostics_init(&diagnostics);

    CHECK(sol_package_load(&package, VALID_DIRECTORY, &diagnostics, error, sizeof(error)));
    CHECK(sol_package_load(&package, SINGLE_FILE, &diagnostics, error, sizeof(error)));
    CHECK(!package.is_directory);
    CHECK(package.file_count == 1);
    CHECK(package.source.length == package.files[0].source.length);
    CHECK(package.syntax.import_count == 1);
    CHECK(package.syntax.item_count == 24);
    CHECK(strcmp(package.path, SINGLE_FILE) == 0);
    CHECK(strcmp(package.files[0].path, SINGLE_FILE) == 0);
    CHECK(package.files[0].aggregate_start == 0);
    CHECK(package.files[0].aggregate_end == package.source.length);
    CHECK(sol_package_file_at(&package, package.source.length) == &package.files[0]);
    CHECK(sol_syntax_contracts_validate(&package.source, &package.syntax));

    CHECK(sol_package_load(&package, VALID_DIRECTORY, &diagnostics, error, sizeof(error)));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    check_valid_package(&package);

    sol_diagnostics_free(&diagnostics);
    sol_package_free(&package);
}

static bool write_text_file(const char *path, const char *text) {
    FILE *stream = fopen(path, "wb");
    if (stream == NULL) return false;
    size_t length = strlen(text);
    bool written = fwrite(text, 1, length, stream) == length;
    return fclose(stream) == 0 && written;
}

static void test_type_declaration_relocation(void) {
    char directory[] = "/tmp/sol-package-types-XXXXXX";
    CHECK(mkdtemp(directory) != NULL);
    char first_path[sizeof(directory) + 8];
    char second_path[sizeof(directory) + 8];
    snprintf(first_path, sizeof(first_path), "%s/a.sol", directory);
    snprintf(second_path, sizeof(second_path), "%s/z.sol", directory);
    CHECK(write_text_file(first_path,
        "module first\ntype Positive = refined Int64 where self > 0\n"
        "function prior() -> () { loop invariant { true } decreases { 1 } { break } }\n"));
    CHECK(write_text_file(second_path,
        "module second\nfunction seed(value: Int64) -> Int64 "
        "{ var pending: Int64 var copy = value modify copy { copy += 1 } "
        "while copy < 3 invariant { copy >= 0, copy <= 3 } decreases { 3 - copy } "
        "{ copy += 1 continue } loop invariant { copy >= 0 } { break } "
        "region scratch { let observed = copy } return copy }\n"));

    SolPackage package;
    SolDiagnostics diagnostics;
    char error[256];
    sol_package_init(&package);
    sol_diagnostics_init(&diagnostics);
    CHECK(sol_package_load(&package, directory, &diagnostics, error, sizeof(error)));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    CHECK(package.file_count == 2);
    CHECK(package.syntax.item_count == 3);
    if (package.syntax.item_count >= 3) {
        SolExprId body = package.syntax.items[2].body;
        CHECK(body < package.syntax.expression_count);
        if (body < package.syntax.expression_count) {
            SolStatementId region = package.syntax.expressions[body].as.block.first_statement;
            while (region < package.syntax.statement_count
                && package.syntax.statements[region].kind != SOL_STATEMENT_REGION) {
                region = package.syntax.statements[region].next;
            }
            CHECK(region < package.syntax.statement_count);
            if (region < package.syntax.statement_count) {
                CHECK(package.syntax.statements[region].kind == SOL_STATEMENT_REGION);
                CHECK(span_text_equal(&package.source,
                    package.syntax.statements[region].as.region_statement.label,
                    "scratch"));
            }
        }
    }
    if (package.syntax.item_count == 3) {
        const SolSyntaxItem *type = &package.syntax.items[0];
        CHECK(type->kind == SOL_ITEM_TYPE);
        CHECK(type->representation_type != SOL_AST_NONE);
        CHECK(type->representation_type < package.syntax.type_count);
        CHECK(package.syntax.types[type->representation_type].owner_item == 0);
        CHECK(span_text_equal(&package.source,
            package.syntax.types[type->representation_type].name, "Int64"));
        CHECK(type->first_contract != SOL_AST_NONE);
        if (type->first_contract != SOL_AST_NONE) {
            const SolContractClause *clause
                = &package.syntax.contract_clauses[type->first_contract];
            CHECK(clause->owner_kind == SOL_CONTRACT_OWNER_TYPE);
            CHECK(clause->owner == 0);
        }
    }
    CHECK(sol_syntax_contracts_validate(&package.source, &package.syntax));
    size_t typed_vars = 0;
    size_t compound_assignments = 0;
    size_t modifies = 0;
    size_t loops = 0;
    size_t whiles = 0;
    size_t exits = 0;
    for (size_t index = 0; index < package.syntax.statement_count; ++index) {
        const SolStatement *statement = &package.syntax.statements[index];
        if (statement->kind == SOL_STATEMENT_VAR
            && statement->as.let_statement.type_id != SOL_AST_NONE) {
            ++typed_vars;
            CHECK(statement->as.let_statement.value == SOL_AST_NONE);
            CHECK(statement->as.let_statement.type_id < package.syntax.type_count);
            CHECK(span_text_equal(&package.source,
                statement->as.let_statement.name, "pending"));
        } else if (statement->kind == SOL_STATEMENT_ASSIGNMENT
            && statement->as.assignment.operator_kind == SOL_TOKEN_PLUS_EQUAL) {
            ++compound_assignments;
        } else if (statement->kind == SOL_STATEMENT_MODIFY) {
            ++modifies;
            CHECK(statement->as.modify.target < package.syntax.expression_count);
            CHECK(statement->as.modify.body < package.syntax.expression_count);
        } else if (statement->kind == SOL_STATEMENT_LOOP
            || statement->kind == SOL_STATEMENT_WHILE) {
            loops += statement->kind == SOL_STATEMENT_LOOP;
            whiles += statement->kind == SOL_STATEMENT_WHILE;
            CHECK(statement->as.loop_statement.body < package.syntax.expression_count);
            CHECK(statement->as.loop_statement.body != SOL_AST_NONE);
            CHECK(statement->as.loop_statement.first_invariant != SOL_AST_NONE);
            if (statement->span.start >= package.files[1].aggregate_start) {
                CHECK(statement->as.loop_statement.first_invariant >= 1);
                CHECK(statement->as.loop_statement.invariant_span.start
                    >= package.files[1].aggregate_start);
                if (statement->as.loop_statement.decreases != SOL_AST_NONE) {
                    CHECK(statement->as.loop_statement.decreases_span.start
                        >= package.files[1].aggregate_start);
                }
            }
            if (statement->kind == SOL_STATEMENT_LOOP) {
                CHECK(statement->as.loop_statement.condition == SOL_AST_NONE);
            } else {
                CHECK(statement->as.loop_statement.condition
                    < package.syntax.expression_count);
            }
        } else if (statement->kind == SOL_STATEMENT_BREAK
            || statement->kind == SOL_STATEMENT_CONTINUE) {
            ++exits;
            CHECK(statement->as.expression == SOL_AST_NONE);
        }
    }
    CHECK(typed_vars == 1);
    CHECK(compound_assignments == 2);
    CHECK(modifies == 1);
    CHECK(loops == 2);
    CHECK(whiles == 1);
    CHECK(exits == 3);
    CHECK(package.syntax.loop_invariant_count == 4);
    for (size_t index = 0; index < package.syntax.loop_invariant_count; ++index) {
        const SolLoopInvariant *invariant = &package.syntax.loop_invariants[index];
        CHECK(invariant->expression < package.syntax.expression_count);
        if (index == 0) {
            CHECK(invariant->span.end <= package.files[0].aggregate_end);
        } else {
            CHECK(invariant->expression > package.syntax.loop_invariants[0].expression);
            CHECK(invariant->span.start >= package.files[1].aggregate_start);
        }
        CHECK(invariant->next == SOL_AST_NONE
            || invariant->next < package.syntax.loop_invariant_count);
    }

    sol_diagnostics_free(&diagnostics);
    sol_package_free(&package);
    CHECK(unlink(first_path) == 0);
    CHECK(unlink(second_path) == 0);
    CHECK(rmdir(directory) == 0);
}

static void test_composite_relocation(void) {
    SolPackage package;
    SolPackage seed;
    SolDiagnostics diagnostics;
    SolDiagnostics seed_diagnostics;
    char error[256];
    sol_package_init(&package);
    sol_package_init(&seed);
    sol_diagnostics_init(&diagnostics);
    sol_diagnostics_init(&seed_diagnostics);

    bool seed_loaded = sol_package_load(&seed, RELOCATION_SEED,
        &seed_diagnostics, error, sizeof(error));
    CHECK(seed_loaded);
    CHECK(!sol_diagnostics_has_errors(&seed_diagnostics));
    if (!seed_loaded) {
        sol_diagnostics_free(&seed_diagnostics);
        sol_diagnostics_free(&diagnostics);
        sol_package_free(&seed);
        sol_package_free(&package);
        return;
    }

    bool loaded = sol_package_load(&package, RELOCATION_DIRECTORY,
        &diagnostics, error, sizeof(error));
    CHECK(loaded);
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    CHECK(package.file_count == 2);
    if (!loaded || package.file_count != 2) {
        sol_diagnostics_free(&seed_diagnostics);
        sol_diagnostics_free(&diagnostics);
        sol_package_free(&seed);
        sol_package_free(&package);
        return;
    }
    CHECK(package.files[1].aggregate_start != 0);
    CHECK(sol_syntax_contracts_validate(&package.source, &package.syntax));

    bool kinds[SOL_EXPR_TYPE_APPLICATION + 1] = {false};
    size_t second_file_expressions = 0;
    CHECK(package.syntax.expression_count > seed.syntax.expression_count);
    for (size_t index = seed.syntax.expression_count;
        index < package.syntax.expression_count; ++index) {
        const SolExpr *expression = &package.syntax.expressions[index];
        CHECK(expression->span.start >= package.files[1].aggregate_start);
        CHECK(expression->span.end <= package.files[1].aggregate_end);
        CHECK((size_t)expression->kind <= SOL_EXPR_TYPE_APPLICATION);
        if ((size_t)expression->kind <= SOL_EXPR_TYPE_APPLICATION) {
            kinds[expression->kind] = true;
        }
        SolExprId children[3] = {SOL_AST_NONE, SOL_AST_NONE, SOL_AST_NONE};
        if (expression->kind == SOL_EXPR_UNARY) {
            children[0] = expression->as.unary.operand;
        } else if (expression->kind == SOL_EXPR_BINARY) {
            children[0] = expression->as.binary.left;
            children[1] = expression->as.binary.right;
        } else if (expression->kind == SOL_EXPR_CALL) {
            children[0] = expression->as.call.callee;
            if (expression->as.call.first_argument != SOL_AST_NONE) {
                CHECK(expression->as.call.first_argument != 0);
            }
        } else if (expression->kind == SOL_EXPR_TYPE_APPLICATION) {
            children[0] = expression->as.type_application.base;
            CHECK(expression->as.type_application.first_argument != SOL_AST_NONE
                && expression->as.type_application.first_argument != 0);
        } else if (expression->kind == SOL_EXPR_FIELD) {
            children[0] = expression->as.field.base;
        } else if (expression->kind == SOL_EXPR_RECORD) {
            children[0] = expression->as.record.type;
            CHECK(expression->as.record.first_field != SOL_AST_NONE
                && expression->as.record.first_field != 0);
        } else if (expression->kind == SOL_EXPR_IF) {
            children[0] = expression->as.if_expr.condition;
            children[1] = expression->as.if_expr.then_branch;
            children[2] = expression->as.if_expr.else_branch;
        } else if (expression->kind == SOL_EXPR_MATCH) {
            children[0] = expression->as.match_expr.scrutinee;
            CHECK(expression->as.match_expr.first_arm != SOL_AST_NONE
                && expression->as.match_expr.first_arm != 0);
        } else if (expression->kind == SOL_EXPR_BLOCK) {
            if (expression->as.block.first_statement != SOL_AST_NONE) {
                CHECK(expression->as.block.first_statement != 0);
            }
        } else if (expression->kind == SOL_EXPR_PROPAGATE) {
            children[0] = expression->as.propagated;
        } else if (expression->kind == SOL_EXPR_HANDLE) {
            children[0] = expression->as.handle.authority;
            children[1] = expression->as.handle.provider;
            children[2] = expression->as.handle.body;
        } else if (expression->kind == SOL_EXPR_OLD) {
            children[0] = expression->as.old_expression;
        }
        for (size_t child = 0; child < 3; ++child) {
            if (children[child] == SOL_AST_NONE) continue;
            CHECK(children[child] < package.syntax.expression_count);
            if (children[child] < package.syntax.expression_count) {
                CHECK(children[child] >= seed.syntax.expression_count);
            }
        }
        ++second_file_expressions;
    }
    CHECK(second_file_expressions != 0);
    CHECK(kinds[SOL_EXPR_UNARY]);
    CHECK(kinds[SOL_EXPR_BINARY]);
    CHECK(kinds[SOL_EXPR_CALL]);
    CHECK(kinds[SOL_EXPR_TYPE_APPLICATION]);
    CHECK(kinds[SOL_EXPR_FIELD]);
    CHECK(kinds[SOL_EXPR_RECORD]);
    CHECK(kinds[SOL_EXPR_IF]);
    CHECK(kinds[SOL_EXPR_MATCH]);
    CHECK(kinds[SOL_EXPR_BLOCK]);
    CHECK(kinds[SOL_EXPR_PROPAGATE]);
    CHECK(kinds[SOL_EXPR_HANDLE]);
    CHECK(kinds[SOL_EXPR_RESULT]);
    CHECK(kinds[SOL_EXPR_OLD]);

    CHECK(package.syntax.argument_count > seed.syntax.argument_count);
    for (size_t index = seed.syntax.argument_count;
        index < package.syntax.argument_count; ++index) {
        const SolArgument *argument = &package.syntax.arguments[index];
        CHECK(argument->value >= seed.syntax.expression_count);
        CHECK(argument->next == SOL_AST_NONE || argument->next >= seed.syntax.argument_count);
    }
    CHECK(package.syntax.statement_count > seed.syntax.statement_count);
    size_t relocated_panics = 0;
    size_t relocated_unreachable = 0;
    size_t relocated_requires = 0;
    for (size_t index = seed.syntax.statement_count;
        index < package.syntax.statement_count; ++index) {
        const SolStatement *statement = &package.syntax.statements[index];
        CHECK(statement->span.start >= package.files[1].aggregate_start);
        CHECK(statement->span.end <= package.files[1].aggregate_end);
        CHECK(statement->next == SOL_AST_NONE || statement->next >= seed.syntax.statement_count);
        if (statement->kind == SOL_STATEMENT_PANIC) {
            ++relocated_panics;
            CHECK(statement->as.panic_statement.message >= seed.syntax.expression_count);
            CHECK(statement->as.panic_statement.message < package.syntax.expression_count);
        } else if (statement->kind == SOL_STATEMENT_UNREACHABLE) {
            ++relocated_unreachable;
            CHECK(statement->as.unreachable_statement.proof >= seed.syntax.expression_count);
            CHECK(statement->as.unreachable_statement.proof
                < package.syntax.expression_count);
            CHECK(statement->as.unreachable_statement.because_span.start
                >= package.files[1].aggregate_start);
            CHECK(span_text_equal(&package.source,
                statement->as.unreachable_statement.because_span,
                "because { selected == local }"));
        } else if (statement->kind == SOL_STATEMENT_REQUIRE) {
            ++relocated_requires;
            CHECK(statement->as.require_statement.condition
                >= seed.syntax.expression_count);
            CHECK(statement->as.require_statement.fallback_block
                >= seed.syntax.expression_count);
            CHECK(statement->as.require_statement.fallback_block
                < package.syntax.expression_count);
            if (statement->as.require_statement.fallback_block
                < package.syntax.expression_count) {
                CHECK(package.syntax.expressions[
                    statement->as.require_statement.fallback_block].kind
                    == SOL_EXPR_BLOCK);
            }
        }
    }
    CHECK(relocated_panics == 2);
    CHECK(relocated_unreachable == 1);
    CHECK(relocated_requires == 1);
    CHECK(package.syntax.type_argument_count > seed.syntax.type_argument_count);
    for (size_t index = seed.syntax.type_argument_count;
        index < package.syntax.type_argument_count; ++index) {
        const SolTypeArgument *argument = &package.syntax.type_arguments[index];
        CHECK(argument->type >= seed.syntax.type_count);
        CHECK(argument->next == SOL_AST_NONE
            || argument->next >= seed.syntax.type_argument_count);
    }
    CHECK(package.syntax.match_arm_count > seed.syntax.match_arm_count);
    for (size_t index = seed.syntax.match_arm_count;
        index < package.syntax.match_arm_count; ++index) {
        const SolMatchArm *arm = &package.syntax.match_arms[index];
        CHECK(arm->span.start >= package.files[1].aggregate_start);
        CHECK(arm->pattern >= seed.syntax.pattern_count);
        CHECK(arm->value >= seed.syntax.expression_count);
        CHECK(arm->next == SOL_AST_NONE || arm->next >= seed.syntax.match_arm_count);
    }
    CHECK(package.syntax.pattern_count > seed.syntax.pattern_count);
    for (size_t index = seed.syntax.pattern_count;
        index < package.syntax.pattern_count; ++index) {
        const SolPattern *pattern = &package.syntax.patterns[index];
        CHECK(pattern->span.start >= package.files[1].aggregate_start);
        CHECK(pattern->first_binding == SOL_AST_NONE
            || pattern->first_binding >= seed.syntax.pattern_binding_count);
    }
    CHECK(package.syntax.effect_count > seed.syntax.effect_count);
    for (size_t index = seed.syntax.effect_count;
        index < package.syntax.effect_count; ++index) {
        const SolEffect *effect = &package.syntax.effects[index];
        CHECK(effect->span.start >= package.files[1].aggregate_start);
        CHECK(effect->next == SOL_AST_NONE || effect->next >= seed.syntax.effect_count);
    }
    CHECK(package.syntax.contract_condition_count > seed.syntax.contract_condition_count);
    for (size_t index = seed.syntax.contract_condition_count;
        index < package.syntax.contract_condition_count; ++index) {
        const SolContractCondition *condition = &package.syntax.contract_conditions[index];
        CHECK(condition->span.start >= package.files[1].aggregate_start);
        CHECK(condition->expression >= seed.syntax.expression_count);
        CHECK(condition->owner_clause >= seed.syntax.contract_clause_count);
    }

    CHECK(package.files[0].item_count != 0);
    CHECK(package.files[1].item_start >= package.files[0].item_count);
    for (size_t index = package.files[1].item_start;
        index < package.files[1].item_start + package.files[1].item_count; ++index) {
        CHECK(package.syntax.items[index].span.start
            >= package.files[1].aggregate_start);
    }

    sol_diagnostics_free(&seed_diagnostics);
    sol_diagnostics_free(&diagnostics);
    sol_package_free(&seed);
    sol_package_free(&package);
}

int main(void) {
    test_directory_load_and_boundaries();
    test_package_reuse();
    test_type_declaration_relocation();
    test_composite_relocation();
    if (failures != 0) {
        fprintf(stderr, "%d package test failure(s)\n", failures);
        return 1;
    }
    puts("package tests passed");
    return 0;
}
