#include "sol/parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SOL_PARSER_CONTRACT_NONE,
    SOL_PARSER_CONTRACT_REQUIRES,
    SOL_PARSER_CONTRACT_ENSURES,
} SolParserContractContext;

typedef struct {
    const SolSource *source;
    const SolTokens *tokens;
    SolSyntaxTree *tree;
    SolDiagnostics *diagnostics;
    size_t cursor;
    size_t type_depth;
    size_t expression_depth;
    size_t loop_depth;
    SolParserContractContext contract_context;
    bool suppress_record_literal;
    bool allocation_failed;
} SolParser;

void sol_syntax_tree_init(SolSyntaxTree *tree) {
    memset(tree, 0, sizeof(*tree));
}

void sol_syntax_tree_free(SolSyntaxTree *tree) {
    free(tree->imports);
    free(tree->items);
    free(tree->expressions);
    free(tree->statements);
    free(tree->loop_invariants);
    free(tree->arguments);
    free(tree->parameters);
    free(tree->types);
    free(tree->type_arguments);
    free(tree->type_parameters);
    free(tree->effect_parameters);
    free(tree->fields);
    free(tree->variants);
    free(tree->patterns);
    free(tree->pattern_bindings);
    free(tree->match_arms);
    free(tree->effects);
    free(tree->capability_members);
    free(tree->trait_methods);
    free(tree->contract_clauses);
    free(tree->contract_conditions);
    memset(tree, 0, sizeof(*tree));
}

static size_t sol_parser_significant_index(SolParser *parser) {
    while (parser->cursor < parser->tokens->count
        && sol_token_is_trivia(parser->tokens->items[parser->cursor].kind)) {
        ++parser->cursor;
    }
    return parser->cursor;
}

static SolToken sol_parser_current(SolParser *parser) {
    size_t index = sol_parser_significant_index(parser);
    if (index >= parser->tokens->count) {
        return (SolToken){
            .kind = SOL_TOKEN_EOF,
            .span = {.start = parser->source->length, .end = parser->source->length},
        };
    }
    return parser->tokens->items[index];
}

static SolTokenKind sol_parser_kind(SolParser *parser) {
    return sol_parser_current(parser).kind;
}

static SolTokenKind sol_parser_peek_kind(SolParser *parser, size_t distance) {
    size_t index = sol_parser_significant_index(parser);
    while (index < parser->tokens->count) {
        SolTokenKind kind = parser->tokens->items[index].kind;
        if (!sol_token_is_trivia(kind)) {
            if (distance == 0) {
                return kind;
            }
            --distance;
        }
        ++index;
    }
    return SOL_TOKEN_EOF;
}

static SolToken sol_parser_advance(SolParser *parser) {
    SolToken token = sol_parser_current(parser);
    if (parser->cursor < parser->tokens->count) {
        ++parser->cursor;
    }
    return token;
}

static bool sol_parser_match(SolParser *parser, SolTokenKind kind) {
    if (sol_parser_kind(parser) != kind) {
        return false;
    }
    sol_parser_advance(parser);
    return true;
}

static void sol_parser_error(SolParser *parser, const char *code, SolToken token, const char *message) {
    sol_diagnostics_add(
        parser->diagnostics,
        code,
        SOL_SEVERITY_ERROR,
        token.span,
        "%s; found %s",
        message,
        sol_token_kind_name(token.kind)
    );
}

static bool sol_parser_expect(SolParser *parser, SolTokenKind kind, const char *message) {
    SolToken token = sol_parser_current(parser);
    if (token.kind == kind) {
        sol_parser_advance(parser);
        return true;
    }
    sol_parser_error(parser, "SOL-PARSE-001", token, message);
    return false;
}

static bool sol_parser_expect_identifier_text(
    SolParser *parser,
    const char *text,
    const char *message
) {
    SolToken token = sol_parser_current(parser);
    if (token.kind == SOL_TOKEN_IDENTIFIER
        && sol_token_text_equal(parser->source, token, text)) {
        sol_parser_advance(parser);
        return true;
    }
    sol_parser_error(parser, "SOL-PARSE-016", token, message);
    if (token.kind != SOL_TOKEN_EOF) sol_parser_advance(parser);
    return false;
}

static bool sol_parser_add_item(SolParser *parser, SolSyntaxItem item) {
    if (parser->tree->item_count == parser->tree->item_capacity) {
        if (parser->tree->item_capacity > SIZE_MAX / 2) {
            parser->allocation_failed = true;
            return false;
        }
        size_t capacity = parser->tree->item_capacity == 0 ? 8 : parser->tree->item_capacity * 2;
        if (capacity > SIZE_MAX / sizeof(*parser->tree->items)) {
            parser->allocation_failed = true;
            return false;
        }
        SolSyntaxItem *items = realloc(parser->tree->items, capacity * sizeof(*items));
        if (items == NULL) {
            parser->allocation_failed = true;
            return false;
        }
        parser->tree->items = items;
        parser->tree->item_capacity = capacity;
    }
    parser->tree->items[parser->tree->item_count++] = item;
    return true;
}

static void *sol_parser_grow(
    SolParser *parser,
    void *items,
    size_t *capacity,
    size_t element_size
) {
    if (*capacity > SIZE_MAX / 2) {
        parser->allocation_failed = true;
        return NULL;
    }
    size_t new_capacity = *capacity == 0 ? 32 : *capacity * 2;
    if (new_capacity > SIZE_MAX / element_size) {
        parser->allocation_failed = true;
        return NULL;
    }
    void *grown = realloc(items, new_capacity * element_size);
    if (grown == NULL) {
        parser->allocation_failed = true;
        return NULL;
    }
    *capacity = new_capacity;
    return grown;
}

static bool sol_parser_add_import(SolParser *parser, SolImport import) {
    if (parser->tree->import_count == parser->tree->import_capacity) {
        SolImport *imports = sol_parser_grow(
            parser,
            parser->tree->imports,
            &parser->tree->import_capacity,
            sizeof(*parser->tree->imports)
        );
        if (imports == NULL) return false;
        parser->tree->imports = imports;
    }
    parser->tree->imports[parser->tree->import_count++] = import;
    return true;
}

static SolExprId sol_parser_add_expression(SolParser *parser, SolExpr expression) {
    if (parser->tree->expression_count == parser->tree->expression_capacity) {
        SolExpr *expressions = sol_parser_grow(
            parser,
            parser->tree->expressions,
            &parser->tree->expression_capacity,
            sizeof(*parser->tree->expressions)
        );
        if (expressions == NULL) {
            return SOL_AST_NONE;
        }
        parser->tree->expressions = expressions;
    }
    SolExprId id = parser->tree->expression_count++;
    parser->tree->expressions[id] = expression;
    return id;
}

static SolStatementId sol_parser_add_statement(SolParser *parser, SolStatement statement) {
    if (parser->tree->statement_count == parser->tree->statement_capacity) {
        SolStatement *statements = sol_parser_grow(
            parser,
            parser->tree->statements,
            &parser->tree->statement_capacity,
            sizeof(*parser->tree->statements)
        );
        if (statements == NULL) {
            return SOL_AST_NONE;
        }
        parser->tree->statements = statements;
    }
    SolStatementId id = parser->tree->statement_count++;
    parser->tree->statements[id] = statement;
    return id;
}

static SolLoopInvariantId sol_parser_add_loop_invariant(
    SolParser *parser,
    SolLoopInvariant invariant
) {
    if (parser->tree->loop_invariant_count == parser->tree->loop_invariant_capacity) {
        SolLoopInvariant *invariants = sol_parser_grow(
            parser,
            parser->tree->loop_invariants,
            &parser->tree->loop_invariant_capacity,
            sizeof(*parser->tree->loop_invariants)
        );
        if (invariants == NULL) return SOL_AST_NONE;
        parser->tree->loop_invariants = invariants;
    }
    SolLoopInvariantId id = parser->tree->loop_invariant_count++;
    parser->tree->loop_invariants[id] = invariant;
    return id;
}

static SolArgumentId sol_parser_add_argument(SolParser *parser, SolArgument argument) {
    if (parser->tree->argument_count == parser->tree->argument_capacity) {
        SolArgument *arguments = sol_parser_grow(
            parser,
            parser->tree->arguments,
            &parser->tree->argument_capacity,
            sizeof(*parser->tree->arguments)
        );
        if (arguments == NULL) {
            return SOL_AST_NONE;
        }
        parser->tree->arguments = arguments;
    }
    SolArgumentId id = parser->tree->argument_count++;
    parser->tree->arguments[id] = argument;
    return id;
}

static SolParameterId sol_parser_add_parameter(SolParser *parser, SolParameter parameter) {
    if (parser->tree->parameter_count == parser->tree->parameter_capacity) {
        SolParameter *parameters = sol_parser_grow(
            parser,
            parser->tree->parameters,
            &parser->tree->parameter_capacity,
            sizeof(*parser->tree->parameters)
        );
        if (parameters == NULL) {
            return SOL_AST_NONE;
        }
        parser->tree->parameters = parameters;
    }
    SolParameterId id = parser->tree->parameter_count++;
    parser->tree->parameters[id] = parameter;
    return id;
}

static SolTypeId sol_parser_add_type(SolParser *parser, SolSyntaxType type) {
    if (parser->tree->type_count == parser->tree->type_capacity) {
        SolSyntaxType *types = sol_parser_grow(
            parser,
            parser->tree->types,
            &parser->tree->type_capacity,
            sizeof(*parser->tree->types)
        );
        if (types == NULL) {
            return SOL_AST_NONE;
        }
        parser->tree->types = types;
    }
    SolTypeId id = parser->tree->type_count++;
    parser->tree->types[id] = type;
    return id;
}

static bool sol_parser_effect_clause(
    SolParser *parser,
    SolEffectOwnerKind owner_kind,
    size_t owner,
    SolEffectId *first_effect
);
static bool sol_parser_is_declaration_start(SolTokenKind kind);

static SolTypeArgumentId sol_parser_add_type_argument(
    SolParser *parser,
    SolTypeArgument argument
) {
    if (parser->tree->type_argument_count == parser->tree->type_argument_capacity) {
        SolTypeArgument *arguments = sol_parser_grow(
            parser,
            parser->tree->type_arguments,
            &parser->tree->type_argument_capacity,
            sizeof(*parser->tree->type_arguments)
        );
        if (arguments == NULL) {
            return SOL_AST_NONE;
        }
        parser->tree->type_arguments = arguments;
    }
    SolTypeArgumentId id = parser->tree->type_argument_count++;
    parser->tree->type_arguments[id] = argument;
    return id;
}

static SolTypeParameterId sol_parser_add_type_parameter(
    SolParser *parser,
    SolTypeParameter parameter
) {
    if (parser->tree->type_parameter_count == parser->tree->type_parameter_capacity) {
        SolTypeParameter *parameters = sol_parser_grow(
            parser,
            parser->tree->type_parameters,
            &parser->tree->type_parameter_capacity,
            sizeof(*parser->tree->type_parameters)
        );
        if (parameters == NULL) return SOL_AST_NONE;
        parser->tree->type_parameters = parameters;
    }
    SolTypeParameterId id = parser->tree->type_parameter_count++;
    parser->tree->type_parameters[id] = parameter;
    return id;
}

static SolEffectParameterId sol_parser_add_effect_parameter(
    SolParser *parser,
    SolEffectParameter parameter
) {
    if (parser->tree->effect_parameter_count == parser->tree->effect_parameter_capacity) {
        SolEffectParameter *parameters = sol_parser_grow(
            parser,
            parser->tree->effect_parameters,
            &parser->tree->effect_parameter_capacity,
            sizeof(*parser->tree->effect_parameters)
        );
        if (parameters == NULL) return SOL_AST_NONE;
        parser->tree->effect_parameters = parameters;
    }
    SolEffectParameterId id = parser->tree->effect_parameter_count++;
    parser->tree->effect_parameters[id] = parameter;
    return id;
}

static SolFieldId sol_parser_add_field(SolParser *parser, SolField field) {
    if (parser->tree->field_count == parser->tree->field_capacity) {
        SolField *fields = sol_parser_grow(
            parser,
            parser->tree->fields,
            &parser->tree->field_capacity,
            sizeof(*parser->tree->fields)
        );
        if (fields == NULL) {
            return SOL_AST_NONE;
        }
        parser->tree->fields = fields;
    }
    SolFieldId id = parser->tree->field_count++;
    parser->tree->fields[id] = field;
    return id;
}

static SolVariantId sol_parser_add_variant(SolParser *parser, SolVariant variant) {
    if (parser->tree->variant_count == parser->tree->variant_capacity) {
        SolVariant *variants = sol_parser_grow(
            parser,
            parser->tree->variants,
            &parser->tree->variant_capacity,
            sizeof(*parser->tree->variants)
        );
        if (variants == NULL) return SOL_AST_NONE;
        parser->tree->variants = variants;
    }
    SolVariantId id = parser->tree->variant_count++;
    parser->tree->variants[id] = variant;
    return id;
}

static SolPatternId sol_parser_add_pattern(SolParser *parser, SolPattern pattern) {
    if (parser->tree->pattern_count == parser->tree->pattern_capacity) {
        SolPattern *patterns = sol_parser_grow(
            parser,
            parser->tree->patterns,
            &parser->tree->pattern_capacity,
            sizeof(*parser->tree->patterns)
        );
        if (patterns == NULL) return SOL_AST_NONE;
        parser->tree->patterns = patterns;
    }
    SolPatternId id = parser->tree->pattern_count++;
    parser->tree->patterns[id] = pattern;
    return id;
}

static SolPatternBindingId sol_parser_add_pattern_binding(
    SolParser *parser,
    SolPatternBinding binding
) {
    if (parser->tree->pattern_binding_count == parser->tree->pattern_binding_capacity) {
        SolPatternBinding *bindings = sol_parser_grow(
            parser,
            parser->tree->pattern_bindings,
            &parser->tree->pattern_binding_capacity,
            sizeof(*parser->tree->pattern_bindings)
        );
        if (bindings == NULL) return SOL_AST_NONE;
        parser->tree->pattern_bindings = bindings;
    }
    SolPatternBindingId id = parser->tree->pattern_binding_count++;
    parser->tree->pattern_bindings[id] = binding;
    return id;
}

static SolMatchArmId sol_parser_add_match_arm(SolParser *parser, SolMatchArm arm) {
    if (parser->tree->match_arm_count == parser->tree->match_arm_capacity) {
        SolMatchArm *arms = sol_parser_grow(
            parser,
            parser->tree->match_arms,
            &parser->tree->match_arm_capacity,
            sizeof(*parser->tree->match_arms)
        );
        if (arms == NULL) return SOL_AST_NONE;
        parser->tree->match_arms = arms;
    }
    SolMatchArmId id = parser->tree->match_arm_count++;
    parser->tree->match_arms[id] = arm;
    return id;
}

static SolEffectId sol_parser_add_effect(SolParser *parser, SolEffect effect) {
    if (parser->tree->effect_count == parser->tree->effect_capacity) {
        SolEffect *effects = sol_parser_grow(
            parser,
            parser->tree->effects,
            &parser->tree->effect_capacity,
            sizeof(*parser->tree->effects)
        );
        if (effects == NULL) return SOL_AST_NONE;
        parser->tree->effects = effects;
    }
    SolEffectId id = parser->tree->effect_count++;
    parser->tree->effects[id] = effect;
    return id;
}

static SolCapabilityMemberId sol_parser_add_capability_member(
    SolParser *parser,
    SolCapabilityMember member
) {
    if (parser->tree->capability_member_count == parser->tree->capability_member_capacity) {
        SolCapabilityMember *members = sol_parser_grow(
            parser,
            parser->tree->capability_members,
            &parser->tree->capability_member_capacity,
            sizeof(*parser->tree->capability_members)
        );
        if (members == NULL) return SOL_AST_NONE;
        parser->tree->capability_members = members;
    }
    SolCapabilityMemberId id = parser->tree->capability_member_count++;
    parser->tree->capability_members[id] = member;
    return id;
}

static SolTraitMethodId sol_parser_add_trait_method(
    SolParser *parser,
    SolTraitMethod method
) {
    if (parser->tree->trait_method_count == parser->tree->trait_method_capacity) {
        SolTraitMethod *methods = sol_parser_grow(
            parser,
            parser->tree->trait_methods,
            &parser->tree->trait_method_capacity,
            sizeof(*parser->tree->trait_methods)
        );
        if (methods == NULL) return SOL_AST_NONE;
        parser->tree->trait_methods = methods;
    }
    SolTraitMethodId id = parser->tree->trait_method_count++;
    parser->tree->trait_methods[id] = method;
    return id;
}

static SolContractClauseId sol_parser_add_contract_clause(
    SolParser *parser,
    SolContractClause clause
) {
    if (parser->tree->contract_clause_count == parser->tree->contract_clause_capacity) {
        SolContractClause *clauses = sol_parser_grow(
            parser,
            parser->tree->contract_clauses,
            &parser->tree->contract_clause_capacity,
            sizeof(*parser->tree->contract_clauses)
        );
        if (clauses == NULL) return SOL_AST_NONE;
        parser->tree->contract_clauses = clauses;
    }
    SolContractClauseId id = parser->tree->contract_clause_count++;
    parser->tree->contract_clauses[id] = clause;
    return id;
}

static SolContractConditionId sol_parser_add_contract_condition(
    SolParser *parser,
    SolContractCondition condition
) {
    if (parser->tree->contract_condition_count
        == parser->tree->contract_condition_capacity) {
        SolContractCondition *conditions = sol_parser_grow(
            parser,
            parser->tree->contract_conditions,
            &parser->tree->contract_condition_capacity,
            sizeof(*parser->tree->contract_conditions)
        );
        if (conditions == NULL) return SOL_AST_NONE;
        parser->tree->contract_conditions = conditions;
    }
    SolContractConditionId id = parser->tree->contract_condition_count++;
    parser->tree->contract_conditions[id] = condition;
    return id;
}

static bool sol_parser_path_components(
    SolParser *parser,
    SolSpan *span,
    const char *description,
    size_t *component_count,
    bool *complete
) {
    SolToken first = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, description)) {
        if (component_count != NULL) *component_count = 0;
        if (complete != NULL) *complete = false;
        return false;
    }
    size_t components = 1;
    bool valid = true;
    SolToken last = first;
    while (sol_parser_match(parser, SOL_TOKEN_DOT)) {
        last = sol_parser_current(parser);
        if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a path component after '.'")) {
            valid = false;
            break;
        }
        ++components;
    }
    *span = (SolSpan){.start = first.span.start, .end = last.span.end};
    if (component_count != NULL) *component_count = components;
    if (complete != NULL) *complete = valid;
    return true;
}

static bool sol_parser_path(SolParser *parser, SolSpan *span, const char *description) {
    return sol_parser_path_components(parser, span, description, NULL, NULL);
}

static bool sol_parser_type(SolParser *parser, SolSpan *span, SolTypeId *type_id);

static bool sol_parser_type_arguments(SolParser *parser, SolTypeArgumentId *first_argument) {
    *first_argument = SOL_AST_NONE;
    if (!sol_parser_match(parser, SOL_TOKEN_LESS)) {
        return true;
    }
    if (sol_parser_match(parser, SOL_TOKEN_GREATER)) {
        SolToken greater = parser->tokens->items[parser->cursor - 1];
        sol_parser_error(
            parser,
            "SOL-PARSE-006",
            greater,
            "generic argument lists cannot be empty"
        );
        return false;
    }
    SolTypeArgumentId last_argument = SOL_AST_NONE;
    for (;;) {
        SolTypeId type;
        if (!sol_parser_type(parser, NULL, &type)) {
            return false;
        }
        SolTypeArgumentId argument = sol_parser_add_type_argument(parser, (SolTypeArgument){
            .type = type,
            .next = SOL_AST_NONE,
        });
        if (argument == SOL_AST_NONE) {
            return false;
        }
        if (*first_argument == SOL_AST_NONE) {
            *first_argument = argument;
        } else {
            parser->tree->type_arguments[last_argument].next = argument;
        }
        last_argument = argument;
        if (sol_parser_match(parser, SOL_TOKEN_GREATER)) {
            return true;
        }
        if (!sol_parser_expect(parser, SOL_TOKEN_COMMA, "expected ',' between generic arguments")) {
            return false;
        }
        if (sol_parser_match(parser, SOL_TOKEN_GREATER)) {
            return true;
        }
    }
}

static bool sol_parser_type(SolParser *parser, SolSpan *span, SolTypeId *type_id) {
    SolToken first = sol_parser_current(parser);
    if (first.kind == SOL_TOKEN_BORROW || first.kind == SOL_TOKEN_INOUT) {
        sol_parser_error(parser, "SOL-PARSE-021", first,
            "borrow and inout are allowed only as callable parameter qualifiers");
        sol_parser_advance(parser);
        return sol_parser_type(parser, span, type_id);
    }
    if (parser->type_depth >= 256) {
        sol_diagnostics_add(
            parser->diagnostics,
            "SOL-PARSE-010",
            SOL_SEVERITY_ERROR,
            sol_parser_current(parser).span,
            "type nesting exceeds the compiler limit of 256"
        );
        return false;
    }
    ++parser->type_depth;

    bool parsed;
    SolSyntaxTypeKind kind;
    SolSpan name = {0};
    SolTypeArgumentId first_argument = SOL_AST_NONE;
    SolTypeId return_type = SOL_AST_NONE;
    SolEffectId first_effect = SOL_AST_NONE;
    SolSpan effect_tail = {0};
    bool has_effect_tail = false;
    bool is_capability = false;
    SolTypeId reserved = SOL_AST_NONE;
    if (sol_parser_kind(parser) == SOL_TOKEN_FUNCTION
        && sol_parser_peek_kind(parser, 1) == SOL_TOKEN_LEFT_PAREN) {
        sol_parser_advance(parser);
        kind = SOL_SYNTAX_TYPE_FUNCTION;
        reserved = sol_parser_add_type(parser, (SolSyntaxType){
            .kind = SOL_SYNTAX_TYPE_FUNCTION,
            .first_argument = SOL_AST_NONE,
            .return_type = SOL_AST_NONE,
            .first_effect = SOL_AST_NONE,
        });
        parsed = reserved != SOL_AST_NONE
            && sol_parser_expect(
                parser,
                SOL_TOKEN_LEFT_PAREN,
                "expected '(' after 'function' in function type"
            );
        SolTypeArgumentId last_argument = SOL_AST_NONE;
        if (parsed && !sol_parser_match(parser, SOL_TOKEN_RIGHT_PAREN)) {
            for (;;) {
                SolAccessMode access = SOL_ACCESS_OWNED;
                if (sol_parser_match(parser, SOL_TOKEN_BORROW)) access = SOL_ACCESS_SHARED;
                else if (sol_parser_match(parser, SOL_TOKEN_INOUT)) {
                    access = SOL_ACCESS_EXCLUSIVE;
                }
                SolTypeId parameter_type;
                if (!sol_parser_type(parser, NULL, &parameter_type)) {
                    parsed = false;
                    break;
                }
                SolTypeArgumentId argument = sol_parser_add_type_argument(
                    parser,
                    (SolTypeArgument){
                        .type = parameter_type, .next = SOL_AST_NONE, .access = access
                    }
                );
                if (argument == SOL_AST_NONE) {
                    parsed = false;
                    break;
                }
                if (first_argument == SOL_AST_NONE) first_argument = argument;
                else parser->tree->type_arguments[last_argument].next = argument;
                last_argument = argument;
                if (sol_parser_match(parser, SOL_TOKEN_RIGHT_PAREN)) break;
                if (!sol_parser_expect(
                    parser,
                    SOL_TOKEN_COMMA,
                    "expected ',' or ')' after function parameter type"
                )) {
                    parsed = false;
                    break;
                }
                if (sol_parser_match(parser, SOL_TOKEN_RIGHT_PAREN)) break;
            }
        }
        parsed = parsed && sol_parser_expect(
            parser,
            SOL_TOKEN_ARROW,
            "expected '->' after function parameter types"
        );
        parsed = parsed && sol_parser_type(parser, NULL, &return_type);
        parsed = parsed && sol_parser_expect(
            parser,
            SOL_TOKEN_EFFECTS,
            "expected 'effects' after function return type"
        );
        if (parsed && sol_parser_kind(parser) == SOL_TOKEN_IDENTIFIER) {
            effect_tail = sol_parser_advance(parser).span;
            has_effect_tail = true;
        } else {
            parsed = parsed && sol_parser_effect_clause(
                parser,
                SOL_EFFECT_OWNER_TYPE,
                reserved,
                &first_effect
            );
        }
    } else if (sol_parser_match(parser, SOL_TOKEN_LEFT_PAREN)) {
        kind = SOL_SYNTAX_TYPE_UNIT;
        parsed = sol_parser_expect(
            parser,
            SOL_TOKEN_RIGHT_PAREN,
            "expected ')' to complete the unit type"
        );
    } else {
        kind = SOL_SYNTAX_TYPE_PATH;
        is_capability = sol_parser_match(parser, SOL_TOKEN_CAPABILITY);
        parsed = sol_parser_path(parser, &name, "expected a type name")
            && sol_parser_type_arguments(parser, &first_argument);
    }
    --parser->type_depth;
    if (parsed) {
        size_t last_index = parser->cursor;
        do {
            --last_index;
        } while (last_index > 0
            && sol_token_is_trivia(parser->tokens->items[last_index].kind));
        SolToken last = parser->tokens->items[last_index];
        SolSpan type_span = {.start = first.span.start, .end = last.span.end};
        SolSyntaxType type = {
            .kind = kind,
            .span = type_span,
            .name = name,
            .first_argument = first_argument,
            .return_type = return_type,
            .first_effect = first_effect,
            .effect_tail = effect_tail,
            .has_effect_tail = has_effect_tail,
            .is_capability = is_capability,
            .owner_item = parser->tree->item_count,
        };
        SolTypeId parsed_type = reserved;
        if (reserved == SOL_AST_NONE) parsed_type = sol_parser_add_type(parser, type);
        else parser->tree->types[reserved] = type;
        if (parsed_type == SOL_AST_NONE) {
            return false;
        }
        if (span != NULL) {
            *span = type_span;
        }
        if (type_id != NULL) {
            *type_id = parsed_type;
        }
    }
    return parsed;
}

static SolExprId sol_parser_expression(SolParser *parser, unsigned int minimum_precedence);
static SolExprId sol_parser_block_expression(SolParser *parser);

static SolExprId sol_parser_handle_expression(SolParser *parser) {
    SolToken handle = sol_parser_advance(parser);
    SolSpan effect_name = {0};
    bool valid = sol_parser_path(parser, &effect_name, "expected an effect name after 'handle'");
    valid = sol_parser_expect(
        parser,
        SOL_TOKEN_LESS,
        "handled effects require an exact capability argument"
    ) && valid;
    SolExprId authority = sol_parser_expression(parser, 5);
    valid = sol_parser_expect(
        parser,
        SOL_TOKEN_GREATER,
        "expected '>' after handled effect authority"
    ) && valid;
    valid = sol_parser_expect(parser, SOL_TOKEN_WITH, "expected 'with' after handled effect")
        && valid;
    bool previous_suppression = parser->suppress_record_literal;
    parser->suppress_record_literal = true;
    SolExprId provider = sol_parser_expression(parser, 1);
    parser->suppress_record_literal = previous_suppression;
    valid = sol_parser_kind(parser) == SOL_TOKEN_LEFT_BRACE && valid;
    if (sol_parser_kind(parser) != SOL_TOKEN_LEFT_BRACE) {
        sol_parser_error(
            parser,
            "SOL-PARSE-001",
            sol_parser_current(parser),
            "expected a handler body"
        );
    }
    SolExprId body = sol_parser_block_expression(parser);
    size_t end = body < parser->tree->expression_count
        ? parser->tree->expressions[body].span.end
        : handle.span.end;
    return sol_parser_add_expression(parser, (SolExpr){
        .kind = valid ? SOL_EXPR_HANDLE : SOL_EXPR_ERROR,
        .span = {.start = handle.span.start, .end = end},
        .as.handle = {
            .effect_name = effect_name,
            .authority = authority,
            .provider = provider,
            .body = body,
        },
    });
}

typedef struct {
    SolArgumentId first;
    SolToken closing;
    bool closed;
} SolParsedArguments;

static SolExprId sol_parser_nested_expression(SolParser *parser, unsigned int minimum_precedence) {
    bool previous_suppression = parser->suppress_record_literal;
    parser->suppress_record_literal = false;
    SolExprId expression = sol_parser_expression(parser, minimum_precedence);
    parser->suppress_record_literal = previous_suppression;
    return expression;
}

static unsigned int sol_parser_binary_precedence(SolTokenKind kind) {
    switch (kind) {
        case SOL_TOKEN_PIPE_PIPE: return 1;
        case SOL_TOKEN_AMP_AMP: return 2;
        case SOL_TOKEN_EQUAL_EQUAL:
        case SOL_TOKEN_BANG_EQUAL: return 3;
        case SOL_TOKEN_LESS:
        case SOL_TOKEN_LESS_EQUAL:
        case SOL_TOKEN_GREATER:
        case SOL_TOKEN_GREATER_EQUAL: return 4;
        case SOL_TOKEN_PLUS:
        case SOL_TOKEN_MINUS: return 5;
        case SOL_TOKEN_STAR:
        case SOL_TOKEN_SLASH:
        case SOL_TOKEN_PERCENT: return 6;
        default: return 0;
    }
}

static bool sol_parser_has_line_break(SolParser *parser, size_t start, size_t end) {
    for (size_t index = start; index < end; ++index) {
        char byte = parser->source->text[index];
        if (byte == '\n' || byte == '\r') {
            return true;
        }
    }
    return false;
}

static size_t sol_parser_next_significant(const SolParser *parser, size_t index) {
    while (index < parser->tokens->count
        && sol_token_is_trivia(parser->tokens->items[index].kind)) {
        ++index;
    }
    return index;
}

typedef enum {
    SOL_TYPE_LOOKAHEAD_NONE,
    SOL_TYPE_LOOKAHEAD_VALID,
    SOL_TYPE_LOOKAHEAD_MALFORMED,
} SolTypeLookaheadKind;

typedef struct {
    SolTypeLookaheadKind kind;
    size_t closing;
} SolTypeLookahead;

static SolTokenKind sol_parser_lookahead_kind(const SolParser *parser, size_t *index) {
    *index = sol_parser_next_significant(parser, *index);
    return *index < parser->tokens->count
        ? parser->tokens->items[*index].kind
        : SOL_TOKEN_EOF;
}

static bool sol_parser_lookahead_type(const SolParser *parser, size_t *index, size_t depth);

static bool sol_parser_lookahead_type_list(
    const SolParser *parser,
    size_t *index,
    SolTokenKind closing,
    size_t depth
) {
    if (sol_parser_lookahead_kind(parser, index) == closing) {
        ++*index;
        return true;
    }
    for (;;) {
        if (!sol_parser_lookahead_type(parser, index, depth + 1)) return false;
        SolTokenKind kind = sol_parser_lookahead_kind(parser, index);
        if (kind == closing) {
            ++*index;
            return true;
        }
        if (kind != SOL_TOKEN_COMMA) return false;
        ++*index;
        if (sol_parser_lookahead_kind(parser, index) == closing) {
            ++*index;
            return true;
        }
    }
}

static bool sol_parser_lookahead_type(const SolParser *parser, size_t *index, size_t depth) {
    if (depth >= 256) return false;
    SolTokenKind kind = sol_parser_lookahead_kind(parser, index);
    if (kind == SOL_TOKEN_BORROW || kind == SOL_TOKEN_INOUT) {
        ++*index;
        return sol_parser_lookahead_type(parser, index, depth + 1);
    }
    if (kind == SOL_TOKEN_FUNCTION) {
        ++*index;
        if (sol_parser_lookahead_kind(parser, index) != SOL_TOKEN_LEFT_PAREN) return false;
        ++*index;
        if (!sol_parser_lookahead_type_list(
            parser,
            index,
            SOL_TOKEN_RIGHT_PAREN,
            depth
        )) return false;
        if (sol_parser_lookahead_kind(parser, index) != SOL_TOKEN_ARROW) return false;
        ++*index;
        if (!sol_parser_lookahead_type(parser, index, depth + 1)) return false;
        if (sol_parser_lookahead_kind(parser, index) != SOL_TOKEN_EFFECTS) return false;
        ++*index;
        if (sol_parser_lookahead_kind(parser, index) == SOL_TOKEN_IDENTIFIER) {
            ++*index;
            return true;
        }
        if (sol_parser_lookahead_kind(parser, index) != SOL_TOKEN_LEFT_BRACE) return false;
        size_t braces = 0;
        do {
            kind = sol_parser_lookahead_kind(parser, index);
            if (kind == SOL_TOKEN_EOF) return false;
            if (kind == SOL_TOKEN_LEFT_BRACE) ++braces;
            if (kind == SOL_TOKEN_RIGHT_BRACE) --braces;
            ++*index;
        } while (braces != 0);
        return true;
    }
    if (kind == SOL_TOKEN_LEFT_PAREN) {
        ++*index;
        if (sol_parser_lookahead_kind(parser, index) != SOL_TOKEN_RIGHT_PAREN) return false;
        ++*index;
        return true;
    }
    if (kind == SOL_TOKEN_CAPABILITY) {
        ++*index;
        kind = sol_parser_lookahead_kind(parser, index);
    }
    if (kind != SOL_TOKEN_IDENTIFIER) return false;
    ++*index;
    while (sol_parser_lookahead_kind(parser, index) == SOL_TOKEN_DOT) {
        ++*index;
        if (sol_parser_lookahead_kind(parser, index) != SOL_TOKEN_IDENTIFIER) return false;
        ++*index;
    }
    if (sol_parser_lookahead_kind(parser, index) == SOL_TOKEN_LESS) {
        ++*index;
        if (!sol_parser_lookahead_type_list(
            parser,
            index,
            SOL_TOKEN_GREATER,
            depth
        )) return false;
    }
    return true;
}

static SolTypeLookahead sol_parser_expression_type_arguments(SolParser *parser) {
    size_t index = sol_parser_next_significant(parser, parser->cursor);
    if (index >= parser->tokens->count
        || parser->tokens->items[index].kind != SOL_TOKEN_LESS) {
        return (SolTypeLookahead){0};
    }
    ++index;
    if (!sol_parser_lookahead_type(parser, &index, 0)) {
        return (SolTypeLookahead){0};
    }
    for (;;) {
        SolTokenKind kind = sol_parser_lookahead_kind(parser, &index);
        if (kind == SOL_TOKEN_GREATER) {
            size_t closing = index++;
            kind = sol_parser_lookahead_kind(parser, &index);
            if (kind == SOL_TOKEN_LEFT_PAREN || kind == SOL_TOKEN_DOT
                || kind == SOL_TOKEN_LEFT_BRACE) {
                return (SolTypeLookahead){SOL_TYPE_LOOKAHEAD_VALID, closing};
            }
            return (SolTypeLookahead){0};
        }
        if (kind == SOL_TOKEN_COMMA) {
            ++index;
            if (sol_parser_lookahead_kind(parser, &index) == SOL_TOKEN_GREATER) continue;
            if (!sol_parser_lookahead_type(parser, &index, 0)) {
                return (SolTypeLookahead){SOL_TYPE_LOOKAHEAD_MALFORMED, 0};
            }
            continue;
        }
        if (kind == SOL_TOKEN_LEFT_PAREN) {
            return (SolTypeLookahead){SOL_TYPE_LOOKAHEAD_MALFORMED, 0};
        }
        return (SolTypeLookahead){0};
    }
}

static void sol_parser_recover_expression_type_arguments(SolParser *parser) {
    sol_parser_match(parser, SOL_TOKEN_LESS);
    size_t depth = 0;
    while (sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        SolTokenKind kind = sol_parser_kind(parser);
        if (kind == SOL_TOKEN_LESS) {
            ++depth;
        } else if (kind == SOL_TOKEN_GREATER) {
            if (depth == 0) {
                sol_parser_advance(parser);
                return;
            }
            --depth;
        } else if (kind == SOL_TOKEN_LEFT_PAREN && depth == 0) {
            return;
        } else if (kind == SOL_TOKEN_RIGHT_BRACE && depth == 0) {
            return;
        }
        sol_parser_advance(parser);
    }
}

static SolParsedArguments sol_parser_argument_list(SolParser *parser, SolTokenKind closing) {
    SolArgumentId first = SOL_AST_NONE;
    SolArgumentId last = SOL_AST_NONE;
    if (sol_parser_match(parser, closing)) {
        return (SolParsedArguments){
            .first = first,
            .closing = parser->tokens->items[parser->cursor - 1],
            .closed = true,
        };
    }

    for (;;) {
        SolSpan name = {0};
        bool is_named = sol_parser_kind(parser) == SOL_TOKEN_IDENTIFIER
            && sol_parser_peek_kind(parser, 1) == SOL_TOKEN_EQUAL;
        if (is_named) {
            name = sol_parser_advance(parser).span;
            sol_parser_advance(parser);
        }

        SolExprId value = sol_parser_nested_expression(parser, 1);
        SolArgumentId argument = sol_parser_add_argument(parser, (SolArgument){
            .name = name,
            .value = value,
            .next = SOL_AST_NONE,
            .is_named = is_named,
        });
        if (argument == SOL_AST_NONE) {
            return (SolParsedArguments){.first = first, .closing = sol_parser_current(parser)};
        }
        if (first == SOL_AST_NONE) {
            first = argument;
        } else {
            parser->tree->arguments[last].next = argument;
        }
        last = argument;

        if (sol_parser_match(parser, closing)) {
            return (SolParsedArguments){
                .first = first,
                .closing = parser->tokens->items[parser->cursor - 1],
                .closed = true,
            };
        }
        if (!sol_parser_expect(parser, SOL_TOKEN_COMMA, "expected ',' between arguments")) {
            return (SolParsedArguments){.first = first, .closing = sol_parser_current(parser)};
        }
        if (sol_parser_match(parser, closing)) {
            return (SolParsedArguments){
                .first = first,
                .closing = parser->tokens->items[parser->cursor - 1],
                .closed = true,
            };
        }
    }
}

static SolParsedArguments sol_parser_record_fields(SolParser *parser) {
    SolArgumentId first = SOL_AST_NONE;
    SolArgumentId last = SOL_AST_NONE;
    if (sol_parser_match(parser, SOL_TOKEN_RIGHT_BRACE)) {
        return (SolParsedArguments){
            .first = first,
            .closing = parser->tokens->items[parser->cursor - 1],
            .closed = true,
        };
    }

    for (;;) {
        SolToken field = sol_parser_current(parser);
        if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a record field name")) {
            return (SolParsedArguments){.first = first, .closing = sol_parser_current(parser)};
        }
        SolExprId value;
        if (sol_parser_match(parser, SOL_TOKEN_EQUAL)) {
            value = sol_parser_nested_expression(parser, 1);
        } else {
            value = sol_parser_add_expression(parser, (SolExpr){
                .kind = SOL_EXPR_PATH,
                .span = field.span,
                .as.name = field.span,
            });
        }

        SolArgumentId argument = sol_parser_add_argument(parser, (SolArgument){
            .name = field.span,
            .value = value,
            .next = SOL_AST_NONE,
            .is_named = true,
        });
        if (argument == SOL_AST_NONE) {
            return (SolParsedArguments){.first = first, .closing = sol_parser_current(parser)};
        }
        if (first == SOL_AST_NONE) {
            first = argument;
        } else {
            parser->tree->arguments[last].next = argument;
        }
        last = argument;

        if (sol_parser_match(parser, SOL_TOKEN_RIGHT_BRACE)) {
            return (SolParsedArguments){
                .first = first,
                .closing = parser->tokens->items[parser->cursor - 1],
                .closed = true,
            };
        }
        if (!sol_parser_expect(parser, SOL_TOKEN_COMMA, "expected ',' between record fields")) {
            return (SolParsedArguments){.first = first, .closing = sol_parser_current(parser)};
        }
        if (sol_parser_match(parser, SOL_TOKEN_RIGHT_BRACE)) {
            return (SolParsedArguments){
                .first = first,
                .closing = parser->tokens->items[parser->cursor - 1],
                .closed = true,
            };
        }
    }
}

static SolPatternId sol_parser_pattern(SolParser *parser) {
    SolToken token = sol_parser_current(parser);
    if (token.kind == SOL_TOKEN_TRUE || token.kind == SOL_TOKEN_FALSE) {
        sol_parser_advance(parser);
        return sol_parser_add_pattern(parser, (SolPattern){
            .kind = SOL_PATTERN_BOOL,
            .span = token.span,
            .bool_value = token.kind == SOL_TOKEN_TRUE,
            .first_binding = SOL_AST_NONE,
        });
    }
    if (token.kind != SOL_TOKEN_IDENTIFIER) {
        sol_parser_error(parser, "SOL-PARSE-016", token, "expected a match pattern");
        if (token.kind != SOL_TOKEN_EOF) sol_parser_advance(parser);
        return SOL_AST_NONE;
    }
    sol_parser_advance(parser);
    if (sol_token_text_equal(parser->source, token, "_")) {
        return sol_parser_add_pattern(parser, (SolPattern){
            .kind = SOL_PATTERN_WILDCARD,
            .span = token.span,
            .first_binding = SOL_AST_NONE,
        });
    }

    SolPatternBindingId first_binding = SOL_AST_NONE;
    SolPatternBindingId last_binding = SOL_AST_NONE;
    size_t end = token.span.end;
    if (sol_parser_match(parser, SOL_TOKEN_LEFT_PAREN)) {
        if (!sol_parser_match(parser, SOL_TOKEN_RIGHT_PAREN)) {
            for (;;) {
                SolToken binding_name = sol_parser_current(parser);
                if (!sol_parser_expect(
                    parser,
                    SOL_TOKEN_IDENTIFIER,
                    "expected a pattern binding"
                )) {
                    return SOL_AST_NONE;
                }
                SolPatternBindingId binding = sol_parser_add_pattern_binding(
                    parser,
                    (SolPatternBinding){
                        .name = binding_name.span,
                        .next = SOL_AST_NONE,
                    }
                );
                if (binding == SOL_AST_NONE) return SOL_AST_NONE;
                if (first_binding == SOL_AST_NONE) {
                    first_binding = binding;
                } else {
                    parser->tree->pattern_bindings[last_binding].next = binding;
                }
                last_binding = binding;
                if (sol_parser_match(parser, SOL_TOKEN_RIGHT_PAREN)) break;
                if (!sol_parser_expect(
                    parser,
                    SOL_TOKEN_COMMA,
                    "expected ',' between pattern bindings"
                )) {
                    return SOL_AST_NONE;
                }
                if (sol_parser_match(parser, SOL_TOKEN_RIGHT_PAREN)) break;
            }
        }
        end = parser->tokens->items[parser->cursor - 1].span.end;
    }
    return sol_parser_add_pattern(parser, (SolPattern){
        .kind = SOL_PATTERN_VARIANT,
        .span = {.start = token.span.start, .end = end},
        .name = token.span,
        .first_binding = first_binding,
    });
}

static SolExprId sol_parser_match_expression(SolParser *parser) {
    SolToken match_token = sol_parser_advance(parser);
    bool previous_suppression = parser->suppress_record_literal;
    parser->suppress_record_literal = true;
    SolExprId scrutinee = sol_parser_expression(parser, 1);
    parser->suppress_record_literal = previous_suppression;
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE, "expected '{' after match value")) {
        return sol_parser_add_expression(parser, (SolExpr){
            .kind = SOL_EXPR_ERROR,
            .span = match_token.span,
        });
    }

    SolMatchArmId first_arm = SOL_AST_NONE;
    SolMatchArmId last_arm = SOL_AST_NONE;
    while (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE
        && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        SolPatternId pattern = sol_parser_pattern(parser);
        if (pattern == SOL_AST_NONE) break;
        sol_parser_expect(parser, SOL_TOKEN_FAT_ARROW, "expected '=>' after match pattern");
        SolExprId value = sol_parser_nested_expression(parser, 1);
        SolSpan arm_span = {
            .start = parser->tree->patterns[pattern].span.start,
            .end = value == SOL_AST_NONE
                ? parser->tree->patterns[pattern].span.end
                : parser->tree->expressions[value].span.end,
        };
        SolMatchArmId arm = sol_parser_add_match_arm(parser, (SolMatchArm){
            .pattern = pattern,
            .value = value,
            .span = arm_span,
            .next = SOL_AST_NONE,
        });
        if (arm == SOL_AST_NONE) break;
        if (first_arm == SOL_AST_NONE) first_arm = arm;
        else parser->tree->match_arms[last_arm].next = arm;
        last_arm = arm;
        sol_parser_match(parser, SOL_TOKEN_COMMA);
    }
    SolToken closing = sol_parser_current(parser);
    sol_parser_expect(parser, SOL_TOKEN_RIGHT_BRACE, "expected '}' after match arms");
    return sol_parser_add_expression(parser, (SolExpr){
        .kind = SOL_EXPR_MATCH,
        .span = {.start = match_token.span.start, .end = closing.span.end},
        .as.match_expr = {.scrutinee = scrutinee, .first_arm = first_arm},
    });
}

static SolExprId sol_parser_primary_expression(SolParser *parser) {
    SolToken token = sol_parser_current(parser);
    if (token.kind == SOL_TOKEN_REGION) {
        sol_parser_error(parser,
            parser->contract_context == SOL_PARSER_CONTRACT_NONE
                ? "SOL-PARSE-011" : "SOL-PARSE-022",
            token,
            parser->contract_context == SOL_PARSER_CONTRACT_NONE
                ? "a region is a statement and is not allowed in expression position"
                : "region statements are not allowed in contract or refinement predicates");
        sol_parser_advance(parser);
        return sol_parser_add_expression(parser, (SolExpr){
            .kind = SOL_EXPR_ERROR,
            .span = token.span,
        });
    }
    if (token.kind == SOL_TOKEN_INTEGER) {
        sol_parser_advance(parser);
        return sol_parser_add_expression(parser, (SolExpr){
            .kind = SOL_EXPR_INTEGER,
            .span = token.span,
        });
    }
    if (token.kind == SOL_TOKEN_STRING) {
        sol_parser_advance(parser);
        return sol_parser_add_expression(parser, (SolExpr){
            .kind = SOL_EXPR_STRING,
            .span = token.span,
        });
    }
    if (token.kind == SOL_TOKEN_TRUE || token.kind == SOL_TOKEN_FALSE) {
        sol_parser_advance(parser);
        return sol_parser_add_expression(parser, (SolExpr){
            .kind = SOL_EXPR_BOOL,
            .span = token.span,
            .as.bool_value = token.kind == SOL_TOKEN_TRUE,
        });
    }
    if (token.kind == SOL_TOKEN_IDENTIFIER
        && parser->contract_context != SOL_PARSER_CONTRACT_NONE
        && sol_token_text_equal(parser->source, token, "result")) {
        sol_parser_advance(parser);
        if (parser->contract_context != SOL_PARSER_CONTRACT_ENSURES) {
            sol_parser_error(
                parser,
                "SOL-PARSE-017",
                token,
                "'result' is only available in ensures clauses"
            );
        }
        return sol_parser_add_expression(parser, (SolExpr){
            .kind = SOL_EXPR_RESULT,
            .span = token.span,
        });
    }
    if (token.kind == SOL_TOKEN_IDENTIFIER
        && parser->contract_context != SOL_PARSER_CONTRACT_NONE
        && sol_token_text_equal(parser->source, token, "old")) {
        sol_parser_advance(parser);
        if (parser->contract_context != SOL_PARSER_CONTRACT_ENSURES) {
            sol_parser_error(
                parser,
                "SOL-PARSE-017",
                token,
                "'old' is only available in ensures clauses"
            );
        }
        if (!sol_parser_expect(
            parser,
            SOL_TOKEN_LEFT_PAREN,
            "expected '(' after 'old'"
        )) {
            return sol_parser_add_expression(parser, (SolExpr){
                .kind = SOL_EXPR_ERROR,
                .span = token.span,
            });
        }
        SolExprId operand = sol_parser_nested_expression(parser, 1);
        SolToken closing = sol_parser_current(parser);
        sol_parser_expect(parser, SOL_TOKEN_RIGHT_PAREN, "expected ')' after old expression");
        return sol_parser_add_expression(parser, (SolExpr){
            .kind = SOL_EXPR_OLD,
            .span = {.start = token.span.start, .end = closing.span.end},
            .as.old_expression = operand,
        });
    }
    if (token.kind == SOL_TOKEN_IDENTIFIER) {
        sol_parser_advance(parser);
        return sol_parser_add_expression(parser, (SolExpr){
            .kind = SOL_EXPR_PATH,
            .span = token.span,
            .as.name = token.span,
        });
    }
    if (token.kind == SOL_TOKEN_LEFT_BRACE) {
        return sol_parser_block_expression(parser);
    }
    if (token.kind == SOL_TOKEN_LEFT_PAREN) {
        SolToken opening = sol_parser_advance(parser);
        if (sol_parser_match(parser, SOL_TOKEN_RIGHT_PAREN)) {
            SolToken closing = parser->tokens->items[parser->cursor - 1];
            return sol_parser_add_expression(parser, (SolExpr){
                .kind = SOL_EXPR_UNIT,
                .span = {.start = opening.span.start, .end = closing.span.end},
            });
        }
        SolExprId expression = sol_parser_nested_expression(parser, 1);
        sol_parser_expect(parser, SOL_TOKEN_RIGHT_PAREN, "expected ')' after expression");
        return expression;
    }
    if (token.kind == SOL_TOKEN_IF) {
        SolToken if_token = sol_parser_advance(parser);
        bool previous_suppression = parser->suppress_record_literal;
        parser->suppress_record_literal = true;
        SolExprId condition = sol_parser_expression(parser, 1);
        parser->suppress_record_literal = previous_suppression;
        SolExprId then_branch = sol_parser_block_expression(parser);
        SolExprId else_branch = SOL_AST_NONE;
        if (!sol_parser_match(parser, SOL_TOKEN_ELSE)) {
            sol_parser_error(
                parser,
                "SOL-PARSE-012",
                sol_parser_current(parser),
                "an expression-valued 'if' requires an 'else' branch"
            );
            else_branch = sol_parser_add_expression(parser, (SolExpr){
                .kind = SOL_EXPR_ERROR,
                .span = sol_parser_current(parser).span,
            });
        } else if (sol_parser_kind(parser) == SOL_TOKEN_IF) {
            else_branch = sol_parser_nested_expression(parser, 1);
        } else {
            else_branch = sol_parser_block_expression(parser);
        }
        size_t end = then_branch == SOL_AST_NONE
            ? if_token.span.end
            : parser->tree->expressions[then_branch].span.end;
        if (else_branch != SOL_AST_NONE) {
            end = parser->tree->expressions[else_branch].span.end;
        }
        return sol_parser_add_expression(parser, (SolExpr){
            .kind = SOL_EXPR_IF,
            .span = {.start = if_token.span.start, .end = end},
            .as.if_expr = {
                .condition = condition,
                .then_branch = then_branch,
                .else_branch = else_branch,
            },
        });
    }
    if (token.kind == SOL_TOKEN_MATCH) {
        return sol_parser_match_expression(parser);
    }
    if (token.kind == SOL_TOKEN_HANDLE) {
        return sol_parser_handle_expression(parser);
    }

    sol_parser_error(parser, "SOL-PARSE-011", token, "expected an expression");
    if (token.kind != SOL_TOKEN_EOF
        && token.kind != SOL_TOKEN_RIGHT_BRACE
        && token.kind != SOL_TOKEN_RIGHT_PAREN
        && token.kind != SOL_TOKEN_COMMA) {
        sol_parser_advance(parser);
    }
    return sol_parser_add_expression(parser, (SolExpr){
        .kind = SOL_EXPR_ERROR,
        .span = token.span,
    });
}

static SolExprId sol_parser_postfix_expression(SolParser *parser) {
    SolExprId expression = sol_parser_primary_expression(parser);
    while (expression != SOL_AST_NONE) {
        SolTokenKind kind = sol_parser_kind(parser);
        SolExprKind expression_kind = parser->tree->expressions[expression].kind;
        bool path_like = expression_kind == SOL_EXPR_PATH
            || expression_kind == SOL_EXPR_FIELD
            || expression_kind == SOL_EXPR_TYPE_APPLICATION;
        SolTypeLookahead type_arguments = kind == SOL_TOKEN_LESS && path_like
            ? sol_parser_expression_type_arguments(parser)
            : (SolTypeLookahead){0};
        if (type_arguments.kind == SOL_TYPE_LOOKAHEAD_VALID) {
            SolTypeArgumentId first_argument;
            if (!sol_parser_type_arguments(parser, &first_argument)) break;
            size_t end = parser->tokens->items[type_arguments.closing].span.end;
            expression = sol_parser_add_expression(parser, (SolExpr){
                .kind = SOL_EXPR_TYPE_APPLICATION,
                .span = {
                    .start = parser->tree->expressions[expression].span.start,
                    .end = end,
                },
                .as.type_application = {
                    .base = expression,
                    .first_argument = first_argument,
                },
            });
        } else if (type_arguments.kind == SOL_TYPE_LOOKAHEAD_MALFORMED) {
            sol_parser_error(
                parser,
                "SOL-PARSE-019",
                sol_parser_current(parser),
                "malformed explicit type argument list; expected ',' or '>'"
            );
            sol_parser_recover_expression_type_arguments(parser);
        } else if (kind == SOL_TOKEN_LEFT_PAREN) {
            sol_parser_advance(parser);
            SolParsedArguments arguments = sol_parser_argument_list(parser, SOL_TOKEN_RIGHT_PAREN);
            if (!arguments.closed) {
                expression = sol_parser_add_expression(parser, (SolExpr){
                    .kind = SOL_EXPR_ERROR,
                    .span = {
                        .start = parser->tree->expressions[expression].span.start,
                        .end = arguments.closing.span.end,
                    },
                });
                break;
            }
            expression = sol_parser_add_expression(parser, (SolExpr){
                .kind = SOL_EXPR_CALL,
                .span = {
                    .start = parser->tree->expressions[expression].span.start,
                    .end = arguments.closing.span.end,
                },
                .as.call = {.callee = expression, .first_argument = arguments.first},
            });
        } else if (kind == SOL_TOKEN_DOT) {
            sol_parser_advance(parser);
            SolToken name = sol_parser_current(parser);
            if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a field name after '.'")) {
                return expression;
            }
            expression = sol_parser_add_expression(parser, (SolExpr){
                .kind = SOL_EXPR_FIELD,
                .span = {
                    .start = parser->tree->expressions[expression].span.start,
                    .end = name.span.end,
                },
                .as.field = {.base = expression, .name = name.span},
            });
        } else if (kind == SOL_TOKEN_LEFT_BRACE
            && !parser->suppress_record_literal
            && !sol_parser_has_line_break(
                parser,
                parser->tree->expressions[expression].span.end,
                sol_parser_current(parser).span.start
            )) {
            if (expression_kind != SOL_EXPR_PATH && expression_kind != SOL_EXPR_FIELD
                && expression_kind != SOL_EXPR_TYPE_APPLICATION) {
                break;
            }
            sol_parser_advance(parser);
            SolParsedArguments fields = sol_parser_record_fields(parser);
            if (!fields.closed) {
                expression = sol_parser_add_expression(parser, (SolExpr){
                    .kind = SOL_EXPR_ERROR,
                    .span = {
                        .start = parser->tree->expressions[expression].span.start,
                        .end = fields.closing.span.end,
                    },
                });
                break;
            }
            expression = sol_parser_add_expression(parser, (SolExpr){
                .kind = SOL_EXPR_RECORD,
                .span = {
                    .start = parser->tree->expressions[expression].span.start,
                    .end = fields.closing.span.end,
                },
                .as.record = {.type = expression, .first_field = fields.first},
            });
        } else if (kind == SOL_TOKEN_QUESTION) {
            SolToken question = sol_parser_advance(parser);
            expression = sol_parser_add_expression(parser, (SolExpr){
                .kind = SOL_EXPR_PROPAGATE,
                .span = {
                    .start = parser->tree->expressions[expression].span.start,
                    .end = question.span.end,
                },
                .as.propagated = expression,
            });
        } else {
            break;
        }
    }
    return expression;
}

static SolExprId sol_parser_unary_expression(SolParser *parser) {
    SolToken operator_token = sol_parser_current(parser);
    if (operator_token.kind != SOL_TOKEN_BANG && operator_token.kind != SOL_TOKEN_MINUS) {
        return sol_parser_postfix_expression(parser);
    }
    sol_parser_advance(parser);
    SolExprId operand = sol_parser_expression(parser, 7);
    size_t end = operand == SOL_AST_NONE
        ? operator_token.span.end
        : parser->tree->expressions[operand].span.end;
    return sol_parser_add_expression(parser, (SolExpr){
        .kind = SOL_EXPR_UNARY,
        .span = {.start = operator_token.span.start, .end = end},
        .as.unary = {.operator_kind = operator_token.kind, .operand = operand},
    });
}

static SolExprId sol_parser_expression(SolParser *parser, unsigned int minimum_precedence) {
    if (parser->expression_depth >= 256) {
        sol_diagnostics_add(
            parser->diagnostics,
            "SOL-PARSE-013",
            SOL_SEVERITY_ERROR,
            sol_parser_current(parser).span,
            "expression nesting exceeds the compiler limit of 256"
        );
        SolToken token = sol_parser_current(parser);
        return sol_parser_add_expression(parser, (SolExpr){
            .kind = SOL_EXPR_ERROR,
            .span = token.span,
        });
    }
    ++parser->expression_depth;

    SolExprId left = sol_parser_unary_expression(parser);
    for (;;) {
        SolToken operator_token = sol_parser_current(parser);
        unsigned int precedence = sol_parser_binary_precedence(operator_token.kind);
        if (precedence < minimum_precedence || precedence == 0) {
            break;
        }
        sol_parser_advance(parser);
        SolExprId right = sol_parser_expression(parser, precedence + 1);
        size_t end = right == SOL_AST_NONE
            ? operator_token.span.end
            : parser->tree->expressions[right].span.end;
        left = sol_parser_add_expression(parser, (SolExpr){
            .kind = SOL_EXPR_BINARY,
            .span = {
                .start = left == SOL_AST_NONE
                    ? operator_token.span.start
                    : parser->tree->expressions[left].span.start,
                .end = end,
            },
            .as.binary = {
                .left = left,
                .operator_kind = operator_token.kind,
                .right = right,
            },
        });
    }

    --parser->expression_depth;
    return left;
}

static void sol_parser_check_multiline_record(SolParser *parser, SolExprId value) {
    if (value != SOL_AST_NONE
        && sol_parser_kind(parser) == SOL_TOKEN_LEFT_BRACE
        && (parser->tree->expressions[value].kind == SOL_EXPR_PATH
            || parser->tree->expressions[value].kind == SOL_EXPR_FIELD
            || parser->tree->expressions[value].kind == SOL_EXPR_TYPE_APPLICATION)
        && sol_parser_has_line_break(
            parser,
            parser->tree->expressions[value].span.end,
            sol_parser_current(parser).span.start
        )) {
        sol_diagnostics_add(
            parser->diagnostics,
            "SOL-PARSE-015",
            SOL_SEVERITY_ERROR,
            sol_parser_current(parser).span,
            "a record literal's opening brace must be on the same line as its type"
        );
    }
}

static bool sol_parser_assignment_operator(SolTokenKind kind) {
    return kind == SOL_TOKEN_EQUAL || kind == SOL_TOKEN_PLUS_EQUAL
        || kind == SOL_TOKEN_MINUS_EQUAL || kind == SOL_TOKEN_STAR_EQUAL
        || kind == SOL_TOKEN_SLASH_EQUAL || kind == SOL_TOKEN_PERCENT_EQUAL;
}

static SolSpan sol_parser_loop_invariant_clause(
    SolParser *parser,
    SolToken keyword,
    SolLoopInvariantId *first_invariant
) {
    SolToken opening = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE,
            "expected an invariant block")) {
        return (SolSpan){.start = keyword.span.start, .end = keyword.span.end};
    }
    SolLoopInvariantId last = SOL_AST_NONE;
    while (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE
        && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        if (sol_parser_match(parser, SOL_TOKEN_COMMA)) {
            sol_parser_error(parser, "SOL-PARSE-020",
                parser->tokens->items[parser->cursor - 1],
                "expected an invariant expression before ','");
            continue;
        }
        size_t before = sol_parser_significant_index(parser);
        bool previous_suppression = parser->suppress_record_literal;
        parser->suppress_record_literal = true;
        SolExprId expression = sol_parser_expression(parser, 1);
        parser->suppress_record_literal = previous_suppression;
        if (expression == SOL_AST_NONE) break;
        SolSpan expression_span = parser->tree->expressions[expression].span;
        SolLoopInvariantId invariant = sol_parser_add_loop_invariant(
            parser,
            (SolLoopInvariant){
                .expression = expression,
                .span = expression_span,
                .next = SOL_AST_NONE,
            }
        );
        if (invariant == SOL_AST_NONE) break;
        if (*first_invariant == SOL_AST_NONE) *first_invariant = invariant;
        else parser->tree->loop_invariants[last].next = invariant;
        last = invariant;

        SolToken next = sol_parser_current(parser);
        if (next.kind == SOL_TOKEN_RIGHT_BRACE || next.kind == SOL_TOKEN_EOF) continue;
        if (sol_parser_match(parser, SOL_TOKEN_COMMA)) {
            if (sol_parser_kind(parser) == SOL_TOKEN_RIGHT_BRACE) {
                sol_parser_error(parser, "SOL-PARSE-020", sol_parser_current(parser),
                    "expected an invariant expression after ','");
            }
            continue;
        }
        if (!sol_parser_has_line_break(parser, expression_span.end, next.span.start)) {
            sol_parser_error(parser, "SOL-PARSE-020", next,
                "expected a newline or ',' between invariant expressions");
        }
        if (sol_parser_significant_index(parser) <= before
            && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
            sol_parser_advance(parser);
        }
    }
    SolToken closing = sol_parser_current(parser);
    if (*first_invariant == SOL_AST_NONE) {
        sol_parser_error(parser, "SOL-PARSE-020", closing,
            "an invariant block requires at least one expression");
    }
    sol_parser_expect(parser, SOL_TOKEN_RIGHT_BRACE,
        "expected '}' after invariant expressions");
    return (SolSpan){
        .start = keyword.span.start,
        .end = closing.kind == SOL_TOKEN_RIGHT_BRACE ? closing.span.end : opening.span.end,
    };
}

static SolSpan sol_parser_loop_decreases_clause(
    SolParser *parser,
    SolToken keyword,
    SolExprId *decreases
) {
    SolToken opening = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE,
            "expected a decreases block")) {
        return (SolSpan){.start = keyword.span.start, .end = keyword.span.end};
    }
    if (sol_parser_kind(parser) == SOL_TOKEN_RIGHT_BRACE) {
        sol_parser_error(parser, "SOL-PARSE-020", sol_parser_current(parser),
            "a decreases block requires exactly one expression");
    } else if (sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        bool previous_suppression = parser->suppress_record_literal;
        parser->suppress_record_literal = true;
        *decreases = sol_parser_expression(parser, 1);
        parser->suppress_record_literal = previous_suppression;
    }
    if (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE
        && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        sol_parser_error(parser, "SOL-PARSE-020", sol_parser_current(parser),
            "a decreases block contains exactly one expression");
        while (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE
            && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
            sol_parser_advance(parser);
        }
    }
    SolToken closing = sol_parser_current(parser);
    sol_parser_expect(parser, SOL_TOKEN_RIGHT_BRACE,
        "expected '}' after decreases expression");
    return (SolSpan){
        .start = keyword.span.start,
        .end = closing.kind == SOL_TOKEN_RIGHT_BRACE ? closing.span.end : opening.span.end,
    };
}

static SolStatementId sol_parser_statement(SolParser *parser) {
    SolToken start = sol_parser_current(parser);
    SolStatement statement = {
        .next = SOL_AST_NONE,
        .as.expression = SOL_AST_NONE,
    };
    if (sol_parser_kind(parser) == SOL_TOKEN_LET
        || sol_parser_kind(parser) == SOL_TOKEN_VAR) {
        bool mutable = sol_parser_kind(parser) == SOL_TOKEN_VAR;
        sol_parser_advance(parser);
        SolToken name = sol_parser_current(parser);
        sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER,
            mutable ? "expected a binding name after 'var'"
                    : "expected a binding name after 'let'");
        SolExprId value = SOL_AST_NONE;
        SolTypeId type_id = SOL_AST_NONE;
        SolSpan type_span = {0};
        if (mutable && sol_parser_match(parser, SOL_TOKEN_COLON)) {
            sol_parser_type(parser, &type_span, &type_id);
            if (sol_parser_match(parser, SOL_TOKEN_EQUAL)) {
                sol_parser_error(parser, "SOL-PARSE-001", sol_parser_current(parser),
                    "typed 'var' declarations cannot have an initializer");
                SolExprId rejected = sol_parser_expression(parser, 1);
                sol_parser_check_multiline_record(parser, rejected);
            }
        } else {
            sol_parser_expect(parser, SOL_TOKEN_EQUAL, "expected '=' after binding name");
            value = sol_parser_expression(parser, 1);
            sol_parser_check_multiline_record(parser, value);
        }
        statement.kind = mutable ? SOL_STATEMENT_VAR : SOL_STATEMENT_LET;
        statement.as.let_statement.name = name.span;
        statement.as.let_statement.value = value;
        statement.as.let_statement.type_id = type_id;
        statement.span = (SolSpan){
            .start = start.span.start,
            .end = type_id != SOL_AST_NONE ? type_span.end
                : value == SOL_AST_NONE ? name.span.end
                : parser->tree->expressions[value].span.end,
        };
    } else if (sol_parser_match(parser, SOL_TOKEN_PANIC)) {
        SolExprId message = sol_parser_expression(parser, 1);
        sol_parser_check_multiline_record(parser, message);
        statement.kind = SOL_STATEMENT_PANIC;
        statement.as.panic_statement.message = message;
        statement.span = (SolSpan){
            .start = start.span.start,
            .end = message == SOL_AST_NONE ? start.span.end
                : parser->tree->expressions[message].span.end,
        };
    } else if (sol_parser_match(parser, SOL_TOKEN_UNREACHABLE)) {
        SolToken because = sol_parser_current(parser);
        sol_parser_expect(parser, SOL_TOKEN_BECAUSE,
            "expected 'because' after 'unreachable'");
        SolToken opening = sol_parser_current(parser);
        SolExprId proof = SOL_AST_NONE;
        if (sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE,
                "expected a proof clause after 'because'")) {
            if (sol_parser_kind(parser) == SOL_TOKEN_RIGHT_BRACE) {
                sol_parser_error(parser, "SOL-PARSE-023", sol_parser_current(parser),
                    "an unreachable proof clause requires exactly one expression");
            } else if (sol_parser_kind(parser) != SOL_TOKEN_EOF) {
                bool previous_suppression = parser->suppress_record_literal;
                parser->suppress_record_literal = true;
                proof = sol_parser_expression(parser, 1);
                parser->suppress_record_literal = previous_suppression;
            }
            if (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE
                && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
                sol_parser_error(parser, "SOL-PARSE-023", sol_parser_current(parser),
                    "an unreachable proof clause contains exactly one expression");
                size_t brace_depth = 0;
                while (sol_parser_kind(parser) != SOL_TOKEN_EOF) {
                    if (sol_parser_kind(parser) == SOL_TOKEN_LEFT_BRACE) {
                        ++brace_depth;
                    } else if (sol_parser_kind(parser) == SOL_TOKEN_RIGHT_BRACE) {
                        if (brace_depth == 0) break;
                        --brace_depth;
                    }
                    sol_parser_advance(parser);
                }
            }
        }
        SolToken closing = sol_parser_current(parser);
        sol_parser_expect(parser, SOL_TOKEN_RIGHT_BRACE,
            "expected '}' after unreachable proof");
        statement.kind = SOL_STATEMENT_UNREACHABLE;
        statement.as.unreachable_statement.proof = proof;
        statement.as.unreachable_statement.because_span = (SolSpan){
            .start = because.span.start,
            .end = closing.kind == SOL_TOKEN_RIGHT_BRACE
                ? closing.span.end : opening.span.end,
        };
        statement.span = (SolSpan){
            .start = start.span.start,
            .end = closing.kind == SOL_TOKEN_RIGHT_BRACE
                ? closing.span.end : start.span.end,
        };
    } else if (sol_parser_match(parser, SOL_TOKEN_REQUIRE)) {
        bool previous_suppression = parser->suppress_record_literal;
        parser->suppress_record_literal = true;
        SolExprId condition = sol_parser_expression(parser, 1);
        parser->suppress_record_literal = previous_suppression;
        sol_parser_expect(parser, SOL_TOKEN_ELSE,
            "expected 'else' after require condition");
        SolExprId fallback = sol_parser_block_expression(parser);
        statement.kind = SOL_STATEMENT_REQUIRE;
        statement.as.require_statement.condition = condition;
        statement.as.require_statement.fallback_block = fallback;
        statement.span = (SolSpan){
            .start = start.span.start,
            .end = fallback == SOL_AST_NONE ? start.span.end
                : parser->tree->expressions[fallback].span.end,
        };
    } else if (sol_parser_match(parser, SOL_TOKEN_RETURN)) {
        SolExprId value = sol_parser_expression(parser, 1);
        sol_parser_check_multiline_record(parser, value);
        statement.kind = SOL_STATEMENT_RETURN;
        statement.as.expression = value;
        statement.span = (SolSpan){
            .start = start.span.start,
            .end = value == SOL_AST_NONE ? start.span.end : parser->tree->expressions[value].span.end,
        };
    } else if (sol_parser_match(parser, SOL_TOKEN_REGION)) {
        SolToken label = sol_parser_current(parser);
        sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER,
            "expected a region label after 'region'");
        SolExprId body = sol_parser_block_expression(parser);
        statement.kind = SOL_STATEMENT_REGION;
        statement.as.region_statement.label = label.span;
        statement.as.region_statement.body = body;
        statement.span = (SolSpan){
            .start = start.span.start,
            .end = body == SOL_AST_NONE ? label.span.end
                : parser->tree->expressions[body].span.end,
        };
    } else if (sol_parser_match(parser, SOL_TOKEN_MODIFY)) {
        bool previous_suppression = parser->suppress_record_literal;
        parser->suppress_record_literal = true;
        SolExprId target = sol_parser_expression(parser, 1);
        parser->suppress_record_literal = previous_suppression;
        SolExprId body = sol_parser_block_expression(parser);
        statement.kind = SOL_STATEMENT_MODIFY;
        statement.as.modify.target = target;
        statement.as.modify.body = body;
        statement.span = (SolSpan){
            .start = start.span.start,
            .end = body == SOL_AST_NONE ? start.span.end
                : parser->tree->expressions[body].span.end,
        };
    } else if (sol_parser_kind(parser) == SOL_TOKEN_LOOP
        || sol_parser_kind(parser) == SOL_TOKEN_WHILE) {
        bool conditional = sol_parser_kind(parser) == SOL_TOKEN_WHILE;
        sol_parser_advance(parser);
        ++parser->loop_depth;
        SolExprId condition = SOL_AST_NONE;
        SolLoopInvariantId first_invariant = SOL_AST_NONE;
        SolSpan invariant_span = {0};
        SolExprId decreases = SOL_AST_NONE;
        SolSpan decreases_span = {0};
        if (conditional) {
            bool previous_suppression = parser->suppress_record_literal;
            parser->suppress_record_literal = true;
            condition = sol_parser_expression(parser, 1);
            parser->suppress_record_literal = previous_suppression;
        }
        bool saw_invariant = false;
        bool saw_decreases = false;
        while (sol_parser_kind(parser) == SOL_TOKEN_INVARIANT
            || sol_parser_kind(parser) == SOL_TOKEN_DECREASES) {
            SolToken clause = sol_parser_advance(parser);
            if (clause.kind == SOL_TOKEN_INVARIANT) {
                if (saw_invariant) {
                    sol_parser_error(parser, "SOL-PARSE-020", clause,
                        "a loop may have at most one invariant clause");
                }
                if (saw_decreases) {
                    sol_parser_error(parser, "SOL-PARSE-020", clause,
                        "invariant must precede decreases");
                }
                SolLoopInvariantId parsed = SOL_AST_NONE;
                SolSpan span = sol_parser_loop_invariant_clause(parser, clause, &parsed);
                if (!saw_invariant) {
                    first_invariant = parsed;
                    invariant_span = span;
                }
                saw_invariant = true;
            } else {
                if (saw_decreases) {
                    sol_parser_error(parser, "SOL-PARSE-020", clause,
                        "a loop may have at most one decreases clause");
                }
                SolExprId parsed = SOL_AST_NONE;
                SolSpan span = sol_parser_loop_decreases_clause(parser, clause, &parsed);
                if (!saw_decreases) {
                    decreases = parsed;
                    decreases_span = span;
                }
                saw_decreases = true;
            }
        }
        SolExprId body = sol_parser_block_expression(parser);
        --parser->loop_depth;
        statement.kind = conditional ? SOL_STATEMENT_WHILE : SOL_STATEMENT_LOOP;
        statement.as.loop_statement.condition = condition;
        statement.as.loop_statement.first_invariant = first_invariant;
        statement.as.loop_statement.invariant_span = invariant_span;
        statement.as.loop_statement.decreases = decreases;
        statement.as.loop_statement.decreases_span = decreases_span;
        statement.as.loop_statement.body = body;
        statement.span = (SolSpan){
            .start = start.span.start,
            .end = body == SOL_AST_NONE ? start.span.end
                : parser->tree->expressions[body].span.end,
        };
    } else if (sol_parser_kind(parser) == SOL_TOKEN_BREAK
        || sol_parser_kind(parser) == SOL_TOKEN_CONTINUE) {
        bool is_break = sol_parser_kind(parser) == SOL_TOKEN_BREAK;
        sol_parser_advance(parser);
        statement.kind = is_break ? SOL_STATEMENT_BREAK : SOL_STATEMENT_CONTINUE;
        statement.span = start.span;
        if (parser->loop_depth == 0) {
            sol_parser_error(
                parser,
                "SOL-PARSE-001",
                start,
                is_break ? "'break' is only valid inside a loop"
                         : "'continue' is only valid inside a loop"
            );
        }
        if (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE) {
            sol_parser_error(
                parser,
                "SOL-PARSE-001",
                sol_parser_current(parser),
                is_break ? "'break' must be the final statement in its block"
                         : "'continue' must be the final statement in its block"
            );
        }
    } else {
        SolExprId value = sol_parser_expression(parser, 1);
        sol_parser_check_multiline_record(parser, value);
        SolTokenKind operator_kind = sol_parser_kind(parser);
        if (sol_parser_assignment_operator(operator_kind)) {
            sol_parser_advance(parser);
            SolExprId rhs = sol_parser_expression(parser, 1);
            sol_parser_check_multiline_record(parser, rhs);
            statement.kind = SOL_STATEMENT_ASSIGNMENT;
            statement.as.assignment.target = value;
            statement.as.assignment.value = rhs;
            statement.as.assignment.operator_kind = operator_kind;
            statement.span = (SolSpan){
                .start = value == SOL_AST_NONE ? start.span.start
                    : parser->tree->expressions[value].span.start,
                .end = rhs == SOL_AST_NONE ? start.span.end
                    : parser->tree->expressions[rhs].span.end,
            };
        } else {
            statement.kind = SOL_STATEMENT_EXPRESSION;
            statement.as.expression = value;
            statement.span = value == SOL_AST_NONE ? start.span
                : parser->tree->expressions[value].span;
        }
    }
    return sol_parser_add_statement(parser, statement);
}

static SolExprId sol_parser_block_expression(SolParser *parser) {
    SolToken opening = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE, "expected a block")) {
        return sol_parser_add_expression(parser, (SolExpr){
            .kind = SOL_EXPR_ERROR,
            .span = opening.span,
        });
    }
    bool previous_suppression = parser->suppress_record_literal;
    parser->suppress_record_literal = false;
    SolStatementId first = SOL_AST_NONE;
    SolStatementId last = SOL_AST_NONE;
    while (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE
        && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        size_t before = sol_parser_significant_index(parser);
        SolStatementId statement = sol_parser_statement(parser);
        if (statement != SOL_AST_NONE) {
            if (first == SOL_AST_NONE) {
                first = statement;
            } else {
                parser->tree->statements[last].next = statement;
            }
            last = statement;
        }
        sol_parser_significant_index(parser);
        if (parser->cursor <= before && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
            sol_parser_advance(parser);
        }
    }
    SolToken closing = sol_parser_current(parser);
    sol_parser_expect(parser, SOL_TOKEN_RIGHT_BRACE, "expected '}' after block");
    parser->suppress_record_literal = previous_suppression;
    return sol_parser_add_expression(parser, (SolExpr){
        .kind = SOL_EXPR_BLOCK,
        .span = {.start = opening.span.start, .end = closing.span.end},
        .as.block.first_statement = first,
    });
}

static bool sol_parser_balanced_block(SolParser *parser, const char *description) {
    SolToken opening = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE, description)) {
        return false;
    }
    size_t depth = 1;
    while (depth > 0 && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        SolTokenKind kind = sol_parser_advance(parser).kind;
        if (kind == SOL_TOKEN_LEFT_BRACE) {
            ++depth;
        } else if (kind == SOL_TOKEN_RIGHT_BRACE) {
            --depth;
        }
    }
    if (depth != 0) {
        sol_diagnostics_add(
            parser->diagnostics,
            "SOL-PARSE-002",
            SOL_SEVERITY_ERROR,
            opening.span,
            "unterminated block"
        );
        return false;
    }
    return true;
}

static bool sol_parser_contract_clause(
    SolParser *parser,
    SolContractClauseKind kind,
    SolContractOwnerKind owner_kind,
    size_t owner,
    SolToken clause_token,
    SolContractClauseId *clause_id
) {
    SolToken opening = sol_parser_current(parser);
    if (!sol_parser_expect(
        parser,
        SOL_TOKEN_LEFT_BRACE,
        "expected a block after the function clause"
    )) {
        return false;
    }
    SolContractClauseId clause = sol_parser_add_contract_clause(
        parser,
        (SolContractClause){
            .kind = kind,
            .span = {.start = clause_token.span.start, .end = opening.span.end},
            .first_condition = SOL_AST_NONE,
            .next = SOL_AST_NONE,
            .owner_kind = owner_kind,
            .owner = owner,
        }
    );
    if (clause == SOL_AST_NONE) return false;
    *clause_id = clause;

    SolParserContractContext previous_context = parser->contract_context;
    parser->contract_context = kind == SOL_CONTRACT_REQUIRES
        ? SOL_PARSER_CONTRACT_REQUIRES
        : SOL_PARSER_CONTRACT_ENSURES;
    SolContractConditionId last_condition = SOL_AST_NONE;
    while (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE
        && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        if (sol_parser_match(parser, SOL_TOKEN_COMMA)) {
            sol_parser_error(
                parser,
                "SOL-PARSE-017",
                parser->tokens->items[parser->cursor - 1],
                "expected a contract condition before ','"
            );
            continue;
        }
        SolToken first = sol_parser_current(parser);
        SolContractOutcomeKind outcome = SOL_CONTRACT_OUTCOME_ALWAYS;
        if (first.kind == SOL_TOKEN_IDENTIFIER
            && sol_parser_peek_kind(parser, 1) == SOL_TOKEN_FAT_ARROW
            && (sol_token_text_equal(parser->source, first, "success")
                || sol_token_text_equal(parser->source, first, "failure"))) {
            outcome = sol_token_text_equal(parser->source, first, "success")
                ? SOL_CONTRACT_OUTCOME_SUCCESS
                : SOL_CONTRACT_OUTCOME_FAILURE;
            sol_parser_advance(parser);
            sol_parser_advance(parser);
            if (kind != SOL_CONTRACT_ENSURES) {
                sol_parser_error(
                    parser,
                    "SOL-PARSE-017",
                    first,
                    "success and failure outcomes are only available in ensures clauses"
                );
            }
        }
        size_t before = sol_parser_significant_index(parser);
        SolExprId expression = sol_parser_expression(parser, 1);
        if (expression == SOL_AST_NONE) {
            parser->contract_context = previous_context;
            return false;
        }
        const SolExpr *root = &parser->tree->expressions[expression];
        SolContractConditionId condition = sol_parser_add_contract_condition(
            parser,
            (SolContractCondition){
                .outcome = outcome,
                .span = {.start = first.span.start, .end = root->span.end},
                .expression = expression,
                .next = SOL_AST_NONE,
                .owner_clause = clause,
            }
        );
        if (condition == SOL_AST_NONE) {
            parser->contract_context = previous_context;
            return false;
        }
        if (parser->tree->contract_clauses[clause].first_condition == SOL_AST_NONE) {
            parser->tree->contract_clauses[clause].first_condition = condition;
        } else {
            parser->tree->contract_conditions[last_condition].next = condition;
        }
        last_condition = condition;

        SolToken next = sol_parser_current(parser);
        if (next.kind == SOL_TOKEN_RIGHT_BRACE || next.kind == SOL_TOKEN_EOF) continue;
        if (sol_parser_match(parser, SOL_TOKEN_COMMA)) continue;
        if (!sol_parser_has_line_break(parser, root->span.end, next.span.start)) {
            sol_parser_error(
                parser,
                "SOL-PARSE-017",
                next,
                "expected a newline or ',' between contract conditions"
            );
        }
        if (sol_parser_significant_index(parser) <= before
            && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
            sol_parser_advance(parser);
        }
    }
    SolToken closing = sol_parser_current(parser);
    bool closed = sol_parser_expect(
        parser,
        SOL_TOKEN_RIGHT_BRACE,
        "expected '}' after contract conditions"
    );
    parser->tree->contract_clauses[clause].span.end = closing.span.end;
    parser->contract_context = previous_context;
    return closed;
}

static bool sol_parser_effect_clause(
    SolParser *parser,
    SolEffectOwnerKind owner_kind,
    size_t owner,
    SolEffectId *first_effect
) {
    *first_effect = SOL_AST_NONE;
    SolEffectId last_effect = SOL_AST_NONE;
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE, "expected an effects block")) {
        return false;
    }
    while (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE
        && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        SolToken first = sol_parser_current(parser);
        bool is_pure = sol_parser_match(parser, SOL_TOKEN_PURE);
        SolSpan name = first.span;
        bool panic_effect = !is_pure && sol_parser_match(parser, SOL_TOKEN_PANIC);
        if (!is_pure && !panic_effect
            && !sol_parser_path(parser, &name, "expected an effect name")) {
            return false;
        }
        SolSpan argument = {0};
        bool has_argument = false;
        size_t end = name.end;
        if (sol_parser_match(parser, SOL_TOKEN_LESS)) {
            has_argument = true;
            if (!sol_parser_path(parser, &argument, "expected an effect argument")) {
                return false;
            }
            SolToken closing = sol_parser_current(parser);
            if (!sol_parser_expect(
                parser,
                SOL_TOKEN_GREATER,
                "expected '>' after effect argument"
            )) {
                return false;
            }
            end = closing.span.end;
        }
        SolEffectId effect = sol_parser_add_effect(parser, (SolEffect){
            .name = name,
            .argument = argument,
            .span = {.start = first.span.start, .end = end},
            .next = SOL_AST_NONE,
            .owner_kind = owner_kind,
            .owner = owner,
            .is_pure = is_pure,
            .has_argument = has_argument,
        });
        if (effect == SOL_AST_NONE) return false;
        if (*first_effect == SOL_AST_NONE) *first_effect = effect;
        else parser->tree->effects[last_effect].next = effect;
        last_effect = effect;
        sol_parser_match(parser, SOL_TOKEN_COMMA);
    }
    return sol_parser_expect(parser, SOL_TOKEN_RIGHT_BRACE, "expected '}' after effects");
}

static bool sol_parser_named_type_list(
    SolParser *parser,
    SolTokenKind closing,
    bool retain_fields,
    SolFieldId *first_field
) {
    *first_field = SOL_AST_NONE;
    SolFieldId last_field = SOL_AST_NONE;
    if (sol_parser_match(parser, closing)) {
        return true;
    }
    for (;;) {
        SolToken name = sol_parser_current(parser);
        if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a name")) {
            return false;
        }
        if (!sol_parser_expect(parser, SOL_TOKEN_COLON, "expected ':' after the name")) {
            return false;
        }
        SolSpan type_span;
        SolTypeId type_id;
        if (!sol_parser_type(parser, &type_span, &type_id)) {
            return false;
        }
        if (retain_fields) {
            SolFieldId field = sol_parser_add_field(parser, (SolField){
                .name = name.span,
                .span = {.start = name.span.start, .end = type_span.end},
                .type = type_id,
                .next = SOL_AST_NONE,
            });
            if (field == SOL_AST_NONE) {
                return false;
            }
            if (*first_field == SOL_AST_NONE) {
                *first_field = field;
            } else {
                parser->tree->fields[last_field].next = field;
            }
            last_field = field;
        }
        if (sol_parser_match(parser, closing)) {
            return true;
        }
        if (!sol_parser_expect(parser, SOL_TOKEN_COMMA, "expected ',' between entries")) {
            return false;
        }
        if (sol_parser_match(parser, closing)) {
            return true;
        }
    }
}

static bool sol_parser_parameters(
    SolParser *parser,
    bool retain,
    SolParameterId *first_parameter
) {
    *first_parameter = SOL_AST_NONE;
    SolParameterId last_parameter = SOL_AST_NONE;
    if (sol_parser_match(parser, SOL_TOKEN_RIGHT_PAREN)) {
        return true;
    }
    for (;;) {
        SolToken name = sol_parser_current(parser);
        if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a parameter name")) {
            return false;
        }
        if (!sol_parser_expect(parser, SOL_TOKEN_COLON, "expected ':' after the parameter name")) {
            return false;
        }
        SolAccessMode access = SOL_ACCESS_OWNED;
        if (sol_parser_match(parser, SOL_TOKEN_BORROW)) access = SOL_ACCESS_SHARED;
        else if (sol_parser_match(parser, SOL_TOKEN_INOUT)) access = SOL_ACCESS_EXCLUSIVE;
        SolSpan type;
        SolTypeId type_id;
        if (!sol_parser_type(parser, &type, &type_id)) {
            return false;
        }
        if (retain) {
            SolParameterId parameter = sol_parser_add_parameter(parser, (SolParameter){
                .name = name.span,
                .type = type,
                .type_id = type_id,
                .next = SOL_AST_NONE,
                .access = access,
            });
            if (parameter == SOL_AST_NONE) {
                return false;
            }
            if (*first_parameter == SOL_AST_NONE) {
                *first_parameter = parameter;
            } else {
                parser->tree->parameters[last_parameter].next = parameter;
            }
            last_parameter = parameter;
        }
        if (sol_parser_match(parser, SOL_TOKEN_RIGHT_PAREN)) {
            return true;
        }
        if (!sol_parser_expect(parser, SOL_TOKEN_COMMA, "expected ',' between parameters")) {
            return false;
        }
        if (sol_parser_match(parser, SOL_TOKEN_RIGHT_PAREN)) {
            return true;
        }
    }
}

static bool sol_parser_generic_parameters(
    SolParser *parser,
    bool type_supported,
    bool effect_supported,
    SolTypeParameterId *first_parameter,
    SolEffectParameterId *first_effect_parameter
) {
    *first_parameter = SOL_AST_NONE;
    *first_effect_parameter = SOL_AST_NONE;
    if (!sol_parser_match(parser, SOL_TOKEN_LESS)) return true;
    SolToken opening = parser->tokens->items[parser->cursor - 1];
    if (!type_supported && !effect_supported) {
        sol_parser_error(
            parser,
            "SOL-PARSE-018",
            opening,
            "generic parameters are unsupported on this declaration"
        );
    }
    if (sol_parser_match(parser, SOL_TOKEN_GREATER)) {
        sol_parser_error(
            parser,
            "SOL-PARSE-018",
            parser->tokens->items[parser->cursor - 1],
            "generic parameter lists cannot be empty"
        );
        return true;
    }
    SolTypeParameterId last = SOL_AST_NONE;
    bool saw_effect = false;
    for (;;) {
        bool is_effect = sol_parser_match(parser, SOL_TOKEN_EFFECTS);
        if (is_effect && saw_effect) {
            sol_parser_error(
                parser, "SOL-PARSE-018", sol_parser_current(parser),
                "only one effect parameter is supported"
            );
        }
        if (!is_effect && saw_effect) {
            sol_parser_error(
                parser, "SOL-PARSE-018", sol_parser_current(parser),
                "type parameters must precede the effect parameter"
            );
        }
        SolToken name = sol_parser_current(parser);
        if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a generic parameter name")) {
            return false;
        }
        if (sol_token_text_equal(parser->source, name, "_")) {
            sol_parser_error(
                parser,
                "SOL-PARSE-018",
                name,
                "'_' is not a generic parameter name"
            );
        }
        SolSpan bound = {0};
        if (sol_parser_match(parser, SOL_TOKEN_COLON)) {
            SolToken bound_name = sol_parser_current(parser);
            if (!sol_parser_expect(
                parser,
                SOL_TOKEN_IDENTIFIER,
                "expected a trait name after ':'"
            )) return false;
            bound = bound_name.span;
            if (is_effect || !type_supported || !effect_supported) {
                sol_parser_error(
                    parser,
                    "SOL-PARSE-018",
                    bound_name,
                    "trait bounds are supported only on free-function type parameters"
                );
                bound = (SolSpan){0};
                size_t nested = 0;
                while (sol_parser_kind(parser) != SOL_TOKEN_EOF) {
                    SolTokenKind kind = sol_parser_kind(parser);
                    if (kind == SOL_TOKEN_LESS) {
                        ++nested;
                    } else if (kind == SOL_TOKEN_GREATER) {
                        if (nested == 0) break;
                        --nested;
                    } else if (kind == SOL_TOKEN_COMMA && nested == 0) {
                        break;
                    }
                    sol_parser_advance(parser);
                }
            }
        }
        if (sol_parser_kind(parser) == SOL_TOKEN_EQUAL) {
            sol_parser_error(
                parser,
                "SOL-PARSE-018",
                sol_parser_current(parser),
                "generic defaults are unsupported"
            );
            size_t nested = 0;
            while (sol_parser_kind(parser) != SOL_TOKEN_EOF) {
                SolTokenKind kind = sol_parser_kind(parser);
                if (kind == SOL_TOKEN_LESS) {
                    ++nested;
                } else if (kind == SOL_TOKEN_GREATER) {
                    if (nested == 0) break;
                    --nested;
                } else if (kind == SOL_TOKEN_COMMA && nested == 0) {
                    break;
                }
                sol_parser_advance(parser);
            }
        }
        if (is_effect && !effect_supported) {
            sol_parser_error(
                parser, "SOL-PARSE-018", name,
                "effect parameters are supported only on free functions"
            );
        } else if (is_effect && !saw_effect) {
            SolEffectParameterId parameter = sol_parser_add_effect_parameter(
                parser,
                (SolEffectParameter){
                    .name = name.span,
                    .next = SOL_AST_NONE,
                    .owner_item = parser->tree->item_count,
                }
            );
            if (parameter == SOL_AST_NONE) return false;
            *first_effect_parameter = parameter;
        } else if (!is_effect && type_supported) {
            SolTypeParameterId parameter = sol_parser_add_type_parameter(
                parser,
                (SolTypeParameter){
                    .name = name.span,
                    .bound = bound,
                    .next = SOL_AST_NONE,
                    .owner_item = parser->tree->item_count,
                }
            );
            if (parameter == SOL_AST_NONE) return false;
            if (*first_parameter == SOL_AST_NONE) *first_parameter = parameter;
            else parser->tree->type_parameters[last].next = parameter;
            last = parameter;
        }
        saw_effect = saw_effect || is_effect;
        if (sol_parser_match(parser, SOL_TOKEN_GREATER)) return true;
        if (!sol_parser_expect(
            parser,
            SOL_TOKEN_COMMA,
            "expected ',' between generic parameters"
        )) return false;
        if (sol_parser_match(parser, SOL_TOKEN_GREATER)) return true;
    }
}

static bool sol_parser_function(
    SolParser *parser,
    bool member,
    bool member_has_body,
    bool member_contracts,
    SolEffectOwnerKind member_effect_owner,
    size_t effect_owner,
    SolSpan *name,
    SolSpan *whole,
    SolExprId *body,
    SolParameterId *first_parameter,
    SolSpan *return_type,
    SolTypeId *return_type_id,
    SolEffectId *first_effect,
    bool *has_effect_clause,
    SolContractClauseId *first_contract,
    bool *result_authority_from_self,
    SolParameterId *result_authority_parameter,
    SolTypeParameterId *first_type_parameter,
    SolEffectParameterId *first_effect_parameter
) {
    *body = SOL_AST_NONE;
    *first_parameter = SOL_AST_NONE;
    *return_type = (SolSpan){0};
    *return_type_id = SOL_AST_NONE;
    *first_effect = SOL_AST_NONE;
    *has_effect_clause = false;
    *first_contract = SOL_AST_NONE;
    *result_authority_from_self = false;
    *result_authority_parameter = SOL_AST_NONE;
    *first_type_parameter = SOL_AST_NONE;
    *first_effect_parameter = SOL_AST_NONE;
    SolToken function_token = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_FUNCTION, "expected 'function'")) {
        return false;
    }
    if (!sol_parser_path(parser, name, "expected a function name")) {
        return false;
    }
    if (!sol_parser_generic_parameters(
        parser, !member, !member, first_type_parameter, first_effect_parameter
    )) return false;
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_PAREN, "expected '(' after the function name")) {
        return false;
    }
    if (!sol_parser_parameters(parser, true, first_parameter)) {
        return false;
    }
    if (!sol_parser_expect(parser, SOL_TOKEN_ARROW, "expected '->' after function parameters")) {
        return false;
    }
    if (!sol_parser_type(parser, return_type, return_type_id)) {
        return false;
    }

    unsigned int last_clause = 0;
    bool seen[4] = {false, false, false, false};
    SolContractClauseId last_contract = SOL_AST_NONE;
    for (;;) {
        SolTokenKind kind = sol_parser_kind(parser);
        SolToken current = sol_parser_current(parser);
        if (kind == SOL_TOKEN_IDENTIFIER
            && sol_token_text_equal(parser->source, current, "authority")) {
            sol_parser_advance(parser);
            if (member && !member_contracts) {
                sol_parser_error(
                    parser,
                    "SOL-PARSE-019",
                    current,
                    "trait and implementation methods cannot declare authority clauses"
                );
                if (!sol_parser_balanced_block(parser, "expected an authority clause")) {
                    return false;
                }
                continue;
            }
            if (*result_authority_from_self
                || *result_authority_parameter != SOL_AST_NONE) {
                sol_parser_error(
                    parser,
                    "SOL-PARSE-016",
                    current,
                    "duplicate return authority clause"
                );
            }
            bool valid_authority = sol_parser_expect(
                parser,
                SOL_TOKEN_LEFT_BRACE,
                "expected '{' after authority"
            );
            valid_authority = valid_authority && sol_parser_expect_identifier_text(
                parser,
                "result",
                "expected 'result' in return authority clause"
            );
            valid_authority = valid_authority && sol_parser_expect_identifier_text(
                parser,
                "derives_from",
                "expected 'derives_from' in return authority clause"
            );
            SolToken authority_source = sol_parser_current(parser);
            valid_authority = valid_authority && sol_parser_expect(
                parser,
                SOL_TOKEN_IDENTIFIER,
                "expected an authority source"
            );
            valid_authority = valid_authority && sol_parser_expect(
                parser,
                SOL_TOKEN_RIGHT_BRACE,
                "expected '}' after authority"
            );
            if (!valid_authority) {
                while (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE
                    && sol_parser_kind(parser) != SOL_TOKEN_EOF
                    && !sol_parser_is_declaration_start(sol_parser_kind(parser))) {
                    sol_parser_advance(parser);
                }
                sol_parser_match(parser, SOL_TOKEN_RIGHT_BRACE);
                return false;
            }
            const SolSyntaxType *authority_type
                = &parser->tree->types[*return_type_id];
            bool direct_capability = authority_type->kind == SOL_SYNTAX_TYPE_PATH
                && authority_type->is_capability
                && authority_type->first_argument == SOL_AST_NONE;
            if (!direct_capability) {
                sol_parser_error(
                    parser,
                    "SOL-PARSE-016",
                    current,
                    "return authority requires a direct capability result type"
                );
            }
            if (member) {
                if (!sol_token_text_equal(parser->source, authority_source, "Self")) {
                    sol_parser_error(
                        parser,
                        "SOL-PARSE-016",
                        authority_source,
                        "capability-member result authority must derive from Self"
                    );
                } else {
                    *result_authority_from_self = direct_capability;
                }
            } else {
                SolParameterId parameter = *first_parameter;
                size_t traversed = 0;
                while (parameter != SOL_AST_NONE) {
                    if (parameter >= parser->tree->parameter_count
                        || traversed++ >= parser->tree->parameter_count) {
                        parameter = SOL_AST_NONE;
                        break;
                    }
                    size_t parameter_length = parser->tree->parameters[parameter].name.end
                        - parser->tree->parameters[parameter].name.start;
                    size_t source_length = authority_source.span.end
                        - authority_source.span.start;
                    if (parameter_length == source_length
                        && memcmp(
                            parser->source->text
                                + parser->tree->parameters[parameter].name.start,
                            parser->source->text + authority_source.span.start,
                            source_length
                        ) == 0) break;
                    parameter = parser->tree->parameters[parameter].next;
                }
                if (parameter == SOL_AST_NONE) {
                    sol_parser_error(
                        parser,
                        "SOL-PARSE-016",
                        authority_source,
                        "return authority source must name a function parameter"
                    );
                } else {
                    const SolSyntaxType *parameter_type = &parser->tree->types[
                        parser->tree->parameters[parameter].type_id
                    ];
                    bool capability_parameter
                        = parameter_type->kind == SOL_SYNTAX_TYPE_PATH
                        && parameter_type->is_capability
                        && parameter_type->first_argument == SOL_AST_NONE;
                    if (!capability_parameter) {
                        sol_parser_error(
                            parser,
                            "SOL-PARSE-016",
                            authority_source,
                            "return authority source must be a capability parameter"
                        );
                    }
                    *result_authority_parameter = direct_capability
                            && capability_parameter
                        ? parameter
                        : SOL_AST_NONE;
                }
            }
            continue;
        }
        unsigned int clause = 0;
        if (kind == SOL_TOKEN_EFFECTS) clause = 1;
        if (kind == SOL_TOKEN_REQUIRES) clause = 2;
        if (kind == SOL_TOKEN_ENSURES) clause = 3;
        if (clause == 0) break;

        SolToken clause_token = sol_parser_advance(parser);
        if (seen[clause]) {
            sol_diagnostics_add(
                parser->diagnostics,
                "SOL-PARSE-008",
                SOL_SEVERITY_ERROR,
                clause_token.span,
                "duplicate function clause"
            );
        }
        if (clause < last_clause) {
            sol_diagnostics_add(
                parser->diagnostics,
                "SOL-PARSE-009",
                SOL_SEVERITY_ERROR,
                clause_token.span,
                "function clauses must be ordered as effects, requires, ensures"
            );
        }
        seen[clause] = true;
        last_clause = clause;
        if (clause == 1) {
            *has_effect_clause = true;
            if (!sol_parser_effect_clause(
                parser,
                member ? member_effect_owner : SOL_EFFECT_OWNER_ITEM,
                effect_owner,
                first_effect
            )) return false;
        } else {
            if (member && !member_contracts) {
                sol_parser_error(
                    parser,
                    "SOL-PARSE-019",
                    clause_token,
                    "trait and implementation methods cannot declare contracts"
                );
                if (!sol_parser_balanced_block(parser, "expected a contract clause")) {
                    return false;
                }
                continue;
            }
            SolContractClauseId contract;
            if (!sol_parser_contract_clause(
                parser,
                clause == 2 ? SOL_CONTRACT_REQUIRES : SOL_CONTRACT_ENSURES,
                member ? SOL_CONTRACT_OWNER_CAPABILITY_MEMBER : SOL_CONTRACT_OWNER_ITEM,
                effect_owner,
                clause_token,
                &contract
            )) return false;
            if (*first_contract == SOL_AST_NONE) *first_contract = contract;
            else parser->tree->contract_clauses[last_contract].next = contract;
            last_contract = contract;
        }
    }

    if (sol_parser_kind(parser) == SOL_TOKEN_LEFT_BRACE && member && !member_has_body) {
        SolToken body_token = sol_parser_current(parser);
        sol_diagnostics_add(
            parser->diagnostics,
            "SOL-PARSE-014",
            SOL_SEVERITY_ERROR,
            body_token.span,
            "capability members declare signatures and cannot define function bodies"
        );
        sol_parser_balanced_block(parser, "expected a capability member body");
    } else if (sol_parser_kind(parser) == SOL_TOKEN_LEFT_BRACE) {
        *body = sol_parser_block_expression(parser);
        if (*body == SOL_AST_NONE) {
            return false;
        }
    } else if (!member || member_has_body) {
        sol_parser_error(
            parser,
            "SOL-PARSE-001",
            sol_parser_current(parser),
            member ? "derived capability members must define function bodies"
                   : "expected a function body"
        );
        return false;
    }

    SolToken previous = parser->tokens->items[parser->cursor - 1];
    *whole = (SolSpan){.start = function_token.span.start, .end = previous.span.end};
    return true;
}

static bool sol_parser_type_declaration(
    SolParser *parser,
    SolSpan *name,
    SolSpan *whole,
    SolTypeDeclarationFlavor *flavor,
    SolTypeId *representation_type,
    SolContractClauseId *first_contract,
    SolTypeParameterId *first_type_parameter
) {
    SolEffectParameterId unsupported_effect;
    SolToken opening = sol_parser_advance(parser);
    SolToken name_token = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a type name")) return false;
    *name = name_token.span;
    if (!sol_parser_generic_parameters(
        parser, true, false, first_type_parameter, &unsupported_effect
    )) return false;
    if (!sol_parser_expect(parser, SOL_TOKEN_EQUAL, "expected '=' after the type name")) {
        return false;
    }
    SolToken flavor_token = sol_parser_current(parser);
    if (sol_parser_match(parser, SOL_TOKEN_DISTINCT)) {
        *flavor = SOL_TYPE_DECLARATION_DISTINCT;
    } else if (sol_parser_match(parser, SOL_TOKEN_REFINED)) {
        *flavor = SOL_TYPE_DECLARATION_REFINED;
    } else {
        sol_parser_error(
            parser,
            "SOL-PARSE-001",
            flavor_token,
            "expected 'distinct' or 'refined' after '='"
        );
        return false;
    }
    if (!sol_parser_type(parser, NULL, representation_type)) return false;

    size_t end = parser->tree->types[*representation_type].span.end;
    *first_contract = SOL_AST_NONE;
    if (*flavor == SOL_TYPE_DECLARATION_REFINED) {
        SolToken where_token = sol_parser_current(parser);
        if (!sol_parser_expect(
            parser, SOL_TOKEN_WHERE, "expected 'where' after the refined representation"
        )) return false;
        if (sol_parser_is_declaration_start(sol_parser_kind(parser))) {
            sol_parser_error(
                parser,
                "SOL-PARSE-011",
                sol_parser_current(parser),
                "expected a refinement predicate"
            );
            return false;
        }
        SolParserContractContext previous_context = parser->contract_context;
        parser->contract_context = SOL_PARSER_CONTRACT_REQUIRES;
        SolExprId predicate = sol_parser_expression(parser, 1);
        parser->contract_context = previous_context;
        if (predicate == SOL_AST_NONE
            || parser->tree->expressions[predicate].kind == SOL_EXPR_ERROR) {
            return false;
        }
        end = parser->tree->expressions[predicate].span.end;
        SolContractClauseId clause = sol_parser_add_contract_clause(
            parser,
            (SolContractClause){
                .kind = SOL_CONTRACT_REQUIRES,
                .span = {.start = where_token.span.start, .end = end},
                .first_condition = SOL_AST_NONE,
                .next = SOL_AST_NONE,
                .owner_kind = SOL_CONTRACT_OWNER_TYPE,
                .owner = parser->tree->item_count,
            }
        );
        if (clause == SOL_AST_NONE) return false;
        SolContractConditionId condition = sol_parser_add_contract_condition(
            parser,
            (SolContractCondition){
                .outcome = SOL_CONTRACT_OUTCOME_ALWAYS,
                .span = parser->tree->expressions[predicate].span,
                .expression = predicate,
                .next = SOL_AST_NONE,
                .owner_clause = clause,
            }
        );
        if (condition == SOL_AST_NONE) return false;
        parser->tree->contract_clauses[clause].first_condition = condition;
        *first_contract = clause;
    } else if (sol_parser_kind(parser) == SOL_TOKEN_WHERE) {
        sol_parser_error(
            parser,
            "SOL-PARSE-001",
            sol_parser_current(parser),
            "distinct type declarations cannot have refinement predicates"
        );
        return false;
    }
    SolToken trailing = sol_parser_current(parser);
    if (trailing.kind == SOL_TOKEN_IDENTIFIER
        && sol_token_text_equal(parser->source, trailing, "using")) {
        sol_parser_error(
            parser,
            "SOL-PARSE-001",
            trailing,
            "type declarations do not support 'using' clauses"
        );
        return false;
    }
    *whole = (SolSpan){.start = opening.span.start, .end = end};
    return true;
}

static bool sol_parser_record(
    SolParser *parser,
    SolSpan *name,
    SolSpan *whole,
    SolFieldId *first_field,
    SolTypeParameterId *first_type_parameter
) {
    SolEffectParameterId unsupported_effect;
    SolToken opening = sol_parser_advance(parser);
    SolToken name_token = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a record name")) {
        return false;
    }
    *name = name_token.span;
    if (!sol_parser_generic_parameters(
        parser, true, false, first_type_parameter, &unsupported_effect
    )) return false;
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE, "expected '{' after the record name")) {
        return false;
    }
    if (!sol_parser_named_type_list(
        parser,
        SOL_TOKEN_RIGHT_BRACE,
        true,
        first_field
    )) {
        return false;
    }
    SolToken previous = parser->tokens->items[parser->cursor - 1];
    *whole = (SolSpan){.start = opening.span.start, .end = previous.span.end};
    return true;
}

static bool sol_parser_enum(
    SolParser *parser,
    SolSpan *name,
    SolSpan *whole,
    SolVariantId *first_variant,
    SolTypeParameterId *first_type_parameter
) {
    SolEffectParameterId unsupported_effect;
    *first_variant = SOL_AST_NONE;
    SolVariantId last_variant = SOL_AST_NONE;
    SolToken opening = sol_parser_advance(parser);
    SolToken name_token = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected an enum name")) {
        return false;
    }
    *name = name_token.span;
    if (!sol_parser_generic_parameters(
        parser, true, false, first_type_parameter, &unsupported_effect
    )) return false;
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE, "expected '{' after the enum name")) {
        return false;
    }
    if (sol_parser_match(parser, SOL_TOKEN_RIGHT_BRACE)) {
        SolToken previous = parser->tokens->items[parser->cursor - 1];
        *whole = (SolSpan){.start = opening.span.start, .end = previous.span.end};
        return true;
    }
    for (;;) {
        SolToken variant_name = sol_parser_current(parser);
        if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected an enum variant")) {
            return false;
        }
        SolFieldId variant_fields = SOL_AST_NONE;
        size_t variant_end = variant_name.span.end;
        if (sol_parser_match(parser, SOL_TOKEN_LEFT_PAREN)
        ) {
            if (!sol_parser_named_type_list(
                parser,
                SOL_TOKEN_RIGHT_PAREN,
                true,
                &variant_fields
            )) {
                return false;
            }
            variant_end = parser->tokens->items[parser->cursor - 1].span.end;
        }
        SolVariantId variant = sol_parser_add_variant(parser, (SolVariant){
            .name = variant_name.span,
            .span = {.start = variant_name.span.start, .end = variant_end},
            .first_field = variant_fields,
            .next = SOL_AST_NONE,
            .owner_item = parser->tree->item_count,
        });
        if (variant == SOL_AST_NONE) return false;
        if (*first_variant == SOL_AST_NONE) {
            *first_variant = variant;
        } else {
            parser->tree->variants[last_variant].next = variant;
        }
        last_variant = variant;
        if (sol_parser_match(parser, SOL_TOKEN_RIGHT_BRACE)) {
            break;
        }
        if (!sol_parser_expect(parser, SOL_TOKEN_COMMA, "expected ',' between enum variants")) {
            return false;
        }
        if (sol_parser_match(parser, SOL_TOKEN_RIGHT_BRACE)) {
            break;
        }
    }
    SolToken previous = parser->tokens->items[parser->cursor - 1];
    *whole = (SolSpan){.start = opening.span.start, .end = previous.span.end};
    return true;
}

static bool sol_parser_capability(
    SolParser *parser,
    SolSpan *name,
    SolSpan *whole,
    SolCapabilityMemberId *first_member,
    SolParameterId *source_parameter
) {
    *first_member = SOL_AST_NONE;
    *source_parameter = SOL_AST_NONE;
    SolCapabilityMemberId last_member = SOL_AST_NONE;
    SolToken opening = sol_parser_advance(parser);
    SolToken name_token = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a capability name")) {
        return false;
    }
    *name = name_token.span;
    SolTypeParameterId unsupported_parameters;
    SolEffectParameterId unsupported_effect;
    if (!sol_parser_generic_parameters(
        parser, false, false, &unsupported_parameters, &unsupported_effect
    )) return false;
    bool derived = false;
    SolToken derives = sol_parser_current(parser);
    if (derives.kind == SOL_TOKEN_IDENTIFIER
        && sol_token_text_equal(parser->source, derives, "derives_from")) {
        derived = true;
        sol_parser_advance(parser);
        SolToken source_name = sol_parser_current(parser);
        if (!sol_parser_expect(
                parser,
                SOL_TOKEN_IDENTIFIER,
                "expected a source name after 'derives_from'"
            )
            || !sol_parser_expect(
                parser,
                SOL_TOKEN_COLON,
                "expected ':' after the capability source name"
            )) {
            return false;
        }
        SolSpan source_type;
        SolTypeId source_type_id;
        if (!sol_parser_type(parser, &source_type, &source_type_id)) return false;
        const SolSyntaxType *type = &parser->tree->types[source_type_id];
        if (type->kind != SOL_SYNTAX_TYPE_PATH || !type->is_capability
            || type->first_argument != SOL_AST_NONE) {
            sol_parser_error(
                parser,
                "SOL-PARSE-016",
                derives,
                "a derived capability source must have a direct capability type"
            );
        }
        *source_parameter = sol_parser_add_parameter(parser, (SolParameter){
            .name = source_name.span,
            .type = source_type,
            .type_id = source_type_id,
            .next = SOL_AST_NONE,
        });
        if (*source_parameter == SOL_AST_NONE) return false;
    }
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE, "expected '{' after the capability name")) {
        return false;
    }
    while (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE
        && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        size_t parameter_mark = parser->tree->parameter_count;
        size_t type_mark = parser->tree->type_count;
        size_t type_argument_mark = parser->tree->type_argument_count;
        size_t type_parameter_mark = parser->tree->type_parameter_count;
        size_t effect_parameter_mark = parser->tree->effect_parameter_count;
        size_t effect_mark = parser->tree->effect_count;
        size_t expression_mark = parser->tree->expression_count;
        size_t statement_mark = parser->tree->statement_count;
        size_t argument_mark = parser->tree->argument_count;
        size_t pattern_mark = parser->tree->pattern_count;
        size_t pattern_binding_mark = parser->tree->pattern_binding_count;
        size_t match_arm_mark = parser->tree->match_arm_count;
        size_t contract_clause_mark = parser->tree->contract_clause_count;
        size_t contract_condition_mark = parser->tree->contract_condition_count;
        SolSpan member_name;
        SolSpan member_span;
        SolExprId member_body;
        SolParameterId member_parameters;
        SolSpan member_return_type;
        SolTypeId member_return_type_id;
        SolEffectId member_effects;
        bool member_has_effects;
        SolContractClauseId member_contracts;
        bool member_result_authority_from_self;
        SolParameterId member_result_authority_parameter;
        SolTypeParameterId member_type_parameters;
        SolEffectParameterId member_effect_parameter;
        if (!sol_parser_function(
            parser,
            true,
            derived,
            true,
            SOL_EFFECT_OWNER_CAPABILITY_MEMBER,
            parser->tree->capability_member_count,
            &member_name,
            &member_span,
            &member_body,
            &member_parameters,
            &member_return_type,
            &member_return_type_id,
            &member_effects,
            &member_has_effects,
            &member_contracts,
            &member_result_authority_from_self,
            &member_result_authority_parameter,
            &member_type_parameters,
            &member_effect_parameter
        )) {
            parser->tree->parameter_count = parameter_mark;
            parser->tree->type_count = type_mark;
            parser->tree->type_argument_count = type_argument_mark;
            parser->tree->type_parameter_count = type_parameter_mark;
            parser->tree->effect_parameter_count = effect_parameter_mark;
            parser->tree->effect_count = effect_mark;
            parser->tree->expression_count = expression_mark;
            parser->tree->statement_count = statement_mark;
            parser->tree->argument_count = argument_mark;
            parser->tree->pattern_count = pattern_mark;
            parser->tree->pattern_binding_count = pattern_binding_mark;
            parser->tree->match_arm_count = match_arm_mark;
            parser->tree->contract_clause_count = contract_clause_mark;
            parser->tree->contract_condition_count = contract_condition_mark;
            if (sol_parser_kind(parser) != SOL_TOKEN_EOF
                && sol_parser_kind(parser) != SOL_TOKEN_FUNCTION
                && sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE) {
                sol_parser_advance(parser);
            }
        } else {
            SolCapabilityMemberId member = sol_parser_add_capability_member(
                parser,
                (SolCapabilityMember){
                    .name = member_name,
                    .span = member_span,
                    .first_parameter = member_parameters,
                    .return_type = member_return_type,
                    .return_type_id = member_return_type_id,
                    .first_effect = member_effects,
                    .first_contract = member_contracts,
                    .body = member_body,
                    .next = SOL_AST_NONE,
                    .owner_item = parser->tree->item_count,
                    .has_effect_clause = member_has_effects,
                    .result_authority_from_self = member_result_authority_from_self,
                }
            );
            if (member == SOL_AST_NONE) return false;
            if (*first_member == SOL_AST_NONE) *first_member = member;
            else parser->tree->capability_members[last_member].next = member;
            last_member = member;
        }
    }
    if (!sol_parser_expect(parser, SOL_TOKEN_RIGHT_BRACE, "expected '}' after capability members")) {
        return false;
    }
    SolToken previous = parser->tokens->items[parser->cursor - 1];
    *whole = (SolSpan){.start = opening.span.start, .end = previous.span.end};
    return true;
}

static void sol_parser_recover_trait_method(SolParser *parser, size_t method_start) {
    parser->cursor = method_start;
    size_t braces = 0;
    size_t parentheses = 0;
    size_t brackets = 0;
    bool at_start = true;
    while (sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        SolTokenKind kind = sol_parser_kind(parser);
        bool top_level = braces == 0 && parentheses == 0 && brackets == 0;
        if (kind == SOL_TOKEN_FUNCTION && top_level && !at_start) {
            return;
        }
        if (kind == SOL_TOKEN_RIGHT_BRACE && braces == 0
            && parentheses == 0 && brackets == 0) {
            return;
        }
        if (kind == SOL_TOKEN_LEFT_BRACE) {
            ++braces;
        } else if (kind == SOL_TOKEN_RIGHT_BRACE && braces != 0) {
            --braces;
        } else if (kind == SOL_TOKEN_LEFT_PAREN) {
            ++parentheses;
        } else if (kind == SOL_TOKEN_RIGHT_PAREN && parentheses != 0) {
            --parentheses;
        } else if (kind == SOL_TOKEN_LEFT_BRACKET) {
            ++brackets;
        } else if (kind == SOL_TOKEN_RIGHT_BRACKET && brackets != 0) {
            --brackets;
        }
        sol_parser_advance(parser);
        at_start = false;
    }
}

static bool sol_parser_trait_like(
    SolParser *parser,
    bool implementation,
    SolSpan *name,
    SolSpan *whole,
    SolSpan *trait_name,
    SolTypeId *implementation_type,
    SolTraitMethodId *first_method
) {
    *first_method = SOL_AST_NONE;
    *implementation_type = SOL_AST_NONE;
    *trait_name = (SolSpan){0};
    SolTraitMethodId last = SOL_AST_NONE;
    SolToken opening = sol_parser_advance(parser);
    SolToken declared = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER,
        implementation ? "expected a trait name after 'implementation'"
                       : "expected a trait name")) return false;
    *name = declared.span;
    if (implementation) {
        *trait_name = declared.span;
        if (!sol_parser_expect(parser, SOL_TOKEN_FOR, "expected 'for' after trait name")) {
            return false;
        }
        SolSpan target;
        if (!sol_parser_type(parser, &target, implementation_type)) return false;
        *name = (SolSpan){.start = opening.span.start, .end = target.end};
    } else if (sol_parser_kind(parser) == SOL_TOKEN_LESS) {
        SolTypeParameterId unsupported;
        SolEffectParameterId unsupported_effect;
        if (!sol_parser_generic_parameters(
            parser, false, false, &unsupported, &unsupported_effect
        )) return false;
    }
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE, "expected '{' before methods")) {
        return false;
    }
    while (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE
        && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        size_t method_start = sol_parser_significant_index(parser);
        size_t parameter_mark = parser->tree->parameter_count;
        size_t type_mark = parser->tree->type_count;
        size_t type_argument_mark = parser->tree->type_argument_count;
        size_t effect_mark = parser->tree->effect_count;
        size_t expression_mark = parser->tree->expression_count;
        size_t statement_mark = parser->tree->statement_count;
        size_t argument_mark = parser->tree->argument_count;
        size_t pattern_mark = parser->tree->pattern_count;
        size_t pattern_binding_mark = parser->tree->pattern_binding_count;
        size_t match_arm_mark = parser->tree->match_arm_count;
        SolSpan method_name;
        SolSpan method_span;
        SolExprId body;
        SolParameterId parameters;
        SolSpan return_type;
        SolTypeId return_type_id;
        SolEffectId effects;
        bool has_effects;
        SolContractClauseId contracts;
        bool result_self;
        SolParameterId result_parameter;
        SolTypeParameterId type_parameters;
        SolEffectParameterId effect_parameter;
        size_t method_id = parser->tree->trait_method_count;
        bool parsed = sol_parser_function(
            parser,
            true,
            implementation,
            false,
            SOL_EFFECT_OWNER_TRAIT_METHOD,
            method_id,
            &method_name,
            &method_span,
            &body,
            &parameters,
            &return_type,
            &return_type_id,
            &effects,
            &has_effects,
            &contracts,
            &result_self,
            &result_parameter,
            &type_parameters,
            &effect_parameter
        );
        if (!parsed) {
            parser->tree->parameter_count = parameter_mark;
            parser->tree->type_count = type_mark;
            parser->tree->type_argument_count = type_argument_mark;
            parser->tree->effect_count = effect_mark;
            parser->tree->expression_count = expression_mark;
            parser->tree->statement_count = statement_mark;
            parser->tree->argument_count = argument_mark;
            parser->tree->pattern_count = pattern_mark;
            parser->tree->pattern_binding_count = pattern_binding_mark;
            parser->tree->match_arm_count = match_arm_mark;
            sol_parser_recover_trait_method(parser, method_start);
            continue;
        }
        if (!has_effects) {
            sol_parser_error(
                parser,
                "SOL-PARSE-019",
                (SolToken){.kind = SOL_TOKEN_FUNCTION, .span = method_span},
                "trait and implementation methods require explicit effects"
            );
        }
        SolTraitMethodId method = sol_parser_add_trait_method(parser, (SolTraitMethod){
            .name = method_name,
            .span = method_span,
            .first_parameter = parameters,
            .return_type = return_type,
            .return_type_id = return_type_id,
            .first_effect = effects,
            .body = body,
            .next = SOL_AST_NONE,
            .owner_item = parser->tree->item_count,
            .has_effect_clause = has_effects,
        });
        if (method == SOL_AST_NONE) return false;
        if (*first_method == SOL_AST_NONE) *first_method = method;
        else parser->tree->trait_methods[last].next = method;
        last = method;
    }
    if (!sol_parser_expect(parser, SOL_TOKEN_RIGHT_BRACE, "expected '}' after methods")) {
        return false;
    }
    SolToken previous = parser->tokens->items[parser->cursor - 1];
    *whole = (SolSpan){.start = opening.span.start, .end = previous.span.end};
    return true;
}

static SolSpan sol_parser_annotation(SolParser *parser) {
    sol_parser_advance(parser);
    SolToken name = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected an annotation name")) {
        return (SolSpan){0};
    }
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_PAREN, "expected '(' after annotation name")) {
        return (SolSpan){0};
    }
    SolToken argument = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_STRING, "expected a string annotation argument")) {
        return (SolSpan){0};
    }
    if (!sol_parser_expect(parser, SOL_TOKEN_RIGHT_PAREN, "expected ')' after annotation")) {
        return (SolSpan){0};
    }
    return sol_token_text_equal(parser->source, name, "stable")
        ? argument.span
        : (SolSpan){0};
}

static bool sol_parser_is_declaration_start(SolTokenKind kind) {
    return kind == SOL_TOKEN_AT || kind == SOL_TOKEN_PUBLIC || kind == SOL_TOKEN_PRIVATE
        || kind == SOL_TOKEN_RECORD || kind == SOL_TOKEN_ENUM || kind == SOL_TOKEN_TYPE
        || kind == SOL_TOKEN_OPEN
        || kind == SOL_TOKEN_CAPABILITY || kind == SOL_TOKEN_TRAIT
        || kind == SOL_TOKEN_IMPLEMENTATION || kind == SOL_TOKEN_FUNCTION
        || kind == SOL_TOKEN_TEST;
}

static void sol_parser_recover_declaration(SolParser *parser, size_t failed_at) {
    sol_parser_significant_index(parser);
    if (parser->cursor <= failed_at && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        sol_parser_advance(parser);
    }
    while (sol_parser_kind(parser) != SOL_TOKEN_EOF
        && (!sol_parser_is_declaration_start(sol_parser_kind(parser))
            || (sol_parser_kind(parser) == SOL_TOKEN_FUNCTION
                && sol_parser_peek_kind(parser, 1) == SOL_TOKEN_LEFT_PAREN))) {
        sol_parser_advance(parser);
    }
}

static void sol_parser_declaration(SolParser *parser) {
    size_t failed_at = sol_parser_significant_index(parser);
    size_t parameter_mark = parser->tree->parameter_count;
    size_t type_mark = parser->tree->type_count;
    size_t type_argument_mark = parser->tree->type_argument_count;
    size_t type_parameter_mark = parser->tree->type_parameter_count;
    size_t effect_parameter_mark = parser->tree->effect_parameter_count;
    size_t field_mark = parser->tree->field_count;
    size_t variant_mark = parser->tree->variant_count;
    size_t effect_mark = parser->tree->effect_count;
    size_t capability_member_mark = parser->tree->capability_member_count;
    size_t trait_method_mark = parser->tree->trait_method_count;
    size_t expression_mark = parser->tree->expression_count;
    size_t statement_mark = parser->tree->statement_count;
    size_t argument_mark = parser->tree->argument_count;
    size_t pattern_mark = parser->tree->pattern_count;
    size_t pattern_binding_mark = parser->tree->pattern_binding_count;
    size_t match_arm_mark = parser->tree->match_arm_count;
    size_t contract_clause_mark = parser->tree->contract_clause_count;
    size_t contract_condition_mark = parser->tree->contract_condition_count;
    size_t start = sol_parser_current(parser).span.start;
    SolSpan stable_identity = {0};
    while (sol_parser_kind(parser) == SOL_TOKEN_AT) {
        SolSpan annotation = sol_parser_annotation(parser);
        if (annotation.start != annotation.end) {
            if (stable_identity.start != stable_identity.end) {
                sol_diagnostics_add(
                    parser->diagnostics,
                    "SOL-PARSE-020",
                    SOL_SEVERITY_ERROR,
                    annotation,
                    "a declaration may have only one @stable annotation"
                );
            } else {
                stable_identity = annotation;
            }
        }
    }
    bool is_public = sol_parser_match(parser, SOL_TOKEN_PUBLIC);
    bool has_modifier = is_public;
    if (!is_public) {
        has_modifier = sol_parser_match(parser, SOL_TOKEN_PRIVATE);
    }

    SolItemKind item_kind;
    SolTypeDeclarationFlavor flavor = SOL_TYPE_DECLARATION_NONE;
    SolSpan name = {0};
    SolSpan whole = {0};
    SolExprId body = SOL_AST_NONE;
    SolParameterId first_parameter = SOL_AST_NONE;
    SolSpan return_type = {0};
    SolTypeId return_type_id = SOL_AST_NONE;
    SolFieldId first_field = SOL_AST_NONE;
    SolVariantId first_variant = SOL_AST_NONE;
    bool is_open = false;
    SolEffectId first_effect = SOL_AST_NONE;
    bool has_effect_clause = false;
    SolContractClauseId first_contract = SOL_AST_NONE;
    SolTypeId representation_type = SOL_AST_NONE;
    bool result_authority_from_self = false;
    SolParameterId result_authority_parameter = SOL_AST_NONE;
    SolCapabilityMemberId first_member = SOL_AST_NONE;
    SolParameterId capability_source = SOL_AST_NONE;
    SolTypeParameterId first_type_parameter = SOL_AST_NONE;
    SolEffectParameterId first_effect_parameter = SOL_AST_NONE;
    SolSpan trait_name = {0};
    SolTypeId implementation_type = SOL_AST_NONE;
    SolTraitMethodId first_trait_method = SOL_AST_NONE;
    bool parsed = false;
    SolTokenKind kind = sol_parser_kind(parser);
    if (kind == SOL_TOKEN_TYPE) {
        item_kind = SOL_ITEM_TYPE;
        parsed = sol_parser_type_declaration(
            parser,
            &name,
            &whole,
            &flavor,
            &representation_type,
            &first_contract,
            &first_type_parameter
        );
    } else if (kind == SOL_TOKEN_RECORD) {
        item_kind = SOL_ITEM_RECORD;
        parsed = sol_parser_record(
            parser,
            &name,
            &whole,
            &first_field,
            &first_type_parameter
        );
    } else if (kind == SOL_TOKEN_OPEN || kind == SOL_TOKEN_ENUM) {
        if (kind == SOL_TOKEN_OPEN) {
            is_open = true;
            sol_parser_advance(parser);
            if (sol_parser_kind(parser) != SOL_TOKEN_ENUM) {
                sol_parser_error(
                    parser,
                    "SOL-PARSE-001",
                    sol_parser_current(parser),
                    "expected 'enum' after 'open'"
                );
                return;
            }
        }
        item_kind = SOL_ITEM_ENUM;
        parsed = sol_parser_enum(
            parser,
            &name,
            &whole,
            &first_variant,
            &first_type_parameter
        );
    } else if (kind == SOL_TOKEN_CAPABILITY) {
        item_kind = SOL_ITEM_CAPABILITY;
        parsed = sol_parser_capability(
            parser,
            &name,
            &whole,
            &first_member,
            &capability_source
        );
    } else if (kind == SOL_TOKEN_TRAIT || kind == SOL_TOKEN_IMPLEMENTATION) {
        bool implementation = kind == SOL_TOKEN_IMPLEMENTATION;
        item_kind = implementation ? SOL_ITEM_IMPLEMENTATION : SOL_ITEM_TRAIT;
        parsed = sol_parser_trait_like(
            parser,
            implementation,
            &name,
            &whole,
            &trait_name,
            &implementation_type,
            &first_trait_method
        );
    } else if (kind == SOL_TOKEN_FUNCTION) {
        item_kind = SOL_ITEM_FUNCTION;
        parsed = sol_parser_function(
            parser,
            false,
            false,
            true,
            SOL_EFFECT_OWNER_ITEM,
            parser->tree->item_count,
            &name,
            &whole,
            &body,
            &first_parameter,
            &return_type,
            &return_type_id,
            &first_effect,
            &has_effect_clause,
            &first_contract,
            &result_authority_from_self,
            &result_authority_parameter,
            &first_type_parameter,
            &first_effect_parameter
        );
    } else if (kind == SOL_TOKEN_TEST) {
        item_kind = SOL_ITEM_TEST;
        SolToken test_token = sol_parser_advance(parser);
        SolToken label = sol_parser_current(parser);
        if (stable_identity.start != stable_identity.end || has_modifier) {
            sol_diagnostics_add(
                parser->diagnostics,
                "SOL-PARSE-021",
                SOL_SEVERITY_ERROR,
                (SolSpan){.start = start, .end = test_token.span.end},
                "test declarations do not accept annotations or modifiers"
            );
        }
        if (sol_parser_expect(parser, SOL_TOKEN_STRING, "expected a test label string")) {
            name = label.span;
            body = sol_parser_expression(parser, 1);
            if (body != SOL_AST_NONE) {
                whole = (SolSpan){
                    .start = test_token.span.start,
                    .end = parser->tree->expressions[body].span.end,
                };
                parsed = true;
                is_public = false;
                stable_identity = (SolSpan){0};
            }
        }
    } else {
        sol_parser_error(
            parser,
            "SOL-PARSE-003",
            sol_parser_current(parser),
            "expected a declaration"
        );
    }

    if (parsed) {
        whole.start = start;
        sol_parser_add_item(parser, (SolSyntaxItem){
            .kind = item_kind,
            .flavor = flavor,
            .name = name,
            .span = whole,
            .stable_identity = stable_identity,
            .is_public = is_public,
            .body = body,
            .first_parameter = first_parameter,
            .return_type = return_type,
            .return_type_id = return_type_id,
            .first_field = first_field,
            .first_variant = first_variant,
            .is_open = is_open,
            .first_effect = first_effect,
            .has_effect_clause = has_effect_clause,
            .first_contract = first_contract,
            .representation_type = representation_type,
            .first_member = first_member,
            .result_authority_parameter = result_authority_parameter,
            .capability_source = capability_source,
            .first_type_parameter = first_type_parameter,
            .first_effect_parameter = first_effect_parameter,
            .trait_name = trait_name,
            .implementation_type = implementation_type,
            .first_trait_method = first_trait_method,
        });
    } else {
        parser->tree->parameter_count = parameter_mark;
        parser->tree->type_count = type_mark;
        parser->tree->type_argument_count = type_argument_mark;
        parser->tree->type_parameter_count = type_parameter_mark;
        parser->tree->effect_parameter_count = effect_parameter_mark;
        parser->tree->field_count = field_mark;
        parser->tree->variant_count = variant_mark;
        parser->tree->effect_count = effect_mark;
        parser->tree->capability_member_count = capability_member_mark;
        parser->tree->trait_method_count = trait_method_mark;
        parser->tree->expression_count = expression_mark;
        parser->tree->statement_count = statement_mark;
        parser->tree->argument_count = argument_mark;
        parser->tree->pattern_count = pattern_mark;
        parser->tree->pattern_binding_count = pattern_binding_mark;
        parser->tree->match_arm_count = match_arm_mark;
        parser->tree->contract_clause_count = contract_clause_mark;
        parser->tree->contract_condition_count = contract_condition_mark;
        sol_parser_recover_declaration(parser, failed_at);
    }
}

static void sol_parser_header(SolParser *parser) {
    if (!sol_parser_match(parser, SOL_TOKEN_MODULE)) {
        sol_parser_error(
            parser,
            "SOL-PARSE-004",
            sol_parser_current(parser),
            "a Sol source file must begin with a module declaration"
        );
    } else {
        sol_parser_path(parser, &parser->tree->module_name, "expected a module path");
    }

    parser->tree->edition = 2027;
    if (sol_parser_match(parser, SOL_TOKEN_EDITION)) {
        SolToken edition = sol_parser_current(parser);
        if (sol_parser_expect(parser, SOL_TOKEN_INTEGER, "expected an edition number")) {
            if (!sol_token_text_equal(parser->source, edition, "2027")) {
                sol_diagnostics_add(
                    parser->diagnostics,
                    "SOL-PARSE-005",
                    SOL_SEVERITY_ERROR,
                    edition.span,
                    "unsupported edition; this compiler supports edition 2027"
                );
            }
        }
    }

    while (sol_parser_match(parser, SOL_TOKEN_USE)) {
        SolToken first = sol_parser_current(parser);
        SolSpan path;
        size_t component_count;
        bool complete;
        bool parsed = sol_parser_path_components(
            parser,
            &path,
            "expected an import path",
            &component_count,
            &complete
        );
        if (parsed && complete && component_count < 2) {
            sol_parser_error(
                parser,
                "SOL-PARSE-001",
                first,
                "an import path must include a module and symbol"
            );
        } else if (parsed && complete) {
            sol_parser_add_import(parser, (SolImport){.path = path});
        }
    }
}

bool sol_parse(
    const SolSource *source,
    const SolTokens *tokens,
    SolSyntaxTree *tree,
    SolDiagnostics *diagnostics
) {
    SolParser parser = {
        .source = source,
        .tokens = tokens,
        .tree = tree,
        .diagnostics = diagnostics,
    };
    sol_parser_header(&parser);
    while (sol_parser_kind(&parser) != SOL_TOKEN_EOF && !parser.allocation_failed) {
        sol_parser_declaration(&parser);
    }
    if (parser.allocation_failed) {
        sol_diagnostics_add(
            diagnostics,
            "SOL-INTERNAL-001",
            SOL_SEVERITY_ERROR,
            (SolSpan){0},
            "out of memory while constructing the syntax tree"
        );
        return false;
    }
    return !diagnostics->allocation_failed;
}
