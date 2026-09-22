#pragma once
#include <string>
#include <imgui.h>
namespace overlay {
    void Install();
    // Thumbnail texture for an image file (PNG/JPG). Returns 0 while loading or if missing; loads a few per frame, LRU-evicted.
    ImTextureID Thumb(const std::string& file, int* w = nullptr, int* h = nullptr);
}
