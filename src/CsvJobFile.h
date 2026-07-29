#pragma once

// Pure C++17 parser for the DTH exporter job file (dth_exporter_jobs.csv).
// Deliberately Qt-free and SDK-free so it builds and unit-tests anywhere,
// including CI runners that have no Daz SDK.
//
// Contract (normative spec: dth-character-studio/docs/exporter-plugin-job-file.md):
//   - UTF-8 (an unexpected BOM is tolerated), LF or CRLF line endings
//   - first line must be the fixed header "daz-scene-path,daz-script-path";
//     otherwise the file is foreign/corrupt: ok=false and the caller must
//     NOT delete the file
//   - RFC-4180 quoting (quoted fields, doubled inner quotes)
//   - extra columns are ignored (forward compatibility)
//   - the scene column may be empty (= run in a new empty scene);
//     an empty script column makes the row invalid (skipped with a warning)

#include <string>
#include <string_view>
#include <vector>

namespace dthjr {

struct JobRow {
    std::string scenePath;  // empty = run the script in a new empty scene
    std::string scriptPath; // never empty in a valid row
};

struct ParseResult {
    bool ok = false;                   // header valid and file parseable
    std::string error;                 // set when !ok
    std::vector<JobRow> rows;          // valid rows, in file order
    std::vector<std::string> warnings; // skipped rows and other non-fatal issues
};

ParseResult parseJobCsv(std::string_view utf8Text);

} // namespace dthjr
