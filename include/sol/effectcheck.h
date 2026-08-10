#ifndef SOL_EFFECTCHECK_H
#define SOL_EFFECTCHECK_H

#include "sol/diagnostic.h"
#include "sol/effect.h"
#include "sol/hir.h"
#include "sol/parser.h"
#include "sol/typecheck.h"

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    SolEffectAtom *atoms;
    size_t count;
    /* Omitted private rows are inferred, including rows in recursive call SCCs. */
    bool inferred;
    SolEffectParameterId effect_parameter;
} SolEffectRow;

typedef struct {
    /* Indexed by call expression; arguments and rows are table-owned slices. */
    SolExprId call;
    SolDefId function;
    SolEffectParameterId parameter;
    size_t argument_offset;
    size_t argument_count;
    size_t row_offset;
    size_t row_count;
} SolEffectCallInstantiation;

typedef struct {
    /* Function rows are indexed by SolDefId; non-function entries are empty. */
    SolEffectRow *functions;
    size_t function_count;
    SolEffectRow *capability_members;
    size_t capability_member_count;
    SolEffectCallInstantiation *call_instantiations;
    size_t call_instantiation_count;
    size_t call_instantiation_capacity;
    SolEffectAtom *call_arguments;
    size_t call_argument_count;
    size_t call_argument_capacity;
    SolEffectAtom *call_rows;
    size_t call_row_count;
    size_t call_row_capacity;
} SolEffectTable;

void sol_effect_table_init(SolEffectTable *table);
void sol_effect_table_free(SolEffectTable *table);
const SolEffectCallInstantiation *sol_effect_call_instantiation(
    const SolEffectTable *table,
    SolExprId call
);
bool sol_effect_call_arguments(
    const SolEffectTable *table,
    SolExprId call,
    const SolEffectAtom **atoms,
    size_t *count
);
bool sol_effect_call_row(
    const SolEffectTable *table,
    SolExprId call,
    const SolEffectAtom **atoms,
    size_t *count
);
bool sol_effect_call_instantiation_valid(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    const SolEffectTable *effects,
    SolExprId call
);

bool sol_effect_check(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    SolEffectTable *effects,
    SolDiagnostics *diagnostics
);

#endif
