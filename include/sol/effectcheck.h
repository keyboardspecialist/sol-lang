#ifndef SOL_EFFECTCHECK_H
#define SOL_EFFECTCHECK_H

#include "sol/diagnostic.h"
#include "sol/hir.h"
#include "sol/parser.h"
#include "sol/typecheck.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SOL_EFFECT_ATOM_NO_ARGUMENT,
    SOL_EFFECT_ATOM_STATIC_PATH,
    SOL_EFFECT_ATOM_PARAMETER,
    SOL_EFFECT_ATOM_SELF,
} SolEffectAtomArgumentKind;

typedef struct {
    SolSpan name;
    SolSpan argument;
    SolSpan span;
    SolEffectAtomArgumentKind argument_kind;
    SolParameterId parameter;
} SolEffectAtom;

typedef struct {
    SolEffectAtom *atoms;
    size_t count;
    bool inferred;
} SolEffectRow;

typedef struct {
    /* Function rows are indexed by SolDefId; non-function entries are empty. */
    SolEffectRow *functions;
    size_t function_count;
    SolEffectRow *capability_members;
    size_t capability_member_count;
} SolEffectTable;

void sol_effect_table_init(SolEffectTable *table);
void sol_effect_table_free(SolEffectTable *table);

bool sol_effect_check(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    SolEffectTable *effects,
    SolDiagnostics *diagnostics
);

#endif
