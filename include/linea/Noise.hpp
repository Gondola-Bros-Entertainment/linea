// linea - seeded noise: Perlin, simplex and fractal noise, Voronoi cells, and paths wobbled by noise.
#pragma once

#include "linea/Geometry.hpp"
#include "linea/Path.hpp"

#include <cstdint>

namespace linea {

// Noise here is a pure function of a point and a seed: lattice coordinates and the seed go through a fixed 32-bit
// integer hash, and the rest is plain arithmetic, so a seed gives the same noise on every platform, to within
// floating-point rounding. Its features are about one unit across; scale a point up to make them smaller.

// Ken Perlin's improved gradient noise (2002) at @p p for @p seed, from -1 to 1. Each point of the integer lattice
// takes one of eight gradients of equal length, which the hash picks, and his quintic fade blends them so that the
// noise is smooth to its second derivative. It is 0 at every lattice point. A point that is not finite gives 0.
double perlin(Vec2 p, std::uint32_t seed);

// Simplex noise at @p p for @p seed, from -1 to 1, as Stefan Gustavson formulates it ("Simplex noise demystified",
// 2005): a radial kernel at each corner of the lattice triangle that holds @p p, each kernel scaling one of the same
// eight gradients. Unlike Perlin noise's, its features do not line up with the axes. It is 0 at every corner of its
// triangles, among them the origin and every point (k, -k) for a whole number k. A point that is not finite gives 0.
double simplex(Vec2 p, std::uint32_t seed);

// A noise function, as perlin and simplex are.
using Noise = double (*)(Vec2 p, std::uint32_t seed);

// The layers of fractal Brownian motion.
struct Fractal {
    // How many layers of noise to sum.
    int octaves = 4;
    // How much each octave's frequency multiplies the one before's.
    double lacunarity = 2.0;
    // How much each octave's amplitude multiplies the one before's.
    double gain = 0.5;
};

// Fractal Brownian motion: @p fractal's octaves of @p noise at @p p summed, the first at frequency 1 and amplitude 1,
// and the whole divided by the sum of the octaves' amplitudes' magnitudes so that it stays within -1 to 1. Octave i
// samples @p noise with seed @p seed + i, so that the octaves' features do not line up. With no octaves it is 0.
double fbm(Noise noise, Vec2 p, std::uint32_t seed, const Fractal &fractal = {});

// Where a point lies among the feature points of Steven Worley's cellular noise ("A Cellular Texture Basis Function",
// 1996): one feature point in each unit cell of the integer lattice, placed uniformly at random within it by the seed.
// The point's Voronoi cell is the region nearer to its nearest feature point than to any other.
struct Voronoi {
    // The distances to the nearest and second-nearest feature points. They are equal on the boundary between two
    // cells, and neither changes faster than the point moves.
    double nearest{};
    double secondNearest{};
    // A hash of the nearest feature point's lattice cell and the seed: the same everywhere in one Voronoi cell, and
    // random from one cell to the next, to give each cell its own color or value.
    std::uint32_t id{};
};

// The feature points nearest @p p for @p seed. The search covers every cell that can hold either of the two nearest,
// so both distances are exact. A point that is not finite has none: both distances are infinite and the id is 0.
Voronoi voronoi(Vec2 p, std::uint32_t seed);

// How wobbled() moves a path's points.
struct Wobble {
    // The farthest the noise moves a point from the line of its segment.
    double amplitude{};
    // The noise is sampled at each point's position times this, so a wobble spans about 1 / frequency units.
    double frequency{};
    // Picks the noise, and the offset its samples shift by.
    std::uint32_t seed{};
};

// @p path flattened within @p tolerance, each point then moved by @p wobble's amplitude times simplex noise where the
// point lies, for an organic, hand-drawn outline. Each flattened segment first splits into pieces short enough that
// the noise strays at most @p tolerance from a straight line between their ends. A point where two segments meet
// moves to where their lines meet once each has moved that far along its normal, a quarter turn clockwise from its
// direction, as a miter joins them, but at most 4 times as far as the noise asks; a point where the contour doubles
// back stays put. Points along a segment move along the blend of its ends' miters, so that, short of that limit, they
// move exactly as far from the segment's line as the noise asks, and the direction they move in turns smoothly from
// point to point. The noise is smooth too, so a smooth outline stays smooth at every tolerance, and a closed contour,
// which starts and ends at one point, stays closed. The noise's samples shift by an offset the seed picks, so that no
// point stays put whatever the seed. Each subpath becomes a polyline that is open or closed as it was.
Path wobbled(const Path &path, const Wobble &wobble, double tolerance);

} // namespace linea
