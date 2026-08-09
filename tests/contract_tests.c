#include "sol/contract.h"
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
    SolContractTable contracts;
} TestCompilation;

static bool compile_source(TestCompilation *compilation, const char *text) {
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    if (!sol_source_from_text(&compilation->source, "contracts.sol", text)
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
    )) return false;
    if (sol_diagnostics_has_errors(&compilation->diagnostics)) return true;
    if (!sol_type_check(
        &compilation->source,
        &compilation->syntax,
        &compilation->hir,
        &compilation->types,
        &compilation->diagnostics
    )) return false;
    if (sol_diagnostics_has_errors(&compilation->diagnostics)) return true;
    if (!sol_effect_check(
        &compilation->source,
        &compilation->syntax,
        &compilation->hir,
        &compilation->types,
        &compilation->effects,
        &compilation->diagnostics
    )) return false;
    if (sol_diagnostics_has_errors(&compilation->diagnostics)) return true;
    return sol_contract_lower(
        &compilation->source,
        &compilation->syntax,
        &compilation->hir,
        &compilation->types,
        &compilation->effects,
        &compilation->contracts,
        &compilation->diagnostics
    );
}

static void free_compilation(TestCompilation *compilation) {
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_syntax_tree_free(&compilation->syntax);
    sol_diagnostics_free(&compilation->diagnostics);
    sol_tokens_free(&compilation->tokens);
    sol_source_free(&compilation->source);
}

static size_t diagnostic_count(const TestCompilation *compilation, const char *code) {
    size_t count = 0;
    for (size_t index = 0; index < compilation->diagnostics.count; ++index) {
        if (strcmp(compilation->diagnostics.items[index].code, code) == 0) ++count;
    }
    return count;
}

static bool has_diagnostic(const TestCompilation *compilation, const char *code) {
    return diagnostic_count(compilation, code) != 0;
}

static bool span_equals(const SolSource *source, SolSpan span, const char *text) {
    size_t length = span.end - span.start;
    return strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static void test_obligations_result_and_snapshots(void) {
    static const char text[] =
        "module obligations\n"
        "enum Failure { invalid }\n"
        "function positive(value: Int64) -> Bool { return value >= 0 }\n"
        "function ordinary(value: Int64) -> Int64 effects { pure }\n"
        "requires { positive(value) }\n"
        "ensures {\n"
        "    result == old(value)\n"
        "    success => old(value) == old(value)\n"
        "}\n"
        "{ return value }\n"
        "function fallible(value: Int64) -> Result<Int64, Failure> effects { pure }\n"
        "ensures {\n"
        "    result == result\n"
        "    success => result == old(value)\n"
        "    failure => old(value) == value\n"
        "}\n"
        "{ return ok(value) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.contracts.obligation_count == 6);
    CHECK(compilation.contracts.snapshot_count == 5);
    for (size_t index = 0; index < compilation.contracts.obligation_count; ++index) {
        const SolObligation *obligation = &compilation.contracts.obligations[index];
        CHECK(obligation->id == (SolObligationId)index);
        CHECK(obligation->condition == index);
        CHECK(obligation->predicate_type.kind == SOL_TYPE_BOOL);
    }
    CHECK(!compilation.contracts.obligations[0].result.available);
    CHECK(compilation.contracts.obligations[1].result.available);
    CHECK(compilation.contracts.obligations[1].result.type.kind == SOL_TYPE_INT64);
    CHECK(compilation.contracts.obligations[2].result.available);
    CHECK(compilation.contracts.obligations[2].result.type.kind == SOL_TYPE_INT64);
    CHECK(compilation.contracts.obligations[3].result.available);
    CHECK(compilation.contracts.obligations[3].result.type.kind == SOL_TYPE_APPLICATION);
    CHECK(compilation.contracts.obligations[4].result.available);
    CHECK(compilation.contracts.obligations[4].result.type.kind == SOL_TYPE_INT64);
    CHECK(!compilation.contracts.obligations[5].result.available);
    CHECK(compilation.contracts.obligations[2].snapshot_count == 2);
    SolSnapshotId first = compilation.contracts.obligations[2].first_snapshot;
    CHECK(first + 1 < compilation.contracts.snapshot_count);
    if (first + 1 < compilation.contracts.snapshot_count) {
        CHECK(compilation.contracts.snapshots[first].id != compilation.contracts.snapshots[first + 1].id);
        CHECK(compilation.contracts.snapshots[first].type.kind == SOL_TYPE_INT64);
    }

    SolType fallible = compilation.types.definitions[3];
    const SolTypeApplication *application = sol_type_application(&compilation.types, fallible);
    CHECK(application != NULL);
    if (application != NULL) {
        CHECK(application->constructor == SOL_TYPE_CONSTRUCTOR_RESULT);
        CHECK(application->argument_count == 2);
        CHECK(application->arguments[0].kind == SOL_TYPE_INT64);
        CHECK(application->arguments[1].kind == SOL_TYPE_NOMINAL);
    }

    bool found_definition = false;
    bool found_parameter = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        const SolExpr *expression = &compilation.syntax.expressions[index];
        if (expression->kind != SOL_EXPR_PATH) continue;
        if (span_equals(&compilation.source, expression->as.name, "positive")
            && compilation.hir.resolutions[index].kind == SOL_RESOLUTION_DEFINITION) {
            found_definition = true;
        }
        if (span_equals(&compilation.source, expression->as.name, "value")
            && compilation.hir.resolutions[index].kind == SOL_RESOLUTION_LOCAL) {
            found_parameter = true;
        }
    }
    CHECK(found_definition);
    CHECK(found_parameter);
    free_compilation(&compilation);
}

static void test_member_contract_ownership(void) {
    static const char text[] =
        "module member_contracts\n"
        "capability Counter {\n"
        "    function next(delta: Int64) -> Int64 effects { pure }\n"
        "    requires { delta >= 0 }\n"
        "    ensures { result >= old(delta) }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.contracts.obligation_count == 2);
    for (size_t index = 0; index < 2; ++index) {
        CHECK(compilation.contracts.obligations[index].owner_kind
            == SOL_CONTRACT_OWNER_CAPABILITY_MEMBER);
        CHECK(compilation.contracts.obligations[index].owner == 0);
    }
    CHECK(compilation.contracts.obligations[1].result.available);
    CHECK(compilation.contracts.obligations[1].result.type.kind == SOL_TYPE_INT64);
    free_compilation(&compilation);
}

static void test_signature_scope_boundaries(void) {
    static const char body_local[] =
        "module body_local\n"
        "function sample(value: Int64) -> Int64\n"
        "requires { hidden == value }\n"
        "{ let hidden = value return hidden }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, body_local));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-002"));
    free_compilation(&compilation);

    static const char fresh_conditions[] =
        "module fresh_conditions\n"
        "function sample() -> Int64 effects { pure }\n"
        "requires { { let temporary = true temporary }, temporary }\n"
        "{ return 1 }\n";
    CHECK(compile_source(&compilation, fresh_conditions));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-002"));
    free_compilation(&compilation);

    static const char private_source[] =
        "module private_source\n"
        "capability Root {}\n"
        "capability Wrapper derives_from source: capability Root {\n"
        "    function value(argument: Int64) -> Int64 effects { pure }\n"
        "    requires { source == source }\n"
        "    { return argument }\n"
        "}\n";
    CHECK(compile_source(&compilation, private_source));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-002"));
    free_compilation(&compilation);
}

static void test_contract_type_errors(void) {
    static const char non_bool[] =
        "module non_bool\n"
        "function sample(value: Int64) -> Int64 effects { pure }\n"
        "requires { value } { return value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, non_bool));
    CHECK(has_diagnostic(&compilation, "SOL-CONTRACT-001"));
    free_compilation(&compilation);

    static const char result_failure[] =
        "module result_failure\n"
        "function sample() -> Int64 effects { pure }\n"
        "ensures { failure => result == 1 } { return 1 }\n";
    CHECK(compile_source(&compilation, result_failure));
    CHECK(has_diagnostic(&compilation, "SOL-CONTRACT-003"));
    free_compilation(&compilation);

    static const char old_forms[] =
        "module old_forms\n"
        "function sample(value: Int64) -> Int64 effects { pure }\n"
        "ensures { old(old(value)) == value, old(result) == value }\n"
        "{ return value }\n";
    CHECK(compile_source(&compilation, old_forms));
    CHECK(diagnostic_count(&compilation, "SOL-CONTRACT-003") == 2);
    free_compilation(&compilation);

    static const char result_requires[] =
        "module result_requires\n"
        "function sample() -> Int64 effects { pure }\n"
        "requires { result == 1 } { return 1 }\n";
    CHECK(compile_source(&compilation, result_requires));
    CHECK(has_diagnostic(&compilation, "SOL-PARSE-017"));
    free_compilation(&compilation);
}

static void test_purity_and_effect_firewall(void) {
    static const char text[] =
        "module purity\n"
        "capability Clock {\n"
        "    function read() -> Int64 effects { clock.read<Self> }\n"
        "    function constant() -> Int64 effects { pure }\n"
        "}\n"
        "function inferred(value: Int64) -> Bool { return value >= 0 }\n"
        "function effectful(clock: capability Clock) -> Bool\n"
        "effects { clock.read<clock> } { return clock.read() >= 0 }\n"
        "function sample(\n"
        "    value: Int64, clock: capability Clock,\n"
        "    callback: function(Int64) -> Bool effects { pure },\n"
        ") -> Int64 effects { pure }\n"
        "requires {\n"
        "    inferred(value) && callback(value) && clock.constant() >= 0\n"
        "    effectful(clock)\n"
        "    clock.read() >= 0\n"
        "}\n"
        "{ return value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (diagnostic_count(&compilation, "SOL-CONTRACT-002") != 2) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(diagnostic_count(&compilation, "SOL-CONTRACT-002") == 2);
    CHECK(compilation.effects.functions != NULL);
    if (compilation.effects.functions != NULL) {
        CHECK(compilation.effects.functions[3].count == 0);
    }
    CHECK(compilation.contracts.obligation_count == 3);
    free_compilation(&compilation);

    static const char forbidden[] =
        "module forbidden\n"
        "function sample() -> Bool effects { pure }\n"
        "requires { { return true }, ok(true)? }\n"
        "{ return true }\n";
    CHECK(compile_source(&compilation, forbidden));
    if (diagnostic_count(&compilation, "SOL-CONTRACT-002") != 2) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(diagnostic_count(&compilation, "SOL-CONTRACT-002") == 2);
    free_compilation(&compilation);

    static const char handler[] =
        "module contract_handler\n"
        "capability Source { function read() -> Bool effects { service.read<Self> } }\n"
        "capability Provider { function read() -> Bool effects { pure } }\n"
        "function sample(\n"
        "    source: capability Source, provider: capability Provider,\n"
        ") -> Bool effects { pure }\n"
        "requires { handle service.read<source> with provider { true } }\n"
        "{ return true }\n";
    CHECK(compile_source(&compilation, handler));
    CHECK(diagnostic_count(&compilation, "SOL-CONTRACT-002") == 1);
    free_compilation(&compilation);
}

static bool tables_equal(
    const SolContractTable *left,
    const SolContractTable *right
) {
    if (left->obligation_count != right->obligation_count
        || left->snapshot_count != right->snapshot_count) return false;
    for (size_t index = 0; index < left->obligation_count; ++index) {
        const SolObligation *a = &left->obligations[index];
        const SolObligation *b = &right->obligations[index];
        if (a->id != b->id || a->condition != b->condition
            || a->owner_kind != b->owner_kind || a->owner != b->owner
            || a->kind != b->kind || a->outcome != b->outcome
            || a->predicate != b->predicate
            || a->predicate_type.kind != b->predicate_type.kind
            || a->predicate_type.definition != b->predicate_type.definition
            || a->result.available != b->result.available
            || a->result.type.kind != b->result.type.kind
            || a->result.type.definition != b->result.type.definition
            || a->first_snapshot != b->first_snapshot
            || a->snapshot_count != b->snapshot_count) return false;
    }
    for (size_t index = 0; index < left->snapshot_count; ++index) {
        const SolSnapshot *a = &left->snapshots[index];
        const SolSnapshot *b = &right->snapshots[index];
        if (a->id != b->id || a->obligation != b->obligation
            || a->old_expression != b->old_expression || a->operand != b->operand
            || a->type.kind != b->type.kind || a->type.definition != b->type.definition) {
            return false;
        }
    }
    return true;
}

static void test_table_determinism_and_malformed_input(void) {
    static const char text[] =
        "module deterministic\n"
        "enum Failure { invalid }\n"
        "function sample(value: Int64) -> Result<Int64, Failure> effects { pure }\n"
        "requires { value >= 0 }\n"
        "ensures { success => result == old(value) }\n"
        "{ return ok(value) }\n";
    TestCompilation first;
    TestCompilation second;
    CHECK(compile_source(&first, text));
    CHECK(compile_source(&second, text));
    CHECK(!sol_diagnostics_has_errors(&first.diagnostics));
    CHECK(!sol_diagnostics_has_errors(&second.diagnostics));
    CHECK(tables_equal(&first.contracts, &second.contracts));

    sol_contract_table_free(&first.contracts);
    sol_contract_table_init(&first.contracts);
    SolContractClauseId owner = first.syntax.contract_conditions[0].owner_clause;
    first.syntax.contract_conditions[0].owner_clause = first.syntax.contract_clause_count;
    sol_diagnostics_free(&first.diagnostics);
    sol_diagnostics_init(&first.diagnostics);
    CHECK(!sol_contract_lower(
        &first.source,
        &first.syntax,
        &first.hir,
        &first.types,
        &first.effects,
        &first.contracts,
        &first.diagnostics
    ));
    CHECK(has_diagnostic(&first, "SOL-INTERNAL-005"));
    first.syntax.contract_conditions[0].owner_clause = owner;

    sol_contract_table_free(&first.contracts);
    sol_contract_table_init(&first.contracts);
    sol_diagnostics_free(&first.diagnostics);
    sol_diagnostics_init(&first.diagnostics);
    CHECK(first.types.type_application_count != 0);
    SolType argument = first.types.type_applications[0].arguments[0];
    first.types.type_applications[0].arguments[0] = (SolType){
        .kind = SOL_TYPE_APPLICATION,
        .definition = 0,
    };
    CHECK(!sol_contract_lower(
        &first.source,
        &first.syntax,
        &first.hir,
        &first.types,
        &first.effects,
        &first.contracts,
        &first.diagnostics
    ));
    CHECK(has_diagnostic(&first, "SOL-INTERNAL-005"));
    first.types.type_applications[0].arguments[0] = argument;
    free_compilation(&first);
    free_compilation(&second);
}

int main(void) {
    test_obligations_result_and_snapshots();
    test_member_contract_ownership();
    test_signature_scope_boundaries();
    test_contract_type_errors();
    test_purity_and_effect_firewall();
    test_table_determinism_and_malformed_input();
    if (failures != 0) {
        fprintf(stderr, "%d contract test failure(s)\n", failures);
        return 1;
    }
    puts("contract tests passed");
    return 0;
}
