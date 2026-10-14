// linea - Snap implementation: snap rounding after Hobby (1999) and Guibas and Marimont (1998), iterated as Halperin
// and Packer (2002) iterate it, so that no grid point a piece does not end in comes within half a step of it.
//
// A Bentley-Ottmann sweep finds every point where two edges cross. A crossing has rational coordinates, so the sweep
// decides how crossings compare, and on which side of an edge one lies, with a floating-point estimate whose error is
// bounded, and only where the estimate is too close to call, exactly with integers of 192 bits. Those crossings and
// the edges' corners make their grid cells hot, and each edge then bends through the hot cells it passes through,
// found through a grid of buckets. Here "above" means toward larger y, and a directed edge's positive side is the one
// where cross(direction, offset) is positive.
#include "Snap.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <iterator>
#include <optional>
#include <queue>
#include <set>
#include <utility>

namespace linea::detail {
namespace {

using Coord = std::int64_t;

int signOf(Coord value) { return (value > 0) - (value < 0); }

// A signed integer of 192 bits in two's complement, its least significant limb first: room for every product the
// sweep forms of a crossing's coordinates, whose numerators stay below 2^93 and denominators below 2^62.
struct Wide {
    std::array<std::uint64_t, 3> limbs{};
};

constexpr int limbBits = 64;
constexpr std::uint64_t lowHalf = 0xffffffff;

Wide wide(Coord value) {
    const std::uint64_t extension = value < 0 ? ~std::uint64_t{0} : 0;
    return {{static_cast<std::uint64_t>(value), extension, extension}};
}

Wide operator+(const Wide &a, const Wide &b) {
    Wide sum;
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < sum.limbs.size(); ++i) {
        const std::uint64_t partial = a.limbs[i] + b.limbs[i];
        sum.limbs[i] = partial + carry;
        carry = static_cast<std::uint64_t>(partial < a.limbs[i]) + static_cast<std::uint64_t>(sum.limbs[i] < partial);
    }
    return sum;
}

Wide operator-(const Wide &a) {
    Wide inverted;
    for (std::size_t i = 0; i < inverted.limbs.size(); ++i)
        inverted.limbs[i] = ~a.limbs[i];
    return inverted + wide(1);
}

Wide operator-(const Wide &a, const Wide &b) { return a + -b; }

bool isNegative(const Wide &a) { return (a.limbs.back() >> (limbBits - 1)) != 0; }

int signOf(const Wide &a) {
    if (isNegative(a))
        return -1;
    return (a.limbs[0] | a.limbs[1] | a.limbs[2]) != 0 ? 1 : 0;
}

// The 128-bit product of @p a and @p b, as its high and low halves, from four products of 32-bit halves.
std::pair<std::uint64_t, std::uint64_t> multiply(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t a0 = a & lowHalf;
    const std::uint64_t a1 = a >> (limbBits / 2);
    const std::uint64_t b0 = b & lowHalf;
    const std::uint64_t b1 = b >> (limbBits / 2);
    const std::uint64_t low = a0 * b0;
    const std::uint64_t mixed1 = a1 * b0;
    const std::uint64_t mixed2 = a0 * b1;
    const std::uint64_t middle = (low >> (limbBits / 2)) + (mixed1 & lowHalf) + (mixed2 & lowHalf);
    const std::uint64_t high =
        a1 * b1 + (mixed1 >> (limbBits / 2)) + (mixed2 >> (limbBits / 2)) + (middle >> (limbBits / 2));
    return {high, (middle << (limbBits / 2)) | (low & lowHalf)};
}

// The product of @p a and @p b, which must fit in 192 bits.
Wide operator*(const Wide &a, Coord b) {
    const Wide magnitude = isNegative(a) ? -a : a;
    const std::uint64_t factor = b < 0 ? 0 - static_cast<std::uint64_t>(b) : static_cast<std::uint64_t>(b);
    Wide product;
    std::uint64_t carry = 0;
    for (std::size_t i = 0; i < product.limbs.size(); ++i) {
        const auto [high, low] = multiply(magnitude.limbs[i], factor);
        product.limbs[i] = low + carry;
        carry = high + static_cast<std::uint64_t>(product.limbs[i] < low);
    }
    return isNegative(a) != (b < 0) ? -product : product;
}

// @p a within a few units in the last place: each limb rounds once, and two sums of nonnegative terms round once each.
double toDouble(const Wide &a) {
    if (isNegative(a))
        return -toDouble(-a);
    return std::ldexp(static_cast<double>(a.limbs[2]), 2 * limbBits) +
           std::ldexp(static_cast<double>(a.limbs[1]), limbBits) + static_cast<double>(a.limbs[0]);
}

// A bound on the relative error of the floating-point estimates below. Each estimate rounds at most eight times, each
// time by at most 2^-53 of what it rounds, and this allows four times that.
constexpr double estimateError = 0x1p-48;

// An edge as the sweep meets it: from its end first in the order of GridPoint, to the other.
struct Segment {
    GridPoint left;
    GridPoint right;
};

GridPoint direction(const Segment &s) { return {s.right.x - s.left.x, s.right.y - s.left.y}; }

// A point where the sweep stops: a segment's end, or where two segments cross. A crossing keeps the two segments it
// lies on, which pass through it and give its exact place; both kinds keep their place as doubles, within
// estimateError of it.
struct Event {
    double nearX = 0.0;
    double nearY = 0.0;
    GridPoint end{};
    const Segment *on1 = nullptr;
    const Segment *on2 = nullptr;
};

// A point at (x / w, y / w), w positive.
struct Exact {
    Wide x;
    Wide y;
    Coord w = 1;
};

Event endEvent(GridPoint p) { return {static_cast<double>(p.x), static_cast<double>(p.y), p, nullptr, nullptr}; }

// Where @p s and @p t cross, for segments that cross properly: s.left + d (along / w), d being the direction of s.
Exact exactCrossing(const Segment &s, const Segment &t) {
    const GridPoint d = direction(s);
    const GridPoint e = direction(t);
    Coord w = cross(GridPoint{}, d, e);
    Coord along = cross(GridPoint{}, {t.left.x - s.left.x, t.left.y - s.left.y}, e);
    if (w < 0) {
        w = -w;
        along = -along;
    }
    return {wide(s.left.x) * w + wide(along) * d.x, wide(s.left.y) * w + wide(along) * d.y, w};
}

Exact exact(const Event &e) {
    if (!e.on1)
        return {wide(e.end.x), wide(e.end.y), 1};
    return exactCrossing(*e.on1, *e.on2);
}

Event crossingEvent(const Segment &s, const Segment &t) {
    const Exact where = exactCrossing(s, t);
    const auto scale = static_cast<double>(where.w);
    return {toDouble(where.x) / scale, toDouble(where.y) / scale, GridPoint{}, &s, &t};
}

// The sign of @p p minus @p q in the order of GridPoint: by x, then y.
int compare(const Event &p, const Event &q) {
    if (!p.on1 && !q.on1)
        return (p.end > q.end) - (p.end < q.end);
    if ((p.on1 == q.on1 && p.on2 == q.on2) || (p.on1 == q.on2 && p.on2 == q.on1))
        return 0;
    const auto settle = [](double a, double b) {
        const double slack = (std::abs(a) + std::abs(b)) * estimateError;
        return a < b - slack ? -1 : (a > b + slack ? 1 : 0);
    };
    if (const int byX = settle(p.nearX, q.nearX))
        return byX;
    const Exact a = exact(p);
    const Exact b = exact(q);
    if (const int byX = signOf(a.x * b.w - b.x * a.w))
        return byX;
    if (const int byY = settle(p.nearY, q.nearY))
        return byY;
    return signOf(a.y * b.w - b.y * a.w);
}

// The grid coordinate of the cell that holds @p numerator / @p w: the nearest, with halves rounding up, as cells are
// closed on their low sides and open on their high ones. @p near estimates the quotient.
Coord cellOf(const Wide &numerator, Coord w, double near) {
    // The estimate is off by at most one; settle it exactly, where 0 <= 2 numerator + w - 2 w q < 2 w.
    auto q = static_cast<Coord>(std::floor(near + 0.5));
    Wide rest = numerator * 2 + wide(w) - wide(w) * (2 * q);
    while (signOf(rest) < 0) {
        --q;
        rest = rest + wide(2 * w);
    }
    while (signOf(rest - wide(2 * w)) >= 0) {
        ++q;
        rest = rest - wide(2 * w);
    }
    return q;
}

// Whether the estimate @p near settles its cell, lying further than its error from the cell's sides.
bool settlesCell(double near) {
    const double shifted = near + 0.5;
    const double below = std::floor(shifted);
    const double slack = 2.0 * std::abs(near) * estimateError;
    return shifted - below > slack && below + 1.0 - shifted > slack;
}

GridPoint cellOf(const Event &e) {
    if (!e.on1)
        return e.end;
    if (settlesCell(e.nearX) && settlesCell(e.nearY))
        return {static_cast<Coord>(std::floor(e.nearX + 0.5)), static_cast<Coord>(std::floor(e.nearY + 0.5))};
    const Exact where = exact(e);
    return {cellOf(where.x, where.w, e.nearX), cellOf(where.y, where.w, e.nearY)};
}

// The side of @p s that @p e lies on: 1 above it, which is its positive side, 0 on its line, -1 below.
int side(const Segment &s, const Event &e) {
    if (!e.on1)
        return signOf(cross(s.left, s.right, e.end));
    if (&s == e.on1 || &s == e.on2)
        return 0;
    const auto dx = static_cast<double>(s.right.x - s.left.x);
    const auto dy = static_cast<double>(s.right.y - s.left.y);
    const auto leftX = static_cast<double>(s.left.x);
    const auto leftY = static_cast<double>(s.left.y);
    const double near = dx * (e.nearY - leftY) - dy * (e.nearX - leftX);
    const double slack =
        (std::abs(dx) * (std::abs(e.nearY) + std::abs(leftY)) + std::abs(dy) * (std::abs(e.nearX) + std::abs(leftX))) *
        estimateError;
    if (near > slack)
        return 1;
    if (near < -slack)
        return -1;
    const Exact p = exact(e);
    const Wide relativeX = p.x - wide(s.left.x) * p.w;
    const Wide relativeY = p.y - wide(s.left.y) * p.w;
    return signOf(relativeY * (s.right.x - s.left.x) - relativeX * (s.right.y - s.left.y));
}

// Whether @p s and @p t cross at one point inside both.
bool crossProperly(const Segment &s, const Segment &t) {
    return signOf(cross(s.left, s.right, t.left)) * signOf(cross(s.left, s.right, t.right)) < 0 &&
           signOf(cross(t.left, t.right, s.left)) * signOf(cross(t.left, t.right, s.right)) < 0;
}

// Stands for the sweep line's current point in lookups among the segments that cross the line.
struct AtSweep {};

// A place in the order of the segments the sweep line crosses. Where two neighbours cross they swap places, which
// keeps the order, so a place's segment may change while it sits in the order.
struct Place {
    mutable const Segment *segment = nullptr;
};

// Every point where segments end or cross, with Bentley and Ottmann's sweep (1979) as de Berg et al. state it, which
// also takes segments that share points, overlap or stand upright. The sweep line moves through points in the order of
// compare() and keeps the segments it crosses in their order along it, from the lowest.
class CrossingSweep {
  public:
    explicit CrossingSweep(std::vector<Segment> segments) : segments_(std::move(segments)) {}
    CrossingSweep(const CrossingSweep &) = delete;
    CrossingSweep &operator=(const CrossingSweep &) = delete;

    // The cells that hold every end and crossing, sorted by GridPoint, each once.
    std::vector<GridPoint> hotCells();

  private:
    struct Lower {
        using is_transparent = void;
        const CrossingSweep *sweep;
        bool operator()(const Place &s, const Place &t) const { return sweep->lower(*s.segment, *t.segment); }
        bool operator()(const Place &s, AtSweep) const { return side(*s.segment, sweep->point_) > 0; }
        bool operator()(AtSweep, const Place &s) const { return side(*s.segment, sweep->point_) < 0; }
    };
    struct Later {
        bool operator()(const Event &p, const Event &q) const { return compare(p, q) > 0; }
    };
    using Crossed = std::set<Place, Lower>;

    [[nodiscard]] bool lower(const Segment &s, const Segment &t) const;
    [[nodiscard]] std::size_t indexOf(const Segment &s) const {
        return static_cast<std::size_t>(&s - segments_.data());
    }
    void checkCrossing(const Segment &s, const Segment &t);
    void place(const Segment &s);
    bool swapAtCrossing(std::size_t nextStart);
    void passPoint(std::size_t &nextStart);

    std::vector<Segment> segments_;
    Event point_;
    // A point can be queued more than once; the sweep takes all its copies together.
    std::priority_queue<Event, std::vector<Event>, Later> events_;
    Crossed crossed_{Lower{this}};
    // Where each segment sits while the sweep line crosses it.
    std::vector<Crossed::iterator> placeOf_;
    // The segments through the sweep's point, and where those that go on past it sit afterwards.
    std::vector<const Segment *> through_;
    std::vector<Crossed::iterator> placed_;
};

// Whether @p s lies below @p t along the sweep line just past its point. Of two segments through the point, the lower
// is the one that turns clockwise from the other; of one through it and one not, the point's side of the other
// decides; and two that miss it keep the order in which the later of them began.
bool CrossingSweep::lower(const Segment &s, const Segment &t) const {
    const int sSide = side(s, point_);
    const int tSide = side(t, point_);
    if (sSide == 0 && tSide == 0) {
        if (const Coord turn = cross(GridPoint{}, direction(s), direction(t)))
            return turn > 0;
        return &s < &t; // Overlapping segments, in a fixed order.
    }
    if (sSide == 0)
        return tSide < 0;
    if (tSide == 0)
        return sSide > 0;
    if (t.left <= s.left) {
        const int at = signOf(cross(t.left, t.right, s.left));
        return at != 0 ? at < 0 : signOf(cross(t.left, t.right, s.right)) < 0;
    }
    const int at = signOf(cross(s.left, s.right, t.left));
    return at != 0 ? at > 0 : signOf(cross(s.left, s.right, t.right)) > 0;
}

// Queues where @p s and @p t cross, if they cross past the sweep line's point.
void CrossingSweep::checkCrossing(const Segment &s, const Segment &t) {
    if (!crossProperly(s, t))
        return;
    const Event where = crossingEvent(s, t);
    if (compare(where, point_) > 0)
        events_.push(where);
}

void CrossingSweep::place(const Segment &s) {
    const auto it = crossed_.insert(Place{&s}).first;
    placeOf_[indexOf(s)] = it;
    placed_.push_back(it);
}

// Passes a crossing of just two neighbours that both go on past it, swapping their places, and reports whether the
// point was one.
bool CrossingSweep::swapAtCrossing(std::size_t nextStart) {
    if (!point_.on1)
        return false;
    auto low = placeOf_[indexOf(*point_.on1)];
    auto high = placeOf_[indexOf(*point_.on2)];
    if (std::next(high) == low)
        std::swap(low, high);
    if (std::next(low) != high)
        return false;
    // No other segment may pass through, end at or start at the point.
    if ((low != crossed_.begin() && side(*std::prev(low)->segment, point_) == 0) ||
        (std::next(high) != crossed_.end() && side(*std::next(high)->segment, point_) == 0) ||
        (nextStart < segments_.size() && compare(endEvent(segments_[nextStart].left), point_) == 0))
        return false;
    std::swap(low->segment, high->segment);
    placeOf_[indexOf(*low->segment)] = low;
    placeOf_[indexOf(*high->segment)] = high;
    if (low != crossed_.begin())
        checkCrossing(*std::prev(low)->segment, *low->segment);
    if (std::next(high) != crossed_.end())
        checkCrossing(*high->segment, *std::next(high)->segment);
    return true;
}

// Passes the sweep's point in general: takes out the segments through it, which lie together from the first not
// below it, then puts back those that go on past it, with those that start there, in their order just past it.
void CrossingSweep::passPoint(std::size_t &nextStart) {
    auto first = crossed_.lower_bound(AtSweep{});
    auto last = first;
    through_.clear();
    while (last != crossed_.end() && side(*last->segment, point_) == 0)
        through_.push_back((last++)->segment);
    crossed_.erase(first, last);
    placed_.clear();
    for (const Segment *s : through_)
        if (compare(endEvent(s->right), point_) != 0)
            place(*s);
    while (nextStart < segments_.size() && compare(endEvent(segments_[nextStart].left), point_) == 0)
        place(segments_[nextStart++]);

    // Segments that have just become neighbours may cross further on: those around the point when none passes it,
    // else those on either side of the ones that do.
    if (placed_.empty()) {
        if (last != crossed_.begin() && last != crossed_.end())
            checkCrossing(*std::prev(last)->segment, *last->segment);
        return;
    }
    const auto isPlaced = [&](Crossed::iterator it) {
        return std::find(placed_.begin(), placed_.end(), it) != placed_.end();
    };
    auto lowest = placed_.front();
    while (lowest != crossed_.begin() && isPlaced(std::prev(lowest)))
        --lowest;
    auto highest = placed_.front();
    while (std::next(highest) != crossed_.end() && isPlaced(std::next(highest)))
        ++highest;
    if (lowest != crossed_.begin())
        checkCrossing(*std::prev(lowest)->segment, *lowest->segment);
    if (std::next(highest) != crossed_.end())
        checkCrossing(*highest->segment, *std::next(highest)->segment);
}

std::vector<GridPoint> CrossingSweep::hotCells() {
    std::sort(segments_.begin(), segments_.end(), [](const Segment &a, const Segment &b) { return a.left < b.left; });
    placeOf_.assign(segments_.size(), crossed_.end());
    for (const Segment &s : segments_) {
        events_.push(endEvent(s.left));
        events_.push(endEvent(s.right));
    }
    std::vector<GridPoint> cells;
    std::size_t nextStart = 0;
    while (!events_.empty()) {
        point_ = events_.top();
        events_.pop();
        while (!events_.empty() && compare(events_.top(), point_) == 0)
            events_.pop();
        cells.push_back(cellOf(point_));
        if (!swapAtCrossing(nextStart))
            passPoint(nextStart);
    }
    std::sort(cells.begin(), cells.end());
    cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    return cells;
}

// Where a segment first meets a cell: at num / den along it, from 0 at its start to 1 at its end, or just past there
// when the cell is open on that side.
struct Entry {
    Coord num = 0;
    Coord den = 1;
    bool open = false;
};

// The sign of @p a minus @p b. Numerators and denominators stay below 2^32, so each product is exact.
int compare(const Entry &a, const Entry &b) {
    const Coord left = a.num * b.den;
    const Coord right = b.num * a.den;
    return (left > right) - (left < right);
}

bool earlier(const Entry &a, const Entry &b) {
    const int order = compare(a, b);
    return order < 0 || (order == 0 && !a.open && b.open);
}

// Where the segment from @p a to @p b first meets the cell centered on @p c, if it does.
std::optional<Entry> entryInto(GridPoint a, GridPoint b, GridPoint c) {
    Entry first{0, 1, false};
    Entry last{1, 1, false};
    const auto raiseFirst = [&](Entry bound) {
        const int order = compare(bound, first);
        if (order > 0)
            first = bound;
        else if (order == 0)
            first.open = first.open || bound.open;
    };
    const auto lowerLast = [&](Entry bound) {
        const int order = compare(bound, last);
        if (order < 0)
            last = bound;
        else if (order == 0)
            last.open = last.open || bound.open;
    };
    // In doubled coordinates relative to a, the cell spans [2 (c - a) - 1, 2 (c - a) + 1) on each axis, and the
    // segment reaches 2 (b - a) t at t.
    const auto clip = [&](Coord reach, Coord centre) {
        const Coord low = 2 * centre - 1;
        const Coord high = low + 2;
        if (reach == 0)
            return low <= 0 && 0 < high;
        if (reach > 0) {
            raiseFirst({low, reach, false});
            lowerLast({high, reach, true});
        } else {
            lowerLast({-low, -reach, false});
            raiseFirst({-high, -reach, true});
        }
        return true;
    };
    if (!clip(2 * (b.x - a.x), c.x - a.x) || !clip(2 * (b.y - a.y), c.y - a.y))
        return std::nullopt;
    const int order = compare(first, last);
    if (order < 0 || (order == 0 && !first.open && !last.open))
        return first;
    return std::nullopt;
}

// The hot cells in square buckets of cells, so that a segment finds the cells it may pass through among those in the
// buckets it crosses. A bucket's sides fall between cells, so each cell lies in one bucket.
class HotIndex {
  public:
    explicit HotIndex(std::vector<GridPoint> hot);

    // Calls @p visit with every hot cell in the buckets that the segment from @p a to @p b, both hot, passes through.
    template <class Visit> void forEachNear(GridPoint a, GridPoint b, Visit visit) const;

  private:
    [[nodiscard]] std::size_t bucket(Coord column, Coord row) const {
        return static_cast<std::size_t>(row * columns_ + column);
    }

    GridPoint min_{};
    // Cells along a bucket's side, a power of two.
    Coord side_ = 1;
    Coord columns_ = 0;
    Coord rows_ = 0;
    std::vector<std::size_t> starts_;
    std::vector<GridPoint> cells_;
};

HotIndex::HotIndex(std::vector<GridPoint> hot) {
    if (hot.empty())
        return;
    GridPoint max = hot.front();
    min_ = hot.front();
    for (const GridPoint c : hot) {
        min_ = {std::min(min_.x, c.x), std::min(min_.y, c.y)};
        max = {std::max(max.x, c.x), std::max(max.y, c.y)};
    }
    const Coord spanX = max.x - min_.x + 1;
    const Coord spanY = max.y - min_.y + 1;
    // About one cell to a bucket where the cells spread evenly; no more buckets than twice the cells in any case.
    const auto most = static_cast<Coord>(2 * hot.size());
    const auto buckets = [&](Coord side) { return ((spanX - 1) / side + 1) * ((spanY - 1) / side + 1); };
    while (buckets(side_) > most)
        side_ *= 2;
    columns_ = (spanX - 1) / side_ + 1;
    rows_ = (spanY - 1) / side_ + 1;

    starts_.assign(static_cast<std::size_t>(columns_ * rows_) + 1, 0);
    for (const GridPoint c : hot)
        ++starts_[bucket((c.x - min_.x) / side_, (c.y - min_.y) / side_) + 1];
    for (std::size_t i = 1; i < starts_.size(); ++i)
        starts_[i] += starts_[i - 1];
    cells_.resize(hot.size());
    std::vector<std::size_t> filled(starts_.begin(), starts_.end() - 1);
    for (const GridPoint c : hot)
        cells_[filled[bucket((c.x - min_.x) / side_, (c.y - min_.y) / side_)]++] = c;
}

template <class Visit> void HotIndex::forEachNear(GridPoint a, GridPoint b, Visit visit) const {
    // In doubled coordinates relative to the lowest cell's low corner, bucket k spans [2 k side, 2 (k + 1) side) on
    // each axis, and the segment runs from a' = 2 (a - min) + 1 to b' = 2 (b - min) + 1. Walk the buckets it passes
    // through, stepping across whichever bucket side it reaches first.
    const Coord size = 2 * side_;
    const GridPoint from{2 * (a.x - min_.x) + 1, 2 * (a.y - min_.y) + 1};
    const GridPoint to{2 * (b.x - min_.x) + 1, 2 * (b.y - min_.y) + 1};
    const Coord dx = to.x - from.x;
    const Coord dy = to.y - from.y;
    Coord column = from.x / size;
    Coord row = from.y / size;
    const Coord lastColumn = to.x / size;
    const Coord lastRow = to.y / size;
    while (true) {
        const std::size_t index = bucket(column, row);
        for (std::size_t i = starts_[index]; i < starts_[index + 1]; ++i)
            visit(cells_[i]);
        if (column == lastColumn && row == lastRow)
            return;
        // How far along the segment the next side lies, as num / |d|; both are reached by its end, so each is at most
        // 1 and the products stay below 2^63.
        const Coord xNum = dx > 0 ? (column + 1) * size - from.x : from.x - column * size;
        const Coord yNum = dy > 0 ? (row + 1) * size - from.y : from.y - row * size;
        // On a tie the segment meets a corner of four buckets, which lies in the one whose low sides meet there:
        // crossing first the side the segment heads toward larger coordinates on reaches it.
        const Coord xReach = xNum * std::abs(dy);
        const Coord yReach = yNum * std::abs(dx);
        const bool stepColumn =
            row == lastRow || (column != lastColumn && (xReach < yReach || (xReach == yReach && dx > 0)));
        if (stepColumn)
            column += dx > 0 ? 1 : -1;
        else
            row += dy > 0 ? 1 : -1;
    }
}

// Bends segments through the hot cells they pass through.
class Router {
  public:
    explicit Router(std::vector<GridPoint> hot) : index_(std::move(hot)) {}

    // Appends to @p pieces the pieces of @p edge, each between hot cells' centers and passing through no other hot
    // cell, directed and labeled as @p edge is.
    void route(const GridEdge &edge, std::vector<GridEdge> &pieces);

  private:
    // Sets cells_ to the hot cells, other than its ends', that the segment from @p a to @p b passes through, in order
    // along it.
    void findCellsAlong(GridPoint a, GridPoint b);

    HotIndex index_;
    // Buffers kept from one segment to the next.
    std::vector<std::pair<Entry, GridPoint>> hits_;
    std::vector<GridPoint> cells_;
    std::vector<std::pair<GridPoint, GridPoint>> pending_;
};

void Router::findCellsAlong(GridPoint a, GridPoint b) {
    hits_.clear();
    index_.forEachNear(a, b, [&](GridPoint c) {
        if (c == a || c == b)
            return;
        if (const std::optional<Entry> entry = entryInto(a, b, c))
            hits_.emplace_back(*entry, c);
    });
    std::sort(hits_.begin(), hits_.end(), [](const auto &l, const auto &r) { return earlier(l.first, r.first); });
    cells_.clear();
    for (const auto &hit : hits_)
        cells_.push_back(hit.second);
}

void Router::route(const GridEdge &edge, std::vector<GridEdge> &pieces) {
    pending_.assign(1, {edge.from, edge.to});
    while (!pending_.empty()) {
        const auto [a, b] = pending_.back();
        pending_.pop_back();
        findCellsAlong(a, b);
        if (cells_.empty()) {
            pieces.push_back({a, b, edge.operand});
            continue;
        }
        GridPoint previous = a;
        for (const GridPoint c : cells_) {
            pending_.emplace_back(previous, c);
            previous = c;
        }
        pending_.emplace_back(previous, b);
    }
}

} // namespace

std::vector<GridEdge> snapRound(const std::vector<GridEdge> &edges) {
    std::vector<Segment> segments;
    for (const GridEdge &edge : edges)
        if (edge.from != edge.to)
            segments.push_back({std::min(edge.from, edge.to), std::max(edge.from, edge.to)});
    CrossingSweep sweep(std::move(segments));
    Router router(sweep.hotCells());
    std::vector<GridEdge> pieces;
    for (const GridEdge &edge : edges)
        if (edge.from != edge.to)
            router.route(edge, pieces);
    return pieces;
}

} // namespace linea::detail
