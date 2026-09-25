// Optional time-of-day and weather controls for World Builder.
//
// Reverse-engineering reference:
//   Nostyxx/CrimsonWeather - https://github.com/Nostyxx/CrimsonWeather
//   Reference snapshot inspected: 24b2a9b503c528c3025e2cfb3274b80fe681dc7c
//
// The environment-manager/time-of-day layout, the idea of freezing visual time
// by clamping its lower/upper limits to one value, and the composed weather-table
// field locations were informed by CrimsonWeather. This file is an independent
// World Builder implementation; CrimsonWeather source is not bundled here.
//
// Important: "Freeze time" below freezes the visual time-of-day / lighting path.
// It does NOT pause the game simulation, NPCs, physics, combat or quest logic.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include "core.h"
#include "core_internal.h"
#include "guard.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace core {
namespace {

static ptrdiff_t g_envGetEntityVt = 0x60;
static ptrdiff_t g_envGetTimeVt = 0x178;
static ptrdiff_t g_entitySetTimeVt = 0x130;

constexpr ptrdiff_t kWeatherWindSpeed = 0x138;
constexpr ptrdiff_t kWeatherAltitudeWindRatio = 0x158;
constexpr ptrdiff_t kWeatherSnow = 0x168;
constexpr ptrdiff_t kWeatherRain = 0x16C;
constexpr ptrdiff_t kWeatherWindBlendContribution = 0x1A4;
constexpr ptrdiff_t kAtmosphereCloudAmount = 0xA8;
constexpr ptrdiff_t kAtmosphereMieAerosolDensity = 0x88;
constexpr ptrdiff_t kAtmosphereNativeFogSecondary = 0x9C;

using WeatherComposeFn = long long(__fastcall*)(long long weatherState, float dt);
using WeatherTickFn = void(__fastcall*)(long long self, float dt);
using ActivateEffectFn = void(__fastcall*)(long long self, int id, long long* slotA, long long* slotB, float v);
using SetIntensityFn = void(__fastcall*)(long long particleMgr, int handle, float v);
using EnvGetEntityFn = long long(__fastcall*)(void* envMgr);
using EnvGetTimeFn = double(__fastcall*)(void* envMgr);
using EntitySetTimeFn = void(__fastcall*)(long long entity, float value);

static uintptr_t* g_envManagerGlobal = nullptr;
static ptrdiff_t g_timeLower = 0;
static ptrdiff_t g_timeUpper = 0;
static ptrdiff_t g_timeCurrentA = 0;
static ptrdiff_t g_timeCurrentB = 0;

static std::atomic<bool> g_timeAvailable{ false };
static std::atomic<bool> g_timeCurrentValid{ false };
static std::atomic<float> g_timeCurrentHour{ 12.0f };
static std::atomic<float> g_timeTargetHour{ 12.0f };
static std::atomic<bool> g_timeFreeze{ false };
static std::atomic<bool> g_timeApply{ false };
static std::atomic<int> g_timeSetHoldTicks{ 0 };
static long long g_timeEntity = 0;
static bool g_timeBaselineValid = false;
static bool g_timeDomainHours = true;
static float g_timeBaseLower = 0.0f;
static float g_timeBaseUpper = 24.0f;
static bool g_timeFreezeApplied = false;

static WeatherComposeFn g_origWeatherCompose = nullptr;
static WeatherTickFn g_origWeatherTick = nullptr;
static ActivateEffectFn g_activateEffect = nullptr;
static SetIntensityFn g_setIntensity = nullptr;
static int* g_nullSentinel = nullptr;
static std::atomic<bool> g_snowEffectsAvailable{ false };
static ptrdiff_t g_weatherNodeContainer = 0x60;
static std::atomic<bool> g_weatherAvailable{ false };
static std::atomic<bool> g_weatherClear{ false };
static std::atomic<bool> g_rainOverride{ false };
static std::atomic<float> g_rainValue{ 0.0f };
static std::atomic<bool> g_snowOverride{ false };
static std::atomic<float> g_snowValue{ 0.0f };
static bool g_snowWasOverridden = false;
static int g_snowCleanupTicks = 0;
static std::atomic<bool> g_cloudOverride{ false };
static std::atomic<float> g_cloudValue{ 1.0f };
static std::atomic<bool> g_windOverride{ false };
static std::atomic<float> g_windMultiplier{ 1.0f };

static long long __fastcall HookWeatherCompose(long long weatherState, float dt);
static void __fastcall HookWeatherTick(long long self, float dt);

static float Clamp(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

static uintptr_t RipTarget6(uintptr_t instruction) {
    int32_t disp = 0;
    if (!ReadBytes(instruction + 2, &disp, sizeof(disp))) return 0;
    return instruction + 6 + static_cast<intptr_t>(disp);
}

static uintptr_t CallTarget(uintptr_t instruction) {
    uint8_t op = 0;
    int32_t disp = 0;
    if (!ReadBytes(instruction, &op, 1) || op != 0xE8 ||
        !ReadBytes(instruction + 1, &disp, sizeof(disp))) return 0;
    return instruction + 5 + static_cast<intptr_t>(disp);
}

static uintptr_t FunctionStartOf(uintptr_t address) {
    if (!g_base || address < g_base) return 0;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    auto* table = reinterpret_cast<RUNTIME_FUNCTION*>(g_base + dir.VirtualAddress);
    const size_t count = dir.Size / sizeof(RUNTIME_FUNCTION);
    const uintptr_t rva = address - g_base;
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (table[mid].BeginAddress <= rva) lo = mid + 1; else hi = mid;
    }
    if (!lo) return 0;
    RUNTIME_FUNCTION fn = table[lo - 1];
    if (!(fn.BeginAddress <= rva && rva < fn.EndAddress)) return 0;
    for (int depth = 0; depth < 16; ++depth) {
#ifdef __MINGW32__
        const uint32_t unwindRva = fn.UnwindData;
#else
        const uint32_t unwindRva = fn.UnwindInfoAddress;
#endif
        uint8_t head[4] = {};
        if (!ReadBytes(g_base + unwindRva, head, sizeof(head))) return 0;
        const uint8_t flags = head[0] >> 3;
        const uint8_t codes = head[2];
        if (!(flags & UNW_FLAG_CHAININFO)) break;
        RUNTIME_FUNCTION chained{};
        const uintptr_t chainedAt = g_base + unwindRva + 4 + ((codes + 1) & ~1u) * 2u;
        if (!ReadBytes(chainedAt, &chained, sizeof(chained))) return 0;
        fn = chained;
    }
    return g_base + fn.BeginAddress;
}

static std::vector<uintptr_t> FindDirectCallsites(uintptr_t target, size_t cap = 8) {
    std::vector<uintptr_t> hits;
    if (!target || !g_base) return hits;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && hits.size() < cap; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uintptr_t start = g_base + sec[i].VirtualAddress;
        const uint8_t* bytes = reinterpret_cast<const uint8_t*>(start);
        const size_t size = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + 5 <= size && hits.size() < cap; ++k) {
            if (bytes[k] != 0xE8) continue;
            int32_t disp = 0;
            memcpy(&disp, bytes + k + 1, sizeof(disp));
            if (start + k + 5 + static_cast<intptr_t>(disp) == target) hits.push_back(start + k);
        }
    }
    return hits;
}

static bool ExecutablePointer(uintptr_t p) {
    if (!p) return false;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!VirtualQuery(reinterpret_cast<const void*>(p), &mbi, sizeof(mbi)) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) return false;
    const DWORD execute = PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
    return (mbi.Protect & execute) != 0;
}
static float NormalizeHour(float h) {
    if (!std::isfinite(h)) return 0.0f;
    h = fmodf(h, 24.0f);
    if (h < 0.0f) h += 24.0f;
    return h;
}

static std::vector<int> ParsePattern(const char* text) {
    std::vector<int> out;
    const char* p = text;
    while (p && *p) {
        while (*p == ' ' || *p == '\t') ++p;
        if (!*p) break;
        if (*p == '?') {
            out.push_back(-1);
            ++p;
            if (*p == '?') ++p;
        } else {
            char* end = nullptr;
            const unsigned long v = strtoul(p, &end, 16);
            if (end == p) break;
            out.push_back(static_cast<int>(v & 0xFFu));
            p = end;
        }
        while (*p && *p != ' ' && *p != '\t') ++p;
    }
    return out;
}

static std::vector<uintptr_t> ScanAll(const char* pattern, size_t cap = 32) {
    std::vector<uintptr_t> hits;
    const std::vector<int> pat = ParsePattern(pattern);
    if (pat.empty() || !g_base) return hits;

    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    auto* sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections && hits.size() < cap; ++i) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* start = reinterpret_cast<const uint8_t*>(g_base + sec[i].VirtualAddress);
        const size_t size = sec[i].Misc.VirtualSize;
        if (size < pat.size()) continue;
        for (size_t k = 0; k + pat.size() <= size && hits.size() < cap; ++k) {
            bool ok = true;
            for (size_t j = 0; j < pat.size(); ++j) {
                if (pat[j] >= 0 && start[k + j] != static_cast<uint8_t>(pat[j])) { ok = false; break; }
            }
            if (ok) hits.push_back(reinterpret_cast<uintptr_t>(start + k));
        }
    }
    return hits;
}

static uintptr_t ScanUnique(const char* pattern, const char* label) {
    const auto hits = ScanAll(pattern, 4);
    if (hits.size() != 1) {
        Log("[environment] %s: %zu signature matches", label, hits.size());
        return 0;
    }
    return hits[0];
}

static uintptr_t RipTarget7(uintptr_t instruction) {
    int32_t disp = 0;
    if (!ReadBytes(instruction + 3, &disp, sizeof(disp))) return 0;
    return instruction + 7 + static_cast<intptr_t>(disp);
}

static bool ReadPointer(uintptr_t address, uintptr_t& value) {
    value = 0;
    if (!ReadBytes(address, &value, sizeof(value))) return false;
    return value >= 0x10000 && (value >> 47) == 0;
}

static bool ReadFloat(uintptr_t address, float& value) {
    value = NAN;
    return ReadBytes(address, &value, sizeof(value)) && std::isfinite(value);
}

static bool ResolveEnvManagerGlobal() {
    // Exact variants observed across the same game family. Keeping them narrow
    // makes this optional feature fail closed after a patch instead of guessing.
    struct EnvPattern { const char* pattern; ptrdiff_t getEntityVt; };
    static const EnvPattern patterns[] = {
        { "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 60 48 8B 88 E0 0E 00 00", 0x60 },
        { "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 48 8B 88 F0 0E 00 00", 0x40 },
        { "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 48 8B 88 D8 0E 00 00", 0x40 },
        { "48 8B 0D ?? ?? ?? ?? 48 8B 01 FF 50 40 48 8B 88 E0 0E 00 00", 0x40 },
    };
    for (const auto& p : patterns) {
        const auto hits = ScanAll(p.pattern, 3);
        if (hits.size() != 1) continue;
        const uintptr_t global = RipTarget7(hits[0]);
        uintptr_t probe = 0;
        if (global && ReadBytes(global, &probe, sizeof(probe))) {
            g_envManagerGlobal = reinterpret_cast<uintptr_t*>(global);
            g_envGetEntityVt = p.getEntityVt;
            Log("[environment] environment manager global resolved at rva 0x%llx (get-entity vt 0x%llx)",
                static_cast<unsigned long long>(global - g_base),
                static_cast<unsigned long long>(g_envGetEntityVt));
            return true;
        }
    }
    Log("[environment] environment manager global not resolved");
    return false;
}

static bool ExtractTimeStoreOffset(uintptr_t functionLikeHit, ptrdiff_t& out) {
    uint8_t code[0x90] = {};
    if (!ReadBytes(functionLikeHit, code, sizeof(code))) return false;
    for (size_t i = 0; i + 8 <= sizeof(code); ++i) {
        const bool sse = code[i] == 0xF3 && code[i + 1] == 0x0F && code[i + 2] == 0x11 && code[i + 3] == 0x8B;
        const bool vex = code[i] == 0xC5 && code[i + 1] == 0xFA && code[i + 2] == 0x11 && code[i + 3] == 0x8B;
        if (!sse && !vex) continue;
        int32_t disp = 0;
        memcpy(&disp, code + i + 4, sizeof(disp));
        if (disp >= 0x200 && disp <= 0x800) {
            out = static_cast<ptrdiff_t>(disp);
            return true;
        }
    }
    return false;
}

static uintptr_t FirstUnique(const char* const* patterns, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const auto hits = ScanAll(patterns[i], 3);
        if (hits.size() == 1) return hits[0];
    }
    return 0;
}

static void ResolveTimeVtableOffsets() {
    static const char* patterns[] = {
        "48 8B 49 30 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B 4B 30 0F 28 F0 F3 0F 59 35 ?? ?? ?? ?? 48 8B 01 F3 0F 59 35 ?? ?? ?? ?? FF 50 ?? 0F 28 CE 48 8B C8 48 8B 10",
        "48 8B 49 30 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B 4B 30 0F 28 F0 48 8B 01 FF 50 ?? 0F 28 CE 48 8B C8 48 8B 10",
        "48 8B 49 30 48 8B 01 FF 90 ?? ?? ?? ?? 48 8B 4B 30 C5 FA 59 0D ?? ?? ?? ?? C5 F2 59 35 ?? ?? ?? ?? 48 8B 01 FF 50 ?? C5 F8 28 CE 48 8B C8 48 8B 10",
    };
    const uintptr_t hit = FirstUnique(patterns, sizeof(patterns) / sizeof(patterns[0]));
    if (!hit) {
        Log("[environment] time vtable layout: using defaults entity=0x%llx get=0x%llx set=0x%llx",
            static_cast<unsigned long long>(g_envGetEntityVt),
            static_cast<unsigned long long>(g_envGetTimeVt),
            static_cast<unsigned long long>(g_entitySetTimeVt));
        return;
    }
    const uintptr_t fn = FunctionStartOf(hit);
    uint8_t code[0x120] = {};
    if (!fn || !ReadBytes(fn, code, sizeof(code))) return;
    ptrdiff_t getEntity = 0, getTime = 0, setTime = 0;
    for (size_t i = 0; i + 6 <= sizeof(code); ++i) {
        if (!getTime && code[i] == 0xFF && code[i + 1] == 0x90) {
            int32_t d = 0; memcpy(&d, code + i + 2, sizeof(d));
            if (d > 0x80 && d < 0x300 && (d % 8) == 0) getTime = d;
        }
        if (!setTime && code[i] == 0xFF && (code[i + 1] == 0xA2 || code[i + 1] == 0x92)) {
            int32_t d = 0; memcpy(&d, code + i + 2, sizeof(d));
            if (d > 0x80 && d < 0x300 && (d % 8) == 0) setTime = d;
        }
    }
    for (size_t i = 0; i + 3 <= sizeof(code); ++i) {
        if (code[i] == 0xFF && code[i + 1] == 0x50) {
            const ptrdiff_t d = static_cast<uint8_t>(code[i + 2]);
            if (d == g_envGetEntityVt) { getEntity = d; break; }
        }
    }
    if (getEntity == g_envGetEntityVt && getTime && setTime && getTime != setTime) {
        g_envGetTimeVt = getTime;
        g_entitySetTimeVt = setTime;
        Log("[environment] time vtable layout entity=0x%llx get=0x%llx set=0x%llx",
            static_cast<unsigned long long>(g_envGetEntityVt),
            static_cast<unsigned long long>(g_envGetTimeVt),
            static_cast<unsigned long long>(g_entitySetTimeVt));
    } else {
        Log("[environment] time vtable derivation rejected; using defaults get=0x%llx set=0x%llx",
            static_cast<unsigned long long>(g_envGetTimeVt),
            static_cast<unsigned long long>(g_entitySetTimeVt));
    }
}

static bool ResolveTimeLayout() {
    static const char* lowerPatterns[] = {
        "40 57 48 83 EC 20 83 7A 08 02 48 8B FA 72 ?? 48 8B 09 48 89 5C 24 30 48 8B 01 FF 50 60 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? C5 FB 5A C8 C5 FA 11 8B B4 03 00 00",
        "57 48 83 EC 20 83 7A 08 02 48 8B FA 72 ?? 48 8B 09 48 89 5C 24 30 48 8B 01 FF 50 40 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? C5 FB 5A C8 C5 FA 11 8B B4 03 00 00",
        "40 57 48 83 EC 20 83 7A 08 02 48 8B FA 72 ?? 48 8B 09 48 89 5C 24 30 48 8B 01 FF 50 40 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? C5 FB 5A C8 C5 FA 11 8B C4 03 00 00",
        "40 57 48 83 EC 20 83 7A 08 02 48 8B FA 72 ?? 48 8B 09 48 89 5C 24 30 48 8B 01 FF 50 40 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? C5 FB 5A C8 C5 FA 11 8B D4 03 00 00",
        "40 57 48 83 EC 20 83 7A 08 02 48 8B FA 72 ?? 48 8B 09 48 89 5C 24 30 48 8B 01 FF 50 40 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? C5 FB 5A C8 C5 FA 11 8B CC 03 00 00",
    };
    static const char* upperPatterns[] = {
        "40 57 48 83 EC 20 83 7A 08 02 48 8B FA 72 ?? 48 8B 09 48 89 5C 24 30 48 8B 01 FF 50 60 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? C5 FB 5A C8 C5 FA 11 8B B8 03 00 00",
        "57 48 83 EC 20 83 7A 08 02 48 8B FA 72 ?? 48 8B 09 48 89 5C 24 30 48 8B 01 FF 50 40 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? C5 FB 5A C8 C5 FA 11 8B B8 03 00 00",
        "40 57 48 83 EC 20 83 7A 08 02 48 8B FA 72 ?? 48 8B 09 48 89 5C 24 30 48 8B 01 FF 50 40 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? C5 FB 5A C8 C5 FA 11 8B C8 03 00 00",
        "40 57 48 83 EC 20 83 7A 08 02 48 8B FA 72 ?? 48 8B 09 48 89 5C 24 30 48 8B 01 FF 50 40 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? C5 FB 5A C8 C5 FA 11 8B D8 03 00 00",
        "40 57 48 83 EC 20 83 7A 08 02 48 8B FA 72 ?? 48 8B 09 48 89 5C 24 30 48 8B 01 FF 50 40 48 8B 0F 48 8B D8 48 8B 49 08 FF 15 ?? ?? ?? ?? C5 FB 5A C8 C5 FA 11 8B D0 03 00 00",
    };
    const uintptr_t lowHit = FirstUnique(lowerPatterns, sizeof(lowerPatterns) / sizeof(lowerPatterns[0]));
    const uintptr_t highHit = FirstUnique(upperPatterns, sizeof(upperPatterns) / sizeof(upperPatterns[0]));
    ptrdiff_t lower = 0, upper = 0;
    if (!lowHit || !highHit || !ExtractTimeStoreOffset(lowHit, lower) || !ExtractTimeStoreOffset(highHit, upper) || upper - lower != 4) {
        Log("[environment] visual-time layout unresolved (low=%p high=%p lower=0x%llx upper=0x%llx)",
            reinterpret_cast<void*>(lowHit), reinterpret_cast<void*>(highHit),
            static_cast<unsigned long long>(lower), static_cast<unsigned long long>(upper));
        return false;
    }

    g_timeLower = lower;
    g_timeUpper = upper;
    g_timeCurrentA = lower - 4;
    g_timeCurrentB = lower - 8;
    Log("[environment] visual-time fields lower=0x%llx upper=0x%llx current=0x%llx/0x%llx",
        static_cast<unsigned long long>(g_timeLower),
        static_cast<unsigned long long>(g_timeUpper),
        static_cast<unsigned long long>(g_timeCurrentA),
        static_cast<unsigned long long>(g_timeCurrentB));
    ResolveTimeVtableOffsets();
    return true;
}

static uintptr_t ResolveWeatherTick(bool& is201) {
    is201 = false;
    const auto current = ScanAll(
        "40 53 48 81 EC C0 00 00 00 C5 F2 58 81 C8 00 00 00 C5 F8 29 B4 24 B0 00 00 00 C5 78 29 54 24 70", 3);
    if (current.size() == 1) { is201 = true; return current[0]; }
    const auto legacyA = ScanAll("48 8B C4 53 48 81 EC ?? 00 00 00 C5 F2 58 81 C8 00 00 00", 3);
    if (legacyA.size() == 1) return legacyA[0];
    const auto legacyB = ScanAll("48 8B C4 53 48 81 EC B0 00 00 00 80 3D", 3);
    if (legacyB.size() == 1) return legacyB[0];
    Log("[environment] weather tick not uniquely resolved");
    return 0;
}

static bool ResolveWeatherEffects() {
    bool is201 = false;
    const uintptr_t tick = ResolveWeatherTick(is201);
    if (!tick) return false;

    uintptr_t activate = CallTarget(tick + (is201 ? 0x159 : 0x2AA));
    uintptr_t intensity = CallTarget(tick + (is201 ? 0x17C : 0x2CC));
    if (!activate) {
        const auto hits = ScanAll(
            "4C 8B DC 49 89 5B 08 49 89 6B 10 49 89 73 18 57 48 83 EC 40 49 8B F1 48 8B F9 49 8B 00 4C 8B 10", 3);
        if (hits.size() == 1) activate = hits[0];
    }
    if (!intensity) {
        const auto hits = ScanAll("89 54 24 10 53 48 83 EC 30 83 79 0C 00 48 8B D9 C5 F8 29 74 24 20", 3);
        if (hits.size() == 1) intensity = hits[0];
    }

    int* sentinel = nullptr;
    // Prefer deriving the shared null-handle sentinel from the activation helper
    // itself. Besides finding the pointer, this validates the 0xFC handle-array
    // layout that SetEffectIntensity relies on.
    if (activate) {
        uint8_t code[0x80] = {};
        if (ReadBytes(activate, code, sizeof(code))) {
            for (size_t i = 0; i + 13 <= sizeof(code); ++i) {
                if (code[i] != 0x8B || code[i + 1] != 0x05 ||
                    code[i + 6] != 0x39 || code[i + 7] != 0x84 || code[i + 8] != 0x99) continue;
                uint32_t handleOff = 0;
                memcpy(&handleOff, code + i + 9, sizeof(handleOff));
                if (handleOff != 0xFC) continue;
                int32_t disp = 0;
                memcpy(&disp, code + i + 2, sizeof(disp));
                const uintptr_t at = activate + i + 6 + static_cast<intptr_t>(disp);
                int value = 0;
                if (ReadBytes(at, &value, sizeof(value))) sentinel = reinterpret_cast<int*>(at);
                break;
            }
        }
    }
    if (!sentinel) {
        const auto sentinelHits = ScanAll("8B 05 ?? ?? ?? ?? 48 8B F9 39 01", 3);
        if (sentinelHits.size() == 1) {
            const uintptr_t at = RipTarget6(sentinelHits[0]);
            int value = 0;
            if (at && ReadBytes(at, &value, sizeof(value))) sentinel = reinterpret_cast<int*>(at);
        }
    }
    if (!activate || !intensity || !sentinel) {
        Log("[environment] snow effect bridge incomplete (tick=%p activate=%p intensity=%p sentinel=%p)",
            reinterpret_cast<void*>(tick), reinterpret_cast<void*>(activate),
            reinterpret_cast<void*>(intensity), reinterpret_cast<void*>(sentinel));
        return false;
    }

    g_activateEffect = reinterpret_cast<ActivateEffectFn>(activate);
    g_setIntensity = reinterpret_cast<SetIntensityFn>(intensity);
    g_nullSentinel = sentinel;
    if (!InstallInternalHook(reinterpret_cast<void*>(tick), reinterpret_cast<void*>(&HookWeatherTick),
                             reinterpret_cast<void**>(&g_origWeatherTick), "environment weather tick")) {
        g_activateEffect = nullptr; g_setIntensity = nullptr; g_nullSentinel = nullptr;
        return false;
    }
    Log("[environment] snow/particle bridge ready (tick rva 0x%llx)",
        static_cast<unsigned long long>(tick - g_base));
    return true;
}

static bool ResolveWeatherLayout() {
    uintptr_t rainGetter = 0;
    static const char* rainPatterns[] = {
        "48 8B 51 60 4C 8B D1 48 85 D2 B9 40 00 00 00 48 8D 42 18 48 0F 44 C1 41 80 7A 31 00 4C 8B 08 4D 8D 81 6C 01 00 00",
        "48 8B 51 58 4C 8B D1 48 85 D2 B9 40 00 00 00 48 8D 42 18 48 0F 44 C1 41 80 7A 31 00 4C 8B 08 4D 8D 81 6C 01 00 00",
        "48 8B 51 50 4C 8B D1",
    };
    for (const char* pattern : rainPatterns) {
        const auto hits = ScanAll(pattern, 3);
        if (hits.size() == 1) { rainGetter = hits[0]; break; }
    }
    if (!rainGetter) Log("[environment] rain getter not uniquely resolved");
    if (!rainGetter) return false;
    uint8_t container = 0;
    if (!ReadBytes(rainGetter + 3, &container, 1) || container < 0x40 || container > 0x80 || (container & 7)) {
        Log("[environment] weather container offset rejected: 0x%02x", container);
        return false;
    }
    g_weatherNodeContainer = container;

    const uintptr_t compositor = ScanUnique(
        "48 8B C4 C5 FA 11 48 10 48 89 48 08 55 53 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 88 48 81 EC",
        "weather compositor");
    if (!compositor) return false;

    // The signature above names the internal compositor. Its single native caller
    // has the (weatherState, dt) ABI used by the stable override hook. Hooking the
    // compositor itself with that ABI would corrupt arguments, so resolve and
    // validate the caller explicitly and fail closed if the call graph changes.
    const auto callsites = FindDirectCallsites(compositor, 4);
    if (callsites.size() != 1) {
        Log("[environment] weather compositor has %zu direct callers; weather control disabled", callsites.size());
        return false;
    }
    const uintptr_t compose = FunctionStartOf(callsites[0]);
    if (!compose) {
        Log("[environment] weather compose caller start not found");
        return false;
    }
    uint8_t prologue[8] = {};
    if (!ReadBytes(compose, prologue, sizeof(prologue)) ||
        prologue[0] != 0x48 || prologue[1] != 0x89 || prologue[2] != 0x5C || prologue[3] != 0x24 ||
        prologue[5] != 0x55 || prologue[6] != 0x56 || prologue[7] != 0x57) {
        Log("[environment] weather compose caller validation failed at rva 0x%llx",
            static_cast<unsigned long long>(compose - g_base));
        return false;
    }

    if (!InstallInternalHook(reinterpret_cast<void*>(compose),
                             reinterpret_cast<void*>(&HookWeatherCompose),
                             reinterpret_cast<void**>(&g_origWeatherCompose),
                             "environment weather compose")) {
        Log("[environment] weather compose hook failed");
        return false;
    }

    Log("[environment] weather controls ready (compose rva 0x%llx, compositor rva 0x%llx, container 0x%llx)",
        static_cast<unsigned long long>(compose - g_base),
        static_cast<unsigned long long>(compositor - g_base),
        static_cast<unsigned long long>(g_weatherNodeContainer));
    return true;
}

static bool ResolveTimeContext(uintptr_t& envMgr, long long& entity) {
    envMgr = 0;
    entity = 0;
    if (!g_envManagerGlobal) return false;
    if (!ReadPointer(reinterpret_cast<uintptr_t>(g_envManagerGlobal), envMgr)) return false;

    uintptr_t vt = 0, fn = 0;
    if (!ReadPointer(envMgr, vt) || !ReadPointer(vt + g_envGetEntityVt, fn) || !ExecutablePointer(fn)) return false;
    auto getEntity = reinterpret_cast<EnvGetEntityFn>(fn);
    CDK_GUARD_BEGIN
        entity = getEntity(reinterpret_cast<void*>(envMgr));
    CDK_GUARD_FAIL
        entity = 0;
    CDK_GUARD_END
    return entity != 0;
}

static bool CaptureTimeBaseline(long long entity) {
    float lo = NAN, hi = NAN;
    if (!ReadFloat(static_cast<uintptr_t>(entity) + g_timeLower, lo) ||
        !ReadFloat(static_cast<uintptr_t>(entity) + g_timeUpper, hi) ||
        hi <= lo + 0.0001f) {
        return false;
    }
    g_timeBaseLower = lo;
    g_timeBaseUpper = hi;
    g_timeDomainHours = hi > 1.5f && hi <= 48.0f;
    g_timeBaselineValid = true;
    Log("[environment] visual-time baseline %.4f..%.4f (%s)",
        lo, hi, g_timeDomainHours ? "hours" : "normalized");
    return true;
}

static float RawToHour(float raw) {
    return NormalizeHour(g_timeDomainHours ? raw : raw * 24.0f);
}
static float HourToRaw(float hour) {
    hour = NormalizeHour(hour);
    return g_timeDomainHours ? hour : hour / 24.0f;
}

static bool ReadCurrentHour(uintptr_t envMgr, long long entity, float& hour) {
    float a = NAN, b = NAN;
    const bool haveA = ReadFloat(static_cast<uintptr_t>(entity) + g_timeCurrentA, a);
    const bool haveB = ReadFloat(static_cast<uintptr_t>(entity) + g_timeCurrentB, b);
    const float pad = (g_timeBaseUpper - g_timeBaseLower) * 0.05f + 0.05f;
    const auto inRange = [&](float v) {
        return std::isfinite(v) && v >= g_timeBaseLower - pad && v <= g_timeBaseUpper + pad;
    };
    if (haveA && inRange(a)) { hour = RawToHour(a); return true; }
    if (haveB && inRange(b)) { hour = RawToHour(b); return true; }

    uintptr_t vt = 0, fn = 0;
    if (!ReadPointer(envMgr, vt) || !ReadPointer(vt + g_envGetTimeVt, fn) || !ExecutablePointer(fn)) return false;
    auto getTime = reinterpret_cast<EnvGetTimeFn>(fn);
    double raw = NAN;
    CDK_GUARD_BEGIN
        raw = getTime(reinterpret_cast<void*>(envMgr));
    CDK_GUARD_FAIL
        raw = NAN;
    CDK_GUARD_END
    if (!std::isfinite(raw)) return false;
    hour = RawToHour(static_cast<float>(raw));
    return true;
}

static bool SetTimeRaw(long long entity, float raw) {
    uintptr_t vt = 0, fn = 0;
    if (!ReadPointer(static_cast<uintptr_t>(entity), vt) || !ReadPointer(vt + g_entitySetTimeVt, fn) || !ExecutablePointer(fn)) return false;
    auto setTime = reinterpret_cast<EntitySetTimeFn>(fn);
    bool ok = true;
    CDK_GUARD_BEGIN
        setTime(entity, raw);
    CDK_GUARD_FAIL
        ok = false;
    CDK_GUARD_END
    return ok;
}

static void RestoreTimeLimits(long long entity) {
    if (!g_timeBaselineValid || !entity) return;
    WriteBytes(static_cast<uintptr_t>(entity) + g_timeLower, &g_timeBaseLower, sizeof(float));
    WriteBytes(static_cast<uintptr_t>(entity) + g_timeUpper, &g_timeBaseUpper, sizeof(float));
}

static long long __fastcall HookWeatherCompose(long long weatherState, float dt) {
    if (!g_origWeatherCompose) return 0;
    const long long result = g_origWeatherCompose(weatherState, dt);

    const bool clear = g_weatherClear.load(std::memory_order_relaxed);
    const bool rainOn = g_rainOverride.load(std::memory_order_relaxed);
    const bool snowOn = g_snowOverride.load(std::memory_order_relaxed);
    const bool cloudOn = g_cloudOverride.load(std::memory_order_relaxed);
    const bool windOn = g_windOverride.load(std::memory_order_relaxed);
    if (!clear && !rainOn && !snowOn && !cloudOn && !windOn) return result;

    uintptr_t parent = 0, child = 0, atmosphere = 0;
    if (!ReadPointer(static_cast<uintptr_t>(weatherState) + g_weatherNodeContainer, parent) ||
        !ReadPointer(parent + 0x18, child)) {
        return result;
    }
    ReadPointer(parent + 0x20, atmosphere); // atmosphere is optional for rain/snow/wind

    if (clear) {
        const float zero = 0.0f;
        WriteBytes(child + kWeatherRain, &zero, sizeof(zero));
        WriteBytes(child + kWeatherSnow, &zero, sizeof(zero));
        if (atmosphere) {
            WriteBytes(atmosphere + kAtmosphereCloudAmount, &zero, sizeof(zero));
            WriteBytes(atmosphere + kAtmosphereMieAerosolDensity, &zero, sizeof(zero));
            WriteBytes(atmosphere + kAtmosphereNativeFogSecondary, &zero, sizeof(zero));
        }
    } else {
        if (rainOn) {
            const float v = Clamp(g_rainValue.load(std::memory_order_relaxed), 0.0f, 1.0f);
            WriteBytes(child + kWeatherRain, &v, sizeof(v));
        }
        if (snowOn) {
            const float v = Clamp(g_snowValue.load(std::memory_order_relaxed), 0.0f, 1.0f);
            WriteBytes(child + kWeatherSnow, &v, sizeof(v));
        }
        if (cloudOn && atmosphere) {
            const float v = Clamp(g_cloudValue.load(std::memory_order_relaxed), 0.0f, 3.0f);
            WriteBytes(atmosphere + kAtmosphereCloudAmount, &v, sizeof(v));
        }
    }

    if (windOn) {
        const float mul = Clamp(g_windMultiplier.load(std::memory_order_relaxed), 0.0f, 3.0f);
        for (ptrdiff_t off : { kWeatherWindSpeed, kWeatherAltitudeWindRatio, kWeatherWindBlendContribution }) {
            float native = 0.0f;
            if (!ReadFloat(child + off, native)) continue;
            const float v = native * mul;
            if (std::isfinite(v)) WriteBytes(child + off, &v, sizeof(v));
        }
    }
    return result;
}

static bool WeatherParticleContext(long long& particleMgr, int& nullSentinel) {
    particleMgr = 0;
    nullSentinel = 0;
    uintptr_t envMgr = 0;
    long long entity = 0;
    if (!ResolveTimeContext(envMgr, entity) || !entity || !g_nullSentinel) return false;
    uintptr_t p = 0;
    if (!ReadPointer(static_cast<uintptr_t>(entity) + 0xEE8, p))
        ReadPointer(static_cast<uintptr_t>(entity) + 0xEE0, p);
    if (!p || !ReadBytes(reinterpret_cast<uintptr_t>(g_nullSentinel), &nullSentinel, sizeof(nullSentinel))) return false;
    particleMgr = static_cast<long long>(p);
    return true;
}

static void SetEffectIntensity(long long self, int effect, int nullSentinel, long long particleMgr, float value, bool activate) {
    static constexpr int ids[9] = { 0,1,2,3,4,5,6,7,8 };
    static constexpr ptrdiff_t slotA[9] = { 0x18,0x28,0x48,0x58,0x68,0x88,0x98,0xA8,0xB8 };
    static constexpr ptrdiff_t slotB[9] = { 0x20,0x30,0x50,0x60,0x70,0x90,0xA0,0xB0,0xC0 };
    if (effect < 0 || effect >= 9 || !g_setIntensity) return;
    int handle = nullSentinel;
    if (!ReadBytes(static_cast<uintptr_t>(self) + 0xFC + effect * 4, &handle, sizeof(handle))) return;
    if (activate && value > 0.001f && handle == nullSentinel && g_activateEffect) {
        bool ok = true;
        CDK_GUARD_BEGIN
            g_activateEffect(self, ids[effect], reinterpret_cast<long long*>(self + slotA[effect]),
                             reinterpret_cast<long long*>(self + slotB[effect]), 1.0f);
        CDK_GUARD_FAIL
            ok = false;
        CDK_GUARD_END
        if (!ok || !ReadBytes(static_cast<uintptr_t>(self) + 0xFC + effect * 4, &handle, sizeof(handle))) return;
    }
    if (handle == nullSentinel) return;
    CDK_GUARD_BEGIN
        g_setIntensity(particleMgr, handle, Clamp(value, 0.0f, 1.0f));
    CDK_GUARD_FAIL
    CDK_GUARD_END
}

static void __fastcall HookWeatherTick(long long self, float dt) {
    if (!g_origWeatherTick) return;
    g_origWeatherTick(self, dt);

    const bool clear = g_weatherClear.load(std::memory_order_relaxed);
    const bool snowOn = g_snowOverride.load(std::memory_order_relaxed);
    if (g_snowWasOverridden && !snowOn) g_snowCleanupTicks = 30;
    g_snowWasOverridden = snowOn;
    if (!clear && !snowOn && g_snowCleanupTicks <= 0) return;

    long long particleMgr = 0;
    int nullSentinel = 0;
    if (!WeatherParticleContext(particleMgr, nullSentinel)) return;

    if (clear) {
        // Rain is normally driven by the finalized table, but explicitly zeroing
        // active handles makes Clear Sky immediate. Snow needs the same cleanup.
        for (int effect : { 0, 1, 2, 3, 4 })
            SetEffectIntensity(self, effect, nullSentinel, particleMgr, 0.0f, false);
        return;
    }

    if (!snowOn) {
        SetEffectIntensity(self, 2, nullSentinel, particleMgr, 0.0f, false);
        SetEffectIntensity(self, 3, nullSentinel, particleMgr, 0.0f, false);
        if (g_snowCleanupTicks > 0) --g_snowCleanupTicks;
        return;
    }

    const float snow = Clamp(g_snowValue.load(std::memory_order_relaxed), 0.0f, 1.0f);
    SetEffectIntensity(self, 2, nullSentinel, particleMgr, snow, snow > 0.01f);
    SetEffectIntensity(self, 3, nullSentinel, particleMgr, snow, snow > 0.30f);
}

} // namespace

void EnvironmentInstall() {
    const bool env = ResolveEnvManagerGlobal();
    const bool timeLayout = env && ResolveTimeLayout();
    g_timeAvailable.store(timeLayout, std::memory_order_relaxed);
    if (timeLayout) Log("[environment] visual-time controls ready");
    else Log("[environment] visual-time controls unavailable (optional)");

    g_weatherAvailable.store(ResolveWeatherLayout(), std::memory_order_relaxed);
    if (g_weatherAvailable.load(std::memory_order_relaxed))
        g_snowEffectsAvailable.store(ResolveWeatherEffects(), std::memory_order_relaxed);
    if (!g_weatherAvailable.load(std::memory_order_relaxed))
        Log("[environment] weather controls unavailable (optional)");
}

void EnvironmentTick() {
    if (!g_timeAvailable.load(std::memory_order_relaxed)) return;

    uintptr_t envMgr = 0;
    long long entity = 0;
    if (!ResolveTimeContext(envMgr, entity)) {
        g_timeCurrentValid.store(false, std::memory_order_relaxed);
        return;
    }

    if (entity != g_timeEntity) {
        g_timeEntity = entity;
        g_timeBaselineValid = false;
        g_timeFreezeApplied = false;
    }
    if (!g_timeBaselineValid && !CaptureTimeBaseline(entity)) {
        g_timeCurrentValid.store(false, std::memory_order_relaxed);
        return;
    }

    float current = 0.0f;
    if (ReadCurrentHour(envMgr, entity, current)) {
        g_timeCurrentHour.store(current, std::memory_order_relaxed);
        g_timeCurrentValid.store(true, std::memory_order_relaxed);
        if (!g_timeFreeze.load(std::memory_order_relaxed) && g_timeSetHoldTicks.load(std::memory_order_relaxed) <= 0)
            g_timeTargetHour.store(current, std::memory_order_relaxed);
    } else {
        g_timeCurrentValid.store(false, std::memory_order_relaxed);
    }

    const bool frozen = g_timeFreeze.load(std::memory_order_relaxed);
    if (!frozen) {
        if (g_timeFreezeApplied) {
            RestoreTimeLimits(entity);
            g_timeFreezeApplied = false;
        }
        int hold = g_timeSetHoldTicks.load(std::memory_order_relaxed);
        if (hold > 0) {
            const float raw = HourToRaw(g_timeTargetHour.load(std::memory_order_relaxed));
            SetTimeRaw(entity, raw);
            g_timeSetHoldTicks.store(hold - 1, std::memory_order_relaxed);
        }
        g_timeApply.store(false, std::memory_order_relaxed);
        return;
    }

    const float raw = HourToRaw(g_timeTargetHour.load(std::memory_order_relaxed));
    if (!g_timeFreezeApplied || g_timeApply.exchange(false, std::memory_order_relaxed))
        SetTimeRaw(entity, raw);
    WriteBytes(static_cast<uintptr_t>(entity) + g_timeLower, &raw, sizeof(raw));
    WriteBytes(static_cast<uintptr_t>(entity) + g_timeUpper, &raw, sizeof(raw));
    g_timeFreezeApplied = true;
    g_timeSetHoldTicks.store(0, std::memory_order_relaxed);
}

bool TimeControlAvailable() { return g_timeAvailable.load(std::memory_order_relaxed); }
bool TimeHour(float* hour) {
    if (!hour || !g_timeCurrentValid.load(std::memory_order_relaxed)) return false;
    *hour = g_timeCurrentHour.load(std::memory_order_relaxed);
    return true;
}
float TimeTargetHour() { return NormalizeHour(g_timeTargetHour.load(std::memory_order_relaxed)); }
bool TimeFrozen() { return g_timeFreeze.load(std::memory_order_relaxed); }
void SetTimeHour(float hour) {
    g_timeTargetHour.store(NormalizeHour(hour), std::memory_order_relaxed);
    g_timeApply.store(true, std::memory_order_relaxed);
    if (!g_timeFreeze.load(std::memory_order_relaxed))
        g_timeSetHoldTicks.store(8, std::memory_order_relaxed);
}
void SetTimeFrozen(bool frozen) {
    if (frozen && !g_timeFreeze.load(std::memory_order_relaxed) &&
        g_timeSetHoldTicks.load(std::memory_order_relaxed) <= 0 &&
        g_timeCurrentValid.load(std::memory_order_relaxed)) {
        g_timeTargetHour.store(g_timeCurrentHour.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    g_timeFreeze.store(frozen, std::memory_order_relaxed);
    g_timeApply.store(frozen, std::memory_order_relaxed);
    if (!frozen) g_timeSetHoldTicks.store(0, std::memory_order_relaxed);
}
void ResetTimeControl() {
    g_timeFreeze.store(false, std::memory_order_relaxed);
    g_timeApply.store(false, std::memory_order_relaxed);
    g_timeSetHoldTicks.store(0, std::memory_order_relaxed);
}

bool WeatherControlAvailable() { return g_weatherAvailable.load(std::memory_order_relaxed); }
bool WeatherSnowEffectsAvailable() { return g_snowEffectsAvailable.load(std::memory_order_relaxed); }
bool WeatherClearSky() { return g_weatherClear.load(std::memory_order_relaxed); }
void SetWeatherClearSky(bool enabled) { g_weatherClear.store(enabled, std::memory_order_relaxed); }
bool WeatherRainOverride(float* value) {
    if (value) *value = g_rainValue.load(std::memory_order_relaxed);
    return g_rainOverride.load(std::memory_order_relaxed);
}
bool WeatherSnowOverride(float* value) {
    if (value) *value = g_snowValue.load(std::memory_order_relaxed);
    return g_snowOverride.load(std::memory_order_relaxed);
}
bool WeatherCloudOverride(float* value) {
    if (value) *value = g_cloudValue.load(std::memory_order_relaxed);
    return g_cloudOverride.load(std::memory_order_relaxed);
}
bool WeatherWindOverride(float* multiplier) {
    if (multiplier) *multiplier = g_windMultiplier.load(std::memory_order_relaxed);
    return g_windOverride.load(std::memory_order_relaxed);
}
void SetWeatherRainOverride(bool enabled, float value) {
    g_rainValue.store(Clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
    g_rainOverride.store(enabled, std::memory_order_relaxed);
}
void SetWeatherSnowOverride(bool enabled, float value) {
    g_snowValue.store(Clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
    g_snowOverride.store(enabled, std::memory_order_relaxed);
}
void SetWeatherCloudOverride(bool enabled, float value) {
    g_cloudValue.store(Clamp(value, 0.0f, 3.0f), std::memory_order_relaxed);
    g_cloudOverride.store(enabled, std::memory_order_relaxed);
}
void SetWeatherWindOverride(bool enabled, float multiplier) {
    g_windMultiplier.store(Clamp(multiplier, 0.0f, 3.0f), std::memory_order_relaxed);
    g_windOverride.store(enabled, std::memory_order_relaxed);
}
void ResetWeatherControl() {
    g_weatherClear.store(false, std::memory_order_relaxed);
    g_rainOverride.store(false, std::memory_order_relaxed);
    g_snowOverride.store(false, std::memory_order_relaxed);
    g_cloudOverride.store(false, std::memory_order_relaxed);
    g_windOverride.store(false, std::memory_order_relaxed);
}

} // namespace core
