#include "wb_group_math.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace wb_group_math {
namespace {
using proj_codec::Bounds;
using proj_codec::Point;
using proj_codec::Record;

bool finite(Point p) { return std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z); }

bool valid(Bounds b) {
    return finite(b.anchor) && finite(b.min) && finite(b.max) &&
        b.min.x <= b.anchor.x && b.anchor.x <= b.max.x &&
        b.min.y <= b.anchor.y && b.anchor.y <= b.max.y &&
        b.min.z <= b.anchor.z && b.anchor.z <= b.max.z;
}

bool valid(const Record& r) {
    return r.state == Record::State::Placeable && finite(r.pos) &&
        std::isfinite(r.yaw) && std::isfinite(r.pitch) && std::isfinite(r.roll) &&
        std::isfinite(r.scale) && r.scale > 0;
}

// Ry * Rx * Rz, the same order the engine's world transform uses.
Point rotate(Point v, double yaw, double pitch, double roll) {
    const double rad = std::acos(-1.0) / 180.0;
    const double z = roll * rad, x = pitch * rad, y = yaw * rad;
    const double cz = std::cos(z), sz = std::sin(z);
    const Point rz{cz * v.x - sz * v.y, sz * v.x + cz * v.y, v.z};
    const double cx = std::cos(x), sx = std::sin(x);
    const Point rx{rz.x, cx * rz.y - sx * rz.z, sx * rz.y + cx * rz.z};
    const double cy = std::cos(y), sy = std::sin(y);
    return {cy * rx.x + sy * rx.z, rx.y, -sy * rx.x + cy * rx.z};
}
} // namespace

bool ComputeBounds(const std::vector<Member>& members, const Bounds* saved, Bounds& out) {
    if (members.empty()) return false;
    if (saved) {
        if (!valid(*saved)) return false;
        out = *saved;
        return true;
    }
    const double inf = std::numeric_limits<double>::infinity();
    Bounds result; result.min = {inf, inf, inf}; result.max = {-inf, -inf, -inf};
    for (const Member& member : members) {
        const Record& r = member.record;
        if (!valid(r)) return false;
        const PrefabBox& box = member.box;
        if (box.known && (!finite(box.size) || !finite(box.center) ||
            box.size.x <= 0 || box.size.y <= 0 || box.size.z <= 0)) return false;
        const Point center = box.known ? box.center : Point{0, 0, 0};
        const Point size = box.known ? box.size : Point{2, 2, 2};
        result.approximate |= !box.known;
        for (int k = 0; k < 8; ++k) {
            const Point local{center.x + ((k & 1) ? size.x : -size.x) * .5,
                              center.y + ((k & 2) ? size.y : -size.y) * .5,
                              center.z + ((k & 4) ? size.z : -size.z) * .5};
            const Point v = rotate(local, r.yaw, r.pitch, r.roll);
            const Point p{r.pos.x + r.scale * v.x, r.pos.y + r.scale * v.y, r.pos.z + r.scale * v.z};
            if (!finite(p)) return false;
            result.min.x = (std::min)(result.min.x, p.x); result.max.x = (std::max)(result.max.x, p.x);
            result.min.y = (std::min)(result.min.y, p.y); result.max.y = (std::max)(result.max.y, p.y);
            result.min.z = (std::min)(result.min.z, p.z); result.max.z = (std::max)(result.max.z, p.z);
        }
    }
    result.anchor = {result.min.x * .5 + result.max.x * .5, result.min.y,
                     result.min.z * .5 + result.max.z * .5};
    if (!valid(result)) return false;
    out = result;
    return true;
}

bool AnchorTransform(const std::vector<Record>& source, const Bounds& bounds, Point target,
                     double deltaYaw, double factor, std::vector<Record>& out) {
    if (source.empty() || !valid(bounds) || !finite(target) ||
        !std::isfinite(deltaYaw) || !std::isfinite(factor) || factor <= 0) return false;
    std::vector<Record> result;
    result.reserve(source.size());
    for (const Record& r : source) {
        if (!valid(r)) return false;
        const Point delta{r.pos.x - bounds.anchor.x, r.pos.y - bounds.anchor.y, r.pos.z - bounds.anchor.z};
        const Point turned = rotate(delta, deltaYaw, 0, 0);
        Record next = r;
        next.pos = {target.x + factor * turned.x, target.y + factor * turned.y, target.z + factor * turned.z};
        next.yaw = r.yaw + deltaYaw;
        next.scale = r.scale * factor;
        if (!finite(next.pos) || !std::isfinite(next.yaw) || !std::isfinite(next.scale) || next.scale <= 0) return false;
        result.push_back(std::move(next));
    }
    out = std::move(result);
    return true;
}

} // namespace wb_group_math
