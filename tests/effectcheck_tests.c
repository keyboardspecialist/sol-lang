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

static bool row_has_parameter_effect(
    const TestCompilation *compilation,
    const SolEffectRow *row,
    const char *name,
    SolParameterId parameter
) {
    for (size_t index = 0; index < row->count; ++index) {
        if (row->atoms[index].argument_kind == SOL_EFFECT_ATOM_PARAMETER
            && row->atoms[index].parameter == parameter
            && span_equals(&compilation->source, row->atoms[index].name, name)) {
            return true;
        }
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
        "public function exposed(value: Int64) -> Int64 {\n"
        "    return if value == 0 { 0 } else { exposed(value - 1) }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-005") == 2);
    free_compilation(&compilation);
}

static void test_recursive_pure_inference(void) {
    static const char text[] =
        "module recursive_boundaries\n"
        "function direct(value: Int64) -> Int64 {\n"
        "    return if value == 0 { 0 } else { direct(value - 1) }\n"
        "}\n"
        "function left(value: Int64) -> Int64 { return right(value) }\n"
        "function right(value: Int64) -> Int64 { return left(value) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.functions[0].inferred);
    CHECK(compilation.effects.functions[0].count == 0);
    CHECK(compilation.effects.functions[1].inferred);
    CHECK(compilation.effects.functions[1].count == 0);
    CHECK(compilation.effects.functions[2].inferred);
    CHECK(compilation.effects.functions[2].count == 0);
    free_compilation(&compilation);
}

static void test_recursive_parameter_effect_fixed_point(void) {
    static const char text[] =
        "module recursive_parameter_effects\n"
        "capability Clock { function now() -> Int64 effects { clock.read<Self> } }\n"
        "function left(\n"
        "    first: capability Clock,\n"
        "    second: capability Clock,\n"
        "    depth: Int64,\n"
        ") -> Int64 { return right(first, second, depth) }\n"
        "function right(\n"
        "    first: capability Clock,\n"
        "    second: capability Clock,\n"
        "    depth: Int64,\n"
        ") -> Int64 {\n"
        "    return if depth == 0 { first.now() } else { left(second, first, depth - 1) }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    for (SolDefId function = 1; function <= 2; ++function) {
        const SolSyntaxItem *item = &compilation.syntax.items[function];
        SolParameterId first = item->first_parameter;
        SolParameterId second = compilation.syntax.parameters[first].next;
        const SolEffectRow *row = &compilation.effects.functions[function];
        CHECK(row->inferred);
        CHECK(row->count == 2);
        CHECK(row_has_parameter_effect(&compilation, row, "clock.read", first));
        CHECK(row_has_parameter_effect(&compilation, row, "clock.read", second));
    }
    free_compilation(&compilation);
}

static void test_mixed_recursive_effect_boundary(void) {
    static const char text[] =
        "module mixed_recursive_boundary\n"
        "function inferred(value: Int64) -> Int64 {\n"
        "    return if value == 0 { 0 } else { declared(value - 1) }\n"
        "}\n"
        "function declared(value: Int64) -> Int64 effects { service.read } {\n"
        "    return if value == 0 { 0 } else { inferred(value - 1) }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.functions[0].inferred);
    CHECK(compilation.effects.functions[0].count == 1);
    CHECK(row_has_effect(&compilation, &compilation.effects.functions[0], "service.read"));
    CHECK(!compilation.effects.functions[1].inferred);
    free_compilation(&compilation);
}

static void test_explicit_caller_checks_recursive_fixed_point(void) {
    static const char text[] =
        "module recursive_caller_validation\n"
        "function source() -> Int64 effects { clock.read } { return 1 }\n"
        "function left(value: Int64) -> Int64 { return right(value) }\n"
        "function right(value: Int64) -> Int64 {\n"
        "    return if value == 0 { source() } else { left(value - 1) }\n"
        "}\n"
        "function caller(value: Int64) -> Int64 effects { pure } { return left(value) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-002") == 1);
    for (SolDefId function = 1; function <= 2; ++function) {
        CHECK(compilation.effects.functions[function].inferred);
        CHECK(compilation.effects.functions[function].count == 1);
        CHECK(row_has_effect(
            &compilation,
            &compilation.effects.functions[function],
            "clock.read"
        ));
    }
    free_compilation(&compilation);
}

static void test_recursive_substitution_diagnostic_deduplication(void) {
    static const char text[] =
        "module recursive_substitution_diagnostic\n"
        "capability Clock { function now() -> Int64 effects { clock.read<Self> } }\n"
        "function helper(clock: capability Clock) -> Int64\n"
        "effects { clock.read<clock> } { return clock.now() }\n"
        "function source() -> Int64 effects { service.read } { return 1 }\n"
        "function recursive(\n"
        "    flag: Bool,\n"
        "    first: capability Clock,\n"
        "    second: capability Clock,\n"
        "    depth: Int64,\n"
        ") -> Int64 {\n"
        "    let value = helper(if flag { first } else { second })\n"
        "    return if depth == 0 { value + source() } else {\n"
        "        value + recursive(flag, first, second, depth - 1)\n"
        "    }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-003") == 1);
    CHECK(compilation.effects.functions[3].inferred);
    CHECK(compilation.effects.functions[3].count == 1);
    CHECK(row_has_effect(&compilation, &compilation.effects.functions[3], "service.read"));
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
        "}\n"
        "function inferred_alias(actual: capability Clock) -> Int64 {\n"
        "    let first = actual\n"
        "    let second = first\n"
        "    let operation = second.now\n"
        "    let operation_alias = operation\n"
        "    return operation_alias() + helper(count = 1, clock = second)\n"
        "}\n"
        "function unused_operation(actual: capability Clock) -> Int64 {\n"
        "    let operation = actual.now\n"
        "    return 1\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.functions[5].inferred);
    CHECK(compilation.effects.functions[5].count == 1);
    CHECK(compilation.effects.functions[5].atoms[0].argument_kind
        == SOL_EFFECT_ATOM_PARAMETER);
    CHECK(compilation.effects.functions[5].atoms[0].parameter
        == compilation.syntax.items[5].first_parameter);
    CHECK(compilation.effects.functions[6].inferred);
    CHECK(compilation.effects.functions[6].count == 1);
    if (compilation.effects.functions[6].count == 1) {
        CHECK(compilation.effects.functions[6].atoms[0].parameter
            == compilation.syntax.items[6].first_parameter);
    }
    CHECK(compilation.effects.functions[7].inferred);
    CHECK(compilation.effects.functions[7].count == 0);
    free_compilation(&compilation);
}

static void test_function_effect_parameter_substitution(void) {
    static const char text[] =
        "module function_effect_substitution\n"
        "capability Clock { function now() -> Int64 effects { clock.read<Self> } }\n"
        "function helper(clock: capability Clock) -> Int64\n"
        "effects { clock.read<clock> } { return clock.now() }\n"
        "function wrong(actual: capability Clock, other: capability Clock) -> Int64\n"
        "effects { clock.read<other> } { let alias = actual return helper(alias) }\n";
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

static void test_computed_effect_argument(void) {
    static const char text[] =
        "module computed_effect_argument\n"
        "capability Clock { function now() -> Int64 effects { clock.read<Self> } }\n"
        "function helper(clock: capability Clock) -> Int64\n"
        "effects { clock.read<clock> clock.observe<clock> } { return clock.now() }\n"
        "function direct_if(flag: Bool, clock: capability Clock) -> Int64 {\n"
        "    return helper(if flag { clock } else { clock })\n"
        "}\n"
        "function alias_match(flag: Bool, clock: capability Clock) -> Int64\n"
        "effects { clock.read<clock> clock.observe<clock> } {\n"
        "    let authority = match flag { true => clock false => clock }\n"
        "    return helper(authority)\n"
        "}\n"
        "function operation_if(flag: Bool, clock: capability Clock) -> Int64 {\n"
        "    let operation = if flag { clock.now } else { clock.now }\n"
        "    return operation()\n"
        "}\n"
        "function operation_match(flag: Bool, clock: capability Clock) -> Int64 {\n"
        "    return (match flag { true => clock.now false => clock.now })()\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    for (size_t function = 2; function <= 5; ++function) {
        const SolEffectRow *row = &compilation.effects.functions[function];
        if (function != 3) CHECK(row->inferred);
        CHECK(row->count == (function <= 3 ? 2 : 1));
        SolParameterId expected_parameter
            = compilation.syntax.parameters[
                compilation.syntax.items[function].first_parameter
            ].next;
        for (size_t atom = 0; atom < row->count; ++atom) {
            CHECK(row->atoms[atom].argument_kind == SOL_EFFECT_ATOM_PARAMETER);
            CHECK(row->atoms[atom].parameter == expected_parameter);
        }
    }
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
        "effects { clock.read<other> } { return clock.now() }\n"
        "function bound_wrong(clock: capability Clock, other: capability Clock) -> Int64\n"
        "effects { clock.read<other> } { let operation = clock.now return operation() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-002") == 3);
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
        "capability Clock { function now(value: Int64) -> Int64 effects { pure } }\n"
        "function consume(\n"
        "    flag: Bool,\n"
        "    first: capability Clock,\n"
        "    second: capability Clock,\n"
        ") -> Int64 effects { pure } {\n"
        "    let selected = if flag { first } else { first }\n"
        "    let operation = first.now\n"
        "    let operation_alias = operation\n"
        "    let matched = match flag { true => first false => first }\n"
        "    let selected_operation = if flag { first.now } else { first.now }\n"
        "    let matched_operation = match flag { true => first.now false => first.now }\n"
        "    return 1\n"
        "}\n"
        "function foreign(other: capability Clock) -> Int64 effects { pure } { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
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
    SolParameterId *origins = compilation.types.local_capability_origins;
    compilation.types.local_capability_origins = NULL;
    CHECK(!sol_effect_check(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.types,
        &compilation.effects,
        &compilation.diagnostics
    ));
    compilation.types.local_capability_origins = origins;
    CHECK(origins != NULL);
    SolParameterId *expression_capability_origins
        = compilation.types.expression_capability_origins;
    compilation.types.expression_capability_origins = NULL;
    CHECK(!sol_effect_check(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.types,
        &compilation.effects,
        &compilation.diagnostics
    ));
    compilation.types.expression_capability_origins = expression_capability_origins;
    if (origins != NULL) {
        SolLocalId binding = SOL_AST_NONE;
        for (size_t index = 0; index < compilation.hir.local_count; ++index) {
            if (compilation.hir.locals[index].kind == SOL_LOCAL_BINDING) binding = index;
        }
        CHECK(binding != SOL_AST_NONE);
        if (binding != SOL_AST_NONE) {
            SolParameterId origin = origins[binding];
            origins[binding] = compilation.syntax.parameter_count;
            CHECK(!sol_effect_check(
                &compilation.source,
                &compilation.syntax,
                &compilation.hir,
                &compilation.types,
                &compilation.effects,
                &compilation.diagnostics
            ));
            SolParameterId first = compilation.syntax.items[1].first_parameter;
            SolParameterId second = compilation.syntax.parameters[first].next;
            second = compilation.syntax.parameters[second].next;
            origins[binding] = second;
            CHECK(!sol_effect_check(
                &compilation.source,
                &compilation.syntax,
                &compilation.hir,
                &compilation.types,
                &compilation.effects,
                &compilation.diagnostics
            ));
            origins[binding] = origin;
        }
    }
    SolParameterId *expression_operation_origins
        = compilation.types.expression_operation_origins;
    compilation.types.expression_operation_origins = NULL;
    CHECK(!sol_effect_check(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.types,
        &compilation.effects,
        &compilation.diagnostics
    ));
    compilation.types.expression_operation_origins = expression_operation_origins;
    SolParameterId *local_operation_origins = compilation.types.local_operation_origins;
    compilation.types.local_operation_origins = NULL;
    CHECK(!sol_effect_check(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.types,
        &compilation.effects,
        &compilation.diagnostics
    ));
    compilation.types.local_operation_origins = local_operation_origins;
    SolParameterId first_parameter = compilation.syntax.items[1].first_parameter;
    SolParameterId first_capability = compilation.syntax.parameters[first_parameter].next;
    SolParameterId second_capability
        = compilation.syntax.parameters[first_capability].next;
    SolExprKind capability_kinds[] = {
        SOL_EXPR_PATH,
        SOL_EXPR_BLOCK,
        SOL_EXPR_IF,
        SOL_EXPR_MATCH,
    };
    for (size_t kind = 0; kind < sizeof(capability_kinds) / sizeof(capability_kinds[0]); ++kind) {
        SolExprId target = SOL_AST_NONE;
        for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
            if (compilation.syntax.expressions[index].kind == capability_kinds[kind]
                && expression_capability_origins[index] == first_capability) {
                target = index;
                break;
            }
        }
        CHECK(target != SOL_AST_NONE);
        if (target != SOL_AST_NONE) {
            expression_capability_origins[target] = second_capability;
            CHECK(!sol_effect_check(
                &compilation.source,
                &compilation.syntax,
                &compilation.hir,
                &compilation.types,
                &compilation.effects,
                &compilation.diagnostics
            ));
            expression_capability_origins[target] = first_capability;
        }
    }
    SolExprKind operation_kinds[] = {
        SOL_EXPR_FIELD,
        SOL_EXPR_PATH,
        SOL_EXPR_BLOCK,
        SOL_EXPR_IF,
        SOL_EXPR_MATCH,
    };
    for (size_t kind = 0; kind < sizeof(operation_kinds) / sizeof(operation_kinds[0]); ++kind) {
        SolExprId target = SOL_AST_NONE;
        for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
            if (compilation.syntax.expressions[index].kind == operation_kinds[kind]
                && expression_operation_origins[index] == first_capability) {
                target = index;
                break;
            }
        }
        CHECK(target != SOL_AST_NONE);
        if (target != SOL_AST_NONE) {
            expression_operation_origins[target] = second_capability;
            CHECK(!sol_effect_check(
                &compilation.source,
                &compilation.syntax,
                &compilation.hir,
                &compilation.types,
                &compilation.effects,
                &compilation.diagnostics
            ));
            expression_operation_origins[target] = first_capability;
        }
    }
    SolLocalId operation_local = SOL_AST_NONE;
    for (size_t index = 0; index < compilation.hir.local_count; ++index) {
        if (compilation.types.locals[index].kind == SOL_TYPE_CAPABILITY_OPERATION) {
            operation_local = index;
        }
    }
    CHECK(operation_local != SOL_AST_NONE);
    if (operation_local != SOL_AST_NONE) {
        SolParameterId origin = local_operation_origins[operation_local];
        SolParameterId first = compilation.syntax.items[1].first_parameter;
        SolParameterId second = compilation.syntax.parameters[first].next;
        second = compilation.syntax.parameters[second].next;
        local_operation_origins[operation_local] = second;
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        local_operation_origins[operation_local] = origin;
    }
    SolParameterId consume_parameter = compilation.syntax.items[1].first_parameter;
    consume_parameter = compilation.syntax.parameters[consume_parameter].next;
    SolParameterId foreign_parameter = compilation.syntax.items[2].first_parameter;
    SolLocalId consume_local = SOL_AST_NONE;
    for (size_t index = 0; index < compilation.hir.local_count; ++index) {
        if (compilation.hir.locals[index].owner == 1
            && compilation.hir.locals[index].kind == SOL_LOCAL_PARAMETER
            && compilation.hir.locals[index].syntax_id == consume_parameter) {
            consume_local = index;
        }
    }
    CHECK(consume_local != SOL_AST_NONE);
    if (consume_local != SOL_AST_NONE) {
        compilation.hir.locals[consume_local].syntax_id = foreign_parameter;
        for (size_t index = 0; index < compilation.hir.local_count; ++index) {
            if (origins[index] == consume_parameter) origins[index] = foreign_parameter;
            if (local_operation_origins[index] == consume_parameter) {
                local_operation_origins[index] = foreign_parameter;
            }
        }
        for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
            if (expression_capability_origins[index] == consume_parameter) {
                expression_capability_origins[index] = foreign_parameter;
            }
            if (expression_operation_origins[index] == consume_parameter) {
                expression_operation_origins[index] = foreign_parameter;
            }
        }
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
    }
    free_compilation(&compilation);
}

static void test_orphan_expression_children_rejected(void) {
    static const char text[] =
        "module orphan_expression_children\n"
        "capability Clock { function now() -> Int64 effects { pure } }\n"
        "function sample(flag: Bool, clock: capability Clock) -> Int64 effects { pure } {\n"
        "    let selected = if flag { clock } else { clock }\n"
        "    let matched = match flag { true => selected false => clock }\n"
        "    return matched.now()\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    sol_effect_table_free(&compilation.effects);
    SolExprId body = compilation.syntax.items[1].body;
    compilation.syntax.items[1].body = SOL_AST_NONE;
    SolExprKind kinds[] = {
        SOL_EXPR_FIELD,
        SOL_EXPR_BLOCK,
        SOL_EXPR_IF,
        SOL_EXPR_MATCH,
    };
    for (size_t kind = 0; kind < sizeof(kinds) / sizeof(kinds[0]); ++kind) {
        SolExprId target = SOL_AST_NONE;
        for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
            if (compilation.syntax.expressions[index].kind == kinds[kind]) {
                target = index;
                break;
            }
        }
        CHECK(target != SOL_AST_NONE);
        if (target == SOL_AST_NONE) continue;
        SolExpr original = compilation.syntax.expressions[target];
        switch (kinds[kind]) {
            case SOL_EXPR_FIELD:
                compilation.syntax.expressions[target].as.field.base
                    = compilation.syntax.expression_count;
                break;
            case SOL_EXPR_BLOCK:
                compilation.syntax.expressions[target].as.block.first_statement
                    = compilation.syntax.statement_count;
                break;
            case SOL_EXPR_IF:
                compilation.syntax.expressions[target].as.if_expr.then_branch
                    = compilation.syntax.expression_count;
                break;
            case SOL_EXPR_MATCH:
                compilation.syntax.expressions[target].as.match_expr.scrutinee
                    = compilation.syntax.expression_count;
                break;
            default:
                break;
        }
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        compilation.syntax.expressions[target] = original;
    }
    compilation.syntax.items[1].body = body;
    free_compilation(&compilation);
}

static void test_expression_cycle_rejected(void) {
    static const char text[] =
        "module expression_cycle\n"
        "function negate(value: Int64) -> Int64 effects { pure } { return -value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    sol_effect_table_free(&compilation.effects);
    SolExprId unary = SOL_AST_NONE;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind == SOL_EXPR_UNARY) {
            unary = index;
            break;
        }
    }
    CHECK(unary != SOL_AST_NONE);
    if (unary != SOL_AST_NONE) {
        SolExprId operand = compilation.syntax.expressions[unary].as.unary.operand;
        compilation.syntax.expressions[unary].as.unary.operand = unary;
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        compilation.syntax.expressions[unary].as.unary.operand = operand;
    }
    free_compilation(&compilation);
}

static void test_forged_self_local_provenance_rejected(void) {
    static const char text[] =
        "module forged_self_local\n"
        "capability Clock {}\n"
        "function sample(clock: capability Clock) -> Int64 effects { pure } {\n"
        "    let alias = clock\n"
        "    return 1\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    sol_effect_table_free(&compilation.effects);
    SolLocalId binding = SOL_AST_NONE;
    for (size_t index = 0; index < compilation.hir.local_count; ++index) {
        if (compilation.hir.locals[index].kind == SOL_LOCAL_BINDING) {
            binding = index;
            break;
        }
    }
    CHECK(binding != SOL_AST_NONE);
    if (binding != SOL_AST_NONE) {
        SolStatementId statement = compilation.hir.locals[binding].syntax_id;
        SolExprId initializer
            = compilation.syntax.statements[statement].as.let_statement.value;
        SolResolution resolution = compilation.hir.resolutions[initializer];
        SolParameterId parameter = compilation.syntax.items[1].first_parameter;
        CHECK(compilation.types.local_capability_origins[binding] == parameter);
        CHECK(compilation.types.expression_capability_origins[initializer] == parameter);
        compilation.hir.resolutions[initializer] = (SolResolution){
            .kind = SOL_RESOLUTION_LOCAL,
            .target = binding,
        };
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        compilation.hir.resolutions[initializer] = resolution;
        SolSpan name = compilation.syntax.expressions[initializer].as.name;
        compilation.syntax.expressions[initializer].as.name = (SolSpan){
            .start = compilation.source.length + 1,
            .end = compilation.source.length + 2,
        };
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        compilation.syntax.expressions[initializer].as.name = name;
    }
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

static void test_function_type_effects_are_not_performed(void) {
    static const char text[] =
        "module function_type_effects\n"
        "function keep(\n"
        "    callback: function() -> Int64 effects { clock.read },\n"
        ") -> function() -> Int64 effects { clock.read } { return callback }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.types.function_type_count == 1);
    CHECK(compilation.types.function_types[0].effects.count == 1);
    CHECK(compilation.effects.functions[0].inferred);
    CHECK(compilation.effects.functions[0].count == 0);
    free_compilation(&compilation);
}

static void test_malformed_function_type_effects_rejected(void) {
    static const char text[] =
        "module malformed_function_type_effects\n"
        "function keep(\n"
        "    callback: function() -> Int64 effects { clock.read },\n"
        ") -> function() -> Int64 effects { clock.read } { return callback }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    sol_effect_table_free(&compilation.effects);
    CHECK(compilation.types.function_type_count == 1);
    SolEffectAtom *atoms = compilation.types.function_types[0].effects.atoms;
    CHECK(atoms != NULL);
    if (atoms != NULL) {
        atoms[0].parameter = 0;
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        atoms[0].parameter = SOL_AST_NONE;
    }
    free_compilation(&compilation);
}

static void test_higher_order_effects(void) {
    static const char text[] =
        "module higher_order_effects\n"
        "function source(value: Int64) -> Int64 effects { clock.read } { return value }\n"
        "function inferred_source(value: Int64) -> Int64 { return source(value) }\n"
        "function pure_source(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function apply(\n"
        "    value: Int64,\n"
        "    callback: function(Int64) -> Int64 effects { clock.read },\n"
        ") -> Int64 { return callback(value) }\n"
        "function valid() -> Int64 effects { clock.read } { return apply(1, source) }\n"
        "function alias_valid() -> Int64 effects { clock.read } {\n"
        "    let callback = source\n"
        "    return apply(1, callback)\n"
        "}\n"
        "function inferred_valid() -> Int64 effects { clock.read } {\n"
        "    return apply(1, inferred_source)\n"
        "}\n"
        "function pure_valid() -> Int64 effects { clock.read } {\n"
        "    return apply(1, pure_source)\n"
        "}\n"
        "function return_source() -> function(Int64) -> Int64 effects { clock.read } {\n"
        "    return source\n"
        "}\n"
        "function missing(\n"
        "    callback: function(Int64) -> Int64 effects { clock.read },\n"
        ") -> Int64 effects { pure } { return callback(1) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-002") == 1);
    CHECK(!has_diagnostic(&compilation, "SOL-EFFECT-006"));
    CHECK(compilation.effects.functions[3].inferred);
    CHECK(compilation.effects.functions[3].count == 1);
    CHECK(row_has_effect(&compilation, &compilation.effects.functions[3], "clock.read"));
    CHECK(compilation.types.function_coercion_count == 5);
    free_compilation(&compilation);
}

static void test_incompatible_callback_effects(void) {
    static const char text[] =
        "module incompatible_callback_effects\n"
        "capability Clock { function now() -> Int64 effects { clock.read<Self> } }\n"
        "function network(value: Int64) -> Int64 effects { network.call } { return value }\n"
        "function read(clock: capability Clock) -> Int64 effects { clock.read<clock> } {\n"
        "    return clock.now()\n"
        "}\n"
        "function apply(\n"
        "    value: Int64,\n"
        "    callback: function(Int64) -> Int64 effects { clock.read },\n"
        ") -> Int64 effects { clock.read } { return callback(value) }\n"
        "function apply_capability(\n"
        "    value: capability Clock,\n"
        "    callback: function(capability Clock) -> Int64 effects { clock.read },\n"
        ") -> Int64 effects { clock.read } { return callback(value) }\n"
        "function bad_effect() -> Int64 effects { clock.read } { return apply(1, network) }\n"
        "function bad_authority(clock: capability Clock) -> Int64 effects { clock.read } {\n"
        "    return apply_capability(clock, read)\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-006") == 1);
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-007") == 1);
    free_compilation(&compilation);
}

static void test_malformed_function_coercion_rejected(void) {
    static const char text[] =
        "module malformed_function_coercion\n"
        "function source(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function apply(\n"
        "    callback: function(Int64) -> Int64 effects { pure },\n"
        ") -> Int64 effects { pure } { return callback(1) }\n"
        "function invoke() -> Int64 effects { pure } { return apply(source) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.types.function_coercion_count == 1);
    if (compilation.types.function_coercion_count == 1) {
        sol_effect_table_free(&compilation.effects);
        compilation.types.function_coercions[0].expression
            = compilation.syntax.expression_count;
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    }
    free_compilation(&compilation);
}

static void test_static_bound_operation_callback(void) {
    static const char text[] =
        "module static_bound_operation_callback\n"
        "capability Gateway {\n"
        "    function send(value: Int64) -> Int64 effects { network.call<Primary> }\n"
        "}\n"
        "function apply(\n"
        "    value: Int64,\n"
        "    callback: function(Int64) -> Int64 effects { network.call<Primary> },\n"
        ") -> Int64 effects { network.call<Primary> } { return callback(value) }\n"
        "function valid(gateway: capability Gateway) -> Int64\n"
        "effects { network.call<Primary> } { return apply(1, gateway.send) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.types.function_coercion_count == 1);
    free_compilation(&compilation);
}

static void test_restricted_capability_return_authority(void) {
    static const char text[] =
        "module restricted_capability_return\n"
        "capability ReadFileSystem {\n"
        "    function read() -> Int64 effects { filesystem.read<Self> }\n"
        "}\n"
        "capability FileSystem {\n"
        "    function read_only() -> capability ReadFileSystem\n"
        "    authority { result derives_from Self }\n"
        "    effects { pure }\n"
        "}\n"
        "function restrict(filesystem: capability FileSystem) -> capability ReadFileSystem\n"
        "authority { result derives_from filesystem }\n"
        "effects { pure } { return filesystem.read_only() }\n"
        "function valid(filesystem: capability FileSystem) -> Int64\n"
        "effects { filesystem.read<filesystem> } {\n"
        "    let wrapper = restrict\n"
        "    let restricted = wrapper(filesystem = filesystem)\n"
        "    return restricted.read()\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolParameterId root = compilation.syntax.items[3].first_parameter;
    bool found_restricted_call = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind == SOL_EXPR_CALL
            && compilation.types.expressions[index].kind == SOL_TYPE_NOMINAL
            && compilation.types.expressions[index].definition == 0) {
            if (compilation.types.expression_capability_origins[index] == root) {
                found_restricted_call = true;
            }
        }
    }
    CHECK(found_restricted_call);
    free_compilation(&compilation);
}

static void test_derived_capability_wrapper(void) {
    static const char text[] =
        "module derived_capability_wrapper\n"
        "capability FileSystem {\n"
        "    function read(path: Text) -> Text effects { filesystem.read<Self> }\n"
        "}\n"
        "capability ReadFileSystem derives_from source: capability FileSystem {\n"
        "    function read(path: Text) -> Text effects { filesystem.read<Self> } {\n"
        "        return source.read(path)\n"
        "    }\n"
        "}\n"
        "function read(filesystem: capability FileSystem, path: Text) -> Text\n"
        "effects { filesystem.read<filesystem> } {\n"
        "    let restricted = ReadFileSystem { source = filesystem }\n"
        "    return restricted.read(path)\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolParameterId root = compilation.syntax.items[2].first_parameter;
    bool found_wrapper = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind == SOL_EXPR_RECORD
            && compilation.types.expressions[index].kind == SOL_TYPE_NOMINAL
            && compilation.types.expressions[index].definition == 1) {
            CHECK(compilation.types.expression_capability_origins[index] == root);
            found_wrapper = true;
        }
    }
    CHECK(found_wrapper);
    CHECK(compilation.effects.capability_member_count == 2);
    if (compilation.effects.capability_member_count == 2) {
        CHECK(compilation.effects.capability_members[1].count == 1);
        if (compilation.effects.capability_members[1].count == 1) {
            CHECK(compilation.effects.capability_members[1].atoms[0].argument_kind
                == SOL_EFFECT_ATOM_SELF);
        }
    }
    free_compilation(&compilation);
}

static void test_derived_capability_body_effect_checked(void) {
    static const char text[] =
        "module invalid_derived_capability_effect\n"
        "capability FileSystem {\n"
        "    function read(path: Text) -> Text effects { filesystem.read<Self> }\n"
        "}\n"
        "capability ReadFileSystem derives_from source: capability FileSystem {\n"
        "    function read(path: Text) -> Text effects { pure } {\n"
        "        return source.read(path)\n"
        "    }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-002") == 1);
    free_compilation(&compilation);
}

int main(void) {
    test_private_pure_inference();
    test_forward_transitive_inference();
    test_capability_self_inference_identity();
    test_branch_argument_union_and_deduplication();
    test_explicit_effect_boundaries();
    test_recursive_pure_inference();
    test_recursive_parameter_effect_fixed_point();
    test_mixed_recursive_effect_boundary();
    test_explicit_caller_checks_recursive_fixed_point();
    test_recursive_substitution_diagnostic_deduplication();
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
    test_computed_effect_argument();
    test_missing_capability_operation_effects();
    test_invalid_capability_effect_row();
    test_malformed_capability_arena_rejected();
    test_orphan_expression_children_rejected();
    test_expression_cycle_rejected();
    test_forged_self_local_provenance_rejected();
    test_nonempty_effect_table_rejected();
    test_function_type_effects_are_not_performed();
    test_malformed_function_type_effects_rejected();
    test_higher_order_effects();
    test_incompatible_callback_effects();
    test_malformed_function_coercion_rejected();
    test_static_bound_operation_callback();
    test_restricted_capability_return_authority();
    test_derived_capability_wrapper();
    test_derived_capability_body_effect_checked();
    if (failures != 0) {
        fprintf(stderr, "%d effect-checking test failure(s)\n", failures);
        return 1;
    }
    puts("effect-checking tests passed");
    return 0;
}
