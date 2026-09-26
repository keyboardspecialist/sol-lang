#ifndef SOL_MIR_LINKAGE_H
#define SOL_MIR_LINKAGE_H

#include "sol/mir_operations.h"

#include <stdint.h>

typedef size_t SolMirLinkageCallableId;
typedef size_t SolMirLinkageHostRequirementId;
typedef size_t SolMirLinkageTableId;

#define SOL_MIR_LINKAGE_NONE SIZE_MAX
#define SOL_MIR_LINKAGE_DIGEST_BYTES 32
#define SOL_MIR_LINKAGE_SYMBOL_LENGTH 104
#define SOL_MIR_LINKAGE_SYMBOL_CAPACITY 105

typedef struct {
    uint8_t bytes[SOL_MIR_LINKAGE_DIGEST_BYTES];
} SolMirLinkageDigest;

typedef struct {
    char bytes[SOL_MIR_LINKAGE_SYMBOL_CAPACITY];
} SolMirLinkageSymbol;

typedef enum {
    SOL_MIR_LINKAGE_TARGET_INTERNAL,
    SOL_MIR_LINKAGE_TARGET_HOST,
} SolMirLinkageTargetKind;

typedef struct {
    SolMirPlanInstanceId instance;
    SolSemanticId semantic_id;
    SolMirLinkageDigest instance_key;
    SolMirLinkageSymbol symbol;
} SolMirLinkageCallable;

typedef struct {
    SolMirMaterializedBindingId binding;
    SolMirLinkageTargetKind target_kind;
    SolMirLinkageCallableId internal;
    SolMirLinkageHostRequirementId host;
} SolMirLinkageBinding;

typedef struct {
    SolMirMaterializedBindingId root_binding;
    SolMirLinkageCallableId callable;
    SolMirLinkageSymbol symbol;
} SolMirLinkageEntryExport;

typedef struct {
    SolMirMaterializedImportId import;
    SolSemanticId semantic_id;
    SolMirLinkageDigest requirement_key;
    SolMirRecipeId receiver;
    SolAccessMode receiver_access;
    SolMirPlanSlice parameters;
    SolMirPlanSlice parameter_accesses;
    SolMirRecipeId result;
    SolMirMaterializedEffectRowId effects;
} SolMirLinkageHostRequirement;

typedef struct {
    SolMirLinkageDigest identity;
    SolMirLinkageTargetKind target_kind;
    SolMirLinkageCallableId internal;
    SolMirLinkageHostRequirementId host;
} SolMirLinkageTableEntry;

typedef struct {
    size_t callable_plan;
    SolMirLinkageTableId table;
} SolMirLinkageCallableValue;

enum {
    SOL_MIR_LINKAGE_RUNTIME_CREATE = 1u << 0,
    SOL_MIR_LINKAGE_RUNTIME_COPY = 1u << 1,
    SOL_MIR_LINKAGE_RUNTIME_DROP = 1u << 2,
    SOL_MIR_LINKAGE_RUNTIME_EQUAL = 1u << 3,
    SOL_MIR_LINKAGE_RUNTIME_BOUND_ENVIRONMENT = 1u << 4,
};

typedef struct {
    SolMirRecipeId recipe;
    SolMirLinkageDigest recipe_key;
    uint32_t operations;
    SolMirStorageKind storage;
    SolMirCopyKind copy_kind;
    SolMirDropKind drop_kind;
} SolMirLinkageRuntimeRequirement;

typedef struct {
    size_t max_callables, max_bindings, max_entry_exports;
    size_t max_table_entries, max_callable_values;
    size_t max_host_requirements, max_runtime_requirements;
    size_t max_owned_bytes, max_build_scratch_bytes, max_build_work;
    size_t max_validation_scratch_bytes, max_validation_work;
} SolMirLinkageLimits;

typedef struct {
    size_t callables, bindings, entry_exports;
    size_t table_entries, callable_values;
    size_t host_requirements, runtime_requirements;
    size_t owned_bytes, build_scratch_bytes, build_work;
    size_t validation_scratch_bytes, validation_work;
} SolMirLinkageUsage;

#define SOL_MIR_LINKAGE_ARENAS(X) \
    X(callables, SolMirLinkageCallable, callable) \
    X(bindings, SolMirLinkageBinding, binding) \
    X(entry_exports, SolMirLinkageEntryExport, entry_export) \
    X(table_entries, SolMirLinkageTableEntry, table_entry) \
    X(callable_values, SolMirLinkageCallableValue, callable_value) \
    X(host_requirements, SolMirLinkageHostRequirement, host_requirement) \
    X(runtime_requirements, SolMirLinkageRuntimeRequirement, runtime_requirement)

/* Unstable target-neutral owner. operations is borrowed and must outlive this
   owner. Symbols and table identities are stable; table IDs are abstract and
   are not target table indices or ABI selections. */
typedef struct {
    const SolMirOperations *operations;
#define SOL_MIR_LINKAGE_MEMBER(member, type, singular) \
    type *member; size_t singular##_count, singular##_capacity;
    SOL_MIR_LINKAGE_ARENAS(SOL_MIR_LINKAGE_MEMBER)
#undef SOL_MIR_LINKAGE_MEMBER
    SolMirLinkageLimits limits;
    SolMirLinkageUsage usage;
} SolMirLinkage;

typedef struct {
    const SolMirOperations *operations;
    /* NULL or wholly zero selects defaults. Partial zero is invalid. */
    const SolMirLinkageLimits *limits;
} SolMirLinkageBuildRequest;

typedef enum {
    SOL_MIR_LINKAGE_BUILD_SUCCEEDED,
    SOL_MIR_LINKAGE_BUILD_INVALID_ARGUMENT,
    SOL_MIR_LINKAGE_BUILD_INVALID_OPERATIONS,
    SOL_MIR_LINKAGE_BUILD_UNRESOLVED_EXTERNAL,
    SOL_MIR_LINKAGE_BUILD_SYMBOL_COLLISION,
    SOL_MIR_LINKAGE_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_LINKAGE_BUILD_ALLOCATION_FAILED,
    SOL_MIR_LINKAGE_BUILD_INTERNAL_FAILED,
} SolMirLinkageBuildOutcome;

void sol_mir_linkage_init(SolMirLinkage *linkage);
void sol_mir_linkage_free(SolMirLinkage *linkage);
SolMirLinkageLimits sol_mir_linkage_default_limits(void);
SolMirLinkageBuildOutcome sol_mir_linkage_build(
    const SolMirLinkageBuildRequest *request,
    SolMirLinkage *linkage,
    SolDiagnostics *diagnostics
);
bool sol_mir_linkage_validate(
    const SolMirLinkage *linkage,
    SolDiagnostics *diagnostics
);
/* Validation and buffering complete before the single output write. */
bool sol_mir_linkage_render(FILE *stream, const SolMirLinkage *linkage);

#endif
