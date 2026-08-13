#include "sol/diagnostic.h"
#include "sol/formatter.h"
#include "sol/source.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(condition) \
    do { \
        if (!(condition)) { \
            fprintf(stderr, "%s:%d: check failed: %s\n", __FILE__, __LINE__, #condition); \
            ++failures; \
        } \
    } while (0)

static bool format_text(
    const char *text,
    SolFormatted *formatted,
    SolDiagnostics *diagnostics
) {
    SolSource source;
    if (!sol_source_from_text(&source, "formatter.sol", text)) return false;
    bool result = sol_format_source(&source, formatted, diagnostics);
    sol_source_free(&source);
    return result;
}

static void check_output(const SolFormatted *formatted, const char *expected) {
    size_t expected_length = strlen(expected);
    bool equal = formatted->length == expected_length
        && memcmp(formatted->text, expected, expected_length) == 0;
    CHECK(equal);
    if (!equal) {
        fprintf(
            stderr,
            "expected (%zu bytes):\n---\n%s---\nactual (%zu bytes):\n---\n%s---\n",
            expected_length,
            expected,
            formatted->length,
            formatted->text
        );
    }
}

static void check_second_pass(const SolFormatted *first) {
    SolFormatted second;
    SolDiagnostics diagnostics;
    sol_formatted_init(&second);
    sol_diagnostics_init(&diagnostics);

    CHECK(format_text(first->text, &second, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    CHECK(second.length == first->length);
    if (second.length == first->length) {
        CHECK(memcmp(second.text, first->text, first->length) == 0);
    }

    sol_diagnostics_free(&diagnostics);
    sol_formatted_free(&second);
}

static void test_golden_formatting(void) {
    static const char source_text[] =
        "\tmodule\t demo.main  \r\n"
        "edition\t2027\t \r\n"
        "\r\n\r\n"
        "use\tcore.Text \r\n"
        "\r\n\r\n\r\n"
        "record\tBox<T>\t{\r\n"
        "\tvalue\t:\tT\t,   \r\n"
        "}\t\r\n"
        "\r\n"
        "function\tcompute<T>(value:T,limit:T,ready:Bool)->T effects { pure } {\r\n"
        "\tlet\tcombined=value+-value*2\t  \r\n"
        "\tlet\tchosen=identity<T>(combined)\r\n"
        "\treturn\tif\t!ready||chosen < limit\t{\r\n"
        "\t\tidentity<T>(chosen)\r\n"
        "\t}\telse\t{\r\n"
        "\t\t-chosen\r\n"
        "\t}\r\n"
        "}\t  ";
    static const char expected[] =
        "module demo.main\n"
        "edition 2027\n"
        "\n"
        "use core.Text\n"
        "\n"
        "record Box<T> {\n"
        "    value: T,\n"
        "}\n"
        "\n"
        "function compute<T>(value: T, limit: T, ready: Bool) -> T effects { pure } {\n"
        "    let combined = value + -value * 2\n"
        "    let chosen = identity<T>(combined)\n"
        "    return if !ready || chosen < limit {\n"
        "        identity<T>(chosen)\n"
        "    } else {\n"
        "        -chosen\n"
        "    }\n"
        "}\n";
    SolFormatted formatted;
    SolDiagnostics diagnostics;
    sol_formatted_init(&formatted);
    sol_diagnostics_init(&diagnostics);

    CHECK(format_text(source_text, &formatted, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    check_output(&formatted, expected);
    check_second_pass(&formatted);

    sol_diagnostics_free(&diagnostics);
    sol_formatted_free(&formatted);
}

static void test_comments_and_preserved_bytes(void) {
    static const char source_text[] =
        "module bytes\n"
        "// Standalone LINE:  two spaces  \n"
        "/* Standalone BLOCK:\tA  B */\n"
        "function text()->Text {\n"
        "\t// nested line:\tkeep  bytes\n"
        "\treturn/* inline BLOCK:\tMiX  */\"a  b\\tC\"// inline LINE:\tTail  \n"
        "}\n";
    static const char expected[] =
        "module bytes\n"
        "// Standalone LINE:  two spaces  \n"
        "/* Standalone BLOCK:\tA  B */\n"
        "function text() -> Text {\n"
        "    // nested line:\tkeep  bytes\n"
        "    return /* inline BLOCK:\tMiX  */ \"a  b\\tC\" // inline LINE:\tTail  \n"
        "}\n";
    SolFormatted formatted;
    SolDiagnostics diagnostics;
    sol_formatted_init(&formatted);
    sol_diagnostics_init(&diagnostics);

    CHECK(format_text(source_text, &formatted, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    check_output(&formatted, expected);
    CHECK(strstr(formatted.text, "// Standalone LINE:  two spaces  ") != NULL);
    CHECK(strstr(formatted.text, "/* Standalone BLOCK:\tA  B */") != NULL);
    CHECK(strstr(formatted.text, "/* inline BLOCK:\tMiX  */") != NULL);
    CHECK(strstr(formatted.text, "\"a  b\\tC\"") != NULL);
    CHECK(strstr(formatted.text, "// inline LINE:\tTail  ") != NULL);
    check_second_pass(&formatted);

    sol_diagnostics_free(&diagnostics);
    sol_formatted_free(&formatted);
}

static void test_final_line_comment_preserves_trailing_spaces(void) {
    static const char source_text[] = "module eof_comment\n// exact trailing bytes   ";
    static const char expected[] = "module eof_comment\n// exact trailing bytes   \n";
    SolFormatted formatted;
    SolDiagnostics diagnostics;
    sol_formatted_init(&formatted);
    sol_diagnostics_init(&diagnostics);

    CHECK(format_text(source_text, &formatted, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    check_output(&formatted, expected);
    check_second_pass(&formatted);

    sol_diagnostics_free(&diagnostics);
    sol_formatted_free(&formatted);
}

static void test_angle_canonicality(void) {
    static const char compact[] =
        "module angles\n"
        "record Pair<T,U> { value: Outer<Pair<T,U>>, }\n"
        "function identity<T>(value:T)->T effects { pure } { return value }\n"
        "capability Clock { function read()->Int64 effects { clock.read<Self> } }\n"
        "capability Provider { function read()->Int64 effects { pure } }\n"
        "function sample<T,effects E>(left:Int64,right:Int64,clock:capability Clock,provider:capability Provider,pair:Pair/* type */<T,Pair<T,Int64>/* nested */>)->Bool effects { clock.read<clock> } {\n"
        "let value=identity<Pair<T,Int64>>(pair)\n"
        "let compared=left/* lhs */</* rhs */right&&right>left\n"
        "return handle clock.read<clock> with provider { compared }\n"
        "}\n";
    static const char spaced[] =
        "module angles\n"
        "record Pair < T , U > { value : Outer < Pair < T , U > > , }\n"
        "function identity < T > ( value : T ) -> T effects { pure } { return value }\n"
        "capability Clock { function read ( ) -> Int64 effects { clock.read < Self > } }\n"
        "capability Provider { function read ( ) -> Int64 effects { pure } }\n"
        "function sample < T , effects E > ( left : Int64 , right : Int64 , clock : capability Clock , provider : capability Provider , pair : Pair /* type */ < T , Pair < T , Int64 > /* nested */ > ) -> Bool effects { clock.read < clock > } {\n"
        "let value = identity < Pair < T , Int64 > > ( pair )\n"
        "let compared = left /* lhs */ < /* rhs */ right && right > left\n"
        "return handle clock.read < clock > with provider { compared }\n"
        "}\n";
    static const char expected[] =
        "module angles\n"
        "record Pair<T, U> { value: Outer<Pair<T, U>>, }\n"
        "function identity<T>(value: T) -> T effects { pure } { return value }\n"
        "capability Clock { function read() -> Int64 effects { clock.read<Self> } }\n"
        "capability Provider { function read() -> Int64 effects { pure } }\n"
        "function sample<T, effects E>(left: Int64, right: Int64, clock: capability Clock, provider: capability Provider, pair: Pair /* type */ <T, Pair<T, Int64> /* nested */ >) -> Bool effects { clock.read<clock> } {\n"
        "    let value = identity<Pair<T, Int64>>(pair)\n"
        "    let compared = left /* lhs */ < /* rhs */ right && right > left\n"
        "    return handle clock.read<clock> with provider { compared }\n"
        "}\n";
    SolFormatted first;
    SolFormatted second;
    SolDiagnostics first_diagnostics;
    SolDiagnostics second_diagnostics;
    sol_formatted_init(&first);
    sol_formatted_init(&second);
    sol_diagnostics_init(&first_diagnostics);
    sol_diagnostics_init(&second_diagnostics);

    CHECK(format_text(compact, &first, &first_diagnostics));
    CHECK(format_text(spaced, &second, &second_diagnostics));
    CHECK(!sol_diagnostics_has_errors(&first_diagnostics));
    CHECK(!sol_diagnostics_has_errors(&second_diagnostics));
    check_output(&first, expected);
    check_output(&second, expected);
    check_second_pass(&first);

    sol_diagnostics_free(&first_diagnostics);
    sol_diagnostics_free(&second_diagnostics);
    sol_formatted_free(&first);
    sol_formatted_free(&second);
}

static void test_blank_line_collapse_and_final_newline(void) {
    static const char source_text[] =
        "module blanks\n\n\n\nrecord Empty {}\n\n\n\n\nfunction empty()->Int64{return 0}   \t";
    static const char expected[] =
        "module blanks\n\nrecord Empty {}\n\nfunction empty() -> Int64 { return 0 }\n";
    SolFormatted formatted;
    SolDiagnostics diagnostics;
    sol_formatted_init(&formatted);
    sol_diagnostics_init(&diagnostics);

    CHECK(format_text(source_text, &formatted, &diagnostics));
    CHECK(!sol_diagnostics_has_errors(&diagnostics));
    check_output(&formatted, expected);
    CHECK(formatted.length > 0 && formatted.text[formatted.length - 1] == '\n');
    CHECK(formatted.length < 2 || formatted.text[formatted.length - 2] != '\n');
    check_second_pass(&formatted);

    sol_diagnostics_free(&diagnostics);
    sol_formatted_free(&formatted);
}

static void test_continuation_indentation(void) {
    static const char source_text[] =
        "module continuation\n"
        "function choose(\n"
        "left:Int64,\n"
        "right:Int64,\n"
        ")->Int64 effects { pure } {\n"
        "return left\n"
        "}\n";
    static const char expected[] =
        "module continuation\n"
        "function choose(\n"
        "    left: Int64,\n"
        "    right: Int64,\n"
        ") -> Int64 effects { pure } {\n"
        "    return left\n"
        "}\n";
    SolFormatted formatted;
    SolDiagnostics diagnostics;
    sol_formatted_init(&formatted);
    sol_diagnostics_init(&diagnostics);
    CHECK(format_text(source_text, &formatted, &diagnostics));
    check_output(&formatted, expected);
    sol_diagnostics_free(&diagnostics);
    sol_formatted_free(&formatted);
}

static void test_malformed_source_preserves_output(void) {
    static const char malformed[] =
        "module broken\nrecord Missing { value: Int64\n";
    static const char sentinel[] = "preexisting output\n";
    SolFormatted formatted;
    SolDiagnostics diagnostics;
    sol_formatted_init(&formatted);
    sol_diagnostics_init(&diagnostics);

    formatted.capacity = sizeof(sentinel) + 16;
    formatted.text = malloc(formatted.capacity);
    CHECK(formatted.text != NULL);
    if (formatted.text != NULL) {
        memcpy(formatted.text, sentinel, sizeof(sentinel));
        formatted.length = sizeof(sentinel) - 1;
        char *original_text = formatted.text;
        size_t original_capacity = formatted.capacity;

        CHECK(!format_text(malformed, &formatted, &diagnostics));
        CHECK(sol_diagnostics_has_errors(&diagnostics));
        CHECK(formatted.text == original_text);
        CHECK(formatted.length == sizeof(sentinel) - 1);
        CHECK(formatted.capacity == original_capacity);
        CHECK(memcmp(formatted.text, sentinel, sizeof(sentinel)) == 0);
    }

    sol_diagnostics_free(&diagnostics);
    sol_formatted_free(&formatted);
}

static void test_embedded_nul_is_rejected(void) {
    char text[] = "module nul\n// embedded\0byte\n";
    SolSource source = {
        .path = "nul.sol",
        .text = text,
        .length = sizeof(text) - 1,
    };
    SolFormatted formatted;
    SolDiagnostics diagnostics;
    sol_formatted_init(&formatted);
    sol_diagnostics_init(&diagnostics);

    CHECK(!sol_format_source(&source, &formatted, &diagnostics));
    CHECK(formatted.text == NULL);
    CHECK(diagnostics.count == 1);
    if (diagnostics.count == 1) {
        CHECK(strcmp(diagnostics.items[0].code, "SOL-FMT-002") == 0);
    }

    sol_diagnostics_free(&diagnostics);
    sol_formatted_free(&formatted);
}

int main(void) {
    test_golden_formatting();
    test_comments_and_preserved_bytes();
    test_final_line_comment_preserves_trailing_spaces();
    test_angle_canonicality();
    test_blank_line_collapse_and_final_newline();
    test_continuation_indentation();
    test_malformed_source_preserves_output();
    test_embedded_nul_is_rejected();

    if (failures != 0) {
        fprintf(stderr, "%d formatter test(s) failed\n", failures);
        return 1;
    }
    return 0;
}
