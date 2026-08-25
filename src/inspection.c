#include "sol/inspection.h"
#include "sol/parser.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdlib.h>
#include "resource_internal.h"
#include <string.h>

typedef struct {
    char *data;
    size_t length;
    size_t capacity;
    bool failed;
} SolInspectionBuffer;

typedef struct {
    SolInspectionBuffer *output;
    const SolPackage *package;
    const SolSource *source;
    const SolSyntaxTree *syntax;
    const SolHirModule *hir;
    const SolTypeTable *types;
    const SolEffectTable *effects;
    const SolContractTable *contracts;
    const SolDiagnostics *diagnostics;
} SolInspector;

static bool sol_inspection_reserve(SolInspectionBuffer *buffer, size_t extra) {
    if (buffer->failed || buffer->length == SIZE_MAX
        || extra > SIZE_MAX - buffer->length - 1) {
        buffer->failed = true;
        return false;
    }
    size_t needed = buffer->length + extra + 1;
    if (needed <= buffer->capacity) return true;
    size_t capacity = buffer->capacity == 0 ? 4096 : buffer->capacity;
    while (capacity < needed) {
        if (capacity > SIZE_MAX / 2) {
            capacity = needed;
            break;
        }
        capacity *= 2;
    }
    char *grown = realloc(buffer->data, capacity);
    if (grown == NULL) {
        buffer->failed = true;
        return false;
    }
    buffer->data = grown;
    buffer->capacity = capacity;
    return true;
}

static void sol_inspection_bytes(SolInspectionBuffer *buffer, const char *data, size_t length) {
    if (!sol_inspection_reserve(buffer, length)) return;
    memcpy(buffer->data + buffer->length, data, length);
    buffer->length += length;
    buffer->data[buffer->length] = '\0';
}

static void sol_inspection_text(SolInspectionBuffer *buffer, const char *text) {
    sol_inspection_bytes(buffer, text, strlen(text));
}

static void sol_inspection_format(SolInspectionBuffer *buffer, const char *format, ...) {
    if (buffer->failed) return;
    va_list arguments;
    va_start(arguments, format);
    va_list copy;
    va_copy(copy, arguments);
    int count = vsnprintf(NULL, 0, format, copy);
    va_end(copy);
    if (count < 0 || !sol_inspection_reserve(buffer, (size_t)count)) {
        buffer->failed = true;
        va_end(arguments);
        return;
    }
    (void)vsnprintf(buffer->data + buffer->length,
        buffer->capacity - buffer->length, format, arguments);
    va_end(arguments);
    buffer->length += (size_t)count;
}

static size_t sol_inspection_utf8(const unsigned char *text, size_t remaining) {
    unsigned char a = text[0];
    if (a < 0x80) return 1;
    if (remaining >= 2 && a >= 0xc2 && a <= 0xdf
        && text[1] >= 0x80 && text[1] <= 0xbf) return 2;
    if (remaining >= 3 && ((a == 0xe0 && text[1] >= 0xa0 && text[1] <= 0xbf)
        || (a >= 0xe1 && a <= 0xec && text[1] >= 0x80 && text[1] <= 0xbf)
        || (a == 0xed && text[1] >= 0x80 && text[1] <= 0x9f)
        || (a >= 0xee && a <= 0xef && text[1] >= 0x80 && text[1] <= 0xbf))
        && text[2] >= 0x80 && text[2] <= 0xbf) return 3;
    if (remaining >= 4 && ((a == 0xf0 && text[1] >= 0x90 && text[1] <= 0xbf)
        || (a >= 0xf1 && a <= 0xf3 && text[1] >= 0x80 && text[1] <= 0xbf)
        || (a == 0xf4 && text[1] >= 0x80 && text[1] <= 0x8f))
        && text[2] >= 0x80 && text[2] <= 0xbf
        && text[3] >= 0x80 && text[3] <= 0xbf) return 4;
    return 0;
}

static void sol_inspection_string_bytes(
    SolInspectionBuffer *buffer, const char *text, size_t length
) {
    sol_inspection_text(buffer, "\"");
    size_t index = 0;
    while (index < length) {
        const unsigned char *bytes = (const unsigned char *)text + index;
        size_t utf8 = sol_inspection_utf8(bytes, length - index);
        if (bytes[0] >= 0x80 && utf8 != 0) {
            sol_inspection_bytes(buffer, (const char *)bytes, utf8);
            index += utf8;
        } else {
            switch (bytes[0]) {
                case '"': sol_inspection_text(buffer, "\\\""); break;
                case '\\': sol_inspection_text(buffer, "\\\\"); break;
                case '\b': sol_inspection_text(buffer, "\\b"); break;
                case '\f': sol_inspection_text(buffer, "\\f"); break;
                case '\n': sol_inspection_text(buffer, "\\n"); break;
                case '\r': sol_inspection_text(buffer, "\\r"); break;
                case '\t': sol_inspection_text(buffer, "\\t"); break;
                default:
                    if (bytes[0] < 0x20 || bytes[0] >= 0x80) {
                        sol_inspection_format(buffer, "\\u%04x", (unsigned int)bytes[0]);
                    } else sol_inspection_bytes(buffer, (const char *)bytes, 1);
            }
            ++index;
        }
    }
    sol_inspection_text(buffer, "\"");
}

static void sol_inspection_string(SolInspectionBuffer *buffer, const char *text) {
    sol_inspection_string_bytes(buffer, text, strlen(text));
}

static const SolPackageFile *sol_inspection_file(const SolInspector *inspector, size_t offset) {
    return inspector->package->is_directory
        ? sol_package_file_at(inspector->package, offset) : NULL;
}

static bool sol_inspection_path_separator(char character) {
    return character == '/' || character == '\\';
}

static bool sol_inspection_path_prefix(const char *path, const char *root, size_t length) {
    for (size_t index = 0; index < length; ++index) {
        if (path[index] == root[index]) continue;
        if (!sol_inspection_path_separator(path[index])
            || !sol_inspection_path_separator(root[index])) return false;
    }
    return true;
}

static bool sol_inspection_output_path_valid(const char *path) {
    if (*path == '\0' || sol_inspection_path_separator(*path)
        || (((path[0] >= 'A' && path[0] <= 'Z')
                || (path[0] >= 'a' && path[0] <= 'z')) && path[1] == ':')) return false;
    const char *segment = path;
    for (const char *cursor = path; ; ++cursor) {
        if (!sol_inspection_path_separator(*cursor) && *cursor != '\0') continue;
        if (cursor - segment == 2 && segment[0] == '.' && segment[1] == '.') return false;
        if (*cursor == '\0') return true;
        segment = cursor + 1;
    }
}

static const char *sol_inspection_path(const SolInspector *inspector, const SolPackageFile *file) {
    const char *path = file == NULL ? inspector->package->source.path : file->path;
    if (inspector->package->is_directory) {
        size_t root = strlen(inspector->package->path);
        while (root != 0 && sol_inspection_path_separator(
                inspector->package->path[root - 1])) --root;
        if (sol_inspection_path_prefix(path, inspector->package->path, root)
            && sol_inspection_path_separator(path[root])) {
            return path + root + 1;
        }
    }
    const char *base = path;
    for (const char *cursor = path; *cursor != '\0'; ++cursor) {
        if (sol_inspection_path_separator(*cursor)) base = cursor + 1;
    }
    return base;
}

static void sol_inspection_path_string(
    SolInspectionBuffer *buffer, const SolInspector *inspector, const SolPackageFile *file
) {
    const char *path = sol_inspection_path(inspector, file);
    size_t length = strlen(path);
    char *normalized = malloc(length + 1);
    if (normalized == NULL) {
        buffer->failed = true;
        return;
    }
    for (size_t index = 0; index < length; ++index) {
        normalized[index] = path[index] == '\\' ? '/' : path[index];
    }
    normalized[length] = '\0';
    sol_inspection_string_bytes(buffer, normalized, length);
    free(normalized);
}

static void sol_inspection_span(SolInspector *inspector, SolSpan span) {
    const SolPackageFile *file = sol_inspection_file(inspector, span.start);
    size_t base = file == NULL ? 0 : file->aggregate_start;
    size_t length = file == NULL ? inspector->source->length : file->source.length;
    size_t start = span.start < base ? 0 : span.start - base;
    size_t end = span.end < base ? 0 : span.end - base;
    if (start > length) start = length;
    if (end > length) end = length;
    sol_inspection_text(inspector->output, "{\"file\":");
    sol_inspection_path_string(inspector->output, inspector, file);
    sol_inspection_format(inspector->output, ",\"start\":%zu,\"end\":%zu}", start, end);
}

static void sol_inspection_source_span(SolInspector *inspector, SolSpan span) {
    size_t start = span.start > inspector->source->length ? inspector->source->length : span.start;
    size_t end = span.end > inspector->source->length ? inspector->source->length : span.end;
    if (end < start) end = start;
    sol_inspection_string_bytes(inspector->output, inspector->source->text + start, end - start);
}

static void sol_inspection_semantic_id(SolInspector *inspector, SolDefId definition) {
    if (definition >= inspector->hir->definition_count) {
        sol_inspection_text(inspector->output, "null");
        return;
    }
    SolSemanticId id = inspector->hir->definitions[definition].semantic_id;
    sol_inspection_format(inspector->output, "\"sem:1:%016llx%016llx\"",
        (unsigned long long)id.high, (unsigned long long)id.low);
}

static const char *sol_inspection_item_kind(SolItemKind kind) {
    static const char *const names[] = {
        "record", "enum", "type", "capability", "function", "trait",
        "implementation", "test"
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

static const char *sol_inspection_expr_kind(SolExprKind kind) {
    static const char *const names[] = {
        "error", "integer", "string", "bool", "unit", "path", "unary",
        "binary", "call", "field", "tuple", "record", "if", "match", "block",
        "propagate", "handle", "result", "old", "type_application"
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

static const char *sol_inspection_pattern_kind(SolPatternKind kind) {
    static const char *const names[] = {
        "wildcard", "variant", "bool", "binding", "record", "tuple"
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

static const char *sol_inspection_type_kind(SolTypeKind kind) {
    static const char *const names[] = {
        "unknown", "error", "int64", "bool", "text", "unit", "nominal",
        "application", "function", "function_signature", "capability_operation",
        "variant", "never", "parameter", "self", "trait_method"
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

static bool sol_inspection_type_has_semantic_definition(SolType type) {
    return type.kind == SOL_TYPE_NOMINAL || type.kind == SOL_TYPE_FUNCTION
        || type.kind == SOL_TYPE_SELF;
}

static void sol_inspection_type(SolInspector *inspector, SolType type) {
    sol_inspection_text(inspector->output, "{\"kind\":");
    sol_inspection_string(inspector->output, sol_inspection_type_kind(type.kind));
    sol_inspection_text(inspector->output, ",\"definition\":");
    if (sol_inspection_type_has_semantic_definition(type)) {
        sol_inspection_semantic_id(inspector, type.definition);
    } else sol_inspection_text(inspector->output, "null");
    sol_inspection_text(inspector->output, ",\"snapshotRef\":");
    if (type.kind == SOL_TYPE_APPLICATION) {
        sol_inspection_format(inspector->output, "\"type-app:%zu\"", type.definition);
    } else if (type.kind == SOL_TYPE_FUNCTION_SIGNATURE) {
        sol_inspection_format(inspector->output, "\"signature:%zu\"", type.definition);
    } else if (type.kind == SOL_TYPE_CAPABILITY_OPERATION) {
        sol_inspection_format(inspector->output, "\"capability-member:%zu\"", type.definition);
    } else if (type.kind == SOL_TYPE_VARIANT) {
        sol_inspection_format(inspector->output, "\"variant:%zu\"", type.definition);
    } else if (type.kind == SOL_TYPE_TRAIT_METHOD) {
        sol_inspection_format(inspector->output, "\"trait-method:%zu\"", type.definition);
    } else if (type.kind == SOL_TYPE_PARAMETER) {
        sol_inspection_format(inspector->output, "\"type-parameter:%zu\"", type.definition);
    } else sol_inspection_text(inspector->output, "null");
    sol_inspection_text(inspector->output, "}");
}

static const char *sol_inspection_authority_kind(SolEffectAtomArgumentKind kind) {
    static const char *const names[] = {"none", "static", "parameter", "self"};
    return (size_t)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

static void sol_inspection_atom(SolInspector *inspector, const SolEffectAtom *atom) {
    sol_inspection_text(inspector->output, "{\"name\":");
    sol_inspection_source_span(inspector, atom->name);
    sol_inspection_text(inspector->output, ",\"authority\":{\"kind\":");
    sol_inspection_string(inspector->output, sol_inspection_authority_kind(atom->argument_kind));
    sol_inspection_text(inspector->output, ",\"parameter\":");
    if (atom->argument_kind == SOL_EFFECT_ATOM_PARAMETER
        && atom->parameter < inspector->syntax->parameter_count) {
        sol_inspection_source_span(inspector, inspector->syntax->parameters[atom->parameter].name);
    } else sol_inspection_text(inspector->output, "null");
    sol_inspection_text(inspector->output, "}}");
}

static void sol_inspection_atoms(
    SolInspector *inspector, const SolEffectAtom *atoms, size_t count
) {
    sol_inspection_text(inspector->output, "[");
    for (size_t index = 0; index < count; ++index) {
        if (index != 0) sol_inspection_text(inspector->output, ",");
        sol_inspection_atom(inspector, &atoms[index]);
    }
    sol_inspection_text(inspector->output, "]");
}

static void sol_inspection_base64(
    SolInspectionBuffer *output, const unsigned char *bytes, size_t length
) {
    static const char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    sol_inspection_text(output, "\"");
    for (size_t index = 0; index < length; index += 3) {
        uint32_t value = (uint32_t)bytes[index] << 16;
        size_t remaining = length - index;
        if (remaining > 1) value |= (uint32_t)bytes[index + 1] << 8;
        if (remaining > 2) value |= bytes[index + 2];
        char encoded[4] = {
            alphabet[(value >> 18) & 63], alphabet[(value >> 12) & 63],
            remaining > 1 ? alphabet[(value >> 6) & 63] : '=',
            remaining > 2 ? alphabet[value & 63] : '='
        };
        sol_inspection_bytes(output, encoded, sizeof(encoded));
    }
    sol_inspection_text(output, "\"");
}

static void sol_inspection_syntax(SolInspector *inspector) {
    SolInspectionBuffer *out = inspector->output;
    sol_inspection_text(out, "\"syntax\":{\"schema\":\"sol.inspection.syntax\",\"version\":3,\"edition\":");
    sol_inspection_format(out, "%u,\"files\":[", inspector->syntax->edition);
    size_t file_count = inspector->package->is_directory ? inspector->package->file_count : 1;
    for (size_t index = 0; index < file_count; ++index) {
        const SolPackageFile *file = inspector->package->is_directory
            ? &inspector->package->files[index] : NULL;
        const SolSource *source = file == NULL ? &inspector->package->source : &file->source;
        SolSpan module = file == NULL ? inspector->syntax->module_name : file->module_name;
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_text(out, "{\"path\":");
        sol_inspection_path_string(out, inspector, file);
        sol_inspection_format(out, ",\"byteLength\":%zu,\"sourceBase64\":", source->length);
        sol_inspection_base64(out, (const unsigned char *)source->text, source->length);
        sol_inspection_text(out, ",\"module\":");
        sol_inspection_source_span(inspector, module);
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"declarations\":[");
    for (size_t index = 0; index < inspector->syntax->item_count; ++index) {
        const SolSyntaxItem *item = &inspector->syntax->items[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"id\":\"declaration:%zu\",\"semanticId\":", index);
        sol_inspection_semantic_id(inspector, index);
        sol_inspection_text(out, ",\"kind\":");
        sol_inspection_string(out, sol_inspection_item_kind(item->kind));
        sol_inspection_text(out, ",\"name\":"); sol_inspection_source_span(inspector, item->name);
        sol_inspection_text(out, ",\"span\":"); sol_inspection_span(inspector, item->span);
        sol_inspection_text(out, ",\"visibility\":");
        sol_inspection_string(out, item->is_public ? "public" : "private");
        sol_inspection_text(out, ",\"stableToken\":");
        if (item->stable_identity.start != item->stable_identity.end) {
            sol_inspection_source_span(inspector, item->stable_identity);
        } else sol_inspection_text(out, "null");
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"expressions\":[");
    for (size_t index = 0; index < inspector->syntax->expression_count; ++index) {
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"id\":\"expr:%zu\",\"kind\":", index);
        sol_inspection_string(out, sol_inspection_expr_kind(inspector->syntax->expressions[index].kind));
        sol_inspection_text(out, ",\"span\":");
        sol_inspection_span(inspector, inspector->syntax->expressions[index].span);
        if (inspector->syntax->expressions[index].kind == SOL_EXPR_FIELD) {
            sol_inspection_format(out, ",\"base\":\"expr:%zu\"",
                inspector->syntax->expressions[index].as.field.base);
        } else if (inspector->syntax->expressions[index].kind == SOL_EXPR_MATCH) {
            const SolExpr *expression = &inspector->syntax->expressions[index];
            sol_inspection_format(out, ",\"scrutinee\":\"expr:%zu\",\"arms\":[",
                expression->as.match_expr.scrutinee);
            SolMatchArmId arm = expression->as.match_expr.first_arm;
            size_t emitted = 0;
            while (arm != SOL_AST_NONE) {
                if (emitted++ != 0) sol_inspection_text(out, ",");
                sol_inspection_format(out, "\"match-arm:%zu\"", arm);
                arm = inspector->syntax->match_arms[arm].next;
            }
            sol_inspection_text(out, "]");
        }
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"patterns\":[");
    for (size_t index = 0; index < inspector->syntax->pattern_count; ++index) {
        const SolPattern *pattern = &inspector->syntax->patterns[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"id\":\"pattern:%zu\",\"kind\":", index);
        sol_inspection_string(out, sol_inspection_pattern_kind(pattern->kind));
        sol_inspection_text(out, ",\"span\":"); sol_inspection_span(inspector, pattern->span);
        sol_inspection_text(out, ",\"name\":");
        if (pattern->kind == SOL_PATTERN_VARIANT || pattern->kind == SOL_PATTERN_BINDING
            || pattern->kind == SOL_PATTERN_RECORD) {
            sol_inspection_source_span(inspector, pattern->name);
        } else sol_inspection_text(out, "null");
        sol_inspection_text(out, ",\"boolValue\":");
        if (pattern->kind == SOL_PATTERN_BOOL) {
            sol_inspection_text(out, pattern->bool_value ? "true" : "false");
        } else sol_inspection_text(out, "null");
        sol_inspection_text(out, ",\"children\":[");
        SolPatternBindingId child = pattern->first_binding;
        size_t emitted = 0;
        while (child != SOL_AST_NONE) {
            const SolPatternBinding *edge = &inspector->syntax->pattern_bindings[child];
            if (emitted++ != 0) sol_inspection_text(out, ",");
            sol_inspection_format(out, "{\"pattern\":\"pattern:%zu\",\"field\":",
                edge->pattern);
            if (pattern->kind == SOL_PATTERN_RECORD) {
                sol_inspection_source_span(inspector, edge->field);
            } else sol_inspection_text(out, "null");
            sol_inspection_text(out, "}");
            child = edge->next;
        }
        sol_inspection_text(out, "]}");
    }
    sol_inspection_text(out, "],\"matchArms\":[");
    for (size_t index = 0; index < inspector->syntax->match_arm_count; ++index) {
        const SolMatchArm *arm = &inspector->syntax->match_arms[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"id\":\"match-arm:%zu\",\"pattern\":\"pattern:%zu\","
            "\"guard\":", index, arm->pattern);
        if (arm->guard == SOL_AST_NONE) sol_inspection_text(out, "null");
        else sol_inspection_format(out, "\"expr:%zu\"", arm->guard);
        sol_inspection_format(out, ",\"value\":\"expr:%zu\",\"span\":", arm->value);
        sol_inspection_span(inspector, arm->span);
        sol_inspection_text(out, "}");
    }
    sol_inspection_format(out,
        "],\"arenaCounts\":{\"imports\":%zu,\"statements\":%zu,\"arguments\":%zu,"
        "\"parameters\":%zu,\"types\":%zu,\"typeArguments\":%zu,\"typeParameters\":%zu,"
        "\"effectParameters\":%zu,\"fields\":%zu,\"variants\":%zu,\"patterns\":%zu,"
        "\"patternBindings\":%zu,\"matchArms\":%zu,\"effects\":%zu,"
        "\"capabilityMembers\":%zu,\"traitMethods\":%zu,\"contractClauses\":%zu,"
        "\"contractConditions\":%zu}}",
        inspector->syntax->import_count, inspector->syntax->statement_count,
        inspector->syntax->argument_count, inspector->syntax->parameter_count,
        inspector->syntax->type_count, inspector->syntax->type_argument_count,
        inspector->syntax->type_parameter_count, inspector->syntax->effect_parameter_count,
        inspector->syntax->field_count, inspector->syntax->variant_count,
        inspector->syntax->pattern_count, inspector->syntax->pattern_binding_count,
        inspector->syntax->match_arm_count, inspector->syntax->effect_count,
        inspector->syntax->capability_member_count, inspector->syntax->trait_method_count,
        inspector->syntax->contract_clause_count, inspector->syntax->contract_condition_count);
}

static const char *sol_inspection_resolution_kind(SolResolutionKind kind) {
    static const char *const names[] = {
        "not_applicable", "error", "definition", "local", "builtin", "refinement_self"
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

static const char *sol_inspection_builtin(SolBuiltin builtin) {
    static const char *const names[] = {"ok", "err", "some", "none"};
    return (size_t)builtin < sizeof(names) / sizeof(names[0]) ? names[builtin] : "unknown";
}

static const char *sol_inspection_type_builtin(SolTypeBuiltin builtin) {
    static const char *const names[] = {"int64", "bool", "text", "option", "result"};
    return (size_t)builtin < sizeof(names) / sizeof(names[0]) ? names[builtin] : "unknown";
}

static const char *sol_inspection_reference_kind(SolSemanticReferenceKind kind) {
    static const char *const names[] = {
        "declaration", "import", "expression", "type", "trait", "bound"
    };
    return (size_t)kind < sizeof(names) / sizeof(names[0]) ? names[kind] : "unknown";
}

static const char *sol_inspection_access(SolAccessMode access) {
    static const char *const names[] = {"owned", "shared", "exclusive"};
    return (size_t)access < sizeof(names) / sizeof(names[0])
        ? names[access] : "unknown";
}

static void sol_inspection_hir(SolInspector *inspector) {
    SolInspectionBuffer *out = inspector->output;
    sol_inspection_text(out, "\"hir\":{\"schema\":\"sol.inspection.hir\",\"version\":1,\"definitions\":[");
    for (size_t index = 0; index < inspector->hir->definition_count; ++index) {
        const SolHirDefinition *definition = &inspector->hir->definitions[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_text(out, "{\"semanticId\":"); sol_inspection_semantic_id(inspector, index);
        sol_inspection_text(out, ",\"kind\":"); sol_inspection_string(out, sol_inspection_item_kind(definition->kind));
        sol_inspection_text(out, ",\"name\":"); sol_inspection_source_span(inspector, definition->name);
        sol_inspection_format(out, ",\"syntaxDeclaration\":\"declaration:%zu\"}", definition->syntax_item);
    }
    sol_inspection_text(out, "],\"locals\":[");
    static const char *const local_kinds[] = {"parameter", "binding", "pattern"};
    for (size_t index = 0; index < inspector->hir->local_count; ++index) {
        const SolHirLocal *local = &inspector->hir->locals[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"id\":\"local:%zu\",\"kind\":", index);
        sol_inspection_string(out, (size_t)local->kind < 3 ? local_kinds[local->kind] : "unknown");
        sol_inspection_text(out, ",\"name\":"); sol_inspection_source_span(inspector, local->name);
        sol_inspection_text(out, ",\"owner\":"); sol_inspection_semantic_id(inspector, local->owner);
        sol_inspection_text(out, ",\"access\":");
        sol_inspection_string(out, sol_inspection_access(local->access));
        if (local->mutable) sol_inspection_text(out, ",\"mutable\":true");
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"expressionResolutions\":[");
    for (size_t index = 0; index < inspector->hir->resolution_count; ++index) {
        SolResolution resolution = inspector->hir->resolutions[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"expression\":\"expr:%zu\",\"kind\":", index);
        sol_inspection_string(out, sol_inspection_resolution_kind(resolution.kind));
        sol_inspection_text(out, ",\"target\":");
        if (resolution.kind == SOL_RESOLUTION_DEFINITION) sol_inspection_semantic_id(inspector, resolution.target);
        else if (resolution.kind == SOL_RESOLUTION_LOCAL) sol_inspection_format(out, "\"local:%zu\"", resolution.target);
        else if (resolution.kind == SOL_RESOLUTION_BUILTIN) {
            sol_inspection_text(out, "\"builtin:");
            sol_inspection_text(out, sol_inspection_builtin((SolBuiltin)resolution.target));
            sol_inspection_text(out, "\"");
        }
        else sol_inspection_text(out, "null");
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"typeResolutions\":[");
    static const char *const type_resolution_kinds[] = {"error", "builtin", "definition", "parameter", "self"};
    for (size_t index = 0; index < inspector->hir->type_resolution_count; ++index) {
        SolTypeResolution resolution = inspector->hir->type_resolutions[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"syntaxType\":\"syntax-type:%zu\",\"kind\":", index);
        sol_inspection_string(out, (size_t)resolution.kind < 5
            ? type_resolution_kinds[resolution.kind] : "unknown");
        sol_inspection_text(out, ",\"target\":");
        if (resolution.kind == SOL_TYPE_RESOLUTION_DEFINITION) sol_inspection_semantic_id(inspector, resolution.target);
        else if (resolution.kind == SOL_TYPE_RESOLUTION_PARAMETER) sol_inspection_format(out, "\"type-parameter:%zu\"", resolution.target);
        else if (resolution.kind == SOL_TYPE_RESOLUTION_BUILTIN) {
            sol_inspection_text(out, "\"builtin-type:");
            sol_inspection_text(out, sol_inspection_type_builtin((SolTypeBuiltin)resolution.target));
            sol_inspection_text(out, "\"");
        }
        else sol_inspection_text(out, "null");
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"effectResolutions\":[");
    size_t emitted = 0;
    for (size_t group = 0; group < 2; ++group) {
        const SolEffectResolution *rows = group == 0 ? inspector->hir->effect_resolutions
            : inspector->hir->type_effect_resolutions;
        size_t count = group == 0 ? inspector->hir->effect_resolution_count
            : inspector->hir->type_effect_resolution_count;
        for (size_t index = 0; index < count; ++index) {
            if (emitted++ != 0) sol_inspection_text(out, ",");
            const char *kind = rows[index].kind == SOL_EFFECT_RESOLUTION_ATOM ? "atom"
                : rows[index].kind == SOL_EFFECT_RESOLUTION_PARAMETER ? "parameter" : "error";
            sol_inspection_text(out, "{\"ownerKind\":");
            sol_inspection_string(out, group == 0 ? "effect" : "syntax_type");
            sol_inspection_format(out, ",\"ownerIndex\":%zu,\"kind\":", index);
            sol_inspection_string(out, kind);
            sol_inspection_text(out, ",\"snapshotTarget\":");
            if (rows[index].kind == SOL_EFFECT_RESOLUTION_PARAMETER) {
                sol_inspection_format(out, "\"effect-parameter:%zu\"}", rows[index].target);
            } else sol_inspection_text(out, "null}");
        }
    }
    sol_inspection_text(out, "],\"occurrences\":[");
    for (size_t index = 0; index < inspector->hir->semantic_reference_count; ++index) {
        const SolSemanticReference *reference = &inspector->hir->semantic_references[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_text(out, "{\"kind\":"); sol_inspection_string(out, sol_inspection_reference_kind(reference->kind));
        sol_inspection_text(out, ",\"span\":"); sol_inspection_span(inspector, reference->span);
        sol_inspection_text(out, ",\"target\":");
        sol_inspection_format(out, "\"sem:1:%016llx%016llx\"}",
            (unsigned long long)reference->target_id.high,
            (unsigned long long)reference->target_id.low);
    }
    sol_inspection_text(out, "]}");
}

static void sol_inspection_type_facts(
    SolInspector *inspector, const char *name, const char *prefix,
    const SolType *facts, size_t count, bool semantic_ids
) {
    SolInspectionBuffer *out = inspector->output;
    sol_inspection_string(out, name); sol_inspection_text(out, ":[");
    for (size_t index = 0; index < count; ++index) {
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_text(out, "{\"subject\":");
        if (semantic_ids) sol_inspection_semantic_id(inspector, index);
        else sol_inspection_format(out, "\"%s:%zu\"", prefix, index);
        sol_inspection_text(out, ",\"type\":"); sol_inspection_type(inspector, facts[index]);
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "]");
}

static bool sol_inspection_field_parent(
    const SolSyntaxTree *syntax,
    SolFieldId target,
    bool *is_variant,
    SolDefId *owner,
    SolVariantId *variant,
    size_t *ordinal
) {
    for (SolDefId item = 0; item < syntax->item_count; ++item) {
        if (syntax->items[item].kind == SOL_ITEM_RECORD) {
            size_t index = 0;
            for (SolFieldId field = syntax->items[item].first_field;
                field != SOL_AST_NONE; field = syntax->fields[field].next, ++index) {
                if (field == target) {
                    *is_variant = false; *owner = item; *variant = SOL_AST_NONE;
                    *ordinal = index;
                    return true;
                }
            }
        } else if (syntax->items[item].kind == SOL_ITEM_ENUM) {
            for (SolVariantId entry = syntax->items[item].first_variant;
                entry != SOL_AST_NONE; entry = syntax->variants[entry].next) {
                size_t index = 0;
                for (SolFieldId field = syntax->variants[entry].first_field;
                    field != SOL_AST_NONE; field = syntax->fields[field].next, ++index) {
                    if (field == target) {
                        *is_variant = true; *owner = item; *variant = entry;
                        *ordinal = index;
                        return true;
                    }
                }
            }
        }
    }
    return false;
}

static void sol_inspection_types(SolInspector *inspector) {
    SolInspectionBuffer *out = inspector->output;
    sol_inspection_text(out, "\"types\":{\"schema\":\"sol.inspection.types\",\"version\":3,");
    sol_inspection_type_facts(inspector, "expressions", "expr", inspector->types->expressions,
        inspector->types->expression_count, false); sol_inspection_text(out, ",");
    sol_inspection_type_facts(inspector, "locals", "local", inspector->types->locals,
        inspector->types->local_count, false); sol_inspection_text(out, ",");
    sol_inspection_type_facts(inspector, "definitions", "definition", inspector->types->definitions,
        inspector->types->definition_count, true); sol_inspection_text(out, ",");
    sol_inspection_type_facts(inspector, "declaredSyntaxTypes", "syntax-type",
        inspector->types->declared_types, inspector->types->declared_type_count, false);
    sol_inspection_text(out, ",\"applications\":[");
    for (size_t index = 0; index < inspector->types->type_application_count; ++index) {
        const SolTypeApplication *application = &inspector->types->type_applications[index];
        if (index != 0) sol_inspection_text(out, ",");
        const char *constructor = application->constructor == SOL_TYPE_CONSTRUCTOR_OPTION ? "option"
            : application->constructor == SOL_TYPE_CONSTRUCTOR_RESULT ? "result"
            : application->constructor == SOL_TYPE_CONSTRUCTOR_TUPLE ? "tuple" : "user";
        sol_inspection_format(out, "{\"id\":\"type-app:%zu\",\"constructor\":", index);
        sol_inspection_string(out, constructor); sol_inspection_text(out, ",\"definition\":");
        if (application->constructor == SOL_TYPE_CONSTRUCTOR_USER) {
            sol_inspection_semantic_id(inspector, application->definition);
        } else sol_inspection_text(out, "null");
        sol_inspection_text(out, ",\"arguments\":[");
        for (size_t argument = 0; argument < application->argument_count; ++argument) {
            if (argument != 0) sol_inspection_text(out, ",");
            sol_inspection_type(inspector, inspector->types->type_application_arguments[
                application->argument_offset + argument]);
        }
        sol_inspection_text(out, "]}");
    }
    sol_inspection_text(out, "],\"functionSignatures\":[");
    for (size_t index = 0; index < inspector->types->function_type_count; ++index) {
        const SolFunctionType *function = &inspector->types->function_types[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"id\":\"signature:%zu\",\"parameters\":[", index);
        for (size_t parameter = 0; parameter < function->parameter_count; ++parameter) {
            if (parameter != 0) sol_inspection_text(out, ",");
            sol_inspection_type(inspector, function->parameters[parameter]);
        }
        sol_inspection_text(out, "],\"accesses\":[");
        for (size_t parameter = 0; parameter < function->parameter_count; ++parameter) {
            if (parameter != 0) sol_inspection_text(out, ",");
            sol_inspection_string(out, sol_inspection_access(function->accesses[parameter]));
        }
        sol_inspection_text(out, "],\"result\":"); sol_inspection_type(inspector, function->result);
        sol_inspection_text(out, ",\"effects\":");
        sol_inspection_atoms(inspector, function->effects.atoms, function->effects.count);
        sol_inspection_text(out, ",\"rowTail\":");
        if (function->effect_parameter == SOL_AST_NONE) sol_inspection_text(out, "null");
        else sol_inspection_format(out, "%zu", function->effect_parameter);
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"provenanceRoots\":[");
    for (size_t index = 0; index < inspector->types->provenance_count; ++index) {
        const SolProvenanceSet *set = &inspector->types->provenances[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"id\":\"provenance:%zu\",\"parameterIndexes\":[", index);
        for (size_t root = 0; root < set->root_count; ++root) {
            if (root != 0) sol_inspection_text(out, ",");
            sol_inspection_format(out, "%zu", inspector->types->provenance_roots[set->root_offset + root]);
        }
        sol_inspection_text(out, "]}");
    }
    sol_inspection_text(out, "],\"callInstantiations\":[");
    size_t call_count = 0;
    for (size_t index = 0; index < inspector->types->call_instantiation_count; ++index) {
        const SolCallInstantiation *call = &inspector->types->call_instantiations[index];
        if (call->function == SOL_AST_NONE) continue;
        if (call_count++ != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"call\":\"expr:%zu\",\"function\":", index);
        sol_inspection_semantic_id(inspector, call->function); sol_inspection_text(out, ",\"arguments\":[");
        for (size_t argument = 0; argument < call->argument_count; ++argument) {
            if (argument != 0) sol_inspection_text(out, ",");
            sol_inspection_type(inspector, inspector->types->call_instantiation_arguments[
                call->argument_offset + argument]);
        }
        sol_inspection_text(out, "]}");
    }
    sol_inspection_text(out, "],\"coercions\":[");
    for (size_t index = 0; index < inspector->types->function_coercion_count; ++index) {
        if (index != 0) sol_inspection_text(out, ",");
        const SolFunctionCoercion *coercion = &inspector->types->function_coercions[index];
        sol_inspection_format(out, "{\"expression\":\"expr:%zu\",\"expected\":", coercion->expression);
        sol_inspection_type(inspector, coercion->expected); sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"handlers\":[");
    size_t emitted = 0;
    for (size_t index = 0; index < inspector->types->handler_count; ++index) {
        const SolHandler *handler = &inspector->types->handlers[index];
        if (handler->source_member == SOL_AST_NONE) continue;
        if (emitted++ != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"expression\":\"expr:%zu\",\"sourceMember\":%zu,"
            "\"providerMember\":%zu,\"rootParameter\":%zu}", index,
            handler->source_member, handler->provider_member, handler->root);
    }
    sol_inspection_text(out, "],\"methodResolutions\":["); emitted = 0;
    for (size_t index = 0; index < inspector->types->method_resolution_count; ++index) {
        const SolMethodResolution *method = &inspector->types->method_resolutions[index];
        if (method->kind == SOL_METHOD_RESOLUTION_NONE) continue;
        if (emitted++ != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"call\":\"expr:%zu\",\"kind\":", index);
        sol_inspection_string(out, method->kind == SOL_METHOD_RESOLUTION_REQUIREMENT
            ? "requirement" : "implementation");
        sol_inspection_text(out, ",\"trait\":"); sol_inspection_semantic_id(inspector, method->trait);
        sol_inspection_text(out, ",\"implementation\":");
        if (method->kind == SOL_METHOD_RESOLUTION_IMPLEMENTATION) {
            sol_inspection_semantic_id(inspector, method->implementation);
        } else sol_inspection_text(out, "null");
        sol_inspection_format(out, ",\"requirementIndex\":%zu,\"methodIndex\":%zu}",
            method->requirement, method->method);
    }
    sol_inspection_text(out, "],\"memberResolutions\":{\"fields\":["); emitted = 0;
    for (size_t index = 0; index < inspector->types->member_resolution_count; ++index) {
        if (inspector->types->field_resolutions[index] == SOL_AST_NONE) continue;
        if (emitted++ != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"expression\":\"expr:%zu\",\"fieldIndex\":%zu}",
            index, inspector->types->field_resolutions[index]);
    }
    sol_inspection_text(out, "],\"variants\":["); emitted = 0;
    for (size_t index = 0; index < inspector->types->member_resolution_count; ++index) {
        if (inspector->types->variant_resolutions[index] == SOL_AST_NONE) continue;
        if (emitted++ != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"expression\":\"expr:%zu\",\"variantIndex\":%zu}",
            index, inspector->types->variant_resolutions[index]);
    }
    sol_inspection_text(out, "]},\"tupleProjections\":[");
    for (size_t index = 0; index < inspector->types->tuple_projection_count; ++index) {
        if (index != 0) sol_inspection_text(out, ",");
        size_t ordinal = inspector->types->tuple_projections[index];
        if (ordinal == SOL_AST_NONE) sol_inspection_text(out, "null");
        else sol_inspection_format(out, "%zu", ordinal);
    }
    sol_inspection_text(out, "],\"representations\":["); emitted = 0;
    for (size_t index = 0; index < inspector->types->representation_count; ++index) {
        const SolTypeRepresentation *representation = &inspector->types->representations[index];
        if (representation->flavor == SOL_TYPE_DECLARATION_NONE) continue;
        if (emitted++ != 0) sol_inspection_text(out, ",");
        sol_inspection_text(out, "{\"definition\":"); sol_inspection_semantic_id(inspector, index);
        const char *flavor = representation->flavor == SOL_TYPE_DECLARATION_DISTINCT
            ? "distinct" : "refined";
        sol_inspection_text(out, ",\"flavor\":"); sol_inspection_string(out, flavor);
        sol_inspection_text(out, ",\"type\":"); sol_inspection_type(inspector, representation->representation);
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"constructions\":["); emitted = 0;
    for (size_t index = 0; index < inspector->types->construction_count; ++index) {
        const SolTypeConstruction *construction = &inspector->types->constructions[index];
        if (construction->definition == SOL_AST_NONE) continue;
        if (emitted++ != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"expression\":\"expr:%zu\",\"definition\":", index);
        sol_inspection_semantic_id(inspector, construction->definition);
        sol_inspection_text(out, ",\"representation\":"); sol_inspection_type(inspector, construction->representation);
        sol_inspection_text(out, ",\"result\":"); sol_inspection_type(inspector, construction->result);
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"provenanceMappings\":{\"expressionCapability\":[");
    for (size_t index = 0; index < inspector->types->expression_count; ++index) {
        if (index != 0) sol_inspection_text(out, ",");
        SolProvenanceId id = inspector->types->expression_capability_origins[index];
        if (id == SOL_PROVENANCE_NONE) sol_inspection_text(out, "null");
        else sol_inspection_format(out, "\"provenance:%zu\"", id);
    }
    sol_inspection_text(out, "],\"expressionOperation\":[");
    for (size_t index = 0; index < inspector->types->expression_count; ++index) {
        if (index != 0) sol_inspection_text(out, ",");
        SolProvenanceId id = inspector->types->expression_operation_origins[index];
        if (id == SOL_PROVENANCE_NONE) sol_inspection_text(out, "null");
        else sol_inspection_format(out, "\"provenance:%zu\"", id);
    }
    sol_inspection_text(out, "],\"localCapability\":[");
    for (size_t index = 0; index < inspector->types->local_count; ++index) {
        if (index != 0) sol_inspection_text(out, ",");
        SolProvenanceId id = inspector->types->local_capability_origins[index];
        if (id == SOL_PROVENANCE_NONE) sol_inspection_text(out, "null");
        else sol_inspection_format(out, "\"provenance:%zu\"", id);
    }
    sol_inspection_text(out, "],\"localOperation\":[");
    for (size_t index = 0; index < inspector->types->local_count; ++index) {
        if (index != 0) sol_inspection_text(out, ",");
        SolProvenanceId id = inspector->types->local_operation_origins[index];
        if (id == SOL_PROVENANCE_NONE) sol_inspection_text(out, "null");
        else sol_inspection_format(out, "\"provenance:%zu\"", id);
    }
    sol_inspection_text(out, "]},\"variantConstructors\":[");
    for (size_t index = 0; index < inspector->types->variant_constructor_count; ++index) {
        if (index != 0) sol_inspection_text(out, ",");
        const SolVariantConstructor *variant = &inspector->types->variant_constructors[index];
        sol_inspection_format(out, "{\"variantIndex\":%zu,\"owner\":", variant->variant);
        sol_inspection_type(inspector, variant->owner); sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"variants\":[");
    for (size_t index = 0; index < inspector->syntax->variant_count; ++index) {
        const SolVariant *variant = &inspector->syntax->variants[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"id\":\"variant:%zu\",\"owner\":", index);
        sol_inspection_semantic_id(inspector, variant->owner_item);
        sol_inspection_text(out, ",\"name\":");
        sol_inspection_source_span(inspector, variant->name);
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"fields\":[");
    for (size_t index = 0; index < inspector->syntax->field_count; ++index) {
        bool is_variant = false;
        SolDefId owner = SOL_AST_NONE;
        SolVariantId variant = SOL_AST_NONE;
        size_t ordinal = 0;
        (void)sol_inspection_field_parent(inspector->syntax, index,
            &is_variant, &owner, &variant, &ordinal);
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"id\":\"field:%zu\",\"parentKind\":", index);
        sol_inspection_string(out, is_variant ? "variant" : "record");
        sol_inspection_text(out, ",\"parent\":");
        if (is_variant) sol_inspection_format(out, "\"variant:%zu\"", variant);
        else sol_inspection_semantic_id(inspector, owner);
        sol_inspection_text(out, ",\"owner\":");
        sol_inspection_semantic_id(inspector, owner);
        sol_inspection_text(out, ",\"name\":");
        sol_inspection_source_span(inspector, inspector->syntax->fields[index].name);
        sol_inspection_format(out, ",\"ordinal\":%zu,\"ownerTypeParameters\":[", ordinal);
        SolTypeParameterId parameter = inspector->syntax->items[owner].first_type_parameter;
        size_t parameter_count = 0;
        while (parameter != SOL_AST_NONE) {
            if (parameter_count++ != 0) sol_inspection_text(out, ",");
            sol_inspection_format(out, "\"type-parameter:%zu\"", parameter);
            parameter = inspector->syntax->type_parameters[parameter].next;
        }
        sol_inspection_text(out, "],\"type\":");
        sol_inspection_type(inspector, inspector->types->declared_types[
            inspector->syntax->fields[index].type]);
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"patterns\":[");
    for (size_t index = 0; index < inspector->types->pattern_resolution_count; ++index) {
        const SolPattern *pattern = &inspector->syntax->patterns[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"subject\":\"pattern:%zu\",\"type\":", index);
        sol_inspection_type(inspector, inspector->types->pattern_types[index]);
        sol_inspection_text(out, ",\"variant\":");
        size_t variant = inspector->types->pattern_variant_resolutions[index];
        if (variant == SOL_AST_NONE) sol_inspection_text(out, "null");
        else sol_inspection_format(out, "\"variant:%zu\"", variant);
        sol_inspection_text(out, ",\"children\":[");
        SolPatternBindingId child = pattern->first_binding;
        SolFieldId variant_field = variant == SOL_AST_NONE ? SOL_AST_NONE
            : inspector->syntax->variants[variant].first_field;
        size_t emitted_child = 0;
        while (child != SOL_AST_NONE) {
            const SolPatternBinding *edge = &inspector->syntax->pattern_bindings[child];
            if (emitted_child++ != 0) sol_inspection_text(out, ",");
            sol_inspection_format(out, "{\"pattern\":\"pattern:%zu\",\"type\":",
                edge->pattern);
            sol_inspection_type(inspector, inspector->types->pattern_types[edge->pattern]);
            sol_inspection_text(out, ",\"field\":");
            SolFieldId field = pattern->kind == SOL_PATTERN_RECORD
                ? inspector->types->pattern_field_resolutions[child] : variant_field;
            if (field == SOL_AST_NONE) sol_inspection_text(out, "null");
            else sol_inspection_format(out, "\"field:%zu\"", field);
            sol_inspection_text(out, ",\"tupleOrdinal\":");
            size_t ordinal = inspector->types->pattern_tuple_ordinals[child];
            if (ordinal == SOL_AST_NONE) sol_inspection_text(out, "null");
            else sol_inspection_format(out, "%zu", ordinal);
            sol_inspection_text(out, "}");
            if (variant_field != SOL_AST_NONE) {
                variant_field = inspector->syntax->fields[variant_field].next;
            }
            child = edge->next;
        }
        sol_inspection_text(out, "]}");
    }
    sol_inspection_text(out, "],\"argumentFieldResolutions\":[");
    for (size_t index = 0; index < inspector->types->argument_resolution_count; ++index) {
        if (index != 0) sol_inspection_text(out, ",");
        size_t value = inspector->types->argument_field_resolutions[index];
        if (value == SOL_AST_NONE) sol_inspection_text(out, "null");
        else sol_inspection_format(out, "%zu", value);
    }
    sol_inspection_text(out, "],\"implementationTargets\":["); emitted = 0;
    for (size_t index = 0; index < inspector->types->implementation_target_count; ++index) {
        if (inspector->syntax->items[index].kind != SOL_ITEM_IMPLEMENTATION) continue;
        if (emitted++ != 0) sol_inspection_text(out, ",");
        sol_inspection_text(out, "{\"definition\":");
        sol_inspection_semantic_id(inspector, index);
        sol_inspection_text(out, ",\"target\":");
        sol_inspection_type(inspector, inspector->types->implementation_targets[index]);
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "]}");
}

static void sol_inspection_effect_row(
    SolInspector *inspector, const char *owner_kind, size_t owner, const SolEffectRow *row
) {
    sol_inspection_text(inspector->output, "{\"ownerKind\":");
    sol_inspection_string(inspector->output, owner_kind);
    sol_inspection_text(inspector->output, ",\"owner\":");
    if (strcmp(owner_kind, "function") == 0) sol_inspection_semantic_id(inspector, owner);
    else sol_inspection_format(inspector->output, "\"%s:%zu\"", owner_kind, owner);
    sol_inspection_text(inspector->output, ",\"inferred\":");
    sol_inspection_text(inspector->output, row->inferred ? "true" : "false");
    sol_inspection_text(inspector->output, ",\"rowTail\":");
    if (row->effect_parameter == SOL_AST_NONE) sol_inspection_text(inspector->output, "null");
    else sol_inspection_format(inspector->output, "%zu", row->effect_parameter);
    sol_inspection_text(inspector->output, ",\"atoms\":");
    sol_inspection_atoms(inspector, row->atoms, row->count);
    sol_inspection_text(inspector->output, "}");
}

static void sol_inspection_effects(SolInspector *inspector) {
    SolInspectionBuffer *out = inspector->output;
    sol_inspection_text(out, "\"effects\":{\"schema\":\"sol.inspection.effects\",\"version\":1,\"rows\":[");
    size_t emitted = 0;
    for (size_t group = 0; group < 3; ++group) {
        const SolEffectRow *rows = group == 0 ? inspector->effects->functions
            : group == 1 ? inspector->effects->capability_members : inspector->effects->trait_methods;
        size_t count = group == 0 ? inspector->effects->function_count
            : group == 1 ? inspector->effects->capability_member_count
            : inspector->effects->trait_method_count;
        const char *kind = group == 0 ? "function" : group == 1 ? "capability-member" : "trait-method";
        for (size_t index = 0; index < count; ++index) {
            if (group == 0 && inspector->syntax->items[index].kind != SOL_ITEM_FUNCTION
                && inspector->syntax->items[index].kind != SOL_ITEM_TEST) continue;
            if (emitted++ != 0) sol_inspection_text(out, ",");
            sol_inspection_effect_row(inspector, kind, index, &rows[index]);
        }
    }
    sol_inspection_text(out, "],\"callInstantiations\":["); emitted = 0;
    for (size_t index = 0; index < inspector->effects->call_instantiation_count; ++index) {
        const SolEffectCallInstantiation *call = &inspector->effects->call_instantiations[index];
        if (call->call == SOL_AST_NONE) continue;
        if (emitted++ != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"call\":\"expr:%zu\",\"function\":", call->call);
        sol_inspection_semantic_id(inspector, call->function);
        sol_inspection_text(out, ",\"rowTail\":");
        if (call->parameter == SOL_AST_NONE) sol_inspection_text(out, "null");
        else sol_inspection_format(out, "%zu", call->parameter);
        sol_inspection_text(out, ",\"arguments\":");
        const SolEffectAtom *arguments = call->argument_count == 0 ? NULL
            : &inspector->effects->call_arguments[call->argument_offset];
        sol_inspection_atoms(inspector, arguments, call->argument_count);
        sol_inspection_text(out, ",\"instantiatedRow\":");
        const SolEffectAtom *row = call->row_count == 0 ? NULL
            : &inspector->effects->call_rows[call->row_offset];
        sol_inspection_atoms(inspector, row, call->row_count);
        sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "]}");
}

static const char *sol_inspection_contract_kind(SolContractClauseKind kind) {
    return kind == SOL_CONTRACT_REQUIRES ? "requires" : "ensures";
}

static const char *sol_inspection_outcome(SolContractOutcomeKind kind) {
    return kind == SOL_CONTRACT_OUTCOME_SUCCESS ? "success"
        : kind == SOL_CONTRACT_OUTCOME_FAILURE ? "failure" : "always";
}

static void sol_inspection_contracts(SolInspector *inspector) {
    SolInspectionBuffer *out = inspector->output;
    sol_inspection_text(out, "\"contracts\":{\"schema\":\"sol.inspection.contracts\",\"version\":1,\"obligations\":[");
    for (size_t index = 0; index < inspector->contracts->obligation_count; ++index) {
        const SolObligation *obligation = &inspector->contracts->obligations[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"id\":\"obligation:%llu\",\"kind\":",
            (unsigned long long)obligation->id);
        sol_inspection_string(out, sol_inspection_contract_kind(obligation->kind));
        sol_inspection_text(out, ",\"outcome\":"); sol_inspection_string(out, sol_inspection_outcome(obligation->outcome));
        sol_inspection_format(out, ",\"predicate\":\"expr:%zu\",\"predicateType\":",
            obligation->predicate);
        sol_inspection_type(inspector, obligation->predicate_type);
        sol_inspection_text(out, ",\"resultType\":");
        if (obligation->result.available) sol_inspection_type(inspector, obligation->result.type);
        else sol_inspection_text(out, "null");
        sol_inspection_format(out, ",\"snapshotStart\":%zu,\"snapshotCount\":%zu}",
            obligation->first_snapshot, obligation->snapshot_count);
    }
    sol_inspection_text(out, "],\"snapshots\":[");
    for (size_t index = 0; index < inspector->contracts->snapshot_count; ++index) {
        const SolSnapshot *snapshot = &inspector->contracts->snapshots[index];
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"id\":\"snapshot:%zu\",\"obligation\":\"obligation:%llu\","
            "\"oldExpression\":\"expr:%zu\",\"operand\":\"expr:%zu\",\"type\":",
            snapshot->id, (unsigned long long)snapshot->obligation,
            snapshot->old_expression, snapshot->operand);
        sol_inspection_type(inspector, snapshot->type); sol_inspection_text(out, "}");
    }
    sol_inspection_text(out, "],\"expressionSnapshots\":[");
    size_t emitted = 0;
    for (size_t index = 0; index < inspector->contracts->expression_count; ++index) {
        if (inspector->contracts->expression_snapshots[index] == SOL_AST_NONE) continue;
        if (emitted++ != 0) sol_inspection_text(out, ",");
        sol_inspection_format(out, "{\"expression\":\"expr:%zu\",\"snapshot\":\"snapshot:%zu\"}",
            index, inspector->contracts->expression_snapshots[index]);
    }
    sol_inspection_text(out, "]}");
}

static void sol_inspection_diagnostics(SolInspector *inspector) {
    SolInspectionBuffer *out = inspector->output;
    sol_inspection_text(out, "\"diagnostics\":{\"schema\":\"sol.inspection.diagnostics\",\"version\":1,\"items\":[");
    for (size_t index = 0; index < inspector->diagnostics->count; ++index) {
        const SolDiagnostic *diagnostic = &inspector->diagnostics->items[index];
        const SolPackageFile *file = sol_inspection_file(inspector, diagnostic->span.start);
        const SolSource *source = file == NULL ? inspector->source : &file->source;
        size_t base = file == NULL ? 0 : file->aggregate_start;
        size_t start = diagnostic->span.start < base ? 0 : diagnostic->span.start - base;
        size_t end = diagnostic->span.end < base ? 0 : diagnostic->span.end - base;
        if (start > source->length) start = source->length;
        if (end > source->length) end = source->length;
        SolPosition start_position = sol_source_position(source, start);
        SolPosition end_position = sol_source_position(source, end);
        if (index != 0) sol_inspection_text(out, ",");
        sol_inspection_text(out, "{\"schema\":\"sol.diagnostic/1\",\"code\":");
        sol_inspection_string(out, diagnostic->code);
        sol_inspection_text(out, ",\"severity\":");
        sol_inspection_string(out, diagnostic->severity == SOL_SEVERITY_ERROR ? "error" : "warning");
        sol_inspection_text(out, ",\"message\":"); sol_inspection_string(out, diagnostic->message);
        sol_inspection_text(out, ",\"locations\":[{\"file\":");
        sol_inspection_path_string(out, inspector, file);
        sol_inspection_format(out, ",\"start\":{\"line\":%zu,\"column\":%zu},"
            "\"end\":{\"line\":%zu,\"column\":%zu},\"role\":\"primary\","
            "\"byteSpan\":{\"start\":%zu,\"end\":%zu}}]}",
            start_position.line, start_position.column, end_position.line,
            end_position.column, start, end);
    }
    sol_inspection_text(out, "]}");
}

static bool sol_inspection_slice(size_t offset, size_t count, size_t total) {
    return offset <= total && count <= total - offset;
}

static bool sol_inspection_span_valid(SolSpan span, size_t length) {
    return span.start <= span.end && span.end <= length;
}

static bool sol_inspection_tuple_ordinal(
    const SolSource *source,
    SolSpan span,
    size_t *ordinal
) {
    if (source == NULL || span.start >= span.end || span.end > source->length
        || (span.end - span.start > 1 && source->text[span.start] == '0')) return false;
    size_t value = 0;
    for (size_t index = span.start; index < span.end; ++index) {
        unsigned char byte = (unsigned char)source->text[index];
        if (byte < '0' || byte > '9') return false;
        size_t digit = (size_t)(byte - '0');
        if (value > (SIZE_MAX - digit) / 10) return false;
        value = value * 10 + digit;
    }
    *ordinal = value;
    return true;
}

static bool sol_inspection_source_valid(const SolSource *source) {
    if (source->path == NULL || source->text == NULL
        || source->line_count == 0 || source->line_starts == NULL
        || source->line_starts[0] != 0) return false;
    size_t previous = 0;
    for (size_t index = 0; index < source->line_count; ++index) {
        if (source->line_starts[index] > source->length
            || (index != 0 && source->line_starts[index] < previous)) return false;
        previous = source->line_starts[index];
    }
    return true;
}

static bool sol_inspection_type_valid(const SolInspector *inspector, SolType type) {
    switch (type.kind) {
        case SOL_TYPE_UNKNOWN:
        case SOL_TYPE_ERROR:
        case SOL_TYPE_INT64:
        case SOL_TYPE_BOOL:
        case SOL_TYPE_TEXT:
        case SOL_TYPE_UNIT:
        case SOL_TYPE_NEVER:
            return true;
        case SOL_TYPE_NOMINAL:
        case SOL_TYPE_FUNCTION:
        case SOL_TYPE_SELF:
            return type.definition < inspector->hir->definition_count;
        case SOL_TYPE_APPLICATION:
            return type.definition < inspector->types->type_application_count;
        case SOL_TYPE_FUNCTION_SIGNATURE:
            return type.definition < inspector->types->function_type_count;
        case SOL_TYPE_CAPABILITY_OPERATION:
            return type.definition < inspector->syntax->capability_member_count;
        case SOL_TYPE_VARIANT:
            return type.definition < inspector->types->variant_constructor_count;
        case SOL_TYPE_PARAMETER:
            return type.definition < inspector->syntax->type_parameter_count;
        case SOL_TYPE_TRAIT_METHOD:
            return type.definition < inspector->syntax->trait_method_count;
    }
    return false;
}

static bool sol_inspection_type_equal(SolType left, SolType right) {
    return left.kind == right.kind && left.definition == right.definition;
}

static SolDefId sol_inspection_nominal_definition(
    const SolInspector *inspector, SolType type
) {
    if (type.kind == SOL_TYPE_NOMINAL) return type.definition;
    if (type.kind == SOL_TYPE_APPLICATION
        && type.definition < inspector->types->type_application_count) {
        const SolTypeApplication *application
            = &inspector->types->type_applications[type.definition];
        if (application->constructor == SOL_TYPE_CONSTRUCTOR_USER) {
            return application->definition;
        }
    }
    return SOL_AST_NONE;
}

static bool sol_inspection_name_equal(
    const SolSource *source, SolSpan left, SolSpan right
) {
    size_t left_length = left.end - left.start;
    size_t right_length = right.end - right.start;
    return left_length == right_length
        && memcmp(source->text + left.start, source->text + right.start, left_length) == 0;
}

static bool sol_inspection_field_in_list(
    const SolSyntaxTree *syntax, SolFieldId first, SolFieldId target
) {
    size_t traversed = 0;
    while (first != SOL_AST_NONE) {
        if (first >= syntax->field_count || traversed++ >= syntax->field_count) return false;
        if (first == target) return true;
        first = syntax->fields[first].next;
    }
    return false;
}

static bool sol_inspection_member_type_equal(
    const SolInspector *inspector,
    SolType owner,
    SolDefId definition,
    SolType declared,
    SolType actual,
    size_t depth
) {
    if (depth >= 64) return false;
    if (declared.kind == SOL_TYPE_SELF) return sol_inspection_type_equal(owner, actual);
    if (declared.kind == SOL_TYPE_PARAMETER && definition < inspector->syntax->item_count
        && owner.kind == SOL_TYPE_APPLICATION
        && owner.definition < inspector->types->type_application_count) {
        const SolTypeApplication *application
            = &inspector->types->type_applications[owner.definition];
        SolTypeParameterId parameter
            = inspector->syntax->items[definition].first_type_parameter;
        size_t ordinal = 0;
        while (parameter != SOL_AST_NONE && parameter != declared.definition) {
            if (parameter >= inspector->syntax->type_parameter_count
                || ordinal++ >= inspector->syntax->type_parameter_count) return false;
            parameter = inspector->syntax->type_parameters[parameter].next;
        }
        return parameter == declared.definition && ordinal < application->argument_count
            && sol_inspection_type_equal(actual,
                inspector->types->type_application_arguments[
                    application->argument_offset + ordinal]);
    }
    if (declared.kind != SOL_TYPE_APPLICATION) {
        return sol_inspection_type_equal(declared, actual);
    }
    if (actual.kind != SOL_TYPE_APPLICATION
        || declared.definition >= inspector->types->type_application_count
        || actual.definition >= inspector->types->type_application_count) return false;
    const SolTypeApplication *declared_application
        = &inspector->types->type_applications[declared.definition];
    const SolTypeApplication *actual_application
        = &inspector->types->type_applications[actual.definition];
    if (declared_application->constructor != actual_application->constructor
        || declared_application->definition != actual_application->definition
        || declared_application->argument_count != actual_application->argument_count) {
        return false;
    }
    for (size_t index = 0; index < declared_application->argument_count; ++index) {
        SolType declared_argument = inspector->types->type_application_arguments[
            declared_application->argument_offset + index];
        SolType actual_argument = inspector->types->type_application_arguments[
            actual_application->argument_offset + index];
        if (!sol_inspection_member_type_equal(inspector, owner, definition,
                declared_argument, actual_argument, depth + 1)) return false;
    }
    return true;
}

static bool sol_inspection_patterns_valid(const SolInspector *inspector) {
    const SolSyntaxTree *syntax = inspector->syntax;
    const SolTypeTable *types = inspector->types;
    for (size_t index = 0; index < syntax->pattern_count; ++index) {
        const SolPattern *pattern = &syntax->patterns[index];
        SolType subject = types->pattern_types[index];
        SolVariantId variant = types->pattern_variant_resolutions[index];
        SolDefId definition = sol_inspection_nominal_definition(inspector, subject);
        if ((pattern->kind == SOL_PATTERN_VARIANT) != (variant != SOL_AST_NONE)) return false;
        SolFieldId variant_field = SOL_AST_NONE;
        if (variant != SOL_AST_NONE) {
            if (variant >= syntax->variant_count
                || syntax->variants[variant].owner_item != definition
                || !sol_inspection_name_equal(inspector->source, pattern->name,
                    syntax->variants[variant].name)) return false;
            variant_field = syntax->variants[variant].first_field;
        }
        if (pattern->kind == SOL_PATTERN_RECORD
            && (definition >= syntax->item_count
                || syntax->items[definition].kind != SOL_ITEM_RECORD
                || !sol_inspection_name_equal(inspector->source, pattern->name,
                    syntax->items[definition].name))) return false;
        const SolTypeApplication *tuple = NULL;
        if (pattern->kind == SOL_PATTERN_TUPLE) {
            if (subject.kind != SOL_TYPE_APPLICATION
                || subject.definition >= types->type_application_count) return false;
            tuple = &types->type_applications[subject.definition];
            if (tuple->constructor != SOL_TYPE_CONSTRUCTOR_TUPLE) return false;
        }
        SolPatternBindingId child = pattern->first_binding;
        size_t ordinal = 0;
        while (child != SOL_AST_NONE) {
            const SolPatternBinding *edge = &syntax->pattern_bindings[child];
            SolFieldId field = types->pattern_field_resolutions[child];
            size_t tuple_ordinal = types->pattern_tuple_ordinals[child];
            if (edge->pattern >= syntax->pattern_count) return false;
            if (pattern->kind == SOL_PATTERN_RECORD) {
                if (field >= syntax->field_count || tuple_ordinal != SOL_AST_NONE
                    || !sol_inspection_field_in_list(syntax,
                        syntax->items[definition].first_field, field)
                    || !sol_inspection_name_equal(inspector->source, edge->field,
                        syntax->fields[field].name)
                    || !sol_inspection_member_type_equal(inspector, subject, definition,
                        types->declared_types[syntax->fields[field].type],
                        types->pattern_types[edge->pattern], 0)) return false;
            } else if (pattern->kind == SOL_PATTERN_TUPLE) {
                if (field != SOL_AST_NONE || tuple_ordinal != ordinal
                    || ordinal >= tuple->argument_count
                    || !sol_inspection_type_equal(types->pattern_types[edge->pattern],
                        types->type_application_arguments[tuple->argument_offset + ordinal])) {
                    return false;
                }
            } else if (field != SOL_AST_NONE || tuple_ordinal != SOL_AST_NONE) {
                return false;
            }
            if (pattern->kind == SOL_PATTERN_VARIANT) {
                if (variant_field == SOL_AST_NONE) return false;
                if (!sol_inspection_member_type_equal(inspector, subject, definition,
                        types->declared_types[syntax->fields[variant_field].type],
                        types->pattern_types[edge->pattern], 0)) return false;
                variant_field = syntax->fields[variant_field].next;
            }
            child = edge->next;
            ++ordinal;
        }
        if ((pattern->kind == SOL_PATTERN_VARIANT && variant_field != SOL_AST_NONE)
            || (pattern->kind == SOL_PATTERN_TUPLE && ordinal != tuple->argument_count)) {
            return false;
        }
    }
    for (size_t expression = 0; expression < syntax->expression_count; ++expression) {
        const SolExpr *match = &syntax->expressions[expression];
        if (match->kind != SOL_EXPR_MATCH) continue;
        SolMatchArmId arm = match->as.match_expr.first_arm;
        while (arm != SOL_AST_NONE) {
            const SolMatchArm *entry = &syntax->match_arms[arm];
            if (!sol_inspection_type_equal(types->pattern_types[entry->pattern],
                    types->expressions[match->as.match_expr.scrutinee])
                || (entry->guard != SOL_AST_NONE
                    && types->expressions[entry->guard].kind != SOL_TYPE_BOOL)) return false;
            arm = entry->next;
        }
    }
    return true;
}

static bool sol_inspection_atom_valid(
    const SolInspector *inspector, const SolEffectAtom *atom
) {
    return (int)atom->argument_kind >= 0
        && atom->argument_kind <= SOL_EFFECT_ATOM_SELF
        && sol_inspection_span_valid(atom->name, inspector->source->length)
        && (atom->argument_kind != SOL_EFFECT_ATOM_PARAMETER
            || atom->parameter < inspector->syntax->parameter_count);
}

static bool sol_inspection_atoms_valid(
    const SolInspector *inspector, const SolEffectAtom *atoms, size_t count
) {
    if (count != 0 && atoms == NULL) return false;
    for (size_t index = 0; index < count; ++index) {
        if (!sol_inspection_atom_valid(inspector, &atoms[index])) return false;
    }
    return true;
}

static bool sol_inspection_preflight(SolInspector *inspector) {
    const SolPackage *package = inspector->package;
    const SolSyntaxTree *syntax = inspector->syntax;
    const SolHirModule *hir = inspector->hir;
    const SolTypeTable *types = inspector->types;
    const SolEffectTable *effects = inspector->effects;
    const SolContractTable *contracts = inspector->contracts;
    const SolDiagnostics *diagnostics = inspector->diagnostics;
#define SOL_INSPECTION_ARENA(name) \
    (syntax->name##_count <= syntax->name##_capacity \
        && (syntax->name##_count == 0 || syntax->name##s != NULL))
    if (package->path == NULL || !sol_inspection_source_valid(&package->source)
        || (!package->is_directory
            && !sol_inspection_output_path_valid(sol_inspection_path(inspector, NULL)))
        || (package->is_directory && (package->file_count == 0
            || package->file_count > package->file_capacity || package->files == NULL))
        || !SOL_INSPECTION_ARENA(item) || !SOL_INSPECTION_ARENA(expression)
        || !SOL_INSPECTION_ARENA(import) || !SOL_INSPECTION_ARENA(statement)
        || !SOL_INSPECTION_ARENA(argument) || !SOL_INSPECTION_ARENA(parameter)
        || !SOL_INSPECTION_ARENA(type) || !SOL_INSPECTION_ARENA(type_argument)
        || !SOL_INSPECTION_ARENA(type_parameter) || !SOL_INSPECTION_ARENA(effect_parameter)
        || !SOL_INSPECTION_ARENA(field) || !SOL_INSPECTION_ARENA(variant)
        || !SOL_INSPECTION_ARENA(pattern) || !SOL_INSPECTION_ARENA(pattern_binding)
        || !SOL_INSPECTION_ARENA(match_arm) || !SOL_INSPECTION_ARENA(effect)
        || !SOL_INSPECTION_ARENA(capability_member) || !SOL_INSPECTION_ARENA(trait_method)
        || !SOL_INSPECTION_ARENA(contract_clause)
        || !SOL_INSPECTION_ARENA(contract_condition)) return false;
#undef SOL_INSPECTION_ARENA
    if (package->is_directory) {
        size_t previous_end = 0;
        size_t root_length = strlen(package->path);
        while (root_length != 0
            && sol_inspection_path_separator(package->path[root_length - 1])) --root_length;
        for (size_t index = 0; index < package->file_count; ++index) {
            const SolPackageFile *file = &package->files[index];
            if (file->path == NULL || strlen(file->path) <= root_length
                || !sol_inspection_path_prefix(file->path, package->path, root_length)
                || !sol_inspection_path_separator(file->path[root_length])
                || !sol_inspection_output_path_valid(sol_inspection_path(inspector, file))
                || !sol_inspection_source_valid(&file->source)
                || file->aggregate_start > file->aggregate_end
                || file->aggregate_end > package->source.length
                || file->aggregate_start < previous_end
                || file->source.length > file->aggregate_end - file->aggregate_start) return false;
            previous_end = file->aggregate_end;
            if (!sol_inspection_span_valid(file->module_name, package->source.length)) {
                return false;
            }
        }
    }
    if (!sol_inspection_span_valid(syntax->module_name, package->source.length)
        || !sol_syntax_contracts_validate(&package->source, syntax)) return false;
    if (hir->definition_count != syntax->item_count
        || hir->resolution_count != syntax->expression_count
        || hir->type_resolution_count != syntax->type_count
        || hir->effect_resolution_count != syntax->effect_count
        || hir->type_effect_resolution_count != syntax->type_count
        || hir->local_count > hir->local_capacity
        || hir->semantic_reference_count > hir->semantic_reference_capacity
        || (hir->definition_count != 0 && hir->definitions == NULL)
        || (hir->local_count != 0 && hir->locals == NULL)
        || (hir->resolution_count != 0 && hir->resolutions == NULL)
        || (hir->type_resolution_count != 0 && hir->type_resolutions == NULL)
        || (hir->effect_resolution_count != 0 && hir->effect_resolutions == NULL)
        || (hir->type_effect_resolution_count != 0 && hir->type_effect_resolutions == NULL)
        || (hir->semantic_reference_count != 0 && hir->semantic_references == NULL)) return false;
    for (size_t index = 0; index < hir->definition_count; ++index) {
        if ((int)syntax->items[index].kind < 0
            || syntax->items[index].kind > SOL_ITEM_TEST
            || (int)hir->definitions[index].kind < 0
            || hir->definitions[index].kind > SOL_ITEM_TEST
            || hir->definitions[index].syntax_item >= syntax->item_count
            || !sol_inspection_span_valid(hir->definitions[index].name, package->source.length)
            || !sol_inspection_span_valid(syntax->items[index].name, package->source.length)
            || !sol_inspection_span_valid(syntax->items[index].span, package->source.length)
            || !sol_inspection_span_valid(syntax->items[index].stable_identity,
                package->source.length)) return false;
    }
    for (size_t index = 0; index < syntax->expression_count; ++index) {
        if ((int)syntax->expressions[index].kind < 0
            || syntax->expressions[index].kind > SOL_EXPR_TYPE_APPLICATION
            || !sol_inspection_span_valid(syntax->expressions[index].span,
                package->source.length)
            || (syntax->expressions[index].kind == SOL_EXPR_FIELD
                && syntax->expressions[index].as.field.base >= syntax->expression_count)) {
            return false;
        }
    }
    for (size_t index = 0; index < hir->local_count; ++index) {
        if ((int)hir->locals[index].kind < 0
            || hir->locals[index].kind > SOL_LOCAL_PATTERN
            || (int)hir->locals[index].access < 0
            || hir->locals[index].access > SOL_ACCESS_EXCLUSIVE
            || (hir->locals[index].kind != SOL_LOCAL_PARAMETER
                && hir->locals[index].access != SOL_ACCESS_OWNED)
            || (hir->locals[index].kind != SOL_LOCAL_BINDING
                && hir->locals[index].mutable)
            || hir->locals[index].owner >= hir->definition_count
            || !sol_inspection_span_valid(hir->locals[index].name, package->source.length)) {
            return false;
        }
    }
    for (size_t index = 0; index < hir->semantic_reference_count; ++index) {
        const SolSemanticReference *reference = &hir->semantic_references[index];
        if ((int)reference->kind < 0
            || reference->kind > SOL_SEMANTIC_REFERENCE_BOUND
            || reference->target >= hir->definition_count
            || reference->target_id.high
                != hir->definitions[reference->target].semantic_id.high
            || reference->target_id.low
                != hir->definitions[reference->target].semantic_id.low
            || !sol_inspection_span_valid(reference->span,
                package->source.length)) return false;
    }
    for (size_t index = 0; index < hir->resolution_count; ++index) {
        SolResolution resolution = hir->resolutions[index];
        if ((int)resolution.kind < 0 || resolution.kind > SOL_RESOLUTION_REFINEMENT_SELF
            || (resolution.kind == SOL_RESOLUTION_DEFINITION
                && resolution.target >= hir->definition_count)
            || (resolution.kind == SOL_RESOLUTION_LOCAL
                && resolution.target >= hir->local_count)
            || (resolution.kind == SOL_RESOLUTION_BUILTIN
                && resolution.target > SOL_BUILTIN_NONE)) return false;
    }
    for (size_t index = 0; index < hir->type_resolution_count; ++index) {
        SolTypeResolution resolution = hir->type_resolutions[index];
        if ((int)resolution.kind < 0 || resolution.kind > SOL_TYPE_RESOLUTION_SELF
            || (resolution.kind == SOL_TYPE_RESOLUTION_DEFINITION
                && resolution.target >= hir->definition_count)
            || (resolution.kind == SOL_TYPE_RESOLUTION_PARAMETER
                && resolution.target >= syntax->type_parameter_count)
            || (resolution.kind == SOL_TYPE_RESOLUTION_BUILTIN
                && resolution.target > SOL_TYPE_BUILTIN_RESULT)) return false;
    }
    const SolEffectResolution *effect_resolution_groups[] = {
        hir->effect_resolutions, hir->type_effect_resolutions
    };
    const size_t effect_resolution_counts[] = {
        hir->effect_resolution_count, hir->type_effect_resolution_count
    };
    for (size_t group = 0; group < 2; ++group) {
        for (size_t index = 0; index < effect_resolution_counts[group]; ++index) {
            SolEffectResolution resolution = effect_resolution_groups[group][index];
            if ((int)resolution.kind < 0 || resolution.kind > SOL_EFFECT_RESOLUTION_ERROR
                || (resolution.kind == SOL_EFFECT_RESOLUTION_PARAMETER
                    && resolution.target >= syntax->effect_parameter_count)) return false;
        }
    }
    if (types->expression_count != syntax->expression_count
        || types->local_count != hir->local_count
        || types->definition_count != hir->definition_count
        || types->declared_type_count != syntax->type_count
        || types->member_resolution_count != syntax->expression_count
        || types->tuple_projection_count != syntax->expression_count
        || types->pattern_resolution_count != syntax->pattern_count
        || types->pattern_child_resolution_count != syntax->pattern_binding_count
        || types->argument_resolution_count != syntax->argument_count
        || types->implementation_target_count != syntax->item_count
        || types->representation_count != syntax->item_count
        || types->construction_count != syntax->expression_count
        || types->loop_fact_count != syntax->statement_count
        || types->unreachable_fact_count != syntax->statement_count
        || types->handler_count != syntax->expression_count
        || types->call_instantiation_count != syntax->expression_count
        || types->method_resolution_count != syntax->expression_count
        || types->type_application_count > types->type_application_capacity
        || types->type_application_argument_count > types->type_application_argument_capacity
        || types->function_type_count > types->function_type_capacity
        || types->function_coercion_count > types->function_coercion_capacity
        || types->provenance_count > types->provenance_capacity
        || types->provenance_root_count > types->provenance_root_capacity
        || types->call_instantiation_argument_count > types->call_instantiation_argument_capacity
        || types->variant_constructor_count > types->variant_constructor_capacity) return false;
#define SOL_INSPECTION_TABLE_SLICE(pointer, count) ((count) == 0 || (pointer) != NULL)
    if (!SOL_INSPECTION_TABLE_SLICE(types->expressions, types->expression_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->locals, types->local_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->definitions, types->definition_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->declared_types, types->declared_type_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->type_applications, types->type_application_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->type_application_arguments,
            types->type_application_argument_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->function_types, types->function_type_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->function_coercions, types->function_coercion_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->provenances, types->provenance_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->provenance_roots, types->provenance_root_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->call_instantiations,
            types->call_instantiation_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->call_instantiation_arguments,
            types->call_instantiation_argument_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->handlers, types->handler_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->method_resolutions, types->method_resolution_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->field_resolutions, types->member_resolution_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->variant_resolutions, types->member_resolution_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->tuple_projections,
            types->tuple_projection_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->representations, types->representation_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->implementation_targets,
            types->implementation_target_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->constructions, types->construction_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->loop_facts, types->loop_fact_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->unreachable_facts,
            types->unreachable_fact_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->expression_capability_origins,
            types->expression_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->expression_operation_origins,
            types->expression_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->local_capability_origins, types->local_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->local_operation_origins, types->local_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->variant_constructors,
            types->variant_constructor_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->pattern_variant_resolutions,
            types->pattern_resolution_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->pattern_types,
            types->pattern_resolution_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->pattern_field_resolutions,
            types->pattern_child_resolution_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->pattern_tuple_ordinals,
            types->pattern_child_resolution_count)
        || !SOL_INSPECTION_TABLE_SLICE(types->argument_field_resolutions,
            types->argument_resolution_count)) return false;
#undef SOL_INSPECTION_TABLE_SLICE
#define SOL_INSPECTION_CHECK_TYPES(pointer, count) \
    for (size_t type_index = 0; type_index < (count); ++type_index) \
        if (!sol_inspection_type_valid(inspector, (pointer)[type_index])) return false
    SOL_INSPECTION_CHECK_TYPES(types->expressions, types->expression_count);
    SOL_INSPECTION_CHECK_TYPES(types->locals, types->local_count);
    SOL_INSPECTION_CHECK_TYPES(types->definitions, types->definition_count);
    SOL_INSPECTION_CHECK_TYPES(types->declared_types, types->declared_type_count);
    SOL_INSPECTION_CHECK_TYPES(types->pattern_types, types->pattern_resolution_count);
    SOL_INSPECTION_CHECK_TYPES(types->type_application_arguments,
        types->type_application_argument_count);
    SOL_INSPECTION_CHECK_TYPES(types->call_instantiation_arguments,
        types->call_instantiation_argument_count);
    SOL_INSPECTION_CHECK_TYPES(types->implementation_targets,
        types->implementation_target_count);
    for (size_t index = 0; index < syntax->variant_count; ++index) {
        SolDefId owner = syntax->variants[index].owner_item;
        if (owner >= syntax->item_count || syntax->items[owner].kind != SOL_ITEM_ENUM
            || !sol_inspection_span_valid(syntax->variants[index].name,
                package->source.length)) return false;
    }
    for (size_t index = 0; index < syntax->field_count; ++index) {
        bool is_variant = false;
        SolDefId owner = SOL_AST_NONE;
        SolVariantId variant = SOL_AST_NONE;
        size_t ordinal = 0;
        if (!sol_inspection_field_parent(syntax, index, &is_variant,
                &owner, &variant, &ordinal)
            || owner >= syntax->item_count
            || (is_variant && variant >= syntax->variant_count)
            || ordinal >= syntax->field_count
            || !sol_inspection_span_valid(syntax->fields[index].name,
                package->source.length)) return false;
    }
    for (size_t index = 0; index < types->type_application_count; ++index) {
        const SolTypeApplication *entry = &types->type_applications[index];
        if ((int)entry->constructor < 0 || entry->constructor > SOL_TYPE_CONSTRUCTOR_TUPLE
            || (entry->constructor == SOL_TYPE_CONSTRUCTOR_USER
                && entry->definition >= hir->definition_count)
            || (entry->constructor != SOL_TYPE_CONSTRUCTOR_USER
                && entry->definition != SOL_AST_NONE)
            || (entry->constructor == SOL_TYPE_CONSTRUCTOR_TUPLE
                && (entry->argument_count < 2 || entry->argument_count > 16))
            || !sol_inspection_slice(entry->argument_offset, entry->argument_count,
                types->type_application_argument_count)) return false;
    }
    for (size_t index = 0; index < types->function_type_count; ++index) {
        const SolFunctionType *entry = &types->function_types[index];
        if ((entry->parameter_count != 0
                && (entry->parameters == NULL || entry->accesses == NULL))
            || !sol_inspection_type_valid(inspector, entry->result)
            || (entry->effect_parameter != SOL_AST_NONE
                && entry->effect_parameter >= syntax->effect_parameter_count)
            || !sol_inspection_atoms_valid(inspector, entry->effects.atoms,
                entry->effects.count)) return false;
        SOL_INSPECTION_CHECK_TYPES(entry->parameters, entry->parameter_count);
        for (size_t parameter = 0; parameter < entry->parameter_count; ++parameter) {
            if ((int)entry->accesses[parameter] < 0
                || entry->accesses[parameter] > SOL_ACCESS_EXCLUSIVE) return false;
        }
    }
#undef SOL_INSPECTION_CHECK_TYPES
    for (size_t index = 0; index < types->provenance_count; ++index) {
        if (!sol_inspection_slice(types->provenances[index].root_offset,
                types->provenances[index].root_count, types->provenance_root_count)) return false;
    }
    for (size_t index = 0; index < types->expression_count; ++index) {
        SolProvenanceId capability = types->expression_capability_origins[index];
        SolProvenanceId operation = types->expression_operation_origins[index];
        if ((capability != SOL_PROVENANCE_NONE && capability >= types->provenance_count)
            || (operation != SOL_PROVENANCE_NONE
                && operation >= types->provenance_count)) return false;
    }
    for (size_t index = 0; index < types->local_count; ++index) {
        SolProvenanceId capability = types->local_capability_origins[index];
        SolProvenanceId operation = types->local_operation_origins[index];
        if ((capability != SOL_PROVENANCE_NONE && capability >= types->provenance_count)
            || (operation != SOL_PROVENANCE_NONE
                && operation >= types->provenance_count)) return false;
    }
    for (size_t index = 0; index < types->call_instantiation_count; ++index) {
        const SolCallInstantiation *entry = &types->call_instantiations[index];
        if (entry->function != SOL_AST_NONE
            && (entry->function >= hir->definition_count
                || !sol_inspection_slice(entry->argument_offset, entry->argument_count,
                    types->call_instantiation_argument_count))) return false;
    }
    for (size_t index = 0; index < types->function_coercion_count; ++index) {
        if (types->function_coercions[index].expression >= syntax->expression_count
            || !sol_inspection_type_valid(inspector,
                types->function_coercions[index].expected)) return false;
    }
    for (size_t index = 0; index < types->variant_constructor_count; ++index) {
        if (types->variant_constructors[index].variant >= syntax->variant_count
            || !sol_inspection_type_valid(inspector,
                types->variant_constructors[index].owner)) return false;
    }
    for (size_t index = 0; index < types->handler_count; ++index) {
        const SolHandler *entry = &types->handlers[index];
        if (entry->source_member != SOL_AST_NONE
            && (entry->source_member >= syntax->capability_member_count
                || entry->provider_member >= syntax->capability_member_count
                || entry->root >= syntax->parameter_count)) return false;
    }
    for (size_t index = 0; index < types->method_resolution_count; ++index) {
        const SolMethodResolution *entry = &types->method_resolutions[index];
        if ((int)entry->kind < 0 || entry->kind > SOL_METHOD_RESOLUTION_IMPLEMENTATION
            || (entry->kind != SOL_METHOD_RESOLUTION_NONE
                && (entry->trait >= hir->definition_count
                    || (entry->kind == SOL_METHOD_RESOLUTION_IMPLEMENTATION
                        && entry->implementation >= hir->definition_count)
                    || entry->requirement >= syntax->trait_method_count
                    || entry->method >= syntax->trait_method_count))) return false;
    }
    for (size_t index = 0; index < types->member_resolution_count; ++index) {
        if ((types->field_resolutions[index] != SOL_AST_NONE
                && types->field_resolutions[index] >= syntax->field_count)
            || (types->variant_resolutions[index] != SOL_AST_NONE
                && types->variant_resolutions[index] >= syntax->variant_count)) return false;
        size_t ordinal = types->tuple_projections[index];
        if (ordinal != SOL_AST_NONE) {
            if (syntax->expressions[index].kind != SOL_EXPR_FIELD
                || types->field_resolutions[index] != SOL_AST_NONE
                || types->variant_resolutions[index] != SOL_AST_NONE) return false;
            SolExprId base = syntax->expressions[index].as.field.base;
            if (base >= types->expression_count
                || types->expressions[base].kind != SOL_TYPE_APPLICATION
                || types->expressions[base].definition >= types->type_application_count) {
                return false;
            }
            const SolTypeApplication *tuple
                = &types->type_applications[types->expressions[base].definition];
            size_t source_ordinal = SOL_AST_NONE;
            if (tuple->constructor != SOL_TYPE_CONSTRUCTOR_TUPLE
                || ordinal >= tuple->argument_count
                || !sol_inspection_tuple_ordinal(&package->source,
                    syntax->expressions[index].as.field.name, &source_ordinal)
                || source_ordinal != ordinal
                || tuple->argument_offset > types->type_application_argument_count
                || tuple->argument_count > types->type_application_argument_count
                    - tuple->argument_offset) return false;
            SolType selected
                = types->type_application_arguments[tuple->argument_offset + ordinal];
            if (types->expressions[index].kind != selected.kind
                || types->expressions[index].definition != selected.definition) return false;
        } else if (syntax->expressions[index].kind == SOL_EXPR_FIELD) {
            SolExprId base = syntax->expressions[index].as.field.base;
            if (base < types->expression_count
                && types->expressions[base].kind == SOL_TYPE_APPLICATION
                && types->expressions[base].definition < types->type_application_count
                && types->type_applications[types->expressions[base].definition].constructor
                    == SOL_TYPE_CONSTRUCTOR_TUPLE) return false;
        }
    }
    for (size_t index = 0; index < types->pattern_resolution_count; ++index) {
        if (types->pattern_variant_resolutions[index] != SOL_AST_NONE
            && types->pattern_variant_resolutions[index] >= syntax->variant_count) return false;
    }
    if (!sol_inspection_patterns_valid(inspector)) return false;
    for (size_t index = 0; index < types->argument_resolution_count; ++index) {
        if (types->argument_field_resolutions[index] != SOL_AST_NONE
            && types->argument_field_resolutions[index] >= syntax->field_count) return false;
    }
    for (size_t index = 0; index < types->representation_count; ++index) {
        if ((int)types->representations[index].flavor < 0
            || types->representations[index].flavor > SOL_TYPE_DECLARATION_REFINED
            || (types->representations[index].flavor != SOL_TYPE_DECLARATION_NONE
                && syntax->items[index].kind != SOL_ITEM_TYPE)
            || !sol_inspection_type_valid(inspector,
                types->representations[index].representation)) return false;
    }
    for (size_t index = 0; index < types->construction_count; ++index) {
        const SolTypeConstruction *entry = &types->constructions[index];
        if (entry->definition != SOL_AST_NONE
            && (entry->definition >= hir->definition_count
                || !sol_inspection_type_valid(inspector, entry->representation)
                || !sol_inspection_type_valid(inspector, entry->result))) return false;
    }
    if (effects->function_count != syntax->item_count
        || effects->capability_member_count != syntax->capability_member_count
        || effects->trait_method_count != syntax->trait_method_count
        || effects->call_instantiation_count > effects->call_instantiation_capacity
        || effects->call_argument_count > effects->call_argument_capacity
        || effects->call_row_count > effects->call_row_capacity
        || (effects->function_count != 0 && effects->functions == NULL)
        || (effects->capability_member_count != 0 && effects->capability_members == NULL)
        || (effects->trait_method_count != 0 && effects->trait_methods == NULL)
        || (effects->call_instantiation_count != 0 && effects->call_instantiations == NULL)
        || !sol_inspection_atoms_valid(inspector, effects->call_arguments,
            effects->call_argument_count)
        || !sol_inspection_atoms_valid(inspector, effects->call_rows,
            effects->call_row_count)) return false;
    const SolEffectRow *row_groups[] = {
        effects->functions, effects->capability_members, effects->trait_methods
    };
    const size_t row_counts[] = {
        effects->function_count, effects->capability_member_count, effects->trait_method_count
    };
    for (size_t group = 0; group < 3; ++group) {
        for (size_t index = 0; index < row_counts[group]; ++index) {
            if (!sol_inspection_atoms_valid(inspector, row_groups[group][index].atoms,
                    row_groups[group][index].count)
                || (row_groups[group][index].effect_parameter != SOL_AST_NONE
                    && row_groups[group][index].effect_parameter
                        >= syntax->effect_parameter_count)) return false;
        }
    }
    for (size_t index = 0; index < effects->call_instantiation_count; ++index) {
        const SolEffectCallInstantiation *entry = &effects->call_instantiations[index];
        if (entry->call != SOL_AST_NONE
            && (entry->call != index || entry->call >= syntax->expression_count
                || entry->function >= hir->definition_count
                || (entry->parameter != SOL_AST_NONE
                    && entry->parameter >= syntax->effect_parameter_count)
                || !sol_inspection_slice(entry->argument_offset, entry->argument_count,
                    effects->call_argument_count)
                || !sol_inspection_slice(entry->row_offset, entry->row_count,
                    effects->call_row_count))) return false;
    }
    if (contracts->obligation_count != syntax->contract_condition_count
        || (contracts->obligation_count != 0 && contracts->obligations == NULL)
        || contracts->snapshot_count > contracts->snapshot_capacity
        || (contracts->snapshot_count != 0 && contracts->snapshots == NULL)
        || contracts->loop_obligation_count > contracts->loop_obligation_capacity
        || (contracts->loop_obligation_count != 0
            && contracts->loop_obligations == NULL)
        || contracts->unreachable_obligation_count
            > contracts->unreachable_obligation_capacity
        || (contracts->unreachable_obligation_count != 0
            && contracts->unreachable_obligations == NULL)
        || contracts->expression_count != syntax->expression_count
        || (contracts->expression_count != 0 && contracts->expression_snapshots == NULL)
        || diagnostics->count > diagnostics->capacity
        || (diagnostics->count != 0 && diagnostics->items == NULL)) return false;
    for (size_t index = 0; index < contracts->obligation_count; ++index) {
        const SolObligation *entry = &contracts->obligations[index];
        if (entry->id != (SolObligationId)index || entry->condition != index
            || entry->condition >= syntax->contract_condition_count
            || (int)entry->owner_kind < 0 || entry->owner_kind > SOL_CONTRACT_OWNER_TYPE
            || (int)entry->kind < 0 || entry->kind > SOL_CONTRACT_ENSURES
            || (int)entry->outcome < 0 || entry->outcome > SOL_CONTRACT_OUTCOME_FAILURE
            || (entry->owner_kind == SOL_CONTRACT_OWNER_CAPABILITY_MEMBER
                && entry->owner >= syntax->capability_member_count)
            || (entry->owner_kind == SOL_CONTRACT_OWNER_ITEM
                && (entry->owner >= syntax->item_count
                    || syntax->items[entry->owner].kind != SOL_ITEM_FUNCTION))
            || (entry->owner_kind == SOL_CONTRACT_OWNER_TYPE
                && (entry->owner >= syntax->item_count
                    || syntax->items[entry->owner].kind != SOL_ITEM_TYPE))
            || entry->predicate >= syntax->expression_count
            || !sol_inspection_type_valid(inspector, entry->predicate_type)
            || entry->predicate_type.kind != types->expressions[entry->predicate].kind
            || entry->predicate_type.definition
                != types->expressions[entry->predicate].definition
            || (entry->result.available
                && !sol_inspection_type_valid(inspector, entry->result.type))
            || !sol_inspection_slice(entry->first_snapshot, entry->snapshot_count,
                contracts->snapshot_count)) return false;
        for (size_t snapshot = 0; snapshot < entry->snapshot_count; ++snapshot) {
            if (contracts->snapshots[entry->first_snapshot + snapshot].obligation
                != entry->id) return false;
        }
        const SolContractCondition *condition
            = &syntax->contract_conditions[entry->condition];
        if (condition->owner_clause >= syntax->contract_clause_count) return false;
        const SolContractClause *clause
            = &syntax->contract_clauses[condition->owner_clause];
        if (entry->owner_kind != clause->owner_kind || entry->owner != clause->owner
            || entry->kind != clause->kind || entry->outcome != condition->outcome
            || entry->predicate != condition->expression) return false;
    }
    for (size_t index = 0; index < contracts->snapshot_count; ++index) {
        const SolSnapshot *entry = &contracts->snapshots[index];
        if (entry->id != index || entry->obligation >= contracts->obligation_count
            || entry->old_expression >= syntax->expression_count
            || entry->operand >= syntax->expression_count
            || !sol_inspection_type_valid(inspector, entry->type)
            || entry->type.kind != types->expressions[entry->old_expression].kind
            || entry->type.definition
                != types->expressions[entry->old_expression].definition) return false;
        const SolObligation *obligation = &contracts->obligations[entry->obligation];
        if (index < obligation->first_snapshot
            || index - obligation->first_snapshot >= obligation->snapshot_count
            || syntax->expressions[entry->old_expression].kind != SOL_EXPR_OLD
            || syntax->expressions[entry->old_expression].as.old_expression != entry->operand
            || contracts->expression_snapshots[entry->old_expression] != index) return false;
    }
    for (size_t index = 0; index < contracts->loop_obligation_count; ++index) {
        const SolLoopObligation *entry = &contracts->loop_obligations[index];
        bool invariant = entry->kind == SOL_LOOP_OBLIGATION_INVARIANT_ENTRY
            || entry->kind == SOL_LOOP_OBLIGATION_INVARIANT_PRESERVATION;
        if (entry->id != index
            || (int)entry->kind < 0
            || entry->kind > SOL_LOOP_OBLIGATION_DECREASES_STRICT
            || entry->loop_statement >= syntax->statement_count
            || (syntax->statements[entry->loop_statement].kind != SOL_STATEMENT_LOOP
                && syntax->statements[entry->loop_statement].kind
                    != SOL_STATEMENT_WHILE)
            || entry->expression >= syntax->expression_count
            || !sol_inspection_type_valid(inspector, entry->expression_type)
            || entry->expression_type.kind
                != types->expressions[entry->expression].kind
            || entry->expression_type.definition
                != types->expressions[entry->expression].definition
            || entry->expression_type.kind
                != (invariant ? SOL_TYPE_BOOL : SOL_TYPE_INT64)
            || entry->span.start >= entry->span.end
            || entry->span.end > package->source.length) return false;
        const SolLoopFact *fact = &types->loop_facts[entry->loop_statement];
        if (!fact->is_loop || entry->owner != fact->owner
            || entry->owner_member != fact->owner_member
            || entry->owner_trait_method != fact->owner_trait_method) return false;
    }
    for (size_t index = 0; index < contracts->unreachable_obligation_count; ++index) {
        const SolUnreachableObligation *entry
            = &contracts->unreachable_obligations[index];
        if (entry->id != index || entry->statement >= syntax->statement_count
            || syntax->statements[entry->statement].kind != SOL_STATEMENT_UNREACHABLE
            || entry->proof >= syntax->expression_count
            || !sol_inspection_type_valid(inspector, entry->proof_type)
            || entry->proof_type.kind != SOL_TYPE_BOOL
            || entry->proof_type.kind != types->expressions[entry->proof].kind
            || entry->proof_type.definition != types->expressions[entry->proof].definition
            || entry->span.start >= entry->span.end
            || entry->span.end > package->source.length) return false;
        const SolUnreachableFact *fact = &types->unreachable_facts[entry->statement];
        if (!fact->is_unreachable || entry->owner != fact->owner
            || entry->owner_member != fact->owner_member
            || entry->owner_trait_method != fact->owner_trait_method) return false;
        if (index != 0) {
            const SolUnreachableObligation *previous
                = &contracts->unreachable_obligations[index - 1];
            if (previous->span.start > entry->span.start
                || (previous->span.start == entry->span.start
                    && (previous->span.end > entry->span.end
                        || (previous->span.end == entry->span.end
                            && previous->statement >= entry->statement)))) return false;
        }
    }
    for (size_t index = 0; index < contracts->expression_count; ++index) {
        SolSnapshotId snapshot = contracts->expression_snapshots[index];
        if (snapshot != SOL_AST_NONE
            && (snapshot >= contracts->snapshot_count
                || contracts->snapshots[snapshot].old_expression != index)) return false;
    }
    for (size_t index = 0; index < diagnostics->count; ++index) {
        if ((int)diagnostics->items[index].severity < 0
            || diagnostics->items[index].severity > SOL_SEVERITY_WARNING
            || memchr(diagnostics->items[index].code, '\0',
                sizeof(diagnostics->items[index].code)) == NULL
            || memchr(diagnostics->items[index].message, '\0',
                sizeof(diagnostics->items[index].message)) == NULL
            || !sol_inspection_span_valid(diagnostics->items[index].span,
                package->source.length)) return false;
    }
    return true;
}

bool sol_inspection_render(
    FILE *stream, const SolPackage *package, const SolHirModule *hir,
    const SolTypeTable *types, const SolEffectTable *effects,
    const SolContractTable *contracts, const SolDiagnostics *diagnostics
) {
    if (stream == NULL || package == NULL || hir == NULL || types == NULL
        || effects == NULL || contracts == NULL || diagnostics == NULL) return false;
    SolInspectionBuffer output = {0};
    SolInspector inspector = {
        .output = &output, .package = package, .source = &package->source,
        .syntax = &package->syntax, .hir = hir, .types = types, .effects = effects,
        .contracts = contracts, .diagnostics = diagnostics,
    };
    if (!sol_inspection_preflight(&inspector)) return false;
    sol_inspection_text(&output, "{\"schema\":\"sol.inspection\",\"version\":3,"
        "\"producer\":{\"name\":\"sol\",\"version\":\"0.1.0-dev\"},\"package\":{\"kind\":");
    sol_inspection_string(&output, package->is_directory ? "directory" : "file");
    sol_inspection_format(&output, ",\"edition\":%u,\"fileCount\":%zu},\"artifacts\":{",
        package->syntax.edition, package->is_directory ? package->file_count : (size_t)1);
    sol_inspection_syntax(&inspector); sol_inspection_text(&output, ",");
    sol_inspection_hir(&inspector); sol_inspection_text(&output, ",");
    sol_inspection_types(&inspector); sol_inspection_text(&output, ",");
    sol_inspection_effects(&inspector); sol_inspection_text(&output, ",");
    sol_inspection_contracts(&inspector); sol_inspection_text(&output, ",");
    sol_inspection_diagnostics(&inspector);
    sol_inspection_text(&output, "}}\n");
    bool complete = !output.failed
        && fwrite(output.data, 1, output.length, stream) == output.length
        && ferror(stream) == 0;
    free(output.data);
    return complete;
}
