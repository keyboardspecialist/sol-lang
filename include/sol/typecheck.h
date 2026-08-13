#ifndef SOL_TYPECHECK_H
#define SOL_TYPECHECK_H

#include "sol/diagnostic.h"
#include "sol/effect.h"
#include "sol/hir.h"
#include "sol/parser.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SOL_TYPE_UNKNOWN,
    SOL_TYPE_ERROR,
    SOL_TYPE_INT64,
    SOL_TYPE_BOOL,
    SOL_TYPE_TEXT,
    SOL_TYPE_UNIT,
    SOL_TYPE_NOMINAL,
    SOL_TYPE_APPLICATION,
    SOL_TYPE_FUNCTION,
    SOL_TYPE_FUNCTION_SIGNATURE,
    SOL_TYPE_CAPABILITY_OPERATION,
    SOL_TYPE_VARIANT,
    SOL_TYPE_NEVER,
    SOL_TYPE_PARAMETER,
    SOL_TYPE_SELF,
    SOL_TYPE_TRAIT_METHOD,
} SolTypeKind;

typedef struct {
    SolTypeKind kind;
    SolDefId definition;
} SolType;

typedef enum {
    SOL_TYPE_CONSTRUCTOR_OPTION,
    SOL_TYPE_CONSTRUCTOR_RESULT,
    SOL_TYPE_CONSTRUCTOR_USER,
} SolTypeConstructor;

typedef struct {
    SolTypeConstructor constructor;
    /* SOL_AST_NONE for builtins; otherwise the record/enum SolDefId. */
    SolDefId definition;
    size_t argument_offset;
    size_t argument_count;
} SolTypeApplication;

typedef struct {
    SolDefId function;
    size_t argument_offset;
    size_t argument_count;
} SolCallInstantiation;

typedef struct {
    SolVariantId variant;
    SolType owner;
} SolVariantConstructor;

typedef struct {
    SolTypeDeclarationFlavor flavor;
    SolType representation;
} SolTypeRepresentation;

typedef struct {
    SolDefId definition;
    SolType representation;
    SolType result;
} SolTypeConstruction;

typedef struct {
    SolType *parameters;
    size_t parameter_count;
    SolType result;
    SolEffectSet effects;
    /* SOL_AST_NONE for a closed row; otherwise declaration-owned identity. */
    SolEffectParameterId effect_parameter;
} SolFunctionType;

typedef struct {
    SolExprId expression;
    SolType expected;
} SolFunctionCoercion;

typedef size_t SolProvenanceId;

#define SOL_PROVENANCE_NONE SIZE_MAX

typedef struct {
    size_t root_offset;
    size_t root_count;
} SolProvenanceSet;

typedef struct {
    const SolParameterId *roots;
    size_t count;
} SolProvenance;

typedef struct {
    SolCapabilityMemberId source_member;
    SolCapabilityMemberId provider_member;
    SolParameterId root;
} SolHandler;

typedef enum {
    SOL_METHOD_RESOLUTION_NONE,
    SOL_METHOD_RESOLUTION_REQUIREMENT,
    SOL_METHOD_RESOLUTION_IMPLEMENTATION,
} SolMethodResolutionKind;

typedef struct {
    SolMethodResolutionKind kind;
    SolExprId call;
    SolDefId trait;
    SolTraitMethodId requirement;
    SolDefId implementation;
    SolTraitMethodId method;
} SolMethodResolution;

typedef struct {
    SolType *expressions;
    size_t expression_count;
    /* Indexed by SolExprId; SOL_PROVENANCE_NONE means no known provenance. */
    SolProvenanceId *expression_capability_origins;
    SolProvenanceId *expression_operation_origins;
    SolType *locals;
    size_t local_count;
    /* Indexed by SolLocalId; SOL_PROVENANCE_NONE means no known provenance. */
    SolProvenanceId *local_capability_origins;
    SolProvenanceId *local_operation_origins;
    SolType *definitions;
    size_t definition_count;
    SolType *declared_types;
    size_t declared_type_count;
    SolTypeApplication *type_applications;
    size_t type_application_count;
    size_t type_application_capacity;
    SolType *type_application_arguments;
    size_t type_application_argument_count;
    size_t type_application_argument_capacity;
    SolFunctionType *function_types;
    size_t function_type_count;
    size_t function_type_capacity;
    SolFunctionCoercion *function_coercions;
    size_t function_coercion_count;
    size_t function_coercion_capacity;
    /* Interned normalized finite nonempty sets of lexical capability roots. */
    SolProvenanceSet *provenances;
    size_t provenance_count;
    size_t provenance_capacity;
    SolParameterId *provenance_roots;
    size_t provenance_root_count;
    size_t provenance_root_capacity;
    /* Indexed by SolExprId; non-handler entries contain SOL_AST_NONE fields. */
    SolHandler *handlers;
    size_t handler_count;
    /* Indexed by SolExprId; non-call entries have function == SOL_AST_NONE. */
    SolCallInstantiation *call_instantiations;
    size_t call_instantiation_count;
    SolType *call_instantiation_arguments;
    size_t call_instantiation_argument_count;
    size_t call_instantiation_argument_capacity;
    SolVariantConstructor *variant_constructors;
    size_t variant_constructor_count;
    size_t variant_constructor_capacity;
    /* Indexed by SolExprId; non-method calls use SOL_METHOD_RESOLUTION_NONE. */
    SolMethodResolution *method_resolutions;
    size_t method_resolution_count;
    /* Indexed by SolDefId; only implementation entries are meaningful. */
    SolType *implementation_targets;
    size_t implementation_target_count;
    /* Indexed by SolDefId; non-type entries use SOL_TYPE_DECLARATION_NONE. */
    SolTypeRepresentation *representations;
    size_t representation_count;
    /* Indexed by SolExprId; non-construction entries have definition == SOL_AST_NONE. */
    SolTypeConstruction *constructions;
    size_t construction_count;
} SolTypeTable;

const SolMethodResolution *sol_type_method_resolution(
    const SolTypeTable *table,
    SolExprId call
);

void sol_type_table_init(SolTypeTable *table);
void sol_type_table_free(SolTypeTable *table);
const SolTypeApplication *sol_type_application(
    const SolTypeTable *table,
    SolType type
);
bool sol_type_application_arguments(
    const SolTypeTable *table,
    SolType type,
    const SolType **arguments,
    size_t *count
);
const SolCallInstantiation *sol_type_call_instantiation(
    const SolTypeTable *table,
    SolExprId expression
);
bool sol_type_call_instantiation_arguments(
    const SolTypeTable *table,
    SolExprId expression,
    const SolType **arguments,
    size_t *count
);
bool sol_type_call_instantiation_valid(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolTypeTable *table,
    SolExprId expression
);
const SolVariantConstructor *sol_type_variant_constructor(
    const SolTypeTable *table,
    SolType type
);
const SolTypeRepresentation *sol_type_representation(
    const SolTypeTable *table,
    SolDefId definition
);
const SolTypeConstruction *sol_type_construction(
    const SolTypeTable *table,
    SolExprId expression
);
bool sol_type_exact_reference_valid(
    const SolSyntaxTree *syntax,
    const SolTypeTable *table,
    SolType type
);
bool sol_type_provenance(
    const SolTypeTable *table,
    SolProvenanceId id,
    SolProvenance *provenance
);
bool sol_type_check(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    SolTypeTable *types,
    SolDiagnostics *diagnostics
);

#endif
