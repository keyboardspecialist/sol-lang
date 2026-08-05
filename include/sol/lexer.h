#ifndef SOL_LEXER_H
#define SOL_LEXER_H

#include "sol/diagnostic.h"
#include "sol/token.h"

#include <stdbool.h>

void sol_tokens_init(SolTokens *tokens);
void sol_tokens_free(SolTokens *tokens);
bool sol_lex(
    const SolSource *source,
    SolTokens *tokens,
    SolDiagnostics *diagnostics
);

#endif
