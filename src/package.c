#define _POSIX_C_SOURCE 200809L

#include "sol/package.h"

#include "sol/lexer.h"

#include <dirent.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

typedef struct {
    char **items;
    size_t count;
    size_t capacity;
} SolPathList;

typedef struct {
    size_t imports;
    size_t items;
    size_t expressions;
    size_t statements;
    size_t arguments;
    size_t parameters;
    size_t types;
    size_t type_arguments;
    size_t type_parameters;
    size_t effect_parameters;
    size_t fields;
    size_t variants;
    size_t patterns;
    size_t pattern_bindings;
    size_t match_arms;
    size_t effects;
    size_t capability_members;
    size_t trait_methods;
    size_t contract_clauses;
    size_t contract_conditions;
} SolArenaOffsets;

static void sol_package_error(char *error, size_t error_size, const char *format, const char *path) {
    if (error != NULL && error_size != 0) snprintf(error, error_size, format, path);
}

static char *sol_package_copy_string(const char *text) {
    size_t length = strlen(text);
    if (length == SIZE_MAX) return NULL;
    char *copy = malloc(length + 1);
    if (copy != NULL) memcpy(copy, text, length + 1);
    return copy;
}

static bool sol_package_add_size(size_t left, size_t right, size_t *result) {
    if (right > SIZE_MAX - left) return false;
    *result = left + right;
    return true;
}

static bool sol_package_is_source_path(const char *path) {
    size_t length = strlen(path);
    return length >= 4 && memcmp(path + length - 4, ".sol", 4) == 0;
}

static bool sol_path_list_add(SolPathList *paths, char *path) {
    if (paths->count == paths->capacity) {
        size_t capacity = paths->capacity == 0 ? 16 : paths->capacity * 2;
        if (capacity < paths->capacity || capacity > SIZE_MAX / sizeof(*paths->items)) return false;
        char **items = realloc(paths->items, capacity * sizeof(*items));
        if (items == NULL) return false;
        paths->items = items;
        paths->capacity = capacity;
    }
    paths->items[paths->count++] = path;
    return true;
}

static void sol_path_list_free(SolPathList *paths) {
    for (size_t index = 0; index < paths->count; ++index) free(paths->items[index]);
    free(paths->items);
    memset(paths, 0, sizeof(*paths));
}

static char *sol_package_join_path(const char *directory, const char *name) {
    size_t directory_length = strlen(directory);
    size_t name_length = strlen(name);
    bool separator = directory_length != 0 && directory[directory_length - 1] != '/';
    size_t length;
    if (!sol_package_add_size(directory_length, separator ? 1 : 0, &length)
        || !sol_package_add_size(length, name_length, &length) || length == SIZE_MAX) return NULL;
    char *path = malloc(length + 1);
    if (path == NULL) return NULL;
    memcpy(path, directory, directory_length);
    size_t cursor = directory_length;
    if (separator) path[cursor++] = '/';
    memcpy(path + cursor, name, name_length + 1);
    return path;
}

static bool sol_package_discover(
    const char *directory,
    SolPathList *paths,
    char *error,
    size_t error_size
) {
    DIR *handle = opendir(directory);
    if (handle == NULL) {
        if (error != NULL && error_size != 0) {
            snprintf(error, error_size, "cannot open directory '%s': %s", directory, strerror(errno));
        }
        return false;
    }
    bool complete = true;
    errno = 0;
    struct dirent *entry;
    while ((entry = readdir(handle)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char *path = sol_package_join_path(directory, entry->d_name);
        if (path == NULL) {
            sol_package_error(error, error_size, "out of memory while discovering '%s'", directory);
            complete = false;
            break;
        }
        struct stat status;
        if (lstat(path, &status) != 0) {
            if (error != NULL && error_size != 0) {
                snprintf(error, error_size, "cannot inspect '%s': %s", path, strerror(errno));
            }
            free(path);
            complete = false;
            break;
        }
        if (S_ISDIR(status.st_mode)) {
            complete = sol_package_discover(path, paths, error, error_size);
            free(path);
            if (!complete) break;
        } else if (S_ISREG(status.st_mode) && sol_package_is_source_path(path)) {
            if (!sol_path_list_add(paths, path)) {
                sol_package_error(error, error_size, "out of memory while discovering '%s'", directory);
                free(path);
                complete = false;
                break;
            }
        } else {
            free(path);
        }
        errno = 0;
    }
    if (complete && errno != 0) {
        if (error != NULL && error_size != 0) {
            snprintf(error, error_size, "cannot read directory '%s': %s", directory, strerror(errno));
        }
        complete = false;
    }
    if (closedir(handle) != 0 && complete) {
        if (error != NULL && error_size != 0) {
            snprintf(error, error_size, "cannot close directory '%s': %s", directory, strerror(errno));
        }
        complete = false;
    }
    return complete;
}

static int sol_package_compare_paths(const void *left, const void *right) {
    const char *const *left_path = left;
    const char *const *right_path = right;
    return strcmp(*left_path, *right_path);
}

void sol_package_init(SolPackage *package) {
    memset(package, 0, sizeof(*package));
    sol_syntax_tree_init(&package->syntax);
}

void sol_package_free(SolPackage *package) {
    for (size_t index = 0; index < package->file_count; ++index) {
        sol_source_free(&package->files[index].source);
        free(package->files[index].path);
    }
    free(package->files);
    sol_syntax_tree_free(&package->syntax);
    sol_source_free(&package->source);
    free(package->path);
    memset(package, 0, sizeof(*package));
}

static SolSpan sol_relocate_span(SolSpan span, size_t base) {
    span.start += base;
    span.end += base;
    return span;
}

static SolSpan sol_relocate_optional_span(SolSpan span, size_t base) {
    return span.start == 0 && span.end == 0 ? span : sol_relocate_span(span, base);
}

static size_t sol_relocate_id(size_t id, size_t base) {
    return id == SOL_AST_NONE ? SOL_AST_NONE : id + base;
}

static bool sol_package_append_storage(
    void **target,
    size_t old_count,
    const void *source,
    size_t count,
    size_t element_size
) {
    size_t total;
    if (!sol_package_add_size(old_count, count, &total) || total > SIZE_MAX / element_size) return false;
    if (count == 0) return true;
    void *grown = realloc(*target, total * element_size);
    if (grown == NULL) return false;
    *target = grown;
    memcpy((unsigned char *)grown + old_count * element_size, source, count * element_size);
    return true;
}

#define SOL_APPEND_ARENA(target, local, field, count_field, capacity_field) \
    (sol_package_append_storage( \
        (void **)&(target)->field, (target)->count_field, (local)->field, \
        (local)->count_field, sizeof(*(target)->field) \
    ) && ((target)->count_field += (local)->count_field, \
        (target)->capacity_field = (target)->count_field, true))

static SolArenaOffsets sol_package_offsets(const SolSyntaxTree *tree) {
    return (SolArenaOffsets){
        .imports = tree->import_count,
        .items = tree->item_count,
        .expressions = tree->expression_count,
        .statements = tree->statement_count,
        .arguments = tree->argument_count,
        .parameters = tree->parameter_count,
        .types = tree->type_count,
        .type_arguments = tree->type_argument_count,
        .type_parameters = tree->type_parameter_count,
        .effect_parameters = tree->effect_parameter_count,
        .fields = tree->field_count,
        .variants = tree->variant_count,
        .patterns = tree->pattern_count,
        .pattern_bindings = tree->pattern_binding_count,
        .match_arms = tree->match_arm_count,
        .effects = tree->effect_count,
        .capability_members = tree->capability_member_count,
        .trait_methods = tree->trait_method_count,
        .contract_clauses = tree->contract_clause_count,
        .contract_conditions = tree->contract_condition_count,
    };
}

static bool sol_package_append_tree(SolSyntaxTree *target, const SolSyntaxTree *local) {
    return SOL_APPEND_ARENA(target, local, imports, import_count, import_capacity)
        && SOL_APPEND_ARENA(target, local, items, item_count, item_capacity)
        && SOL_APPEND_ARENA(target, local, expressions, expression_count, expression_capacity)
        && SOL_APPEND_ARENA(target, local, statements, statement_count, statement_capacity)
        && SOL_APPEND_ARENA(target, local, arguments, argument_count, argument_capacity)
        && SOL_APPEND_ARENA(target, local, parameters, parameter_count, parameter_capacity)
        && SOL_APPEND_ARENA(target, local, types, type_count, type_capacity)
        && SOL_APPEND_ARENA(target, local, type_arguments, type_argument_count, type_argument_capacity)
        && SOL_APPEND_ARENA(target, local, type_parameters, type_parameter_count, type_parameter_capacity)
        && SOL_APPEND_ARENA(target, local, effect_parameters, effect_parameter_count, effect_parameter_capacity)
        && SOL_APPEND_ARENA(target, local, fields, field_count, field_capacity)
        && SOL_APPEND_ARENA(target, local, variants, variant_count, variant_capacity)
        && SOL_APPEND_ARENA(target, local, patterns, pattern_count, pattern_capacity)
        && SOL_APPEND_ARENA(target, local, pattern_bindings, pattern_binding_count, pattern_binding_capacity)
        && SOL_APPEND_ARENA(target, local, match_arms, match_arm_count, match_arm_capacity)
        && SOL_APPEND_ARENA(target, local, effects, effect_count, effect_capacity)
        && SOL_APPEND_ARENA(target, local, capability_members, capability_member_count, capability_member_capacity)
        && SOL_APPEND_ARENA(target, local, trait_methods, trait_method_count, trait_method_capacity)
        && SOL_APPEND_ARENA(target, local, contract_clauses, contract_clause_count, contract_clause_capacity)
        && SOL_APPEND_ARENA(target, local, contract_conditions, contract_condition_count, contract_condition_capacity);
}

static void sol_package_relocate_items(
    SolSyntaxTree *tree, const SolSyntaxTree *local, SolArenaOffsets offsets, size_t base
) {
    for (size_t index = offsets.imports; index < tree->import_count; ++index) {
        tree->imports[index].path = sol_relocate_span(tree->imports[index].path, base);
    }
    for (size_t index = offsets.items; index < tree->item_count; ++index) {
        SolSyntaxItem *item = &tree->items[index];
        item->name = sol_relocate_span(item->name, base);
        item->span = sol_relocate_span(item->span, base);
        item->stable_identity = sol_relocate_optional_span(item->stable_identity, base);
        item->body = sol_relocate_id(item->body, offsets.expressions);
        item->first_parameter = sol_relocate_id(item->first_parameter, offsets.parameters);
        item->return_type = sol_relocate_optional_span(item->return_type, base);
        item->return_type_id = sol_relocate_id(item->return_type_id, offsets.types);
        item->representation_type = sol_relocate_id(
            item->representation_type, offsets.types
        );
        item->first_field = sol_relocate_id(item->first_field, offsets.fields);
        item->first_variant = sol_relocate_id(item->first_variant, offsets.variants);
        item->first_effect = sol_relocate_id(item->first_effect, offsets.effects);
        item->first_contract = sol_relocate_id(item->first_contract, offsets.contract_clauses);
        item->first_member = sol_relocate_id(item->first_member, offsets.capability_members);
        item->result_authority_parameter = sol_relocate_id(
            item->result_authority_parameter, offsets.parameters
        );
        item->capability_source = sol_relocate_id(item->capability_source, offsets.parameters);
        item->first_type_parameter = sol_relocate_id(
            item->first_type_parameter, offsets.type_parameters
        );
        item->first_effect_parameter = sol_relocate_id(
            item->first_effect_parameter, offsets.effect_parameters
        );
        item->trait_name = sol_relocate_optional_span(item->trait_name, base);
        item->implementation_type = sol_relocate_id(item->implementation_type, offsets.types);
        item->first_trait_method = sol_relocate_id(item->first_trait_method, offsets.trait_methods);
    }
    (void)local;
}

static void sol_package_relocate_expressions(
    SolSyntaxTree *tree, SolArenaOffsets offsets, size_t base
) {
    for (size_t index = offsets.expressions; index < tree->expression_count; ++index) {
        SolExpr *expression = &tree->expressions[index];
        expression->span = sol_relocate_span(expression->span, base);
        switch (expression->kind) {
            case SOL_EXPR_PATH:
                expression->as.name = sol_relocate_span(expression->as.name, base);
                break;
            case SOL_EXPR_UNARY:
                expression->as.unary.operand = sol_relocate_id(
                    expression->as.unary.operand, offsets.expressions
                );
                break;
            case SOL_EXPR_BINARY:
                expression->as.binary.left = sol_relocate_id(
                    expression->as.binary.left, offsets.expressions
                );
                expression->as.binary.right = sol_relocate_id(
                    expression->as.binary.right, offsets.expressions
                );
                break;
            case SOL_EXPR_CALL:
                expression->as.call.callee = sol_relocate_id(
                    expression->as.call.callee, offsets.expressions
                );
                expression->as.call.first_argument = sol_relocate_id(
                    expression->as.call.first_argument, offsets.arguments
                );
                break;
            case SOL_EXPR_TYPE_APPLICATION:
                expression->as.type_application.base = sol_relocate_id(
                    expression->as.type_application.base, offsets.expressions
                );
                expression->as.type_application.first_argument = sol_relocate_id(
                    expression->as.type_application.first_argument, offsets.type_arguments
                );
                break;
            case SOL_EXPR_FIELD:
                expression->as.field.base = sol_relocate_id(
                    expression->as.field.base, offsets.expressions
                );
                expression->as.field.name = sol_relocate_span(expression->as.field.name, base);
                break;
            case SOL_EXPR_RECORD:
                expression->as.record.type = sol_relocate_id(
                    expression->as.record.type, offsets.expressions
                );
                expression->as.record.first_field = sol_relocate_id(
                    expression->as.record.first_field, offsets.arguments
                );
                break;
            case SOL_EXPR_IF:
                expression->as.if_expr.condition = sol_relocate_id(
                    expression->as.if_expr.condition, offsets.expressions
                );
                expression->as.if_expr.then_branch = sol_relocate_id(
                    expression->as.if_expr.then_branch, offsets.expressions
                );
                expression->as.if_expr.else_branch = sol_relocate_id(
                    expression->as.if_expr.else_branch, offsets.expressions
                );
                break;
            case SOL_EXPR_MATCH:
                expression->as.match_expr.scrutinee = sol_relocate_id(
                    expression->as.match_expr.scrutinee, offsets.expressions
                );
                expression->as.match_expr.first_arm = sol_relocate_id(
                    expression->as.match_expr.first_arm, offsets.match_arms
                );
                break;
            case SOL_EXPR_BLOCK:
                expression->as.block.first_statement = sol_relocate_id(
                    expression->as.block.first_statement, offsets.statements
                );
                break;
            case SOL_EXPR_PROPAGATE:
                expression->as.propagated = sol_relocate_id(
                    expression->as.propagated, offsets.expressions
                );
                break;
            case SOL_EXPR_HANDLE:
                expression->as.handle.effect_name = sol_relocate_span(
                    expression->as.handle.effect_name, base
                );
                expression->as.handle.authority = sol_relocate_id(
                    expression->as.handle.authority, offsets.expressions
                );
                expression->as.handle.provider = sol_relocate_id(
                    expression->as.handle.provider, offsets.expressions
                );
                expression->as.handle.body = sol_relocate_id(
                    expression->as.handle.body, offsets.expressions
                );
                break;
            case SOL_EXPR_OLD:
                expression->as.old_expression = sol_relocate_id(
                    expression->as.old_expression, offsets.expressions
                );
                break;
            default:
                break;
        }
    }
}

static size_t sol_package_effect_owner_offset(SolEffectOwnerKind kind, SolArenaOffsets offsets) {
    switch (kind) {
        case SOL_EFFECT_OWNER_ITEM: return offsets.items;
        case SOL_EFFECT_OWNER_CAPABILITY_MEMBER: return offsets.capability_members;
        case SOL_EFFECT_OWNER_TRAIT_METHOD: return offsets.trait_methods;
        case SOL_EFFECT_OWNER_TYPE: return offsets.types;
    }
    return 0;
}

static size_t sol_package_contract_owner_offset(
    SolContractOwnerKind kind, SolArenaOffsets offsets
) {
    switch (kind) {
        case SOL_CONTRACT_OWNER_ITEM:
        case SOL_CONTRACT_OWNER_TYPE:
            return offsets.items;
        case SOL_CONTRACT_OWNER_CAPABILITY_MEMBER:
            return offsets.capability_members;
    }
    return 0;
}

static void sol_package_relocate_arenas(SolSyntaxTree *tree, SolArenaOffsets o, size_t base) {
    for (size_t i = o.arguments; i < tree->argument_count; ++i) {
        tree->arguments[i].name = sol_relocate_optional_span(tree->arguments[i].name, base);
        tree->arguments[i].value = sol_relocate_id(tree->arguments[i].value, o.expressions);
        tree->arguments[i].next = sol_relocate_id(tree->arguments[i].next, o.arguments);
    }
    for (size_t i = o.statements; i < tree->statement_count; ++i) {
        SolStatement *entry = &tree->statements[i];
        entry->span = sol_relocate_span(entry->span, base);
        entry->next = sol_relocate_id(entry->next, o.statements);
        if (entry->kind == SOL_STATEMENT_LET) {
            entry->as.let_statement.name = sol_relocate_span(entry->as.let_statement.name, base);
            entry->as.let_statement.value = sol_relocate_id(
                entry->as.let_statement.value, o.expressions
            );
        } else {
            entry->as.expression = sol_relocate_id(entry->as.expression, o.expressions);
        }
    }
    for (size_t i = o.parameters; i < tree->parameter_count; ++i) {
        tree->parameters[i].name = sol_relocate_span(tree->parameters[i].name, base);
        tree->parameters[i].type = sol_relocate_span(tree->parameters[i].type, base);
        tree->parameters[i].type_id = sol_relocate_id(tree->parameters[i].type_id, o.types);
        tree->parameters[i].next = sol_relocate_id(tree->parameters[i].next, o.parameters);
    }
    for (size_t i = o.types; i < tree->type_count; ++i) {
        tree->types[i].span = sol_relocate_span(tree->types[i].span, base);
        tree->types[i].name = sol_relocate_optional_span(tree->types[i].name, base);
        tree->types[i].first_argument = sol_relocate_id(
            tree->types[i].first_argument, o.type_arguments
        );
        tree->types[i].return_type = sol_relocate_id(tree->types[i].return_type, o.types);
        tree->types[i].first_effect = sol_relocate_id(tree->types[i].first_effect, o.effects);
        tree->types[i].effect_tail = sol_relocate_optional_span(
            tree->types[i].effect_tail, base
        );
        tree->types[i].owner_item += o.items;
    }
    for (size_t i = o.type_arguments; i < tree->type_argument_count; ++i) {
        tree->type_arguments[i].type = sol_relocate_id(tree->type_arguments[i].type, o.types);
        tree->type_arguments[i].next = sol_relocate_id(
            tree->type_arguments[i].next, o.type_arguments
        );
    }
    for (size_t i = o.type_parameters; i < tree->type_parameter_count; ++i) {
        tree->type_parameters[i].name = sol_relocate_span(tree->type_parameters[i].name, base);
        tree->type_parameters[i].bound = sol_relocate_optional_span(
            tree->type_parameters[i].bound, base
        );
        tree->type_parameters[i].next = sol_relocate_id(
            tree->type_parameters[i].next, o.type_parameters
        );
        tree->type_parameters[i].owner_item += o.items;
    }
    for (size_t i = o.effect_parameters; i < tree->effect_parameter_count; ++i) {
        tree->effect_parameters[i].name = sol_relocate_span(
            tree->effect_parameters[i].name, base
        );
        tree->effect_parameters[i].next = sol_relocate_id(
            tree->effect_parameters[i].next, o.effect_parameters
        );
        tree->effect_parameters[i].owner_item += o.items;
    }
    for (size_t i = o.fields; i < tree->field_count; ++i) {
        tree->fields[i].name = sol_relocate_span(tree->fields[i].name, base);
        tree->fields[i].span = sol_relocate_span(tree->fields[i].span, base);
        tree->fields[i].type = sol_relocate_id(tree->fields[i].type, o.types);
        tree->fields[i].next = sol_relocate_id(tree->fields[i].next, o.fields);
    }
    for (size_t i = o.variants; i < tree->variant_count; ++i) {
        tree->variants[i].name = sol_relocate_span(tree->variants[i].name, base);
        tree->variants[i].span = sol_relocate_span(tree->variants[i].span, base);
        tree->variants[i].first_field = sol_relocate_id(tree->variants[i].first_field, o.fields);
        tree->variants[i].next = sol_relocate_id(tree->variants[i].next, o.variants);
        tree->variants[i].owner_item += o.items;
    }
    for (size_t i = o.patterns; i < tree->pattern_count; ++i) {
        tree->patterns[i].span = sol_relocate_span(tree->patterns[i].span, base);
        tree->patterns[i].name = sol_relocate_optional_span(tree->patterns[i].name, base);
        tree->patterns[i].first_binding = sol_relocate_id(
            tree->patterns[i].first_binding, o.pattern_bindings
        );
    }
    for (size_t i = o.pattern_bindings; i < tree->pattern_binding_count; ++i) {
        tree->pattern_bindings[i].name = sol_relocate_span(
            tree->pattern_bindings[i].name, base
        );
        tree->pattern_bindings[i].next = sol_relocate_id(
            tree->pattern_bindings[i].next, o.pattern_bindings
        );
    }
    for (size_t i = o.match_arms; i < tree->match_arm_count; ++i) {
        tree->match_arms[i].pattern = sol_relocate_id(tree->match_arms[i].pattern, o.patterns);
        tree->match_arms[i].value = sol_relocate_id(tree->match_arms[i].value, o.expressions);
        tree->match_arms[i].span = sol_relocate_span(tree->match_arms[i].span, base);
        tree->match_arms[i].next = sol_relocate_id(tree->match_arms[i].next, o.match_arms);
    }
    for (size_t i = o.effects; i < tree->effect_count; ++i) {
        tree->effects[i].name = sol_relocate_span(tree->effects[i].name, base);
        tree->effects[i].argument = sol_relocate_optional_span(
            tree->effects[i].argument, base
        );
        tree->effects[i].span = sol_relocate_span(tree->effects[i].span, base);
        tree->effects[i].next = sol_relocate_id(tree->effects[i].next, o.effects);
        tree->effects[i].owner += sol_package_effect_owner_offset(tree->effects[i].owner_kind, o);
    }
    for (size_t i = o.contract_clauses; i < tree->contract_clause_count; ++i) {
        tree->contract_clauses[i].span = sol_relocate_span(tree->contract_clauses[i].span, base);
        tree->contract_clauses[i].first_condition = sol_relocate_id(
            tree->contract_clauses[i].first_condition, o.contract_conditions
        );
        tree->contract_clauses[i].next = sol_relocate_id(
            tree->contract_clauses[i].next, o.contract_clauses
        );
        tree->contract_clauses[i].owner += sol_package_contract_owner_offset(
            tree->contract_clauses[i].owner_kind, o
        );
    }
    for (size_t i = o.contract_conditions; i < tree->contract_condition_count; ++i) {
        tree->contract_conditions[i].span = sol_relocate_span(
            tree->contract_conditions[i].span, base
        );
        tree->contract_conditions[i].expression = sol_relocate_id(
            tree->contract_conditions[i].expression, o.expressions
        );
        tree->contract_conditions[i].next = sol_relocate_id(
            tree->contract_conditions[i].next, o.contract_conditions
        );
        tree->contract_conditions[i].owner_clause = sol_relocate_id(
            tree->contract_conditions[i].owner_clause, o.contract_clauses
        );
    }
}

static void sol_package_relocate_members(SolSyntaxTree *tree, SolArenaOffsets o, size_t base) {
    for (size_t i = o.capability_members; i < tree->capability_member_count; ++i) {
        SolCapabilityMember *entry = &tree->capability_members[i];
        entry->name = sol_relocate_span(entry->name, base);
        entry->span = sol_relocate_span(entry->span, base);
        entry->first_parameter = sol_relocate_id(entry->first_parameter, o.parameters);
        entry->return_type = sol_relocate_optional_span(entry->return_type, base);
        entry->return_type_id = sol_relocate_id(entry->return_type_id, o.types);
        entry->first_effect = sol_relocate_id(entry->first_effect, o.effects);
        entry->first_contract = sol_relocate_id(entry->first_contract, o.contract_clauses);
        entry->body = sol_relocate_id(entry->body, o.expressions);
        entry->next = sol_relocate_id(entry->next, o.capability_members);
        entry->owner_item += o.items;
    }
    for (size_t i = o.trait_methods; i < tree->trait_method_count; ++i) {
        SolTraitMethod *entry = &tree->trait_methods[i];
        entry->name = sol_relocate_span(entry->name, base);
        entry->span = sol_relocate_span(entry->span, base);
        entry->first_parameter = sol_relocate_id(entry->first_parameter, o.parameters);
        entry->return_type = sol_relocate_optional_span(entry->return_type, base);
        entry->return_type_id = sol_relocate_id(entry->return_type_id, o.types);
        entry->first_effect = sol_relocate_id(entry->first_effect, o.effects);
        entry->body = sol_relocate_id(entry->body, o.expressions);
        entry->next = sol_relocate_id(entry->next, o.trait_methods);
        entry->owner_item += o.items;
    }
}

static bool sol_package_add_diagnostics(
    SolDiagnostics *target, const SolDiagnostics *local, size_t base
) {
    for (size_t index = 0; index < local->count; ++index) {
        const SolDiagnostic *entry = &local->items[index];
        if (!sol_diagnostics_add(
            target,
            entry->code,
            entry->severity,
            sol_relocate_span(entry->span, base),
            "%s",
            entry->message
        )) return false;
    }
    return !local->allocation_failed;
}

static bool sol_package_parse_file(
    SolPackage *package,
    SolPackageFile *file,
    SolDiagnostics *diagnostics,
    char *error,
    size_t error_size
) {
    SolTokens tokens;
    SolSyntaxTree local;
    SolDiagnostics local_diagnostics;
    sol_tokens_init(&tokens);
    sol_syntax_tree_init(&local);
    sol_diagnostics_init(&local_diagnostics);
    bool complete = sol_lex(&file->source, &tokens, &local_diagnostics);
    if (complete) complete = sol_parse(&file->source, &tokens, &local, &local_diagnostics);
    SolArenaOffsets offsets = sol_package_offsets(&package->syntax);
    file->module_name = sol_relocate_span(local.module_name, file->aggregate_start);
    file->import_start = offsets.imports;
    file->import_count = local.import_count;
    file->item_start = offsets.items;
    file->item_count = local.item_count;
    if (file == package->files) {
        package->syntax.edition = local.edition;
    }
    if (complete) complete = sol_package_append_tree(&package->syntax, &local);
    if (complete) {
        sol_package_relocate_items(&package->syntax, &local, offsets, file->aggregate_start);
        sol_package_relocate_expressions(&package->syntax, offsets, file->aggregate_start);
        sol_package_relocate_arenas(&package->syntax, offsets, file->aggregate_start);
        sol_package_relocate_members(&package->syntax, offsets, file->aggregate_start);
        complete = sol_package_add_diagnostics(
            diagnostics, &local_diagnostics, file->aggregate_start
        );
    }
    if (!complete) sol_package_error(error, error_size, "out of memory while parsing '%s'", file->path);
    sol_diagnostics_free(&local_diagnostics);
    sol_syntax_tree_free(&local);
    sol_tokens_free(&tokens);
    return complete;
}

static bool sol_package_load_paths(
    SolPackage *package,
    const char *root,
    SolPathList *paths,
    SolDiagnostics *diagnostics,
    char *error,
    size_t error_size
) {
    package->path = sol_package_copy_string(root);
    if (package->path == NULL) {
        sol_package_error(error, error_size, "out of memory while loading '%s'", root);
        return false;
    }
    package->files = calloc(paths->count, sizeof(*package->files));
    if (paths->count != 0 && package->files == NULL) {
        sol_package_error(error, error_size, "out of memory while loading '%s'", root);
        return false;
    }
    package->file_capacity = paths->count;
    size_t aggregate_length = 0;
    for (size_t index = 0; index < paths->count; ++index) {
        SolPackageFile *file = &package->files[index];
        file->path = paths->items[index];
        paths->items[index] = NULL;
        package->file_count = index + 1;
        if (!sol_source_load(&file->source, file->path, error, error_size)) return false;
        if (memchr(file->source.text, '\0', file->source.length) != NULL) {
            sol_package_error(error, error_size, "source contains an embedded NUL byte: '%s'", file->path);
            return false;
        }
        file->aggregate_start = aggregate_length;
        if (!sol_package_add_size(aggregate_length, file->source.length, &aggregate_length)) {
            sol_package_error(error, error_size, "package source is too large at '%s'", file->path);
            return false;
        }
        file->aggregate_end = aggregate_length;
        if (index + 1 < paths->count
            && !sol_package_add_size(aggregate_length, 1, &aggregate_length)) {
            sol_package_error(error, error_size, "package source is too large at '%s'", file->path);
            return false;
        }
    }
    if (aggregate_length == SIZE_MAX) {
        sol_package_error(error, error_size, "package source is too large at '%s'", root);
        return false;
    }
    char *text = malloc(aggregate_length + 1);
    if (text == NULL) {
        sol_package_error(error, error_size, "out of memory while aggregating '%s'", root);
        return false;
    }
    size_t cursor = 0;
    for (size_t index = 0; index < package->file_count; ++index) {
        const SolSource *source = &package->files[index].source;
        memcpy(text + cursor, source->text, source->length);
        cursor += source->length;
        if (index + 1 < package->file_count) text[cursor++] = '\n';
    }
    text[cursor] = '\0';
    bool complete = sol_source_from_text(&package->source, package->path, text);
    free(text);
    if (!complete) {
        sol_package_error(error, error_size, "out of memory while indexing '%s'", root);
        return false;
    }
    if (package->file_count == 0) {
        bool diagnosed = sol_diagnostics_add(
            diagnostics,
            "SOL-PACKAGE-001",
            SOL_SEVERITY_ERROR,
            (SolSpan){0},
            "package contains no .sol source files"
        );
        if (!diagnosed) {
            sol_package_error(error, error_size, "out of memory while diagnosing '%s'", root);
        }
        return diagnosed;
    }
    for (size_t index = 0; index < package->file_count; ++index) {
        if (!sol_package_parse_file(
            package, &package->files[index], diagnostics, error, error_size
        )) return false;
    }
    package->syntax.module_name = package->files[0].module_name;
    if (!sol_diagnostics_has_errors(diagnostics)
        && !sol_syntax_contracts_validate(&package->source, &package->syntax)) {
        if (!sol_diagnostics_add(
            diagnostics,
            "SOL-INTERNAL-002",
            SOL_SEVERITY_ERROR,
            (SolSpan){0},
            "aggregated syntax tree failed structural validation"
        )) {
            sol_package_error(error, error_size, "out of memory while validating '%s'", root);
            return false;
        }
    }
    return true;
}

static bool sol_package_load_kind(
    SolPackage *package,
    const char *path,
    bool directory_only,
    SolDiagnostics *diagnostics,
    char *error,
    size_t error_size
) {
    sol_package_free(package);
    sol_package_init(package);
    if (error != NULL && error_size != 0) error[0] = '\0';
    struct stat status;
    if (stat(path, &status) != 0) {
        if (error != NULL && error_size != 0) {
            snprintf(error, error_size, "cannot inspect '%s': %s", path, strerror(errno));
        }
        return false;
    }
    SolPathList paths = {0};
    bool complete;
    if (S_ISDIR(status.st_mode)) {
        package->is_directory = true;
        complete = sol_package_discover(path, &paths, error, error_size);
    } else if (!directory_only && S_ISREG(status.st_mode) && sol_package_is_source_path(path)) {
        char *copy = sol_package_copy_string(path);
        complete = copy != NULL && sol_path_list_add(&paths, copy);
        if (!complete) {
            free(copy);
            sol_package_error(error, error_size, "out of memory while loading '%s'", path);
        }
    } else {
        sol_package_error(
            error,
            error_size,
            directory_only ? "not a directory: '%s'" : "not a directory or regular .sol file: '%s'",
            path
        );
        complete = false;
    }
    if (complete) {
        qsort(paths.items, paths.count, sizeof(*paths.items), sol_package_compare_paths);
        complete = sol_package_load_paths(package, path, &paths, diagnostics, error, error_size);
    }
    sol_path_list_free(&paths);
    return complete;
}

bool sol_package_load(
    SolPackage *package,
    const char *path,
    SolDiagnostics *diagnostics,
    char *error,
    size_t error_size
) {
    return sol_package_load_kind(package, path, false, diagnostics, error, error_size);
}

bool sol_package_load_directory(
    SolPackage *package,
    const char *directory,
    SolDiagnostics *diagnostics,
    char *error,
    size_t error_size
) {
    return sol_package_load_kind(package, directory, true, diagnostics, error, error_size);
}

const SolPackageFile *sol_package_file_at(const SolPackage *package, size_t aggregate_offset) {
    size_t low = 0;
    size_t high = package->file_count;
    while (low < high) {
        size_t middle = low + (high - low) / 2;
        const SolPackageFile *file = &package->files[middle];
        if (aggregate_offset < file->aggregate_start) {
            high = middle;
        } else if (aggregate_offset > file->aggregate_end) {
            low = middle + 1;
        } else {
            return file;
        }
    }
    return NULL;
}

static const char *sol_package_severity_name(SolSeverity severity) {
    return severity == SOL_SEVERITY_ERROR ? "error" : "warning";
}

static SolSpan sol_package_local_span(const SolPackageFile *file, SolSpan span) {
    size_t start = span.start < file->aggregate_start ? 0 : span.start - file->aggregate_start;
    size_t end = span.end < file->aggregate_start ? 0 : span.end - file->aggregate_start;
    if (start > file->source.length) start = file->source.length;
    if (end > file->source.length) end = file->source.length;
    return (SolSpan){.start = start, .end = end};
}

void sol_package_diagnostics_render_human(
    FILE *stream,
    const SolPackage *package,
    const SolDiagnostics *diagnostics
) {
    for (size_t index = 0; index < diagnostics->count; ++index) {
        const SolDiagnostic *diagnostic = &diagnostics->items[index];
        const SolPackageFile *file = sol_package_file_at(package, diagnostic->span.start);
        const SolSource *source = file == NULL ? &package->source : &file->source;
        SolSpan span = file == NULL
            ? diagnostic->span
            : sol_package_local_span(file, diagnostic->span);
        SolPosition position = sol_source_position(source, span.start);
        fprintf(
            stream,
            "%s:%zu:%zu: %s[%s]: %s\n",
            source->path,
            position.line,
            position.column,
            sol_package_severity_name(diagnostic->severity),
            diagnostic->code,
            diagnostic->message
        );
    }
}

static void sol_package_json_string(FILE *stream, const char *text) {
    fputc('"', stream);
    for (const unsigned char *cursor = (const unsigned char *)text; *cursor != '\0'; ++cursor) {
        switch (*cursor) {
            case '"': fputs("\\\"", stream); break;
            case '\\': fputs("\\\\", stream); break;
            case '\b': fputs("\\b", stream); break;
            case '\f': fputs("\\f", stream); break;
            case '\n': fputs("\\n", stream); break;
            case '\r': fputs("\\r", stream); break;
            case '\t': fputs("\\t", stream); break;
            default:
                if (*cursor < 0x20) fprintf(stream, "\\u%04x", (unsigned int)*cursor);
                else fputc((int)*cursor, stream);
        }
    }
    fputc('"', stream);
}

void sol_package_diagnostics_render_json(
    FILE *stream,
    const SolPackage *package,
    const SolDiagnostics *diagnostics
) {
    fputs("[", stream);
    for (size_t index = 0; index < diagnostics->count; ++index) {
        const SolDiagnostic *diagnostic = &diagnostics->items[index];
        const SolPackageFile *file = sol_package_file_at(package, diagnostic->span.start);
        const SolSource *source = file == NULL ? &package->source : &file->source;
        SolSpan span = file == NULL
            ? diagnostic->span
            : sol_package_local_span(file, diagnostic->span);
        SolPosition start = sol_source_position(source, span.start);
        SolPosition end = sol_source_position(source, span.end);
        if (index != 0) fputs(",", stream);
        fputs("{\"schema\":\"sol.diagnostic/1\",\"code\":", stream);
        sol_package_json_string(stream, diagnostic->code);
        fputs(",\"severity\":", stream);
        sol_package_json_string(stream, sol_package_severity_name(diagnostic->severity));
        fputs(",\"message\":", stream);
        sol_package_json_string(stream, diagnostic->message);
        fputs(",\"locations\":[{\"file\":", stream);
        sol_package_json_string(stream, source->path);
        fprintf(
            stream,
            ",\"start\":{\"line\":%zu,\"column\":%zu},"
            "\"end\":{\"line\":%zu,\"column\":%zu},\"role\":\"primary\"}]}",
            start.line,
            start.column,
            end.line,
            end.column
        );
    }
    fputs("]\n", stream);
}

#undef SOL_APPEND_ARENA
