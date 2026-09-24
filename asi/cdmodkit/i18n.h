#pragma once

#include <cstddef>
#include <string>

struct ImFontAtlas;

// UI translations from bin64\cdmodkit\locales.tsv (English text = key, English is the fallback).
// UI code marks every translatable string explicitly: T("text"), or TStable("text") for labels whose ImGui ID must not
// change with the language (tabs, popups, headers). A translated format keeps its printf placeholders (check_locales.py).
namespace i18n {
    struct LanguageOption { const char* id; const char* name; };

    void Initialize(const std::string& modDir);
    const char* Translate(const char* source);   // leading icons / blanks and a trailing "##id" are kept, not part of the key
    const char* T(const char* english);
    const char* TStable(const char* english);    // "translated###english"
    const char* Preference();
    std::string ActiveLanguage();   // resolved id ("auto" becomes the system language), e.g. "de"
    const LanguageOption* Languages(int* count);  // names: native spelling first, then the name in the current UI language
    bool SetPreference(const char* id);
    const unsigned short* GlyphRanges();
    void MergeSystemFonts(ImFontAtlas* atlas, float size);
    void ReleaseMergedFontData(ImFontAtlas* atlas);   // after the atlas was built; it must not be built again
}
