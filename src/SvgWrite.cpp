// linea - SVG writing: a drawing's nodes as path and g elements, every number in its shortest exact form.
#include "linea/Svg.hpp"

#include "SvgSyntax.hpp"
#include "Xml.hpp"
#include "linea/Color.hpp"

#include <cmath>
#include <cstdint>
#include <initializer_list>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace linea {
namespace {

constexpr std::uint8_t opaque = 255;
constexpr Color black{0, 0, 0, opaque};
// Two spaces indent each level of nesting.
constexpr std::string_view indentStep = "  ";
constexpr double byteMax = 255.0;
// An alpha byte needs three decimal places at most to read back as itself: rounded to three, a/255 is within 0.0005 of
// itself, which 255 scales to less than half a step.
constexpr int mostAlphaDecimals = 3;

void requireFinite(double value, std::string_view what) {
    if (!std::isfinite(value))
        throw SvgError(std::string(what) + " is not finite, and SVG numbers must be");
}

std::string_view joinName(Join join) {
    switch (join) {
    case Join::miter:
        return "miter";
    case Join::round:
        return "round";
    case Join::bevel:
        return "bevel";
    }
    return "miter";
}

std::string_view capName(Cap cap) {
    switch (cap) {
    case Cap::butt:
        return "butt";
    case Cap::round:
        return "round";
    case Cap::square:
        return "square";
    }
    return "butt";
}

// The shortest decimal opacity that detail::scaledAlpha() turns back into @p alpha from an opaque color.
std::string alphaText(std::uint8_t alpha) {
    double scale = 1.0;
    for (int decimals = 1;; ++decimals) {
        scale *= 10.0;
        const double candidate = std::round(alpha / byteMax * scale) / scale;
        if (decimals == mostAlphaDecimals || detail::scaledAlpha(opaque, candidate) == alpha) {
            std::string text;
            detail::appendNumber(text, candidate);
            return text;
        }
    }
}

// Throws for a node that uses an effect this writer does not write, naming the field: written without it, the node
// would draw differently, with no error to say so.
void refuseEffects(std::string_view node, double blur, const std::optional<DropShadow> &shadow,
                   const std::optional<Clip> &clip) {
    const auto refuse = [&](std::string_view field) {
        throw SvgError(std::string(node) + "::" + std::string(field) +
                       " is set, and toSvg() does not write it; the node would draw differently without it");
    };
    if (blur != 0.0)
        refuse("blur");
    if (shadow)
        refuse("shadow");
    if (clip)
        refuse("clip");
}

std::string_view spreadName(SpreadMethod spread) {
    switch (spread) {
    case SpreadMethod::pad:
        return "pad";
    case SpreadMethod::repeat:
        return "repeat";
    case SpreadMethod::reflect:
        return "reflect";
    }
    return "pad";
}

// Appends the attribute @p name with @p value to @p out. Every value written is a number, a color, a keyword or an id
// made of letters and digits, none of which holds a character that XML must escape.
void attribute(std::string &out, std::string_view name, std::string_view value) {
    out += ' ';
    out += name;
    out += "=\"";
    out += value;
    out += '"';
}

void number(std::string &out, std::string_view name, double value, std::string_view what) {
    requireFinite(value, what);
    std::string text;
    detail::appendNumber(text, value);
    attribute(out, name, text);
}

void indent(std::string &out, int depth) {
    for (int i = 0; i < depth; ++i)
        out += indentStep;
}

class Writer {
  public:
    std::string write(const Drawing &drawing) {
        requireFinite(drawing.width, "the drawing's width");
        requireFinite(drawing.height, "the drawing's height");
        if (drawing.width < 0.0 || drawing.height < 0.0)
            throw SvgError("the drawing's width and height must not be negative");
        for (const Node &node : drawing.nodes)
            write(node, 1);
        std::string document = "<svg";
        attribute(document, "xmlns", detail::svgNamespaceUri);
        if (lineaNamespace_)
            attribute(document, "xmlns:linea", detail::lineaNamespaceUri);
        number(document, "width", drawing.width, "the drawing's width");
        number(document, "height", drawing.height, "the drawing's height");
        std::string viewBox = "0 0 ";
        detail::appendNumber(viewBox, drawing.width);
        viewBox += ' ';
        detail::appendNumber(viewBox, drawing.height);
        attribute(document, "viewBox", viewBox);
        attribute(document, "preserveAspectRatio", "none");
        if (body_.empty())
            return document + "/>\n";
        document += ">\n";
        if (!defs_.empty()) {
            indent(document, 1);
            document += "<defs>\n" + defs_;
            indent(document, 1);
            document += "</defs>\n";
        }
        return document + body_ + "</svg>\n";
    }

  private:
    void write(const Node &node, int depth) {
        std::visit([&](const auto &content) { write(content, depth); }, node.content);
    }

    void write(const Indirect<Group> &group, int depth) { write(*group, depth); }

    void write(const Group &group, int depth) {
        refuseEffects("Group", group.blur, group.shadow, group.clip);
        if (group.mask)
            throw SvgError("Group::mask is set, and toSvg() does not write it; the node would draw differently "
                           "without it");
        indent(body_, depth);
        body_ += "<g";
        transform(body_, group.transform);
        if (group.opacity != 1.0)
            number(body_, "opacity", group.opacity, "a group's opacity");
        if (group.children.empty()) {
            body_ += "/>\n";
            return;
        }
        body_ += ">\n";
        for (const Node &child : group.children)
            write(child, depth + 1);
        indent(body_, depth);
        body_ += "</g>\n";
    }

    void write(const Layer &layer, int depth) {
        refuseEffects("Layer", layer.blur, layer.shadow, layer.clip);
        std::string element;
        indent(element, depth);
        element += "<path";
        transform(element, layer.transform);
        if (layer.opacity != 1.0)
            number(element, "opacity", layer.opacity, "a layer's opacity");
        attribute(element, "d", toSvgPathData(layer.path));
        // SVG fills with opaque black unless told otherwise.
        if (!layer.fill)
            attribute(element, "fill", "none");
        else if (*layer.fill != Paint{black})
            paint(element, "fill", *layer.fill);
        if (layer.fillRule == FillRule::evenOdd)
            attribute(element, "fill-rule", "evenodd");
        if (layer.stroke)
            paint(element, "stroke", *layer.stroke);
        strokeStyle(element, layer.strokeStyle, layer.stroke ? detail::svgInitialStroke() : StrokeStyle{});
        body_ += element + "/>\n";
    }

    // The fill or stroke attribute, @p name, and its opacity, on @p element.
    void paint(std::string &element, std::string_view name, const Paint &value) {
        if (const auto *color = std::get_if<Color>(&value)) {
            attribute(element, name, toHex({color->r, color->g, color->b, opaque}));
            if (color->a != opaque)
                attribute(element, std::string(name) + "-opacity", alphaText(color->a));
            return;
        }
        if (std::holds_alternative<ConicGradient>(value))
            throw SvgError("a conic gradient cannot be written: SVG has none");
        if (std::holds_alternative<Pattern>(value))
            throw SvgError("a pattern paint is not written to SVG yet");
        attribute(element, name, "url(#" + gradientId(value) + ")");
    }

    // The id of a gradient element in the defs that draws @p value, written there unless an identical one is.
    std::string gradientId(const Paint &value) {
        std::string definition;
        std::vector<GradientStop> stops;
        Interpolation interpolation = Interpolation::srgb;
        bool degenerate = false;
        if (const auto *linear = std::get_if<LinearGradient>(&value)) {
            definition = "linearGradient";
            attribute(definition, "gradientUnits", "userSpaceOnUse");
            number(definition, "x1", linear->from.x, "a gradient's start");
            number(definition, "y1", linear->from.y, "a gradient's start");
            number(definition, "x2", linear->to.x, "a gradient's end");
            number(definition, "y2", linear->to.y, "a gradient's end");
            if (linear->spread != SpreadMethod::pad)
                attribute(definition, "spreadMethod", spreadName(linear->spread));
            stops = linear->stops;
            interpolation = linear->interpolation;
            degenerate = linear->from == linear->to;
        } else {
            const auto &radial = std::get<RadialGradient>(value);
            if (radial.radius < 0.0 || radial.focalRadius < 0.0)
                throw SvgError("a radial gradient's radius is negative, which SVG does not allow");
            definition = "radialGradient";
            attribute(definition, "gradientUnits", "userSpaceOnUse");
            number(definition, "cx", radial.center.x, "a gradient's center");
            number(definition, "cy", radial.center.y, "a gradient's center");
            number(definition, "r", radial.radius, "a gradient's radius");
            if (radial.focal) {
                number(definition, "fx", radial.focal->x, "a gradient's focal point");
                number(definition, "fy", radial.focal->y, "a gradient's focal point");
            }
            if (radial.focalRadius != 0.0)
                number(definition, "fr", radial.focalRadius, "a gradient's focal radius");
            if (radial.spread != SpreadMethod::pad)
                attribute(definition, "spreadMethod", spreadName(radial.spread));
            stops = radial.stops;
            interpolation = radial.interpolation;
        }
        for (const GradientStop &stop : stops)
            requireFinite(stop.offset, "a gradient stop's offset");
        const std::vector<GradientStop> visible = detail::svgStops(stops, interpolation, degenerate);
        // Where SVG cannot draw the stops as they are, their originals go along for fromSvg() to restore.
        if (visible != stops || interpolation != Interpolation::srgb) {
            lineaNamespace_ = true;
            attribute(definition, "linea:interpolation", interpolationName(interpolation));
            std::string originals;
            for (const GradientStop &stop : stops) {
                if (!originals.empty())
                    originals += ' ';
                detail::appendNumber(originals, stop.offset);
                originals += ' ';
                originals += toHex(stop.color);
            }
            attribute(definition, "linea:stops", originals);
        }
        std::string children;
        for (const GradientStop &stop : visible) {
            indent(children, 3);
            children += "<stop";
            number(children, "offset", stop.offset, "a gradient stop's offset");
            attribute(children, "stop-color", toHex({stop.color.r, stop.color.g, stop.color.b, opaque}));
            if (stop.color.a != opaque)
                attribute(children, "stop-opacity", alphaText(stop.color.a));
            children += "/>\n";
        }
        // Identical gradients share one element.
        const std::string key = definition + children;
        if (const auto found = gradients_.find(key); found != gradients_.end())
            return found->second;
        const std::string id = "gradient" + std::to_string(gradients_.size() + 1);
        gradients_.emplace(key, id);
        const std::string name = definition.substr(0, definition.find(' '));
        indent(defs_, 2);
        defs_ += "<" + name + " id=\"" + id + "\"" + definition.substr(name.size());
        if (children.empty()) {
            defs_ += "/>\n";
        } else {
            defs_ += ">\n" + children;
            indent(defs_, 2);
            defs_ += "</" + name + ">\n";
        }
        return id;
    }

    static std::string_view interpolationName(Interpolation interpolation) {
        switch (interpolation) {
        case Interpolation::oklab:
            return "oklab";
        case Interpolation::srgb:
            return "srgb";
        case Interpolation::srgbLinear:
            return "srgb-linear";
        }
        return "oklab";
    }

    // The stroke properties of @p style that differ from @p base, which a reader assumes for those it is not given.
    static void strokeStyle(std::string &element, const StrokeStyle &style, const StrokeStyle &base) {
        if (style.width != base.width) {
            if (style.width < 0.0)
                throw SvgError("a stroke width is negative, which SVG does not allow");
            number(element, "stroke-width", style.width, "a stroke width");
        }
        if (style.join != base.join)
            attribute(element, "stroke-linejoin", joinName(style.join));
        if (style.cap != base.cap)
            attribute(element, "stroke-linecap", capName(style.cap));
        if (style.miterLimit != base.miterLimit) {
            if (style.miterLimit < 1.0)
                throw SvgError("a miter limit is below 1, which SVG does not allow");
            number(element, "stroke-miterlimit", style.miterLimit, "a miter limit");
        }
        if (style.dashArray != base.dashArray) {
            std::string dashes;
            for (const double dash : style.dashArray) {
                requireFinite(dash, "a dash length");
                if (dash < 0.0)
                    throw SvgError("a dash length is negative, which SVG does not allow");
                if (!dashes.empty())
                    dashes += ' ';
                detail::appendNumber(dashes, dash);
            }
            attribute(element, "stroke-dasharray", dashes.empty() ? "none" : dashes);
        }
        if (style.dashOffset != base.dashOffset)
            number(element, "stroke-dashoffset", style.dashOffset, "a dash offset");
    }

    // A transform attribute for @p m on @p element, unless it is the identity: as a translation or a scale when it is
    // one, which read back as exactly @p m, and as a matrix otherwise.
    static void transform(std::string &element, const Affine &m) {
        if (m == identity())
            return;
        std::string text;
        const auto numbers = [&](std::initializer_list<double> values) {
            bool first = true;
            for (const double value : values) {
                requireFinite(value, "a transform");
                if (!first)
                    text += ' ';
                detail::appendNumber(text, value);
                first = false;
            }
            text += ')';
        };
        if (m.a == 1.0 && m.b == 0.0 && m.c == 0.0 && m.d == 1.0) {
            text = "translate(";
            numbers({m.e, m.f});
        } else if (m.b == 0.0 && m.c == 0.0 && m.e == 0.0 && m.f == 0.0) {
            text = "scale(";
            numbers({m.a, m.d});
        } else {
            text = "matrix(";
            numbers({m.a, m.b, m.c, m.d, m.e, m.f});
        }
        attribute(element, "transform", text);
    }

    std::string body_;
    std::string defs_;
    // Each gradient element written, by what follows its id, with that id.
    std::unordered_map<std::string, std::string> gradients_;
    bool lineaNamespace_ = false;
};

} // namespace

std::string toSvg(const Drawing &drawing) { return Writer().write(drawing); }

} // namespace linea
