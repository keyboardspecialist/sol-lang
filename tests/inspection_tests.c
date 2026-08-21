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
        "record Pair { left: Int64, right: Int64 }\n"
        "enum Choice { yes(value: Int64), no }\n"
        "capability Read { function read() -> Int64 effects { service.read<Self> } }\n"
        "capability Mock { function read() -> Int64 effects { pure } }\n"
        "function identity<T>(value: T) -> T { return value }\n"
        "function propagated(value: Option<Int64>) -> Option<Int64> "
        "{ let item = value? return some(item) }\n"
        "function expressions(source: capability Read, mock: capability Mock) -> Int64 "
        "effects { pure } { let text = \"fixture\" let unit = () "
        "let tuple = (text, 7,) let projected = tuple.1 "
        "let pair = Pair { left = 1, right = 2 } "
        "let selected = if true { match false { true => 1 false => -pair.left } } "
        "else { 0 } return identity<Int64>(handle service.read<source> with mock "
        "{ source.read() }) + selected }\n"
        "function checked(value: Int64) -> Int64 "
        "ensures { result == old(value) } { var local = value local = value "
        "return local }\n"
        "function looped() -> Int64 { var n = 0 while n < 1 "
        "invariant { false } decreases { 1 / 0 } { n += 1 } return n }\n"
        "function impossible() -> () { unreachable because { true } }\n";
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

static void test_expression_kind_spellings(TestCompilation *compilation) {
    static const char *const spellings[] = {
        "error", "integer", "string", "bool", "unit", "path", "unary",
        "binary", "call", "field", "tuple", "record", "if", "match", "block",
        "propagate", "handle", "result", "old", "type_application",
    };
    size_t census[sizeof(spellings) / sizeof(spellings[0])] = {0};
    for (size_t index = 0; index < compilation->package.syntax.expression_count; ++index) {
        SolExprKind kind = compilation->package.syntax.expressions[index].kind;
        CHECK((size_t)kind < sizeof(census) / sizeof(census[0]));
        if ((size_t)kind < sizeof(census) / sizeof(census[0])) ++census[kind];
    }
    CHECK(census[SOL_EXPR_ERROR] == 0);
    for (size_t kind = SOL_EXPR_INTEGER; kind <= SOL_EXPR_TYPE_APPLICATION; ++kind) {
        CHECK(census[kind] != 0);
    }

    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    if (stream == NULL) return;
    CHECK(sol_inspection_render(stream, &compilation->package,
        &compilation->hir, &compilation->types, &compilation->effects,
        &compilation->contracts, &compilation->diagnostics));
    long length = ftell(stream);
    CHECK(length > 0);
    rewind(stream);
    char output[65536];
    size_t read = fread(output, 1, sizeof(output) - 1, stream);
    output[read] = '\0';
    CHECK(length >= 0 && (size_t)length == read);
    for (size_t kind = SOL_EXPR_INTEGER; kind <= SOL_EXPR_TYPE_APPLICATION; ++kind) {
        char expected[64];
        int written = snprintf(expected, sizeof(expected),
            "\"kind\":\"%s\"", spellings[kind]);
        CHECK(written > 0 && (size_t)written < sizeof(expected));
        CHECK(strstr(output, expected) != NULL);
    }
    CHECK(compilation->contracts.loop_obligation_count == 4);
    CHECK(compilation->contracts.unreachable_obligation_count == 1);
    CHECK(strstr(output, "\"loopObligations\"") == NULL);
    CHECK(strstr(output, "\"unreachableObligations\"") == NULL);
    CHECK(strstr(output, "\"schema\":\"sol.inspection\",\"version\":2") != NULL);
    CHECK(strstr(output, "\"syntax\":{\"schema\":\"sol.inspection.syntax\",\"version\":2") != NULL);
    CHECK(strstr(output, "\"types\":{\"schema\":\"sol.inspection.types\",\"version\":2") != NULL);
    CHECK(strstr(output, "\"hir\":{\"schema\":\"sol.inspection.hir\",\"version\":1") != NULL);
    CHECK(strstr(output, "\"constructor\":\"tuple\"") != NULL);
    CHECK(strstr(output, "\"tupleProjections\":[") != NULL);
    fclose(stream);
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

    CHECK(table->loop_obligation_count == 4);
    if (table->loop_obligation_count != 0) {
        size_t capacity = table->loop_obligation_capacity;
        table->loop_obligation_capacity = 0;
        check_rejected(compilation);
        table->loop_obligation_capacity = capacity;
        SolLoopObligation *entries = table->loop_obligations;
        table->loop_obligations = NULL;
        check_rejected(compilation);
        table->loop_obligations = entries;
    }

    CHECK(table->unreachable_obligation_count == 1);
    if (table->unreachable_obligation_count == 1) {
        SolUnreachableObligation entry = table->unreachable_obligations[0];
        table->unreachable_obligations[0].id = 1;
        check_rejected(compilation);
        table->unreachable_obligations[0] = entry;
        size_t capacity = table->unreachable_obligation_capacity;
        table->unreachable_obligation_capacity = 0;
        check_rejected(compilation);
        table->unreachable_obligation_capacity = capacity;
        SolUnreachableObligation *entries = table->unreachable_obligations;
        table->unreachable_obligations = NULL;
        check_rejected(compilation);
        table->unreachable_obligations = entries;
    }

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

static void test_mutable_projection(TestCompilation *compilation) {
    FILE *stream = tmpfile();
    CHECK(stream != NULL);
    if (stream != NULL) {
        CHECK(sol_inspection_render(stream, &compilation->package,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics));
        rewind(stream);
        char output[16384];
        size_t length = fread(output, 1, sizeof(output) - 1, stream);
        output[length] = '\0';
        CHECK(strstr(output, "\"mutable\":true") != NULL);
        fclose(stream);
    }
    size_t mutable_local = 0;
    while (mutable_local < compilation->hir.local_count
        && !compilation->hir.locals[mutable_local].mutable) ++mutable_local;
    CHECK(mutable_local < compilation->hir.local_count);
    if (mutable_local < compilation->hir.local_count) {
        compilation->hir.locals[mutable_local].kind = SOL_LOCAL_PARAMETER;
        check_rejected(compilation);
        compilation->hir.locals[mutable_local].kind = SOL_LOCAL_BINDING;
    }
}

static void test_tuple_projection_preflight(TestCompilation *compilation) {
    SolTypeTable *table = &compilation->types;
    size_t expression = 0;
    while (expression < table->tuple_projection_count
        && table->tuple_projections[expression] == SOL_AST_NONE) ++expression;
    CHECK(expression < table->tuple_projection_count);
    if (expression == table->tuple_projection_count) return;

    size_t count = table->tuple_projection_count;
    table->tuple_projection_count = count - 1;
    check_rejected(compilation);
    table->tuple_projection_count = count;

    size_t *projections = table->tuple_projections;
    table->tuple_projections = NULL;
    check_rejected(compilation);
    table->tuple_projections = projections;

    size_t ordinal = projections[expression];
    projections[expression] = ordinal == 0 ? 1 : 0;
    check_rejected(compilation);
    projections[expression] = ordinal;
    projections[expression] = 16;
    check_rejected(compilation);
    projections[expression] = SOL_AST_NONE;
    check_rejected(compilation);
    projections[expression] = ordinal;

    SolExprId base = compilation->package.syntax.expressions[expression].as.field.base;
    CHECK(table->expressions[base].kind == SOL_TYPE_APPLICATION);
    SolTypeApplication *application
        = &table->type_applications[table->expressions[base].definition];
    CHECK(application->constructor == SOL_TYPE_CONSTRUCTOR_TUPLE);
    SolType projected_type = table->expressions[expression];
    table->expressions[expression] = table->type_application_arguments[
        application->argument_offset + (ordinal == 0 ? 1 : 0)];
    check_rejected(compilation);
    table->expressions[expression] = projected_type;
    size_t argument_count = application->argument_count;
    application->argument_count = 17;
    check_rejected(compilation);
    application->argument_count = argument_count;
    application->definition = 0;
    check_rejected(compilation);
    application->definition = SOL_AST_NONE;
}

static void test_semantic_reference_preflight(TestCompilation *compilation) {
    CHECK(compilation->hir.semantic_reference_count != 0);
    if (compilation->hir.semantic_reference_count == 0) return;
    SolSemanticReference *reference = &compilation->hir.semantic_references[0];
    SolDefId target = reference->target;
    SolSemanticId target_id = reference->target_id;
    reference->target = compilation->hir.definition_count;
    check_rejected(compilation);
    reference->target = target;
    reference->target_id.low ^= UINT64_C(1);
    check_rejected(compilation);
    reference->target_id = target_id;
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
        test_expression_kind_spellings(&compilation);
        test_source_line_preflight(&compilation);
        test_contract_preflight(&compilation);
        test_mutable_projection(&compilation);
        test_tuple_projection_preflight(&compilation);
        test_semantic_reference_preflight(&compilation);
        test_windows_basename(&compilation);
        test_windows_package_path(&compilation);
    }
    free_fixture(&compilation);
    return failures == 0 ? 0 : 1;
}
