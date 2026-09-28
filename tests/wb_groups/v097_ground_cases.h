// Real ServiceGroundQueue policy. Only physical cast results are supplied at the native boundary.
#pragma once
#include <array>
#include <limits>

namespace v097_ground {
using Status = core::GroundProbeStatus;
struct Sample { bool hit; float y; };
struct Scenario { const char* name; std::array<Sample, 5> samples; bool hit; float y; int picked; };
static void Setup(const char* id) {
    BeginCase(id);
    Require(core::ClearScene(), "robust-clear-prior-logical-scene"); PumpRounds(); core::GroundFrame();
    host::Seam().probeReady = true; host::SetGroundRadius(.25f);
}
static void FiveCasts() {
    Setup("V097-GROUND-FIVE-CASTS");
    const float nan = (std::numeric_limits<float>::quiet_NaN)();
    const Scenario cases[] = {
        {"fall-through", {{{true,-10}, {true,2}, {true,2.2f}, {false,0}, {false,0}}}, true,2.2f,2},
        {"center-miss", {{{false,0}, {true,2}, {true,2.2f}, {false,0}, {false,0}}}, true,2.2f,2},
        {"bridge", {{{true,10}, {true,2}, {true,2.2f}, {false,0}, {false,0}}}, true,10,0},
        {"one-neighbor", {{{false,0}, {true,2}, {false,0}, {false,0}, {false,0}}}, false,0,0},
        {"agreement-outside", {{{true,-10}, {true,0}, {true,.4501f}, {false,0}, {false,0}}}, true,-10,0},
        {"agreement-inclusive", {{{true,-10}, {true,0}, {true,.45f}, {false,0}, {false,0}}}, true,.45f,2},
        {"height-strict", {{{true,0}, {true,.55f}, {true,.55f}, {false,0}, {false,0}}}, true,0,0},
        {"height-above", {{{true,0}, {true,.5501f}, {true,.5501f}, {false,0}, {false,0}}}, true,.5501f,1},
        {"nonfinite-neighbor", {{{false,0}, {true,nan}, {true,2}, {false,0}, {false,0}}}, false,0,0},
        {"nonfinite-center", {{{true,nan}, {true,2}, {true,2}, {false,0}, {false,0}}}, true,2,1},
        {"all-miss", {{{false,0}, {false,0}, {false,0}, {false,0}, {false,0}}}, false,0,0},
    };
    const int uid = core::SpawnAt("/object/robust.prefab", {999.9f,20,-1000.1f}); PumpGame_();
    const Vec3 start{999.9f,40,-1000.1f};
    const std::array<Vec3,5> positions{{start, {start.x+.36f,40,start.z}, {start.x-.36f,40,start.z},
                                     {start.x,40,start.z+.36f}, {start.x,40,start.z-.36f}}};
    uint64_t serial = 1000;
    for (bool owned : {false, true}) for (const auto& scenario : cases) {
        size_t calls = 0;
        host::SetGroundRadius(.25f);
        host::Seam().groundCast = [&](Vec3 pos, float length, core::GroundHit* out) {
            Require(calls < scenario.samples.size(), "exactly-five-native-calls", scenario.name);
            const auto index = calls++;
            Check("center-and-ordered-four-neighbor-positions", pos.x == positions[index].x && pos.y == 40 &&
                  pos.z == positions[index].z && length == 80, scenario.name);
            // Changing calibration inside a cast must not change this result's captured radius.
            if (index == 0) host::SetGroundRadius(.75f);
            out->done = true; out->hit = scenario.samples[index].hit; out->centerY = scenario.samples[index].y;
            out->center = {pos.x,out->centerY,pos.z}; out->normal = {static_cast<float>(index),1,0};
            return true;
        };
        const auto operation = owned ? core::BeginGround({uid}, ++serial, 1) : core::GroundHandle{};
        const int ticket = owned ? core::GroundTicket(operation,start,80) : core::GroundProbe(start,80);
        Require(ticket != 0, "robust-ticket-admitted", scenario.name);
        core::GroundHit result; result.centerY = -1234;
        Check("robust-positive-pending-state", owned ? !core::GroundPoll(operation,&result) :
              core::GroundResultState(ticket,&result) == Status::Pending, scenario.name);
        Check("pending-keeps-output-untouched", result.centerY == -1234);
        PumpGame_();
        const bool terminal = owned ? core::GroundPoll(operation,&result) :
            core::GroundResultState(ticket,&result) == (scenario.hit ? Status::Hit : Status::Miss);
        Check("robust-five-cast-terminal", terminal && calls == 5 && result.done && result.hit == scenario.hit, scenario.name);
        Check("robust-selected-height", result.centerY == scenario.y, scenario.name);
        Check("robust-semantic-center-and-captured-radius", result.center.x == start.x && result.center.z == start.z && result.radius == .25f, scenario.name);
        Check("robust-selected-native-normal", result.normal.x == static_cast<float>(scenario.picked), scenario.name);
        Check("robust-ticket-consumed-once", core::GroundResultState(ticket,&result) == Status::Unknown);
        if (owned) core::GroundCancel(operation);
    }
    host::Seam().groundCast = {}; core::GroundFrame();
    Check("robust-no-ticket-leak", host::GroundTicketCount() == 0);
}
static void NeighborInvalidation() {
    Setup("V097-GROUND-NEIGHBOR-INVALIDATION");
    const int uid = core::SpawnAt("/object/neighbor.prefab", {1,20,3}); PumpGame_();
    for (bool owned : {false, true}) {
        int calls = 0;
        host::Seam().groundCast = [&](Vec3 pos, float, core::GroundHit* out) {
            if (++calls == 3) core::InvalidateGroundWorld();
            out->done = out->hit = true; out->centerY = 5; out->center = {pos.x,5,pos.z}; return true;
        };
        const auto operation = owned ? core::BeginGround({uid},2000,1) : core::GroundHandle{};
        const int ticket = owned ? core::GroundTicket(operation,{1,40,3},80) : core::GroundProbe({1,40,3},80);
        Require(ticket != 0, "neighbor-invalidation-admitted"); PumpGame_();
        core::GroundHit result; result.centerY = -1234;
        Check("invalidation-occurs-inside-five-physical-casts", calls == 5);
        Check("neighbor-invalidation-never-publishes-hit-or-fake-miss", owned ?
              !core::GroundPoll(operation,&result) && core::GroundStateOf(operation).state == core::GroundInvalidated :
              core::GroundResultState(ticket,&result) == Status::Invalidated);
        Check("neighbor-invalidation-keeps-output-untouched", result.centerY == -1234);
        Check("neighbor-late-results-cannot-resurrect", core::GroundResultState(ticket,&result) == Status::Unknown);
    }
    int calls = 0;
    host::Seam().groundCast = [&](Vec3 pos, float, core::GroundHit* out) {
        ++calls; out->done = out->hit = true; out->centerY = 7; out->center = {pos.x,7,pos.z}; return true;
    };
    for (const Vec3 direction : {Vec3{2,0,0}, Vec3{0,-2,0}}) {
        calls = 0; const int ticket = core::RayProbe({1,40,3},direction,80);
        Require(ticket != 0, "directional-ray-admitted"); PumpGame_(); core::GroundHit result;
        Check("directional-ray-single-physical-cast-even-downward", calls == 1 && core::GroundResultState(ticket,&result) == Status::Hit);
    }
    const int fresh = core::GroundProbe({1,40,3},80); calls = 0; PumpGame_(); core::GroundHit result;
    Check("fresh-world-rearms-robust-probe", calls == 5 && core::GroundResultState(fresh,&result) == Status::Hit);
    host::Seam().groundCast = {}; core::GroundFrame();
    Check("neighbor-invalidation-no-ticket-leak", host::GroundTicketCount() == 0);
}
} // namespace v097_ground
