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
    SOL_ITEM_TYPE,
    SOL_ITEM_CAPABILITY,
    SOL_ITEM_FUNCTION,
    SOL_ITEM_TRAIT,
    SOL_ITEM_IMPLEMENTATION,
    SOL_ITEM_TEST,
} SolItemKind;

typedef enum {
    SOL_TYPE_DECLARATION_NONE,
    SOL_TYPE_DECLARATION_DISTINCT,
    SOL_TYPE_DECLARATION_REFINED,
} SolTypeDeclarationFlavor;

typedef struct {
    SolItemKind kind;
    SolTypeDeclarationFlavor flavor;
    SolSpan name;
    SolSpan span;
    /* Quoted token for @stable("..."); empty when no stable identity is declared. */
    SolSpan stable_identity;
    bool is_public;
    bool is_entrypoint;
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
    SolTypeId representation_type;
    SolCapabilityMemberId first_member;
    SolParameterId result_authority_parameter;
    SolParameterId capability_source;
    SolTypeParameterId first_type_parameter;
    SolEffectParameterId first_effect_parameter;
    SolSpan trait_name;
    SolTypeId implementation_type;
    SolTraitMethodId first_trait_method;
} SolSyntaxItem;

typedef struct {
    SolSpan path;
} SolImport;

typedef struct {
    SolSpan module_name;
    unsigned int edition;
    SolImport *imports;
    size_t import_count;
    size_t import_capacity;
    SolSyntaxItem *items;
    size_t item_count;
    size_t item_capacity;
    SolExpr *expressions;
    size_t expression_count;
    size_t expression_capacity;
    SolStatement *statements;
    size_t statement_count;
    size_t statement_capacity;
    SolLoopInvariant *loop_invariants;
    size_t loop_invariant_count;
    size_t loop_invariant_capacity;
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
    SolTypeParameter *type_parameters;
    size_t type_parameter_count;
    size_t type_parameter_capacity;
    SolEffectParameter *effect_parameters;
    size_t effect_parameter_count;
    size_t effect_parameter_capacity;
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
    SolTraitMethod *trait_methods;
    size_t trait_method_count;
    size_t trait_method_capacity;
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
