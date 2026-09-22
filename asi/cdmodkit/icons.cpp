// Procedural icon glyphs: every icon is a small composition of circles, segments, rectangles and polygons rasterized with
// 4x4 supersampling into an alpha mask, then copied into the ImGui font atlas as a custom glyph (white, alpha = coverage).
#include "icons.h"
#include <imgui.h>
#include <vector>
#include <cmath>
#include <functional>
#include <algorithm>

namespace icons {
    static const int kCount = 17;
    static int g_rectId[kCount];
    static int g_size = 0;

    // ---- tiny shape language in unit coordinates (0..1, y down) ----
    using Shape = std::function<bool(float, float)>;
    static Shape Circle(float cx, float cy, float r) { return [=](float x, float y) { float dx = x - cx, dy = y - cy; return dx * dx + dy * dy <= r * r; }; }
    static Shape Ring(float cx, float cy, float r, float t) { return [=](float x, float y) { float d = sqrtf((x - cx) * (x - cx) + (y - cy) * (y - cy)); return fabsf(d - r) <= t * 0.5f; }; }
    static Shape Rect(float x0, float y0, float x1, float y1) { return [=](float x, float y) { return x >= x0 && x <= x1 && y >= y0 && y <= y1; }; }
    static Shape Seg(float x0, float y0, float x1, float y1, float t) {
        return [=](float x, float y) {
            float vx = x1 - x0, vy = y1 - y0, l2 = vx * vx + vy * vy; float u = l2 > 0 ? ((x - x0) * vx + (y - y0) * vy) / l2 : 0; u = std::max(0.0f, std::min(1.0f, u));
            float px = x0 + u * vx - x, py = y0 + u * vy - y; return px * px + py * py <= t * t * 0.25f;
        };
    }
    static Shape Poly(std::vector<ImVec2> pts) {   // even-odd fill
        return [=](float x, float y) {
            bool in = false; size_t n = pts.size();
            for (size_t i = 0, j = n - 1; i < n; j = i++) {
                if ((pts[i].y > y) != (pts[j].y > y) && x < (pts[j].x - pts[i].x) * (y - pts[i].y) / (pts[j].y - pts[i].y) + pts[i].x) in = !in;
            }
            return in;
        };
    }
    static Shape Or(Shape a, Shape b) { return [=](float x, float y) { return a(x, y) || b(x, y); }; }
    static Shape And(Shape a, Shape b) { return [=](float x, float y) { return a(x, y) && b(x, y); }; }
    static Shape Not(Shape a) { return [=](float x, float y) { return !a(x, y); }; }
    static Shape Sector(float cx, float cy, float a0, float a1) {   // angles in degrees, y down, 0 = +x, counter-clockwise on screen
        return [=](float x, float y) { float a = atan2f(-(y - cy), x - cx) * 180.0f / 3.14159265f; if (a < 0) a += 360; float b0 = a0, b1 = a1; if (b0 <= b1) return a >= b0 && a <= b1; return a >= b0 || a <= b1; };
    }

    struct Canvas {
        int w, h; std::vector<float> cov;
        Canvas(int s) : w(s), h(s), cov((size_t)s * s, 0.0f) {}
        void Fill(const Shape& s) { Apply(s, false); }
        void Cut(const Shape& s) { Apply(s, true); }
        void Apply(const Shape& s, bool cut) {
            const int SS = 4;
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                int hit = 0;
                for (int sy = 0; sy < SS; sy++) for (int sx = 0; sx < SS; sx++) if (s((x + (sx + 0.5f) / SS) / w, (y + (sy + 0.5f) / SS) / h)) hit++;
                float c = hit / float(SS * SS); float& d = cov[(size_t)y * w + x];
                d = cut ? d * (1.0f - c) : std::max(d, c);
            }
        }
    };

    static void Draw(int icon, Canvas& c) {
        switch (icon) {
        case 0: {   // star
            std::vector<ImVec2> p; for (int i = 0; i < 10; i++) { float a = -90.0f + i * 36.0f; float r = (i & 1) ? 0.2f : 0.47f; p.push_back(ImVec2(0.5f + r * cosf(a * 3.14159265f / 180), 0.53f + r * sinf(a * 3.14159265f / 180))); }
            c.Fill(Poly(p)); break; }
        case 1: {   // cube (isometric hexagon with three faces)
            std::vector<ImVec2> hex; for (int i = 0; i < 6; i++) { float a = -90.0f + i * 60.0f; hex.push_back(ImVec2(0.5f + 0.45f * cosf(a * 3.14159265f / 180), 0.5f + 0.45f * sinf(a * 3.14159265f / 180))); }
            c.Fill(Poly(hex));
            c.Cut(Seg(0.5f, 0.5f, 0.5f, 0.95f, 0.07f)); c.Cut(Seg(0.5f, 0.5f, hex[1].x, hex[1].y, 0.07f)); c.Cut(Seg(0.5f, 0.5f, hex[5].x, hex[5].y, 0.07f));
            break; }
        case 2: {   // trash can
            c.Fill(Rect(0.15f, 0.17f, 0.85f, 0.26f)); c.Fill(Rect(0.38f, 0.08f, 0.62f, 0.17f));
            c.Fill(Poly({ ImVec2(0.2f, 0.32f), ImVec2(0.8f, 0.32f), ImVec2(0.74f, 0.94f), ImVec2(0.26f, 0.94f) }));
            c.Cut(Seg(0.4f, 0.42f, 0.4f, 0.84f, 0.06f)); c.Cut(Seg(0.6f, 0.42f, 0.6f, 0.84f, 0.06f));
            break; }
        case 3: {   // map pin
            c.Fill(Circle(0.5f, 0.38f, 0.3f)); c.Fill(Poly({ ImVec2(0.24f, 0.5f), ImVec2(0.76f, 0.5f), ImVec2(0.5f, 0.96f) })); c.Cut(Circle(0.5f, 0.38f, 0.12f));
            break; }
        case 4: {   // floppy disk
            c.Fill(Poly({ ImVec2(0.1f, 0.1f), ImVec2(0.78f, 0.1f), ImVec2(0.9f, 0.22f), ImVec2(0.9f, 0.9f), ImVec2(0.1f, 0.9f) }));
            c.Cut(Rect(0.28f, 0.1f, 0.68f, 0.34f)); c.Fill(Rect(0.52f, 0.14f, 0.62f, 0.3f));
            c.Cut(Rect(0.24f, 0.58f, 0.76f, 0.9f)); c.Fill(Rect(0.3f, 0.64f, 0.7f, 0.9f));
            break; }
        case 5: {   // x mark
            c.Fill(Seg(0.2f, 0.2f, 0.8f, 0.8f, 0.17f)); c.Fill(Seg(0.8f, 0.2f, 0.2f, 0.8f, 0.17f)); break; }
        case 6: {   // ruler (L shape with ticks)
            c.Fill(Rect(0.08f, 0.08f, 0.36f, 0.92f)); c.Fill(Rect(0.08f, 0.64f, 0.92f, 0.92f));
            for (int i = 0; i < 4; i++) { float y = 0.18f + i * 0.12f; c.Cut(Rect(0.08f, y, i & 1 ? 0.2f : 0.26f, y + 0.05f)); }
            for (int i = 0; i < 4; i++) { float x = 0.44f + i * 0.12f; c.Cut(Rect(x, i & 1 ? 0.8f : 0.74f, x + 0.05f, 0.92f)); }
            break; }
        case 7: {   // magnifying glass
            c.Fill(Ring(0.42f, 0.42f, 0.27f, 0.12f)); c.Fill(Seg(0.62f, 0.62f, 0.9f, 0.9f, 0.16f)); break; }
        case 8: {   // crosshairs
            c.Fill(Ring(0.5f, 0.5f, 0.27f, 0.1f)); c.Fill(Circle(0.5f, 0.5f, 0.1f));
            c.Fill(Seg(0.5f, 0.05f, 0.5f, 0.24f, 0.1f)); c.Fill(Seg(0.5f, 0.76f, 0.5f, 0.95f, 0.1f)); c.Fill(Seg(0.05f, 0.5f, 0.24f, 0.5f, 0.1f)); c.Fill(Seg(0.76f, 0.5f, 0.95f, 0.5f, 0.1f));
            break; }
        case 9: {   // list
            for (int i = 0; i < 3; i++) { float y = 0.22f + i * 0.28f; c.Fill(Circle(0.15f, y, 0.075f)); c.Fill(Rect(0.32f, y - 0.065f, 0.92f, y + 0.065f)); }
            break; }
        case 10: {  // hand
            c.Fill(Circle(0.5f, 0.7f, 0.26f)); c.Fill(Rect(0.24f, 0.5f, 0.76f, 0.72f));
            c.Fill(Rect(0.25f, 0.2f, 0.34f, 0.6f)); c.Fill(Rect(0.37f, 0.1f, 0.46f, 0.6f)); c.Fill(Rect(0.49f, 0.13f, 0.58f, 0.6f)); c.Fill(Rect(0.61f, 0.22f, 0.70f, 0.6f));
            c.Fill(Seg(0.26f, 0.66f, 0.08f, 0.45f, 0.11f));
            break; }
        case 11: {  // folder tree
            c.Fill(Poly({ ImVec2(0.05f, 0.1f), ImVec2(0.2f, 0.1f), ImVec2(0.25f, 0.16f), ImVec2(0.45f, 0.16f), ImVec2(0.45f, 0.4f), ImVec2(0.05f, 0.4f) }));
            c.Fill(Seg(0.15f, 0.4f, 0.15f, 0.82f, 0.06f)); c.Fill(Seg(0.15f, 0.55f, 0.42f, 0.55f, 0.06f)); c.Fill(Seg(0.15f, 0.82f, 0.42f, 0.82f, 0.06f));
            c.Fill(Rect(0.48f, 0.45f, 0.95f, 0.65f)); c.Fill(Rect(0.48f, 0.72f, 0.95f, 0.92f));
            break; }
        case 12: {  // eye
            c.Fill(And(Circle(0.5f, 0.95f, 0.66f), Circle(0.5f, 0.05f, 0.66f))); c.Cut(Circle(0.5f, 0.5f, 0.2f)); c.Fill(Circle(0.5f, 0.5f, 0.11f));
            break; }
        case 13: {  // copy (two sheets)
            c.Fill(And(Rect(0.1f, 0.06f, 0.64f, 0.64f), Not(Rect(0.2f, 0.16f, 0.54f, 0.54f)))); c.Cut(Rect(0.34f, 0.32f, 0.92f, 0.94f)); c.Fill(Rect(0.36f, 0.34f, 0.9f, 0.92f));
            break; }
        case 14: {  // clock rotate left (history)
            c.Fill(And(Ring(0.54f, 0.52f, 0.36f, 0.1f), Not(Sector(0.54f, 0.52f, 150.0f, 215.0f))));
            c.Fill(Poly({ ImVec2(0.06f, 0.36f), ImVec2(0.3f, 0.36f), ImVec2(0.18f, 0.56f) }));
            c.Fill(Seg(0.54f, 0.52f, 0.54f, 0.3f, 0.08f)); c.Fill(Seg(0.54f, 0.52f, 0.7f, 0.62f, 0.08f));
            break; }
        case 15: {  // circle check
            c.Fill(Circle(0.5f, 0.5f, 0.45f)); c.Cut(Or(Seg(0.27f, 0.52f, 0.43f, 0.68f, 0.12f), Seg(0.43f, 0.68f, 0.74f, 0.36f, 0.12f)));
            break; }
        case 16: {  // circle info (i)
            c.Fill(Ring(0.5f, 0.5f, 0.42f, 0.09f)); c.Fill(Circle(0.5f, 0.3f, 0.075f)); c.Fill(Rect(0.44f, 0.42f, 0.56f, 0.74f));
            break; }
        }
    }

    void Register(ImFontAtlas* atlas, ImFont* font, float fontSize) {
        g_size = (int)(fontSize * 0.9f + 0.5f); if (g_size < 8) g_size = 8;
        const float offY = (fontSize - g_size) * 0.5f + 1.0f;
        for (int i = 0; i < kCount; i++)
            g_rectId[i] = atlas->AddCustomRectFontGlyph(font, (ImWchar)(0xE000 + i), g_size, g_size, (float)g_size + 3.0f, ImVec2(1.0f, offY));
    }
    void Paint(ImFontAtlas* atlas) {
        unsigned char* pixels = nullptr; int w = 0, h = 0;
        atlas->GetTexDataAsRGBA32(&pixels, &w, &h);
        if (!pixels) return;
        for (int i = 0; i < kCount; i++) {
            const ImFontAtlasCustomRect* r = atlas->GetCustomRectByIndex(g_rectId[i]);
            if (!r || !r->IsPacked()) continue;
            Canvas c(g_size); Draw(i, c);
            for (int y = 0; y < r->Height && y < g_size; y++) for (int x = 0; x < r->Width && x < g_size; x++) {
                unsigned char* p = pixels + ((size_t)(r->Y + y) * w + (r->X + x)) * 4;
                p[0] = p[1] = p[2] = 255; p[3] = (unsigned char)(std::min(1.0f, c.cov[(size_t)y * g_size + x]) * 255.0f + 0.5f);
            }
        }
    }
}
