// Icons drawn by the plugin itself at startup (icons.cpp) into the ImGui font atlas: no icon font, no third-party assets.
// The codepoints live in the Unicode private use area and are used inside normal UI strings.
#pragma once
struct ImFontAtlas; struct ImFont;
#define ICON_STAR                "\xee\x80\x80"   // U+E000
#define ICON_CUBE                "\xee\x80\x81"   // U+E001
#define ICON_TRASH               "\xee\x80\x82"   // U+E002
#define ICON_LOCATION_DOT        "\xee\x80\x83"   // U+E003
#define ICON_FLOPPY_DISK         "\xee\x80\x84"   // U+E004
#define ICON_XMARK               "\xee\x80\x85"   // U+E005
#define ICON_RULER_COMBINED      "\xee\x80\x86"   // U+E006
#define ICON_MAGNIFYING_GLASS    "\xee\x80\x87"   // U+E007
#define ICON_LOCATION_CROSSHAIRS "\xee\x80\x88"   // U+E008
#define ICON_LIST                "\xee\x80\x89"   // U+E009
#define ICON_HAND                "\xee\x80\x8a"   // U+E00A
#define ICON_FOLDER_TREE         "\xee\x80\x8b"   // U+E00B
#define ICON_EYE                 "\xee\x80\x8c"   // U+E00C
#define ICON_COPY                "\xee\x80\x8d"   // U+E00D
#define ICON_CLOCK_ROTATE_LEFT   "\xee\x80\x8e"   // U+E00E
#define ICON_CIRCLE_CHECK        "\xee\x80\x8f"   // U+E00F
#define ICON_CIRCLE_INFO         "\xee\x80\x90"   // U+E010
namespace icons {
    // 1) before atlas->Build(): reserves one custom glyph per icon in `font` (icon size derived from fontSize)
    void Register(ImFontAtlas* atlas, ImFont* font, float fontSize);
    // 2) after atlas->Build(), before the backend uploads the texture: rasterizes the icons into the atlas pixels
    void Paint(ImFontAtlas* atlas);
}
