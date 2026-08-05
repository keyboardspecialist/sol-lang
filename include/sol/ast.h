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
    SOL_EXPR_BLOCK,
    SOL_EXPR_PROPAGATE,
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
            SolStatementId first_statement;
        } block;
        SolExprId propagated;
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

#endif
