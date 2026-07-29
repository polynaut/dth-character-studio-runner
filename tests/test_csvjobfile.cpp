// Framework-less test harness for the pure job-file parser.
// Builds with no Qt and no Daz SDK; returns non-zero on any failure.

#include "CsvJobFile.h"

#include <cstdio>
#include <string>

static int g_failures = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);      \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

using dthjr::parseJobCsv;
using dthjr::ParseResult;

static void test_happy_path()
{
    const ParseResult r = parseJobCsv(
        "daz-scene-path,daz-script-path\n"
        "C:\\Projects\\Electra\\primary\\Electra.duf,C:\\Lib\\Scripts\\ROM_Electra_G9.dsa\n"
        "C:/Projects/Electra/armor/Electra_Armor.duf,C:/Lib/Scripts/ROM_Electra_G9.dsa\n");
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(r.rows.size() == 2);
    CHECK(r.warnings.empty());
    CHECK(r.rows[0].scenePath == "C:\\Projects\\Electra\\primary\\Electra.duf");
    CHECK(r.rows[0].scriptPath == "C:\\Lib\\Scripts\\ROM_Electra_G9.dsa");
    CHECK(r.rows[1].scenePath == "C:/Projects/Electra/armor/Electra_Armor.duf");
}

static void test_crlf()
{
    const ParseResult r = parseJobCsv(
        "daz-scene-path,daz-script-path\r\n"
        "C:\\a.duf,C:\\b.dsa\r\n");
    CHECK(r.ok);
    CHECK(r.rows.size() == 1);
    CHECK(r.rows[0].scenePath == "C:\\a.duf");
    CHECK(r.rows[0].scriptPath == "C:\\b.dsa");
}

static void test_no_trailing_newline()
{
    const ParseResult r = parseJobCsv(
        "daz-scene-path,daz-script-path\n"
        "C:\\a.duf,C:\\b.dsa");
    CHECK(r.ok);
    CHECK(r.rows.size() == 1);
}

static void test_quoted_comma()
{
    const ParseResult r = parseJobCsv(
        "daz-scene-path,daz-script-path\n"
        "\"C:\\a, with comma\\s.duf\",C:\\b.dsa\n");
    CHECK(r.ok);
    CHECK(r.rows.size() == 1);
    CHECK(r.rows[0].scenePath == "C:\\a, with comma\\s.duf");
}

static void test_doubled_quotes()
{
    const ParseResult r = parseJobCsv(
        "daz-scene-path,daz-script-path\n"
        "\"C:\\a \"\"quoted\"\" dir\\s.duf\",C:\\b.dsa\n");
    CHECK(r.ok);
    CHECK(r.rows.size() == 1);
    CHECK(r.rows[0].scenePath == "C:\\a \"quoted\" dir\\s.duf");
}

static void test_empty_scene_column_is_valid()
{
    const ParseResult r = parseJobCsv(
        "daz-scene-path,daz-script-path\n"
        ",C:\\b.dsa\n");
    CHECK(r.ok);
    CHECK(r.rows.size() == 1);
    CHECK(r.rows[0].scenePath.empty());
    CHECK(r.rows[0].scriptPath == "C:\\b.dsa");
}

static void test_empty_script_column_is_skipped()
{
    const ParseResult r = parseJobCsv(
        "daz-scene-path,daz-script-path\n"
        "C:\\a.duf,\n"
        "C:\\c.duf,C:\\d.dsa\n");
    CHECK(r.ok);
    CHECK(r.rows.size() == 1);
    CHECK(r.rows[0].scenePath == "C:\\c.duf");
    CHECK(r.warnings.size() == 1);
}

static void test_single_column_row_is_skipped()
{
    const ParseResult r = parseJobCsv(
        "daz-scene-path,daz-script-path\n"
        "just-one-field\n"
        "C:\\c.duf,C:\\d.dsa\n");
    CHECK(r.ok);
    CHECK(r.rows.size() == 1);
    CHECK(r.warnings.size() == 1);
}

static void test_wrong_header_is_foreign()
{
    const ParseResult r = parseJobCsv("some,other,csv\n1,2,3\n");
    CHECK(!r.ok);
    CHECK(!r.error.empty());
    CHECK(r.rows.empty());
}

static void test_binary_junk_is_foreign()
{
    const ParseResult r = parseJobCsv(std::string_view("\x00\x01\x02\xff garbage without header", 26));
    CHECK(!r.ok);
    CHECK(r.rows.empty());
}

static void test_extra_columns_ignored()
{
    const ParseResult r = parseJobCsv(
        "daz-scene-path,daz-script-path,future-column\n"
        "C:\\a.duf,C:\\b.dsa,whatever,more\n");
    CHECK(r.ok);
    CHECK(r.rows.size() == 1);
    CHECK(r.rows[0].scenePath == "C:\\a.duf");
    CHECK(r.rows[0].scriptPath == "C:\\b.dsa");
}

static void test_utf8_bom_tolerated()
{
    const ParseResult r = parseJobCsv(
        "\xEF\xBB\xBF"
        "daz-scene-path,daz-script-path\n"
        "C:\\a.duf,C:\\b.dsa\n");
    CHECK(r.ok);
    CHECK(r.rows.size() == 1);
}

static void test_empty_file()
{
    const ParseResult r = parseJobCsv("");
    CHECK(!r.ok);
    CHECK(r.rows.empty());
}

static void test_header_only()
{
    const ParseResult r = parseJobCsv("daz-scene-path,daz-script-path\n");
    CHECK(r.ok);
    CHECK(r.rows.empty());
    CHECK(r.warnings.empty());
}

static void test_blank_lines_skipped()
{
    const ParseResult r = parseJobCsv(
        "daz-scene-path,daz-script-path\n"
        "\n"
        "C:\\a.duf,C:\\b.dsa\n"
        "\n");
    CHECK(r.ok);
    CHECK(r.rows.size() == 1);
    CHECK(r.warnings.empty());
}

static void test_unterminated_quote_is_error()
{
    const ParseResult r = parseJobCsv(
        "daz-scene-path,daz-script-path\n"
        "\"C:\\never closed,C:\\b.dsa\n");
    CHECK(!r.ok);
    CHECK(!r.error.empty());
}

int main()
{
    test_happy_path();
    test_crlf();
    test_no_trailing_newline();
    test_quoted_comma();
    test_doubled_quotes();
    test_empty_scene_column_is_valid();
    test_empty_script_column_is_skipped();
    test_single_column_row_is_skipped();
    test_wrong_header_is_foreign();
    test_binary_junk_is_foreign();
    test_extra_columns_ignored();
    test_utf8_bom_tolerated();
    test_empty_file();
    test_header_only();
    test_blank_lines_skipped();
    test_unterminated_quote_is_error();

    if (g_failures) {
        std::printf("%d check(s) FAILED\n", g_failures);
        return 1;
    }
    std::printf("all checks passed\n");
    return 0;
}
