#include "sol/package.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

#define VALID_DIRECTORY SOL_TEST_SOURCE_DIR "/tests/packages/valid"
#define SINGLE_FILE SOL_TEST_SOURCE_DIR "/tests/valid.sol"

typedef struct {
    const char *relative_path;
    const char *module_name;
    size_t import_start;
    size_t import_count;
    size_t item_start;
    size_t item_count;
} ExpectedFile;

static const ExpectedFile expected_files[] = {
    {"/interfaces/display.sol", "example.interfaces", 0, 0, 0, 1},
    {"/main.sol", "example.main", 0, 6, 1, 1},
    {"/models/core.sol", "example.models", 6, 0, 2, 2},
    {"/models/create.sol", "example.models", 6, 0, 4, 1},
    {"/models/display.sol", "example.models", 6, 1, 5, 2},
    {"/rules/numbers.sol", "example.rules", 7, 0, 7, 1},
    {"/services/read.sol", "example.services", 7, 0, 8, 2},
};

static bool span_text_equal(const SolSource *source, SolSpan span, const char *text) {
    size_t length = span.end - span.start;
    return span.end <= source->length
        && strlen(text) == length
        && memcmp(source->text + span.start, text, length) == 0;
}

static void check_valid_package(const SolPackage *package) {
    CHECK(package->is_directory);
    CHECK(strcmp(package->path, VALID_DIRECTORY) == 0);
    CHECK(package->file_count == sizeof(expected_files) / sizeof(expected_files[0]));
    CHECK(package->syntax.import_count == 7);
    CHECK(package->syntax.item_count == 10);
    CHECK(sol_syntax_contracts_validate(&package->source, &package->syntax));

    if (package->file_count != sizeof(expected_files) / sizeof(expected_files[0])) return;
    size_t aggregate_offset = 0;
    for (size_t index = 0; index < package->file_count; ++index) {
        const SolPackageFile *file = &package->files[index];
        const ExpectedFile *expected = &expected_files[index];
        char path[sizeof(VALID_DIRECTORY) + 32];
        int written = snprintf(path, sizeof(path), "%s%s", VALID_DIRECTORY, expected->relative_path);
        CHECK(written >= 0 && (size_t)written < sizeof(path));
        CHECK(strcmp(file->path, path) == 0);
        CHECK(file->aggregate_start == aggregate_offset);
        CHECK(file->aggregate_end == aggregate_offset + file->source.length);
        aggregate_offset = file->aggregate_end;
        if (index + 1 < package->file_count) ++aggregate_offset;
        CHECK(span_text_equal(&package->source, file->module_name, expected->module_name));
        CHECK(file->import_start == expected->import_start);
        CHECK(file->import_count == expected->import_count);
        CHECK(file->item_start == expected->item_start);
        CHECK(file->item_count == expected->item_count);
    }
    CHECK(package->source.length == aggregate_offset);

    CHECK(span_text_equal(&package->source, package->syntax.module_name, "example.interfaces"));
    CHECK(expected_files[2].item_start + expected_files[2].item_count
        == expected_files[3].item_start);
    CHECK(expected_files[3].item_start + expected_files[3].item_count
        == expected_files[4].item_start);
}

static void test_directory_load_and_boundaries(void) {
    SolPackage package;
    SolDiagnostics diagnostics;
    char error[256];
    sol_package_init(&package);
    sol_diagnostics_init(&diagnostics);

    CHECK(sol_package_file_at(&package, 0) == NULL);
    CHECK(sol_package_load(&package, VALID_DIRECTORY, &diagnostics, error, sizeof(error)));
    CHECK(error[0] == '\0');
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    check_valid_package(&package);

    for (size_t index = 0; index < package.file_count; ++index) {
        const SolPackageFile *file = &package.files[index];
        CHECK(sol_package_file_at(&package, file->aggregate_start) == file);
        CHECK(sol_package_file_at(&package, file->aggregate_end) == file);
        if (index + 1 < package.file_count) {
            CHECK(sol_package_file_at(&package, file->aggregate_end + 1)
                == &package.files[index + 1]);
        }
    }
    CHECK(sol_package_file_at(&package, package.source.length + 1) == NULL);
    CHECK(sol_package_file_at(&package, SIZE_MAX) == NULL);

    size_t item_count = package.syntax.item_count;
    package.syntax.item_count = package.syntax.item_capacity + 1;
    CHECK(!sol_syntax_contracts_validate(&package.source, &package.syntax));
    package.syntax.item_count = item_count;
    CHECK(sol_syntax_contracts_validate(&package.source, &package.syntax));

    sol_diagnostics_free(&diagnostics);
    sol_package_free(&package);
}

static void test_package_reuse(void) {
    SolPackage package;
    SolDiagnostics diagnostics;
    char error[256];
    sol_package_init(&package);
    sol_diagnostics_init(&diagnostics);

    CHECK(sol_package_load(&package, VALID_DIRECTORY, &diagnostics, error, sizeof(error)));
    CHECK(sol_package_load(&package, SINGLE_FILE, &diagnostics, error, sizeof(error)));
    CHECK(!package.is_directory);
    CHECK(package.file_count == 1);
    CHECK(package.source.length == package.files[0].source.length);
    CHECK(package.syntax.import_count == 1);
    CHECK(package.syntax.item_count == 24);
    CHECK(strcmp(package.path, SINGLE_FILE) == 0);
    CHECK(strcmp(package.files[0].path, SINGLE_FILE) == 0);
    CHECK(package.files[0].aggregate_start == 0);
    CHECK(package.files[0].aggregate_end == package.source.length);
    CHECK(sol_package_file_at(&package, package.source.length) == &package.files[0]);
    CHECK(sol_syntax_contracts_validate(&package.source, &package.syntax));

    CHECK(sol_package_load(&package, VALID_DIRECTORY, &diagnostics, error, sizeof(error)));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    check_valid_package(&package);

    sol_diagnostics_free(&diagnostics);
    sol_package_free(&package);
}

int main(void) {
    test_directory_load_and_boundaries();
    test_package_reuse();
    if (failures != 0) {
        fprintf(stderr, "%d package test failure(s)\n", failures);
        return 1;
    }
    puts("package tests passed");
    return 0;
}
