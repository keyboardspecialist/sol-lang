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

static size_t diagnostic_count(const TestCompilation *compilation, const char *code) {
    size_t count = 0;
    for (size_t index = 0; index < compilation->diagnostics.count; ++index) {
        if (strcmp(compilation->diagnostics.items[index].code, code) == 0) ++count;
    }
    return count;
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
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
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

static void test_structural_generic_types(void) {
    static const char text[] =
        "module generic_types\n"
        "enum Failure { invalid }\n"
        "function option(value: Option<Int64>) -> Option<Int64> { return value }\n"
        "function result(value: Result<Int64, Failure>) -> Result<Int64, Failure> {\n"
        "    return value\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.types.type_application_count == 2);
    const SolTypeApplication *option = sol_type_application(
        &compilation.types,
        compilation.types.definitions[1]
    );
    const SolTypeApplication *result = sol_type_application(
        &compilation.types,
        compilation.types.definitions[2]
    );
    CHECK(option != NULL);
    CHECK(result != NULL);
    if (option != NULL) {
        CHECK(option->constructor == SOL_TYPE_CONSTRUCTOR_OPTION);
        CHECK(option->argument_count == 1);
        CHECK(option->arguments[0].kind == SOL_TYPE_INT64);
    }
    if (result != NULL) {
        CHECK(result->constructor == SOL_TYPE_CONSTRUCTOR_RESULT);
        CHECK(result->argument_count == 2);
        CHECK(result->arguments[0].kind == SOL_TYPE_INT64);
        CHECK(result->arguments[1].kind == SOL_TYPE_NOMINAL);
        CHECK(result->arguments[1].definition == 0);
    }
    free_compilation(&compilation);
}

static void test_invalid_generic_component(void) {
    static const char text[] =
        "module invalid_generic\n"
        "function missing(value: Option<Missing>) -> Option<Missing> { return value }\n"
        "function arity(value: Result<Int64>) -> Result<Int64> { return value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-009"));
    free_compilation(&compilation);
}

static void test_record_fields(void) {
    static const char text[] =
        "module record_fields\n"
        "record Pair { left: Int64, ready: Bool, }\n"
        "function make() -> Pair { return Pair { ready = true, left = 1, } }\n"
        "function read(pair: Pair) -> Int64 { return pair.left }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);
}

static void test_invalid_record_fields(void) {
    static const char text[] =
        "module invalid_record_fields\n"
        "record Pair { left: Int64, ready: Bool, }\n"
        "function missing() -> Pair { return Pair { left = 1, } }\n"
        "function unknown() -> Pair { return Pair { left = 1, ready = true, extra = 2, } }\n"
        "function duplicate() -> Pair { return Pair { left = 1, left = 2, ready = true, } }\n"
        "function wrong() -> Pair { return Pair { left = true, ready = true, } }\n"
        "function access(pair: Pair) -> Int64 { return pair.missing }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-013"));
    free_compilation(&compilation);
}

static void test_invalid_record_declaration(void) {
    static const char text[] =
        "module invalid_record_declaration\n"
        "record Duplicate { value: Int64, value: Bool, }\n"
        "record Missing { value: Unknown, }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-013"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-009"));
    free_compilation(&compilation);
}

static void test_enum_constructors_and_match(void) {
    static const char text[] =
        "module enum_match\n"
        "enum State { idle, running(speed: Int64), failed(message: Text), pair(left: Int64, ready: Bool), }\n"
        "function running() -> State { return State.running(speed = 10) }\n"
        "function pair() -> State { return State.pair(ready = true, left = 10) }\n"
        "function idle() -> State { return State.idle }\n"
        "function code(state: State) -> Int64 {\n"
        "    return match state {\n"
        "        idle => 0\n"
        "        running(speed) => speed\n"
        "        failed(message) => 1\n"
        "        pair(left, ready) => left\n"
        "    }\n"
        "}\n"
        "function bool_code(value: Bool) -> Int64 {\n"
        "    return match value { true => 1 false => 0 }\n"
        "}\n"
        "open enum Wire { known, }\n"
        "function wire(value: Wire) -> Int64 { return match value { known => 1 _ => 0 } }\n"
        "enum Empty {}\n"
        "function absurd(value: Empty) -> Int64 { return match value {} }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);
}

static void test_invalid_match(void) {
    static const char text[] =
        "module invalid_match\n"
        "enum State { idle, running(speed: Int64), }\n"
        "function incomplete(state: State) -> Int64 {\n"
        "    return match state { idle => 0 }\n"
        "}\n"
        "function duplicate(state: State) -> Int64 {\n"
        "    return match state { idle => 0 idle => 1 running(speed) => speed }\n"
        "}\n"
        "function unknown(state: State) -> Int64 {\n"
        "    return match state { missing => 0 _ => 1 }\n"
        "}\n"
        "function payload(state: State) -> Int64 {\n"
        "    return match state { idle => 0 running => 1 }\n"
        "}\n"
        "function branch(state: State) -> Int64 {\n"
        "    return match state { idle => 0 running(speed) => \"bad\" }\n"
        "}\n"
        "function unreachable(state: State) -> Int64 {\n"
        "    return match state { _ => 0 idle => 1 }\n"
        "}\n"
        "function complete_then_wildcard(state: State) -> Int64 {\n"
        "    return match state { idle => 0 running(speed) => speed _ => 1 }\n"
        "}\n"
        "open enum Wire { known, }\n"
        "function open_incomplete(value: Wire) -> Int64 { return match value { known => 1 } }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-MATCH-001"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-008"));
    free_compilation(&compilation);
}

static void test_invalid_enum_constructor(void) {
    static const char text[] =
        "module invalid_constructor\n"
        "enum State { idle, running(speed: Int64), pair(left: Int64, ready: Bool), }\n"
        "function wrong_type() -> State { return State.running(speed = true) }\n"
        "function wrong_count() -> State { return State.running() }\n"
        "function wrong_name() -> State { return State.running(value = 1) }\n"
        "function mixed() -> State { return State.pair(left = 1, true) }\n"
        "function runtime(state: State) -> State { return state.running(1) }\n"
        "function unknown() -> State { return State.missing }\n"
        "function constructor_choice(flag: Bool) -> State {\n"
        "    let constructor = if flag { State.running } else { State.pair }\n"
        "    return constructor(1)\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-014"));
    free_compilation(&compilation);
}

static void test_capability_operation_calls(void) {
    static const char text[] =
        "module capability_calls\n"
        "capability Clock {\n"
        "    function offset(delta: Int64) -> Int64\n"
        "    effects { clock.read<Self> }\n"
        "}\n"
        "function read(clock: capability Clock) -> Int64 {\n"
        "    let first = clock\n"
        "    let second = first\n"
        "    let operation = second.offset\n"
        "    let alias = operation\n"
        "    return alias(delta = 1)\n"
        "}\n"
        "function unused(clock: capability Clock) -> Int64 {\n"
        "    let operation = clock.offset\n"
        "    return 1\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    bool found_operation = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.types.expressions[index].kind == SOL_TYPE_CAPABILITY_OPERATION) {
            found_operation = true;
        }
    }
    CHECK(found_operation);
    SolParameterId origin = compilation.syntax.items[1].first_parameter;
    size_t capability_locals = 0;
    size_t operation_locals = 0;
    for (size_t index = 0; index < compilation.hir.local_count; ++index) {
        if (compilation.hir.locals[index].owner == 1) {
            if (compilation.types.local_capability_origins[index] != SOL_AST_NONE) {
                CHECK(compilation.types.local_capability_origins[index] == origin);
                ++capability_locals;
            }
            if (compilation.types.local_operation_origins[index] != SOL_AST_NONE) {
                CHECK(compilation.types.local_operation_origins[index] == origin);
                ++operation_locals;
            }
        }
    }
    CHECK(capability_locals == 3);
    CHECK(operation_locals == 2);
    free_compilation(&compilation);
}

static void test_invalid_capability_operations(void) {
    static const char text[] =
        "module invalid_capability_calls\n"
        "capability Clock { function offset(delta: Int64) -> Int64 effects { pure } }\n"
        "function wrong_type(clock: capability Clock) -> Int64 { return clock.offset(true) }\n"
        "function missing(clock: capability Clock) -> Int64 { return clock.missing() }\n"
        "function no_authority(clock: Clock) -> Int64 { return clock.offset(1) }\n"
        "function indirect(clock: capability Clock) -> Int64 {\n"
        "    let operation = clock.offset\n"
        "    return operation(1)\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-005"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-009"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-015"));
    free_compilation(&compilation);
}

static void test_computed_capability_provenance(void) {
    static const char text[] =
        "module computed_capability\n"
        "enum Empty {}\n"
        "capability Clock { function offset(delta: Int64) -> Int64 effects { pure } }\n"
        "function direct_if(flag: Bool, clock: capability Clock) -> Int64 {\n"
        "    return (if flag { clock } else { clock }).offset(1)\n"
        "}\n"
        "function aliased_if(flag: Bool, clock: capability Clock) -> Int64 {\n"
        "    let selected = if flag { clock } else { clock }\n"
        "    return selected.offset(1)\n"
        "}\n"
        "function direct_match(flag: Bool, clock: capability Clock) -> Int64 {\n"
        "    return (match flag { true => clock false => clock }).offset(1)\n"
        "}\n"
        "function aliased_match(flag: Bool, clock: capability Clock) -> Int64 {\n"
        "    let selected = match flag { true => clock false => clock }\n"
        "    return selected.offset(1)\n"
        "}\n"
        "function direct_operation_if(flag: Bool, clock: capability Clock) -> Int64 {\n"
        "    return (if flag { clock.offset } else { clock.offset })(1)\n"
        "}\n"
        "function aliased_operation_if(flag: Bool, clock: capability Clock) -> Int64 {\n"
        "    let selected = if flag { clock.offset } else { clock.offset }\n"
        "    return selected(1)\n"
        "}\n"
        "function operation_match(flag: Bool, clock: capability Clock) -> Int64 {\n"
        "    let selected = match flag { true => clock.offset false => clock.offset }\n"
        "    return selected(1)\n"
        "}\n"
        "function ignore_never(\n"
        "    flag: Bool, impossible: Empty, clock: capability Clock,\n"
        ") -> Int64 {\n"
        "    let selected = if flag { match impossible {} } else { clock }\n"
        "    return selected.offset(1)\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);
}

static void test_mixed_computed_capability_provenance(void) {
    static const char capability_text[] =
        "module mixed_capability\n"
        "capability Clock { function offset(delta: Int64) -> Int64 effects { pure } }\n"
        "function mixed(flag: Bool, first: capability Clock, second: capability Clock)\n"
        "-> Int64 { return (if flag { first } else { second }).offset(1) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, capability_text));
    CHECK(diagnostic_count(&compilation, "SOL-TYPE-015") == 1);
    CHECK(compilation.diagnostics.count == 1);
    free_compilation(&compilation);

    static const char operation_text[] =
        "module mixed_operation\n"
        "capability Clock { function offset(delta: Int64) -> Int64 effects { pure } }\n"
        "function mixed(flag: Bool, first: capability Clock, second: capability Clock)\n"
        "-> Int64 { return (if flag { first.offset } else { second.offset })(1) }\n";
    CHECK(compile_source(&compilation, operation_text));
    CHECK(diagnostic_count(&compilation, "SOL-TYPE-015") == 1);
    CHECK(compilation.diagnostics.count == 1);
    free_compilation(&compilation);
}

static void test_invalid_capability_declarations(void) {
    static const char text[] =
        "module invalid_capability_declarations\n"
        "capability Broken {\n"
        "    function duplicate(value: Int64) -> Int64 effects { pure }\n"
        "    function duplicate(value: Int64) -> Int64 effects { pure }\n"
        "    function unresolved(value: Missing) -> Missing effects { pure }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-009"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-015"));
    free_compilation(&compilation);
}

static void test_general_function_types(void) {
    static const char text[] =
        "module general_function_types\n"
        "capability Registry {\n"
        "    function keep(\n"
        "        callback: function(Int64) -> Bool effects { clock.read },\n"
        "    ) -> function(Int64) -> Bool effects { clock.read } effects { pure }\n"
        "}\n"
        "function keep(\n"
        "    callback: function(Int64) -> Bool effects {\n"
        "        clock.read\n"
        "        network.call<Primary>\n"
        "    },\n"
        ") -> function(Int64) -> Bool effects {\n"
        "    network.call<Primary>\n"
        "    clock.read\n"
        "} { return callback }\n"
        "function keep_pure(\n"
        "    callback: function() -> Int64 effects { pure },\n"
        ") -> function() -> Int64 effects {} { return callback }\n"
        "function keep_optional(\n"
        "    callback: Option<function(Int64) -> Bool effects { pure }>,\n"
        ") -> Option<function(Int64) -> Bool effects {}> { return callback }\n"
        "function invoke(\n"
        "    callback: function(Int64) -> Bool effects { pure },\n"
        "    value: Int64,\n"
        ") -> Bool { return callback(value) }\n"
        "function widen(\n"
        "    callback: function() -> Int64 effects { pure },\n"
        ") -> function() -> Int64 effects { clock.read } { return callback }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.types.function_type_count >= 4);
    for (size_t index = 0; index < compilation.types.function_type_count; ++index) {
        const SolFunctionType *function = &compilation.types.function_types[index];
        CHECK(function->result.kind != SOL_TYPE_ERROR);
        CHECK(function->effects.count <= 2);
    }
    free_compilation(&compilation);
}

static void test_invalid_general_function_types(void) {
    static const char text[] =
        "module invalid_general_function_types\n"
        "function wrong_effect(\n"
        "    callback: function(Int64) -> Bool effects { clock.read },\n"
        ") -> function(Int64) -> Bool effects { network.call } { return callback }\n"
        "function invalid_rows(\n"
        "    duplicate: function() -> Int64 effects { clock.read clock.read },\n"
        "    mixed: function() -> Int64 effects { pure clock.read },\n"
        ") -> Int64 { return 1 }\n"
        "function invoke(\n"
        "    callback: function(Int64) -> Bool effects { pure },\n"
        ") -> Bool { return callback(true) }\n"
        "function wrong_count(\n"
        "    callback: function(Int64) -> Bool effects { pure },\n"
        ") -> Bool { return callback() }\n"
        "function named(\n"
        "    callback: function(Int64) -> Bool effects { pure },\n"
        ") -> Bool { return callback(value = 1) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-004"));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-001") == 2);
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-005"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-006"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-012"));
    free_compilation(&compilation);
}

static void test_invalid_return_authority_contract(void) {
    static const char text[] =
        "module invalid_return_authority\n"
        "capability Restricted {}\n"
        "capability Root {\n"
        "    function restrict() -> capability Restricted\n"
        "    authority { result derives_from Self }\n"
        "    effects { pure }\n"
        "}\n"
        "function lie(\n"
        "    claimed: capability Root,\n"
        "    actual: capability Root,\n"
        ") -> capability Restricted\n"
        "authority { result derives_from claimed }\n"
        "effects { pure } { return actual.restrict() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-AUTHORITY-001"));
    free_compilation(&compilation);
}

static void test_invalid_derived_capabilities(void) {
    static const char text[] =
        "module invalid_derived_capabilities\n"
        "capability FileSystem {\n"
        "    function read(path: Text) -> Text effects { filesystem.read<Self> }\n"
        "}\n"
        "capability Other {}\n"
        "capability ReadFileSystem derives_from source: capability FileSystem {\n"
        "    function read(path: Text) -> Text effects { filesystem.read<Self> } {\n"
        "        return source.read(path)\n"
        "    }\n"
        "}\n"
        "function wrong_source(fs: capability Other) -> capability ReadFileSystem {\n"
        "    return ReadFileSystem { source = fs }\n"
        "}\n"
        "function extra(fs: capability FileSystem) -> capability ReadFileSystem {\n"
        "    return ReadFileSystem { source = fs, extra = fs }\n"
        "}\n"
        "function missing() -> capability ReadFileSystem { return ReadFileSystem {} }\n"
        "function expose(fs: capability FileSystem) -> capability FileSystem {\n"
        "    let restricted = ReadFileSystem { source = fs }\n"
        "    return restricted.source\n"
        "}\n"
        "function nominal(fs: capability FileSystem) -> capability ReadFileSystem {\n"
        "    return fs\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-TYPE-015") >= 5);
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-004"));
    free_compilation(&compilation);
}

static void test_derived_capability_cycles(void) {
    static const char text[] =
        "module derived_capability_cycles\n"
        "capability First derives_from second: capability Second {}\n"
        "capability Second derives_from first: capability First {}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-015"));
    free_compilation(&compilation);
}

static void test_handler_types_and_metadata(void) {
    static const char text[] =
        "module handler_types\n"
        "capability Clock { function read(delta: Int64) -> Int64 effects { clock.read<Self> } }\n"
        "capability TestClock { function read(delta: Int64) -> Int64 effects { pure } }\n"
        "function handled(\n"
        "    choose: Bool, clock: capability Clock, provider: capability TestClock,\n"
        ") -> Text {\n"
        "    let selected = if choose { provider } else { provider }\n"
        "    return handle clock.read<clock> with selected { clock.read(1) \"handled\" }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    if (compilation.types.expressions == NULL || compilation.types.handlers == NULL) {
        free_compilation(&compilation);
        return;
    }
    CHECK(compilation.types.handler_count == compilation.syntax.expression_count);
    size_t handlers = 0;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind != SOL_EXPR_HANDLE) continue;
        CHECK(compilation.types.expressions[index].kind == SOL_TYPE_TEXT);
        CHECK(compilation.types.handlers[index].source_member == 0);
        CHECK(compilation.types.handlers[index].provider_member == 1);
        CHECK(compilation.types.handlers[index].root
            == compilation.syntax.parameters[
                compilation.syntax.items[2].first_parameter
            ].next);
        ++handlers;
    }
    CHECK(handlers == 1);
    free_compilation(&compilation);
}

static void test_invalid_handlers(void) {
    static const char text[] =
        "module invalid_handlers\n"
        "capability Clock { function read(value: Int64) -> Int64 effects { clock.read<Self> } }\n"
        "capability OtherClock { function read(value: Int64) -> Int64 effects { clock.read<Self> } }\n"
        "capability Impure { function read(value: Int64) -> Int64 effects { service.read<Self> } }\n"
        "capability Wrong { function read(value: Bool) -> Int64 effects { pure } }\n"
        "capability Missing { function value(value: Int64) -> Int64 effects { pure } }\n"
        "function ambiguous(clock: capability Clock, provider: capability Wrong) -> Int64 {\n"
        "    return handle clock.read<clock> with provider { 1 }\n"
        "}\n"
        "function no_root(provider: capability Wrong) -> Int64 {\n"
        "    return handle service.read<Impure> with provider { 1 }\n"
        "}\n"
        "function incompatible(authority: capability Impure, provider: capability Wrong) -> Int64 {\n"
        "    return handle service.read<authority> with provider { 1 }\n"
        "}\n"
        "function missing(authority: capability Impure, provider: capability Missing) -> Int64 {\n"
        "    return handle service.read<authority> with provider { 1 }\n"
        "}\n"
        "function impure(authority: capability Impure, provider: capability Impure) -> Int64 {\n"
        "    return handle service.read<authority> with provider { 1 }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-HANDLER-001"));
    CHECK(diagnostic_count(&compilation, "SOL-HANDLER-001") >= 5);
    free_compilation(&compilation);
}

static void test_contract_expression_types(void) {
    static const char text[] =
        "module contract_expression_types\n"
        "function sample(value: Int64) -> Int64\n"
        "requires { value > 0 }\n"
        "ensures { result == old(value) }\n"
        "{ return value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.syntax.contract_condition_count == 2);
    for (size_t index = 0; index < compilation.syntax.contract_condition_count; ++index) {
        SolExprId expression = compilation.syntax.contract_conditions[index].expression;
        CHECK(compilation.types.expressions[expression].kind == SOL_TYPE_BOOL);
        CHECK(compilation.types.expression_capability_origins[expression] == SOL_AST_NONE);
        CHECK(compilation.types.expression_operation_origins[expression] == SOL_AST_NONE);
    }

    SolExprId expression = compilation.syntax.contract_conditions[0].expression;
    compilation.syntax.contract_conditions[0].expression
        = compilation.syntax.items[0].body;
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
    compilation.syntax.contract_conditions[0].expression = expression;
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
    test_structural_generic_types();
    test_invalid_generic_component();
    test_record_fields();
    test_invalid_record_fields();
    test_invalid_record_declaration();
    test_enum_constructors_and_match();
    test_invalid_match();
    test_invalid_enum_constructor();
    test_capability_operation_calls();
    test_invalid_capability_operations();
    test_computed_capability_provenance();
    test_mixed_computed_capability_provenance();
    test_invalid_capability_declarations();
    test_general_function_types();
    test_invalid_general_function_types();
    test_invalid_return_authority_contract();
    test_invalid_derived_capabilities();
    test_derived_capability_cycles();
    test_handler_types_and_metadata();
    test_invalid_handlers();
    test_contract_expression_types();
    if (failures != 0) {
        fprintf(stderr, "%d type-checking test failure(s)\n", failures);
        return 1;
    }
    puts("type-checking tests passed");
    return 0;
}
