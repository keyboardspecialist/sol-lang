#include "sol/contract.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const SolSource *source;
    const SolSyntaxTree *syntax;
    const SolHirModule *hir;
    const SolTypeTable *types;
    const SolEffectTable *effects;
    SolContractTable *contracts;
    SolDiagnostics *diagnostics;
    SolObligation *obligation;
    size_t depth;
    bool malformed;
    bool allocation_failed;
    bool depth_reported;
} SolContractLowerer;

void sol_contract_table_init(SolContractTable *table) {
    memset(table, 0, sizeof(*table));
}

void sol_contract_table_free(SolContractTable *table) {
    free(table->obligations);
    free(table->snapshots);
    free(table->expression_snapshots);
    memset(table, 0, sizeof(*table));
}

static void sol_contract_error(
    SolContractLowerer *lowerer,
    const char *code,
    SolSpan span,
    const char *message
) {
    sol_diagnostics_add(
        lowerer->diagnostics,
        code,
        SOL_SEVERITY_ERROR,
        span,
        "%s",
        message
    );
}

static bool sol_contract_type_valid(
    const SolContractLowerer *lowerer,
    SolType type
) {
    if ((int)type.kind < 0 || type.kind > SOL_TYPE_NEVER) return false;
    switch (type.kind) {
        case SOL_TYPE_NOMINAL:
            return type.definition < lowerer->syntax->item_count
                && lowerer->syntax->items[type.definition].kind != SOL_ITEM_FUNCTION;
        case SOL_TYPE_FUNCTION:
            return type.definition < lowerer->syntax->item_count
                && lowerer->syntax->items[type.definition].kind == SOL_ITEM_FUNCTION;
        case SOL_TYPE_APPLICATION:
            return type.definition < lowerer->types->type_application_count;
        case SOL_TYPE_FUNCTION_SIGNATURE:
            return type.definition < lowerer->types->function_type_count;
        case SOL_TYPE_CAPABILITY_OPERATION:
            return type.definition < lowerer->syntax->capability_member_count;
        case SOL_TYPE_VARIANT:
            return type.definition < lowerer->syntax->variant_count;
        default:
            return type.definition == 0;
    }
}

static bool sol_contract_validate(SolContractLowerer *lowerer) {
    const SolSyntaxTree *syntax = lowerer->syntax;
    const SolHirModule *hir = lowerer->hir;
    const SolTypeTable *types = lowerer->types;
    const SolEffectTable *effects = lowerer->effects;
    SolContractTable *contracts = lowerer->contracts;
    if (!sol_syntax_contracts_validate(lowerer->source, syntax)
        || hir->definition_count != syntax->item_count
        || hir->resolution_count != syntax->expression_count
        || hir->local_count > hir->local_capacity
        || types->expression_count != syntax->expression_count
        || types->local_count != hir->local_count
        || types->definition_count != syntax->item_count
        || types->declared_type_count != syntax->type_count
        || types->type_application_count > types->type_application_capacity
        || types->function_type_count > types->function_type_capacity
        || effects->function_count != syntax->item_count
        || effects->capability_member_count != syntax->capability_member_count
        || (hir->definition_count != 0 && hir->definitions == NULL)
        || (hir->resolution_count != 0 && hir->resolutions == NULL)
        || (hir->local_count != 0 && hir->locals == NULL)
        || (types->expression_count != 0 && types->expressions == NULL)
        || (types->local_count != 0 && types->locals == NULL)
        || (types->definition_count != 0 && types->definitions == NULL)
        || (types->declared_type_count != 0 && types->declared_types == NULL)
        || (types->type_application_capacity != 0 && types->type_applications == NULL)
        || (types->function_type_capacity != 0 && types->function_types == NULL)
        || (effects->function_count != 0 && effects->functions == NULL)
        || (effects->capability_member_count != 0
            && effects->capability_members == NULL)
        || contracts->obligations != NULL || contracts->obligation_count != 0
        || contracts->snapshots != NULL || contracts->snapshot_count != 0
        || contracts->snapshot_capacity != 0 || contracts->expression_snapshots != NULL
        || contracts->expression_count != 0) {
        return false;
    }
    for (size_t index = 0; index < syntax->expression_count; ++index) {
        if (!sol_contract_type_valid(lowerer, types->expressions[index])) return false;
        SolResolution resolution = hir->resolutions[index];
        if ((int)resolution.kind < 0 || resolution.kind > SOL_RESOLUTION_BUILTIN
            || (resolution.kind == SOL_RESOLUTION_DEFINITION
                && resolution.target >= hir->definition_count)
            || (resolution.kind == SOL_RESOLUTION_LOCAL
                && resolution.target >= hir->local_count)
            || (resolution.kind == SOL_RESOLUTION_BUILTIN
                && resolution.target > SOL_BUILTIN_NONE)) {
            return false;
        }
    }
    for (size_t index = 0; index < types->type_application_count; ++index) {
        const SolTypeApplication *application = &types->type_applications[index];
        size_t expected = application->constructor == SOL_TYPE_CONSTRUCTOR_OPTION
            ? 1
            : application->constructor == SOL_TYPE_CONSTRUCTOR_RESULT ? 2 : 0;
        if (application->argument_count != expected) return false;
        for (size_t argument = 0; argument < expected; ++argument) {
            SolType type = application->arguments[argument];
            if (!sol_contract_type_valid(lowerer, type)
                || (type.kind == SOL_TYPE_APPLICATION && type.definition >= index)) {
                return false;
            }
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const SolTypeApplication *other = &types->type_applications[previous];
            bool equal = application->constructor == other->constructor
                && application->argument_count == other->argument_count;
            for (size_t argument = 0; equal && argument < expected; ++argument) {
                equal = application->arguments[argument].kind
                        == other->arguments[argument].kind
                    && application->arguments[argument].definition
                        == other->arguments[argument].definition;
            }
            if (equal) return false;
        }
    }
    for (size_t index = 0; index < types->function_type_count; ++index) {
        const SolFunctionType *function = &types->function_types[index];
        if ((function->parameter_count != 0 && function->parameters == NULL)
            || (function->effects.count != 0 && function->effects.atoms == NULL)
            || !sol_contract_type_valid(lowerer, function->result)) return false;
        for (size_t parameter = 0; parameter < function->parameter_count; ++parameter) {
            if (!sol_contract_type_valid(lowerer, function->parameters[parameter])) {
                return false;
            }
        }
    }
    for (size_t index = 0; index < effects->function_count; ++index) {
        if (effects->functions[index].count != 0
            && effects->functions[index].atoms == NULL) return false;
    }
    for (size_t index = 0; index < effects->capability_member_count; ++index) {
        if (effects->capability_members[index].count != 0
            && effects->capability_members[index].atoms == NULL) return false;
    }
    return true;
}

static bool sol_contract_append_snapshot(
    SolContractLowerer *lowerer,
    SolExprId old_expression,
    SolExprId operand
) {
    SolContractTable *table = lowerer->contracts;
    if (table->snapshot_count == table->snapshot_capacity) {
        size_t capacity = table->snapshot_capacity == 0
            ? 8
            : table->snapshot_capacity * 2;
        if (capacity < table->snapshot_capacity
            || capacity > SIZE_MAX / sizeof(*table->snapshots)) {
            lowerer->allocation_failed = true;
            return false;
        }
        SolSnapshot *grown = realloc(
            table->snapshots,
            capacity * sizeof(*table->snapshots)
        );
        if (grown == NULL) {
            lowerer->allocation_failed = true;
            return false;
        }
        table->snapshots = grown;
        table->snapshot_capacity = capacity;
    }
    SolSnapshotId id = table->snapshot_count++;
    table->snapshots[id] = (SolSnapshot){
        .id = id,
        .obligation = lowerer->obligation->id,
        .old_expression = old_expression,
        .operand = operand,
        .type = lowerer->types->expressions[old_expression],
    };
    table->expression_snapshots[old_expression] = id;
    ++lowerer->obligation->snapshot_count;
    return true;
}

static void sol_contract_expression(
    SolContractLowerer *lowerer,
    SolExprId expression_id,
    bool in_old
);

static void sol_contract_arguments(
    SolContractLowerer *lowerer,
    SolArgumentId argument,
    bool in_old
) {
    size_t traversed = 0;
    while (argument != SOL_AST_NONE) {
        if (argument >= lowerer->syntax->argument_count
            || traversed++ >= lowerer->syntax->argument_count) {
            lowerer->malformed = true;
            return;
        }
        const SolArgument *entry = &lowerer->syntax->arguments[argument];
        sol_contract_expression(lowerer, entry->value, in_old);
        argument = entry->next;
    }
}

static bool sol_contract_call_is_pure(
    const SolContractLowerer *lowerer,
    const SolExpr *call
) {
    SolExprId callee_id = call->as.call.callee;
    SolType callee = lowerer->types->expressions[callee_id];
    if (callee.kind == SOL_TYPE_FUNCTION
        && callee.definition < lowerer->effects->function_count) {
        return lowerer->effects->functions[callee.definition].count == 0;
    }
    if (callee.kind == SOL_TYPE_CAPABILITY_OPERATION
        && callee.definition < lowerer->effects->capability_member_count) {
        return lowerer->effects->capability_members[callee.definition].count == 0;
    }
    if (callee.kind == SOL_TYPE_FUNCTION_SIGNATURE
        && callee.definition < lowerer->types->function_type_count) {
        return lowerer->types->function_types[callee.definition].effects.count == 0;
    }
    if (callee.kind == SOL_TYPE_VARIANT) return true;
    SolResolution resolution = lowerer->hir->resolutions[callee_id];
    return resolution.kind == SOL_RESOLUTION_BUILTIN;
}

static void sol_contract_statements(
    SolContractLowerer *lowerer,
    SolStatementId statement,
    bool in_old
) {
    size_t traversed = 0;
    while (statement != SOL_AST_NONE) {
        if (statement >= lowerer->syntax->statement_count
            || traversed++ >= lowerer->syntax->statement_count) {
            lowerer->malformed = true;
            return;
        }
        const SolStatement *entry = &lowerer->syntax->statements[statement];
        if (entry->kind == SOL_STATEMENT_RETURN) {
            sol_contract_error(
                lowerer,
                "SOL-CONTRACT-002",
                entry->span,
                "return is not allowed in a contract predicate"
            );
        }
        SolExprId value = entry->kind == SOL_STATEMENT_LET
            ? entry->as.let_statement.value
            : entry->as.expression;
        sol_contract_expression(lowerer, value, in_old);
        statement = entry->next;
    }
}

static void sol_contract_expression(
    SolContractLowerer *lowerer,
    SolExprId expression_id,
    bool in_old
) {
    if (expression_id >= lowerer->syntax->expression_count) {
        lowerer->malformed = true;
        return;
    }
    if (lowerer->depth >= 256) {
        if (!lowerer->depth_reported) {
            sol_contract_error(
                lowerer,
                "SOL-CONTRACT-004",
                lowerer->syntax->expressions[expression_id].span,
                "contract expression exceeds the lowering limit of 256"
            );
            lowerer->depth_reported = true;
        }
        return;
    }
    ++lowerer->depth;
    const SolExpr *expression = &lowerer->syntax->expressions[expression_id];
    switch (expression->kind) {
        case SOL_EXPR_UNARY:
            sol_contract_expression(lowerer, expression->as.unary.operand, in_old);
            break;
        case SOL_EXPR_BINARY:
            sol_contract_expression(lowerer, expression->as.binary.left, in_old);
            sol_contract_expression(lowerer, expression->as.binary.right, in_old);
            break;
        case SOL_EXPR_CALL:
            if (!sol_contract_call_is_pure(lowerer, expression)) {
                sol_contract_error(
                    lowerer,
                    "SOL-CONTRACT-002",
                    expression->span,
                    "contract predicates may only call pure functions or operations"
                );
            }
            sol_contract_expression(lowerer, expression->as.call.callee, in_old);
            sol_contract_arguments(lowerer, expression->as.call.first_argument, in_old);
            break;
        case SOL_EXPR_FIELD:
            sol_contract_expression(lowerer, expression->as.field.base, in_old);
            break;
        case SOL_EXPR_RECORD:
            sol_contract_expression(lowerer, expression->as.record.type, in_old);
            sol_contract_arguments(lowerer, expression->as.record.first_field, in_old);
            break;
        case SOL_EXPR_IF:
            sol_contract_expression(lowerer, expression->as.if_expr.condition, in_old);
            sol_contract_expression(lowerer, expression->as.if_expr.then_branch, in_old);
            sol_contract_expression(lowerer, expression->as.if_expr.else_branch, in_old);
            break;
        case SOL_EXPR_MATCH: {
            sol_contract_expression(lowerer, expression->as.match_expr.scrutinee, in_old);
            SolMatchArmId arm = expression->as.match_expr.first_arm;
            size_t traversed = 0;
            while (arm != SOL_AST_NONE) {
                if (arm >= lowerer->syntax->match_arm_count
                    || traversed++ >= lowerer->syntax->match_arm_count) {
                    lowerer->malformed = true;
                    break;
                }
                sol_contract_expression(
                    lowerer,
                    lowerer->syntax->match_arms[arm].value,
                    in_old
                );
                arm = lowerer->syntax->match_arms[arm].next;
            }
            break;
        }
        case SOL_EXPR_BLOCK:
            sol_contract_statements(lowerer, expression->as.block.first_statement, in_old);
            break;
        case SOL_EXPR_PROPAGATE:
            sol_contract_error(
                lowerer,
                "SOL-CONTRACT-002",
                expression->span,
                "effect propagation is not allowed in a contract predicate"
            );
            sol_contract_expression(lowerer, expression->as.propagated, in_old);
            break;
        case SOL_EXPR_HANDLE:
            sol_contract_error(
                lowerer,
                "SOL-CONTRACT-002",
                expression->span,
                "effect handlers are not allowed in a contract predicate"
            );
            sol_contract_expression(lowerer, expression->as.handle.authority, in_old);
            sol_contract_expression(lowerer, expression->as.handle.provider, in_old);
            sol_contract_expression(lowerer, expression->as.handle.body, in_old);
            break;
        case SOL_EXPR_RESULT:
            if (in_old) {
                sol_contract_error(
                    lowerer,
                    "SOL-CONTRACT-003",
                    expression->span,
                    "old(result) is not a valid entry-state snapshot"
                );
            }
            break;
        case SOL_EXPR_OLD:
            if (in_old) {
                sol_contract_error(
                    lowerer,
                    "SOL-CONTRACT-003",
                    expression->span,
                    "nested old expressions are not allowed"
                );
            }
            sol_contract_append_snapshot(
                lowerer,
                expression_id,
                expression->as.old_expression
            );
            sol_contract_expression(lowerer, expression->as.old_expression, true);
            break;
        default:
            break;
    }
    --lowerer->depth;
}

static SolType sol_contract_owner_result(
    const SolContractLowerer *lowerer,
    const SolContractClause *clause
) {
    if (clause->owner_kind == SOL_CONTRACT_OWNER_ITEM) {
        return lowerer->types->definitions[clause->owner];
    }
    const SolCapabilityMember *member
        = &lowerer->syntax->capability_members[clause->owner];
    return lowerer->types->declared_types[member->return_type_id];
}

static SolResultBinding sol_contract_result_binding(
    const SolContractLowerer *lowerer,
    const SolContractClause *clause,
    const SolContractCondition *condition
) {
    SolResultBinding binding = {0};
    if (clause->kind != SOL_CONTRACT_ENSURES
        || condition->outcome == SOL_CONTRACT_OUTCOME_FAILURE) {
        return binding;
    }
    binding.available = true;
    binding.type = sol_contract_owner_result(lowerer, clause);
    const SolTypeApplication *application = sol_type_application(
        lowerer->types,
        binding.type
    );
    if (condition->outcome == SOL_CONTRACT_OUTCOME_SUCCESS
        && application != NULL
        && application->constructor == SOL_TYPE_CONSTRUCTOR_RESULT
        && application->argument_count == 2) {
        binding.type = application->arguments[0];
    }
    return binding;
}

bool sol_contract_lower(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    const SolEffectTable *effects,
    SolContractTable *contracts,
    SolDiagnostics *diagnostics
) {
    if (diagnostics == NULL) return false;
    if (source == NULL || source->text == NULL || syntax == NULL || hir == NULL
        || types == NULL || effects == NULL || contracts == NULL) {
        sol_diagnostics_add(
            diagnostics,
            "SOL-INTERNAL-005",
            SOL_SEVERITY_ERROR,
            (SolSpan){0},
            "null compiler input passed to contract lowering"
        );
        return false;
    }
    SolContractLowerer lowerer = {
        .source = source,
        .syntax = syntax,
        .hir = hir,
        .types = types,
        .effects = effects,
        .contracts = contracts,
        .diagnostics = diagnostics,
    };
    if (!sol_contract_validate(&lowerer)) {
        sol_contract_error(
            &lowerer,
            "SOL-INTERNAL-005",
            (SolSpan){0},
            "invalid semantic input or output table passed to contract lowering"
        );
        return false;
    }
    size_t count = syntax->contract_condition_count;
    if (count > SIZE_MAX / sizeof(*contracts->obligations)
        || syntax->expression_count > SIZE_MAX / sizeof(*contracts->expression_snapshots)) {
        return false;
    }
    contracts->obligations = calloc(count, sizeof(*contracts->obligations));
    contracts->expression_snapshots = malloc(
        syntax->expression_count * sizeof(*contracts->expression_snapshots)
    );
    if ((count != 0 && contracts->obligations == NULL)
        || (syntax->expression_count != 0 && contracts->expression_snapshots == NULL)) {
        lowerer.allocation_failed = true;
    }
    if (!lowerer.allocation_failed) {
        contracts->obligation_count = count;
        contracts->expression_count = syntax->expression_count;
        for (size_t index = 0; index < syntax->expression_count; ++index) {
            contracts->expression_snapshots[index] = SOL_AST_NONE;
        }
        for (size_t condition_id = 0; condition_id < count; ++condition_id) {
            const SolContractCondition *condition
                = &syntax->contract_conditions[condition_id];
            if (condition->owner_clause >= syntax->contract_clause_count) {
                lowerer.malformed = true;
                break;
            }
            const SolContractClause *clause
                = &syntax->contract_clauses[condition->owner_clause];
            SolObligation *obligation = &contracts->obligations[condition_id];
            *obligation = (SolObligation){
                .id = (SolObligationId)condition_id,
                .condition = condition_id,
                .owner_kind = clause->owner_kind,
                .owner = clause->owner,
                .kind = clause->kind,
                .outcome = condition->outcome,
                .predicate = condition->expression,
                .predicate_type = types->expressions[condition->expression],
                .result = sol_contract_result_binding(&lowerer, clause, condition),
                .first_snapshot = contracts->snapshot_count,
            };
            lowerer.obligation = obligation;
            lowerer.depth = 0;
            lowerer.depth_reported = false;
            if (obligation->predicate_type.kind != SOL_TYPE_BOOL
                && obligation->predicate_type.kind != SOL_TYPE_ERROR) {
                sol_contract_error(
                    &lowerer,
                    "SOL-CONTRACT-001",
                    syntax->expressions[obligation->predicate].span,
                    "contract predicate must have type Bool"
                );
            }
            sol_contract_expression(&lowerer, obligation->predicate, false);
            if (lowerer.malformed || lowerer.allocation_failed) break;
        }
    }
    if (lowerer.allocation_failed) diagnostics->allocation_failed = true;
    if (lowerer.malformed) {
        sol_contract_error(
            &lowerer,
            "SOL-INTERNAL-005",
            (SolSpan){0},
            "malformed semantic input encountered during contract lowering"
        );
    }
    return !lowerer.malformed && !lowerer.allocation_failed
        && !diagnostics->allocation_failed;
}
