// linea - Boolean implementation: flatten both paths, snap them to an integer grid, and trace the result's boundary
// through the arrangement their snapped edges make.
//
// After snap rounding, edges meet only at their ends, so they form a planar arrangement. Each arc of it knows how each
// operand's winding number changes across it; a sweep from left to right gives every arc the winding numbers beside
// it; and the arcs where the operation's result differs from one side to the other bound the result. A directed edge's
// positive side is the one where cross(direction, offset) is positive, its right on a y-down canvas. The result's edges
// run with the result on that side, so its outer contours wind clockwise and its holes counterclockwise.
#include "linea/Boolean.hpp"

#include "Snap.hpp"

#include "linea/Flatten.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <set>
#include <vector>

namespace linea {
namespace {

using detail::cross;
using detail::GridEdge;
using detail::GridPoint;
using Coord = std::int64_t;
// Winding numbers of the first operand and the second.
using Windings = std::array<int, 2>;

// The grid's spacing is at most this share of the tolerance, so that snapping to it moves a shape by far less than
// flattening does.
constexpr double gridShare = 1.0 / 64.0;

// Grid point q stands for origin + q * spacing. The spacing is a power of two, so the mapping is exact for every point
// that lies on the grid.
struct Grid {
    Vec2 origin{};
    double spacing{};
};

bool finite(Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }

// The largest power of two no more than @p x, and the smallest no less, for positive finite @p x.
double powerOfTwoAtMost(double x) { return std::ldexp(1.0, std::ilogb(x)); }

double powerOfTwoAtLeast(double x) {
    const double below = powerOfTwoAtMost(x);
    return below == x ? below : 2.0 * below;
}

// The subpaths of @p path as polylines, leaving out any with a point that is not finite.
std::vector<Contour> outline(const Path &path, double tolerance) {
    std::vector<Contour> contours = flatten(path, tolerance);
    std::erase_if(contours, [](const Contour &contour) {
        return !std::all_of(contour.points.begin(), contour.points.end(), finite);
    });
    return contours;
}

// The grid for points within @p reach of @p center on either axis, or nothing when they all coincide.
std::optional<Grid> gridFor(Vec2 center, double reach, double tolerance) {
    if (!(reach > 0.0))
        return std::nullopt;
    // A point rounds at most half a step from where it was, and the origin at most half a step from the center, so
    // gridReach - 1 steps of reach keep every grid coordinate within gridReach.
    double spacing = powerOfTwoAtLeast(reach / static_cast<double>(detail::gridReach - 1));
    if (const double wanted = tolerance * gridShare; wanted > 0.0 && std::isfinite(wanted))
        spacing = std::max(spacing, powerOfTwoAtMost(wanted));
    return Grid{{spacing * std::round(center.x / spacing), spacing * std::round(center.y / spacing)}, spacing};
}

// The point of @p grid nearest @p p.
GridPoint toGrid(Vec2 p, const Grid &grid) {
    return {static_cast<Coord>(std::llround((p.x - grid.origin.x) / grid.spacing)),
            static_cast<Coord>(std::llround((p.y - grid.origin.y) / grid.spacing))};
}

// The point that grid point @p q of @p grid stands for.
Vec2 fromGrid(GridPoint q, const Grid &grid) {
    return grid.origin + Vec2{static_cast<double>(q.x), static_cast<double>(q.y)} * grid.spacing;
}

// Appends the edges of @p contours, each closed, snapped to @p grid and labeled @p operand.
void appendEdges(const std::vector<Contour> &contours, const Grid &grid, int operand, std::vector<GridEdge> &edges) {
    for (const Contour &contour : contours)
        for (std::size_t i = 0; i < contour.points.size(); ++i)
            edges.push_back({toGrid(contour.points[i], grid),
                             toGrid(contour.points[(i + 1) % contour.points.size()], grid), operand});
}

GridPoint offset(GridPoint from, GridPoint to) { return {to.x - from.x, to.y - from.y}; }

// An edge of the arrangement: every snapped piece between two grid points, from the one first in the order of
// GridPoint to the other. Crossing it from its negative side to its positive side raises each operand's winding number
// by rise.
struct Arc {
    GridPoint start;
    GridPoint end;
    Windings rise{};
    // The winding numbers on its negative side, which is below it, toward smaller y, or for an upright arc toward
    // larger x.
    Windings below{};
};

Windings above(const Arc &arc) { return {arc.below[0] + arc.rise[0], arc.below[1] + arc.rise[1]}; }

// The arcs that the snapped @p pieces make, sorted by start. Pieces between the same two points merge, their
// contributions to each winding number summed, and arcs across which no winding number changes go.
std::vector<Arc> arcsOf(const std::vector<GridEdge> &pieces) {
    std::vector<Arc> arcs;
    arcs.reserve(pieces.size());
    for (const GridEdge &piece : pieces) {
        const bool forward = piece.from < piece.to;
        Arc arc{forward ? piece.from : piece.to, forward ? piece.to : piece.from, {}, {}};
        arc.rise[static_cast<std::size_t>(piece.operand)] = forward ? 1 : -1;
        arcs.push_back(arc);
    }
    std::sort(arcs.begin(), arcs.end(),
              [](const Arc &a, const Arc &b) { return a.start != b.start ? a.start < b.start : a.end < b.end; });
    std::vector<Arc> merged;
    for (const Arc &arc : arcs) {
        if (!merged.empty() && merged.back().start == arc.start && merged.back().end == arc.end) {
            merged.back().rise[0] += arc.rise[0];
            merged.back().rise[1] += arc.rise[1];
        } else {
            merged.push_back(arc);
        }
    }
    std::erase_if(merged, [](const Arc &arc) { return arc.rise == Windings{}; });
    return merged;
}

// Gives every arc of @p arcs, sorted by start, the winding numbers below it. A sweep from left to right keeps the arcs
// it crosses in order from the lowest, and the region below an arc is the region above the arc below it there, or
// outside both shapes. Arcs meet only at their ends, so that order holds between the points where they start and end.
void wind(std::vector<Arc> &arcs) {
    std::vector<std::size_t> byEnd(arcs.size());
    for (std::size_t i = 0; i < byEnd.size(); ++i)
        byEnd[i] = i;
    std::sort(byEnd.begin(), byEnd.end(), [&](std::size_t a, std::size_t b) { return arcs[a].end < arcs[b].end; });
    // Arcs that start together go by direction; an arc that starts at the sweep's point goes by which side of another
    // the point lies on; and of two arcs already crossed, the one that began later lies on one side of the other.
    GridPoint at{};
    const auto lower = [&](std::size_t i, std::size_t j) {
        const Arc &a = arcs[i];
        const Arc &b = arcs[j];
        if (a.start == b.start)
            return cross(GridPoint{}, offset(a.start, a.end), offset(b.start, b.end)) > 0;
        if (a.start == at)
            return cross(b.start, b.end, at) < 0;
        if (b.start == at)
            return cross(a.start, a.end, at) > 0;
        if (b.start < a.start)
            return cross(b.start, b.end, a.start) < 0;
        return cross(a.start, a.end, b.start) > 0;
    };
    std::set<std::size_t, decltype(lower)> crossed(lower);
    std::vector<std::set<std::size_t, decltype(lower)>::iterator> place(arcs.size());
    std::size_t nextStart = 0;
    std::size_t nextEnd = 0;
    // Once every arc has started, the rest only end.
    while (nextStart < arcs.size()) {
        GridPoint point = arcs[nextStart].start;
        if (nextEnd < byEnd.size() && arcs[byEnd[nextEnd]].end < point)
            point = arcs[byEnd[nextEnd]].end;
        at = point;
        while (nextEnd < byEnd.size() && arcs[byEnd[nextEnd]].end == point)
            crossed.erase(place[byEnd[nextEnd++]]);
        const std::size_t firstNew = nextStart;
        while (nextStart < arcs.size() && arcs[nextStart].start == point) {
            place[nextStart] = crossed.insert(nextStart).first;
            ++nextStart;
        }
        if (firstNew == nextStart)
            continue;
        // The arcs that start here lie together; wind them from the lowest up.
        auto it = place[firstNew];
        while (it != crossed.begin() && arcs[*std::prev(it)].start == point)
            --it;
        Windings winding = it == crossed.begin() ? Windings{} : above(arcs[*std::prev(it)]);
        for (; it != crossed.end() && arcs[*it].start == point; ++it) {
            arcs[*it].below = winding;
            winding = above(arcs[*it]);
        }
    }
}

bool fills(int winding, FillRule rule) { return rule == FillRule::nonZero ? winding != 0 : winding % 2 != 0; }

// Whether the region with winding numbers @p w belongs to the result.
bool inResult(const Windings &w, BooleanOp op, FillRule rule) {
    const bool a = fills(w[0], rule);
    const bool b = fills(w[1], rule);
    switch (op) {
    case BooleanOp::unite:
        return a || b;
    case BooleanOp::intersect:
        return a && b;
    case BooleanOp::subtract:
        return a && !b;
    case BooleanOp::exclude:
        return a != b;
    }
    return false;
}

// A directed edge of the result, with the result on its positive side.
struct Step {
    GridPoint from;
    GridPoint to;
};

// How far @p d lies from @p back, turning from @p back toward its negative side: 0 within a half turn, 1 exactly
// opposite, 2 beyond.
int turnGroup(GridPoint back, GridPoint d) {
    const Coord turn = cross(GridPoint{}, back, d);
    if (turn < 0)
        return 0;
    return turn == 0 ? 1 : 2;
}

// Whether @p d comes before @p e turning from @p back toward its negative side.
bool sooner(GridPoint back, GridPoint d, GridPoint e) {
    const int dGroup = turnGroup(back, d);
    const int eGroup = turnGroup(back, e);
    if (dGroup != eGroup)
        return dGroup < eGroup;
    return cross(GridPoint{}, d, e) < 0;
}

// The contours that @p steps make, each a simple cycle. Arriving at a point, a contour leaves by the step that turns
// most sharply toward the result's side, the next step around the point, so that shapes touching only at a corner stay
// apart; a walk that comes back to a point it has passed splits there. A point where a contour runs straight on, and
// no other contour passes, is no corner.
std::vector<std::vector<GridPoint>> contoursOf(std::vector<Step> steps) {
    const auto byFrom = [](const Step &a, const Step &b) { return a.from < b.from; };
    std::sort(steps.begin(), steps.end(), byFrom);
    const auto leaving = [&](GridPoint p) { return std::equal_range(steps.begin(), steps.end(), Step{p, p}, byFrom); };
    std::vector<bool> used(steps.size(), false);
    std::vector<std::vector<GridPoint>> contours;
    for (std::size_t start = 0; start < steps.size(); ++start) {
        if (used[start])
            continue;
        // At every point the steps around it alternate between arriving and leaving, as each separates the result
        // from the rest, so the walk comes back to its first step.
        std::vector<GridPoint> walk;
        std::map<GridPoint, std::size_t> seen;
        std::size_t current = start;
        while (!used[current]) {
            used[current] = true;
            const GridPoint p = steps[current].from;
            if (const auto again = seen.find(p); again != seen.end()) {
                const std::size_t loop = again->second;
                contours.emplace_back(walk.begin() + static_cast<std::ptrdiff_t>(loop), walk.end());
                for (std::size_t i = loop; i < walk.size(); ++i)
                    seen.erase(walk[i]);
                walk.resize(loop);
            }
            seen[p] = walk.size();
            walk.push_back(p);
            const GridPoint back = offset(steps[current].to, steps[current].from);
            const auto [first, last] = leaving(steps[current].to);
            auto next = first;
            for (auto it = std::next(first); it != last; ++it)
                if (sooner(back, offset(it->from, it->to), offset(next->from, next->to)))
                    next = it;
            current = static_cast<std::size_t>(next - steps.begin());
        }
        contours.push_back(std::move(walk));
    }

    // One step leaves a point that only one contour passes, and there a contour that runs straight on needs no corner.
    for (std::vector<GridPoint> &contour : contours) {
        std::vector<GridPoint> corners;
        for (std::size_t i = 0; i < contour.size(); ++i) {
            const GridPoint previous = contour[(i + contour.size() - 1) % contour.size()];
            const GridPoint next = contour[(i + 1) % contour.size()];
            const auto [first, last] = leaving(contour[i]);
            if (last - first == 1 && cross(previous, contour[i], next) == 0)
                continue;
            corners.push_back(contour[i]);
        }
        contour = std::move(corners);
    }
    return contours;
}

} // namespace

Path combine(const Path &a, const Path &b, BooleanOp op, double tolerance, FillRule rule) {
    const std::vector<Contour> first = outline(a, tolerance);
    const std::vector<Contour> second = outline(b, tolerance);

    // Halves keep the bounds' arithmetic finite for any finite coordinates.
    Vec2 low{};
    Vec2 high{};
    bool any = false;
    for (const std::vector<Contour> *contours : {&first, &second}) {
        for (const Contour &contour : *contours) {
            for (const Vec2 p : contour.points) {
                const Vec2 half = p * 0.5;
                low = any ? Vec2{std::min(low.x, half.x), std::min(low.y, half.y)} : half;
                high = any ? Vec2{std::max(high.x, half.x), std::max(high.y, half.y)} : half;
                any = true;
            }
        }
    }
    Path result;
    const std::optional<Grid> grid = gridFor(low + high, std::max(high.x - low.x, high.y - low.y), tolerance);
    if (!grid)
        return result;

    std::vector<GridEdge> edges;
    appendEdges(first, *grid, 0, edges);
    appendEdges(second, *grid, 1, edges);
    std::vector<Arc> arcs = arcsOf(detail::snapRound(edges));
    wind(arcs);
    std::vector<Step> steps;
    for (const Arc &arc : arcs) {
        const bool belowIn = inResult(arc.below, op, rule);
        const bool aboveIn = inResult(above(arc), op, rule);
        if (belowIn != aboveIn)
            steps.push_back(aboveIn ? Step{arc.start, arc.end} : Step{arc.end, arc.start});
    }
    for (const std::vector<GridPoint> &contour : contoursOf(std::move(steps))) {
        result.moveTo(fromGrid(contour.front(), *grid));
        for (std::size_t i = 1; i < contour.size(); ++i)
            result.lineTo(fromGrid(contour[i], *grid));
        result.close();
    }
    return result;
}

} // namespace linea
