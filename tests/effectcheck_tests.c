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

static bool rerun_effectcheck(TestCompilation *compilation) {
    sol_effect_table_free(&compilation->effects);
    sol_effect_table_init(&compilation->effects);
    sol_diagnostics_free(&compilation->diagnostics);
    sol_diagnostics_init(&compilation->diagnostics);
    return sol_effect_check(
        &compilation->source,
        &compilation->syntax,
        &compilation->hir,
        &compilation->types,
        &compilation->effects,
        &compilation->diagnostics
    );
}

static bool rerun_typecheck(TestCompilation *compilation) {
    sol_effect_table_free(&compilation->effects);
    sol_effect_table_init(&compilation->effects);
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

static bool span_equals(const SolSource *source, SolSpan span, const char *text) {
    size_t length = span.end - span.start;
    return strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static bool diagnostic_message_contains(
    const TestCompilation *compilation,
    const char *code,
    const char *text
) {
    for (size_t index = 0; index < compilation->diagnostics.count; ++index) {
        if (strcmp(compilation->diagnostics.items[index].code, code) == 0
            && strstr(compilation->diagnostics.items[index].message, text) != NULL) {
            return true;
        }
    }
    return false;
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

static void test_effect_row_generic_instantiation(void) {
    static const char text[] =
        "module row_instantiation\n"
        "function pure_value(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function read_value(value: Int64) -> Int64 effects { panic } { return value }\n"
        "function write_value(value: Int64) -> Int64 effects { diverge } { return value }\n"
        "function apply<T, effects E>(value: T, callback: function(T) -> T effects E) -> T\n"
        "effects { E } { return callback(value) }\n"
        "function twice<effects E>(value: Int64, first: function(Int64) -> Int64 effects E, second: function(Int64) -> Int64 effects E) -> Int64\n"
        "effects { E } { return first(value) + second(value) }\n"
        "function pure_call() -> Int64 effects { pure } { return apply(1, pure_value) }\n"
        "function read_call() -> Int64 effects { panic } { return apply<Int64>(1, read_value) }\n"
        "function union_call() -> Int64 effects { panic diverge } { return twice(1, read_value, write_value) }\n"
        "function nested<effects E>(value: Int64, callback: function(Int64) -> Int64 effects E) -> Int64\n"
        "effects { E } { return apply(value, callback) }\n"
        "function nested_call() -> Int64 effects { diverge } { return nested(1, write_value) }\n"
        "function fixed_value(value: Int64) -> Int64 effects { diverge } { return value }\n"
        "function ignore<effects E>(value: Int64, callback: function(Int64) -> Int64 effects { diverge E }) -> Int64\n"
        "effects { E } { return value }\n"
        "function fixed_call() -> Int64 effects { pure } { return ignore(1, fixed_value) }\n"
        "function alias_call() -> Int64 effects { panic } { let callback = read_value return apply(1, callback) }\n"
        "function fixed_read_value(value: Int64) -> Int64 effects { diverge panic } { return value }\n"
        "function forward<effects E>(value: Int64, callback: function(Int64) -> Int64 effects { diverge E }) -> Int64\n"
        "effects { diverge E } { return callback(value) }\n"
        "function forward_call() -> Int64 effects { diverge panic } { return forward(1, fixed_read_value) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    size_t pure_rows = 0;
    size_t read_rows = 0;
    size_t union_rows = 0;
    size_t symbolic_rows = 0;
    size_t fixed_rows = 0;
    size_t prefixed_rows = 0;
    for (SolExprId expression = 0;
        expression < compilation.effects.call_instantiation_count;
        ++expression) {
        const SolEffectCallInstantiation *entry = sol_effect_call_instantiation(
            &compilation.effects, expression
        );
        if (entry == NULL) continue;
        pure_rows += entry->function == 3 && entry->row_count == 0
            && entry->parameter == SOL_AST_NONE ? 1 : 0;
        read_rows += entry->function == 3 && entry->row_count == 1
            && entry->parameter == SOL_AST_NONE ? 1 : 0;
        union_rows += entry->function == 4 && entry->row_count == 2 ? 1 : 0;
        symbolic_rows += entry->function == 3 && entry->parameter != SOL_AST_NONE ? 1 : 0;
        fixed_rows += entry->function == 11 && entry->row_count == 0
            && entry->parameter == SOL_AST_NONE ? 1 : 0;
        if (entry->function == 15 && entry->parameter == SOL_AST_NONE) {
            const SolEffectAtom *arguments = NULL;
            const SolEffectAtom *row = NULL;
            size_t argument_count = 0;
            size_t row_count = 0;
            CHECK(sol_effect_call_arguments(
                &compilation.effects, expression, &arguments, &argument_count
            ));
            CHECK(sol_effect_call_row(
                &compilation.effects, expression, &row, &row_count
            ));
            SolEffectRow argument_view = {(SolEffectAtom *)arguments, argument_count, false,
                SOL_AST_NONE};
            SolEffectRow row_view = {(SolEffectAtom *)row, row_count, false, SOL_AST_NONE};
            CHECK(argument_count == 1);
            CHECK(row_has_effect(&compilation, &argument_view, "panic"));
            CHECK(row_count == 2);
            CHECK(row_has_effect(&compilation, &row_view, "diverge"));
            CHECK(row_has_effect(&compilation, &row_view, "panic"));
            ++prefixed_rows;
        }
    }
    CHECK(pure_rows == 1);
    CHECK(read_rows == 2);
    CHECK(union_rows == 1);
    CHECK(symbolic_rows == 1);
    CHECK(fixed_rows == 1);
    CHECK(prefixed_rows == 1);
    free_compilation(&compilation);

    static const char uninferred[] =
        "module uninferred_row\n"
        "function bad<effects E>() -> Int64 effects { E } { return 1 }\n";
    CHECK(compile_source(&compilation, uninferred));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-008"));
    free_compilation(&compilation);

    static const char authority[] =
        "module authority_row\n"
        "capability Clock { function read(value: Int64) -> Int64 effects { clock.read<Self> } }\n"
        "function apply<effects E>(value: Int64, callback: function(Int64) -> Int64 effects E) -> Int64 effects { E } { return callback(value) }\n"
        "function bad(clock: capability Clock) -> Int64 effects { clock.read<clock> } { return apply(1, clock.read) }\n";
    CHECK(compile_source(&compilation, authority));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-007"));
    free_compilation(&compilation);

    static const char missing_effect[] =
        "module missing_row_effect\n"
        "function read() -> Int64 effects { panic } { return 1 }\n"
        "function apply<effects E>(callback: function() -> Int64 effects E) -> Int64 effects { E } { return callback() }\n"
        "function bad() -> Int64 effects { pure } { return apply(read) }\n";
    CHECK(compile_source(&compilation, missing_effect));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-002"));
    free_compilation(&compilation);

    static const char open_handler[] =
        "module open_row_handler\n"
        "capability Clock { function read() -> Int64 effects { clock.read<Self> } }\n"
        "capability TestClock { function read() -> Int64 effects { pure } }\n"
        "function bad<effects E>(clock: capability Clock, provider: capability TestClock, callback: function() -> Int64 effects E) -> Int64\n"
        "effects { E } { return handle clock.read<clock> with provider { callback() } }\n";
    CHECK(compile_source(&compilation, open_handler));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-008"));
    free_compilation(&compilation);

    static const char nested_output[] =
        "module nested_output_row\n"
        "function bad<effects E>(callback: function() -> Int64 effects E) -> Option<function() -> Int64 effects E> effects { E } { return none() }\n";
    CHECK(compile_source(&compilation, nested_output));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-008"));
    free_compilation(&compilation);

    static const char missing_declared_tail[] =
        "module missing_declared_tail\n"
        "function inner<effects F>(callback: function() -> Int64 effects F) -> Int64 effects { F } { return callback() }\n"
        "function outer<effects E>(callback: function() -> Int64 effects E) -> Int64 effects { pure } { return inner(callback) }\n";
    CHECK(compile_source(&compilation, missing_declared_tail));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-008"));
    CHECK(!has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    free_compilation(&compilation);

    static const char forged_resolution[] =
        "module forged_row_resolution\n"
        "function apply<effects E>(callback: function() -> Int64 effects { diverge E }) -> Int64 effects { E } { return 1 }\n";
    CHECK(compile_source(&compilation, forged_resolution));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolEffectId fixed = SOL_AST_NONE;
    for (SolEffectId effect = 0; effect < compilation.syntax.effect_count; ++effect) {
        if (span_equals(&compilation.source, compilation.syntax.effects[effect].name,
            "diverge")) {
            fixed = effect;
            break;
        }
    }
    CHECK(fixed != SOL_AST_NONE);
    if (fixed != SOL_AST_NONE) {
        sol_effect_table_free(&compilation.effects);
        sol_effect_table_init(&compilation.effects);
        sol_diagnostics_free(&compilation.diagnostics);
        sol_diagnostics_init(&compilation.diagnostics);
        compilation.hir.effect_resolutions[fixed] = (SolEffectResolution){
            SOL_EFFECT_RESOLUTION_PARAMETER,
            0,
        };
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

static SolProvenanceId singleton_provenance(
    const SolTypeTable *types,
    SolParameterId root
) {
    for (SolProvenanceId id = 0; id < types->provenance_count; ++id) {
        if (provenance_equals(types, id, &root, 1)) return id;
    }
    return SOL_PROVENANCE_NONE;
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

static void test_generic_closed_effect_rows(void) {
    static const char text[] =
        "module generic_effects\n"
        "capability Clock { function now() -> Int64 effects { clock.read<Self> } }\n"
        "function pass<T>(value: T, clock: capability Clock) -> T\n"
        "effects { clock.read<clock> } { let ignored = clock.now() return value }\n"
        "function integer(clock: capability Clock) -> Int64\n"
        "effects { clock.read<clock> } { return pass(1, clock) }\n"
        "function text(clock: capability Clock) -> Text\n"
        "effects { clock.read<clock> } { return pass<Text>(\"text\", clock) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.functions[1].count == 1);
    CHECK(!compilation.effects.functions[1].inferred);
    size_t instantiations = 0;
    for (size_t expression = 0; expression < compilation.syntax.expression_count; ++expression) {
        const SolCallInstantiation *instantiation = sol_type_call_instantiation(
            &compilation.types,
            expression
        );
        if (instantiation != NULL && instantiation->function == 1) ++instantiations;
    }
    CHECK(instantiations == 2);
    free_compilation(&compilation);
}

static void test_type_parameter_is_not_effect_authority(void) {
    static const char text[] =
        "module generic_authority\n"
        "function invalid<T>(value: T) -> T effects { service.read<T> } { return value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-010"));
    free_compilation(&compilation);
}

static void test_malformed_generic_instantiation_rejected(void) {
    static const char text[] =
        "module malformed_generic_instantiation\n"
        "function identity<T>(value: T) -> T { return value }\n"
        "function call() -> Int64 { return identity(1) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolExprId call = SOL_AST_NONE;
    for (size_t expression = 0; expression < compilation.syntax.expression_count; ++expression) {
        if (sol_type_call_instantiation(&compilation.types, expression) != NULL) {
            call = expression;
        }
    }
    CHECK(call != SOL_AST_NONE);
    if (call != SOL_AST_NONE) {
        SolType *arguments = compilation.types.call_instantiation_arguments;
        compilation.types.call_instantiation_arguments = NULL;
        sol_effect_table_free(&compilation.effects);
        sol_diagnostics_free(&compilation.diagnostics);
        sol_diagnostics_init(&compilation.diagnostics);
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        compilation.types.call_instantiation_arguments = arguments;
    }
    free_compilation(&compilation);
}

static void test_forged_generic_type_metadata_rejected(void) {
    static const char text[] =
        "module forged_generic_metadata\n"
        "record Box<T> { value: T, }\n"
        "enum Choice<T> { some(value: T), }\n"
        "function identity<T>(value: T) -> T { return value }\n"
        "function other<T>(value: T) -> T { return value }\n"
        "function make() -> Choice<Int64> {\n"
        "    let box = Box<Int64> { value = 1, }\n"
        "    let value = identity(2)\n"
        "    return Choice<Int64>.some(value = value)\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolExprId call = SOL_AST_NONE;
    for (SolExprId expression = 0; expression < compilation.syntax.expression_count;
        ++expression) {
        if (sol_type_call_instantiation(&compilation.types, expression) != NULL) {
            call = expression;
            break;
        }
    }
    CHECK(call != SOL_AST_NONE);
    CHECK(compilation.types.type_application_count >= 2);
    CHECK(compilation.types.variant_constructor_count != 0);

    if (call != SOL_AST_NONE) {
        size_t offset = compilation.types.call_instantiations[call].argument_offset;
        SolType original = compilation.types.call_instantiation_arguments[offset];
        compilation.types.call_instantiation_arguments[offset]
            = (SolType){.kind = SOL_TYPE_BOOL};
        CHECK(!rerun_effectcheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        compilation.types.call_instantiation_arguments[offset] = original;

        SolDefId function = compilation.types.call_instantiations[call].function;
        compilation.types.call_instantiations[call].function = 3;
        CHECK(!rerun_effectcheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        compilation.types.call_instantiations[call].function = function;
    }

    SolExprId primitive = SOL_AST_NONE;
    for (SolExprId expression = 0; expression < compilation.types.expression_count;
        ++expression) {
        if (compilation.types.expressions[expression].kind == SOL_TYPE_INT64) {
            primitive = expression;
            break;
        }
    }
    CHECK(primitive != SOL_AST_NONE);
    if (primitive != SOL_AST_NONE) {
        compilation.types.expressions[primitive].definition = 1;
        CHECK(!rerun_effectcheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        compilation.types.expressions[primitive].definition = 0;
    }

    SolTypeApplication *first = NULL;
    SolTypeApplication *second = NULL;
    for (size_t index = 0; index < compilation.types.type_application_count; ++index) {
        SolTypeApplication *application = &compilation.types.type_applications[index];
        if (application->constructor != SOL_TYPE_CONSTRUCTOR_USER) continue;
        if (first == NULL) first = application;
        else if (application->argument_count == first->argument_count) {
            second = application;
            break;
        }
    }
    CHECK(first != NULL);
    CHECK(second != NULL);
    if (first != NULL && second != NULL) {
        SolDefId definition = second->definition;
        second->definition = first->definition;
        CHECK(!rerun_effectcheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        second->definition = definition;
    }

    SolTypeApplication *box_application = NULL;
    for (size_t index = 0; index < compilation.types.type_application_count; ++index) {
        if (compilation.types.type_applications[index].constructor
                == SOL_TYPE_CONSTRUCTOR_USER
            && compilation.types.type_applications[index].definition == 0) {
            box_application = &compilation.types.type_applications[index];
            break;
        }
    }
    CHECK(box_application != NULL);
    if (compilation.types.variant_constructor_count != 0 && box_application != NULL) {
        SolType owner = compilation.types.variant_constructors[0].owner;
        size_t first_id = (size_t)(box_application - compilation.types.type_applications);
        compilation.types.variant_constructors[0].owner
            = (SolType){SOL_TYPE_APPLICATION, first_id};
        CHECK(!rerun_effectcheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        compilation.types.variant_constructors[0].owner = owner;
    }

    SolType *application_arguments = compilation.types.type_application_arguments;
    compilation.types.type_application_arguments = NULL;
    CHECK(!rerun_effectcheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    compilation.types.type_application_arguments = application_arguments;
    free_compilation(&compilation);
}

static void test_refined_type_effect_inputs(void) {
    static const char text[] =
        "module refined_effect_inputs\n"
        "type Positive = refined Int64 where self > 0\n"
        "type Meter = distinct Int64\n"
        "function make(value: Int64) -> Meter effects { pure } { return Meter(value) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolExprId self = SOL_AST_NONE;
    SolExprId construction = SOL_AST_NONE;
    for (SolExprId expression = 0;
        expression < compilation.syntax.expression_count;
        ++expression) {
        if (compilation.hir.resolutions[expression].kind
            == SOL_RESOLUTION_REFINEMENT_SELF) self = expression;
        if (sol_type_construction(&compilation.types, expression) != NULL) {
            construction = expression;
        }
    }
    CHECK(self != SOL_AST_NONE);
    CHECK(construction != SOL_AST_NONE);
    if (self != SOL_AST_NONE) {
        SolResolution resolution = compilation.hir.resolutions[self];
        compilation.hir.resolutions[self].target = 1;
        CHECK(!rerun_effectcheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        compilation.hir.resolutions[self] = resolution;

        size_t start = compilation.syntax.expressions[self].as.name.start;
        char spelling = compilation.source.text[start];
        compilation.source.text[start] = 'S';
        CHECK(!rerun_effectcheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        compilation.source.text[start] = spelling;
    }
    SolTypeRepresentation representation = compilation.types.representations[1];
    compilation.types.representations[1].flavor = SOL_TYPE_DECLARATION_REFINED;
    CHECK(!rerun_effectcheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    compilation.types.representations[1] = representation;
    if (construction != SOL_AST_NONE) {
        SolType result = compilation.types.constructions[construction].result;
        compilation.types.constructions[construction].result
            = (SolType){.kind = SOL_TYPE_NOMINAL, .definition = 0};
        CHECK(!rerun_effectcheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        compilation.types.constructions[construction].result = result;
    }
    free_compilation(&compilation);
}

static void test_forward_transitive_inference(void) {
    static const char text[] =
        "module forward_transitive\n"
        "function outer() -> Int64 { return middle() }\n"
        "function middle() -> Int64 { return source() }\n"
        "function source() -> Int64 effects { panic } { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.functions[0].inferred);
    CHECK(compilation.effects.functions[0].count == 1);
    CHECK(row_has_effect(&compilation, &compilation.effects.functions[0], "panic"));
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
        "function first() -> Int64 effects { panic } { return 1 }\n"
        "function second() -> Int64 effects { diverge } { return 2 }\n"
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
    CHECK(row_has_effect(&compilation, row, "panic"));
    CHECK(row_has_effect(&compilation, row, "diverge"));
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
        "function declared(value: Int64) -> Int64 effects { panic } {\n"
        "    return if value == 0 { 0 } else { inferred(value - 1) }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.functions[0].inferred);
    CHECK(compilation.effects.functions[0].count == 1);
    CHECK(row_has_effect(&compilation, &compilation.effects.functions[0], "panic"));
    CHECK(!compilation.effects.functions[1].inferred);
    free_compilation(&compilation);
}

static void test_explicit_caller_checks_recursive_fixed_point(void) {
    static const char text[] =
        "module recursive_caller_validation\n"
        "function source() -> Int64 effects { panic } { return 1 }\n"
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
            "panic"
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
        "function source() -> Int64 effects { panic } { return 1 }\n"
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
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.functions[3].inferred);
    CHECK(compilation.effects.functions[3].count == 3);
    CHECK(row_has_effect(&compilation, &compilation.effects.functions[3], "panic"));
    SolParameterId parameter = compilation.syntax.items[3].first_parameter;
    SolParameterId first = compilation.syntax.parameters[parameter].next;
    SolParameterId second = compilation.syntax.parameters[first].next;
    CHECK(row_has_parameter_effect(
        &compilation,
        &compilation.effects.functions[3],
        "clock.read",
        first
    ));
    CHECK(row_has_parameter_effect(
        &compilation,
        &compilation.effects.functions[3],
        "clock.read",
        second
    ));
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
        "function source() -> Int64 effects { panic } { return 1 }\n"
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
        "function read() -> Int64 effects { panic } { return 1 }\n"
        "function caller() -> Int64 effects { panic } { return read() }\n"
        "function pure_value() -> Int64 effects { pure } { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);
}

static void test_undeclared_effect(void) {
    static const char text[] =
        "module undeclared_effect\n"
        "function read() -> Int64 effects { panic } { return 1 }\n"
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
        "function duplicate() -> Int64 effects { panic panic } { return 1 }\n"
        "function mixed() -> Int64 effects { pure panic } { return 1 }\n"
        "function parameterized_pure() -> Int64 effects { pure<Value> } { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-001"));
    CHECK(compilation.effects.functions[0].count == 1);
    CHECK(compilation.effects.functions[1].count == 1);
    CHECK(compilation.effects.functions[2].count == 0);
    free_compilation(&compilation);
}

static void test_bounded_effect_authority(void) {
    static const char valid[] =
        "module bounded_authority\n"
        "capability Clock { function read() -> Int64 effects { clock.read<Self> } }\n"
        "function read(clock: capability Clock) -> Int64\n"
        "effects { panic diverge clock.read<clock> } { return clock.read() }\n"
        "function apply<effects E>(callback: function() -> Int64 effects E) -> Int64\n"
        "effects { E } { return callback() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, valid));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);

    static const char missing[] =
        "module missing_authority\n"
        "function bad() -> Int64 effects { filesystem.read } { return 1 }\n";
    CHECK(compile_source(&compilation, missing));
    CHECK(diagnostic_message_contains(
        &compilation,
        "SOL-EFFECT-010",
        "requires an explicit lexical capability authority"
    ));
    free_compilation(&compilation);

    static const char static_authority[] =
        "module static_authority\n"
        "function bad() -> Int64 effects { filesystem.read<Primary> } { return 1 }\n";
    CHECK(compile_source(&compilation, static_authority));
    CHECK(diagnostic_message_contains(
        &compilation,
        "SOL-EFFECT-010",
        "static authority is unavailable"
    ));
    free_compilation(&compilation);

    static const char callback_rows[] =
        "module callback_authority\n"
        "function missing(callback: function() -> Int64 effects { filesystem.read })\n"
        "-> Int64 effects { pure } { return 1 }\n"
        "function static(callback: function() -> Int64 effects { filesystem.read<Primary> })\n"
        "-> Int64 effects { pure } { return 1 }\n";
    CHECK(!compile_source(&compilation, callback_rows));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-010") == 2);
    CHECK(diagnostic_message_contains(
        &compilation,
        "SOL-EFFECT-010",
        "callback effects require an explicit lexical capability authority"
    ));
    CHECK(diagnostic_message_contains(
        &compilation,
        "SOL-EFFECT-010",
        "static authority is unavailable in callback function types"
    ));
    free_compilation(&compilation);
}

static void test_forged_effect_metadata_rejected(void) {
    static const char text[] =
        "module forged_effect_metadata\n"
        "function panics() -> Int64 effects { panic } { return 1 }\n"
        "function clean() -> Int64 effects { pure } { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolEffect *panic_effect = &compilation.syntax.effects[0];
    SolEffect *pure_effect = &compilation.syntax.effects[1];

    panic_effect->argument = panic_effect->name;
    CHECK(!rerun_effectcheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    panic_effect->argument = (SolSpan){0};

    panic_effect->has_argument = true;
    CHECK(!rerun_effectcheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    panic_effect->has_argument = false;

    panic_effect->name = pure_effect->name;
    CHECK(!rerun_typecheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    panic_effect->name = (SolSpan){
        .start = panic_effect->span.start,
        .end = panic_effect->span.end,
    };

    panic_effect->argument = panic_effect->name;
    CHECK(!rerun_typecheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    panic_effect->argument = (SolSpan){0};

    panic_effect->has_argument = true;
    CHECK(!rerun_typecheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    panic_effect->has_argument = false;

    panic_effect->name = (SolSpan){0};
    CHECK(!rerun_typecheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    panic_effect->name = (SolSpan){
        .start = panic_effect->span.start,
        .end = panic_effect->span.end,
    };

    pure_effect->is_pure = false;
    CHECK(!rerun_typecheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    pure_effect->is_pure = true;

    pure_effect->span.end = pure_effect->name.end + 1;
    CHECK(!rerun_typecheck(&compilation));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
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
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-010") == 3);
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
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-010") == 2);
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
        "function read() -> Int64 effects { panic } { return 1 }\n"
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
    CHECK(row_has_effect(&compilation, &compilation.effects.functions[4], "panic"));
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

static void test_mixed_authority_effect_expansion(void) {
    static const char text[] =
        "module mixed_authority_effects\n"
        "capability Restricted {\n"
        "    function now() -> Int64 effects { clock.read<Self> }\n"
        "}\n"
        "capability Clock {\n"
        "    function now() -> Int64 effects { clock.read<Self> }\n"
        "    function restrict() -> capability Restricted\n"
        "    authority { result derives_from Self } effects { pure }\n"
        "}\n"
        "function helper(clock: capability Clock) -> Int64\n"
        "effects { clock.read<clock> clock.observe<clock> } { return clock.now() }\n"
        "function pass(clock: capability Clock) -> capability Clock\n"
        "authority { result derives_from clock } effects { pure } { return clock }\n"
        "function inferred(\n"
        "    flag: Bool, first: capability Clock, second: capability Clock,\n"
        ") -> Int64 {\n"
        "    let selected = if flag { first } else { second }\n"
        "    let operation = match flag { true => first.now false => second.now }\n"
        "    return helper(selected) + operation() + pass(selected).now()\n"
        "}\n"
        "function explicit(\n"
        "    flag: Bool, first: capability Clock, second: capability Clock,\n"
        ") -> Int64 effects {\n"
        "    clock.read<first> clock.read<second>\n"
        "    clock.observe<first> clock.observe<second>\n"
        "} { return helper(if flag { first } else { second }) }\n"
        "function omitted(\n"
        "    flag: Bool, first: capability Clock, second: capability Clock,\n"
        ") -> Int64 effects { clock.read<first> clock.observe<first> } {\n"
        "    return helper(match flag { true => first false => second })\n"
        "}\n"
        "function wrapped(\n"
        "    flag: Bool, first: capability Clock, second: capability Clock,\n"
        ") -> Int64 {\n"
        "    let restricted = (if flag { first } else { second }).restrict()\n"
        "    return restricted.now()\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-002") == 2);
    const SolEffectRow *inferred = &compilation.effects.functions[4];
    SolParameterId parameter = compilation.syntax.items[4].first_parameter;
    SolParameterId first = compilation.syntax.parameters[parameter].next;
    SolParameterId second = compilation.syntax.parameters[first].next;
    CHECK(inferred->inferred);
    CHECK(inferred->count == 4);
    CHECK(row_has_parameter_effect(&compilation, inferred, "clock.read", first));
    CHECK(row_has_parameter_effect(&compilation, inferred, "clock.read", second));
    CHECK(row_has_parameter_effect(&compilation, inferred, "clock.observe", first));
    CHECK(row_has_parameter_effect(&compilation, inferred, "clock.observe", second));
    const SolEffectRow *wrapped = &compilation.effects.functions[7];
    parameter = compilation.syntax.items[7].first_parameter;
    first = compilation.syntax.parameters[parameter].next;
    second = compilation.syntax.parameters[first].next;
    CHECK(wrapped->inferred);
    CHECK(wrapped->count == 2);
    CHECK(row_has_parameter_effect(&compilation, wrapped, "clock.read", first));
    CHECK(row_has_parameter_effect(&compilation, wrapped, "clock.read", second));
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
    SolProvenanceId *origins = compilation.types.local_capability_origins;
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
    SolProvenanceId *expression_capability_origins
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
            SolProvenanceId origin = origins[binding];
            origins[binding] = compilation.types.provenance_count;
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
            origins[binding] = singleton_provenance(&compilation.types, second);
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
    SolProvenanceId *expression_operation_origins
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
    SolProvenanceId *local_operation_origins = compilation.types.local_operation_origins;
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
    SolProvenanceId first_provenance = singleton_provenance(
        &compilation.types,
        first_capability
    );
    SolProvenanceId second_provenance = singleton_provenance(
        &compilation.types,
        second_capability
    );
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
                && expression_capability_origins[index] == first_provenance) {
                target = index;
                break;
            }
        }
        CHECK(target != SOL_AST_NONE);
        if (target != SOL_AST_NONE) {
            expression_capability_origins[target] = second_provenance;
            CHECK(!sol_effect_check(
                &compilation.source,
                &compilation.syntax,
                &compilation.hir,
                &compilation.types,
                &compilation.effects,
                &compilation.diagnostics
            ));
            expression_capability_origins[target] = first_provenance;
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
                && expression_operation_origins[index] == first_provenance) {
                target = index;
                break;
            }
        }
        CHECK(target != SOL_AST_NONE);
        if (target != SOL_AST_NONE) {
            expression_operation_origins[target] = second_provenance;
            CHECK(!sol_effect_check(
                &compilation.source,
                &compilation.syntax,
                &compilation.hir,
                &compilation.types,
                &compilation.effects,
                &compilation.diagnostics
            ));
            expression_operation_origins[target] = first_provenance;
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
        SolProvenanceId origin = local_operation_origins[operation_local];
        SolParameterId first = compilation.syntax.items[1].first_parameter;
        SolParameterId second = compilation.syntax.parameters[first].next;
        second = compilation.syntax.parameters[second].next;
        local_operation_origins[operation_local]
            = singleton_provenance(&compilation.types, second);
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

static void test_contract_effect_firewall(void) {
    static const char text[] =
        "module contract_effect_firewall\n"
        "capability Clock {\n"
        "    function now() -> Int64 effects { clock.read<Self> }\n"
        "}\n"
        "function sample(clock: capability Clock) -> Int64\n"
        "effects { pure }\n"
        "requires { clock.now() == 1 }\n"
        "ensures { result == old(clock.now()) }\n"
        "{ return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    if (compilation.effects.functions == NULL) {
        free_compilation(&compilation);
        return;
    }
    CHECK(compilation.effects.functions[1].count == 0);
    for (size_t index = 0; index < compilation.syntax.contract_condition_count; ++index) {
        SolExprId expression = compilation.syntax.contract_conditions[index].expression;
        CHECK(compilation.types.expressions[expression].kind == SOL_TYPE_BOOL);
    }

    SolContractClauseId owner_clause
        = compilation.syntax.contract_conditions[0].owner_clause;
    compilation.syntax.contract_conditions[0].owner_clause
        = compilation.syntax.contract_clause_count;
    sol_effect_table_free(&compilation.effects);
    sol_effect_table_init(&compilation.effects);
    sol_diagnostics_free(&compilation.diagnostics);
    sol_diagnostics_init(&compilation.diagnostics);
    CHECK(!sol_effect_check(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.types,
        &compilation.effects,
        &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    compilation.syntax.contract_conditions[0].owner_clause = owner_clause;
    free_compilation(&compilation);
}

static void test_contract_result_provenance(void) {
    static const char text[] =
        "module contract_result_provenance\n"
        "capability Clock {}\n"
        "function accepts(value: capability Clock) -> Bool effects { pure } { return true }\n"
        "function keep(clock: capability Clock) -> capability Clock\n"
        "authority { result derives_from clock }\n"
        "effects { pure }\n"
        "ensures { accepts(result) }\n"
        "{ return clock }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolParameterId root = compilation.syntax.items[2].first_parameter;
    bool found = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind == SOL_EXPR_RESULT) {
            CHECK(provenance_equals(
                &compilation.types,
                compilation.types.expression_capability_origins[index],
                &root,
                1
            ));
            found = true;
        }
    }
    CHECK(found);
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
        CHECK(provenance_equals(
            &compilation.types,
            compilation.types.local_capability_origins[binding],
            &parameter,
            1
        ));
        CHECK(provenance_equals(
            &compilation.types,
            compilation.types.expression_capability_origins[initializer],
            &parameter,
            1
        ));
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
        "    callback: function() -> Int64 effects { panic },\n"
        ") -> function() -> Int64 effects { panic } { return callback }\n";
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
        "    callback: function() -> Int64 effects { panic },\n"
        ") -> function() -> Int64 effects { panic } { return callback }\n";
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
        atoms[0].name = compilation.syntax.items[0].name;
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        atoms[0].name = compilation.syntax.effects[
            compilation.syntax.types[0].first_effect
        ].name;
        compilation.types.function_types[0].effects.count = 0;
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        compilation.types.function_types[0].effects.count = 1;
    }
    free_compilation(&compilation);
}

static void test_higher_order_effects(void) {
    static const char text[] =
        "module higher_order_effects\n"
        "function source(value: Int64) -> Int64 effects { panic } { return value }\n"
        "function inferred_source(value: Int64) -> Int64 { return source(value) }\n"
        "function pure_source(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function apply(\n"
        "    value: Int64,\n"
        "    callback: function(Int64) -> Int64 effects { panic },\n"
        ") -> Int64 { return callback(value) }\n"
        "function valid() -> Int64 effects { panic } { return apply(1, source) }\n"
        "function alias_valid() -> Int64 effects { panic } {\n"
        "    let callback = source\n"
        "    return apply(1, callback)\n"
        "}\n"
        "function inferred_valid() -> Int64 effects { panic } {\n"
        "    return apply(1, inferred_source)\n"
        "}\n"
        "function pure_valid() -> Int64 effects { panic } {\n"
        "    return apply(1, pure_source)\n"
        "}\n"
        "function return_source() -> function(Int64) -> Int64 effects { panic } {\n"
        "    return source\n"
        "}\n"
        "function missing(\n"
        "    callback: function(Int64) -> Int64 effects { panic },\n"
        ") -> Int64 effects { pure } { return callback(1) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-002") == 1);
    CHECK(!has_diagnostic(&compilation, "SOL-EFFECT-006"));
    CHECK(compilation.effects.functions[3].inferred);
    CHECK(compilation.effects.functions[3].count == 1);
    CHECK(row_has_effect(&compilation, &compilation.effects.functions[3], "panic"));
    CHECK(compilation.types.function_coercion_count == 5);
    free_compilation(&compilation);
}

static void test_incompatible_callback_effects(void) {
    static const char text[] =
        "module incompatible_callback_effects\n"
        "function network(value: Int64) -> Int64 effects { diverge } { return value }\n"
        "function apply(\n"
        "    value: Int64,\n"
        "    callback: function(Int64) -> Int64 effects { panic },\n"
        ") -> Int64 effects { panic } { return callback(value) }\n"
        "function bad_effect() -> Int64 effects { panic } { return apply(1, network) }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(diagnostic_count(&compilation, "SOL-EFFECT-006") == 1);
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
    CHECK(!compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-010"));
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
            if (provenance_equals(
                &compilation.types,
                compilation.types.expression_capability_origins[index],
                &root,
                1
            )) {
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
            CHECK(provenance_equals(
                &compilation.types,
                compilation.types.expression_capability_origins[index],
                &root,
                1
            ));
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

static void test_exact_effect_handlers(void) {
    static const char text[] =
        "module exact_effect_handlers\n"
        "capability Clock {\n"
        "    function read(value: Int64) -> Int64 effects { clock.read<Self> }\n"
        "}\n"
        "capability TestClock {\n"
        "    function read(value: Int64) -> Int64 effects { pure }\n"
        "}\n"
        "function evaluate(provider: capability TestClock) -> capability TestClock\n"
        "authority { result derives_from provider }\n"
        "effects { provider.evaluate<provider> } { return provider }\n"
        "function read(clock: capability Clock) -> Int64\n"
        "effects { clock.read<clock> } { return clock.read(1) }\n"
        "function mixed(clock: capability Clock) -> Int64\n"
        "effects { clock.read<clock> service.log<clock> } { return clock.read(1) }\n"
        "function handled(\n"
        "    clock: capability Clock, other: capability Clock, provider: capability TestClock,\n"
        ") -> Int64 {\n"
        "    return handle clock.read<clock> with evaluate(provider) {\n"
        "        mixed(clock) + other.read(1)\n"
        "    }\n"
        "}\n"
        "function transitive(clock: capability Clock, provider: capability TestClock) -> Int64 {\n"
        "    return handle clock.read<clock> with provider { read(clock) }\n"
        "}\n"
        "function recursive(\n"
        "    clock: capability Clock, provider: capability TestClock, depth: Int64,\n"
        ") -> Int64 {\n"
        "    return handle clock.read<clock> with provider {\n"
        "        if depth == 0 { clock.read(1) } else { recursive(clock, provider, depth - 1) }\n"
        "    }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    if (compilation.effects.functions == NULL) {
        free_compilation(&compilation);
        return;
    }
    const SolSyntaxItem *handled_item = &compilation.syntax.items[5];
    SolParameterId handled_clock = handled_item->first_parameter;
    SolParameterId handled_other = compilation.syntax.parameters[handled_clock].next;
    const SolEffectRow *handled = &compilation.effects.functions[5];
    CHECK(handled->inferred);
    CHECK(handled->count == 3);
    CHECK(row_has_effect(&compilation, handled, "provider.evaluate"));
    CHECK(row_has_effect(&compilation, handled, "service.log"));
    CHECK(row_has_parameter_effect(
        &compilation,
        handled,
        "clock.read",
        handled_other
    ));
    CHECK(!row_has_parameter_effect(
        &compilation,
        handled,
        "clock.read",
        handled_clock
    ));
    CHECK(compilation.effects.functions[6].inferred);
    CHECK(compilation.effects.functions[6].count == 0);
    CHECK(compilation.effects.functions[7].inferred);
    CHECK(compilation.effects.functions[7].count == 0);

    size_t handler_count = 0;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind != SOL_EXPR_HANDLE) continue;
        const SolHandler *handler = &compilation.types.handlers[index];
        CHECK(handler->source_member == 0);
        CHECK(handler->provider_member == 1);
        CHECK(handler->root != SOL_AST_NONE);
        ++handler_count;
    }
    CHECK(handler_count == 3);
    free_compilation(&compilation);
}

static void test_mixed_handler_boundary(void) {
    static const char safe_text[] =
        "module mixed_handler_boundary\n"
        "capability Clock { function read() -> Int64 effects { clock.read<Self> } }\n"
        "capability TestClock { function read() -> Int64 effects { pure } }\n"
        "function safe_provider(\n"
        "    flag: Bool, clock: capability Clock,\n"
        "    first: capability TestClock, second: capability TestClock,\n"
        ") -> Int64 {\n"
        "    return handle clock.read<clock> with if flag { first } else { second } {\n"
        "        clock.read()\n"
        "    }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, safe_text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.functions[2].inferred);
    CHECK(compilation.effects.functions[2].count == 0);
    free_compilation(&compilation);

    static const char dynamic_text[] =
        "module dynamic_handler_target\n"
        "capability Clock { function read() -> Int64 effects { clock.read<Self> } }\n"
        "capability TestClock { function read() -> Int64 effects { pure } }\n"
        "function dynamic_target(\n"
        "    flag: Bool, first: capability Clock, second: capability Clock,\n"
        "    provider: capability TestClock,\n"
        ") -> Int64 {\n"
        "    return handle clock.read<if flag { first } else { second }> with provider { 1 }\n"
        "}\n";
    CHECK(!compile_source(&compilation, dynamic_text));
    CHECK(diagnostic_count(&compilation, "SOL-HANDLER-001") == 1);
    bool specific = false;
    for (size_t index = 0; index < compilation.diagnostics.count; ++index) {
        specific = specific || strstr(
            compilation.diagnostics.items[index].message,
            "dynamic authority matching is unsupported"
        ) != NULL;
    }
    CHECK(specific);
    free_compilation(&compilation);
}

static void test_malformed_provenance_sets_rejected(void) {
    static const char text[] =
        "module malformed_provenance_sets\n"
        "capability Restricted { function now() -> Int64 effects { pure } }\n"
        "capability Clock {\n"
        "    function now() -> Int64 effects { pure }\n"
        "    function restrict() -> capability Restricted\n"
        "    authority { result derives_from Self } effects { pure }\n"
        "}\n"
        "capability Wrapped derives_from source: capability Clock {}\n"
        "function mixed(flag: Bool, first: capability Clock, second: capability Clock)\n"
        "-> Int64 effects { pure } {\n"
        "    let selected = if flag { first } else { second }\n"
        "    let restricted = selected.restrict()\n"
        "    let wrapped = Wrapped { source = selected }\n"
        "    return restricted.now()\n"
        "}\n"
        "function foreign(clock: capability Clock) -> Int64 effects { pure } { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    sol_effect_table_free(&compilation.effects);
    SolProvenanceId mixed = SOL_PROVENANCE_NONE;
    for (SolProvenanceId id = 0; id < compilation.types.provenance_count; ++id) {
        SolProvenance provenance;
        if (sol_type_provenance(&compilation.types, id, &provenance)
            && provenance.count == 2) mixed = id;
    }
    CHECK(mixed != SOL_PROVENANCE_NONE);
    if (mixed != SOL_PROVENANCE_NONE) {
        SolExprId join = SOL_AST_NONE;
        for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
            if (compilation.syntax.expressions[index].kind == SOL_EXPR_IF) join = index;
        }
        CHECK(join != SOL_AST_NONE);
        if (join != SOL_AST_NONE) {
            SolExprId branch = compilation.syntax.expressions[
                join
            ].as.if_expr.then_branch;
            SolType type = compilation.types.expressions[branch];
            compilation.types.expressions[branch] = (SolType){.kind = SOL_TYPE_UNKNOWN};
            CHECK(!sol_effect_check(
                &compilation.source,
                &compilation.syntax,
                &compilation.hir,
                &compilation.types,
                &compilation.effects,
                &compilation.diagnostics
            ));
            compilation.types.expressions[branch] = type;
        }
        SolProvenanceSet *set = &compilation.types.provenances[mixed];
        SolParameterId *roots
            = compilation.types.provenance_roots + set->root_offset;
        SolParameterId first = roots[0];
        SolParameterId second = roots[1];
        SolProvenanceId singleton = singleton_provenance(&compilation.types, first);
        for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
            SolExprKind kind = compilation.syntax.expressions[index].kind;
            if ((kind != SOL_EXPR_CALL && kind != SOL_EXPR_RECORD)
                || compilation.types.expression_capability_origins[index] != mixed) {
                continue;
            }
            compilation.types.expression_capability_origins[index] = singleton;
            CHECK(!sol_effect_check(
                &compilation.source,
                &compilation.syntax,
                &compilation.hir,
                &compilation.types,
                &compilation.effects,
                &compilation.diagnostics
            ));
            compilation.types.expression_capability_origins[index] = mixed;
        }
        roots[0] = second;
        roots[1] = first;
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        roots[0] = first;
        roots[1] = first;
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        roots[1] = compilation.syntax.items[4].first_parameter;
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        roots[1] = second;
        size_t count = set->root_count;
        set->root_count = 0;
        SolProvenance invalid;
        CHECK(!sol_type_provenance(&compilation.types, mixed, &invalid));
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        set->root_count = count;
    }
    free_compilation(&compilation);
}

static void test_handler_provider_constraints(void) {
    static const char text[] =
        "module handler_provider_constraints\n"
        "capability Clock { function read(value: Int64) -> Int64 effects { clock.read<Self> } }\n"
        "capability ImpureClock {\n"
        "    function read(value: Int64) -> Int64 effects { service.fake<Self> }\n"
        "}\n"
        "function invalid(clock: capability Clock, provider: capability ImpureClock) -> Int64 {\n"
        "    return handle clock.read<clock> with provider { clock.read(1) }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(!compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-HANDLER-001"));
    free_compilation(&compilation);
}

static void test_forged_handler_metadata_rejected(void) {
    static const char text[] =
        "module forged_handler_metadata\n"
        "capability Clock { function read() -> Int64 effects { clock.read<Self> } }\n"
        "capability TestClock { function read() -> Int64 effects { pure } }\n"
        "function sample(clock: capability Clock, provider: capability TestClock) -> Int64 {\n"
        "    return handle clock.read<clock> with provider { clock.read() }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolExprId handler = SOL_AST_NONE;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind == SOL_EXPR_HANDLE) handler = index;
    }
    CHECK(handler != SOL_AST_NONE);
    if (handler != SOL_AST_NONE) {
        sol_effect_table_free(&compilation.effects);
        SolParameterId root = compilation.types.handlers[handler].root;
        compilation.types.handlers[handler].root = compilation.syntax.parameters[root].next;
        CHECK(!sol_effect_check(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.types,
            &compilation.effects,
            &compilation.diagnostics
        ));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        compilation.types.handlers[handler].root = root;
    }
    free_compilation(&compilation);
}

static void test_trait_method_effects_and_metadata(void) {
    static const char valid[] =
        "module trait_effects\n"
        "trait Load { function load(self: Self) -> Text effects { panic } }\n"
        "implementation Load for Int64 { function load(self: Self) -> Text effects { panic } { return \"ok\" } }\n"
        "function load(value: Int64) -> Text effects { panic } { return value.load() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, valid));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.effects.trait_method_count == 2);
    SolExprId method_call = SOL_AST_NONE;
    for (SolExprId expression = 0; expression < compilation.syntax.expression_count; ++expression) {
        if (sol_type_method_resolution(&compilation.types, expression) != NULL) {
            method_call = expression;
            break;
        }
    }
    CHECK(method_call != SOL_AST_NONE);
    if (method_call != SOL_AST_NONE) {
        SolTraitMethodId saved = compilation.types.method_resolutions[method_call].method;
        compilation.types.method_resolutions[method_call].method
            = compilation.syntax.trait_method_count;
        CHECK(!rerun_effectcheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
        compilation.types.method_resolutions[method_call].method = saved;
    }
    free_compilation(&compilation);

    static const char forged_selection[] =
        "module forged_selection\n"
        "trait Load {\n"
        " function load(self: Self) -> Text effects { panic }\n"
        " function cached(self: Self) -> Text effects { pure }\n"
        "}\n"
        "implementation Load for Int64 {\n"
        " function load(self: Self) -> Text effects { panic } { return \"loaded\" }\n"
        " function cached(self: Self) -> Text effects { pure } { return \"cached\" }\n"
        "}\n"
        "function call(value: Int64) -> Text effects { panic } { return value.load() }\n";
    CHECK(compile_source(&compilation, forged_selection));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    method_call = SOL_AST_NONE;
    for (SolExprId expression = 0; expression < compilation.syntax.expression_count;
        ++expression) {
        if (sol_type_method_resolution(&compilation.types, expression) != NULL) {
            method_call = expression;
            break;
        }
    }
    CHECK(method_call != SOL_AST_NONE);
    if (method_call != SOL_AST_NONE) {
        SolTraitMethodId pure = compilation.syntax.items[1].first_trait_method;
        pure = compilation.syntax.trait_methods[pure].next;
        CHECK(pure != SOL_AST_NONE);
        compilation.types.method_resolutions[method_call].method = pure;
        CHECK(!rerun_effectcheck(&compilation));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    }
    free_compilation(&compilation);

    static const char mismatch[] =
        "module mismatch\n"
        "trait Load { function load(self: Self) -> Text effects { panic } }\n"
        "implementation Load for Int64 { function load(self: Self) -> Text effects { pure } { return \"ok\" } }\n";
    CHECK(compile_source(&compilation, mismatch));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-009"));
    free_compilation(&compilation);

    static const char missing[] =
        "module missing\n"
        "trait Load { function load(self: Self) -> Text effects { panic } }\n"
        "implementation Load for Int64 { function load(self: Self) -> Text effects { panic } { return \"ok\" } }\n"
        "function bad(value: Int64) -> Text effects { pure } { return value.load() }\n";
    CHECK(compile_source(&compilation, missing));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-002"));
    free_compilation(&compilation);

    static const char dependent[] =
        "module dependent\n"
        "trait Load { function load(self: Self) -> Text effects { io.read<Self> } }\n"
        "implementation Load for Int64 { function load(self: Self) -> Text effects { io.read<Self> } { return \"ok\" } }\n";
    CHECK(compile_source(&compilation, dependent));
    CHECK(has_diagnostic(&compilation, "SOL-EFFECT-009"));
    free_compilation(&compilation);
}

static void test_distinct_implementation_and_function_representation(void) {
    static const char text[] =
        "module distinct_pipeline\n"
        "type Identifier = distinct Int64\n"
        "type Callback = distinct function(Int64) -> Int64 effects { pure }\n"
        "trait Value { function value(self: Self) -> Int64 effects { pure } }\n"
        "implementation Value for Identifier {\n"
        " function value(self: Self) -> Int64 effects { pure } { return 1 }\n"
        "}\n"
        "function source(value: Int64) -> Int64 effects { pure } { return value }\n"
        "function make() -> Callback effects { pure } { return Callback(source) }\n"
        "function read_value(value: Identifier) -> Int64 effects { pure } { return value.value() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    if (sol_diagnostics_has_errors(&compilation.diagnostics)) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.types.function_coercion_count == 1);
    free_compilation(&compilation);
}

static void test_distinct_borrowed_function_signatures(void) {
    static const char text[] =
        "module borrowed_signatures\n"
        "function shared(callback: function(borrow Text) -> Int64 effects { pure }) "
        "-> Int64 effects { pure } { return 1 }\n"
        "function exclusive(callback: function(inout Text) -> Int64 effects { pure }) "
        "-> Int64 effects { pure } { return 2 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.types.function_type_count >= 2);
    free_compilation(&compilation);
}

int main(void) {
    test_private_pure_inference();
    test_generic_closed_effect_rows();
    test_effect_row_generic_instantiation();
    test_type_parameter_is_not_effect_authority();
    test_malformed_generic_instantiation_rejected();
    test_forged_generic_type_metadata_rejected();
    test_refined_type_effect_inputs();
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
    test_bounded_effect_authority();
    test_forged_effect_metadata_rejected();
    test_parameterized_effects();
    test_static_effect_argument_name_collision();
    test_capability_operation_effects();
    test_function_effect_parameter_substitution();
    test_exact_function_alias_effects();
    test_computed_effect_argument();
    test_mixed_authority_effect_expansion();
    test_missing_capability_operation_effects();
    test_invalid_capability_effect_row();
    test_malformed_capability_arena_rejected();
    test_orphan_expression_children_rejected();
    test_expression_cycle_rejected();
    test_contract_effect_firewall();
    test_contract_result_provenance();
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
    test_exact_effect_handlers();
    test_mixed_handler_boundary();
    test_handler_provider_constraints();
    test_forged_handler_metadata_rejected();
    test_malformed_provenance_sets_rejected();
    test_trait_method_effects_and_metadata();
    test_distinct_implementation_and_function_representation();
    test_distinct_borrowed_function_signatures();
    if (failures != 0) {
        fprintf(stderr, "%d effect-checking test failure(s)\n", failures);
        return 1;
    }
    puts("effect-checking tests passed");
    return 0;
}
