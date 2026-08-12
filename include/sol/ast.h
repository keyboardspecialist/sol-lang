#ifndef SOL_AST_H
#define SOL_AST_H

#include "sol/source.h"
#include "sol/token.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef size_t SolExprId;
typedef size_t SolStatementId;
typedef size_t SolArgumentId;
typedef size_t SolParameterId;
typedef size_t SolTypeId;
typedef size_t SolTypeArgumentId;
typedef size_t SolTypeParameterId;
typedef size_t SolEffectParameterId;
typedef size_t SolFieldId;
typedef size_t SolVariantId;
typedef size_t SolPatternId;
typedef size_t SolPatternBindingId;
typedef size_t SolMatchArmId;
typedef size_t SolEffectId;
typedef size_t SolCapabilityMemberId;
typedef size_t SolTraitMethodId;
typedef size_t SolContractClauseId;
typedef size_t SolContractConditionId;

#define SOL_AST_NONE SIZE_MAX

typedef enum {
    SOL_EXPR_ERROR,
    SOL_EXPR_INTEGER,
    SOL_EXPR_STRING,
    SOL_EXPR_BOOL,
    SOL_EXPR_UNIT,
    SOL_EXPR_PATH,
    SOL_EXPR_UNARY,
    SOL_EXPR_BINARY,
    SOL_EXPR_CALL,
    SOL_EXPR_FIELD,
    SOL_EXPR_RECORD,
    SOL_EXPR_IF,
    SOL_EXPR_MATCH,
    SOL_EXPR_BLOCK,
    SOL_EXPR_PROPAGATE,
    SOL_EXPR_HANDLE,
    SOL_EXPR_RESULT,
    SOL_EXPR_OLD,
    SOL_EXPR_TYPE_APPLICATION,
} SolExprKind;

typedef struct {
    SolExprKind kind;
    SolSpan span;
    union {
        bool bool_value;
        SolSpan name;
        struct {
            SolTokenKind operator_kind;
            SolExprId operand;
        } unary;
        struct {
            SolExprId left;
            SolTokenKind operator_kind;
            SolExprId right;
        } binary;
        struct {
            SolExprId callee;
            SolArgumentId first_argument;
        } call;
        struct {
            SolExprId base;
            SolTypeArgumentId first_argument;
        } type_application;
        struct {
            SolExprId base;
            SolSpan name;
        } field;
        struct {
            SolExprId type;
            SolArgumentId first_field;
        } record;
        struct {
            SolExprId condition;
            SolExprId then_branch;
            SolExprId else_branch;
        } if_expr;
        struct {
            SolExprId scrutinee;
            SolMatchArmId first_arm;
        } match_expr;
        struct {
            SolStatementId first_statement;
        } block;
        SolExprId propagated;
        SolExprId old_expression;
        struct {
            SolSpan effect_name;
            SolExprId authority;
            SolExprId provider;
            SolExprId body;
        } handle;
    } as;
} SolExpr;

typedef struct {
    SolSpan name;
    SolExprId value;
    SolArgumentId next;
    bool is_named;
} SolArgument;

typedef enum {
    SOL_STATEMENT_LET,
    SOL_STATEMENT_RETURN,
    SOL_STATEMENT_EXPRESSION,
} SolStatementKind;

typedef struct {
    SolStatementKind kind;
    SolSpan span;
    SolStatementId next;
    union {
        struct {
            SolSpan name;
            SolExprId value;
        } let_statement;
        SolExprId expression;
    } as;
} SolStatement;

typedef struct {
    SolSpan name;
    SolSpan type;
    SolTypeId type_id;
    SolParameterId next;
} SolParameter;

typedef enum {
    SOL_SYNTAX_TYPE_PATH,
    SOL_SYNTAX_TYPE_UNIT,
    SOL_SYNTAX_TYPE_FUNCTION,
} SolSyntaxTypeKind;

typedef struct {
    SolSyntaxTypeKind kind;
    SolSpan span;
    SolSpan name;
    SolTypeArgumentId first_argument;
    SolTypeId return_type;
    SolEffectId first_effect;
    /* Open callback-row tail spelling; resolved declaration-owned in HIR. */
    SolSpan effect_tail;
    bool has_effect_tail;
    bool is_capability;
    size_t owner_item;
} SolSyntaxType;

typedef enum {
    SOL_EFFECT_OWNER_ITEM,
    SOL_EFFECT_OWNER_CAPABILITY_MEMBER,
    SOL_EFFECT_OWNER_TRAIT_METHOD,
    SOL_EFFECT_OWNER_TYPE,
} SolEffectOwnerKind;

typedef struct {
    SolTypeId type;
    SolTypeArgumentId next;
} SolTypeArgument;

typedef struct {
    SolSpan name;
    SolSpan bound;
    SolTypeParameterId next;
    size_t owner_item;
} SolTypeParameter;

typedef struct {
    SolSpan name;
    SolEffectParameterId next;
    size_t owner_item;
} SolEffectParameter;

typedef struct {
    SolSpan name;
    SolSpan span;
    SolTypeId type;
    SolFieldId next;
} SolField;

typedef struct {
    SolSpan name;
    SolSpan span;
    SolFieldId first_field;
    SolVariantId next;
    size_t owner_item;
} SolVariant;

typedef enum {
    SOL_PATTERN_WILDCARD,
    SOL_PATTERN_VARIANT,
    SOL_PATTERN_BOOL,
} SolPatternKind;

typedef struct {
    SolPatternKind kind;
    SolSpan span;
    SolSpan name;
    SolPatternBindingId first_binding;
    bool bool_value;
} SolPattern;

typedef struct {
    SolSpan name;
    SolPatternBindingId next;
} SolPatternBinding;

typedef struct {
    SolPatternId pattern;
    SolExprId value;
    SolSpan span;
    SolMatchArmId next;
} SolMatchArm;

typedef struct {
    SolSpan name;
    SolSpan argument;
    SolSpan span;
    SolEffectId next;
    SolEffectOwnerKind owner_kind;
    size_t owner;
    bool is_pure;
    bool has_argument;
} SolEffect;

typedef enum {
    SOL_CONTRACT_OWNER_ITEM,
    SOL_CONTRACT_OWNER_CAPABILITY_MEMBER,
} SolContractOwnerKind;

typedef enum {
    SOL_CONTRACT_REQUIRES,
    SOL_CONTRACT_ENSURES,
} SolContractClauseKind;

typedef enum {
    SOL_CONTRACT_OUTCOME_ALWAYS,
    SOL_CONTRACT_OUTCOME_SUCCESS,
    SOL_CONTRACT_OUTCOME_FAILURE,
} SolContractOutcomeKind;

typedef struct {
    SolContractClauseKind kind;
    SolSpan span;
    SolContractConditionId first_condition;
    SolContractClauseId next;
    SolContractOwnerKind owner_kind;
    size_t owner;
} SolContractClause;

typedef struct {
    SolContractOutcomeKind outcome;
    SolSpan span;
    SolExprId expression;
    SolContractConditionId next;
    SolContractClauseId owner_clause;
} SolContractCondition;

typedef struct {
    SolSpan name;
    SolSpan span;
    SolParameterId first_parameter;
    SolSpan return_type;
    SolTypeId return_type_id;
    SolEffectId first_effect;
    SolContractClauseId first_contract;
    SolExprId body;
    SolCapabilityMemberId next;
    size_t owner_item;
    bool has_effect_clause;
    bool result_authority_from_self;
} SolCapabilityMember;

typedef struct {
    SolSpan name;
    SolSpan span;
    SolParameterId first_parameter;
    SolSpan return_type;
    SolTypeId return_type_id;
    SolEffectId first_effect;
    SolExprId body;
    SolTraitMethodId next;
    size_t owner_item;
    bool has_effect_clause;
} SolTraitMethod;

#endif
