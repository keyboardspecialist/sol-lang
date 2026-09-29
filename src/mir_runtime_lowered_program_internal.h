#ifndef SOL_MIR_RUNTIME_LOWERED_PROGRAM_INTERNAL_H
#define SOL_MIR_RUNTIME_LOWERED_PROGRAM_INTERNAL_H
#include "sol/mir_runtime_lowered_program.h"
typedef struct { size_t limit, used; bool exhausted; } SolMirRuntimeLoweredWorkMeter;
static inline bool sol_mir_runtime_lowered_tick(SolMirRuntimeLoweredWorkMeter *m) { if(!m||m->used==SIZE_MAX||m->used>=m->limit){if(m)m->exhausted=true;return false;}++m->used;return true; }
SolMirRuntimeLoweredProgramBuildOutcome sol_mir_runtime_lowered_program_internal_validate(const SolMirRuntimeLoweredProgram *, SolDiagnostics *, bool, size_t *);
/* A non-NULL meter makes sealing part of the builder's work budget. */
uint64_t sol_mir_runtime_lowered_program_internal_seal(
    const SolMirRuntimeLoweredProgram *, SolMirRuntimeLoweredWorkMeter *);
void *sol_mir_runtime_lowered_program_internal_validation_scratch(size_t,size_t);
#endif
