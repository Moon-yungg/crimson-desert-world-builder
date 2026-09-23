#pragma once
#include "i18n.h"
#include <imgui.h>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace ImGui {
    inline void TextLocalized(const char* fmt, ...) { va_list args; va_start(args, fmt); TextV(i18n::Translate(fmt), args); va_end(args); }
    inline void TextWrappedLocalized(const char* fmt, ...) { PushTextWrapPos(0.0f); va_list args; va_start(args, fmt); TextV(i18n::Translate(fmt), args); va_end(args); PopTextWrapPos(); }
    inline void TextDisabledLocalized(const char* fmt, ...) { PushStyleColor(ImGuiCol_Text, GetStyleColorVec4(ImGuiCol_TextDisabled)); va_list args; va_start(args, fmt); TextV(i18n::Translate(fmt), args); va_end(args); PopStyleColor(); }
    inline void TextColoredLocalized(const ImVec4& color, const char* fmt, ...) { PushStyleColor(ImGuiCol_Text, color); va_list args; va_start(args, fmt); TextV(i18n::Translate(fmt), args); va_end(args); PopStyleColor(); }
    inline void TextUnformattedLocalized(const char* text, const char* textEnd = nullptr) { if (!textEnd) TextUnformatted(i18n::Translate(text)); else TextUnformatted(text, textEnd); }
    inline void SetTooltipLocalized(const char* fmt, ...) { if (!BeginTooltip()) return; va_list args; va_start(args, fmt); TextV(i18n::Translate(fmt), args); va_end(args); EndTooltip(); }
    inline bool BeginPopupModalLocalized(const char* name, bool* open = nullptr, ImGuiWindowFlags flags = 0) { return BeginPopupModal(i18n::Translate(name), open, flags); }
    inline bool BeginTabItemLocalized(const char* label, bool* open = nullptr, ImGuiTabItemFlags flags = 0) { return BeginTabItem(i18n::Translate(label), open, flags); }
    inline bool ButtonLocalized(const char* label, const ImVec2& size = ImVec2(0, 0)) { return Button(i18n::Translate(label), size); }
    inline bool SmallButtonLocalized(const char* label) { return SmallButton(i18n::Translate(label)); }
    inline bool CheckboxLocalized(const char* label, bool* value) { return Checkbox(i18n::Translate(label), value); }
    inline bool RadioButtonLocalized(const char* label, bool active) { return RadioButton(i18n::Translate(label), active); }
    inline bool SelectableLocalized(const char* label, bool selected = false, ImGuiSelectableFlags flags = 0, const ImVec2& size = ImVec2(0, 0)) { return Selectable(i18n::Translate(label), selected, flags, size); }
    inline bool MenuItemLocalized(const char* label, const char* shortcut = nullptr, bool selected = false, bool enabled = true) { return MenuItem(i18n::Translate(label), shortcut, selected, enabled); }
    inline bool BeginComboLocalized(const char* label, const char* preview, ImGuiComboFlags flags = 0) { return BeginCombo(i18n::Translate(label), i18n::Translate(preview), flags); }
    inline bool InputTextLocalized(const char* label, char* buf, size_t size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* userData = nullptr) { return InputText(i18n::Translate(label), buf, size, flags, callback, userData); }
    inline bool InputTextWithHintLocalized(const char* label, const char* hint, char* buf, size_t size, ImGuiInputTextFlags flags = 0, ImGuiInputTextCallback callback = nullptr, void* userData = nullptr) { return InputTextWithHint(i18n::Translate(label), i18n::Translate(hint), buf, size, flags, callback, userData); }
    inline bool InputIntLocalized(const char* label, int* value, int step = 1, int stepFast = 100, ImGuiInputTextFlags flags = 0) { return InputInt(i18n::Translate(label), value, step, stepFast, flags); }
    inline bool DragFloatLocalized(const char* label, float* value, float speed = 1.0f, float min = 0.0f, float max = 0.0f, const char* format = "%.3f", ImGuiSliderFlags flags = 0) { return DragFloat(i18n::Translate(label), value, speed, min, max, format, flags); }
    inline bool DragFloat3Localized(const char* label, float value[3], float speed = 1.0f, float min = 0.0f, float max = 0.0f, const char* format = "%.3f", ImGuiSliderFlags flags = 0) { return DragFloat3(i18n::Translate(label), value, speed, min, max, format, flags); }
    inline bool SliderFloatLocalized(const char* label, float* value, float min, float max, const char* format = "%.3f", ImGuiSliderFlags flags = 0) { return SliderFloat(i18n::Translate(label), value, min, max, format, flags); }
    inline bool SliderIntLocalized(const char* label, int* value, int min, int max, const char* format = "%d", ImGuiSliderFlags flags = 0) { return SliderInt(i18n::Translate(label), value, min, max, format, flags); }
    inline bool CollapsingHeaderLocalized(const char* label, ImGuiTreeNodeFlags flags = 0) { return CollapsingHeader(i18n::Translate(label), flags); }
    inline bool TreeNodeExLocalized(const char* label, ImGuiTreeNodeFlags flags = 0) { return TreeNodeEx(i18n::Translate(label), flags); }
    inline void TableSetupColumnLocalized(const char* label, ImGuiTableColumnFlags flags = 0, float width = 0.0f, ImGuiID id = 0) { TableSetupColumn(i18n::Translate(label), flags, width, id); }
    inline bool ComboLocalized(const char* label, int* current, const char* const items[], int count) {
        std::vector<std::string> storage; std::vector<const char*> localized;
        storage.reserve((size_t)count); localized.reserve((size_t)count);
        for (int i = 0; i < count; ++i) storage.emplace_back(i18n::Translate(items[i]));
        for (const auto& item : storage) localized.push_back(item.c_str());
        return Combo(i18n::Translate(label), current, localized.data(), count);
    }
}

#define Text(...) TextLocalized(__VA_ARGS__)
#define TextWrapped(...) TextWrappedLocalized(__VA_ARGS__)
#define TextDisabled(...) TextDisabledLocalized(__VA_ARGS__)
#define TextColored(...) TextColoredLocalized(__VA_ARGS__)
#define TextUnformatted(...) TextUnformattedLocalized(__VA_ARGS__)
#define SetTooltip(...) SetTooltipLocalized(__VA_ARGS__)
#define BeginPopupModal(...) BeginPopupModalLocalized(__VA_ARGS__)
#define BeginTabItem(...) BeginTabItemLocalized(__VA_ARGS__)
#define Button(...) ButtonLocalized(__VA_ARGS__)
#define SmallButton(...) SmallButtonLocalized(__VA_ARGS__)
#define Checkbox(...) CheckboxLocalized(__VA_ARGS__)
#define RadioButton(...) RadioButtonLocalized(__VA_ARGS__)
#define Selectable(...) SelectableLocalized(__VA_ARGS__)
#define MenuItem(...) MenuItemLocalized(__VA_ARGS__)
#define BeginCombo(...) BeginComboLocalized(__VA_ARGS__)
#define InputText(...) InputTextLocalized(__VA_ARGS__)
#define InputTextWithHint(...) InputTextWithHintLocalized(__VA_ARGS__)
#define InputInt(...) InputIntLocalized(__VA_ARGS__)
#define DragFloat(...) DragFloatLocalized(__VA_ARGS__)
#define DragFloat3(...) DragFloat3Localized(__VA_ARGS__)
#define SliderFloat(...) SliderFloatLocalized(__VA_ARGS__)
#define SliderInt(...) SliderIntLocalized(__VA_ARGS__)
#define CollapsingHeader(...) CollapsingHeaderLocalized(__VA_ARGS__)
#define TreeNodeEx(...) TreeNodeExLocalized(__VA_ARGS__)
#define TableSetupColumn(...) TableSetupColumnLocalized(__VA_ARGS__)
#define Combo(...) ComboLocalized(__VA_ARGS__)
