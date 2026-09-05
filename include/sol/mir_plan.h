#ifndef SOL_MIR_PLAN_H
#define SOL_MIR_PLAN_H

#include "sol/mir_program.h"

/* Unstable trusted mutable compiler-internal owner. It borrows one validated
   SolMirProgram and never owns, mutates, or copies its MIR arenas. Validation
   supports bounded mutations that retain original allocation bases/extents.
   Restore pointer, capacity, and atom-name mutations before free. */
typedef size_t SolMirPlanTypeId;
typedef size_t SolMirPlanEffectRowId;
typedef size_t SolMirPlanInstanceId;
typedef size_t SolMirPlanImportId;
typedef size_t SolMirPlanContextId;

#define SOL_MIR_PLAN_NONE SIZE_MAX

typedef enum {
    SOL_MIR_PLAN_EFFECT_AUTHORITY_NONE,
    SOL_MIR_PLAN_EFFECT_AUTHORITY_RECEIVER,
    SOL_MIR_PLAN_EFFECT_AUTHORITY_PARAMETER,
    /* Closed effect-tail substitutions are deliberately authority-independent. */
    SOL_MIR_PLAN_EFFECT_AUTHORITY_TAIL,
} SolMirPlanEffectAuthority;

typedef struct {
    char *name;
    size_t length;
    SolMirPlanEffectAuthority authority;
    size_t ordinal;
} SolMirPlanEffectAtom;

typedef struct {
    size_t atom_offset;
    size_t atom_count;
} SolMirPlanEffectRow;

typedef struct {
    SolIrTypeKind kind;
    SolIrDefinitionId definition;
    size_t argument_offset;
    size_t argument_count;
    size_t parameter_offset;
    size_t parameter_count;
    size_t parameter_access_offset;
    SolMirPlanTypeId result;
    SolMirPlanEffectRowId effects;
    /* Concrete derived-capability source; not an ownership component. */
    SolMirPlanTypeId capability_source;
    /* Substituted fields/representation used to classify ownership. */
    size_t ownership_component_offset;
    size_t ownership_component_count;
} SolMirPlanType;

typedef struct {
    size_t offset;
    size_t count;
} SolMirPlanSlice;

typedef struct {
    size_t generic_ordinal;
    SolIrDefinitionId trait;
    SolIrCallableId requirement;
    SolMirPlanTypeId type;
    SolIrDefinitionId implementation;
    SolIrCallableId method;
} SolMirPlanDictionaryEntry;

typedef enum {
    SOL_MIR_PLAN_USE_RECEIVER,
    SOL_MIR_PLAN_USE_PARAMETER,
    SOL_MIR_PLAN_USE_RESULT,
    SOL_MIR_PLAN_USE_LOCAL,
    SOL_MIR_PLAN_USE_PLACE_ROOT,
    SOL_MIR_PLAN_USE_PLACE_PROJECTION,
    SOL_MIR_PLAN_USE_PLACE_FINAL,
    SOL_MIR_PLAN_USE_MIR_VALUE,
    SOL_MIR_PLAN_USE_MIR_INSTRUCTION,
    SOL_MIR_PLAN_USE_MIR_TEMPORARY,
    SOL_MIR_PLAN_USE_EXPRESSION,
    SOL_MIR_PLAN_USE_PATTERN,
    SOL_MIR_PLAN_USE_SOURCE_OPERAND,
    SOL_MIR_PLAN_USE_CONSTRUCT_OPERAND,
    SOL_MIR_PLAN_USE_SNAPSHOT,
    SOL_MIR_PLAN_USE_OBLIGATION_RESULT,
    SOL_MIR_PLAN_USE_OBLIGATION_PREDICATE,
    SOL_MIR_PLAN_USE_LOOP_OBLIGATION,
    SOL_MIR_PLAN_USE_UNREACHABLE_PROOF,
} SolMirPlanTypedUseKind;

/* source is the owning-IR or MIR arena id selected by kind. ordinal identifies
   a signature parameter, place projection, or construct operand as applicable. */
typedef struct {
    SolMirPlanTypedUseKind kind;
    size_t source;
    size_t ordinal;
    SolMirPlanContextId context;
    SolMirPlanTypeId type;
    SolAccessMode access;
} SolMirPlanTypedUse;

typedef enum {
    SOL_MIR_PLAN_CONTEXT_BODY,
    SOL_MIR_PLAN_CONTEXT_CONTRACT,
    SOL_MIR_PLAN_CONTEXT_REFINEMENT,
} SolMirPlanContextKind;

typedef enum {
    SOL_MIR_PLAN_TARGET_INSTANCE,
    SOL_MIR_PLAN_TARGET_IMPORT,
} SolMirPlanTargetKind;

typedef struct {
    SolMirPlanContextKind kind;
    SolMirPlanInstanceId instance;
    SolMirBlockId source_block;
    SolIrDefinitionId definition;
    SolObligationId obligation;
    SolMirProgramSource source;
    SolMirPlanTargetKind target_kind;
    SolMirPlanImportId import;
} SolMirPlanContext;

typedef struct {
    SolIrCallableId callable;
    SolMirPlanTypeId receiver;
    SolMirPlanSlice type_arguments;
    SolMirPlanSlice dictionary;
    SolMirPlanSlice parameter_types;
    SolMirPlanSlice parameter_accesses;
    SolMirPlanTypeId result;
    /* Closed authority-independent substitution for the callable tail. */
    SolMirPlanEffectRowId effect_tail;
    SolMirPlanEffectRowId effects;
    SolMirPlanSlice typed_uses;
    SolMirPlanSlice contexts;
} SolMirPlanInstance;

typedef struct {
    SolIrCallableId callable;
    SolMirPlanTypeId receiver;
    SolMirPlanSlice parameter_types;
    SolMirPlanSlice parameter_accesses;
    SolMirPlanTypeId result;
    SolMirPlanEffectRowId effects;
    SolMirPlanSlice typed_uses;
    SolMirPlanSlice contexts;
} SolMirPlanImport;

typedef enum {
    SOL_MIR_PLAN_DEMAND_ROOT,
    SOL_MIR_PLAN_DEMAND_INVOKE,
    SOL_MIR_PLAN_DEMAND_CALLBACK,
    SOL_MIR_PLAN_DEMAND_PREDICATE,
    SOL_MIR_PLAN_DEMAND_HANDLER_SOURCE,
    SOL_MIR_PLAN_DEMAND_HANDLER_PROVIDER,
    SOL_MIR_PLAN_DEMAND_FUNCTION_VALUE,
    SOL_MIR_PLAN_DEMAND_BOUND_OPERATION,
    SOL_MIR_PLAN_DEMAND_PREDICATE_FUNCTION_VALUE,
} SolMirPlanDemandKind;

typedef enum {
    SOL_MIR_PLAN_DEMAND_OWNER_ROOT,
    SOL_MIR_PLAN_DEMAND_OWNER_INSTANCE,
    SOL_MIR_PLAN_DEMAND_OWNER_IMPORT,
} SolMirPlanDemandOwnerKind;

typedef struct {
    SolMirPlanDemandKind kind;
    SolMirPlanDemandOwnerKind owner_kind;
    SolMirPlanInstanceId parent;
    SolMirPlanImportId parent_import;
    SolMirProgramSource source;
    SolIrCallableId symbolic_target;
    SolMirPlanInstanceId instance;
    SolMirPlanImportId import;
    SolIrDefinitionId dispatch_trait;
    SolIrCallableId dispatch_requirement;
    SolMirPlanContextId context;
} SolMirPlanDemand;

typedef struct {
    size_t max_instances;
    size_t max_concrete_types;
    size_t max_demands;
    size_t max_typed_uses;
    size_t max_contexts;
    size_t max_planning_work;
    size_t max_substitution_depth;
} SolMirPlanLimits;

typedef struct {
    size_t instances;
    size_t concrete_types;
    size_t demands;
    size_t typed_uses;
    size_t contexts;
    size_t planning_work;
    size_t substitution_depth;
} SolMirPlanUsage;

typedef struct {
    const SolMirProgram *program; /* Borrowed; must outlive this owner. */
    SolMirPlanType *types;
    size_t type_count;
    size_t type_capacity;
    SolMirPlanTypeId *type_components;
    size_t type_component_count;
    size_t type_component_capacity;
    SolAccessMode *type_parameter_accesses;
    size_t type_parameter_access_count;
    size_t type_parameter_access_capacity;
    SolMirPlanEffectAtom *effect_atoms;
    size_t effect_atom_count;
    size_t effect_atom_capacity;
    SolMirPlanEffectRow *effect_rows;
    size_t effect_row_count;
    size_t effect_row_capacity;
    size_t *effect_row_atoms;
    size_t effect_row_atom_count;
    size_t effect_row_atom_capacity;
    SolMirPlanInstance *instances;
    size_t instance_count;
    size_t instance_capacity;
    SolMirPlanTypeId *instance_type_ids;
    size_t instance_type_id_count;
    size_t instance_type_id_capacity;
    SolAccessMode *instance_accesses;
    size_t instance_access_count;
    size_t instance_access_capacity;
    SolMirPlanDictionaryEntry *dictionary_entries;
    size_t dictionary_entry_count;
    size_t dictionary_entry_capacity;
    SolMirPlanImport *imports;
    size_t import_count;
    size_t import_capacity;
    SolMirPlanTypedUse *typed_uses;
    size_t typed_use_count;
    size_t typed_use_capacity;
    SolMirPlanContext *contexts;
    size_t context_count;
    size_t context_capacity;
    SolMirPlanDemand *demands;
    size_t demand_count;
    size_t demand_capacity;
    SolMirPlanLimits limits;
    SolMirPlanUsage usage;
} SolMirPlan;

typedef struct {
    const SolMirProgram *program;
    /* NULL or wholly zero selects the defaults. Partial zero is invalid.
       Work charges structural interner/equality probes, recursive source-type
       substitutions, and instance-key probes. Other
       scans/sorts are bounded by the explicit array dimensions and validated
       source-program domains. substitution_depth is the maximum recursive
       source-type expansion, not a cumulative meter. */
    const SolMirPlanLimits *limits;
} SolMirPlanBuildRequest;

typedef enum {
    SOL_MIR_PLAN_BUILD_SUCCEEDED,
    SOL_MIR_PLAN_BUILD_INVALID_ARGUMENT,
    SOL_MIR_PLAN_BUILD_INVALID_PROGRAM,
    SOL_MIR_PLAN_BUILD_UNSUPPORTED_OR_UNRESOLVED,
    SOL_MIR_PLAN_BUILD_EXPANDING_RECURSION,
    SOL_MIR_PLAN_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_PLAN_BUILD_ALLOCATION_FAILED,
    SOL_MIR_PLAN_BUILD_INTERNAL_FAILED,
} SolMirPlanBuildOutcome;

void sol_mir_plan_init(SolMirPlan *plan);
void sol_mir_plan_free(SolMirPlan *plan);
SolMirPlanLimits sol_mir_plan_default_limits(void);
SolMirPlanBuildOutcome sol_mir_plan_build(
    const SolMirPlanBuildRequest *request,
    SolMirPlan *plan,
    SolDiagnostics *diagnostics
);
bool sol_mir_plan_validate(const SolMirPlan *plan, SolDiagnostics *diagnostics);

#endif
