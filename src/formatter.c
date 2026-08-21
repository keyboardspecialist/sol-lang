#include "sol/formatter.h"

#include "sol/lexer.h"
#include "sol/parser.h"
#include "sol/token.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    SolFormatted *output;
    bool allocation_failed;
} SolFormatWriter;

typedef enum {
    SOL_FORMAT_ANGLE_NONE,
    SOL_FORMAT_ANGLE_DELIMITER,
    SOL_FORMAT_ANGLE_COMPARISON,
} SolFormatAngleRole;

static bool sol_format_reserve(SolFormatWriter *writer, size_t additional) {
    SolFormatted *output = writer->output;
    if (additional > SIZE_MAX - output->length - 1) {
        writer->allocation_failed = true;
        return false;
    }
    size_t required = output->length + additional + 1;
    if (required <= output->capacity) return true;

    size_t capacity = output->capacity == 0 ? 256 : output->capacity;
    while (capacity < required) {
        if (capacity > SIZE_MAX / 2) {
            capacity = required;
            break;
        }
        capacity *= 2;
    }
    char *text = realloc(output->text, capacity);
    if (text == NULL) {
        writer->allocation_failed = true;
        return false;
    }
    output->text = text;
    output->capacity = capacity;
    return true;
}

static bool sol_format_append_bytes(SolFormatWriter *writer, const char *text, size_t length) {
    if (!sol_format_reserve(writer, length)) return false;
    memcpy(writer->output->text + writer->output->length, text, length);
    writer->output->length += length;
    writer->output->text[writer->output->length] = '\0';
    return true;
}

static bool sol_format_append_byte(SolFormatWriter *writer, char byte) {
    return sol_format_append_bytes(writer, &byte, 1);
}

static bool sol_format_at_line_start(const SolFormatted *output) {
    return output->length == 0 || output->text[output->length - 1] == '\n';
}

static bool sol_format_indent(SolFormatWriter *writer, size_t depth) {
    static const char spaces[] = "                                ";
    if (depth > SIZE_MAX / 4) {
        writer->allocation_failed = true;
        return false;
    }
    size_t remaining = depth * 4;
    while (remaining != 0) {
        size_t count = remaining < sizeof(spaces) - 1 ? remaining : sizeof(spaces) - 1;
        if (!sol_format_append_bytes(writer, spaces, count)) return false;
        remaining -= count;
    }
    return true;
}

static bool sol_format_break(SolFormatWriter *writer, size_t count) {
    size_t wanted = count > 2 ? 2 : count;
    size_t present = 0;
    size_t cursor = writer->output->length;
    while (cursor != 0 && writer->output->text[cursor - 1] == '\n') {
        --cursor;
        ++present;
    }
    while (present < wanted) {
        if (!sol_format_append_byte(writer, '\n')) return false;
        ++present;
    }
    return true;
}

static bool sol_format_is_word(SolTokenKind kind) {
    return (kind >= SOL_TOKEN_IDENTIFIER && kind <= SOL_TOKEN_FALSE);
}

static bool sol_format_is_binary(SolTokenKind kind) {
    switch (kind) {
        case SOL_TOKEN_LESS_EQUAL:
        case SOL_TOKEN_GREATER_EQUAL:
        case SOL_TOKEN_EQUAL:
        case SOL_TOKEN_EQUAL_EQUAL:
        case SOL_TOKEN_BANG_EQUAL:
        case SOL_TOKEN_PLUS:
        case SOL_TOKEN_MINUS:
        case SOL_TOKEN_STAR:
        case SOL_TOKEN_SLASH:
        case SOL_TOKEN_PERCENT:
        case SOL_TOKEN_PLUS_EQUAL:
        case SOL_TOKEN_MINUS_EQUAL:
        case SOL_TOKEN_STAR_EQUAL:
        case SOL_TOKEN_SLASH_EQUAL:
        case SOL_TOKEN_PERCENT_EQUAL:
        case SOL_TOKEN_AMP_AMP:
        case SOL_TOKEN_PIPE_PIPE:
        case SOL_TOKEN_ARROW:
        case SOL_TOKEN_FAT_ARROW:
            return true;
        default:
            return false;
    }
}

static bool sol_format_is_left_delimiter(SolTokenKind kind) {
    return kind == SOL_TOKEN_LEFT_PAREN || kind == SOL_TOKEN_LEFT_BRACKET;
}

static bool sol_format_is_right_delimiter(SolTokenKind kind) {
    return kind == SOL_TOKEN_RIGHT_PAREN || kind == SOL_TOKEN_RIGHT_BRACKET;
}

static bool sol_format_is_unary(
    const SolSyntaxTree *tree,
    SolToken token
) {
    if (token.kind != SOL_TOKEN_MINUS && token.kind != SOL_TOKEN_BANG) return false;
    for (size_t index = 0; index < tree->expression_count; ++index) {
        const SolExpr *expression = &tree->expressions[index];
        if (expression->kind == SOL_EXPR_UNARY
            && expression->span.start == token.span.start
            && expression->as.unary.operator_kind == token.kind) {
            return true;
        }
    }
    return false;
}

static bool sol_format_needs_space(
    SolTokenKind previous,
    SolTokenKind current,
    SolFormatAngleRole previous_angle,
    SolFormatAngleRole current_angle,
    bool previous_unary,
    bool current_unary
) {
    if (current == SOL_TOKEN_LINE_COMMENT || current == SOL_TOKEN_BLOCK_COMMENT
        || previous == SOL_TOKEN_LINE_COMMENT || previous == SOL_TOKEN_BLOCK_COMMENT) {
        return true;
    }
    if (previous_angle == SOL_FORMAT_ANGLE_COMPARISON
        || current_angle == SOL_FORMAT_ANGLE_COMPARISON) {
        return true;
    }
    if (current_angle == SOL_FORMAT_ANGLE_DELIMITER
        || (previous_angle == SOL_FORMAT_ANGLE_DELIMITER
            && previous == SOL_TOKEN_LESS)) {
        return false;
    }
    if (previous_angle == SOL_FORMAT_ANGLE_DELIMITER
        && previous == SOL_TOKEN_GREATER
        && sol_format_is_word(current)) {
        return true;
    }
    if (current == SOL_TOKEN_COMMA || current == SOL_TOKEN_DOT
        || current == SOL_TOKEN_QUESTION || current == SOL_TOKEN_COLON
        || sol_format_is_right_delimiter(current)) {
        return false;
    }
    if (previous == SOL_TOKEN_DOT || previous == SOL_TOKEN_AT
        || sol_format_is_left_delimiter(previous)) {
        return false;
    }
    if (previous == SOL_TOKEN_COMMA || previous == SOL_TOKEN_COLON
        || (sol_format_is_binary(previous) && !previous_unary)
        || (sol_format_is_binary(current) && !current_unary)) {
        return true;
    }
    if (current_unary && sol_format_is_word(previous)) return true;
    if (previous_unary) return false;
    if (previous == SOL_TOKEN_LEFT_BRACE) {
        return current != SOL_TOKEN_RIGHT_BRACE;
    }
    if (current == SOL_TOKEN_RIGHT_BRACE) {
        return previous != SOL_TOKEN_LEFT_BRACE;
    }
    if (previous == SOL_TOKEN_RIGHT_BRACE && sol_format_is_word(current)) return true;
    if (previous == SOL_TOKEN_RIGHT_BRACE && current == SOL_TOKEN_ELSE) return true;
    if (current == SOL_TOKEN_LEFT_BRACE) return true;
    if (sol_format_is_word(previous) && sol_format_is_word(current)) return true;
    return false;
}

static bool sol_format_append_token_text(
    SolFormatWriter *writer,
    const SolSource *source,
    SolToken token
) {
    const char *text = source->text + token.span.start;
    size_t length = token.span.end - token.span.start;
    return sol_format_append_bytes(writer, text, length);
}

static bool sol_format_classify_angles(
    const SolTokens *tokens,
    const SolSyntaxTree *tree,
    SolFormatAngleRole *roles
) {
    for (size_t index = 0; index < tokens->count; ++index) {
        SolTokenKind kind = tokens->items[index].kind;
        if (kind == SOL_TOKEN_LESS || kind == SOL_TOKEN_GREATER) {
            roles[index] = SOL_FORMAT_ANGLE_DELIMITER;
        }
    }

    for (size_t expression_index = 0;
         expression_index < tree->expression_count;
         ++expression_index) {
        const SolExpr *expression = &tree->expressions[expression_index];
        if (expression->kind != SOL_EXPR_BINARY
            || (expression->as.binary.operator_kind != SOL_TOKEN_LESS
                && expression->as.binary.operator_kind != SOL_TOKEN_GREATER)) {
            continue;
        }
        if (expression->as.binary.left >= tree->expression_count
            || expression->as.binary.right >= tree->expression_count) {
            return false;
        }

        size_t left_end = tree->expressions[expression->as.binary.left].span.end;
        size_t right_start = tree->expressions[expression->as.binary.right].span.start;
        bool found = false;
        for (size_t token_index = 0; token_index < tokens->count; ++token_index) {
            SolToken token = tokens->items[token_index];
            if (token.span.start < left_end) continue;
            if (token.span.end > right_start) break;
            if (token.kind == expression->as.binary.operator_kind) {
                roles[token_index] = SOL_FORMAT_ANGLE_COMPARISON;
                found = true;
                break;
            }
        }
        if (!found) return false;
    }
    return true;
}

static bool sol_format_render_raw(
    const SolSource *source,
    const SolTokens *tokens,
    const SolSyntaxTree *tree,
    SolFormatted *output
) {
    SolFormatWriter writer = {.output = output};
    SolFormatAngleRole *angle_roles = NULL;
    if (tokens->count != 0) {
        angle_roles = calloc(tokens->count, sizeof(*angle_roles));
        if (angle_roles == NULL) return false;
    }
    if (!sol_format_classify_angles(tokens, tree, angle_roles)) {
        free(angle_roles);
        return false;
    }
    size_t depth = 0;
    size_t continuation_depth = 0;
    size_t pending_newlines = 0;
    bool have_previous = false;
    bool previous_unary = false;
    SolFormatAngleRole previous_angle = SOL_FORMAT_ANGLE_NONE;
    SolTokenKind previous = SOL_TOKEN_EOF;

    for (size_t index = 0; index < tokens->count; ++index) {
        SolToken token = tokens->items[index];
        if (token.kind == SOL_TOKEN_EOF) break;
        if (token.kind == SOL_TOKEN_WHITESPACE) {
            continue;
        }
        if (token.kind == SOL_TOKEN_NEWLINE) {
            ++pending_newlines;
            continue;
        }

        bool current_unary = sol_format_is_unary(tree, token);
        SolFormatAngleRole current_angle = angle_roles[index];
        if (pending_newlines != 0) {
            if (!sol_format_break(&writer, pending_newlines)) goto failure;
        } else if (have_previous && !sol_format_at_line_start(output)
            && sol_format_needs_space(
                previous,
                token.kind,
                previous_angle,
                current_angle,
                previous_unary,
                current_unary
            )) {
            if (!sol_format_append_byte(&writer, ' ')) goto failure;
        }

        if (sol_format_at_line_start(output)) {
            size_t token_depth = depth + continuation_depth;
            if (token.kind == SOL_TOKEN_RIGHT_BRACE && token_depth != 0) --token_depth;
            if ((token.kind == SOL_TOKEN_RIGHT_PAREN
                    || token.kind == SOL_TOKEN_RIGHT_BRACKET)
                && token_depth != 0) {
                --token_depth;
            }
            if (!sol_format_indent(&writer, token_depth)) goto failure;
        }
        if (!sol_format_append_token_text(&writer, source, token)) goto failure;

        if (token.kind == SOL_TOKEN_LEFT_BRACE) {
            ++depth;
        } else if (token.kind == SOL_TOKEN_RIGHT_BRACE && depth != 0) {
            --depth;
        } else if (token.kind == SOL_TOKEN_LEFT_PAREN
            || token.kind == SOL_TOKEN_LEFT_BRACKET) {
            ++continuation_depth;
        } else if ((token.kind == SOL_TOKEN_RIGHT_PAREN
                || token.kind == SOL_TOKEN_RIGHT_BRACKET)
            && continuation_depth != 0) {
            --continuation_depth;
        }
        previous = token.kind;
        previous_angle = current_angle;
        previous_unary = current_unary;
        have_previous = true;
        pending_newlines = 0;
    }

    if (!sol_format_append_byte(&writer, '\n')) goto failure;
    free(angle_roles);
    return !writer.allocation_failed;

failure:
    free(angle_roles);
    return false;
}

static bool sol_format_tokens_equal(
    const SolSource *left_source,
    const SolTokens *left,
    const SolSource *right_source,
    const SolTokens *right
) {
    size_t left_index = 0;
    size_t right_index = 0;
    for (;;) {
        while (left_index < left->count
            && (left->items[left_index].kind == SOL_TOKEN_WHITESPACE
                || left->items[left_index].kind == SOL_TOKEN_NEWLINE)) {
            ++left_index;
        }
        while (right_index < right->count
            && (right->items[right_index].kind == SOL_TOKEN_WHITESPACE
                || right->items[right_index].kind == SOL_TOKEN_NEWLINE)) {
            ++right_index;
        }
        if (left_index == left->count || right_index == right->count) {
            return left_index == left->count && right_index == right->count;
        }

        SolToken left_token = left->items[left_index++];
        SolToken right_token = right->items[right_index++];
        size_t left_length = left_token.span.end - left_token.span.start;
        size_t right_length = right_token.span.end - right_token.span.start;
        if (left_token.kind != right_token.kind || left_length != right_length
            || memcmp(
                left_source->text + left_token.span.start,
                right_source->text + right_token.span.start,
                left_length
            ) != 0) {
            return false;
        }
        if (left_token.kind == SOL_TOKEN_EOF) return true;
    }
}

static bool sol_format_add_error(
    SolDiagnostics *diagnostics,
    const char *code,
    const char *message
) {
    return sol_diagnostics_add(
        diagnostics,
        code,
        SOL_SEVERITY_ERROR,
        (SolSpan){0},
        "%s",
        message
    );
}

static void sol_format_report_oom(SolDiagnostics *diagnostics) {
    sol_format_add_error(
        diagnostics,
        "SOL-INTERNAL-001",
        "out of memory while formatting source"
    );
}

void sol_formatted_init(SolFormatted *formatted) {
    memset(formatted, 0, sizeof(*formatted));
}

void sol_formatted_free(SolFormatted *formatted) {
    free(formatted->text);
    memset(formatted, 0, sizeof(*formatted));
}

bool sol_format_source(
    const SolSource *source,
    SolFormatted *formatted,
    SolDiagnostics *diagnostics
) {
    SolTokens original_tokens;
    SolSyntaxTree original_tree;
    SolFormatted first;
    SolSource first_source;
    SolTokens first_tokens;
    SolSyntaxTree first_tree;
    SolDiagnostics validation_diagnostics;
    SolFormatted second;
    bool first_source_initialized = false;
    bool success = false;

    sol_tokens_init(&original_tokens);
    sol_syntax_tree_init(&original_tree);
    sol_formatted_init(&first);
    memset(&first_source, 0, sizeof(first_source));
    sol_tokens_init(&first_tokens);
    sol_syntax_tree_init(&first_tree);
    sol_diagnostics_init(&validation_diagnostics);
    sol_formatted_init(&second);

    if (memchr(source->text, '\0', source->length) != NULL) {
        sol_format_add_error(
            diagnostics,
            "SOL-FMT-002",
            "formatter input contains an embedded NUL byte"
        );
        goto cleanup;
    }

    if (!sol_lex(source, &original_tokens, diagnostics)) {
        if (!sol_diagnostics_has_errors(diagnostics)) sol_format_report_oom(diagnostics);
        goto cleanup;
    }
    if (sol_diagnostics_has_errors(diagnostics)) goto cleanup;
    if (!sol_parse(source, &original_tokens, &original_tree, diagnostics)) goto cleanup;
    if (sol_diagnostics_has_errors(diagnostics)) goto cleanup;

    if (!sol_format_render_raw(source, &original_tokens, &original_tree, &first)) {
        sol_format_report_oom(diagnostics);
        goto cleanup;
    }
    if (!sol_source_from_text(&first_source, source->path, first.text)) {
        sol_format_report_oom(diagnostics);
        goto cleanup;
    }
    first_source_initialized = true;
    if (!sol_lex(&first_source, &first_tokens, &validation_diagnostics)
        || validation_diagnostics.allocation_failed) {
        sol_format_report_oom(diagnostics);
        goto cleanup;
    }
    if (sol_diagnostics_has_errors(&validation_diagnostics)) {
        sol_format_add_error(
            diagnostics,
            "SOL-FMT-001",
            "formatter produced source that does not lex"
        );
        goto cleanup;
    }
    if (!sol_parse(
        &first_source,
        &first_tokens,
        &first_tree,
        &validation_diagnostics
    )) {
        sol_format_report_oom(diagnostics);
        goto cleanup;
    }
    if (sol_diagnostics_has_errors(&validation_diagnostics)) {
        sol_format_add_error(
            diagnostics,
            "SOL-FMT-001",
            "formatter produced source that does not parse"
        );
        goto cleanup;
    }
    if (!sol_format_tokens_equal(source, &original_tokens, &first_source, &first_tokens)) {
        sol_format_add_error(
            diagnostics,
            "SOL-FMT-001",
            "formatter did not preserve the source token sequence"
        );
        goto cleanup;
    }
    if (!sol_format_render_raw(&first_source, &first_tokens, &first_tree, &second)) {
        sol_format_report_oom(diagnostics);
        goto cleanup;
    }
    if (first.length != second.length
        || memcmp(first.text, second.text, first.length) != 0) {
        sol_format_add_error(
            diagnostics,
            "SOL-FMT-001",
            "formatter output is not idempotent"
        );
        goto cleanup;
    }

    free(formatted->text);
    *formatted = first;
    sol_formatted_init(&first);
    success = true;

cleanup:
    sol_tokens_free(&original_tokens);
    sol_syntax_tree_free(&original_tree);
    sol_formatted_free(&first);
    if (first_source_initialized) sol_source_free(&first_source);
    sol_tokens_free(&first_tokens);
    sol_syntax_tree_free(&first_tree);
    sol_diagnostics_free(&validation_diagnostics);
    sol_formatted_free(&second);
    return success && !diagnostics->allocation_failed;
}
