// Receipt-only presentation state. The editor owns one archive of each kind for the session.
// Neither archive cancels/reconciles operations, reads the scene, or changes History/redo.
#pragma once
#include "core.h"
#include <map>

namespace report_projection {
using Id = uint64_t;

struct Order {
    Id newest = 0;
    std::vector<Id> olderActive;       // always visible, including cleanup and carried-after-Final
    std::vector<Id> earlierCompleted; // disclosure only; no receipt is removed
    size_t earlierAttentionCount = 0; // completed REPORTS needing attention, not a sum of row outcomes
};

struct Placement {
    Id id = 0;                       // stable within this session's PlacementArchive
    std::string name;
    core::PlaceRequestHandle request; // the exact target for an explicit user Cancel
    core::PlaceRequestView receipt;   // ONE atomic request snapshot supplies all fields and details
    bool approximate = false, carrying = false;
    int removedAfterAttachment = 0;

    // Raw outcomes are disjoint. Do not use the legacy receipt.displayedExcluded() bucket here.
    int displayedExcluded() const;
    bool final() const;               // Final is not success or Drop; Attached is historical
    bool active() const;
    bool attention() const;
};
struct Placements { std::vector<Placement> reports; Order order; };

class PlacementArchive {
public:
    // Call at admission, once per non-null request (including all-excluded), never from a completion.
    // Re-registering the same handle is idempotent and cannot change its order/name. Same names differ.
    Id Admit(const core::PlaceRequestHandle& request, const std::string& name, bool approximate = false);
    Placements Snapshot(const core::PlaceRequestHandle& carried = {}) const;
private:
    struct Entry { Id id; core::PlaceRequestHandle request; std::string name; bool approximate; };
    std::vector<Entry> entries_;      // retains every request; no size/time cap or shell-reset API
};

struct Ground {
    core::GroundView receipt;         // includes ID/serial/branch/epoch/probe and exact member reasons
    size_t accepted = 0, notApplied = 0, pending = 0;
    bool final() const;
    bool active() const;
    bool attention() const;
};
struct Grounds { std::vector<Ground> reports; Order order; };

class GroundArchive {
public:
    // Observe real GroundStateOf/GroundBarrier values at admission and before reconciliation.
    // Also observe EVERY sibling while another is pending. First terminal snapshot is immutable;
    // late nonterminal/reconciled observations cannot erase its original terminal state or reason.
    // IDs come from BeginGround, so even reverse first-observation order keeps newest-by-admission.
    void Observe(const core::GroundView& receipt);
    Grounds Snapshot() const;
private:
    std::map<Id, core::GroundView> entries_; // independent values, never g_groundLastResults or History
};
} // namespace report_projection
