#define WIN32_LEAN_AND_MEAN
#include "i18n.h"
#include <windows.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cstring>
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
        // native = how speakers of that language write its name: the selector always shows it, so a player who picked a
        // language they cannot read still finds their own; the name in the current UI language follows in brackets
        struct Language { const char* id; const char* name; const char* native; };
        constexpr Language kLanguages[] = {
            { "auto", "Auto (system)", nullptr }, { "en", "English", "English" },
            { "zh-CN", "Simplified Chinese", "简体中文" }, { "zh-TW", "Traditional Chinese", "繁體中文" },
            { "de", "German", "Deutsch" }, { "fr", "French", "Français" }, { "ko", "Korean", "한국어" },
            { "ja", "Japanese", "日本語" }, { "es", "Spanish", "Español" }, { "pt-BR", "Portuguese (Brazil)", "Português (Brasil)" },
            { "ru", "Russian", "Русский" }, { "tr", "Turkish", "Türkçe" },
        };
        constexpr int kLanguageCount = (int)(sizeof kLanguages / sizeof kLanguages[0]);
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
            for (const auto& l : kLanguages) if (l.native) collect(l.native);   // the selector needs them even without locales.tsv
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
            // U+E000..U+F8FF in UTF-8: EE 80..BF xx or EF 80..A3 xx (icons.h starts at U+E000, so the lead byte is 0xEE)
            auto isPua = [&](size_t at) { const unsigned char a = (unsigned char)text[at], b = (unsigned char)text[at + 1];
                                          return (a == 0xEE && b >= 0x80 && b <= 0xBF) || (a == 0xEF && b >= 0x80 && b <= 0xA3); };
            while (offset + 2 < text.size() && isPua(offset)) {
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

    static void LoadPack(const std::string& path) {
        std::ifstream file(path, std::ios::binary);
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
    }
    void Initialize(const std::string& modDir) {
        if (g_initialized) return;
        g_initialized = true; g_systemLanguage = ResolveSystemLocale();
        LoadPack(modDir + "\\locales.tsv");
        BuildGlyphRanges();   // also without locales.tsv: the language selector shows the native names
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
        // several decorated results can be alive at once (BeginCombo label + preview, a format plus translated arguments):
        // a single buffer would be overwritten by the second call and leave the first pointer dangling
        thread_local std::string ring[16]; thread_local unsigned next = 0;
        std::string& decorated = ring[next++ & 15];
        decorated = prefix + translated + suffix;
        return decorated.c_str();
    }
    const char* T(const char* english) { return Translate(english); }
    const char* TStable(const char* english) {
        const char* t = Translate(english);
        if (t == english || strstr(english, "##")) return t;
        thread_local std::string ring[8]; thread_local unsigned next = 0;
        std::string& s = ring[next++ & 7]; s = t; s += "###"; s += english; return s.c_str();
    }
    std::string ActiveLanguage() { return ActiveId(); }
    const char* Preference() { return g_preference.c_str(); }
    const LanguageOption* Languages(int* count) {
        // "Deutsch (German)" in an English UI, "日本語 (Japanisch)" in a German one, just "Deutsch" where both agree;
        // auto names the language it resolved to: "Auto (system): Deutsch"
        static LanguageOption options[kLanguageCount]; static std::string names[kLanguageCount];
        auto nativeOf = [](const std::string& id) { for (const auto& l : kLanguages) if (l.native && id == l.id) return l.native; return "English"; };
        for (int i = 0; i < kLanguageCount; ++i) {
            const Language& l = kLanguages[i];
            if (!l.native) names[i] = std::string(Translate(l.name)) + ": " + nativeOf(g_systemLanguage);
            else { const char* local = Translate(l.name); names[i] = l.native; if (strcmp(local, l.native) != 0) { names[i] += " ("; names[i] += local; names[i] += ")"; } }
            options[i] = { l.id, names[i].c_str() };
        }
        if (count) *count = kLanguageCount;
        return options;
    }
    bool SetPreference(const char* id) {
        if (!id) return false;
        for (const auto& option : kLanguages) if (_stricmp(option.id, id) == 0) { g_preference = option.id; return true; }
        return false;
    }
    const unsigned short* GlyphRanges() { return g_glyphRanges.empty() ? nullptr : g_glyphRanges.data(); }
    void MergeSystemFonts(ImFontAtlas* atlas, float size) {
        if (!atlas || g_glyphRanges.empty()) return;
        // One font per script (the first one installed), the active language's first: Han characters shared by zh-CN, zh-TW
        // and ja take their shapes from the first merged font. Every extra font would only add 10-20 MB of TTF data.
        struct Script { const char* lang; const char* files[3]; };
        static const Script scripts[] = {
            { "zh-CN", { "msyh.ttc", "simsun.ttc", "simhei.ttf" } }, { "zh-TW", { "msjh.ttc", "mingliu.ttc", nullptr } },
            { "ja", { "meiryo.ttc", "YuGothR.ttc", "msgothic.ttc" } }, { "ko", { "malgun.ttf", "gulim.ttc", nullptr } },
        };
        const std::string active = ActiveId();
        std::vector<const Script*> order;
        for (const auto& s : scripts) if (active == s.lang) order.push_back(&s);
        for (const auto& s : scripts) if (active != s.lang) order.push_back(&s);
        for (const Script* s : order) {
            for (const char* file : s->files) {
                if (!file) break;
                const std::string path = std::string("C:\\Windows\\Fonts\\") + file;
                if (GetFileAttributesA(path.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
                ImFontConfig config{}; config.MergeMode = true; config.PixelSnapH = true;
                atlas->AddFontFromFileTTF(path.c_str(), size, &config, (const ImWchar*)g_glyphRanges.data());
                break;
            }
        }
    }
    void ReleaseMergedFontData(ImFontAtlas* atlas) {
        // the overlay builds its atlas exactly once; afterwards the merged TTF files are dead weight (ClearInputData would
        // also drop the custom rects of the icons and the mouse cursor, so only the merged sources are freed)
        if (!atlas || !atlas->IsBuilt()) return;
        for (ImFontConfig& c : atlas->ConfigData)
            if (c.MergeMode && c.FontDataOwnedByAtlas && c.FontData) { IM_FREE(c.FontData); c.FontData = nullptr; c.FontDataSize = 0; }
    }
}
