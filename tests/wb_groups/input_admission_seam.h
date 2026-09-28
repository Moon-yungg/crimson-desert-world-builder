// Native CRITICAL_SECTION contention observation for the real InputHotkeys TU. No router policy substitute.
#pragma once
#include <windows.h>
namespace input_host {
void ArmContention(DWORD producerThread);
bool WaitContention(unsigned timeoutMs);
void DisarmContention();
}
