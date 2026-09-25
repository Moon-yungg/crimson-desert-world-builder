// ImGui user config: asserts log instead of aborting the game.
#pragma once
#define IMGUI_ENABLE_WIN32_DEFAULT_IME_FUNCTIONS
void CdImguiAssert(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((void)((!!(_EXPR)) || (CdImguiAssert(#_EXPR, __FILE__, __LINE__), 0)))
