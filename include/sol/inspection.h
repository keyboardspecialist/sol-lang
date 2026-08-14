#ifndef SOL_INSPECTION_H
#define SOL_INSPECTION_H

#include "sol/contract.h"
#include "sol/package.h"

#include <stdbool.h>
#include <stdio.h>

/*
 * Unstable compiler-internal API. Inputs must be one mutually consistent set of
 * successful frontend tables. Malformed shapes are rejected before output.
 * Construction failure writes nothing; a transport failure may truncate output.
 */
bool sol_inspection_render(
    FILE *stream,
    const SolPackage *package,
    const SolHirModule *hir,
    const SolTypeTable *types,
    const SolEffectTable *effects,
    const SolContractTable *contracts,
    const SolDiagnostics *diagnostics
);

#endif
