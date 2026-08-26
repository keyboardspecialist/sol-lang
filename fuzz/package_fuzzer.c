#define _DARWIN_C_SOURCE
#define _POSIX_C_SOURCE 200809L

#include "sol/package.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

static char directory[] = "/tmp/sol-package-fuzz-XXXXXX";
static char source_path[sizeof(directory) + 12];

static void cleanup(void) {
    if (source_path[0] != '\0') (void)unlink(source_path);
    if (directory[0] != '\0') (void)rmdir(directory);
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    static bool initialized;
    if (size > 262144) return 0;
    if (!initialized) {
        if (mkdtemp(directory) == NULL) return 0;
        (void)snprintf(source_path, sizeof(source_path), "%s/input.sol", directory);
        if (atexit(cleanup) != 0) {
            cleanup();
            return 0;
        }
        initialized = true;
    }
    FILE *stream = fopen(source_path, "wb");
    if (stream == NULL) return 0;
    bool written = fwrite(data, 1, size, stream) == size;
    if (fclose(stream) != 0) written = false;
    if (!written) {
        (void)unlink(source_path);
        return 0;
    }
    SolPackage package;
    SolDiagnostics diagnostics;
    char error[256];
    sol_package_init(&package);
    sol_diagnostics_init(&diagnostics);
    bool loaded = sol_package_load_directory(
        &package, directory, &diagnostics, error, sizeof(error));
    if (loaded && !sol_diagnostics_has_errors(&diagnostics)
        && !sol_syntax_contracts_validate(&package.source, &package.syntax)) abort();
    sol_diagnostics_free(&diagnostics);
    sol_package_free(&package);
    (void)unlink(source_path);
    return 0;
}
