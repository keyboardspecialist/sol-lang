#ifndef SOL_PARSER_H
#define SOL_PARSER_H

#include "sol/ast.h"
#include "sol/diagnostic.h"
#include "sol/token.h"

#include <stdbool.h>
#include <stddef.h>

typedef enum {
    SOL_ITEM_RECORD,
    SOL_ITEM_ENUM,
    SOL_ITEM_CAPABILITY,
    SOL_ITEM_FUNCTION,
} SolItemKind;

typedef struct {
    SolItemKind kind;
    SolSpan name;
    SolSpan span;
    bool is_public;
    SolExprId body;
    SolParameterId first_parameter;
    SolSpan return_type;
    SolTypeId return_type_id;
    SolFieldId first_field;
    SolVariantId first_variant;
    bool is_open;
    SolEffectId first_effect;
    bool has_effect_clause;
    SolContractClauseId first_contract;
    SolCapabilityMemberId first_member;
    SolParameterId result_authority_parameter;
    SolParameterId capability_source;
} SolSyntaxItem;

typedef struct {
    SolSpan module_name;
    unsigned int edition;
    SolSyntaxItem *items;
    size_t item_count;
    size_t item_capacity;
    SolExpr *expressions;
    size_t expression_count;
    size_t expression_capacity;
    SolStatement *statements;
    size_t statement_count;
    size_t statement_capacity;
    SolArgument *arguments;
    size_t argument_count;
    size_t argument_capacity;
    SolParameter *parameters;
    size_t parameter_count;
    size_t parameter_capacity;
    SolSyntaxType *types;
    size_t type_count;
    size_t type_capacity;
    SolTypeArgument *type_arguments;
    size_t type_argument_count;
    size_t type_argument_capacity;
    SolField *fields;
    size_t field_count;
    size_t field_capacity;
    SolVariant *variants;
    size_t variant_count;
    size_t variant_capacity;
    SolPattern *patterns;
    size_t pattern_count;
    size_t pattern_capacity;
    SolPatternBinding *pattern_bindings;
    size_t pattern_binding_count;
    size_t pattern_binding_capacity;
    SolMatchArm *match_arms;
    size_t match_arm_count;
    size_t match_arm_capacity;
    SolEffect *effects;
    size_t effect_count;
    size_t effect_capacity;
    SolCapabilityMember *capability_members;
    size_t capability_member_count;
    size_t capability_member_capacity;
    SolContractClause *contract_clauses;
    size_t contract_clause_count;
    size_t contract_clause_capacity;
    SolContractCondition *contract_conditions;
    size_t contract_condition_count;
    size_t contract_condition_capacity;
} SolSyntaxTree;

void sol_syntax_tree_init(SolSyntaxTree *tree);
void sol_syntax_tree_free(SolSyntaxTree *tree);
bool sol_parse(
    const SolSource *source,
    const SolTokens *tokens,
    SolSyntaxTree *tree,
    SolDiagnostics *diagnostics
);
bool sol_syntax_contracts_validate(
    const SolSource *source,
    const SolSyntaxTree *tree
);

#endif
