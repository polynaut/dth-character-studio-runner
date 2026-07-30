// Framework-less test harness for the pure JSON job-file reader/writer
// (contract v2). Builds with no Qt and no Daz SDK; returns non-zero on any
// failure.

#include "JsonJobFile.h"

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

using dthjr::JobFileModel;
using dthjr::JobStatus;
using dthjr::JsonJob;
using dthjr::JsonParseResult;
using dthjr::parseJobJson;
using dthjr::writeJobJson;

static void test_happy_path()
{
    const JsonParseResult r = parseJobJson(
        "{\n"
        "  \"version\": 1,\n"
        "  \"type\": \"bulk-export\",\n"
        "  \"progress\": 0,\n"
        "  \"jobs\": [\n"
        "    { \"scenePath\": \"C:\\\\P\\\\primary\\\\Kira.duf\", \"scriptPath\": \"C:\\\\L\\\\.Bulk_ROM_Export.dsa\", \"status\": \"pending\" },\n"
        "    { \"scenePath\": \"\", \"scriptPath\": \"C:/L/Other.dsa\" }\n"
        "  ]\n"
        "}\n");
    CHECK(r.ok);
    CHECK(r.error.empty());
    CHECK(r.file.progress == 0);
    CHECK(r.file.jobs.size() == 2);
    CHECK(r.file.jobs[0].scenePath == "C:\\P\\primary\\Kira.duf");
    CHECK(r.file.jobs[0].scriptPath == "C:\\L\\.Bulk_ROM_Export.dsa");
    CHECK(r.file.jobs[0].status == JobStatus::Pending);
    CHECK(r.file.jobs[1].scenePath.empty()); // empty scene = new empty scene
}

static void test_round_trip_via_writer()
{
    JobFileModel model;
    model.progress = 50;
    JsonJob a;
    a.scenePath = "C:\\scenes\\A \"quoted\".duf";
    a.scriptPath = "C:\\lib\\.Bulk_ROM_Export.dsa";
    a.status = JobStatus::Done;
    JsonJob b;
    b.scenePath = "C:\\scenes\\B.duf";
    b.scriptPath = "C:\\lib\\.Bulk_ROM_Export.dsa";
    b.status = JobStatus::Failed;
    b.error = "scene not found";
    model.jobs.push_back(a);
    model.jobs.push_back(b);

    const std::string text = writeJobJson(model);
    const JsonParseResult r = parseJobJson(text);
    CHECK(r.ok);
    CHECK(r.file.progress == 50);
    CHECK(r.file.jobs.size() == 2);
    CHECK(r.file.jobs[0].scenePath == "C:\\scenes\\A \"quoted\".duf");
    CHECK(r.file.jobs[0].status == JobStatus::Done);
    CHECK(r.file.jobs[1].status == JobStatus::Failed);
    CHECK(r.file.jobs[1].error == "scene not found");
}

static void test_foreign_versions_and_types_are_rejected()
{
    CHECK(!parseJobJson("{\"version\": 2, \"jobs\": []}").ok);
    CHECK(!parseJobJson("{\"jobs\": []}").ok); // no version at all
    CHECK(!parseJobJson("{\"version\": 1, \"type\": \"mystery\", \"jobs\": []}").ok);
    CHECK(!parseJobJson("{\"version\": 1, \"type\": \"bulk-export\"}").ok); // no jobs
    CHECK(!parseJobJson("not json").ok);
    CHECK(!parseJobJson("").ok);
    // Torn write (the studio writes atomically, but be safe): reject cleanly.
    CHECK(!parseJobJson("{\"version\": 1, \"type\": \"bulk-export\", \"progress\": 4").ok);
}

static void test_tolerance()
{
    // Unknown fields anywhere are ignored; unknown statuses read as pending;
    // rows without a scriptPath are skipped with a warning; BOM tolerated;
    // progress clamps to 0..100.
    const JsonParseResult r = parseJobJson(
        "\xEF\xBB\xBF{\n"
        "  \"version\": 1,\n"
        "  \"type\": \"bulk-export\",\n"
        "  \"progress\": 250,\n"
        "  \"futureField\": { \"nested\": [1, 2, {\"x\": null}] },\n"
        "  \"jobs\": [\n"
        "    { \"scenePath\": \"C:/a.duf\", \"scriptPath\": \"C:/s.dsa\", \"status\": \"sideways\", \"futureFlag\": true },\n"
        "    { \"scenePath\": \"C:/b.duf\" }\n"
        "  ]\n"
        "}\n");
    CHECK(r.ok);
    CHECK(r.file.progress == 100);
    CHECK(r.file.jobs.size() == 1);
    CHECK(r.file.jobs[0].status == JobStatus::Pending);
    CHECK(r.warnings.size() == 1);
}

static void test_unicode_escapes()
{
    const JsonParseResult r = parseJobJson(
        "{\"version\": 1, \"type\": \"bulk-export\", \"progress\": 0, \"jobs\": ["
        "{ \"scenePath\": \"C:/\\u00c4rger/scene.duf\", \"scriptPath\": \"C:/s.dsa\" }"
        "]}");
    CHECK(r.ok);
    CHECK(r.file.jobs.size() == 1);
    CHECK(r.file.jobs[0].scenePath == "C:/\xC3\x84rger/scene.duf"); // Ä in UTF-8
}

static void test_writer_empty_batch()
{
    JobFileModel model;
    model.progress = 100;
    const JsonParseResult r = parseJobJson(writeJobJson(model));
    CHECK(r.ok);
    CHECK(r.file.progress == 100);
    CHECK(r.file.jobs.empty());
}

int main()
{
    test_happy_path();
    test_round_trip_via_writer();
    test_foreign_versions_and_types_are_rejected();
    test_tolerance();
    test_unicode_escapes();
    test_writer_empty_batch();

    if (g_failures == 0)
        std::printf("all json job-file tests passed\n");
    return g_failures == 0 ? 0 : 1;
}
