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
} TestCompilation;

static void reset_hir_diagnostics(TestCompilation *compilation);

static bool compile_source(TestCompilation *compilation, const char *text) {
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    if (!sol_source_from_text(&compilation->source, "sema.sol", text)) {
        return false;
    }
    if (!sol_lex(&compilation->source, &compilation->tokens, &compilation->diagnostics)) {
        return false;
    }
    if (!sol_parse(
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
    return sol_hir_lower(
        &compilation->source,
        &compilation->syntax,
        &compilation->hir,
        &compilation->diagnostics
    );
}

static void free_compilation(TestCompilation *compilation) {
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

static bool span_text_equal(const SolSource *source, SolSpan span, const char *text) {
    size_t length = span.end - span.start;
    return strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static SolSpan text_span(const SolSource *source, const char *text) {
    const char *start = strstr(source->text, text);
    CHECK(start != NULL);
    if (start == NULL) return (SolSpan){0};
    return (SolSpan){
        .start = (size_t)(start - source->text),
        .end = (size_t)(start - source->text) + strlen(text),
    };
}

static bool semantic_id_equal(SolSemanticId left, SolSemanticId right) {
    return left.high == right.high && left.low == right.low;
}

static SolSemanticId definition_id(const TestCompilation *compilation, const char *name) {
    for (size_t index = 0; index < compilation->hir.definition_count; ++index) {
        if (span_text_equal(
            &compilation->source, compilation->hir.definitions[index].name, name
        )) return compilation->hir.definitions[index].semantic_id;
    }
    CHECK(false);
    return (SolSemanticId){0};
}

static void test_successful_resolution(void) {
    static const char text[] =
        "module resolution\n"
        "record Pair { left: Int64, right: Int64, }\n"
        "function make(a: Int64 /* input */, b: Int64) -> Pair {\n"
        "    let sum = a + b\n"
        "    return Pair { left = sum, right = b, }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.hir.definition_count == 2);
    CHECK(compilation.syntax.parameter_count == 2);
    CHECK(compilation.hir.local_count == 3);
    CHECK(span_text_equal(
        &compilation.source,
        compilation.syntax.parameters[0].type,
        "Int64"
    ));

    size_t resolved_paths = 0;
    bool pair_resolved = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        const SolExpr *expression = &compilation.syntax.expressions[index];
        if (expression->kind != SOL_EXPR_PATH) {
            continue;
        }
        ++resolved_paths;
        SolResolution resolution = compilation.hir.resolutions[index];
        CHECK(resolution.kind == SOL_RESOLUTION_LOCAL
            || resolution.kind == SOL_RESOLUTION_DEFINITION);
        if (span_text_equal(&compilation.source, expression->as.name, "Pair")) {
            CHECK(resolution.kind == SOL_RESOLUTION_DEFINITION);
            CHECK(resolution.target == 0);
            pair_resolved = true;
        }
    }
    CHECK(resolved_paths == 5);
    CHECK(pair_resolved);
    free_compilation(&compilation);
}

static void test_unresolved_name(void) {
    static const char text[] =
        "module unresolved\n"
        "function bad() -> Int64 { return missing }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-002"));
    free_compilation(&compilation);
}

static void test_duplicate_declaration(void) {
    static const char text[] =
        "module duplicate\n"
        "record Value {}\n"
        "record Value {}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-001"));
    free_compilation(&compilation);
}

static void test_stable_semantic_identity(void) {
    static const char first[] =
        "module original.location\n"
        "@stable(\"example.User.v1\") public record User {}\n"
        "public record Other {}\n"
        "function consume(value: User) -> User { return User {} }\n";
    static const char second[] =
        "module moved.location\n"
        "public record Other {}\n"
        "@stable(\"example.User.v1\") public record RenamedUser {}\n"
        "function consume(value: RenamedUser) -> RenamedUser { return RenamedUser {} }\n";
    TestCompilation left;
    TestCompilation right;
    CHECK(compile_source(&left, first));
    CHECK(compile_source(&right, second));
    CHECK(!sol_diagnostics_has_errors(&left.diagnostics));
    CHECK(!sol_diagnostics_has_errors(&right.diagnostics));
    SolSemanticId user = definition_id(&left, "User");
    CHECK(semantic_id_equal(user, definition_id(&right, "RenamedUser")));
    CHECK(!semantic_id_equal(
        definition_id(&left, "Other"), definition_id(&right, "Other")
    ));
    size_t user_references = 0;
    for (size_t index = 0; index < left.hir.semantic_reference_count; ++index) {
        const SolSemanticReference *reference = &left.hir.semantic_references[index];
        if (semantic_id_equal(reference->target_id, user)) ++user_references;
        CHECK(reference->target < left.hir.definition_count);
        CHECK(semantic_id_equal(
            reference->target_id,
            left.hir.definitions[reference->target].semantic_id
        ));
    }
    CHECK(user_references == 4);
    free_compilation(&right);
    free_compilation(&left);

    static const char ordered[] =
        "module deterministic\n"
        "public record First {}\n"
        "public record Second {}\n";
    static const char reordered[] =
        "module deterministic\n"
        "public record Second {}\n"
        "public record First {}\n";
    CHECK(compile_source(&left, ordered));
    CHECK(compile_source(&right, reordered));
    CHECK(semantic_id_equal(
        definition_id(&left, "First"), definition_id(&right, "First")
    ));
    CHECK(semantic_id_equal(
        definition_id(&left, "Second"), definition_id(&right, "Second")
    ));
    free_compilation(&right);
    free_compilation(&left);

    static const char collisions[] =
        "module identity_errors\n"
        "@stable(\"duplicate\") public record First {}\n"
        "@stable(\"duplicate\") public record Second {}\n"
        "@stable(\"bad key\") public record Invalid {}\n"
        "@stable(\"private\") record Private {}\n";
    TestCompilation invalid;
    CHECK(compile_source(&invalid, collisions));
    CHECK(has_diagnostic(&invalid, "SOL-IDENTITY-001"));
    CHECK(has_diagnostic(&invalid, "SOL-IDENTITY-002"));
    CHECK(has_diagnostic(&invalid, "SOL-IDENTITY-003"));
    free_compilation(&invalid);

    static const char malformed_span[] =
        "module malformed_identity\npublic record Value {}\n";
    CHECK(compile_source(&invalid, malformed_span));
    CHECK(!sol_diagnostics_has_errors(&invalid.diagnostics));
    invalid.syntax.items[0].stable_identity = (SolSpan){0, 1};
    reset_hir_diagnostics(&invalid);
    CHECK(!sol_hir_lower(
        &invalid.source, &invalid.syntax, &invalid.hir, &invalid.diagnostics
    ));
    CHECK(has_diagnostic(&invalid, "SOL-INTERNAL-002"));
    free_compilation(&invalid);
}

static void test_generic_type_namespace(void) {
    static const char text[] =
        "module generic_namespace\n"
        "record Box<T> { value: T, }\n"
        "enum Pair<T, U> { pair(first: T, second: U), }\n"
        "function project<T>(box: Box<T>) -> T { return box.value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.hir.type_resolution_count == compilation.syntax.type_count);
    size_t parameter_resolutions = 0;
    size_t definition_resolutions = 0;
    for (size_t index = 0; index < compilation.hir.type_resolution_count; ++index) {
        SolTypeResolution resolution = compilation.hir.type_resolutions[index];
        parameter_resolutions += resolution.kind == SOL_TYPE_RESOLUTION_PARAMETER ? 1 : 0;
        definition_resolutions += resolution.kind == SOL_TYPE_RESOLUTION_DEFINITION ? 1 : 0;
        if (resolution.kind == SOL_TYPE_RESOLUTION_PARAMETER) {
            CHECK(compilation.syntax.type_parameters[resolution.target].owner_item
                == compilation.syntax.types[index].owner_item);
        }
    }
    CHECK(parameter_resolutions == 5);
    CHECK(definition_resolutions == 1);
    free_compilation(&compilation);
}

static void test_type_declaration_hir(void) {
    static const char text[] =
        "module type_hir\n"
        "public type UserId = distinct Int64\n"
        "type Positive = refined Int64 where self > 0\n"
        "type Negative = refined Int64 where self < 0\n"
        "function consume(value: UserId) -> Int64 { return self }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-002"));
    size_t refinement_self_count = 0;
    bool outside_self_unresolved = false;
    bool declared_type_resolved = false;
    for (SolExprId expression = 0;
        expression < compilation.syntax.expression_count;
        ++expression) {
        if (compilation.syntax.expressions[expression].kind != SOL_EXPR_PATH
            || !span_text_equal(
                &compilation.source,
                compilation.syntax.expressions[expression].as.name,
                "self"
            )) continue;
        SolResolution resolution = compilation.hir.resolutions[expression];
        if (resolution.kind == SOL_RESOLUTION_REFINEMENT_SELF) {
            CHECK(resolution.target == 1 || resolution.target == 2);
            CHECK(compilation.hir.expression_owners[expression] == resolution.target);
            ++refinement_self_count;
        } else {
            CHECK(resolution.kind == SOL_RESOLUTION_ERROR);
            outside_self_unresolved = true;
        }
    }
    for (SolTypeId type = 0; type < compilation.syntax.type_count; ++type) {
        if (span_text_equal(&compilation.source, compilation.syntax.types[type].name,
                "UserId")) {
            CHECK(compilation.hir.type_resolutions[type].kind
                == SOL_TYPE_RESOLUTION_DEFINITION);
            CHECK(compilation.hir.type_resolutions[type].target == 0);
            declared_type_resolved = true;
        }
    }
    CHECK(refinement_self_count == 2);
    CHECK(outside_self_unresolved);
    CHECK(declared_type_resolved);

    compilation.syntax.items[0].flavor = SOL_TYPE_DECLARATION_NONE;
    reset_hir_diagnostics(&compilation);
    CHECK(!sol_hir_lower(&compilation.source, &compilation.syntax,
        &compilation.hir, &compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    compilation.syntax.items[0].flavor = SOL_TYPE_DECLARATION_DISTINCT;

    compilation.syntax.items[0].representation_type = SOL_AST_NONE;
    reset_hir_diagnostics(&compilation);
    CHECK(!sol_hir_lower(&compilation.source, &compilation.syntax,
        &compilation.hir, &compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    compilation.syntax.items[0].representation_type = 0;

    SolContractClauseId clause = compilation.syntax.items[1].first_contract;
    compilation.syntax.contract_clauses[clause].owner_kind
        = SOL_CONTRACT_OWNER_CAPABILITY_MEMBER;
    reset_hir_diagnostics(&compilation);
    CHECK(!sol_hir_lower(&compilation.source, &compilation.syntax,
        &compilation.hir, &compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    free_compilation(&compilation);
}

static void test_imported_public_type_resolution(void) {
    static const char text[] =
        "module consumer\n"
        "use types.Identifier\n"
        "public type Identifier = distinct Int64\n"
        "function consume(value: Identifier) -> Int64 { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolSpan import_path = compilation.syntax.imports[0].path;
    SolHirFileScope scopes[] = {
        {
            .module_name = (SolSpan){import_path.start, import_path.start + 5},
            .item_start = 0,
            .item_count = 1,
        },
        {
            .module_name = text_span(&compilation.source, "consumer"),
            .import_start = 0,
            .import_count = 1,
            .item_start = 1,
            .item_count = 1,
        },
    };
    reset_hir_diagnostics(&compilation);
    CHECK(sol_hir_lower_scoped(&compilation.source, &compilation.syntax,
        scopes, 2, &compilation.hir, &compilation.diagnostics));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    bool imported = false;
    for (SolTypeId type = 0; type < compilation.syntax.type_count; ++type) {
        if (compilation.syntax.types[type].owner_item == 1
            && span_text_equal(&compilation.source,
                compilation.syntax.types[type].name, "Identifier")) {
            CHECK(compilation.hir.type_resolutions[type].kind
                == SOL_TYPE_RESOLUTION_DEFINITION);
            CHECK(compilation.hir.type_resolutions[type].target == 0);
            imported = true;
        }
    }
    CHECK(imported);
    free_compilation(&compilation);
}

static void test_effect_row_resolution_identity(void) {
    static const char text[] =
        "module row_resolution\n"
        "function apply<effects E>(callback: function() -> Int64 effects E) -> Int64\n"
        "effects { E } { return callback() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.syntax.effect_parameter_count == 1);
    size_t row_references = 0;
    for (SolEffectId effect = 0; effect < compilation.hir.effect_resolution_count; ++effect) {
        SolEffectResolution resolution = compilation.hir.effect_resolutions[effect];
        if (resolution.kind == SOL_EFFECT_RESOLUTION_PARAMETER) {
            CHECK(resolution.target == 0);
            ++row_references;
        }
    }
    for (SolTypeId type = 0; type < compilation.hir.type_effect_resolution_count; ++type) {
        SolEffectResolution resolution = compilation.hir.type_effect_resolutions[type];
        if (resolution.kind == SOL_EFFECT_RESOLUTION_PARAMETER) {
            CHECK(resolution.target == 0);
            ++row_references;
        }
    }
    CHECK(row_references == 2);
    SolTypeId tail_type = SOL_AST_NONE;
    for (SolTypeId type = 0; type < compilation.syntax.type_count; ++type) {
        if (compilation.syntax.types[type].has_effect_tail) {
            tail_type = type;
            break;
        }
    }
    CHECK(tail_type != SOL_AST_NONE);
    if (tail_type != SOL_AST_NONE) {
        compilation.syntax.types[tail_type].effect_tail.end
            = compilation.syntax.types[tail_type].effect_tail.start - 1;
        reset_hir_diagnostics(&compilation);
        CHECK(!sol_hir_lower(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.diagnostics
        ));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    }
    free_compilation(&compilation);

    static const char duplicate[] =
        "module duplicate_row\n"
        "function bad<E, effects E>(callback: function() -> Int64 effects E) -> Int64\n"
        "effects { E } { return callback() }\n";
    CHECK(compile_source(&compilation, duplicate));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-005"));
    free_compilation(&compilation);

    static const char isolated[] =
        "module isolated_rows\n"
        "function first<effects E>(callback: function() -> Int64 effects E) -> Int64 effects { E } { return callback() }\n"
        "function second<effects E>(callback: function() -> Int64 effects E) -> Int64 effects { E } { return callback() }\n";
    CHECK(compile_source(&compilation, isolated));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.syntax.effect_parameter_count == 2);
    for (SolTypeId type = 0; type < compilation.syntax.type_count; ++type) {
        if (!compilation.syntax.types[type].has_effect_tail) continue;
        SolEffectResolution resolution
            = compilation.hir.type_effect_resolutions[type];
        CHECK(resolution.kind == SOL_EFFECT_RESOLUTION_PARAMETER);
        CHECK(compilation.syntax.effect_parameters[resolution.target].owner_item
            == compilation.syntax.types[type].owner_item);
    }
    free_compilation(&compilation);

    static const char out_of_scope[] =
        "module out_of_scope_row\n"
        "function bad(callback: function() -> Int64 effects E) -> Int64 effects { pure } { return 1 }\n";
    CHECK(compile_source(&compilation, out_of_scope));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-006"));
    free_compilation(&compilation);
}

static void test_duplicate_and_malformed_generic_parameters(void) {
    static const char duplicate[] =
        "module duplicate_generic\nrecord Box<T, T> { value: T, }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, duplicate));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-005"));
    free_compilation(&compilation);

    static const char valid[] =
        "module malformed_generic\nrecord Box<T> { value: T, }\n";
    CHECK(compile_source(&compilation, valid));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    compilation.syntax.type_parameters[0].next = 0;
    reset_hir_diagnostics(&compilation);
    CHECK(!sol_hir_lower(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    free_compilation(&compilation);

    static const char application[] =
        "module malformed_type_arguments\n"
        "record Box<T> { value: T, }\n"
        "function sample(value: Box<Int64>) -> Box<Int64> { return value }\n";
    CHECK(compile_source(&compilation, application));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.syntax.type_argument_count == 2);
    if (compilation.syntax.type_argument_count == 2) {
        compilation.syntax.type_arguments[0].next = 0;
        reset_hir_diagnostics(&compilation);
        CHECK(!sol_hir_lower(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.diagnostics
        ));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    }
    free_compilation(&compilation);

    CHECK(compile_source(&compilation, application));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolTypeId cyclic_type = SOL_AST_NONE;
    for (SolTypeId type = 0; type < compilation.syntax.type_count; ++type) {
        if (compilation.syntax.types[type].first_argument != SOL_AST_NONE) {
            cyclic_type = type;
            break;
        }
    }
    CHECK(cyclic_type != SOL_AST_NONE);
    if (cyclic_type != SOL_AST_NONE) {
        SolTypeArgumentId argument = compilation.syntax.types[cyclic_type].first_argument;
        compilation.syntax.type_arguments[argument].type = cyclic_type;
        reset_hir_diagnostics(&compilation);
        CHECK(!sol_hir_lower(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.diagnostics
        ));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    }
    free_compilation(&compilation);
}

static void test_scope_rules(void) {
    static const char text[] =
        "module scopes\n"
        "function bad(value: Int64) -> Int64 {\n"
        "    let repeated = value\n"
        "    let repeated = value\n"
        "    if true { let hidden = value hidden } else { 0 }\n"
        "    return hidden\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-003"));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-002"));
    free_compilation(&compilation);
}

static void test_qualified_function_resolution(void) {
    static const char text[] =
        "module qualified\n"
        "function service.client.value() -> Int64 { return 1 }\n"
        "function call() -> Int64 { return service /* lookup */ . client . value() }\n"
        "function shadow(service: Int64) -> Int64 { return service.client.value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    bool found_definition = false;
    bool found_shadow = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        const SolExpr *expression = &compilation.syntax.expressions[index];
        if (expression->kind == SOL_EXPR_FIELD
            && compilation.hir.resolutions[index].kind == SOL_RESOLUTION_DEFINITION) {
            CHECK(compilation.hir.resolutions[index].target == 0);
            found_definition = true;
        } else if (expression->kind == SOL_EXPR_FIELD) {
            SolExprId base = expression->as.field.base;
            if (compilation.syntax.expressions[base].kind == SOL_EXPR_PATH
                && compilation.hir.resolutions[base].kind == SOL_RESOLUTION_LOCAL) {
                found_shadow = true;
            }
        }
    }
    CHECK(found_definition);
    CHECK(found_shadow);
    free_compilation(&compilation);
}

static void test_qualified_duplicate_normalization(void) {
    static const char text[] =
        "module qualified_duplicate\n"
        "function service.value() -> Int64 { return 1 }\n"
        "function service /* same path */ . value() -> Int64 { return 2 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-001"));
    free_compilation(&compilation);
}

static void test_semantic_depth_limit(void) {
    char text[32768];
    size_t used = (size_t)snprintf(
        text,
        sizeof(text),
        "module deep\nfunction sum(value: Int64) -> Int64 { return value"
    );
    for (size_t index = 0; index < 600; ++index) {
        used += (size_t)snprintf(text + used, sizeof(text) - used, "+value");
    }
    snprintf(text + used, sizeof(text) - used, " }\n");

    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-004"));
    free_compilation(&compilation);
}

static void test_malformed_ast_rejected(void) {
    static const char text[] =
        "module malformed_ast\n"
        "function value() -> Int64 { return 1 + 2 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolExprId binary = SOL_AST_NONE;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind == SOL_EXPR_BINARY) {
            binary = index;
            break;
        }
    }
    CHECK(binary != SOL_AST_NONE);
    if (binary != SOL_AST_NONE) {
        compilation.syntax.expressions[binary].as.binary.left = SOL_AST_NONE;
    }

    sol_hir_module_free(&compilation.hir);
    sol_hir_module_init(&compilation.hir);
    sol_diagnostics_free(&compilation.diagnostics);
    sol_diagnostics_init(&compilation.diagnostics);
    CHECK(!sol_hir_lower(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    free_compilation(&compilation);
}

static void test_malformed_arena_metadata_rejected(void) {
    SolSource source;
    SolSyntaxTree syntax;
    SolHirModule hir;
    SolDiagnostics diagnostics;
    CHECK(sol_source_from_text(&source, "metadata.sol", "module metadata\n"));
    sol_syntax_tree_init(&syntax);
    sol_hir_module_init(&hir);
    sol_diagnostics_init(&diagnostics);
    syntax.item_count = 1;
    syntax.item_capacity = 1;
    CHECK(!sol_hir_lower(&source, &syntax, &hir, &diagnostics));
    bool found = false;
    for (size_t index = 0; index < diagnostics.count; ++index) {
        found = found || strcmp(diagnostics.items[index].code, "SOL-INTERNAL-002") == 0;
    }
    CHECK(found);

    sol_diagnostics_free(&diagnostics);
    sol_hir_module_free(&hir);
    sol_syntax_tree_free(&syntax);
    sol_source_free(&source);
}

static void test_scoped_package_resolution(void) {
    static const char text[] =
        "module alpha\n"
        "use beta.External\n"
        "public function External() -> Int64 { return 1 }\n"
        "function call() -> Int64 { return External() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.syntax.item_count == 2);
    CHECK(compilation.syntax.import_count == 1);

    SolSpan import_path = compilation.syntax.imports[0].path;
    SolHirFileScope scopes[] = {
        {
            .module_name = (SolSpan){import_path.start, import_path.start + 4},
            .import_start = 0,
            .import_count = 0,
            .item_start = 0,
            .item_count = 1,
        },
        {
            .module_name = text_span(&compilation.source, "alpha"),
            .import_start = 0,
            .import_count = 1,
            .item_start = 1,
            .item_count = 1,
        },
    };
    reset_hir_diagnostics(&compilation);
    CHECK(sol_hir_lower_scoped(
        &compilation.source, &compilation.syntax, scopes, 2,
        &compilation.hir, &compilation.diagnostics
    ));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.hir.file_scope_count == 2);
    CHECK(compilation.hir.item_files[0] == 0);
    CHECK(compilation.hir.item_files[1] == 1);
    bool imported = false;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        if (compilation.syntax.expressions[index].kind == SOL_EXPR_PATH
            && span_text_equal(
                &compilation.source,
                compilation.syntax.expressions[index].as.name,
                "External"
            ) && compilation.hir.resolutions[index].kind
                == SOL_RESOLUTION_DEFINITION) {
            CHECK(compilation.hir.resolutions[index].target == 0);
            imported = true;
        }
    }
    CHECK(imported);

    scopes[1].item_count = 2;
    reset_hir_diagnostics(&compilation);
    CHECK(!sol_hir_lower_scoped(
        &compilation.source, &compilation.syntax, scopes, 2,
        &compilation.hir, &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    free_compilation(&compilation);
}

static void test_scoped_import_requires_exact_module(void) {
    static const char text[] =
        "module consumer\n"
        "use alpha.beta.Missing\n"
        "public function Present() -> Int64 { return 1 }\n"
        "function call() -> Int64 { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolSpan path = compilation.syntax.imports[0].path;
    SolHirFileScope scopes[] = {
        {
            .module_name = (SolSpan){path.start, path.start + 5},
            .item_start = 0,
            .item_count = 1,
        },
        {
            .module_name = text_span(&compilation.source, "consumer"),
            .import_start = 0,
            .import_count = 1,
            .item_start = 1,
            .item_count = 1,
        },
    };
    reset_hir_diagnostics(&compilation);
    CHECK(sol_hir_lower_scoped(
        &compilation.source, &compilation.syntax, scopes, 2,
        &compilation.hir, &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-008"));
    CHECK(!has_diagnostic(&compilation, "SOL-RESOLVE-009"));
    free_compilation(&compilation);
}

static void test_scoped_builtin_type_precedence(void) {
    static const char text[] =
        "module consumer\n"
        "use types.Text\n"
        "public record Text {}\n"
        "record Int64 {}\n"
        "function builtin(value: Text) -> Int64 { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolSpan path = compilation.syntax.imports[0].path;
    SolHirFileScope scopes[] = {
        {
            .module_name = (SolSpan){path.start, path.start + 5},
            .item_start = 0,
            .item_count = 1,
        },
        {
            .module_name = text_span(&compilation.source, "consumer"),
            .import_start = 0,
            .import_count = 1,
            .item_start = 1,
            .item_count = 2,
        },
    };
    reset_hir_diagnostics(&compilation);
    CHECK(sol_hir_lower_scoped(
        &compilation.source, &compilation.syntax, scopes, 2,
        &compilation.hir, &compilation.diagnostics
    ));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    for (size_t type = 0; type < compilation.syntax.type_count; ++type) {
        SolSpan name = compilation.syntax.types[type].name;
        if (span_text_equal(&compilation.source, name, "Text")) {
            CHECK(compilation.hir.type_resolutions[type].kind
                == SOL_TYPE_RESOLUTION_BUILTIN);
            CHECK(compilation.hir.type_resolutions[type].target
                == SOL_TYPE_BUILTIN_TEXT);
        } else if (span_text_equal(&compilation.source, name, "Int64")) {
            CHECK(compilation.hir.type_resolutions[type].kind
                == SOL_TYPE_RESOLUTION_BUILTIN);
            CHECK(compilation.hir.type_resolutions[type].target
                == SOL_TYPE_BUILTIN_INT64);
        }
    }
    free_compilation(&compilation);
}

static void test_effectcheck_rejects_forged_scoped_metadata(void) {
    static const char text[] =
        "module scoped_effect\n"
        "function value() -> Int64 effects { pure } { return 1 }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolHirFileScope scope = {
        .module_name = compilation.syntax.module_name,
        .item_start = 0,
        .item_count = compilation.syntax.item_count,
    };
    reset_hir_diagnostics(&compilation);
    CHECK(sol_hir_lower_scoped(
        &compilation.source, &compilation.syntax, &scope, 1,
        &compilation.hir, &compilation.diagnostics
    ));
    SolTypeTable types;
    SolEffectTable effects;
    sol_type_table_init(&types);
    sol_effect_table_init(&effects);
    CHECK(sol_type_check(
        &compilation.source, &compilation.syntax, &compilation.hir,
        &types, &compilation.diagnostics
    ));

    SolHirFileScope *file_scopes = compilation.hir.file_scopes;
    compilation.hir.file_scopes = NULL;
    sol_diagnostics_free(&compilation.diagnostics);
    sol_diagnostics_init(&compilation.diagnostics);
    CHECK(!sol_effect_check(
        &compilation.source, &compilation.syntax, &compilation.hir,
        &types, &effects, &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    compilation.hir.file_scopes = file_scopes;

    compilation.hir.item_files[0] = 1;
    sol_diagnostics_free(&compilation.diagnostics);
    sol_diagnostics_init(&compilation.diagnostics);
    CHECK(!sol_effect_check(
        &compilation.source, &compilation.syntax, &compilation.hir,
        &types, &effects, &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    compilation.hir.item_files[0] = 0;

    SolSemanticReference *semantic_references = compilation.hir.semantic_references;
    compilation.hir.semantic_references = NULL;
    sol_diagnostics_free(&compilation.diagnostics);
    sol_diagnostics_init(&compilation.diagnostics);
    CHECK(!sol_effect_check(
        &compilation.source, &compilation.syntax, &compilation.hir,
        &types, &effects, &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-004"));
    compilation.hir.semantic_references = semantic_references;

    sol_effect_table_free(&effects);
    sol_type_table_free(&types);
    free_compilation(&compilation);
}

static void test_derived_capability_resolution_and_malformed_body(void) {
    static const char text[] =
        "module derived_resolution\n"
        "capability Root {}\n"
        "capability Wrapper derives_from source: capability Root {\n"
        "    function invalid() -> capability Root effects { pure } { return Self }\n"
        "}\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(has_diagnostic(&compilation, "SOL-RESOLVE-002"));
    free_compilation(&compilation);

    static const char valid_text[] =
        "module malformed_derived_body\n"
        "capability Root {}\n"
        "capability Wrapper derives_from source: capability Root {\n"
        "    function root() -> capability Root\n"
        "    authority { result derives_from Self }\n"
        "    effects { pure } { return source }\n"
        "}\n";
    CHECK(compile_source(&compilation, valid_text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    SolExprId body = compilation.syntax.capability_members[0].body;
    compilation.syntax.capability_members[0].body = compilation.syntax.expression_count;
    sol_hir_module_free(&compilation.hir);
    sol_hir_module_init(&compilation.hir);
    sol_diagnostics_free(&compilation.diagnostics);
    sol_diagnostics_init(&compilation.diagnostics);
    CHECK(!sol_hir_lower(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    compilation.syntax.capability_members[0].body = body;
    free_compilation(&compilation);
}

static void test_malformed_handler_ast_rejected(void) {
    static const char text[] =
        "module malformed_handler_ast\n"
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
        compilation.syntax.expressions[handler].as.handle.body
            = compilation.syntax.expression_count;
        sol_hir_module_free(&compilation.hir);
        sol_hir_module_init(&compilation.hir);
        sol_diagnostics_free(&compilation.diagnostics);
        sol_diagnostics_init(&compilation.diagnostics);
        CHECK(!sol_hir_lower(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.diagnostics
        ));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    }
    free_compilation(&compilation);
}

static void reset_hir_diagnostics(TestCompilation *compilation) {
    sol_hir_module_free(&compilation->hir);
    sol_hir_module_init(&compilation->hir);
    sol_diagnostics_free(&compilation->diagnostics);
    sol_diagnostics_init(&compilation->diagnostics);
}

static void test_contract_resolution_and_cycles(void) {
    static const char text[] =
        "module contract_resolution\n"
        "function same(value: Int64) -> Bool effects { pure } { return value == value }\n"
        "function sample(value: Int64) -> Int64\n"
        "requires { same(value) }\n"
        "ensures { result == old(value) }\n"
        "{ return value }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.syntax.contract_clause_count == 2);
    CHECK(compilation.syntax.contract_condition_count == 2);
    SolExprId old_expression = SOL_AST_NONE;
    for (size_t index = 0; index < compilation.syntax.expression_count; ++index) {
        const SolExpr *expression = &compilation.syntax.expressions[index];
        if (expression->kind == SOL_EXPR_OLD) {
            old_expression = index;
            CHECK(compilation.hir.resolutions[index].kind == SOL_RESOLUTION_NOT_APPLICABLE);
        } else if (expression->kind == SOL_EXPR_RESULT) {
            CHECK(compilation.hir.resolutions[index].kind == SOL_RESOLUTION_NOT_APPLICABLE);
        } else if (expression->kind == SOL_EXPR_PATH
            && span_text_equal(&compilation.source, expression->as.name, "same")) {
            CHECK(compilation.hir.resolutions[index].kind == SOL_RESOLUTION_DEFINITION);
            CHECK(compilation.hir.resolutions[index].target == 0);
        } else if (expression->kind == SOL_EXPR_PATH
            && span_text_equal(&compilation.source, expression->as.name, "value")) {
            CHECK(compilation.hir.resolutions[index].kind == SOL_RESOLUTION_LOCAL);
        }
    }
    CHECK(!has_diagnostic(&compilation, "SOL-RESOLVE-002"));
    CHECK(old_expression != SOL_AST_NONE);

    SolContractConditionId first
        = compilation.syntax.contract_clauses[0].first_condition;
    SolContractConditionId next = compilation.syntax.contract_conditions[first].next;
    compilation.syntax.contract_conditions[first].next = first;
    reset_hir_diagnostics(&compilation);
    CHECK(!sol_hir_lower(
        &compilation.source,
        &compilation.syntax,
        &compilation.hir,
        &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    compilation.syntax.contract_conditions[first].next = next;

    if (old_expression != SOL_AST_NONE) {
        SolExprId operand
            = compilation.syntax.expressions[old_expression].as.old_expression;
        compilation.syntax.expressions[old_expression].as.old_expression = old_expression;
        reset_hir_diagnostics(&compilation);
        CHECK(!sol_hir_lower(
            &compilation.source,
            &compilation.syntax,
            &compilation.hir,
            &compilation.diagnostics
        ));
        CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
        compilation.syntax.expressions[old_expression].as.old_expression = operand;
    }
    free_compilation(&compilation);
}

static void test_trait_resolution_and_malformed_metadata(void) {
    static const char text[] =
        "module traits\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "implementation Show for Int64 { function show(self: Self) -> Text effects { pure } { return \"ok\" } }\n"
        "function render<T: Show>(value: T) -> Text effects { pure } { return value.show() }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(compilation.hir.trait_resolutions[1].kind == SOL_RESOLUTION_DEFINITION);
    CHECK(compilation.hir.trait_resolutions[1].target == 0);
    CHECK(compilation.hir.bound_resolutions[0].target == 0);
    SolTraitMethodId first = compilation.syntax.items[0].first_trait_method;
    compilation.syntax.trait_methods[first].next = first;
    reset_hir_diagnostics(&compilation);
    CHECK(!sol_hir_lower(&compilation.source, &compilation.syntax,
        &compilation.hir, &compilation.diagnostics));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    free_compilation(&compilation);
}

static void test_malformed_implementation_trait_span(void) {
    static const char text[] =
        "module malformed_trait_span\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "implementation Show for Int64 { function show(self: Self) -> Text effects { pure } { return \"ok\" } }\n";
    TestCompilation compilation;
    CHECK(compile_source(&compilation, text));
    CHECK(!sol_diagnostics_has_errors(&compilation.diagnostics));
    compilation.syntax.items[1].trait_name = (SolSpan){
        .start = compilation.source.length + 1,
        .end = compilation.source.length + 5,
    };
    reset_hir_diagnostics(&compilation);
    CHECK(!sol_hir_lower(
        &compilation.source, &compilation.syntax, &compilation.hir,
        &compilation.diagnostics
    ));
    CHECK(has_diagnostic(&compilation, "SOL-INTERNAL-002"));
    free_compilation(&compilation);
}

int main(void) {
    test_successful_resolution();
    test_unresolved_name();
    test_duplicate_declaration();
    test_stable_semantic_identity();
    test_generic_type_namespace();
    test_type_declaration_hir();
    test_imported_public_type_resolution();
    test_effect_row_resolution_identity();
    test_duplicate_and_malformed_generic_parameters();
    test_scope_rules();
    test_qualified_function_resolution();
    test_qualified_duplicate_normalization();
    test_semantic_depth_limit();
    test_malformed_ast_rejected();
    test_malformed_arena_metadata_rejected();
    test_scoped_package_resolution();
    test_scoped_import_requires_exact_module();
    test_scoped_builtin_type_precedence();
    test_effectcheck_rejects_forged_scoped_metadata();
    test_derived_capability_resolution_and_malformed_body();
    test_malformed_handler_ast_rejected();
    test_contract_resolution_and_cycles();
    test_trait_resolution_and_malformed_metadata();
    test_malformed_implementation_trait_span();
    if (failures != 0) {
        fprintf(stderr, "%d semantic test failure(s)\n", failures);
        return 1;
    }
    puts("semantic tests passed");
    return 0;
}
