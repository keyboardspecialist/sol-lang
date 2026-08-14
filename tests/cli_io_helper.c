#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

static bool wait_for(pid_t child, int *exit_code) {
    int status;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR) return false;
    }
    if (!WIFEXITED(status)) return false;
    *exit_code = WEXITSTATUS(status);
    return true;
}

static bool run_closed(const char *sol, char *const arguments[]) {
    int descriptors[2];
    int gate[2];
    if (pipe(descriptors) != 0) return false;
    if (pipe(gate) != 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        close(gate[0]);
        close(gate[1]);
        return false;
    }
    if (child == 0) {
        close(descriptors[0]);
        close(gate[1]);
        char ready;
        while (read(gate[0], &ready, 1) < 0 && errno == EINTR) {}
        close(gate[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(125);
        close(descriptors[1]);
        execv(sol, arguments);
        _exit(126);
    }
    close(descriptors[0]);
    close(descriptors[1]);
    close(gate[0]);
    char ready = 1;
    (void)write(gate[1], &ready, 1);
    close(gate[1]);
    int exit_code;
    return wait_for(child, &exit_code) && exit_code != 0;
}

static bool run_capture(
    const char *sol, char *const arguments[], char *output, size_t capacity
) {
    int descriptors[2];
    if (capacity == 0 || pipe(descriptors) != 0) return false;
    pid_t child = fork();
    if (child < 0) {
        close(descriptors[0]);
        close(descriptors[1]);
        return false;
    }
    if (child == 0) {
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0) _exit(125);
        close(descriptors[1]);
        execv(sol, arguments);
        _exit(126);
    }
    close(descriptors[1]);
    size_t count = 0;
    for (;;) {
        ssize_t received = read(descriptors[0], output + count, capacity - count - 1);
        if (received < 0 && errno == EINTR) continue;
        if (received <= 0) break;
        count += (size_t)received;
        if (count + 1 == capacity) break;
    }
    close(descriptors[0]);
    output[count] = '\0';
    int exit_code;
    return wait_for(child, &exit_code) && exit_code == 0;
}

static bool valid_utf8(const unsigned char *text) {
    while (*text != 0) {
        if (*text < 0x80) {
            ++text;
        } else if (*text >= 0xc2 && *text <= 0xdf
            && text[1] >= 0x80 && text[1] <= 0xbf) {
            text += 2;
        } else if (*text == 0xe0 && text[1] >= 0xa0 && text[1] <= 0xbf
            && text[2] >= 0x80 && text[2] <= 0xbf) {
            text += 3;
        } else if (((*text >= 0xe1 && *text <= 0xec)
                || (*text >= 0xee && *text <= 0xef))
            && text[1] >= 0x80 && text[1] <= 0xbf
            && text[2] >= 0x80 && text[2] <= 0xbf) {
            text += 3;
        } else if (*text == 0xed && text[1] >= 0x80 && text[1] <= 0x9f
            && text[2] >= 0x80 && text[2] <= 0xbf) {
            text += 3;
        } else if (*text == 0xf0 && text[1] >= 0x90 && text[1] <= 0xbf
            && text[2] >= 0x80 && text[2] <= 0xbf
            && text[3] >= 0x80 && text[3] <= 0xbf) {
            text += 4;
        } else if (*text >= 0xf1 && *text <= 0xf3
            && text[1] >= 0x80 && text[1] <= 0xbf
            && text[2] >= 0x80 && text[2] <= 0xbf
            && text[3] >= 0x80 && text[3] <= 0xbf) {
            text += 4;
        } else if (*text == 0xf4 && text[1] >= 0x80 && text[1] <= 0x8f
            && text[2] >= 0x80 && text[2] <= 0xbf
            && text[3] >= 0x80 && text[3] <= 0xbf) {
            text += 4;
        } else {
            return false;
        }
    }
    return true;
}

static int hex_value(unsigned char byte) {
    if (byte >= '0' && byte <= '9') return (int)(byte - '0');
    if (byte >= 'a' && byte <= 'f') return (int)(byte - 'a') + 10;
    if (byte >= 'A' && byte <= 'F') return (int)(byte - 'A') + 10;
    return -1;
}

static bool decode_label(const char *json, char *label, size_t capacity) {
    const char *cursor = strstr(json, "\"label\":\"");
    if (cursor == NULL || capacity == 0) return false;
    cursor += strlen("\"label\":\"");
    size_t count = 0;
    while (*cursor != '\0' && *cursor != '"') {
        unsigned char byte = (unsigned char)*cursor++;
        if (byte != '\\') {
            if (count + 1 >= capacity) return false;
            label[count++] = (char)byte;
            continue;
        }
        byte = (unsigned char)*cursor++;
        if (byte == 'u') {
            unsigned int scalar = 0;
            for (size_t digit = 0; digit < 4; ++digit) {
                int value = hex_value((unsigned char)*cursor++);
                if (value < 0) return false;
                scalar = scalar * 16 + (unsigned int)value;
            }
            if (scalar < 0x80) {
                if (count + 1 >= capacity) return false;
                label[count++] = (char)scalar;
            } else {
                if (count + 2 >= capacity) return false;
                label[count++] = (char)(0xc0 | (scalar >> 6));
                label[count++] = (char)(0x80 | (scalar & 0x3f));
            }
        } else {
            if (count + 1 >= capacity) return false;
            if (byte == 'n') label[count++] = '\n';
            else if (byte == 'r') label[count++] = '\r';
            else if (byte == 't') label[count++] = '\t';
            else if (byte == 'b') label[count++] = '\b';
            else if (byte == 'f') label[count++] = '\f';
            else label[count++] = (char)byte;
        }
    }
    if (*cursor != '"') return false;
    label[count] = '\0';
    return true;
}

static bool write_invalid_source(char *path, size_t capacity) {
    char directory[] = "/tmp/sol-cli-io-XXXXXX";
    int reservation = mkstemp(directory);
    if (reservation < 0) return false;
    if (close(reservation) != 0 || unlink(directory) != 0
        || mkdir(directory, 0700) != 0) return false;
    int length = snprintf(path, capacity, "%s/invalid.sol", directory);
    if (length < 0 || (size_t)length >= capacity) return false;
    int descriptor = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (descriptor < 0) return false;
    static const unsigned char source[] = {
        'm','o','d','u','l','e',' ','i','n','v','a','l','i','d','\n',
        't','e','s','t',' ','"','b','a','d',0xff,'"',' ','t','r','u','e','\n'
    };
    size_t offset = 0;
    while (offset < sizeof(source)) {
        ssize_t written = write(descriptor, source + offset, sizeof(source) - offset);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            close(descriptor);
            return false;
        }
        offset += (size_t)written;
    }
    return close(descriptor) == 0;
}

int main(int argc, char **argv) {
    if (argc != 4) return 2;
    const char *sol = argv[1];
    const char *valid_source = argv[2];
    const char *unicode_source = argv[3];
    char *check_human[] = {(char *)sol, "check", (char *)valid_source, NULL};
    char *check_json[] = {(char *)sol, "check", "--diagnostic-format=json",
        (char *)valid_source, NULL};
    char *test_human[] = {(char *)sol, "test", (char *)valid_source, NULL};
    char *test_json[] = {(char *)sol, "test", "--diagnostic-format=json",
        (char *)valid_source, NULL};
    char *effects_human[] = {(char *)sol, "effects", (char *)valid_source, NULL};
    char *effects_json[] = {(char *)sol, "effects", "--diagnostic-format=json",
        (char *)valid_source, NULL};
    char *inspect[] = {(char *)sol, "inspect", (char *)valid_source, NULL};
    if (!run_closed(sol, check_human) || !run_closed(sol, check_json)
        || !run_closed(sol, test_human) || !run_closed(sol, test_json)
        || !run_closed(sol, effects_human) || !run_closed(sol, effects_json)
        || !run_closed(sol, inspect)) return 1;

    char output[8192];
    char *unicode_json[] = {(char *)sol, "test", "--diagnostic-format=json",
        (char *)unicode_source, NULL};
    char decoded[128];
    if (!run_capture(sol, unicode_json, output, sizeof(output))
        || !valid_utf8((const unsigned char *)output)
        || !decode_label(output, decoded, sizeof(decoded))
        || strcmp(decoded, "café") != 0) return 1;
    char *unicode_human[] = {(char *)sol, "test", (char *)unicode_source, NULL};
    if (!run_capture(sol, unicode_human, output, sizeof(output))
        || strstr(output, "café") == NULL) return 1;

    char invalid_path[512];
    if (!write_invalid_source(invalid_path, sizeof(invalid_path))) return 1;
    char *invalid_json[] = {(char *)sol, "test", "--diagnostic-format=json",
        invalid_path, NULL};
    bool captured = run_capture(sol, invalid_json, output, sizeof(output));
    bool safe_json = captured && valid_utf8((const unsigned char *)output)
        && strstr(output, "\"label\":\"bad\\u00ff\"") != NULL
        && memchr(output, 0xff, strlen(output)) == NULL;
    char *invalid_human[] = {(char *)sol, "test", invalid_path, NULL};
    bool safe_human = run_capture(sol, invalid_human, output, sizeof(output))
        && valid_utf8((const unsigned char *)output)
        && strstr(output, "bad\\u00ff") != NULL
        && memchr(output, 0xff, strlen(output)) == NULL;
    unlink(invalid_path);
    char *slash = strrchr(invalid_path, '/');
    if (slash != NULL) {
        *slash = '\0';
        rmdir(invalid_path);
    }
    return safe_json && safe_human ? 0 : 1;
}
