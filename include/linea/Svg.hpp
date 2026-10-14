// linea - SVG: drawings written as SVG documents, and SVG documents read as drawings.
#pragma once

#include "linea/Drawing.hpp"
#include "linea/Path.hpp"

#include <cstddef>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace linea {

// What toSvg() throws for a drawing that SVG cannot hold, and what fromSvg() throws for a document that is not
// well-formed XML or that asks for something linea cannot draw as a browser would. The message names the element,
// attribute or property at fault. fromSvg() also reports, without throwing, the errors that SVG draws past.
class SvgError : public std::runtime_error {
  public:
    // An error that @p message describes, at @p line and @p column of a document, both counted from 1, or at no place
    // in one when they are 0. what() starts with the place when there is one.
    explicit SvgError(const std::string &message, int line = 0, int column = 0);

    // The line of the document where the error lies, or 0 for an error that lies in no document.
    [[nodiscard]] int line() const { return line_; }
    // The column, in characters from 1, of the document where the error lies, or 0 for an error that lies in no
    // document.
    [[nodiscard]] int column() const { return column_; }

  private:
    int line_ = 0;
    int column_ = 0;
};

// @p drawing as an SVG document that a browser draws as render() draws it, and that fromSvg() reads back as a drawing
// equal to @p drawing.
//
// The svg element is width by height units with a viewBox over the same box and preserveAspectRatio="none", so that
// drawn at any size it stretches to fill it, as render() stretches a drawing. Each layer becomes a path element and
// each group a g element, with transform and opacity attributes where they differ from the identity and 1. SVG
// composites an element's opacity as one image, as linea composites a group's and a layer's fill and stroke. Paths are
// written as absolute coordinates, and every number in the shortest form that reads back as the same double. A
// translucent color is written as an opaque color and a fill-opacity or stroke-opacity, which every SVG reader
// understands. A layer that strokes has the stroke properties that differ from SVG's initial values; one that does not
// stroke has those that differ from linea's default StrokeStyle, which is what fromSvg() reads for a layer without a
// stroke.
//
// Gradients are linearGradient and radialGradient elements in a defs element, in userSpaceOnUse units, with the ids
// gradient1, gradient2 and so on in the order the drawing first uses them, identical gradients sharing one. SVG blends
// stops in sRGB without premultiplying them, so a gradient whose stops SVG would blend differently, by more than
// svgGradientTolerance, is written with more stops between them, and its own stops and interpolation go along in the
// linea:stops and linea:interpolation attributes, in linea's namespace, for fromSvg() to restore, as Inkscape keeps a
// path's original in inkscape:original-d. Browsers and other readers ignore attributes in a namespace they do not know.
//
// The document is ASCII from a fixed set of characters: every value it holds is a number, a color, a keyword or one of
// those ids, so none needs escaping, and it is the same for the same drawing.
//
// Throws SvgError for a number that is not finite; a negative width, height, stroke width, dash or gradient radius, or
// a miter limit below 1, all of which SVG holds invalid; a conic gradient, which SVG has no element for, and a pattern;
// and a node that has a blur, a drop shadow, a clip or a mask, which this writer does not write rather than write a
// drawing that would draw differently. Each message names the field at fault.
std::string toSvg(const Drawing &drawing);

// What fromSvg() reads from an SVG document.
struct SvgDocument {
    Drawing drawing;
    // The errors that SVG draws past rather than refusing the document: each in a path's data, or in a polyline's or
    // polygon's points, where SVG draws the shape up to its last whole segment before the error (SVG 2, section 9.5.4),
    // as the drawing does too. Each names the element and gives its place in the document. Every other error throws.
    std::vector<SvgError> errors;
};

// The drawing that the SVG document @p text describes, its width and height those of the svg element, in px, and its
// viewBox and preserveAspectRatio a transform on a group around the content where they move or scale it. The document
// is UTF-8 XML 1.0 with namespaces; its root is an svg element in the SVG namespace.
//
// Read: the svg, g, a, defs, path, rect, circle, ellipse, line, polyline, polygon and use elements; presentation
// attributes and style attribute declarations, inherited as SVG inherits them; every CSS Color 4 syntax for sRGB
// colors, currentColor and the color property; lengths in px, in, cm, mm, Q, pt and pc at 96 px to the inch, and
// percentages of the viewport as SVG defines them; and linearGradient and radialGradient with their stops, fx, fy and
// fr, spreadMethod, gradientUnits, gradientTransform, color-interpolation and the attributes and stops that their href
// templates lend them. A gradient reads as one in the srgb interpolation space, or in srgbLinear where its
// color-interpolation is linearRGB, which Chrome honors, with more stops between those that SVG blends differently from
// linea's premultiplied blend, by more than svgGradientTolerance, unless it holds the originals that toSvg() recorded
// and those still give the stops it holds. A radial gradient stretched unevenly, by its gradientTransform or
// by objectBoundingBox units on a box that is not square, fills as a layer of its own in the coordinates where it is
// circular. title, desc and metadata, and every element and attribute in another namespace, such as Inkscape's, are
// ignored, as SVG requires.
//
// Refused, by throwing SvgError naming the element or property: XML that is not well-formed, entity declarations,
// elements that nest deeper than deepestSvgNesting, a drawing larger than mostSvgDrawingBytes, values that SVG holds
// invalid, numbers that reach past the largest double, and every rendering feature linea cannot draw as a browser
// would: text, image, pattern, clipPath, mask, filter, marker, nested svg, switch, foreignObject, the style element and
// CSS selectors, script, animation, em and ex units, colors outside sRGB, stroke-first paint-order and vector-effect,
// and a stroke painted with an unevenly stretched radial gradient. A document never reads as a drawing that differs
// from what a browser draws without an error.
SvgDocument fromSvg(std::string_view text);

// @p path as SVG path data: the absolute commands M, L, Q, C and Z, one per verb, each number in the shortest form that
// reads back as the same double. fromSvgPathData() reads it back as a path equal to @p path. Throws SvgError for a
// point that is not finite.
std::string toSvgPathData(const Path &path);

// What fromSvgPathData() reads from SVG path data.
struct SvgPathData {
    // The path up to its last whole segment before the first error, or the whole path when there is none.
    Path path;
    // The first error, its line and column counted in the data, or nothing for data without one.
    std::optional<SvgError> error;
};

// The path that SVG path data @p data describes, by SVG 2's grammar (section 9.3.9): every command, absolute and
// relative, a command repeated by more numbers without its letter, a moveto followed by more pairs as linetos, numbers
// such as 1.5.5 (1.5 then .5), -.5 and 1e-5, and arc flags written without separators, as in a1 1 0 00 1 1. Arcs become
// cubic curves as Path::arcTo() makes them, and S and T reflect the previous curve's last control point. As SVG
// specifies, the path ends at the last whole segment before the first error, and the error is reported. Empty data, or
// data of only whitespace, is an empty path without error.
SvgPathData fromSvgPathData(std::string_view data);

// How far a gradient that toSvg() writes may stray, drawn by a browser, from the gradient linea draws, and a gradient
// that fromSvg() reads, drawn by linea, from the gradient a browser draws: on each sRGB-encoded channel multiplied by
// alpha, and on alpha, as a fraction of full scale. SVG blends a gradient's stops in sRGB without premultiplying them
// (SVG 2, section 14.2.4.2), where linea blends in its gradient's interpolation space, premultiplied, so where the two
// blends differ by more than this, toSvg() and fromSvg() add stops between those given until they do not. One 8-bit
// step.
inline constexpr double svgGradientTolerance = 1.0 / 255.0;

// fromSvg() refuses a document whose elements nest deeper than this, counting the elements that use elements place
// within one another, so that no document can exhaust the stack. libxml2 bounds nesting at the same depth.
inline constexpr int deepestSvgNesting = 256;

// fromSvg() refuses a document whose drawing would take more than this many bytes, counted as the sizes of its nodes,
// path verbs and points, gradient stops and dash lengths. A use element copies the element it refers to, and a
// reference to a gradient copies its stops, so without a bound a few kilobytes of use elements that refer to one
// another could ask for more memory than any machine has, as nested entities do in XML's billion laughs attack.
inline constexpr std::size_t mostSvgDrawingBytes = std::size_t{1} << 28;

} // namespace linea
