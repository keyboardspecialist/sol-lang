#define _POSIX_C_SOURCE 200809L

#include "sol/source.h"

#include "source_internal.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include "resource_internal.h"
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static char *sol_copy_string(const char *text) {
    size_t length = strlen(text);
    char *copy = malloc(length + 1);
    if (copy != NULL) {
        memcpy(copy, text, length + 1);
    }
    return copy;
}

static bool sol_source_index_lines(SolSource *source) {
    size_t count = 1;
    for (size_t index = 0; index < source->length; ++index) {
        if (source->text[index] == '\r') {
            if (index + 1 < source->length && source->text[index + 1] == '\n') {
                ++index;
            }
            ++count;
        } else if (source->text[index] == '\n') {
            ++count;
        }
    }

    if (count > SIZE_MAX / sizeof(*source->line_starts)) {
        return false;
    }
    size_t *starts = malloc(count * sizeof(*starts));
    if (starts == NULL) {
        return false;
    }

    starts[0] = 0;
    size_t line = 1;
    for (size_t index = 0; index < source->length; ++index) {
        if (source->text[index] == '\r') {
            if (index + 1 < source->length && source->text[index + 1] == '\n') {
                ++index;
            }
            starts[line++] = index + 1;
        } else if (source->text[index] == '\n') {
            starts[line++] = index + 1;
        }
    }

    source->line_starts = starts;
    source->line_count = count;
    return true;
}

bool sol_source_from_text(SolSource *source, const char *path, const char *text) {
    memset(source, 0, sizeof(*source));
    source->path = path;
    source->length = strlen(text);
    source->text = sol_copy_string(text);
    if (source->text == NULL || !sol_source_index_lines(source)) {
        sol_source_free(source);
        return false;
    }
    return true;
}

static bool sol_source_same_file(
    const struct stat *left, const struct stat *right
) {
    if (left->st_dev != right->st_dev || left->st_ino != right->st_ino
        || left->st_size != right->st_size) return false;
#if defined(__APPLE__)
    return left->st_mtime == right->st_mtime
        && left->st_mtimensec == right->st_mtimensec
        && left->st_ctime == right->st_ctime
        && left->st_ctimensec == right->st_ctimensec;
#else
    return left->st_mtim.tv_sec == right->st_mtim.tv_sec
        && left->st_mtim.tv_nsec == right->st_mtim.tv_nsec
        && left->st_ctim.tv_sec == right->st_ctim.tv_sec
        && left->st_ctim.tv_nsec == right->st_ctim.tv_nsec;
#endif
}

SolSourceLoadOutcome sol_source_load_expected(
    SolSource *source, const char *path, const SolSourceIdentity *expected,
    char *error, size_t error_size
) {
    memset(source, 0, sizeof(*source));
    source->path = path;

    int flags = O_RDONLY;
#ifdef O_CLOEXEC
    flags |= O_CLOEXEC;
#endif
#ifdef O_NOFOLLOW
    flags |= O_NOFOLLOW;
#endif
    int descriptor = open(path, flags);
    if (descriptor < 0) {
        snprintf(error, error_size, "cannot open '%s': %s", path, strerror(errno));
        return SOL_SOURCE_LOAD_IO_FAILED;
    }
    struct stat before;
    if (fstat(descriptor, &before) != 0 || !S_ISREG(before.st_mode)) {
        snprintf(error, error_size, "opened source is not a regular file: '%s'", path);
        close(descriptor);
        return SOL_SOURCE_LOAD_IO_FAILED;
    }
    if (expected != NULL && (expected->device != (uintmax_t)before.st_dev
        || expected->inode != (uintmax_t)before.st_ino)) {
        snprintf(error, error_size, "source changed after discovery: '%s'", path);
        close(descriptor);
        return SOL_SOURCE_LOAD_IO_FAILED;
    }
    if (before.st_size < 0 || (uintmax_t)before.st_size >= SIZE_MAX) {
        snprintf(error, error_size, "source is too large: '%s'", path);
        close(descriptor);
        return SOL_SOURCE_LOAD_RESOURCE_FAILED;
    }
    source->length = (size_t)before.st_size;
    if (!sol_resource_charge_source(source->length)) {
        snprintf(error, error_size, "source resource limit exceeded: '%s'", path);
        close(descriptor);
        return SOL_SOURCE_LOAD_RESOURCE_FAILED;
    }
    source->text = malloc(source->length + 1);
    if (source->text == NULL) {
        snprintf(error, error_size, "out of memory while reading '%s'", path);
        close(descriptor);
        return SOL_SOURCE_LOAD_RESOURCE_FAILED;
    }
    size_t cursor = 0;
    while (cursor < source->length) {
        ssize_t count = read(descriptor, source->text + cursor, source->length - cursor);
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) break;
        cursor += (size_t)count;
    }
    char extra;
    ssize_t extra_count;
    do {
        extra_count = read(descriptor, &extra, 1);
    } while (extra_count < 0 && errno == EINTR);
    struct stat after;
    bool stable = cursor == source->length && extra_count == 0
        && fstat(descriptor, &after) == 0 && sol_source_same_file(&before, &after);
    int close_result = close(descriptor);
    if (!stable || close_result != 0) {
        snprintf(error, error_size, "cannot read '%s'", path);
        sol_source_free(source);
        return SOL_SOURCE_LOAD_IO_FAILED;
    }
    source->text[source->length] = '\0';

    if (!sol_source_index_lines(source)) {
        snprintf(error, error_size, "out of memory while indexing '%s'", path);
        sol_source_free(source);
        return SOL_SOURCE_LOAD_RESOURCE_FAILED;
    }
    return SOL_SOURCE_LOAD_SUCCEEDED;
}

SolSourceLoadOutcome sol_source_load_outcome(
    SolSource *source, const char *path, char *error, size_t error_size
) {
    return sol_source_load_expected(source, path, NULL, error, error_size);
}

bool sol_source_load(SolSource *source, const char *path, char *error, size_t error_size) {
    return sol_source_load_outcome(source, path, error, error_size)
        == SOL_SOURCE_LOAD_SUCCEEDED;
}

void sol_source_free(SolSource *source) {
    free(source->text);
    free(source->line_starts);
    memset(source, 0, sizeof(*source));
}

SolPosition sol_source_position(const SolSource *source, size_t offset) {
    if (offset > source->length) {
        offset = source->length;
    }

    size_t low = 0;
    size_t high = source->line_count;
    while (low + 1 < high) {
        size_t middle = low + (high - low) / 2;
        if (source->line_starts[middle] <= offset) {
            low = middle;
        } else {
            high = middle;
        }
    }

    SolPosition position = {
        .line = low + 1,
        .column = offset - source->line_starts[low] + 1,
    };
    return position;
}
