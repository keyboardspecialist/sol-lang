#ifndef SOL_IR_H
#define SOL_IR_H

#include "sol/contract.h"
#include "sol/package.h"

#include <stdbool.h>
#include <stdint.h>

/* Explicitly unstable compiler-internal interpreter IR; not a serialized ABI. */
typedef size_t SolIrTypeId;
typedef size_t SolIrDefinitionId;
typedef size_t SolIrCallableId;
typedef size_t SolIrLocalId;
typedef size_t SolIrFieldId;
typedef size_t SolIrVariantId;
typedef size_t SolIrExpressionId;
typedef size_t SolIrStatementId;
typedef size_t SolIrArmId;
typedef size_t SolIrSnapshotId;
typedef size_t SolIrMemberId;
typedef size_t SolIrEvidenceId;
typedef size_t SolIrGenericParameterId;
typedef size_t SolIrEffectParameterId;

#define SOL_IR_NONE SIZE_MAX

typedef struct {
    size_t offset;
    size_t count;
} SolIrSlice;

typedef enum {
    SOL_IR_TYPE_INT64,
    SOL_IR_TYPE_BOOL,
    SOL_IR_TYPE_TEXT,
    SOL_IR_TYPE_UNIT,
    SOL_IR_TYPE_NEVER,
    SOL_IR_TYPE_NOMINAL,
    SOL_IR_TYPE_OPTION,
    SOL_IR_TYPE_RESULT,
    SOL_IR_TYPE_FUNCTION,
    SOL_IR_TYPE_PARAMETER,
    SOL_IR_TYPE_SELF,
} SolIrTypeKind;

typedef struct {
    SolIrTypeKind kind;
    SolIrDefinitionId definition;
    size_t argument_offset;
    size_t argument_count;
    size_t parameter_offset;
    size_t parameter_count;
    SolIrTypeId result;
    SolIrSlice effects;
} SolIrType;

typedef enum {
    SOL_IR_AUTHORITY_NONE,
    SOL_IR_AUTHORITY_LOCAL,
    SOL_IR_AUTHORITY_SELF,
} SolIrAuthorityKind;

typedef struct {
    char *name;
    SolIrAuthorityKind authority_kind;
    SolIrLocalId authority;
} SolIrEffect;

typedef struct {
    SolIrDefinitionId owner;
    char *name;
    size_t ordinal;
    SolIrDefinitionId trait_bound;
} SolIrGenericParameter;

typedef struct {
    SolIrDefinitionId owner;
    char *name;
    size_t ordinal;
} SolIrEffectParameter;

typedef enum {
    SOL_IR_DEFINITION_RECORD,
    SOL_IR_DEFINITION_ENUM,
    SOL_IR_DEFINITION_DISTINCT,
    SOL_IR_DEFINITION_REFINED,
    SOL_IR_DEFINITION_CAPABILITY,
    SOL_IR_DEFINITION_FUNCTION,
    SOL_IR_DEFINITION_TRAIT,
    SOL_IR_DEFINITION_IMPLEMENTATION,
    SOL_IR_DEFINITION_TEST,
} SolIrDefinitionKind;

typedef struct {
    SolIrDefinitionKind kind;
    SolSemanticId semantic_id;
    char *name;
    SolSpan span;
    SolIrCallableId callable;
    SolIrTypeId declared_type;
    SolIrTypeId representation;
    SolIrDefinitionId implementation_trait;
    SolIrTypeId implementation_target;
    SolIrSlice fields;
    SolIrSlice variants;
    SolIrSlice members;
    SolIrSlice generic_parameters;
    SolIrSlice effect_parameters;
    SolIrLocalId capability_source;
} SolIrDefinition;

typedef enum {
    SOL_IR_CALLABLE_FUNCTION,
    SOL_IR_CALLABLE_CAPABILITY,
    SOL_IR_CALLABLE_TRAIT_REQUIREMENT,
    SOL_IR_CALLABLE_TRAIT_IMPLEMENTATION,
    SOL_IR_CALLABLE_TEST,
} SolIrCallableKind;

typedef struct {
    SolIrCallableKind kind;
    SolIrDefinitionId owner;
    char *name;
    SolSpan span;
    SolIrSlice parameters;
    SolIrTypeId result;
    SolIrExpressionId body;
    SolIrSlice effects;
    SolIrSlice generic_parameters;
    SolIrSlice effect_parameters;
    SolIrLocalId receiver;
    SolIrLocalId capability_source;
    SolIrAuthorityKind result_authority_kind;
    SolIrLocalId result_authority;
} SolIrCallable;

typedef struct {
    SolIrCallableId callable;
} SolIrMember;

typedef struct {
    SolIrDefinitionId trait;
    SolIrCallableId requirement;
    SolIrDefinitionId implementation;
    SolIrCallableId method;
    SolIrTypeId type;
    /* Invocation binding target; SOL_IR_NONE for immediate method evidence. */
    SolIrGenericParameterId binding;
    /* Caller parameter used when forwarded; otherwise SOL_IR_NONE. */
    SolIrGenericParameterId parameter;
    bool forwarded;
} SolIrDispatchEvidence;

typedef enum {
    SOL_IR_LOCAL_PARAMETER,
    SOL_IR_LOCAL_BINDING,
    SOL_IR_LOCAL_PATTERN,
} SolIrLocalKind;

typedef struct {
    SolIrLocalKind kind;
    SolIrDefinitionId owner;
    char *name;
    SolIrTypeId type;
    SolIrSlice capability_roots;
    SolIrSlice operation_roots;
} SolIrLocal;

typedef struct {
    SolIrDefinitionId owner;
    char *name;
    SolIrTypeId type;
} SolIrField;

typedef struct {
    SolIrDefinitionId owner;
    char *name;
    SolIrSlice fields;
} SolIrVariant;

typedef enum {
    SOL_IR_EXPR_INTEGER,
    SOL_IR_EXPR_STRING,
    SOL_IR_EXPR_BOOL,
    SOL_IR_EXPR_UNIT,
    SOL_IR_EXPR_LOCAL,
    SOL_IR_EXPR_DEFINITION,
    SOL_IR_EXPR_REFINEMENT_SELF,
    SOL_IR_EXPR_UNARY,
    SOL_IR_EXPR_BINARY,
    SOL_IR_EXPR_CALL,
    SOL_IR_EXPR_RECORD,
    SOL_IR_EXPR_VARIANT,
    SOL_IR_EXPR_FIELD,
    SOL_IR_EXPR_IF,
    SOL_IR_EXPR_MATCH,
    SOL_IR_EXPR_BLOCK,
    SOL_IR_EXPR_PROPAGATE,
    SOL_IR_EXPR_HANDLE,
    SOL_IR_EXPR_RESULT,
    SOL_IR_EXPR_SNAPSHOT_READ,
    SOL_IR_EXPR_COMPILE_TIME_HEAD,
    SOL_IR_EXPR_BOUND_OPERATION,
} SolIrExpressionKind;

typedef enum {
    SOL_IR_CALL_FUNCTION,
    SOL_IR_CALL_CALLBACK,
    SOL_IR_CALL_CAPABILITY,
    SOL_IR_CALL_METHOD,
    SOL_IR_CALL_BUILTIN_OK,
    SOL_IR_CALL_BUILTIN_ERR,
    SOL_IR_CALL_BUILTIN_SOME,
    SOL_IR_CALL_BUILTIN_NONE,
    SOL_IR_CALL_ENUM_CONSTRUCTOR,
    SOL_IR_CALL_DISTINCT_CONSTRUCTOR,
} SolIrCallKind;

typedef enum {
    SOL_IR_PROPAGATE_OPTION,
    SOL_IR_PROPAGATE_RESULT,
} SolIrPropagationKind;

typedef struct {
    size_t formal;
    SolIrExpressionId value;
} SolIrOperand;

typedef struct {
    SolIrExpressionKind kind;
    SolSpan span;
    SolIrTypeId type;
    SolIrSlice capability_roots;
    SolIrSlice operation_roots;
    union {
        int64_t integer;
        char *string;
        bool boolean;
        SolIrLocalId local;
        SolIrDefinitionId definition;
        struct { SolTokenKind operator_kind; SolIrExpressionId operand; } unary;
        struct {
            SolIrExpressionId left;
            SolTokenKind operator_kind;
            SolIrExpressionId right;
        } binary;
        struct {
            SolIrCallKind kind;
            SolIrCallableId callable;
            SolIrExpressionId callee;
            SolIrExpressionId receiver;
            SolIrVariantId variant;
            SolIrDefinitionId definition;
            SolIrSlice operands;
            SolIrSlice type_arguments;
            SolIrSlice effects;
            SolIrSlice evidence;
        } call;
        struct { SolIrDefinitionId definition; SolIrSlice fields; } record;
        struct { SolIrVariantId variant; } variant;
        struct { SolIrExpressionId base; SolIrFieldId field; } field;
        struct { SolIrExpressionId receiver; SolIrCallableId callable; } operation;
        struct {
            SolIrExpressionId condition;
            SolIrExpressionId then_branch;
            SolIrExpressionId else_branch;
        } if_expr;
        struct { SolIrExpressionId scrutinee; SolIrSlice arms; } match_expr;
        SolIrSlice block;
        struct {
            SolIrPropagationKind kind;
            SolIrExpressionId operand;
            SolIrTypeId residual;
        } propagate;
        struct {
            char *effect_name;
            SolIrExpressionId authority;
            SolIrExpressionId provider;
            SolIrExpressionId body;
            SolIrCallableId source;
            SolIrCallableId provider_callable;
            SolIrLocalId root;
        } handler;
        SolIrSnapshotId snapshot;
    } as;
} SolIrExpression;

typedef enum {
    SOL_IR_STATEMENT_LET,
    SOL_IR_STATEMENT_RETURN,
    SOL_IR_STATEMENT_EXPRESSION,
} SolIrStatementKind;

typedef struct {
    SolIrStatementKind kind;
    SolSpan span;
    SolIrLocalId local;
    SolIrExpressionId expression;
} SolIrStatement;

typedef enum {
    SOL_IR_PATTERN_WILDCARD,
    SOL_IR_PATTERN_BOOL,
    SOL_IR_PATTERN_VARIANT,
} SolIrPatternKind;

typedef struct {
    SolIrPatternKind kind;
    bool boolean;
    SolIrVariantId variant;
    SolIrSlice bindings;
    SolIrExpressionId value;
    SolSpan span;
} SolIrArm;

typedef struct {
    SolObligationId id;
    SolContractOwnerKind owner_kind;
    size_t owner;
    SolContractClauseKind kind;
    SolContractOutcomeKind outcome;
    SolIrExpressionId predicate;
    bool result_available;
    SolIrTypeId result_type;
    SolIrSlice snapshots;
} SolIrObligation;

typedef struct {
    SolIrSnapshotId id;
    SolObligationId obligation;
    SolIrExpressionId read;
    SolIrExpressionId operand;
    SolIrTypeId type;
} SolIrSnapshot;

typedef struct {
    char *path;
    size_t aggregate_start;
    size_t aggregate_end;
} SolIrSourceFile;

typedef struct {
    char *source_path;
    char *source_bytes;
    size_t source_length;
    SolIrType *types;
    size_t type_count;
    SolIrTypeId *type_ids;
    size_t type_id_count;
    SolIrDefinition *definitions;
    size_t definition_count;
    SolIrCallable *callables;
    size_t callable_count;
    SolIrMember *members;
    size_t member_count;
    SolIrDispatchEvidence *evidence;
    size_t evidence_count;
    SolIrLocal *locals;
    size_t local_count;
    SolIrField *fields;
    size_t field_count;
    SolIrVariant *variants;
    size_t variant_count;
    SolIrExpression *expressions;
    size_t expression_count;
    SolIrStatement *statements;
    size_t statement_count;
    SolIrStatementId *statement_ids;
    size_t statement_id_count;
    SolIrArm *arms;
    size_t arm_count;
    SolIrArmId *arm_ids;
    size_t arm_id_count;
    SolIrOperand *operands;
    size_t operand_count;
    SolIrLocalId *roots;
    size_t root_count;
    SolIrEffect *effects;
    size_t effect_count;
    SolIrGenericParameter *generic_parameters;
    size_t generic_parameter_count;
    SolIrEffectParameter *effect_parameters;
    size_t effect_parameter_count;
    SolIrObligation *obligations;
    size_t obligation_count;
    SolIrSnapshot *snapshots;
    size_t snapshot_count;
    SolIrSourceFile *files;
    size_t file_count;
} SolIr;

void sol_ir_init(SolIr *ir);
void sol_ir_free(SolIr *ir);
bool sol_ir_lower(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    const SolEffectTable *effects,
    const SolContractTable *contracts,
    SolIr *ir,
    SolDiagnostics *diagnostics
);
bool sol_ir_lower_scoped(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    const SolEffectTable *effects,
    const SolContractTable *contracts,
    const SolPackageFile *files,
    size_t file_count,
    SolIr *ir,
    SolDiagnostics *diagnostics
);
bool sol_ir_validate(const SolIr *ir, SolDiagnostics *diagnostics);

#endif
