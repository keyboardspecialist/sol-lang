#include "sol/diagnostic.h"
#include "sol/effectcheck.h"
#include "sol/hir.h"
#include "sol/lexer.h"
#include "sol/parser.h"
#include "sol/source.h"
#include "sol/typecheck.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static void sol_print_usage(FILE *stream) {
    fputs(
        "usage: sol check [--diagnostic-format=human|json] <file.sol>\n"
        "       sol --version\n",
        stream
    );
}

static int sol_check_file(const char *path, bool json) {
    SolSource source;
    char load_error[512];
    if (!sol_source_load(&source, path, load_error, sizeof(load_error))) {
        fprintf(stderr, "sol: %s\n", load_error);
        return 1;
    }

    SolDiagnostics diagnostics;
    SolTokens tokens;
    SolSyntaxTree tree;
    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    sol_diagnostics_init(&diagnostics);
    sol_tokens_init(&tokens);
    sol_syntax_tree_init(&tree);
    sol_hir_module_init(&hir);
    sol_type_table_init(&types);
    sol_effect_table_init(&effects);

    bool completed = sol_lex(&source, &tokens, &diagnostics);
    if (completed) {
        completed = sol_parse(&source, &tokens, &tree, &diagnostics);
    }
    if (completed && !sol_diagnostics_has_errors(&diagnostics)) {
        completed = sol_hir_lower(&source, &tree, &hir, &diagnostics);
    }
    if (completed && !sol_diagnostics_has_errors(&diagnostics)) {
        completed = sol_type_check(&source, &tree, &hir, &types, &diagnostics);
    }
    if (completed && !sol_diagnostics_has_errors(&diagnostics)) {
        completed = sol_effect_check(&source, &tree, &hir, &types, &effects, &diagnostics);
    }
    bool failed = !completed || sol_diagnostics_has_errors(&diagnostics);

    if (json) {
        sol_diagnostics_render_json(stdout, &source, &diagnostics);
    } else if (diagnostics.count != 0) {
        sol_diagnostics_render_human(stderr, &source, &diagnostics);
    } else if (failed) {
        fputs("sol: compilation stopped because the compiler ran out of memory\n", stderr);
    } else {
        printf("checked %s: %zu declaration%s\n", path, tree.item_count, tree.item_count == 1 ? "" : "s");
    }

    sol_effect_table_free(&effects);
    sol_type_table_free(&types);
    sol_hir_module_free(&hir);
    sol_syntax_tree_free(&tree);
    sol_tokens_free(&tokens);
    sol_diagnostics_free(&diagnostics);
    sol_source_free(&source);
    return failed ? 1 : 0;
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("sol 0.1.0-dev");
        return 0;
    }
    if (argc < 3 || strcmp(argv[1], "check") != 0) {
        sol_print_usage(stderr);
        return 2;
    }

    bool json = false;
    const char *path = NULL;
    for (int index = 2; index < argc; ++index) {
        if (strcmp(argv[index], "--diagnostic-format=json") == 0) {
            json = true;
        } else if (strcmp(argv[index], "--diagnostic-format=human") == 0) {
            json = false;
        } else if (argv[index][0] == '-') {
            fprintf(stderr, "sol: unknown option '%s'\n", argv[index]);
            return 2;
        } else if (path != NULL) {
            fputs("sol: check currently accepts one source file\n", stderr);
            return 2;
        } else {
            path = argv[index];
        }
    }
    if (path == NULL) {
        sol_print_usage(stderr);
        return 2;
    }
    return sol_check_file(path, json);
}
