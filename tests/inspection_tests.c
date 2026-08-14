#include "sol/inspection.h"

#include "sol/effectcheck.h"
#include "sol/hir.h"
#include "sol/lexer.h"
#include "sol/parser.h"
#include "sol/typecheck.h"

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
    SolPackage package;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    SolContractTable contracts;
} TestCompilation;

static bool compile_fixture(TestCompilation *compilation) {
    static const char source[] =
        "module fixture\n"
        "function checked(value: Int64) -> Int64 "
        "ensures { result == old(value) } { return value }\n";
    memset(compilation, 0, sizeof(*compilation));
    compilation->package.path = "fixture.sol";
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->package.syntax);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    return sol_source_from_text(&compilation->package.source, "fixture.sol", source)
        && sol_lex(&compilation->package.source, &compilation->tokens,
            &compilation->diagnostics)
        && sol_parse(&compilation->package.source, &compilation->tokens,
            &compilation->package.syntax, &compilation->diagnostics)
        && !sol_diagnostics_has_errors(&compilation->diagnostics)
        && sol_hir_lower(&compilation->package.source, &compilation->package.syntax,
            &compilation->hir, &compilation->diagnostics)
        && sol_type_check(&compilation->package.source, &compilation->package.syntax,
            &compilation->hir, &compilation->types, &compilation->diagnostics)
        && sol_effect_check(&compilation->package.source, &compilation->package.syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->diagnostics)
        && sol_contract_lower(&compilation->package.source, &compilation->package.syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics)
        && !sol_diagnostics_has_errors(&compilation->diagnostics);
}

static void free_fixture(TestCompilation *compilation) {
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_syntax_tree_free(&compilation->package.syntax);
    sol_diagnostics_free(&compilation->diagnostics);
    sol_tokens_free(&compilation->tokens);
    sol_source_free(&compilation->package.source);
}

static bool render(TestCompilation *compilation, long *length) {
    FILE *stream = tmpfile();
    if (stream == NULL) return false;
    bool rendered = sol_inspection_render(stream, &compilation->package,
        &compilation->hir, &compilation->types, &compilation->effects,
        &compilation->contracts, &compilation->diagnostics);
    *length = ftell(stream);
    fclose(stream);
    return rendered;
}

static void check_rejected(TestCompilation *compilation) {
    long length = -1;
    CHECK(!render(compilation, &length));
    CHECK(length == 0);
}

static void test_source_line_preflight(TestCompilation *compilation) {
    CHECK(sol_diagnostics_add(&compilation->diagnostics, "SOL-TEST-001",
        SOL_SEVERITY_WARNING, (SolSpan){0}, "test diagnostic"));
    size_t *line_starts = compilation->package.source.line_starts;
    compilation->package.source.line_starts = NULL;
    check_rejected(compilation);
    compilation->package.source.line_starts = line_starts;
    compilation->diagnostics.count = 0;

    size_t line_count = compilation->package.source.line_count;
    compilation->package.source.line_count = 0;
    check_rejected(compilation);
    compilation->package.source.line_count = line_count;
}

static void test_contract_preflight(TestCompilation *compilation) {
    SolContractTable *table = &compilation->contracts;
    CHECK(table->obligation_count == 1);
    CHECK(table->snapshot_count == 1);
    if (table->obligation_count != 1 || table->snapshot_count != 1) return;

    SolObligation obligation = table->obligations[0];
    SolSnapshot snapshot = table->snapshots[0];
    SolSnapshotId mapping = table->expression_snapshots[snapshot.old_expression];

    table->obligations[0].id = 1;
    check_rejected(compilation);
    table->obligations[0] = obligation;

    table->obligations[0].condition = compilation->package.syntax.contract_condition_count;
    check_rejected(compilation);
    table->obligations[0] = obligation;

    table->obligations[0].owner_kind = (SolContractOwnerKind)99;
    check_rejected(compilation);
    table->obligations[0] = obligation;

    table->obligations[0].owner = compilation->package.syntax.item_count;
    check_rejected(compilation);
    table->obligations[0] = obligation;

    table->obligations[0].kind = (SolContractClauseKind)99;
    check_rejected(compilation);
    table->obligations[0] = obligation;

    table->obligations[0].outcome = (SolContractOutcomeKind)99;
    check_rejected(compilation);
    table->obligations[0] = obligation;

    table->obligations[0].predicate = compilation->package.syntax.expression_count;
    check_rejected(compilation);
    table->obligations[0] = obligation;

    table->obligations[0].predicate_type.kind = SOL_TYPE_TEXT;
    check_rejected(compilation);
    table->obligations[0] = obligation;

    table->obligations[0].first_snapshot = table->snapshot_count;
    check_rejected(compilation);
    table->obligations[0] = obligation;

    table->snapshots[0].id = 1;
    check_rejected(compilation);
    table->snapshots[0] = snapshot;

    table->snapshots[0].obligation = table->obligation_count;
    check_rejected(compilation);
    table->snapshots[0] = snapshot;

    table->snapshots[0].operand = compilation->package.syntax.expression_count;
    check_rejected(compilation);
    table->snapshots[0] = snapshot;

    table->snapshots[0].type.kind = SOL_TYPE_TEXT;
    check_rejected(compilation);
    table->snapshots[0] = snapshot;

    table->expression_snapshots[snapshot.old_expression] = SOL_AST_NONE;
    check_rejected(compilation);
    table->expression_snapshots[snapshot.old_expression] = mapping;

    size_t snapshot_capacity = table->snapshot_capacity;
    table->snapshot_capacity = 0;
    check_rejected(compilation);
    table->snapshot_capacity = snapshot_capacity;

    size_t expression_count = table->expression_count;
    table->expression_count = 0;
    check_rejected(compilation);
    table->expression_count = expression_count;

    SolSnapshotId *expression_snapshots = table->expression_snapshots;
    table->expression_snapshots = NULL;
    check_rejected(compilation);
    table->expression_snapshots = expression_snapshots;
}

static void test_windows_basename(TestCompilation *compilation) {
    compilation->package.path = "C:\\root\\fixture.sol";
    compilation->package.source.path = "C:\\root\\fixture.sol";
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    if (stream != NULL) {
        CHECK(sol_inspection_render(stream, &compilation->package,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics));
        rewind(stream);
        char output[4096];
        size_t length = fread(output, 1, sizeof(output) - 1, stream);
        output[length] = '\0';
        CHECK(strstr(output, "\"path\":\"fixture.sol\"") != NULL);
        CHECK(strstr(output, "C:\\\\root") == NULL);
        fclose(stream);
    }
}

static void test_windows_package_path(TestCompilation *compilation) {
    SolPackageFile file = {
        .path = "C:\\root\\services\\fixture.sol",
        .source = compilation->package.source,
        .aggregate_end = compilation->package.source.length,
        .module_name = compilation->package.syntax.module_name,
    };
    compilation->package.path = "C:\\root\\";
    compilation->package.is_directory = true;
    compilation->package.files = &file;
    compilation->package.file_count = 1;
    compilation->package.file_capacity = 1;
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    if (stream != NULL) {
        CHECK(sol_inspection_render(stream, &compilation->package,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics));
        rewind(stream);
        char output[4096];
        size_t length = fread(output, 1, sizeof(output) - 1, stream);
        output[length] = '\0';
        CHECK(strstr(output, "\"path\":\"services/fixture.sol\"") != NULL);
        CHECK(strchr(output, '\\') == NULL);
        fclose(stream);
    }
    compilation->package.is_directory = false;
    compilation->package.files = NULL;
    compilation->package.file_count = 0;
    compilation->package.file_capacity = 0;
}

int main(void) {
    TestCompilation compilation;
    CHECK(compile_fixture(&compilation));
    if (failures == 0) {
        long length = 0;
        CHECK(render(&compilation, &length));
        CHECK(length > 0);
        test_source_line_preflight(&compilation);
        test_contract_preflight(&compilation);
        test_windows_basename(&compilation);
        test_windows_package_path(&compilation);
    }
    free_fixture(&compilation);
    return failures == 0 ? 0 : 1;
}
