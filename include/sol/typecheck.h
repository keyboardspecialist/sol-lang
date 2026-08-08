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
    SOL_TYPE_OPAQUE,
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

typedef struct {
    SolCapabilityMemberId source_member;
    SolCapabilityMemberId provider_member;
    SolParameterId root;
} SolHandler;

typedef struct {
    SolType *expressions;
    size_t expression_count;
    /* Indexed by SolExprId; SOL_AST_NONE means no capability parameter origin. */
    SolParameterId *expression_capability_origins;
    SolParameterId *expression_operation_origins;
    SolType *locals;
    size_t local_count;
    /* Indexed by SolLocalId; SOL_AST_NONE means no capability parameter origin. */
    SolParameterId *local_capability_origins;
    SolParameterId *local_operation_origins;
    SolType *definitions;
    size_t definition_count;
    SolType *declared_types;
    size_t declared_type_count;
    SolFunctionType *function_types;
    size_t function_type_count;
    size_t function_type_capacity;
    SolFunctionCoercion *function_coercions;
    size_t function_coercion_count;
    size_t function_coercion_capacity;
    /* Indexed by SolExprId; non-handler entries contain SOL_AST_NONE fields. */
    SolHandler *handlers;
    size_t handler_count;
} SolTypeTable;

void sol_type_table_init(SolTypeTable *table);
void sol_type_table_free(SolTypeTable *table);
bool sol_type_check(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    SolTypeTable *types,
    SolDiagnostics *diagnostics
);

#endif
