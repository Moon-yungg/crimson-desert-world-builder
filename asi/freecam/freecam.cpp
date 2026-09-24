// Fly Mode for Crimson Desert (c) dofo7777 and Zappenduster: a standalone ASI plugin. One key (F7 by default) switches a
// free-flying camera on and off.
// No UI. Same technique as World Builder's free camera (cdmodkit), reduced to what the camera needs:
//  - the renderer camera: a global read by "mov rax,[rip+X]; vmovsd xmm6,[rax+0xC8]; mov ebx,[rax+0xD0]", recognised by two vtable
//    slot fingerprints (the class has no RTTI); path from CrimsonDesertTelemetry (github.com/fabianviol/CrimsonDesertTelemetry, MIT)
//  - its per-frame pose function (camera, rot[16], pos[3], a[3], tilePos[3], eye[3], p7): our rotation / position replace the game's
//  - the camera scene object, moved every frame by setWorldTransform from one call site: its pose decides culling, LOD, the
//    minimap and sound, so it gets the same pose (otherwise the world disappears where the game camera does not look)
// Nothing is hardcoded: every address comes from a byte signature at startup; if one is missing the plugin stays inactive.
#include <windows.h>
#include <MinHook.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cmath>
#include <cstdarg>
#include <string>
#include <vector>

static uintptr_t g_base = 0;
static FILE* g_log = nullptr;
static std::string g_dir;
static void Log(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t); fprintf(g_log, "[%02d:%02d:%02d.%03d] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a); fputc('\n', g_log); fflush(g_log);
}
// every read of game memory is guarded: a pointer that went stale must never take the game down
static bool ReadBytes(uintptr_t addr, void* out, size_t n) {
    if (addr < 0x10000) return false;
    __try { memcpy(out, (const void*)addr, n); return true; } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
static bool ReadPtr(uintptr_t addr, uintptr_t* out) { return ReadBytes(addr, out, 8); }

// ---- signatures ----
static std::vector<int> ParsePattern(const char* s) {
    std::vector<int> out; for (const char* p = s; *p; ) { while (*p == ' ') p++; if (!*p) break; if (*p == '?') { out.push_back(-1); while (*p == '?') p++; } else out.push_back((int)strtoul(p, (char**)&p, 16)); }
    return out;
}
static uintptr_t FindPattern(const char* pat, int* count) {
    auto p = ParsePattern(pat); uintptr_t first = 0; int n = 0;
    auto dos = (IMAGE_DOS_HEADER*)g_base; auto nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew); auto sec = IMAGE_FIRST_SECTION(nt);
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        if (!(sec[i].Characteristics & IMAGE_SCN_MEM_EXECUTE)) continue;
        const uint8_t* s = (const uint8_t*)(g_base + sec[i].VirtualAddress); const size_t sz = sec[i].Misc.VirtualSize;
        for (size_t k = 0; k + p.size() <= sz; k++) {
            if (p[0] >= 0 && s[k] != p[0]) continue;
            bool ok = true; for (size_t j = 1; j < p.size(); j++) if (p[j] >= 0 && s[k + j] != p[j]) { ok = false; break; }
            if (ok) { if (!first) first = (uintptr_t)(s + k); if (++n > 8) break; }
        }
    }
    *count = n; return first;
}
static uintptr_t FindVtableBySlots(uintptr_t f1, uintptr_t f2) {   // read-only data holding {slot1, slot2} = {f1, f2}, unique
    auto dos = (IMAGE_DOS_HEADER*)g_base; auto nt = (IMAGE_NT_HEADERS64*)(g_base + dos->e_lfanew); auto sec = IMAGE_FIRST_SECTION(nt);
    uintptr_t found = 0; int n = 0;
    for (int i = 0; i < nt->FileHeader.NumberOfSections; i++) {
        const DWORD ch = sec[i].Characteristics;
        if ((ch & IMAGE_SCN_MEM_EXECUTE) || (ch & IMAGE_SCN_MEM_WRITE) || !(ch & IMAGE_SCN_MEM_READ)) continue;
        const uint8_t* p = (const uint8_t*)(g_base + sec[i].VirtualAddress); const size_t sz = sec[i].Misc.VirtualSize;
        for (size_t k = 8; k + 16 <= sz; k += 8) { uintptr_t a, b; memcpy(&a, p + k, 8); if (a != f1) continue; memcpy(&b, p + k + 8, 8); if (b == f2) { found = (uintptr_t)(p + k - 8); n++; } }
    }
    return n == 1 ? found : 0;
}

static uintptr_t g_camGlobal = 0, g_camVt = 0, g_camSyncRet = 0;
static void* g_fnSetCamPose = nullptr; static void* g_fnSetXf = nullptr;
static bool Resolve(bool quiet) {
    int n = 0, n1 = 0, n2 = 0, n3 = 0, n4 = 0, n5 = 0;
    const uintptr_t ref = FindPattern("48 8B 05 ?? ?? ?? ?? C5 FB 10 B0 C8 00 00 00 8B 98 D0 00 00 00", &n);
    const uintptr_t f1 = FindPattern("48 8B C4 48 89 58 10 48 89 70 18 41 54 41 56 41 57 48 81 EC 10 01 00 00 8B B1 A8 02 00 00 4D 8B ?? 4D 8B ?? 4C 8B F2 48 8B D9 85 F6", &n1);
    const uintptr_t f2 = FindPattern("40 53 48 83 EC 20 48 8B 01 48 8B DA FF 50 68 4C 8B C8 48 63 48 08 85 C9 75 08 32 C0 48 83 C4 20 5B C3 48 89 7C 24 30 41 B0 01 33 FF 4C 8B D1 C5 F8 57 C0 48 83 F9 04", &n2);
    const uintptr_t pose = FindPattern("48 81 EC 88 00 00 00 C5 FC 10 02 C5 FC 11 41 48 C5 FC 10 4A 20 C5 FA 10 2D ?? ?? ?? ?? 48 8B 84 24 B8 00 00 00", &n3);
    const uintptr_t sxf = FindPattern("48 8B C4 48 89 58 08 48 89 68 10 48 89 70 18 57 48 81 EC E0 00 00 00 C5 F8 29 70 E8 C5 F8 29 78 D8 C5 78 29 40 C8 41 0F B6 E8", &n4);
    // the camera's own call of setWorldTransform: "mov rax,[rbx+0xE0] ... lea rcx,[rdx-0x28] ... call setWorldTransform"
    const uintptr_t site = FindPattern("48 8B 83 E0 00 00 00 41 B1 01 89 54 24 78 45 0F B6 C1 C5 FC 11 44 24 50 C5 FB 11 4C 24 70 48 8B 50 08 33 C0 48 85 D2 48 8D 4A D8 48 0F 44 C8 48 8D 54 24 50 C5 F8 77 E8 ?? ?? ?? ??", &n5);
    if (n != 1 || n1 != 1 || n2 != 1 || n3 != 1 || n4 != 1 || n5 != 1) {
        if (!quiet) Log("signatures not found (camera global %d, camera slots %d/%d, pose %d, setWorldTransform %d, camera call %d): fly mode off", n, n1, n2, n3, n4, n5);
        return false;
    }
    int32_t d = 0; memcpy(&d, (const void*)(ref + 3), 4); const uintptr_t global = ref + 7 + d;
    memcpy(&d, (const void*)(site + 56), 4); const uintptr_t target = site + 60 + d;
    if (target != sxf) { if (!quiet) Log("camera call does not lead to setWorldTransform: fly mode off"); return false; }
    const uintptr_t vt = FindVtableBySlots(f1, f2); if (!vt) { if (!quiet) Log("camera vtable not found: fly mode off"); return false; }
    g_camGlobal = global; g_camVt = vt; g_camSyncRet = site + 60; g_fnSetCamPose = (void*)pose; g_fnSetXf = (void*)sxf;
    Log("resolved: camera global rva 0x%llx, vtable rva 0x%llx, pose rva 0x%llx, setWorldTransform rva 0x%llx, camera call rva 0x%llx",
        (unsigned long long)(global - g_base), (unsigned long long)(vt - g_base), (unsigned long long)(pose - g_base), (unsigned long long)(sxf - g_base), (unsigned long long)(site - g_base));
    return true;
}
static uintptr_t RendererCamera() { uintptr_t c = 0, v = 0; return ReadPtr(g_camGlobal, &c) && c && ReadPtr(c, &v) && v == g_camVt ? c : 0; }

// ---- settings (FlyMode.ini next to the .asi) ----
static int g_key = VK_F7; static float g_speed = 10.0f, g_sens = 0.12f, g_fast = 4.0f;
static int KeyFromName(std::string v) {
    for (auto& c : v) c = (char)toupper((unsigned char)c);
    if (v.size() >= 2 && v[0] == 'F' && isdigit((unsigned char)v[1])) { const int f = atoi(v.c_str() + 1); if (f >= 1 && f <= 24) return VK_F1 + f - 1; }
    if (v.size() == 1 && (isalnum((unsigned char)v[0]))) return v[0];
    struct { const char* n; int vk; } map[] = { { "INSERT", VK_INSERT }, { "HOME", VK_HOME }, { "END", VK_END }, { "DELETE", VK_DELETE }, { "PAGEUP", VK_PRIOR }, { "PAGEDOWN", VK_NEXT },
                                                { "SCROLLLOCK", VK_SCROLL }, { "PAUSE", VK_PAUSE }, { "BACKQUOTE", VK_OEM_3 }, { "NUMPAD0", VK_NUMPAD0 }, { "CAPSLOCK", VK_CAPITAL } };
    for (auto& m : map) if (v == m.n) return m.vk;
    return 0;
}
static void LoadSettings() {
    const std::string path = g_dir + "\\FlyMode.ini";
    FILE* f = fopen(path.c_str(), "r");
    if (!f) {   // first start: write the defaults so they can be edited
        if ((f = fopen(path.c_str(), "w"))) {
            fputs("; Fly Mode for Crimson Desert\n; key: F1..F12, a letter or digit, INSERT HOME END DELETE PAGEUP PAGEDOWN SCROLLLOCK PAUSE BACKQUOTE NUMPAD0 CAPSLOCK\n"
                  "key=F7\n; flying speed in metres per second, and the factor while Shift is held\nspeed=10\nfast=4\n; mouse: degrees per mouse count\nsensitivity=0.12\n", f);
            fclose(f);
        }
        return;
    }
    char line[256];
    while (fgets(line, sizeof line, f)) {
        std::string l(line); while (!l.empty() && (l.back() == '\n' || l.back() == '\r' || l.back() == ' ')) l.pop_back();
        if (l.empty() || l[0] == ';' || l[0] == '#') continue;
        const size_t eq = l.find('='); if (eq == std::string::npos) continue;
        const std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "key") { const int vk = KeyFromName(v); if (vk) g_key = vk; }
        else if (k == "speed") { const float s = (float)atof(v.c_str()); if (s >= 0.5f && s <= 500.0f) g_speed = s; }
        else if (k == "fast") { const float s = (float)atof(v.c_str()); if (s >= 1.0f && s <= 50.0f) g_fast = s; }
        else if (k == "sensitivity") { const float s = (float)atof(v.c_str()); if (s >= 0.01f && s <= 2.0f) g_sens = s; }
    }
    fclose(f);
}

// ---- input: movement keys and mouse belong to the camera while flying ----
static HWND g_hwnd = nullptr; static WNDPROC g_origWndProc = nullptr;
static volatile bool g_on = false;
static CRITICAL_SECTION g_cs;
static float g_lookDx = 0, g_lookDy = 0;
static bool IsCamScan(int scan) { return scan == 0x11 || scan == 0x1E || scan == 0x1F || scan == 0x20 || scan == 0x10 || scan == 0x12 || scan == 0x2A || scan == 0x1D || scan == 0x39; }   // W A S D Q E Shift Ctrl Space
static bool KeyDown(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }
static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM w, LPARAM l) {
    if (g_on) {
        if (msg == WM_INPUT) {
            UINT size = 0; alignas(8) unsigned char buf[1024];
            if (GetRawInputData((HRAWINPUT)l, RID_INPUT, nullptr, &size, sizeof(RAWINPUTHEADER)) == 0 && size && size <= sizeof buf &&
                GetRawInputData((HRAWINPUT)l, RID_INPUT, buf, &size, sizeof(RAWINPUTHEADER)) == size) {
                const RAWINPUT* ri = (const RAWINPUT*)buf;
                if (ri->header.dwType == RIM_TYPEMOUSE) {
                    const RAWMOUSE& m = ri->data.mouse;
                    if (!(m.usFlags & MOUSE_MOVE_ABSOLUTE)) { EnterCriticalSection(&g_cs); g_lookDx += (float)m.lLastX; g_lookDy += (float)m.lLastY; LeaveCriticalSection(&g_cs); }
                    const USHORT ups = RI_MOUSE_LEFT_BUTTON_UP | RI_MOUSE_RIGHT_BUTTON_UP | RI_MOUSE_MIDDLE_BUTTON_UP | RI_MOUSE_BUTTON_4_UP | RI_MOUSE_BUTTON_5_UP;
                    if (!(m.usButtonFlags & ups)) return DefWindowProcW(h, msg, w, l);   // the game's camera must not turn (its view decides culling); releases pass
                } else if (ri->header.dwType == RIM_TYPEKEYBOARD) {
                    const RAWKEYBOARD& k = ri->data.keyboard;
                    if (IsCamScan(k.MakeCode & 0xFF) && !(k.Flags & RI_KEY_BREAK)) return DefWindowProcW(h, msg, w, l);   // presses only: a key held when flying starts must be released in the game too
                }
            }
        }
        if ((msg == WM_KEYDOWN || msg == WM_SYSKEYDOWN || msg == WM_CHAR) && IsCamScan((int)((l >> 16) & 0xFF))) return 0;
        if (msg == WM_LBUTTONDOWN || msg == WM_RBUTTONDOWN || msg == WM_MBUTTONDOWN || msg == WM_XBUTTONDOWN || msg == WM_MOUSEWHEEL) return 0;   // no attack or aim while flying
    }
    return CallWindowProcW(g_origWndProc, h, msg, w, l);
}
struct WndPick { HWND h; long area; };
static BOOL CALLBACK FindWnd(HWND h, LPARAM p) {   // the largest visible top-level window of this process (a splash window can come first)
    DWORD pid = 0; GetWindowThreadProcessId(h, &pid);
    if (pid != GetCurrentProcessId() || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT r; GetClientRect(h, &r); const long area = (r.right - r.left) * (r.bottom - r.top);
    auto* w = (WndPick*)p; if (area > w->area) { w->h = h; w->area = area; }
    return TRUE;
}
static bool OurWindowInFront() { DWORD pid = 0; HWND f = GetForegroundWindow(); if (f) GetWindowThreadProcessId(f, &pid); return f && pid == GetCurrentProcessId(); }
static void AttachWindow() {   // subclass the game window; again if a larger one appears (the real game window after a splash)
    WndPick w = { nullptr, 0 }; EnumWindows(FindWnd, (LPARAM)&w);
    if (!w.h || w.h == g_hwnd || w.area < 640 * 360) return;
    if (g_hwnd && g_origWndProc) SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, (LONG_PTR)g_origWndProc);   // leave the old one as it was
    g_origWndProc = (WNDPROC)SetWindowLongPtrW(w.h, GWLP_WNDPROC, (LONG_PTR)WndProc); g_hwnd = w.h;
    char cls[64] = ""; GetClassNameA(w.h, cls, sizeof cls); Log("game window %p (%s, %ld px)", (void*)w.h, cls, w.area);
}

// ---- the camera ----
static float g_pos[3] = {}, g_yaw = 0, g_pitch = 0, g_rightSign = 1, g_upSign = 1;
static volatile bool g_init = false; static LARGE_INTEGER g_last = {};
static void Cross(const float* a, const float* b, float* o) { o[0] = a[1] * b[2] - a[2] * b[1]; o[1] = a[2] * b[0] - a[0] * b[2]; o[2] = a[0] * b[1] - a[1] * b[0]; }
static void Norm(float* v) { const float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; } }
static void Basis(float* r, float* u, float* f) {   // from yaw/pitch, with the game's handedness learned when flying starts
    const float y = g_yaw * 3.14159265f / 180.0f, p = g_pitch * 3.14159265f / 180.0f;
    f[0] = cosf(p) * sinf(y); f[1] = sinf(p); f[2] = cosf(p) * cosf(y);
    const float wu[3] = { 0, 1, 0 }; Cross(wu, f, r); Norm(r); for (int i = 0; i < 3; i++) r[i] *= g_rightSign;
    Cross(f, r, u); Norm(u); for (int i = 0; i < 3; i++) u[i] *= g_upSign;
}
static void QToM(const float* q, float* m) {   // columns = rotated basis, m[col*3+row]
    const float x = q[0], y = q[1], z = q[2], w = q[3];
    m[0] = 1 - 2 * (y * y + z * z); m[1] = 2 * (x * y + z * w); m[2] = 2 * (x * z - y * w);
    m[3] = 2 * (x * y - z * w); m[4] = 1 - 2 * (x * x + z * z); m[5] = 2 * (y * z + x * w);
    m[6] = 2 * (x * z + y * w); m[7] = 2 * (y * z - x * w); m[8] = 1 - 2 * (x * x + y * y);
}
static void MToQ(const float* m, float* q) {
    auto R = [&](int r, int c) { return m[c * 3 + r]; };
    const float tr = R(0, 0) + R(1, 1) + R(2, 2);
    if (tr > 0) { const float s = sqrtf(tr + 1.0f) * 2; q[3] = 0.25f * s; q[0] = (R(2, 1) - R(1, 2)) / s; q[1] = (R(0, 2) - R(2, 0)) / s; q[2] = (R(1, 0) - R(0, 1)) / s; }
    else if (R(0, 0) > R(1, 1) && R(0, 0) > R(2, 2)) { const float s = sqrtf(1.0f + R(0, 0) - R(1, 1) - R(2, 2)) * 2; q[3] = (R(2, 1) - R(1, 2)) / s; q[0] = 0.25f * s; q[1] = (R(0, 1) + R(1, 0)) / s; q[2] = (R(0, 2) + R(2, 0)) / s; }
    else if (R(1, 1) > R(2, 2)) { const float s = sqrtf(1.0f + R(1, 1) - R(0, 0) - R(2, 2)) * 2; q[3] = (R(0, 2) - R(2, 0)) / s; q[0] = (R(0, 1) + R(1, 0)) / s; q[1] = 0.25f * s; q[2] = (R(1, 2) + R(2, 1)) / s; }
    else { const float s = sqrtf(1.0f + R(2, 2) - R(0, 0) - R(1, 1)) * 2; q[3] = (R(1, 0) - R(0, 1)) / s; q[0] = (R(0, 2) + R(2, 0)) / s; q[1] = (R(1, 2) + R(2, 1)) / s; q[2] = 0.25f * s; }
    const float l = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]); if (l > 1e-6f) for (int i = 0; i < 4; i++) q[i] /= l;
}
static void Step() {   // mouse turn and key movement since the last frame
    LARGE_INTEGER now, freq; QueryPerformanceCounter(&now); QueryPerformanceFrequency(&freq);
    float dt = (float)(now.QuadPart - g_last.QuadPart) / (float)freq.QuadPart; g_last = now; if (dt < 0 || dt > 0.1f) dt = 0.1f;
    EnterCriticalSection(&g_cs); const float dx = g_lookDx, dy = g_lookDy; g_lookDx = g_lookDy = 0; LeaveCriticalSection(&g_cs);
    g_yaw += dx * g_sens; g_pitch -= dy * g_sens;
    if (g_pitch > 89.0f) g_pitch = 89.0f; if (g_pitch < -89.0f) g_pitch = -89.0f;
    if (g_yaw > 180.0f) g_yaw -= 360.0f; if (g_yaw < -180.0f) g_yaw += 360.0f;
    if (!OurWindowInFront()) return;
    float r[3], u[3], f[3]; Basis(r, u, f);
    const float v = g_speed * dt * (KeyDown(VK_SHIFT) ? g_fast : 1.0f);
    const float fw = (KeyDown('W') ? 1.0f : 0.0f) - (KeyDown('S') ? 1.0f : 0.0f), sd = (KeyDown('D') ? 1.0f : 0.0f) - (KeyDown('A') ? 1.0f : 0.0f);
    const float up = (KeyDown('E') || KeyDown(VK_SPACE) ? 1.0f : 0.0f) - (KeyDown('Q') || KeyDown(VK_CONTROL) ? 1.0f : 0.0f);
    for (int i = 0; i < 3; i++) g_pos[i] += v * (f[i] * fw + r[i] * sd) + (i == 1 ? v * up : 0.0f);
}

// setWorldTransform(obj, TiledTransform* {scale3, quat4, pos3 in tile, int16 tileX, tileZ}, u8, u8), from the camera's call site:
// the camera scene object's pose equals the view (measured: same rotation, no offset), so it gets ours directly. It is set before
// the renderer camera in each frame, so the step happens here and culling and view use the same pose.
typedef void(__fastcall* SetXfFn)(void*, const float*, uint8_t, uint8_t);
static SetXfFn g_origSetXf = nullptr;
static void __fastcall HookSetXf(void* obj, const float* xf, uint8_t a, uint8_t b) {
    if (g_on && (uintptr_t)_ReturnAddress() == g_camSyncRet && xf) {
        float t[11];
        if (ReadBytes((uintptr_t)xf, t, 44)) {
            if (!g_init) {   // start where the game camera is, looking the same way; learn which way its right / up vectors point
                float m[9]; QToM(&t[3], m); int16_t tile[2]; memcpy(tile, &t[10], 4);
                g_pos[0] = t[7] + tile[0] * 1000.0f; g_pos[1] = t[8]; g_pos[2] = t[9] + tile[1] * 1000.0f;
                const float f0[3] = { m[6], m[7], m[8] }, r0[3] = { m[0], m[1], m[2] }, u0[3] = { m[3], m[4], m[5] };
                g_yaw = atan2f(f0[0], f0[2]) * 180.0f / 3.14159265f; g_pitch = asinf(f0[1] < -1 ? -1 : f0[1] > 1 ? 1 : f0[1]) * 180.0f / 3.14159265f;
                g_rightSign = g_upSign = 1; float r[3], u[3], f[3]; Basis(r, u, f);
                g_rightSign = r[0] * r0[0] + r[1] * r0[1] + r[2] * r0[2] >= 0 ? 1.0f : -1.0f; Basis(r, u, f);
                g_upSign = u[0] * u0[0] + u[1] * u0[1] + u[2] * u0[2] >= 0 ? 1.0f : -1.0f;
                QueryPerformanceCounter(&g_last); g_init = true;
                Log("flying from (%.1f %.1f %.1f), yaw %.1f pitch %.1f", g_pos[0], g_pos[1], g_pos[2], g_yaw, g_pitch);
            }
            Step();
            float r[3], u[3], f[3]; Basis(r, u, f);
            const float S[9] = { r[0], r[1], r[2], u[0], u[1], u[2], f[0], f[1], f[2] }; float q[4]; MToQ(S, q);
            alignas(16) float o[12]; memcpy(o, t, 44);
            o[3] = q[0]; o[4] = q[1]; o[5] = q[2]; o[6] = q[3];
            const int tx = (int)(g_pos[0] * 0.001), tz = (int)(g_pos[2] * 0.001);   // truncation toward zero, like the game
            o[7] = g_pos[0] - tx * 1000.0f; o[8] = g_pos[1]; o[9] = g_pos[2] - tz * 1000.0f;
            const int16_t nt[2] = { (int16_t)tx, (int16_t)tz }; memcpy(&o[10], nt, 4); o[11] = 0;
            g_origSetXf(obj, o, a, b); return;
        }
    }
    g_origSetXf(obj, xf, a, b);
}
// the renderer camera's pose: rot with right / up / forward as columns (m0,m4,m8 = right), pos = world position, tilePos = the same
// relative to its tile, eye = (0,0,0) (camera-relative rendering, left alone)
typedef void* (__fastcall* SetCamPoseFn)(void*, const float*, const float*, const float*, const float*, const float*, void*);
static SetCamPoseFn g_origSetCamPose = nullptr;
static void* __fastcall HookSetCamPose(void* cam, const float* rot, const float* pos, const float* a, const float* tp, const float* eye, void* c) {
    if (g_on && g_init && rot && pos && tp && (uintptr_t)cam == RendererCamera()) {
        float m[16], po[3], to[3];
        if (ReadBytes((uintptr_t)rot, m, sizeof m) && ReadBytes((uintptr_t)pos, po, sizeof po) && ReadBytes((uintptr_t)tp, to, sizeof to)) {
            float r[3], u[3], f[3]; Basis(r, u, f);
            m[0] = r[0]; m[4] = r[1]; m[8] = r[2]; m[1] = u[0]; m[5] = u[1]; m[9] = u[2]; m[2] = f[0]; m[6] = f[1]; m[10] = f[2];
            static float s_rot[16], s_pos[3], s_tile[3];   // the game thread is the only caller
            memcpy(s_rot, m, sizeof m);
            for (int i = 0; i < 3; i++) { s_pos[i] = g_pos[i]; s_tile[i] = g_pos[i] - (po[i] - to[i]); }
            return g_origSetCamPose(cam, s_rot, s_pos, a, s_tile, eye, c);
        }
    }
    return g_origSetCamPose(cam, rot, pos, a, tp, eye, c);
}

static void SetOn(bool on) {
    if (on == g_on) return;
    g_init = false; EnterCriticalSection(&g_cs); g_lookDx = g_lookDy = 0; LeaveCriticalSection(&g_cs);
    g_on = on; Log("fly mode %s", on ? "on" : "off");
}
static DWORD WINAPI MainThread(LPVOID) {
    Sleep(1500);   // all .asi plugins are loaded by then
    if (GetModuleHandleA("cdmodkit.asi")) { Log("World Builder (cdmodkit.asi) is installed and has the same free camera on its own key (F6): Fly Mode stays inactive so the two do not hook the same functions"); return 0; }
    // the game unpacks parts of its code after start: retry the signatures for a while
    bool ok = false;
    for (int i = 0; i < 120 && !ok; i++) { ok = Resolve(i < 119); if (!ok) Sleep(1000); }
    if (!ok) return 0;
    if (MH_Initialize() != MH_OK && MH_Initialize() != MH_ERROR_ALREADY_INITIALIZED) { Log("MinHook init failed"); return 0; }
    if (MH_CreateHook(g_fnSetXf, (void*)HookSetXf, (void**)&g_origSetXf) != MH_OK || MH_CreateHook(g_fnSetCamPose, (void*)HookSetCamPose, (void**)&g_origSetCamPose) != MH_OK ||
        MH_EnableHook(g_fnSetXf) != MH_OK || MH_EnableHook(g_fnSetCamPose) != MH_OK) { Log("hooks failed: fly mode off"); return 0; }
    for (int i = 0; i < 600 && !g_hwnd; i++) { AttachWindow(); if (!g_hwnd) Sleep(500); }
    if (!g_hwnd) { Log("game window not found: fly mode off"); return 0; }
    char kn[32] = "?"; GetKeyNameTextA((LONG)(MapVirtualKeyA(g_key, MAPVK_VK_TO_VSC) << 16), kn, sizeof kn);
    Log("ready: %s switches fly mode (speed %.1f m/s, x%.1f with Shift, sensitivity %.3f)", kn, g_speed, g_fast, g_sens);
    bool was = false; DWORD lastCheck = GetTickCount();
    for (;;) {   // the key works only while the game is in front
        if (GetTickCount() - lastCheck > 2000) { AttachWindow(); lastCheck = GetTickCount(); }
        const bool down = KeyDown(g_key) && OurWindowInFront();
        if (down && !was) SetOn(!g_on);
        was = down;
        if (g_on && !RendererCamera()) SetOn(false);   // loading screen or main menu: the camera went away
        Sleep(15);
    }
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID) {
    if (reason != DLL_PROCESS_ATTACH) return TRUE;
    DisableThreadLibraryCalls(h);
    g_base = (uintptr_t)GetModuleHandleA(nullptr);
    char path[MAX_PATH]; GetModuleFileNameA(h, path, MAX_PATH); g_dir = path; g_dir = g_dir.substr(0, g_dir.find_last_of("\\/"));
    // the game's launcher process loads the plugin too: append (with the process id) instead of overwriting, restart when large
    const std::string lp = g_dir + "\\FlyMode.log"; WIN32_FILE_ATTRIBUTE_DATA fa{};
    const bool big = GetFileAttributesExA(lp.c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 256 * 1024;
    g_log = fopen(lp.c_str(), big ? "w" : "a");
    InitializeCriticalSection(&g_cs);
    LoadSettings();
    Log("Fly Mode 1.0 loaded (process %lu)", GetCurrentProcessId());
    CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
    return TRUE;
}
