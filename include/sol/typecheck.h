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
} SolTypeKind;

typedef struct {
    SolTypeKind kind;
    SolDefId definition;
} SolType;

typedef enum {
    SOL_TYPE_CONSTRUCTOR_OPTION,
    SOL_TYPE_CONSTRUCTOR_RESULT,
} SolTypeConstructor;

typedef struct {
    SolTypeConstructor constructor;
    SolType arguments[2];
    size_t argument_count;
} SolTypeApplication;

typedef struct {
    SolType *parameters;
    size_t parameter_count;
    SolType result;
    SolEffectSet effects;
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
} SolTypeTable;

void sol_type_table_init(SolTypeTable *table);
void sol_type_table_free(SolTypeTable *table);
const SolTypeApplication *sol_type_application(
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
