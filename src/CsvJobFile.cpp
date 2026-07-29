#include "CsvJobFile.h"

namespace dthjr {

namespace {

const char *const kHeaderScene = "daz-scene-path";
const char *const kHeaderScript = "daz-script-path";

// Splits the whole text into records of fields, RFC-4180 style: quoted
// fields may contain commas, quotes (doubled) and even newlines. Returns
// false on an unterminated quoted field.
bool tokenize(std::string_view text, std::vector<std::vector<std::string>> &records, std::string &error)
{
    std::vector<std::string> record;
    std::string field;
    bool inQuotes = false;
    bool fieldStarted = false; // true once the current field has any content or quoting

    auto endField = [&]() {
        record.push_back(field);
        field.clear();
        fieldStarted = false;
    };
    auto endRecord = [&]() {
        // A completely blank line produces one empty, never-started field —
        // skip it silently instead of recording an empty record.
        if (record.empty() && field.empty() && !fieldStarted)
            return;
        endField();
        records.push_back(record);
        record.clear();
    };

    size_t i = 0;
    while (i < text.size()) {
        const char c = text[i];
        if (inQuotes) {
            if (c == '"') {
                if (i + 1 < text.size() && text[i + 1] == '"') {
                    field += '"';
                    i += 2;
                } else {
                    inQuotes = false;
                    ++i;
                }
            } else {
                field += c;
                ++i;
            }
            continue;
        }
        switch (c) {
        case '"':
            if (!fieldStarted && field.empty()) {
                inQuotes = true;
                fieldStarted = true;
            } else {
                field += c; // stray quote mid-field: keep it literally
            }
            ++i;
            break;
        case ',':
            endField();
            ++i;
            break;
        case '\r':
            if (i + 1 < text.size() && text[i + 1] == '\n')
                ++i;
            endRecord();
            ++i;
            break;
        case '\n':
            endRecord();
            ++i;
            break;
        default:
            field += c;
            fieldStarted = true;
            ++i;
            break;
        }
    }
    if (inQuotes) {
        error = "unterminated quoted field";
        return false;
    }
    endRecord(); // flush a final record without trailing newline
    return true;
}

} // namespace

ParseResult parseJobCsv(std::string_view utf8Text)
{
    ParseResult result;

    // The studio writes without a BOM, but tolerate one defensively.
    if (utf8Text.size() >= 3 && static_cast<unsigned char>(utf8Text[0]) == 0xEF &&
        static_cast<unsigned char>(utf8Text[1]) == 0xBB && static_cast<unsigned char>(utf8Text[2]) == 0xBF) {
        utf8Text.remove_prefix(3);
    }

    std::vector<std::vector<std::string>> records;
    if (!tokenize(utf8Text, records, result.error))
        return result;

    if (records.empty()) {
        result.error = "empty file";
        return result;
    }

    // Header: the first two columns must match exactly; extra columns from a
    // future version are ignored.
    const std::vector<std::string> &header = records.front();
    if (header.size() < 2 || header[0] != kHeaderScene || header[1] != kHeaderScript) {
        result.error = "header mismatch (expected \"daz-scene-path,daz-script-path\")";
        return result;
    }

    for (size_t i = 1; i < records.size(); ++i) {
        const std::vector<std::string> &rec = records[i];
        if (rec.size() < 2) {
            result.warnings.push_back("row " + std::to_string(i) + " skipped: fewer than two columns");
            continue;
        }
        if (rec[1].empty()) {
            result.warnings.push_back("row " + std::to_string(i) + " skipped: empty script path");
            continue;
        }
        JobRow row;
        row.scenePath = rec[0]; // may legitimately be empty (= new empty scene)
        row.scriptPath = rec[1];
        result.rows.push_back(row);
    }

    result.ok = true;
    return result;
}

} // namespace dthjr
