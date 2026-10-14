// linea - Shapes implementation.
#include "linea/Shapes.hpp"

#include <algorithm>
#include <cstddef>
#include <numbers>

namespace linea {
namespace {

// An ellipse traced from the end of its rotated x axis, clockwise or not, as two half-turn arcs.
Path ellipseWinding(Vec2 center, Vec2 radii, double rotation, bool clockwise) {
    const Vec2 axis = applyLinear(rotate(rotation), {radii.x, 0.0});
    Path path;
    path.moveTo(center + axis)
        .arcTo(radii, rotation, false, clockwise, center - axis)
        .arcTo(radii, rotation, false, clockwise, center + axis)
        .close();
    return path;
}

} // namespace

Path circle(Vec2 center, double radius) { return ellipse(center, {radius, radius}); }

Path ellipse(Vec2 center, Vec2 radii, double rotation) { return ellipseWinding(center, radii, rotation, true); }

Path rectangle(Vec2 min, Vec2 max) {
    Path path;
    path.moveTo(min).lineTo({max.x, min.y}).lineTo(max).lineTo({min.x, max.y}).close();
    return path;
}

Path roundedRectangle(Vec2 center, Vec2 halfSize, double radius, double rotation) {
    const double hx = halfSize.x;
    const double hy = halfSize.y;
    const double r = std::clamp(radius, 0.0, std::min(hx, hy));
    const Vec2 corner{r, r};
    Path local;
    local.moveTo({-hx + r, -hy})
        .lineTo({hx - r, -hy})
        .arcTo(corner, 0.0, false, true, {hx, -hy + r})
        .lineTo({hx, hy - r})
        .arcTo(corner, 0.0, false, true, {hx - r, hy})
        .lineTo({-hx + r, hy})
        .arcTo(corner, 0.0, false, true, {-hx, hy - r})
        .lineTo({-hx, -hy + r})
        .arcTo(corner, 0.0, false, true, {-hx + r, -hy})
        .close();
    return transformed(local, translate(center) * rotate(rotation));
}

Path polygon(std::span<const Vec2> points) {
    Path path;
    if (points.size() < 3)
        return path;
    path.moveTo(points.front());
    for (const Vec2 p : points.subspan(1))
        path.lineTo(p);
    path.close();
    return path;
}

Path star(Vec2 center, double outer, double inner, int tips, double rotation) {
    Path path;
    if (tips < 2)
        return path;
    const int corners = tips * 2;
    for (int corner = 0; corner < corners; ++corner) {
        const double angle = rotation + 2.0 * std::numbers::pi * corner / corners;
        const Vec2 p = polar(center, corner % 2 == 0 ? outer : inner, angle);
        if (corner == 0)
            path.moveTo(p);
        else
            path.lineTo(p);
    }
    path.close();
    return path;
}

Path ring(Vec2 center, double outer, double inner) {
    Path path = ellipseWinding(center, {outer, outer}, 0.0, true);
    path.append(ellipseWinding(center, {inner, inner}, 0.0, false));
    return path;
}

} // namespace linea
