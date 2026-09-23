#pragma once

#include <cstdarg>
#include <cstddef>
#include <string>

struct ImFontAtlas;

namespace i18n {
    struct LanguageOption { const char* id; const char* name; };

    void Initialize(const std::string& modDir);
    const char* Translate(const char* source);
    const char* Preference();
    const char* ActiveLanguage();
    const LanguageOption* Languages(int* count);
    bool SetPreference(const char* id);
    const unsigned short* GlyphRanges();
    void MergeSystemFonts(ImFontAtlas* atlas, float size);
    void FormatV(char* out, size_t capacity, const char* source, va_list args);
}
