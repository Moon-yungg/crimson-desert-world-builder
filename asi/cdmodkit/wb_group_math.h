#pragma once
#include "proj_codec.h"
#include <vector>

// Pure disk-space geometry (C4). A prefab box is local to its pivot; saved bounds
// are authoritative even when the receiver has a different prefab cache. Every
// entry point is transactional: false leaves out unchanged.
namespace wb_group_math {

struct PrefabBox {
    proj_codec::Point size, center;
    bool known = false;
};

struct Member {
    proj_codec::Record record;
    PrefabBox box;
};

// Bounds for the member set: all eight corners of every box under Ry*Rx*Rz, with
// the anchor at the AABB bottom centre. Unknown boxes use a pivot-centred 2 m cube
// and mark the aggregate approximate. A saved envelope is returned unchanged (it is
// never recomputed from the receiver's measurements; a single imported survivor
// keeps its own saved position).
bool ComputeBounds(const std::vector<Member>& members, const proj_codec::Bounds* saved,
                   proj_codec::Bounds& out);

// Rigid group transform about the saved anchor: yaw delta plus one positive uniform
// scale; member tilt is preserved and the group tilt stays disabled. T is the
// destination of the source AABB anchor.
bool AnchorTransform(const std::vector<proj_codec::Record>& source,
                     const proj_codec::Bounds& bounds, proj_codec::Point target,
                     double deltaYaw, double factor, std::vector<proj_codec::Record>& out);

} // namespace wb_group_math
