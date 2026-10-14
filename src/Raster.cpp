// linea - Raster implementation: an exact-area coverage accumulator, after font-rs and stb_truetype's second
// rasterizer. Each edge adds the signed area it sweeps to the cells it crosses, and a running sum along each row turns
// those areas into the winding-weighted coverage of every pixel.
#include "linea/Raster.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <utility>

namespace linea {
namespace {

class Accumulator {
  public:
    Accumulator(int width, int height)
        : width_(width), height_(height), stride_(static_cast<std::size_t>(width) + padding),
          cells_(stride_ * static_cast<std::size_t>(height), 0.0) {}

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

    // The coverage of every pixel under @p rule.
    std::vector<float> coverage(FillRule rule) const {
        std::vector<float> result(static_cast<std::size_t>(width_) * static_cast<std::size_t>(height_));
        for (int y = 0; y < height_; ++y) {
            const double *row = cells_.data() + static_cast<std::size_t>(y) * stride_;
            double winding = 0.0;
            for (int x = 0; x < width_; ++x) {
                winding += row[x];
                const double magnitude = std::abs(winding);
                double cover = 0.0;
                if (rule == FillRule::nonZero) {
                    cover = std::min(magnitude, 1.0);
                } else {
                    const double folded = std::fmod(magnitude, 2.0);
                    cover = folded > 1.0 ? 2.0 - folded : folded;
                }
                result[static_cast<std::size_t>(y) * static_cast<std::size_t>(width_) + static_cast<std::size_t>(x)] =
                    static_cast<float>(cover);
            }
        }
        return result;
    }

  private:
    // Cells beyond the last column: an edge on the right border writes up to two cells past it.
    static constexpr std::size_t padding = 2;

    // The edge from @p p0 to @p p1 with x clamped to the image, which moves it onto the side it lies beyond when it
    // lies wholly outside. Rows carry padding cells for edges on the right side, so nothing spills into the next row.
    void clamped(Vec2 p0, Vec2 p1) {
        const double right = width_;
        p0.x = std::clamp(p0.x, 0.0, right);
        p1.x = std::clamp(p1.x, 0.0, right);
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
            const double next = std::clamp(x + dxdy * dy, 0.0, right);
            x = std::clamp(x, 0.0, right);
            sweep(y, x, next, dy * direction);
            x = next;
        }
    }

    // The part of an edge within row @p y, from x @p from to x @p to, both inside the image, sweeping @p dy signed.
    void sweep(int y, double from, double to, double dy) {
        double *row = cells_.data() + static_cast<std::size_t>(y) * stride_;
        const double x0 = std::min(from, to);
        const double x1 = std::max(from, to);
        const double x0floor = std::floor(x0);
        const auto x0i = static_cast<std::size_t>(x0floor);
        const double x1ceil = std::ceil(x1);
        const auto x1i = static_cast<std::size_t>(x1ceil);
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
    std::size_t stride_;
    std::vector<double> cells_;
};

bool finite(Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }

} // namespace

std::vector<float> coverage(const std::vector<Contour> &contours, int width, int height, FillRule rule) {
    Accumulator accumulator(std::max(1, width), std::max(1, height));
    for (const Contour &contour : contours) {
        const std::vector<Vec2> &p = contour.points;
        if (p.size() < 3 || !std::all_of(p.begin(), p.end(), finite))
            continue;
        for (std::size_t i = 0; i < p.size(); ++i)
            accumulator.edge(p[i], p[(i + 1) % p.size()]);
    }
    return accumulator.coverage(rule);
}

} // namespace linea
