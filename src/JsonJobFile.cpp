#include "JsonJobFile.h"

#include <cctype>
#include <cstdio>

// A minimal recursive-descent parser for exactly the JSON subset the job file
// uses (objects, arrays, strings, numbers, true/false/null). Unknown values
// are parsed and DISCARDED — forward compatibility is "ignore, don't choke".
// No exceptions, no locale dependence, no allocation surprises.

namespace dthjr {
namespace {

struct Cursor {
    const char *p;
    const char *end;
    bool failed = false;
};

void skipWs(Cursor &c)
{
    while (c.p < c.end && (*c.p == ' ' || *c.p == '\t' || *c.p == '\r' || *c.p == '\n'))
        ++c.p;
}

bool consume(Cursor &c, char ch)
{
    skipWs(c);
    if (c.p < c.end && *c.p == ch) {
        ++c.p;
        return true;
    }
    return false;
}

void appendUtf8(std::string &out, unsigned int cp)
{
    if (cp <= 0x7f) {
        out += static_cast<char>(cp);
    } else if (cp <= 0x7ff) {
        out += static_cast<char>(0xc0 | (cp >> 6));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else if (cp <= 0xffff) {
        out += static_cast<char>(0xe0 | (cp >> 12));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    } else {
        out += static_cast<char>(0xf0 | (cp >> 18));
        out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
        out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
        out += static_cast<char>(0x80 | (cp & 0x3f));
    }
}

bool parseHex4(Cursor &c, unsigned int &out)
{
    if (c.end - c.p < 4)
        return false;
    unsigned int value = 0;
    for (int i = 0; i < 4; ++i) {
        const char ch = c.p[i];
        value <<= 4;
        if (ch >= '0' && ch <= '9')
            value |= static_cast<unsigned int>(ch - '0');
        else if (ch >= 'a' && ch <= 'f')
            value |= static_cast<unsigned int>(ch - 'a' + 10);
        else if (ch >= 'A' && ch <= 'F')
            value |= static_cast<unsigned int>(ch - 'A' + 10);
        else
            return false;
    }
    c.p += 4;
    out = value;
    return true;
}

bool parseString(Cursor &c, std::string &out)
{
    out.clear();
    if (!consume(c, '"'))
        return false;
    while (c.p < c.end) {
        const char ch = *c.p++;
        if (ch == '"')
            return true;
        if (ch == '\\') {
            if (c.p >= c.end)
                return false;
            const char esc = *c.p++;
            switch (esc) {
            case '"': out += '"'; break;
            case '\\': out += '\\'; break;
            case '/': out += '/'; break;
            case 'b': out += '\b'; break;
            case 'f': out += '\f'; break;
            case 'n': out += '\n'; break;
            case 'r': out += '\r'; break;
            case 't': out += '\t'; break;
            case 'u': {
                unsigned int cp = 0;
                if (!parseHex4(c, cp))
                    return false;
                // Surrogate pair (paths rarely need it, but be correct).
                if (cp >= 0xd800 && cp <= 0xdbff && c.end - c.p >= 6 && c.p[0] == '\\' && c.p[1] == 'u') {
                    Cursor peek = c;
                    peek.p += 2;
                    unsigned int low = 0;
                    if (parseHex4(peek, low) && low >= 0xdc00 && low <= 0xdfff) {
                        cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00);
                        c = peek;
                    }
                }
                appendUtf8(out, cp);
                break;
            }
            default:
                return false;
            }
        } else {
            out += ch;
        }
    }
    return false; // unterminated
}

bool skipValue(Cursor &c); // forward

bool skipObject(Cursor &c)
{
    if (!consume(c, '{'))
        return false;
    skipWs(c);
    if (consume(c, '}'))
        return true;
    while (true) {
        std::string key;
        if (!parseString(c, key))
            return false;
        if (!consume(c, ':'))
            return false;
        if (!skipValue(c))
            return false;
        if (consume(c, ','))
            continue;
        return consume(c, '}');
    }
}

bool skipArray(Cursor &c)
{
    if (!consume(c, '['))
        return false;
    skipWs(c);
    if (consume(c, ']'))
        return true;
    while (true) {
        if (!skipValue(c))
            return false;
        if (consume(c, ','))
            continue;
        return consume(c, ']');
    }
}

bool skipLiteral(Cursor &c, const char *lit)
{
    const char *q = lit;
    while (*q) {
        if (c.p >= c.end || *c.p != *q)
            return false;
        ++c.p;
        ++q;
    }
    return true;
}

bool parseNumber(Cursor &c, double &out)
{
    skipWs(c);
    const char *start = c.p;
    if (c.p < c.end && (*c.p == '-'))
        ++c.p;
    while (c.p < c.end && (std::isdigit(static_cast<unsigned char>(*c.p)) || *c.p == '.' || *c.p == 'e' || *c.p == 'E' || *c.p == '+' || *c.p == '-'))
        ++c.p;
    if (c.p == start)
        return false;
    // sscanf on the bounded slice — no locale-dependent strtod surprises with
    // the plain digit/dot grammar JSON allows.
    const std::string slice(start, static_cast<size_t>(c.p - start));
    return std::sscanf(slice.c_str(), "%lf", &out) == 1;
}

bool skipValue(Cursor &c)
{
    skipWs(c);
    if (c.p >= c.end)
        return false;
    switch (*c.p) {
    case '{': return skipObject(c);
    case '[': return skipArray(c);
    case '"': {
        std::string ignored;
        return parseString(c, ignored);
    }
    case 't': return skipLiteral(c, "true");
    case 'f': return skipLiteral(c, "false");
    case 'n': return skipLiteral(c, "null");
    default: {
        double ignored = 0;
        return parseNumber(c, ignored);
    }
    }
}

JobStatus statusFromString(const std::string &value)
{
    if (value == "running")
        return JobStatus::Running;
    if (value == "done")
        return JobStatus::Done;
    if (value == "failed")
        return JobStatus::Failed;
    return JobStatus::Pending; // unknown statuses read as pending (tolerant)
}

const char *statusToString(JobStatus status)
{
    switch (status) {
    case JobStatus::Running: return "running";
    case JobStatus::Done: return "done";
    case JobStatus::Failed: return "failed";
    case JobStatus::Pending: break;
    }
    return "pending";
}

// Parses one row as-is. Validity is TYPE-dependent (an open-scene row is
// legal without a script), so it is judged after the whole file is read —
// never here.
bool parseJob(Cursor &c, JsonJob &job)
{
    if (!consume(c, '{'))
        return false;
    skipWs(c);
    if (!consume(c, '}')) {
        while (true) {
            std::string key;
            if (!parseString(c, key))
                return false;
            if (!consume(c, ':'))
                return false;
            if (key == "scenePath") {
                if (!parseString(c, job.scenePath))
                    return false;
            } else if (key == "scriptPath") {
                if (!parseString(c, job.scriptPath))
                    return false;
            } else if (key == "status") {
                std::string status;
                if (!parseString(c, status))
                    return false;
                job.status = statusFromString(status);
            } else if (key == "error") {
                if (!parseString(c, job.error))
                    return false;
            } else if (!skipValue(c)) {
                return false; // unknown key: value must still be well-formed
            }
            if (consume(c, ','))
                continue;
            if (!consume(c, '}'))
                return false;
            break;
        }
    }
    return true;
}

void escapeInto(std::string &out, const std::string &value)
{
    for (const char ch : value) {
        switch (ch) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\b': out += "\\b"; break;
        case '\f': out += "\\f"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (static_cast<unsigned char>(ch) < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned char>(ch));
                out += buf;
            } else {
                out += ch; // UTF-8 bytes pass through verbatim
            }
        }
    }
}

} // namespace

JsonParseResult parseJobJson(std::string_view utf8Text)
{
    JsonParseResult result;
    Cursor c{utf8Text.data(), utf8Text.data() + utf8Text.size()};
    // Tolerate a UTF-8 BOM.
    if (utf8Text.size() >= 3 && static_cast<unsigned char>(c.p[0]) == 0xef
        && static_cast<unsigned char>(c.p[1]) == 0xbb && static_cast<unsigned char>(c.p[2]) == 0xbf)
        c.p += 3;

    if (!consume(c, '{')) {
        result.error = "not a JSON object";
        return result;
    }

    bool sawVersion = false;
    double version = 0;
    std::string type = "bulk-export"; // absent type reads as the default
    double progress = 0;
    std::vector<JsonJob> jobs;
    bool sawJobs = false;

    skipWs(c);
    if (!consume(c, '}')) {
        while (true) {
            std::string key;
            if (!parseString(c, key)) {
                result.error = "malformed object key";
                return result;
            }
            if (!consume(c, ':')) {
                result.error = "missing ':' after key";
                return result;
            }
            if (key == "version") {
                if (!parseNumber(c, version)) {
                    result.error = "malformed version";
                    return result;
                }
                sawVersion = true;
            } else if (key == "type") {
                if (!parseString(c, type)) {
                    result.error = "malformed type";
                    return result;
                }
            } else if (key == "progress") {
                if (!parseNumber(c, progress)) {
                    result.error = "malformed progress";
                    return result;
                }
            } else if (key == "jobs") {
                if (!consume(c, '[')) {
                    result.error = "jobs is not an array";
                    return result;
                }
                sawJobs = true;
                skipWs(c);
                if (!consume(c, ']')) {
                    while (true) {
                        JsonJob job;
                        if (!parseJob(c, job)) {
                            result.error = "malformed job row";
                            return result;
                        }
                        jobs.push_back(job);
                        if (consume(c, ','))
                            continue;
                        if (!consume(c, ']'))
                        {
                            result.error = "unterminated jobs array";
                            return result;
                        }
                        break;
                    }
                }
            } else if (!skipValue(c)) {
                result.error = "malformed value for key '" + key + "'";
                return result;
            }
            if (consume(c, ','))
                continue;
            if (!consume(c, '}')) {
                result.error = "unterminated top-level object";
                return result;
            }
            break;
        }
    }

    if (!sawVersion || version != 1) {
        result.error = "unsupported version (expected 1)";
        return result;
    }
    if (type != "bulk-export" && type != "open-scene") {
        result.error = "unknown job type '" + type + "'";
        return result;
    }
    if (!sawJobs) {
        result.error = "missing jobs array";
        return result;
    }

    if (type == "open-scene") {
        // Contract v3: exactly one row, with a scene, nothing to execute.
        // Anything else makes the whole file foreign — a scene load is never
        // best-effort ("open nothing"/"open several" is not a request).
        if (jobs.size() != 1 || jobs[0].scenePath.empty()) {
            result.error = "invalid open-scene batch (exactly one row with a scenePath)";
            return result;
        }
    } else {
        // bulk-export: a row without a script cannot run — skip it, keep going.
        std::vector<JsonJob> runnable;
        for (size_t i = 0; i < jobs.size(); ++i) {
            if (jobs[i].scriptPath.empty())
                result.warnings.push_back("skipped a job row without a scriptPath");
            else
                runnable.push_back(jobs[i]);
        }
        jobs.swap(runnable);
    }

    if (progress < 0)
        progress = 0;
    if (progress > 100)
        progress = 100;
    result.file.type = type;
    result.file.progress = static_cast<int>(progress);
    result.file.jobs = jobs;
    result.ok = true;
    return result;
}

std::string writeJobJson(const JobFileModel &file)
{
    std::string out = "{\n  \"version\": 1,\n  \"type\": \"";
    escapeInto(out, file.type.empty() ? std::string("bulk-export") : file.type);
    out += "\",\n  \"progress\": ";
    int progress = file.progress;
    if (progress < 0)
        progress = 0;
    if (progress > 100)
        progress = 100;
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%d", progress);
    out += buf;
    out += ",\n  \"jobs\": [";
    for (size_t i = 0; i < file.jobs.size(); ++i) {
        const JsonJob &job = file.jobs[i];
        out += (i == 0) ? "\n" : ",\n";
        out += "    {\n      \"scenePath\": \"";
        escapeInto(out, job.scenePath);
        out += "\",\n      \"scriptPath\": \"";
        escapeInto(out, job.scriptPath);
        out += "\",\n      \"status\": \"";
        out += statusToString(job.status);
        out += "\"";
        if (!job.error.empty()) {
            out += ",\n      \"error\": \"";
            escapeInto(out, job.error);
            out += "\"";
        }
        out += "\n    }";
    }
    out += file.jobs.empty() ? "]\n}\n" : "\n  ]\n}\n";
    return out;
}

} // namespace dthjr
