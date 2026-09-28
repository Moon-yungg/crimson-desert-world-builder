// Bounded PE read/discovery moved out of overlay.cpp: the validated image range, the RVA readers and the
// game-boundary resolve scan. Behavior-preserving extraction: the bounds checks, the fail-closed results and
// the log lines are the ones the overlay already emitted; the image base is the caller's argument.
#include "overlay_discovery.h"
#include "core.h"

#include <cstring>
#include <vector>

namespace overlay::discovery {
    static const char kMarker[] = "WB_BINDING_FUNCTIONAL";   // same marker text the overlay log lines carry

    // The bound is the mapped image range, not a fixed ceiling: a real main module (the shipped game image is
    // 0x173ab000 = 389 722 112 bytes) is accepted, while an unreadable or unmapped range is not.
    static bool ImageMapped(ImageInfo img) {
        if (!img.base || !img.size) return false;
        MEMORY_BASIC_INFORMATION mbi = {};
        if (VirtualQuery(reinterpret_cast<const void*>(img.base), &mbi, sizeof mbi) != sizeof mbi) return false;
        if (mbi.State != MEM_COMMIT) return false;
        const DWORD protect = mbi.Protect & 0xff;
        if (protect == PAGE_NOACCESS || protect == PAGE_EXECUTE) return false;
        if (mbi.AllocationBase != reinterpret_cast<void*>(img.base)) return false;   // the image is its own allocation, not a run of adjacent readable regions
        size_t checked = 0;
        uintptr_t at = img.base;
        while (checked < img.size) {                                          // every page of the claim must be committed in that same allocation
            MEMORY_BASIC_INFORMATION part = {};
            if (VirtualQuery(reinterpret_cast<const void*>(at), &part, sizeof part) != sizeof part) return false;
            if (part.State != MEM_COMMIT) return false;
            if (part.AllocationBase != reinterpret_cast<void*>(img.base)) return false;   // a different allocation ends the image
            const DWORD p2 = part.Protect & 0xff;
            if (p2 == PAGE_NOACCESS || p2 == PAGE_EXECUTE) return false;
            if (!part.RegionSize) return false;
            checked += part.RegionSize;
            at += part.RegionSize;
        }
        return checked >= img.size;
    }
    // What a caller needs to decide its OWN eligibility, collected by the one bounded structural walk so neither
    // consumer duplicates the PE checks.
    struct ImageFacts {
        IMAGE_DATA_DIRECTORY importDir = {};   // raw import-directory entry (validated only by the binding layer)
        uint32_t numberOfRvaAndSizes = 0;      // header field the import-directory index is checked against
        uint32_t sectionAlignment = 0, fileAlignment = 0;   // reported, not enforced: only binding requires 0x1000/0x200
        bool hasImportEntry = false;           // NumberOfRvaAndSizes covers IMAGE_DIRECTORY_ENTRY_IMPORT
        bool haveText = false;                 // at least one executable section
        bool importInSection = false;          // the import-directory RVA lies inside a declared section
        bool standardAlignment = false;        // SectionAlignment 0x1000 and FileAlignment 0x200
    };
    // Structural validation shared by both consumers: mapped and fully committed range, DOS/NT/PE64 headers, the
    // header window and the section table read through the bounded reader. It REPORTS the binding layout facts
    // instead of deciding them: import-directory presence/size, the section containing it and the fixed alignment
    // are eligibility for the overlay's binding route only (ImageBounds), while the wrapper scanners need bounded
    // access and completeness (ImageCoreBounds). The executable-section requirement is left to the callers because
    // each reports it in its own diagnostic.
    static bool ImageStructure(uintptr_t base, ImageInfo* img, ImageFacts* facts) {
        if (!base) return false;
        IMAGE_DOS_HEADER dos = {};
        if (!core::ReadBytes(base, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) { core::Log("[overlay] %s delegate install reason image_reject dos", kMarker); return false; }
        if (dos.e_lfanew <= 0 || dos.e_lfanew > 0x1000) { core::Log("[overlay] %s delegate install reason image_reject lfanew %ld", kMarker, (long)dos.e_lfanew); return false; }
        IMAGE_NT_HEADERS64 nt = {};
        if (!core::ReadBytes(base + static_cast<uintptr_t>(dos.e_lfanew), &nt, sizeof nt)) { core::Log("[overlay] %s delegate install reason image_reject nt_read", kMarker); return false; }
        if (nt.Signature != IMAGE_NT_SIGNATURE) { core::Log("[overlay] %s delegate install reason image_reject signature 0x%08x", kMarker, (unsigned)nt.Signature); return false; }
        if (nt.FileHeader.Machine != IMAGE_FILE_MACHINE_AMD64) { core::Log("[overlay] %s delegate install reason image_reject machine 0x%04x", kMarker, (unsigned)nt.FileHeader.Machine); return false; }
        if (nt.OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC) { core::Log("[overlay] %s delegate install reason image_reject magic 0x%04x", kMarker, (unsigned)nt.OptionalHeader.Magic); return false; }
        if (nt.OptionalHeader.SizeOfImage < 0x1000) { core::Log("[overlay] %s delegate install reason image_reject size 0x%x", kMarker, (unsigned)nt.OptionalHeader.SizeOfImage); return false; }
        if (nt.FileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64) || (nt.FileHeader.SizeOfOptionalHeader % 8) != 0) { core::Log("[overlay] %s delegate install reason image_reject optional_header %u", kMarker, (unsigned)nt.FileHeader.SizeOfOptionalHeader); return false; }
        const uintptr_t headerEnd = static_cast<uintptr_t>(dos.e_lfanew) + sizeof(IMAGE_NT_HEADERS64);
        if (nt.OptionalHeader.SizeOfHeaders < headerEnd || nt.OptionalHeader.SizeOfHeaders > nt.OptionalHeader.SizeOfImage) { core::Log("[overlay] %s delegate install reason image_reject headers 0x%x end 0x%llx", kMarker, (unsigned)nt.OptionalHeader.SizeOfHeaders, (unsigned long long)headerEnd); return false; }
        facts->numberOfRvaAndSizes = nt.OptionalHeader.NumberOfRvaAndSizes;
        facts->sectionAlignment = nt.OptionalHeader.SectionAlignment;
        facts->fileAlignment = nt.OptionalHeader.FileAlignment;
        facts->standardAlignment = (nt.OptionalHeader.SectionAlignment == 0x1000 && nt.OptionalHeader.FileAlignment == 0x200);
        facts->hasImportEntry = nt.OptionalHeader.NumberOfRvaAndSizes > IMAGE_DIRECTORY_ENTRY_IMPORT;
        facts->importDir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        const IMAGE_DATA_DIRECTORY dir = facts->importDir;
        // The PE format imposes no alignment on the import-directory RVA (the shipped image's is 0x170d322e, mod 4 == 2)
        // and every read below goes through core::ReadBytes, which is a byte copy: bounds are enforced, alignment is not.
        // How that directory is validated is the binding layer's decision, not this walk's: see ImageBounds.
        // Section headers are read from the mapped image (never from a local header copy), bounds-checked per entry.
        if (nt.FileHeader.NumberOfSections == 0 || nt.FileHeader.NumberOfSections > 96) { core::Log("[overlay] %s delegate install reason image_reject sections %u", kMarker, (unsigned)nt.FileHeader.NumberOfSections); return false; }
        const uintptr_t secBase = static_cast<uintptr_t>(dos.e_lfanew) + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
        const size_t secSpan = static_cast<size_t>(nt.FileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        // The section table's own offset is read with byte copies as well: only its bounds matter, not its alignment.
        if (secBase > nt.OptionalHeader.SizeOfHeaders || secSpan > nt.OptionalHeader.SizeOfHeaders - secBase) { core::Log("[overlay] %s delegate install reason image_reject section_array 0x%llx span %llu headers 0x%x", kMarker, (unsigned long long)secBase, (unsigned long long)secSpan, (unsigned)nt.OptionalHeader.SizeOfHeaders); return false; }
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; i++) {
            IMAGE_SECTION_HEADER sc = {};
            if (!core::ReadBytes(base + secBase + static_cast<uintptr_t>(i) * sizeof(IMAGE_SECTION_HEADER), &sc, sizeof sc)) { core::Log("[overlay] %s delegate install reason image_reject section_read %u", kMarker, i); return false; }
            const uintptr_t lo = sc.VirtualAddress;
            const uintptr_t hi = static_cast<uintptr_t>(sc.VirtualAddress) + sc.Misc.VirtualSize;
            if (hi > nt.OptionalHeader.SizeOfImage) { core::Log("[overlay] %s delegate install reason image_reject section_range 0x%x+0x%x image 0x%x", kMarker, (unsigned)sc.VirtualAddress, (unsigned)sc.Misc.VirtualSize, (unsigned)nt.OptionalHeader.SizeOfImage); return false; }
            if ((sc.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) facts->haveText = true;
            if (dir.VirtualAddress >= lo && dir.VirtualAddress < hi) facts->importInSection = true;
        }
        img->base = base; img->size = nt.OptionalHeader.SizeOfImage;
        if (!ImageMapped(*img)) { core::Log("[overlay] %s delegate install reason image_range_unmapped base %p size 0x%llx", kMarker, (void*)base, (unsigned long long)img->size); return false; }
        return true;
    }
    // Core consumer: bounded access and completeness. No import-directory, containing-section or fixed-alignment
    // eligibility - a code scanner needs a readable PE64 image with somewhere to scan, nothing about the overlay's
    // binding layout.
    bool ImageCoreBounds(uintptr_t base, ImageInfo* img) {
        ImageFacts facts;
        if (!ImageStructure(base, img, &facts)) return false;
        if (!facts.haveText) { core::Log("[overlay] %s delegate install reason image_reject text 0", kMarker); return false; }
        return true;
    }
    // Binding consumer: the core bounds plus the layout facts the overlay's import-directory walk and tap
    // installation depend on. Every rejection below is binding eligibility, never a scanning decision.
    bool ImageBounds(uintptr_t base, ImageInfo* img) {
        ImageFacts facts;
        if (!ImageStructure(base, img, &facts)) return false;
        const IMAGE_DATA_DIRECTORY dir = facts.importDir;
        if (!facts.hasImportEntry) { core::Log("[overlay] %s delegate install reason image_reject rva_count %u", kMarker, (unsigned)facts.numberOfRvaAndSizes); return false; }
        if (!dir.VirtualAddress || dir.Size < sizeof(IMAGE_IMPORT_DESCRIPTOR) ||
            dir.VirtualAddress > img->size ||
            dir.Size > img->size - dir.VirtualAddress) { core::Log("[overlay] %s delegate install reason image_reject import_dir 0x%x/0x%x size 0x%x", kMarker, (unsigned)dir.VirtualAddress, (unsigned)dir.Size, (unsigned)img->size); return false; }
        if (!facts.standardAlignment) { core::Log("[overlay] %s delegate install reason image_reject align 0x%x/0x%x", kMarker, (unsigned)facts.sectionAlignment, (unsigned)facts.fileAlignment); return false; }
        if (!facts.haveText || !facts.importInSection) { core::Log("[overlay] %s delegate install reason image_reject text %d containing %d", kMarker, facts.haveText ? 1 : 0, facts.importInSection ? 1 : 0); return false; }
        return true;
    }
    bool ImageRange(const ImageInfo& img, uintptr_t off, size_t n) {   // RVA + length inside the mapped image, no unsigned wrap
        return off <= img.size && n <= img.size - off;
    }
    bool ImageRead(const ImageInfo& img, uintptr_t off, void* out, size_t n) {
        if (!ImageRange(img, off, n)) return false;                  // bounds before access
        return core::ReadBytes(img.base + off, out, n);
    }
    bool ImageCString(const ImageInfo& img, uintptr_t off, char* out, size_t cap) {
        if (!out || cap < 2 || off >= img.size) return false;
        for (size_t i = 0; i + 1 < cap; i++) {
            char c = 0;
            if (!ImageRead(img, off + i, &c, 1)) return false;
            out[i] = c;
            if (!c) return i != 0;
        }
        out[cap - 1] = 0;
        return false;                                       // no terminator inside the bound: not a name we accept
    }

    // Pattern-string -> value/mask bytes for GameBoundaryScan (wildcards match any byte).
    int GameBoundaryParsePattern(const char* pat, unsigned char* val, unsigned char* mask) {
        int n = 0;
        for (const char* s = pat; *s && n < 64;) {
            while (*s == ' ') s++;
            if (!*s) break;
            if (*s == '?') { val[n] = 0; mask[n] = 0; n++; while (*s == '?') s++; }
            else { val[n] = (unsigned char)strtoul(s, const_cast<char**>(&s), 16); mask[n] = 0xFF; n++; }
        }
        return n;
    }

    // One bounded, chunked scan over the sections that carry the requested access. The needle is a byte pattern with
    // optional wildcards; every read goes through ImageRead/ImageRange, so a malformed image cannot make us read out
    // of the mapping, and the scan is O(image) with 64 KiB copies rather than per-byte guarded reads.
    uintptr_t GameBoundaryScan(const ImageInfo& img, bool executable, const unsigned char* val, const unsigned char* mask, int nlen, int* count, bool* complete) {
        *count = 0; uintptr_t first = 0;
        if (complete) *complete = false;   // only set true after every planned read succeeded
        if (!nlen || nlen > 64) return 0;
        IMAGE_DOS_HEADER dos = {};
        if (!ImageRead(img, 0, &dos, sizeof dos) || dos.e_magic != IMAGE_DOS_SIGNATURE) return 0;
        IMAGE_NT_HEADERS64 nt = {};
        if (!ImageRead(img, (uintptr_t)dos.e_lfanew, &nt, sizeof nt) || nt.Signature != IMAGE_NT_SIGNATURE) return 0;
        const uintptr_t secBase = (uintptr_t)dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
        std::vector<unsigned char> buf(0x10000);
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; i++) {
            IMAGE_SECTION_HEADER sc = {};
            if (!ImageRead(img, secBase + (uintptr_t)i * sizeof(IMAGE_SECTION_HEADER), &sc, sizeof sc)) return first;   // incomplete
            const bool want = executable ? ((sc.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0) : ((sc.Characteristics & IMAGE_SCN_MEM_READ) != 0);
            if (!want || !sc.Misc.VirtualSize || sc.VirtualAddress >= img.size) continue;
            const uintptr_t span = (sc.Misc.VirtualSize > img.size - sc.VirtualAddress) ? (img.size - sc.VirtualAddress) : sc.Misc.VirtualSize;
            for (uintptr_t base = 0; base + (uintptr_t)nlen <= span; ) {
                const size_t want64 = (size_t)((span - base) < buf.size() ? (span - base) : buf.size());
                if (!ImageRead(img, sc.VirtualAddress + base, buf.data(), want64)) return first;   // incomplete: never claim uniqueness
                if (want64 >= (size_t)nlen) {
                    for (size_t k = 0; k + (size_t)nlen <= want64; k++) {
                        bool ok = true;
                        for (int j = 0; j < nlen; j++) if ((buf[k + (size_t)j] & mask[j]) != (val[j] & mask[j])) { ok = false; break; }
                        if (!ok) continue;
                        (*count)++;
                        if (!first) first = sc.VirtualAddress + base + k;
                    }
                }
                if (want64 < buf.size()) break;
                base += want64 - (uintptr_t)(nlen - 1);   // overlap so a needle across the chunk boundary is found
            }
        }
        if (complete) *complete = true;   // every planned read succeeded
        return first;
    }
    // Exact-byte scan (the referenced string): mask all ones.
    static uintptr_t GameBoundaryScanExact(const ImageInfo& img, bool executable, const unsigned char* bytes, int nlen, int* count, bool* complete) {
        unsigned char mask[64];
        for (int i = 0; i < nlen && i < 64; i++) mask[i] = 0xFF;
        return GameBoundaryScan(img, executable, bytes, mask, nlen, count, complete);
    }
    // Function start for an RVA inside a function, via the PE exception directory (chain info followed, bounded).
    uintptr_t GameBoundaryFuncStart(const ImageInfo& img, uintptr_t rva) {
        IMAGE_DOS_HEADER dos = {};
        if (!ImageRead(img, 0, &dos, sizeof dos)) return 0;
        IMAGE_NT_HEADERS64 nt = {};
        if (!ImageRead(img, (uintptr_t)dos.e_lfanew, &nt, sizeof nt)) return 0;
        const IMAGE_DATA_DIRECTORY dir = nt.OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (!dir.VirtualAddress || dir.Size < sizeof(RUNTIME_FUNCTION)) return 0;
        const size_t n = dir.Size / sizeof(RUNTIME_FUNCTION);
        size_t lo = 0, hi = n;
        while (lo < hi) {
            const size_t mid = (lo + hi) / 2;
            RUNTIME_FUNCTION rf = {};
            if (!ImageRead(img, dir.VirtualAddress + (uintptr_t)mid * sizeof(RUNTIME_FUNCTION), &rf, sizeof rf)) return 0;
            if (rf.BeginAddress <= rva) lo = mid + 1; else hi = mid;
        }
        if (lo == 0) return 0;
        RUNTIME_FUNCTION cur = {};
        if (!ImageRead(img, dir.VirtualAddress + (uintptr_t)(lo - 1) * sizeof(RUNTIME_FUNCTION), &cur, sizeof cur)) return 0;
        if (!(cur.BeginAddress <= rva && rva < cur.EndAddress)) return 0;
        uintptr_t seen[16] = {}; int seenCount = 0;
        int level = 0;
        for (;;) {
            unsigned char ui[4] = {};
            if (!ImageRead(img, cur.UnwindInfoAddress, ui, sizeof ui)) return 0;   // unreadable unwind info: fail closed
            const unsigned flags = ui[0] >> 3;
            // winnt.h: UNW_FLAG_EHANDLER 0x1, UNW_FLAG_UHANDLER 0x2, UNW_FLAG_CHAININFO 0x4. Only chain info carries a
            // linked RUNTIME_FUNCTION right after the unwind codes; a handler payload is never parsed as one.
            const unsigned kUnwFlagChainInfo = 0x4;
            if (!(flags & kUnwFlagChainInfo)) break;                              // a read, non-chained record is the head
            if (level >= 16) return 0;                                            // depth budget exhausted with another link pending: fail closed
            RUNTIME_FUNCTION nxt = {};
            const uintptr_t off = cur.UnwindInfoAddress + 4 + ((uintptr_t)(ui[2] + 1) & ~(uintptr_t)1) * 2;
            if (!ImageRead(img, off, &nxt, sizeof nxt)) return 0;                 // truncated chain: fail closed
            if (nxt.BeginAddress >= nxt.EndAddress || nxt.EndAddress > img.size) return 0;   // invalid linked entry
            for (int j = 0; j < seenCount; j++) if (seen[j] == nxt.BeginAddress) return 0;   // cyclic chain: fail closed
            if (seenCount < 16) seen[seenCount++] = cur.BeginAddress;
            cur = nxt;
            ++level;
        }
        return cur.BeginAddress;
    }
    // Finds `lea r64,[rip+disp32]` instructions referencing the string and returns the containing function head.
    uintptr_t GameBoundaryFuncReferencingString(const ImageInfo& img, const char* s, bool* complete) {
        if (complete) *complete = false;
        const int len = (int)strlen(s) + 1;
        std::vector<unsigned char> needle((size_t)len);
        for (int i = 0; i < len; i++) needle[(size_t)i] = (unsigned char)s[i];
        int strHits = 0; bool strComplete = false;
        const uintptr_t str = GameBoundaryScanExact(img, false, needle.data(), len, &strHits, &strComplete);
        if (!strComplete) return 0;                                           // a partial scan cannot prove uniqueness
        if (!str || strHits != 1) { if (complete) *complete = true; return 0; }   // a completed scan without a unique anchor is an absence, not a read error
        IMAGE_DOS_HEADER dos = {};
        if (!ImageRead(img, 0, &dos, sizeof dos)) return 0;
        IMAGE_NT_HEADERS64 nt = {};
        if (!ImageRead(img, (uintptr_t)dos.e_lfanew, &nt, sizeof nt)) return 0;
        const uintptr_t secBase = (uintptr_t)dos.e_lfanew + sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER) + nt.FileHeader.SizeOfOptionalHeader;
        std::vector<unsigned char> buf(0x10000);
        uintptr_t found = 0; int refs = 0;
        for (unsigned i = 0; i < nt.FileHeader.NumberOfSections; i++) {
            IMAGE_SECTION_HEADER sc = {};
            if (!ImageRead(img, secBase + (uintptr_t)i * sizeof(IMAGE_SECTION_HEADER), &sc, sizeof sc)) return 0;   // unreadable section header: the reference scan is incomplete
            if (!(sc.Characteristics & IMAGE_SCN_MEM_EXECUTE) || !sc.Misc.VirtualSize || sc.VirtualAddress >= img.size) continue;
            const uintptr_t span = (sc.Misc.VirtualSize > img.size - sc.VirtualAddress) ? (img.size - sc.VirtualAddress) : sc.Misc.VirtualSize;
            for (uintptr_t base = 0; base + 7 <= span; ) {
                const size_t want64 = (size_t)((span - base) < buf.size() ? (span - base) : buf.size());
                if (!ImageRead(img, sc.VirtualAddress + base, buf.data(), want64)) return 0;   // incomplete reference scan
                for (size_t k = 0; k + 7 <= want64; k++) {
                    if (buf[k] != 0x48 && buf[k] != 0x4C) continue;
                    if (buf[k + 1] != 0x8D || (buf[k + 2] & 0xC7) != 0x05) continue;   // lea reg, [rip+disp32]
                    int32_t disp = 0;
                    memcpy(&disp, &buf[k + 3], 4);
                    const uintptr_t at = sc.VirtualAddress + base + k;
                    if (at + 7 + (intptr_t)disp == str) { refs++; if (!found) found = GameBoundaryFuncStart(img, at); }
                }
                if (want64 < buf.size()) break;
                base += want64 - 6;
            }
        }
        if (complete) *complete = true;
        return (refs == 1) ? found : 0;                                       // exactly one reference, or the anchor is rejected
    }
}
