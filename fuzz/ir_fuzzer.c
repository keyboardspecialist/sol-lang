#include "sol/contract.h"
#include "sol/effectcheck.h"
#include "sol/hir.h"
#include "sol/ir.h"
#include "sol/lexer.h"
#include "sol/typecheck.h"

#include <stdint.h>
#include <string.h>

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

static bool compile(Compilation *compilation) {
    static const char source[] =
        "module fuzz_ir\n"
        "record Pair { left: Int64, right: Int64 }\n"
        "function sum(pair: Pair) -> Int64 { return pair.left + pair.right }\n"
        "function entry() -> Int64 { return sum(Pair { left = 1, right = 2 }) }\n";
    memset(compilation, 0, sizeof(*compilation));
    sol_tokens_init(&compilation->tokens);
    sol_diagnostics_init(&compilation->diagnostics);
    sol_syntax_tree_init(&compilation->syntax);
    sol_hir_module_init(&compilation->hir);
    sol_type_table_init(&compilation->types);
    sol_effect_table_init(&compilation->effects);
    sol_contract_table_init(&compilation->contracts);
    sol_ir_init(&compilation->ir);
    return sol_source_from_text(&compilation->source, "fuzz.sol", source)
        && sol_lex(&compilation->source, &compilation->tokens, &compilation->diagnostics)
        && sol_parse(&compilation->source, &compilation->tokens,
            &compilation->syntax, &compilation->diagnostics)
        && sol_hir_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->diagnostics)
        && sol_type_check(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->diagnostics)
        && sol_effect_check(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->diagnostics)
        && sol_contract_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->diagnostics)
        && sol_ir_lower(&compilation->source, &compilation->syntax,
            &compilation->hir, &compilation->types, &compilation->effects,
            &compilation->contracts, &compilation->ir, &compilation->diagnostics);
}

static void destroy(Compilation *compilation) {
    sol_ir_free(&compilation->ir);
    sol_contract_table_free(&compilation->contracts);
    sol_effect_table_free(&compilation->effects);
    sol_type_table_free(&compilation->types);
    sol_hir_module_free(&compilation->hir);
    sol_syntax_tree_free(&compilation->syntax);
    sol_tokens_free(&compilation->tokens);
    sol_source_free(&compilation->source);
    sol_diagnostics_free(&compilation->diagnostics);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 4096) return 0;
    Compilation compilation;
    if (!compile(&compilation)) {
        destroy(&compilation);
        return 0;
    }
    size_t operation = size == 0 ? 0 : data[0];
    size_t value = size < 2 ? 0 : data[1];
    SolIrExpression *expression = NULL;
    SolIrDefinition *definition = NULL;
    SolIrCallable *callable = NULL;
    SolIrLocal *local = NULL;
    SolIrExpressionKind old_expression_kind = 0;
    SolIrTypeId old_type = 0;
    SolIrDefinitionKind old_definition_kind = 0;
    SolIrExpressionId old_body = 0;
    SolAccessMode old_access = 0;
    SolIrLocalUse old_local_use = 0;
    switch (operation % 6) {
        case 0:
            if (compilation.ir.expression_count != 0) {
                expression = &compilation.ir.expressions[
                    value % compilation.ir.expression_count];
                old_expression_kind = expression->kind;
                expression->kind = (SolIrExpressionKind)-1;
            }
            break;
        case 1:
            if (compilation.ir.expression_count != 0) {
                expression = &compilation.ir.expressions[
                    value % compilation.ir.expression_count];
                old_type = expression->type;
                expression->type = value;
            }
            break;
        case 2:
            if (compilation.ir.definition_count != 0) {
                definition = &compilation.ir.definitions[
                    value % compilation.ir.definition_count];
                old_definition_kind = definition->kind;
                definition->kind = (SolIrDefinitionKind)-1;
            }
            break;
        case 3:
            if (compilation.ir.callable_count != 0) {
                callable = &compilation.ir.callables[value % compilation.ir.callable_count];
                old_body = callable->body;
                callable->body = value;
            }
            break;
        case 4:
            if (compilation.ir.local_count != 0) {
                local = &compilation.ir.locals[value % compilation.ir.local_count];
                old_access = local->access;
                local->access = (SolAccessMode)-1;
            }
            break;
        case 5:
            if (compilation.ir.expression_count != 0) {
                expression = &compilation.ir.expressions[
                    value % compilation.ir.expression_count];
                old_local_use = expression->local_use;
                expression->local_use = (SolIrLocalUse)-1;
            }
            break;
    }
    SolDiagnostics diagnostics;
    sol_diagnostics_init(&diagnostics);
    (void)sol_ir_validate(&compilation.ir, &diagnostics);
    sol_diagnostics_free(&diagnostics);
    switch (operation % 6) {
        case 0: if (expression != NULL) expression->kind = old_expression_kind; break;
        case 1: if (expression != NULL) expression->type = old_type; break;
        case 2: if (definition != NULL) definition->kind = old_definition_kind; break;
        case 3: if (callable != NULL) callable->body = old_body; break;
        case 4: if (local != NULL) local->access = old_access; break;
        case 5: if (expression != NULL) expression->local_use = old_local_use; break;
    }
    destroy(&compilation);
    return 0;
}
