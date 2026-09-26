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
//
// Every offset, vtable slot and helper used below is derived at startup from the game's own code (the weather tick, the
// weather-node getters it calls, the time-limit console setters, the time-advance virtual, the weather summary reader).
// Each control is enabled only when everything it writes through was derived; otherwise it stays off with a log line.

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

using WeatherComposeFn = long long(__fastcall*)(long long weatherState, float dt);
using WeatherTickFn = void(__fastcall*)(long long self, float dt);
using ActivateEffectFn = void(__fastcall*)(long long self, int id, long long* slotA, long long* slotB, float v);
using SetIntensityFn = void(__fastcall*)(long long particleMgr, int handle, float v);
using EnvGetEntityFn = long long(__fastcall*)(void* envMgr);
using EntitySetTimeFn = void(__fastcall*)(long long entity, float value, float epsilon);

// --- resolved at install; -1 / 0 = not derived, and every consumer checks that before touching game memory ---
static uintptr_t g_tick = 0, g_tickEnd = 0;          // weather tick and the end of its (chained) unwind extent
static uintptr_t g_rainGetter = 0, g_snowGetter = 0, g_windGetter = 0;
static uintptr_t g_intensityFn = 0, g_activateFn = 0;
static uintptr_t* g_envManagerGlobal = nullptr;
static ptrdiff_t g_envGetEntityVt = -1;              // envMgr vtable slot returning the environment entity
static ptrdiff_t g_entityWeatherState = -1;          // entity field the tick passes to the weather getters
static ptrdiff_t g_entityParticleMgr = -1;           // entity field the tick passes to SetEffectIntensity
static ptrdiff_t g_weatherNodeContainer = -1;        // weatherState -> node container
static constexpr ptrdiff_t kWeatherChildSlot = 0x18; // fixed by the getter signatures (lea rax,[rdx+18h])
static ptrdiff_t g_weatherAtmosphereSlot = -1;       // container -> atmosphere node

enum WeatherField { FRain, FSnow, FWindSpeed, FWindAltitude, FWindBlend, FCloud, FMie, FFog, FCount };
static const char* const kFieldName[FCount] = { "rain", "snow", "wind speed", "altitude wind", "wind blend", "cloud", "mie aerosol", "fog" };
static ptrdiff_t g_fieldOff[FCount] = { -1, -1, -1, -1, -1, -1, -1, -1 };
static bool FieldIsAtmosphere(int f) { return f == FCloud || f == FMie || f == FFog; }

// weather effect object (the tick's `this`): activation slot pairs and handle array, all read off the tick's own call sites
static ptrdiff_t g_effectSlotA[9] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static ptrdiff_t g_effectSlotB[9] = { -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static ptrdiff_t g_effectHandleBase = -1;
static unsigned g_effectHandleMask = 0;              // bit n: the tick itself reads handle n before SetEffectIntensity
static int* g_nullSentinel = nullptr;

// visual time
static ptrdiff_t g_timeLower = 0;
static ptrdiff_t g_timeUpper = 0;
static ptrdiff_t g_timeCurrent = 0;
static uintptr_t g_timeAddFn = 0;                    // entity virtual "advance time": add to current, tail-call set-time
static ptrdiff_t g_entitySetTimeVt = -1;
static float g_timeSetEpsilon = 0.0f;

static std::atomic<bool> g_timeAvailable{ false };
static std::atomic<bool> g_timeCurrentValid{ false };
static std::atomic<float> g_timeCurrentHour{ 12.0f };
static std::atomic<float> g_timeTargetHour{ 12.0f };  // written by the UI thread only, so the game thread cannot race a request
static std::atomic<bool> g_timeFreeze{ false };
static std::atomic<bool> g_timeApply{ false };
static std::atomic<int> g_timeSetHoldTicks{ 0 };
static long long g_timeEntity = 0;
static bool g_timeEntityOk = false;
static bool g_timeDomainHours = true;
static bool g_timeOverriding = false;                 // our frozen limits are in the entity
static float g_timeNativeLower = 0.0f;                // what the game had right before we froze (or what a script set since)
static float g_timeNativeUpper = 24.0f;
static float g_timeWritten = 0.0f;                    // the value we last wrote into both limits

// weather
static WeatherComposeFn g_origWeatherCompose = nullptr;
static WeatherTickFn g_origWeatherTick = nullptr;
static std::atomic<bool> g_snowEffectsAvailable{ false };
static std::atomic<bool> g_weatherAvailable{ false };
static std::atomic<bool> g_rainAvailable{ false };
static std::atomic<bool> g_snowTableAvailable{ false };
static std::atomic<bool> g_windAvailable{ false };
static std::atomic<bool> g_cloudAvailable{ false };
static std::atomic<bool> g_weatherClear{ false };
static std::atomic<bool> g_rainOverride{ false };
static std::atomic<float> g_rainValue{ 0.0f };
static std::atomic<bool> g_snowOverride{ false };
static std::atomic<float> g_snowValue{ 0.0f };
static std::atomic<bool> g_cloudOverride{ false };
static std::atomic<float> g_cloudValue{ 1.0f };
static std::atomic<bool> g_windOverride{ false };
static std::atomic<float> g_windMultiplier{ 1.0f };

// per-field override state, touched only inside the compose hook (one game thread)
struct FieldState { uintptr_t base; float native; float written; bool active; };
static FieldState g_field[FCount] = {};

static long long __fastcall HookWeatherCompose(long long weatherState, float dt);
static void __fastcall HookWeatherTick(long long self, float dt);

static float Clamp(float v, float lo, float hi) {
    return std::max(lo, std::min(hi, v));
}

static const RUNTIME_FUNCTION* PdataTable(size_t& count) {
    count = 0;
    if (!g_base) return nullptr;
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(g_base);
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(g_base + dos->e_lfanew);
    auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
    count = dir.Size / sizeof(RUNTIME_FUNCTION);
    return reinterpret_cast<const RUNTIME_FUNCTION*>(g_base + dir.VirtualAddress);
}

// Root function start of one pdata entry, following UNW_FLAG_CHAININFO (MSVC splits big functions into chained fragments).
static uintptr_t RootOfEntry(RUNTIME_FUNCTION fn) {
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

static size_t PdataIndexOf(uintptr_t address, const RUNTIME_FUNCTION* table, size_t count) {
    const uintptr_t rva = address - g_base;
    size_t lo = 0, hi = count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (table[mid].BeginAddress <= rva) lo = mid + 1; else hi = mid;
    }
    if (!lo || !(table[lo - 1].BeginAddress <= rva && rva < table[lo - 1].EndAddress)) return SIZE_MAX;
    return lo - 1;
}

static uintptr_t FunctionStartOf(uintptr_t address) {
    size_t count = 0;
    const RUNTIME_FUNCTION* table = PdataTable(count);
    if (!table || address < g_base) return 0;
    const size_t i = PdataIndexOf(address, table, count);
    return i == SIZE_MAX ? 0 : RootOfEntry(table[i]);
}

// End of a function including every chained fragment that directly follows it: the bounded window for code scans.
static uintptr_t FunctionEndOf(uintptr_t start) {
    size_t count = 0;
    const RUNTIME_FUNCTION* table = PdataTable(count);
    if (!table || start < g_base) return 0;
    size_t i = PdataIndexOf(start, table, count);
    if (i == SIZE_MAX || g_base + table[i].BeginAddress != start) return 0;
    uintptr_t end = g_base + table[i].EndAddress;
    for (++i; i < count && RootOfEntry(table[i]) == start; ++i) end = g_base + table[i].EndAddress;
    return end;
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

static bool MatchAt(const uint8_t* at, const std::vector<int>& pat) {
    for (size_t j = 0; j < pat.size(); ++j)
        if (pat[j] >= 0 && at[j] != static_cast<uint8_t>(pat[j])) return false;
    return true;
}

// Pattern check at one address (a call target), through a guarded copy so a bad target cannot fault.
static bool MatchesAt(uintptr_t address, const char* pattern) {
    const std::vector<int> pat = ParsePattern(pattern);
    uint8_t buf[128] = {};
    if (pat.empty() || pat.size() > sizeof(buf) || !ReadBytes(address, buf, pat.size())) return false;
    return MatchAt(buf, pat);
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
        for (size_t k = 0; k + pat.size() <= size && hits.size() < cap; ++k)
            if (MatchAt(start + k, pat)) hits.push_back(reinterpret_cast<uintptr_t>(start + k));
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

static uintptr_t FirstUnique(const char* const* patterns, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        const auto hits = ScanAll(patterns[i], 3);
        if (hits.size() == 1) return hits[0];
    }
    return 0;
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

static int32_t Disp32(const uint8_t* p) { int32_t d = 0; memcpy(&d, p, sizeof(d)); return d; }

// ---------------------------------------------------------------------------------------------------------------------
// Resolution
// ---------------------------------------------------------------------------------------------------------------------

static uintptr_t ResolveWeatherTick() {
    static const char* patterns[] = {
        "40 53 48 81 EC C0 00 00 00 C5 F2 58 81 C8 00 00 00 C5 F8 29 B4 24 B0 00 00 00 C5 78 29 54 24 70",
        "48 8B C4 53 48 81 EC ?? 00 00 00 C5 F2 58 81 C8 00 00 00",
        "48 8B C4 53 48 81 EC B0 00 00 00 80 3D",
    };
    const uintptr_t tick = FirstUnique(patterns, sizeof(patterns) / sizeof(patterns[0]));
    if (!tick) { Log("[environment] weather tick not uniquely resolved"); return 0; }
    const uintptr_t end = FunctionEndOf(tick);
    if (!end || end <= tick || end - tick > 0x4000) {
        Log("[environment] weather tick extent not resolved (rva 0x%llx)", static_cast<unsigned long long>(tick - g_base));
        return 0;
    }
    g_tickEnd = end;
    return tick;
}

// Node getter shape shared by the rain and snow getters: weatherState->container->child, field at +34.
static const char* kNodeGetterShape =
    "48 8B 51 ?? 4C 8B D1 48 85 D2 B9 40 00 00 00 48 8D 42 18 48 0F 44 C1 41 80 7A 31 00 4C 8B 08 4D 8D 81 ?? ?? 00 00";
// Wind getter: blend at +41, speed at +52, their null-node defaults (field+0x28) at +24 / +15.
static const char* kWindGetterShape =
    "48 8B 41 ?? 41 B8 40 00 00 00 48 85 C0 41 B9 ?? ?? 00 00 48 8D 50 18 B8 ?? ?? 00 00 49 0F 44 D0 4C 8B 02 4D 85 C0 "
    "49 8D 90 ?? ?? 00 00 48 0F 44 D0 49 8D 80 ?? ?? 00 00 49 0F 44 C1";

static bool ResolveRainGetter() {
    // Only the two exact variants: the old third pattern (7 bytes) was generic enough to hit unrelated code after a patch.
    static const char* rainPatterns[] = {
        "48 8B 51 60 4C 8B D1 48 85 D2 B9 40 00 00 00 48 8D 42 18 48 0F 44 C1 41 80 7A 31 00 4C 8B 08 4D 8D 81 6C 01 00 00",
        "48 8B 51 58 4C 8B D1 48 85 D2 B9 40 00 00 00 48 8D 42 18 48 0F 44 C1 41 80 7A 31 00 4C 8B 08 4D 8D 81 6C 01 00 00",
    };
    const uintptr_t getter = FirstUnique(rainPatterns, sizeof(rainPatterns) / sizeof(rainPatterns[0]));
    if (!getter) { Log("[environment] rain getter not uniquely resolved"); return false; }
    uint8_t code[38] = {};
    if (!ReadBytes(getter, code, sizeof(code))) return false;
    const uint8_t container = code[3];
    if (container < 0x40 || container > 0x80 || (container & 7)) {
        Log("[environment] weather container offset rejected: 0x%02x", container);
        return false;
    }
    g_rainGetter = getter;
    g_weatherNodeContainer = container;
    g_fieldOff[FRain] = Disp32(code + 34);
    return true;
}

// Parses the node getter at `fn` and returns its field, or -1 when the shape / container does not match.
static ptrdiff_t NodeGetterField(uintptr_t fn) {
    if (!MatchesAt(fn, kNodeGetterShape)) return -1;
    uint8_t code[38] = {};
    if (!ReadBytes(fn, code, sizeof(code)) || code[3] != g_weatherNodeContainer) return -1;
    const int32_t field = Disp32(code + 34);
    return field > 0 && field < 0x400 ? field : -1;
}

// Wind getter: speed and blend from its main path; altitude ratio from the second path after its first ret.
static bool ParseWindGetter(uintptr_t fn, ptrdiff_t& speed, ptrdiff_t& blend, ptrdiff_t& altitude) {
    speed = blend = altitude = -1;
    if (!MatchesAt(fn, kWindGetterShape)) return false;
    uint8_t code[0x90] = {};
    if (!ReadBytes(fn, code, sizeof(code)) || code[3] != g_weatherNodeContainer) return false;
    const int32_t b = Disp32(code + 41), s = Disp32(code + 52);
    if (Disp32(code + 24) != b + 0x28 || Disp32(code + 15) != s + 0x28) return false;
    size_t ret = 0;
    for (size_t i = 60; i < sizeof(code); ++i) if (code[i] == 0xC3) { ret = i; break; }
    for (size_t i = ret ? ret + 1 : sizeof(code); i + 12 <= sizeof(code); ++i) {
        // lea rax,[r8+disp32] ; mov edx,disp+0x28 (null-node default)
        if (code[i] == 0x49 && code[i + 1] == 0x8D && code[i + 2] == 0x80 && code[i + 7] == 0xBA &&
            Disp32(code + i + 8) == Disp32(code + i + 3) + 0x28) {
            altitude = Disp32(code + i + 3);
            break;
        }
    }
    speed = s; blend = b;
    return true;
}

// Activation helper validation: it compares handle[id] against the shared null sentinel; this also yields both.
static bool ParseActivate(uintptr_t fn, int*& sentinel, ptrdiff_t& handleBase) {
    uint8_t code[0x80] = {};
    if (!ReadBytes(fn, code, sizeof(code))) return false;
    for (size_t i = 0; i + 13 <= sizeof(code); ++i) {
        // mov eax,[rip+x] ; cmp [rcx+rbx*4+disp32],eax
        if (code[i] != 0x8B || code[i + 1] != 0x05 ||
            code[i + 6] != 0x39 || code[i + 7] != 0x84 || code[i + 8] != 0x99) continue;
        const int32_t base = Disp32(code + i + 9);
        if (base < 0x40 || base > 0x400 || (base & 3)) return false;
        const uintptr_t at = fn + i + 6 + static_cast<intptr_t>(Disp32(code + i + 2));
        int value = 0;
        if (!ReadBytes(at, &value, sizeof(value))) return false;
        sentinel = reinterpret_cast<int*>(at);
        handleBase = base;
        return true;
    }
    return false;
}

// Reads the effect id and slot pair the tick loads right before calling the activation helper at `call`.
static bool ParseActivateSite(const uint8_t* code, size_t call, int& id, ptrdiff_t& slotA, ptrdiff_t& slotB) {
    id = -1; slotA = slotB = -1;
    const size_t from = call > 0x28 ? call - 0x28 : 0;
    for (size_t i = from; i < call; ++i) {
        const uint8_t* p = code + i;
        if (i + 4 <= call && p[0] == 0x4C && p[1] == 0x8D && p[2] == 0x4B) slotB = p[3];              // lea r9,[rbx+ib]
        if (i + 7 <= call && p[0] == 0x4C && p[1] == 0x8D && p[2] == 0x8B) slotB = Disp32(p + 3);      // lea r9,[rbx+id]
        if (i + 4 <= call && p[0] == 0x4C && p[1] == 0x8D && p[2] == 0x43) slotA = p[3];              // lea r8,[rbx+ib]
        if (i + 7 <= call && p[0] == 0x4C && p[1] == 0x8D && p[2] == 0x83) slotA = Disp32(p + 3);      // lea r8,[rbx+id]
        if (i + 2 <= call && p[0] == 0x33 && p[1] == 0xD2) id = 0;                                     // xor edx,edx
        if (i + 5 <= call && p[0] == 0xBA) { const int32_t v = Disp32(p + 1); if (v >= 0 && v < 9) id = v; } // mov edx,imm
    }
    return id >= 0 && slotA > 0 && slotB == slotA + 8 && slotA < 0x400;
}

// One bounded pass over the weather tick: every relationship is taken from a call whose target was resolved separately.
static void ScanWeatherTick() {
    const size_t size = g_tickEnd - g_tick;
    std::vector<uint8_t> code(size);
    if (!ReadBytes(g_tick, code.data(), size)) return;

    // 1) env global, get-entity slot and weather-state field: mov rcx,[rip+g]; mov rax,[rcx]; call [rax+s]; mov rcx,[rax+f]; call rain
    for (size_t k = 20; k + 5 <= size; ++k) {
        if (code[k] != 0xE8 || g_tick + k + 5 + static_cast<intptr_t>(Disp32(&code[k + 1])) != g_rainGetter) continue;
        const uint8_t* p = &code[k - 20];
        if (!(p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x0D && p[7] == 0x48 && p[8] == 0x8B && p[9] == 0x01 &&
              p[10] == 0xFF && p[11] == 0x50 && p[13] == 0x48 && p[14] == 0x8B && p[15] == 0x88)) continue;
        const uintptr_t global = g_tick + (k - 20) + 7 + static_cast<intptr_t>(Disp32(p + 3));
        const ptrdiff_t slot = p[12], state = Disp32(p + 16);
        if (g_envManagerGlobal && (reinterpret_cast<uintptr_t>(g_envManagerGlobal) != global ||
                                   slot != g_envGetEntityVt || state != g_entityWeatherState)) {
            Log("[environment] weather tick loads the environment inconsistently; environment manager rejected");
            g_envManagerGlobal = nullptr; g_envGetEntityVt = -1; g_entityWeatherState = -1;
            break;
        }
        uintptr_t probe = 0;
        if (!ReadBytes(global, &probe, sizeof(probe))) break;
        g_envManagerGlobal = reinterpret_cast<uintptr_t*>(global);
        g_envGetEntityVt = slot;
        g_entityWeatherState = state;
    }

    // 2) particle manager field: mov rcx,[rax+f]; call SetEffectIntensity (the signature-resolved one); all sites must agree
    ptrdiff_t particle = -1;
    bool particleOk = g_intensityFn != 0;
    for (size_t k = 7; particleOk && k + 5 <= size; ++k) {
        if (code[k] != 0xE8 || g_tick + k + 5 + static_cast<intptr_t>(Disp32(&code[k + 1])) != g_intensityFn) continue;
        const uint8_t* p = &code[k - 7];
        if (!(p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x88)) { particleOk = false; break; }
        const ptrdiff_t f = Disp32(p + 3);
        if (particle >= 0 && particle != f) particleOk = false;
        particle = f;
        // the handle the tick passes: mov edx,[rbx+disp32] shortly before; records which handles the layout covers
        for (size_t i = k > 0x20 ? k - 0x20 : 0; i + 6 <= k; ++i) {
            if (code[i] != 0x8B || code[i + 1] != 0x93) continue;
            const int32_t h = Disp32(&code[i + 2]);
            if (g_effectHandleBase >= 0 && h >= g_effectHandleBase && ((h - g_effectHandleBase) & 3) == 0 &&
                (h - g_effectHandleBase) / 4 < 9)
                g_effectHandleMask |= 1u << ((h - g_effectHandleBase) / 4);
        }
    }
    if (particleOk && particle > 0) g_entityParticleMgr = particle;

    // 3) weather getters called on [entity+weatherState]: exactly one other node getter (snow) and one wind getter
    if (g_entityWeatherState > 0) {
        uintptr_t snow = 0, wind = 0;
        bool snowAmbiguous = false, windAmbiguous = false;
        size_t snowSite = 0;
        for (size_t k = 7; k + 5 <= size; ++k) {
            if (code[k] != 0xE8) continue;
            const uint8_t* p = &code[k - 7];
            if (!(p[0] == 0x48 && p[1] == 0x8B && p[2] == 0x88 && Disp32(p + 3) == g_entityWeatherState)) continue;
            const uintptr_t target = g_tick + k + 5 + static_cast<intptr_t>(Disp32(&code[k + 1]));
            if (target == g_rainGetter) continue;
            const ptrdiff_t f = NodeGetterField(target);
            if (f >= 0 && f != g_fieldOff[FRain]) {
                if (snow && snow != target) snowAmbiguous = true;
                if (!snow) snowSite = k;
                snow = target;
                continue;
            }
            ptrdiff_t s, b, a;
            if (ParseWindGetter(target, s, b, a)) {
                if (wind && wind != target) windAmbiguous = true;
                wind = target;
            }
        }
        // snow semantics: the tick starts effect 2 (the snow effect) right after reading this getter
        bool snowFeedsEffect2 = false;
        if (snow && !snowAmbiguous && g_activateFn) {
            for (size_t k = snowSite + 5; k + 5 <= size && k < snowSite + 0x80; ++k) {
                if (code[k] != 0xE8 || g_tick + k + 5 + static_cast<intptr_t>(Disp32(&code[k + 1])) != g_activateFn) continue;
                int id; ptrdiff_t a, b;
                snowFeedsEffect2 = ParseActivateSite(code.data(), k, id, a, b) && id == 2;
                break;
            }
        }
        if (snow && !snowAmbiguous && snowFeedsEffect2) { g_snowGetter = snow; g_fieldOff[FSnow] = NodeGetterField(snow); }
        else Log("[environment] snow getter not derived from the weather tick (found=%d ambiguous=%d effect2=%d)",
                 snow != 0, snowAmbiguous, snowFeedsEffect2);
        if (wind && !windAmbiguous) {
            ptrdiff_t s, b, a;
            ParseWindGetter(wind, s, b, a);
            g_windGetter = wind;
            g_fieldOff[FWindSpeed] = s;
            g_fieldOff[FWindBlend] = b;
            g_fieldOff[FWindAltitude] = a;  // -1 when the altitude path was not found: that field is then left alone
        } else {
            Log("[environment] wind getter not derived from the weather tick (found=%d ambiguous=%d)", wind != 0, windAmbiguous);
        }
    }
}

// Activation helper: the call target in the tick that has the handle/sentinel compare, then its slot pairs per effect id.
static void ResolveEffectLayout() {
    const size_t size = g_tickEnd - g_tick;
    std::vector<uint8_t> code(size);
    if (!ReadBytes(g_tick, code.data(), size)) return;
    for (size_t k = 0; k + 5 <= size && !g_activateFn; ++k) {
        if (code[k] != 0xE8) continue;
        const uintptr_t target = g_tick + k + 5 + static_cast<intptr_t>(Disp32(&code[k + 1]));
        if (target == g_intensityFn || target == g_rainGetter || target < g_base) continue;
        int* sentinel = nullptr; ptrdiff_t base = -1;
        int id; ptrdiff_t a, b;
        if (!ParseActivateSite(code.data(), k, id, a, b) || !ParseActivate(target, sentinel, base)) continue;
        g_activateFn = target; g_nullSentinel = sentinel; g_effectHandleBase = base;
    }
    if (!g_activateFn) return;
    for (size_t k = 0; k + 5 <= size; ++k) {
        if (code[k] != 0xE8 || g_tick + k + 5 + static_cast<intptr_t>(Disp32(&code[k + 1])) != g_activateFn) continue;
        int id; ptrdiff_t a, b;
        if (!ParseActivateSite(code.data(), k, id, a, b)) continue;
        if (g_effectSlotA[id] >= 0 && (g_effectSlotA[id] != a || g_effectSlotB[id] != b)) {
            Log("[environment] effect %d has conflicting activation slots; effect layout rejected", id);
            for (int i = 0; i < 9; ++i) g_effectSlotA[i] = g_effectSlotB[i] = -1;
            g_activateFn = 0;
            return;
        }
        g_effectSlotA[id] = a; g_effectSlotB[id] = b;
    }
}

static bool ExtractTimeStoreOffset(uintptr_t functionLikeHit, ptrdiff_t& out) {
    uint8_t code[0x90] = {};
    if (!ReadBytes(functionLikeHit, code, sizeof(code))) return false;
    for (size_t i = 0; i + 8 <= sizeof(code); ++i) {
        const bool sse = code[i] == 0xF3 && code[i + 1] == 0x0F && code[i + 2] == 0x11 && code[i + 3] == 0x8B;
        const bool vex = code[i] == 0xC5 && code[i + 1] == 0xFA && code[i + 2] == 0x11 && code[i + 3] == 0x8B;
        if (!sse && !vex) continue;
        const int32_t disp = Disp32(code + i + 4);
        if (disp >= 0x200 && disp <= 0x800) {
            out = static_cast<ptrdiff_t>(disp);
            return true;
        }
    }
    return false;
}

// get-entity slot the setter calls (mov rax,[rcx]; call [rax+ib]) must match the one the weather tick uses
static ptrdiff_t SetterGetEntitySlot(uintptr_t hit) {
    uint8_t code[0x30] = {};
    if (!ReadBytes(hit, code, sizeof(code))) return -1;
    for (size_t i = 0; i + 6 <= sizeof(code); ++i)
        if (code[i] == 0x48 && code[i + 1] == 0x8B && code[i + 2] == 0x01 && code[i + 3] == 0xFF && code[i + 4] == 0x50)
            return code[i + 5];
    return -1;
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
    if (SetterGetEntitySlot(lowHit) != g_envGetEntityVt || SetterGetEntitySlot(highHit) != g_envGetEntityVt) {
        Log("[environment] time-limit setters use get-entity slot 0x%llx/0x%llx, weather tick 0x%llx; time control disabled",
            static_cast<unsigned long long>(SetterGetEntitySlot(lowHit)), static_cast<unsigned long long>(SetterGetEntitySlot(highHit)),
            static_cast<unsigned long long>(g_envGetEntityVt));
        return false;
    }

    // Entity "advance time" virtual: mov rax,[rcx]; vaddss xmm1,xmm1,[rcx+current]; vmovss xmm2,[rip+eps]; jmp [rax+setTime].
    // It names both the current-time field and the set-time slot with its (entity, value, epsilon) ABI; no other source
    // for those exists on build 2976 (the old defaults 0x130/0x178 point at unrelated virtuals there).
    const uintptr_t add = ScanUnique(
        "48 8B 01 C5 F2 58 89 ?? ?? 00 00 C5 FA 10 15 ?? ?? ?? ?? 48 FF A0 ?? ?? 00 00", "time advance virtual");
    uint8_t code[26] = {};
    if (!add || !ReadBytes(add, code, sizeof(code))) {
        Log("[environment] time advance virtual not resolved; time control disabled");
        return false;
    }
    const ptrdiff_t current = Disp32(code + 7);
    const ptrdiff_t setSlot = Disp32(code + 22);
    float eps = NAN;
    const uintptr_t epsAt = add + 19 + static_cast<intptr_t>(Disp32(code + 15));
    if (current != lower - 4 || setSlot <= 0 || setSlot > 0x800 || (setSlot & 7) ||
        !ReadFloat(epsAt, eps) || eps <= 0.0f || eps >= 1.0f) {
        Log("[environment] time advance virtual rejected (current=0x%llx lower=0x%llx slot=0x%llx eps=%g); time control disabled",
            static_cast<unsigned long long>(current), static_cast<unsigned long long>(lower),
            static_cast<unsigned long long>(setSlot), eps);
        return false;
    }

    g_timeLower = lower;
    g_timeUpper = upper;
    g_timeCurrent = current;
    g_timeAddFn = add;
    g_entitySetTimeVt = setSlot;
    g_timeSetEpsilon = eps;
    Log("[environment] visual-time fields lower=0x%llx upper=0x%llx current=0x%llx, set-time vt 0x%llx (eps %g)",
        static_cast<unsigned long long>(g_timeLower), static_cast<unsigned long long>(g_timeUpper),
        static_cast<unsigned long long>(g_timeCurrent), static_cast<unsigned long long>(g_entitySetTimeVt), eps);
    return true;
}

// Atmosphere fields from the weather summary reader: it reads container->atmosphere, calls the rain and snow getters,
// then reads cloud (plain), mie aerosol (min/scaled) and the fog term (max with it), each with its null-node default.
static void ResolveAtmosphere() {
    const uintptr_t hit = ScanUnique(
        "48 8B 41 ?? 48 8D 50 ?? 41 B8 ?? 00 00 00 48 85 C0 49 0F 44 D0 48 8B 1A E8 ?? ?? ?? ?? C5 FA 11 45 ?? 49 8B 4B ?? "
        "E8 ?? ?? ?? ?? C5 FA 11 45 ?? 49 8B 4B ?? E8 ?? ?? ?? ?? C5 FA 11 45 ?? 48 8D 83 ?? ?? 00 00 B9 ?? ?? 00 00 48 85 DB "
        "48 0F 44 C1 C5 FA 10 00 C5 FA 11 45 ?? 48 8D 83 ?? ?? 00 00 B9 ?? ?? 00 00 48 0F 44 C1 C5 FA 10 00 C5 FA 5D 0D ?? ?? ?? ?? "
        "C5 F2 59 15 ?? ?? ?? ?? 48 8D 83 ?? ?? 00 00 B9 ?? ?? 00 00 48 0F 44 C1 C5 FA 10 00 C5 FA 5F CA",
        "weather summary reader");
    uint8_t code[150] = {};
    if (!hit || !ReadBytes(hit, code, sizeof(code))) { Log("[environment] atmosphere fields not derived; cloud control disabled"); return; }
    const uintptr_t call1 = hit + 24 + 5 + static_cast<intptr_t>(Disp32(code + 25));
    const uintptr_t call2 = hit + 38 + 5 + static_cast<intptr_t>(Disp32(code + 39));
    const ptrdiff_t slot = code[7];
    const int32_t cloud = Disp32(code + 65), mie = Disp32(code + 93), fog = Disp32(code + 129);
    const bool ok = code[3] == g_weatherNodeContainer && Disp32(code + 10) == slot + 0x28 &&
                    call1 == g_rainGetter && (!g_snowGetter || call2 == g_snowGetter) &&
                    Disp32(code + 70) == cloud + 0x28 && Disp32(code + 98) == mie + 0x28 && Disp32(code + 134) == fog + 0x28;
    if (!ok) { Log("[environment] weather summary reader did not validate; cloud control disabled"); return; }
    g_weatherAtmosphereSlot = slot;
    g_fieldOff[FCloud] = cloud;
    g_fieldOff[FMie] = mie;
    g_fieldOff[FFog] = fog;
}

static bool ResolveWeatherCompose() {
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
    Log("[environment] weather compose hooked (compose rva 0x%llx, compositor rva 0x%llx, container 0x%llx)",
        static_cast<unsigned long long>(compose - g_base),
        static_cast<unsigned long long>(compositor - g_base),
        static_cast<unsigned long long>(g_weatherNodeContainer));
    return true;
}

static bool ResolveWeatherEffects() {
    if (!g_tick || !g_activateFn || !g_intensityFn || !g_nullSentinel || g_entityParticleMgr < 0 ||
        !g_envManagerGlobal || g_envGetEntityVt < 0 || g_fieldOff[FSnow] < 0 ||
        g_effectSlotA[2] < 0 || g_effectSlotA[3] < 0 || !(g_effectHandleMask & 0xCu)) {
        Log("[environment] snow/particle bridge incomplete (activate=%p intensity=%p sentinel=%p particle=0x%llx slots2/3=%d/%d handles=0x%x); snow particles disabled",
            reinterpret_cast<void*>(g_activateFn), reinterpret_cast<void*>(g_intensityFn), reinterpret_cast<void*>(g_nullSentinel),
            static_cast<unsigned long long>(g_entityParticleMgr), g_effectSlotA[2] >= 0, g_effectSlotA[3] >= 0, g_effectHandleMask);
        return false;
    }
    if (!InstallInternalHook(reinterpret_cast<void*>(g_tick), reinterpret_cast<void*>(&HookWeatherTick),
                             reinterpret_cast<void**>(&g_origWeatherTick), "environment weather tick"))
        return false;
    Log("[environment] snow/particle bridge ready (tick rva 0x%llx, activate rva 0x%llx, particle mgr 0x%llx, handles 0x%llx mask 0x%x)",
        static_cast<unsigned long long>(g_tick - g_base), static_cast<unsigned long long>(g_activateFn - g_base),
        static_cast<unsigned long long>(g_entityParticleMgr), static_cast<unsigned long long>(g_effectHandleBase), g_effectHandleMask);
    return true;
}

// ---------------------------------------------------------------------------------------------------------------------
// Runtime
// ---------------------------------------------------------------------------------------------------------------------

static bool ResolveTimeContext(uintptr_t& envMgr, long long& entity) {
    envMgr = 0;
    entity = 0;
    if (!g_envManagerGlobal || g_envGetEntityVt < 0) return false;
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

// The entity must carry the advance-time virtual we derived the set-time slot from, otherwise the slot means nothing.
static bool EntityHasTimeVirtuals(long long entity) {
    uintptr_t vt = 0, fn = 0;
    if (!ReadPointer(static_cast<uintptr_t>(entity), vt)) return false;
    bool found = false;
    for (ptrdiff_t slot = 0; slot < 0x800 && !found; slot += 8)
        found = ReadPointer(vt + slot, fn) && fn == g_timeAddFn;
    return found && ReadPointer(vt + g_entitySetTimeVt, fn) && ExecutablePointer(fn);
}

static float RawToHour(float raw) {
    return NormalizeHour(g_timeDomainHours ? raw : raw * 24.0f);
}
static float HourToRaw(float hour) {
    hour = NormalizeHour(hour);
    return g_timeDomainHours ? hour : hour / 24.0f;
}

static bool ReadCurrentHour(long long entity, float& hour) {
    float raw = NAN;
    if (!ReadFloat(static_cast<uintptr_t>(entity) + g_timeCurrent, raw)) return false;
    hour = RawToHour(raw);
    return true;
}

static bool SetTimeRaw(long long entity, float raw) {
    uintptr_t vt = 0, fn = 0;
    if (!ReadPointer(static_cast<uintptr_t>(entity), vt) || !ReadPointer(vt + g_entitySetTimeVt, fn) || !ExecutablePointer(fn)) return false;
    auto setTime = reinterpret_cast<EntitySetTimeFn>(fn);
    const float eps = g_timeSetEpsilon;
    bool ok = true;
    CDK_GUARD_BEGIN
        setTime(entity, raw, eps);
    CDK_GUARD_FAIL
        ok = false;
    CDK_GUARD_END
    return ok;
}

// Weather table fields: the native value is kept apart from what we write, so a multiplier cannot compound when the
// table is not rebuilt, and releasing an override puts back the game's value unless the game already replaced ours.
static bool FieldNative(int f, uintptr_t base, float& native) {
    FieldState& s = g_field[f];
    float cur = NAN;
    if (g_fieldOff[f] < 0 || !base || !ReadFloat(base + g_fieldOff[f], cur)) return false;
    if (!s.active || s.base != base || cur != s.written) { s.native = cur; s.base = base; }
    native = s.native;
    return true;
}
static void FieldWrite(int f, float v) {
    FieldState& s = g_field[f];
    if (!std::isfinite(v) || !WriteBytes(s.base + g_fieldOff[f], &v, sizeof(v))) return;
    s.written = v;
    s.active = true;
}
static void FieldSet(int f, uintptr_t base, float v) {
    float native = 0.0f;
    if (FieldNative(f, base, native)) FieldWrite(f, v);
}
static void FieldRelease(int f, uintptr_t base) {
    FieldState& s = g_field[f];
    if (!s.active) return;
    s.active = false;
    float cur = NAN;
    if (s.base == base && base && ReadFloat(base + g_fieldOff[f], cur) && cur == s.written)
        WriteBytes(base + g_fieldOff[f], &s.native, sizeof(s.native));
}

static long long __fastcall HookWeatherCompose(long long weatherState, float dt) {
    if (!g_origWeatherCompose) return 0;
    const long long result = g_origWeatherCompose(weatherState, dt);

    const bool clear = g_weatherClear.load(std::memory_order_relaxed);
    const bool rainOn = g_rainOverride.load(std::memory_order_relaxed);
    const bool snowOn = g_snowOverride.load(std::memory_order_relaxed);
    const bool cloudOn = g_cloudOverride.load(std::memory_order_relaxed);
    const bool windOn = g_windOverride.load(std::memory_order_relaxed);
    bool anyActive = false;
    for (const FieldState& s : g_field) anyActive |= s.active;
    if (!clear && !rainOn && !snowOn && !cloudOn && !windOn && !anyActive) return result;

    uintptr_t parent = 0, child = 0, atmosphere = 0;
    if (!ReadPointer(static_cast<uintptr_t>(weatherState) + g_weatherNodeContainer, parent) ||
        !ReadPointer(parent + kWeatherChildSlot, child)) {
        return result;
    }
    if (g_weatherAtmosphereSlot >= 0) ReadPointer(parent + g_weatherAtmosphereSlot, atmosphere);

    bool want[FCount] = {};
    float value[FCount] = {};
    if (clear) {
        for (int f : { FRain, FSnow, FCloud, FMie, FFog }) { want[f] = true; value[f] = 0.0f; }
    } else {
        if (rainOn) { want[FRain] = true; value[FRain] = Clamp(g_rainValue.load(std::memory_order_relaxed), 0.0f, 1.0f); }
        if (snowOn) { want[FSnow] = true; value[FSnow] = Clamp(g_snowValue.load(std::memory_order_relaxed), 0.0f, 1.0f); }
        if (cloudOn) { want[FCloud] = true; value[FCloud] = Clamp(g_cloudValue.load(std::memory_order_relaxed), 0.0f, 3.0f); }
    }
    const float mul = Clamp(g_windMultiplier.load(std::memory_order_relaxed), 0.0f, 3.0f);
    for (int f = 0; f < FCount; ++f) {
        const uintptr_t base = FieldIsAtmosphere(f) ? atmosphere : child;
        const bool isWind = f == FWindSpeed || f == FWindAltitude || f == FWindBlend;
        if (isWind && windOn) {
            float native = 0.0f;
            if (FieldNative(f, base, native)) FieldWrite(f, native * mul);
        } else if (want[f]) {
            FieldSet(f, base, value[f]);
        } else {
            FieldRelease(f, base);
        }
    }
    return result;
}

static bool WeatherParticleContext(long long& particleMgr, int& nullSentinel) {
    particleMgr = 0;
    nullSentinel = 0;
    uintptr_t envMgr = 0;
    long long entity = 0;
    if (g_entityParticleMgr < 0 || !ResolveTimeContext(envMgr, entity) || !entity || !g_nullSentinel) return false;
    uintptr_t p = 0;
    if (!ReadPointer(static_cast<uintptr_t>(entity) + g_entityParticleMgr, p)) return false;
    if (!ReadBytes(reinterpret_cast<uintptr_t>(g_nullSentinel), &nullSentinel, sizeof(nullSentinel))) return false;
    particleMgr = static_cast<long long>(p);
    return true;
}

static void SetEffectIntensity(long long self, int effect, int nullSentinel, long long particleMgr, float value, bool activate) {
    if (effect < 0 || effect >= 9 || !g_intensityFn || g_effectHandleBase < 0 || !(g_effectHandleMask & (1u << effect))) return;
    const uintptr_t handleAt = static_cast<uintptr_t>(self) + g_effectHandleBase + effect * 4;
    int handle = nullSentinel;
    if (!ReadBytes(handleAt, &handle, sizeof(handle))) return;
    if (activate && value > 0.001f && handle == nullSentinel && g_activateFn && g_effectSlotA[effect] >= 0) {
        auto activateFn = reinterpret_cast<ActivateEffectFn>(g_activateFn);
        bool ok = true;
        CDK_GUARD_BEGIN
            activateFn(self, effect, reinterpret_cast<long long*>(self + g_effectSlotA[effect]),
                       reinterpret_cast<long long*>(self + g_effectSlotB[effect]), 1.0f);
        CDK_GUARD_FAIL
            ok = false;
        CDK_GUARD_END
        if (!ok || !ReadBytes(handleAt, &handle, sizeof(handle))) return;
    }
    if (handle == nullSentinel) return;
    auto setIntensity = reinterpret_cast<SetIntensityFn>(g_intensityFn);
    CDK_GUARD_BEGIN
        setIntensity(particleMgr, handle, Clamp(value, 0.0f, 1.0f));
    CDK_GUARD_FAIL
    CDK_GUARD_END
}

static void __fastcall HookWeatherTick(long long self, float dt) {
    if (!g_origWeatherTick) return;
    // The native tick runs first and drives every effect from the (possibly overridden) table, including releasing the
    // snow effects when the native snow value is low. So once an override ends, the game's own state is already back
    // this very tick; nothing is forced afterwards.
    g_origWeatherTick(self, dt);

    const bool clear = g_weatherClear.load(std::memory_order_relaxed);
    const bool snowOn = g_snowOverride.load(std::memory_order_relaxed);
    if (!clear && !snowOn) return;

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

    const float snow = Clamp(g_snowValue.load(std::memory_order_relaxed), 0.0f, 1.0f);
    SetEffectIntensity(self, 2, nullSentinel, particleMgr, snow, snow > 0.01f);
    SetEffectIntensity(self, 3, nullSentinel, particleMgr, snow, snow > 0.30f);
}

} // namespace

void EnvironmentInstall() {
    g_tick = ResolveWeatherTick();
    const bool rain = ResolveRainGetter();
    const auto intensityHits = ScanAll("89 54 24 10 53 48 83 EC 30 83 79 0C 00 48 8B D9 C5 F8 29 74 24 20", 3);
    if (intensityHits.size() == 1) g_intensityFn = intensityHits[0];
    else Log("[environment] effect intensity setter: %zu signature matches", intensityHits.size());
    if (g_tick && rain) {
        ResolveEffectLayout();   // first: the snow-getter check and the handle mask use the activation helper
        ScanWeatherTick();
    }
    if (!g_envManagerGlobal) Log("[environment] environment manager not derived from the weather tick");
    else Log("[environment] environment manager rva 0x%llx, get-entity vt 0x%llx, weather state 0x%llx, particle mgr 0x%llx",
             static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(g_envManagerGlobal) - g_base),
             static_cast<unsigned long long>(g_envGetEntityVt), static_cast<unsigned long long>(g_entityWeatherState),
             static_cast<unsigned long long>(g_entityParticleMgr));

    const bool timeLayout = g_envManagerGlobal && ResolveTimeLayout();
    g_timeAvailable.store(timeLayout, std::memory_order_relaxed);
    if (timeLayout) Log("[environment] visual-time controls ready");
    else Log("[environment] visual-time controls unavailable (optional)");

    if (rain) ResolveAtmosphere();
    const bool compose = rain && ResolveWeatherCompose();
    g_rainAvailable.store(compose && g_fieldOff[FRain] >= 0, std::memory_order_relaxed);
    g_snowTableAvailable.store(compose && g_fieldOff[FSnow] >= 0, std::memory_order_relaxed);
    g_windAvailable.store(compose && g_fieldOff[FWindSpeed] >= 0 && g_fieldOff[FWindBlend] >= 0, std::memory_order_relaxed);
    g_cloudAvailable.store(compose && g_fieldOff[FCloud] >= 0, std::memory_order_relaxed);
    g_weatherAvailable.store(compose, std::memory_order_relaxed);
    if (compose) {
        for (int f = 0; f < FCount; ++f) {
            if (g_fieldOff[f] >= 0) Log("[environment] weather field %s at 0x%llx", kFieldName[f], static_cast<unsigned long long>(g_fieldOff[f]));
            else Log("[environment] weather field %s not derived; left untouched", kFieldName[f]);
        }
        g_snowEffectsAvailable.store(g_snowTableAvailable.load(std::memory_order_relaxed) && ResolveWeatherEffects(),
                                     std::memory_order_relaxed);
    } else {
        Log("[environment] weather controls unavailable (optional)");
    }
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
        // World change: the old entity may be gone, so its limits are neither restored nor reused. A running freeze
        // re-captures the native limits on the new entity below.
        g_timeEntity = entity;
        g_timeOverriding = false;
        g_timeEntityOk = EntityHasTimeVirtuals(entity);
        if (!g_timeEntityOk) Log("[environment] environment entity %p lacks the derived time virtuals; time control idle", reinterpret_cast<void*>(entity));
    }
    if (!g_timeEntityOk) {
        g_timeCurrentValid.store(false, std::memory_order_relaxed);
        return;
    }

    float lo = NAN, hi = NAN;
    const bool haveLimits = ReadFloat(static_cast<uintptr_t>(entity) + g_timeLower, lo) &&
                            ReadFloat(static_cast<uintptr_t>(entity) + g_timeUpper, hi);
    if (!g_timeOverriding) {
        if (!haveLimits || hi <= lo + 0.0001f) {
            g_timeCurrentValid.store(false, std::memory_order_relaxed);
            return;
        }
        g_timeDomainHours = hi > 1.5f && hi <= 48.0f;
    }

    float current = 0.0f;
    if (ReadCurrentHour(entity, current)) {
        g_timeCurrentHour.store(current, std::memory_order_relaxed);
        g_timeCurrentValid.store(true, std::memory_order_relaxed);
    } else {
        g_timeCurrentValid.store(false, std::memory_order_relaxed);
    }

    const bool frozen = g_timeFreeze.load(std::memory_order_acquire);
    if (!frozen) {
        if (g_timeOverriding) {
            // undo only our own write: if a script replaced our limits since, its values stay
            if (haveLimits && lo == g_timeWritten && hi == g_timeWritten) {
                WriteBytes(static_cast<uintptr_t>(entity) + g_timeLower, &g_timeNativeLower, sizeof(float));
                WriteBytes(static_cast<uintptr_t>(entity) + g_timeUpper, &g_timeNativeUpper, sizeof(float));
            }
            g_timeOverriding = false;
        }
        int hold = g_timeSetHoldTicks.load(std::memory_order_acquire);
        if (hold > 0) {
            SetTimeRaw(entity, HourToRaw(g_timeTargetHour.load(std::memory_order_relaxed)));
            g_timeSetHoldTicks.compare_exchange_strong(hold, hold - 1, std::memory_order_relaxed);  // a new UI request wins
        }
        g_timeApply.store(false, std::memory_order_relaxed);
        return;
    }

    const float raw = HourToRaw(g_timeTargetHour.load(std::memory_order_relaxed));
    bool write = g_timeApply.exchange(false, std::memory_order_relaxed);
    if (!g_timeOverriding) {
        // capture exactly what the game has right before the freeze starts, on this entity
        g_timeNativeLower = lo;
        g_timeNativeUpper = hi;
        g_timeOverriding = true;
        write = true;
        Log("[environment] visual-time native limits %.4f..%.4f (%s)", lo, hi, g_timeDomainHours ? "hours" : "normalized");
    } else if (haveLimits && (lo != g_timeWritten || hi != g_timeWritten)) {
        // something else (a script / the game) set the limits while frozen: that is what restore must give back
        g_timeNativeLower = lo;
        g_timeNativeUpper = hi;
        write = true;
    }
    if (write) {
        // limits first: the native setter clamps into [lower, upper], so stale frozen limits would swallow the new time
        WriteBytes(static_cast<uintptr_t>(entity) + g_timeLower, &raw, sizeof(raw));
        WriteBytes(static_cast<uintptr_t>(entity) + g_timeUpper, &raw, sizeof(raw));
        g_timeWritten = raw;
        SetTimeRaw(entity, raw);
    }
    g_timeSetHoldTicks.store(0, std::memory_order_relaxed);
}

bool TimeControlAvailable() { return g_timeAvailable.load(std::memory_order_relaxed); }
bool TimeHour(float* hour) {
    if (!hour || !g_timeCurrentValid.load(std::memory_order_relaxed)) return false;
    *hour = g_timeCurrentHour.load(std::memory_order_relaxed);
    return true;
}
float TimeTargetHour() {
    // the target only means something while a request is pending or frozen; otherwise the slider follows the game
    const bool pending = g_timeFreeze.load(std::memory_order_relaxed) || g_timeSetHoldTicks.load(std::memory_order_relaxed) > 0;
    if (!pending && g_timeCurrentValid.load(std::memory_order_relaxed)) return NormalizeHour(g_timeCurrentHour.load(std::memory_order_relaxed));
    return NormalizeHour(g_timeTargetHour.load(std::memory_order_relaxed));
}
bool TimeFrozen() { return g_timeFreeze.load(std::memory_order_relaxed); }
void SetTimeHour(float hour) {
    // target before the flags (release): the game thread reads the flags with acquire and then sees this target
    g_timeTargetHour.store(NormalizeHour(hour), std::memory_order_relaxed);
    g_timeApply.store(true, std::memory_order_release);
    if (!g_timeFreeze.load(std::memory_order_relaxed))
        g_timeSetHoldTicks.store(8, std::memory_order_release);
}
void SetTimeFrozen(bool frozen) {
    if (frozen && !g_timeFreeze.load(std::memory_order_relaxed) &&
        g_timeSetHoldTicks.load(std::memory_order_relaxed) <= 0 &&
        g_timeCurrentValid.load(std::memory_order_relaxed)) {
        g_timeTargetHour.store(g_timeCurrentHour.load(std::memory_order_relaxed), std::memory_order_relaxed);
    }
    g_timeApply.store(frozen, std::memory_order_relaxed);
    g_timeFreeze.store(frozen, std::memory_order_release);
    if (!frozen) g_timeSetHoldTicks.store(0, std::memory_order_relaxed);
}
void ResetTimeControl() {
    g_timeFreeze.store(false, std::memory_order_release);
    g_timeApply.store(false, std::memory_order_relaxed);
    g_timeSetHoldTicks.store(0, std::memory_order_relaxed);
}

bool WeatherControlAvailable() { return g_weatherAvailable.load(std::memory_order_relaxed); }
bool WeatherRainAvailable() { return g_rainAvailable.load(std::memory_order_relaxed); }
bool WeatherCloudAvailable() { return g_cloudAvailable.load(std::memory_order_relaxed); }
bool WeatherWindAvailable() { return g_windAvailable.load(std::memory_order_relaxed); }
bool WeatherSnowEffectsAvailable() { return g_snowEffectsAvailable.load(std::memory_order_relaxed); }
bool WeatherClearSky() { return g_weatherClear.load(std::memory_order_relaxed); }
void SetWeatherClearSky(bool enabled) { g_weatherClear.store(enabled && WeatherControlAvailable(), std::memory_order_relaxed); }
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
    g_rainOverride.store(enabled && WeatherRainAvailable(), std::memory_order_relaxed);
}
void SetWeatherSnowOverride(bool enabled, float value) {
    g_snowValue.store(Clamp(value, 0.0f, 1.0f), std::memory_order_relaxed);
    g_snowOverride.store(enabled && WeatherSnowEffectsAvailable(), std::memory_order_relaxed);
}
void SetWeatherCloudOverride(bool enabled, float value) {
    g_cloudValue.store(Clamp(value, 0.0f, 3.0f), std::memory_order_relaxed);
    g_cloudOverride.store(enabled && WeatherCloudAvailable(), std::memory_order_relaxed);
}
void SetWeatherWindOverride(bool enabled, float multiplier) {
    g_windMultiplier.store(Clamp(multiplier, 0.0f, 3.0f), std::memory_order_relaxed);
    g_windOverride.store(enabled && WeatherWindAvailable(), std::memory_order_relaxed);
}
void ResetWeatherControl() {
    g_weatherClear.store(false, std::memory_order_relaxed);
    g_rainOverride.store(false, std::memory_order_relaxed);
    g_snowOverride.store(false, std::memory_order_relaxed);
    g_cloudOverride.store(false, std::memory_order_relaxed);
    g_windOverride.store(false, std::memory_order_relaxed);
}

} // namespace core
