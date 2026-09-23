// Fault guards that work with MSVC and with GCC / MinGW.
//
// MSVC: structured exception handling (__try / __except). The plugin is built with /EHa there.
// Other compilers have no __try, so the plugin's vectored exception handler (cdmodkit.cpp, VectoredHandler) jumps back into
// the innermost guarded frame of the faulting thread with __builtin_longjmp; the handler part of the guard then runs.
// Objects that live inside a guarded region are not destroyed on a fault under GCC (nor under MSVC without /EHa), so a
// guarded region should hold plain data only, or accept the leak (a fault is a bug that is being survived, not a code path).
//
//   CDK_GUARD_BEGIN
//       ... code that may fault ...
//   CDK_GUARD_FAIL
//       ... runs after a fault; cdk::GuardCode() is the exception code, cdk::GuardInfo() the record + context ...
//   CDK_GUARD_END
#pragma once
#include <windows.h>
#include <cstdint>

namespace cdk {
    struct FaultInfo { EXCEPTION_RECORD rec; CONTEXT ctx; };
    extern thread_local FaultInfo t_fault;   // the last fault a guard on this thread caught (defined in cdmodkit.cpp)
    inline DWORD GuardCode() { return t_fault.rec.ExceptionCode; }
    inline EXCEPTION_POINTERS GuardInfo() { EXCEPTION_POINTERS ep; ep.ExceptionRecord = &t_fault.rec; ep.ContextRecord = &t_fault.ctx; return ep; }
}

#ifdef _MSC_VER
namespace cdk { inline int GuardFilter(EXCEPTION_POINTERS* ep) { t_fault.rec = *ep->ExceptionRecord; t_fault.ctx = *ep->ContextRecord; return EXCEPTION_EXECUTE_HANDLER; } }
#define CDK_GUARD_BEGIN __try {
#define CDK_GUARD_FAIL  } __except (cdk::GuardFilter(GetExceptionInformation())) {
#define CDK_GUARD_END   }
#else
namespace cdk {
    struct GuardFrame;
    extern thread_local GuardFrame* t_guardTop;   // innermost active guard of this thread (defined in cdmodkit.cpp)
    // Registers itself on construction and leaves on destruction, so a `return` inside the guarded region (ReadBytes returns
    // from within it) still unregisters the frame: a stale frame here would send the next fault into a dead stack frame.
    struct GuardFrame { intptr_t jb[5]; GuardFrame* prev; GuardFrame() : prev(t_guardTop) { t_guardTop = this; } ~GuardFrame() { t_guardTop = prev; } };
    // Called by the vectored handler on the faulting thread. Returns only when no guard is active.
    inline void GuardDispatch(EXCEPTION_POINTERS* ep) {
        GuardFrame* f = t_guardTop; if (!f) return;
        t_fault.rec = *ep->ExceptionRecord; t_fault.ctx = *ep->ContextRecord; t_guardTop = f->prev;
        __builtin_longjmp(f->jb, 1);
    }
}
#define CDK_GUARD_BEGIN { cdk::GuardFrame cdk_gf_; if (__builtin_setjmp(cdk_gf_.jb) == 0) {
#define CDK_GUARD_FAIL  } else {
#define CDK_GUARD_END   } }
// MSVC intrinsics used by the plugin
#ifndef _ReturnAddress
#define _ReturnAddress() __builtin_return_address(0)
#endif
#ifndef __forceinline
#define __forceinline inline __attribute__((always_inline))
#endif
#endif
