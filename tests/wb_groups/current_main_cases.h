// Included by PlacementResults: actual core queues/receipts plus actual travel and terrain TUs.
// Native cast/stage/file/player/clock observations are the only substituted boundaries.
#pragma once
#include <array>
#include <atomic>
#include <future>
#include <limits>

namespace current_main {
using Status = core::GroundProbeStatus;
struct Stage { uint32_t key, b, c; std::array<float, 10> transform; DWORD thread; };
std::vector<Stage> stages;
std::atomic<int> casts{0};
bool faultStage = false;
bool PhysicalHit(Vec3 start, float, core::GroundHit* out) {
    ++casts; out->done = out->hit = true; out->centerY = 10.25f;
    out->center = {start.x, out->centerY, start.z}; out->normal = {0, 1, 0}; return true;
}
void Setup(const char* id) {
    BeginCase(id); stages.clear(); casts = 0; faultStage = false;
    host::Seam().probeReady = true; host::Seam().groundCast = PhysicalHit;
    host::SetGroundRadius(.25f);
    host::SetTravelStage([](uint32_t key, uint32_t b, uint32_t c, const float* transform) {
        Stage record{key, b, c, {}, GetCurrentThreadId()};
        std::copy(transform, transform + 10, record.transform.begin()); stages.push_back(record);
        Check("native-stage-no-apply-lease", host::GroundLeaseCount() == 0);
        Check("native-stage-reservation-held", host::GroundWorldWriters() != 0 && core::GroundProbe({0, 30, 0}, 40) == 0);
        host::SetTerrainPlayer({transform[7], transform[8], transform[9]});
        if (faultStage) RaiseException(0xE0001234, 0, 0, nullptr);
    });
}
struct GateWorker {
    Gate& gate; std::thread worker;
    ~GateWorker() { gate.Release(); if (worker.joinable()) worker.join(); }
};
void RawInvalidation() {
    Setup("main-raw-ticket-invalidation"); core::GroundHit hit; hit.centerY = -123;
    Check("unknown-is-terminal-untouched", core::GroundResultState(0, &hit) == Status::Unknown && hit.centerY == -123);
    int ticket = core::RayProbe({1, 20, 3}, {2, 0, 0}, 30); Vec3 direction{};
    Require(ticket != 0, "directional-raw-admitted");
    Check("direction-retained-at-admission", host::RawGroundDirection(ticket, &direction) && direction.x == 1 && direction.y == 0);
    Check("pending-is-not-miss", core::GroundResultState(ticket, &hit) == Status::Pending && hit.centerY == -123);
    const uint64_t epoch = host::GroundEpoch();
    Require(core::TravelTo({5000, 600, 200}, 30) && host::PumpGameAt(1), "dispatch-real-travel-before-queued-cast");
    Check("travel-native-transform", stages.size() == 1 && stages[0].transform[7] == 5000 && stages[0].key == 3);
    Check("travel-advances-existing-epoch", host::GroundEpoch() > epoch);
    core::GroundFrame();
    Check("queued-ticket-invalidated-not-fake-miss", core::GroundResultState(ticket, &hit) == Status::Invalidated && hit.centerY == -123);
    PumpGame_(); Check("consumed-invalidation-never-casts", casts == 0 && core::GroundResultState(ticket, &hit) == Status::Unknown);

    ticket = core::GroundProbe({2, 30, 4}, 40); PumpGame_();
    float transform[10] = {1,1,1, 0,0,0,1, 17,18,19};
    host::NativeTravel(transform);
    Check("game-native-hook-preserves-arguments", stages.back().key == 7 && stages.back().b == 8 && stages.back().c == 9);
    hit.centerY = -123;
    Check("ready-result-invalidated-untouched", core::GroundResultState(ticket, &hit) == Status::Invalidated && hit.centerY == -123);
    {
        Gate gate;
        host::Seam().groundCast = [&](Vec3 start, float len, core::GroundHit* out) { gate.Arrive(); return PhysicalHit(start, len, out); };
        ticket = core::GroundProbe({3, 30, 5}, 40);
        GateWorker worker{gate, std::thread([] { host::PumpGame(); })};
        Require(gate.Entered(), "inflight-cast-event-entered");
        host::NativeTravel(transform);
        Check("inflight-result-terminal-before-cast-return", core::GroundResultState(ticket, &hit) == Status::Invalidated);
        gate.Release(); worker.worker.join();
        Check("late-hit-cannot-resurrect-consumed-ticket", core::GroundResultState(ticket, &hit) == Status::Unknown);
    }
    host::Seam().groundCast = PhysicalHit;
    ticket = core::RayProbe({6, 30, 7}, {0, -1, 0}, 40); PumpGame_();
    host::SetGroundRadius(.75f);
    Check("fresh-ticket-rearms-with-own-radius-center", core::GroundResultState(ticket, &hit) == Status::Hit && hit.radius == .25f && hit.center.x == 6);
    host::Seam().groundCast = [](Vec3, float, core::GroundHit* out) { out->done = true; out->hit = false; return true; };
    ticket = core::GroundProbe({0, 30, 0}, 40); PumpGame_();
    Check("real-physical-miss-distinct", core::GroundResultState(ticket, &hit) == Status::Miss && !hit.hit);
    ticket = core::GroundProbe({0, 30, 0}, 40); core::InvalidateGroundWorld();
    Check("legacy-bool-never-fakes-completed-miss", !core::GroundResult(ticket, &hit));
    PumpGame_(); core::GroundFrame();
    Check("raw-tickets-drained", host::GroundTicketCount() == 0);
}
void TravelLease() {
    Setup("main-travel-native-apply-lease"); host::SetDirectGimmick(true);
    const int uid = core::SpawnAt("/object/cd_gimmick/native_travel.prefab", {0, 20, 0});
    host::PumpServer();
    auto operation = core::BeginGround({uid}, 400, 1);
    Require(core::GroundApply(operation, {{uid, {0, 19, 0}, {}, 1}}), "interactive-ground-admitted");
    Require(host::PumpGame() && core::GroundStateOf(operation).state == core::GroundApplying, "actual-apply-lease-entered");
    const DWORD gameThread = GetCurrentThreadId();
    float transform[10] = {1,1,1, 0,0,0,1, 17,18,19};
    {
        Gate gate; oracle::duringDirect = [&] { gate.Arrive(); };
        GateWorker worker{gate, std::thread([] { host::PumpServer(); })};
        Require(gate.Entered(), "apply-server-native-boundary-held");
        Require(core::TravelTo({101,102,103}, 90) && host::PumpGame(), "travel-queued-during-apply");
        host::NativeTravel(transform); transform[7] = -999;
        Check("both-native-transitions-deferred", stages.empty() && host::GroundDeferredCount() == 2 && host::GroundLeaseCount() == 1);
        Check("applying-not-relabeled-canceled", core::GroundStateOf(operation).state == core::GroundApplying);
        Check("world-reservation-blocks-new-raw", core::GroundProbe({0,30,0},40) == 0 && core::RayProbe({0,30,0},{1,0,0},40) == 0);
        auto newer = core::BeginGround({uid}, 401, 1);
        Check("world-reservation-blocks-new-apply", !core::GroundApply(newer, {{uid,{0,18,0},{},1}}));
        gate.Release(); worker.worker.join(); oracle::duringDirect = {};
    }
    Check("apply-settles-before-native-reload", core::GroundStateOf(operation).state == core::GroundSettled && core::GroundStateOf(operation).members[0].accepted);
    Check("server-only-queues-reload", stages.empty() && host::GroundWorldWriters() == 2);
    Require(host::PumpGame(), "first-deferred-game-dispatch");
    Check("first-reservation-released", stages.size() == 1 && host::GroundWorldWriters() == 1);
    Require(host::PumpGame(), "second-deferred-game-dispatch");
    Check("second-reservation-released", stages.size() == 2 && host::GroundWorldWriters() == 0);
    Check("deferred-native-transform-owned", stages[0].transform[7] == 101 && stages[1].transform[7] == 17);
    Check("reloads-execute-on-game-not-server", stages[0].thread == gameThread && stages[1].thread == gameThread);
    Check("apply-authorities-drained", host::GroundLeaseCount() == 0 && host::GroundDeferredCount() == 0);
    core::GroundReconcile({operation}); PumpRounds(); core::GroundFrame();
}
void TravelFault() {
    Setup("main-travel-native-failure"); core::GroundHit hit;
    const int old = core::GroundProbe({0,30,0},40); faultStage = true;
    Require(core::TravelTo({1,2,3},0) && host::PumpGameAt(1), "faulting-native-travel-dispatched"); faultStage = false;
    Check("native-failure-releases-reservation", host::GroundWorldWriters() == 0);
    Check("native-failure-keeps-old-result-invalid", core::GroundResultState(old,&hit) == Status::Invalidated);
    PumpGame_(); const int fresh = core::GroundProbe({0,30,0},40); PumpGame_();
    Check("new-cast-after-native-failure", core::GroundResultState(fresh,&hit) == Status::Hit);
}
struct TerrainWorker {
    std::thread worker;
    ~TerrainWorker() { host::ResumeTerrainStep((std::numeric_limits<unsigned>::max)(), 4001); if (worker.joinable()) worker.join(); }
};
void TerrainTwoTrips() {
    Setup("main-terrain-apply-two-trips");
    const Vec3 back{100, 20, 200}; host::ResetTerrainClock(back); host::SetTerrainAvailable(true);
    core::TerrainStroke stroke{}; stroke.r = 2; stroke.strength = 1;
    core::TerrainReplaceProject(9001, {stroke});
    Require(core::TerrainNeedsApply(), "real-terrain-state-needs-apply");
    const int uid = core::SpawnAt("/object/terrain_trip.prefab", back); PumpGame_();
    auto first = core::BeginGround({uid}, 501, 1);
    Require(core::GroundTicket(first, {100,30,200},40) != 0, "owned-ticket-before-first-trip");
    const int rawFirst = core::GroundProbe({100,30,200},40); PumpGame_();
    const uint64_t epoch = host::GroundEpoch();
    std::packaged_task<bool()> apply([back] { return core::TerrainApply(back); });
    auto completed = apply.get_future(); // subscribe before the actual TerrainApply worker is triggered
    TerrainWorker worker{std::thread(std::move(apply))};
    Require(host::WaitTerrainStep(1), "terrain-away-arrival-wait-entered");
    Check("second-apply-refused-while-worker-active", !core::TerrainApply(back));
    PumpGame_(); core::GroundHit hit;
    Check("real-first-trip-native-destination", stages.size() == 1 && stages[0].transform[7] == back.x + 5000 && stages[0].transform[8] == back.y + 300);
    Check("first-trip-invalidates-owned-and-raw", core::GroundStateOf(first).state == core::GroundInvalidated && core::GroundResultState(rawFirst,&hit) == Status::Invalidated);
    Check("first-trip-not-marked-applied", core::TerrainNeedsApply());
    host::ResumeTerrainStep(1, 1);
    Require(host::WaitTerrainStep(2), "terrain-away-stability-wait-entered");
    auto second = core::BeginGround({uid}, 502, 1);
    Require(core::GroundTicket(second, {100,30,200},40) != 0, "owned-ticket-between-trips");
    const int rawSecond = core::GroundProbe({100,30,200},40); PumpGame_();
    host::ResumeTerrainStep(2, 4001); // advance the production arrival clock, never sleep in real time
    Require(host::WaitTerrainStep(3), "terrain-return-arrival-wait-entered");
    PumpGame_();
    Check("real-return-trip-native-destination", stages.size() == 2 && stages[1].transform[7] == back.x && stages[1].transform[8] == back.y + 1 && stages[1].transform[9] == back.z);
    Check("second-trip-invalidates-new-owned-and-raw", core::GroundStateOf(second).state == core::GroundInvalidated && core::GroundResultState(rawSecond,&hit) == Status::Invalidated);
    Check("each-trip-advances-authority", host::GroundEpoch() == epoch + 2 && host::GroundWorldWriters() == 0);
    Check("return-arrival-still-required", core::TerrainNeedsApply());
    host::ResumeTerrainStep(3, 1);
    Require(host::WaitTerrainStep(4), "terrain-return-stability-wait-entered"); host::ResumeTerrainStep(4, 4001);
    Require(completed.wait_for(std::chrono::seconds(10)) == std::future_status::ready, "actual-apply-worker-completed");
    Check("actual-apply-admitted-and-finished", completed.get()); worker.worker.join();
    Check("terrain-marked-applied-only-after-return", !core::TerrainNeedsApply() && core::TerrainApplyState().empty());
    Check("no-extra-native-trip", stages.size() == 2);
    core::TerrainClear(); host::SetTerrainAvailable(false); core::GroundFrame();
    Check("two-trip-ticket-cleanup", host::GroundTicketCount() == 0);
}
}
