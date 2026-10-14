// linea - Noise implementation. Lattice points hash with nested calls of the PCG hash, which Jarzynski and Olano
// recommend in "Hash Functions for GPU Rendering" (2020), and everything else is arithmetic that IEEE 754 defines.
#include "linea/Noise.hpp"

#include "linea/Flatten.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numbers>
#include <span>
#include <vector>

namespace linea {
namespace {

// A step of a 32-bit linear congruential generator, then PCG's RXS M XS output permutation, each step invertible, so
// that the whole is a bijection on 32 bits. Unsigned arithmetic wraps the same way on every platform.
constexpr std::uint32_t pcg(std::uint32_t v) {
    const std::uint32_t state = v * 747796405u + 2891336453u;
    const std::uint32_t word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// The hash of lattice point (@p x, @p y) under a seed already hashed to @p seedHash.
constexpr std::uint32_t hash(std::uint32_t seedHash, std::uint32_t x, std::uint32_t y) {
    return pcg(x + pcg(y + seedHash));
}

// How many values a 32-bit hash takes, 2^32.
constexpr double hashRange = 4294967296.0;

// @p h mapped into [0, 1), exactly.
double unit(std::uint32_t h) { return static_cast<double>(h) / hashRange; }

// The lattice index of @p floored, a whole number, wrapped as the hash's 32-bit arithmetic wraps, so that every finite
// coordinate has one. Dividing by a power of two, flooring and the subtraction are all exact here.
std::uint32_t latticeIndex(double floored) {
    return static_cast<std::uint32_t>(floored - std::floor(floored / hashRange) * hashRange);
}

bool finite(Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }

// Eight gradients an eighth of a turn apart. Perlin's improved noise draws its gradients from a small set of equal
// length rather than at random, which keeps them from clumping. Their length, sqrt 2, makes the noise peak at exactly
// 1: at a cell's center, where every corner's gradient points at it.
constexpr double root2 = std::numbers::sqrt2;
constexpr std::array<Vec2, 8> gradients{
    {{root2, 0.0}, {1.0, 1.0}, {0.0, root2}, {-1.0, 1.0}, {-root2, 0.0}, {-1.0, -1.0}, {0.0, -root2}, {1.0, -1.0}}};

// The gradient that a lattice hash picks, by its top three bits.
constexpr Vec2 gradient(std::uint32_t h) { return gradients[h >> 29u]; }

// Perlin's quintic fade, 6t^5 - 15t^4 + 10t^3: its first and second derivatives are 0 at both ends, so the noise is
// smooth across lattice cells.
constexpr double fade(double t) { return t * t * t * (t * (t * 6.0 - 15.0) + 10.0); }

constexpr double mix(double a, double b, double t) { return a + (b - a) * t; }

// Gustavson's factors that skew the plane onto a lattice of squares split along their diagonals into triangles, and
// unskew it back: (sqrt 3 - 1) / 2 and (3 - sqrt 3) / 6.
constexpr double skew = (std::numbers::sqrt3 - 1.0) / 2.0;
constexpr double unskew = (3.0 - std::numbers::sqrt3) / 6.0;
// The squared distance at which a corner's kernel reaches 0: the squared height of the triangles, so that no kernel
// reaches beyond the triangles around its corner.
constexpr double kernelReach = 0.5;
// The reciprocal of simplex noise's peak, 2 (1/3)^4 / sqrt 3, which it reaches midway along a triangle's diagonal edge
// when the gradients at both ends point at it, so that it spans -1 to 1. Gustavson rounds this to 70.
constexpr double simplexScale = 81.0 * std::numbers::sqrt3 / 2.0;

// Feature points this many cells either side of a point's own cell can be among its two nearest, and no farther ones
// can: two of the four cells around its nearest lattice corner hold feature points within sqrt 3.25 of it, and every
// feature point three or more cells away lies at least 2 away.
constexpr int searchReach = 2;

// Simplex noise bends by at most this much along any line, per unit squared: the largest sum of its three corners'
// second derivatives, over every point, every direction and every gradient, is 55.78.
constexpr double simplexBend = 56.0;
// A segment that the bend asks to split into more pieces than this takes this many, so that coordinates far beyond any
// canvas cannot exhaust memory.
constexpr int mostPieces = 4096;
// A point where two segments meet moves at most this many times as far as the noise asks, as SVG's default miter limit
// caps a stroke's miter joins; a sharper corner would send it toward infinity.
constexpr double mostMiter = 4.0;

// The pieces to split a segment @p span long into, so that a displacement bending by at most @p bend per unit squared
// strays at most @p tolerance from a straight line between them: n pieces stray at most bend (span / n)^2 / 8.
int piecesFor(double span, double bend, double tolerance) {
    if (!(tolerance > 0.0))
        return 1;
    const double wanted = span * std::sqrt(bend / (8.0 * tolerance));
    if (!(wanted > 1.0))
        return 1;
    return static_cast<int>(std::min(std::ceil(wanted), static_cast<double>(mostPieces)));
}

// Where the point at which segments with unit normals @p before and @p after meet moves for each unit the noise asks:
// to where the segments' lines meet once each has moved a unit along its normal, as a miter joins them, or mostMiter
// units along the bisector for a sharper corner. A side with no segment has a zero normal, so an open contour's end
// moves along its one segment's normal.
Vec2 miter(Vec2 before, Vec2 after) {
    const Vec2 sum = before + after;
    // One plus the cosine of the turn between the segments, which is twice the squared cosine of half the turn, so
    // that the miter's length is sqrt(2 / meet).
    const double meet = 1.0 + dot(before, after);
    if (meet * mostMiter * mostMiter >= 2.0)
        return sum / meet;
    return normalized(sum) * mostMiter;
}

} // namespace

double perlin(Vec2 p, std::uint32_t seed) {
    if (!finite(p))
        return 0.0;
    const Vec2 cell{std::floor(p.x), std::floor(p.y)};
    const std::uint32_t x = latticeIndex(cell.x);
    const std::uint32_t y = latticeIndex(cell.y);
    const Vec2 f = p - cell;
    const std::uint32_t seedHash = pcg(seed);
    // A corner's gradient dotted with the offset from that corner to p.
    const auto corner = [&](std::uint32_t dx, std::uint32_t dy) {
        return dot(gradient(hash(seedHash, x + dx, y + dy)),
                   f - Vec2{static_cast<double>(dx), static_cast<double>(dy)});
    };
    const double u = fade(f.x);
    return mix(mix(corner(0, 0), corner(1, 0), u), mix(corner(0, 1), corner(1, 1), u), fade(f.y));
}

double simplex(Vec2 p, std::uint32_t seed) {
    if (!finite(p))
        return 0.0;
    // The skewed square that holds p, and p's offset from its first corner, unskewed.
    const double s = (p.x + p.y) * skew;
    const Vec2 cell{std::floor(p.x + s), std::floor(p.y + s)};
    const double t = (cell.x + cell.y) * unskew;
    const Vec2 d0 = p - (cell - Vec2{t, t});
    // The square's lower triangle steps along x to its middle corner, and its upper triangle along y.
    const std::uint32_t mx = d0.x > d0.y ? 1u : 0u;
    const std::uint32_t my = 1u - mx;
    const Vec2 d1 = d0 - Vec2{static_cast<double>(mx), static_cast<double>(my)} + Vec2{unskew, unskew};
    const Vec2 d2 = d0 - Vec2{1.0, 1.0} + Vec2{2.0 * unskew, 2.0 * unskew};
    const std::uint32_t x = latticeIndex(cell.x);
    const std::uint32_t y = latticeIndex(cell.y);
    const std::uint32_t seedHash = pcg(seed);
    // A corner's kernel, (r^2 - |d|^2)^4, scaling its gradient dotted with the offset @p d from it to p.
    const auto kernel = [&](std::uint32_t dx, std::uint32_t dy, Vec2 d) {
        const double w = kernelReach - dot(d, d);
        if (w <= 0.0)
            return 0.0;
        return w * w * w * w * dot(gradient(hash(seedHash, x + dx, y + dy)), d);
    };
    return simplexScale * (kernel(0, 0, d0) + kernel(mx, my, d1) + kernel(1, 1, d2));
}

double fbm(Noise noise, Vec2 p, std::uint32_t seed, const Fractal &fractal) {
    double sum = 0.0;
    double amplitudes = 0.0;
    double frequency = 1.0;
    double amplitude = 1.0;
    for (int octave = 0; octave < fractal.octaves; ++octave) {
        sum += amplitude * noise(p * frequency, seed + static_cast<std::uint32_t>(octave));
        amplitudes += std::abs(amplitude);
        frequency *= fractal.lacunarity;
        amplitude *= fractal.gain;
    }
    return amplitudes > 0.0 ? sum / amplitudes : 0.0;
}

Voronoi voronoi(Vec2 p, std::uint32_t seed) {
    constexpr double infinity = std::numeric_limits<double>::infinity();
    if (!finite(p))
        return {infinity, infinity, 0};
    const Vec2 cell{std::floor(p.x), std::floor(p.y)};
    const std::uint32_t x = latticeIndex(cell.x);
    const std::uint32_t y = latticeIndex(cell.y);
    const Vec2 f = p - cell;
    const std::uint32_t seedHash = pcg(seed);
    double nearest = infinity;
    double secondNearest = infinity;
    std::uint32_t nearestHash = 0;
    for (int dy = -searchReach; dy <= searchReach; ++dy) {
        for (int dx = -searchReach; dx <= searchReach; ++dx) {
            // The cell's feature point: the cell's hash places it across, and that hash hashed again places it down.
            const std::uint32_t across =
                hash(seedHash, x + static_cast<std::uint32_t>(dx), y + static_cast<std::uint32_t>(dy));
            const std::uint32_t down = pcg(across);
            const Vec2 offset = Vec2{static_cast<double>(dx) + unit(across), static_cast<double>(dy) + unit(down)} - f;
            const double distance2 = dot(offset, offset);
            if (distance2 < nearest) {
                secondNearest = nearest;
                nearest = distance2;
                nearestHash = down;
            } else if (distance2 < secondNearest) {
                secondNearest = distance2;
            }
        }
    }
    return {std::sqrt(nearest), std::sqrt(secondNearest), pcg(nearestHash)};
}

Path wobbled(const Path &path, const Wobble &wobble, double tolerance) {
    const double bend = std::abs(wobble.amplitude) * wobble.frequency * wobble.frequency * simplexBend;
    const std::uint32_t shiftHash = pcg(wobble.seed);
    const Vec2 shift{unit(shiftHash), unit(pcg(shiftHash))};
    // How far the noise moves the point at @p at, in units of the direction it moves along.
    const auto offset = [&](Vec2 at) { return wobble.amplitude * simplex(at * wobble.frequency + shift, wobble.seed); };
    Path result;
    for (const Contour &contour : flatten(path, tolerance)) {
        // Repeated points have no direction to move across.
        std::vector<Vec2> p;
        for (const Vec2 point : contour.points)
            if (p.empty() || point != p.back())
                p.push_back(point);
        if (contour.closed && p.size() > 1 && p.back() == p.front())
            p.pop_back();
        if (p.empty())
            continue;
        const std::size_t n = p.size();
        const std::size_t segments = contour.closed ? n : n - 1;
        std::vector<Vec2> normals(segments);
        for (std::size_t i = 0; i < segments; ++i)
            normals[i] = perpendicular(normalized(p[(i + 1) % n] - p[i]));
        std::vector<Vec2> miters(n);
        for (std::size_t i = 0; i < n; ++i)
            miters[i] = miter(contour.closed || i > 0 ? normals[(i + segments - 1) % segments] : Vec2{},
                              i < segments ? normals[i] : Vec2{});
        // Points along a segment move along the blend of its ends' miters, which, as each miter short of the cap does,
        // reaches exactly a unit along the segment's normal.
        std::vector<Vec2> moved;
        for (std::size_t i = 0; i < n; ++i) {
            moved.push_back(p[i] + miters[i] * offset(p[i]));
            if (i == segments)
                break;
            const std::size_t next = (i + 1) % n;
            const int pieces = piecesFor(length(p[next] - p[i]), bend, tolerance);
            for (int k = 1; k < pieces; ++k) {
                const double t = static_cast<double>(k) / pieces;
                const Vec2 q = lerp(p[i], p[next], t);
                moved.push_back(q + lerp(miters[i], miters[next], t) * offset(q));
            }
        }
        result.moveTo(moved.front());
        for (const Vec2 q : std::span(moved).subspan(1))
            result.lineTo(q);
        if (contour.closed)
            result.close();
    }
    return result;
}

} // namespace linea
