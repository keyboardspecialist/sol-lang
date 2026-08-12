#include "sol/parser.h"

#include <stdint.h>
#include <stdlib.h>

typedef enum {
    SOL_EXPRESSION_CONTEXT_BODY,
    SOL_EXPRESSION_CONTEXT_REQUIRES,
    SOL_EXPRESSION_CONTEXT_ENSURES,
} SolExpressionContext;

typedef struct {
    const SolSource *source;
    const SolSyntaxTree *tree;
    unsigned char *expressions;
    unsigned char *arguments;
    unsigned char *statements;
    unsigned char *arms;
    unsigned char *clauses;
    unsigned char *conditions;
} SolStructureValidator;

static bool sol_structure_span_valid(const SolStructureValidator *validator, SolSpan span) {
    return span.start <= span.end && span.end <= validator->source->length;
}

static bool sol_structure_expression(
    SolStructureValidator *validator,
    SolExprId expression_id,
    SolExpressionContext context,
    size_t depth
);

static bool sol_structure_arguments(
    SolStructureValidator *validator,
    SolArgumentId argument,
    SolExpressionContext context,
    size_t depth
) {
    while (argument != SOL_AST_NONE) {
        if (argument >= validator->tree->argument_count || validator->arguments[argument]) {
            return false;
        }
        validator->arguments[argument] = 1;
        const SolArgument *current = &validator->tree->arguments[argument];
        if (!sol_structure_expression(validator, current->value, context, depth)) return false;
        argument = current->next;
    }
    return true;
}

static bool sol_structure_statements(
    SolStructureValidator *validator,
    SolStatementId statement,
    SolExpressionContext context,
    size_t depth
) {
    while (statement != SOL_AST_NONE) {
        if (statement >= validator->tree->statement_count || validator->statements[statement]) {
            return false;
        }
        validator->statements[statement] = 1;
        const SolStatement *current = &validator->tree->statements[statement];
        SolExprId value = current->kind == SOL_STATEMENT_LET
            ? current->as.let_statement.value
            : current->as.expression;
        if (!sol_structure_expression(validator, value, context, depth)) return false;
        statement = current->next;
    }
    return true;
}

static bool sol_structure_arms(
    SolStructureValidator *validator,
    SolMatchArmId arm,
    SolExpressionContext context,
    size_t depth
) {
    while (arm != SOL_AST_NONE) {
        if (arm >= validator->tree->match_arm_count || validator->arms[arm]) return false;
        validator->arms[arm] = 1;
        const SolMatchArm *current = &validator->tree->match_arms[arm];
        if (!sol_structure_expression(validator, current->value, context, depth)) return false;
        arm = current->next;
    }
    return true;
}

static bool sol_structure_expression(
    SolStructureValidator *validator,
    SolExprId expression_id,
    SolExpressionContext context,
    size_t depth
) {
    if (expression_id >= validator->tree->expression_count || depth >= 4096
        || validator->expressions[expression_id] != 0) {
        return false;
    }
    validator->expressions[expression_id] = 1;
    const SolExpr *expression = &validator->tree->expressions[expression_id];
    bool valid = (int)expression->kind >= 0
        && expression->kind <= SOL_EXPR_TYPE_APPLICATION
        && sol_structure_span_valid(validator, expression->span);
    switch (expression->kind) {
        case SOL_EXPR_UNARY:
            valid = valid && sol_structure_expression(
                validator,
                expression->as.unary.operand,
                context,
                depth + 1
            );
            break;
        case SOL_EXPR_BINARY:
            valid = valid
                && sol_structure_expression(
                    validator,
                    expression->as.binary.left,
                    context,
                    depth + 1
                )
                && sol_structure_expression(
                    validator,
                    expression->as.binary.right,
                    context,
                    depth + 1
                );
            break;
        case SOL_EXPR_CALL:
            valid = valid
                && sol_structure_expression(
                    validator,
                    expression->as.call.callee,
                    context,
                    depth + 1
                )
                && sol_structure_arguments(
                    validator,
                    expression->as.call.first_argument,
                    context,
                    depth + 1
                );
            break;
        case SOL_EXPR_TYPE_APPLICATION:
            valid = valid && sol_structure_expression(
                validator,
                expression->as.type_application.base,
                context,
                depth + 1
            );
            break;
        case SOL_EXPR_FIELD:
            valid = valid && sol_structure_expression(
                validator,
                expression->as.field.base,
                context,
                depth + 1
            );
            break;
        case SOL_EXPR_RECORD:
            valid = valid
                && sol_structure_expression(
                    validator,
                    expression->as.record.type,
                    context,
                    depth + 1
                )
                && sol_structure_arguments(
                    validator,
                    expression->as.record.first_field,
                    context,
                    depth + 1
                );
            break;
        case SOL_EXPR_IF:
            valid = valid
                && sol_structure_expression(
                    validator,
                    expression->as.if_expr.condition,
                    context,
                    depth + 1
                )
                && sol_structure_expression(
                    validator,
                    expression->as.if_expr.then_branch,
                    context,
                    depth + 1
                )
                && sol_structure_expression(
                    validator,
                    expression->as.if_expr.else_branch,
                    context,
                    depth + 1
                );
            break;
        case SOL_EXPR_MATCH:
            valid = valid
                && sol_structure_expression(
                    validator,
                    expression->as.match_expr.scrutinee,
                    context,
                    depth + 1
                )
                && sol_structure_arms(
                    validator,
                    expression->as.match_expr.first_arm,
                    context,
                    depth + 1
                );
            break;
        case SOL_EXPR_BLOCK:
            valid = valid && sol_structure_statements(
                validator,
                expression->as.block.first_statement,
                context,
                depth + 1
            );
            break;
        case SOL_EXPR_PROPAGATE:
            valid = valid && sol_structure_expression(
                validator,
                expression->as.propagated,
                context,
                depth + 1
            );
            break;
        case SOL_EXPR_HANDLE:
            valid = valid
                && sol_structure_expression(
                    validator,
                    expression->as.handle.authority,
                    context,
                    depth + 1
                )
                && sol_structure_expression(
                    validator,
                    expression->as.handle.provider,
                    context,
                    depth + 1
                )
                && sol_structure_expression(
                    validator,
                    expression->as.handle.body,
                    context,
                    depth + 1
                );
            break;
        case SOL_EXPR_RESULT:
            valid = valid && context == SOL_EXPRESSION_CONTEXT_ENSURES;
            break;
        case SOL_EXPR_OLD:
            valid = valid && context == SOL_EXPRESSION_CONTEXT_ENSURES
                && sol_structure_expression(
                    validator,
                    expression->as.old_expression,
                    context,
                    depth + 1
                );
            break;
        default:
            break;
    }
    if (!valid) return false;
    validator->expressions[expression_id] = 2;
    return true;
}

static bool sol_structure_clause_list(
    SolStructureValidator *validator,
    SolContractClauseId clause,
    SolContractOwnerKind owner_kind,
    size_t owner
) {
    while (clause != SOL_AST_NONE) {
        if (clause >= validator->tree->contract_clause_count || validator->clauses[clause]) {
            return false;
        }
        validator->clauses[clause] = 1;
        const SolContractClause *current = &validator->tree->contract_clauses[clause];
        if ((int)current->kind < 0 || current->kind > SOL_CONTRACT_ENSURES
            || current->owner_kind != owner_kind || current->owner != owner
            || !sol_structure_span_valid(validator, current->span)) {
            return false;
        }
        SolExpressionContext context = current->kind == SOL_CONTRACT_REQUIRES
            ? SOL_EXPRESSION_CONTEXT_REQUIRES
            : SOL_EXPRESSION_CONTEXT_ENSURES;
        SolContractConditionId condition = current->first_condition;
        while (condition != SOL_AST_NONE) {
            if (condition >= validator->tree->contract_condition_count
                || validator->conditions[condition]) {
                return false;
            }
            validator->conditions[condition] = 1;
            const SolContractCondition *entry
                = &validator->tree->contract_conditions[condition];
            if ((int)entry->outcome < 0 || entry->outcome > SOL_CONTRACT_OUTCOME_FAILURE
                || entry->owner_clause != clause
                || (current->kind == SOL_CONTRACT_REQUIRES
                    && entry->outcome != SOL_CONTRACT_OUTCOME_ALWAYS)
                || !sol_structure_span_valid(validator, entry->span)
                || !sol_structure_expression(
                    validator,
                    entry->expression,
                    context,
                    0
                )) {
                return false;
            }
            condition = entry->next;
        }
        clause = current->next;
    }
    return true;
}

static bool sol_structure_all_marked(const unsigned char *items, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (items[index] == 0) return false;
    }
    return true;
}

bool sol_syntax_contracts_validate(const SolSource *source, const SolSyntaxTree *tree) {
    if (source == NULL || source->text == NULL || tree == NULL
        || tree->import_count > tree->import_capacity
        || tree->item_count > tree->item_capacity
        || tree->capability_member_count > tree->capability_member_capacity
        || tree->trait_method_count > tree->trait_method_capacity
        || tree->expression_count > tree->expression_capacity
        || tree->argument_count > tree->argument_capacity
        || tree->statement_count > tree->statement_capacity
        || tree->match_arm_count > tree->match_arm_capacity
        || tree->contract_clause_count > tree->contract_clause_capacity
        || tree->contract_condition_count > tree->contract_condition_capacity
        || (tree->import_count != 0 && tree->imports == NULL)
        || (tree->item_count != 0 && tree->items == NULL)
        || (tree->capability_member_count != 0 && tree->capability_members == NULL)
        || (tree->trait_method_count != 0 && tree->trait_methods == NULL)
        || (tree->expression_count != 0 && tree->expressions == NULL)
        || (tree->argument_count != 0 && tree->arguments == NULL)
        || (tree->statement_count != 0 && tree->statements == NULL)
        || (tree->match_arm_count != 0 && tree->match_arms == NULL)
        || (tree->contract_clause_count != 0 && tree->contract_clauses == NULL)
        || (tree->contract_condition_count != 0 && tree->contract_conditions == NULL)) {
        return false;
    }
    SolStructureValidator validator = {.source = source, .tree = tree};
    validator.expressions = calloc(tree->expression_count, 1);
    validator.arguments = calloc(tree->argument_count, 1);
    validator.statements = calloc(tree->statement_count, 1);
    validator.arms = calloc(tree->match_arm_count, 1);
    validator.clauses = calloc(tree->contract_clause_count, 1);
    validator.conditions = calloc(tree->contract_condition_count, 1);
    bool allocated = (tree->expression_count == 0 || validator.expressions != NULL)
        && (tree->argument_count == 0 || validator.arguments != NULL)
        && (tree->statement_count == 0 || validator.statements != NULL)
        && (tree->match_arm_count == 0 || validator.arms != NULL)
        && (tree->contract_clause_count == 0 || validator.clauses != NULL)
        && (tree->contract_condition_count == 0 || validator.conditions != NULL);
    bool valid = allocated;
    for (size_t index = 0; valid && index < tree->import_count; ++index) {
        valid = sol_structure_span_valid(&validator, tree->imports[index].path);
    }
    for (size_t index = 0; valid && index < tree->item_count; ++index) {
        const SolSyntaxItem *item = &tree->items[index];
        if ((item->kind != SOL_ITEM_FUNCTION && item->first_contract != SOL_AST_NONE)
            || (item->body != SOL_AST_NONE
                && !sol_structure_expression(
                    &validator,
                    item->body,
                    SOL_EXPRESSION_CONTEXT_BODY,
                    0
                ))
            || !sol_structure_clause_list(
                &validator,
                item->first_contract,
                SOL_CONTRACT_OWNER_ITEM,
                index
            )) {
            valid = false;
        }
    }
    for (size_t index = 0; valid && index < tree->capability_member_count; ++index) {
        const SolCapabilityMember *member = &tree->capability_members[index];
        if ((member->body != SOL_AST_NONE
                && !sol_structure_expression(
                    &validator,
                    member->body,
                    SOL_EXPRESSION_CONTEXT_BODY,
                    0
                ))
            || !sol_structure_clause_list(
                &validator,
                member->first_contract,
                SOL_CONTRACT_OWNER_CAPABILITY_MEMBER,
                index
            )) {
            valid = false;
        }
    }
    for (size_t index = 0; valid && index < tree->trait_method_count; ++index) {
        const SolTraitMethod *method = &tree->trait_methods[index];
        if (method->body != SOL_AST_NONE && !sol_structure_expression(
            &validator, method->body, SOL_EXPRESSION_CONTEXT_BODY, 0
        )) valid = false;
    }
    valid = valid
        && sol_structure_all_marked(validator.expressions, tree->expression_count)
        && sol_structure_all_marked(validator.arguments, tree->argument_count)
        && sol_structure_all_marked(validator.statements, tree->statement_count)
        && sol_structure_all_marked(validator.arms, tree->match_arm_count)
        && sol_structure_all_marked(validator.clauses, tree->contract_clause_count)
        && sol_structure_all_marked(validator.conditions, tree->contract_condition_count);
    free(validator.expressions);
    free(validator.arguments);
    free(validator.statements);
    free(validator.arms);
    free(validator.clauses);
    free(validator.conditions);
    return valid;
}
