#ifndef SOL_PARSER_H
#define SOL_PARSER_H

#include "sol/ast.h"
#include "sol/diagnostic.h"
#include "sol/token.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SOL_ITEM_RECORD,
    SOL_ITEM_ENUM,
    SOL_ITEM_CAPABILITY,
    SOL_ITEM_FUNCTION,
} SolItemKind;

typedef struct {
    SolItemKind kind;
    SolSpan name;
    SolSpan span;
    bool is_public;
    SolExprId body;
    SolParameterId first_parameter;
} SolSyntaxItem;

typedef struct {
    SolSpan module_name;
    unsigned int edition;
    SolSyntaxItem *items;
    size_t item_count;
    size_t item_capacity;
    SolExpr *expressions;
    size_t expression_count;
    size_t expression_capacity;
    SolStatement *statements;
    size_t statement_count;
    size_t statement_capacity;
    SolArgument *arguments;
    size_t argument_count;
    size_t argument_capacity;
    SolParameter *parameters;
    size_t parameter_count;
    size_t parameter_capacity;
} SolSyntaxTree;

void sol_syntax_tree_init(SolSyntaxTree *tree);
void sol_syntax_tree_free(SolSyntaxTree *tree);
bool sol_parse(
    const SolSource *source,
    const SolTokens *tokens,
    SolSyntaxTree *tree,
    SolDiagnostics *diagnostics
);

#endif
