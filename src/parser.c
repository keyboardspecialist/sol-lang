#include "sol/parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const SolSource *source;
    const SolTokens *tokens;
    SolSyntaxTree *tree;
    SolDiagnostics *diagnostics;
    size_t cursor;
    size_t type_depth;
    size_t expression_depth;
    bool suppress_record_literal;
    bool allocation_failed;
} SolParser;

void sol_syntax_tree_init(SolSyntaxTree *tree) {
    memset(tree, 0, sizeof(*tree));
}

void sol_syntax_tree_free(SolSyntaxTree *tree) {
    free(tree->items);
    free(tree->expressions);
    free(tree->statements);
    free(tree->arguments);
    free(tree->parameters);
    free(tree->types);
    free(tree->type_arguments);
    free(tree->fields);
    free(tree->variants);
    free(tree->patterns);
    free(tree->pattern_bindings);
    free(tree->match_arms);
    free(tree->effects);
    free(tree->capability_members);
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

static bool sol_parser_path(SolParser *parser, SolSpan *span, const char *description) {
    SolToken first = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, description)) {
        return false;
    }
    SolToken last = first;
    while (sol_parser_match(parser, SOL_TOKEN_DOT)) {
        last = sol_parser_current(parser);
        if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a path component after '.'")) {
            break;
        }
    }
    *span = (SolSpan){.start = first.span.start, .end = last.span.end};
    return true;
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
    bool is_capability = false;
    if (sol_parser_match(parser, SOL_TOKEN_LEFT_PAREN)) {
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
        SolTypeId parsed_type = sol_parser_add_type(parser, (SolSyntaxType){
            .kind = kind,
            .span = type_span,
            .name = name,
            .first_argument = first_argument,
            .is_capability = is_capability,
        });
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
        if (kind == SOL_TOKEN_LEFT_PAREN) {
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
            SolExprKind expression_kind = parser->tree->expressions[expression].kind;
            if (expression_kind != SOL_EXPR_PATH && expression_kind != SOL_EXPR_FIELD) {
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
            || parser->tree->expressions[value].kind == SOL_EXPR_FIELD)
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

static SolStatementId sol_parser_statement(SolParser *parser) {
    SolToken start = sol_parser_current(parser);
    SolStatement statement = {.next = SOL_AST_NONE};
    if (sol_parser_match(parser, SOL_TOKEN_LET)) {
        SolToken name = sol_parser_current(parser);
        sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a binding name after 'let'");
        sol_parser_expect(parser, SOL_TOKEN_EQUAL, "expected '=' after binding name");
        SolExprId value = sol_parser_expression(parser, 1);
        sol_parser_check_multiline_record(parser, value);
        statement.kind = SOL_STATEMENT_LET;
        statement.as.let_statement.name = name.span;
        statement.as.let_statement.value = value;
        statement.span = (SolSpan){
            .start = start.span.start,
            .end = value == SOL_AST_NONE ? name.span.end : parser->tree->expressions[value].span.end,
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
    } else {
        SolExprId value = sol_parser_expression(parser, 1);
        sol_parser_check_multiline_record(parser, value);
        statement.kind = SOL_STATEMENT_EXPRESSION;
        statement.as.expression = value;
        statement.span = value == SOL_AST_NONE ? start.span : parser->tree->expressions[value].span;
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

static bool sol_parser_effect_clause(
    SolParser *parser,
    bool retain,
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
        if (!is_pure && !sol_parser_path(parser, &name, "expected an effect name")) {
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
        if (retain) {
            SolEffectId effect = sol_parser_add_effect(parser, (SolEffect){
                .name = name,
                .argument = argument,
                .span = {.start = first.span.start, .end = end},
                .next = SOL_AST_NONE,
                .owner_item = parser->tree->item_count,
                .is_pure = is_pure,
                .has_argument = has_argument,
            });
            if (effect == SOL_AST_NONE) return false;
            if (*first_effect == SOL_AST_NONE) *first_effect = effect;
            else parser->tree->effects[last_effect].next = effect;
            last_effect = effect;
        }
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

static bool sol_parser_function(
    SolParser *parser,
    bool member,
    SolSpan *name,
    SolSpan *whole,
    SolExprId *body,
    SolParameterId *first_parameter,
    SolSpan *return_type,
    SolTypeId *return_type_id,
    SolEffectId *first_effect,
    bool *has_effect_clause
) {
    *body = SOL_AST_NONE;
    *first_parameter = SOL_AST_NONE;
    *return_type = (SolSpan){0};
    *return_type_id = SOL_AST_NONE;
    *first_effect = SOL_AST_NONE;
    *has_effect_clause = false;
    SolToken function_token = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_FUNCTION, "expected 'function'")) {
        return false;
    }
    if (!sol_parser_path(parser, name, "expected a function name")) {
        return false;
    }
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
    for (;;) {
        SolTokenKind kind = sol_parser_kind(parser);
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
            if (!sol_parser_effect_clause(parser, true, first_effect)) return false;
        } else {
            sol_parser_balanced_block(parser, "expected a block after the function clause");
        }
    }

    if (sol_parser_kind(parser) == SOL_TOKEN_LEFT_BRACE && member) {
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
    } else if (!member) {
        sol_parser_error(
            parser,
            "SOL-PARSE-001",
            sol_parser_current(parser),
            "expected a function body"
        );
        return false;
    }

    SolToken previous = parser->tokens->items[parser->cursor - 1];
    *whole = (SolSpan){.start = function_token.span.start, .end = previous.span.end};
    return true;
}

static bool sol_parser_record(
    SolParser *parser,
    SolSpan *name,
    SolSpan *whole,
    SolFieldId *first_field
) {
    SolToken opening = sol_parser_advance(parser);
    SolToken name_token = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a record name")) {
        return false;
    }
    *name = name_token.span;
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
    SolVariantId *first_variant
) {
    *first_variant = SOL_AST_NONE;
    SolVariantId last_variant = SOL_AST_NONE;
    SolToken opening = sol_parser_advance(parser);
    SolToken name_token = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected an enum name")) {
        return false;
    }
    *name = name_token.span;
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
    SolCapabilityMemberId *first_member
) {
    *first_member = SOL_AST_NONE;
    SolCapabilityMemberId last_member = SOL_AST_NONE;
    SolToken opening = sol_parser_advance(parser);
    SolToken name_token = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a capability name")) {
        return false;
    }
    *name = name_token.span;
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE, "expected '{' after the capability name")) {
        return false;
    }
    while (sol_parser_kind(parser) != SOL_TOKEN_RIGHT_BRACE
        && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        size_t parameter_mark = parser->tree->parameter_count;
        size_t type_mark = parser->tree->type_count;
        size_t type_argument_mark = parser->tree->type_argument_count;
        size_t effect_mark = parser->tree->effect_count;
        SolSpan member_name;
        SolSpan member_span;
        SolExprId member_body;
        SolParameterId member_parameters;
        SolSpan member_return_type;
        SolTypeId member_return_type_id;
        SolEffectId member_effects;
        bool member_has_effects;
        if (!sol_parser_function(
            parser,
            true,
            &member_name,
            &member_span,
            &member_body,
            &member_parameters,
            &member_return_type,
            &member_return_type_id,
            &member_effects,
            &member_has_effects
        )) {
            parser->tree->parameter_count = parameter_mark;
            parser->tree->type_count = type_mark;
            parser->tree->type_argument_count = type_argument_mark;
            parser->tree->effect_count = effect_mark;
            if (sol_parser_kind(parser) != SOL_TOKEN_EOF) {
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
                    .next = SOL_AST_NONE,
                    .owner_item = parser->tree->item_count,
                    .has_effect_clause = member_has_effects,
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

static void sol_parser_annotation(SolParser *parser) {
    SolToken annotation = sol_parser_advance(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected an annotation name")) {
        return;
    }
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_PAREN, "expected '(' after annotation name")) {
        return;
    }
    if (!sol_parser_expect(parser, SOL_TOKEN_STRING, "expected a string annotation argument")) {
        return;
    }
    if (!sol_parser_expect(parser, SOL_TOKEN_RIGHT_PAREN, "expected ')' after annotation")) {
        return;
    }
    (void)annotation;
}

static bool sol_parser_is_declaration_start(SolTokenKind kind) {
    return kind == SOL_TOKEN_AT || kind == SOL_TOKEN_PUBLIC || kind == SOL_TOKEN_PRIVATE
        || kind == SOL_TOKEN_RECORD || kind == SOL_TOKEN_ENUM || kind == SOL_TOKEN_OPEN
        || kind == SOL_TOKEN_CAPABILITY || kind == SOL_TOKEN_FUNCTION;
}

static void sol_parser_recover_declaration(SolParser *parser, size_t failed_at) {
    sol_parser_significant_index(parser);
    if (parser->cursor <= failed_at && sol_parser_kind(parser) != SOL_TOKEN_EOF) {
        sol_parser_advance(parser);
    }
    while (sol_parser_kind(parser) != SOL_TOKEN_EOF
        && !sol_parser_is_declaration_start(sol_parser_kind(parser))) {
        sol_parser_advance(parser);
    }
}

static void sol_parser_declaration(SolParser *parser) {
    size_t failed_at = sol_parser_significant_index(parser);
    size_t parameter_mark = parser->tree->parameter_count;
    size_t type_mark = parser->tree->type_count;
    size_t type_argument_mark = parser->tree->type_argument_count;
    size_t field_mark = parser->tree->field_count;
    size_t variant_mark = parser->tree->variant_count;
    size_t effect_mark = parser->tree->effect_count;
    size_t capability_member_mark = parser->tree->capability_member_count;
    size_t start = sol_parser_current(parser).span.start;
    while (sol_parser_kind(parser) == SOL_TOKEN_AT) {
        sol_parser_annotation(parser);
    }
    bool is_public = sol_parser_match(parser, SOL_TOKEN_PUBLIC);
    if (!is_public) {
        sol_parser_match(parser, SOL_TOKEN_PRIVATE);
    }

    SolItemKind item_kind;
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
    SolCapabilityMemberId first_member = SOL_AST_NONE;
    bool parsed = false;
    SolTokenKind kind = sol_parser_kind(parser);
    if (kind == SOL_TOKEN_RECORD) {
        item_kind = SOL_ITEM_RECORD;
        parsed = sol_parser_record(parser, &name, &whole, &first_field);
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
        parsed = sol_parser_enum(parser, &name, &whole, &first_variant);
    } else if (kind == SOL_TOKEN_CAPABILITY) {
        item_kind = SOL_ITEM_CAPABILITY;
        parsed = sol_parser_capability(parser, &name, &whole, &first_member);
    } else if (kind == SOL_TOKEN_FUNCTION) {
        item_kind = SOL_ITEM_FUNCTION;
        parsed = sol_parser_function(
            parser,
            false,
            &name,
            &whole,
            &body,
            &first_parameter,
            &return_type,
            &return_type_id,
            &first_effect,
            &has_effect_clause
        );
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
            .name = name,
            .span = whole,
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
            .first_member = first_member,
        });
    } else {
        parser->tree->parameter_count = parameter_mark;
        parser->tree->type_count = type_mark;
        parser->tree->type_argument_count = type_argument_mark;
        parser->tree->field_count = field_mark;
        parser->tree->variant_count = variant_mark;
        parser->tree->effect_count = effect_mark;
        parser->tree->capability_member_count = capability_member_mark;
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
        SolSpan ignored;
        sol_parser_path(parser, &ignored, "expected an import path");
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
