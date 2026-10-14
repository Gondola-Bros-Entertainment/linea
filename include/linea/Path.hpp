// linea - paths: subpaths of lines, quadratic and cubic Bezier curves and elliptical arcs.
#pragma once

#include "linea/Geometry.hpp"

#include <cstdint>
#include <vector>

namespace linea {

enum class Verb : std::uint8_t {
    // Starts a subpath at one point.
    move,
    // A straight line to one point.
    line,
    // A quadratic Bezier curve through one control point to an end point.
    quad,
    // A cubic Bezier curve through two control points to an end point.
    cubic,
    // A straight line back to the subpath's start, which closes it. Takes no points.
    close,
};

// The number of points that follow @p verb in a path's point list.
constexpr int pointCount(Verb verb) {
    switch (verb) {
    case Verb::move:
    case Verb::line:
        return 1;
    case Verb::quad:
        return 2;
    case Verb::cubic:
        return 3;
    case Verb::close:
        return 0;
    }
    return 0;
}

// A sequence of subpaths, as SVG paths and PostScript describe them. Each drawing call adds to the current subpath;
// one made before any move starts a subpath at the origin. Arcs become cubic curves as they are added.
class Path {
  public:
    Path &moveTo(Vec2 to);
    Path &lineTo(Vec2 to);
    Path &quadTo(Vec2 control, Vec2 to);
    Path &cubicTo(Vec2 control1, Vec2 control2, Vec2 to);
    // An elliptical arc to @p to, as SVG's A command draws one: an ellipse with @p radii, rotated by @p rotation
    // radians, and of the two arcs and two ellipses that pass through both ends, the larger arc when @p largeArc and
    // the one that turns clockwise on a y-down canvas when @p clockwise. Radii too small to reach scale up until they
    // do, and a zero radius draws a line, as SVG specifies.
    Path &arcTo(Vec2 radii, double rotation, bool largeArc, bool clockwise, Vec2 to);
    Path &close();

    // Every subpath of @p other, after this one's.
    Path &append(const Path &other);

    [[nodiscard]] const std::vector<Verb> &verbs() const { return verbs_; }
    [[nodiscard]] const std::vector<Vec2> &points() const { return points_; }
    [[nodiscard]] bool empty() const { return verbs_.empty(); }
    // The pen position after the last command: where the next one starts.
    [[nodiscard]] Vec2 current() const { return current_; }

    friend bool operator==(const Path &, const Path &) = default;

  private:
    void ensureStarted();

    std::vector<Verb> verbs_;
    std::vector<Vec2> points_;
    Vec2 start_{};
    Vec2 current_{};
    bool open_ = false;
};

// @p path with every point moved by @p m.
Path transformed(const Path &path, const Affine &m);

// The smallest axis-aligned box around every point of @p path, control points included, which therefore contains
// the whole path.
struct Bounds {
    Vec2 min{};
    Vec2 max{};
};
Bounds controlBounds(const Path &path);

} // namespace linea
