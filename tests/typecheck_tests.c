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

static bool span_text_equal(const SolSource *source, SolSpan span, const char *text) {
    size_t length = span.end - span.start;
    return strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static bool rerun_typecheck(TestCompilation *compilation) {
    sol_type_table_free(&compilation->types);
    sol_type_table_init(&compilation->types);
    sol_diagnostics_free(&compilation->diagnostics);
    sol_diagnostics_init(&compilation->diagnostics);
    return sol_type_check(
        &compilation->source,
        &compilation->syntax,
        &compilation->hir,
        &compilation->types,
        &compilation->diagnostics
    );
}

static bool provenance_equals(
    const SolTypeTable *types,
    SolProvenanceId id,
    const SolParameterId *roots,
    size_t count
) {
    SolProvenance provenance;
    return sol_type_provenance(types, id, &provenance)
        && provenance.count == count
        && memcmp(provenance.roots, roots, count * sizeof(*roots)) == 0;
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

static void test_forged_type_resolutions_rejected(void) {
    static const char text[] =
        "module forged_type_resolutions\n"
        "record Left {}\n"
        "record Right {}\n"
        "function generic<T, U>(value: T) -> T { return value }\n"
        "function sample(value: Left) -> Int64 { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolTypeId left = SOL_AST_NONE;
    SolTypeId integer = SOL_AST_NONE;
    SolTypeId parameter = SOL_AST_NONE;
    for (SolTypeId type = 0; type < compilation.syntax.type_count; ++type) {
        if (span_text_equal(&compilation.source, compilation.syntax.types[type].name, "Left")) {
            left = type;
        } else if (span_text_equal(
            &compilation.source,
            compilation.syntax.types[type].name,
            "Int64"
        )) {
            integer = type;
        } else if (span_text_equal(
            &compilation.source,
            compilation.syntax.types[type].name,
            "T"
        )) {
            parameter = type;
        }
    }
    CHECK(left != SOL_AST_NONE);
    CHECK(integer != SOL_AST_NONE);
    CHECK(parameter != SOL_AST_NONE);

    if (left != SOL_AST_NONE) {
        SolTypeResolution original = compilation.hir.type_resolutions[left];
        compilation.hir.type_resolutions[left]
            = (SolTypeResolution){SOL_TYPE_RESOLUTION_DEFINITION, 1};
        CHECK(!rerun_typecheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-003"));
        compilation.hir.type_resolutions[left] = original;
    }
    if (integer != SOL_AST_NONE) {
        SolTypeResolution original = compilation.hir.type_resolutions[integer];
        compilation.hir.type_resolutions[integer]
            = (SolTypeResolution){SOL_TYPE_RESOLUTION_BUILTIN, SOL_TYPE_BUILTIN_BOOL};
        CHECK(!rerun_typecheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-003"));
        compilation.hir.type_resolutions[integer] = original;
    }
    if (parameter != SOL_AST_NONE) {
        SolTypeResolution original = compilation.hir.type_resolutions[parameter];
        compilation.hir.type_resolutions[parameter]
            = (SolTypeResolution){SOL_TYPE_RESOLUTION_PARAMETER, 1};
        CHECK(!rerun_typecheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-003"));
        compilation.hir.type_resolutions[parameter] = original;
    }
    if (left != SOL_AST_NONE) {
        compilation.hir.type_resolutions[left]
            = (SolTypeResolution){SOL_TYPE_RESOLUTION_ERROR, SOL_AST_NONE};
        CHECK(!rerun_typecheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-003"));
    }
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
        const SolType *arguments = NULL;
        size_t count = 0;
        CHECK(option->constructor == SOL_TYPE_CONSTRUCTOR_OPTION);
        CHECK(sol_type_application_arguments(
            &compilation.types,
            compilation.types.definitions[1],
            &arguments,
            &count
        ));
        CHECK(count == 1);
        if (arguments != NULL) CHECK(arguments[0].kind == SOL_TYPE_INT64);
    }
    if (result != NULL) {
        const SolType *arguments = NULL;
        size_t count = 0;
        CHECK(result->constructor == SOL_TYPE_CONSTRUCTOR_RESULT);
        CHECK(sol_type_application_arguments(
            &compilation.types,
            compilation.types.definitions[2],
            &arguments,
            &count
        ));
        CHECK(count == 2);
        if (arguments != NULL) {
            CHECK(arguments[0].kind == SOL_TYPE_INT64);
            CHECK(arguments[1].kind == SOL_TYPE_NOMINAL);
            CHECK(arguments[1].definition == 0);
        }
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

static void test_user_generics_and_instantiations(void) {
    static const char text[] =
        "module user_generics\n"
        "record Box<T> { value: T, }\n"
        "enum Either<L, R> { left(value: L), right(value: R), empty, }\n"
        "function identity<T>(value: T) -> T { return value }\n"
        "function unbox<T>(box: Box<T>) -> T { return box.value }\n"
        "function explicit() -> Int64 { return identity<Int64>(1) }\n"
        "function inferred() -> Text { return identity(\"text\") }\n"
        "function named() -> Int64 { return identity(value = 2) }\n"
        "function boxed() -> Box<Int64> { return Box<Int64> { value = 3, } }\n"
        "function nested() -> Int64 { return unbox(Box<Int64> { value = 4, }) }\n"
        "function left() -> Either<Int64, Text> {\n"
        "    return Either<Int64, Text>.left(value = 5)\n"
        "}\n"
        "function right() -> Either<Int64, Text> {\n"
        "    return Either<Int64, Text>.right(value = \"right\")\n"
        "}\n"
        "function read(value: Either<Int64, Text>) -> Int64 {\n"
        "    return match value {\n"
        "        left(number) => number\n"
        "        right(message) => 0\n"
        "        empty => 1\n"
        "    }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    size_t generic_calls = 0;
    bool saw_explicit = false;
    bool saw_nested = false;
    for (size_t expression = 0; expression < compilation.syntax.expression_count; ++expression) {
        const SolCallInstantiation *instantiation = sol_type_call_instantiation(
            &compilation.types,
            expression
        );
        if (instantiation == NULL) continue;
        const SolType *arguments = NULL;
        size_t argument_count = 0;
        CHECK(instantiation->argument_count == 1);
        CHECK(sol_type_call_instantiation_arguments(
            &compilation.types,
            expression,
            &arguments,
            &argument_count
        ));
        CHECK(argument_count == 1);
        if (arguments != NULL) {
            CHECK(arguments[0].kind == SOL_TYPE_INT64
                || arguments[0].kind == SOL_TYPE_TEXT);
        }
        saw_explicit = saw_explicit
            || compilation.syntax.expressions[
                compilation.syntax.expressions[expression].as.call.callee
            ].kind == SOL_EXPR_TYPE_APPLICATION;
        saw_nested = saw_nested || instantiation->function == 3;
        ++generic_calls;
    }
    CHECK(generic_calls == 4);
    CHECK(saw_explicit);
    CHECK(saw_nested);

    SolType boxed = compilation.types.definitions[7];
    const SolType *arguments = NULL;
    size_t argument_count = 0;
    CHECK(sol_type_application_arguments(
        &compilation.types,
        boxed,
        &arguments,
        &argument_count
    ));
    CHECK(argument_count == 1);
    if (arguments != NULL) CHECK(arguments[0].kind == SOL_TYPE_INT64);
    const SolTypeApplication *application = sol_type_application(&compilation.types, boxed);
    CHECK(application != NULL);
    if (application != NULL) {
        CHECK(application->constructor == SOL_TYPE_CONSTRUCTOR_USER);
        CHECK(application->definition == 0);
    }
    free_compilation(&compilation);
}

static void test_nested_generic_substitution_growth(void) {
    static const char text[] =
        "module nested_substitution\n"
        "enum Empty {}\n"
        "function build<T>(empty: Empty, value: T)\n"
        "    -> Option<Option<Option<Option<Option<Option<Option<Option<Option<Option<Option<Option<T>>>>>>>>>>>> {\n"
        "    return match empty {}\n"
        "}\n"
        "function probe(empty: Empty) -> () {\n"
        "    let built = build(empty, 1)\n"
        "    return ()\n"
        "}\n"
        "function marker<T>() -> () { return () }\n"
        "function marker_probe() -> () { return marker<Int64>() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.types.type_application_count >= 24);
    free_compilation(&compilation);
}

static void test_generic_variant_constructor_identity(void) {
    static const char text[] =
        "module generic_variants\n"
        "enum Choice<T> { some(value: T), }\n"
        "function integer() -> Choice<Int64> {\n"
        "    let first = Choice<Int64>.some\n"
        "    let second = Choice<Int64>.some\n"
        "    return first(value = 1)\n"
        "}\n"
        "function text() -> Choice<Text> { return Choice<Text>.some(value = \"x\") }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.types.variant_constructor_count == 2);
    if (compilation.types.variant_constructor_count == 2) {
        const SolVariantConstructor *left = &compilation.types.variant_constructors[0];
        const SolVariantConstructor *right = &compilation.types.variant_constructors[1];
        CHECK(left->variant == right->variant);
        CHECK(left->owner.kind == SOL_TYPE_APPLICATION);
        CHECK(right->owner.kind == SOL_TYPE_APPLICATION);
        CHECK(left->owner.definition != right->owner.definition);
        bool saw_int = false;
        bool saw_text = false;
        for (size_t index = 0; index < 2; ++index) {
            const SolType *arguments = NULL;
            size_t argument_count = 0;
            CHECK(sol_type_application_arguments(
                &compilation.types,
                compilation.types.variant_constructors[index].owner,
                &arguments,
                &argument_count
            ));
            CHECK(argument_count == 1);
            if (arguments != NULL) {
                saw_int = saw_int || arguments[0].kind == SOL_TYPE_INT64;
                saw_text = saw_text || arguments[0].kind == SOL_TYPE_TEXT;
            }
        }
        CHECK(saw_int);
        CHECK(saw_text);
    }
    free_compilation(&compilation);
}

static void test_generic_function_signature_substitution_and_inference(void) {
    static const char text[] =
        "module generic_callbacks\n"
        "enum Empty {}\n"
        "function choose<T>(\n"
        "    callback: function(T) -> T effects { pure },\n"
        "    empty: Empty,\n"
        ") -> T effects { pure } { return match empty {} }\n"
        "function explicit(\n"
        "    callback: function(Int64) -> Int64 effects { pure },\n"
        "    empty: Empty,\n"
        ") -> Int64 effects { pure } { return choose<Int64>(callback, empty) }\n"
        "function inferred(\n"
        "    callback: function(Text) -> Text effects { pure },\n"
        "    empty: Empty,\n"
        ") -> Text effects { pure } { return choose(callback, empty) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    size_t calls = 0;
    bool saw_int = false;
    bool saw_text = false;
    for (SolExprId expression = 0; expression < compilation.syntax.expression_count;
        ++expression) {
        const SolCallInstantiation *instantiation = sol_type_call_instantiation(
            &compilation.types,
            expression
        );
        if (instantiation == NULL || instantiation->function != 1) continue;
        const SolType *arguments = NULL;
        size_t argument_count = 0;
        CHECK(sol_type_call_instantiation_arguments(
            &compilation.types,
            expression,
            &arguments,
            &argument_count
        ));
        CHECK(argument_count == 1);
        if (arguments != NULL) {
            saw_int = saw_int || arguments[0].kind == SOL_TYPE_INT64;
            saw_text = saw_text || arguments[0].kind == SOL_TYPE_TEXT;
        }
        ++calls;
    }
    CHECK(calls == 2);
    CHECK(saw_int);
    CHECK(saw_text);
    free_compilation(&compilation);
}

static void test_effect_row_type_boundaries(void) {
    static const char open_to_closed[] =
        "module open_to_closed\n"
        "function closed(callback: function() -> Int64 effects { pure }) -> Int64 { return callback() }\n"
        "function bad<effects E>(callback: function() -> Int64 effects E) -> Int64 effects { E } { return closed(callback) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, open_to_closed));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-005"));
    free_compilation(&compilation);

    static const char recursive[] =
        "module recursive_row\n"
        "function bad<effects E>(callback: function() -> Int64 effects E) -> Int64 effects { E } { return bad(callback) }\n";
    CHECK(compile_source(&compilation, recursive));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-019"));
    free_compilation(&compilation);

    static const char forged[] =
        "module forged_row_type\n"
        "function apply<effects E>(callback: function() -> Int64 effects E) -> Int64 effects { E } { return callback() }\n";
    CHECK(compile_source(&compilation, forged));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolTypeId tail = SOL_AST_NONE;
    for (SolTypeId type = 0; type < compilation.syntax.type_count; ++type) {
        if (compilation.syntax.types[type].has_effect_tail) {
            tail = type;
            break;
        }
    }
    CHECK(tail != SOL_AST_NONE);
    if (tail != SOL_AST_NONE) {
        sol_type_table_free(&compilation.types);
        sol_type_table_init(&compilation.types);
        sol_diagnostics_free(&compilation.diagnostics);
        sol_diagnostics_init(&compilation.diagnostics);
        compilation.hir.type_effect_resolutions[tail] = (SolEffectResolution){
            SOL_EFFECT_RESOLUTION_ERROR,
            SOL_AST_NONE,
        };
        CHECK(!sol_type_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.diagnostics
        ));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-003"));
    }
    free_compilation(&compilation);
}

static void test_bounded_callback_effect_authority(void) {
    static const char invalid[] =
        "module invalid_callback_authority\n"
        "function unparameterized(\n"
        "    callback: function() -> Int64 effects { service.read },\n"
        ") -> Int64 { return 1 }\n"
        "function static_path(\n"
        "    callback: function() -> Int64 effects { database.read<Primary> },\n"
        ") -> Int64 { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, invalid));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-010") == 2);
    free_compilation(&compilation);

    static const char valid[] =
        "module authority_free_callbacks\n"
        "function keep_panic(\n"
        "    callback: function() -> Int64 effects { panic },\n"
        ") -> function() -> Int64 effects { panic } { return callback }\n"
        "function keep_diverge(\n"
        "    callback: function() -> Int64 effects { diverge },\n"
        ") -> function() -> Int64 effects { diverge } { return callback }\n";
    CHECK(compile_source(&compilation, valid));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);
}

static void test_invalid_capability_qualified_generic_types(void) {
    static const char text[] =
        "module generic_capabilities\n"
        "record Box<T> { value: T, }\n"
        "capability Clock {}\n"
        "function parameter<T>(value: capability T) -> T { return value }\n"
        "function application(value: capability Box<Int64>) -> Int64 { return 1 }\n"
        "function builtin(value: capability Option<Int64>) -> Int64 { return 1 }\n"
        "function valid(clock: capability Clock) -> Int64 { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-TYPE-009") >= 3);
    free_compilation(&compilation);
}

static void test_generic_recursion_through_nongeneric_helper(void) {
    static const char text[] =
        "module indirect_generic_recursion\n"
        "function generic<T>(value: T) -> T { let ignored = helper() return value }\n"
        "function helper() -> Int64 { return generic(1) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-019"));
    free_compilation(&compilation);
}

static void test_invalid_user_generics(void) {
    static const char text[] =
        "module invalid_user_generics\n"
        "record Box<T> { value: T, }\n"
        "record Plain { value: Int64, }\n"
        "enum Either<L, R> { left(value: L), right(value: R), }\n"
        "enum Empty {}\n"
        "function same<T>(left: T, right: T) -> T { return left }\n"
        "function first<T, U>(left: T, right: U) -> T { return left }\n"
        "function result_only<T>(empty: Empty) -> T { return match empty {} }\n"
        "function conflict() -> Int64 { return same(1, true) }\n"
        "function uninferred(empty: Empty) -> Int64 { return result_only(empty) }\n"
        "function explicit_arity() -> Int64 { return same<Int64, Bool>(1, 2) }\n"
        "function partial() -> Int64 { return first<Int64>(1, true) }\n"
        "function wildcard() -> Int64 { return same<_>(1, 2) }\n"
        "function recur<T>(value: T) -> T { return recur(value) }\n"
        "function recursive_left<T>(value: T) -> T { return recursive_right(value) }\n"
        "function recursive_right<T>(value: T) -> T { return recursive_left(value) }\n"
        "function consume(callback: function(Int64) -> Int64 effects { pure }) -> Int64 {\n"
        "    return callback(1)\n"
        "}\n"
        "function generic_callback() -> Int64 { return consume(same) }\n"
        "function invalid_effect_authority<T>(\n"
        "    callback: function() -> T effects { service.read<T> },\n"
        ") -> T { return callback() }\n"
        "function wrong_box() -> Box<Int64> { return Box<Int64> { value = true } }\n"
        "function wrong_enum() -> Either<Int64, Text> {\n"
        "    return Either<Int64, Text>.left(value = true)\n"
        "}\n"
        "function bare_type(value: Box) -> Box { return value }\n"
        "function applied_plain(value: Plain<Int64>) -> Plain<Int64> { return value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-017"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-018"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-019"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-020"));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-010"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-013"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-014"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-009"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-016"));
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
                CHECK(provenance_equals(
                    &compilation.types,
                    compilation.types.local_capability_origins[index],
                    &origin,
                    1
                ));
                ++capability_locals;
            }
            if (compilation.types.local_operation_origins[index] != SOL_AST_NONE) {
                CHECK(provenance_equals(
                    &compilation.types,
                    compilation.types.local_operation_origins[index],
                    &origin,
                    1
                ));
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
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolParameterId first = compilation.syntax.parameters[
        compilation.syntax.items[1].first_parameter
    ].next;
    SolParameterId roots[] = {
        first,
        compilation.syntax.parameters[first].next,
    };
    bool found_mixed = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind == SOL_EXPR_IF
            && compilation.types.expressions[index].kind == SOL_TYPE_NOMINAL) {
            CHECK(provenance_equals(
                &compilation.types,
                compilation.types.expression_capability_origins[index],
                roots,
                2
            ));
            found_mixed = true;
        }
    }
    CHECK(found_mixed);
    free_compilation(&compilation);

    static const char operation_text[] =
        "module mixed_operation\n"
        "capability Clock { function offset(delta: Int64) -> Int64 effects { pure } }\n"
        "function mixed(flag: Bool, first: capability Clock, second: capability Clock)\n"
        "-> Int64 { return (if flag { first.offset } else { second.offset })(1) }\n";
    CHECK(compile_source(&compilation, operation_text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    first = compilation.syntax.parameters[compilation.syntax.items[1].first_parameter].next;
    roots[0] = first;
    roots[1] = compilation.syntax.parameters[first].next;
    found_mixed = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind == SOL_EXPR_IF
            && compilation.types.expressions[index].kind
                == SOL_TYPE_CAPABILITY_OPERATION) {
            CHECK(provenance_equals(
                &compilation.types,
                compilation.types.expression_operation_origins[index],
                roots,
                2
            ));
            found_mixed = true;
        }
    }
    CHECK(found_mixed);
    free_compilation(&compilation);
}

static void test_mixed_match_alias_and_never_provenance(void) {
    static const char text[] =
        "module mixed_match_provenance\n"
        "enum Empty {}\n"
        "capability Clock { function now() -> Int64 effects { pure } }\n"
        "function sample(\n"
        "    flag: Bool, impossible: Empty,\n"
        "    first: capability Clock, second: capability Clock,\n"
        ") -> Int64 {\n"
        "    let selected = match flag { true => first false => second }\n"
        "    let operation = match flag { true => selected.now false => first.now }\n"
        "    let reachable = if flag { match impossible {} } else { selected }\n"
        "    return operation() + reachable.now()\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolParameterId parameter = compilation.syntax.items[2].first_parameter;
    parameter = compilation.syntax.parameters[parameter].next;
    SolParameterId roots[] = {
        compilation.syntax.parameters[parameter].next,
        compilation.syntax.parameters[
            compilation.syntax.parameters[parameter].next
        ].next,
    };
    size_t mixed = 0;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        SolProvenanceId id = compilation.types.expressions[index].kind
                == SOL_TYPE_CAPABILITY_OPERATION
            ? compilation.types.expression_operation_origins[index]
            : compilation.types.expression_capability_origins[index];
        if (provenance_equals(&compilation.types, id, roots, 2)) ++mixed;
    }
    CHECK(mixed >= 5);
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
        "        callback: function(Int64) -> Bool effects { panic },\n"
        "    ) -> function(Int64) -> Bool effects { panic } effects { pure }\n"
        "}\n"
        "function keep(\n"
        "    callback: function(Int64) -> Bool effects {\n"
        "        panic\n"
        "        diverge\n"
        "    },\n"
        ") -> function(Int64) -> Bool effects {\n"
        "    diverge\n"
        "    panic\n"
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
        ") -> function() -> Int64 effects { panic } { return callback }\n";
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
        "    callback: function(Int64) -> Bool effects { panic },\n"
        ") -> function(Int64) -> Bool effects { diverge } { return callback }\n"
        "function invalid_rows(\n"
        "    duplicate: function() -> Int64 effects { panic panic },\n"
        "    mixed: function() -> Int64 effects { pure panic },\n"
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

    static const char mixed_text[] =
        "module mixed_return_authority\n"
        "capability Root {}\n"
        "function choose(\n"
        "    flag: Bool, claimed: capability Root, other: capability Root,\n"
        ") -> capability Root\n"
        "authority { result derives_from claimed }\n"
        "effects { pure } { return if flag { claimed } else { other } }\n";
    CHECK(compile_source(&compilation, mixed_text));
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
        "    other_provider: capability TestClock,\n"
        ") -> Text {\n"
        "    let selected = if choose { provider } else { other_provider }\n"
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
        "capability Borrowed { function read(value: borrow Text) -> Int64 effects { service.read<Self> } }\n"
        "capability Exclusive { function read(value: inout Text) -> Int64 effects { pure } }\n"
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
        "}\n"
        "function access_mismatch(authority: capability Borrowed, provider: capability Exclusive) -> Int64 {\n"
        "    return handle service.read<authority> with provider { 1 }\n"
        "}\n"
        "function dynamic(\n"
        "    flag: Bool, first: capability Impure, second: capability Impure,\n"
        "    provider: capability Wrong,\n"
        ") -> Int64 {\n"
        "    return handle service.read<first> with provider {\n"
        "        handle service.read<if flag { first } else { second }> with provider { 1 }\n"
        "    }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-HANDLER-001"));
    CHECK(diagnostic_count(&compilation, "SOL-HANDLER-001") >= 5);
    bool found_dynamic = false;
    for (size_t index = 0; index < compilation.diagnostics.count; ++index) {
        found_dynamic = found_dynamic || strstr(
            compilation.diagnostics.items[index].message,
            "dynamic authority matching is unsupported"
        ) != NULL;
    }
    CHECK(found_dynamic);
    free_compilation(&compilation);

    CHECK(compile_source(&compilation,
        "module handler_access_mismatch\n"
        "capability Source { function read(value: borrow Text) -> Int64 "
        "effects { service.read<Self> } }\n"
        "capability Provider { function read(value: inout Text) -> Int64 "
        "effects { pure } }\n"
        "function bad(authority: capability Source, provider: capability Provider) "
        "-> Int64 { return handle service.read<authority> with provider { 1 } }\n"));
    CHECK(has_diagnostic(&compilation, "SOL-HANDLER-001"));
    CHECK(!has_diagnostic(&compilation, "SOL-INTERNAL-006"));
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

static void test_traits_bounds_and_method_metadata(void) {
    static const char valid[] =
        "module traits\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "implementation Show for Int64 { function show(self: Self) -> Text effects { pure } { return \"ok\" } }\n"
        "function render<T: Show>(value: T) -> Text effects { pure } { return value.show() }\n"
        "function concrete(value: Int64) -> Text effects { pure } { return value.show() }\n"
        "function instantiate(value: Int64) -> Text effects { pure } { return render(value) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, valid));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    size_t requirements = 0;
    size_t implementations = 0;
    for (SolExprId call = 0; call < compilation.types.method_resolution_count; ++call) {
        const SolMethodResolution *method = sol_type_method_resolution(&compilation.types, call);
        if (method == NULL) continue;
        requirements += method->kind == SOL_METHOD_RESOLUTION_REQUIREMENT;
        implementations += method->kind == SOL_METHOD_RESOLUTION_IMPLEMENTATION;
    }
    CHECK(requirements == 1);
    CHECK(implementations == 1);
    compilation.hir.bound_resolutions[0].target = compilation.syntax.item_count;
    CHECK(!rerun_typecheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-003"));
    free_compilation(&compilation);

    static const char bad_bound[] =
        "module bad_bound\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "function render<T: Show>(value: T) -> Text effects { pure } { return value.show() }\n"
        "function bad(value: Bool) -> Text effects { pure } { return render(value) }\n";
    CHECK(compile_source(&compilation, bad_bound));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-022"));
    free_compilation(&compilation);

    static const char mismatch[] =
        "module mismatch\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "implementation Show for Int64 { function show(self: Self) -> Int64 effects { pure } { return 1 } }\n";
    CHECK(compile_source(&compilation, mismatch));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-023"));
    free_compilation(&compilation);

    static const char method_value[] =
        "module method_value\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "implementation Show for Int64 { function show(self: Self) -> Text effects { pure } { return \"ok\" } }\n"
        "function bad(value: Int64) -> Int64 effects { pure } { let method = value.show return 1 }\n";
    CHECK(compile_source(&compilation, method_value));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-021"));
    free_compilation(&compilation);
}

static void test_nested_self_substitution_growth(void) {
    static const char text[] =
        "module nested_self\n"
        "record Pair<A, B> { left: A, right: B }\n"
        "trait Nested {\n"
        " function nested(self: Self, value: Pair<Pair<Pair<Pair<Pair<Pair<Pair<Pair<Pair<Self, Self>, Self>, Self>, Self>, Self>, Self>, Self>, Self>, Self>) -> Self effects { pure }\n"
        "}\n"
        "implementation Nested for Int64 {\n"
        " function nested(self: Self, value: Pair<Pair<Pair<Pair<Pair<Pair<Pair<Pair<Pair<Self, Self>, Self>, Self>, Self>, Self>, Self>, Self>, Self>, Self>) -> Self effects { pure } { return self }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.types.type_application_count > 16);
    CHECK(compilation.types.type_application_argument_count > 32);
    free_compilation(&compilation);
}

static void test_malformed_trait_method_links(void) {
    static const char text[] =
        "module malformed_traits\n"
        "trait First { function first(self: Self) -> Int64 effects { pure } }\n"
        "trait Second { function second(self: Self) -> Int64 effects { pure } }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolTraitMethodId first = compilation.syntax.items[0].first_trait_method;
    SolTraitMethodId second = compilation.syntax.items[1].first_trait_method;

    compilation.syntax.trait_methods[first].next = first;
    CHECK(!rerun_typecheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-003"));
    compilation.syntax.trait_methods[first].next = SOL_AST_NONE;

    compilation.syntax.trait_methods[first].next = second;
    CHECK(!rerun_typecheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-003"));
    compilation.syntax.trait_methods[first].next = SOL_AST_NONE;

    compilation.syntax.items[1].first_trait_method = SOL_AST_NONE;
    CHECK(!rerun_typecheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-003"));
    free_compilation(&compilation);
}

static void test_computed_enum_receiver_methods(void) {
    static const char text[] =
        "module enum_methods\n"
        "enum Choice { left, right }\n"
        "trait Tag { function tag(self: Self) -> Int64 effects { pure } }\n"
        "implementation Tag for Choice { function tag(self: Self) -> Int64 effects { pure } { return 1 } }\n"
        "function make() -> Choice effects { pure } { return Choice.left }\n"
        "function from_call() -> Int64 effects { pure } { return make().tag() }\n"
        "function from_if(flag: Bool) -> Int64 effects { pure } { return (if flag { Choice.left } else { Choice.right }).tag() }\n"
        "function from_block() -> Int64 effects { pure } { return ({ Choice.left }).tag() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    size_t methods = 0;
    for (SolExprId expression = 0; expression < compilation.syntax.expression_count;
        ++expression) {
        methods += sol_type_method_resolution(&compilation.types, expression) != NULL;
    }
    CHECK(methods == 3);
    free_compilation(&compilation);
}

static void test_bound_evidence_forwarding(void) {
    static const char valid[] =
        "module bound_forwarding\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "function inner<U: Show>(value: U) -> Text effects { pure } { return value.show() }\n"
        "function inferred<T: Show>(value: T) -> Text effects { pure } { return inner(value) }\n"
        "function explicit<T: Show>(value: T) -> Text effects { pure } { return inner<T>(value) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, valid));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);

    static const char different[] =
        "module different_bound\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "trait Other { function other(self: Self) -> Text effects { pure } }\n"
        "function inner<U: Show>(value: U) -> Text effects { pure } { return value.show() }\n"
        "function outer<T: Other>(value: T) -> Text effects { pure } { return inner(value) }\n";
    CHECK(compile_source(&compilation, different));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-022"));
    free_compilation(&compilation);

    static const char unbounded[] =
        "module no_bound\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "function inner<U: Show>(value: U) -> Text effects { pure } { return value.show() }\n"
        "function outer<T>(value: T) -> Text effects { pure } { return inner<T>(value) }\n";
    CHECK(compile_source(&compilation, unbounded));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-022"));
    free_compilation(&compilation);
}

static void test_method_named_arguments(void) {
    static const char valid[] =
        "module method_names\n"
        "trait Mix { function mix(self: Self, left: Int64, right: Bool) -> Int64 effects { pure } }\n"
        "implementation Mix for Int64 { function mix(self: Self, left: Int64, right: Bool) -> Int64 effects { pure } { return left } }\n"
        "function call(value: Int64) -> Int64 effects { pure } { return value.mix(right = true, left = 2) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, valid));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);

    static const char *invalid[] = {
        "module unknown\ntrait Mix { function mix(self: Self, left: Int64, right: Bool) -> Int64 effects { pure } }\nimplementation Mix for Int64 { function mix(self: Self, left: Int64, right: Bool) -> Int64 effects { pure } { return left } }\nfunction bad(value: Int64) -> Int64 effects { pure } { return value.mix(missing = 1, right = true) }\n",
        "module duplicate\ntrait Mix { function mix(self: Self, left: Int64, right: Bool) -> Int64 effects { pure } }\nimplementation Mix for Int64 { function mix(self: Self, left: Int64, right: Bool) -> Int64 effects { pure } { return left } }\nfunction bad(value: Int64) -> Int64 effects { pure } { return value.mix(left = 1, left = 2) }\n",
        "module missing\ntrait Mix { function mix(self: Self, left: Int64, right: Bool) -> Int64 effects { pure } }\nimplementation Mix for Int64 { function mix(self: Self, left: Int64, right: Bool) -> Int64 effects { pure } { return left } }\nfunction bad(value: Int64) -> Int64 effects { pure } { return value.mix(left = 1) }\n",
        "module positional_after\ntrait Mix { function mix(self: Self, left: Int64, right: Bool) -> Int64 effects { pure } }\nimplementation Mix for Int64 { function mix(self: Self, left: Int64, right: Bool) -> Int64 effects { pure } { return left } }\nfunction bad(value: Int64) -> Int64 effects { pure } { return value.mix(left = 1, true) }\n",
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        CHECK(compile_source(&compilation, invalid[index]));
        CHECK(has_diagnostic(&compilation,
            index == 2 ? "SOL-TYPE-006" : "SOL-TYPE-012"));
        free_compilation(&compilation);
    }
}

static void test_distinct_type_identity_and_construction(void) {
    static const char text[] =
        "module distinct_types\n"
        "type UserId = distinct Int64\n"
        "type Count = distinct Int64\n"
        "type Wrapped<T> = distinct Option<T>\n"
        "trait Tag { function tag(self: Self) -> Int64 effects { pure } }\n"
        "implementation Tag for UserId {\n"
        "    function tag(self: Self) -> Int64 effects { pure } { return 1 }\n"
        "}\n"
        "implementation Tag for Wrapped<Int64> {\n"
        "    function tag(self: Self) -> Int64 effects { pure } { return 2 }\n"
        "}\n"
        "function user() -> UserId { return UserId(1) }\n"
        "function count() -> Count { return Count(1) }\n"
        "function wrapped(value: Option<Int64>) -> Wrapped<Int64> {\n"
        "    return Wrapped<Int64>(value)\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.types.definitions[0].kind == SOL_TYPE_NOMINAL);
    CHECK(compilation.types.definitions[1].kind == SOL_TYPE_NOMINAL);
    CHECK(compilation.types.definitions[0].definition != compilation.types.definitions[1].definition);
    const SolTypeRepresentation *user = sol_type_representation(&compilation.types, 0);
    const SolTypeRepresentation *wrapped = sol_type_representation(&compilation.types, 2);
    CHECK(user != NULL);
    CHECK(wrapped != NULL);
    if (user != NULL) CHECK(user->representation.kind == SOL_TYPE_INT64);
    if (wrapped != NULL) CHECK(wrapped->representation.kind == SOL_TYPE_APPLICATION);
    size_t constructions = 0;
    bool generic = false;
    for (SolExprId expression = 0; expression < compilation.syntax.expression_count;
        ++expression) {
        const SolTypeConstruction *construction = sol_type_construction(
            &compilation.types, expression
        );
        if (construction == NULL) continue;
        generic = generic || construction->result.kind == SOL_TYPE_APPLICATION;
        ++constructions;
    }
    CHECK(constructions == 3);
    CHECK(generic);
    CHECK(sol_type_exact_reference_valid(
        &compilation.syntax, &compilation.types, compilation.types.definitions[0]
    ));
    CHECK(compilation.types.implementation_targets[4].kind == SOL_TYPE_NOMINAL);
    CHECK(compilation.types.implementation_targets[5].kind == SOL_TYPE_APPLICATION);
    free_compilation(&compilation);
}

static void test_refined_type_predicates_and_construction_rejection(void) {
    static const char valid[] =
        "module refined_self\n"
        "type Positive = refined Int64 where self > 0\n"
        "type Identity<T> = refined T where true\n"
        "function retain(value: Identity<Int64>) -> Identity<Int64> { return value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, valid));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolExprId predicate = compilation.syntax.contract_conditions[0].expression;
    CHECK(compilation.types.expressions[predicate].kind == SOL_TYPE_BOOL);
    free_compilation(&compilation);

    static const char invalid[] =
        "module invalid_refined\n"
        "type NotBool = refined Int64 where self\n"
        "type Positive = refined Int64 where self > 0\n"
        "function bad() -> Positive { return Positive(1) }\n";
    CHECK(compile_source(&compilation, invalid));
    CHECK(has_diagnostic(&compilation, "SOL-CONTRACT-001"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-024"));
    bool bootstrap_message = false;
    for (size_t index = 0; index < compilation.diagnostics.count; ++index) {
        bootstrap_message = bootstrap_message || strstr(
            compilation.diagnostics.items[index].message,
            "unsupported by this bootstrap"
        ) != NULL;
    }
    CHECK(bootstrap_message);
    free_compilation(&compilation);
}

static void test_declared_constructor_requires_definition_head(void) {
    static const char text[] =
        "module constructor_head\n"
        "type Id = distinct Int64\n"
        "function existing() -> Id { return Id(1) }\n"
        "function bad() -> Id { return existing()(2) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-010"));
    size_t constructions = 0;
    for (SolExprId expression = 0; expression < compilation.types.construction_count;
        ++expression) {
        constructions += sol_type_construction(&compilation.types, expression) != NULL;
    }
    CHECK(constructions == 1);
    free_compilation(&compilation);
}

static void test_invalid_type_representations_and_constructors(void) {
    static const char text[] =
        "module invalid_type_items\n"
        "capability Clock {}\n"
        "type A = distinct B\n"
        "type B = distinct A\n"
        "type Authority = distinct capability Clock\n"
        "type Box<T> = distinct T\n"
        "function implicit(value: Int64) -> A { return value }\n"
        "function bare(value: Int64) -> Box<Int64> { return Box(value) }\n"
        "function named() -> Box<Int64> { return Box<Int64>(value = 1) }\n"
        "function wrong() -> Box<Int64> { return Box<Int64>(true) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-TYPE-024") >= 5);
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-004"));
    free_compilation(&compilation);
}

static void test_generic_representation_safety(void) {
    static const char text[] =
        "module generic_representation_safety\n"
        "capability Clock {}\n"
        "record Box<T> { value: T, }\n"
        "enum Envelope<T> { value(item: T), fixed(clock: capability Clock), }\n"
        "record Phantom<T> {}\n"
        "type Laundered<T> = distinct Box<T>\n"
        "type Fixed<T> = distinct Envelope<T>\n"
        "type SafePhantom<T> = distinct Phantom<T>\n"
        "function safe(clock: capability Clock) -> SafePhantom<capability Clock> {\n"
        "    return SafePhantom<capability Clock>(Phantom<capability Clock> {})\n"
        "}\n"
        "function bad(clock: capability Clock) -> Laundered<capability Clock> {\n"
        "    return Laundered<capability Clock>(Box<capability Clock> { value = clock, })\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-TYPE-024") >= 2);
    CHECK(!has_diagnostic(&compilation, "SOL-INTERNAL-003"));
    free_compilation(&compilation);
}

static void test_representation_storage_cycles(void) {
    static const char text[] =
        "module representation_storage_cycles\n"
        "record RecordStorage { value: RecordCycle, }\n"
        "type RecordCycle = distinct RecordStorage\n"
        "enum EnumStorage { value(item: EnumCycle), }\n"
        "type EnumCycle = distinct EnumStorage\n"
        "record OrdinaryRecord { next: OrdinaryRecord, }\n"
        "enum OrdinaryEnum { next(value: OrdinaryEnum), }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-TYPE-024") == 2);
    free_compilation(&compilation);
}

static void test_type_metadata_defensive_validation(void) {
    static const char text[] =
        "module type_metadata\n"
        "type Id = distinct Int64\n"
        "function make() -> Id { return Id(1) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(sol_type_representation(&compilation.types, 0) != NULL);
    SolExprId construction = SOL_AST_NONE;
    for (SolExprId expression = 0; expression < compilation.syntax.expression_count;
        ++expression) {
        if (sol_type_construction(&compilation.types, expression) != NULL) {
            construction = expression;
            break;
        }
    }
    CHECK(construction != SOL_AST_NONE);
    if (construction != SOL_AST_NONE) {
        compilation.types.representations[0].flavor = SOL_TYPE_DECLARATION_REFINED;
        CHECK(sol_type_construction(&compilation.types, construction) == NULL);
        compilation.types.representations[0].flavor = SOL_TYPE_DECLARATION_DISTINCT;
    }
    compilation.types.representations[0].representation.kind = SOL_TYPE_ERROR;
    CHECK(sol_type_representation(&compilation.types, 0) == NULL);
    if (construction != SOL_AST_NONE) {
        CHECK(sol_type_construction(&compilation.types, construction) == NULL);
    }
    compilation.types.representation_count = compilation.types.definition_count + 1;
    CHECK(sol_type_representation(&compilation.types, 0) == NULL);
    free_compilation(&compilation);
}

static void test_runtime_identity_equality_rejected(void) {
    static const char text[] =
        "module equality_domains\n"
        "capability Token { function read() -> Int64 effects { pure } }\n"
        "record Holder { token: capability Token }\n"
        "record Outer { holder: Holder }\n"
        "function function_value(value: function() -> Int64 effects { pure }) -> Bool "
        "effects { pure } { return value == value }\n"
        "function capability_value(value: capability Token) -> Bool effects { pure } "
        "{ return value == value }\n"
        "function operation_value(value: capability Token) -> Bool effects { pure } "
        "{ return value.read == value.read }\n"
        "function aggregate_value(value: Outer) -> Bool effects { pure } "
        "{ return value == value }\n"
        "function generic_value<T>(left: T, right: T) -> Bool effects { pure } "
        "{ return left == right }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-TYPE-027") == 5);
    CHECK(!has_diagnostic(&compilation, "SOL-TYPE-002"));
    free_compilation(&compilation);
}

static void test_access_modes_are_exact(void) {
    TestCompilation compilation;
    CHECK(compile_source(&compilation,
        "module access_exact\n"
        "trait Show { function show(self: borrow Self) -> Text effects { pure } }\n"
        "implementation Show for Int64 { "
        "function show(self: borrow Self) -> Text effects { pure } { return \"ok\" } }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);

    CHECK(compile_source(&compilation,
        "module generic_access\n"
        "function apply<T>(callback: function(borrow T) -> Int64 effects { pure }, "
        "value: borrow T) -> Int64 effects { pure } { return callback(value) }\n"
        "function inspect(value: borrow Text) -> Int64 effects { pure } { return 1 }\n"
        "function shared_callback(callback: function(borrow Text) -> Int64 "
        "effects { pure }) -> Int64 effects { pure } { return 1 }\n"
        "function exclusive_callback(callback: function(inout Text) -> Int64 "
        "effects { pure }) -> Int64 effects { pure } { return 2 }\n"
        "function good(value: borrow Text) -> Int64 effects { pure } "
        "{ return apply<Text>(inspect, value) }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);

    CHECK(compile_source(&compilation,
        "module generic_access_mismatch\n"
        "function apply<T>(callback: function(borrow T) -> Int64 effects { pure }, "
        "value: borrow T) -> Int64 effects { pure } { return callback(value) }\n"
        "function inspect(value: Text) -> Int64 effects { pure } { return 1 }\n"
        "function bad(value: borrow Text) -> Int64 effects { pure } "
        "{ return apply<Text>(inspect, value) }\n"));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);

    CHECK(compile_source(&compilation,
        "module access_mismatch\n"
        "trait Show { function show(self: borrow Self) -> Text effects { pure } }\n"
        "implementation Show for Int64 { "
        "function show(self: inout Self) -> Text effects { pure } { return \"bad\" } }\n"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-023"));
    free_compilation(&compilation);
}

static void test_region_types(void) {
    TestCompilation compilation;
    CHECK(compile_source(&compilation,
        "module regions\nfunction valid() -> () { region outer { "
        "let a = 1 region inner { let b = 2 } } }\n"));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);
    CHECK(compile_source(&compilation,
        "module bad_region\nfunction bad() -> () { region r { 1 } }\n"));
    CHECK(has_diagnostic(&compilation, "SOL-TYPE-002"));
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
    test_forged_type_resolutions_rejected();
    test_structural_generic_types();
    test_invalid_generic_component();
    test_user_generics_and_instantiations();
    test_nested_generic_substitution_growth();
    test_generic_variant_constructor_identity();
    test_generic_function_signature_substitution_and_inference();
    test_effect_row_type_boundaries();
    test_bounded_callback_effect_authority();
    test_invalid_capability_qualified_generic_types();
    test_generic_recursion_through_nongeneric_helper();
    test_invalid_user_generics();
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
    test_mixed_match_alias_and_never_provenance();
    test_invalid_capability_declarations();
    test_general_function_types();
    test_invalid_general_function_types();
    test_invalid_return_authority_contract();
    test_invalid_derived_capabilities();
    test_derived_capability_cycles();
    test_handler_types_and_metadata();
    test_invalid_handlers();
    test_contract_expression_types();
    test_traits_bounds_and_method_metadata();
    test_nested_self_substitution_growth();
    test_malformed_trait_method_links();
    test_computed_enum_receiver_methods();
    test_bound_evidence_forwarding();
    test_method_named_arguments();
    test_distinct_type_identity_and_construction();
    test_refined_type_predicates_and_construction_rejection();
    test_declared_constructor_requires_definition_head();
    test_invalid_type_representations_and_constructors();
    test_generic_representation_safety();
    test_representation_storage_cycles();
    test_type_metadata_defensive_validation();
    test_runtime_identity_equality_rejected();
    test_access_modes_are_exact();
    test_region_types();
    if (failures != 0) {
        fprintf(stderr, "%d type-checking test failure(s)\n", failures);
        return 1;
    }
    puts("type-checking tests passed");
    return 0;
}
