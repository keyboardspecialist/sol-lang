#ifndef SOL_OWNERSHIP_H
#define SOL_OWNERSHIP_H

#include "sol/ir.h"

/* Compiler-internal ownership metadata over a completed typed SolIr. */
bool sol_ir_compute_copyability(const SolIr *ir, bool *copyable, size_t count);
bool sol_ir_analyze_ownership(SolIr *ir, SolDiagnostics *diagnostics);
bool sol_ir_validate_ownership(const SolIr *ir, SolDiagnostics *diagnostics);

#endif
