// Window subclass + virtual cursor for the overlay (adapted from master-looter / Trinity, MIT).
#pragma once
#include <windows.h>
struct ImGuiIO;
namespace input {
    void Init(HWND hwnd);
    void Shutdown();
    void MenuOpened();          // centres the virtual cursor
    void MenuClosed();
    void FeedMouse(ImGuiIO& io);   // render thread, once per frame while the menu is open
    // key state by scan code, tracked from the window messages (GetAsyncKeyState can stick after Shift+Numpad); ext = extended key flag
    bool ScanDown(int scan, bool ext);
    bool ScanDownAny(int scan);    // either variant (numpad key with NumLock off arrives as the extended arrow/page key)
    void ClearKeys();
    bool VkDown(int vk);           // key state for a virtual key (numpad keys accept the Shift variant, navigation keys their extended code)
    void SetPlaceVks(const int* vks, int count);
    void SetFreeCam(bool on);      // free-fly camera: WASD/QE/Shift/Ctrl/Space and the look mouse go to World Builder
    bool FreeCamLooking();         // the mouse currently turns the free camera (menu closed, or right button held over the world)
    void TakeLookDelta(float* dx, float* dy);   // raw mouse movement collected for the free camera since the last call   // keys that belong to World Builder while an object is carried
}
