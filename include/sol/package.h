#ifndef SOL_PACKAGE_H
#define SOL_PACKAGE_H

#include "sol/diagnostic.h"
#include "sol/parser.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>

typedef struct {
    char *path;
    SolSource source;
    size_t aggregate_start;
    size_t aggregate_end;
    SolSpan module_name;
    size_t import_start;
    size_t import_count;
    size_t item_start;
    size_t item_count;
} SolPackageFile;

typedef struct {
    char *path;
    bool is_directory;
    SolSource source;
    SolSyntaxTree syntax;
    SolPackageFile *files;
    size_t file_count;
    size_t file_capacity;
} SolPackage;

/* Initializes an empty package. Call once before loading or freeing the package. */
void sol_package_init(SolPackage *package);
void sol_package_free(SolPackage *package);

/*
 * Loads one regular .sol file, or recursively aggregates a directory.
 * The package must be initialized. Each load discards its prior state, including
 * state left by a failed load; diagnostics remain owned by the caller.
 */
bool sol_package_load(
    SolPackage *package,
    const char *path,
    SolDiagnostics *diagnostics,
    char *error,
    size_t error_size
);

/* Behaves like sol_package_load, but requires a directory. */
bool sol_package_load_directory(
    SolPackage *package,
    const char *directory,
    SolDiagnostics *diagnostics,
    char *error,
    size_t error_size
);

const SolPackageFile *sol_package_file_at(const SolPackage *package, size_t aggregate_offset);
void sol_package_diagnostics_render_human(
    FILE *stream,
    const SolPackage *package,
    const SolDiagnostics *diagnostics
);
void sol_package_diagnostics_render_json(
    FILE *stream,
    const SolPackage *package,
    const SolDiagnostics *diagnostics
);

#endif
