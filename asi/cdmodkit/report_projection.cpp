#include "report_projection.h"
#include <utility>

namespace report_projection {
int Placement::displayedExcluded() const { return receipt.excluded; }
bool Placement::final() const { return receipt.settled && receipt.pending == 0 && !receipt.cleanupPending; }
bool Placement::active() const { return !final() || carrying; }
bool Placement::attention() const {
    return receipt.excluded != 0 || receipt.failed != 0 || receipt.canceled != 0 ||
           receipt.requestCanceled || receipt.cleanupPending || approximate || removedAfterAttachment != 0;
}

static void ClassifyOlder(Order& order, Id id, bool active, bool attention) {
    if (id == order.newest) return;
    if (active) order.olderActive.push_back(id);
    else {
        order.earlierCompleted.push_back(id);
        if (attention) ++order.earlierAttentionCount;
    }
}

Id PlacementArchive::Admit(const core::PlaceRequestHandle& request, const std::string& name, bool approximate) {
    for (const auto& entry : entries_) if (entry.request == request) return entry.id;
    const Id id = static_cast<Id>(entries_.size()) + 1;
    entries_.push_back({ id, request, name, approximate });
    return id;
}
Placements PlacementArchive::Snapshot(const core::PlaceRequestHandle& carried) const {
    Placements result;
    if (!entries_.empty()) result.order.newest = entries_.back().id;
    for (const auto& entry : entries_) {
        Placement report;
        report.id = entry.id; report.name = entry.name; report.request = entry.request;
        report.approximate = entry.approximate; report.carrying = entry.request == carried;
        report.receipt = core::PlaceRequestState(entry.request);
        for (const auto& row : report.receipt.rows) if (row.removedAfterAttach) ++report.removedAfterAttachment;
        ClassifyOlder(result.order, report.id, report.active(), report.attention());
        result.reports.push_back(std::move(report));
    }
    return result;
}

bool Ground::final() const { return receipt.terminal() && pending == 0; }
bool Ground::active() const { return !final(); }
bool Ground::attention() const {
    return notApplied != 0 || (receipt.terminal() && receipt.reason != "applied");
}
void GroundArchive::Observe(const core::GroundView& receipt) {
    const auto found = entries_.find(receipt.id);
    if (found != entries_.end() && found->second.terminal()) return;
    entries_[receipt.id] = receipt;
}
Grounds GroundArchive::Snapshot() const {
    Grounds result;
    if (!entries_.empty()) result.order.newest = entries_.rbegin()->first;
    for (const auto& entry : entries_) {
        Ground report; report.receipt = entry.second;
        for (const auto& member : report.receipt.members) {
            if (!member.terminal) ++report.pending;
            else if (member.accepted) ++report.accepted;
            else ++report.notApplied;
        }
        ClassifyOlder(result.order, report.receipt.id, report.active(), report.attention());
        result.reports.push_back(std::move(report));
    }
    return result;
}
} // namespace report_projection
