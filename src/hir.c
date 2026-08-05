#include "sol/hir.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SolSpan name;
    SolResolution resolution;
    size_t scope_depth;
} SolBinding;

typedef struct {
    const SolSource *source;
    const SolSyntaxTree *syntax;
    SolHirModule *module;
    SolDiagnostics *diagnostics;
    SolBinding *bindings;
    unsigned char *expression_states;
    size_t binding_count;
    size_t binding_capacity;
    size_t scope_depth;
    size_t traversal_depth;
    SolDefId current_definition;
    bool allocation_failed;
    bool malformed;
    bool traversal_limit_reported;
} SolResolver;

void sol_hir_module_init(SolHirModule *module) {
    memset(module, 0, sizeof(*module));
}

void sol_hir_module_free(SolHirModule *module) {
    free(module->definitions);
    free(module->locals);
    free(module->resolutions);
    memset(module, 0, sizeof(*module));
}

static bool sol_span_equal(const SolSource *source, SolSpan left, SolSpan right) {
    size_t left_length = left.end - left.start;
    size_t right_length = right.end - right.start;
    return left_length == right_length
        && memcmp(source->text + left.start, source->text + right.start, left_length) == 0;
}

static int sol_path_next_byte(const SolSource *source, SolSpan span, size_t *cursor) {
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

static bool sol_path_span_equal(const SolSource *source, SolSpan left, SolSpan right) {
    size_t left_cursor = left.start;
    size_t right_cursor = right.start;
    for (;;) {
        int left_byte = sol_path_next_byte(source, left, &left_cursor);
        int right_byte = sol_path_next_byte(source, right, &right_cursor);
        if (left_byte != right_byte) {
            return false;
        }
        if (left_byte < 0) {
            return true;
        }
    }
}

static bool sol_span_text_equal(const SolSource *source, SolSpan span, const char *text) {
    size_t length = span.end - span.start;
    return strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static bool sol_span_valid(const SolSource *source, SolSpan span) {
    return span.start <= span.end && span.end <= source->length;
}

static void sol_resolver_malformed(SolResolver *resolver) {
    if (!resolver->malformed) {
        sol_diagnostics_add(
            resolver->diagnostics,
            "SOL-INTERNAL-002",
            SOL_SEVERITY_ERROR,
            (SolSpan){0},
            "malformed syntax AST passed to semantic lowering"
        );
    }
    resolver->malformed = true;
}

static bool sol_resolver_validate(SolResolver *resolver) {
    const SolSyntaxTree *syntax = resolver->syntax;
    if (syntax->item_count > syntax->item_capacity
        || syntax->parameter_count > syntax->parameter_capacity
        || syntax->argument_count > syntax->argument_capacity
        || syntax->statement_count > syntax->statement_capacity
        || syntax->expression_count > syntax->expression_capacity
        || (syntax->item_count != 0 && syntax->items == NULL)
        || (syntax->parameter_count != 0 && syntax->parameters == NULL)
        || (syntax->argument_count != 0 && syntax->arguments == NULL)
        || (syntax->statement_count != 0 && syntax->statements == NULL)
        || (syntax->expression_count != 0 && syntax->expressions == NULL)) {
        sol_resolver_malformed(resolver);
        return false;
    }
    for (size_t index = 0; index < syntax->item_count; ++index) {
        const SolSyntaxItem *item = &syntax->items[index];
        if ((int)item->kind < 0 || item->kind > SOL_ITEM_FUNCTION
            || !sol_span_valid(resolver->source, item->name)
            || !sol_span_valid(resolver->source, item->span)
            || (item->body != SOL_AST_NONE && item->body >= syntax->expression_count)
            || (item->first_parameter != SOL_AST_NONE
                && item->first_parameter >= syntax->parameter_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->parameter_count; ++index) {
        const SolParameter *parameter = &syntax->parameters[index];
        if (!sol_span_valid(resolver->source, parameter->name)
            || !sol_span_valid(resolver->source, parameter->type)
            || (parameter->next != SOL_AST_NONE && parameter->next >= syntax->parameter_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->argument_count; ++index) {
        const SolArgument *argument = &syntax->arguments[index];
        if (!sol_span_valid(resolver->source, argument->name)
            || argument->value >= syntax->expression_count
            || (argument->next != SOL_AST_NONE && argument->next >= syntax->argument_count)) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->statement_count; ++index) {
        const SolStatement *statement = &syntax->statements[index];
        if ((int)statement->kind < 0 || statement->kind > SOL_STATEMENT_EXPRESSION) {
            sol_resolver_malformed(resolver);
            return false;
        }
        SolExprId value = statement->kind == SOL_STATEMENT_LET
            ? statement->as.let_statement.value
            : statement->as.expression;
        if (!sol_span_valid(resolver->source, statement->span)
            || value >= syntax->expression_count
            || (statement->next != SOL_AST_NONE && statement->next >= syntax->statement_count)
            || (statement->kind == SOL_STATEMENT_LET
                && !sol_span_valid(resolver->source, statement->as.let_statement.name))) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    for (size_t index = 0; index < syntax->expression_count; ++index) {
        const SolExpr *expression = &syntax->expressions[index];
        bool valid = (int)expression->kind >= 0
            && expression->kind <= SOL_EXPR_PROPAGATE
            && sol_span_valid(resolver->source, expression->span);
        switch (expression->kind) {
            case SOL_EXPR_PATH:
                valid = valid && sol_span_valid(resolver->source, expression->as.name);
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
                    && sol_span_valid(resolver->source, expression->as.field.name);
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
            case SOL_EXPR_BLOCK:
                valid = valid
                    && (expression->as.block.first_statement == SOL_AST_NONE
                        || expression->as.block.first_statement < syntax->statement_count);
                break;
            case SOL_EXPR_PROPAGATE:
                valid = valid && expression->as.propagated < syntax->expression_count;
                break;
            default:
                break;
        }
        if (!valid) {
            sol_resolver_malformed(resolver);
            return false;
        }
    }
    return true;
}

static bool sol_resolver_allocate(SolResolver *resolver) {
    const SolSyntaxTree *syntax = resolver->syntax;
    if (syntax->item_count > SIZE_MAX / sizeof(*resolver->module->definitions)
        || syntax->expression_count > SIZE_MAX / sizeof(*resolver->module->resolutions)) {
        return false;
    }

    resolver->module->definition_count = syntax->item_count;
    resolver->module->resolution_count = syntax->expression_count;
    resolver->module->definitions = calloc(
        syntax->item_count,
        sizeof(*resolver->module->definitions)
    );
    resolver->module->resolutions = calloc(
        syntax->expression_count,
        sizeof(*resolver->module->resolutions)
    );

    if ((syntax->item_count != 0 && resolver->module->definitions == NULL)
        || (syntax->expression_count != 0 && resolver->module->resolutions == NULL)) {
        return false;
    }
    resolver->expression_states = calloc(
        syntax->expression_count,
        sizeof(*resolver->expression_states)
    );
    if (syntax->expression_count != 0 && resolver->expression_states == NULL) {
        return false;
    }

    if (syntax->parameter_count > SIZE_MAX - syntax->statement_count) {
        return false;
    }
    size_t local_capacity = syntax->parameter_count + syntax->statement_count;
    if (local_capacity > SIZE_MAX / sizeof(*resolver->module->locals)
        || local_capacity > SIZE_MAX / sizeof(*resolver->bindings)) {
        return false;
    }
    resolver->module->locals = calloc(local_capacity, sizeof(*resolver->module->locals));
    resolver->bindings = calloc(local_capacity, sizeof(*resolver->bindings));
    if (local_capacity != 0
        && (resolver->module->locals == NULL || resolver->bindings == NULL)) {
        return false;
    }
    resolver->module->local_capacity = local_capacity;
    resolver->binding_capacity = local_capacity;
    return true;
}

static SolResolution sol_resolver_builtin(const SolSource *source, SolSpan name) {
    if (sol_span_text_equal(source, name, "ok")) {
        return (SolResolution){.kind = SOL_RESOLUTION_BUILTIN, .target = SOL_BUILTIN_OK};
    }
    if (sol_span_text_equal(source, name, "err")) {
        return (SolResolution){.kind = SOL_RESOLUTION_BUILTIN, .target = SOL_BUILTIN_ERR};
    }
    if (sol_span_text_equal(source, name, "some")) {
        return (SolResolution){.kind = SOL_RESOLUTION_BUILTIN, .target = SOL_BUILTIN_SOME};
    }
    if (sol_span_text_equal(source, name, "none")) {
        return (SolResolution){.kind = SOL_RESOLUTION_BUILTIN, .target = SOL_BUILTIN_NONE};
    }
    return (SolResolution){.kind = SOL_RESOLUTION_ERROR};
}

static SolResolution sol_resolver_lookup(SolResolver *resolver, SolSpan name) {
    for (size_t index = resolver->binding_count; index > 0; --index) {
        SolBinding *binding = &resolver->bindings[index - 1];
        if (sol_span_equal(resolver->source, binding->name, name)) {
            return binding->resolution;
        }
    }
    for (size_t index = 0; index < resolver->module->definition_count; ++index) {
        if (sol_path_span_equal(
            resolver->source,
            resolver->module->definitions[index].name,
            name
        )) {
            return (SolResolution){
                .kind = SOL_RESOLUTION_DEFINITION,
                .target = index,
            };
        }
    }
    return sol_resolver_builtin(resolver->source, name);
}

static SolResolution sol_resolver_lookup_definition(SolResolver *resolver, SolSpan name) {
    for (size_t index = 0; index < resolver->module->definition_count; ++index) {
        if (sol_path_span_equal(
            resolver->source,
            resolver->module->definitions[index].name,
            name
        )) {
            return (SolResolution){
                .kind = SOL_RESOLUTION_DEFINITION,
                .target = index,
            };
        }
    }
    return (SolResolution){.kind = SOL_RESOLUTION_ERROR};
}

static SolExprId sol_resolver_field_root(SolResolver *resolver, SolExprId expression_id) {
    SolExprId root = expression_id;
    while (root < resolver->syntax->expression_count
        && resolver->syntax->expressions[root].kind == SOL_EXPR_FIELD) {
        root = resolver->syntax->expressions[root].as.field.base;
    }
    return root;
}

static bool sol_resolver_add_binding(
    SolResolver *resolver,
    SolSpan name,
    SolLocalKind kind,
    size_t syntax_id
) {
    for (size_t index = resolver->binding_count; index > 0; --index) {
        SolBinding *binding = &resolver->bindings[index - 1];
        if (binding->scope_depth != resolver->scope_depth) {
            break;
        }
        if (sol_span_equal(resolver->source, binding->name, name)) {
            sol_diagnostics_add(
                resolver->diagnostics,
                "SOL-RESOLVE-003",
                SOL_SEVERITY_ERROR,
                name,
                "duplicate name in the same lexical scope"
            );
            break;
        }
    }

    if (resolver->module->local_count >= resolver->module->local_capacity
        || resolver->binding_count >= resolver->binding_capacity) {
        resolver->allocation_failed = true;
        return false;
    }
    SolLocalId local = resolver->module->local_count++;
    resolver->module->locals[local] = (SolHirLocal){
        .kind = kind,
        .name = name,
        .owner = resolver->current_definition,
        .syntax_id = syntax_id,
    };
    resolver->bindings[resolver->binding_count++] = (SolBinding){
        .name = name,
        .resolution = {.kind = SOL_RESOLUTION_LOCAL, .target = local},
        .scope_depth = resolver->scope_depth,
    };
    return true;
}

static void sol_resolver_expression(SolResolver *resolver, SolExprId expression_id);

static void sol_resolver_arguments(SolResolver *resolver, SolArgumentId argument_id) {
    size_t traversed = 0;
    while (argument_id != SOL_AST_NONE) {
        if (argument_id >= resolver->syntax->argument_count
            || traversed++ >= resolver->syntax->argument_count) {
            sol_resolver_malformed(resolver);
            return;
        }
        const SolArgument *argument = &resolver->syntax->arguments[argument_id];
        sol_resolver_expression(resolver, argument->value);
        argument_id = argument->next;
    }
}

static void sol_resolver_block(SolResolver *resolver, const SolExpr *block) {
    size_t binding_mark = resolver->binding_count;
    ++resolver->scope_depth;
    SolStatementId statement_id = block->as.block.first_statement;
    size_t traversed = 0;
    while (statement_id != SOL_AST_NONE) {
        if (statement_id >= resolver->syntax->statement_count
            || traversed++ >= resolver->syntax->statement_count) {
            sol_resolver_malformed(resolver);
            break;
        }
        const SolStatement *statement = &resolver->syntax->statements[statement_id];
        if (statement->kind == SOL_STATEMENT_LET) {
            sol_resolver_expression(resolver, statement->as.let_statement.value);
            sol_resolver_add_binding(
                resolver,
                statement->as.let_statement.name,
                SOL_LOCAL_BINDING,
                statement_id
            );
        } else {
            sol_resolver_expression(resolver, statement->as.expression);
        }
        statement_id = statement->next;
    }
    resolver->binding_count = binding_mark;
    --resolver->scope_depth;
}

static void sol_resolver_expression(SolResolver *resolver, SolExprId expression_id) {
    if (expression_id == SOL_AST_NONE || expression_id >= resolver->syntax->expression_count) {
        sol_resolver_malformed(resolver);
        return;
    }
    if (resolver->expression_states[expression_id] == 1) {
        sol_resolver_malformed(resolver);
        return;
    }
    if (resolver->expression_states[expression_id] == 2) {
        return;
    }
    if (resolver->traversal_depth >= 256) {
        if (!resolver->traversal_limit_reported) {
            sol_diagnostics_add(
                resolver->diagnostics,
                "SOL-RESOLVE-004",
                SOL_SEVERITY_ERROR,
                resolver->syntax->expressions[expression_id].span,
                "expression structure exceeds the semantic traversal limit of 256"
            );
            resolver->traversal_limit_reported = true;
        }
        return;
    }
    resolver->expression_states[expression_id] = 1;
    ++resolver->traversal_depth;
    const SolExpr *expression = &resolver->syntax->expressions[expression_id];
    switch (expression->kind) {
        case SOL_EXPR_PATH: {
            SolResolution resolution = sol_resolver_lookup(resolver, expression->as.name);
            resolver->module->resolutions[expression_id] = resolution;
            if (resolution.kind == SOL_RESOLUTION_ERROR) {
                sol_diagnostics_add(
                    resolver->diagnostics,
                    "SOL-RESOLVE-002",
                    SOL_SEVERITY_ERROR,
                    expression->as.name,
                    "unresolved name"
                );
            }
            break;
        }
        case SOL_EXPR_UNARY:
            sol_resolver_expression(resolver, expression->as.unary.operand);
            break;
        case SOL_EXPR_BINARY:
            sol_resolver_expression(resolver, expression->as.binary.left);
            sol_resolver_expression(resolver, expression->as.binary.right);
            break;
        case SOL_EXPR_CALL:
            sol_resolver_expression(resolver, expression->as.call.callee);
            sol_resolver_arguments(resolver, expression->as.call.first_argument);
            break;
        case SOL_EXPR_FIELD:
            {
                SolExprId root = sol_resolver_field_root(resolver, expression_id);
                if (root < resolver->syntax->expression_count
                    && resolver->syntax->expressions[root].kind == SOL_EXPR_PATH) {
                    SolSpan base_name = resolver->syntax->expressions[root].as.name;
                    SolResolution base_resolution = sol_resolver_lookup(resolver, base_name);
                    if (base_resolution.kind == SOL_RESOLUTION_LOCAL) {
                        sol_resolver_expression(resolver, expression->as.field.base);
                        break;
                    }
                }
            }
            resolver->module->resolutions[expression_id] = sol_resolver_lookup_definition(
                resolver,
                expression->span
            );
            if (resolver->module->resolutions[expression_id].kind == SOL_RESOLUTION_ERROR) {
                resolver->module->resolutions[expression_id] = (SolResolution){
                    .kind = SOL_RESOLUTION_NOT_APPLICABLE,
                };
                sol_resolver_expression(resolver, expression->as.field.base);
            }
            break;
        case SOL_EXPR_RECORD:
            sol_resolver_expression(resolver, expression->as.record.type);
            sol_resolver_arguments(resolver, expression->as.record.first_field);
            break;
        case SOL_EXPR_IF:
            sol_resolver_expression(resolver, expression->as.if_expr.condition);
            sol_resolver_expression(resolver, expression->as.if_expr.then_branch);
            sol_resolver_expression(resolver, expression->as.if_expr.else_branch);
            break;
        case SOL_EXPR_BLOCK:
            sol_resolver_block(resolver, expression);
            break;
        case SOL_EXPR_PROPAGATE:
            sol_resolver_expression(resolver, expression->as.propagated);
            break;
        default:
            break;
    }
    --resolver->traversal_depth;
    resolver->expression_states[expression_id] = 2;
}

static void sol_resolver_collect_definitions(SolResolver *resolver) {
    for (size_t index = 0; index < resolver->syntax->item_count; ++index) {
        const SolSyntaxItem *item = &resolver->syntax->items[index];
        for (size_t previous = 0; previous < index; ++previous) {
            if (sol_path_span_equal(
                resolver->source,
                resolver->module->definitions[previous].name,
                item->name
            )) {
                sol_diagnostics_add(
                    resolver->diagnostics,
                    "SOL-RESOLVE-001",
                    SOL_SEVERITY_ERROR,
                    item->name,
                    "duplicate declaration in module"
                );
                break;
            }
        }
        resolver->module->definitions[index] = (SolHirDefinition){
            .kind = item->kind,
            .name = item->name,
            .syntax_item = index,
        };
    }
}

static void sol_resolver_function(SolResolver *resolver, SolDefId definition) {
    const SolSyntaxItem *item = &resolver->syntax->items[definition];
    resolver->current_definition = definition;
    resolver->binding_count = 0;
    resolver->scope_depth = 0;

    SolParameterId parameter_id = item->first_parameter;
    size_t traversed = 0;
    while (parameter_id != SOL_AST_NONE) {
        if (parameter_id >= resolver->syntax->parameter_count
            || traversed++ >= resolver->syntax->parameter_count) {
            sol_resolver_malformed(resolver);
            break;
        }
        const SolParameter *parameter = &resolver->syntax->parameters[parameter_id];
        sol_resolver_add_binding(
            resolver,
            parameter->name,
            SOL_LOCAL_PARAMETER,
            parameter_id
        );
        parameter_id = parameter->next;
    }
    if (item->body != SOL_AST_NONE) {
        sol_resolver_expression(resolver, item->body);
    }
    resolver->binding_count = 0;
}

bool sol_hir_lower(
    const SolSource *source,
    const SolSyntaxTree *syntax,
    SolHirModule *module,
    SolDiagnostics *diagnostics
) {
    SolResolver resolver = {
        .source = source,
        .syntax = syntax,
        .module = module,
        .diagnostics = diagnostics,
    };
    if (!sol_resolver_validate(&resolver)) {
        return false;
    }
    if (!sol_resolver_allocate(&resolver)) {
        sol_diagnostics_add(
            diagnostics,
            "SOL-INTERNAL-001",
            SOL_SEVERITY_ERROR,
            (SolSpan){0},
            "out of memory while lowering semantic HIR"
        );
        free(resolver.bindings);
        free(resolver.expression_states);
        return false;
    }

    sol_resolver_collect_definitions(&resolver);
    for (size_t index = 0; index < syntax->item_count; ++index) {
        if (syntax->items[index].kind == SOL_ITEM_FUNCTION) {
            sol_resolver_function(&resolver, index);
        }
    }

    free(resolver.bindings);
    free(resolver.expression_states);
    return !resolver.allocation_failed
        && !resolver.malformed
        && !diagnostics->allocation_failed;
}
