// linea - Stroke implementation. A stroke is the union of a quad for every segment, a join at every corner and a cap at
// every open end. Each piece winds clockwise, so the nonzero rule fills their union however they overlap. A dashed
// stroke is the union of its dashes' strokes, each dash an open stretch of its contour.
#include "linea/Stroke.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <numbers>
#include <numeric>
#include <span>
#include <utility>

namespace linea {
namespace {

// Round joins and caps take at least this many sides, and at most the other, however small or large they are.
constexpr int fewestSides = 8;
constexpr int mostSides = 256;
// Segments that turn by less than this, in radians, meet in a straight line and need no join.
constexpr double straight = 1e-9;

// A contour ready to stroke: its points without repeats, and its segments' unit directions and distances along it.
// Segment i runs from points[i] to the next point, on a closed line from the last point back to the first, and from
// positions[i] to positions[i + 1] along the line, so the last position is the line's length.
struct Polyline {
    std::vector<Vec2> points;
    std::vector<Vec2> directions;
    std::vector<double> positions;
    bool closed = false;
};

// A place on a polyline: a distance along it, and the segment that holds it. A stretch that runs on around a closed
// line, through its first point, counts the segments it reaches after that on from the last one.
struct Stop {
    std::size_t segment{};
    double position{};
};

// The stretch of a polyline from one distance along it to another.
struct Dash {
    double from{};
    double to{};
};

void add(std::vector<Contour> &out, std::vector<Vec2> points) {
    Contour piece{std::move(points), true};
    if (twiceSignedArea(piece) < 0.0)
        std::reverse(piece.points.begin(), piece.points.end());
    out.push_back(std::move(piece));
}

// A polygon with the same area as the circle: its corners lie a little outside and its edges a little inside, where an
// inscribed polygon would lie wholly inside and leave every round join and cap thin.
void addCircle(std::vector<Contour> &out, Vec2 center, double radius, double tolerance) {
    // A chord of a circle of radius r through an angle a strays r (1 - cos(a / 2)) from the arc. Equal areas then put
    // the corners 2/3 of that outside the circle and the edges' middles 1/3 of it inside.
    int sides = fewestSides;
    if (tolerance < radius)
        sides = static_cast<int>(std::clamp(std::ceil(std::numbers::pi / std::acos(1.0 - tolerance / radius)),
                                            static_cast<double>(fewestSides), static_cast<double>(mostSides)));
    const double step = 2.0 * std::numbers::pi / sides;
    const double corner = radius * std::sqrt(step / std::sin(step));
    std::vector<Vec2> points;
    points.reserve(static_cast<std::size_t>(sides));
    for (int i = 0; i < sides; ++i)
        points.push_back(polar(center, corner, step * i));
    add(out, std::move(points));
}

// The band a pen sweeps from @p a to @p b, heading along the unit vector @p direction.
void addSegment(std::vector<Contour> &out, Vec2 a, Vec2 b, Vec2 direction, double half) {
    const Vec2 normal = perpendicular(direction) * half;
    add(out, {a + normal, b + normal, b - normal, a - normal});
}

// The join at @p corner between a segment along @p before and the next along @p after, both unit vectors.
void addJoin(std::vector<Contour> &out, Vec2 corner, Vec2 before, Vec2 after, const StrokeStyle &style, double half,
             double tolerance) {
    const double turn = cross(before, after);
    if (std::abs(turn) < straight && dot(before, after) > 0.0)
        return;
    if (style.join == Join::round) {
        addCircle(out, corner, half, tolerance);
        return;
    }
    // The outer side of the corner is the one the path turns away from.
    const double side = turn > 0.0 ? -1.0 : 1.0;
    const Vec2 out0 = perpendicular(before) * (half * side);
    const Vec2 out1 = perpendicular(after) * (half * side);
    if (style.join == Join::miter) {
        const Vec2 bisector = normalized(out0 + out1);
        const double cosHalf = dot(bisector, out0) / half;
        if (cosHalf > 0.0 && 1.0 / cosHalf <= style.miterLimit) {
            add(out, {corner, corner + out0, corner + bisector * (half / cosHalf), corner + out1});
            return;
        }
    }
    add(out, {corner, corner + out0, corner + out1});
}

// The cap at @p end of an open stroke, which leaves the end along the unit vector @p outward.
void addCap(std::vector<Contour> &out, Vec2 end, Vec2 outward, const StrokeStyle &style, double half,
            double tolerance) {
    switch (style.cap) {
    case Cap::butt:
        return;
    case Cap::round:
        addCircle(out, end, half, tolerance);
        return;
    case Cap::square: {
        const Vec2 normal = perpendicular(outward) * half;
        const Vec2 beyond = end + outward * half;
        add(out, {end + normal, beyond + normal, beyond - normal, end - normal});
        return;
    }
    }
}

// The pen mark of a stroke with no length, which its two caps make between them: a round mark, or a square one that
// faces along the unit vector @p direction.
void addDot(std::vector<Contour> &out, Vec2 at, Vec2 direction, const StrokeStyle &style, double half,
            double tolerance) {
    switch (style.cap) {
    case Cap::butt:
        return;
    case Cap::round:
        addCircle(out, at, half, tolerance);
        return;
    case Cap::square: {
        const Vec2 along = direction * half;
        const Vec2 across = perpendicular(direction) * half;
        add(out, {at - along - across, at + along - across, at + along + across, at - along + across});
        return;
    }
    }
}

Polyline polyline(const Contour &contour) {
    Polyline line;
    // Repeated points have no direction to stroke along.
    for (const Vec2 point : contour.points)
        if (line.points.empty() || point != line.points.back())
            line.points.push_back(point);
    line.closed = contour.closed && line.points.size() > 2;
    if (line.closed && line.points.back() == line.points.front())
        line.points.pop_back();
    if (line.points.empty())
        return line;
    const std::size_t n = line.points.size();
    const std::size_t segments = line.closed ? n : n - 1;
    line.positions.push_back(0.0);
    for (std::size_t i = 0; i < segments; ++i) {
        const Vec2 delta = line.points[(i + 1) % n] - line.points[i];
        const double size = length(delta);
        line.directions.push_back(delta / size);
        line.positions.push_back(line.positions.back() + size);
    }
    return line;
}

// The point @p position along @p line, on its segment @p segment.
Vec2 pointOn(const Polyline &line, std::size_t segment, double position) {
    const Vec2 start = line.points[segment];
    const Vec2 end = line.points[(segment + 1) % line.points.size()];
    const double from = line.positions[segment];
    const double to = line.positions[segment + 1];
    if (position <= from)
        return start;
    if (position >= to)
        return end;
    return lerp(start, end, (position - from) / (to - from));
}

// The distances along @p line, which has at least one segment, at which one segment ends and the next starts, short of
// the line's end.
std::span<const double> corners(const Polyline &line) {
    return std::span(line.positions).subspan(1, line.directions.size() - 1);
}

// The segment that a dash starting @p position along @p line leaves along: the last to start at or before it.
std::size_t segmentLeaving(const Polyline &line, double position) {
    const std::span<const double> starts = corners(line);
    return static_cast<std::size_t>(std::upper_bound(starts.begin(), starts.end(), position) - starts.begin());
}

// The segment that a dash ending @p position along @p line arrives along: the first to end at or after it.
std::size_t segmentArriving(const Polyline &line, double position) {
    const std::span<const double> ends = corners(line);
    return static_cast<std::size_t>(std::lower_bound(ends.begin(), ends.end(), position) - ends.begin());
}

// The open stroke of @p line from @p from to @p to: a cap at each end and a join at each corner between.
void addRun(std::vector<Contour> &out, const Polyline &line, Stop from, Stop to, const StrokeStyle &style, double half,
            double tolerance) {
    const std::size_t segments = line.directions.size();
    Vec2 a = pointOn(line, from.segment, from.position);
    addCap(out, a, -line.directions[from.segment], style, half, tolerance);
    for (std::size_t k = from.segment; k < to.segment; ++k) {
        const std::size_t next = (k + 1) % segments;
        const Vec2 corner = line.points[next];
        addSegment(out, a, corner, line.directions[k % segments], half);
        addJoin(out, corner, line.directions[k % segments], line.directions[next], style, half, tolerance);
        a = corner;
    }
    const std::size_t last = to.segment % segments;
    const Vec2 b = pointOn(line, last, to.position);
    addSegment(out, a, b, line.directions[last], half);
    addCap(out, b, line.directions[last], style, half, tolerance);
}

// The stroke of the whole of the closed @p line: a join at every corner, and no caps.
void addLoop(std::vector<Contour> &out, const Polyline &line, const StrokeStyle &style, double half, double tolerance) {
    const std::size_t n = line.points.size();
    for (std::size_t i = 0; i < n; ++i) {
        addSegment(out, line.points[i], line.points[(i + 1) % n], line.directions[i], half);
        addJoin(out, line.points[i], line.directions[(i + n - 1) % n], line.directions[i], style, half, tolerance);
    }
}

// The dash pattern that @p style asks for, its array repeated to an even count, or nothing when it strokes solid.
std::vector<double> dashPattern(const StrokeStyle &style) {
    const std::vector<double> &lengths = style.dashArray;
    const auto valid = [](double l) { return std::isfinite(l) && l >= 0.0; };
    const auto positive = [](double l) { return l > 0.0; };
    if (!std::isfinite(style.dashOffset) || !std::all_of(lengths.begin(), lengths.end(), valid) ||
        std::none_of(lengths.begin(), lengths.end(), positive))
        return {};
    std::vector<double> pattern = lengths;
    if (pattern.size() % 2 == 1)
        pattern.insert(pattern.end(), lengths.begin(), lengths.end());
    return pattern;
}

// The dashes that SVG's dash positions algorithm cuts from a line @p total long with @p pattern, which sums to
// @p period, starting @p offset into the pattern.
std::vector<Dash> dashPositions(double total, const std::vector<double> &pattern, double period, double offset) {
    double phase = std::fmod(offset, period);
    if (phase < 0.0)
        phase += period;
    // The line starts in the first length of the pattern that reaches the phase, with what remains of it to run.
    std::size_t index = 0;
    double reached = pattern[0];
    while (reached < phase && index + 1 < pattern.size())
        reached += pattern[++index];
    std::vector<Dash> dashes;
    double position = 0.0;
    double run = reached - phase;
    for (;;) {
        // A length that would run past the line's end stops exactly at it, however the sum rounds.
        const double end = std::min(position + run, total);
        if (index % 2 == 0)
            dashes.push_back({position, end});
        if (!(end < total))
            break;
        position = end;
        index = (index + 1) % pattern.size();
        run = pattern[index];
    }
    return dashes;
}

// Strokes each of @p dashes along @p line, which has at least one segment. On a closed line, the last dash runs on into
// the first where the pattern is on through the line's first point, and a dash all the way around is the line's whole
// stroke.
void addDashes(std::vector<Contour> &out, const Polyline &line, const std::vector<Dash> &dashes,
               const StrokeStyle &style, double half, double tolerance) {
    std::span<const Dash> apart(dashes);
    if (line.closed && !dashes.empty() && dashes.front().from == 0.0 && dashes.back().to == line.positions.back()) {
        if (dashes.size() == 1) {
            addLoop(out, line, style, half, tolerance);
            return;
        }
        const Dash head = dashes.front();
        const Dash tail = dashes.back();
        // A first dash of no length only touches the first point, where the last dash ends, so the two stay apart.
        if (head.to > 0.0) {
            addRun(out, line, {segmentLeaving(line, tail.from), tail.from},
                   {line.directions.size() + segmentArriving(line, head.to), head.to}, style, half, tolerance);
            apart = apart.subspan(1, apart.size() - 2);
        }
    }
    for (const Dash &dash : apart) {
        const std::size_t first = segmentLeaving(line, dash.from);
        if (dash.to == dash.from)
            addDot(out, pointOn(line, first, dash.from), line.directions[first], style, half, tolerance);
        else
            addRun(out, line, {first, dash.from}, {segmentArriving(line, dash.to), dash.to}, style, half, tolerance);
    }
}

} // namespace

std::vector<Contour> strokeOutline(const std::vector<Contour> &contours, const StrokeStyle &style, double tolerance) {
    std::vector<Contour> out;
    const double half = style.width / 2.0;
    if (!(half > 0.0))
        return out;
    std::vector<Polyline> lines;
    for (const Contour &contour : contours) {
        Polyline line = polyline(contour);
        if (!line.points.empty())
            lines.push_back(std::move(line));
    }
    std::vector<double> pattern = dashPattern(style);
    const double period = std::accumulate(pattern.begin(), pattern.end(), 0.0);
    if (!pattern.empty()) {
        // Each repeat of the pattern holds a dash for every two of its lengths, as Skia counts them. A contour that
        // reaches infinity, or has a coordinate that is not a number, fails the count, and strokes solid too.
        double count = 0.0;
        for (const Polyline &line : lines)
            count += line.positions.back() / period * static_cast<double>(pattern.size() / 2);
        if (!(count <= mostDashes))
            pattern.clear();
    }
    for (const Polyline &line : lines) {
        const double total = line.positions.back();
        if (line.directions.empty()) {
            // A contour of one point is a dot, there unless a dash pattern is off where it starts. It has no direction
            // of its own, so a square dot lines up with the axes.
            if (pattern.empty() || !dashPositions(total, pattern, period, style.dashOffset).empty())
                addDot(out, line.points.front(), {1.0, 0.0}, style, half, tolerance);
        } else if (!pattern.empty()) {
            addDashes(out, line, dashPositions(total, pattern, period, style.dashOffset), style, half, tolerance);
        } else if (line.closed) {
            addLoop(out, line, style, half, tolerance);
        } else {
            addRun(out, line, {0, 0.0}, {line.directions.size() - 1, total}, style, half, tolerance);
        }
    }
    return out;
}

} // namespace linea
