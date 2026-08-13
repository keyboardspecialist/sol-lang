#include "sol/lexer.h"

#include <ctype.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    const char *text;
    SolTokenKind kind;
} SolKeyword;

static const SolKeyword SOL_KEYWORDS[] = {
    {"module", SOL_TOKEN_MODULE},
    {"edition", SOL_TOKEN_EDITION},
    {"use", SOL_TOKEN_USE},
    {"public", SOL_TOKEN_PUBLIC},
    {"private", SOL_TOKEN_PRIVATE},
    {"record", SOL_TOKEN_RECORD},
    {"enum", SOL_TOKEN_ENUM},
    {"type", SOL_TOKEN_TYPE},
    {"distinct", SOL_TOKEN_DISTINCT},
    {"refined", SOL_TOKEN_REFINED},
    {"where", SOL_TOKEN_WHERE},
    {"open", SOL_TOKEN_OPEN},
    {"capability", SOL_TOKEN_CAPABILITY},
    {"trait", SOL_TOKEN_TRAIT},
    {"implementation", SOL_TOKEN_IMPLEMENTATION},
    {"for", SOL_TOKEN_FOR},
    {"function", SOL_TOKEN_FUNCTION},
    {"effects", SOL_TOKEN_EFFECTS},
    {"requires", SOL_TOKEN_REQUIRES},
    {"ensures", SOL_TOKEN_ENSURES},
    {"pure", SOL_TOKEN_PURE},
    {"let", SOL_TOKEN_LET},
    {"return", SOL_TOKEN_RETURN},
    {"if", SOL_TOKEN_IF},
    {"else", SOL_TOKEN_ELSE},
    {"match", SOL_TOKEN_MATCH},
    {"handle", SOL_TOKEN_HANDLE},
    {"with", SOL_TOKEN_WITH},
    {"true", SOL_TOKEN_TRUE},
    {"false", SOL_TOKEN_FALSE},
};

void sol_tokens_init(SolTokens *tokens) {
    memset(tokens, 0, sizeof(*tokens));
}

void sol_tokens_free(SolTokens *tokens) {
    free(tokens->items);
    memset(tokens, 0, sizeof(*tokens));
}

static bool sol_tokens_add(SolTokens *tokens, SolTokenKind kind, size_t start, size_t end) {
    if (tokens->count == tokens->capacity) {
        if (tokens->capacity > SIZE_MAX / 2) {
            return false;
        }
        size_t capacity = tokens->capacity == 0 ? 64 : tokens->capacity * 2;
        if (capacity > SIZE_MAX / sizeof(*tokens->items)) {
            return false;
        }
        SolToken *items = realloc(tokens->items, capacity * sizeof(*items));
        if (items == NULL) {
            return false;
        }
        tokens->items = items;
        tokens->capacity = capacity;
    }
    tokens->items[tokens->count++] = (SolToken){
        .kind = kind,
        .span = {.start = start, .end = end},
    };
    return true;
}

bool sol_token_is_trivia(SolTokenKind kind) {
    return kind == SOL_TOKEN_WHITESPACE || kind == SOL_TOKEN_NEWLINE
        || kind == SOL_TOKEN_LINE_COMMENT || kind == SOL_TOKEN_BLOCK_COMMENT;
}

bool sol_token_text_equal(const SolSource *source, SolToken token, const char *text) {
    size_t length = token.span.end - token.span.start;
    return strlen(text) == length
        && memcmp(source->text + token.span.start, text, length) == 0;
}

static SolTokenKind sol_identifier_kind(const SolSource *source, size_t start, size_t end) {
    SolToken token = {.span = {.start = start, .end = end}};
    size_t keyword_count = sizeof(SOL_KEYWORDS) / sizeof(SOL_KEYWORDS[0]);
    for (size_t index = 0; index < keyword_count; ++index) {
        if (sol_token_text_equal(source, token, SOL_KEYWORDS[index].text)) {
            return SOL_KEYWORDS[index].kind;
        }
    }
    return SOL_TOKEN_IDENTIFIER;
}

static bool sol_is_identifier_start(unsigned char byte) {
    return byte == '_' || isalpha(byte) != 0;
}

static bool sol_is_identifier_continue(unsigned char byte) {
    return byte == '_' || isalnum(byte) != 0;
}

static bool sol_lex_string(
    const SolSource *source,
    SolTokens *tokens,
    SolDiagnostics *diagnostics,
    size_t *cursor
) {
    size_t start = *cursor;
    ++*cursor;
    bool terminated = false;
    while (*cursor < source->length) {
        char current = source->text[*cursor];
        if (current == '"') {
            ++*cursor;
            terminated = true;
            break;
        }
        if (current == '\n' || current == '\r') {
            break;
        }
        if (current == '\\' && *cursor + 1 < source->length) {
            *cursor += 2;
        } else {
            ++*cursor;
        }
    }
    if (!terminated) {
        sol_diagnostics_add(
            diagnostics,
            "SOL-LEX-002",
            SOL_SEVERITY_ERROR,
            (SolSpan){.start = start, .end = *cursor},
            "unterminated string literal"
        );
    }
    return sol_tokens_add(tokens, SOL_TOKEN_STRING, start, *cursor);
}

static bool sol_lex_block_comment(
    const SolSource *source,
    SolTokens *tokens,
    SolDiagnostics *diagnostics,
    size_t *cursor
) {
    size_t start = *cursor;
    *cursor += 2;
    size_t depth = 1;
    while (*cursor < source->length && depth > 0) {
        if (*cursor + 1 < source->length
            && source->text[*cursor] == '/'
            && source->text[*cursor + 1] == '*') {
            ++depth;
            *cursor += 2;
        } else if (*cursor + 1 < source->length
            && source->text[*cursor] == '*'
            && source->text[*cursor + 1] == '/') {
            --depth;
            *cursor += 2;
        } else {
            ++*cursor;
        }
    }
    if (depth != 0) {
        sol_diagnostics_add(
            diagnostics,
            "SOL-LEX-003",
            SOL_SEVERITY_ERROR,
            (SolSpan){.start = start, .end = *cursor},
            "unterminated block comment"
        );
    }
    return sol_tokens_add(tokens, SOL_TOKEN_BLOCK_COMMENT, start, *cursor);
}

#define SOL_ADD_SINGLE(kind) \
    do { \
        if (!sol_tokens_add(tokens, (kind), cursor, cursor + 1)) return false; \
        ++cursor; \
    } while (0)

#define SOL_ADD_DOUBLE(kind) \
    do { \
        if (!sol_tokens_add(tokens, (kind), cursor, cursor + 2)) return false; \
        cursor += 2; \
    } while (0)

bool sol_lex(
    const SolSource *source,
    SolTokens *tokens,
    SolDiagnostics *diagnostics
) {
    size_t cursor = 0;
    while (cursor < source->length) {
        unsigned char byte = (unsigned char)source->text[cursor];
        size_t start = cursor;

        if (byte == ' ' || byte == '\t') {
            do {
                ++cursor;
            } while (cursor < source->length
                && (source->text[cursor] == ' ' || source->text[cursor] == '\t'));
            if (!sol_tokens_add(tokens, SOL_TOKEN_WHITESPACE, start, cursor)) return false;
            continue;
        }

        if (byte == '\n' || byte == '\r') {
            if (byte == '\r' && cursor + 1 < source->length && source->text[cursor + 1] == '\n') {
                cursor += 2;
            } else {
                ++cursor;
            }
            if (!sol_tokens_add(tokens, SOL_TOKEN_NEWLINE, start, cursor)) return false;
            continue;
        }

        if (sol_is_identifier_start(byte)) {
            ++cursor;
            while (cursor < source->length
                && sol_is_identifier_continue((unsigned char)source->text[cursor])) {
                ++cursor;
            }
            SolTokenKind kind = sol_identifier_kind(source, start, cursor);
            if (!sol_tokens_add(tokens, kind, start, cursor)) return false;
            continue;
        }

        if (isdigit(byte) != 0) {
            ++cursor;
            while (cursor < source->length
                && (isdigit((unsigned char)source->text[cursor]) != 0
                    || source->text[cursor] == '_')) {
                ++cursor;
            }
            if (!sol_tokens_add(tokens, SOL_TOKEN_INTEGER, start, cursor)) return false;
            continue;
        }

        if (byte == '"') {
            if (!sol_lex_string(source, tokens, diagnostics, &cursor)) return false;
            continue;
        }

        if (byte == '/' && cursor + 1 < source->length && source->text[cursor + 1] == '/') {
            cursor += 2;
            while (cursor < source->length
                && source->text[cursor] != '\n' && source->text[cursor] != '\r') {
                ++cursor;
            }
            if (!sol_tokens_add(tokens, SOL_TOKEN_LINE_COMMENT, start, cursor)) return false;
            continue;
        }

        if (byte == '/' && cursor + 1 < source->length && source->text[cursor + 1] == '*') {
            if (!sol_lex_block_comment(source, tokens, diagnostics, &cursor)) return false;
            continue;
        }

        switch (byte) {
            case '(': SOL_ADD_SINGLE(SOL_TOKEN_LEFT_PAREN); break;
            case ')': SOL_ADD_SINGLE(SOL_TOKEN_RIGHT_PAREN); break;
            case '{': SOL_ADD_SINGLE(SOL_TOKEN_LEFT_BRACE); break;
            case '}': SOL_ADD_SINGLE(SOL_TOKEN_RIGHT_BRACE); break;
            case '[': SOL_ADD_SINGLE(SOL_TOKEN_LEFT_BRACKET); break;
            case ']': SOL_ADD_SINGLE(SOL_TOKEN_RIGHT_BRACKET); break;
            case '+': SOL_ADD_SINGLE(SOL_TOKEN_PLUS); break;
            case '*': SOL_ADD_SINGLE(SOL_TOKEN_STAR); break;
            case '%': SOL_ADD_SINGLE(SOL_TOKEN_PERCENT); break;
            case '.': SOL_ADD_SINGLE(SOL_TOKEN_DOT); break;
            case ',': SOL_ADD_SINGLE(SOL_TOKEN_COMMA); break;
            case ':': SOL_ADD_SINGLE(SOL_TOKEN_COLON); break;
            case '@': SOL_ADD_SINGLE(SOL_TOKEN_AT); break;
            case '?': SOL_ADD_SINGLE(SOL_TOKEN_QUESTION); break;
            case '/': SOL_ADD_SINGLE(SOL_TOKEN_SLASH); break;
            case '-':
                if (cursor + 1 < source->length && source->text[cursor + 1] == '>') {
                    SOL_ADD_DOUBLE(SOL_TOKEN_ARROW);
                } else {
                    SOL_ADD_SINGLE(SOL_TOKEN_MINUS);
                }
                break;
            case '=':
                if (cursor + 1 < source->length && source->text[cursor + 1] == '=') {
                    SOL_ADD_DOUBLE(SOL_TOKEN_EQUAL_EQUAL);
                } else if (cursor + 1 < source->length && source->text[cursor + 1] == '>') {
                    SOL_ADD_DOUBLE(SOL_TOKEN_FAT_ARROW);
                } else {
                    SOL_ADD_SINGLE(SOL_TOKEN_EQUAL);
                }
                break;
            case '!':
                if (cursor + 1 < source->length && source->text[cursor + 1] == '=') {
                    SOL_ADD_DOUBLE(SOL_TOKEN_BANG_EQUAL);
                } else {
                    SOL_ADD_SINGLE(SOL_TOKEN_BANG);
                }
                break;
            case '<':
                if (cursor + 1 < source->length && source->text[cursor + 1] == '=') {
                    SOL_ADD_DOUBLE(SOL_TOKEN_LESS_EQUAL);
                } else {
                    SOL_ADD_SINGLE(SOL_TOKEN_LESS);
                }
                break;
            case '>':
                if (cursor + 1 < source->length && source->text[cursor + 1] == '=') {
                    SOL_ADD_DOUBLE(SOL_TOKEN_GREATER_EQUAL);
                } else {
                    SOL_ADD_SINGLE(SOL_TOKEN_GREATER);
                }
                break;
            case '&':
                if (cursor + 1 < source->length && source->text[cursor + 1] == '&') {
                    SOL_ADD_DOUBLE(SOL_TOKEN_AMP_AMP);
                } else {
                    goto invalid_character;
                }
                break;
            case '|':
                if (cursor + 1 < source->length && source->text[cursor + 1] == '|') {
                    SOL_ADD_DOUBLE(SOL_TOKEN_PIPE_PIPE);
                } else {
                    goto invalid_character;
                }
                break;
            default:
invalid_character:
                ++cursor;
                if (!sol_tokens_add(tokens, SOL_TOKEN_INVALID, start, cursor)) return false;
                sol_diagnostics_add(
                    diagnostics,
                    "SOL-LEX-001",
                    SOL_SEVERITY_ERROR,
                    (SolSpan){.start = start, .end = cursor},
                    "invalid source byte 0x%02x",
                    (unsigned int)byte
                );
                break;
        }
    }

    return sol_tokens_add(tokens, SOL_TOKEN_EOF, source->length, source->length)
        && !diagnostics->allocation_failed;
}

#undef SOL_ADD_SINGLE
#undef SOL_ADD_DOUBLE

const char *sol_token_kind_name(SolTokenKind kind) {
    static const char *names[] = {
        "end of file", "invalid token", "whitespace", "newline", "line comment",
        "block comment", "identifier", "integer", "string", "module", "edition",
        "use", "public", "private", "record", "enum", "type", "distinct", "refined", "where", "open", "capability",
        "trait", "implementation", "for", "function", "effects", "requires", "ensures", "pure", "let", "return",
        "if", "else", "match", "handle", "with", "true", "false", "(", ")", "{", "}", "[",
        "]", "<", ">", "<=", ">=", "=", "==", "!", "!=", "+", "-", "*",
        "/", "%", "&&", "||", ".", ",", ":", "@", "?", "->", "=>",
    };
    size_t count = sizeof(names) / sizeof(names[0]);
    size_t index = (size_t)kind;
    return index < count ? names[index] : "unknown token";
}
