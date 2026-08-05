#include "sol/source.h"

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

bool sol_source_load(SolSource *source, const char *path, char *error, size_t error_size) {
    memset(source, 0, sizeof(*source));
    source->path = path;

    FILE *file = fopen(path, "rb");
    if (file == NULL) {
        snprintf(error, error_size, "cannot open '%s': %s", path, strerror(errno));
        return false;
    }

    if (fseek(file, 0, SEEK_END) != 0) {
        snprintf(error, error_size, "cannot seek '%s'", path);
        fclose(file);
        return false;
    }

    long measured = ftell(file);
    if (measured < 0 || fseek(file, 0, SEEK_SET) != 0) {
        snprintf(error, error_size, "cannot measure '%s'", path);
        fclose(file);
        return false;
    }

    source->length = (size_t)measured;
    source->text = malloc(source->length + 1);
    if (source->text == NULL) {
        snprintf(error, error_size, "out of memory while reading '%s'", path);
        fclose(file);
        return false;
    }

    size_t read_count = fread(source->text, 1, source->length, file);
    if (read_count != source->length || ferror(file)) {
        snprintf(error, error_size, "cannot read '%s'", path);
        fclose(file);
        sol_source_free(source);
        return false;
    }
    fclose(file);
    source->text[source->length] = '\0';

    if (!sol_source_index_lines(source)) {
        snprintf(error, error_size, "out of memory while indexing '%s'", path);
        sol_source_free(source);
        return false;
    }
    return true;
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
