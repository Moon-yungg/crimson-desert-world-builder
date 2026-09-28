// CorePatternScan host fixture (plan wb079-unified-77f68967, Task 3).
//
// The production core TU is textually included ONCE here (the ui_shell_host.cpp convention), so the cases call the
// actual static ScanImage/ScanPattern/FindPattern/FindPatternCount instead of a copy of the scanner; the real
// overlay_discovery.cpp is linked as its own translation unit by core_pattern_scan.sources. Substituted are only
// the engine/service boundaries this suite never exercises (overlay install, thumbgen, input, httpapi, diag, the
// MinHook installation path). DllMain/Attach/InitThread are never run: the fixture allocates its own synthetic
// mapped PE64 images and points core::g_base at them.
//
// One case runs per FRESH process (the runner launches one process per case name): the production ScanImage caches
// a successful image in a function-local static, so a process carries exactly one accepted image - which is also
// the property the cache-retry case verifies.
//
// The eligibility separation is a BEHAVIOURAL contract here: a core-valid / binding-invalid image (no import
// directory, nonstandard section and file alignment) must be scanned by FindPattern/FindPatternCount while
// overlay::discovery::ImageBounds - the overlay's binding-eligibility view - still rejects it. The fixture
// deliberately does not name the core-bounds entry point, so the same fixture body runs red against the pre-fix
// source (where the wrapper still routed through binding eligibility) and green after the fix, with no edit in
// between.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <vector>

#include "../../asi/cdmodkit/cdmodkit.cpp"   // production core: the actual scanner wrappers live in this TU

// =========================================================================================================
// Engine/service boundaries the suite never exercises. These are the exact symbols the included production TU
// references and no production definition is linked for; none of them is a scanning decision.
// =========================================================================================================
namespace overlay { void Install() {} }
namespace core {
void CamWatch(int, int) {}
void FovTrace(int) {}
void CamTrace(int) {}
void InstallIoTrace() {}
void SetIoTrace(bool) {}
// Task 15 moved the console line dispatch into the production TU; its 'viewscan'/'findcamera' branches call the
// diag.cpp implementations this fixture deliberately does not link, exactly like the other diag symbols above.
void ViewScan() {}
void FindRenderCamera() {}
}
namespace thumbgen {
void Start() {}
void SetBackground(bool) {}
void SetQuality(int) {}
int Quality() { return 0; }
bool Background() { return false; }
unsigned GimmickKey(const std::string&) { return 0; }
bool Lz4Decode(const unsigned char*, size_t, std::vector<unsigned char>&, size_t) { return false; }
}
namespace input {
bool ScanDown(int, bool) { return false; }
void SetPlaceVks(const int*, int) {}
void SetFreeCam(bool) {}
void TakeLookDelta(float*, float*) {}
}
namespace httpapi { bool Start(int) { return false; } }
extern "C" {
MH_STATUS MH_Initialize() { return MH_ERROR_NOT_INITIALIZED; }
MH_STATUS MH_CreateHook(void*, void*, void**) { return MH_ERROR_FUNCTION_NOT_FOUND; }
MH_STATUS MH_EnableHook(void*) { return MH_ERROR_DISABLED; }
MH_STATUS MH_Uninitialize() { return MH_ERROR_NOT_INITIALIZED; }
const char* MH_StatusToString(MH_STATUS) { return "not installed by this fixture"; }
}

namespace {
int assertions = 0;
std::filesystem::path evidence;
struct CheckRec { std::string label; bool ok; std::string observed; };
std::vector<CheckRec> checks;

std::string Hex(uintptr_t v) { char b[32] = {}; std::snprintf(b, sizeof b, "0x%llx", (unsigned long long)v); return b; }
std::string Num(long long v) { char b[32] = {}; std::snprintf(b, sizeof b, "%lld", v); return b; }
std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') { out.push_back('\\'); out.push_back(c); }
        else if (c == '\n' || c == '\r' || c == '\t') out.push_back(' ');
        else out.push_back(c);
    }
    return out;
}

// A check records the machine values it observed even when it passes: the case receipt carries them, so a red run
// shows the actual scanner answer and not just "false".
void Check(bool ok, const std::string& label, const std::string& observed = "") {
    ++assertions;
    checks.push_back({label, ok, observed});
    std::printf("CHECK %s %s%s\n", ok ? "PASS" : "FAIL", label.c_str(), observed.empty() ? "" : (" | " + observed).c_str());
    if (!ok) throw std::runtime_error(label + (observed.empty() ? "" : (" | " + observed)));
}
void CaseHeader(const char* name) { std::printf("CASE: %s\n", name); }

// ---- production log sink -----------------------------------------------------------------------------------
// core::g_log is a translation-unit static of the included production TU and Attach() is what normally opens it;
// the fixture opens the same sink itself (never Attach/DllMain) so the production diagnostics - including the
// >64-token rejection - are real, machine-readable case evidence in the run log the runner requires.
std::string g_logPath;
FILE* OpenLog() {
    g_logPath = (evidence / "core_pattern_scan.log").string();
    core::g_log = std::fopen(g_logPath.c_str(), "a");
    return core::g_log;
}
// Reading the log needs a separate handle, so the sink is flushed and closed, read, then reopened for append.
std::string ReadLogAndReopen() {
    std::string text;
    if (core::g_log) { std::fflush(core::g_log); std::fclose(core::g_log); core::g_log = nullptr; }
    { std::ifstream in(g_logPath, std::ios::binary); text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()); }
    core::g_log = std::fopen(g_logPath.c_str(), "a");
    return text;
}
bool LogContains(const std::string& token) { return ReadLogAndReopen().find(token) != std::string::npos; }

// ---- synthetic mapped PE64 images ---------------------------------------------------------------------------
struct SectionSpec { uint32_t rva; uint32_t vsize; uint32_t characteristics; };
struct ImageSpec {
    uint32_t sizeOfImage = 0x80000;
    uint32_t sizeOfHeaders = 0x400;
    int32_t lfanew = 0x80;
    WORD machine = IMAGE_FILE_MACHINE_AMD64;
    WORD optionalMagic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
    uint32_t sectionAlignment = 0x1000;
    uint32_t fileAlignment = 0x200;
    uint32_t numberOfRvaAndSizes = 16;
    uint32_t importDirRva = 0x4000;                       // inside the first .rdata/data section: the binding layout fact
                                                          // (0 = no import directory, which only a core consumer accepts)
    uint32_t importDirSize = sizeof(IMAGE_IMPORT_DESCRIPTOR);
    std::vector<SectionSpec> sections = {
        { 0x1000, 0x3000, IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_CODE },
        { 0x4000, 0x1000, IMAGE_SCN_MEM_READ | IMAGE_SCN_CNT_INITIALIZED_DATA },
    };
    WORD dosMagic = IMAGE_DOS_SIGNATURE;
    DWORD ntSignature = IMAGE_NT_SIGNATURE;
    bool writeHeaders = true;                             // false: the DOS header only (NT headers never written)
};
constexpr uint32_t kExecRead = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
constexpr uint32_t kReadOnlyData = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;

void WriteImage(unsigned char* at, const ImageSpec& spec);
unsigned char* CreateImage(const ImageSpec& spec) {
    unsigned char* at = static_cast<unsigned char*>(VirtualAlloc(nullptr, spec.sizeOfImage, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE));
    if (!at) throw std::runtime_error("VirtualAlloc failed for the synthetic image");
    WriteImage(at, spec);
    return at;
}
void WriteImage(unsigned char* at, const ImageSpec& spec) {
    std::memset(at, 0, spec.sizeOfImage);
    IMAGE_DOS_HEADER* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(at);
    dos->e_magic = spec.dosMagic;
    dos->e_lfanew = spec.lfanew;
    if (!spec.writeHeaders) return;
    IMAGE_NT_HEADERS64* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(at + spec.lfanew);
    nt->Signature = spec.ntSignature;
    nt->FileHeader.Machine = spec.machine;
    nt->FileHeader.NumberOfSections = static_cast<WORD>(spec.sections.size());
    nt->FileHeader.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
    nt->OptionalHeader.Magic = spec.optionalMagic;
    nt->OptionalHeader.SizeOfImage = spec.sizeOfImage;
    nt->OptionalHeader.SizeOfHeaders = spec.sizeOfHeaders;
    nt->OptionalHeader.SectionAlignment = spec.sectionAlignment;
    nt->OptionalHeader.FileAlignment = spec.fileAlignment;
    nt->OptionalHeader.NumberOfRvaAndSizes = spec.numberOfRvaAndSizes;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].VirtualAddress = spec.importDirRva;
    nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT].Size = spec.importDirSize;
    const uintptr_t secBase = static_cast<uintptr_t>(spec.lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + sizeof(IMAGE_OPTIONAL_HEADER64);
    for (size_t i = 0; i < spec.sections.size(); i++) {
        IMAGE_SECTION_HEADER* sc = reinterpret_cast<IMAGE_SECTION_HEADER*>(at + secBase + i * sizeof(IMAGE_SECTION_HEADER));
        const char* name = i == 0 ? ".text" : (i == 1 ? ".text2" : ".rdata");
        std::memcpy(sc->Name, name, 7);
        sc->Misc.VirtualSize = spec.sections[i].vsize;
        sc->VirtualAddress = spec.sections[i].rva;
        sc->Characteristics = spec.sections[i].characteristics;
    }
}
void PutBytes(unsigned char* at, uint32_t rva, const unsigned char* bytes, size_t n) {
    std::memcpy(at + rva, bytes, n);
}
// A deterministic non-zero sequence: zero-filled padding in the synthetic sections can never match it.
void MakeNeedle(unsigned char* out, size_t n) {
    for (size_t i = 0; i < n; i++) out[i] = static_cast<unsigned char>(0x40 + ((i * 37 + 11) & 0x7F));
}
std::string PatternText(const unsigned char* bytes, size_t n) {
    std::string out;
    char b[8] = {};
    for (size_t i = 0; i < n; i++) { std::snprintf(b, sizeof b, "%02X", bytes[i]); if (i) out.push_back(' '); out += b; }
    return out;
}

// ---- cases -------------------------------------------------------------------------------------------------
unsigned char* g_image = nullptr;
std::string g_pattern;
unsigned char g_needle[96] = {};   // 65 tokens is the longest pattern a case builds

struct Scan { uintptr_t hit; int count; };
Scan Scanned(const std::string& pattern) { int n = -1; const uintptr_t hit = core::FindPatternCount(pattern.c_str(), &n); return { hit, n }; }
std::string Observed(const Scan& s) { return "hit=" + Hex(s.hit) + " count=" + Num(s.count) + " base=" + Hex(core::g_base); }

void CaseUnique() {
    CaseHeader("unique: one occurrence in the first executable section of an image both consumers accept");
    ImageSpec spec;
    g_image = CreateImage(spec);
    core::g_base = reinterpret_cast<uintptr_t>(g_image);
    MakeNeedle(g_needle, 16);
    PutBytes(g_image, 0x2000, g_needle, 16);
    g_pattern = PatternText(g_needle, 16);
    const Scan s = Scanned(g_pattern);
    Check(s.hit == core::g_base + 0x2000 && s.count == 1, "unique: FindPatternCount returns the single occurrence with count 1", Observed(s));
    Check(core::FindPattern(g_pattern.c_str()) == core::g_base + 0x2000, "unique: FindPattern returns the same address", "hit=" + Hex(core::FindPattern(g_pattern.c_str())));
    overlay::discovery::ImageInfo info = {};
    Check(overlay::discovery::ImageBounds(core::g_base, &info), "unique: the image is binding-eligible too (both policies agree on this one)",
          "base=" + Hex(info.base) + " size=" + Hex(info.size));
    Check(info.base == core::g_base && info.size == spec.sizeOfImage, "unique: the validated range is the image base and SizeOfImage", "base=" + Hex(info.base) + " size=" + Hex(info.size));
    unsigned char absent[16];
    MakeNeedle(absent, 16);
    absent[0] ^= 0xFF; absent[15] ^= 0x5A;
    const Scan a = Scanned(PatternText(absent, 16));
    Check(a.hit == 0 && a.count == 0, "unique: a completed scan without the needle is an absence, not a read error", Observed(a));
}

void CaseAmbiguous() {
    CaseHeader("ambiguous: two occurrences - the count is 2 and the first hit wins");
    ImageSpec spec;
    g_image = CreateImage(spec);
    core::g_base = reinterpret_cast<uintptr_t>(g_image);
    MakeNeedle(g_needle, 16);
    PutBytes(g_image, 0x2000, g_needle, 16);
    PutBytes(g_image, 0x2500, g_needle, 16);
    g_pattern = PatternText(g_needle, 16);
    const Scan s = Scanned(g_pattern);
    Check(s.hit == core::g_base + 0x2000 && s.count == 2, "ambiguous: both occurrences are counted and the first is returned", Observed(s));
    Check(s.count != 1, "ambiguous: the uniqueness decision is 'not unique' (count != 1)", Observed(s));
}

void CaseCorePolicy() {
    CaseHeader("core-policy: a core-valid / binding-invalid image is scanned by the core wrapper and still rejected by binding eligibility");
    ImageSpec spec;
    spec.importDirRva = 0;              // no import directory: the overlay's binding layout is absent
    spec.importDirSize = 0;
    spec.sectionAlignment = 0x2000;     // and the fixed 0x1000/0x200 alignment contract is absent too
    spec.fileAlignment = 0x400;
    spec.sections = { { 0x2000, 0x6000, kExecRead }, { 0x9000, 0x1000, kReadOnlyData } };
    spec.sizeOfImage = 0x10000;
    g_image = CreateImage(spec);
    core::g_base = reinterpret_cast<uintptr_t>(g_image);
    MakeNeedle(g_needle, 16);
    PutBytes(g_image, 0x6000, g_needle, 16);
    g_pattern = PatternText(g_needle, 16);
    overlay::discovery::ImageInfo info = {};
    Check(!overlay::discovery::ImageBounds(core::g_base, &info), "core-policy: binding eligibility rejects the image (no import directory, nonstandard alignment)",
          "base=" + Hex(info.base) + " size=" + Hex(info.size));
    const Scan s = Scanned(g_pattern);
    Check(s.hit == core::g_base + 0x6000 && s.count == 1, "core-policy: the production core wrapper scans it (bounded access + completeness only)", Observed(s));
    Check(core::FindPattern(g_pattern.c_str()) == core::g_base + 0x6000, "core-policy: FindPattern takes the same core route", "hit=" + Hex(core::FindPattern(g_pattern.c_str())));
}

void CaseChunkBoundary() {
    CaseHeader("chunk-boundary: a needle straddling the 64 KiB read boundary is found exactly once");
    ImageSpec spec;
    spec.sections = { { 0x1000, 0x40000, kExecRead }, { 0x50000, 0x1000, kReadOnlyData } };
    spec.sizeOfImage = 0x80000;
    g_image = CreateImage(spec);
    core::g_base = reinterpret_cast<uintptr_t>(g_image);
    MakeNeedle(g_needle, 16);
    const uint32_t straddle = 0x1000 + 0x10000 - 5;      // 5 bytes in the first 64 KiB chunk, 11 in the second
    PutBytes(g_image, straddle, g_needle, 16);
    g_pattern = PatternText(g_needle, 16);
    const Scan s = Scanned(g_pattern);
    Check(s.hit == core::g_base + straddle && s.count == 1, "chunk-boundary: the boundary-straddling needle is found once", Observed(s));
    unsigned char inner[16];
    MakeNeedle(inner, 16);
    inner[3] ^= 0x2C;                                    // a different needle: both must be found exactly once
    const uint32_t inside = 0x1000 + 0x10000 + 40;
    PutBytes(g_image, inside, inner, 16);
    const Scan s2 = Scanned(PatternText(inner, 16));
    Check(s2.hit == core::g_base + inside && s2.count == 1, "chunk-boundary: a needle fully inside the next chunk is found once", Observed(s2));
}

void CaseUnreadable() {
    CaseHeader("unreadable: a faulting chunk read fails closed and is not a sticky rejection");
    ImageSpec spec;
    spec.sections = { { 0x1000, 0x8000, kExecRead }, { 0x9000, 0x1000, kReadOnlyData } };
    spec.sizeOfImage = 0x10000;
    g_image = CreateImage(spec);
    core::g_base = reinterpret_cast<uintptr_t>(g_image);
    MakeNeedle(g_needle, 16);
    PutBytes(g_image, 0x7000, g_needle, 16);             // after the guard page
    g_pattern = PatternText(g_needle, 16);
    DWORD old = 0;
    if (!VirtualProtect(g_image + 0x5000, 0x1000, PAGE_READWRITE | PAGE_GUARD, &old)) throw std::runtime_error("VirtualProtect(PAGE_GUARD) failed");
    overlay::discovery::ImageInfo info = {};
    Check(overlay::discovery::ImageBounds(core::g_base, &info), "unreadable: the image passes bounds validation (the failure is at the chunk read)",
          "base=" + Hex(info.base) + " size=" + Hex(info.size));
    const Scan s = Scanned(g_pattern);
    Check(s.hit == 0 && s.count == 0, "unreadable: an incomplete scan returns no match and no count (never a partial uniqueness claim)", Observed(s));
    const Scan retry = Scanned(g_pattern);               // the guard page was consumed by the fault: the read now succeeds
    Check(retry.hit == core::g_base + 0x7000 && retry.count == 1, "unreadable: the incomplete read was a transient fault, not a cached rejection", Observed(retry));
}

void CaseMalformed() {
    CaseHeader("malformed: structurally invalid images are rejected fail-closed and never poison a later valid scan");
    MakeNeedle(g_needle, 16);
    g_pattern = PatternText(g_needle, 16);
    struct Bad { const char* label; ImageSpec spec; };
    std::vector<Bad> bad;
    { ImageSpec s; s.dosMagic = 0x1234; bad.push_back({ "dos_magic", s }); }
    { ImageSpec s; s.ntSignature = 0xDEADBEEF; bad.push_back({ "nt_signature", s }); }
    { ImageSpec s; s.machine = IMAGE_FILE_MACHINE_I386; bad.push_back({ "machine_i386", s }); }
    { ImageSpec s; s.optionalMagic = IMAGE_NT_OPTIONAL_HDR32_MAGIC; bad.push_back({ "optional_magic_pe32", s }); }
    { ImageSpec s; s.sections.clear(); bad.push_back({ "no_sections", s }); }
    { ImageSpec s; s.sizeOfImage = 0x10000; s.sections = { { 0x3000, 0xE000, kExecRead } }; bad.push_back({ "section_beyond_image", s }); }
    { ImageSpec s; s.lfanew = 0x2000; bad.push_back({ "lfanew_too_far", s }); }
    { ImageSpec s; s.sizeOfImage = 0x800; bad.push_back({ "size_of_image_too_small", s }); }
    { ImageSpec s; s.sizeOfHeaders = 0x100; bad.push_back({ "headers_below_section_table", s }); }
    { ImageSpec s; s.writeHeaders = false; bad.push_back({ "headers_not_written", s }); }
    for (const Bad& b : bad) {
        unsigned char* img = CreateImage(b.spec);
        core::g_base = reinterpret_cast<uintptr_t>(img);
        const Scan s = Scanned(g_pattern);
        Check(s.hit == 0 && s.count == 0, std::string("malformed(") + b.label + "): the core wrapper rejects it without a match or a count", Observed(s));
        overlay::discovery::ImageInfo info = {};
        Check(!overlay::discovery::ImageBounds(core::g_base, &info), std::string("malformed(") + b.label + "): binding eligibility rejects it as well");
        VirtualFree(img, 0, MEM_RELEASE);
    }
    ImageSpec good;
    g_image = CreateImage(good);
    core::g_base = reinterpret_cast<uintptr_t>(g_image);
    PutBytes(g_image, 0x2000, g_needle, 16);
    const Scan s = Scanned(g_pattern);
    Check(s.hit == core::g_base + 0x2000 && s.count == 1, "malformed: a valid image after the rejected ones still scans (no sticky rejection)", Observed(s));
    overlay::discovery::ImageInfo info = {};
    Check(overlay::discovery::ImageBounds(core::g_base, &info), "malformed: the final image is binding-eligible too");
}

void CaseCrossSection() {
    CaseHeader("cross-section: the executable scan crosses into the second executable section and never reads the read-only one");
    ImageSpec spec;
    spec.sections = { { 0x1000, 0x2000, kExecRead }, { 0x4000, 0x2000, kExecRead }, { 0x8000, 0x1000, kReadOnlyData } };
    spec.sizeOfImage = 0x10000;
    g_image = CreateImage(spec);
    core::g_base = reinterpret_cast<uintptr_t>(g_image);
    MakeNeedle(g_needle, 16);
    PutBytes(g_image, 0x5000, g_needle, 16);             // second executable section
    g_pattern = PatternText(g_needle, 16);
    unsigned char ro[16];
    MakeNeedle(ro, 16);
    ro[7] ^= 0x11;
    PutBytes(g_image, 0x8800, ro, 16);                   // read-only section
    const Scan s = Scanned(g_pattern);
    Check(s.hit == core::g_base + 0x5000 && s.count == 1, "cross-section: the executable scan reaches the second executable section", Observed(s));
    const Scan roScan = Scanned(PatternText(ro, 16));
    Check(roScan.hit == 0 && roScan.count == 0, "cross-section: the executable scan does not read the non-executable section", Observed(roScan));
    overlay::discovery::ImageInfo info = {};
    Check(overlay::discovery::ImageBounds(core::g_base, &info), "cross-section: the image is valid for both consumers");
    unsigned char val[64] = {}, mask[64] = {};
    for (int i = 0; i < 16; i++) { val[i] = ro[i]; mask[i] = 0xFF; }
    int hits = 0; bool complete = false;
    const uintptr_t hit = overlay::discovery::GameBoundaryScan(info, false, val, mask, 16, &hits, &complete);
    Check(complete && hit == 0x8800 && hits == 1, "cross-section: the read-access scan finds the read-only occurrence once and reports a complete scan",
          "hit=" + Hex(hit) + " count=" + Num(hits) + " complete=" + (complete ? "1" : "0"));
}

void CasePatternTokens(size_t tokens) {
    const std::string label = "pattern" + Num(static_cast<long long>(tokens));
    CaseHeader(("pattern boundary: " + label + " tokens").c_str());
    ImageSpec spec;
    g_image = CreateImage(spec);
    core::g_base = reinterpret_cast<uintptr_t>(g_image);
    MakeNeedle(g_needle, tokens);
    PutBytes(g_image, 0x2000, g_needle, tokens);
    g_pattern = PatternText(g_needle, tokens);
    const Scan s = Scanned(g_pattern);
    Check(s.hit == core::g_base + 0x2000 && s.count == 1, label + ": the wrapper accepts the pattern and finds the single occurrence", Observed(s));
    Check(core::FindPattern(g_pattern.c_str()) == core::g_base + 0x2000, label + ": FindPattern returns the same address", "hit=" + Hex(core::FindPattern(g_pattern.c_str())));
    overlay::discovery::ImageInfo info = {};
    Check(overlay::discovery::ImageBounds(core::g_base, &info), label + ": the image is valid for both consumers");
}

void CasePattern65() {
    CaseHeader("pattern65: a 65-token pattern is refused loudly, never prefix-matched");
    ImageSpec spec;
    g_image = CreateImage(spec);
    core::g_base = reinterpret_cast<uintptr_t>(g_image);
    MakeNeedle(g_needle, 65);
    PutBytes(g_image, 0x2000, g_needle, 65);             // present in the image: a prefix scan would find it
    g_pattern = PatternText(g_needle, 65);
    const Scan s = Scanned(g_pattern);
    Check(s.hit == 0 && s.count == 0, "pattern65: the over-long pattern is rejected without scanning", Observed(s));
    Check(core::FindPattern(g_pattern.c_str()) == 0, "pattern65: FindPattern refuses the same pattern", "hit=" + Hex(core::FindPattern(g_pattern.c_str())));
    Check(LogContains("scan] pattern_tokens_rejected tokens=65 limit=64"), "pattern65: the wrapper logs the token-limit rejection instead of returning 0 silently");
}

void CaseCacheRetry() {
    CaseHeader("cache-retry: a rejected image is not cached; the repaired core-only image is then scanned");
    ImageSpec broken;
    broken.writeHeaders = false;                         // DOS header only: nothing to validate
    g_image = CreateImage(broken);
    core::g_base = reinterpret_cast<uintptr_t>(g_image);
    MakeNeedle(g_needle, 16);
    g_pattern = PatternText(g_needle, 16);
    const Scan s = Scanned(g_pattern);
    Check(s.hit == 0 && s.count == 0, "cache-retry: the incomplete image is rejected", Observed(s));
    ImageSpec repaired;
    repaired.importDirRva = 0;                           // core-valid, binding-invalid: the acceptance can only come from the core route
    repaired.importDirSize = 0;
    repaired.sectionAlignment = 0x2000;
    repaired.fileAlignment = 0x400;
    repaired.sections = { { 0x2000, 0x6000, kExecRead }, { 0x9000, 0x1000, kReadOnlyData } };
    repaired.sizeOfImage = broken.sizeOfImage;
    WriteImage(g_image, repaired);
    PutBytes(g_image, 0x6000, g_needle, 16);
    const Scan retry = Scanned(g_pattern);
    Check(retry.hit == core::g_base + 0x6000 && retry.count == 1, "cache-retry: the repaired image is scanned (only a successful image is cached)", Observed(retry));
    Check(core::FindPattern(g_pattern.c_str()) == core::g_base + 0x6000, "cache-retry: the second call reuses the successful cached bounds", "hit=" + Hex(core::FindPattern(g_pattern.c_str())));
    overlay::discovery::ImageInfo info = {};
    Check(!overlay::discovery::ImageBounds(core::g_base, &info), "cache-retry: the image stays binding-ineligible, so the acceptance came from the core route");
}

void WriteCaseJson(const std::string& caseName, const char* status, const std::string& error) {
    if (evidence.empty()) return;
    const std::filesystem::path dir = evidence / "cases";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream f(dir / (caseName + ".json"), std::ios::binary);
    f << "{\n";
    f << "  \"suite\": \"CorePatternScan\",\n";
    f << "  \"case\": \"" << JsonEscape(caseName) << "\",\n";
    f << "  \"status\": \"" << status << "\",\n";
    f << "  \"assertions\": " << assertions << ",\n";
    f << "  \"checks\": [\n";
    for (size_t i = 0; i < checks.size(); i++) {
        f << "    { \"label\": \"" << JsonEscape(checks[i].label) << "\", \"ok\": " << (checks[i].ok ? "true" : "false")
          << ", \"observed\": \"" << JsonEscape(checks[i].observed) << "\" }" << (i + 1 < checks.size() ? "," : "") << "\n";
    }
    f << "  ],\n";
    const std::string log = ReadLogAndReopen();
    size_t reject = 0;
    for (size_t at = log.find("image_reject"); at != std::string::npos; at = log.find("image_reject", at + 1)) ++reject;
    f << "  \"log\": { \"path\": \"" << JsonEscape(g_logPath) << "\", \"bytes\": " << log.size()
      << ", \"image_reject_count\": " << reject
      << ", \"pattern_tokens_rejected\": " << (log.find("pattern_tokens_rejected") != std::string::npos ? "true" : "false") << " },\n";
    f << "  \"error\": \"" << JsonEscape(error) << "\",\n";
    f << "  \"image\": { \"base\": \"" << Hex(core::g_base) << "\", \"pattern\": \"" << JsonEscape(g_pattern) << "\" }\n";
    f << "}\n";
}
}   // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);   // a crash still shows which case ran
    std::string caseName;
    std::string error;
    for (int i = 1; i < argc; i++) {
        if (std::strcmp(argv[i], "--case") == 0 && i + 1 < argc) caseName = argv[++i];
        else if (evidence.empty()) evidence = argv[i];
    }
    if (caseName.empty()) { std::printf("usage: <fixture-dir> --case <name>\n"); return 2; }
    if (evidence.empty()) { std::printf("missing fixture directory argument\n"); return 2; }
    OpenLog();
    core::Log("[scan-test] case %s start", caseName.c_str());
    const char* status = "PASS";
    try {
        if (caseName == "unique") CaseUnique();
        else if (caseName == "ambiguous") CaseAmbiguous();
        else if (caseName == "core-policy") CaseCorePolicy();
        else if (caseName == "chunk-boundary") CaseChunkBoundary();
        else if (caseName == "unreadable") CaseUnreadable();
        else if (caseName == "malformed") CaseMalformed();
        else if (caseName == "cross-section") CaseCrossSection();
        else if (caseName == "pattern63") CasePatternTokens(63);
        else if (caseName == "pattern64") CasePatternTokens(64);
        else if (caseName == "pattern65") CasePattern65();
        else if (caseName == "cache-retry") CaseCacheRetry();
        else { std::printf("unknown case '%s'\n", caseName.c_str()); return 2; }
    } catch (const std::exception& e) {
        status = "FAIL";
        error = e.what();
        std::printf("FAIL %s: %s\n", caseName.c_str(), error.c_str());
    }
    core::Log("[scan-test] case %s %s assertions=%d", caseName.c_str(), status, assertions);
    WriteCaseJson(caseName, status, error);
    if (core::g_log) { std::fflush(core::g_log); std::fclose(core::g_log); core::g_log = nullptr; }
    std::printf("ASSERTIONS=%d\n", assertions);
    return std::strcmp(status, "PASS") == 0 ? 0 : 1;
}
