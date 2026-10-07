#ifndef SOL_MIR_RUNTIME_CONVENTIONS_H
#define SOL_MIR_RUNTIME_CONVENTIONS_H

#include "sol/mir_concrete.h"

#include <stdint.h>

typedef size_t SolMirRuntimeSignatureId;
typedef size_t SolMirRuntimeSignatureSlotId;
typedef size_t SolMirRuntimeCallId;
typedef size_t SolMirRuntimeOperandId;
typedef size_t SolMirRuntimeWritebackId;
typedef size_t SolMirRuntimeEntryId;
typedef size_t SolMirRuntimeImportId;
typedef size_t SolMirRuntimeFailureSiteId;

#define SOL_MIR_RUNTIME_NONE SIZE_MAX
#define SOL_MIR_RUNTIME_IMPORT_SYMBOL_CAPACITY 105
#define SOL_MIR_RUNTIME_HOST_DETAIL_MAX 191

typedef struct {
    size_t offset;
    size_t count;
} SolMirRuntimeSlice;

typedef struct {
    size_t file;
    size_t start;
    size_t end;
} SolMirRuntimeSource;

typedef enum {
    SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_ARITHMETIC,
    SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_CALL,
    SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_PANIC,
    SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_NO_MATCH,
    SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_UNREACHABLE,
    SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_ARITHMETIC,
    SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_CALL,
    SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_NO_MATCH,
    SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_RESULT,
    SOL_MIR_RUNTIME_FAILURE_ORIGIN_IMAGE_STEP,
    SOL_MIR_RUNTIME_FAILURE_ORIGIN_PREDICATE_STEP,
} SolMirRuntimeFailureOriginKind;

typedef struct {
    SolMirRuntimeFailureOriginKind origin_kind;
    size_t owner;
    size_t block;
    size_t instruction;
    SolMirRuntimeSource source;
    /* Stable source-order discriminator for distinct sites sharing a span. */
    size_t occurrence;
    uint32_t allowed_codes;
} SolMirRuntimeFailureSite;

typedef enum {
    SOL_MIR_RUNTIME_SIGNATURE_INTERNAL,
    SOL_MIR_RUNTIME_SIGNATURE_HOST,
    SOL_MIR_RUNTIME_SIGNATURE_FUNCTION_RECIPE,
} SolMirRuntimeSignatureOrigin;

typedef enum {
    SOL_MIR_RUNTIME_SLOT_RECEIVER,
    SOL_MIR_RUNTIME_SLOT_PARAMETER,
} SolMirRuntimeSlotRole;

typedef enum {
    SOL_MIR_RUNTIME_RESULT_VALUE,
    SOL_MIR_RUNTIME_RESULT_UNIT,
    SOL_MIR_RUNTIME_RESULT_NEVER,
} SolMirRuntimeResultClass;

typedef struct {
    SolMirRuntimeSlotRole role;
    size_t formal;
    SolMirRecipeId recipe;
    SolAccessMode access;
} SolMirRuntimeSignatureSlot;

typedef struct {
    SolMirRuntimeSignatureOrigin origin;
    SolMirLinkageCallableId internal;
    SolMirLinkageHostRequirementId host;
    SolMirRecipeId function_recipe;
    SolMirRuntimeSlice slots;
    SolMirRecipeId result;
    SolMirRuntimeResultClass result_class;
    SolMirMaterializedEffectRowId effects;
} SolMirRuntimeSignature;

typedef enum {
    SOL_MIR_RUNTIME_CALL_OWNER_IMAGE,
    SOL_MIR_RUNTIME_CALL_OWNER_PREDICATE,
} SolMirRuntimeCallOwnerKind;

typedef enum {
    SOL_MIR_RUNTIME_TARGET_DIRECT_INTERNAL,
    SOL_MIR_RUNTIME_TARGET_DIRECT_HOST,
    SOL_MIR_RUNTIME_TARGET_INDIRECT_TABLE,
} SolMirRuntimeCallTargetKind;

typedef enum {
    SOL_MIR_RUNTIME_VALUE_NONE,
    SOL_MIR_RUNTIME_VALUE_MATERIALIZED_TEMPORARY,
    SOL_MIR_RUNTIME_VALUE_MATERIALIZED_PLACE,
    SOL_MIR_RUNTIME_VALUE_MATERIALIZED_VALUE,
    SOL_MIR_RUNTIME_VALUE_PREDICATE_VALUE,
    SOL_MIR_RUNTIME_VALUE_BOUND_RECEIVER,
} SolMirRuntimeValueKind;

typedef struct {
    SolMirRuntimeValueKind kind;
    size_t id;
} SolMirRuntimeValueRef;

typedef struct {
    SolMirRuntimeSignatureSlotId signature_slot;
    SolMirRuntimeValueRef value;
} SolMirRuntimeOperand;

typedef struct {
    SolMirRuntimeOperandId operand;
    bool receiver;
    size_t formal;
    SolMirMaterializedPlaceId place;
    SolMirRecipeId recipe;
} SolMirRuntimeWriteback;

typedef struct {
    SolMirRuntimeCallOwnerKind owner_kind;
    SolMirPlanInstanceId image;
    SolMirPredicateBodyId predicate;
    size_t block;
    SolIrCallKind call_kind;
    SolMirRuntimeSignatureId signature;
    SolMirRuntimeCallTargetKind target_kind;
    SolMirLinkageCallableId internal;
    SolMirLinkageHostRequirementId host;
    SolMirLinkageTableId table;
    SolMirRuntimeValueRef callee;
    SolMirRuntimeSlice operands;
    SolMirRuntimeValueRef result;
    size_t normal_edge;
    size_t failure_edge;
    SolMirRuntimeSlice writebacks;
    SolMirRuntimeFailureSiteId failure_site;
} SolMirRuntimeCall;

typedef struct {
    size_t export_id;
    SolMirMaterializedBindingId binding;
    SolMirLinkageCallableId callable;
    SolMirRuntimeSignatureId signature;
    SolMirLinkageSymbol symbol;
    SolMirProgramSource source;
    SolMirRuntimeResultClass result_class;
} SolMirRuntimeEntry;

typedef enum {
    SOL_MIR_RUNTIME_IMPORT_HOST,
    SOL_MIR_RUNTIME_IMPORT_RECIPE_CREATE,
    SOL_MIR_RUNTIME_IMPORT_RECIPE_COPY,
    SOL_MIR_RUNTIME_IMPORT_RECIPE_DROP,
    SOL_MIR_RUNTIME_IMPORT_RECIPE_EQUAL,
    SOL_MIR_RUNTIME_IMPORT_RECIPE_BOUND_ENVIRONMENT,
} SolMirRuntimeImportKind;

typedef struct {
    SolMirRuntimeImportKind kind;
    SolMirLinkageHostRequirementId host;
    SolMirRecipeId recipe;
    uint32_t recipe_operation;
    SolMirLinkageDigest identity;
    SolMirLinkageSymbol symbol;
} SolMirRuntimeImport;

typedef enum {
    SOL_MIR_RUNTIME_FAILURE_NONE = 0,
    SOL_MIR_RUNTIME_FAILURE_PANIC = 1,
    SOL_MIR_RUNTIME_FAILURE_INTEGER_OVERFLOW = 2,
    SOL_MIR_RUNTIME_FAILURE_DIVISION_BY_ZERO = 3,
    SOL_MIR_RUNTIME_FAILURE_ALLOCATION_FAILED = 4,
    SOL_MIR_RUNTIME_FAILURE_ALLOCATION_LIMIT = 5,
    SOL_MIR_RUNTIME_FAILURE_STEP_LIMIT = 6,
    SOL_MIR_RUNTIME_FAILURE_CALL_DEPTH_LIMIT = 7,
    SOL_MIR_RUNTIME_FAILURE_VALUE_LIMIT = 8,
    SOL_MIR_RUNTIME_FAILURE_TEXT_LIMIT = 9,
    SOL_MIR_RUNTIME_FAILURE_HOST_CALL_LIMIT = 10,
    SOL_MIR_RUNTIME_FAILURE_NO_MATCH = 11,
    SOL_MIR_RUNTIME_FAILURE_REACHED_UNREACHABLE = 12,
    SOL_MIR_RUNTIME_FAILURE_REQUIRE_VIOLATION = 13,
    SOL_MIR_RUNTIME_FAILURE_ENSURE_VIOLATION = 14,
    SOL_MIR_RUNTIME_FAILURE_REFINEMENT_VIOLATION = 15,
    SOL_MIR_RUNTIME_FAILURE_HOST_ERROR = 16,
} SolMirRuntimeFailureCode;

typedef enum {
    SOL_MIR_RUNTIME_FAILURE_DETAIL_NONE,
    SOL_MIR_RUNTIME_FAILURE_DETAIL_PANIC_TEXT,
    SOL_MIR_RUNTIME_FAILURE_DETAIL_HOST_BYTES,
} SolMirRuntimeFailureDetailKind;

typedef struct {
    SolMirRuntimeFailureCode code;
    SolMirRuntimeSource source;
    SolMirRuntimeFailureDetailKind detail_kind;
    const uint8_t *bytes;
    size_t length;
} SolMirRuntimeFailureRecord;

typedef enum {
    SOL_MIR_RUNTIME_EXIT_APPLICATION,
    SOL_MIR_RUNTIME_EXIT_BOUNDARY_ERROR,
    SOL_MIR_RUNTIME_EXIT_RUNTIME_FAILURE,
} SolMirRuntimeExitKind;

typedef struct {
    SolMirRuntimeExitKind kind;
    int driver_status;
    uint8_t application_status;
    const char *boundary_code;
    SolMirRuntimeFailureCode failure_code;
} SolMirRuntimeExit;

typedef struct {
    size_t max_signatures, max_signature_slots, max_calls, max_operands;
    size_t max_writebacks, max_entries, max_imports, max_failure_sites;
    size_t max_owned_bytes, max_build_scratch_bytes, max_build_work;
    size_t max_validation_scratch_bytes, max_validation_work;
} SolMirRuntimeConventionsLimits;

typedef struct {
    size_t signatures, signature_slots, calls, operands, writebacks;
    size_t entries, imports, failure_sites;
    size_t owned_bytes, build_scratch_bytes, build_work;
    size_t validation_scratch_bytes, validation_work;
} SolMirRuntimeConventionsUsage;

#define SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(X) \
    X(signatures, SolMirRuntimeSignature, signature) \
    X(signature_slots, SolMirRuntimeSignatureSlot, signature_slot) \
    X(calls, SolMirRuntimeCall, call) \
    X(operands, SolMirRuntimeOperand, operand) \
    X(writebacks, SolMirRuntimeWriteback, writeback) \
    X(entries, SolMirRuntimeEntry, entry) \
    X(imports, SolMirRuntimeImport, import) \
    X(failure_sites, SolMirRuntimeFailureSite, failure_site)

/* Unstable target-neutral post-P2 owner. concrete is borrowed and must outlive
   this address-stable owner. Dense IDs are local to this owner. */
typedef struct {
    const SolMirConcreteProgram *concrete;
#define SOL_MIR_RUNTIME_MEMBER(member, type, singular) \
    type *member; size_t singular##_count, singular##_capacity;
    SOL_MIR_RUNTIME_CONVENTIONS_ARENAS(SOL_MIR_RUNTIME_MEMBER)
#undef SOL_MIR_RUNTIME_MEMBER
    SolMirRuntimeConventionsLimits limits;
    SolMirRuntimeConventionsUsage usage;
} SolMirRuntimeConventions;

typedef struct {
    const SolMirConcreteProgram *concrete;
    /* NULL or wholly zero selects defaults. Partial zero is invalid. */
    const SolMirRuntimeConventionsLimits *limits;
} SolMirRuntimeConventionsBuildRequest;

typedef enum {
    SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SUCCEEDED,
    SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_ARGUMENT,
    SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INVALID_CONCRETE_PROGRAM,
    SOL_MIR_RUNTIME_CONVENTIONS_BUILD_UNSUPPORTED_CALLABLE,
    SOL_MIR_RUNTIME_CONVENTIONS_BUILD_SYMBOL_COLLISION,
    SOL_MIR_RUNTIME_CONVENTIONS_BUILD_RESOURCE_EXHAUSTED,
    SOL_MIR_RUNTIME_CONVENTIONS_BUILD_ALLOCATION_FAILED,
    SOL_MIR_RUNTIME_CONVENTIONS_BUILD_INTERNAL_FAILED,
} SolMirRuntimeConventionsBuildOutcome;

void sol_mir_runtime_conventions_init(SolMirRuntimeConventions *conventions);
void sol_mir_runtime_conventions_free(SolMirRuntimeConventions *conventions);
SolMirRuntimeConventionsLimits sol_mir_runtime_conventions_default_limits(void);
SolMirRuntimeConventionsBuildOutcome sol_mir_runtime_conventions_build(
    const SolMirRuntimeConventionsBuildRequest *request,
    SolMirRuntimeConventions *conventions,
    SolDiagnostics *diagnostics
);
bool sol_mir_runtime_conventions_validate(
    const SolMirRuntimeConventions *conventions,
    SolDiagnostics *diagnostics
);
bool sol_mir_runtime_conventions_render(
    FILE *stream,
    const SolMirRuntimeConventions *conventions
);

const char *sol_mir_runtime_failure_name(SolMirRuntimeFailureCode code);
bool sol_mir_runtime_failure_record_validate(
    const SolMirRuntimeConventions *conventions,
    const SolMirRuntimeFailureRecord *record
);
/* Failure validation and exit mapping require an immutable owner returned by
   build or accepted by full validation. They do not revalidate predecessors. */
bool sol_mir_runtime_exit_map(
    const SolMirRuntimeConventions *conventions,
    SolMirRuntimeEntryId entry,
    int64_t result,
    const SolMirRuntimeFailureRecord *failure,
    SolMirRuntimeExit *exit
);

#ifdef SOL_MIR_PLAN_TEST_HOOKS
typedef enum {
    SOL_MIR_RUNTIME_BUILD_TEST_HASH,
    SOL_MIR_RUNTIME_BUILD_TEST_TARGET,
    SOL_MIR_RUNTIME_BUILD_TEST_DIGEST,
    SOL_MIR_RUNTIME_BUILD_TEST_SORT,
    SOL_MIR_RUNTIME_BUILD_TEST_CATEGORY_COUNT,
} SolMirRuntimeBuildTestCategory;
void sol_mir_runtime_conventions_test_force_host_collision(bool force);
void sol_mir_runtime_conventions_test_force_recipe_collision(bool force);
void sol_mir_runtime_conventions_test_force_host_symbol_collision(bool force);
void sol_mir_runtime_conventions_test_force_recipe_symbol_collision(bool force);
void sol_mir_runtime_conventions_test_force_allocation_failure(bool force);
size_t sol_mir_runtime_conventions_test_observed_validation_work(void);
bool sol_mir_runtime_conventions_test_reconstruct_usage(
    const SolMirConcreteProgram *concrete,
    const SolMirRuntimeConventionsLimits *limits,
    SolMirRuntimeConventionsUsage *usage
);
void sol_mir_runtime_conventions_test_force_build_category(
    SolMirRuntimeBuildTestCategory category);
size_t sol_mir_runtime_conventions_test_build_category_attempts(
    SolMirRuntimeBuildTestCategory category);
size_t sol_mir_runtime_conventions_test_build_events_after_exhaustion(void);
SolMirRuntimeConventionsBuildOutcome
sol_mir_runtime_conventions_test_classify_call_kind(SolIrCallKind kind);
SolMirRuntimeResultClass
sol_mir_runtime_conventions_test_classify_result_kind(SolMirRecipeKind kind);
bool sol_mir_runtime_conventions_test_indirect_target_valid(
    const SolMirRuntimeCall *call,
    size_t table,
    size_t table_count
);
bool sol_mir_runtime_conventions_test_build_call_failure_mask(
    const SolMirRuntimeCall *call,
    const SolMirLinkage *linkage,
    uint32_t *mask
);
bool sol_mir_runtime_conventions_test_validate_call_failure_mask(
    const SolMirRuntimeCall *call,
    const SolMirLinkage *linkage,
    uint32_t *mask
);
#endif

#endif
