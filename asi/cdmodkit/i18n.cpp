#define WIN32_LEAN_AND_MEAN
#include "i18n.h"
#include <windows.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cstdarg>
#include <cstdio>
#include <fstream>
#include <map>
#include <set>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace i18n {
    namespace {
        constexpr LanguageOption kLanguages[] = {
            { "auto", "Auto (system)" }, { "en", "English" },
            { "zh-CN", "Simplified Chinese" }, { "zh-TW", "Traditional Chinese" },
            { "de", "German" }, { "fr", "French" }, { "ko", "Korean" },
            { "ja", "Japanese" }, { "es", "Spanish" }, { "pt-BR", "Portuguese (Brazil)" },
            { "ru", "Russian" }, { "tr", "Turkish" },
        };
        constexpr const char* kLocaleIds[] = { "en", "zh-CN", "zh-TW", "de", "fr", "ko", "ja", "es", "pt-BR", "ru", "tr" };
        using PackRow = std::array<std::string, 11>;
        std::unordered_map<std::string, PackRow> g_pack;
        std::vector<unsigned short> g_glyphRanges;
        std::string g_preference = "auto";
        std::string g_systemLanguage = "en";
        bool g_initialized = false;

        int LocaleIndex(const std::string& id) {
            for (int i = 0; i < 11; ++i) if (id == kLocaleIds[i]) return i;
            return 0;
        }
        std::string ResolveSystemLocale() {
            wchar_t wide[LOCALE_NAME_MAX_LENGTH] = {};
            if (!GetUserDefaultLocaleName(wide, LOCALE_NAME_MAX_LENGTH)) return "en";
            char utf8[LOCALE_NAME_MAX_LENGTH * 4] = {};
            const int bytes = WideCharToMultiByte(CP_UTF8, 0, wide, -1, utf8, sizeof utf8, nullptr, nullptr);
            if (bytes <= 1) return "en";
            std::string locale(utf8);
            for (char& c : locale) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            if (locale.rfind("zh-hant", 0) == 0 || locale.rfind("zh-tw", 0) == 0 || locale.rfind("zh-hk", 0) == 0 || locale.rfind("zh-mo", 0) == 0) return "zh-TW";
            if (locale.rfind("zh", 0) == 0) return "zh-CN";
            if (locale.rfind("pt-br", 0) == 0) return "pt-BR";
            for (int i = 3; i < 11; ++i) {
                const char* id = kLocaleIds[i];
                if (locale.rfind(id, 0) == 0) return id;
            }
            return "en";
        }
        std::string ActiveId() { return g_preference == "auto" ? g_systemLanguage : g_preference; }
        std::string Unescape(std::string_view value) {
            std::string out; out.reserve(value.size());
            for (size_t i = 0; i < value.size(); ++i) {
                if (value[i] == '\\' && i + 1 < value.size()) {
                    const char n = value[i + 1];
                    if (n == 'n') { out.push_back('\n'); ++i; continue; }
                    if (n == 'r') { out.push_back('\r'); ++i; continue; }
                    if (n == 't') { out.push_back('\t'); ++i; continue; }
                    if (n == '\\') { out.push_back('\\'); ++i; continue; }
                }
                out.push_back(value[i]);
            }
            return out;
        }
        std::vector<std::string> SplitRow(const std::string& line) {
            std::vector<std::string> fields;
            size_t at = 0;
            for (;;) {
                const size_t tab = line.find('\t', at);
                fields.push_back(Unescape(std::string_view(line).substr(at, tab == std::string::npos ? tab : tab - at)));
                if (tab == std::string::npos) break;
                at = tab + 1;
            }
            return fields;
        }
        void BuildGlyphRanges() {
            std::set<unsigned short> codepoints;
            auto collect = [&](const std::string& text) {
                if (text.empty()) return;
                const int count = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0);
                if (count <= 0) return;
                std::vector<wchar_t> wide((size_t)count);
                if (!MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), wide.data(), count)) return;
                for (wchar_t c : wide) if ((unsigned int)c >= 0x0100 && (unsigned int)c <= 0xFFFF) codepoints.insert((unsigned short)c);
            };
            for (const auto& row : g_pack) { collect(row.first); for (const std::string& text : row.second) collect(text); }
            g_glyphRanges.clear();
            g_glyphRanges.push_back(0x0020); g_glyphRanges.push_back(0x00FF);
            for (auto it = codepoints.begin(); it != codepoints.end();) {
                const unsigned short first = *it;
                unsigned short last = first;
                ++it;
                while (it != codepoints.end() && (unsigned int)*it == (unsigned int)last + 1) { last = *it; ++it; }
                g_glyphRanges.push_back(first); g_glyphRanges.push_back(last);
            }
            g_glyphRanges.push_back(0);
        }
        std::string StripVisibleKey(const char* source, std::string& prefix, std::string& suffix) {
            if (!source) return {};
            std::string_view text(source);
            size_t offset = 0;
            // Icon macros use private-use Unicode code points. Keep them in the rendered label but omit them from lookup keys.
            while (offset + 2 < text.size() && (unsigned char)text[offset] == 0xEF && (unsigned char)text[offset + 1] >= 0x80 && (unsigned char)text[offset + 1] <= 0xA3) {
                prefix.append(text.substr(offset, 3)); offset += 3;
            }
            while (offset < text.size() && (text[offset] == ' ' || text[offset] == '\t')) prefix.push_back(text[offset++]);
            text.remove_prefix(offset);
            while (!text.empty() && (text.back() == ' ' || text.back() == '\t')) {
                suffix.insert(suffix.begin(), text.back());
                text.remove_suffix(1);
            }
            const size_t hidden = text.find("##");
            if (hidden != std::string_view::npos) { suffix.assign(text.substr(hidden)); text = text.substr(0, hidden); }
            return std::string(text);
        }
    }

    void Initialize(const std::string& modDir) {
        if (g_initialized) return;
        g_initialized = true; g_systemLanguage = ResolveSystemLocale();
        std::ifstream file(modDir + "\\locales.tsv", std::ios::binary);
        std::string line;
        if (!std::getline(file, line)) return;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        const auto header = SplitRow(line);
        if (header.size() != 12 || header[0] != "key") return;
        for (int i = 0; i < 11; ++i) if (header[(size_t)i + 1] != kLocaleIds[i]) return;
        while (std::getline(file, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty() || line[0] == '#') continue;
            auto fields = SplitRow(line);
            if (fields.size() != 12 || fields[0].empty()) continue;
            PackRow row;
            for (size_t i = 0; i < row.size(); ++i) row[i] = std::move(fields[i + 1]);
            g_pack.emplace(std::move(fields[0]), std::move(row));
        }
        BuildGlyphRanges();
    }
    const char* Translate(const char* source) {
        if (!source) return "";
        std::string prefix, suffix;
        const std::string key = StripVisibleKey(source, prefix, suffix);
        auto it = g_pack.find(key);
        if (it == g_pack.end()) return source;
        const int locale = LocaleIndex(ActiveId());
        const std::string& translated = it->second[(size_t)locale];
        if (translated.empty()) return source;
        if (prefix.empty() && suffix.empty()) return translated.c_str();
        thread_local std::string decorated;
        decorated = prefix + translated + suffix;
        return decorated.c_str();
    }
    const char* Preference() { return g_preference.c_str(); }
    const char* ActiveLanguage() { static thread_local std::string active; active = ActiveId(); return active.c_str(); }
    const LanguageOption* Languages(int* count) {
        static LanguageOption localized[sizeof kLanguages / sizeof kLanguages[0]];
        for (size_t i = 0; i < sizeof kLanguages / sizeof kLanguages[0]; ++i) {
            localized[i] = { kLanguages[i].id, Translate(kLanguages[i].name) };
        }
        if (count) *count = (int)(sizeof localized / sizeof localized[0]);
        return localized;
    }
    bool SetPreference(const char* id) {
        if (!id) return false;
        for (const auto& option : kLanguages) if (_stricmp(option.id, id) == 0) { g_preference = option.id; return true; }
        return false;
    }
    const unsigned short* GlyphRanges() { return g_glyphRanges.empty() ? nullptr : g_glyphRanges.data(); }
    void MergeSystemFonts(ImFontAtlas* atlas, float size) {
        if (!atlas || g_glyphRanges.empty()) return;
        static const char* paths[] = {
            "C:\\Windows\\Fonts\\msyh.ttc", "C:\\Windows\\Fonts\\msjh.ttc", "C:\\Windows\\Fonts\\simsun.ttc", "C:\\Windows\\Fonts\\simhei.ttf",
            "C:\\Windows\\Fonts\\meiryo.ttc", "C:\\Windows\\Fonts\\YuGothR.ttc", "C:\\Windows\\Fonts\\msgothic.ttc",
            "C:\\Windows\\Fonts\\malgun.ttf", "C:\\Windows\\Fonts\\gulim.ttc",
        };
        for (const char* path : paths) {
            if (GetFileAttributesA(path) == INVALID_FILE_ATTRIBUTES) continue;
            ImFontConfig config{}; config.MergeMode = true; config.PixelSnapH = true;
            atlas->AddFontFromFileTTF(path, size, &config, (const ImWchar*)g_glyphRanges.data());
        }
    }
    void FormatV(char* out, size_t capacity, const char* source, va_list args) {
        if (!out || !capacity) return;
        vsnprintf(out, capacity, Translate(source), args);
        out[capacity - 1] = 0;
    }
}
