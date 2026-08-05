#ifndef SOL_TYPECHECK_H
#define SOL_TYPECHECK_H

#include "sol/diagnostic.h"
#include "sol/hir.h"
#include "sol/parser.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SOL_TYPE_UNKNOWN,
    SOL_TYPE_ERROR,
    SOL_TYPE_INT64,
    SOL_TYPE_BOOL,
    SOL_TYPE_TEXT,
    SOL_TYPE_UNIT,
    SOL_TYPE_NOMINAL,
    SOL_TYPE_OPAQUE,
    SOL_TYPE_FUNCTION,
    SOL_TYPE_NEVER,
} SolTypeKind;

typedef struct {
    SolTypeKind kind;
    SolDefId definition;
} SolType;

typedef struct {
    SolType *expressions;
    size_t expression_count;
    SolType *locals;
    size_t local_count;
    SolType *definitions;
    size_t definition_count;
} SolTypeTable;

void sol_type_table_init(SolTypeTable *table);
void sol_type_table_free(SolTypeTable *table);
bool sol_type_check(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    SolTypeTable *types,
    SolDiagnostics *diagnostics
);

#endif
