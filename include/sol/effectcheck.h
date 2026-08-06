#ifndef SOL_EFFECTCHECK_H
#define SOL_EFFECTCHECK_H

#include "sol/diagnostic.h"
#include "sol/hir.h"
#include "sol/parser.h"
#include "sol/typecheck.h"

#include <stdbool.h>

bool sol_effect_check(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    SolDiagnostics *diagnostics
);

#endif
