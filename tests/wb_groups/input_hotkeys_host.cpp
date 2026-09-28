// Actual input.cpp, with only its native blocking-lock entry observed. A failed real try-acquire
// signals that the producer reached the contended authority; tests never infer this from elapsed time.
#include <windows.h>
#include <atomic>
#include <mutex>
#include <stdexcept>
#include "input_admission_seam.h"
#include "../../asi/cdmodkit/input.h"
namespace input_host {
namespace {
struct Probe {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::atomic<DWORD> thread{0};
    Probe() { if (!event) throw std::runtime_error("input contention event creation failed"); }
    ~Probe() { CloseHandle(event); }
};
Probe probe;
}
void ArmContention(DWORD producerThread) { ResetEvent(probe.event); probe.thread.store(producerThread); }
bool WaitContention(unsigned timeoutMs) { return WaitForSingleObject(probe.event, timeoutMs) == WAIT_OBJECT_0; }
void DisarmContention() { probe.thread.store(0); }
void Enter(LPCRITICAL_SECTION section) {
    if (TryEnterCriticalSection(section)) return; // exactly one native acquisition, including recursive entry
    if (probe.thread.load() == GetCurrentThreadId()) SetEvent(probe.event);
    ::EnterCriticalSection(section); // still the real blocking lock; no fixture gate replaces its behavior
}
}
#define EnterCriticalSection input_host::Enter
#include "../../asi/cdmodkit/input.cpp"
#undef EnterCriticalSection
