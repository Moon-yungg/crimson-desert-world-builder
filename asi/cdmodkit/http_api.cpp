// Loopback-only HTTP access to the same scene and prefab operations used by the editor.
#define WIN32_LEAN_AND_MEAN
#include <winsock2.h>
#include <ws2tcpip.h>
#include "core.h"
#include "http_api.h"
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

#pragma comment(lib, "ws2_32.lib")

namespace httpapi {
namespace {

using Fields = std::map<std::string, std::string>;
static std::atomic<int> activePort{ 0 };

static std::string Quote(const std::string& s) {
    std::string out = "\"";
    const char* hex = "0123456789abcdef";
    for (unsigned char c : s) {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 32) { out += "\\u00"; out += hex[c >> 4]; out += hex[c & 15]; }
        else out += (char)c;
    }
    return out + '"';
}

static std::string Num(double x) { char b[64]; snprintf(b, sizeof b, "%.9g", x); return b; }
static std::string Int(int x) { return std::to_string(x); }
static std::string Bool(bool x) { return x ? "true" : "false"; }
static std::string Error(const char* s) { return "{\"error\":" + Quote(s) + "}"; }
static void Space(const std::string& s, size_t& p) { while (p < s.size() && (s[p] == ' ' || s[p] == '\r' || s[p] == '\n' || s[p] == '\t')) ++p; }
static bool Hex(char c, unsigned& n) {
    if (c >= '0' && c <= '9') n = c - '0';
    else if (c >= 'a' && c <= 'f') n = c - 'a' + 10;
    else if (c >= 'A' && c <= 'F') n = c - 'A' + 10;
    else return false;
    return true;
}
static void Utf8(std::string& out, unsigned u) {
    if (u < 0x80) out += (char)u;
    else if (u < 0x800) { out += (char)(0xc0 | (u >> 6)); out += (char)(0x80 | (u & 63)); }
    else { out += (char)(0xe0 | (u >> 12)); out += (char)(0x80 | ((u >> 6) & 63)); out += (char)(0x80 | (u & 63)); }
}
static bool JsonString(const std::string& s, size_t& p, std::string& out) {
    if (p >= s.size() || s[p++] != '"') return false;
    out.clear();
    while (p < s.size()) {
        unsigned char c = (unsigned char)s[p++];
        if (c == '"') return true;
        if (c < 32) return false;
        if (c != '\\') { out += (char)c; continue; }
        if (p == s.size()) return false;
        c = (unsigned char)s[p++];
        if (c == '"' || c == '\\' || c == '/') out += (char)c;
        else if (c == 'n') out += '\n'; else if (c == 'r') out += '\r'; else if (c == 't') out += '\t';
        else if (c == 'b') out += '\b'; else if (c == 'f') out += '\f';
        else if (c == 'u') {
            if (p + 4 > s.size()) return false;
            unsigned u = 0, v = 0;
            for (int i = 0; i < 4; ++i) { if (!Hex(s[p++], v)) return false; u = (u << 4) | v; }
            if (u >= 0xd800 && u <= 0xdfff) return false;
            Utf8(out, u);
        } else return false;
    }
    return false;
}
static bool JsonObject(const std::string& s, Fields& out) {
    size_t p = 0; Space(s, p);
    if (p >= s.size() || s[p++] != '{') return false;
    Space(s, p);
    if (p < s.size() && s[p] == '}') { ++p; Space(s, p); return p == s.size(); }
    while (p < s.size()) {
        std::string key, val;
        if (!JsonString(s, p, key)) return false;
        Space(s, p); if (p >= s.size() || s[p++] != ':') return false;
        Space(s, p); if (p >= s.size()) return false;
        if (s[p] == '"') { if (!JsonString(s, p, val)) return false; }
        else {
            size_t a = p;
            while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ' ' && s[p] != '\t' && s[p] != '\r' && s[p] != '\n') ++p;
            val = s.substr(a, p - a);
            if (val != "true" && val != "false" && val != "null") {
                char* end = nullptr; strtod(val.c_str(), &end);
                if (val.empty() || !end || *end) return false;
            }
        }
        if (!out.emplace(key, val).second) return false;
        Space(s, p); if (p >= s.size()) return false;
        char next = s[p++];
        if (next == '}') { Space(s, p); return p == s.size(); }
        if (next != ',') return false;
        Space(s, p);
    }
    return false;
}
static std::string Decode(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '%' && i + 2 < s.size()) {
            unsigned a, b; if (Hex(s[i + 1], a) && Hex(s[i + 2], b)) { out += (char)((a << 4) | b); i += 2; continue; }
        }
        out += s[i] == '+' ? ' ' : s[i];
    }
    return out;
}
static Fields Query(const std::string& s) {
    Fields out;
    size_t p = 0;
    while (p < s.size()) { size_t end = s.find('&', p); if (end == std::string::npos) end = s.size();
        size_t eq = s.find('=', p); if (eq > end) eq = end;
        out[Decode(s.substr(p, eq - p))] = eq < end ? Decode(s.substr(eq + 1, end - eq - 1)) : "";
        p = end + 1;
    }
    return out;
}
static bool Number(const Fields& f, const char* k, float& out, bool required = false) {
    auto it = f.find(k); if (it == f.end()) return !required;
    char* end = nullptr; double d = strtod(it->second.c_str(), &end);
    if (it->second.empty() || !end || *end || !std::isfinite(d) || d < -10000000 || d > 10000000) return false;
    out = (float)d; return true;
}
static bool Integer(const Fields& f, const char* k, int& out, bool required = false) {
    auto it = f.find(k); if (it == f.end()) return !required;
    char* end = nullptr; long n = strtol(it->second.c_str(), &end, 10);
    if (it->second.empty() || !end || *end || n < 0 || n > 1000000) return false;
    out = (int)n; return true;
}
static bool Page(const Fields& f, int& offset, int& limit) {
    return Integer(f, "offset", offset) && Integer(f, "limit", limit) && limit >= 1 && limit <= 500;
}
static std::string PageTail(int total, int offset, int limit, const std::string& items) {
    int next = offset + limit;
    return "{\"total\":" + Int(total) + ",\"offset\":" + Int(offset) + ",\"limit\":" + Int(limit) +
        ",\"nextOffset\":" + (next < total ? Int(next) : "null") + ",\"items\":" + items + "]}";
}
static bool Flag(const Fields& f, const char* k) { auto it = f.find(k); return it != f.end() && (it->second == "true" || it->second == "1"); }
static bool Name(const Fields& f, std::string& name) {
    auto it = f.find("name"); if (it == f.end() || it->second.empty() || it->second.size() > 100) return false;
    name = it->second;
    if (name == "." || name == ".." || name.back() == '.' || name.back() == ' ') return false;
    for (unsigned char c : name) if (c < 32 || c == '/' || c == '\\' || c == ':' || c == '|' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>') return false;
    return true;
}
static std::string ObjectJson(const SpawnedObj& o) {
    return "{\"uid\":" + Int(o.uid) + ",\"prefab\":" + Quote(o.prefab) +
        ",\"x\":" + Num(o.pos.x) + ",\"y\":" + Num(o.pos.y) + ",\"z\":" + Num(o.pos.z) +
        ",\"yaw\":" + Num(o.rot.yaw) + ",\"pitch\":" + Num(o.rot.pitch) + ",\"roll\":" + Num(o.rot.roll) +
        ",\"scale\":" + Num(o.scale) + ",\"hidden\":" + Bool(o.hidden) +
        ",\"group\":" + Int(o.group) + ",\"project\":" + Quote(core::ProjectNameOf(o.proj)) + "}";
}
static bool Ready(int& status, std::string& out) {
    if (core::HooksReady() && core::GameThreadReady()) return true;
    status = 503; out = Error("game thread is not ready"); return false;
}
static std::string Handle(const std::string& method, const std::string& path, const Fields& arg, int& status) {
    status = 200;
    if (method == "GET" && path == "/api/status") {
        return "{\"apiVersion\":1,\"ready\":" + Bool(core::HooksReady() && core::GameThreadReady()) +
            ",\"gameVersion\":" + Quote(core::GameVersion()) + ",\"buildOk\":" + Bool(core::BuildOk()) +
            ",\"buildMessage\":" + Quote(core::BuildMessage()) + ",\"pending\":" + Int(core::PendingSpawns()) + "}";
    }
    if (method == "GET" && path == "/api/player") {
        Vec3 p{}; if (!core::PlayerWorldPos(&p)) { status = 503; return Error("player is not in the world"); }
        return "{\"x\":" + Num(p.x) + ",\"y\":" + Num(p.y) + ",\"z\":" + Num(p.z) + "}";
    }
    if (method == "GET" && path == "/api/prefabs") {
        int offset = 0, limit = 100;
        if (!Page(arg, offset, limit)) { status = 400; return Error("invalid offset or limit (page size 1..500)"); }
        std::string search; auto q = arg.find("q"); if (q != arg.end()) search = q->second;
        std::transform(search.begin(), search.end(), search.begin(), [](unsigned char c) { return (char)tolower(c); });
        const auto& list = core::PrefabIndex(); int total = 0; std::string items = "[";
        for (size_t i = 0; i < list.size(); ++i) {
            const auto& p = list[i]; std::string hay = p.path + " " + p.name + " " + p.tags;
            std::transform(hay.begin(), hay.end(), hay.begin(), [](unsigned char c) { return (char)tolower(c); });
            if (!search.empty() && hay.find(search) == std::string::npos) continue;
            if (total >= offset && total < offset + limit) {
                if (items.size() > 1) items += ',';
                items += "{\"index\":" + Int((int)i) + ",\"path\":" + Quote(p.path) + ",\"name\":" + Quote(p.name) +
                    ",\"tags\":" + Quote(p.tags) + ",\"mesh\":" + Quote(p.mesh) + ",\"meshes\":" + Int(p.meshes) +
                    ",\"children\":" + Int(p.children) + "}";
            }
            ++total;
        }
        return PageTail(total, offset, limit, items);
    }
    if (method == "GET" && path == "/api/objects") {
        int offset = 0, limit = 100;
        if (!Page(arg, offset, limit)) { status = 400; return Error("invalid offset or limit (page size 1..500)"); }
        auto list = core::Spawned(); std::string items = "[";
        for (size_t i = (size_t)offset; i < list.size() && i < (size_t)(offset + limit); ++i) {
            if (items.size() > 1) items += ',';
            items += ObjectJson(list[i]);
        }
        return PageTail((int)list.size(), offset, limit, items);
    }
    if (method == "POST" && path == "/api/objects") {
        std::string prefab; auto it = arg.find("prefab"); if (it != arg.end()) prefab = it->second;
        float x = 0, y = 0, z = 0, yaw = 0, pitch = 0, roll = 0, scale = 1;
        if (prefab.empty() || prefab.size() > 600 || prefab[0] != '/' || prefab.find("..") != std::string::npos ||
            !Number(arg, "x", x, true) || !Number(arg, "y", y, true) || !Number(arg, "z", z, true) ||
            !Number(arg, "yaw", yaw) || !Number(arg, "pitch", pitch) || !Number(arg, "roll", roll) || !Number(arg, "scale", scale) || scale <= 0) {
            status = 400; return Error("invalid prefab or transform");
        }
        std::string out; if (!Ready(status, out)) return out;
        int uid = core::SpawnAt(prefab, { x, y, z }, { yaw, pitch, roll }, scale);
        if (!uid) { status = 503; return Error("spawn could not be queued"); }
        status = 202; return "{\"uid\":" + Int(uid) + ",\"queued\":true}";
    }
    if (path.rfind("/api/objects/", 0) == 0) {
        std::string tail = path.substr(13); size_t slash = tail.find('/');
        std::string idText = tail.substr(0, slash); int uid = 0;
        Fields idField{ { "id", idText } }; if (!Integer(idField, "id", uid, true) || uid <= 0) { status = 400; return Error("invalid uid"); }
        int index = core::IndexOfUid(uid); if (index < 0) { status = 404; return Error("object not found"); }
        auto list = core::Spawned(); if (index >= (int)list.size() || list[index].uid != uid) { status = 409; return Error("scene changed; retry"); }
        const auto& o = list[index]; std::string action = slash == std::string::npos ? "" : tail.substr(slash + 1);
        if (method == "GET" && action.empty()) return ObjectJson(o);
        std::string out; if (!Ready(status, out)) return out;
        if (method == "PATCH" && action.empty()) {
            Vec3 p = o.pos; Rot r = o.rot; float scale = o.scale;
            if (!Number(arg, "x", p.x) || !Number(arg, "y", p.y) || !Number(arg, "z", p.z) ||
                !Number(arg, "yaw", r.yaw) || !Number(arg, "pitch", r.pitch) || !Number(arg, "roll", r.roll) ||
                !Number(arg, "scale", scale) || scale <= 0) { status = 400; return Error("invalid transform"); }
            if (!core::MoveMany({ core::MoveReq{ uid, p, r, scale } }, true)) { status = 409; return Error("move failed"); }
            status = 202; return "{\"uid\":" + Int(uid) + ",\"queued\":true}";
        }
        if (method == "POST" && action == "hide") {
            if (!core::HideUid(uid)) { status = 409; return Error("hide failed"); }
            status = 202; return "{\"uid\":" + Int(uid) + ",\"queued\":true}";
        }
        if (method == "DELETE" && action.empty()) {
            if (!o.hidden && !core::HideUid(uid)) { status = 409; return Error("hide failed"); }
            core::ForgetUid(uid); status = 202; return "{\"uid\":" + Int(uid) + ",\"queued\":true}";
        }
        if (method == "POST" && action == "group") {
            int group = 0; if (!Integer(arg, "group", group, true)) { status = 400; return Error("invalid group"); }
            core::SetGroup(uid, group); return "{\"uid\":" + Int(uid) + ",\"group\":" + Int(group) + "}";
        }
        if (method == "POST" && action == "project") {
            std::string name; if (!Name(arg, name)) { status = 400; return Error("invalid name"); }
            core::AssignProject(uid, core::ProjectId(name)); return "{\"uid\":" + Int(uid) + ",\"project\":" + Quote(name) + "}";
        }
    }
    if (method == "POST" && path == "/api/groups") {
        return "{\"group\":" + Int(core::NewGroupId()) + "}";
    }
    if (method == "POST" && path == "/api/scene/clear") {
        std::string out; if (!Ready(status, out)) return out;
        core::DeleteAllSpawned(); status = 202; return "{\"queued\":true}";
    }
    if (method == "GET" && path == "/api/projects") {
        auto list = core::ListProjects(); std::string out = "{\"items\":[";
        for (size_t i = 0; i < list.size(); ++i) { if (i) out += ','; out += Quote(list[i]); }
        return out + "]}";
    }
    if (method == "GET" && path == "/api/autoload") {
        auto list = core::Autoload(); std::string out = "{\"items\":[";
        for (size_t i = 0; i < list.size(); ++i) { if (i) out += ','; out += Quote(list[i]); }
        return out + "]}";
    }
    if (method == "POST" && (path == "/api/projects/save" || path == "/api/projects/load" || path == "/api/autoload")) {
        std::string name; if (!Name(arg, name)) { status = 400; return Error("invalid project name"); }
        if (path == "/api/autoload") { core::SetAutoload(name, Flag(arg, "enabled")); return "{\"ok\":true}"; }
        std::string out; if (!Ready(status, out)) return out;
        if (path == "/api/projects/save") {
            int scope = 0; if (!Integer(arg, "scope", scope) || scope > 3) { status = 400; return Error("scope must be 0..3"); }
            if (!core::SaveProject(name, scope)) { status = 500; return Error("save failed"); }
        } else if (!core::LoadProject(name, Flag(arg, "clearFirst"))) { status = 404; return Error("project not found"); }
        return "{\"ok\":true,\"pending\":" + Int(core::PendingSpawns()) + "}";
    }
    if (method == "POST" && path == "/api/log") {
        auto it = arg.find("text"); if (it == arg.end() || it->second.size() > 1000) { status = 400; return Error("invalid text"); }
        core::Log("[http] %s", it->second.c_str()); return "{\"ok\":true}";
    }
    status = 404; return Error("endpoint not found");
}

static bool SendAll(SOCKET s, const std::string& data) {
    size_t p = 0; while (p < data.size()) { int n = send(s, data.data() + p, (int)(data.size() - p), 0); if (n <= 0) return false; p += n; } return true;
}
static void Reply(SOCKET s, int status, const std::string& body) {
    const char* reason = status == 200 ? "OK" : status == 202 ? "Accepted" : status == 400 ? "Bad Request" :
        status == 404 ? "Not Found" : status == 409 ? "Conflict" : status == 503 ? "Service Unavailable" : "Internal Server Error";
    std::string head = "HTTP/1.1 " + Int(status) + " " + reason + "\r\nContent-Type: application/json; charset=utf-8\r\nContent-Length: " +
        Int((int)body.size()) + "\r\nConnection: close\r\nCache-Control: no-store\r\nX-Content-Type-Options: nosniff\r\n\r\n";
    SendAll(s, head + body);
}
static void Client(SOCKET s, int port) {
    DWORD timeout = 3000; setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, (const char*)&timeout, sizeof timeout);
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, (const char*)&timeout, sizeof timeout);
    std::string request; char buf[4096]; size_t end = std::string::npos;
    while ((end = request.find("\r\n\r\n")) == std::string::npos && request.size() < 16384) {
        int n = recv(s, buf, sizeof buf, 0); if (n <= 0) return; request.append(buf, n);
    }
    if (end == std::string::npos) { Reply(s, 400, Error("headers too large")); return; }
    size_t lineEnd = request.find("\r\n"); if (lineEnd == std::string::npos) { Reply(s, 400, Error("invalid request")); return; }
    std::string line = request.substr(0, lineEnd); size_t a = line.find(' '), b = line.find(' ', a + 1);
    if (a == std::string::npos || b == std::string::npos || line.substr(b + 1).rfind("HTTP/1.", 0) != 0) { Reply(s, 400, Error("invalid request line")); return; }
    std::string method = line.substr(0, a), target = line.substr(a + 1, b - a - 1);
    if (method != "GET" && method != "POST" && method != "PATCH" && method != "DELETE") { Reply(s, 400, Error("unsupported method")); return; }
    size_t length = 0; bool seenLength = false, seenHost = false;
    for (size_t p = lineEnd + 2; p < end;) {
        size_t q = request.find("\r\n", p); if (q == std::string::npos || q > end) break;
        std::string h = request.substr(p, q - p); p = q + 2;
        size_t colon = h.find(':'); if (colon == std::string::npos) { Reply(s, 400, Error("invalid header")); return; }
        std::string key = h.substr(0, colon); std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return (char)tolower(c); });
        if (key == "host") {
            std::string host = h.substr(colon + 1); size_t first = host.find_first_not_of(" \t"), last = host.find_last_not_of(" \t");
            host = first == std::string::npos ? "" : host.substr(first, last - first + 1);
            std::string suffix = ":" + Int(port);
            if (host != "127.0.0.1" && host != "localhost" && host != "127.0.0.1" + suffix && host != "localhost" + suffix) {
                Reply(s, 400, Error("invalid host")); return;
            }
            seenHost = true;
        }
        if (key == "origin") { Reply(s, 400, Error("browser origin requests are unsupported")); return; }
        if (key == "transfer-encoding") { Reply(s, 400, Error("transfer encoding is unsupported")); return; }
        if (key == "content-length") {
            if (seenLength) { Reply(s, 400, Error("duplicate content length")); return; }
            seenLength = true; std::string val = h.substr(colon + 1); char* stop = nullptr; unsigned long n = strtoul(val.c_str(), &stop, 10);
            while (stop && *stop == ' ') ++stop;
            if (!stop || *stop || n > 65536) { Reply(s, 400, Error("invalid content length")); return; }
            length = (size_t)n;
        }
    }
    if (!seenHost) { Reply(s, 400, Error("host required")); return; }
    if (method != "GET" && method != "DELETE" && !seenLength) { Reply(s, 400, Error("content length required")); return; }
    std::string body = request.substr(end + 4); if (body.size() > length) body.resize(length);
    while (body.size() < length) { int n = recv(s, buf, (int)(std::min)(sizeof buf, length - body.size()), 0); if (n <= 0) return; body.append(buf, n); }
    size_t qm = target.find('?'); std::string path = target.substr(0, qm);
    if (path.empty() || path[0] != '/' || path.find('%') != std::string::npos) { Reply(s, 400, Error("invalid path")); return; }
    Fields fields = qm == std::string::npos ? Fields{} : Query(target.substr(qm + 1));
    if (length && !JsonObject(body, fields)) { Reply(s, 400, Error("expected flat JSON object")); return; }
    int status = 200; std::string result = Handle(method, path, fields, status); Reply(s, status, result);
}
// The listener is owned by Start/Stop, not by the server thread: Stop closes it, which makes the blocked accept() fail and
// the thread exit after the request it is serving. The generation tells a stopped thread from a transient accept error.
static std::mutex g_mx;                          // Start/Stop come from the init thread and the render thread (settings checkbox)
static SOCKET g_listener = INVALID_SOCKET;
static std::atomic<unsigned> g_gen{ 0 };
static std::string g_error;
struct ServerArg { SOCKET listener; int port; unsigned gen; };
static DWORD WINAPI Server(void* param) {
    const ServerArg a = *(ServerArg*)param; delete (ServerArg*)param;
    for (;;) {
        SOCKET s = accept(a.listener, nullptr, nullptr);
        if (g_gen.load() != a.gen) { if (s != INVALID_SOCKET) closesocket(s); break; }
        if (s == INVALID_SOCKET) { Sleep(50); continue; }   // e.g. WSAECONNRESET from a client that gave up: keep serving
        Client(s, a.port); closesocket(s);
    }
    core::Log("http: server on port %d stopped", a.port);
    return 0;
}
static void StopLocked() {
    if (g_listener == INVALID_SOCKET) return;
    g_gen.fetch_add(1); activePort.store(0);
    closesocket(g_listener); g_listener = INVALID_SOCKET;
}
}
bool Start(int port) {
    std::lock_guard<std::mutex> l(g_mx);
    if (port <= 0 || port > 65535) { g_error = "invalid port"; return false; }
    if (g_listener != INVALID_SOCKET && activePort.load() == port) return true;
    StopLocked();
    static bool s_wsa = false;
    if (!s_wsa) { WSADATA data{}; if (WSAStartup(MAKEWORD(2, 2), &data)) { g_error = "WSAStartup failed"; core::Log("http: WSAStartup failed"); return false; } s_wsa = true; }
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { g_error = "socket failed (WSA " + Int(WSAGetLastError()) + ")"; core::Log("http: %s", g_error.c_str()); return false; }
    BOOL excl = TRUE; setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char*)&excl, sizeof excl);   // no other process can bind over us
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK); addr.sin_port = htons((u_short)port);
    if (bind(listener, (sockaddr*)&addr, sizeof addr) == SOCKET_ERROR || listen(listener, 8) == SOCKET_ERROR) {
        const int e = WSAGetLastError(); closesocket(listener);
        g_error = e == WSAEADDRINUSE || e == WSAEACCES ? "port " + Int(port) + " is used by another program" : "cannot listen on port " + Int(port) + " (WSA " + Int(e) + ")";
        core::Log("http: %s", g_error.c_str()); return false;
    }
    const unsigned gen = g_gen.fetch_add(1) + 1;
    HANDLE h = CreateThread(nullptr, 0, Server, new ServerArg{ listener, port, gen }, 0, nullptr);
    if (!h) { closesocket(listener); g_error = "thread creation failed"; core::Log("http: %s", g_error.c_str()); return false; }
    CloseHandle(h);
    g_listener = listener; activePort.store(port); g_error.clear();
    core::Log("http: listening on http://127.0.0.1:%d/api/status", port);
    return true;
}
void Stop() { std::lock_guard<std::mutex> l(g_mx); if (g_listener != INVALID_SOCKET) core::Log("http: stopping"); StopLocked(); }
int ActivePort() { return activePort.load(); }
std::string LastError() { std::lock_guard<std::mutex> l(g_mx); return g_error; }
}
