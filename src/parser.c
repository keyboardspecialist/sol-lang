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
    bool allocation_failed;
} SolParser;

void sol_syntax_tree_init(SolSyntaxTree *tree) {
    memset(tree, 0, sizeof(*tree));
}

void sol_syntax_tree_free(SolSyntaxTree *tree) {
    free(tree->items);
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

static bool sol_parser_type(SolParser *parser);

static bool sol_parser_type_arguments(SolParser *parser) {
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
    for (;;) {
        if (!sol_parser_type(parser)) {
            return false;
        }
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

static bool sol_parser_type(SolParser *parser) {
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
    if (sol_parser_match(parser, SOL_TOKEN_LEFT_PAREN)) {
        parsed = sol_parser_expect(
            parser,
            SOL_TOKEN_RIGHT_PAREN,
            "expected ')' to complete the unit type"
        );
    } else {
        sol_parser_match(parser, SOL_TOKEN_CAPABILITY);
        SolSpan ignored;
        parsed = sol_parser_path(parser, &ignored, "expected a type name")
            && sol_parser_type_arguments(parser);
    }
    --parser->type_depth;
    return parsed;
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

static bool sol_parser_named_type_list(SolParser *parser, SolTokenKind closing) {
    if (sol_parser_match(parser, closing)) {
        return true;
    }
    for (;;) {
        if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a name")) {
            return false;
        }
        if (!sol_parser_expect(parser, SOL_TOKEN_COLON, "expected ':' after the name")) {
            return false;
        }
        if (!sol_parser_type(parser)) {
            return false;
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

static bool sol_parser_function(SolParser *parser, bool member, SolSpan *name, SolSpan *whole) {
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
    if (!sol_parser_named_type_list(parser, SOL_TOKEN_RIGHT_PAREN)) {
        return false;
    }
    if (!sol_parser_expect(parser, SOL_TOKEN_ARROW, "expected '->' after function parameters")) {
        return false;
    }
    if (!sol_parser_type(parser)) {
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
        sol_parser_balanced_block(parser, "expected a block after the function clause");
    }

    if (sol_parser_kind(parser) == SOL_TOKEN_LEFT_BRACE) {
        if (!sol_parser_balanced_block(parser, "expected a function body")) {
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

static bool sol_parser_record(SolParser *parser, SolSpan *name, SolSpan *whole) {
    SolToken opening = sol_parser_advance(parser);
    SolToken name_token = sol_parser_current(parser);
    if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected a record name")) {
        return false;
    }
    *name = name_token.span;
    if (!sol_parser_expect(parser, SOL_TOKEN_LEFT_BRACE, "expected '{' after the record name")) {
        return false;
    }
    if (!sol_parser_named_type_list(parser, SOL_TOKEN_RIGHT_BRACE)) {
        return false;
    }
    SolToken previous = parser->tokens->items[parser->cursor - 1];
    *whole = (SolSpan){.start = opening.span.start, .end = previous.span.end};
    return true;
}

static bool sol_parser_enum(SolParser *parser, SolSpan *name, SolSpan *whole) {
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
        if (!sol_parser_expect(parser, SOL_TOKEN_IDENTIFIER, "expected an enum variant")) {
            return false;
        }
        if (sol_parser_match(parser, SOL_TOKEN_LEFT_PAREN)
            && !sol_parser_named_type_list(parser, SOL_TOKEN_RIGHT_PAREN)) {
            return false;
        }
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

static bool sol_parser_capability(SolParser *parser, SolSpan *name, SolSpan *whole) {
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
        SolSpan member_name;
        SolSpan member_span;
        if (!sol_parser_function(parser, true, &member_name, &member_span)) {
            if (sol_parser_kind(parser) != SOL_TOKEN_EOF) {
                sol_parser_advance(parser);
            }
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
    bool parsed = false;
    SolTokenKind kind = sol_parser_kind(parser);
    if (kind == SOL_TOKEN_RECORD) {
        item_kind = SOL_ITEM_RECORD;
        parsed = sol_parser_record(parser, &name, &whole);
    } else if (kind == SOL_TOKEN_OPEN || kind == SOL_TOKEN_ENUM) {
        if (kind == SOL_TOKEN_OPEN) {
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
        parsed = sol_parser_enum(parser, &name, &whole);
    } else if (kind == SOL_TOKEN_CAPABILITY) {
        item_kind = SOL_ITEM_CAPABILITY;
        parsed = sol_parser_capability(parser, &name, &whole);
    } else if (kind == SOL_TOKEN_FUNCTION) {
        item_kind = SOL_ITEM_FUNCTION;
        parsed = sol_parser_function(parser, false, &name, &whole);
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
        });
    } else {
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
