#include "sol/ir.h"
#include "sol/lexer.h"
#include "sol/package.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures;

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
    SolIr ir;
} TestCompilation;

static bool frontend(TestCompilation *compilation, const char *text) {
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
    return sol_source_from_text(&compilation->source, "ir.sol", text)
        && sol_lex(&compilation->source, &compilation->tokens, &compilation->diagnostics)
        && sol_parse(&compilation->source, &compilation->tokens,
            &compilation->syntax, &compilation->diagnostics)
        && !sol_diagnostics_has_errors(&compilation->diagnostics)
        && sol_hir_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->diagnostics)
        && !sol_diagnostics_has_errors(&compilation->diagnostics)
        && sol_type_check(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->diagnostics)
        && !sol_diagnostics_has_errors(&compilation->diagnostics)
        && sol_effect_check(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types,
            &compilation->effects, &compilation->diagnostics)
        && !sol_diagnostics_has_errors(&compilation->diagnostics)
        && sol_contract_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics)
        && !sol_diagnostics_has_errors(&compilation->diagnostics);
}

static bool compile_ir(TestCompilation *compilation, const char *text) {
    return frontend(compilation, text)
        && sol_ir_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->ir, &compilation->diagnostics);
}

static void free_frontend(TestCompilation *compilation) {
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_syntax_tree_free(&compilation->syntax);
    sol_tokens_free(&compilation->tokens);
    sol_source_free(&compilation->source);
}

static void free_compilation(TestCompilation *compilation) {
    sol_ir_free(&compilation->ir);
    free_frontend(compilation);
    sol_diagnostics_free(&compilation->diagnostics);
}

static bool ir_equal(const SolIr *left, const SolIr *right) {
    if (left->source_length != right->source_length
        || memcmp(left->source_bytes, right->source_bytes, left->source_length) != 0
        || left->type_count != right->type_count
        || left->type_id_count != right->type_id_count
        || left->access_count != right->access_count
        || left->definition_count != right->definition_count
        || left->callable_count != right->callable_count
        || left->member_count != right->member_count
        || left->evidence_count != right->evidence_count
        || left->local_count != right->local_count
        || left->field_count != right->field_count
        || left->variant_count != right->variant_count
        || left->expression_count != right->expression_count
        || left->statement_count != right->statement_count
        || left->statement_id_count != right->statement_id_count
        || left->arm_count != right->arm_count
        || left->arm_id_count != right->arm_id_count
        || left->cleanup_local_count != right->cleanup_local_count
        || left->operand_count != right->operand_count
        || left->root_count != right->root_count
        || left->effect_count != right->effect_count
        || left->obligation_count != right->obligation_count
        || left->snapshot_count != right->snapshot_count
        || left->file_count != right->file_count) return false;
    if (memcmp(left->types, right->types, left->type_count * sizeof(*left->types)) != 0
        || memcmp(left->type_ids, right->type_ids,
            left->type_id_count * sizeof(*left->type_ids)) != 0
        || memcmp(left->accesses, right->accesses,
            left->access_count * sizeof(*left->accesses)) != 0
        || memcmp(left->operands, right->operands,
            left->operand_count * sizeof(*left->operands)) != 0
        || memcmp(left->members, right->members,
            left->member_count * sizeof(*left->members)) != 0
        || memcmp(left->evidence, right->evidence,
            left->evidence_count * sizeof(*left->evidence)) != 0
        || memcmp(left->statement_ids, right->statement_ids,
            left->statement_id_count * sizeof(*left->statement_ids)) != 0
        || memcmp(left->arm_ids, right->arm_ids,
            left->arm_id_count * sizeof(*left->arm_ids)) != 0
        || memcmp(left->cleanup_locals, right->cleanup_locals,
            left->cleanup_local_count * sizeof(*left->cleanup_locals)) != 0
        || memcmp(left->roots, right->roots,
            left->root_count * sizeof(*left->roots)) != 0) return false;
    for (size_t index = 0; index < left->definition_count; ++index) {
        if (left->definitions[index].semantic_id.high
                != right->definitions[index].semantic_id.high
            || left->definitions[index].semantic_id.low
                != right->definitions[index].semantic_id.low
            || strcmp(left->definitions[index].name, right->definitions[index].name) != 0) {
            return false;
        }
    }
    for (size_t index = 0; index < left->effect_count; ++index) {
        if (strcmp(left->effects[index].name, right->effects[index].name) != 0
            || left->effects[index].authority_kind != right->effects[index].authority_kind
            || left->effects[index].authority != right->effects[index].authority) return false;
    }
    for (size_t index = 0; index < left->expression_count; ++index) {
        if (left->expressions[index].kind != right->expressions[index].kind
            || left->expressions[index].local_use
                != right->expressions[index].local_use
            || left->expressions[index].type != right->expressions[index].type
            || left->expressions[index].span.start != right->expressions[index].span.start
            || left->expressions[index].span.end != right->expressions[index].span.end
            || (left->expressions[index].kind == SOL_IR_EXPR_BLOCK
                && (left->expressions[index].as.block.statements.offset
                        != right->expressions[index].as.block.statements.offset
                    || left->expressions[index].as.block.statements.count
                        != right->expressions[index].as.block.statements.count
                    || left->expressions[index].as.block.cleanup.offset
                        != right->expressions[index].as.block.cleanup.offset
                    || left->expressions[index].as.block.cleanup.count
                        != right->expressions[index].as.block.cleanup.count))) {
            return false;
        }
    }
    for (size_t index = 0; index < left->statement_count; ++index) {
        const SolIrStatement *a = &left->statements[index];
        const SolIrStatement *b = &right->statements[index];
        if (a->kind != b->kind || a->local != b->local || a->target != b->target
            || a->expression != b->expression || a->span.start != b->span.start
            || a->span.end != b->span.end
            || a->region_label_span.start != b->region_label_span.start
            || a->region_label_span.end != b->region_label_span.end
            || ((a->region_label == NULL) != (b->region_label == NULL))
            || (a->region_label != NULL
                && strcmp(a->region_label, b->region_label) != 0)) return false;
    }
    for (size_t index = 0; index < left->local_count; ++index) {
        if (left->locals[index].mutable != right->locals[index].mutable) return false;
    }
    for (size_t index = 0; index < left->arm_count; ++index) {
        if (left->arms[index].cleanup.offset != right->arms[index].cleanup.offset
            || left->arms[index].cleanup.count != right->arms[index].cleanup.count) {
            return false;
        }
    }
    for (size_t index = 0; index < left->file_count; ++index) {
        if (strcmp(left->files[index].path, right->files[index].path) != 0
            || left->files[index].aggregate_start != right->files[index].aggregate_start
            || left->files[index].aggregate_end != right->files[index].aggregate_end) {
            return false;
        }
    }
    return true;
}

static bool has_code(const TestCompilation *compilation, const char *code) {
    for (size_t index = 0; index < compilation->diagnostics.count; ++index) {
        if (strcmp(compilation->diagnostics.items[index].code, code) == 0) return true;
    }
    return false;
}

static char *growth_source(void) {
    size_t capacity = 65536;
    char *text = malloc(capacity);
    if (text == NULL) return NULL;
    size_t length = 0;
#define APPEND(...) \
    do { \
        int written = snprintf(text + length, capacity - length, __VA_ARGS__); \
        if (written < 0 || (size_t)written >= capacity - length) { \
            free(text); \
            return NULL; \
        } \
        length += (size_t)written; \
    } while (0)
    APPEND("module growth\n");
    APPEND("enum Choice { yes(value: Int64), no }\n");
    APPEND("capability Measure {\n");
    for (size_t index = 0; index < 20; ++index) {
        APPEND("function m%zu(value: Int64) -> Int64 effects { pure }\n", index);
    }
    APPEND("}\n");
    for (size_t index = 0; index < 20; ++index) {
        APPEND("record R%zu { value: ", index);
        for (size_t depth = 0; depth <= index; ++depth) APPEND("Option<");
        APPEND("Int64");
        for (size_t depth = 0; depth <= index; ++depth) APPEND(">");
        APPEND(" }\n");
    }
    APPEND("function base(value: Int64) -> Int64 effects { panic } { return value }\n");
    for (size_t index = 0; index < 40; ++index) {
        APPEND("function f%zu(value: Int64, choice: Choice) -> Int64 effects { panic } "
            "{ return { let picked = match choice { yes(item) => item, no => value } "
            "base(picked) } }\n", index);
    }
#undef APPEND
    return text;
}

static void test_geometric_growth(void) {
    char *text = growth_source();
    CHECK(text != NULL);
    if (text == NULL) return;
    TestCompilation compilation;
    bool compiled = compile_ir(&compilation, text);
    CHECK(compiled);
    if (!compiled) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
        free_compilation(&compilation);
        free(text);
        return;
    }
    CHECK(compilation.ir.type_count > 16);
    CHECK(compilation.ir.type_id_count > 16);
    CHECK(compilation.ir.callable_count > 32);
    CHECK(compilation.ir.member_count > 16);
    CHECK(compilation.ir.statement_id_count > 32);
    CHECK(compilation.ir.arm_id_count > 32);
    CHECK(compilation.ir.operand_count > 32);
    CHECK(compilation.ir.root_count > 32);
    CHECK(compilation.ir.effect_count > 32);
    CHECK(sol_ir_validate(&compilation.ir, &compilation.diagnostics));
    free_compilation(&compilation);
    free(text);
}

static void test_complete_ir_and_lifetime(void) {
    static const char text[] =
        "module complete\n"
        "record Pair { left: Option<Int64>, right: Int64 }\n"
        "enum Choice { yes(value: Int64), no }\n"
        "function make() -> Option<Option<Int64>> effects { panic, diverge } { return some(none()) }\n"
        "function get(pair: Pair) -> Int64 { return pair.right }\n"
        "function choose(choice: Choice) -> Int64 ensures { result == result } "
        "{ return match choice { yes(value) => value, no => 0 } }\n";
    TestCompilation left;
    TestCompilation right;
    CHECK(compile_ir(&left, text));
    CHECK(compile_ir(&right, text));
    if (left.ir.source_bytes == NULL || right.ir.source_bytes == NULL) {
        free_compilation(&left);
        free_compilation(&right);
        return;
    }
    CHECK(ir_equal(&left.ir, &right.ir));
    CHECK(left.ir.definition_count == 5);
    CHECK(left.ir.definitions[0].semantic_id.high
        == right.ir.definitions[0].semantic_id.high);
    bool field = false;
    bool pattern = false;
    bool nested_none = false;
    for (size_t index = 0; index < left.ir.expression_count; ++index) {
        const SolIrExpression *expression = &left.ir.expressions[index];
        field = field || expression->kind == SOL_IR_EXPR_FIELD;
        nested_none = nested_none || (expression->kind == SOL_IR_EXPR_CALL
            && expression->as.call.kind == SOL_IR_CALL_BUILTIN_NONE);
    }
    for (size_t index = 0; index < left.ir.arm_count; ++index) {
        pattern = pattern || left.ir.arms[index].kind == SOL_IR_PATTERN_VARIANT;
    }
    CHECK(field);
    CHECK(pattern);
    CHECK(nested_none);
    bool nested_type = false;
    for (size_t index = 0; index < left.ir.type_count; ++index) {
        const SolIrType *outer = &left.ir.types[index];
        if (outer->kind != SOL_IR_TYPE_OPTION || outer->argument_count != 1) continue;
        SolIrTypeId child = left.ir.type_ids[outer->argument_offset];
        if (child < left.ir.type_count
            && left.ir.types[child].kind == SOL_IR_TYPE_OPTION
            && left.ir.types[child].argument_count == 1) {
            SolIrTypeId leaf = left.ir.type_ids[left.ir.types[child].argument_offset];
            nested_type = leaf < left.ir.type_count
                && left.ir.types[leaf].kind == SOL_IR_TYPE_INT64;
        }
    }
    CHECK(nested_type);
    bool normalized = false;
    for (size_t index = 0; index + 1 < left.ir.effect_count; ++index) {
        if (strcmp(left.ir.effects[index].name, "diverge") == 0
            && strcmp(left.ir.effects[index + 1].name, "panic") == 0) normalized = true;
    }
    CHECK(normalized);
    free_frontend(&left);
    CHECK(sol_ir_validate(&left.ir, &left.diagnostics));
    CHECK(strstr(left.ir.source_bytes, "module complete") != NULL);
    sol_ir_free(&left.ir);
    sol_diagnostics_free(&left.diagnostics);
    free_compilation(&right);
}

static void test_variants_nested_arenas_and_evidence(void) {
    static const char text[] =
        "module executable\n"
        "enum Choice { yes(value: Int64), no }\n"
        "trait Show { function show(self: Self, prefix: Text) -> Text effects { pure } }\n"
        "implementation Show for Int64 { function show(self: Self, renamed: Text) -> Text effects { pure } { return renamed } }\n"
        "function render<T: Show>(value: T) -> Text effects { pure } { return value.show(prefix = \"x\") }\n"
        "function concrete(value: Int64) -> Text effects { pure } { return render(value) }\n"
        "function empty() -> Choice { return Choice.no }\n"
        "function nested(flag: Bool, choice: Choice) -> Int64 { return { if flag { match choice { yes(value) => value, no => 0 } } else { 1 } } }\n";
    TestCompilation compilation;
    bool compiled = compile_ir(&compilation, text);
    CHECK(compiled);
    if (!compiled) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
        free_compilation(&compilation);
        return;
    }
    bool variant = false;
    bool evidence = false;
    bool nested = false;
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        const SolIrExpression *expression = &compilation.ir.expressions[index];
        variant = variant || expression->kind == SOL_IR_EXPR_VARIANT;
        evidence = evidence || (expression->kind == SOL_IR_EXPR_CALL
            && expression->as.call.kind == SOL_IR_CALL_METHOD
            && expression->as.call.evidence.count != 0);
        nested = nested || (expression->kind == SOL_IR_EXPR_BLOCK
            && expression->as.block.statements.count != 0
            && expression->as.block.statements.offset
                < compilation.ir.statement_id_count);
        if (expression->kind == SOL_IR_EXPR_MATCH) {
            CHECK(expression->as.match_expr.arms.offset
                + expression->as.match_expr.arms.count <= compilation.ir.arm_id_count);
        }
    }
    CHECK(variant);
    CHECK(evidence);
    CHECK(nested);
    for (size_t definition = 0; definition < compilation.ir.definition_count; ++definition) {
        SolIrSlice members = compilation.ir.definitions[definition].members;
        CHECK(members.offset + members.count <= compilation.ir.member_count);
        for (size_t index = 0; index < members.count; ++index) {
            CHECK(compilation.ir.members[members.offset + index].callable
                < compilation.ir.callable_count);
        }
    }
    free_compilation(&compilation);
}

static void test_context_probes_and_escapes(void) {
    TestCompilation compilation;
    bool compiled = compile_ir(&compilation,
        "module probes\n"
        "enum Failure { bad }\n"
        "type Wrapped = distinct Option<Int64>\n"
        "function probe() -> Option<Int64> { { let unrelated = 1 none() } }\n"
        "function wrapped() -> Wrapped { return Wrapped(none()) }\n"
        "function propagated() -> Option<Int64> { none()? return none() }\n"
        "function failed() -> Result<Int64, Failure> { err(Failure.bad)? return ok(1) }\n"
        "function contract() -> Option<Int64> ensures { result == none() } { return none() }\n"
        "function escaped() -> Text { return \"a\\n\\t\\\\\\\"b\" }\n");
    CHECK(compiled);
    if (!compiled) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    bool decoded = false;
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        const SolIrExpression *expression = &compilation.ir.expressions[index];
        if (expression->kind == SOL_IR_EXPR_STRING
            && strcmp(expression->as.string, "a\n\t\\\"b") == 0) decoded = true;
    }
    CHECK(decoded);
    free_compilation(&compilation);

    CHECK(compile_ir(&compilation,
        "module equality_context\n"
        "function left() -> Bool { return none() == some(1) }\n"
        "function right() -> Bool { return some(1) == none() }\n"
        "function nested_left() -> Bool { return some(none()) == some(some(1)) }\n"
        "function nested_right() -> Bool { return some(some(1)) == some(none()) }\n"));
    free_compilation(&compilation);

    CHECK(!frontend(&compilation,
        "module equality_ambiguous\n"
        "function bad() -> Bool { return some(none()) == some(none()) }\n"));
    CHECK(has_code(&compilation, "SOL-TYPE-025"));
    free_compilation(&compilation);

    CHECK(!frontend(&compilation,
        "module bad_escape\nfunction bad() -> Text { return \"bad\\q\" }\n"));
    CHECK(has_code(&compilation, "SOL-LEX-003"));
    free_compilation(&compilation);
}

static void test_generic_metadata_and_cycles(void) {
    TestCompilation compilation;
    CHECK(compile_ir(&compilation,
        "module generic_metadata\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "implementation Show for Int64 { function show(self: Self) -> Text effects { pure } { return \"ok\" } }\n"
        "function f<A: Show, B>(x: B, y: A) -> A { return y }\n"));
    CHECK(compilation.ir.generic_parameter_count == 2);
    SolIrDefinitionId f = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.definition_count; ++index) {
        if (strcmp(compilation.ir.definitions[index].name, "f") == 0) f = index;
    }
    CHECK(f != SOL_IR_NONE);
    if (f != SOL_IR_NONE) {
        SolIrSlice parameters = compilation.ir.definitions[f].generic_parameters;
        CHECK(parameters.count == 2);
        CHECK(strcmp(compilation.ir.generic_parameters[parameters.offset].name, "A") == 0);
        CHECK(compilation.ir.generic_parameters[parameters.offset].ordinal == 0);
        CHECK(compilation.ir.generic_parameters[parameters.offset].trait_bound != SOL_IR_NONE);
        CHECK(strcmp(compilation.ir.generic_parameters[parameters.offset + 1].name, "B") == 0);
        CHECK(compilation.ir.generic_parameters[parameters.offset + 1].ordinal == 1);
    }
    free_frontend(&compilation);
    CHECK(sol_ir_validate(&compilation.ir, &compilation.diagnostics));
    sol_ir_free(&compilation.ir);
    sol_diagnostics_free(&compilation.diagnostics);

    CHECK(frontend(&compilation,
        "module cycle\nfunction value() -> Option<Int64> { return none() }\n"));
    CHECK(compilation.types.type_application_count != 0);
    if (compilation.types.type_application_count != 0) {
        SolTypeApplication *application = &compilation.types.type_applications[0];
        SolType saved = compilation.types.type_application_arguments[
            application->argument_offset
        ];
        compilation.types.type_application_arguments[application->argument_offset]
            = (SolType){SOL_TYPE_APPLICATION, 0};
        SolIr empty;
        SolDiagnostics diagnostics;
        sol_ir_init(&empty);
        sol_diagnostics_init(&diagnostics);
        CHECK(!sol_ir_lower(&compilation.source, &compilation.syntax,
            &compilation.hir, &compilation.types, &compilation.effects,
            &compilation.contracts, &empty, &diagnostics));
        CHECK(empty.source_bytes == NULL);
        sol_ir_free(&empty);
        sol_diagnostics_free(&diagnostics);
        compilation.types.type_application_arguments[application->argument_offset] = saved;
    }
    free_compilation(&compilation);

    CHECK(frontend(&compilation,
        "module mutual_cycle\nfunction value() -> Option<Option<Int64>> { return none() }\n"));
    if (compilation.types.type_application_count >= 2) {
        SolTypeApplication *first = &compilation.types.type_applications[0];
        SolTypeApplication *second = &compilation.types.type_applications[1];
        SolType saved_first = compilation.types.type_application_arguments[first->argument_offset];
        SolType saved_second = compilation.types.type_application_arguments[second->argument_offset];
        compilation.types.type_application_arguments[first->argument_offset]
            = (SolType){SOL_TYPE_APPLICATION, 1};
        compilation.types.type_application_arguments[second->argument_offset]
            = (SolType){SOL_TYPE_APPLICATION, 0};
        SolIr empty;
        SolDiagnostics diagnostics;
        sol_ir_init(&empty);
        sol_diagnostics_init(&diagnostics);
        CHECK(!sol_ir_lower(&compilation.source, &compilation.syntax,
            &compilation.hir, &compilation.types, &compilation.effects,
            &compilation.contracts, &empty, &diagnostics));
        sol_ir_free(&empty);
        sol_diagnostics_free(&diagnostics);
        compilation.types.type_application_arguments[first->argument_offset] = saved_first;
        compilation.types.type_application_arguments[second->argument_offset] = saved_second;
    }
    free_compilation(&compilation);
}

static void test_classified_calls_handler_contract(void) {
    static const char text[] =
        "module metadata\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "implementation Show for Int64 { function show(self: Self) -> Text effects { pure } { return \"x\" } }\n"
        "capability Read { function read() -> Int64 effects { service.read<Self> } }\n"
        "capability Mock { function read() -> Int64 effects { pure } }\n"
        "function render(value: Int64) -> Text effects { pure } ensures { result == result } { return value.show() }\n"
        "function handled(source: capability Read, mock: capability Mock) -> Int64 effects { pure } "
        "{ return handle service.read<source> with mock { source.read() } }\n";
    TestCompilation compilation;
    CHECK(compile_ir(&compilation, text));
    bool method = false;
    bool capability = false;
    bool handler = false;
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        const SolIrExpression *expression = &compilation.ir.expressions[index];
        method = method || (expression->kind == SOL_IR_EXPR_CALL
            && expression->as.call.kind == SOL_IR_CALL_METHOD
            && expression->as.call.callable != SOL_IR_NONE);
        capability = capability || (expression->kind == SOL_IR_EXPR_CALL
            && expression->as.call.kind == SOL_IR_CALL_CAPABILITY);
        handler = handler || (expression->kind == SOL_IR_EXPR_HANDLE
            && expression->as.handler.source != SOL_IR_NONE
            && expression->as.handler.provider_callable != SOL_IR_NONE);
    }
    CHECK(method);
    CHECK(capability);
    CHECK(handler);
    CHECK(compilation.ir.obligation_count == 1);
    free_compilation(&compilation);
}

static void test_builtin_context_and_propagation_rejection(void) {
    TestCompilation compilation;
    bool compiled = compile_ir(&compilation,
        "module context\n"
        "enum Failure { bad }\n"
        "record Values { value: Option<Int64> }\n"
        "function wrap(value: Option<Int64>) -> Option<Int64> { return value }\n"
        "function nested() -> Result<Option<Int64>, Failure> { return ok(none()) }\n"
        "function branch(flag: Bool) -> Option<Int64> { return if flag { none() } else { some(1) } }\n"
        "function field() -> Values { return Values { value = none() } }\n"
        "function argument() -> Option<Int64> { return wrap(none()) }\n");
    CHECK(compiled);
    if (!compiled) {
        sol_diagnostics_render_human(stderr, &compilation.source, &compilation.diagnostics);
    }
    free_compilation(&compilation);

    CHECK(compile_ir(&compilation,
        "module generic_builtin\n"
        "function choose<T>(first: T, second: Option<T>) -> Option<T> { return second }\n"
        "function good() -> Option<Int64> { return choose(1, none()) }\n"));
    free_compilation(&compilation);

    CHECK(!frontend(&compilation,
        "module generic_builtin_ambiguous\n"
        "function choose<T>(first: Option<T>, second: T) -> T { return second }\n"
        "function bad() -> Int64 { return choose(none(), 1) }\n"));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    free_compilation(&compilation);

    CHECK(!frontend(&compilation,
        "module ambiguous\nfunction bad() -> Int64 { let value = none() return 1 }\n"));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(has_code(&compilation, "SOL-TYPE-025"));
    free_compilation(&compilation);

    CHECK(!frontend(&compilation,
        "module generic_propagation\nrecord Box<T> { value: T }\n"
        "function bad(value: Box<Int64>) -> Box<Int64> { return value? }\n"));
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    CHECK(has_code(&compilation, "SOL-TYPE-026"));
    free_compilation(&compilation);

    CHECK(!frontend(&compilation,
        "module builtin_head\nfunction bad() -> Int64 { let value = none return 1 }\n"));
    CHECK(has_code(&compilation, "SOL-TYPE-025"));
    free_compilation(&compilation);
}

static void test_record_resolution_and_malformed_rejection(void) {
    TestCompilation compilation;
    CHECK(compile_ir(&compilation,
        "module record_fields\nrecord Pair { left: Int64, right: Int64 }\n"
        "record Other { value: Int64 }\n"
        "function make() -> Pair { return Pair { right = 2, left = 1 } }\n"));
    bool record = false;
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        const SolIrExpression *expression = &compilation.ir.expressions[index];
        if (expression->kind != SOL_IR_EXPR_RECORD) continue;
        record = true;
        CHECK(expression->as.record.fields.count == 2);
        CHECK(compilation.ir.operands[expression->as.record.fields.offset].formal == 0);
        CHECK(compilation.ir.operands[expression->as.record.fields.offset + 1].formal == 1);
    }
    CHECK(record);

    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    SolIr forged = compilation.ir;
    forged.fields = NULL;
    CHECK(!sol_ir_validate(&forged, &diagnostics));
    sol_diagnostics_free(&diagnostics);
    sol_diagnostics_init(&diagnostics);
    SolIrField *saved_fields = compilation.ir.fields;
    compilation.ir.fields[0].owner = 1;
    CHECK(!sol_ir_validate(&compilation.ir, &diagnostics));
    compilation.ir.fields[0].owner = 0;
    CHECK(saved_fields == compilation.ir.fields);
    sol_diagnostics_free(&diagnostics);

    sol_diagnostics_init(&diagnostics);
    CHECK(!sol_ir_lower(&compilation.source, &compilation.syntax,
        &compilation.hir, &compilation.types, &compilation.effects,
        &compilation.contracts, &compilation.ir, &diagnostics));
    CHECK(compilation.ir.source_bytes != NULL);
    sol_diagnostics_free(&diagnostics);

    SolType *saved_expressions = compilation.types.expressions;
    compilation.types.expressions = NULL;
    SolIr empty;
    sol_ir_init(&empty);
    sol_diagnostics_init(&diagnostics);
    CHECK(!sol_ir_lower(&compilation.source, &compilation.syntax,
        &compilation.hir, &compilation.types, &compilation.effects,
        &compilation.contracts, &empty, &diagnostics));
    CHECK(empty.source_bytes == NULL);
    compilation.types.expressions = saved_expressions;
    sol_ir_free(&empty);
    sol_diagnostics_free(&diagnostics);

    SolArgumentId first = SOL_AST_NONE;
    for (size_t expression = 0; expression < compilation.syntax.expression_count; ++expression) {
        if (compilation.syntax.expressions[expression].kind == SOL_EXPR_RECORD) {
            first = compilation.syntax.expressions[expression].as.record.first_field;
            break;
        }
    }
    CHECK(first != SOL_AST_NONE);
    if (first != SOL_AST_NONE) {
        SolFieldId saved = compilation.types.argument_field_resolutions[first];
        compilation.types.argument_field_resolutions[first] = 2;
        sol_ir_init(&empty);
        sol_diagnostics_init(&diagnostics);
        CHECK(!sol_ir_lower(&compilation.source, &compilation.syntax,
            &compilation.hir, &compilation.types, &compilation.effects,
            &compilation.contracts, &empty, &diagnostics));
        CHECK(empty.source_bytes == NULL);
        compilation.types.argument_field_resolutions[first] = saved;
        sol_ir_free(&empty);
        sol_diagnostics_free(&diagnostics);
    }
    free_compilation(&compilation);
}

static void test_package_file_lifetime(void) {
    SolPackage package;
    SolDiagnostics diagnostics;
    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    SolContractTable contracts;
    SolIr ir;
    char error[256];
    sol_package_init(&package);
    sol_diagnostics_init(&diagnostics);
    sol_hir_module_init(&hir);
    sol_type_table_init(&types);
    sol_effect_table_init(&effects);
    sol_contract_table_init(&contracts);
    sol_ir_init(&ir);
    CHECK(sol_package_load_directory(&package,
        SOL_TEST_SOURCE_DIR "/tests/packages/valid", &diagnostics, error, sizeof(error)));
    SolHirFileScope *scopes = package.file_count == 0 ? NULL
        : malloc(package.file_count * sizeof(*scopes));
    CHECK(package.file_count == 0 || scopes != NULL);
    for (size_t index = 0; index < package.file_count; ++index) {
        scopes[index] = (SolHirFileScope){
            .module_name = package.files[index].module_name,
            .import_start = package.files[index].import_start,
            .import_count = package.files[index].import_count,
            .item_start = package.files[index].item_start,
            .item_count = package.files[index].item_count,
        };
    }
    CHECK(sol_hir_lower_scoped(&package.source, &package.syntax, scopes,
        package.file_count, &hir, &diagnostics));
    CHECK(sol_type_check(&package.source, &package.syntax, &hir, &types, &diagnostics));
    CHECK(sol_effect_check(&package.source, &package.syntax, &hir,
        &types, &effects, &diagnostics));
    CHECK(sol_contract_lower(&package.source, &package.syntax, &hir,
        &types, &effects, &contracts, &diagnostics));
    CHECK(sol_ir_lower_scoped(&package.source, &package.syntax, &hir,
        &types, &effects, &contracts, package.files, package.file_count,
        &ir, &diagnostics));
    CHECK(ir.file_count == package.file_count);
    free(scopes);
    sol_contract_table_free(&contracts);
    sol_effect_table_free(&effects);
    sol_type_table_free(&types);
    sol_hir_module_free(&hir);
    sol_package_free(&package);
    CHECK(sol_ir_validate(&ir, &diagnostics));
    CHECK(ir.file_count == 7);
    for (size_t index = 0; index < ir.file_count; ++index) {
        CHECK(ir.files[index].path != NULL);
        CHECK(ir.files[index].aggregate_start <= ir.files[index].aggregate_end);
    }
    sol_ir_free(&ir);
    sol_diagnostics_free(&diagnostics);
}

static void test_forged_ir_domains(void) {
    TestCompilation compilation;
    CHECK(compile_ir(&compilation,
        "module forged_domains\nenum Choice { yes(value: Int64), no }\n"
        "function value(choice: Choice) -> Int64 { return match choice { yes(value) => { value }, no => 0 } }\n"));
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    if (compilation.ir.statement_id_count != 0) {
        SolIrStatementId saved = compilation.ir.statement_ids[0];
        compilation.ir.statement_ids[0] = compilation.ir.statement_count;
        CHECK(!sol_ir_validate(&compilation.ir, &diagnostics));
        compilation.ir.statement_ids[0] = saved;
    }
    sol_diagnostics_free(&diagnostics);
    sol_diagnostics_init(&diagnostics);
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        SolIrExpression *expression = &compilation.ir.expressions[index];
        if (expression->kind != SOL_IR_EXPR_CALL) continue;
        SolIrSlice saved = expression->as.call.type_arguments;
        expression->as.call.type_arguments = (SolIrSlice){
            .offset = compilation.ir.type_id_count,
            .count = 1,
        };
        CHECK(!sol_ir_validate(&compilation.ir, &diagnostics));
        expression->as.call.type_arguments = saved;
        break;
    }
    sol_diagnostics_free(&diagnostics);
    sol_diagnostics_init(&diagnostics);
    if (compilation.ir.arm_id_count != 0) {
        SolIrArmId saved = compilation.ir.arm_ids[0];
        compilation.ir.arm_ids[0] = compilation.ir.arm_count;
        CHECK(!sol_ir_validate(&compilation.ir, &diagnostics));
        compilation.ir.arm_ids[0] = saved;
    }
    sol_diagnostics_free(&diagnostics);
    sol_diagnostics_init(&diagnostics);
    if (compilation.ir.root_count != 0) {
        SolIrLocalId saved = compilation.ir.roots[0];
        compilation.ir.roots[0] = compilation.ir.local_count;
        CHECK(!sol_ir_validate(&compilation.ir, &diagnostics));
        compilation.ir.roots[0] = saved;
    }
    sol_diagnostics_free(&diagnostics);
    free_compilation(&compilation);
}

static bool lower_rejected(TestCompilation *compilation) {
    SolIr ir;
    SolDiagnostics diagnostics;
    sol_ir_init(&ir);
    sol_diagnostics_init(&diagnostics);
    bool rejected = !sol_ir_lower(&compilation->source, &compilation->syntax,
        &compilation->hir, &compilation->types, &compilation->effects,
        &compilation->contracts, &ir, &diagnostics);
    sol_ir_free(&ir);
    sol_diagnostics_free(&diagnostics);
    return rejected;
}

static bool validate_rejected(SolIr *ir) {
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    bool rejected = !sol_ir_validate(ir, &diagnostics);
    sol_diagnostics_free(&diagnostics);
    return rejected;
}

static void check_call_operand_rejections(SolIr *ir, SolIrExpression *call) {
    SolIrSlice saved = call->as.call.operands;
    if (saved.count != 0) {
        call->as.call.operands.count = saved.count - 1;
        CHECK(validate_rejected(ir));
        call->as.call.operands = saved;
        SolIrExpressionId value = ir->operands[saved.offset].value;
        ir->operands[saved.offset].value = ir->expression_count;
        CHECK(validate_rejected(ir));
        ir->operands[saved.offset].value = value;
        size_t formal = ir->operands[saved.offset].formal;
        ir->operands[saved.offset].formal = formal == 0 ? 1 : 0;
        CHECK(validate_rejected(ir));
        ir->operands[saved.offset].formal = formal;
    }
    size_t forged_count = ir->operand_count + saved.count + 1;
    SolIrOperand *extra = malloc(forged_count * sizeof(*extra));
    CHECK(extra != NULL);
    if (extra == NULL) return;
    memcpy(extra, ir->operands, ir->operand_count * sizeof(*extra));
    SolIr forged = *ir;
    forged.operands = extra;
    forged.operand_count = forged_count;
    forged.expressions = malloc(ir->expression_count * sizeof(*forged.expressions));
    CHECK(forged.expressions != NULL);
    if (forged.expressions != NULL) {
        memcpy(forged.expressions, ir->expressions,
            ir->expression_count * sizeof(*forged.expressions));
        size_t call_id = (size_t)(call - ir->expressions);
        forged.expressions[call_id].as.call.operands
            = (SolIrSlice){.offset = ir->operand_count, .count = saved.count + 1};
        for (size_t index = 0; index < saved.count; ++index) {
            extra[ir->operand_count + index] = ir->operands[saved.offset + index];
        }
        extra[ir->operand_count + saved.count]
            = (SolIrOperand){.formal = saved.count, .value = 0};
        CHECK(validate_rejected(&forged));
        free(forged.expressions);
    }
    free(extra);
}

static void test_exact_validation_findings(void) {
    TestCompilation compilation;
    CHECK(compile_ir(&compilation,
        "module callback_domain\n"
        "function apply(callback: function(Int64) -> Int64 effects { pure }) -> Int64 "
        "{ return callback(1) }\n"));
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        SolIrExpression *call = &compilation.ir.expressions[index];
        if (call->kind != SOL_IR_EXPR_CALL
            || call->as.call.kind != SOL_IR_CALL_CALLBACK) continue;
        SolIrExpressionId saved_callee = call->as.call.callee;
        call->as.call.callee = compilation.ir.expression_count;
        CHECK(validate_rejected(&compilation.ir));
        call->as.call.callee = saved_callee;
        SolIrTypeId saved_type = compilation.ir.expressions[saved_callee].type;
        compilation.ir.expressions[saved_callee].type = compilation.ir.type_count;
        CHECK(validate_rejected(&compilation.ir));
        compilation.ir.expressions[saved_callee].type = saved_type;
        SolIrTypeId non_function = SOL_IR_NONE;
        for (size_t type = 0; type < compilation.ir.type_count; ++type) {
            if (compilation.ir.types[type].kind == SOL_IR_TYPE_INT64) {
                non_function = type;
                break;
            }
        }
        CHECK(non_function != SOL_IR_NONE);
        if (non_function != SOL_IR_NONE) {
            compilation.ir.expressions[saved_callee].type = non_function;
            CHECK(validate_rejected(&compilation.ir));
            compilation.ir.expressions[saved_callee].type = saved_type;
        }
        break;
    }
    free_compilation(&compilation);

    CHECK(compile_ir(&compilation,
        "module record_domain\n"
        "record Pair { left: Int64, right: Int64 }\n"
        "record Other { value: Int64 }\n"
        "function make() -> Pair { return Pair { left = 1, right = 2 } }\n"));
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        SolIrExpression *record = &compilation.ir.expressions[index];
        if (record->kind != SOL_IR_EXPR_RECORD
            || compilation.ir.definitions[record->as.record.definition].kind
                != SOL_IR_DEFINITION_RECORD) continue;
        SolIrSlice fields = record->as.record.fields;
        CHECK(fields.count == 2);
        SolIrOperand *first = &compilation.ir.operands[fields.offset];
        SolIrOperand *second = &compilation.ir.operands[fields.offset + 1];
        SolIrExpressionId saved_value = first->value;
        first->value = compilation.ir.expression_count;
        CHECK(validate_rejected(&compilation.ir));
        first->value = saved_value;
        size_t saved_formal = first->formal;
        first->formal = compilation.ir.field_count;
        CHECK(validate_rejected(&compilation.ir));
        first->formal = 2;
        CHECK(validate_rejected(&compilation.ir));
        first->formal = second->formal;
        CHECK(validate_rejected(&compilation.ir));
        first->formal = saved_formal;
        size_t saved_second = second->formal;
        second->formal = first->formal;
        first->formal = saved_second;
        CHECK(validate_rejected(&compilation.ir));
        first->formal = saved_formal;
        second->formal = saved_second;
        record->as.record.fields.count = 1;
        CHECK(validate_rejected(&compilation.ir));
        record->as.record.fields = fields;
        break;
    }
    free_compilation(&compilation);

    CHECK(compile_ir(&compilation,
        "module parameter_domains\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "implementation Show for Int64 { function show(self: Self) -> Text effects { pure } "
        "{ return \"ok\" } }\n"
        "function generic<T: Show>(value: T) -> T { return value }\n"
        "function effectful<effects E>(callback: function() -> Int64 effects E) -> Int64 "
        "effects { E } { return callback() }\n"
        "function untouched<effects F>(callback: function() -> Int64 effects F) -> Int64 "
        "effects { F } { return callback() }\n"));
    bool retained_tail = false;
    bool callback_tail = false;
    SolIrEffectParameterId foreign_tail = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.effect_parameter_count; ++index) {
        if (strcmp(compilation.ir.effect_parameters[index].name, "F") == 0) {
            foreign_tail = index;
        }
    }
    for (size_t index = 0; index < compilation.ir.callable_count; ++index) {
        SolIrCallable *callable = &compilation.ir.callables[index];
        if (strcmp(callable->name, "effectful") == 0) {
            retained_tail = callable->effect_parameter < compilation.ir.effect_parameter_count
                && strcmp(compilation.ir.effect_parameters[callable->effect_parameter].name,
                    "E") == 0;
        }
    }
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        SolIrExpression *expression = &compilation.ir.expressions[index];
        if (expression->kind == SOL_IR_EXPR_CALL
            && expression->as.call.kind == SOL_IR_CALL_CALLBACK) {
            callback_tail = expression->as.call.effect_parameter
                < compilation.ir.effect_parameter_count;
            SolIrEffectParameterId saved = expression->as.call.effect_parameter;
            expression->as.call.effect_parameter = compilation.ir.effect_parameter_count;
            CHECK(validate_rejected(&compilation.ir));
            if (foreign_tail != SOL_IR_NONE && saved != foreign_tail) {
                expression->as.call.effect_parameter = foreign_tail;
                CHECK(validate_rejected(&compilation.ir));
            }
            expression->as.call.effect_parameter = saved;
        }
    }
    CHECK(retained_tail);
    CHECK(callback_tail);
    for (size_t index = 0; index < compilation.ir.type_count; ++index) {
        if (compilation.ir.types[index].effect_parameter == SOL_IR_NONE) continue;
        SolIrEffectParameterId saved = compilation.ir.types[index].effect_parameter;
        compilation.ir.types[index].effect_parameter = compilation.ir.effect_parameter_count;
        CHECK(validate_rejected(&compilation.ir));
        compilation.ir.types[index].effect_parameter = saved;
        break;
    }
    for (size_t index = 0; index < compilation.ir.callable_count; ++index) {
        if (compilation.ir.callables[index].effect_parameter == SOL_IR_NONE) continue;
        SolIrEffectParameterId saved = compilation.ir.callables[index].effect_parameter;
        compilation.ir.callables[index].effect_parameter = compilation.ir.effect_parameter_count;
        CHECK(validate_rejected(&compilation.ir));
        compilation.ir.callables[index].effect_parameter = saved;
        break;
    }
    SolIr forged = compilation.ir;
    forged.generic_parameters = NULL;
    CHECK(validate_rejected(&forged));
    forged = compilation.ir;
    forged.effect_parameters = NULL;
    CHECK(validate_rejected(&forged));
    if (compilation.ir.generic_parameter_count != 0) {
        SolIrGenericParameter *parameter = &compilation.ir.generic_parameters[0];
        size_t saved_owner = parameter->owner;
        parameter->owner = 0;
        CHECK(validate_rejected(&compilation.ir));
        parameter->owner = saved_owner;
        size_t saved_ordinal = parameter->ordinal;
        parameter->ordinal = 1;
        CHECK(validate_rejected(&compilation.ir));
        parameter->ordinal = saved_ordinal;
    }
    if (compilation.ir.effect_parameter_count != 0) {
        SolIrEffectParameter *parameter = &compilation.ir.effect_parameters[0];
        size_t saved_owner = parameter->owner;
        parameter->owner = 0;
        CHECK(validate_rejected(&compilation.ir));
        parameter->owner = saved_owner;
        size_t saved_ordinal = parameter->ordinal;
        parameter->ordinal = 1;
        CHECK(validate_rejected(&compilation.ir));
        parameter->ordinal = saved_ordinal;
    }
    for (size_t index = 0; index < compilation.ir.callable_count; ++index) {
        SolIrCallable *callable = &compilation.ir.callables[index];
        SolIrDefinition *owner = &compilation.ir.definitions[callable->owner];
        if (owner->generic_parameters.count != 0) {
            SolIrSlice saved = callable->generic_parameters;
            callable->generic_parameters = (SolIrSlice){0};
            CHECK(validate_rejected(&compilation.ir));
            callable->generic_parameters = saved;
            break;
        }
    }
    for (size_t index = 0; index < compilation.ir.callable_count; ++index) {
        SolIrCallable *callable = &compilation.ir.callables[index];
        SolIrDefinition *owner = &compilation.ir.definitions[callable->owner];
        if (owner->effect_parameters.count != 0) {
            SolIrSlice saved = callable->effect_parameters;
            callable->effect_parameters = (SolIrSlice){0};
            CHECK(validate_rejected(&compilation.ir));
            callable->effect_parameters = saved;
            break;
        }
    }
    free_compilation(&compilation);

    CHECK(frontend(&compilation,
        "module frontend_domains\n"
        "type Meter = distinct Int64\n"
        "function make() -> Meter { return Meter(1) }\n"));
    size_t saved_count = compilation.types.declared_type_count;
    compilation.types.declared_type_count = saved_count + 1;
    CHECK(lower_rejected(&compilation));
    compilation.types.declared_type_count = saved_count;
    SolType *saved_declared = compilation.types.declared_types;
    compilation.types.declared_types = NULL;
    CHECK(lower_rejected(&compilation));
    compilation.types.declared_types = saved_declared;
    saved_count = compilation.types.representation_count;
    compilation.types.representation_count = saved_count - 1;
    CHECK(lower_rejected(&compilation));
    compilation.types.representation_count = saved_count;
    SolTypeRepresentation *saved_representations = compilation.types.representations;
    compilation.types.representations = NULL;
    CHECK(lower_rejected(&compilation));
    compilation.types.representations = saved_representations;
    saved_count = compilation.types.implementation_target_count;
    compilation.types.implementation_target_count = saved_count - 1;
    CHECK(lower_rejected(&compilation));
    compilation.types.implementation_target_count = saved_count;
    SolType *saved_targets = compilation.types.implementation_targets;
    compilation.types.implementation_targets = NULL;
    CHECK(lower_rejected(&compilation));
    compilation.types.implementation_targets = saved_targets;
    free_compilation(&compilation);

    CHECK(frontend(&compilation,
        "module field_cycles\n"
        "record Pair { left: Int64, right: Int64 }\n"
        "enum Choice { yes(value: Int64), no }\n"
        "function make() -> Pair { return Pair { left = 1, right = 2 } }\n"));
    SolFieldId saved_next = compilation.syntax.fields[0].next;
    compilation.syntax.fields[0].next = 0;
    CHECK(lower_rejected(&compilation));
    compilation.syntax.fields[0].next = saved_next;
    SolFieldId variant_field = compilation.syntax.variants[0].first_field;
    CHECK(variant_field != SOL_AST_NONE);
    if (variant_field != SOL_AST_NONE) {
        saved_next = compilation.syntax.fields[variant_field].next;
        compilation.syntax.fields[variant_field].next = variant_field;
        CHECK(lower_rejected(&compilation));
        compilation.syntax.fields[variant_field].next = saved_next;
    }
    free_compilation(&compilation);

    CHECK(frontend(&compilation,
        "module named_span\n"
        "enum Choice { yes(value: Int64), no }\n"
        "trait Show { function show(self: Self, prefix: Text) -> Text effects { pure } }\n"
        "implementation Show for Int64 { function show(self: Self, renamed: Text) -> Text "
        "effects { pure } { return renamed } }\n"
        "function render<T: Show>(value: T) -> Text effects { pure } "
        "{ return value.show(prefix = \"x\") }\n"
        "function concrete(value: Int64) -> Text effects { pure } { return render(value) }\n"
        "function empty() -> Choice { return Choice.no }\n"));
    SolArgumentId named = SOL_AST_NONE;
    for (size_t index = 0; index < compilation.syntax.argument_count; ++index) {
        if (compilation.syntax.arguments[index].is_named) {
            named = index;
            break;
        }
    }
    CHECK(named != SOL_AST_NONE);
    if (named != SOL_AST_NONE) {
        SolSpan saved = compilation.syntax.arguments[named].name;
        compilation.syntax.arguments[named].name = (SolSpan){2, 1};
        CHECK(lower_rejected(&compilation));
        compilation.syntax.arguments[named].name = saved;
    }
    free_compilation(&compilation);
}

static void test_release_gate_domains(void) {
    TestCompilation compilation;
    CHECK(frontend(&compilation,
        "module hir_domains\n"
        "trait Show { function show(self: Self) -> Text effects { pure } }\n"
        "implementation Show for Int64 { function show(self: Self) -> Text effects { pure } "
        "{ return \"ok\" } }\n"
        "function value<T: Show>(item: T) -> T { return item }\n"));
    size_t saved_count = compilation.hir.bound_resolution_count;
    compilation.hir.bound_resolution_count = saved_count - 1;
    CHECK(lower_rejected(&compilation));
    compilation.hir.bound_resolution_count = saved_count;
    SolResolution *saved_resolutions = compilation.hir.bound_resolutions;
    compilation.hir.bound_resolutions = NULL;
    CHECK(lower_rejected(&compilation));
    compilation.hir.bound_resolutions = saved_resolutions;
    saved_count = compilation.hir.trait_resolution_count;
    compilation.hir.trait_resolution_count = saved_count - 1;
    CHECK(lower_rejected(&compilation));
    compilation.hir.trait_resolution_count = saved_count;
    saved_resolutions = compilation.hir.trait_resolutions;
    compilation.hir.trait_resolutions = NULL;
    CHECK(lower_rejected(&compilation));
    compilation.hir.trait_resolutions = saved_resolutions;
    free_compilation(&compilation);

    CHECK(compile_ir(&compilation,
        "module call_domains\n"
        "enum Choice { yes(value: Int64), no }\n"
        "type Meter = distinct Int64\n"
        "capability Root { function read(value: Int64) -> Int64 effects { pure } }\n"
        "capability Wrapper derives_from source: capability Root {}\n"
        "trait Show { function show(self: Self, prefix: Text) -> Text effects { pure } }\n"
        "implementation Show for Int64 { function show(self: Self, renamed: Text) -> Text "
        "effects { pure } { return renamed } }\n"
        "function direct(value: Int64) -> Int64 { return value }\n"
        "function direct_call() -> Int64 { return direct(1) }\n"
        "function callback_call(callback: function(Int64) -> Int64 effects { pure }) -> Int64 "
        "{ return callback(1) }\n"
        "function capability_call(root: capability Root) -> Int64 effects { pure } "
        "{ return root.read(1) }\n"
        "function method_call<T: Show>(value: T) -> Text effects { pure } "
        "{ return value.show(prefix = \"x\") }\n"
        "function make_ok() -> Result<Int64, Text> { return ok(1) }\n"
        "function make_err() -> Result<Int64, Text> { return err(\"x\") }\n"
        "function make_some() -> Option<Int64> { return some(1) }\n"
        "function make_none() -> Option<Int64> { return none() }\n"
        "function make_meter() -> Meter { return Meter(1) }\n"
        "function make_choice() -> Choice { return Choice.yes(value = 1) }\n"
        "function make_wrapper(root: capability Root) -> capability Wrapper "
        "{ return Wrapper { source = root } }\n"));
    bool seen[SOL_IR_CALL_DISTINCT_CONSTRUCTOR + 1] = {false};
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        SolIrExpression *expression = &compilation.ir.expressions[index];
        if (expression->kind == SOL_IR_EXPR_CALL) {
            seen[expression->as.call.kind] = true;
            check_call_operand_rejections(&compilation.ir, expression);
        } else if (expression->kind == SOL_IR_EXPR_RECORD
            && compilation.ir.definitions[expression->as.record.definition].kind
                == SOL_IR_DEFINITION_CAPABILITY) {
            SolIrSlice saved = expression->as.record.fields;
            CHECK(saved.count == 1);
            expression->as.record.fields.count = 0;
            CHECK(validate_rejected(&compilation.ir));
            expression->as.record.fields = saved;
            SolIrExpressionId value = compilation.ir.operands[saved.offset].value;
            compilation.ir.operands[saved.offset].value = compilation.ir.expression_count;
            CHECK(validate_rejected(&compilation.ir));
            compilation.ir.operands[saved.offset].value = value;
            size_t formal = compilation.ir.operands[saved.offset].formal;
            compilation.ir.operands[saved.offset].formal = 1;
            CHECK(validate_rejected(&compilation.ir));
            compilation.ir.operands[saved.offset].formal = formal;
            expression->as.record.fields.count = 2;
            CHECK(validate_rejected(&compilation.ir));
            expression->as.record.fields = saved;
        }
    }
    for (size_t kind = 0; kind <= SOL_IR_CALL_DISTINCT_CONSTRUCTOR; ++kind) {
        CHECK(seen[kind]);
    }
    free_compilation(&compilation);
}

static void test_exact_member_table_ownership(void) {
    TestCompilation compilation;
    CHECK(compile_ir(&compilation,
        "module member_ownership\n"
        "capability PairCap {\n"
        "    function first() -> Bool effects { pure }\n"
        "    function second() -> Bool effects { pure }\n"
        "}\n"
        "trait Pair {\n"
        "    function first(self: Self) -> Bool effects { pure }\n"
        "    function second(self: Self) -> Bool effects { pure }\n"
        "}\n"
        "implementation Pair for Bool {\n"
        "    function first(self: Self) -> Bool effects { pure } { return self }\n"
        "    function second(self: Self) -> Bool effects { pure } { return self }\n"
        "}\n"));
    SolIrDefinitionId owners[3] = {SOL_IR_NONE, SOL_IR_NONE, SOL_IR_NONE};
    for (size_t definition = 0; definition < compilation.ir.definition_count;
        ++definition) {
        SolIrDefinitionKind kind = compilation.ir.definitions[definition].kind;
        if (kind == SOL_IR_DEFINITION_CAPABILITY) owners[0] = definition;
        else if (kind == SOL_IR_DEFINITION_TRAIT) owners[1] = definition;
        else if (kind == SOL_IR_DEFINITION_IMPLEMENTATION) owners[2] = definition;
    }
    for (size_t owner_index = 0; owner_index < 3; ++owner_index) {
        CHECK(owners[owner_index] != SOL_IR_NONE);
        if (owners[owner_index] == SOL_IR_NONE) continue;
        SolIrDefinition *owner = &compilation.ir.definitions[owners[owner_index]];
        CHECK(owner->members.count == 2);
        if (owner->members.count != 2) continue;
        SolIrSlice saved = owner->members;
        owner->members = (SolIrSlice){0};
        CHECK(validate_rejected(&compilation.ir));
        owner->members = saved;

        SolIrCallableId second
            = compilation.ir.members[saved.offset + 1].callable;
        compilation.ir.members[saved.offset + 1].callable
            = compilation.ir.members[saved.offset].callable;
        CHECK(validate_rejected(&compilation.ir));
        compilation.ir.members[saved.offset + 1].callable = second;

        SolIrDefinitionId foreign_owner = owners[(owner_index + 1) % 3];
        CHECK(foreign_owner != SOL_IR_NONE);
        if (foreign_owner != SOL_IR_NONE) {
            SolIrCallableId first = compilation.ir.members[saved.offset].callable;
            SolIrSlice foreign = compilation.ir.definitions[foreign_owner].members;
            compilation.ir.members[saved.offset].callable
                = compilation.ir.members[foreign.offset].callable;
            CHECK(validate_rejected(&compilation.ir));
            compilation.ir.members[saved.offset].callable = first;
        }
    }
    free_compilation(&compilation);
}

static void test_affine_ownership(void) {
    TestCompilation compilation;
    CHECK(compile_ir(&compilation,
        "module ownership_copy\n"
        "record Pair { left: Int64, right: Text }\n"
        "enum Payload { pair(left: Int64, right: Int64) }\n"
        "capability Clock { function read() -> Int64 effects { pure } }\n"
        "function copies(value: Pair) -> Text { let first = value return value.right }\n"
        "function two(first: Int64, second: Int64) -> Int64 { return first + second }\n"
        "function unpack(value: Payload) -> Int64 "
        "{ return match value { pair(left, right) => left + right } }\n"
        "function repeated(clock: capability Clock) -> Int64 effects { pure } "
        "{ let first = clock.read() return clock.read() + first }\n"
        "function scope(clock: capability Clock) -> capability Clock "
        "{ let outer = { let inner = clock inner } return outer }\n"));
    bool saw_copy = false;
    bool saw_receiver = false;
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        const SolIrExpression *expression = &compilation.ir.expressions[index];
        saw_copy = saw_copy || expression->local_use == SOL_IR_LOCAL_USE_COPY;
        saw_receiver = saw_receiver
            || expression->local_use == SOL_IR_LOCAL_USE_SHARED
            || expression->local_use == SOL_IR_LOCAL_USE_EXCLUSIVE;
    }
    CHECK(saw_copy);
    CHECK(saw_receiver);

    for (size_t index = 0; index < compilation.ir.callable_count; ++index) {
        SolIrCallable *callable = &compilation.ir.callables[index];
        if (strcmp(callable->name, "two") != 0) continue;
        CHECK(callable->parameters.count == 2);
        SolIrLocalId saved = compilation.ir.roots[callable->parameters.offset + 1];
        compilation.ir.roots[callable->parameters.offset + 1]
            = compilation.ir.roots[callable->parameters.offset];
        CHECK(validate_rejected(&compilation.ir));
        compilation.ir.roots[callable->parameters.offset + 1] = saved;
    }
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        SolIrExpression *expression = &compilation.ir.expressions[index];
        if (expression->kind != SOL_IR_EXPR_BINARY) continue;
        SolIrExpressionId saved = expression->as.binary.right;
        expression->as.binary.right = expression->as.binary.left;
        CHECK(validate_rejected(&compilation.ir));
        expression->as.binary.right = saved;
        break;
    }
    for (size_t index = 0; index < compilation.ir.arm_count; ++index) {
        SolIrArm *arm = &compilation.ir.arms[index];
        if (arm->bindings.count != 2) continue;
        SolIrLocalId saved = compilation.ir.roots[arm->bindings.offset + 1];
        compilation.ir.roots[arm->bindings.offset + 1]
            = compilation.ir.roots[arm->bindings.offset];
        CHECK(validate_rejected(&compilation.ir));
        compilation.ir.roots[arm->bindings.offset + 1] = saved;
        break;
    }
    SolIrLocalId inner = SOL_IR_NONE;
    SolIrLocalId outer = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.local_count; ++index) {
        if (strcmp(compilation.ir.locals[index].name, "inner") == 0) inner = index;
        if (strcmp(compilation.ir.locals[index].name, "outer") == 0) outer = index;
    }
    CHECK(inner != SOL_IR_NONE && outer != SOL_IR_NONE);
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        SolIrExpression *expression = &compilation.ir.expressions[index];
        if (expression->kind != SOL_IR_EXPR_LOCAL || expression->as.local != outer) continue;
        expression->as.local = inner;
        CHECK(validate_rejected(&compilation.ir));
        expression->as.local = outer;
        break;
    }
    for (size_t index = 0; index < compilation.ir.statement_count; ++index) {
        SolIrStatement *statement = &compilation.ir.statements[index];
        if (statement->kind != SOL_IR_STATEMENT_LET || statement->local != outer) continue;
        statement->local = inner;
        CHECK(validate_rejected(&compilation.ir));
        statement->local = outer;
        break;
    }
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        SolIrExpression *expression = &compilation.ir.expressions[index];
        if (expression->local_use == SOL_IR_LOCAL_USE_SHARED
            || expression->local_use == SOL_IR_LOCAL_USE_EXCLUSIVE) {
            SolIrLocalUse saved = expression->local_use;
            expression->local_use = SOL_IR_LOCAL_USE_MOVE;
            CHECK(validate_rejected(&compilation.ir));
            expression->local_use = (SolIrLocalUse)(SOL_IR_LOCAL_USE_EXCLUSIVE + 1);
            CHECK(validate_rejected(&compilation.ir));
            expression->local_use = saved;
            break;
        }
    }
    free_compilation(&compilation);

    CHECK(compile_ir(&compilation,
        "module borrow_valid\n"
        "capability Token { function read() -> Int64 effects { pure } }\n"
        "function inspect(value: borrow capability Token) -> Int64 effects { pure } "
        "{ return value.read() }\n"
        "function apply(callback: function(borrow capability Token) -> Int64 "
        "effects { pure }, value: borrow capability Token) -> Int64 effects { pure } "
        "{ return callback(value) }\n"
        "function entry(value: capability Token) -> Int64 effects { pure } "
        "{ return apply(inspect, value) }\n"));
    bool saw_shared_parameter = false;
    bool saw_shared_operand = false;
    for (size_t index = 0; index < compilation.ir.local_count; ++index) {
        saw_shared_parameter = saw_shared_parameter
            || compilation.ir.locals[index].access == SOL_ACCESS_SHARED;
    }
    for (size_t index = 0; index < compilation.ir.operand_count; ++index) {
        saw_shared_operand = saw_shared_operand
            || compilation.ir.operands[index].access == SOL_ACCESS_SHARED;
    }
    CHECK(saw_shared_parameter && saw_shared_operand);
    for (size_t index = 0; index < compilation.ir.operand_count; ++index) {
        if (compilation.ir.operands[index].access != SOL_ACCESS_SHARED) continue;
        SolAccessMode saved = compilation.ir.operands[index].access;
        compilation.ir.operands[index].access = SOL_ACCESS_OWNED;
        CHECK(validate_rejected(&compilation.ir));
        compilation.ir.operands[index].access = saved;
        break;
    }
    for (size_t index = 0; index < compilation.ir.type_count; ++index) {
        SolIrType *type = &compilation.ir.types[index];
        if (type->kind != SOL_IR_TYPE_FUNCTION || type->parameter_count == 0) continue;
        size_t access = type->parameter_access_offset;
        SolAccessMode saved = compilation.ir.accesses[access];
        compilation.ir.accesses[access]
            = (SolAccessMode)(SOL_ACCESS_EXCLUSIVE + 1);
        CHECK(validate_rejected(&compilation.ir));
        compilation.ir.accesses[access] = saved;
        break;
    }
    free_compilation(&compilation);

    const struct { const char *source; const char *code; } borrow_invalid[] = {
        {
            "module borrow_conflict\n"
            "capability Token { function read() -> Int64 effects { pure } }\n"
            "function clash(left: inout capability Token, "
            "right: borrow capability Token) -> Int64 { return 0 }\n"
            "function bad(value: capability Token) -> Int64 "
            "{ return clash(value, value) }\n",
            "SOL-OWNERSHIP-002"
        },
        {
            "module borrow_move\n"
            "capability Token { function read() -> Int64 effects { pure } }\n"
            "function clash(left: borrow capability Token, "
            "right: capability Token) -> Int64 { return 0 }\n"
            "function bad(value: capability Token) -> Int64 "
            "{ return clash(value, value) }\n",
            "SOL-OWNERSHIP-003"
        },
        {
            "module exclusive_copy\n"
            "function clash(left: inout Text, right: Text) -> Int64 { return 0 }\n"
            "function bad(value: Text) -> Int64 { return clash(value, value) }\n",
            "SOL-OWNERSHIP-003"
        },
        {
            "module borrow_temporary\n"
            "function inspect(value: borrow Text) -> Int64 { return 0 }\n"
            "function bad(flag: Bool, left: Text, right: Text) -> Int64 "
            "{ return inspect(if flag { left } else { right }) }\n",
            "SOL-OWNERSHIP-004"
        },
        {
            "module unreachable_borrow_temporary\n"
            "function inspect(value: borrow Text) -> Int64 { return 0 }\n"
            "function bad(flag: Bool, left: Text, right: Text) -> Int64 "
            "{ return 0 inspect(if flag { left } else { right }) }\n",
            "SOL-OWNERSHIP-004"
        },
        {
            "module borrow_escape\n"
            "capability Token { function read() -> Int64 effects { pure } }\n"
            "function bad(value: borrow capability Token) -> capability Token "
            "{ return value }\n",
            "SOL-OWNERSHIP-004"
        },
        {
            "module invalid_reborrow\n"
            "capability Token { function read() -> Int64 effects { pure } }\n"
            "function exclusive(value: inout capability Token) -> Int64 { return 0 }\n"
            "function bad(value: borrow capability Token) -> Int64 "
            "{ return exclusive(value) }\n",
            "SOL-OWNERSHIP-004"
        },
    };
    for (size_t index = 0; index < sizeof(borrow_invalid) / sizeof(borrow_invalid[0]);
        ++index) {
        CHECK(frontend(&compilation, borrow_invalid[index].source));
        CHECK(!sol_ir_lower(&compilation.source, &compilation.syntax,
            &compilation.hir, &compilation.types, &compilation.effects,
            &compilation.contracts, &compilation.ir, &compilation.diagnostics));
        CHECK(has_code(&compilation, borrow_invalid[index].code));
        free_compilation(&compilation);
    }

    const char *invalid[] = {
        "module moved_alias\n"
        "capability Clock { function read() -> Int64 effects { pure } }\n"
        "function bad(clock: capability Clock) -> Int64 effects { pure } "
        "{ let alias = clock return clock.read() }\n",
        "module moved_operation\n"
        "capability Clock { function read() -> Int64 effects { pure } }\n"
        "function bad(clock: capability Clock) -> Int64 effects { pure } "
        "{ let operation = clock.read let first = operation() return operation() + first }\n",
        "module branch_join\n"
        "capability Clock { function read() -> Int64 effects { pure } }\n"
        "function consume(value: capability Clock) -> Int64 { return 0 }\n"
        "function bad(flag: Bool, clock: capability Clock) -> Int64 effects { pure } "
        "{ let choice = if flag { consume(clock) } else { 0 } return clock.read() }\n",
        "module match_join\n"
        "capability Clock { function read() -> Int64 effects { pure } }\n"
        "function consume(value: capability Clock) -> Int64 { return 0 }\n"
        "function bad(flag: Bool, clock: capability Clock) -> Int64 effects { pure } "
        "{ let choice = match flag { true => consume(clock) false => 0 } "
        "return clock.read() }\n",
        "module short_join\n"
        "capability Clock { function read() -> Int64 effects { pure } }\n"
        "function consume(value: capability Clock) -> Bool { return false }\n"
        "function bad(flag: Bool, clock: capability Clock) -> Int64 effects { pure } "
        "{ let result = flag && consume(clock) return clock.read() }\n",
        "module aggregate_move\n"
        "capability Clock { function read() -> Int64 effects { pure } }\n"
        "record Holder { clock: capability Clock }\n"
        "function bad(value: Holder) -> Holder "
        "{ let alias = value return value }\n",
        "module generic_move\n"
        "record Box<T> { value: T }\n"
        "function bad<T>(value: Box<T>) -> Box<T> "
        "{ let alias = value return value }\n",
    };
    for (size_t index = 0; index < sizeof(invalid) / sizeof(invalid[0]); ++index) {
        CHECK(frontend(&compilation, invalid[index]));
        CHECK(!sol_ir_lower(&compilation.source, &compilation.syntax,
            &compilation.hir, &compilation.types, &compilation.effects,
            &compilation.contracts, &compilation.ir, &compilation.diagnostics));
        CHECK(has_code(&compilation, "SOL-OWNERSHIP-001"));
        CHECK(compilation.ir.expression_count == 0);
        free_compilation(&compilation);
    }

    CHECK(compile_ir(&compilation,
        "module terminating_join\n"
        "capability Clock { function read() -> Int64 effects { pure } }\n"
        "function keep(flag: Bool, clock: capability Clock) -> capability Clock "
        "{ if flag { return clock } else { () } return clock }\n"));
    free_compilation(&compilation);
}

static void test_region_cleanup_metadata(void) {
    TestCompilation compilation;
    CHECK(compile_ir(&compilation,
        "module region_ir\n"
        "enum Payload { item(value: Text) }\n"
        "function first(input: Text, payload: Payload) -> () { "
        "region outer { let a = input region inner { let b = input } } "
        "match payload { item(text) => () } }\n"
        "function second() -> () { let foreign = \"x\" }\n"));
    SolIrExpression *block = NULL;
    SolIrStatement *region = NULL;
    SolIrArm *arm = NULL;
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        SolIrExpression *candidate = &compilation.ir.expressions[index];
        if (candidate->kind == SOL_IR_EXPR_BLOCK
            && candidate->as.block.cleanup.count == 1 && block == NULL) block = candidate;
    }
    for (size_t index = 0; index < compilation.ir.statement_count; ++index) {
        if (compilation.ir.statements[index].kind == SOL_IR_STATEMENT_REGION) {
            region = &compilation.ir.statements[index];
            break;
        }
    }
    for (size_t index = 0; index < compilation.ir.arm_count; ++index) {
        if (compilation.ir.arms[index].cleanup.count != 0) {
            arm = &compilation.ir.arms[index];
            break;
        }
    }
    CHECK(block != NULL && region != NULL && arm != NULL);
    if (block != NULL) {
        SolIrLocalId saved = compilation.ir.cleanup_locals[
            block->as.block.cleanup.offset];
        compilation.ir.cleanup_locals[block->as.block.cleanup.offset] = SOL_IR_NONE;
        CHECK(!sol_ir_validate(&compilation.ir, NULL));
        compilation.ir.cleanup_locals[block->as.block.cleanup.offset] = saved;
        SolIrSlice saved_slice = block->as.block.cleanup;
        block->as.block.cleanup.count = 0;
        CHECK(!sol_ir_validate(&compilation.ir, NULL));
        block->as.block.cleanup = saved_slice;
    }
    if (arm != NULL) {
        SolIrLocalId saved = compilation.ir.cleanup_locals[arm->cleanup.offset];
        compilation.ir.cleanup_locals[arm->cleanup.offset] = SOL_IR_NONE;
        CHECK(!sol_ir_validate(&compilation.ir, NULL));
        compilation.ir.cleanup_locals[arm->cleanup.offset] = saved;
    }
    if (region != NULL) {
        char *saved_label = region->region_label;
        region->region_label = NULL;
        CHECK(!sol_ir_validate(&compilation.ir, NULL));
        region->region_label = saved_label;
        SolIrExpressionId saved_body = region->expression;
        region->expression = 0;
        CHECK(!sol_ir_validate(&compilation.ir, NULL));
        region->expression = saved_body;
    }
    CHECK(sol_ir_validate(&compilation.ir, NULL));
    free_compilation(&compilation);

    CHECK(compile_ir(&compilation,
        "module region_result_propagation\n"
        "capability Token {}\n"
        "function pass(value: Result<capability Token, Text>) "
        "-> Result<Int64, Text> { region temporary { let token = value? } "
        "return ok(1) }\n"));
    free_compilation(&compilation);
}

static void test_mutable_assignment_ir_and_ownership(void) {
    TestCompilation compilation;
    CHECK(compile_ir(&compilation,
        "module mutable_ir\n"
        "capability Token {}\n"
        "function update(flag: Bool) -> Int64 { var value = 1 "
        "if flag { let moved = value } else { () } value = 2 return value }\n"
        "function self_update(value: capability Token) -> Int64 { var slot = value "
        "slot = slot return 0 }\n"
        "function declaration_order() -> Int64 { var first = 1 first = 2 "
        "var later = 3 return later }\n"));
    SolIrStatement *assignment = NULL;
    SolIrStatement *later_let = NULL;
    SolIrStatement *later_return = NULL;
    SolIrExpression *block = NULL;
    SolIrExpression *order_block = NULL;
    for (size_t index = 0; index < compilation.ir.statement_count; ++index) {
        SolIrStatement *statement = &compilation.ir.statements[index];
        if (statement->kind == SOL_IR_STATEMENT_ASSIGNMENT
            && strcmp(compilation.ir.locals[statement->local].name, "first") == 0) {
            assignment = statement;
        } else if (statement->kind == SOL_IR_STATEMENT_LET
            && strcmp(compilation.ir.locals[statement->local].name, "later") == 0) {
            later_let = statement;
        } else if (statement->kind == SOL_IR_STATEMENT_RETURN
            && compilation.ir.expressions[statement->expression].kind == SOL_IR_EXPR_LOCAL
            && strcmp(compilation.ir.locals[
                compilation.ir.expressions[statement->expression].as.local].name,
                "later") == 0) {
            later_return = statement;
        }
    }
    for (size_t index = 0; index < compilation.ir.expression_count; ++index) {
        if (compilation.ir.expressions[index].kind == SOL_IR_EXPR_BLOCK
            && compilation.ir.expressions[index].as.block.cleanup.count == 1) {
            block = &compilation.ir.expressions[index];
        }
    }
    for (size_t index = 0; assignment != NULL && index < compilation.ir.expression_count;
        ++index) {
        SolIrExpression *candidate = &compilation.ir.expressions[index];
        if (candidate->kind != SOL_IR_EXPR_BLOCK) continue;
        for (size_t statement = 0; statement < candidate->as.block.statements.count;
            ++statement) {
            SolIrStatementId id = compilation.ir.statement_ids[
                candidate->as.block.statements.offset + statement];
            if (&compilation.ir.statements[id] == assignment) order_block = candidate;
        }
    }
    CHECK(assignment != NULL && block != NULL);
    if (assignment != NULL) {
        CHECK(compilation.ir.locals[assignment->local].mutable);
        CHECK(compilation.ir.expressions[assignment->target].local_use
            == SOL_IR_LOCAL_USE_UPDATE);
        SolIrExpressionId saved = assignment->target;
        assignment->target = assignment->expression;
        CHECK(!sol_ir_validate(&compilation.ir, NULL));
        assignment->target = saved;
        bool saved_mutable = compilation.ir.locals[assignment->local].mutable;
        compilation.ir.locals[assignment->local].mutable = false;
        CHECK(!sol_ir_validate(&compilation.ir, NULL));
        compilation.ir.locals[assignment->local].mutable = saved_mutable;
    }
    CHECK(assignment != NULL && later_let != NULL && later_return != NULL
        && order_block != NULL);
    if (assignment != NULL && later_let != NULL && later_return != NULL
        && order_block != NULL) {
        SolIrExpressionId saved_return = later_return->expression;
        later_return->expression = assignment->target;
        CHECK(!sol_ir_validate(&compilation.ir, NULL));
        later_return->expression = saved_return;

        SolIrLocalId saved_local = assignment->local;
        SolIrExpressionId saved_target = assignment->target;
        assignment->local = later_let->local;
        assignment->target = saved_return;
        later_return->expression = saved_target;
        CHECK(!sol_ir_validate(&compilation.ir, NULL));

        size_t assignment_slot = SIZE_MAX;
        size_t let_slot = SIZE_MAX;
        size_t return_slot = SIZE_MAX;
        for (size_t index = 0; index < order_block->as.block.statements.count; ++index) {
            size_t slot = order_block->as.block.statements.offset + index;
            SolIrStatement *statement
                = &compilation.ir.statements[compilation.ir.statement_ids[slot]];
            if (statement == assignment) assignment_slot = slot;
            else if (statement == later_let) let_slot = slot;
            else if (statement == later_return) return_slot = slot;
        }
        CHECK(assignment_slot != SIZE_MAX && let_slot != SIZE_MAX
            && return_slot != SIZE_MAX);
        if (assignment_slot != SIZE_MAX && let_slot != SIZE_MAX
            && return_slot != SIZE_MAX) {
            SolIrStatementId assignment_id = compilation.ir.statement_ids[assignment_slot];
            SolIrStatementId let_id = compilation.ir.statement_ids[let_slot];
            SolIrStatementId return_id = compilation.ir.statement_ids[return_slot];
            compilation.ir.statement_ids[assignment_slot] = return_id;
            compilation.ir.statement_ids[let_slot] = assignment_id;
            compilation.ir.statement_ids[return_slot] = let_id;
            CHECK(!sol_ir_validate(&compilation.ir, NULL));
            compilation.ir.statement_ids[assignment_slot] = assignment_id;
            compilation.ir.statement_ids[let_slot] = let_id;
            compilation.ir.statement_ids[return_slot] = return_id;
        }
        assignment->local = saved_local;
        assignment->target = saved_target;
        later_return->expression = saved_return;

        SolIrSlice saved_roots = compilation.ir.locals[assignment->local].capability_roots;
        compilation.ir.locals[assignment->local].capability_roots
            = (SolIrSlice){.offset = compilation.ir.root_count + 1, .count = 1};
        CHECK(!sol_ir_validate(&compilation.ir, NULL));
        compilation.ir.locals[assignment->local].capability_roots = saved_roots;
    }
    CHECK(block == NULL || block->as.block.cleanup.count == 1);
    CHECK(sol_ir_validate(&compilation.ir, NULL));
    free_compilation(&compilation);

    CHECK(!compile_ir(&compilation,
        "module region_update\ncapability Token {}\n"
        "function bad(value: capability Token) -> Int64 { var slot = value "
        "region deeper { slot = slot } return 0 }\n"));
    CHECK(has_code(&compilation, "SOL-REGION-001"));
    free_compilation(&compilation);

    CHECK(!compile_ir(&compilation,
        "module loan_update\n"
        "capability Read { function read() -> Int64 effects { service.read<Self> } }\n"
        "capability Mock { function read() -> Int64 effects { pure } }\n"
        "function bad(source: capability Read, mock: capability Mock) -> Int64 "
        "effects { pure } { var slot = source return handle service.read<slot> "
        "with mock { slot = slot 1 } }\n"));
    CHECK(has_code(&compilation, "SOL-OWNERSHIP-003"));
    free_compilation(&compilation);
}

int main(void) {
    test_geometric_growth();
    test_complete_ir_and_lifetime();
    test_classified_calls_handler_contract();
    test_variants_nested_arenas_and_evidence();
    test_context_probes_and_escapes();
    test_generic_metadata_and_cycles();
    test_builtin_context_and_propagation_rejection();
    test_record_resolution_and_malformed_rejection();
    test_package_file_lifetime();
    test_forged_ir_domains();
    test_exact_validation_findings();
    test_release_gate_domains();
    test_exact_member_table_ownership();
    test_affine_ownership();
    test_region_cleanup_metadata();
    test_mutable_assignment_ir_and_ownership();
    if (failures != 0) fprintf(stderr, "%d IR test failure(s)\n", failures);
    return failures == 0 ? 0 : 1;
}
