#include "sol/contract.h"
#include "sol/effectcheck.h"
#include "sol/hir.h"
#include "sol/interpreter.h"
#include "sol/ir.h"
#include "sol/lexer.h"
#include "sol/parser.h"
#include "sol/typecheck.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", \
                __FILE__, __LINE__, #condition); \
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
} Compilation;

static void compilation_init(Compilation *compilation) {
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
}

static bool compile(Compilation *compilation, const char *text) {
    compilation_init(compilation);
    return sol_source_from_text(&compilation->source, "test.sol", text)
        && sol_lex(&compilation->source, &compilation->tokens,
            &compilation->diagnostics)
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
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->diagnostics)
        && !sol_diagnostics_has_errors(&compilation->diagnostics)
        && sol_contract_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics)
        && sol_ir_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->ir, &compilation->diagnostics);
}

static void frontend_free(Compilation *compilation) {
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_syntax_tree_free(&compilation->syntax);
    sol_tokens_free(&compilation->tokens);
    sol_source_free(&compilation->source);
    sol_diagnostics_free(&compilation->diagnostics);
}

static void compilation_free(Compilation *compilation) {
    sol_ir_free(&compilation->ir);
    frontend_free(compilation);
}

static bool hir_rejects_shape(const SolSource *source, const SolSyntaxTree *syntax) {
    SolHirModule hir;
    SolDiagnostics diagnostics;
    sol_hir_module_init(&hir);
    sol_diagnostics_init(&diagnostics);
    bool rejected = !sol_hir_lower(source, syntax, &hir, &diagnostics);
    sol_hir_module_free(&hir);
    sol_diagnostics_free(&diagnostics);
    return rejected;
}

static void test_pipeline_and_entry(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module tests\n"
        "private function helper() -> Bool { true }\n"
        "test \"decoded\\nlabel\" { helper() }\n"));
    CHECK(compilation.syntax.item_count == 2);
    CHECK(compilation.syntax.items[1].kind == SOL_ITEM_TEST);
    CHECK(compilation.types.definitions[1].kind == SOL_TYPE_BOOL);
    CHECK(compilation.effects.functions[1].inferred);
    CHECK(compilation.contracts.obligation_count == 0);
    CHECK(compilation.ir.definitions[1].kind == SOL_IR_DEFINITION_TEST);
    CHECK(compilation.ir.callables[
        compilation.ir.definitions[1].callable].kind == SOL_IR_CALLABLE_TEST);
    CHECK(strcmp(compilation.ir.definitions[1].name, "decoded\nlabel") == 0);

    SolIr ir = compilation.ir;
    sol_ir_init(&compilation.ir);
    frontend_free(&compilation);
    SolInterpreterRequest request = {
        .ir = &ir,
        .callable = ir.definitions[1].callable,
        .definition = SOL_IR_NONE,
        .contracts = SOL_INTERPRETER_CONTRACTS_IGNORE,
        .test_entry = true,
    };
    SolInterpreterResult result;
    CHECK(sol_interpret(&request, &result));
    CHECK(result.value.kind == SOL_INTERPRETER_VALUE_BOOL);
    CHECK(result.value.as.boolean);
    sol_interpreter_result_free(&result);
    request.test_entry = false;
    CHECK(!sol_interpret(&request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    sol_interpreter_result_free(&result);

    request.callable = SOL_IR_NONE;
    request.definition = 1;
    request.test_entry = true;
    CHECK(sol_interpret(&request, &result));
    sol_interpreter_result_free(&result);
    request.definition = 0;
    CHECK(!sol_interpret(&request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    sol_interpreter_result_free(&result);
    request.test_entry = false;
    CHECK(sol_interpret(&request, &result));
    sol_interpreter_result_free(&result);
    request.definition = 1;
    CHECK(!sol_interpret(&request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_REQUEST);
    sol_interpreter_result_free(&result);

    SolIrCallableId callable = ir.definitions[1].callable;
    SolIrExpressionId test_body = ir.callables[callable].body;
    SolIrExpression saved_body = ir.expressions[test_body];
    ir.expressions[test_body].kind = SOL_IR_EXPR_DEFINITION;
    ir.expressions[test_body].as.definition = 1;
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    CHECK(!sol_ir_validate(&ir, &diagnostics));
    sol_diagnostics_free(&diagnostics);
    ir.expressions[test_body] = saved_body;

    ir.callables[callable].parameters.count = 1;
    sol_diagnostics_init(&diagnostics);
    CHECK(!sol_ir_validate(&ir, &diagnostics));
    sol_diagnostics_free(&diagnostics);
    ir.callables[callable].parameters.count = 0;

    SolIrCallableId function_callable = ir.definitions[0].callable;
    ir.definitions[0].callable = callable;
    sol_diagnostics_init(&diagnostics);
    CHECK(!sol_ir_validate(&ir, &diagnostics));
    sol_diagnostics_free(&diagnostics);
    request.definition = 0;
    CHECK(!sol_interpret(&request, &result));
    CHECK(result.diagnostic.code == SOL_INTERPRETER_INVALID_IR);
    sol_interpreter_result_free(&result);
    ir.definitions[0].callable = function_callable;

    ir.definitions[1].callable = function_callable;
    sol_diagnostics_init(&diagnostics);
    CHECK(!sol_ir_validate(&ir, &diagnostics));
    sol_diagnostics_free(&diagnostics);
    ir.definitions[1].callable = callable;

    ir.callables[callable].owner = 0;
    sol_diagnostics_init(&diagnostics);
    CHECK(!sol_ir_validate(&ir, &diagnostics));
    sol_diagnostics_free(&diagnostics);
    ir.callables[callable].owner = 1;
    sol_ir_free(&ir);
}

static void test_ordinary_function_value(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module function_value\n"
        "private function identity(value: Int64) -> Int64 { return value }\n"
        "private function apply(callback: function(Int64) -> Int64 effects { pure }) "
        "-> Int64 { return callback(1) }\n"
        "test \"ordinary function value\" apply(identity) == 1\n"));
    bool found = false;
    SolIrExpressionId function_value = SOL_IR_NONE;
    for (size_t expression = 0; expression < compilation.ir.expression_count;
        ++expression) {
        const SolIrExpression *entry = &compilation.ir.expressions[expression];
        if (entry->kind != SOL_IR_EXPR_DEFINITION) continue;
        SolIrDefinitionId definition = entry->as.definition;
        if (definition < compilation.ir.definition_count
            && strcmp(compilation.ir.definitions[definition].name, "identity") == 0) {
            found = true;
            function_value = expression;
            CHECK(compilation.ir.definitions[definition].kind
                == SOL_IR_DEFINITION_FUNCTION);
            CHECK(entry->type < compilation.ir.type_count);
            CHECK(compilation.ir.types[entry->type].kind == SOL_IR_TYPE_FUNCTION);
        }
    }
    CHECK(found);
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    CHECK(sol_ir_validate(&compilation.ir, &diagnostics));
    sol_diagnostics_free(&diagnostics);
    if (function_value != SOL_IR_NONE) {
        SolIrTypeId saved = compilation.ir.expressions[function_value].type;
        compilation.ir.expressions[function_value].type
            = compilation.ir.callables[compilation.ir.definitions[2].callable].result;
        sol_diagnostics_init(&diagnostics);
        CHECK(!sol_ir_validate(&compilation.ir, &diagnostics));
        sol_diagnostics_free(&diagnostics);
        compilation.ir.expressions[function_value].type = saved;
    }
    compilation_free(&compilation);
}

static void test_nonfunction_definition_value_rejected(void) {
    Compilation compilation;
    CHECK(compile(&compilation,
        "module nonfunction_value\n"
        "record Marker { value: Bool }\n"
        "test \"entry\" true\n"));
    SolIrDefinitionId record = SOL_IR_NONE;
    SolIrDefinitionId test = SOL_IR_NONE;
    for (size_t index = 0; index < compilation.ir.definition_count; ++index) {
        if (compilation.ir.definitions[index].kind == SOL_IR_DEFINITION_RECORD) {
            record = index;
        } else if (compilation.ir.definitions[index].kind == SOL_IR_DEFINITION_TEST) {
            test = index;
        }
    }
    CHECK(record != SOL_IR_NONE);
    CHECK(test != SOL_IR_NONE);
    if (record != SOL_IR_NONE && test != SOL_IR_NONE) {
        SolIrExpressionId body
            = compilation.ir.callables[compilation.ir.definitions[test].callable].body;
        SolIrExpression saved = compilation.ir.expressions[body];
        compilation.ir.expressions[body].kind = SOL_IR_EXPR_DEFINITION;
        compilation.ir.expressions[body].as.definition = record;
        SolDiagnostics diagnostics;
        sol_diagnostics_init(&diagnostics);
        CHECK(!sol_ir_validate(&compilation.ir, &diagnostics));
        sol_diagnostics_free(&diagnostics);
        compilation.ir.expressions[body] = saved;
    }
    compilation_free(&compilation);
}

static void test_non_addressable_and_duplicates(void) {
    Compilation compilation;
    compilation_init(&compilation);
    const char *text =
        "module tests\n"
        "test \"same\" true\n"
        "test \"same\" true\n"
        "private function bad() -> Bool { same() }\n";
    CHECK(sol_source_from_text(&compilation.source, "duplicate.sol", text));
    CHECK(sol_lex(&compilation.source, &compilation.tokens, &compilation.diagnostics));
    CHECK(sol_parse(&compilation.source, &compilation.tokens,
        &compilation.syntax, &compilation.diagnostics));
    CHECK(sol_hir_lower(&compilation.source, &compilation.syntax,
        &compilation.hir, &compilation.diagnostics));
    bool duplicate = false;
    bool unresolved = false;
    for (size_t index = 0; index < compilation.diagnostics.count; ++index) {
        duplicate = duplicate
            || strcmp(compilation.diagnostics.items[index].code, "SOL-RESOLVE-001") == 0;
        unresolved = unresolved
            || strcmp(compilation.diagnostics.items[index].code, "SOL-RESOLVE-002") == 0;
    }
    CHECK(duplicate);
    CHECK(unresolved);
    compilation_free(&compilation);
}

static void test_parser_recovery_and_restrictions(void) {
    Compilation compilation;
    compilation_init(&compilation);
    const char *text =
        "module tests\n"
        "public test \"modified\" true\n"
        "test 12\n"
        "test \"recovered\" true\n";
    CHECK(sol_source_from_text(&compilation.source, "recovery.sol", text));
    CHECK(sol_lex(&compilation.source, &compilation.tokens, &compilation.diagnostics));
    CHECK(sol_parse(&compilation.source, &compilation.tokens,
        &compilation.syntax, &compilation.diagnostics));
    CHECK(compilation.syntax.item_count == 2);
    CHECK(compilation.syntax.items[0].kind == SOL_ITEM_TEST);
    CHECK(compilation.syntax.items[1].kind == SOL_ITEM_TEST);
    CHECK(sol_diagnostics_has_errors(&compilation.diagnostics));
    compilation_free(&compilation);
}

static void test_complete_test_shape_validation(void) {
    Compilation compilation;
    CHECK(compile(&compilation, "module shape\ntest \"shape\" true\n"));
    SolSyntaxItem *item = &compilation.syntax.items[0];
#define REJECT_TEST_SHAPE(field, value) \
    do { \
        SolSyntaxItem saved = *item; \
        item->field = (value); \
        CHECK(!sol_syntax_contracts_validate( \
            &compilation.source, &compilation.syntax)); \
        CHECK(hir_rejects_shape(&compilation.source, &compilation.syntax)); \
        *item = saved; \
    } while (0)
    REJECT_TEST_SHAPE(flavor, SOL_TYPE_DECLARATION_DISTINCT);
    REJECT_TEST_SHAPE(stable_identity, item->name);
    REJECT_TEST_SHAPE(is_public, true);
    REJECT_TEST_SHAPE(body, SOL_AST_NONE);
    REJECT_TEST_SHAPE(first_parameter, 0);
    REJECT_TEST_SHAPE(return_type, item->name);
    REJECT_TEST_SHAPE(return_type_id, 0);
    REJECT_TEST_SHAPE(first_field, 0);
    REJECT_TEST_SHAPE(first_variant, 0);
    REJECT_TEST_SHAPE(is_open, true);
    REJECT_TEST_SHAPE(first_effect, 0);
    REJECT_TEST_SHAPE(has_effect_clause, true);
    REJECT_TEST_SHAPE(first_contract, 0);
    REJECT_TEST_SHAPE(representation_type, 0);
    REJECT_TEST_SHAPE(first_member, 0);
    REJECT_TEST_SHAPE(result_authority_parameter, 0);
    REJECT_TEST_SHAPE(capability_source, 0);
    REJECT_TEST_SHAPE(first_type_parameter, 0);
    REJECT_TEST_SHAPE(first_effect_parameter, 0);
    REJECT_TEST_SHAPE(trait_name, item->name);
    REJECT_TEST_SHAPE(implementation_type, 0);
    REJECT_TEST_SHAPE(first_trait_method, 0);
#undef REJECT_TEST_SHAPE
    SolSpan stable = compilation.hir.definitions[0].stable_identity;
    compilation.hir.definitions[0].stable_identity = item->name;
    SolTypeTable types;
    SolDiagnostics diagnostics;
    sol_type_table_init(&types);
    sol_diagnostics_init(&diagnostics);
    CHECK(!sol_type_check(&compilation.source, &compilation.syntax,
        &compilation.hir, &types, &diagnostics));
    sol_type_table_free(&types);
    sol_diagnostics_free(&diagnostics);
    compilation.hir.definitions[0].stable_identity = stable;
    compilation_free(&compilation);
}

int main(void) {
    test_pipeline_and_entry();
    test_ordinary_function_value();
    test_nonfunction_definition_value_rejected();
    test_non_addressable_and_duplicates();
    test_parser_recovery_and_restrictions();
    test_complete_test_shape_validation();
    return failures == 0 ? 0 : 1;
}
