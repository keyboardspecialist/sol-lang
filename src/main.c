#define _POSIX_C_SOURCE 200809L

#include "sol/compilation.h"
#include "sol/diagnostic.h"
#include "sol/formatter.h"
#include "sol/interpreter.h"
#include "sol/package.h"
#include "sol/source.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static void sol_print_usage(FILE *stream) {
    fputs(
        "usage: sol check [--diagnostic-format=human|json] <file.sol|package-directory>\n"
        "       sol inspect <file.sol|package-directory>\n"
        "       sol effects [--diagnostic-format=human|json] <file.sol|package-directory>\n"
        "       sol test [--diagnostic-format=human|json] <file.sol|package-directory>\n"
        "       sol fmt [--check|--stdout] <file.sol|package-directory>\n"
        "       sol --version\n",
        stream
    );
}

static bool sol_write_all(int descriptor, const char *text, size_t length) {
    size_t written = 0;
    while (written < length) {
        ssize_t result = write(descriptor, text + written, length - written);
        if (result < 0) {
            if (errno == EINTR) continue;
            return false;
        }
        if (result == 0) {
            errno = EIO;
            return false;
        }
        written += (size_t)result;
    }
    return true;
}

typedef struct {
    const char *path;
    const SolSource *source;
    struct stat loaded_status;
    char *temporary;
    char *backup;
    bool original_in_backup;
} SolFormatWrite;

static bool sol_same_file_status(const struct stat *left, const struct stat *right) {
    return left->st_dev == right->st_dev
        && left->st_ino == right->st_ino
        && left->st_mode == right->st_mode
        && left->st_nlink == right->st_nlink
        && left->st_uid == right->st_uid
        && left->st_gid == right->st_gid
        && left->st_rdev == right->st_rdev
        && left->st_size == right->st_size
        && left->st_mtime == right->st_mtime
        && left->st_ctime == right->st_ctime;
}

static bool sol_format_write_unchanged(const SolFormatWrite *write, bool *changed) {
    *changed = false;
    struct stat path_status;
    if (lstat(write->path, &path_status) != 0) {
        if (errno == ENOENT || errno == ENOTDIR) *changed = true;
        return false;
    }
    if (!S_ISREG(path_status.st_mode)
        || !sol_same_file_status(&write->loaded_status, &path_status)) {
        *changed = true;
        return false;
    }

    int descriptor = open(write->path, O_RDONLY);
    if (descriptor < 0) {
        if (errno == ENOENT || errno == ENOTDIR || errno == ELOOP) *changed = true;
        return false;
    }
    struct stat before;
    struct stat after;
    bool complete = fstat(descriptor, &before) == 0;
    if (complete && (!S_ISREG(before.st_mode)
        || !sol_same_file_status(&write->loaded_status, &before))) {
        *changed = true;
        complete = false;
    }
    size_t offset = 0;
    char buffer[16384];
    while (complete && offset < write->source->length) {
        size_t remaining = write->source->length - offset;
        size_t requested = remaining < sizeof(buffer) ? remaining : sizeof(buffer);
        ssize_t received = read(descriptor, buffer, requested);
        if (received < 0) {
            if (errno == EINTR) continue;
            complete = false;
        } else if (received == 0) {
            *changed = true;
            complete = false;
        } else if (memcmp(buffer, write->source->text + offset, (size_t)received) != 0) {
            *changed = true;
            complete = false;
        } else {
            offset += (size_t)received;
        }
    }
    if (complete) {
        char extra;
        ssize_t received;
        do {
            received = read(descriptor, &extra, 1);
        } while (received < 0 && errno == EINTR);
        if (received > 0) {
            *changed = true;
            complete = false;
        } else if (received < 0) {
            complete = false;
        }
    }
    if (complete && fstat(descriptor, &after) != 0) complete = false;
    if (complete && (!sol_same_file_status(&before, &after)
        || !sol_same_file_status(&write->loaded_status, &after))) {
        *changed = true;
        complete = false;
    }
    int error = errno;
    if (close(descriptor) != 0 && complete) {
        complete = false;
        error = errno;
    }
    errno = error;
    return complete;
}

static bool sol_create_sibling(
    const char *path, const char *suffix, char **temporary, int *descriptor
) {
    size_t path_length = strlen(path);
    size_t suffix_length = strlen(suffix);
    if (path_length > SIZE_MAX - suffix_length - 1) {
        errno = ENAMETOOLONG;
        return false;
    }
    *temporary = malloc(path_length + suffix_length + 1);
    if (*temporary == NULL) {
        errno = ENOMEM;
        return false;
    }
    memcpy(*temporary, path, path_length);
    memcpy(*temporary + path_length, suffix, suffix_length + 1);
    *descriptor = mkstemp(*temporary);
    if (*descriptor >= 0) return true;
    free(*temporary);
    *temporary = NULL;
    return false;
}

static void sol_format_writes_cleanup(SolFormatWrite *writes, size_t count) {
    for (size_t index = 0; index < count; ++index) {
        if (writes[index].temporary != NULL && unlink(writes[index].temporary) != 0) {
            fprintf(
                stderr,
                "sol: warning: cannot remove formatter staging file '%s': %s\n",
                writes[index].temporary,
                strerror(errno)
            );
        }
        if (writes[index].backup != NULL && !writes[index].original_in_backup) {
            if (unlink(writes[index].backup) != 0) {
                fprintf(
                    stderr,
                    "sol: warning: cannot remove formatter backup reservation '%s': %s\n",
                    writes[index].backup,
                    strerror(errno)
                );
            }
        }
        free(writes[index].temporary);
        free(writes[index].backup);
    }
    free(writes);
}

static bool sol_fsync_parent(const char *path) {
    const char *slash = strrchr(path, '/');
    size_t length = slash == NULL ? 1 : (size_t)(slash - path);
    if (slash == path) length = 1;
    char *directory = malloc(length + 1);
    if (directory == NULL) {
        errno = ENOMEM;
        return false;
    }
    if (slash == NULL) {
        directory[0] = '.';
    } else {
        memcpy(directory, path, length);
    }
    directory[length] = '\0';
    int descriptor = open(directory, O_RDONLY);
    free(directory);
    if (descriptor < 0) return false;
    bool complete = fsync(descriptor) == 0;
    int error = errno;
    if (close(descriptor) != 0 && complete) {
        complete = false;
        error = errno;
    }
    errno = error;
    return complete;
}

static bool sol_rollback_format_writes(SolFormatWrite *writes, size_t count) {
    bool complete = true;
    while (count > 0) {
        SolFormatWrite *write = &writes[--count];
        if (!write->original_in_backup) continue;
        if (rename(write->backup, write->path) != 0) {
            fprintf(
                stderr,
                "sol: cannot restore '%s' after formatter write failure: %s\n",
                write->path,
                strerror(errno)
            );
            complete = false;
        } else {
            write->original_in_backup = false;
            free(write->backup);
            write->backup = NULL;
            if (!sol_fsync_parent(write->path)) {
                fprintf(
                    stderr,
                    "sol: cannot sync restored '%s' after formatter write failure: %s\n",
                    write->path,
                    strerror(errno)
                );
                complete = false;
            }
        }
    }
    return complete;
}

static bool sol_format_write_transaction(
    const SolPackage *package,
    const SolFormatted *formatted,
    const struct stat *loaded_statuses
) {
    static const char stage_suffix[] = ".solfmt-stage.XXXXXX";
    static const char backup_suffix[] = ".solfmt-backup.XXXXXX";
    size_t count = 0;
    for (size_t index = 0; index < package->file_count; ++index) {
        const SolSource *source = &package->files[index].source;
        if (source->length != formatted[index].length
            || memcmp(source->text, formatted[index].text, source->length) != 0) {
            ++count;
        }
    }
    if (count == 0) return true;

    SolFormatWrite *writes = calloc(count, sizeof(*writes));
    if (writes == NULL) {
        fprintf(stderr, "sol: out of memory while preparing formatter writes\n");
        return false;
    }

    size_t write_index = 0;
    bool complete = true;
    for (size_t index = 0; index < package->file_count && complete; ++index) {
        const SolSource *source = &package->files[index].source;
        if (source->length == formatted[index].length
            && memcmp(source->text, formatted[index].text, source->length) == 0) {
            continue;
        }
        SolFormatWrite *write = &writes[write_index++];
        write->path = package->files[index].path;
        write->source = source;
        write->loaded_status = loaded_statuses[index];

        int descriptor = -1;
        if (!sol_create_sibling(write->path, stage_suffix, &write->temporary, &descriptor)) {
            fprintf(stderr, "sol: cannot stage '%s': %s\n", write->path, strerror(errno));
            complete = false;
            break;
        }
        bool staged = sol_write_all(descriptor, formatted[index].text, formatted[index].length);
        // Formatter rewrites preserve mode bits only, not ownership or extended attributes.
        if (staged && fchmod(descriptor, write->loaded_status.st_mode & 07777) != 0) staged = false;
        if (staged && fsync(descriptor) != 0) staged = false;
        int error = errno;
        if (close(descriptor) != 0 && staged) {
            staged = false;
            error = errno;
        }
        errno = error;
        if (!staged) {
            fprintf(stderr, "sol: cannot stage '%s': %s\n", write->path, strerror(errno));
            complete = false;
            break;
        }

        int backup_descriptor = -1;
        if (!sol_create_sibling(
            write->path, backup_suffix, &write->backup, &backup_descriptor
        )) {
            fprintf(stderr, "sol: cannot reserve backup for '%s': %s\n", write->path, strerror(errno));
            complete = false;
            break;
        }
        if (close(backup_descriptor) != 0) {
            fprintf(stderr, "sol: cannot reserve backup for '%s': %s\n", write->path, strerror(errno));
            complete = false;
        }
    }
    if (!complete) {
        sol_format_writes_cleanup(writes, count);
        return false;
    }

    for (size_t index = 0; index < count; ++index) {
        bool changed;
        if (sol_format_write_unchanged(&writes[index], &changed)) continue;
        if (changed) {
            fprintf(stderr, "sol: concurrent modification of '%s'; formatter write aborted\n", writes[index].path);
        } else {
            fprintf(stderr, "sol: cannot verify '%s' before formatter write: %s\n", writes[index].path, strerror(errno));
        }
        sol_format_writes_cleanup(writes, count);
        return false;
    }

    size_t committed = 0;
    for (; committed < count; ++committed) {
        SolFormatWrite *write = &writes[committed];
        if (rename(write->path, write->backup) != 0) break;
        write->original_in_backup = true;
        if (rename(write->temporary, write->path) != 0) break;
        free(write->temporary);
        write->temporary = NULL;
    }
    if (committed != count) {
        int error = errno;
        sol_rollback_format_writes(writes, committed + 1);
        fprintf(stderr, "sol: cannot commit formatter write for '%s': %s\n", writes[committed].path, strerror(error));
        sol_format_writes_cleanup(writes, count);
        return false;
    }

    for (size_t index = 0; index < count; ++index) {
        if (!sol_fsync_parent(writes[index].path)) {
            int error = errno;
            sol_rollback_format_writes(writes, count);
            fprintf(stderr, "sol: cannot sync formatter write for '%s': %s\n", writes[index].path, strerror(error));
            sol_format_writes_cleanup(writes, count);
            return false;
        }
    }
    for (size_t index = 0; index < count; ++index) {
        if (unlink(writes[index].backup) != 0) {
            fprintf(
                stderr,
                "sol: warning: cannot remove formatter backup '%s' for '%s': %s\n",
                writes[index].backup,
                writes[index].path,
                strerror(errno)
            );
        } else {
            writes[index].original_in_backup = false;
            free(writes[index].backup);
            writes[index].backup = NULL;
        }
    }
    for (size_t index = 0; index < count; ++index) {
        if (!sol_fsync_parent(writes[index].path)) {
            fprintf(
                stderr,
                "sol: warning: cannot sync formatter cleanup for '%s': %s\n",
                writes[index].path,
                strerror(errno)
            );
        }
    }
    sol_format_writes_cleanup(writes, count);
    return true;
}

static int sol_fmt_path(const char *path, bool check, bool to_stdout) {
    struct stat operand_status;
    if (lstat(path, &operand_status) == 0 && S_ISLNK(operand_status.st_mode)) {
        fprintf(stderr, "sol: refusing to format symbolic link '%s'\n", path);
        return 1;
    }
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
    if (to_stdout && package.is_directory) {
        fputs("sol: fmt --stdout accepts only a regular .sol file\n", stderr);
        sol_diagnostics_free(&diagnostics);
        sol_package_free(&package);
        return 2;
    }
    if (diagnostics.count != 0) {
        sol_package_diagnostics_render_human(stderr, &package, &diagnostics);
        sol_diagnostics_free(&diagnostics);
        sol_package_free(&package);
        return 1;
    }

    struct stat *loaded_statuses = NULL;
    if (!check && !to_stdout) {
        loaded_statuses = calloc(package.file_count, sizeof(*loaded_statuses));
        if (loaded_statuses == NULL && package.file_count != 0) {
            fprintf(stderr, "sol: out of memory while preparing formatter writes\n");
            sol_diagnostics_free(&diagnostics);
            sol_package_free(&package);
            return 1;
        }
        for (size_t index = 0; index < package.file_count; ++index) {
            const char *file_path = package.files[index].path;
            if (lstat(file_path, &loaded_statuses[index]) != 0) {
                fprintf(stderr, "sol: cannot inspect '%s': %s\n", file_path, strerror(errno));
                free(loaded_statuses);
                sol_diagnostics_free(&diagnostics);
                sol_package_free(&package);
                return 1;
            }
            if (!S_ISREG(loaded_statuses[index].st_mode)) {
                fprintf(stderr, "sol: refusing to rewrite non-regular file '%s'\n", file_path);
                free(loaded_statuses);
                sol_diagnostics_free(&diagnostics);
                sol_package_free(&package);
                return 1;
            }
        }
    }

    SolFormatted *formatted = calloc(package.file_count, sizeof(*formatted));
    if (formatted == NULL) {
        fprintf(stderr, "sol: out of memory while formatting '%s'\n", path);
        free(loaded_statuses);
        sol_diagnostics_free(&diagnostics);
        sol_package_free(&package);
        return 1;
    }

    bool complete = true;
    for (size_t index = 0; index < package.file_count; ++index) {
        SolDiagnostics file_diagnostics;
        sol_formatted_init(&formatted[index]);
        sol_diagnostics_init(&file_diagnostics);
        if (!sol_format_source(
            &package.files[index].source, &formatted[index], &file_diagnostics
        )) {
            if (file_diagnostics.count != 0) {
                sol_diagnostics_render_human(
                    stderr, &package.files[index].source, &file_diagnostics
                );
            } else {
                fprintf(
                    stderr,
                    "sol: out of memory while formatting '%s'\n",
                    package.files[index].path
                );
            }
            complete = false;
        }
        sol_diagnostics_free(&file_diagnostics);
        if (!complete) break;
    }

    int result = complete ? 0 : 1;
    if (complete && to_stdout) {
        if (!sol_write_all(STDOUT_FILENO, formatted[0].text, formatted[0].length)) {
            fprintf(stderr, "sol: cannot write formatted source: %s\n", strerror(errno));
            result = 1;
        }
    } else if (complete && check) {
        for (size_t index = 0; index < package.file_count; ++index) {
            const SolSource *source = &package.files[index].source;
            if (source->length != formatted[index].length
                || memcmp(source->text, formatted[index].text, source->length) != 0) {
                fprintf(stderr, "%s: not formatted\n", package.files[index].path);
                result = 1;
            }
        }
    } else if (complete) {
        if (!sol_format_write_transaction(&package, formatted, loaded_statuses)) result = 1;
    }

    for (size_t index = 0; index < package.file_count; ++index) {
        sol_formatted_free(&formatted[index]);
    }
    free(formatted);
    free(loaded_statuses);
    sol_diagnostics_free(&diagnostics);
    sol_package_free(&package);
    return result;
}

static void sol_print_json_string(FILE *stream, const char *text);

static void sol_render_cli_error(
    FILE *stream, const char *kind, const char *message, const char *path
) {
    fputs("{\"schema\":\"sol.cli-error/1\",\"kind\":", stream);
    sol_print_json_string(stream, kind);
    fputs(",\"message\":", stream);
    sol_print_json_string(stream, message);
    fputs(",\"path\":", stream);
    sol_print_json_string(stream, path);
    fputs("}\n", stream);
}

static int sol_cli_infrastructure_failure(
    const char *path, bool json, const char *message
) {
    if (json) sol_render_cli_error(stdout, "infrastructure", message, path);
    else fprintf(stderr, "sol: %s\n", message);
    return 1;
}

static int sol_check_path(const char *path, bool json) {
    SolCompilationSession *session = sol_compilation_create();
    if (session == NULL) {
        if (json) sol_render_cli_error(stdout, "infrastructure",
            "compilation stopped because the compiler ran out of memory", path);
        else fputs("sol: compilation stopped because the compiler ran out of memory\n", stderr);
        return 1;
    }
    SolCompilationOutcome outcome = sol_compilation_compile_path(session, path);
    SolCompilationSummary summary;
    const char *error = sol_compilation_error(session);
    if (!sol_compilation_summary(session, &summary)) {
        sol_compilation_free(session);
        return sol_cli_infrastructure_failure(path, json,
            "compilation result is unavailable");
    }
    if (json && outcome == SOL_COMPILATION_LOAD_FAILED) {
        sol_render_cli_error(stdout, "load", error, path);
    } else if (json && outcome == SOL_COMPILATION_RESOURCE_FAILED
        && summary.diagnostic_count == 0) {
        sol_render_cli_error(stdout, "infrastructure",
            "compilation stopped because the compiler ran out of memory", path);
    } else if (json) {
        (void)sol_compilation_diagnostics_render_json(stdout, session);
    } else if (outcome == SOL_COMPILATION_LOAD_FAILED) {
        fprintf(stderr, "sol: %s\n", error);
    } else if (summary.diagnostic_count != 0) {
        (void)sol_compilation_diagnostics_render_human(stderr, session);
    } else if (outcome != SOL_COMPILATION_SUCCEEDED) {
        fputs("sol: compilation stopped because the compiler ran out of memory\n", stderr);
    } else if (summary.is_directory) {
        printf("checked %s: %zu file%s, %zu declaration%s\n", path,
            summary.file_count,
            summary.file_count == 1 ? "" : "s",
            summary.declaration_count,
            summary.declaration_count == 1 ? "" : "s");
    } else {
        printf("checked %s: %zu declaration%s\n", path,
            summary.declaration_count,
            summary.declaration_count == 1 ? "" : "s");
    }
    sol_compilation_free(session);
    return outcome == SOL_COMPILATION_SUCCEEDED ? 0 : 1;
}

static bool sol_json_continuation(unsigned char byte) {
    return byte >= 0x80 && byte <= 0xbf;
}

static size_t sol_json_utf8_length(const unsigned char *text) {
    unsigned char first = text[0];
    if (first < 0x80) return 1;
    unsigned char second = text[1];
    if (second == 0) return 0;
    if (first >= 0xc2 && first <= 0xdf) {
        return sol_json_continuation(second) ? 2 : 0;
    }
    unsigned char third = text[2];
    if (third == 0 || !sol_json_continuation(third)) return 0;
    if ((first == 0xe0 && second >= 0xa0 && second <= 0xbf)
        || ((first >= 0xe1 && first <= 0xec) && sol_json_continuation(second))
        || (first == 0xed && second >= 0x80 && second <= 0x9f)
        || ((first == 0xee || first == 0xef) && sol_json_continuation(second))) {
        return 3;
    }
    if (first < 0xf0 || first > 0xf4) return 0;
    unsigned char fourth = text[3];
    if (fourth == 0 || !sol_json_continuation(fourth)) return 0;
    if ((first == 0xf0 && second >= 0x90 && second <= 0xbf)
        || ((first >= 0xf1 && first <= 0xf3) && sol_json_continuation(second))
        || (first == 0xf4 && second >= 0x80 && second <= 0x8f)) return 4;
    return 0;
}

static void sol_print_json_string(FILE *stream, const char *text) {
    fputc('"', stream);
    const unsigned char *cursor = (const unsigned char *)text;
    while (*cursor != 0) {
        size_t utf8_length = sol_json_utf8_length(cursor);
        if (*cursor >= 0x80 && utf8_length != 0) {
            (void)fwrite(cursor, 1, utf8_length, stream);
            cursor += utf8_length;
            continue;
        }
        if (*cursor == '"') fputs("\\\"", stream);
        else if (*cursor == '\\') fputs("\\\\", stream);
        else if (*cursor == '\n') fputs("\\n", stream);
        else if (*cursor == '\r') fputs("\\r", stream);
        else if (*cursor == '\t') fputs("\\t", stream);
        else if (*cursor < 0x20 || *cursor >= 0x80) {
            fprintf(stream, "\\u%04x", (unsigned int)*cursor);
        }
        else fputc((int)*cursor, stream);
        ++cursor;
    }
    fputc('"', stream);
}

static int sol_test_path(const char *path, bool json) {
    SolCompilationSession *session = sol_compilation_create();
    if (session == NULL) {
        if (json) sol_render_cli_error(stdout, "infrastructure",
            "compilation stopped because the compiler ran out of memory", path);
        else fputs("sol: compilation stopped because the compiler ran out of memory\n", stderr);
        return 1;
    }
    SolCompilationOutcome outcome = sol_compilation_compile_path(session, path);
    SolCompilationSummary summary;
    if (!sol_compilation_summary(session, &summary)) {
        sol_compilation_free(session);
        return sol_cli_infrastructure_failure(path, json,
            "compilation result is unavailable");
    }
    if (outcome != SOL_COMPILATION_SUCCEEDED) {
        if (json && outcome == SOL_COMPILATION_LOAD_FAILED) {
            sol_render_cli_error(stdout, "load", sol_compilation_error(session), path);
        } else if (json && summary.diagnostic_count == 0) {
            sol_render_cli_error(stdout, "infrastructure",
                "compilation stopped because the compiler ran out of memory", path);
        } else if (json) {
            (void)sol_compilation_diagnostics_render_json(stdout, session);
        } else if (outcome == SOL_COMPILATION_LOAD_FAILED) {
            fprintf(stderr, "sol: %s\n", sol_compilation_error(session));
        } else if (summary.diagnostic_count != 0) {
            (void)sol_compilation_diagnostics_render_human(stderr, session);
        }
        else fputs("sol: compilation stopped because the compiler ran out of memory\n", stderr);
        sol_compilation_free(session);
        return 1;
    }
    if (!json && summary.diagnostic_count != 0) {
        (void)sol_compilation_diagnostics_render_human(stderr, session);
    }
    SolValidatedIr *validated = NULL;
    SolCompilationOutcome transfer = sol_compilation_take_ir(session, &validated);
    sol_compilation_free(session);
    if (transfer != SOL_COMPILATION_SUCCEEDED || validated == NULL) {
        sol_validated_ir_free(validated);
        return sol_cli_infrastructure_failure(path, json,
            "validated compilation result is unavailable");
    }

    size_t total = 0;
    size_t passed = 0;
    if (json) fputs("{\"schema\":\"sol.test-results\",\"version\":1,\"tests\":[", stdout);
    size_t definition_count = sol_validated_ir_definition_count(validated);
    for (size_t id = 0; id < definition_count; ++id) {
        SolValidatedDefinitionView test;
        if (!sol_validated_ir_definition_at(validated, id, &test)) {
            sol_validated_ir_free(validated);
            return sol_cli_infrastructure_failure(path, json,
                "validated definition is unavailable");
        }
        if (test.kind != SOL_VALIDATED_DEFINITION_TEST) continue;
        SolInterpreterRequest request = {
            .ir = NULL, .callable = test.callable, .definition = SOL_IR_NONE,
            .contracts = SOL_INTERPRETER_CONTRACTS_IGNORE, .test_entry = true,
        };
        SolInterpreterResult result;
        bool executed = sol_validated_ir_interpret(validated, &request, &result);
        bool truth = executed && result.value.kind == SOL_INTERPRETER_VALUE_BOOL
            && result.value.as.boolean;
        const char *status = truth ? "passed" : executed ? "false" : "runtime_error";
        const char *source_path = sol_validated_ir_path_at(validated, test.span);
        if (source_path == NULL || test.name == NULL) {
            sol_interpreter_result_free(&result);
            sol_validated_ir_free(validated);
            return sol_cli_infrastructure_failure(path, json,
                "validated definition metadata is unavailable");
        }
        if (json) {
            if (total != 0) fputc(',', stdout);
            fputs("{\"path\":", stdout); sol_print_json_string(stdout, source_path);
            fputs(",\"label\":", stdout); sol_print_json_string(stdout, test.name);
            fputs(",\"status\":", stdout); sol_print_json_string(stdout, status);
            if (executed) fputs(",\"diagnostic\":null}", stdout);
            else {
                fprintf(stdout, ",\"diagnostic\":{\"code\":%d,\"message\":",
                    (int)result.diagnostic.code);
                sol_print_json_string(stdout, result.diagnostic.message);
                fputs(",\"path\":", stdout);
                sol_print_json_string(stdout, result.diagnostic.file);
                fprintf(stdout, ",\"offset\":%zu}}", result.diagnostic.file_offset);
            }
        } else {
            fputs(truth ? "PASS " : "FAIL ", stdout);
            sol_print_json_string(stdout, test.name);
            printf(" (%s)", source_path);
            if (executed && !truth) fputs(": evaluated to false", stdout);
            else if (!executed) printf(": runtime at %s:%zu: %s",
                result.diagnostic.file, result.diagnostic.file_offset,
                result.diagnostic.message);
            fputc('\n', stdout);
        }
        ++total;
        if (truth) ++passed;
        sol_interpreter_result_free(&result);
    }
    size_t failed = total - passed;
    if (json) fprintf(stdout,
        "],\"summary\":{\"total\":%zu,\"passed\":%zu,\"failed\":%zu}}\n",
        total, passed, failed);
    else printf("%zu test%s, %zu passed, %zu failed\n",
        total, total == 1 ? "" : "s", passed, failed);
    sol_validated_ir_free(validated);
    return failed == 0 ? 0 : 1;
}

static int sol_effects_path(const char *path, bool json) {
    SolCompilationSession *session = sol_compilation_create();
    if (session == NULL) {
        if (json) sol_render_cli_error(stdout, "infrastructure",
            "compilation stopped because the compiler ran out of memory", path);
        else fputs("sol: compilation stopped because the compiler ran out of memory\n", stderr);
        return 1;
    }
    SolCompilationOutcome outcome = sol_compilation_compile_path(session, path);
    SolCompilationSummary summary;
    if (!sol_compilation_summary(session, &summary)) {
        sol_compilation_free(session);
        return sol_cli_infrastructure_failure(path, json,
            "compilation result is unavailable");
    }
    if (outcome != SOL_COMPILATION_SUCCEEDED) {
        if (json && outcome == SOL_COMPILATION_LOAD_FAILED) {
            sol_render_cli_error(stdout, "load", sol_compilation_error(session), path);
        } else if (json && summary.diagnostic_count == 0) {
            sol_render_cli_error(stdout, "infrastructure",
                "compilation stopped because the compiler ran out of memory", path);
        } else if (json) {
            (void)sol_compilation_diagnostics_render_json(stdout, session);
        } else if (outcome == SOL_COMPILATION_LOAD_FAILED) {
            fprintf(stderr, "sol: %s\n", sol_compilation_error(session));
        } else if (summary.diagnostic_count != 0) {
            (void)sol_compilation_diagnostics_render_human(stderr, session);
        } else {
            fputs("sol: compilation stopped because the compiler ran out of memory\n", stderr);
        }
        sol_compilation_free(session);
        return 1;
    }
    SolValidatedIr *validated = NULL;
    SolCompilationOutcome transfer = sol_compilation_take_ir(session, &validated);
    sol_compilation_free(session);
    if (transfer != SOL_COMPILATION_SUCCEEDED || validated == NULL) {
        sol_validated_ir_free(validated);
        return sol_cli_infrastructure_failure(path, json,
            "validated compilation result is unavailable");
    }
    bool rendered = sol_validated_ir_effects_render(stdout, validated, json);
    sol_validated_ir_free(validated);
    if (!rendered && ferror(stdout) == 0) {
        if (json) sol_render_cli_error(stdout, "infrastructure",
            "cannot construct effects report", path);
        else fputs("sol: cannot construct effects report\n", stderr);
    }
    return rendered ? 0 : 1;
}

static int sol_inspect_path(const char *path) {
    SolCompilationSession *session = sol_compilation_create();
    if (session == NULL) {
        sol_render_cli_error(stdout, "infrastructure",
            "compilation stopped because the compiler ran out of memory", path);
        return 1;
    }
    SolCompilationOutcome outcome = sol_compilation_compile_path(session, path);
    SolCompilationSummary summary;
    if (!sol_compilation_summary(session, &summary)) {
        sol_compilation_free(session);
        return sol_cli_infrastructure_failure(path, true,
            "compilation result is unavailable");
    }
    if (outcome == SOL_COMPILATION_LOAD_FAILED) {
        sol_render_cli_error(stdout, "load", sol_compilation_error(session), path);
    } else if (outcome == SOL_COMPILATION_RESOURCE_FAILED
        && summary.diagnostic_count == 0) {
        sol_render_cli_error(stdout, "infrastructure",
            "compilation stopped because the compiler ran out of memory", path);
    } else if (outcome != SOL_COMPILATION_SUCCEEDED) {
        (void)sol_compilation_diagnostics_render_json(stdout, session);
    } else if (!sol_compilation_inspection_render(stdout, session)) {
        if (ferror(stdout) == 0) sol_render_cli_error(stdout, "infrastructure",
            "cannot construct inspection projection", path);
        outcome = SOL_COMPILATION_RESOURCE_FAILED;
    }
    sol_compilation_free(session);
    return outcome == SOL_COMPILATION_SUCCEEDED ? 0 : 1;
}

static int sol_main(int argc, char **argv) {
    signal(SIGPIPE, SIG_IGN);
    if (argc == 2 && strcmp(argv[1], "--version") == 0) {
        puts("sol 0.1.0-dev");
        return 0;
    }
    if (argc < 3) {
        sol_print_usage(stderr);
        return 2;
    }

    if (strcmp(argv[1], "fmt") == 0) {
        bool check = false;
        bool to_stdout = false;
        const char *path = NULL;
        for (int index = 2; index < argc; ++index) {
            if (strcmp(argv[index], "--check") == 0) {
                check = true;
            } else if (strcmp(argv[index], "--stdout") == 0) {
                to_stdout = true;
            } else if (argv[index][0] == '-') {
                fprintf(stderr, "sol: unknown option '%s'\n", argv[index]);
                return 2;
            } else if (path != NULL) {
                fputs("sol: fmt accepts one source file or package directory\n", stderr);
                return 2;
            } else {
                path = argv[index];
            }
        }
        if (check && to_stdout) {
            fputs("sol: fmt --check and --stdout are mutually exclusive\n", stderr);
            return 2;
        }
        if (path == NULL) {
            sol_print_usage(stderr);
            return 2;
        }
        return sol_fmt_path(path, check, to_stdout);
    }

    if (strcmp(argv[1], "inspect") == 0) {
        if (argc != 3 || argv[2][0] == '-') {
            fputs("sol: inspect accepts exactly one source file or package directory\n", stderr);
            return 2;
        }
        return sol_inspect_path(argv[2]);
    }

    bool testing = strcmp(argv[1], "test") == 0;
    bool inspecting_effects = strcmp(argv[1], "effects") == 0;
    if (!testing && !inspecting_effects && strcmp(argv[1], "check") != 0) {
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
            fprintf(stderr, "sol: %s accepts one source file or package directory\n",
                testing ? "test" : inspecting_effects ? "effects" : "check");
            return 2;
        } else {
            path = argv[index];
        }
    }
    if (path == NULL) {
        sol_print_usage(stderr);
        return 2;
    }
    if (testing) return sol_test_path(path, json);
    if (inspecting_effects) return sol_effects_path(path, json);
    return sol_check_path(path, json);
}

int main(int argc, char **argv) {
    int result = sol_main(argc, argv);
    bool output_failed = fflush(stdout) == EOF || ferror(stdout) != 0;
    return output_failed && result == 0 ? 1 : result;
}
