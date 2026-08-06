#include "sol/effectcheck.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SOL_EFFECT_WALK_GRAPH,
    SOL_EFFECT_WALK_INFER,
    SOL_EFFECT_WALK_VALIDATE,
} SolEffectWalkMode;

typedef struct {
    SolDefId from;
    SolDefId to;
    size_t next_outgoing;
    size_t next_incoming;
} SolEffectEdge;

typedef struct {
    const SolSource *source;
    const SolSyntaxTree *syntax;
    const SolHirModule *hir;
    const SolTypeTable *types;
    SolEffectTable *effects;
    SolDiagnostics *diagnostics;
    unsigned char *visited;
    unsigned char *reported_substitutions;
    SolEffectEdge *edges;
    size_t *first_outgoing;
    size_t *first_incoming;
    size_t edge_count;
    size_t edge_capacity;
    SolDefId current_function;
    SolEffectWalkMode mode;
    size_t depth;
    bool depth_reported;
    bool malformed;
    bool allocation_failed;
} SolEffectChecker;

void sol_effect_table_init(SolEffectTable *table) {
    memset(table, 0, sizeof(*table));
}

void sol_effect_table_free(SolEffectTable *table) {
    for (size_t index = 0; index < table->function_count; ++index) {
        free(table->functions[index].atoms);
    }
    for (size_t index = 0; index < table->capability_member_count; ++index) {
        free(table->capability_members[index].atoms);
    }
    free(table->functions);
    free(table->capability_members);
    memset(table, 0, sizeof(*table));
}

static bool sol_effect_span_valid(const SolSource *source, SolSpan span) {
    return span.start <= span.end && span.end <= source->length;
}

static int sol_effect_path_next_byte(const SolSource *source, SolSpan span, size_t *cursor) {
    while (*cursor < span.end) {
        unsigned char byte = (unsigned char)source->text[*cursor];
        if (byte == ' ' || byte == '\t' || byte == '\r' || byte == '\n') {
            ++*cursor;
            continue;
        }
        if (byte == '/' && *cursor + 1 < span.end && source->text[*cursor + 1] == '/') {
            *cursor += 2;
            while (*cursor < span.end
                && source->text[*cursor] != '\r'
                && source->text[*cursor] != '\n') {
                ++*cursor;
            }
            continue;
        }
        if (byte == '/' && *cursor + 1 < span.end && source->text[*cursor + 1] == '*') {
            *cursor += 2;
            size_t depth = 1;
            while (*cursor < span.end && depth != 0) {
                if (*cursor + 1 < span.end
                    && source->text[*cursor] == '/'
                    && source->text[*cursor + 1] == '*') {
                    ++depth;
                    *cursor += 2;
                } else if (*cursor + 1 < span.end
                    && source->text[*cursor] == '*'
                    && source->text[*cursor + 1] == '/') {
                    --depth;
                    *cursor += 2;
                } else {
                    ++*cursor;
                }
            }
            continue;
        }
        ++*cursor;
        return (int)byte;
    }
    return -1;
}

static bool sol_effect_span_equal(const SolSource *source, SolSpan left, SolSpan right) {
    size_t left_cursor = left.start;
    size_t right_cursor = right.start;
    for (;;) {
        int left_byte = sol_effect_path_next_byte(source, left, &left_cursor);
        int right_byte = sol_effect_path_next_byte(source, right, &right_cursor);
        if (left_byte != right_byte) return false;
        if (left_byte < 0) return true;
    }
}

static bool sol_effect_span_text_equal(
    const SolSource *source,
    SolSpan span,
    const char *text
) {
    size_t length = span.end - span.start;
    return strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static void sol_effect_error(
    SolEffectChecker *checker,
    const char *code,
    SolSpan span,
    const char *message
) {
    sol_diagnostics_add(
        checker->diagnostics,
        code,
        SOL_SEVERITY_ERROR,
        span,
        "%s",
        message
    );
}

static bool sol_effect_atom_equal(
    const SolEffectChecker *checker,
    const SolEffectAtom *left,
    const SolEffectAtom *right
) {
    if (left->argument_kind != right->argument_kind
        || !sol_effect_span_equal(checker->source, left->name, right->name)) {
        return false;
    }
    switch (left->argument_kind) {
        case SOL_EFFECT_ATOM_STATIC_PATH:
            return sol_effect_span_equal(checker->source, left->argument, right->argument);
        case SOL_EFFECT_ATOM_PARAMETER:
            return left->parameter == right->parameter;
        case SOL_EFFECT_ATOM_NO_ARGUMENT:
        case SOL_EFFECT_ATOM_SELF:
            return true;
    }
    return false;
}

static bool sol_effect_row_append(
    SolEffectChecker *checker,
    SolEffectRow *row,
    SolEffectAtom atom
) {
    for (size_t index = 0; index < row->count; ++index) {
        if (sol_effect_atom_equal(checker, &row->atoms[index], &atom)) return true;
    }
    if (row->count == SIZE_MAX / sizeof(*row->atoms)) {
        checker->allocation_failed = true;
        return false;
    }
    SolEffectAtom *grown = realloc(row->atoms, (row->count + 1) * sizeof(*row->atoms));
    if (grown == NULL) {
        checker->allocation_failed = true;
        return false;
    }
    row->atoms = grown;
    row->atoms[row->count++] = atom;
    return true;
}

static bool sol_effect_parameter_is_capability(
    SolEffectChecker *checker,
    SolParameterId parameter_id
) {
    if (parameter_id >= checker->syntax->parameter_count) {
        checker->malformed = true;
        return false;
    }
    const SolParameter *parameter = &checker->syntax->parameters[parameter_id];
    if (parameter->type_id >= checker->types->declared_type_count) {
        checker->malformed = true;
        return false;
    }
    SolType type = checker->types->declared_types[parameter->type_id];
    return type.kind == SOL_TYPE_NOMINAL
        && type.definition < checker->syntax->item_count
        && checker->syntax->items[type.definition].kind == SOL_ITEM_CAPABILITY;
}

static SolParameterId sol_effect_find_parameter(
    SolEffectChecker *checker,
    SolParameterId parameter_id,
    SolSpan name,
    bool capability_only
) {
    size_t traversed = 0;
    while (parameter_id != SOL_AST_NONE) {
        if (parameter_id >= checker->syntax->parameter_count
            || traversed++ >= checker->syntax->parameter_count) {
            checker->malformed = true;
            return SOL_AST_NONE;
        }
        const SolParameter *parameter = &checker->syntax->parameters[parameter_id];
        if (sol_effect_span_equal(checker->source, parameter->name, name)
            && (!capability_only
                || sol_effect_parameter_is_capability(checker, parameter_id))) {
            return parameter_id;
        }
        parameter_id = parameter->next;
    }
    return SOL_AST_NONE;
}

static SolEffectAtom sol_effect_normalize_atom(
    SolEffectChecker *checker,
    const SolEffect *effect,
    SolParameterId first_parameter,
    bool member
) {
    SolEffectAtom atom = {
        .name = effect->name,
        .argument = effect->argument,
        .span = effect->span,
        .argument_kind = SOL_EFFECT_ATOM_NO_ARGUMENT,
        .parameter = SOL_AST_NONE,
    };
    if (!effect->has_argument) return atom;
    if (member && sol_effect_span_text_equal(checker->source, effect->argument, "Self")) {
        atom.argument_kind = SOL_EFFECT_ATOM_SELF;
        return atom;
    }
    SolParameterId parameter = sol_effect_find_parameter(
        checker,
        first_parameter,
        effect->argument,
        true
    );
    if (parameter != SOL_AST_NONE) {
        atom.argument_kind = SOL_EFFECT_ATOM_PARAMETER;
        atom.parameter = parameter;
    } else {
        atom.argument_kind = SOL_EFFECT_ATOM_STATIC_PATH;
    }
    return atom;
}

static bool sol_effect_syntax_equal(
    const SolEffectChecker *checker,
    const SolEffect *left,
    const SolEffect *right
) {
    return left->is_pure == right->is_pure
        && left->has_argument == right->has_argument
        && sol_effect_span_equal(checker->source, left->name, right->name)
        && (!left->has_argument
            || sol_effect_span_equal(checker->source, left->argument, right->argument));
}

static void sol_effect_validate_and_normalize_row(
    SolEffectChecker *checker,
    SolEffectId first_effect,
    SolSpan owner_span,
    size_t owner_item,
    SolParameterId first_parameter,
    bool member,
    SolEffectRow *row
) {
    SolEffectId effect_id = first_effect;
    size_t count = 0;
    bool saw_pure = false;
    while (effect_id != SOL_AST_NONE) {
        if (effect_id >= checker->syntax->effect_count
            || count++ >= checker->syntax->effect_count) {
            checker->malformed = true;
            return;
        }
        const SolEffect *effect = &checker->syntax->effects[effect_id];
        if (effect->owner_item != owner_item) {
            checker->malformed = true;
            return;
        }
        if (effect->is_pure) saw_pure = true;
        if (effect->is_pure && effect->has_argument) {
            sol_effect_error(
                checker,
                "SOL-EFFECT-001",
                effect->span,
                "pure cannot have an effect argument"
            );
        }
        SolEffectId previous = first_effect;
        size_t previous_count = 0;
        while (previous != effect_id) {
            if (previous >= checker->syntax->effect_count
                || previous_count++ >= checker->syntax->effect_count) {
                checker->malformed = true;
                return;
            }
            if (sol_effect_syntax_equal(
                checker,
                &checker->syntax->effects[previous],
                effect
            )) {
                sol_effect_error(
                    checker,
                    "SOL-EFFECT-001",
                    effect->span,
                    "duplicate effect in function row"
                );
                break;
            }
            previous = checker->syntax->effects[previous].next;
        }
        if (!effect->is_pure) {
            SolEffectAtom atom = sol_effect_normalize_atom(
                checker,
                effect,
                first_parameter,
                member
            );
            sol_effect_row_append(checker, row, atom);
        }
        effect_id = effect->next;
    }
    if (saw_pure && count > 1) {
        sol_effect_error(
            checker,
            "SOL-EFFECT-001",
            owner_span,
            "pure cannot be combined with other effects"
        );
    }
}

static bool sol_effect_capability_origin(
    SolEffectChecker *checker,
    SolExprId expression_id,
    SolParameterId *parameter_id
) {
    if (expression_id >= checker->syntax->expression_count) {
        checker->malformed = true;
        return false;
    }
    const SolExpr *expression = &checker->syntax->expressions[expression_id];
    SolResolution resolution = checker->hir->resolutions[expression_id];
    if (expression->kind != SOL_EXPR_PATH
        || resolution.kind != SOL_RESOLUTION_LOCAL
        || resolution.target >= checker->hir->local_count) {
        return false;
    }
    const SolHirLocal *local = &checker->hir->locals[resolution.target];
    if (local->owner != checker->current_function
        || resolution.target >= checker->types->local_count
        || checker->types->local_capability_origins[resolution.target] == SOL_AST_NONE) {
        return false;
    }
    SolType type = checker->types->locals[resolution.target];
    if (type.kind != SOL_TYPE_NOMINAL
        || type.definition >= checker->syntax->item_count
        || checker->syntax->items[type.definition].kind != SOL_ITEM_CAPABILITY) {
        return false;
    }
    *parameter_id = checker->types->local_capability_origins[resolution.target];
    return true;
}

static SolExprId sol_effect_find_actual_argument(
    SolEffectChecker *checker,
    SolParameterId first_parameter,
    SolParameterId required_parameter,
    SolArgumentId argument_id
) {
    SolParameterId positional_parameter = first_parameter;
    size_t traversed = 0;
    while (argument_id != SOL_AST_NONE) {
        if (argument_id >= checker->syntax->argument_count
            || traversed++ >= checker->syntax->argument_count) {
            checker->malformed = true;
            return SOL_AST_NONE;
        }
        const SolArgument *argument = &checker->syntax->arguments[argument_id];
        SolParameterId matched = SOL_AST_NONE;
        if (argument->is_named) {
            matched = sol_effect_find_parameter(
                checker,
                first_parameter,
                argument->name,
                false
            );
        } else {
            matched = positional_parameter;
            if (positional_parameter != SOL_AST_NONE) {
                if (positional_parameter >= checker->syntax->parameter_count) {
                    checker->malformed = true;
                    return SOL_AST_NONE;
                }
                positional_parameter = checker->syntax->parameters[positional_parameter].next;
            }
        }
        if (matched == required_parameter) return argument->value;
        argument_id = argument->next;
    }
    return SOL_AST_NONE;
}

static bool sol_effect_instantiate_atom(
    SolEffectChecker *checker,
    const SolExpr *call,
    const SolEffectAtom *required,
    SolParameterId first_parameter,
    SolExprId receiver,
    SolEffectAtom *instantiated
) {
    *instantiated = *required;
    if (required->argument_kind == SOL_EFFECT_ATOM_NO_ARGUMENT
        || required->argument_kind == SOL_EFFECT_ATOM_STATIC_PATH) {
        return true;
    }
    SolExprId actual = receiver;
    if (required->argument_kind == SOL_EFFECT_ATOM_PARAMETER) {
        actual = sol_effect_find_actual_argument(
            checker,
            first_parameter,
            required->parameter,
            call->as.call.first_argument
        );
        if (actual == SOL_AST_NONE) {
            if (!checker->malformed) checker->malformed = true;
            return false;
        }
    }
    if (actual == SOL_AST_NONE) {
        checker->malformed = true;
        return false;
    }
    SolParameterId caller_parameter = SOL_AST_NONE;
    if (!sol_effect_capability_origin(checker, actual, &caller_parameter)) {
        if (checker->malformed) return false;
        if (!checker->reported_substitutions[actual]) {
            sol_effect_error(
                checker,
                "SOL-EFFECT-003",
                checker->syntax->expressions[actual].span,
                "effect-bearing arguments must have known capability parameter authority"
            );
            checker->reported_substitutions[actual] = 1;
        }
        return false;
    }
    instantiated->argument_kind = SOL_EFFECT_ATOM_PARAMETER;
    instantiated->parameter = caller_parameter;
    instantiated->argument = checker->syntax->parameters[caller_parameter].name;
    return true;
}

typedef enum {
    SOL_EFFECT_CALL_NONE,
    SOL_EFFECT_CALL_FUNCTION,
    SOL_EFFECT_CALL_MEMBER,
} SolEffectCallKind;

typedef struct {
    SolEffectCallKind kind;
    size_t target;
    SolParameterId first_parameter;
    SolExprId receiver;
} SolEffectCall;

static SolEffectCall sol_effect_resolve_call(
    SolEffectChecker *checker,
    const SolExpr *call
) {
    SolEffectCall result = {
        .kind = SOL_EFFECT_CALL_NONE,
        .target = SOL_AST_NONE,
        .first_parameter = SOL_AST_NONE,
        .receiver = SOL_AST_NONE,
    };
    SolExprId callee_id = call->as.call.callee;
    if (callee_id >= checker->syntax->expression_count) {
        checker->malformed = true;
        return result;
    }
    const SolExpr *callee = &checker->syntax->expressions[callee_id];
    SolResolution resolution = checker->hir->resolutions[callee_id];
    SolType callee_type = checker->types->expressions[callee_id];
    if (callee_type.kind == SOL_TYPE_FUNCTION
        && callee_type.definition < checker->syntax->item_count
        && checker->syntax->items[callee_type.definition].kind == SOL_ITEM_FUNCTION) {
        result.kind = SOL_EFFECT_CALL_FUNCTION;
        result.target = callee_type.definition;
        result.first_parameter = checker->syntax->items[result.target].first_parameter;
    } else if (resolution.kind == SOL_RESOLUTION_DEFINITION
        && resolution.target < checker->syntax->item_count
        && checker->syntax->items[resolution.target].kind == SOL_ITEM_FUNCTION) {
        result.kind = SOL_EFFECT_CALL_FUNCTION;
        result.target = resolution.target;
        result.first_parameter = checker->syntax->items[result.target].first_parameter;
    } else if (callee_type.kind == SOL_TYPE_CAPABILITY_OPERATION
        && callee_type.definition < checker->syntax->capability_member_count
        && callee->kind == SOL_EXPR_FIELD) {
        result.kind = SOL_EFFECT_CALL_MEMBER;
        result.target = callee_type.definition;
        result.first_parameter = checker->syntax->capability_members[result.target].first_parameter;
        result.receiver = callee->as.field.base;
    }
    return result;
}

static void sol_effect_add_edge(
    SolEffectChecker *checker,
    SolDefId from,
    SolDefId to
) {
    if (checker->edge_count == checker->edge_capacity) {
        size_t capacity = checker->edge_capacity == 0 ? 16 : checker->edge_capacity * 2;
        if (capacity < checker->edge_capacity
            || capacity > SIZE_MAX / sizeof(*checker->edges)) {
            checker->allocation_failed = true;
            return;
        }
        SolEffectEdge *grown = realloc(checker->edges, capacity * sizeof(*checker->edges));
        if (grown == NULL) {
            checker->allocation_failed = true;
            return;
        }
        checker->edges = grown;
        checker->edge_capacity = capacity;
    }
    size_t edge = checker->edge_count++;
    checker->edges[edge] = (SolEffectEdge){
        .from = from,
        .to = to,
        .next_outgoing = checker->first_outgoing[from],
        .next_incoming = checker->first_incoming[to],
    };
    checker->first_outgoing[from] = edge;
    checker->first_incoming[to] = edge;
}

static void sol_effect_process_call(SolEffectChecker *checker, const SolExpr *call) {
    SolEffectCall resolved = sol_effect_resolve_call(checker, call);
    if (resolved.kind == SOL_EFFECT_CALL_NONE || checker->malformed) return;
    if (checker->mode == SOL_EFFECT_WALK_GRAPH) {
        if (resolved.kind == SOL_EFFECT_CALL_FUNCTION) {
            sol_effect_add_edge(checker, checker->current_function, resolved.target);
        }
        return;
    }
    const SolEffectRow *required = resolved.kind == SOL_EFFECT_CALL_FUNCTION
        ? &checker->effects->functions[resolved.target]
        : &checker->effects->capability_members[resolved.target];
    SolEffectRow *caller = &checker->effects->functions[checker->current_function];
    memset(
        checker->reported_substitutions,
        0,
        checker->syntax->expression_count * sizeof(*checker->reported_substitutions)
    );
    for (size_t index = 0; index < required->count; ++index) {
        SolEffectAtom instantiated;
        if (!sol_effect_instantiate_atom(
            checker,
            call,
            &required->atoms[index],
            resolved.first_parameter,
            resolved.receiver,
            &instantiated
        )) {
            continue;
        }
        if (checker->mode == SOL_EFFECT_WALK_INFER) {
            sol_effect_row_append(checker, caller, instantiated);
            continue;
        }
        bool found = false;
        for (size_t declared = 0; declared < caller->count; ++declared) {
            if (sol_effect_atom_equal(checker, &caller->atoms[declared], &instantiated)) {
                found = true;
                break;
            }
        }
        if (!found) {
            sol_effect_error(
                checker,
                "SOL-EFFECT-002",
                call->span,
                "call performs an effect not declared by the caller"
            );
        }
    }
}

static void sol_effect_expression(SolEffectChecker *checker, SolExprId expression_id);

static void sol_effect_arguments(SolEffectChecker *checker, SolArgumentId argument_id) {
    size_t traversed = 0;
    while (argument_id != SOL_AST_NONE) {
        if (argument_id >= checker->syntax->argument_count
            || traversed++ >= checker->syntax->argument_count) {
            checker->malformed = true;
            return;
        }
        const SolArgument *argument = &checker->syntax->arguments[argument_id];
        sol_effect_expression(checker, argument->value);
        argument_id = argument->next;
    }
}

static void sol_effect_expression(SolEffectChecker *checker, SolExprId expression_id) {
    if (expression_id >= checker->syntax->expression_count) {
        checker->malformed = true;
        return;
    }
    if (checker->visited[expression_id]) return;
    if (checker->depth >= 256) {
        if (!checker->depth_reported) {
            sol_effect_error(
                checker,
                "SOL-EFFECT-004",
                checker->syntax->expressions[expression_id].span,
                "expression structure exceeds the effect-checking limit of 256"
            );
            checker->depth_reported = true;
        }
        return;
    }
    checker->visited[expression_id] = 1;
    ++checker->depth;
    const SolExpr *expression = &checker->syntax->expressions[expression_id];
    switch (expression->kind) {
        case SOL_EXPR_UNARY:
            sol_effect_expression(checker, expression->as.unary.operand);
            break;
        case SOL_EXPR_BINARY:
            sol_effect_expression(checker, expression->as.binary.left);
            sol_effect_expression(checker, expression->as.binary.right);
            break;
        case SOL_EXPR_CALL:
            sol_effect_process_call(checker, expression);
            sol_effect_expression(checker, expression->as.call.callee);
            sol_effect_arguments(checker, expression->as.call.first_argument);
            break;
        case SOL_EXPR_FIELD:
            sol_effect_expression(checker, expression->as.field.base);
            break;
        case SOL_EXPR_RECORD:
            sol_effect_expression(checker, expression->as.record.type);
            sol_effect_arguments(checker, expression->as.record.first_field);
            break;
        case SOL_EXPR_IF:
            sol_effect_expression(checker, expression->as.if_expr.condition);
            sol_effect_expression(checker, expression->as.if_expr.then_branch);
            sol_effect_expression(checker, expression->as.if_expr.else_branch);
            break;
        case SOL_EXPR_MATCH: {
            sol_effect_expression(checker, expression->as.match_expr.scrutinee);
            SolMatchArmId arm_id = expression->as.match_expr.first_arm;
            size_t traversed = 0;
            while (arm_id != SOL_AST_NONE) {
                if (arm_id >= checker->syntax->match_arm_count
                    || traversed++ >= checker->syntax->match_arm_count) {
                    checker->malformed = true;
                    break;
                }
                const SolMatchArm *arm = &checker->syntax->match_arms[arm_id];
                sol_effect_expression(checker, arm->value);
                arm_id = arm->next;
            }
            break;
        }
        case SOL_EXPR_BLOCK: {
            SolStatementId statement_id = expression->as.block.first_statement;
            size_t traversed = 0;
            while (statement_id != SOL_AST_NONE) {
                if (statement_id >= checker->syntax->statement_count
                    || traversed++ >= checker->syntax->statement_count) {
                    checker->malformed = true;
                    break;
                }
                const SolStatement *statement = &checker->syntax->statements[statement_id];
                SolExprId value = statement->kind == SOL_STATEMENT_LET
                    ? statement->as.let_statement.value
                    : statement->as.expression;
                sol_effect_expression(checker, value);
                statement_id = statement->next;
            }
            break;
        }
        case SOL_EXPR_PROPAGATE:
            sol_effect_expression(checker, expression->as.propagated);
            break;
        default:
            break;
    }
    --checker->depth;
}

static void sol_effect_walk_function(
    SolEffectChecker *checker,
    SolDefId function,
    SolEffectWalkMode mode
) {
    const SolSyntaxItem *item = &checker->syntax->items[function];
    if (item->body == SOL_AST_NONE) return;
    memset(
        checker->visited,
        0,
        checker->syntax->expression_count * sizeof(*checker->visited)
    );
    checker->current_function = function;
    checker->mode = mode;
    checker->depth = 0;
    sol_effect_expression(checker, item->body);
}

static bool sol_effect_validate_inputs(SolEffectChecker *checker) {
    const SolSource *source = checker->source;
    const SolSyntaxTree *syntax = checker->syntax;
    const SolHirModule *hir = checker->hir;
    const SolTypeTable *types = checker->types;
    if (syntax->item_count > syntax->item_capacity
        || syntax->expression_count > syntax->expression_capacity
        || syntax->parameter_count > syntax->parameter_capacity
        || syntax->argument_count > syntax->argument_capacity
        || syntax->statement_count > syntax->statement_capacity
        || syntax->match_arm_count > syntax->match_arm_capacity
        || syntax->effect_count > syntax->effect_capacity
        || syntax->capability_member_count > syntax->capability_member_capacity
        || (syntax->item_count != 0 && syntax->items == NULL)
        || (syntax->expression_count != 0 && syntax->expressions == NULL)
        || (syntax->parameter_count != 0 && syntax->parameters == NULL)
        || (syntax->argument_count != 0 && syntax->arguments == NULL)
        || (syntax->statement_count != 0 && syntax->statements == NULL)
        || (syntax->match_arm_count != 0 && syntax->match_arms == NULL)
        || (syntax->effect_count != 0 && syntax->effects == NULL)
        || (syntax->capability_member_count != 0 && syntax->capability_members == NULL)
        || hir->definition_count != syntax->item_count
        || hir->resolution_count != syntax->expression_count
        || hir->local_count > hir->local_capacity
        || (hir->resolution_count != 0 && hir->resolutions == NULL)
        || (hir->local_count != 0 && hir->locals == NULL)
        || types->expression_count != syntax->expression_count
        || types->local_count != hir->local_count
        || types->declared_type_count != syntax->type_count
        || (types->expression_count != 0 && types->expressions == NULL)
        || (types->local_count != 0 && types->locals == NULL)
        || (types->local_count != 0 && types->local_capability_origins == NULL)
        || (types->declared_type_count != 0 && types->declared_types == NULL)) {
        return false;
    }
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if ((item->body != SOL_AST_NONE && item->body >= syntax->expression_count)
            || (item->first_effect != SOL_AST_NONE
                && item->first_effect >= syntax->effect_count)
            || (item->first_parameter != SOL_AST_NONE
                && item->first_parameter >= syntax->parameter_count)
            || (item->first_member != SOL_AST_NONE
                && item->first_member >= syntax->capability_member_count)) {
            return false;
        }
    }
    for (size_t index = 0; index < syntax->parameter_count; ++index) {
        const SolParameter *parameter = &syntax->parameters[index];
        if (!sol_effect_span_valid(source, parameter->name)
            || parameter->type_id >= syntax->type_count
            || (parameter->next != SOL_AST_NONE
                && parameter->next >= syntax->parameter_count)) {
            return false;
        }
    }
    for (size_t index = 0; index < syntax->argument_count; ++index) {
        const SolArgument *argument = &syntax->arguments[index];
        if (argument->value >= syntax->expression_count
            || (argument->is_named && !sol_effect_span_valid(source, argument->name))
            || (argument->next != SOL_AST_NONE
                && argument->next >= syntax->argument_count)) {
            return false;
        }
    }
    for (size_t index = 0; index < syntax->expression_count; ++index) {
        const SolExpr *expression = &syntax->expressions[index];
        if ((expression->kind == SOL_EXPR_CALL
                && expression->as.call.first_argument != SOL_AST_NONE
                && expression->as.call.first_argument >= syntax->argument_count)
            || (expression->kind == SOL_EXPR_RECORD
                && expression->as.record.first_field != SOL_AST_NONE
                && expression->as.record.first_field >= syntax->argument_count)) {
            return false;
        }
    }
    for (size_t index = 0; index < syntax->effect_count; ++index) {
        const SolEffect *effect = &syntax->effects[index];
        if (!sol_effect_span_valid(source, effect->name)
            || !sol_effect_span_valid(source, effect->argument)
            || !sol_effect_span_valid(source, effect->span)
            || (effect->next != SOL_AST_NONE && effect->next >= syntax->effect_count)
            || effect->owner_item >= syntax->item_count) {
            return false;
        }
    }
    for (size_t index = 0; index < syntax->capability_member_count; ++index) {
        const SolCapabilityMember *member = &syntax->capability_members[index];
        if (!sol_effect_span_valid(source, member->span)
            || (member->first_effect != SOL_AST_NONE
                && member->first_effect >= syntax->effect_count)
            || (member->first_parameter != SOL_AST_NONE
                && member->first_parameter >= syntax->parameter_count)
            || (member->next != SOL_AST_NONE
                && member->next >= syntax->capability_member_count)
            || member->owner_item >= syntax->item_count) {
            return false;
        }
    }
    for (size_t index = 0; index < hir->local_count; ++index) {
        const SolHirLocal *local = &hir->locals[index];
        SolParameterId origin = types->local_capability_origins[index];
        SolType type = types->locals[index];
        bool capability_type = type.kind == SOL_TYPE_NOMINAL
            && type.definition < syntax->item_count
            && syntax->items[type.definition].kind == SOL_ITEM_CAPABILITY;
        SolParameterId expected = SOL_AST_NONE;
        if (local->kind == SOL_LOCAL_PARAMETER) {
            if (local->syntax_id >= syntax->parameter_count) return false;
            if (capability_type) expected = local->syntax_id;
        } else if (local->kind == SOL_LOCAL_BINDING) {
            if (local->syntax_id >= syntax->statement_count) return false;
            const SolStatement *statement = &syntax->statements[local->syntax_id];
            if (statement->kind != SOL_STATEMENT_LET
                || statement->as.let_statement.value >= syntax->expression_count) {
                return false;
            }
            SolExprId initializer_id = statement->as.let_statement.value;
            const SolExpr *initializer = &syntax->expressions[initializer_id];
            SolResolution resolution = hir->resolutions[initializer_id];
            if (initializer->kind == SOL_EXPR_PATH
                && resolution.kind == SOL_RESOLUTION_LOCAL
                && resolution.target < index
                && hir->locals[resolution.target].owner == local->owner) {
                expected = types->local_capability_origins[resolution.target];
            }
        }
        if (origin != expected) return false;
        if (origin != SOL_AST_NONE
            && (!capability_type || origin >= syntax->parameter_count)) return false;
    }
    return true;
}

static bool sol_effect_allocate_table(SolEffectChecker *checker) {
    size_t function_count = checker->syntax->item_count;
    size_t member_count = checker->syntax->capability_member_count;
    SolEffectRow *functions = calloc(function_count, sizeof(*functions));
    SolEffectRow *members = calloc(member_count, sizeof(*members));
    if ((function_count != 0 && functions == NULL)
        || (member_count != 0 && members == NULL)) {
        free(functions);
        free(members);
        checker->allocation_failed = true;
        return false;
    }
    checker->effects->functions = functions;
    checker->effects->function_count = function_count;
    checker->effects->capability_members = members;
    checker->effects->capability_member_count = member_count;
    return true;
}

static bool sol_effect_prepare_graph(SolEffectChecker *checker) {
    size_t count = checker->syntax->item_count;
    if (count > SIZE_MAX / sizeof(*checker->first_outgoing)) {
        checker->allocation_failed = true;
        return false;
    }
    checker->first_outgoing = malloc(count * sizeof(*checker->first_outgoing));
    checker->first_incoming = malloc(count * sizeof(*checker->first_incoming));
    if (count != 0
        && (checker->first_outgoing == NULL || checker->first_incoming == NULL)) {
        checker->allocation_failed = true;
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        checker->first_outgoing[index] = SOL_AST_NONE;
        checker->first_incoming[index] = SOL_AST_NONE;
    }
    for (size_t index = 0; index < count; ++index) {
        if (checker->syntax->items[index].kind == SOL_ITEM_FUNCTION) {
            sol_effect_walk_function(checker, index, SOL_EFFECT_WALK_GRAPH);
        }
    }
    return !checker->malformed && !checker->allocation_failed;
}

static unsigned char *sol_effect_find_recursive_functions(SolEffectChecker *checker) {
    size_t count = checker->syntax->item_count;
    if (count > SIZE_MAX / sizeof(SolDefId)
        || count > SIZE_MAX / sizeof(size_t)) {
        checker->allocation_failed = true;
        return NULL;
    }
    unsigned char *recursive = calloc(count, sizeof(*recursive));
    unsigned char *state = calloc(count, sizeof(*state));
    size_t *component = malloc(count * sizeof(*component));
    size_t *component_sizes = calloc(count, sizeof(*component_sizes));
    SolDefId *order = count == 0 ? NULL : malloc(count * sizeof(*order));
    SolDefId *stack = count == 0 ? NULL : malloc(count * sizeof(*stack));
    size_t *edge_stack = count == 0 ? NULL : malloc(count * sizeof(*edge_stack));
    if (count != 0 && (recursive == NULL || state == NULL || component == NULL
            || component_sizes == NULL || order == NULL || stack == NULL
            || edge_stack == NULL)) {
        free(recursive);
        free(state);
        free(component);
        free(component_sizes);
        free(order);
        free(stack);
        free(edge_stack);
        checker->allocation_failed = true;
        return NULL;
    }
    for (size_t index = 0; index < count; ++index) component[index] = SOL_AST_NONE;

    size_t order_count = 0;
    for (size_t root = 0; root < count; ++root) {
        if (state[root] != 0) continue;
        size_t stack_count = 1;
        stack[0] = root;
        edge_stack[0] = checker->first_outgoing[root];
        state[root] = 1;
        while (stack_count != 0) {
            size_t top = stack_count - 1;
            size_t edge = edge_stack[top];
            if (edge == SOL_AST_NONE) {
                SolDefId finished = stack[top];
                state[finished] = 2;
                order[order_count++] = finished;
                --stack_count;
                continue;
            }
            edge_stack[top] = checker->edges[edge].next_outgoing;
            SolDefId target = checker->edges[edge].to;
            if (state[target] == 0) {
                state[target] = 1;
                stack[stack_count] = target;
                edge_stack[stack_count] = checker->first_outgoing[target];
                ++stack_count;
            }
        }
    }

    size_t component_count = 0;
    while (order_count != 0) {
        SolDefId root = order[--order_count];
        if (component[root] != SOL_AST_NONE) continue;
        size_t stack_count = 1;
        stack[0] = root;
        component[root] = component_count;
        while (stack_count != 0) {
            SolDefId current = stack[--stack_count];
            ++component_sizes[component_count];
            size_t edge = checker->first_incoming[current];
            while (edge != SOL_AST_NONE) {
                SolDefId source = checker->edges[edge].from;
                if (component[source] == SOL_AST_NONE) {
                    component[source] = component_count;
                    stack[stack_count++] = source;
                }
                edge = checker->edges[edge].next_incoming;
            }
        }
        ++component_count;
    }

    for (size_t node = 0; node < count; ++node) {
        if (component_sizes[component[node]] > 1) recursive[node] = 1;
    }
    for (size_t edge = 0; edge < checker->edge_count; ++edge) {
        if (checker->edges[edge].from == checker->edges[edge].to) {
            recursive[checker->edges[edge].from] = 1;
        }
    }

    free(state);
    free(component);
    free(component_sizes);
    free(order);
    free(stack);
    free(edge_stack);
    return recursive;
}

static void sol_effect_infer_functions(
    SolEffectChecker *checker,
    const unsigned char *recursive
) {
    size_t count = checker->syntax->item_count;
    unsigned char *ready = calloc(count, sizeof(*ready));
    size_t *pending = calloc(count, sizeof(*pending));
    SolDefId *queue = count == 0 ? NULL : malloc(count * sizeof(*queue));
    if (count != 0 && (ready == NULL || pending == NULL || queue == NULL)) {
        free(ready);
        free(pending);
        free(queue);
        checker->allocation_failed = true;
        return;
    }
    for (size_t index = 0; index < count; ++index) {
        const SolSyntaxItem *item = &checker->syntax->items[index];
        if (item->kind != SOL_ITEM_FUNCTION || item->has_effect_clause
            || item->is_public || recursive[index]) {
            ready[index] = 1;
        }
    }
    for (size_t edge = 0; edge < checker->edge_count; ++edge) {
        SolDefId from = checker->edges[edge].from;
        SolDefId to = checker->edges[edge].to;
        if (!ready[from] && !ready[to]) ++pending[from];
    }
    size_t queue_start = 0;
    size_t queue_count = 0;
    for (size_t index = 0; index < count; ++index) {
        if (!ready[index] && pending[index] == 0) queue[queue_count++] = index;
    }
    while (queue_start < queue_count) {
        SolDefId function = queue[queue_start++];
        checker->effects->functions[function].inferred = true;
        sol_effect_walk_function(checker, function, SOL_EFFECT_WALK_INFER);
        ready[function] = 1;
        size_t edge = checker->first_incoming[function];
        while (edge != SOL_AST_NONE) {
            SolDefId dependent = checker->edges[edge].from;
            if (!ready[dependent] && pending[dependent] != 0) {
                --pending[dependent];
                if (pending[dependent] == 0) queue[queue_count++] = dependent;
            }
            edge = checker->edges[edge].next_incoming;
        }
    }
    free(ready);
    free(pending);
    free(queue);
}

bool sol_effect_check(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    SolEffectTable *effects,
    SolDiagnostics *diagnostics
) {
    if (diagnostics == NULL) return false;
    if (source == NULL || source->text == NULL || syntax == NULL || hir == NULL || types == NULL
        || effects == NULL) {
        sol_diagnostics_add(
            diagnostics,
            "SOL-INTERNAL-004",
            SOL_SEVERITY_ERROR,
            (SolSpan){0},
            "null compiler input passed to effect checking"
        );
        return false;
    }
    SolEffectChecker checker = {
        .source = source,
        .syntax = syntax,
        .hir = hir,
        .types = types,
        .effects = effects,
        .diagnostics = diagnostics,
    };
    if (effects->functions != NULL || effects->function_count != 0
        || effects->capability_members != NULL || effects->capability_member_count != 0
        || !sol_effect_validate_inputs(&checker)) {
        sol_effect_error(
            &checker,
            "SOL-INTERNAL-004",
            (SolSpan){0},
            "invalid syntax, semantic input, or output table passed to effect checking"
        );
        return false;
    }
    if (!sol_effect_allocate_table(&checker)) return false;
    checker.visited = calloc(syntax->expression_count, sizeof(*checker.visited));
    checker.reported_substitutions = calloc(
        syntax->expression_count,
        sizeof(*checker.reported_substitutions)
    );
    if (syntax->expression_count != 0
        && (checker.visited == NULL || checker.reported_substitutions == NULL)) {
        checker.allocation_failed = true;
    }

    unsigned char *boundary_reported = calloc(syntax->item_count, sizeof(*boundary_reported));
    if (syntax->item_count != 0 && boundary_reported == NULL) {
        checker.allocation_failed = true;
    }
    if (!checker.allocation_failed) {
        for (size_t index = 0; index < syntax->item_count; ++index) {
            const SolSyntaxItem *item = &syntax->items[index];
            if (item->kind != SOL_ITEM_FUNCTION) continue;
            SolEffectRow *row = &effects->functions[index];
            sol_effect_validate_and_normalize_row(
                &checker,
                item->first_effect,
                item->span,
                index,
                item->first_parameter,
                false,
                row
            );
            if (item->is_public && !item->has_effect_clause) {
                sol_effect_error(
                    &checker,
                    "SOL-EFFECT-005",
                    item->span,
                    "public functions must declare an explicit effects clause"
                );
                boundary_reported[index] = 1;
            }
        }
        for (size_t index = 0; index < syntax->capability_member_count; ++index) {
            const SolCapabilityMember *member = &syntax->capability_members[index];
            SolEffectRow *row = &effects->capability_members[index];
            sol_effect_validate_and_normalize_row(
                &checker,
                member->first_effect,
                member->span,
                member->owner_item,
                member->first_parameter,
                true,
                row
            );
            if (!member->has_effect_clause) {
                sol_effect_error(
                    &checker,
                    "SOL-EFFECT-005",
                    member->span,
                    "capability members must declare an explicit effects clause"
                );
            }
        }
    }

    if (!checker.malformed && !checker.allocation_failed
        && sol_effect_prepare_graph(&checker)) {
        unsigned char *recursive = sol_effect_find_recursive_functions(&checker);
        size_t count = syntax->item_count;
        if (!checker.allocation_failed) {
            for (size_t index = 0; index < count; ++index) {
                const SolSyntaxItem *item = &syntax->items[index];
                if (item->kind == SOL_ITEM_FUNCTION && !item->has_effect_clause
                    && recursive[index] && !boundary_reported[index]) {
                    sol_effect_error(
                        &checker,
                        "SOL-EFFECT-005",
                        item->span,
                        "recursive functions must declare an explicit effects clause"
                    );
                }
            }
            sol_effect_infer_functions(&checker, recursive);
        }
        free(recursive);
        if (!checker.malformed && !checker.allocation_failed) {
            for (size_t index = 0; index < syntax->item_count; ++index) {
                const SolSyntaxItem *item = &syntax->items[index];
                if (item->kind == SOL_ITEM_FUNCTION && item->has_effect_clause) {
                    sol_effect_walk_function(&checker, index, SOL_EFFECT_WALK_VALIDATE);
                }
            }
        }
    }

    free(boundary_reported);
    free(checker.edges);
    free(checker.first_outgoing);
    free(checker.first_incoming);
    free(checker.visited);
    free(checker.reported_substitutions);
    if (checker.malformed) {
        sol_effect_error(
            &checker,
            "SOL-INTERNAL-004",
            (SolSpan){0},
            "malformed syntax encountered during effect checking"
        );
    }
    return !checker.malformed
        && !checker.allocation_failed
        && !diagnostics->allocation_failed;
}
