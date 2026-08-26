#include "sol/lexer.h"
#include "sol/parser.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size > 262144 || size == SIZE_MAX) return 0;
    char *text = malloc(size + 1);
    if (text == NULL) return 0;
    for (size_t index = 0; index < size; ++index) {
        text[index] = data[index] == 0 ? (char)0xff : (char)data[index];
    }
    text[size] = '\0';

    SolSource source;
    SolTokens tokens;
    SolDiagnostics diagnostics;
    SolSyntaxTree syntax;
    sol_tokens_init(&tokens);
    sol_diagnostics_init(&diagnostics);
    sol_syntax_tree_init(&syntax);
    bool loaded = sol_source_from_text(&source, "fuzz.sol", text);
    bool parsed = loaded && sol_lex(&source, &tokens, &diagnostics)
        && sol_parse(&source, &tokens, &syntax, &diagnostics);
    if (parsed && !sol_diagnostics_has_errors(&diagnostics)
        && !sol_syntax_contracts_validate(&source, &syntax)) abort();
    sol_syntax_tree_free(&syntax);
    sol_diagnostics_free(&diagnostics);
    sol_tokens_free(&tokens);
    if (loaded) sol_source_free(&source);
    free(text);
    return 0;
}
