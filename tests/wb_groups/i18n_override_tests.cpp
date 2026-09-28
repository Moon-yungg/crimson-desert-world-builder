// I18nOverride suite: the production asi/cdmodkit/i18n.cpp TU is the unit under test. The fixture
// substitutes only the three core boundaries i18n.cpp touches - EmbeddedResource (resource 103 bytes),
// Log (captured diagnostics) and ModDir (per-case scratch directory) - and drives the real Translate,
// preference, glyph-range and Initialize logic. Every assertion below is a value comparison (translated
// text, log text, file bytes, glyph-range coverage), never a prose match.
//
// Contract under test (WB080 Task 14 core slice):
//   * ASI boot loads the embedded resource-103 table as the base.
//   * bin64\cdmodkit\locales.tsv is optional and read-only: each nonempty cell overrides the embedded
//     value for its locale/key, empty cells fall back to embedded, keys that exist only in the personal
//     file are retained.
//   * A missing, wrong-header, truncated or unreadable personal file leaves embedded translations in
//     place and emits a diagnostic naming the file; the personal file's bytes are never modified.
//   * Merged Korean text joins the font glyph ranges.
#include "../../asi/cdmodkit/i18n.h"

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#pragma comment(lib, "advapi32.lib")

#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

// ---- core boundary substitutes (only what the production i18n TU calls) ----
namespace core {
std::vector<std::string> g_fxLog;
std::vector<uint8_t> g_fxEmbedded;
bool g_fxEmbeddedAvailable = true;
std::string g_fxModDir;

void Log(const char* fmt, ...) {
    char buf[4096];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buf, sizeof buf, fmt, args);
    va_end(args);
    g_fxLog.push_back(std::string(buf));
}
// Resource 103 is the raw locales.tsv text linked into cdmodkit.asi.
bool EmbeddedResource(int resourceId, const uint8_t** data, size_t* size) {
    if (resourceId != 103 || !g_fxEmbeddedAvailable) {
        if (data) *data = nullptr;
        if (size) *size = 0;
        return false;
    }
    if (data) *data = g_fxEmbedded.data();
    if (size) *size = g_fxEmbedded.size();
    return true;
}
std::string ModDir() { return g_fxModDir; }
}   // namespace core

static int assertions = 0;
static std::vector<std::string> failureLog;

struct CaseRec { std::string id; int assertions = 0; int failures = 0; };
static std::vector<CaseRec> cases;
static std::string currentId;
static int caseStart = 0;
static size_t caseFailStart = 0;

static void Check(bool ok, const char* what) {
    ++assertions;
    if (!ok) {
        failureLog.push_back(currentId + ": " + what);
        std::fprintf(stderr, "FAIL [%s] %s\n", currentId.c_str(), what);
    }
}
static void BeginCase(const char* id) { currentId = id; caseStart = assertions; caseFailStart = failureLog.size(); }
static void EndCase() { cases.push_back({currentId, assertions - caseStart, (int)(failureLog.size() - caseFailStart)}); }

static const char* kHeader = "key\ten\tzh-CN\tzh-TW\tde\tfr\tko\tja\tes\tpt-BR\tru\ttr";

// A 12-column personal row: only the ko cell carries text unless stated otherwise (every other cell
// stays empty so the embedded value must survive there).
static std::string PersonalRow(const char* key, const char* ko, const char* en = "") {
    std::string row(key);
    row += "\t"; row += en;          // en
    row += "\t\t\t\t\t";      // zh-CN zh-TW de fr (4 empty cells = 4 tabs after en... see below)
    row += ko;                       // ko
    row += "\t\t\t\t\t";      // ja es pt-BR ru tr (5 empty cells)
    return row;
}

static bool WriteBytes(const std::string& path, const std::string& data) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(data.data(), (std::streamsize)data.size());
    f.close();
    if (!f) return false;
    // An old fixed timestamp makes even a same-bytes rewrite observable without sleeping.
    using FileTime = std::filesystem::file_time_type;
    std::filesystem::last_write_time(path, FileTime(FileTime::duration(132537600000000000LL)));
    return true;
}
static std::string ReadBytes(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw std::runtime_error("cannot read fixture file: " + path);
    std::string bytes((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    if (f.bad()) throw std::runtime_error("fixture read failed: " + path);
    return bytes;
}
static std::string Sha256(const std::string& bytes) {
    HCRYPTPROV provider = 0;
    if (!CryptAcquireContextW(&provider, nullptr, nullptr, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        throw std::runtime_error("CryptAcquireContextW failed");
    HCRYPTHASH hash = 0;
    BYTE digest[32] = {};
    DWORD size = sizeof digest;
    const bool ok = CryptCreateHash(provider, CALG_SHA_256, 0, 0, &hash) &&
        CryptHashData(hash, reinterpret_cast<const BYTE*>(bytes.data()), (DWORD)bytes.size(), 0) &&
        CryptGetHashParam(hash, HP_HASHVAL, digest, &size, 0) && size == sizeof digest;
    if (hash) CryptDestroyHash(hash);
    CryptReleaseContext(provider, 0);
    if (!ok) throw std::runtime_error("SHA-256 calculation failed");
    static const char hex[] = "0123456789abcdef";
    std::string result;
    for (BYTE value : digest) { result += hex[value >> 4]; result += hex[value & 15]; }
    return result;
}
struct FileState {
    std::string kind = "missing", bytes, sha256;
    uint64_t mtime = 0;
};
struct FileIdentity { std::string id, relativePath; FileState before, after; };
static std::vector<FileIdentity> identities;
static FileState Snapshot(const std::string& path) {
    FileState state;
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(std::filesystem::path(path).c_str(), GetFileExInfoStandard, &info)) {
        const DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
            throw std::runtime_error("cannot stat fixture file: " + path);
        return state;
    }
    state.kind = (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? "directory" : "file";
    state.mtime = (uint64_t(info.ftLastWriteTime.dwHighDateTime) << 32) | info.ftLastWriteTime.dwLowDateTime;
    if (state.kind == "file") { state.bytes = ReadBytes(path); state.sha256 = Sha256(state.bytes); }
    return state;
}
// Compare with shipped cells, not pinned UI prose. The two probe keys have no TSV escapes.
static std::string ShippedCell(const char* key, const char* language) {
    std::istringstream table(std::string(core::g_fxEmbedded.begin(), core::g_fxEmbedded.end()));
    std::string line, cell;
    size_t column = 0;
    std::getline(table, line);
    if (!line.empty() && line.back() == '\r') line.pop_back();
    std::istringstream header(line);
    while (std::getline(header, cell, '\t') && cell != language) ++column;
    if (cell != language) throw std::runtime_error("missing shipped locale column");
    while (std::getline(table, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        std::istringstream row(line);
        std::getline(row, cell, '\t');
        if (cell != key) continue;
        for (size_t i = 0; i < column; ++i)
            if (!std::getline(row, cell, '\t')) throw std::runtime_error("truncated shipped probe row");
        if (cell.empty()) throw std::runtime_error("empty shipped probe cell");
        return cell;
    }
    throw std::runtime_error("missing shipped probe key");
}
static bool LogHas(const char* needle) {
    for (const auto& line : core::g_fxLog)
        if (line.find(needle) != std::string::npos) return true;
    return false;
}
// A fresh boot for one case: the scratch mod dir becomes ModDir, then the production Initialize runs.
static void Boot(const std::string& modDir, const char* preference, bool denyRead = false) {
    const std::string path = (std::filesystem::path(modDir) / "locales.tsv").string();
    const FileState before = Snapshot(path);
    core::g_fxModDir = modDir;
    core::g_fxLog.clear();
    i18n::TestReset();
    Check(i18n::SetPreference(preference), "the preference id is accepted");
    {
        struct ReadLock {
            HANDLE handle = INVALID_HANDLE_VALUE;
            ~ReadLock() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
        } lock;
        if (denyRead) {
            lock.handle = CreateFileW(std::filesystem::path(path).c_str(), GENERIC_READ, 0, nullptr,
                                      OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (lock.handle == INVALID_HANDLE_VALUE) throw std::runtime_error("fixture read lock failed");
            std::ifstream probe(path, std::ios::binary);
            Check(!probe, "an existing regular file is unreadable while the exclusive handle is held");
        }
        i18n::Initialize();
    }
    const FileState after = Snapshot(path);
    identities.push_back({ currentId, "mod/" + std::filesystem::path(modDir).filename().string() + "/locales.tsv", before, after });
    Check(before.kind == after.kind, "Initialize preserves personal-file presence and kind");
    if (before.kind != "missing") Check(before.mtime == after.mtime, "Initialize preserves personal-file mtime");
    if (before.kind == "file") {
        Check(before.bytes == after.bytes, "Initialize preserves every personal-file byte");
        Check(before.sha256 == after.sha256, "pre/post personal-file SHA-256 values match");
    }
}
static std::vector<unsigned short> DecodeNonAscii(const std::string& utf8) {
    std::vector<unsigned short> out;
    size_t i = 0;
    while (i < utf8.size()) {
        const unsigned char c = (unsigned char)utf8[i];
        unsigned cp = 0;
        size_t len = 0;
        if (c < 0x80) { cp = c; len = 1; }
        else if ((c & 0xE0) == 0xC0) { cp = c & 0x1F; len = 2; }
        else if ((c & 0xF0) == 0xE0) { cp = c & 0x0F; len = 3; }
        else if ((c & 0xF8) == 0xF0) { cp = c & 0x07; len = 4; }
        else { ++i; continue; }
        if (i + len > utf8.size()) break;
        bool ok = true;
        for (size_t k = 1; k < len; ++k) {
            const unsigned char d = (unsigned char)utf8[i + k];
            if ((d & 0xC0) != 0x80) { ok = false; break; }
            cp = (cp << 6) | (d & 0x3F);
        }
        if (!ok) { ++i; continue; }
        i += len;
        if (cp >= 0x100 && cp <= 0xFFFF) out.push_back((unsigned short)cp);
    }
    return out;
}
static bool RangesCover(const unsigned short* ranges, unsigned short cp) {
    if (!ranges) return false;
    for (int i = 0; ranges[i] != 0; i += 2) {
        if (cp >= ranges[i] && cp <= ranges[i + 1]) return true;
    }
    return false;
}

static void RunCases(const std::string& dir) {
    const std::string modBase = (std::filesystem::path(dir) / "mod").string();

    // -------------------------------------------------------------------------------------------------
    // Embedded base: no personal file exists, the resource-103 table answers in the active language.
    // -------------------------------------------------------------------------------------------------
    BeginCase("I18N-EMBEDDED-BASE");
    {
        const std::string mod = (std::filesystem::path(modBase) / "base").string();
        std::filesystem::create_directories(mod);
        Boot(mod, "ko");
        Check(std::string(i18n::ActiveLanguage()) == "ko", "the ko preference resolves");
        Check(std::string(i18n::Translate("Cancel")) == ShippedCell("Cancel", "ko"),
              "embedded ko answers without any personal file");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Per-cell override: a nonempty personal ko cell wins, empty personal cells keep embedded values.
    // -------------------------------------------------------------------------------------------------
    BeginCase("I18N-OVERRIDE-CELL");
    {
        const std::string mod = (std::filesystem::path(modBase) / "override").string();
        std::filesystem::create_directories(mod);
        const std::string path = (std::filesystem::path(mod) / "locales.tsv").string();
        // CRLF endings like the shipped table; the en cell stays empty so embedded en must survive.
        const std::string body = std::string(kHeader) + "\r\n" +
            PersonalRow("Cancel", "\xEC\xB7\xA8\xEC\x86\x8C-\xEA\xB0\x9C\xEC\x9D\xB8") + "\r\n";
        Check(WriteBytes(path, body), "the synthetic personal file is written");
        const std::string before = ReadBytes(path);
        Boot(mod, "ko");
        Check(std::string(i18n::Translate("Cancel")) ==
                  "\xEC\xB7\xA8\xEC\x86\x8C-\xEA\xB0\x9C\xEC\x9D\xB8",
              "a nonempty personal ko cell overrides the embedded value");
        Check(i18n::SetPreference("en"), "switching to en is accepted");
        Check(std::string(i18n::Translate("Cancel")) == "Cancel",
              "an empty personal en cell falls back to the embedded value");
        Check(i18n::SetPreference("de"), "switching to a non-source fallback locale is accepted");
        const std::string embeddedGerman = ShippedCell("Cancel", "de");
        Check(embeddedGerman != "Cancel", "the empty-cell probe distinguishes embedded from source fallback");
        Check(std::string(i18n::Translate("Cancel")) == embeddedGerman,
              "an empty personal de cell keeps the embedded translation, not the source key");
        Check(ReadBytes(path) == before, "the personal file bytes are unchanged by the read");
        Check(LogHas("locales.tsv"), "the merge names the personal file in the log");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Absent key: a key the personal file never mentions keeps its embedded translation.
    // -------------------------------------------------------------------------------------------------
    BeginCase("I18N-ABSENT-KEY");
    {
        const std::string mod = (std::filesystem::path(modBase) / "absent").string();
        std::filesystem::create_directories(mod);
        const std::string path = (std::filesystem::path(mod) / "locales.tsv").string();
        const std::string body = std::string(kHeader) + "\n" +
            PersonalRow("Cancel", "\xEC\xB7\xA8\xEC\x86\x8C-\xEB\xB6\x80\xEC\x9E\xAC") + "\n";
        Check(WriteBytes(path, body), "the synthetic personal file is written");
        Boot(mod, "ko");
        Check(std::string(i18n::Translate("Cancel")) ==
                  "\xEC\xB7\xA8\xEC\x86\x8C-\xEB\xB6\x80\xEC\x9E\xAC",
              "the mentioned key is overridden");
        Check(std::string(i18n::Translate("Delete")) == ShippedCell("Delete", "ko"),
              "an unmentioned key keeps its embedded translation");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Installed-only key: a personal row for a key the embedded table lacks is retained as-is.
    // -------------------------------------------------------------------------------------------------
    BeginCase("I18N-INSTALLED-ONLY");
    {
        const std::string mod = (std::filesystem::path(modBase) / "installed").string();
        std::filesystem::create_directories(mod);
        const std::string path = (std::filesystem::path(mod) / "locales.tsv").string();
        static const char* kKey = "wb080-personal-only-label";
        const std::string ko = "\xEA\xB0\x9C\xEC\x9D\xB8 \xEC\xA0\x84\xEC\x9A\xA9 \xEB\x9D\xBC\xEB\xB2\xA8 \xEB\x9C\x95";
        const std::string body = std::string(kHeader) + "\n" +
            PersonalRow(kKey, ko.c_str(), "Personal only label") + "\n";
        Check(WriteBytes(path, body), "the synthetic personal file is written");
        Boot(mod, "ko");
        Check(std::string(i18n::Translate(kKey)) == ko,
              "an installed-only key answers from the personal file");
        Check(i18n::SetPreference("en"), "switching to en is accepted");
        Check(std::string(i18n::Translate(kKey)) == "Personal only label",
              "the installed-only en cell is retained too");
        Check(i18n::SetPreference("de"), "an empty installed-only locale can be selected");
        Check(std::string(i18n::Translate(kKey)) == kKey,
              "an empty installed-only cell falls back to its source key");
        Check(std::string(i18n::Translate("wb080-no-such-key")) == "wb080-no-such-key",
              "an unknown key still falls back to its source text");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Missing file: no personal file at all leaves embedded translations with a diagnostic.
    // -------------------------------------------------------------------------------------------------
    BeginCase("I18N-MISSING");
    {
        const std::string mod = (std::filesystem::path(modBase) / "missing").string();
        std::filesystem::create_directories(mod);
        Boot(mod, "ko");
        Check(std::string(i18n::Translate("Cancel")) == ShippedCell("Cancel", "ko"),
              "embedded ko survives a missing personal file");
        Check(LogHas("locales.tsv"), "the missing personal file is diagnosed by name");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Wrong header: the whole personal file is ignored, embedded stays, diagnostic names the file.
    // -------------------------------------------------------------------------------------------------
    BeginCase("I18N-WRONG-HEADER");
    {
        const std::string mod = (std::filesystem::path(modBase) / "wrongheader").string();
        std::filesystem::create_directories(mod);
        const std::string path = (std::filesystem::path(mod) / "locales.tsv").string();
        // Correct width, wrong locale order: a width-only header validator must fail this case.
        const std::string body = std::string("key\tko\tzh-CN\tzh-TW\tde\tfr\ten\tja\tes\tpt-BR\tru\ttr\n") +
            PersonalRow("Cancel", "\xEC\xB7\xA8\xEC\x86\x8C-\xEC\x9E\x98\xEB\xAA\xBB\xEB\x90\xA8") + "\n";
        Check(WriteBytes(path, body), "the synthetic personal file is written");
        const std::string before = ReadBytes(path);
        Boot(mod, "ko");
        Check(std::string(i18n::Translate("Cancel")) == ShippedCell("Cancel", "ko"),
              "embedded ko survives a wrong-header personal file");
        Check(ReadBytes(path) == before, "the wrong-header file bytes are unchanged by the read");
        Check(LogHas("locales.tsv"), "the wrong header is diagnosed by file name");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Truncated row: a malformed row is skipped (embedded kept for its key) while good rows still merge.
    // -------------------------------------------------------------------------------------------------
    BeginCase("I18N-TRUNCATED");
    {
        const std::string mod = (std::filesystem::path(modBase) / "truncated").string();
        std::filesystem::create_directories(mod);
        const std::string path = (std::filesystem::path(mod) / "locales.tsv").string();
        const std::string body = std::string(kHeader) + "\n" +
            PersonalRow("Cancel", "\xEC\xB7\xA8\xEC\x86\x8C-\xEC\x9C\xA0\xEC\xA7\x80") + "\n" +
            "Delete\t\xEC\x82\xAD\xEC\xA0\x9C\n";
        Check(WriteBytes(path, body), "the synthetic personal file is written");
        Boot(mod, "ko");
        Check(std::string(i18n::Translate("Cancel")) ==
                  "\xEC\xB7\xA8\xEC\x86\x8C-\xEC\x9C\xA0\xEC\xA7\x80",
              "the well-formed personal row still merges");
        Check(std::string(i18n::Translate("Delete")) == ShippedCell("Delete", "ko"),
              "embedded ko survives for the truncated row's key");
        Check(LogHas("locales.tsv"), "the truncated row is diagnosed by file name");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Unreadable file: a locales.tsv that cannot be read as a file keeps embedded with a diagnostic.
    // (A directory at the file path deterministically fails the text read on every machine.)
    // -------------------------------------------------------------------------------------------------
    BeginCase("I18N-UNREADABLE");
    {
        const std::string mod = (std::filesystem::path(modBase) / "unreadable").string();
        std::filesystem::create_directories(mod);
        std::filesystem::create_directories(std::filesystem::path(mod) / "locales.tsv");
        Boot(mod, "ko");
        Check(std::string(i18n::Translate("Cancel")) == ShippedCell("Cancel", "ko"),
              "embedded ko survives an unreadable personal file");
        Check(LogHas("locales.tsv"), "the unreadable personal file is diagnosed by name");
    }
    EndCase();

    BeginCase("I18N-LOCKED-FILE");
    {
        const std::string mod = (std::filesystem::path(modBase) / "locked").string();
        std::filesystem::create_directories(mod);
        const std::string path = (std::filesystem::path(mod) / "locales.tsv").string();
        Check(WriteBytes(path, std::string(kHeader) + "\n" + PersonalRow("Cancel", "locked-override") + "\n"),
              "the unreadable probe is a real, well-formed personal file");
        Boot(mod, "ko", true);
        Check(std::string(i18n::Translate("Cancel")) == ShippedCell("Cancel", "ko"),
              "embedded ko survives a sharing-denied file, without merging its override");
        Check(LogHas("locales.tsv"), "the sharing-denied file is diagnosed by name");
    }
    EndCase();

    BeginCase("I18N-EMPTY-FILE");
    {
        const std::string mod = (std::filesystem::path(modBase) / "empty").string();
        std::filesystem::create_directories(mod);
        const std::string path = (std::filesystem::path(mod) / "locales.tsv").string();
        Check(WriteBytes(path, ""), "a zero-byte personal file is created");
        Boot(mod, "ko");
        Check(std::string(i18n::Translate("Cancel")) == ShippedCell("Cancel", "ko"),
              "embedded ko survives a zero-byte personal file");
        Check(LogHas("locales.tsv"), "the zero-byte personal file is diagnosed by name");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Read-only boundary: bytes and mtime of the personal file are identical before and after the read.
    // -------------------------------------------------------------------------------------------------
    BeginCase("I18N-READONLY");
    {
        const std::string mod = (std::filesystem::path(modBase) / "readonly").string();
        std::filesystem::create_directories(mod);
        const std::string path = (std::filesystem::path(mod) / "locales.tsv").string();
        const std::string body = std::string(kHeader) + "\n" +
            PersonalRow("Cancel", "\xEC\xB7\xA8\xEC\x86\x8C-\xEC\x9D\xBD\xEA\xB8\xB0") + "\n";
        Check(WriteBytes(path, body), "the synthetic personal file is written");
        const std::string before = ReadBytes(path);
        const auto mtimeBefore = std::filesystem::last_write_time(path);
        Boot(mod, "ko");
        Check(std::string(i18n::Translate("Cancel")) ==
                  "\xEC\xB7\xA8\xEC\x86\x8C-\xEC\x9D\xBD\xEA\xB8\xB0",
              "the override applies in the read-only case");
        Check(ReadBytes(path) == before, "the personal file bytes are identical after the read");
        Check(std::filesystem::last_write_time(path) == mtimeBefore,
              "the personal file mtime is identical after the read");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Merged Korean glyphs: every non-ASCII code point of merged personal text is in the font ranges.
    // -------------------------------------------------------------------------------------------------
    BeginCase("I18N-GLYPHS");
    {
        const std::string mod = (std::filesystem::path(modBase) / "glyphs").string();
        std::filesystem::create_directories(mod);
        const std::string path = (std::filesystem::path(mod) / "locales.tsv").string();
        static const char* kKey = "wb080-personal-glyph-label";
        const std::string ko = "\xEA\xB0\x9C\xEC\x9D\xB8 \xEC\xA0\x84\xEC\x9A\xA9 \xEB\x9D\xBC\xEB\xB2\xA8 \xEB\x9C\x95";
        const std::string body = std::string(kHeader) + "\n" +
            PersonalRow(kKey, ko.c_str(), "Personal glyph label") + "\n";
        Check(WriteBytes(path, body), "the synthetic personal file is written");
        Boot(mod, "ko");
        Check(std::string(i18n::Translate(kKey)) == ko, "the installed-only ko text merged");
        const unsigned short* ranges = i18n::GlyphRanges();
        Check(ranges != nullptr, "glyph ranges exist after the merge");
        const std::vector<unsigned short> codepoints = DecodeNonAscii(ko);
        Check(!codepoints.empty(), "the probe text carries non-ASCII code points");
        bool covered = true;
        for (size_t i = 0; i < codepoints.size(); ++i) {
            if (!RangesCover(ranges, codepoints[i])) { covered = false; break; }
        }
        Check(covered, "every merged Korean code point is registered in the glyph ranges");
    }
    EndCase();

    // -------------------------------------------------------------------------------------------------
    // Preference boundary: unknown ids are refused, valid ids resolve through ActiveLanguage.
    // -------------------------------------------------------------------------------------------------
    BeginCase("I18N-PREFERENCE");
    {
        const std::string mod = (std::filesystem::path(modBase) / "preference").string();
        std::filesystem::create_directories(mod);
        Boot(mod, "en");
        Check(!i18n::SetPreference("xx"), "an unknown preference id is refused");
        Check(std::string(i18n::ActiveLanguage()) == "en", "the en preference resolves");
        Check(std::string(i18n::Translate("Cancel")) == "Cancel", "en answers the source text");
    }
    EndCase();
}

static void WriteEvidence(const std::string& dir) {
    if (dir.empty()) return;
    std::filesystem::create_directories(dir);
    std::ofstream log(std::filesystem::path(dir) / "i18n_override.log", std::ios::binary | std::ios::trunc);
    for (const auto& c : cases) {
        log << "case " << c.id << ": " << (c.failures == 0 ? "PASS" : "FAIL")
            << " assertions=" << c.assertions << " failures=" << c.failures << "\n";
    }
    for (const auto& f : failureLog) log << "FAILURE " << f << "\n";
    log.flush();
    if (!log) throw std::runtime_error("cannot write fixture log");
    std::ofstream json(std::filesystem::path(dir) / "file-identities.json", std::ios::binary | std::ios::trunc);
    auto state = [&](const FileState& value) {
        json << "{\"kind\":\"" << value.kind << "\",\"bytes\":" << value.bytes.size()
             << ",\"sha256\":\"" << value.sha256 << "\",\"mtimeFiletime100ns\":" << value.mtime << "}";
    };
    json << "{\"kind\":\"i18n-personal-file-identities\",\"files\":[";
    for (size_t i = 0; i < identities.size(); ++i) {
        const auto& identity = identities[i];
        if (i) json << ",";
        json << "\n{\"case\":\"" << identity.id << "\",\"relativePath\":\"" << identity.relativePath << "\",\"before\":";
        state(identity.before); json << ",\"after\":"; state(identity.after); json << "}";
    }
    json << "\n]}\n";
    json.flush();
    if (!json) throw std::runtime_error("cannot write file-identity evidence");
}

int main(int argc, char** argv) {
    const std::string dir = argc > 1 ? argv[1] : "";
    // The embedded base is the real shipped table: the runner starts the test with the repo root as
    // the working directory, so the resource-103 bytes are read from the candidate source table.
    {
        std::ifstream embedded("asi/cdmodkit/data/locales.tsv", std::ios::binary);
        core::g_fxEmbedded = std::vector<uint8_t>((std::istreambuf_iterator<char>(embedded)),
                                                  std::istreambuf_iterator<char>());
    }
    if (core::g_fxEmbedded.empty()) {
        std::fprintf(stderr, "FAIL [init] embedded locales.tsv did not load\n");
        std::printf("ASSERTIONS=%d\n", assertions);
        return 1;
    }
    try {
        RunCases(dir);
    } catch (const std::exception& e) {
        Check(false, "unhandled fixture exception");
        std::fprintf(stderr, "EXCEPTION %s\n", e.what());
    }
    WriteEvidence(dir);
    std::printf("ASSERTIONS=%d\n", assertions);
    if (!failureLog.empty()) { std::printf("FAILURES=%d\n", (int)failureLog.size()); return 1; }
    return 0;
}
