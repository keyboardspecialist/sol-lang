#include "test_progress.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

static const char *test_name = "unnamed";
static const char *case_name = "startup";
static const char *phase_name = "startup";
static const char *unit_name = "operation";
static size_t completed_units;
static size_t total_units;
static double last_display;
static char fixture_name[512];
static const char *ordinal_kind = "none";
static size_t ordinal_value, ordinal_total;
static size_t hook_after;
static const char *hook_kind;
static struct { const char *phase, *unit, *group; size_t completed, total; } parents[32];
static size_t parent_count;
static size_t overflow_count;

static double display_clock(void) {
    struct timespec now;
    if (timespec_get(&now, TIME_UTC) != TIME_UTC) return 0.0;
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
}

static void field(const char *key, const char *value) {
    fprintf(stderr, "\t%s=", key);
    /* Protocol tokens cannot inject records, tabs or terminal control codes. */
    for (const unsigned char *p = (const unsigned char *)value; *p; ++p) {
        unsigned char c = *p;
        fputc((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
            || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.'
            || c == '/' || c == ':' ? (int)c : '_', stderr);
    }
}

static void emit(const char *event) {
    fputs("SOL_TEST_PROGRESS", stderr);
    field("event", event);
    field("test", test_name);
    field("case", case_name);
    field("phase", phase_name);
    field("unit", unit_name);
    if (parent_count != 0) field("outer_phase", parents[parent_count - 1].phase);
    if (fixture_name[0] != '\0') field("fixture", fixture_name);
    if (ordinal_value != 0) {
        field("ordinal_kind", ordinal_kind);
        fprintf(stderr, "\tordinal=%zu", ordinal_value);
        if (ordinal_total != 0) fprintf(stderr, "\tordinal_total=%zu", ordinal_total);
    }
    if (hook_kind != NULL) {
        field("hook", hook_kind);
        fprintf(stderr, "\thook_after=%zu", hook_after);
    }
    fprintf(stderr, "\tcompleted=%zu", completed_units);
    if (total_units != 0) fprintf(stderr, "\ttotal=%zu", total_units);
    fputc('\n', stderr);
    fflush(stderr);
    last_display = display_clock();
}

void sol_test_progress_begin(const char *name) {
    test_name = name;
    case_name = "startup";
    phase_name = "startup";
    unit_name = "operation";
    completed_units = total_units = 0;
    fixture_name[0] = '\0';
    ordinal_value = ordinal_total = parent_count = 0;
    hook_kind = NULL;
    overflow_count = 0;
    emit("begin");
}

void sol_test_progress_case(const char *name) {
    case_name = name;
    fixture_name[0] = '\0';
    ordinal_value = ordinal_total = 0;
    hook_kind = NULL;
    sol_test_progress_phase("case", "case", 1);
}

void sol_test_progress_case_end(void) {
    /* Inner phase counts belong to their own domains, never to the case count. */
    phase_name = "case";
    unit_name = "case";
    completed_units = total_units = 1;
    emit("case_done");
    case_name = "owner-setup-and-invariants";
    phase_name = "between-cases";
    completed_units = total_units = 0;
}

void sol_test_progress_phase(const char *name, const char *unit, size_t total) {
    phase_name = name;
    unit_name = unit;
    completed_units = 0;
    total_units = total;
    emit("phase");
}

void sol_test_progress_completed(size_t completed) {
    completed_units = completed;
    double now = display_clock();
    /* Wall time is only a display throttle. Jumps never suppress indefinitely. */
    if (now <= last_display || now - last_display >= 1.0) emit("units");
}

void sol_test_progress_phase_end(void) {
    emit("phase_done");
}

void sol_test_progress_operation_begin(const char *name) {
    sol_test_progress_operation_at(case_name, name);
}

void sol_test_progress_operation_at(const char *group, const char *name) {
    if (parent_count == sizeof parents / sizeof parents[0]) {
        ++overflow_count;
        return;
    }
    if (parent_count < sizeof parents / sizeof parents[0]) {
        parents[parent_count].group = case_name;
        parents[parent_count].phase = phase_name;
        parents[parent_count].unit = unit_name;
        parents[parent_count].completed = completed_units;
        parents[parent_count++].total = total_units;
    }
    case_name = strcmp(group, "sol_test_body") == 0 ? "owner-setup-and-invariants" : group;
    sol_test_progress_phase(name, "operation", 1);
}

void sol_test_progress_operation_end(void) {
    if (overflow_count != 0) { --overflow_count; return; }
    sol_test_progress_completed(1);
    sol_test_progress_phase_end();
    if (parent_count != 0) {
        --parent_count;
        case_name = parents[parent_count].group;
        phase_name = parents[parent_count].phase;
        unit_name = parents[parent_count].unit;
        completed_units = parents[parent_count].completed;
        total_units = parents[parent_count].total;
    }
}

void sol_test_progress_fixture(const char *name) {
    (void)snprintf(fixture_name, sizeof fixture_name, "%s", name);
    sol_test_progress_phase("fixture", "fixture", 0);
}

void sol_test_progress_ordinal(const char *kind, size_t ordinal, size_t total) {
    ordinal_kind = ordinal == 0 ? "retry-or-control" : kind;
    if (ordinal != 0) ordinal_value = ordinal;
    ordinal_total = total;
    emit("ordinal");
}

void sol_test_progress_hook(const char *kind, size_t after) {
    hook_kind = kind;
    hook_after = after;
    emit("hook");
}

int sol_test_progress_finish(int status) {
    emit(status == 0 ? "done_pass" : "done_fail");
    return status;
}
