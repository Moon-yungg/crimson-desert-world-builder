#pragma once
#include <string>
#include <imgui.h>
namespace overlay {
    void Install();
    // Thumbnail texture for an image file (PNG/JPG). Returns 0 while loading or if missing; loads a few per frame, LRU-evicted.
    ImTextureID Thumb(const std::string& file, int* w = nullptr, int* h = nullptr);
    void* D3DDevice(); void* D3DQueue();   // the game's device / direct queue the overlay draws with (nullptr until the swapchain was seen)
}
