#pragma once

// Pure C++17 reader/writer for the DTH exporter job file (contracts v2+v3,
// dth_exporter_jobs.json). Deliberately Qt-free and SDK-free so it builds and
// unit-tests anywhere — and because the DS4 build is Qt 4.8, which has no
// QJsonDocument at all.
//
// Contract (normative spec: dth-character-studio/docs/exporter-plugin-job-file.md):
//   - UTF-8 (an unexpected BOM is tolerated)
//   - top-level object: { version: 1, type, progress: 0-100, progressLogPath?,
//     jobs: [{ scenePath, scriptPath?, steps?, status?, error? }, ...] }
//   - `progressLogPath` (v1.2.0, optional): absolute path of a VERBOSE
//     progress log. When present the plugin truncates it at batch start and
//     appends "[<percent>] <message>" lines as it works; the studio watches
//     the file for a live per-scene progress display. The percent is
//     PER-SCENE: each scene's export is `steps` equal steps (the row's
//     `steps`, e.g. 5 = open scene / generate ROM / export character / export
//     hair / deliver CSV) — the plugin reports the steps it OWNS (the scene
//     open, terminal done/failed), and the studio-generated export script
//     appends the interior steps to the same file with the same scale.
//   - version != 1 or an unknown type ⇒ foreign/future: ok=false and the
//     caller must NOT touch the file
//   - unknown fields anywhere are ignored (forward compatibility); the
//     writer emits only the known fields
//   - row validity is TYPE-dependent:
//       "bulk-export" — scenePath may be empty (= run in a new empty scene);
//         an empty scriptPath makes the row invalid (skipped with a warning)
//       "open-scene" (contract v3) — exactly ONE row with a non-empty
//         scenePath; scriptPath absent/empty is legal (nothing is executed).
//         Any other shape makes the whole file foreign (ok=false) — a scene
//         load is never best-effort.
//
// The plugin parses the studio-written file once, then repeatedly WRITES the
// renamed running_ file with updated per-row statuses + whole-batch progress.

#include <string>
#include <string_view>
#include <vector>

namespace dthjr {

enum class JobStatus { Pending, Running, Done, Failed };

struct JsonJob {
    std::string scenePath;  // bulk-export: empty = run the script in a new empty scene
    std::string scriptPath; // never empty in a valid bulk-export row; empty for open-scene
    // How many equal steps this row's export comprises (v1.2.0, for the
    // progress log's per-scene percent — the studio writes 4 or 5 depending
    // on whether the queued script builds a ROM). 0 = unknown/absent: the
    // plugin then only writes the 0/100 boundary lines.
    int steps = 0;
    JobStatus status = JobStatus::Pending;
    std::string error;      // short reason, set with Failed
};

struct JobFileModel {
    std::string type = "bulk-export"; // "bulk-export" | "open-scene" (validated)
    int progress = 0;       // whole-batch 0..100, plugin-owned after pickup
    std::string progressLogPath; // v1.2.0 (optional): the verbose progress log
    std::vector<JsonJob> jobs;
};

struct JsonParseResult {
    bool ok = false;                   // version/type valid and file parseable
    std::string error;                 // set when !ok
    JobFileModel file;                 // valid rows, in file order
    std::vector<std::string> warnings; // skipped rows and other non-fatal issues
};

JsonParseResult parseJobJson(std::string_view utf8Text);

// Serializes the model back to the contract shape (pretty, LF, trailing \n).
std::string writeJobJson(const JobFileModel &file);

// One progress-log line: "[<percent>] <message>\n", percent clamped to 0..100.
// Pure (Qt-free) so the format — which the studio parses — is unit-tested.
std::string formatProgressLine(int percent, const std::string &message);

} // namespace dthjr
