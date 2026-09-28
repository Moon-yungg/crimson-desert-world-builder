#pragma once
#include <windows.h>
#include <functional>
#include <string>
// Only Win32 clipboard allocation/transfer boundaries are intercepted. Success always calls the OS.
namespace cliptest {
    enum class Fail { None, Open, Empty, Allocate, Set };
    inline Fail fail = Fail::None;
    inline int opens=0, empties=0, allocations=0, frees=0, sets=0, closes=0, transfers=0;
    inline HANDLE transferEvent=nullptr;
    inline std::function<void()> onOpen;
    inline void Reset() { fail=Fail::None; opens=empties=allocations=frees=sets=closes=transfers=0; onOpen={}; }
    inline BOOL WINAPI Open(HWND owner) {
        ++opens;if(onOpen)onOpen();
        if(fail==Fail::Open){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}
        return ::OpenClipboard(owner);
    }
    inline BOOL WINAPI Empty() { ++empties;if(fail==Fail::Empty){SetLastError(ERROR_ACCESS_DENIED);return FALSE;}return ::EmptyClipboard(); }
    inline HGLOBAL WINAPI Allocate(UINT flags,SIZE_T size) { ++allocations;if(fail==Fail::Allocate){SetLastError(ERROR_NOT_ENOUGH_MEMORY);return nullptr;}return ::GlobalAlloc(flags,size); }
    inline HGLOBAL WINAPI Free(HGLOBAL memory) { ++frees;return ::GlobalFree(memory); }
    inline HANDLE WINAPI Set(UINT format,HANDLE data) {
        ++sets;if(fail==Fail::Set){SetLastError(ERROR_ACCESS_DENIED);return nullptr;}
        HANDLE result=::SetClipboardData(format,data);
        if(result){++transfers;if(transferEvent)SetEvent(transferEvent);}return result;
    }
    inline BOOL WINAPI Close() { ++closes;return ::CloseClipboard(); }
}
