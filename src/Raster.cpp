// linea - Raster implementation: an exact-area coverage accumulator, after font-rs and stb_truetype's second
// rasterizer. Each edge adds the signed area it sweeps to the cells it crosses, and a running sum along each row turns
// those areas into the winding-weighted coverage of every pixel. Only the box of pixels around the contours takes
// cells, so a shape costs time and memory in proportion to its box rather than to the image.
#include "linea/Raster.hpp"

#include "Coverage.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <utility>

namespace linea {
namespace {

using detail::PixelBox;

class Accumulator {
  public:
    // Cells for the rows of @p box, which lies in an image @p width by @p height, and for its columns from one before
    // its first to one after its last, and padding.
    Accumulator(int width, int height, PixelBox box)
        : width_(width), height_(height), box_(box), left_(std::max(0, box.x0 - 1)),
          right_(std::min(width, box.x1 + 1)), stride_(static_cast<std::size_t>(right_ - left_) + padding),
          cells_(stride_ * static_cast<std::size_t>(box.height()), 0.0) {}

    void edge(Vec2 p0, Vec2 p1) {
        // An edge, or the part of one, left of the image covers every pixel of its rows from the image's left side, as
        // the running sum carries it rightward, and one right of the image covers nothing that shows, so moving each
        // part outside onto the side it lies beyond keeps every visible pixel's coverage exact. An edge that crosses a
        // side is cut there first, so that the part inside keeps its slope.
        Vec2 from = p0;
        if (p0.x != p1.x) {
            std::array<double, 2> cuts{-p0.x / (p1.x - p0.x), (width_ - p0.x) / (p1.x - p0.x)};
            std::ranges::sort(cuts);
            for (const double t : cuts) {
                if (t > 0.0 && t < 1.0) {
                    const Vec2 to = lerp(p0, p1, t);
                    clamped(from, to);
                    from = to;
                }
            }
        }
        clamped(from, p1);
    }

    // The coverage of every pixel of the box under @p rule.
    std::vector<float> coverage(FillRule rule) const {
        std::vector<float> result(static_cast<std::size_t>(box_.width()) * static_cast<std::size_t>(box_.height()));
        for (int y = box_.y0; y < box_.y1; ++y) {
            const double *row = cells_.data() + static_cast<std::size_t>(y - box_.y0) * stride_;
            double winding = 0.0;
            for (int x = left_; x < box_.x1; ++x) {
                winding += row[x - left_];
                if (x < box_.x0)
                    continue;
                const double magnitude = std::abs(winding);
                double cover = 0.0;
                if (rule == FillRule::nonZero) {
                    cover = std::min(magnitude, 1.0);
                } else {
                    const double folded = std::fmod(magnitude, 2.0);
                    cover = folded > 1.0 ? 2.0 - folded : folded;
                }
                result[box_.index(x, y)] = static_cast<float>(cover);
            }
        }
        return result;
    }

  private:
    // Cells beyond the last column: an edge on the right border writes up to two cells past it.
    static constexpr std::size_t padding = 2;

    // The edge from @p p0 to @p p1 with x clamped to the image, which moves it onto the side it lies beyond when it
    // lies wholly outside. Rows carry padding cells for edges on the right side, so nothing spills into the next row.
    //
    // Only the box's rows take cells, and x clamps to the box's columns and one more on each side, so that every edge
    // stays inside the cells whatever its coordinates. Stepping an edge from row to row can carry it a hair past its
    // contour's box. The column before the box takes what lands there, so that the running sum adds exactly the terms
    // it would over the whole image. What lands in that column, or would land in rows or columns beyond the box, is
    // rounding error where the shape covers nothing, and those pixels, outside the box, have coverage 0.
    void clamped(Vec2 p0, Vec2 p1) {
        const auto left = static_cast<double>(left_);
        const auto right = static_cast<double>(right_);
        p0.x = std::clamp(p0.x, left, right);
        p1.x = std::clamp(p1.x, left, right);
        if (p0.y == p1.y)
            return;
        double direction = 1.0;
        if (p0.y > p1.y) {
            std::swap(p0, p1);
            direction = -1.0;
        }
        if (p1.y <= 0.0 || p0.y >= height_)
            return;
        const double dxdy = (p1.x - p0.x) / (p1.y - p0.y);
        double x = p0.x;
        if (p0.y < 0.0)
            x -= p0.y * dxdy;
        const int firstRow = static_cast<int>(std::floor(std::max(p0.y, 0.0)));
        const int endRow = std::min(height_, static_cast<int>(std::ceil(p1.y)));
        for (int y = firstRow; y < endRow; ++y) {
            const double dy = std::min(static_cast<double>(y + 1), p1.y) - std::max(static_cast<double>(y), p0.y);
            const double next = std::clamp(x + dxdy * dy, left, right);
            x = std::clamp(x, left, right);
            if (y >= box_.y0 && y < box_.y1)
                sweep(y, x, next, dy * direction);
            x = next;
        }
    }

    // The part of an edge within row @p y, from x @p from to x @p to, both inside the cells, sweeping @p dy signed.
    void sweep(int y, double from, double to, double dy) {
        double *row = cells_.data() + static_cast<std::size_t>(y - box_.y0) * stride_;
        const double x0 = std::min(from, to);
        const double x1 = std::max(from, to);
        const double x0floor = std::floor(x0);
        const auto x0i = static_cast<std::size_t>(static_cast<int>(x0floor) - left_);
        const double x1ceil = std::ceil(x1);
        const auto x1i = static_cast<std::size_t>(static_cast<int>(x1ceil) - left_);
        if (x1i <= x0i + 1) {
            // Within one cell: the area right of the edge's midpoint in that cell, and the rest beyond it.
            const double right = 0.5 * (from + to) - x0floor;
            row[x0i] += dy - dy * right;
            row[x0i + 1] += dy * right;
            return;
        }
        // Across several cells: a triangle in the first, a trapezoid band through the middle, a triangle in the last.
        const double slope = 1.0 / (x1 - x0);
        const double x0f = x0 - x0floor;
        const double first = 0.5 * slope * (1.0 - x0f) * (1.0 - x0f);
        const double x1f = x1 - x1ceil + 1.0;
        const double last = 0.5 * slope * x1f * x1f;
        row[x0i] += dy * first;
        if (x1i == x0i + 2) {
            row[x0i + 1] += dy * (1.0 - first - last);
        } else {
            const double second = slope * (1.5 - x0f);
            row[x0i + 1] += dy * (second - first);
            for (std::size_t xi = x0i + 2; xi + 1 < x1i; ++xi)
                row[xi] += dy * slope;
            const double before = second + static_cast<double>(x1i - x0i - 3) * slope;
            row[x1i - 1] += dy * (1.0 - before - last);
        }
        row[x1i] += dy * last;
    }

    int width_;
    int height_;
    PixelBox box_;
    // The columns that x clamps to: the first takes the first cell.
    int left_;
    int right_;
    std::size_t stride_;
    std::vector<double> cells_;
};

bool finite(Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }

// Whether coverage() fills @p contour: one of fewer than three points encloses nothing, and one with a point that is
// not finite has no place on the image.
bool filled(const Contour &contour) {
    return contour.points.size() >= 3 && std::all_of(contour.points.begin(), contour.points.end(), finite);
}

} // namespace

namespace detail {

PixelBox intersection(PixelBox a, PixelBox b) {
    return {std::max(a.x0, b.x0), std::max(a.y0, b.y0), std::min(a.x1, b.x1), std::min(a.y1, b.y1)};
}

PixelBox hull(PixelBox a, PixelBox b) {
    if (a.empty())
        return b;
    if (b.empty())
        return a;
    return {std::min(a.x0, b.x0), std::min(a.y0, b.y0), std::max(a.x1, b.x1), std::max(a.y1, b.y1)};
}

PixelBox pixelBounds(const std::vector<Contour> &contours, int width, int height) {
    double minX = std::numeric_limits<double>::infinity();
    double minY = minX;
    double maxX = -minX;
    double maxY = -minX;
    for (const Contour &contour : contours) {
        if (!filled(contour))
            continue;
        for (const Vec2 p : contour.points) {
            minX = std::min(minX, p.x);
            minY = std::min(minY, p.y);
            maxX = std::max(maxX, p.x);
            maxY = std::max(maxY, p.y);
        }
    }
    if (!(minX <= maxX))
        return {};
    // The box clamps to the image like the edges: a contour left of it covers its rows from its left side.
    const auto columns = static_cast<double>(width);
    const auto rows = static_cast<double>(height);
    return {static_cast<int>(std::floor(std::clamp(minX, 0.0, columns))),
            static_cast<int>(std::floor(std::clamp(minY, 0.0, rows))),
            static_cast<int>(std::ceil(std::clamp(maxX, 0.0, columns))),
            static_cast<int>(std::ceil(std::clamp(maxY, 0.0, rows)))};
}

BoxCoverage boxCoverage(const std::vector<Contour> &contours, int width, int height, FillRule rule) {
    const PixelBox box = pixelBounds(contours, width, height);
    if (box.empty())
        return {box, {}};
    Accumulator accumulator(width, height, box);
    for (const Contour &contour : contours) {
        if (!filled(contour))
            continue;
        const std::vector<Vec2> &p = contour.points;
        for (std::size_t i = 0; i < p.size(); ++i)
            accumulator.edge(p[i], p[(i + 1) % p.size()]);
    }
    return {box, accumulator.coverage(rule)};
}

} // namespace detail

std::vector<float> coverage(const std::vector<Contour> &contours, int width, int height, FillRule rule) {
    const PixelBox image{0, 0, std::max(1, width), std::max(1, height)};
    const detail::BoxCoverage part = detail::boxCoverage(contours, image.x1, image.y1, rule);
    std::vector<float> result(static_cast<std::size_t>(image.width()) * static_cast<std::size_t>(image.height()));
    for (int y = part.box.y0; y < part.box.y1; ++y) {
        const auto row = part.values.begin() + static_cast<std::ptrdiff_t>(part.box.index(part.box.x0, y));
        const auto to = result.begin() + static_cast<std::ptrdiff_t>(image.index(part.box.x0, y));
        std::copy(row, row + part.box.width(), to);
    }
    return result;
}

} // namespace linea
