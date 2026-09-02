#ifndef SOL_MIR_PROGRAM_H
#define SOL_MIR_PROGRAM_H

#include "sol/mir.h"

/* Unstable trusted mutable compiler-internal owner; not a concrete program ABI
   or hostile arbitrary-pointer input format. Validation supports owners produced
   by sol_mir_program_build and test mutations that preserve every original
   allocation base. sol_mir_program_free requires those original builder-owned
   allocation pointers; restore pointer mutations before freeing. */
typedef enum {
    SOL_MIR_PROGRAM_ROOT_ENTRY,
    SOL_MIR_PROGRAM_ROOT_TEST,
    SOL_MIR_PROGRAM_ROOT_INTERNAL_FIXTURE,
} SolMirProgramRootKind;

typedef struct {
    SolIrCallableId callable;
    SolMirProgramRootKind kind;
} SolMirProgramRoot;

typedef struct {
    size_t max_callable_classifications;
    size_t max_references;
    size_t max_discovery_work;
} SolMirProgramLimits;

typedef struct {
    size_t callable_classifications;
    size_t references;
    size_t discovery_work;
} SolMirProgramUsage;

typedef enum {
    SOL_MIR_PROGRAM_REFERENCE_INVOKE,
    SOL_MIR_PROGRAM_REFERENCE_FUNCTION_VALUE,
    SOL_MIR_PROGRAM_REFERENCE_BOUND_OPERATION,
    SOL_MIR_PROGRAM_REFERENCE_HANDLER_SOURCE,
    SOL_MIR_PROGRAM_REFERENCE_HANDLER_PROVIDER,
    SOL_MIR_PROGRAM_REFERENCE_PREDICATE_CALL,
    SOL_MIR_PROGRAM_REFERENCE_PREDICATE_FUNCTION_VALUE,
} SolMirProgramReferenceKind;

/* File is an owning-IR file ordinal, and offsets are relative to that file.
   No source path or process pointer is retained. */
typedef struct {
    SolIrCallableId callable;
    SolIrExpressionId expression;
    size_t file;
    size_t start;
    size_t end;
} SolMirProgramSource;

typedef struct {
    SolMirProgramReferenceKind kind;
    SolMirProgramSource source;
    SolIrCallableId target;
} SolMirProgramReference;

typedef struct {
    SolIrCallableId callable;
    SolMirProgramSource first_source;
    size_t source_count;
} SolMirProgramImport;

typedef struct {
    SolIrDefinitionId trait;
    SolIrCallableId requirement;
    SolIrTypeId type;
    SolIrDefinitionId implementation;
    SolIrCallableId method;
    SolMirProgramSource first_source;
    size_t source_count;
} SolMirProgramSpecialization;

typedef struct {
    SolIrCallableId callable;
    SolMir mir;
} SolMirProgramTemplate;

typedef struct {
    const SolIr *ir; /* Borrowed; must outlive this owner. */
    SolMirProgramRoot *roots;
    size_t root_count;
    /* Canonical copy of every valid request approval, including unreachable extras. */
    SolIrCallableId *approved_imports;
    size_t approved_import_count;
    SolMirProgramTemplate *templates;
    size_t template_count;
    /* Reachable subset of approved_imports. */
    SolMirProgramImport *imports;
    size_t import_count;
    SolMirProgramSpecialization *specializations;
    size_t specialization_count;
    SolMirProgramReference *references;
    size_t reference_count;
    SolMirProgramLimits limits;
    SolMirProgramUsage usage;
} SolMirProgram;

/* Canonical ownership requires NULL for every zero-count top-level array and
   for every zero-count/zero-capacity nested MIR arena. As defense in depth,
   sol_mir_program_validate rejects detectable exact or partial overlap among
   declared owned ranges. Portable C cannot prove independent allocator
   provenance for arbitrary adjacent/interior pointers. */

typedef struct {
    const SolIr *ir;
    /* Trait-bounded generic callables cannot be roots: each root is an
       independent invocation context and cannot borrow evidence from another
       root. Unbounded generic INTERNAL_FIXTURE roots remain eligible. */
    const SolMirProgramRoot *roots;
    size_t root_count;
    const SolIrCallableId *approved_imports;
    size_t approved_import_count;
    /* NULL or a wholly zero value selects sol_mir_program_default_limits().
       A partially zero limits value is invalid. max_discovery_work meters
       deterministic P2.1 normalized-root/approval processing,
       queue/classification, source DAG, MIR-source, evidence, obligation,
       import-shape, and canonical-sort work. Input copying/deduplication,
       prerequisite SolIr validation, and the internal work performed by
       sol_mir_lower_callable/sol_mir_validate are outside this local budget;
       input counts are independently bounded by the validated callable domain. */
    const SolMirProgramLimits *limits;
} SolMirProgramBuildRequest;

typedef enum {
    SOL_MIR_PROGRAM_BUILD_SUCCEEDED,
    SOL_MIR_PROGRAM_BUILD_INVALID_ARGUMENT,
    SOL_MIR_PROGRAM_BUILD_INVALID_IR,
    SOL_MIR_PROGRAM_BUILD_UNSUPPORTED_CLOSURE,
    SOL_MIR_PROGRAM_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_PROGRAM_BUILD_ALLOCATION_FAILED,
    SOL_MIR_PROGRAM_BUILD_INTERNAL_FAILED,
} SolMirProgramBuildOutcome;

void sol_mir_program_init(SolMirProgram *program);
void sol_mir_program_free(SolMirProgram *program);
SolMirProgramLimits sol_mir_program_default_limits(void);
/* A successful result has passed independent sol_mir_program_validate while
   still private; failed validation is transactional and is never published. */
SolMirProgramBuildOutcome sol_mir_program_build(
    const SolMirProgramBuildRequest *request,
    SolMirProgram *program,
    SolDiagnostics *diagnostics
);
bool sol_mir_program_validate(
    const SolMirProgram *program,
    SolDiagnostics *diagnostics
);

#endif
