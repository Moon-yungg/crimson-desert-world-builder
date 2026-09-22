// ImGui user config: asserts log instead of aborting the game.
#pragma once
void CdImguiAssert(const char* expr, const char* file, int line);
#define IM_ASSERT(_EXPR) ((void)((!!(_EXPR)) || (CdImguiAssert(#_EXPR, __FILE__, __LINE__), 0)))
