// linea - Measure implementation. A line's length is the distance between its ends. A curve's length is the integral of
// its speed, the length of its derivative. The curve is first cut where its speed is least, then Gauss-Legendre
// quadrature estimates each piece whole and in halves, and halves again wherever the two disagree. A distance along a
// curve becomes a parameter by Newton's method on that integral, kept inside a bracket around the answer, and the
// curve is cut there by de Casteljau's construction.
#include "linea/Measure.hpp"

#include "linea/Flatten.hpp"

#include "Roots.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <numeric>
#include <utility>
#include <vector>

namespace linea {
namespace {

// The eight-point Gauss-Legendre rule on [-1, 1], exact for polynomials up to degree 15: the positive roots of the
// Legendre polynomial P8 and their weights, as Abramowitz and Stegun tabulate them in table 25.4 of the Handbook of
// Mathematical Functions, here to the 17 significant digits a double holds. The rule is symmetric: each node's
// negation is a node of the same weight.
constexpr std::array<double, 4> gaussNodes{0.18343464249564980, 0.52553240991632899, 0.79666647741362674,
                                           0.96028985649753623};
constexpr std::array<double, 4> gaussWeights{0.36268378337836198, 0.31370664587788729, 0.22238103445337447,
                                             0.10122853629037626};
// A curve's two estimates must agree within this fraction of the length of its control polygon. The finer estimate is
// the one kept, so the length is in practice much closer than this.
constexpr double lengthPrecision = 1e-12;
// Halving stops this deep, at pieces 2^-50 of the curve, whatever the estimates say. A smooth curve settles within a
// few halvings, and the sharp dip in speed where a curve nearly stops takes a few dozen.
constexpr int deepestHalving = 50;
// Inverting a curve's length stops after this many steps whatever its error. Newton's method takes a handful; at a
// cusp, where the speed is zero, it only halves the error each step, and this many halvings reach 2^-64 of the curve.
constexpr int mostInversionSteps = 64;

// A segment of a path as a Bezier curve of degree 1, 2 or 3: a line, or a quadratic or cubic curve. A derivative of
// one can also have degree 0, a constant.
struct Bezier {
    std::array<Vec2, 4> points{};
    std::size_t degree{};
};

// The point at @p t, from 0 to 1, by de Casteljau's algorithm.
Vec2 evaluate(Bezier curve, double t) {
    for (std::size_t n = curve.degree; n > 0; --n)
        for (std::size_t i = 0; i < n; ++i)
            curve.points[i] = lerp(curve.points[i], curve.points[i + 1], t);
    return curve.points[0];
}

// The derivative of @p curve, whose degree is at least 1: its hodograph, a Bezier curve of one degree less through the
// differences of the curve's points, each times the curve's degree.
Bezier derivative(const Bezier &curve) {
    Bezier result{{}, curve.degree - 1};
    const auto degree = static_cast<double>(curve.degree);
    for (std::size_t i = 0; i < curve.degree; ++i)
        result.points[i] = (curve.points[i + 1] - curve.points[i]) * degree;
    return result;
}

// One Gauss-Legendre estimate of a curve's length from @p from to @p to, the integral of the length of its derivative
// @p velocity.
double gauss(const Bezier &velocity, double from, double to) {
    const double middle = 0.5 * (from + to);
    const double half = 0.5 * (to - from);
    double sum = 0.0;
    for (std::size_t i = 0; i < gaussNodes.size(); ++i) {
        const double offset = half * gaussNodes[i];
        sum += gaussWeights[i] *
               (length(evaluate(velocity, middle - offset)) + length(evaluate(velocity, middle + offset)));
    }
    return sum * half;
}

// A curve's length from @p from to @p to, given its derivative @p velocity and @p whole, the estimate over that whole
// span: the sum of the estimates over its two halves once that sum agrees with @p whole within @p tolerance, and
// otherwise the sum of each half measured the same way, to half the tolerance. A difference that is not a number stops
// the halving too.
double adaptive(const Bezier &velocity, double from, double to, double whole, double tolerance, int depth) {
    const double middle = 0.5 * (from + to);
    const double left = gauss(velocity, from, middle);
    const double right = gauss(velocity, middle, to);
    if (depth == deepestHalving || !(std::abs(left + right - whole) > tolerance))
        return left + right;
    return adaptive(velocity, from, middle, left, tolerance / 2.0, depth + 1) +
           adaptive(velocity, middle, to, right, tolerance / 2.0, depth + 1);
}

// The value at @p t of the polynomial whose coefficients, constant term first, are @p c.
double polynomial(const std::array<double, 4> &c, double t) { return ((c[3] * t + c[2]) * t + c[1]) * t + c[0]; }

// The parameters strictly between 0 and 1, in order, where the speed of the curve whose derivative is @p velocity falls
// to a minimum: where velocity . velocity', half the derivative of the squared speed, crosses 0 from below. That
// product is a cubic, whose own turning points cut [0, 1] into spans where it only rises or only falls, so each span
// holds one crossing at most, which bisection pins to the last bit.
std::vector<double> slowestPoints(const Bezier &velocity) {
    // The velocity, of degree 1 or 2, as a t^2 + b t + c.
    const std::array<Vec2, 4> &h = velocity.points;
    const Vec2 a = velocity.degree == 2 ? h[0] - h[1] * 2.0 + h[2] : Vec2{};
    const Vec2 b = (h[1] - h[0]) * static_cast<double>(velocity.degree);
    const Vec2 c = h[0];
    // (a t^2 + b t + c) . (2 a t + b).
    const std::array<double, 4> product{dot(b, c), dot(b, b) + 2.0 * dot(a, c), 3.0 * dot(a, b), 2.0 * dot(a, a)};
    std::vector<double> spans{0.0};
    detail::forEachUnitRoot(3.0 * product[3], 2.0 * product[2], product[1], [&](double t) { spans.push_back(t); });
    std::sort(spans.begin(), spans.end());
    spans.push_back(1.0);
    std::vector<double> slowest;
    for (std::size_t i = 0; i + 1 < spans.size(); ++i) {
        double low = spans[i];
        double high = spans[i + 1];
        if (!(polynomial(product, low) < 0.0 && polynomial(product, high) > 0.0))
            continue;
        for (double middle = 0.5 * (low + high); middle > low && middle < high; middle = 0.5 * (low + high)) {
            if (polynomial(product, middle) < 0.0)
                low = middle;
            else
                high = middle;
        }
        slowest.push_back(high);
    }
    return slowest;
}

// What measuring a curve takes: its derivative, the parameters where its speed is least, and how closely two estimates
// of its whole length must agree, a fraction of the length of its control polygon.
struct Gauge {
    Bezier velocity;
    std::vector<double> slowest;
    double tolerance{};
};

Gauge gaugeFor(const Bezier &curve) {
    double polygon = 0.0;
    for (std::size_t i = 0; i < curve.degree; ++i)
        polygon += length(curve.points[i + 1] - curve.points[i]);
    Bezier velocity = derivative(curve);
    std::vector<double> slowest = slowestPoints(velocity);
    return {velocity, std::move(slowest), lengthPrecision * polygon};
}

// The length of the curve that @p gauge measures, from @p from to @p to, each piece of it to its share of the
// tolerance. The span is cut where the speed is least, since a cusp, where the speed falls to 0 with a corner, would
// fool the halving from inside a piece: the estimate over a span and the estimates over its halves can be wrong alike,
// and agree. At the end of a piece, the corner does no harm.
double lengthBetween(const Gauge &gauge, double from, double to) {
    const auto piece = [&](double a, double b) {
        return adaptive(gauge.velocity, a, b, gauss(gauge.velocity, a, b), gauge.tolerance * (b - a), 0);
    };
    double sum = 0.0;
    double start = from;
    for (const double cut : gauge.slowest) {
        if (cut > start && cut < to) {
            sum += piece(start, cut);
            start = cut;
        }
    }
    return sum + piece(start, to);
}

double segmentLength(const Bezier &curve) {
    if (curve.degree == 1)
        return length(curve.points[1] - curve.points[0]);
    return lengthBetween(gaugeFor(curve), 0.0, 1.0);
}

// The parameter at which @p curve has run @p distance along it, more than 0 and less than its length @p total.
double parameterAt(const Bezier &curve, double distance, double total) {
    double t = distance / total;
    if (curve.degree == 1)
        return t;
    const Gauge gauge = gaugeFor(curve);
    double low = 0.0;
    double high = 1.0;
    for (int step = 0; step < mostInversionSteps; ++step) {
        const double error = lengthBetween(gauge, 0.0, t) - distance;
        if (!(std::abs(error) > gauge.tolerance))
            break;
        if (error < 0.0)
            low = t;
        else
            high = t;
        // A step that would leave the bracket, as one where the curve stops for an instant would, bisects it instead.
        const double next = t - error / length(evaluate(gauge.velocity, t));
        t = next > low && next < high ? next : 0.5 * (low + high);
    }
    return t;
}

// The unit vector along which @p curve moves at @p t. Where its velocity vanishes, as at a cusp or at an end whose
// control point sits on it, the first derivative that does not gives the direction: near t the velocity is that
// derivative times (s - t)^(order - 1), up to a positive factor. A derivative of even order so points back the way the
// curve came, and is turned around to give the way it arrives, except at the start, which the curve only leaves.
Vec2 tangentAt(const Bezier &curve, double t) {
    Bezier derived = curve;
    for (std::size_t order = 1; order <= curve.degree; ++order) {
        derived = derivative(derived);
        const Vec2 direction = evaluate(derived, t);
        if (direction != Vec2{})
            return normalized(t > 0.0 && order % 2 == 0 ? -direction : direction);
    }
    return {};
}

// The segments of @p path in order, with the line that closes each closed subpath.
std::vector<Bezier> segments(const Path &path) {
    std::vector<Bezier> result;
    const std::vector<Vec2> &points = path.points();
    std::size_t at = 0;
    Vec2 start{};
    Vec2 current{};
    for (const Verb verb : path.verbs()) {
        const Vec2 *p = points.data() + at;
        switch (verb) {
        case Verb::move:
            start = p[0];
            current = p[0];
            break;
        case Verb::line:
            result.push_back({{current, p[0]}, 1});
            current = p[0];
            break;
        case Verb::quad:
            result.push_back({{current, p[0], p[1]}, 2});
            current = p[1];
            break;
        case Verb::cubic:
            result.push_back({{current, p[0], p[1], p[2]}, 3});
            current = p[2];
            break;
        case Verb::close:
            result.push_back({{current, start}, 1});
            current = start;
            break;
        }
        at += static_cast<std::size_t>(pointCount(verb));
    }
    return result;
}

// The lengths of @p curves, in order.
std::vector<double> lengthsOf(const std::vector<Bezier> &curves) {
    std::vector<double> lengths;
    lengths.reserve(curves.size());
    for (const Bezier &curve : curves)
        lengths.push_back(segmentLength(curve));
    return lengths;
}

// A distance along a path, found: the segment that holds it, the first of any length to reach it, and the parameter
// there, exactly 0 or 1 at the segment's ends.
struct Place {
    std::size_t segment{};
    double t{};
};

// Where @p distance, at least 0, falls along the path whose segments are @p curves, of @p lengths, or nothing past its
// end. The running sum is arcLength()'s, term for term, so a distance of exactly the path's length lands on its end.
std::optional<Place> locate(const std::vector<Bezier> &curves, const std::vector<double> &lengths, double distance) {
    double start = 0.0;
    for (std::size_t i = 0; i < curves.size(); ++i) {
        const double end = start + lengths[i];
        if (lengths[i] > 0.0 && distance <= end) {
            if (distance <= start)
                return Place{i, 0.0};
            if (distance == end)
                return Place{i, 1.0};
            return Place{i, parameterAt(curves[i], distance - start, lengths[i])};
        }
        start = end;
    }
    return std::nullopt;
}

// @p curve split at @p t, more than 0 and less than 1, into two curves of its degree that meet at the point at t.
std::array<Bezier, 2> halves(const Bezier &curve, double t) {
    const std::array<Vec2, 4> &p = curve.points;
    switch (curve.degree) {
    case 2: {
        const std::array<Vec2, 5> q = splitQuad(p[0], p[1], p[2], t);
        return {Bezier{{q[0], q[1], q[2]}, 2}, Bezier{{q[2], q[3], q[4]}, 2}};
    }
    case 3: {
        const std::array<Vec2, 7> q = splitCubic(p[0], p[1], p[2], p[3], t);
        return {Bezier{{q[0], q[1], q[2], q[3]}, 3}, Bezier{{q[3], q[4], q[5], q[6]}, 3}};
    }
    default: {
        const Vec2 cut = lerp(p[0], p[1], t);
        return {Bezier{{p[0], cut}, 1}, Bezier{{cut, p[1]}, 1}};
    }
    }
}

// The point at @p t on @p curve: exactly its ends at 0 and 1, and between them the point where splitting cuts it, so
// that pointAt() and splitAt() agree to the last bit.
Vec2 pointOn(const Bezier &curve, double t) {
    if (t == 0.0)
        return curve.points[0];
    if (t == 1.0)
        return curve.points[curve.degree];
    return halves(curve, t)[1].points[0];
}

// Draws @p curve, which starts at @p path's current point, onto @p path.
void extend(Path &path, const Bezier &curve) {
    const std::array<Vec2, 4> &p = curve.points;
    if (curve.degree == 3)
        path.cubicTo(p[1], p[2], p[3]);
    else if (curve.degree == 2)
        path.quadTo(p[1], p[2]);
    else
        path.lineTo(p[1]);
}

// Draws @p verb, followed in its path by the points from @p p, onto @p path.
void replay(Path &path, Verb verb, const Vec2 *p) {
    switch (verb) {
    case Verb::move:
        path.moveTo(p[0]);
        break;
    case Verb::line:
        path.lineTo(p[0]);
        break;
    case Verb::quad:
        path.quadTo(p[0], p[1]);
        break;
    case Verb::cubic:
        path.cubicTo(p[0], p[1], p[2]);
        break;
    case Verb::close:
        path.close();
        break;
    }
}

} // namespace

double arcLength(const Path &path) {
    const std::vector<double> lengths = lengthsOf(segments(path));
    return std::accumulate(lengths.begin(), lengths.end(), 0.0);
}

std::optional<PathPoint> pointAt(const Path &path, double distance) {
    if (!(distance >= 0.0))
        return std::nullopt;
    const std::vector<Bezier> curves = segments(path);
    const std::optional<Place> place = locate(curves, lengthsOf(curves), distance);
    if (!place)
        return std::nullopt;
    const Bezier &curve = curves[place->segment];
    return PathPoint{pointOn(curve, place->t), tangentAt(curve, place->t)};
}

PathParts splitAt(const Path &path, double distance) {
    if (!(distance > 0.0))
        return {Path{}, path};
    const std::vector<Bezier> curves = segments(path);
    const std::vector<double> lengths = lengthsOf(curves);
    if (!(distance < std::accumulate(lengths.begin(), lengths.end(), 0.0)))
        return {path, Path{}};
    // A distance short of the path's length always lands on a segment.
    const Place place = *locate(curves, lengths, distance);

    const std::vector<Verb> &verbs = path.verbs();
    const std::vector<Vec2> &points = path.points();
    PathParts parts;
    // Everything ahead of the segment that holds the cut goes into the part before as it is. Every verb but a move
    // draws a segment, so the walk counts segments as it goes, and at counts the points that follow each verb.
    std::size_t verb = 0;
    std::size_t at = 0;
    Vec2 start{};
    for (std::size_t segment = 0;; ++verb) {
        if (verbs[verb] == Verb::move) {
            start = points[at];
        } else {
            if (segment == place.segment)
                break;
            ++segment;
        }
        replay(parts.before, verbs[verb], points.data() + at);
        at += static_cast<std::size_t>(pointCount(verbs[verb]));
    }
    // The cut subpath runs to just before the next move, and goes on past the cut if any of it beyond has length.
    std::size_t next = verb + 1;
    while (next < verbs.size() && verbs[next] != Verb::move)
        ++next;
    bool goesOn = place.t < 1.0;
    for (std::size_t segment = place.segment + 1; !goesOn && segment < place.segment + (next - verb); ++segment)
        goesOn = lengths[segment] > 0.0;

    if (!goesOn) {
        // The cut is where the subpath's length ends: all of it stays before, and the part after starts with the next.
        for (; verb < next; ++verb) {
            replay(parts.before, verbs[verb], points.data() + at);
            at += static_cast<std::size_t>(pointCount(verbs[verb]));
        }
    } else {
        const Bezier &curve = curves[place.segment];
        if (place.t < 1.0) {
            const std::array<Bezier, 2> cut = halves(curve, place.t);
            extend(parts.before, cut[0]);
            parts.after.moveTo(cut[1].points[0]);
            extend(parts.after, cut[1]);
        } else {
            extend(parts.before, curve);
            parts.after.moveTo(curve.points[curve.degree]);
        }
        at += static_cast<std::size_t>(pointCount(verbs[verb]));
        // The rest of the subpath now starts from the cut, so a close becomes a line back to the subpath's start.
        for (++verb; verb < next; ++verb) {
            if (verbs[verb] == Verb::close)
                parts.after.lineTo(start);
            else
                replay(parts.after, verbs[verb], points.data() + at);
            at += static_cast<std::size_t>(pointCount(verbs[verb]));
        }
    }
    // The subpaths beyond the cut one go into the part after as they are.
    for (; verb < verbs.size(); ++verb) {
        replay(parts.after, verbs[verb], points.data() + at);
        at += static_cast<std::size_t>(pointCount(verbs[verb]));
    }
    return parts;
}

} // namespace linea
