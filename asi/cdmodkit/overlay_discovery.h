// Bounded PE read/discovery for the main module: the validated image range, the RVA readers and the
// game-boundary resolve scan, extracted from overlay.cpp. State-free by design: callers pass the image base
// (overlay.cpp keeps g_os) and every access is bounds-checked against the mapped range before it happens.
#pragma once
#include <windows.h>
#include <cstddef>
#include <cstdint>

namespace overlay::discovery {

// A validated main-image range: base/size come from the caller's module lookup and ImageBounds' checks.
struct ImageInfo { uintptr_t base = 0; size_t size = 0; };

// Validates `base` as a mapped image and fills `img` (header, directory, section table and page checks).
//
// Consumer split (Task 3): ImageCoreBounds validates exactly what a bounded scanning consumer needs - a mapped,
// fully committed PE64 image whose headers and section table are readable with at least one executable section.
// ImageBounds adds the overlay's BINDING eligibility on top: an import directory inside the image that lies in a
// declared section, and the fixed SectionAlignment 0x1000 / FileAlignment 0x200 layout the binding route requires.
// Neither decides what is scanned: GameBoundaryScan's access flag selects the sections.
bool ImageCoreBounds(uintptr_t base, ImageInfo* img);
bool ImageBounds(uintptr_t base, ImageInfo* img);
// RVA + length inside the mapped image, no unsigned wrap.
bool ImageRange(const ImageInfo& img, uintptr_t off, size_t n);
// Bounds before access: the range is checked, then the bytes are copied with core::ReadBytes.
bool ImageRead(const ImageInfo& img, uintptr_t off, void* out, size_t n);
// NUL-terminated string inside the image; false when unterminated or unreadable.
bool ImageCString(const ImageInfo& img, uintptr_t off, char* out, size_t cap);

// Pattern string ("48 8D ? 05 ...") -> value/mask bytes for GameBoundaryScan (wildcards match any byte);
// returns the byte count.
int GameBoundaryParsePattern(const char* pat, unsigned char* val, unsigned char* mask);
// Chunked scan over the sections carrying the requested access; *complete is set only when every planned read
// succeeded, so a partial scan can never prove a unique anchor.
uintptr_t GameBoundaryScan(const ImageInfo& img, bool executable, const unsigned char* val, const unsigned char* mask, int nlen, int* count, bool* complete);
// Function start for an RVA inside a function, through the PE exception directory (bounded chain walk).
uintptr_t GameBoundaryFuncStart(const ImageInfo& img, uintptr_t rva);
// Function head whose code references the given string with exactly one lea rip+disp32; *complete as above.
uintptr_t GameBoundaryFuncReferencingString(const ImageInfo& img, const char* s, bool* complete);

}   // namespace overlay::discovery
