// Compile the real travel TU once; only resolved native manager memory and the stage entry are supplied.
#include "production_host.h"
#include "../../asi/cdmodkit/travel.cpp"
namespace host {
namespace {
uintptr_t manager = 0;
TravelStage nativeStage;
void __fastcall RecordStage(uintptr_t, uint32_t key, uint32_t b, uint32_t c, const float* transform) {
    nativeStage(key, b, c, transform);
}
}
void SetTravelStage(TravelStage stage) {
    nativeStage = std::move(stage);
    core::g_mgrVt = reinterpret_cast<uintptr_t>(&manager);
    manager = core::g_mgrVt;
    core::g_mgr = reinterpret_cast<uintptr_t>(&manager);
    core::g_origStage = RecordStage;
    core::g_travelOk = static_cast<bool>(nativeStage);
    core::g_key = 3; core::g_argB = 1; core::g_argC = 0;
}
void NativeTravel(const float* transform) {
    core::HookStage(reinterpret_cast<uintptr_t>(&manager), 7, 8, 9, transform);
}
}
