#include "sol/effectcheck.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    SOL_EFFECT_WALK_GRAPH,
    SOL_EFFECT_WALK_INFER,
    SOL_EFFECT_WALK_VALIDATE,
    SOL_EFFECT_WALK_RECORD,
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
    SolTraitMethodId current_trait_method;
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
    for (size_t index = 0; index < table->trait_method_count; ++index) {
        free(table->trait_methods[index].atoms);
    }
    free(table->functions);
    free(table->capability_members);
    free(table->trait_methods);
    free(table->call_instantiations);
    free(table->call_arguments);
    free(table->call_rows);
    memset(table, 0, sizeof(*table));
}

const SolEffectCallInstantiation *sol_effect_call_instantiation(
    const SolEffectTable *table,
    SolExprId call
) {
    if (table == NULL
        || table->call_instantiation_count > table->call_instantiation_capacity
        || call >= table->call_instantiation_count
        || table->call_instantiations == NULL
        || table->call_instantiations[call].call == SOL_AST_NONE) return NULL;
    return &table->call_instantiations[call];
}

static bool sol_effect_call_slice(
    const SolEffectTable *table,
    SolExprId call,
    bool row,
    const SolEffectAtom **atoms,
    size_t *count
) {
    if (atoms == NULL || count == NULL) return false;
    *atoms = NULL;
    *count = 0;
    const SolEffectCallInstantiation *entry
        = sol_effect_call_instantiation(table, call);
    if (entry == NULL) return false;
    size_t offset = row ? entry->row_offset : entry->argument_offset;
    size_t length = row ? entry->row_count : entry->argument_count;
    size_t arena_count = row ? table->call_row_count : table->call_argument_count;
    size_t arena_capacity = row ? table->call_row_capacity : table->call_argument_capacity;
    const SolEffectAtom *arena = row ? table->call_rows : table->call_arguments;
    if (arena_count > arena_capacity || offset > arena_count
        || length > arena_count - offset
        || ((arena_capacity != 0) != (arena != NULL))) return false;
    *atoms = length == 0 ? NULL : arena + offset;
    *count = length;
    return true;
}

bool sol_effect_call_arguments(
    const SolEffectTable *table,
    SolExprId call,
    const SolEffectAtom **atoms,
    size_t *count
) {
    return sol_effect_call_slice(table, call, false, atoms, count);
}

bool sol_effect_call_row(
    const SolEffectTable *table,
    SolExprId call,
    const SolEffectAtom **atoms,
    size_t *count
) {
    return sol_effect_call_slice(table, call, true, atoms, count);
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

static bool sol_effect_authority_free_atom(
    const SolEffectChecker *checker,
    const SolEffect *effect
) {
    return !effect->has_argument
        && (sol_effect_span_text_equal(checker->source, effect->name, "panic")
            || sol_effect_span_text_equal(checker->source, effect->name, "diverge"));
}

static bool sol_effect_authority_free_semantic_atom(
    const SolEffectChecker *checker,
    const SolEffectAtom *atom
) {
    return atom->argument_kind == SOL_EFFECT_ATOM_NO_ARGUMENT
        && (sol_effect_span_text_equal(checker->source, atom->name, "panic")
            || sol_effect_span_text_equal(checker->source, atom->name, "diverge"));
}

static bool sol_effect_function_atom_matches_syntax(
    const SolEffectChecker *checker,
    const SolEffectAtom *atom
) {
    for (size_t index = 0; index < checker->syntax->effect_count; ++index) {
        const SolEffect *effect = &checker->syntax->effects[index];
        if (effect->owner_kind == SOL_EFFECT_OWNER_TYPE
            && !effect->is_pure && !effect->has_argument
            && effect->name.start == atom->name.start
            && effect->name.end == atom->name.end
            && effect->span.start == atom->span.start
            && effect->span.end == atom->span.end) {
            return true;
        }
    }
    return false;
}

static bool sol_effect_function_type_matches_syntax(
    const SolEffectChecker *checker,
    SolTypeId type_id
) {
    const SolSyntaxType *syntax_type = &checker->syntax->types[type_id];
    if (syntax_type->kind != SOL_SYNTAX_TYPE_FUNCTION) return true;
    SolType semantic = checker->types->declared_types[type_id];
    if (semantic.kind != SOL_TYPE_FUNCTION_SIGNATURE
        || semantic.definition >= checker->types->function_type_count) return false;
    const SolFunctionType *function = &checker->types->function_types[semantic.definition];
    size_t expected_count = 0;
    SolEffectId effect_id = syntax_type->first_effect;
    while (effect_id != SOL_AST_NONE) {
        if (effect_id >= checker->syntax->effect_count) return false;
        const SolEffect *effect = &checker->syntax->effects[effect_id];
        if (effect->owner_kind != SOL_EFFECT_OWNER_TYPE || effect->owner != type_id) {
            return false;
        }
        if (!effect->is_pure
            && checker->hir->effect_resolutions[effect_id].kind
                != SOL_EFFECT_RESOLUTION_PARAMETER) {
            ++expected_count;
            bool found = false;
            for (size_t atom = 0; atom < function->effects.count; ++atom) {
                const SolEffectAtom *candidate = &function->effects.atoms[atom];
                if (candidate->argument_kind == SOL_EFFECT_ATOM_NO_ARGUMENT
                    && sol_effect_span_equal(
                        checker->source, candidate->name, effect->name
                    )) {
                    found = true;
                    break;
                }
            }
            if (!found) return false;
        }
        effect_id = effect->next;
    }
    return function->effects.count == expected_count;
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
            SolEffectResolution resolution = checker->hir->effect_resolutions[effect_id];
            if (resolution.kind == SOL_EFFECT_RESOLUTION_PARAMETER) {
                if (effect->has_argument) {
                    sol_effect_error(
                        checker,
                        "SOL-EFFECT-008",
                        effect->span,
                        "an effect-row parameter cannot have an authority argument"
                    );
                }
                if (row->effect_parameter != SOL_AST_NONE
                    && row->effect_parameter != resolution.target) {
                    checker->malformed = true;
                    return;
                }
                row->effect_parameter = resolution.target;
                effect_id = effect->next;
                continue;
            }
            SolEffectAtom atom = sol_effect_normalize_atom(
                checker,
                effect,
                first_parameter,
                member
            );
            if (!effect->has_argument && !sol_effect_authority_free_atom(checker, effect)) {
                sol_effect_error(
                    checker,
                    "SOL-EFFECT-010",
                    effect->span,
                    "effect requires an explicit lexical capability authority"
                );
            } else if (atom.argument_kind == SOL_EFFECT_ATOM_STATIC_PATH) {
                sol_effect_error(
                    checker,
                    "SOL-EFFECT-010",
                    effect->argument,
                    "static authority is unavailable; use a lexical capability parameter"
                );
            }
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
    SolProvenanceId *provenance_id
) {
    if (expression_id >= checker->syntax->expression_count) {
        checker->malformed = true;
        return false;
    }
    SolProvenanceId origin = checker->types->expression_capability_origins[expression_id];
    SolType type = checker->types->expressions[expression_id];
    SolProvenance provenance;
    if (type.kind != SOL_TYPE_NOMINAL
        || type.definition >= checker->syntax->item_count
        || checker->syntax->items[type.definition].kind != SOL_ITEM_CAPABILITY
        || origin == SOL_PROVENANCE_NONE) {
        return false;
    }
    if (!sol_type_provenance(checker->types, origin, &provenance)) {
        checker->malformed = true;
        return false;
    }
    for (size_t index = 0; index < provenance.count; ++index) {
        SolParameterId root = provenance.roots[index];
        if (root >= checker->syntax->parameter_count
            || checker->parameter_owners[root] != checker->current_function) {
            checker->malformed = true;
            return false;
        }
    }
    *provenance_id = origin;
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
    SolProvenanceId receiver_provenance,
    SolEffectAtom *instantiated,
    SolProvenance *roots
) {
    *instantiated = *required;
    *roots = (SolProvenance){0};
    if (required->argument_kind == SOL_EFFECT_ATOM_NO_ARGUMENT
        || required->argument_kind == SOL_EFFECT_ATOM_STATIC_PATH) {
        return true;
    }
    if (required->argument_kind == SOL_EFFECT_ATOM_SELF) {
        if (!sol_type_provenance(checker->types, receiver_provenance, roots)) {
            checker->malformed = true;
            return false;
        }
        instantiated->argument_kind = SOL_EFFECT_ATOM_PARAMETER;
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
    SolProvenanceId caller_provenance = SOL_PROVENANCE_NONE;
    if (!sol_effect_capability_origin(checker, actual, &caller_provenance)) {
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
    if (!sol_type_provenance(checker->types, caller_provenance, roots)) {
        checker->malformed = true;
        return false;
    }
    return true;
}

typedef enum {
    SOL_EFFECT_CALL_NONE,
    SOL_EFFECT_CALL_FUNCTION,
    SOL_EFFECT_CALL_SIGNATURE,
    SOL_EFFECT_CALL_MEMBER,
    SOL_EFFECT_CALL_TRAIT_METHOD,
} SolEffectCallKind;

typedef struct {
    SolEffectCallKind kind;
    size_t target;
    SolParameterId first_parameter;
    SolProvenanceId receiver_provenance;
} SolEffectCall;

static SolEffectCall sol_effect_resolve_call(
    SolEffectChecker *checker,
    const SolExpr *call
) {
    SolEffectCall result = {
        .kind = SOL_EFFECT_CALL_NONE,
        .target = SOL_AST_NONE,
        .first_parameter = SOL_AST_NONE,
        .receiver_provenance = SOL_PROVENANCE_NONE,
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
        result.receiver_provenance
            = checker->types->expression_operation_origins[callee_id];
        SolProvenance provenance;
        if (!sol_type_provenance(
                checker->types,
                result.receiver_provenance,
                &provenance
            )) {
            checker->malformed = true;
            result.kind = SOL_EFFECT_CALL_NONE;
        } else {
            for (size_t index = 0; index < provenance.count; ++index) {
                SolParameterId root = provenance.roots[index];
                if (root >= checker->syntax->parameter_count
                    || checker->parameter_owners[root] != checker->current_function) {
                    checker->malformed = true;
                    result.kind = SOL_EFFECT_CALL_NONE;
                    break;
                }
            }
        }
    } else if (callee_type.kind == SOL_TYPE_TRAIT_METHOD) {
        SolExprId call_id = (SolExprId)(call - checker->syntax->expressions);
        const SolMethodResolution *method = sol_type_method_resolution(
            checker->types, call_id
        );
        if (method == NULL || method->method >= checker->syntax->trait_method_count) {
            checker->malformed = true;
        } else {
            result.kind = SOL_EFFECT_CALL_TRAIT_METHOD;
            result.target = method->kind == SOL_METHOD_RESOLUTION_REQUIREMENT
                ? method->requirement : method->method;
            result.first_parameter
                = checker->syntax->trait_methods[result.target].first_parameter;
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

static bool sol_effect_row_contains(
    const SolEffectChecker *checker,
    const SolEffectRow *row,
    const SolEffectAtom *atom
) {
    for (size_t index = 0; index < row->count; ++index) {
        if (sol_effect_atom_equal(checker, &row->atoms[index], atom)) return true;
    }
    return false;
}

static bool sol_effect_callback_row(
    SolEffectChecker *checker,
    SolExprId expression,
    const SolEffectAtom **atoms,
    size_t *count,
    SolEffectParameterId *tail,
    SolDefId *exact
) {
    *atoms = NULL;
    *count = 0;
    *tail = SOL_AST_NONE;
    *exact = SOL_AST_NONE;
    if (expression >= checker->types->expression_count) {
        checker->malformed = true;
        return false;
    }
    SolType type = checker->types->expressions[expression];
    if (type.kind == SOL_TYPE_FUNCTION_SIGNATURE) {
        if (type.definition >= checker->types->function_type_count) {
            checker->malformed = true;
            return false;
        }
        const SolFunctionType *function = &checker->types->function_types[type.definition];
        *atoms = function->effects.atoms;
        *count = function->effects.count;
        *tail = function->effect_parameter;
        return true;
    }
    if (type.kind == SOL_TYPE_FUNCTION) {
        if (type.definition >= checker->effects->function_count) {
            checker->malformed = true;
            return false;
        }
        const SolEffectRow *row = &checker->effects->functions[type.definition];
        *atoms = row->atoms;
        *count = row->count;
        *tail = row->effect_parameter;
        *exact = type.definition;
        return true;
    }
    if (type.kind == SOL_TYPE_CAPABILITY_OPERATION) {
        if (type.definition >= checker->effects->capability_member_count) {
            checker->malformed = true;
            return false;
        }
        const SolEffectRow *row = &checker->effects->capability_members[type.definition];
        *atoms = row->atoms;
        *count = row->count;
        *tail = row->effect_parameter;
        return true;
    }
    return false;
}

static void sol_effect_graph_callback_dependencies(
    SolEffectChecker *checker,
    const SolExpr *call,
    SolDefId function
) {
    const SolSyntaxItem *item = &checker->syntax->items[function];
    if (item->first_effect_parameter == SOL_AST_NONE) return;
    SolParameterId parameter = item->first_parameter;
    while (parameter != SOL_AST_NONE) {
        SolType type = checker->types->declared_types[
            checker->syntax->parameters[parameter].type_id
        ];
        if (type.kind == SOL_TYPE_FUNCTION_SIGNATURE
            && type.definition < checker->types->function_type_count
            && checker->types->function_types[type.definition].effect_parameter
                == item->first_effect_parameter) {
            SolExprId actual = sol_effect_find_actual_argument(
                checker, item->first_parameter, parameter, call->as.call.first_argument
            );
            const SolEffectAtom *atoms;
            size_t count;
            SolEffectParameterId tail;
            SolDefId exact;
            if (actual != SOL_AST_NONE && sol_effect_callback_row(
                checker, actual, &atoms, &count, &tail, &exact
            ) && exact != SOL_AST_NONE) {
                sol_effect_add_edge(checker, checker->current_function, exact);
            }
        }
        parameter = checker->syntax->parameters[parameter].next;
    }
}

static bool sol_effect_infer_call_row(
    SolEffectChecker *checker,
    const SolExpr *call,
    SolDefId function,
    SolEffectRow *arguments,
    SolEffectRow *row
) {
    const SolSyntaxItem *item = &checker->syntax->items[function];
    const SolEffectRow *declared = &checker->effects->functions[function];
    SolEffectParameterId parameter_id = item->first_effect_parameter;
    row->effect_parameter = SOL_AST_NONE;
    arguments->effect_parameter = SOL_AST_NONE;
    for (size_t index = 0; index < declared->count; ++index) {
        sol_effect_row_append(checker, row, declared->atoms[index]);
    }
    bool determined = false;
    SolParameterId parameter = item->first_parameter;
    while (parameter != SOL_AST_NONE) {
        SolType type = checker->types->declared_types[
            checker->syntax->parameters[parameter].type_id
        ];
        if (type.kind == SOL_TYPE_FUNCTION_SIGNATURE
            && type.definition < checker->types->function_type_count) {
            const SolFunctionType *expected = &checker->types->function_types[type.definition];
            if (expected->effect_parameter == parameter_id) {
                SolExprId actual = sol_effect_find_actual_argument(
                    checker, item->first_parameter, parameter, call->as.call.first_argument
                );
                const SolEffectAtom *actual_atoms;
                size_t actual_count;
                SolEffectParameterId actual_tail;
                SolDefId exact;
                if (actual == SOL_AST_NONE || !sol_effect_callback_row(
                    checker,
                    actual,
                    &actual_atoms,
                    &actual_count,
                    &actual_tail,
                    &exact
                )) {
                    parameter = checker->syntax->parameters[parameter].next;
                    continue;
                }
                determined = true;
                for (size_t atom = 0; atom < actual_count; ++atom) {
                    if (actual_atoms[atom].argument_kind == SOL_EFFECT_ATOM_PARAMETER
                        || actual_atoms[atom].argument_kind == SOL_EFFECT_ATOM_SELF) {
                        if (checker->mode == SOL_EFFECT_WALK_RECORD) {
                            sol_effect_error(
                                checker,
                                "SOL-EFFECT-007",
                                checker->syntax->expressions[actual].span,
                                "effect-row arguments must be authority-independent closed sets"
                            );
                        }
                        continue;
                    }
                    SolEffectRow fixed = {
                        .atoms = expected->effects.atoms,
                        .count = expected->effects.count,
                        .effect_parameter = SOL_AST_NONE,
                    };
                    if (!sol_effect_row_contains(checker, &fixed, &actual_atoms[atom])) {
                        sol_effect_row_append(checker, arguments, actual_atoms[atom]);
                        sol_effect_row_append(checker, row, actual_atoms[atom]);
                    }
                }
                if (actual_tail != SOL_AST_NONE) {
                    SolEffectParameterId owner_tail = checker->syntax->items[
                        checker->current_function
                    ].first_effect_parameter;
                    if (actual_tail != owner_tail) {
                        checker->malformed = true;
                    } else if (row->effect_parameter != SOL_AST_NONE
                        && row->effect_parameter != actual_tail) {
                        checker->malformed = true;
                    } else {
                        row->effect_parameter = actual_tail;
                        arguments->effect_parameter = actual_tail;
                    }
                }
            }
        }
        parameter = checker->syntax->parameters[parameter].next;
    }
    if (!determined && checker->mode == SOL_EFFECT_WALK_RECORD) {
        sol_effect_error(
            checker,
            "SOL-EFFECT-008",
            call->span,
            "effect-row argument cannot be inferred from a callback parameter"
        );
    }
    return determined;
}

static bool sol_effect_append_call_atoms(
    SolEffectChecker *checker,
    bool row,
    const SolEffectAtom *atoms,
    size_t count,
    size_t *offset
) {
    SolEffectTable *table = checker->effects;
    SolEffectAtom **arena = row ? &table->call_rows : &table->call_arguments;
    size_t *arena_count = row ? &table->call_row_count : &table->call_argument_count;
    size_t *capacity = row ? &table->call_row_capacity : &table->call_argument_capacity;
    *offset = *arena_count;
    if (count > SIZE_MAX - *arena_count) {
        checker->allocation_failed = true;
        return false;
    }
    size_t required = *arena_count + count;
    if (required > *capacity) {
        size_t grown_capacity = *capacity == 0 ? 16 : *capacity;
        while (grown_capacity < required) {
            if (grown_capacity > SIZE_MAX / 2) {
                checker->allocation_failed = true;
                return false;
            }
            grown_capacity *= 2;
        }
        if (grown_capacity > SIZE_MAX / sizeof(**arena)) {
            checker->allocation_failed = true;
            return false;
        }
        SolEffectAtom *grown = realloc(*arena, grown_capacity * sizeof(**arena));
        if (grown == NULL) {
            checker->allocation_failed = true;
            return false;
        }
        *arena = grown;
        *capacity = grown_capacity;
    }
    if (count != 0) memcpy(*arena + *arena_count, atoms, count * sizeof(*atoms));
    *arena_count = required;
    return true;
}

static void sol_effect_record_call(
    SolEffectChecker *checker,
    const SolExpr *call,
    SolDefId function,
    const SolEffectRow *arguments,
    const SolEffectRow *row
) {
    SolExprId call_id = (SolExprId)(call - checker->syntax->expressions);
    if (call_id >= checker->effects->call_instantiation_count
        || checker->effects->call_instantiations[call_id].call != SOL_AST_NONE) {
        checker->malformed = true;
        return;
    }
    size_t argument_offset;
    size_t row_offset;
    if (!sol_effect_append_call_atoms(
            checker, false, arguments->atoms, arguments->count, &argument_offset
        ) || !sol_effect_append_call_atoms(
            checker, true, row->atoms, row->count, &row_offset
        )) return;
    checker->effects->call_instantiations[call_id] = (SolEffectCallInstantiation){
        .call = call_id,
        .function = function,
        .parameter = row->effect_parameter,
        .argument_offset = argument_offset,
        .argument_count = arguments->count,
        .row_offset = row_offset,
        .row_count = row->count,
    };
}

bool sol_effect_call_instantiation_valid(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    const SolHirModule *hir,
    const SolTypeTable *types,
    const SolEffectTable *effects,
    SolExprId call_id
) {
    if (source == NULL || syntax == NULL || hir == NULL || types == NULL
        || effects == NULL || call_id >= syntax->expression_count
        || call_id >= hir->resolution_count || hir->expression_owners == NULL
        || call_id >= types->expression_count
        || syntax->expressions[call_id].kind != SOL_EXPR_CALL) return false;
    const SolEffectCallInstantiation *entry = sol_effect_call_instantiation(
        effects, call_id
    );
    if (entry == NULL || entry->call != call_id
        || entry->function >= syntax->item_count
        || syntax->items[entry->function].kind != SOL_ITEM_FUNCTION
        || syntax->items[entry->function].first_effect_parameter == SOL_AST_NONE
        || entry->function >= effects->function_count
        || effects->functions[entry->function].effect_parameter
            != syntax->items[entry->function].first_effect_parameter) return false;
    SolExprId callee = syntax->expressions[call_id].as.call.callee;
    if (callee >= types->expression_count
        || types->expressions[callee].kind != SOL_TYPE_FUNCTION
        || types->expressions[callee].definition != entry->function
        || hir->expression_owners[call_id] >= syntax->item_count) return false;
    const SolEffectAtom *stored_arguments = NULL;
    const SolEffectAtom *stored_row = NULL;
    size_t stored_argument_count = 0;
    size_t stored_row_count = 0;
    if (!sol_effect_call_arguments(
            effects, call_id, &stored_arguments, &stored_argument_count
        ) || !sol_effect_call_row(
            effects, call_id, &stored_row, &stored_row_count
        )) return false;
    SolEffectTable *mutable_effects = (SolEffectTable *)effects;
    SolEffectChecker checker = {
        .source = source,
        .syntax = syntax,
        .hir = hir,
        .types = types,
        .effects = mutable_effects,
        .current_function = hir->expression_owners[call_id],
        .current_member = SOL_AST_NONE,
        .current_trait_method = SOL_AST_NONE,
        .mode = SOL_EFFECT_WALK_VALIDATE,
    };
    SolEffectRow arguments = {.effect_parameter = SOL_AST_NONE};
    SolEffectRow row = {.effect_parameter = SOL_AST_NONE};
    bool inferred = sol_effect_infer_call_row(
        &checker,
        &syntax->expressions[call_id],
        entry->function,
        &arguments,
        &row
    );
    bool valid = inferred && !checker.malformed && !checker.allocation_failed
        && entry->parameter == row.effect_parameter
        && stored_argument_count == arguments.count
        && stored_row_count == row.count;
    for (size_t index = 0; valid && index < arguments.count; ++index) {
        valid = sol_effect_atom_equal(
            &checker, &stored_arguments[index], &arguments.atoms[index]
        );
    }
    for (size_t index = 0; valid && index < row.count; ++index) {
        valid = sol_effect_atom_equal(&checker, &stored_row[index], &row.atoms[index]);
    }
    free(arguments.atoms);
    free(row.atoms);
    return valid;
}

static void sol_effect_process_call(SolEffectChecker *checker, const SolExpr *call) {
    SolEffectCall resolved = sol_effect_resolve_call(checker, call);
    if (resolved.kind == SOL_EFFECT_CALL_NONE || checker->malformed) return;
    if (checker->mode == SOL_EFFECT_WALK_GRAPH) {
        if (resolved.kind == SOL_EFFECT_CALL_FUNCTION) {
            sol_effect_add_edge(checker, checker->current_function, resolved.target);
            sol_effect_graph_callback_dependencies(checker, call, resolved.target);
        }
        return;
    }
    SolEffectRow instantiated_arguments = {.effect_parameter = SOL_AST_NONE};
    SolEffectRow instantiated_row = {.effect_parameter = SOL_AST_NONE};
    bool polymorphic = resolved.kind == SOL_EFFECT_CALL_FUNCTION
        && checker->effects->functions[resolved.target].effect_parameter != SOL_AST_NONE;
    if (polymorphic) {
        sol_effect_infer_call_row(
            checker,
            call,
            resolved.target,
            &instantiated_arguments,
            &instantiated_row
        );
        if (checker->mode == SOL_EFFECT_WALK_RECORD) {
            sol_effect_record_call(
                checker,
                call,
                resolved.target,
                &instantiated_arguments,
                &instantiated_row
            );
            free(instantiated_arguments.atoms);
            free(instantiated_row.atoms);
            return;
        }
    } else if (checker->mode == SOL_EFFECT_WALK_RECORD) {
        return;
    }
    const SolEffectAtom *required_atoms;
    size_t required_count;
    if (polymorphic) {
        required_atoms = instantiated_row.atoms;
        required_count = instantiated_row.count;
    } else if (resolved.kind == SOL_EFFECT_CALL_FUNCTION) {
        required_atoms = checker->effects->functions[resolved.target].atoms;
        required_count = checker->effects->functions[resolved.target].count;
    } else if (resolved.kind == SOL_EFFECT_CALL_SIGNATURE) {
        required_atoms = checker->types->function_types[resolved.target].effects.atoms;
        required_count = checker->types->function_types[resolved.target].effects.count;
    } else if (resolved.kind == SOL_EFFECT_CALL_MEMBER) {
        required_atoms = checker->effects->capability_members[resolved.target].atoms;
        required_count = checker->effects->capability_members[resolved.target].count;
    } else {
        required_atoms = checker->effects->trait_methods[resolved.target].atoms;
        required_count = checker->effects->trait_methods[resolved.target].count;
    }
    SolEffectRow *caller = checker->current_trait_method != SOL_AST_NONE
        ? &checker->effects->trait_methods[checker->current_trait_method]
        : checker->current_member == SOL_AST_NONE
            ? &checker->effects->functions[checker->current_function]
            : &checker->effects->capability_members[checker->current_member];
    SolEffectParameterId required_tail = polymorphic
        ? instantiated_row.effect_parameter
        : resolved.kind == SOL_EFFECT_CALL_SIGNATURE
            ? checker->types->function_types[resolved.target].effect_parameter
            : SOL_AST_NONE;
    if (required_tail != SOL_AST_NONE) {
        if (checker->handled_count != 0) {
            sol_effect_error(
                checker,
                "SOL-EFFECT-008",
                call->span,
                "handlers cannot transform an unresolved effect row"
            );
        } else if (caller->effect_parameter != required_tail
            && checker->mode == SOL_EFFECT_WALK_VALIDATE) {
            sol_effect_error(
                checker,
                "SOL-EFFECT-008",
                call->span,
                "an unresolved callee row must be present in the caller's declared row"
            );
        }
    }
    for (size_t index = 0; index < required_count; ++index) {
        SolEffectAtom instantiated;
        SolProvenance roots;
        if (!sol_effect_instantiate_atom(
            checker,
            call,
            &required_atoms[index],
            resolved.first_parameter,
            resolved.receiver_provenance,
            &instantiated,
            &roots
        )) {
            continue;
        }
        size_t instance_count = roots.count == 0 ? 1 : roots.count;
        for (size_t instance = 0; instance < instance_count; ++instance) {
            if (roots.count != 0) {
                SolParameterId root = roots.roots[instance];
                instantiated.parameter = root;
                instantiated.argument = checker->syntax->parameters[root].name;
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
                    && sol_effect_span_equal(
                        checker->source,
                        allowed->name,
                        instantiated.name
                    );
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
    free(instantiated_arguments.atoms);
    free(instantiated_row.atoms);
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
        case SOL_EXPR_TYPE_APPLICATION:
            sol_effect_expression(checker, expression->as.type_application.base);
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
    checker->current_trait_method = SOL_AST_NONE;
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
    checker->current_trait_method = SOL_AST_NONE;
    checker->mode = SOL_EFFECT_WALK_VALIDATE;
    checker->depth = 0;
    checker->handled_count = 0;
    sol_effect_expression(checker, member->body);
}

static void sol_effect_walk_trait_method(
    SolEffectChecker *checker,
    SolTraitMethodId method_id
) {
    const SolTraitMethod *method = &checker->syntax->trait_methods[method_id];
    if (method->body == SOL_AST_NONE) return;
    memset(checker->visited, 0,
        checker->syntax->expression_count * sizeof(*checker->visited));
    checker->current_function = method->owner_item;
    checker->current_member = SOL_AST_NONE;
    checker->current_trait_method = method_id;
    checker->mode = SOL_EFFECT_WALK_VALIDATE;
    checker->depth = 0;
    checker->handled_count = 0;
    sol_effect_expression(checker, method->body);
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
            && expression->kind <= SOL_EXPR_TYPE_APPLICATION
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
            case SOL_EXPR_TYPE_APPLICATION:
                valid = valid
                    && expression->as.type_application.base < syntax->expression_count
                    && expression->as.type_application.first_argument
                        < syntax->type_argument_count;
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
            && resolution.kind <= SOL_RESOLUTION_REFINEMENT_SELF;
        if (resolution.kind == SOL_RESOLUTION_DEFINITION) {
            valid = valid && resolution.target < hir->definition_count;
        } else if (resolution.kind == SOL_RESOLUTION_LOCAL) {
            valid = valid && resolution.target < hir->local_count;
        } else if (resolution.kind == SOL_RESOLUTION_BUILTIN) {
            valid = valid && resolution.target <= SOL_BUILTIN_NONE;
        } else if (resolution.kind == SOL_RESOLUTION_REFINEMENT_SELF) {
            valid = valid
                && expression->kind == SOL_EXPR_PATH
                && sol_effect_span_text_equal(source, expression->as.name, "self")
                && resolution.target < syntax->item_count
                && syntax->items[resolution.target].kind == SOL_ITEM_TYPE
                && syntax->items[resolution.target].flavor
                    == SOL_TYPE_DECLARATION_REFINED
                && hir->expression_owners[index] == resolution.target;
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
    bool completed = checker->hir->file_scope_count != 0
        ? sol_hir_lower_scoped(
            checker->source,
            checker->syntax,
            checker->hir->file_scopes,
            checker->hir->file_scope_count,
            &rebuilt,
            &diagnostics
        )
        : sol_hir_lower(
            checker->source, checker->syntax, &rebuilt, &diagnostics
        );
    bool matches = completed
        && !sol_diagnostics_has_errors(&diagnostics)
        && rebuilt.definition_count == checker->hir->definition_count
        && rebuilt.local_count == checker->hir->local_count
        && rebuilt.resolution_count == checker->hir->resolution_count
        && rebuilt.type_resolution_count == checker->hir->type_resolution_count
        && rebuilt.effect_resolution_count == checker->hir->effect_resolution_count
        && rebuilt.type_effect_resolution_count
            == checker->hir->type_effect_resolution_count
        && rebuilt.trait_resolution_count == checker->hir->trait_resolution_count
        && rebuilt.bound_resolution_count == checker->hir->bound_resolution_count
        && rebuilt.semantic_reference_count == checker->hir->semantic_reference_count
        && rebuilt.import_resolution_count == checker->hir->import_resolution_count
        && rebuilt.file_scope_count == checker->hir->file_scope_count;
    for (size_t index = 0; matches && index < rebuilt.file_scope_count; ++index) {
        const SolHirFileScope *left = &rebuilt.file_scopes[index];
        const SolHirFileScope *right = &checker->hir->file_scopes[index];
        matches = left->module_name.start == right->module_name.start
            && left->module_name.end == right->module_name.end
            && left->import_start == right->import_start
            && left->import_count == right->import_count
            && left->item_start == right->item_start
            && left->item_count == right->item_count;
    }
    for (size_t index = 0;
        matches && rebuilt.file_scope_count != 0 && index < rebuilt.definition_count;
        ++index) {
        matches = rebuilt.item_files[index] == checker->hir->item_files[index];
    }
    for (size_t index = 0; matches && index < rebuilt.definition_count; ++index) {
        const SolHirDefinition *left = &rebuilt.definitions[index];
        const SolHirDefinition *right = &checker->hir->definitions[index];
        matches = left->kind == right->kind
            && left->name.start == right->name.start
            && left->name.end == right->name.end
            && left->stable_identity.start == right->stable_identity.start
            && left->stable_identity.end == right->stable_identity.end
            && left->semantic_id.high == right->semantic_id.high
            && left->semantic_id.low == right->semantic_id.low
            && left->syntax_item == right->syntax_item;
    }
    for (size_t index = 0; matches && index < rebuilt.semantic_reference_count; ++index) {
        const SolSemanticReference *left = &rebuilt.semantic_references[index];
        const SolSemanticReference *right = &checker->hir->semantic_references[index];
        matches = left->kind == right->kind
            && left->span.start == right->span.start
            && left->span.end == right->span.end
            && left->target == right->target
            && left->target_id.high == right->target_id.high
            && left->target_id.low == right->target_id.low;
    }
    for (size_t index = 0; matches && index < rebuilt.import_resolution_count; ++index) {
        matches = rebuilt.import_resolutions[index].kind
                == checker->hir->import_resolutions[index].kind
            && rebuilt.import_resolutions[index].target
                == checker->hir->import_resolutions[index].target;
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
            && rebuilt.resolutions[index].target == checker->hir->resolutions[index].target
            && rebuilt.expression_owners[index] == checker->hir->expression_owners[index];
    }
    for (size_t index = 0; matches && index < rebuilt.type_resolution_count; ++index) {
        matches = rebuilt.type_resolutions[index].kind
                == checker->hir->type_resolutions[index].kind
            && rebuilt.type_resolutions[index].target
                == checker->hir->type_resolutions[index].target;
    }
    for (size_t index = 0; matches && index < rebuilt.effect_resolution_count; ++index) {
        matches = rebuilt.effect_resolutions[index].kind
                == checker->hir->effect_resolutions[index].kind
            && rebuilt.effect_resolutions[index].target
                == checker->hir->effect_resolutions[index].target;
    }
    for (size_t index = 0;
        matches && index < rebuilt.type_effect_resolution_count;
        ++index) {
        matches = rebuilt.type_effect_resolutions[index].kind
                == checker->hir->type_effect_resolutions[index].kind
            && rebuilt.type_effect_resolutions[index].target
                == checker->hir->type_effect_resolutions[index].target;
    }
    for (size_t index = 0; matches && index < rebuilt.trait_resolution_count; ++index) {
        matches = rebuilt.trait_resolutions[index].kind
                == checker->hir->trait_resolutions[index].kind
            && rebuilt.trait_resolutions[index].target
                == checker->hir->trait_resolutions[index].target;
    }
    for (size_t index = 0; matches && index < rebuilt.bound_resolution_count; ++index) {
        matches = rebuilt.bound_resolutions[index].kind
                == checker->hir->bound_resolutions[index].kind
            && rebuilt.bound_resolutions[index].target
                == checker->hir->bound_resolutions[index].target;
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
        || checker->syntax->item_count + checker->syntax->capability_member_count
            > SIZE_MAX - checker->syntax->trait_method_count
        || count > SIZE_MAX / sizeof(*checker->expression_owners)
        || count > SIZE_MAX / 2
        || count * 2 > SIZE_MAX / sizeof(SolEffectExpressionEntry)
        || checker->syntax->argument_count > SIZE_MAX / sizeof(SolExprId)
        || checker->syntax->statement_count > SIZE_MAX / sizeof(SolExprId)
        || checker->syntax->match_arm_count > SIZE_MAX / sizeof(SolExprId)) {
        return false;
    }
    size_t declared_roots = checker->syntax->item_count
        + checker->syntax->capability_member_count
        + checker->syntax->trait_method_count;
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
        } else if (root < checker->syntax->item_count
            + checker->syntax->capability_member_count
            + checker->syntax->trait_method_count) {
            const SolTraitMethod *method = &checker->syntax->trait_methods[
                root - checker->syntax->item_count
                    - checker->syntax->capability_member_count
            ];
            if (method->body == SOL_AST_NONE) continue;
            owner = method->owner_item;
            root_expression = method->body;
        } else if (root < declared_roots) {
            const SolContractCondition *condition
                = &checker->syntax->contract_conditions[
                    root - checker->syntax->item_count
                        - checker->syntax->capability_member_count
                        - checker->syntax->trait_method_count
                ];
            const SolContractClause *clause
                = &checker->syntax->contract_clauses[condition->owner_clause];
            owner = clause->owner_kind == SOL_CONTRACT_OWNER_CAPABILITY_MEMBER
                ? checker->syntax->capability_members[clause->owner].owner_item
                : clause->owner;
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
                case SOL_EXPR_TYPE_APPLICATION:
                    valid = sol_effect_schedule_owned_expression(
                        checker,
                        owner,
                        expression->as.type_application.base,
                        states,
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

static SolProvenanceId sol_effect_block_origin(
    const SolEffectChecker *checker,
    const SolExpr *block,
    const SolProvenanceId *expression_origins
) {
    SolProvenanceId result = SOL_PROVENANCE_NONE;
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
                : SOL_PROVENANCE_NONE;
            terminated = current->kind == SOL_STATEMENT_RETURN
                || value.kind == SOL_TYPE_NEVER;
        }
        statement = current->next;
    }
    return result;
}

static SolProvenanceId sol_effect_union_provenance(
    const SolEffectChecker *checker,
    SolProvenanceId left_id,
    SolProvenanceId right_id
) {
    SolProvenance left;
    SolProvenance right;
    if (!sol_type_provenance(checker->types, left_id, &left)
        || !sol_type_provenance(checker->types, right_id, &right)) {
        return SOL_PROVENANCE_NONE;
    }
    for (SolProvenanceId candidate_id = 0;
        candidate_id < checker->types->provenance_count;
        ++candidate_id) {
        SolProvenance candidate;
        if (!sol_type_provenance(checker->types, candidate_id, &candidate)) {
            return SOL_PROVENANCE_NONE;
        }
        size_t left_index = 0;
        size_t right_index = 0;
        size_t candidate_index = 0;
        bool equal = true;
        while (left_index < left.count || right_index < right.count) {
            SolParameterId root;
            if (right_index == right.count
                || (left_index < left.count
                    && left.roots[left_index] < right.roots[right_index])) {
                root = left.roots[left_index++];
            } else if (left_index == left.count
                || right.roots[right_index] < left.roots[left_index]) {
                root = right.roots[right_index++];
            } else {
                root = left.roots[left_index++];
                ++right_index;
            }
            if (candidate_index >= candidate.count
                || candidate.roots[candidate_index++] != root) {
                equal = false;
            }
        }
        if (equal && candidate_index == candidate.count) return candidate_id;
    }
    return SOL_PROVENANCE_NONE;
}

static SolProvenanceId sol_effect_singleton_provenance(
    const SolEffectChecker *checker,
    SolParameterId root
) {
    for (SolProvenanceId id = 0; id < checker->types->provenance_count; ++id) {
        SolProvenance provenance;
        if (!sol_type_provenance(checker->types, id, &provenance)) {
            return SOL_PROVENANCE_NONE;
        }
        if (provenance.count == 1 && provenance.roots[0] == root) return id;
    }
    return SOL_PROVENANCE_NONE;
}

static bool sol_effect_join_origin(
    const SolEffectChecker *checker,
    SolExprId value_id,
    const SolProvenanceId *expression_origins,
    SolProvenanceId *origin,
    bool *have_value
) {
    SolType value = checker->types->expressions[value_id];
    if (value.kind == SOL_TYPE_NEVER) return true;
    SolProvenanceId value_origin = expression_origins[value_id];
    if (value.kind == SOL_TYPE_UNKNOWN || value.kind == SOL_TYPE_ERROR
        || value_origin == SOL_PROVENANCE_NONE) {
        return false;
    }
    if (!*have_value) {
        *origin = value_origin;
        *have_value = true;
        return true;
    }
    *origin = sol_effect_union_provenance(checker, *origin, value_origin);
    return *origin != SOL_PROVENANCE_NONE;
}

static SolProvenanceId sol_effect_join_expression_origin(
    const SolEffectChecker *checker,
    const SolExpr *expression,
    const SolProvenanceId *expression_origins
) {
    SolProvenanceId origin = SOL_PROVENANCE_NONE;
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
            return SOL_PROVENANCE_NONE;
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
                return SOL_PROVENANCE_NONE;
            }
            arm = checker->syntax->match_arms[arm].next;
        }
    }
    return have_value ? origin : SOL_PROVENANCE_NONE;
}

static SolParameterId sol_effect_contract_result_root(
    const SolEffectChecker *checker,
    SolExprId expression_id
) {
    SolSpan expression_span = checker->syntax->expressions[expression_id].span;
    for (size_t index = 0; index < checker->syntax->contract_condition_count; ++index) {
        const SolContractCondition *condition
            = &checker->syntax->contract_conditions[index];
        SolSpan condition_span = checker->syntax->expressions[
            condition->expression
        ].span;
        if (expression_span.start < condition_span.start
            || expression_span.end > condition_span.end) continue;
        const SolContractClause *clause
            = &checker->syntax->contract_clauses[condition->owner_clause];
        if (clause->owner_kind == SOL_CONTRACT_OWNER_ITEM) {
            return checker->syntax->items[clause->owner].result_authority_parameter;
        }
        const SolCapabilityMember *member
            = &checker->syntax->capability_members[clause->owner];
        return member->result_authority_from_self
            ? checker->syntax->items[member->owner_item].capability_source
            : SOL_AST_NONE;
    }
    return SOL_AST_NONE;
}

static SolProvenanceId sol_effect_expected_expression_origin(
    SolEffectChecker *checker,
    SolExprId expression_id,
    bool capability
) {
    SolType type = checker->types->expressions[expression_id];
    if (capability ? !sol_effect_type_is_capability(checker, type)
                   : type.kind != SOL_TYPE_CAPABILITY_OPERATION) {
        return SOL_PROVENANCE_NONE;
    }
    const SolExpr *expression = &checker->syntax->expressions[expression_id];
    const SolProvenanceId *expression_origins = capability
        ? checker->types->expression_capability_origins
        : checker->types->expression_operation_origins;
    const SolProvenanceId *local_origins = capability
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
        return SOL_PROVENANCE_NONE;
    }
    if (expression->kind == SOL_EXPR_HANDLE) {
        SolExprId body = expression->as.handle.body;
        return body < checker->types->expression_count
            ? expression_origins[body]
            : SOL_PROVENANCE_NONE;
    }
    if (expression->kind == SOL_EXPR_BLOCK) {
        return sol_effect_block_origin(checker, expression, expression_origins);
    }
    if (expression->kind == SOL_EXPR_IF || expression->kind == SOL_EXPR_MATCH) {
        return sol_effect_join_expression_origin(checker, expression, expression_origins);
    }
    if (expression->kind == SOL_EXPR_OLD) {
        return expression_origins[expression->as.old_expression];
    }
    if (capability && expression->kind == SOL_EXPR_RESULT) {
        return sol_effect_singleton_provenance(
            checker,
            sol_effect_contract_result_root(checker, expression_id)
        );
    }
    return SOL_PROVENANCE_NONE;
}

static bool sol_effect_origin_matches_type(
    const SolEffectChecker *checker,
    SolProvenanceId origin,
    SolType value_type,
    SolDefId owner,
    bool operation
) {
    if (origin == SOL_PROVENANCE_NONE) return true;
    SolProvenance provenance;
    if (owner == SOL_AST_NONE
        || !sol_type_provenance(checker->types, origin, &provenance)) return false;
    for (size_t index = 0; index < provenance.count; ++index) {
        SolParameterId root = provenance.roots[index];
        if (root >= checker->syntax->parameter_count
            || checker->parameter_owners[root] != owner) return false;
        const SolParameter *parameter = &checker->syntax->parameters[root];
        if (parameter->type_id >= checker->types->declared_type_count) return false;
        SolType parameter_type = checker->types->declared_types[parameter->type_id];
        if (!sol_effect_type_is_capability(checker, parameter_type)) return false;
    }
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
    } else if (expression->kind == SOL_EXPR_OLD) {
        sol_effect_push_provenance_dependency(
            stack,
            stack_count,
            expression->as.old_expression
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
        SolProvenanceId actual = capability
            ? checker->types->expression_capability_origins[node]
            : checker->types->expression_operation_origins[node];
        SolProvenanceId expected = sol_effect_expected_expression_origin(
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
    SolProvenanceId actual = capability
        ? checker->types->local_capability_origins[local_id]
        : checker->types->local_operation_origins[local_id];
    SolProvenanceId expected = SOL_PROVENANCE_NONE;
    if (sol_effect_provenance_node_relevant(checker, node, capability)) {
        if (capability && local->kind == SOL_LOCAL_PARAMETER) {
            expected = sol_effect_singleton_provenance(checker, local->syntax_id);
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
    return kind == SOL_TYPE_NOMINAL || kind == SOL_TYPE_APPLICATION
        || kind == SOL_TYPE_FUNCTION || kind == SOL_TYPE_FUNCTION_SIGNATURE
        || kind == SOL_TYPE_CAPABILITY_OPERATION || kind == SOL_TYPE_VARIANT
        || kind == SOL_TYPE_PARAMETER || kind == SOL_TYPE_SELF
        || kind == SOL_TYPE_TRAIT_METHOD;
}

static bool sol_effect_semantic_type_equal(SolType left, SolType right) {
    return left.kind == right.kind
        && (!sol_effect_type_has_identity(left.kind) || left.definition == right.definition);
}

static bool sol_effect_closed_implementation_target(
    const SolEffectChecker *checker,
    SolType type,
    size_t depth
) {
    if (depth >= 256) return false;
    if (type.kind == SOL_TYPE_INT64 || type.kind == SOL_TYPE_BOOL
        || type.kind == SOL_TYPE_TEXT || type.kind == SOL_TYPE_UNIT) return true;
    if (type.kind == SOL_TYPE_NOMINAL) {
        if (type.definition >= checker->syntax->item_count
            || (checker->syntax->items[type.definition].kind != SOL_ITEM_RECORD
                && checker->syntax->items[type.definition].kind != SOL_ITEM_ENUM
                && checker->syntax->items[type.definition].kind != SOL_ITEM_TYPE)) return false;
        return checker->syntax->items[type.definition].first_type_parameter == SOL_AST_NONE
            && (checker->syntax->items[type.definition].kind == SOL_ITEM_RECORD
                || checker->syntax->items[type.definition].kind == SOL_ITEM_ENUM
                || checker->syntax->items[type.definition].kind == SOL_ITEM_TYPE);
    }
    if (type.kind != SOL_TYPE_APPLICATION) return false;
    const SolTypeApplication *application = sol_type_application(checker->types, type);
    const SolType *arguments = NULL;
    size_t count = 0;
    if (application == NULL || application->constructor != SOL_TYPE_CONSTRUCTOR_USER
        || !sol_type_application_arguments(checker->types, type, &arguments, &count)) {
        return false;
    }
    for (size_t index = 0; index < count; ++index) {
        if (!sol_effect_closed_implementation_target(
            checker, arguments[index], depth + 1
        )) return false;
    }
    return true;
}

static bool sol_effect_trait_method_reachable(
    const SolEffectChecker *checker,
    SolDefId owner,
    SolTraitMethodId target
) {
    if (owner >= checker->syntax->item_count) return false;
    SolTraitMethodId method = checker->syntax->items[owner].first_trait_method;
    size_t traversed = 0;
    while (method != SOL_AST_NONE) {
        if (method >= checker->syntax->trait_method_count
            || traversed++ >= checker->syntax->trait_method_count) return false;
        if (method == target) return true;
        method = checker->syntax->trait_methods[method].next;
    }
    return false;
}

static bool sol_effect_method_resolution_valid(
    const SolEffectChecker *checker,
    SolExprId expression,
    const SolMethodResolution *resolution
) {
    const SolSyntaxTree *syntax = checker->syntax;
    const SolTypeTable *types = checker->types;
    if (resolution->call != expression || expression >= syntax->expression_count
        || syntax->expressions[expression].kind != SOL_EXPR_CALL) return false;
    SolExprId callee_id = syntax->expressions[expression].as.call.callee;
    if (callee_id >= syntax->expression_count
        || syntax->expressions[callee_id].kind != SOL_EXPR_FIELD
        || callee_id >= types->expression_count
        || types->expressions[callee_id].kind != SOL_TYPE_TRAIT_METHOD
        || types->expressions[callee_id].definition != resolution->method
        || resolution->trait >= syntax->item_count
        || syntax->items[resolution->trait].kind != SOL_ITEM_TRAIT
        || resolution->requirement >= syntax->trait_method_count
        || resolution->method >= syntax->trait_method_count
        || !sol_effect_trait_method_reachable(
            checker, resolution->trait, resolution->requirement
        )) return false;
    SolSpan field_name = syntax->expressions[callee_id].as.field.name;
    const SolTraitMethod *requirement
        = &syntax->trait_methods[resolution->requirement];
    const SolTraitMethod *method = &syntax->trait_methods[resolution->method];
    if (!sol_effect_span_equal(checker->source, requirement->name, field_name)
        || !sol_effect_span_equal(checker->source, method->name, field_name)
        || !sol_effect_span_equal(checker->source, requirement->name, method->name)) {
        return false;
    }
    SolExprId receiver_id = syntax->expressions[callee_id].as.field.base;
    if (receiver_id >= types->expression_count) return false;
    SolType receiver = types->expressions[receiver_id];
    if (resolution->kind == SOL_METHOD_RESOLUTION_REQUIREMENT) {
        if (resolution->implementation != SOL_AST_NONE
            || resolution->method != resolution->requirement
            || receiver.kind != SOL_TYPE_PARAMETER
            || receiver.definition >= checker->hir->bound_resolution_count) return false;
        SolResolution bound = checker->hir->bound_resolutions[receiver.definition];
        return bound.kind == SOL_RESOLUTION_DEFINITION
            && bound.target == resolution->trait;
    }
    if (resolution->kind != SOL_METHOD_RESOLUTION_IMPLEMENTATION
        || resolution->implementation >= syntax->item_count
        || syntax->items[resolution->implementation].kind != SOL_ITEM_IMPLEMENTATION
        || !sol_effect_trait_method_reachable(
            checker, resolution->implementation, resolution->method
        ) || resolution->implementation >= checker->hir->trait_resolution_count
        || checker->hir->trait_resolutions[resolution->implementation].kind
            != SOL_RESOLUTION_DEFINITION
        || checker->hir->trait_resolutions[resolution->implementation].target
            != resolution->trait
        || resolution->implementation >= types->implementation_target_count) return false;
    SolType target = types->implementation_targets[resolution->implementation];
    return target.kind == receiver.kind && target.definition == receiver.definition;
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
            ] != sol_effect_singleton_provenance(checker, handler->root)
            || checker->types->expression_capability_origins[
                expression->as.handle.provider
            ] == SOL_PROVENANCE_NONE
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

static bool sol_effect_construction_argument_matches(
    const SolEffectChecker *checker,
    SolExprId expression,
    SolType expected
) {
    SolType actual = checker->types->expressions[expression];
    if (sol_effect_semantic_type_equal(expected, actual)) return true;
    for (size_t index = 0; index < checker->types->function_coercion_count; ++index) {
        const SolFunctionCoercion *coercion
            = &checker->types->function_coercions[index];
        if (coercion->expression == expression
            && sol_effect_semantic_type_equal(coercion->expected, expected)
            && sol_effect_coercion_shape_matches(checker, coercion, actual)) return true;
    }
    return false;
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
        || syntax->type_count > syntax->type_capacity
        || syntax->type_argument_count > syntax->type_argument_capacity
        || syntax->type_parameter_count > syntax->type_parameter_capacity
        || syntax->effect_parameter_count > syntax->effect_parameter_capacity
        || syntax->match_arm_count > syntax->match_arm_capacity
        || syntax->effect_count > syntax->effect_capacity
        || syntax->capability_member_count > syntax->capability_member_capacity
        || syntax->trait_method_count > syntax->trait_method_capacity
        || syntax->contract_clause_count > syntax->contract_clause_capacity
        || syntax->contract_condition_count > syntax->contract_condition_capacity
        || (syntax->item_count != 0 && syntax->items == NULL)
        || (syntax->expression_count != 0 && syntax->expressions == NULL)
        || (syntax->parameter_count != 0 && syntax->parameters == NULL)
        || (syntax->argument_count != 0 && syntax->arguments == NULL)
        || (syntax->statement_count != 0 && syntax->statements == NULL)
        || (syntax->type_count != 0 && syntax->types == NULL)
        || (syntax->type_argument_count != 0 && syntax->type_arguments == NULL)
        || (syntax->type_parameter_count != 0 && syntax->type_parameters == NULL)
        || (syntax->effect_parameter_count != 0 && syntax->effect_parameters == NULL)
        || (syntax->match_arm_count != 0 && syntax->match_arms == NULL)
        || (syntax->effect_count != 0 && syntax->effects == NULL)
        || (syntax->capability_member_count != 0 && syntax->capability_members == NULL)
        || (syntax->trait_method_count != 0 && syntax->trait_methods == NULL)
        || (syntax->contract_clause_count != 0 && syntax->contract_clauses == NULL)
        || (syntax->contract_condition_count != 0
            && syntax->contract_conditions == NULL)
        || hir->definition_count != syntax->item_count
        || hir->resolution_count != syntax->expression_count
        || hir->type_resolution_count != syntax->type_count
        || hir->effect_resolution_count != syntax->effect_count
        || hir->type_effect_resolution_count != syntax->type_count
        || hir->trait_resolution_count != syntax->item_count
        || hir->bound_resolution_count != syntax->type_parameter_count
        || hir->semantic_reference_count > hir->semantic_reference_capacity
        || hir->semantic_reference_capacity > SIZE_MAX / sizeof(*hir->semantic_references)
        || hir->import_resolution_count
            != (hir->file_scope_count == 0 ? 0 : syntax->import_count)
        || hir->local_count > hir->local_capacity
        || hir->file_scope_count > SIZE_MAX / sizeof(*hir->file_scopes)
        || ((hir->file_scope_count == 0) != (hir->file_scopes == NULL))
        || (hir->file_scope_count == 0 && hir->item_files != NULL)
        || (hir->file_scope_count != 0 && hir->definition_count != 0
            && hir->item_files == NULL)
        || (hir->definition_count != 0 && hir->definitions == NULL)
        || (hir->resolution_count != 0 && hir->resolutions == NULL)
        || (hir->resolution_count != 0 && hir->expression_owners == NULL)
        || (hir->type_resolution_count != 0 && hir->type_resolutions == NULL)
        || (hir->effect_resolution_count != 0 && hir->effect_resolutions == NULL)
        || (hir->type_effect_resolution_count != 0
            && hir->type_effect_resolutions == NULL)
        || (hir->trait_resolution_count != 0 && hir->trait_resolutions == NULL)
        || (hir->bound_resolution_count != 0 && hir->bound_resolutions == NULL)
        || (hir->semantic_reference_count != 0 && hir->semantic_references == NULL)
        || (hir->import_resolution_count != 0 && hir->import_resolutions == NULL)
        || (hir->local_count != 0 && hir->locals == NULL)
        || types->expression_count != syntax->expression_count
        || types->local_count != hir->local_count
        || types->definition_count != hir->definition_count
        || types->declared_type_count != syntax->type_count
        || types->type_application_count > types->type_application_capacity
        || types->type_application_argument_count
            > types->type_application_argument_capacity
        || types->function_type_count > types->function_type_capacity
        || types->function_coercion_count > types->function_coercion_capacity
        || types->provenance_count > types->provenance_capacity
        || types->provenance_root_count > types->provenance_root_capacity
        || types->provenance_root_count > SIZE_MAX / sizeof(*types->provenance_roots)
        || ((types->provenance_count == 0) != (types->provenance_capacity == 0))
        || ((types->provenance_root_count == 0)
            != (types->provenance_root_capacity == 0))
        || types->handler_count != syntax->expression_count
        || types->call_instantiation_count != syntax->expression_count
        || types->call_instantiation_argument_count
            > types->call_instantiation_argument_capacity
        || types->variant_constructor_count > types->variant_constructor_capacity
        || types->method_resolution_count != syntax->expression_count
        || types->implementation_target_count != syntax->item_count
        || types->representation_count != syntax->item_count
        || types->construction_count != syntax->expression_count
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
        || (types->type_application_capacity != 0 && types->type_applications == NULL)
        || (types->type_application_argument_capacity != 0
            && types->type_application_arguments == NULL)
        || (types->function_type_capacity != 0 && types->function_types == NULL)
        || (types->function_coercion_capacity != 0
            && types->function_coercions == NULL)
        || (types->provenance_capacity != 0 && types->provenances == NULL)
        || (types->provenance_root_capacity != 0 && types->provenance_roots == NULL)
        || (types->handler_count != 0 && types->handlers == NULL)
        || (types->call_instantiation_count != 0
            && types->call_instantiations == NULL)
        || (types->call_instantiation_argument_capacity != 0
            && types->call_instantiation_arguments == NULL)
        || (types->variant_constructor_capacity != 0
            && types->variant_constructors == NULL)
        || (types->method_resolution_count != 0 && types->method_resolutions == NULL)
        || (types->implementation_target_count != 0
            && types->implementation_targets == NULL)
        || (types->representation_count != 0 && types->representations == NULL)
        || (types->construction_count != 0 && types->constructions == NULL)) {
        return false;
    }
    if (!sol_syntax_contracts_validate(source, syntax)) return false;
    size_t provenance_root_offset = 0;
    for (size_t index = 0; index < types->provenance_count; ++index) {
        SolProvenance provenance;
        const SolProvenanceSet *set = &types->provenances[index];
        if (set->root_offset != provenance_root_offset
            || !sol_type_provenance(types, index, &provenance)) return false;
        for (size_t root = 0; root < provenance.count; ++root) {
            if (provenance.roots[root] >= syntax->parameter_count
                || (root != 0 && provenance.roots[root - 1] >= provenance.roots[root])) {
                return false;
            }
        }
        for (size_t previous = 0; previous < index; ++previous) {
            SolProvenance other;
            if (!sol_type_provenance(types, previous, &other)) return false;
            if (other.count == provenance.count
                && memcmp(
                    other.roots,
                    provenance.roots,
                    provenance.count * sizeof(*provenance.roots)
                ) == 0) return false;
        }
        provenance_root_offset += provenance.count;
    }
    if (provenance_root_offset != types->provenance_root_count) return false;
    size_t application_argument_offset = 0;
    for (size_t index = 0; index < types->type_application_count; ++index) {
        const SolTypeApplication *application = &types->type_applications[index];
        const SolType *arguments = NULL;
        size_t argument_count = 0;
        size_t expected = application->constructor == SOL_TYPE_CONSTRUCTOR_OPTION
            ? 1
            : application->constructor == SOL_TYPE_CONSTRUCTOR_RESULT ? 2 : 0;
        if (application->constructor == SOL_TYPE_CONSTRUCTOR_USER
            && application->definition < syntax->item_count) {
            SolTypeParameterId parameter
                = syntax->items[application->definition].first_type_parameter;
            while (parameter != SOL_AST_NONE) {
                if (parameter >= syntax->type_parameter_count
                    || expected++ >= syntax->type_parameter_count
                    || syntax->type_parameters[parameter].owner_item
                        != application->definition) return false;
                parameter = syntax->type_parameters[parameter].next;
            }
        }
        if (application->argument_count == 0
            || application->argument_count != expected
            || application->argument_offset != application_argument_offset
            || !sol_type_application_arguments(
                types,
                (SolType){SOL_TYPE_APPLICATION, index},
                &arguments,
                &argument_count
            )
            || argument_count != expected
            || (application->constructor != SOL_TYPE_CONSTRUCTOR_USER
                && application->definition != SOL_AST_NONE)
            || (application->constructor == SOL_TYPE_CONSTRUCTOR_USER
                && (application->definition >= syntax->item_count
                    || (syntax->items[application->definition].kind != SOL_ITEM_RECORD
                        && syntax->items[application->definition].kind != SOL_ITEM_ENUM
                        && syntax->items[application->definition].kind != SOL_ITEM_TYPE)))) {
            return false;
        }
        for (size_t argument = 0; argument < application->argument_count; ++argument) {
            SolType type = arguments[argument];
            if (!sol_type_exact_reference_valid(syntax, types, type)
                || (type.kind == SOL_TYPE_APPLICATION && type.definition >= index)
                ) {
                return false;
            }
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const SolTypeApplication *other = &types->type_applications[previous];
            const SolType *other_arguments = NULL;
            size_t other_count = 0;
            if (!sol_type_application_arguments(
                types,
                (SolType){SOL_TYPE_APPLICATION, previous},
                &other_arguments,
                &other_count
            )) return false;
            bool equal = application->constructor == other->constructor
                && application->definition == other->definition
                && argument_count == other_count;
            for (size_t argument = 0; equal && argument < application->argument_count;
                ++argument) {
                equal = sol_effect_semantic_type_equal(
                    arguments[argument],
                    other_arguments[argument]
                );
            }
            if (equal) return false;
        }
        application_argument_offset += argument_count;
    }
    if (application_argument_offset != types->type_application_argument_count) return false;
    for (SolDefId definition = 0; definition < types->representation_count; ++definition) {
        const SolSyntaxItem *item = &syntax->items[definition];
        const SolTypeRepresentation *representation = &types->representations[definition];
        if (item->kind == SOL_ITEM_TYPE) {
            if (item->representation_type >= types->declared_type_count
                || representation->flavor != item->flavor
                || !sol_effect_semantic_type_equal(
                    representation->representation,
                    types->declared_types[item->representation_type]
                )
                || !sol_type_exact_reference_valid(
                    syntax, types, representation->representation
                )) return false;
        } else if (representation->flavor != SOL_TYPE_DECLARATION_NONE
            || representation->representation.kind != SOL_TYPE_UNKNOWN
            || representation->representation.definition != 0) {
            return false;
        }
    }
    for (SolExprId expression = 0; expression < types->construction_count; ++expression) {
        const SolTypeConstruction *construction = &types->constructions[expression];
        if (construction->definition == SOL_AST_NONE) {
            if (construction->representation.kind != SOL_TYPE_UNKNOWN
                || construction->representation.definition != 0
                || construction->result.kind != SOL_TYPE_UNKNOWN
                || construction->result.definition != 0) return false;
            continue;
        }
        if (construction->definition >= syntax->item_count
            || syntax->items[construction->definition].kind != SOL_ITEM_TYPE
            || syntax->items[construction->definition].flavor
                != SOL_TYPE_DECLARATION_DISTINCT
            || syntax->expressions[expression].kind != SOL_EXPR_CALL
            || !sol_type_exact_reference_valid(syntax, types, construction->representation)
            || !sol_type_exact_reference_valid(syntax, types, construction->result)
            || !sol_effect_semantic_type_equal(
                construction->result, types->expressions[expression]
            )
            || sol_type_construction(types, expression) == NULL) return false;
        SolArgumentId argument = syntax->expressions[expression].as.call.first_argument;
        if (argument >= syntax->argument_count || syntax->arguments[argument].is_named
            || syntax->arguments[argument].next != SOL_AST_NONE
            || !sol_effect_construction_argument_matches(
                checker,
                syntax->arguments[argument].value,
                construction->representation
            )) return false;
        SolExprId callee = syntax->expressions[expression].as.call.callee;
        SolType callee_type = types->expressions[callee];
        const SolTypeApplication *callee_application
            = sol_type_application(types, callee_type);
        SolDefId callee_definition = callee_type.kind == SOL_TYPE_NOMINAL
            ? callee_type.definition
            : callee_application != NULL
                    && callee_application->constructor == SOL_TYPE_CONSTRUCTOR_USER
                ? callee_application->definition
                : SOL_AST_NONE;
        if (callee_definition != construction->definition) return false;
    }
    size_t call_argument_total = 0;
    for (size_t expression = 0; expression < types->call_instantiation_count; ++expression) {
        const SolCallInstantiation *instantiation
            = &types->call_instantiations[expression];
        if (instantiation->function == SOL_AST_NONE) {
            if (instantiation->argument_count != 0 || instantiation->argument_offset != 0) {
                return false;
            }
            continue;
        }
        if (syntax->expressions[expression].kind != SOL_EXPR_CALL
            || instantiation->function >= syntax->item_count
            || syntax->items[instantiation->function].kind != SOL_ITEM_FUNCTION) {
            return false;
        }
        size_t expected = 0;
        SolTypeParameterId parameter
            = syntax->items[instantiation->function].first_type_parameter;
        while (parameter != SOL_AST_NONE) {
            if (parameter >= syntax->type_parameter_count
                || expected++ >= syntax->type_parameter_count) return false;
            parameter = syntax->type_parameters[parameter].next;
        }
        const SolType *arguments = NULL;
        size_t argument_count = 0;
        SolExprId callee = syntax->expressions[expression].as.call.callee;
        if (expected == 0 || expected != instantiation->argument_count
            || callee >= types->expression_count
            || types->expressions[callee].kind != SOL_TYPE_FUNCTION
            || types->expressions[callee].definition != instantiation->function
            || !sol_type_call_instantiation_arguments(
                types,
                expression,
                &arguments,
                &argument_count
            )
            || argument_count != expected
            || !sol_type_call_instantiation_valid(
                checker->source,
                syntax,
                types,
                expression
            )) return false;
        if (call_argument_total > SIZE_MAX - argument_count) return false;
        call_argument_total += argument_count;
        for (size_t index = 0; index < expected; ++index) {
            if (!sol_type_exact_reference_valid(syntax, types, arguments[index])) {
                return false;
            }
        }
        for (size_t previous = 0; previous < expression; ++previous) {
            const SolCallInstantiation *other = &types->call_instantiations[previous];
            if (other->function == SOL_AST_NONE) continue;
            size_t left_end = instantiation->argument_offset + argument_count;
            size_t right_end = other->argument_offset + other->argument_count;
            if (instantiation->argument_offset < right_end
                && other->argument_offset < left_end) return false;
        }
    }
    if (call_argument_total != types->call_instantiation_argument_count) return false;
    for (size_t index = 0; index < types->variant_constructor_count; ++index) {
        const SolVariantConstructor *constructor = &types->variant_constructors[index];
        if (constructor->variant >= syntax->variant_count
            || !sol_type_exact_reference_valid(syntax, types, constructor->owner)) {
            return false;
        }
        SolDefId owner = syntax->variants[constructor->variant].owner_item;
        bool owner_matches = constructor->owner.kind == SOL_TYPE_NOMINAL
            && constructor->owner.definition == owner;
        const SolTypeApplication *application = sol_type_application(
            types,
            constructor->owner
        );
        owner_matches = owner_matches
            || (application != NULL
                && application->constructor == SOL_TYPE_CONSTRUCTOR_USER
                && application->definition == owner);
        if (!owner_matches || owner >= syntax->item_count
            || syntax->items[owner].kind != SOL_ITEM_ENUM
            || syntax->variants[constructor->variant].first_field == SOL_AST_NONE) {
            return false;
        }
        for (size_t previous = 0; previous < index; ++previous) {
            const SolVariantConstructor *other = &types->variant_constructors[previous];
            if (other->variant == constructor->variant
                && sol_effect_semantic_type_equal(other->owner, constructor->owner)) {
                return false;
            }
        }
    }
    for (size_t index = 0; index < types->function_type_count; ++index) {
        const SolFunctionType *function = &types->function_types[index];
        if ((function->parameter_count != 0 && function->parameters == NULL)
            || (function->effects.count != 0 && function->effects.atoms == NULL)
            || !sol_type_exact_reference_valid(syntax, types, function->result)
            || (function->result.kind == SOL_TYPE_FUNCTION_SIGNATURE
                && function->result.definition >= index)
            || (function->effect_parameter != SOL_AST_NONE
                && function->effect_parameter >= syntax->effect_parameter_count)
            ) {
                return false;
            }
        for (size_t parameter = 0; parameter < function->parameter_count; ++parameter) {
            SolType type = function->parameters[parameter];
            if (!sol_type_exact_reference_valid(syntax, types, type)
                || (type.kind == SOL_TYPE_FUNCTION_SIGNATURE && type.definition >= index)
                ) {
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
            if (!sol_effect_authority_free_semantic_atom(checker, effect)
                || !sol_effect_function_atom_matches_syntax(checker, effect)) {
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
                && function->effects.count == other->effects.count
                && function->effect_parameter == other->effect_parameter;
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
    for (SolTypeId type_id = 0; type_id < syntax->type_count; ++type_id) {
        if (!sol_effect_function_type_matches_syntax(checker, type_id)) return false;
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
            bool recovery = type.kind == SOL_TYPE_UNKNOWN || type.kind == SOL_TYPE_ERROR
                || type.kind == SOL_TYPE_NEVER;
            if ((recovery && type.definition != 0)
                || (!recovery && !sol_type_exact_reference_valid(syntax, types, type))) {
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
    for (SolExprId expression = 0; expression < types->method_resolution_count; ++expression) {
        const SolMethodResolution *method = &types->method_resolutions[expression];
        if (method->kind == SOL_METHOD_RESOLUTION_NONE) {
            if (method->call != SOL_AST_NONE || method->trait != SOL_AST_NONE
                || method->requirement != SOL_AST_NONE
                || method->implementation != SOL_AST_NONE
                || method->method != SOL_AST_NONE) return false;
            continue;
        }
        if (!sol_effect_method_resolution_valid(checker, expression, method)) return false;
    }
    for (SolDefId item = 0; item < types->implementation_target_count; ++item) {
        SolType target = types->implementation_targets[item];
        if (syntax->items[item].kind == SOL_ITEM_IMPLEMENTATION) {
            if (!sol_type_exact_reference_valid(syntax, types, target)
                || !sol_effect_closed_implementation_target(checker, target, 0)) return false;
        } else if (target.kind != SOL_TYPE_UNKNOWN || target.definition != 0) {
            return false;
        }
    }
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if ((int)item->kind < 0 || item->kind > SOL_ITEM_IMPLEMENTATION
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
                && item->result_authority_parameter >= syntax->parameter_count)
            || (item->first_trait_method != SOL_AST_NONE
                && item->first_trait_method >= syntax->trait_method_count)) {
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
        } else if (item->kind == SOL_ITEM_CAPABILITY) {
            if (item->capability_source != SOL_AST_NONE) {
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
            }
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
        } else if (item->kind == SOL_ITEM_TRAIT
            || item->kind == SOL_ITEM_IMPLEMENTATION) {
            SolTraitMethodId method = item->first_trait_method;
            size_t method_count = 0;
            while (method != SOL_AST_NONE) {
                if (method >= syntax->trait_method_count
                    || method_count++ >= syntax->trait_method_count) return false;
                SolParameterId parameter = syntax->trait_methods[method].first_parameter;
                size_t parameter_count = 0;
                while (parameter != SOL_AST_NONE) {
                    if (parameter >= syntax->parameter_count
                        || parameter_count++ >= syntax->parameter_count
                        || checker->parameter_owners[parameter] != SOL_AST_NONE) return false;
                    checker->parameter_owners[parameter] = owner;
                    parameter = syntax->parameters[parameter].next;
                }
                method = syntax->trait_methods[method].next;
            }
        }
    }
    for (size_t index = 0; index < types->provenance_count; ++index) {
        SolProvenance provenance;
        if (!sol_type_provenance(types, index, &provenance)) return false;
        SolDefId owner = checker->parameter_owners[provenance.roots[0]];
        if (owner == SOL_AST_NONE) return false;
        for (size_t root = 0; root < provenance.count; ++root) {
            SolParameterId parameter = provenance.roots[root];
            if (checker->parameter_owners[parameter] != owner
                || !sol_effect_parameter_is_capability(checker, parameter)) return false;
        }
    }
    if (!sol_effect_validate_expression_arena(checker)) return false;
    for (size_t index = 0; index < hir->local_count; ++index) {
        const SolHirLocal *local = &hir->locals[index];
        if ((int)local->kind < 0 || local->kind > SOL_LOCAL_PATTERN
            || !sol_effect_span_valid(source, local->name)
            || local->owner >= syntax->item_count
            || (syntax->items[local->owner].kind != SOL_ITEM_FUNCTION
                && syntax->items[local->owner].kind != SOL_ITEM_TYPE
                && syntax->items[local->owner].kind != SOL_ITEM_CAPABILITY
                && syntax->items[local->owner].kind != SOL_ITEM_TRAIT
                && syntax->items[local->owner].kind != SOL_ITEM_IMPLEMENTATION)
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
        SolEffectResolution resolution = hir->effect_resolutions[index];
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
            || (effect->owner_kind == SOL_EFFECT_OWNER_TRAIT_METHOD
                && effect->owner >= syntax->trait_method_count)
            || (effect->owner_kind == SOL_EFFECT_OWNER_TYPE
                && effect->owner >= syntax->type_count)
            || (int)resolution.kind < 0
            || resolution.kind > SOL_EFFECT_RESOLUTION_ERROR
            || (resolution.kind == SOL_EFFECT_RESOLUTION_PARAMETER
                && (resolution.target >= syntax->effect_parameter_count
                    || syntax->effect_parameters[resolution.target].owner_item
                        != (effect->owner_kind == SOL_EFFECT_OWNER_ITEM
                            ? effect->owner
                            : effect->owner_kind == SOL_EFFECT_OWNER_TYPE
                                ? syntax->types[effect->owner].owner_item
                                : SOL_AST_NONE)))
            || (resolution.kind != SOL_EFFECT_RESOLUTION_PARAMETER
                && resolution.target != SOL_AST_NONE)) {
            return false;
        }
        bool zero_argument = effect->argument.start == 0 && effect->argument.end == 0;
        bool pure_spelling = sol_effect_span_text_equal(source, effect->name, "pure");
        if (effect->name.start == effect->name.end
            || effect->span.start != effect->name.start
            || (effect->has_argument
                ? (effect->argument.start == effect->argument.end
                    || effect->name.end >= effect->argument.start
                    || effect->argument.end >= effect->span.end)
                : (!zero_argument || effect->span.end != effect->name.end))
            || effect->is_pure != pure_spelling
            ) return false;
    }
    for (size_t index = 0; index < syntax->type_count; ++index) {
        SolEffectResolution resolution = hir->type_effect_resolutions[index];
        const SolSyntaxType *type = &syntax->types[index];
        if ((int)resolution.kind < 0 || resolution.kind > SOL_EFFECT_RESOLUTION_ERROR
            || (type->has_effect_tail
                != (resolution.kind == SOL_EFFECT_RESOLUTION_PARAMETER))
            || (resolution.kind == SOL_EFFECT_RESOLUTION_PARAMETER
                && (resolution.target >= syntax->effect_parameter_count
                    || syntax->effect_parameters[resolution.target].owner_item
                        != type->owner_item))
            || (resolution.kind != SOL_EFFECT_RESOLUTION_PARAMETER
                && resolution.target != SOL_AST_NONE)) return false;
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
    size_t trait_method_count = checker->syntax->trait_method_count;
    SolEffectRow *functions = calloc(function_count, sizeof(*functions));
    SolEffectRow *members = calloc(member_count, sizeof(*members));
    SolEffectRow *trait_methods = calloc(trait_method_count, sizeof(*trait_methods));
    if ((function_count != 0 && functions == NULL)
        || (member_count != 0 && members == NULL)
        || (trait_method_count != 0 && trait_methods == NULL)) {
        free(functions);
        free(members);
        free(trait_methods);
        checker->allocation_failed = true;
        return false;
    }
    checker->effects->functions = functions;
    checker->effects->function_count = function_count;
    checker->effects->capability_members = members;
    checker->effects->capability_member_count = member_count;
    checker->effects->trait_methods = trait_methods;
    checker->effects->trait_method_count = trait_method_count;
    for (size_t index = 0; index < function_count; ++index) {
        functions[index].effect_parameter = SOL_AST_NONE;
    }
    for (size_t index = 0; index < member_count; ++index) {
        members[index].effect_parameter = SOL_AST_NONE;
    }
    for (size_t index = 0; index < trait_method_count; ++index) {
        trait_methods[index].effect_parameter = SOL_AST_NONE;
    }
    checker->effects->call_instantiations = calloc(
        checker->syntax->expression_count,
        sizeof(*checker->effects->call_instantiations)
    );
    if (checker->syntax->expression_count != 0
        && checker->effects->call_instantiations == NULL) {
        checker->allocation_failed = true;
        return false;
    }
    checker->effects->call_instantiation_count = checker->syntax->expression_count;
    checker->effects->call_instantiation_capacity = checker->syntax->expression_count;
    for (size_t index = 0; index < checker->syntax->expression_count; ++index) {
        checker->effects->call_instantiations[index].call = SOL_AST_NONE;
        checker->effects->call_instantiations[index].function = SOL_AST_NONE;
        checker->effects->call_instantiations[index].parameter = SOL_AST_NONE;
    }
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
        const SolFunctionType *expected_function = &checker->types->function_types[
            coercion->expected.definition
        ];
        const SolEffectSet *expected = &expected_function->effects;
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
            if (!found && expected_function->effect_parameter == SOL_AST_NONE
                && !reported) {
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

static bool sol_effect_type_uses_parameter(
    const SolEffectChecker *checker,
    SolType type,
    SolEffectParameterId parameter,
    size_t depth
) {
    if (depth >= 256) return true;
    if (type.kind == SOL_TYPE_FUNCTION_SIGNATURE) {
        if (type.definition >= checker->types->function_type_count) return true;
        const SolFunctionType *function = &checker->types->function_types[type.definition];
        if (function->effect_parameter == parameter
            || sol_effect_type_uses_parameter(
                checker, function->result, parameter, depth + 1
            )) return true;
        for (size_t index = 0; index < function->parameter_count; ++index) {
            if (sol_effect_type_uses_parameter(
                checker, function->parameters[index], parameter, depth + 1
            )) return true;
        }
        return false;
    }
    if (type.kind != SOL_TYPE_APPLICATION) return false;
    const SolType *arguments = NULL;
    size_t count = 0;
    if (!sol_type_application_arguments(
        checker->types, type, &arguments, &count
    )) return true;
    for (size_t index = 0; index < count; ++index) {
        if (sol_effect_type_uses_parameter(
            checker, arguments[index], parameter, depth + 1
        )) return true;
    }
    return false;
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
        .current_trait_method = SOL_AST_NONE,
    };
    bool valid_inputs = effects->functions == NULL && effects->function_count == 0
        && effects->capability_members == NULL && effects->capability_member_count == 0
        && effects->trait_methods == NULL && effects->trait_method_count == 0
        && effects->call_instantiations == NULL
        && effects->call_instantiation_count == 0
        && effects->call_instantiation_capacity == 0
        && effects->call_arguments == NULL && effects->call_argument_count == 0
        && effects->call_argument_capacity == 0
        && effects->call_rows == NULL && effects->call_row_count == 0
        && effects->call_row_capacity == 0
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
            if (item->first_effect_parameter != SOL_AST_NONE) {
                SolEffectParameterId effect_parameter = item->first_effect_parameter;
                bool input_determined = false;
                SolParameterId parameter = item->first_parameter;
                while (parameter != SOL_AST_NONE) {
                    SolType type = types->declared_types[
                        syntax->parameters[parameter].type_id
                    ];
                    if (type.kind == SOL_TYPE_FUNCTION_SIGNATURE
                        && type.definition < types->function_type_count
                        && types->function_types[type.definition].effect_parameter
                            == effect_parameter) input_determined = true;
                    parameter = syntax->parameters[parameter].next;
                }
                bool output_use = sol_effect_type_uses_parameter(
                    &checker,
                    types->definitions[index],
                    effect_parameter,
                    0
                );
                if (!item->has_effect_clause || row->effect_parameter != effect_parameter) {
                    sol_effect_error(
                        &checker,
                        "SOL-EFFECT-008",
                        syntax->effect_parameters[effect_parameter].name,
                        "an effect parameter must occur in its owning function's explicit row"
                    );
                }
                if (!input_determined || output_use) {
                    sol_effect_error(
                        &checker,
                        "SOL-EFFECT-008",
                        syntax->effect_parameters[effect_parameter].name,
                        output_use
                            ? "an effect parameter cannot occur in a result type"
                            : "an effect parameter must be determined by a callback parameter"
                    );
                }
            } else if (row->effect_parameter != SOL_AST_NONE) {
                checker.malformed = true;
            }
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
        for (size_t index = 0; index < syntax->trait_method_count; ++index) {
            const SolTraitMethod *method = &syntax->trait_methods[index];
            SolEffectRow *row = &effects->trait_methods[index];
            sol_effect_validate_and_normalize_row(
                &checker,
                method->first_effect,
                method->span,
                SOL_EFFECT_OWNER_TRAIT_METHOD,
                index,
                method->first_parameter,
                false,
                row
            );
            if (!method->has_effect_clause) {
                sol_effect_error(&checker, "SOL-EFFECT-005", method->span,
                    "trait and implementation methods require explicit effects");
            }
            SolEffectId syntax_effect = method->first_effect;
            while (syntax_effect != SOL_AST_NONE) {
                const SolEffect *entry = &syntax->effects[syntax_effect];
                bool dependent = entry->has_argument
                    && sol_effect_span_text_equal(source, entry->argument, "Self");
                SolParameterId parameter = method->first_parameter;
                while (!dependent && parameter != SOL_AST_NONE) {
                    dependent = entry->has_argument && sol_effect_span_equal(
                        source, entry->argument, syntax->parameters[parameter].name
                    );
                    parameter = syntax->parameters[parameter].next;
                }
                if (dependent) {
                    sol_effect_error(&checker, "SOL-EFFECT-009", entry->span,
                        "trait method effects cannot depend on parameters or Self");
                }
                syntax_effect = entry->next;
            }
            for (size_t atom = 0; atom < row->count; ++atom) {
                if (row->atoms[atom].argument_kind == SOL_EFFECT_ATOM_PARAMETER
                    || row->atoms[atom].argument_kind == SOL_EFFECT_ATOM_SELF) {
                    sol_effect_error(&checker, "SOL-EFFECT-009", row->atoms[atom].span,
                        "trait method effects cannot depend on parameters or Self");
                }
            }
        }
        for (SolDefId item = 0; item < syntax->item_count; ++item) {
            if (syntax->items[item].kind != SOL_ITEM_IMPLEMENTATION
                || hir->trait_resolutions[item].kind != SOL_RESOLUTION_DEFINITION) continue;
            SolDefId trait = hir->trait_resolutions[item].target;
            SolTraitMethodId method = syntax->items[item].first_trait_method;
            while (method != SOL_AST_NONE) {
                SolTraitMethodId requirement = syntax->items[trait].first_trait_method;
                while (requirement != SOL_AST_NONE && !sol_effect_span_equal(
                    source, syntax->trait_methods[requirement].name,
                    syntax->trait_methods[method].name
                )) requirement = syntax->trait_methods[requirement].next;
                if (requirement != SOL_AST_NONE) {
                    const SolEffectRow *actual = &effects->trait_methods[method];
                    const SolEffectRow *expected = &effects->trait_methods[requirement];
                    bool equal = actual->count == expected->count;
                    for (size_t atom = 0; equal && atom < actual->count; ++atom) {
                        equal = sol_effect_row_contains(&checker, expected, &actual->atoms[atom]);
                    }
                    if (!equal) {
                        sol_effect_error(&checker, "SOL-EFFECT-009",
                            syntax->trait_methods[method].span,
                            "implementation method effects must exactly match the trait requirement");
                    }
                }
                method = syntax->trait_methods[method].next;
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
            if (!checker.malformed && !checker.allocation_failed) {
                for (SolExprId expression = 0;
                    expression < syntax->expression_count;
                    ++expression) {
                    if (syntax->expressions[expression].kind != SOL_EXPR_CALL) continue;
                    SolDefId owner = checker.expression_owners[expression];
                    if (owner >= syntax->item_count) {
                        checker.malformed = true;
                        break;
                    }
                    checker.current_function = owner;
                    checker.current_member = SOL_AST_NONE;
                    checker.current_trait_method = SOL_AST_NONE;
                    checker.mode = SOL_EFFECT_WALK_RECORD;
                    checker.handled_count = 0;
                    sol_effect_process_call(&checker, &syntax->expressions[expression]);
                }
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
            for (size_t index = 0; index < syntax->trait_method_count; ++index) {
                if (syntax->trait_methods[index].body != SOL_AST_NONE) {
                    sol_effect_walk_trait_method(&checker, index);
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
