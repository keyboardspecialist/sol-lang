#include "sol/contract.h"
#include "sol/diagnostic.h"
#include "sol/effectcheck.h"
#include "sol/hir.h"
#include "sol/lexer.h"
#include "sol/package.h"
#include "sol/parser.h"
#include "sol/source.h"
#include "sol/typecheck.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void sol_print_usage(FILE *stream) {
    fputs(
        "usage: sol check [--diagnostic-format=human|json] <file.sol|package-directory>\n"
        "       sol --version\n",
        stream
    );
}

static int sol_check_path(const char *path, bool json) {
    SolPackage package;
    SolDiagnostics diagnostics;
    char load_error[512];
    sol_package_init(&package);
    sol_diagnostics_init(&diagnostics);
    if (!sol_package_load(&package, path, &diagnostics, load_error, sizeof(load_error))) {
        fprintf(stderr, "sol: %s\n", load_error);
        sol_diagnostics_free(&diagnostics);
        sol_package_free(&package);
        return 1;
    }

    SolHirModule hir;
    SolTypeTable types;
    SolEffectTable effects;
    SolContractTable contracts;
    sol_hir_module_init(&hir);
    sol_type_table_init(&types);
    sol_effect_table_init(&effects);
    sol_contract_table_init(&contracts);

    bool completed = true;
    if (completed && !sol_diagnostics_has_errors(&diagnostics)) {
        if (!package.is_directory) {
            completed = sol_hir_lower(
                &package.source, &package.syntax, &hir, &diagnostics
            );
        } else {
            SolHirFileScope *scopes = malloc(
                package.file_count * sizeof(*scopes)
            );
            if (scopes == NULL) {
                sol_diagnostics_add(
                    &diagnostics,
                    "SOL-INTERNAL-001",
                    SOL_SEVERITY_ERROR,
                    (SolSpan){0},
                    "out of memory while constructing package scopes"
                );
                completed = false;
            } else {
                for (size_t index = 0; index < package.file_count; ++index) {
                    scopes[index] = (SolHirFileScope){
                        .module_name = package.files[index].module_name,
                        .import_start = package.files[index].import_start,
                        .import_count = package.files[index].import_count,
                        .item_start = package.files[index].item_start,
                        .item_count = package.files[index].item_count,
                    };
                }
                completed = sol_hir_lower_scoped(
                    &package.source,
                    &package.syntax,
                    scopes,
                    package.file_count,
                    &hir,
                    &diagnostics
                );
                free(scopes);
            }
        }
    }
    if (completed && !sol_diagnostics_has_errors(&diagnostics)) {
        completed = sol_type_check(
            &package.source, &package.syntax, &hir, &types, &diagnostics
        );
    }
    if (completed && !sol_diagnostics_has_errors(&diagnostics)) {
        completed = sol_effect_check(
            &package.source,
            &package.syntax,
            &hir,
            &types,
            &effects,
            &diagnostics
        );
    }
    if (completed && !sol_diagnostics_has_errors(&diagnostics)) {
        completed = sol_contract_lower(
            &package.source,
            &package.syntax,
            &hir,
            &types,
            &effects,
            &contracts,
            &diagnostics
        );
    }
    bool failed = !completed || sol_diagnostics_has_errors(&diagnostics);

    if (json) {
        sol_package_diagnostics_render_json(stdout, &package, &diagnostics);
    } else if (diagnostics.count != 0) {
        sol_package_diagnostics_render_human(stderr, &package, &diagnostics);
    } else if (failed) {
        fputs("sol: compilation stopped because the compiler ran out of memory\n", stderr);
    } else {
        if (package.is_directory) {
            printf(
                "checked %s: %zu file%s, %zu declaration%s\n",
                path,
                package.file_count,
                package.file_count == 1 ? "" : "s",
                package.syntax.item_count,
                package.syntax.item_count == 1 ? "" : "s"
            );
        } else {
            printf(
                "checked %s: %zu declaration%s\n",
                path,
                package.syntax.item_count,
                package.syntax.item_count == 1 ? "" : "s"
            );
        }
    }

    sol_contract_table_free(&contracts);
    sol_effect_table_free(&effects);
    sol_type_table_free(&types);
    sol_hir_module_free(&hir);
    sol_diagnostics_free(&diagnostics);
    sol_package_free(&package);
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
            fputs("sol: check accepts one source file or package directory\n", stderr);
            return 2;
        } else {
            path = argv[index];
        }
    }
    if (path == NULL) {
        sol_print_usage(stderr);
        return 2;
    }
    return sol_check_path(path, json);
}
