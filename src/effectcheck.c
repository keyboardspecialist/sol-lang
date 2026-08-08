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
    SolExprId expression;
    bool exiting;
} SolEffectExpressionEntry;

typedef struct {
    size_t node;
    bool exiting;
} SolEffectProvenanceEntry;

typedef struct {
    SolSpan name;
    SolParameterId root;
} SolHandledEffect;

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
    SolDefId *parameter_owners;
    SolDefId *expression_owners;
    size_t edge_count;
    size_t edge_capacity;
    SolHandledEffect handled[256];
    size_t handled_count;
    SolDefId current_function;
    SolCapabilityMemberId current_member;
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

static bool sol_effect_atom_is_handled(
    const SolEffectChecker *checker,
    const SolEffectAtom *atom
) {
    if (atom->argument_kind != SOL_EFFECT_ATOM_PARAMETER) return false;
    for (size_t index = checker->handled_count; index > 0; --index) {
        const SolHandledEffect *handled = &checker->handled[index - 1];
        if (atom->parameter == handled->root
            && sol_effect_span_equal(checker->source, atom->name, handled->name)) {
            return true;
        }
    }
    return false;
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
    SolEffectOwnerKind owner_kind,
    size_t owner,
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
        if (effect->owner_kind != owner_kind || effect->owner != owner) {
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
    SolParameterId origin = checker->types->expression_capability_origins[expression_id];
    SolType type = checker->types->expressions[expression_id];
    if (type.kind != SOL_TYPE_NOMINAL
        || type.definition >= checker->syntax->item_count
        || checker->syntax->items[type.definition].kind != SOL_ITEM_CAPABILITY
        || origin == SOL_AST_NONE) {
        return false;
    }
    if (origin >= checker->syntax->parameter_count
        || checker->parameter_owners[origin] != checker->current_function) {
        checker->malformed = true;
        return false;
    }
    *parameter_id = origin;
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
    SolParameterId receiver_parameter,
    SolEffectAtom *instantiated
) {
    *instantiated = *required;
    if (required->argument_kind == SOL_EFFECT_ATOM_NO_ARGUMENT
        || required->argument_kind == SOL_EFFECT_ATOM_STATIC_PATH) {
        return true;
    }
    if (required->argument_kind == SOL_EFFECT_ATOM_SELF) {
        if (receiver_parameter == SOL_AST_NONE
            || receiver_parameter >= checker->syntax->parameter_count) {
            checker->malformed = true;
            return false;
        }
        instantiated->argument_kind = SOL_EFFECT_ATOM_PARAMETER;
        instantiated->parameter = receiver_parameter;
        instantiated->argument = checker->syntax->parameters[receiver_parameter].name;
        return true;
    }
    SolExprId actual = SOL_AST_NONE;
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
    SOL_EFFECT_CALL_SIGNATURE,
    SOL_EFFECT_CALL_MEMBER,
} SolEffectCallKind;

typedef struct {
    SolEffectCallKind kind;
    size_t target;
    SolParameterId first_parameter;
    SolParameterId receiver_parameter;
} SolEffectCall;

static SolEffectCall sol_effect_resolve_call(
    SolEffectChecker *checker,
    const SolExpr *call
) {
    SolEffectCall result = {
        .kind = SOL_EFFECT_CALL_NONE,
        .target = SOL_AST_NONE,
        .first_parameter = SOL_AST_NONE,
        .receiver_parameter = SOL_AST_NONE,
    };
    SolExprId callee_id = call->as.call.callee;
    if (callee_id >= checker->syntax->expression_count) {
        checker->malformed = true;
        return result;
    }
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
    } else if (callee_type.kind == SOL_TYPE_FUNCTION_SIGNATURE
        && callee_type.definition < checker->types->function_type_count) {
        result.kind = SOL_EFFECT_CALL_SIGNATURE;
        result.target = callee_type.definition;
    } else if (callee_type.kind == SOL_TYPE_CAPABILITY_OPERATION
        && callee_type.definition < checker->syntax->capability_member_count) {
        result.kind = SOL_EFFECT_CALL_MEMBER;
        result.target = callee_type.definition;
        result.first_parameter = checker->syntax->capability_members[result.target].first_parameter;
        result.receiver_parameter = checker->types->expression_operation_origins[callee_id];
        if (result.receiver_parameter != SOL_AST_NONE
            && (result.receiver_parameter >= checker->syntax->parameter_count
                || checker->parameter_owners[result.receiver_parameter]
                    != checker->current_function)) {
            checker->malformed = true;
            result.kind = SOL_EFFECT_CALL_NONE;
        }
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
    const SolEffectAtom *required_atoms;
    size_t required_count;
    if (resolved.kind == SOL_EFFECT_CALL_FUNCTION) {
        required_atoms = checker->effects->functions[resolved.target].atoms;
        required_count = checker->effects->functions[resolved.target].count;
    } else if (resolved.kind == SOL_EFFECT_CALL_SIGNATURE) {
        required_atoms = checker->types->function_types[resolved.target].effects.atoms;
        required_count = checker->types->function_types[resolved.target].effects.count;
    } else {
        required_atoms = checker->effects->capability_members[resolved.target].atoms;
        required_count = checker->effects->capability_members[resolved.target].count;
    }
    SolEffectRow *caller = checker->current_member == SOL_AST_NONE
        ? &checker->effects->functions[checker->current_function]
        : &checker->effects->capability_members[checker->current_member];
    for (size_t index = 0; index < required_count; ++index) {
        SolEffectAtom instantiated;
        if (!sol_effect_instantiate_atom(
            checker,
            call,
            &required_atoms[index],
            resolved.first_parameter,
            resolved.receiver_parameter,
            &instantiated
        )) {
            continue;
        }
        if (sol_effect_atom_is_handled(checker, &instantiated)) continue;
        if (checker->mode == SOL_EFFECT_WALK_INFER) {
            sol_effect_row_append(checker, caller, instantiated);
            continue;
        }
        bool found = false;
        for (size_t declared = 0; declared < caller->count; ++declared) {
            const SolEffectAtom *allowed = &caller->atoms[declared];
            bool derived_self = checker->current_member != SOL_AST_NONE
                && allowed->argument_kind == SOL_EFFECT_ATOM_SELF
                && instantiated.argument_kind == SOL_EFFECT_ATOM_PARAMETER
                && instantiated.parameter == checker->syntax->items[
                    checker->syntax->capability_members[
                        checker->current_member
                    ].owner_item
                ].capability_source
                && sol_effect_span_equal(checker->source, allowed->name, instantiated.name);
            if (derived_self
                || sol_effect_atom_equal(checker, allowed, &instantiated)) {
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
        case SOL_EXPR_HANDLE: {
            sol_effect_expression(checker, expression->as.handle.authority);
            sol_effect_expression(checker, expression->as.handle.provider);
            if (checker->mode == SOL_EFFECT_WALK_GRAPH) {
                sol_effect_expression(checker, expression->as.handle.body);
                break;
            }
            if (expression_id >= checker->types->handler_count
                || checker->handled_count >= 256) {
                checker->malformed = true;
                break;
            }
            const SolHandler *handler = &checker->types->handlers[expression_id];
            checker->handled[checker->handled_count++] = (SolHandledEffect){
                .name = expression->as.handle.effect_name,
                .root = handler->root,
            };
            sol_effect_expression(checker, expression->as.handle.body);
            --checker->handled_count;
            break;
        }
        case SOL_EXPR_RESULT:
        case SOL_EXPR_OLD:
            checker->malformed = true;
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
    checker->current_member = SOL_AST_NONE;
    checker->mode = mode;
    checker->depth = 0;
    checker->handled_count = 0;
    sol_effect_expression(checker, item->body);
}

static void sol_effect_walk_member(
    SolEffectChecker *checker,
    SolCapabilityMemberId member_id
) {
    const SolCapabilityMember *member = &checker->syntax->capability_members[member_id];
    if (member->body == SOL_AST_NONE) return;
    memset(
        checker->visited,
        0,
        checker->syntax->expression_count * sizeof(*checker->visited)
    );
    checker->current_function = member->owner_item;
    checker->current_member = member_id;
    checker->mode = SOL_EFFECT_WALK_VALIDATE;
    checker->depth = 0;
    checker->handled_count = 0;
    sol_effect_expression(checker, member->body);
}

static bool sol_effect_validate_expression_arena(SolEffectChecker *checker) {
    const SolSource *source = checker->source;
    const SolSyntaxTree *syntax = checker->syntax;
    const SolHirModule *hir = checker->hir;
    for (size_t index = 0; index < syntax->argument_count; ++index) {
        const SolArgument *argument = &syntax->arguments[index];
        if (argument->value >= syntax->expression_count
            || (argument->is_named && !sol_effect_span_valid(source, argument->name))
            || (argument->next != SOL_AST_NONE
                && argument->next >= syntax->argument_count)) {
            return false;
        }
    }
    for (size_t index = 0; index < syntax->statement_count; ++index) {
        const SolStatement *statement = &syntax->statements[index];
        if ((int)statement->kind < 0 || statement->kind > SOL_STATEMENT_EXPRESSION
            || !sol_effect_span_valid(source, statement->span)
            || (statement->kind == SOL_STATEMENT_LET
                && !sol_effect_span_valid(source, statement->as.let_statement.name))
            || (statement->next != SOL_AST_NONE
                && statement->next >= syntax->statement_count)) {
            return false;
        }
        SolExprId value = statement->kind == SOL_STATEMENT_LET
            ? statement->as.let_statement.value
            : statement->as.expression;
        if (value >= syntax->expression_count) return false;
    }
    for (size_t index = 0; index < syntax->match_arm_count; ++index) {
        const SolMatchArm *arm = &syntax->match_arms[index];
        if (arm->pattern >= syntax->pattern_count
            || arm->value >= syntax->expression_count
            || !sol_effect_span_valid(source, arm->span)
            || (arm->next != SOL_AST_NONE && arm->next >= syntax->match_arm_count)) {
            return false;
        }
    }
    for (size_t index = 0; index < syntax->expression_count; ++index) {
        const SolExpr *expression = &syntax->expressions[index];
        bool valid = (int)expression->kind >= 0
            && expression->kind <= SOL_EXPR_OLD
            && sol_effect_span_valid(source, expression->span);
        switch (expression->kind) {
            case SOL_EXPR_PATH:
                valid = valid && sol_effect_span_valid(source, expression->as.name);
                break;
            case SOL_EXPR_UNARY:
                valid = valid && expression->as.unary.operand < syntax->expression_count;
                break;
            case SOL_EXPR_BINARY:
                valid = valid
                    && expression->as.binary.left < syntax->expression_count
                    && expression->as.binary.right < syntax->expression_count;
                break;
            case SOL_EXPR_CALL:
                valid = valid
                    && expression->as.call.callee < syntax->expression_count
                    && (expression->as.call.first_argument == SOL_AST_NONE
                        || expression->as.call.first_argument < syntax->argument_count);
                break;
            case SOL_EXPR_FIELD:
                valid = valid
                    && expression->as.field.base < syntax->expression_count
                    && sol_effect_span_valid(source, expression->as.field.name);
                break;
            case SOL_EXPR_RECORD:
                valid = valid
                    && expression->as.record.type < syntax->expression_count
                    && (expression->as.record.first_field == SOL_AST_NONE
                        || expression->as.record.first_field < syntax->argument_count);
                break;
            case SOL_EXPR_IF:
                valid = valid
                    && expression->as.if_expr.condition < syntax->expression_count
                    && expression->as.if_expr.then_branch < syntax->expression_count
                    && expression->as.if_expr.else_branch < syntax->expression_count;
                break;
            case SOL_EXPR_MATCH:
                valid = valid
                    && expression->as.match_expr.scrutinee < syntax->expression_count
                    && (expression->as.match_expr.first_arm == SOL_AST_NONE
                        || expression->as.match_expr.first_arm < syntax->match_arm_count);
                break;
            case SOL_EXPR_BLOCK:
                valid = valid
                    && (expression->as.block.first_statement == SOL_AST_NONE
                        || expression->as.block.first_statement < syntax->statement_count);
                break;
            case SOL_EXPR_PROPAGATE:
                valid = valid && expression->as.propagated < syntax->expression_count;
                break;
            case SOL_EXPR_HANDLE:
                valid = valid
                    && sol_effect_span_valid(source, expression->as.handle.effect_name)
                    && expression->as.handle.effect_name.start
                        < expression->as.handle.effect_name.end
                    && expression->as.handle.authority < syntax->expression_count
                    && expression->as.handle.provider < syntax->expression_count
                    && expression->as.handle.body < syntax->expression_count
                    && syntax->expressions[expression->as.handle.body].kind == SOL_EXPR_BLOCK;
                break;
            case SOL_EXPR_OLD:
                valid = valid && expression->as.old_expression < syntax->expression_count;
                break;
            default:
                break;
        }
        SolResolution resolution = hir->resolutions[index];
        valid = valid
            && (int)resolution.kind >= 0
            && resolution.kind <= SOL_RESOLUTION_BUILTIN;
        if (resolution.kind == SOL_RESOLUTION_DEFINITION) {
            valid = valid && resolution.target < hir->definition_count;
        } else if (resolution.kind == SOL_RESOLUTION_LOCAL) {
            valid = valid && resolution.target < hir->local_count;
        } else if (resolution.kind == SOL_RESOLUTION_BUILTIN) {
            valid = valid && resolution.target <= SOL_BUILTIN_NONE;
        }
        if (!valid) return false;
    }
    return true;
}

static bool sol_effect_hir_matches_source(SolEffectChecker *checker) {
    SolHirModule rebuilt;
    SolDiagnostics diagnostics;
    sol_hir_module_init(&rebuilt);
    sol_diagnostics_init(&diagnostics);
    bool completed = sol_hir_lower(
        checker->source,
        checker->syntax,
        &rebuilt,
        &diagnostics
    );
    bool matches = completed
        && !sol_diagnostics_has_errors(&diagnostics)
        && rebuilt.definition_count == checker->hir->definition_count
        && rebuilt.local_count == checker->hir->local_count
        && rebuilt.resolution_count == checker->hir->resolution_count;
    for (size_t index = 0; matches && index < rebuilt.definition_count; ++index) {
        const SolHirDefinition *left = &rebuilt.definitions[index];
        const SolHirDefinition *right = &checker->hir->definitions[index];
        matches = left->kind == right->kind
            && left->name.start == right->name.start
            && left->name.end == right->name.end
            && left->syntax_item == right->syntax_item;
    }
    for (size_t index = 0; matches && index < rebuilt.local_count; ++index) {
        const SolHirLocal *left = &rebuilt.locals[index];
        const SolHirLocal *right = &checker->hir->locals[index];
        matches = left->kind == right->kind
            && left->name.start == right->name.start
            && left->name.end == right->name.end
            && left->owner == right->owner
            && left->syntax_id == right->syntax_id;
    }
    for (size_t index = 0; matches && index < rebuilt.resolution_count; ++index) {
        matches = rebuilt.resolutions[index].kind == checker->hir->resolutions[index].kind
            && rebuilt.resolutions[index].target == checker->hir->resolutions[index].target;
    }
    if (!completed && !sol_diagnostics_has_errors(&diagnostics)) {
        checker->allocation_failed = true;
    }
    sol_hir_module_free(&rebuilt);
    sol_diagnostics_free(&diagnostics);
    return matches;
}

static bool sol_effect_schedule_owned_expression(
    SolEffectChecker *checker,
    SolDefId owner,
    SolExprId expression,
    unsigned char *states,
    SolEffectExpressionEntry *stack,
    size_t *stack_count
) {
    if (expression >= checker->syntax->expression_count) return false;
    if (states[expression] != 0) return false;
    states[expression] = 1;
    checker->expression_owners[expression] = owner;
    stack[(*stack_count)++] = (SolEffectExpressionEntry){
        .expression = expression,
    };
    return true;
}

static bool sol_effect_schedule_argument_expressions(
    SolEffectChecker *checker,
    SolDefId owner,
    SolExprId parent,
    SolArgumentId argument,
    unsigned char *states,
    SolExprId *argument_owners,
    SolEffectExpressionEntry *stack,
    size_t *stack_count
) {
    while (argument != SOL_AST_NONE) {
        if (argument_owners[argument] != SOL_AST_NONE) return false;
        argument_owners[argument] = parent;
        const SolArgument *current = &checker->syntax->arguments[argument];
        if (!sol_effect_schedule_owned_expression(
            checker,
            owner,
            current->value,
            states,
            stack,
            stack_count
        )) {
            return false;
        }
        argument = current->next;
    }
    return true;
}

static bool sol_effect_build_expression_owners(SolEffectChecker *checker) {
    size_t count = checker->syntax->expression_count;
    if (checker->syntax->item_count
            > SIZE_MAX - checker->syntax->capability_member_count
        || count > SIZE_MAX / sizeof(*checker->expression_owners)
        || count > SIZE_MAX / 2
        || count * 2 > SIZE_MAX / sizeof(SolEffectExpressionEntry)
        || checker->syntax->argument_count > SIZE_MAX / sizeof(SolExprId)
        || checker->syntax->statement_count > SIZE_MAX / sizeof(SolExprId)
        || checker->syntax->match_arm_count > SIZE_MAX / sizeof(SolExprId)) {
        return false;
    }
    size_t declared_roots = checker->syntax->item_count
        + checker->syntax->capability_member_count;
    if (declared_roots > SIZE_MAX - checker->syntax->contract_condition_count) {
        return false;
    }
    declared_roots += checker->syntax->contract_condition_count;
    if (declared_roots > SIZE_MAX - count) return false;
    checker->expression_owners = malloc(count * sizeof(*checker->expression_owners));
    unsigned char *states = calloc(count, sizeof(*states));
    SolEffectExpressionEntry *stack = count == 0
        ? NULL
        : malloc(count * 2 * sizeof(*stack));
    SolExprId *argument_owners = malloc(
        checker->syntax->argument_count * sizeof(*argument_owners)
    );
    SolExprId *statement_owners = malloc(
        checker->syntax->statement_count * sizeof(*statement_owners)
    );
    SolExprId *arm_owners = malloc(
        checker->syntax->match_arm_count * sizeof(*arm_owners)
    );
    if ((count != 0
            && (checker->expression_owners == NULL || states == NULL || stack == NULL))
        || (checker->syntax->argument_count != 0 && argument_owners == NULL)
        || (checker->syntax->statement_count != 0 && statement_owners == NULL)
        || (checker->syntax->match_arm_count != 0 && arm_owners == NULL)) {
        free(states);
        free(stack);
        free(argument_owners);
        free(statement_owners);
        free(arm_owners);
        checker->allocation_failed = true;
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        checker->expression_owners[index] = SOL_AST_NONE;
    }
    for (size_t index = 0; index < checker->syntax->argument_count; ++index) {
        argument_owners[index] = SOL_AST_NONE;
    }
    for (size_t index = 0; index < checker->syntax->statement_count; ++index) {
        statement_owners[index] = SOL_AST_NONE;
    }
    for (size_t index = 0; index < checker->syntax->match_arm_count; ++index) {
        arm_owners[index] = SOL_AST_NONE;
    }
    for (size_t root = 0; root < declared_roots + count; ++root) {
        SolDefId owner = SOL_AST_NONE;
        SolExprId root_expression;
        if (root < checker->syntax->item_count) {
            const SolSyntaxItem *item = &checker->syntax->items[root];
            if (item->kind != SOL_ITEM_FUNCTION || item->body == SOL_AST_NONE) continue;
            owner = root;
            root_expression = item->body;
        } else if (root < checker->syntax->item_count
            + checker->syntax->capability_member_count) {
            const SolCapabilityMember *member = &checker->syntax->capability_members[
                root - checker->syntax->item_count
            ];
            if (member->body == SOL_AST_NONE) continue;
            owner = member->owner_item;
            root_expression = member->body;
        } else if (root < declared_roots) {
            const SolContractCondition *condition
                = &checker->syntax->contract_conditions[
                    root - checker->syntax->item_count
                        - checker->syntax->capability_member_count
                ];
            const SolContractClause *clause
                = &checker->syntax->contract_clauses[condition->owner_clause];
            owner = clause->owner_kind == SOL_CONTRACT_OWNER_ITEM
                ? clause->owner
                : checker->syntax->capability_members[clause->owner].owner_item;
            root_expression = condition->expression;
        } else {
            root_expression = root - declared_roots;
            if (states[root_expression] != 0) continue;
        }
        size_t stack_count = 0;
        if (!sol_effect_schedule_owned_expression(
            checker,
            owner,
            root_expression,
            states,
            stack,
            &stack_count
        )) {
            goto invalid;
        }
        while (stack_count != 0) {
            SolEffectExpressionEntry entry = stack[--stack_count];
            SolExprId expression_id = entry.expression;
            if (entry.exiting) {
                states[expression_id] = 2;
                continue;
            }
            stack[stack_count++] = (SolEffectExpressionEntry){
                .expression = expression_id,
                .exiting = true,
            };
            const SolExpr *expression = &checker->syntax->expressions[expression_id];
            bool valid = true;
            switch (expression->kind) {
                case SOL_EXPR_UNARY:
                    valid = sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.unary.operand,
                        states,
                        stack,
                        &stack_count
                    );
                    break;
                case SOL_EXPR_BINARY:
                    valid = sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.binary.left,
                        states,
                        stack,
                        &stack_count
                    ) && sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.binary.right,
                        states,
                        stack,
                        &stack_count
                    );
                    break;
                case SOL_EXPR_CALL:
                    valid = sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.call.callee,
                        states,
                        stack,
                        &stack_count
                    ) && sol_effect_schedule_argument_expressions(
                        checker,
                        owner,
                        expression_id,
                        expression->as.call.first_argument,
                        states,
                        argument_owners,
                        stack,
                        &stack_count
                    );
                    break;
                case SOL_EXPR_FIELD:
                    valid = sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.field.base,
                        states,
                        stack,
                        &stack_count
                    );
                    break;
                case SOL_EXPR_RECORD:
                    valid = sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.record.type,
                        states,
                        stack,
                        &stack_count
                    ) && sol_effect_schedule_argument_expressions(
                        checker,
                        owner,
                        expression_id,
                        expression->as.record.first_field,
                        states,
                        argument_owners,
                        stack,
                        &stack_count
                    );
                    break;
                case SOL_EXPR_IF:
                    valid = sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.if_expr.condition,
                        states,
                        stack,
                        &stack_count
                    ) && sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.if_expr.then_branch,
                        states,
                        stack,
                        &stack_count
                    ) && sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.if_expr.else_branch,
                        states,
                        stack,
                        &stack_count
                    );
                    break;
                case SOL_EXPR_MATCH: {
                    valid = sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.match_expr.scrutinee,
                        states,
                        stack,
                        &stack_count
                    );
                    SolMatchArmId arm = expression->as.match_expr.first_arm;
                    while (valid && arm != SOL_AST_NONE) {
                        if (arm_owners[arm] != SOL_AST_NONE) {
                            valid = false;
                            break;
                        }
                        arm_owners[arm] = expression_id;
                        valid = sol_effect_schedule_owned_expression(
                            checker,
                            owner,
                            checker->syntax->match_arms[arm].value,
                            states,
                            stack,
                            &stack_count
                        );
                        arm = checker->syntax->match_arms[arm].next;
                    }
                    break;
                }
                case SOL_EXPR_BLOCK: {
                    SolStatementId statement = expression->as.block.first_statement;
                    while (valid && statement != SOL_AST_NONE) {
                        if (statement_owners[statement] != SOL_AST_NONE) {
                            valid = false;
                            break;
                        }
                        statement_owners[statement] = expression_id;
                        const SolStatement *current = &checker->syntax->statements[statement];
                        SolExprId value = current->kind == SOL_STATEMENT_LET
                            ? current->as.let_statement.value
                            : current->as.expression;
                        valid = sol_effect_schedule_owned_expression(
                            checker,
                            owner,
                            value,
                            states,
                            stack,
                            &stack_count
                        );
                        statement = current->next;
                    }
                    break;
                }
                case SOL_EXPR_PROPAGATE:
                    valid = sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.propagated,
                        states,
                        stack,
                        &stack_count
                    );
                    break;
                case SOL_EXPR_HANDLE:
                    valid = sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.handle.authority,
                        states,
                        stack,
                        &stack_count
                    ) && sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.handle.provider,
                        states,
                        stack,
                        &stack_count
                    ) && sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.handle.body,
                        states,
                        stack,
                        &stack_count
                    );
                    break;
                case SOL_EXPR_OLD:
                    valid = sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.old_expression,
                        states,
                        stack,
                        &stack_count
                    );
                    break;
                default:
                    break;
            }
            if (!valid) goto invalid;
        }
    }
    free(states);
    free(stack);
    free(argument_owners);
    free(statement_owners);
    free(arm_owners);
    return true;

invalid:
    free(states);
    free(stack);
    free(argument_owners);
    free(statement_owners);
    free(arm_owners);
    return false;
}

static bool sol_effect_type_is_capability(
    const SolEffectChecker *checker,
    SolType type
) {
    return type.kind == SOL_TYPE_NOMINAL
        && type.definition < checker->syntax->item_count
        && checker->syntax->items[type.definition].kind == SOL_ITEM_CAPABILITY;
}

static SolParameterId sol_effect_block_origin(
    const SolEffectChecker *checker,
    const SolExpr *block,
    const SolParameterId *expression_origins
) {
    SolParameterId result = SOL_AST_NONE;
    bool terminated = false;
    SolStatementId statement = block->as.block.first_statement;
    size_t traversed = 0;
    while (statement != SOL_AST_NONE && traversed++ < checker->syntax->statement_count) {
        const SolStatement *current = &checker->syntax->statements[statement];
        SolExprId value_id = current->kind == SOL_STATEMENT_LET
            ? current->as.let_statement.value
            : current->as.expression;
        SolType value = checker->types->expressions[value_id];
        if (!terminated) {
            result = current->kind == SOL_STATEMENT_EXPRESSION
                    && value.kind != SOL_TYPE_NEVER
                    && value.kind != SOL_TYPE_UNKNOWN
                    && value.kind != SOL_TYPE_ERROR
                ? expression_origins[value_id]
                : SOL_AST_NONE;
            terminated = current->kind == SOL_STATEMENT_RETURN
                || value.kind == SOL_TYPE_NEVER;
        }
        statement = current->next;
    }
    return result;
}

static bool sol_effect_join_origin(
    const SolEffectChecker *checker,
    SolExprId value_id,
    const SolParameterId *expression_origins,
    SolParameterId *origin,
    bool *have_value
) {
    SolType value = checker->types->expressions[value_id];
    if (value.kind == SOL_TYPE_NEVER) return true;
    SolParameterId value_origin = expression_origins[value_id];
    if (value.kind == SOL_TYPE_UNKNOWN || value.kind == SOL_TYPE_ERROR
        || value_origin == SOL_AST_NONE) {
        return false;
    }
    if (!*have_value) {
        *origin = value_origin;
        *have_value = true;
        return true;
    }
    return *origin == value_origin;
}

static SolParameterId sol_effect_join_expression_origin(
    const SolEffectChecker *checker,
    const SolExpr *expression,
    const SolParameterId *expression_origins
) {
    SolParameterId origin = SOL_AST_NONE;
    bool have_value = false;
    if (expression->kind == SOL_EXPR_IF) {
        if (!sol_effect_join_origin(
                checker,
                expression->as.if_expr.then_branch,
                expression_origins,
                &origin,
                &have_value
            )
            || !sol_effect_join_origin(
                checker,
                expression->as.if_expr.else_branch,
                expression_origins,
                &origin,
                &have_value
            )) {
            return SOL_AST_NONE;
        }
    } else {
        SolMatchArmId arm = expression->as.match_expr.first_arm;
        size_t traversed = 0;
        while (arm != SOL_AST_NONE && traversed++ < checker->syntax->match_arm_count) {
            if (!sol_effect_join_origin(
                checker,
                checker->syntax->match_arms[arm].value,
                expression_origins,
                &origin,
                &have_value
            )) {
                return SOL_AST_NONE;
            }
            arm = checker->syntax->match_arms[arm].next;
        }
    }
    return have_value ? origin : SOL_AST_NONE;
}

static SolParameterId sol_effect_expected_expression_origin(
    SolEffectChecker *checker,
    SolExprId expression_id,
    bool capability
) {
    SolType type = checker->types->expressions[expression_id];
    if (capability ? !sol_effect_type_is_capability(checker, type)
                   : type.kind != SOL_TYPE_CAPABILITY_OPERATION) {
        return SOL_AST_NONE;
    }
    const SolExpr *expression = &checker->syntax->expressions[expression_id];
    const SolParameterId *expression_origins = capability
        ? checker->types->expression_capability_origins
        : checker->types->expression_operation_origins;
    const SolParameterId *local_origins = capability
        ? checker->types->local_capability_origins
        : checker->types->local_operation_origins;
    if (!capability && expression->kind == SOL_EXPR_FIELD) {
        return checker->types->expression_capability_origins[expression->as.field.base];
    }
    if (capability && expression->kind == SOL_EXPR_CALL) {
        SolExprId callee = expression->as.call.callee;
        if (callee < checker->types->expression_count) {
            SolType callee_type = checker->types->expressions[callee];
            if (callee_type.kind == SOL_TYPE_FUNCTION
                && callee_type.definition < checker->syntax->item_count) {
                const SolSyntaxItem *function
                    = &checker->syntax->items[callee_type.definition];
                if (function->result_authority_parameter != SOL_AST_NONE) {
                    SolExprId actual = sol_effect_find_actual_argument(
                        checker,
                        function->first_parameter,
                        function->result_authority_parameter,
                        expression->as.call.first_argument
                    );
                    if (actual < checker->types->expression_count) {
                        return sol_effect_expected_expression_origin(
                            checker,
                            actual,
                            true
                        );
                    }
                }
            }
            if (callee_type.kind == SOL_TYPE_CAPABILITY_OPERATION
                && callee_type.definition < checker->syntax->capability_member_count
                && checker->syntax->capability_members[
                    callee_type.definition
                ].result_authority_from_self) {
                return sol_effect_expected_expression_origin(checker, callee, false);
            }
        }
    }
    if (capability && expression->kind == SOL_EXPR_RECORD) {
        SolExprId type_expression = expression->as.record.type;
        SolResolution resolution = checker->hir->resolutions[type_expression];
        if (resolution.kind == SOL_RESOLUTION_DEFINITION
            && resolution.target < checker->syntax->item_count) {
            const SolSyntaxItem *item = &checker->syntax->items[resolution.target];
            if (item->kind == SOL_ITEM_CAPABILITY
                && item->capability_source != SOL_AST_NONE) {
                SolArgumentId source = expression->as.record.first_field;
                if (source < checker->syntax->argument_count) {
                    return sol_effect_expected_expression_origin(
                        checker,
                        checker->syntax->arguments[source].value,
                        true
                    );
                }
            }
        }
    }
    if (expression->kind == SOL_EXPR_PATH) {
        SolResolution resolution = checker->hir->resolutions[expression_id];
        if (resolution.kind == SOL_RESOLUTION_LOCAL
            && resolution.target < checker->hir->local_count
            && checker->hir->locals[resolution.target].owner
                == checker->expression_owners[expression_id]) {
            return local_origins[resolution.target];
        }
        return SOL_AST_NONE;
    }
    if (expression->kind == SOL_EXPR_HANDLE) {
        SolExprId body = expression->as.handle.body;
        return body < checker->types->expression_count
            ? expression_origins[body]
            : SOL_AST_NONE;
    }
    if (expression->kind == SOL_EXPR_BLOCK) {
        return sol_effect_block_origin(checker, expression, expression_origins);
    }
    if (expression->kind == SOL_EXPR_IF || expression->kind == SOL_EXPR_MATCH) {
        return sol_effect_join_expression_origin(checker, expression, expression_origins);
    }
    return SOL_AST_NONE;
}

static bool sol_effect_origin_matches_type(
    const SolEffectChecker *checker,
    SolParameterId origin,
    SolType value_type,
    SolDefId owner,
    bool operation
) {
    if (origin == SOL_AST_NONE) return true;
    if (origin >= checker->syntax->parameter_count
        || owner == SOL_AST_NONE
        || checker->parameter_owners[origin] != owner) {
        return false;
    }
    const SolParameter *parameter = &checker->syntax->parameters[origin];
    if (parameter->type_id >= checker->types->declared_type_count) return false;
    SolType parameter_type = checker->types->declared_types[parameter->type_id];
    if (!sol_effect_type_is_capability(checker, parameter_type)) return false;
    if (!operation) {
        return sol_effect_type_is_capability(checker, value_type);
    }
    return value_type.kind == SOL_TYPE_CAPABILITY_OPERATION
        && value_type.definition < checker->syntax->capability_member_count;
}

static bool sol_effect_validate_local_resolutions(const SolEffectChecker *checker) {
    for (size_t index = 0; index < checker->syntax->expression_count; ++index) {
        SolResolution resolution = checker->hir->resolutions[index];
        if (resolution.kind != SOL_RESOLUTION_LOCAL) continue;
        const SolExpr *expression = &checker->syntax->expressions[index];
        const SolHirLocal *local = &checker->hir->locals[resolution.target];
        SolDefId owner = checker->expression_owners[index];
        if (expression->kind != SOL_EXPR_PATH
            || owner == SOL_AST_NONE
            || local->owner != owner
            || !sol_effect_span_equal(checker->source, expression->as.name, local->name)) {
            return false;
        }
        if (local->kind == SOL_LOCAL_PARAMETER) {
            if (local->syntax_id >= checker->syntax->parameter_count
                || checker->parameter_owners[local->syntax_id] != owner
                || !sol_effect_span_equal(
                    checker->source,
                    local->name,
                    checker->syntax->parameters[local->syntax_id].name
                )) {
                return false;
            }
        } else if (local->kind == SOL_LOCAL_BINDING) {
            if (local->syntax_id >= checker->syntax->statement_count) return false;
            const SolStatement *statement = &checker->syntax->statements[local->syntax_id];
            if (statement->kind != SOL_STATEMENT_LET
                || statement->span.end > expression->span.start
                || !sol_effect_span_equal(
                    checker->source,
                    local->name,
                    statement->as.let_statement.name
                )) {
                return false;
            }
        }
    }
    return true;
}

static bool sol_effect_provenance_node_relevant(
    const SolEffectChecker *checker,
    size_t node,
    bool capability
) {
    size_t expression_count = checker->syntax->expression_count;
    SolType type = node < expression_count
        ? checker->types->expressions[node]
        : checker->types->locals[node - expression_count];
    return capability
        ? sol_effect_type_is_capability(checker, type)
        : type.kind == SOL_TYPE_CAPABILITY_OPERATION;
}

static void sol_effect_push_provenance_dependency(
    SolEffectProvenanceEntry *stack,
    size_t *stack_count,
    size_t node
) {
    stack[(*stack_count)++] = (SolEffectProvenanceEntry){.node = node};
}

static void sol_effect_push_expression_provenance_dependencies(
    const SolEffectChecker *checker,
    SolExprId expression_id,
    bool capability,
    SolEffectProvenanceEntry *stack,
    size_t *stack_count
) {
    const SolExpr *expression = &checker->syntax->expressions[expression_id];
    size_t expression_count = checker->syntax->expression_count;
    if (expression->kind == SOL_EXPR_PATH) {
        SolResolution resolution = checker->hir->resolutions[expression_id];
        if (resolution.kind == SOL_RESOLUTION_LOCAL) {
            sol_effect_push_provenance_dependency(
                stack,
                stack_count,
                expression_count + resolution.target
            );
        }
    } else if (expression->kind == SOL_EXPR_BLOCK) {
        SolStatementId statement = expression->as.block.first_statement;
        while (statement != SOL_AST_NONE) {
            const SolStatement *current = &checker->syntax->statements[statement];
            SolExprId value = current->kind == SOL_STATEMENT_LET
                ? current->as.let_statement.value
                : current->as.expression;
            sol_effect_push_provenance_dependency(stack, stack_count, value);
            statement = current->next;
        }
    } else if (expression->kind == SOL_EXPR_IF) {
        sol_effect_push_provenance_dependency(
            stack,
            stack_count,
            expression->as.if_expr.then_branch
        );
        sol_effect_push_provenance_dependency(
            stack,
            stack_count,
            expression->as.if_expr.else_branch
        );
    } else if (expression->kind == SOL_EXPR_MATCH) {
        SolMatchArmId arm = expression->as.match_expr.first_arm;
        while (arm != SOL_AST_NONE) {
            sol_effect_push_provenance_dependency(
                stack,
                stack_count,
                checker->syntax->match_arms[arm].value
            );
            arm = checker->syntax->match_arms[arm].next;
        }
    } else if (capability && expression->kind == SOL_EXPR_RECORD) {
        SolArgumentId source = expression->as.record.first_field;
        if (source < checker->syntax->argument_count) {
            sol_effect_push_provenance_dependency(
                stack,
                stack_count,
                checker->syntax->arguments[source].value
            );
        }
    } else if (!capability && expression->kind == SOL_EXPR_FIELD) {
        /* Capability expression provenance is validated before operation provenance. */
    } else if (expression->kind == SOL_EXPR_HANDLE) {
        sol_effect_push_provenance_dependency(
            stack,
            stack_count,
            expression->as.handle.body
        );
    }
}

static bool sol_effect_validate_provenance_node(
    SolEffectChecker *checker,
    size_t node,
    bool capability
) {
    size_t expression_count = checker->syntax->expression_count;
    if (node < expression_count) {
        SolParameterId actual = capability
            ? checker->types->expression_capability_origins[node]
            : checker->types->expression_operation_origins[node];
        SolParameterId expected = sol_effect_expected_expression_origin(
            checker,
            node,
            capability
        );
        return actual == expected
            && sol_effect_origin_matches_type(
                checker,
                actual,
                checker->types->expressions[node],
                checker->expression_owners[node],
                !capability
            );
    }
    SolLocalId local_id = node - expression_count;
    const SolHirLocal *local = &checker->hir->locals[local_id];
    SolType type = checker->types->locals[local_id];
    SolParameterId actual = capability
        ? checker->types->local_capability_origins[local_id]
        : checker->types->local_operation_origins[local_id];
    SolParameterId expected = SOL_AST_NONE;
    if (sol_effect_provenance_node_relevant(checker, node, capability)) {
        if (capability && local->kind == SOL_LOCAL_PARAMETER) {
            expected = local->syntax_id;
        } else if (local->kind == SOL_LOCAL_BINDING) {
            const SolStatement *statement = &checker->syntax->statements[local->syntax_id];
            expected = capability
                ? checker->types->expression_capability_origins[
                    statement->as.let_statement.value
                ]
                : checker->types->expression_operation_origins[
                    statement->as.let_statement.value
                ];
        }
    }
    return actual == expected
        && sol_effect_origin_matches_type(
            checker,
            actual,
            type,
            local->owner,
            !capability
        );
}

static bool sol_effect_validate_provenance(
    SolEffectChecker *checker,
    bool capability
) {
    size_t expression_count = checker->syntax->expression_count;
    if (expression_count > SIZE_MAX - checker->hir->local_count) return false;
    size_t count = expression_count + checker->hir->local_count;
    if (count > SIZE_MAX / 2
        || count * 2 > SIZE_MAX / sizeof(SolEffectProvenanceEntry)) {
        return false;
    }
    unsigned char *states = calloc(count, sizeof(*states));
    SolEffectProvenanceEntry *stack = count == 0
        ? NULL
        : malloc(count * 2 * sizeof(*stack));
    if (count != 0 && (states == NULL || stack == NULL)) {
        free(states);
        free(stack);
        checker->allocation_failed = true;
        return false;
    }
    for (size_t root = 0; root < count; ++root) {
        if (states[root] == 2) continue;
        size_t stack_count = 0;
        sol_effect_push_provenance_dependency(stack, &stack_count, root);
        while (stack_count != 0) {
            SolEffectProvenanceEntry entry = stack[--stack_count];
            if (entry.exiting) {
                if (!sol_effect_validate_provenance_node(checker, entry.node, capability)) {
                    free(states);
                    free(stack);
                    return false;
                }
                states[entry.node] = 2;
                continue;
            }
            if (states[entry.node] == 2) continue;
            if (states[entry.node] == 1) {
                free(states);
                free(stack);
                return false;
            }
            states[entry.node] = 1;
            stack[stack_count++] = (SolEffectProvenanceEntry){
                .node = entry.node,
                .exiting = true,
            };
            if (!sol_effect_provenance_node_relevant(checker, entry.node, capability)) {
                continue;
            }
            if (entry.node < expression_count) {
                sol_effect_push_expression_provenance_dependencies(
                    checker,
                    entry.node,
                    capability,
                    stack,
                    &stack_count
                );
            } else {
                SolLocalId local_id = entry.node - expression_count;
                const SolHirLocal *local = &checker->hir->locals[local_id];
                if (local->kind == SOL_LOCAL_BINDING) {
                    SolExprId initializer = checker->syntax->statements[
                        local->syntax_id
                    ].as.let_statement.value;
                    sol_effect_push_provenance_dependency(
                        stack,
                        &stack_count,
                        initializer
                    );
                }
            }
        }
    }
    free(states);
    free(stack);
    return true;
}

static bool sol_effect_type_has_identity(SolTypeKind kind) {
    return kind == SOL_TYPE_NOMINAL || kind == SOL_TYPE_OPAQUE
        || kind == SOL_TYPE_FUNCTION || kind == SOL_TYPE_FUNCTION_SIGNATURE
        || kind == SOL_TYPE_CAPABILITY_OPERATION || kind == SOL_TYPE_VARIANT;
}

static bool sol_effect_semantic_type_equal(SolType left, SolType right) {
    return left.kind == right.kind
        && (!sol_effect_type_has_identity(left.kind) || left.definition == right.definition);
}

static bool sol_effect_member_is_family(
    const SolEffectChecker *checker,
    SolCapabilityMemberId member_id,
    SolSpan effect_name
) {
    if (member_id >= checker->syntax->capability_member_count) return false;
    const SolCapabilityMember *member = &checker->syntax->capability_members[member_id];
    SolEffectId effect_id = member->first_effect;
    if (effect_id == SOL_AST_NONE || effect_id >= checker->syntax->effect_count) return false;
    const SolEffect *effect = &checker->syntax->effects[effect_id];
    return effect->owner_kind == SOL_EFFECT_OWNER_CAPABILITY_MEMBER
        && effect->owner == member_id
        && effect->next == SOL_AST_NONE
        && !effect->is_pure
        && effect->has_argument
        && sol_effect_span_equal(checker->source, effect->name, effect_name)
        && sol_effect_span_text_equal(checker->source, effect->argument, "Self");
}

static bool sol_effect_member_is_pure(
    const SolEffectChecker *checker,
    SolCapabilityMemberId member_id
) {
    const SolCapabilityMember *member = &checker->syntax->capability_members[member_id];
    if (!member->has_effect_clause) return false;
    SolEffectId effect = member->first_effect;
    size_t traversed = 0;
    while (effect != SOL_AST_NONE) {
        if (effect >= checker->syntax->effect_count
            || traversed++ >= checker->syntax->effect_count) return false;
        const SolEffect *entry = &checker->syntax->effects[effect];
        if (entry->owner_kind != SOL_EFFECT_OWNER_CAPABILITY_MEMBER
            || entry->owner != member_id || !entry->is_pure || entry->has_argument) {
            return false;
        }
        effect = entry->next;
    }
    return true;
}

static bool sol_effect_member_signatures_equal(
    const SolEffectChecker *checker,
    SolCapabilityMemberId left_id,
    SolCapabilityMemberId right_id
) {
    const SolCapabilityMember *left = &checker->syntax->capability_members[left_id];
    const SolCapabilityMember *right = &checker->syntax->capability_members[right_id];
    SolParameterId left_parameter = left->first_parameter;
    SolParameterId right_parameter = right->first_parameter;
    size_t traversed = 0;
    while (left_parameter != SOL_AST_NONE && right_parameter != SOL_AST_NONE) {
        if (left_parameter >= checker->syntax->parameter_count
            || right_parameter >= checker->syntax->parameter_count
            || traversed++ >= checker->syntax->parameter_count) return false;
        const SolParameter *left_entry = &checker->syntax->parameters[left_parameter];
        const SolParameter *right_entry = &checker->syntax->parameters[right_parameter];
        if (left_entry->type_id >= checker->types->declared_type_count
            || right_entry->type_id >= checker->types->declared_type_count
            || !sol_effect_span_equal(checker->source, left_entry->name, right_entry->name)
            || !sol_effect_semantic_type_equal(
                checker->types->declared_types[left_entry->type_id],
                checker->types->declared_types[right_entry->type_id]
            )) return false;
        left_parameter = left_entry->next;
        right_parameter = right_entry->next;
    }
    return left_parameter == SOL_AST_NONE && right_parameter == SOL_AST_NONE
        && left->return_type_id < checker->types->declared_type_count
        && right->return_type_id < checker->types->declared_type_count
        && sol_effect_semantic_type_equal(
            checker->types->declared_types[left->return_type_id],
            checker->types->declared_types[right->return_type_id]
        );
}

static bool sol_effect_validate_handlers(const SolEffectChecker *checker) {
    for (size_t index = 0; index < checker->syntax->expression_count; ++index) {
        const SolExpr *expression = &checker->syntax->expressions[index];
        const SolHandler *handler = &checker->types->handlers[index];
        if (expression->kind != SOL_EXPR_HANDLE) {
            if (handler->source_member != SOL_AST_NONE
                || handler->provider_member != SOL_AST_NONE
                || handler->root != SOL_AST_NONE) return false;
            continue;
        }
        if (handler->source_member >= checker->syntax->capability_member_count
            || handler->provider_member >= checker->syntax->capability_member_count
            || handler->root >= checker->syntax->parameter_count) return false;
        const SolCapabilityMember *source
            = &checker->syntax->capability_members[handler->source_member];
        const SolCapabilityMember *provider
            = &checker->syntax->capability_members[handler->provider_member];
        if (source->owner_item >= checker->syntax->item_count
            || provider->owner_item >= checker->syntax->item_count
            || !sol_effect_member_is_family(
                checker,
                handler->source_member,
                expression->as.handle.effect_name
            )
            || !sol_effect_span_equal(checker->source, source->name, provider->name)
            || !sol_effect_member_signatures_equal(
                checker,
                handler->source_member,
                handler->provider_member
            )
            || !sol_effect_member_is_pure(checker, handler->provider_member)) return false;
        size_t matches = 0;
        for (size_t candidate = 0;
            candidate < checker->syntax->capability_member_count;
            ++candidate) {
            const SolCapabilityMember *member
                = &checker->syntax->capability_members[candidate];
            if (member->owner_item < checker->syntax->item_count
                && sol_effect_member_is_family(
                    checker,
                    candidate,
                    expression->as.handle.effect_name
                )) ++matches;
        }
        SolType authority = checker->types->expressions[expression->as.handle.authority];
        SolType provider_type = checker->types->expressions[expression->as.handle.provider];
        if (matches != 1
            || authority.kind != SOL_TYPE_NOMINAL
            || authority.definition != source->owner_item
            || provider_type.kind != SOL_TYPE_NOMINAL
            || provider_type.definition != provider->owner_item
            || checker->types->expression_capability_origins[
                expression->as.handle.authority
            ] != handler->root
            || checker->types->expression_capability_origins[
                expression->as.handle.provider
            ] == SOL_AST_NONE
            || checker->parameter_owners[handler->root]
                != checker->expression_owners[index]) return false;
    }
    return true;
}

static bool sol_effect_coercion_shape_matches(
    const SolEffectChecker *checker,
    const SolFunctionCoercion *coercion,
    SolType actual
) {
    const SolFunctionType *expected
        = &checker->types->function_types[coercion->expected.definition];
    SolParameterId parameter;
    SolType result;
    if (actual.kind == SOL_TYPE_FUNCTION) {
        parameter = checker->syntax->items[actual.definition].first_parameter;
        result = checker->types->definitions[actual.definition];
    } else {
        const SolCapabilityMember *member
            = &checker->syntax->capability_members[actual.definition];
        if (member->return_type_id >= checker->types->declared_type_count) return false;
        parameter = member->first_parameter;
        result = checker->types->declared_types[member->return_type_id];
    }
    size_t index = 0;
    while (parameter != SOL_AST_NONE && index < expected->parameter_count) {
        if (parameter >= checker->syntax->parameter_count
            || checker->syntax->parameters[parameter].type_id
                >= checker->types->declared_type_count) {
            return false;
        }
        SolType actual_parameter = checker->types->declared_types[
            checker->syntax->parameters[parameter].type_id
        ];
        if (!sol_effect_semantic_type_equal(
            actual_parameter,
            expected->parameters[index]
        )) return false;
        parameter = checker->syntax->parameters[parameter].next;
        ++index;
    }
    return parameter == SOL_AST_NONE && index == expected->parameter_count
        && sol_effect_semantic_type_equal(result, expected->result);
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
        || syntax->contract_clause_count > syntax->contract_clause_capacity
        || syntax->contract_condition_count > syntax->contract_condition_capacity
        || (syntax->item_count != 0 && syntax->items == NULL)
        || (syntax->expression_count != 0 && syntax->expressions == NULL)
        || (syntax->parameter_count != 0 && syntax->parameters == NULL)
        || (syntax->argument_count != 0 && syntax->arguments == NULL)
        || (syntax->statement_count != 0 && syntax->statements == NULL)
        || (syntax->match_arm_count != 0 && syntax->match_arms == NULL)
        || (syntax->effect_count != 0 && syntax->effects == NULL)
        || (syntax->capability_member_count != 0 && syntax->capability_members == NULL)
        || (syntax->contract_clause_count != 0 && syntax->contract_clauses == NULL)
        || (syntax->contract_condition_count != 0
            && syntax->contract_conditions == NULL)
        || hir->definition_count != syntax->item_count
        || hir->resolution_count != syntax->expression_count
        || hir->local_count > hir->local_capacity
        || (hir->definition_count != 0 && hir->definitions == NULL)
        || (hir->resolution_count != 0 && hir->resolutions == NULL)
        || (hir->local_count != 0 && hir->locals == NULL)
        || types->expression_count != syntax->expression_count
        || types->local_count != hir->local_count
        || types->definition_count != hir->definition_count
        || types->declared_type_count != syntax->type_count
        || types->function_type_count > types->function_type_capacity
        || types->function_coercion_count > types->function_coercion_capacity
        || types->handler_count != syntax->expression_count
        || ((types->function_coercion_count == 0)
            != (types->function_coercion_capacity == 0))
        || (types->expression_count != 0 && types->expressions == NULL)
        || (types->expression_count != 0 && types->expression_capability_origins == NULL)
        || (types->expression_count != 0 && types->expression_operation_origins == NULL)
        || (types->local_count != 0 && types->locals == NULL)
        || (types->local_count != 0 && types->local_capability_origins == NULL)
        || (types->local_count != 0 && types->local_operation_origins == NULL)
        || (types->definition_count != 0 && types->definitions == NULL)
        || (types->declared_type_count != 0 && types->declared_types == NULL)
        || (types->function_type_capacity != 0 && types->function_types == NULL)
        || (types->function_coercion_capacity != 0
            && types->function_coercions == NULL)
        || (types->handler_count != 0 && types->handlers == NULL)) {
        return false;
    }
    if (!sol_syntax_contracts_validate(source, syntax)) return false;
    for (size_t index = 0; index < types->function_type_count; ++index) {
        const SolFunctionType *function = &types->function_types[index];
        if ((function->parameter_count != 0 && function->parameters == NULL)
            || (function->effects.count != 0 && function->effects.atoms == NULL)
            || (int)function->result.kind < 0 || function->result.kind > SOL_TYPE_NEVER
            || (function->result.kind == SOL_TYPE_FUNCTION_SIGNATURE
                && function->result.definition >= index)
            || (!sol_effect_type_has_identity(function->result.kind)
                && function->result.definition != 0)) {
            return false;
        }
        for (size_t parameter = 0; parameter < function->parameter_count; ++parameter) {
            SolType type = function->parameters[parameter];
            if ((int)type.kind < 0 || type.kind > SOL_TYPE_NEVER
                || (type.kind == SOL_TYPE_FUNCTION_SIGNATURE && type.definition >= index)
                || (!sol_effect_type_has_identity(type.kind) && type.definition != 0)) {
                return false;
            }
        }
        for (size_t atom = 0; atom < function->effects.count; ++atom) {
            const SolEffectAtom *effect = &function->effects.atoms[atom];
            if ((int)effect->argument_kind < 0
                || effect->argument_kind > SOL_EFFECT_ATOM_STATIC_PATH
                || !sol_effect_span_valid(source, effect->name)
                || !sol_effect_span_valid(source, effect->argument)
                || !sol_effect_span_valid(source, effect->span)
                || effect->parameter != SOL_AST_NONE
                || (effect->argument_kind == SOL_EFFECT_ATOM_NO_ARGUMENT
                    && (effect->argument.start != 0 || effect->argument.end != 0))
                || (effect->argument_kind == SOL_EFFECT_ATOM_STATIC_PATH
                    && effect->argument.start == effect->argument.end)) {
                return false;
            }
            for (size_t previous = 0; previous < atom; ++previous) {
                if (sol_effect_atom_equal(
                    checker,
                    &function->effects.atoms[previous],
                    effect
                )) {
                    return false;
                }
            }
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const SolFunctionType *other = &types->function_types[previous];
            bool equal = function->parameter_count == other->parameter_count
                && sol_effect_semantic_type_equal(function->result, other->result)
                && function->effects.count == other->effects.count;
            for (size_t parameter = 0; equal && parameter < function->parameter_count;
                ++parameter) {
                equal = sol_effect_semantic_type_equal(
                    function->parameters[parameter],
                    other->parameters[parameter]
                );
            }
            for (size_t atom = 0; equal && atom < function->effects.count; ++atom) {
                bool found = false;
                for (size_t candidate = 0; candidate < other->effects.count; ++candidate) {
                    if (sol_effect_atom_equal(
                        checker,
                        &function->effects.atoms[atom],
                        &other->effects.atoms[candidate]
                    )) {
                        found = true;
                        break;
                    }
                }
                equal = found;
            }
            if (equal) return false;
        }
    }
    const SolType *type_arenas[] = {
        types->expressions,
        types->locals,
        types->definitions,
        types->declared_types,
    };
    const size_t type_counts[] = {
        types->expression_count,
        types->local_count,
        types->definition_count,
        types->declared_type_count,
    };
    for (size_t arena = 0; arena < sizeof(type_arenas) / sizeof(type_arenas[0]); ++arena) {
        for (size_t index = 0; index < type_counts[arena]; ++index) {
            SolType type = type_arenas[arena][index];
            if ((int)type.kind < 0 || type.kind > SOL_TYPE_NEVER
                || (type.kind == SOL_TYPE_FUNCTION_SIGNATURE
                    && type.definition >= types->function_type_count)
                || (!sol_effect_type_has_identity(type.kind) && type.definition != 0)) {
                return false;
            }
        }
    }
    for (size_t index = 0; index < types->function_coercion_count; ++index) {
        const SolFunctionCoercion *coercion = &types->function_coercions[index];
        if (coercion->expression >= syntax->expression_count
            || coercion->expected.kind != SOL_TYPE_FUNCTION_SIGNATURE
            || coercion->expected.definition >= types->function_type_count) {
            return false;
        }
        SolType actual = types->expressions[coercion->expression];
        if ((actual.kind == SOL_TYPE_FUNCTION
                && (actual.definition >= syntax->item_count
                    || syntax->items[actual.definition].kind != SOL_ITEM_FUNCTION))
            || (actual.kind == SOL_TYPE_CAPABILITY_OPERATION
                && actual.definition >= syntax->capability_member_count)
            || (actual.kind != SOL_TYPE_FUNCTION
                && actual.kind != SOL_TYPE_CAPABILITY_OPERATION)) {
            return false;
        }
        if (!sol_effect_coercion_shape_matches(checker, coercion, actual)) return false;
    }
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if ((int)item->kind < 0 || item->kind > SOL_ITEM_FUNCTION
            || (item->body != SOL_AST_NONE && item->body >= syntax->expression_count)
            || (item->first_effect != SOL_AST_NONE
                && item->first_effect >= syntax->effect_count)
            || (item->first_parameter != SOL_AST_NONE
                && item->first_parameter >= syntax->parameter_count)
            || (item->first_member != SOL_AST_NONE
                && item->first_member >= syntax->capability_member_count)
            || (item->first_contract != SOL_AST_NONE
                && item->first_contract >= syntax->contract_clause_count)
            || (item->capability_source != SOL_AST_NONE
                && item->capability_source >= syntax->parameter_count)
            || (item->result_authority_parameter != SOL_AST_NONE
                && item->result_authority_parameter >= syntax->parameter_count)) {
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
    if (syntax->parameter_count > SIZE_MAX / sizeof(*checker->parameter_owners)) return false;
    checker->parameter_owners = malloc(
        syntax->parameter_count * sizeof(*checker->parameter_owners)
    );
    if (syntax->parameter_count != 0 && checker->parameter_owners == NULL) {
        checker->allocation_failed = true;
        return false;
    }
    for (size_t index = 0; index < syntax->parameter_count; ++index) {
        checker->parameter_owners[index] = SOL_AST_NONE;
    }
    for (size_t owner = 0; owner < syntax->item_count; ++owner) {
        const SolSyntaxItem *item = &syntax->items[owner];
        if (item->kind == SOL_ITEM_FUNCTION) {
            SolParameterId parameter = item->first_parameter;
            size_t traversed = 0;
            while (parameter != SOL_AST_NONE) {
                if (parameter >= syntax->parameter_count
                    || traversed++ >= syntax->parameter_count
                    || checker->parameter_owners[parameter] != SOL_AST_NONE) {
                    return false;
                }
                checker->parameter_owners[parameter] = owner;
                parameter = syntax->parameters[parameter].next;
            }
        } else if (item->kind == SOL_ITEM_CAPABILITY
            && item->capability_source != SOL_AST_NONE) {
            SolTypeId source_type = syntax->parameters[
                item->capability_source
            ].type_id;
            if (source_type >= syntax->type_count
                || syntax->types[source_type].kind != SOL_SYNTAX_TYPE_PATH
                || !syntax->types[source_type].is_capability
                || syntax->types[source_type].first_argument != SOL_AST_NONE) {
                return false;
            }
            if (checker->parameter_owners[item->capability_source] != SOL_AST_NONE) {
                return false;
            }
            checker->parameter_owners[item->capability_source] = owner;
            SolCapabilityMemberId member = item->first_member;
            size_t member_count = 0;
            while (member != SOL_AST_NONE) {
                if (member >= syntax->capability_member_count
                    || member_count++ >= syntax->capability_member_count) {
                    return false;
                }
                SolParameterId parameter
                    = syntax->capability_members[member].first_parameter;
                size_t parameter_count = 0;
                while (parameter != SOL_AST_NONE) {
                    if (parameter >= syntax->parameter_count
                        || parameter_count++ >= syntax->parameter_count
                        || checker->parameter_owners[parameter] != SOL_AST_NONE) {
                        return false;
                    }
                    checker->parameter_owners[parameter] = owner;
                    parameter = syntax->parameters[parameter].next;
                }
                member = syntax->capability_members[member].next;
            }
        }
    }
    if (!sol_effect_validate_expression_arena(checker)) return false;
    for (size_t index = 0; index < hir->local_count; ++index) {
        const SolHirLocal *local = &hir->locals[index];
        if ((int)local->kind < 0 || local->kind > SOL_LOCAL_PATTERN
            || !sol_effect_span_valid(source, local->name)
            || local->owner >= syntax->item_count
            || (syntax->items[local->owner].kind != SOL_ITEM_FUNCTION
                && !(syntax->items[local->owner].kind == SOL_ITEM_CAPABILITY
                    && syntax->items[local->owner].capability_source != SOL_AST_NONE))
            || (local->kind == SOL_LOCAL_PARAMETER
                && (local->syntax_id >= syntax->parameter_count
                    || local->name.start != syntax->parameters[local->syntax_id].name.start
                    || local->name.end != syntax->parameters[local->syntax_id].name.end))
            || (local->kind == SOL_LOCAL_BINDING
                && (local->syntax_id >= syntax->statement_count
                    || syntax->statements[local->syntax_id].kind != SOL_STATEMENT_LET
                    || local->name.start
                        != syntax->statements[
                            local->syntax_id
                        ].as.let_statement.name.start
                    || local->name.end
                        != syntax->statements[
                            local->syntax_id
                        ].as.let_statement.name.end))
            || (local->kind == SOL_LOCAL_PATTERN
                && local->syntax_id >= syntax->pattern_binding_count)) {
            return false;
        }
    }
    if (!sol_effect_build_expression_owners(checker)
        || !sol_effect_validate_local_resolutions(checker)
        || !sol_effect_validate_handlers(checker)) return false;
    if (!sol_effect_hir_matches_source(checker)) return false;
    for (size_t index = 0; index < hir->local_count; ++index) {
        const SolHirLocal *local = &hir->locals[index];
        if (local->kind == SOL_LOCAL_BINDING) {
            SolExprId initializer = syntax->statements[
                local->syntax_id
            ].as.let_statement.value;
            if (checker->expression_owners[initializer] != local->owner) return false;
        }
    }
    for (size_t index = 0; index < syntax->effect_count; ++index) {
        const SolEffect *effect = &syntax->effects[index];
        if (!sol_effect_span_valid(source, effect->name)
            || !sol_effect_span_valid(source, effect->argument)
            || !sol_effect_span_valid(source, effect->span)
            || (effect->next != SOL_AST_NONE && effect->next >= syntax->effect_count)
            || (int)effect->owner_kind < 0
            || effect->owner_kind > SOL_EFFECT_OWNER_TYPE
            || (effect->owner_kind == SOL_EFFECT_OWNER_ITEM
                && effect->owner >= syntax->item_count)
            || (effect->owner_kind == SOL_EFFECT_OWNER_CAPABILITY_MEMBER
                && effect->owner >= syntax->capability_member_count)
            || (effect->owner_kind == SOL_EFFECT_OWNER_TYPE
                && effect->owner >= syntax->type_count)) {
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
            || (member->body != SOL_AST_NONE
                && member->body >= syntax->expression_count)
            || (member->next != SOL_AST_NONE
                && member->next >= syntax->capability_member_count)
            || (member->first_contract != SOL_AST_NONE
                && member->first_contract >= syntax->contract_clause_count)
            || member->owner_item >= syntax->item_count) {
            return false;
        }
        if ((syntax->items[member->owner_item].capability_source != SOL_AST_NONE)
            != (member->body != SOL_AST_NONE)) return false;
    }
    return sol_effect_validate_provenance(checker, true)
        && sol_effect_validate_provenance(checker, false);
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

static size_t *sol_effect_find_components(
    SolEffectChecker *checker,
    size_t *component_count_out
) {
    size_t count = checker->syntax->item_count;
    if (count > SIZE_MAX / sizeof(SolDefId)
        || count > SIZE_MAX / sizeof(size_t)) {
        checker->allocation_failed = true;
        return NULL;
    }
    unsigned char *state = calloc(count, sizeof(*state));
    size_t *component = malloc(count * sizeof(*component));
    SolDefId *order = count == 0 ? NULL : malloc(count * sizeof(*order));
    SolDefId *stack = count == 0 ? NULL : malloc(count * sizeof(*stack));
    size_t *edge_stack = count == 0 ? NULL : malloc(count * sizeof(*edge_stack));
    if (count != 0 && (state == NULL || component == NULL || order == NULL
            || stack == NULL || edge_stack == NULL)) {
        free(state);
        free(component);
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

    free(state);
    free(order);
    free(stack);
    free(edge_stack);
    *component_count_out = component_count;
    return component;
}

static void sol_effect_infer_functions(
    SolEffectChecker *checker,
    const size_t *components,
    size_t component_count
) {
    size_t count = checker->syntax->item_count;
    for (size_t index = 0; index < count; ++index) {
        const SolSyntaxItem *item = &checker->syntax->items[index];
        if (item->kind == SOL_ITEM_FUNCTION && !item->has_effect_clause
            && !item->is_public) {
            checker->effects->functions[index].inferred = true;
        }
    }

    while (component_count != 0 && !checker->malformed
        && !checker->allocation_failed) {
        size_t component = --component_count;
        bool changed;
        do {
            changed = false;
            for (size_t index = 0; index < count; ++index) {
                const SolSyntaxItem *item = &checker->syntax->items[index];
                if (components[index] != component || item->kind != SOL_ITEM_FUNCTION
                    || item->has_effect_clause || item->is_public) {
                    continue;
                }
                SolEffectRow *row = &checker->effects->functions[index];
                size_t previous_count = row->count;
                sol_effect_walk_function(checker, index, SOL_EFFECT_WALK_INFER);
                if (row->count != previous_count) changed = true;
            }
        } while (changed && !checker->malformed && !checker->allocation_failed);
    }
}

static void sol_effect_validate_function_coercions(SolEffectChecker *checker) {
    for (size_t index = 0; index < checker->types->function_coercion_count; ++index) {
        const SolFunctionCoercion *coercion
            = &checker->types->function_coercions[index];
        SolType actual = checker->types->expressions[coercion->expression];
        const SolEffectAtom *actual_atoms;
        size_t actual_count;
        if (actual.kind == SOL_TYPE_FUNCTION) {
            actual_atoms = checker->effects->functions[actual.definition].atoms;
            actual_count = checker->effects->functions[actual.definition].count;
        } else if (actual.kind == SOL_TYPE_CAPABILITY_OPERATION) {
            actual_atoms = checker->effects->capability_members[actual.definition].atoms;
            actual_count = checker->effects->capability_members[actual.definition].count;
        } else {
            checker->malformed = true;
            return;
        }
        const SolEffectSet *expected = &checker->types->function_types[
            coercion->expected.definition
        ].effects;
        bool reported = false;
        for (size_t atom = 0; atom < actual_count; ++atom) {
            if (actual_atoms[atom].argument_kind == SOL_EFFECT_ATOM_PARAMETER
                || actual_atoms[atom].argument_kind == SOL_EFFECT_ATOM_SELF) {
                if (!reported) {
                    sol_effect_error(
                        checker,
                        "SOL-EFFECT-007",
                        checker->syntax->expressions[coercion->expression].span,
                        "function value has authority-dependent effects that a closed callback type cannot represent"
                    );
                    reported = true;
                }
                continue;
            }
            bool found = false;
            for (size_t candidate = 0; candidate < expected->count; ++candidate) {
                if (sol_effect_atom_equal(
                    checker,
                    &actual_atoms[atom],
                    &expected->atoms[candidate]
                )) {
                    found = true;
                    break;
                }
            }
            if (!found && !reported) {
                sol_effect_error(
                    checker,
                    "SOL-EFFECT-006",
                    checker->syntax->expressions[coercion->expression].span,
                    "function value performs effects not permitted by the expected callback type"
                );
                reported = true;
            }
        }
    }
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
        .current_member = SOL_AST_NONE,
    };
    bool valid_inputs = effects->functions == NULL && effects->function_count == 0
        && effects->capability_members == NULL && effects->capability_member_count == 0
        && sol_effect_validate_inputs(&checker);
    if (!valid_inputs) {
        sol_effect_error(
            &checker,
            "SOL-INTERNAL-004",
            (SolSpan){0},
            "invalid syntax, semantic input, or output table passed to effect checking"
        );
        free(checker.parameter_owners);
        free(checker.expression_owners);
        return false;
    }
    if (!sol_effect_allocate_table(&checker)) {
        free(checker.parameter_owners);
        free(checker.expression_owners);
        return false;
    }
    checker.visited = calloc(syntax->expression_count, sizeof(*checker.visited));
    checker.reported_substitutions = calloc(
        syntax->expression_count,
        sizeof(*checker.reported_substitutions)
    );
    if (syntax->expression_count != 0
        && (checker.visited == NULL || checker.reported_substitutions == NULL)) {
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
                SOL_EFFECT_OWNER_ITEM,
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
            }
        }
        for (size_t index = 0; index < syntax->capability_member_count; ++index) {
            const SolCapabilityMember *member = &syntax->capability_members[index];
            SolEffectRow *row = &effects->capability_members[index];
            sol_effect_validate_and_normalize_row(
                &checker,
                member->first_effect,
                member->span,
                SOL_EFFECT_OWNER_CAPABILITY_MEMBER,
                index,
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
        size_t component_count = 0;
        size_t *components = sol_effect_find_components(&checker, &component_count);
        if (!checker.allocation_failed) {
            sol_effect_infer_functions(&checker, components, component_count);
            if (!checker.malformed && !checker.allocation_failed) {
                sol_effect_validate_function_coercions(&checker);
            }
        }
        free(components);
        if (!checker.malformed && !checker.allocation_failed) {
            for (size_t index = 0; index < syntax->item_count; ++index) {
                const SolSyntaxItem *item = &syntax->items[index];
                if (item->kind == SOL_ITEM_FUNCTION && item->has_effect_clause) {
                    sol_effect_walk_function(&checker, index, SOL_EFFECT_WALK_VALIDATE);
                }
            }
            for (size_t index = 0; index < syntax->capability_member_count; ++index) {
                if (syntax->capability_members[index].body != SOL_AST_NONE) {
                    sol_effect_walk_member(&checker, index);
                }
            }
        }
    }

    free(checker.edges);
    free(checker.first_outgoing);
    free(checker.first_incoming);
    free(checker.visited);
    free(checker.reported_substitutions);
    free(checker.parameter_owners);
    free(checker.expression_owners);
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
