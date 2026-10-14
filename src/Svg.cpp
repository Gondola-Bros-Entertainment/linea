// linea - SVG's shared syntax: errors, numbers, and path data after SVG 2's grammar (section 9.3.9) and its error
// handling (section 9.5.4).
#include "linea/Svg.hpp"

#include "Shading.hpp"
#include "SvgSyntax.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <cstdlib>
#include <functional>
#include <numbers>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace linea {
namespace {

constexpr double radiansPerDegree = std::numbers::pi / 180.0;
constexpr double byteMax = 255.0;
// The longest number std::to_chars writes for a double is 24 characters, such as -2.2250738585072014e-308.
constexpr std::size_t numberBuffer = 32;
// An exponent's digits stop counting here: past it a number is 0 or an infinity whatever its mantissa, for any text
// shorter than this many characters.
constexpr long long exponentCap = 1'000'000'000'000'000;

std::string placed(const std::string &message, int line, int column) {
    if (line <= 0)
        return message;
    return "line " + std::to_string(line) + ", column " + std::to_string(column) + ": " + message;
}

// The double that the decimal mantissa @p digits times 10 to the @p exponent comes to, correctly rounded. strtod rounds
// correctly, and with no decimal point in what it reads, the C locale's choice of one does not matter.
double decimalValue(std::string_view digits, long long exponent) {
    const std::size_t first = std::min(digits.find_first_not_of('0'), digits.size());
    if (first == digits.size())
        return 0.0;
    std::string text(digits.substr(first));
    text += 'e';
    text += std::to_string(exponent);
    return std::strtod(text.c_str(), nullptr);
}

// The letters of SVG's path commands, absolute and relative.
constexpr std::string_view commandLetters = "MmZzLlHhVvCcSsQqTtAa";

// The command that writes @p verb's segment in absolute coordinates.
constexpr char verbLetter(Verb verb) {
    switch (verb) {
    case Verb::move:
        return 'M';
    case Verb::line:
        return 'L';
    case Verb::quad:
        return 'Q';
    case Verb::cubic:
        return 'C';
    case Verb::close:
        return 'Z';
    }
    return 'Z';
}

constexpr std::string_view byteOrderMark = "\xEF\xBB\xBF";
// UTF-8 continuation bytes are 10xxxxxx.
constexpr unsigned utf8ContinuationMask = 0xC0;
constexpr unsigned utf8Continuation = 0x80;

constexpr char upper(char c) { return c >= 'a' && c <= 'z' ? static_cast<char>(c - 'a' + 'A') : c; }

// Reads path data one segment at a time into a path, stopping at the first error.
class PathDataReader {
  public:
    explicit PathDataReader(std::string_view data) : data_(data) {}

    detail::PathDataReading read() && {
        skipSpace();
        if (at_ == data_.size())
            return std::move(reading_);
        if (upper(data_[at_]) != 'M') {
            fail("path data must start with a moveto, M or m");
            return std::move(reading_);
        }
        while (at_ < data_.size()) {
            const char command = data_[at_];
            if (commandLetters.find(command) == std::string_view::npos) {
                fail("expected a command letter");
                break;
            }
            ++at_;
            skipSpace();
            if (upper(command) == 'Z') {
                reading_.path.close();
                previous_ = 'Z';
            } else if (!segments(command)) {
                break;
            }
            skipSpace();
        }
        return std::move(reading_);
    }

  private:
    // Reads the argument groups that follow @p command, the first one required, adding a segment for each. Whether no
    // error stopped them.
    bool segments(char command) {
        const bool relative = command >= 'a';
        char kind = upper(command);
        while (true) {
            if (!segment(kind, relative))
                return false;
            // A moveto's further pairs are linetos, relative when it is.
            if (kind == 'M')
                kind = 'L';
            const bool comma = skipCommaSpace();
            if (at_ < data_.size() && startsNumber(data_[at_]))
                continue;
            if (comma) {
                fail("expected a number after the comma");
                return false;
            }
            return true;
        }
    }

    // Reads one argument group of a command of @p kind, an upper-case letter, and adds its segment. Whether the group
    // was whole.
    bool segment(char kind, bool relative) {
        const std::size_t start = at_;
        const Vec2 current = reading_.path.current();
        // Relative coordinates count from the current point where the segment starts.
        const Vec2 base = relative ? current : Vec2{};
        switch (kind) {
        case 'M':
        case 'L': {
            const std::optional<Vec2> to = pair();
            if (!to)
                return false;
            const Vec2 point = base + *to;
            if (kind == 'M') {
                if (!finite(start, std::span(&point, 1)))
                    return false;
                reading_.path.moveTo(point);
            } else if (!add(start, [&](Path &piece) { piece.lineTo(point); })) {
                return false;
            }
            break;
        }
        case 'H':
        case 'V': {
            const std::optional<double> value = number();
            if (!value)
                return false;
            const Vec2 point = kind == 'H' ? Vec2{base.x + *value, current.y} : Vec2{current.x, base.y + *value};
            if (!add(start, [&](Path &piece) { piece.lineTo(point); }))
                return false;
            break;
        }
        case 'C':
        case 'S': {
            const std::optional<Vec2> first = pair();
            const std::optional<Vec2> second = first ? nextPair() : std::nullopt;
            const std::optional<Vec2> third = second && kind == 'C' ? nextPair() : std::nullopt;
            if (!second || (kind == 'C' && !third))
                return false;
            // S reflects the previous cubic's second control point about the current point (SVG 2, section 9.5.2).
            const Vec2 control1 = kind == 'C'
                                      ? base + *first
                                      : (previous_ == 'C' || previous_ == 'S' ? current * 2.0 - control_ : current);
            const Vec2 control2 = base + (kind == 'C' ? *second : *first);
            const Vec2 end = base + (kind == 'C' ? *third : *second);
            if (!add(start, [&](Path &piece) { piece.cubicTo(control1, control2, end); }))
                return false;
            control_ = control2;
            break;
        }
        case 'Q':
        case 'T': {
            const std::optional<Vec2> first = pair();
            const std::optional<Vec2> second = first && kind == 'Q' ? nextPair() : std::nullopt;
            if (!first || (kind == 'Q' && !second))
                return false;
            const Vec2 control = kind == 'Q'
                                     ? base + *first
                                     : (previous_ == 'Q' || previous_ == 'T' ? current * 2.0 - control_ : current);
            const Vec2 end = base + (kind == 'Q' ? *second : *first);
            if (!add(start, [&](Path &piece) { piece.quadTo(control, end); }))
                return false;
            control_ = control;
            break;
        }
        default: {
            // An arc: SVG 2 permits any number for the radii and the rotation (section 9.5.1), though not for the
            // flags.
            const std::optional<double> rx = number();
            const std::optional<double> ry = rx ? nextNumber() : std::nullopt;
            const std::optional<double> rotation = ry ? nextNumber() : std::nullopt;
            const std::optional<bool> largeArc = rotation ? nextFlag() : std::nullopt;
            const std::optional<bool> sweep = largeArc ? nextFlag() : std::nullopt;
            const std::optional<Vec2> to = sweep ? nextPair() : std::nullopt;
            if (!to)
                return false;
            const Vec2 end = base + *to;
            if (!add(start, [&](Path &piece) {
                    piece.arcTo({*rx, *ry}, *rotation * radiansPerDegree, *largeArc, *sweep, end);
                }))
                return false;
            break;
        }
        }
        previous_ = kind;
        return true;
    }

    // Adds the segments that @p draw adds to a path at the current point, unless one reaches a point that is not
    // finite, as a relative coordinate or an arc can from finite numbers; then reports that at @p start. Whether it
    // added them.
    template <typename Draw> bool add(std::size_t start, Draw draw) {
        Path piece;
        piece.moveTo(reading_.path.current());
        draw(piece);
        if (!finite(start, piece.points()))
            return false;
        std::size_t at = 1;
        const std::vector<Vec2> &points = piece.points();
        for (const Verb verb : std::span(piece.verbs()).subspan(1)) {
            const Vec2 *p = points.data() + at;
            if (verb == Verb::line)
                reading_.path.lineTo(p[0]);
            else if (verb == Verb::quad)
                reading_.path.quadTo(p[0], p[1]);
            else if (verb == Verb::cubic)
                reading_.path.cubicTo(p[0], p[1], p[2]);
            at += static_cast<std::size_t>(pointCount(verb));
        }
        return true;
    }

    // Whether every one of @p points is finite; if not, reports it at @p start.
    bool finite(std::size_t start, std::span<const Vec2> points) {
        if (std::ranges::all_of(points, [](Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }))
            return true;
        at_ = start;
        fail("a coordinate here is too large for a double");
        return false;
    }

    // A coordinate pair, or nothing after reporting what was missing.
    std::optional<Vec2> pair() {
        const std::optional<double> x = number();
        const std::optional<double> y = x ? nextNumber() : std::nullopt;
        if (!y)
            return std::nullopt;
        return Vec2{*x, *y};
    }

    // The pair, number or flag after an argument and the comma_wsp that may separate them.
    std::optional<Vec2> nextPair() {
        skipCommaSpace();
        return pair();
    }
    std::optional<double> nextNumber() {
        skipCommaSpace();
        return number();
    }
    std::optional<bool> nextFlag() {
        skipCommaSpace();
        return flag();
    }

    // A number, or nothing after reporting what was wrong.
    std::optional<double> number() {
        const std::optional<detail::ScannedNumber> scanned = detail::scanNumber(data_, at_);
        if (!scanned) {
            fail("expected a number");
            return std::nullopt;
        }
        if (!std::isfinite(scanned->value)) {
            fail("a number too large for a double");
            return std::nullopt;
        }
        at_ = scanned->end;
        return scanned->value;
    }

    // An arc flag, 0 or 1, or nothing after reporting that it was missing.
    std::optional<bool> flag() {
        if (at_ < data_.size() && (data_[at_] == '0' || data_[at_] == '1'))
            return data_[at_++] == '1';
        fail("expected an arc flag, 0 or 1");
        return std::nullopt;
    }

    static constexpr bool startsNumber(char c) { return detail::isDigit(c) || c == '.' || c == '+' || c == '-'; }

    void skipSpace() {
        while (at_ < data_.size() && detail::isSvgSpace(data_[at_]))
            ++at_;
    }

    // Skips SVG's comma_wsp, white space with at most one comma among it. Whether there was a comma.
    bool skipCommaSpace() {
        skipSpace();
        if (at_ < data_.size() && data_[at_] == ',') {
            ++at_;
            skipSpace();
            return true;
        }
        return false;
    }

    // Records the error @p message at the current place.
    void fail(std::string message) {
        reading_.errorAt = at_;
        reading_.error = std::move(message);
    }

    std::string_view data_;
    std::size_t at_ = 0;
    detail::PathDataReading reading_;
    // The previous segment's command, as an upper-case letter, and the last control point of a curve, for S and T.
    char previous_ = 0;
    Vec2 control_{};
};

// A straight color, its channels sRGB-encoded, and its alpha, each from 0 to 1.
struct Straight {
    double r{};
    double g{};
    double b{};
    double a{};
};

Straight straight(Color c) { return {c.r / byteMax, c.g / byteMax, c.b / byteMax, c.a / byteMax}; }

Straight straight(const detail::LinearColor &c) {
    return {detail::encodeSrgb(c.r), detail::encodeSrgb(c.g), detail::encodeSrgb(c.b), c.a};
}

std::uint8_t toByte(double fraction) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(fraction, 0.0, 1.0) * byteMax));
}

Color quantized(const Straight &c) { return {toByte(c.r), toByte(c.g), toByte(c.b), toByte(c.a)}; }

// The color @p u of the way from @p from to @p to, its channels blended in @p space, sRGB's encoded channels or its
// linear ones: as SVG blends, each channel and alpha in proportion, or when @p premultiplied, as linea blends, alpha
// and each channel times alpha in proportion.
Straight blend(const Straight &from, const Straight &to, double u, Interpolation space, bool premultiplied) {
    const bool linearLight = space == Interpolation::srgbLinear;
    const auto into = [&](double channel) { return linearLight ? detail::decodeSrgb(channel) : channel; };
    const auto outOf = [&](double channel) { return linearLight ? detail::encodeSrgb(channel) : channel; };
    const double a = from.a + (to.a - from.a) * u;
    if (premultiplied && !(a > 0.0))
        return {};
    const double fromWeight = premultiplied ? from.a : 1.0;
    const double toWeight = premultiplied ? to.a : 1.0;
    const double total = premultiplied ? a : 1.0;
    const auto channel = [&](double first, double second) {
        const double start = into(first) * fromWeight;
        return outOf((start + (into(second) * toWeight - start) * u) / total);
    };
    return {channel(from.r, to.r), channel(from.g, to.g), channel(from.b, to.b), a};
}

// How far apart @p x and @p y look over any background: the most that a channel times alpha, or alpha, differs.
double difference(const Straight &x, const Straight &y) {
    return std::max({std::abs(x.r * x.a - y.r * y.a), std::abs(x.g * x.a - y.g * y.a), std::abs(x.b * x.a - y.b * y.a),
                     std::abs(x.a - y.a)});
}

// Fits stops to a ramp segment by segment: where the blend between a segment's two ends strays from the ramp by more
// than svgGradientTolerance, it adds a stop at the ramp's color halfway along, and fits each half the same way.
class StopFitter {
  public:
    // Each piece's blend is measured at this many points less one, evenly spaced inside it.
    static constexpr int samples = 16;
    // Halvings of one segment at most: 4096 pieces, far more than a ramp between two stops needs.
    static constexpr int deepest = 12;

    // @p ramp gives a segment's color @p u of the way along it. The stops are for a blend in @p space, linea's when
    // @p premultiplied and SVG's otherwise.
    using Ramp = std::function<Straight(std::size_t segment, double u)>;
    StopFitter(const std::vector<GradientStop> &sorted, Ramp ramp, Interpolation space, bool premultiplied)
        : sorted_(sorted), ramp_(std::move(ramp)), space_(space), premultiplied_(premultiplied) {}

    std::vector<GradientStop> fit() {
        if (sorted_.empty())
            return {};
        out_.push_back(sorted_.front());
        for (std::size_t i = 0; i + 1 < sorted_.size(); ++i) {
            if (sorted_[i].offset == sorted_[i + 1].offset)
                out_.push_back(sorted_[i + 1]);
            else
                piece(i, 0.0, 1.0, sorted_[i].color, sorted_[i + 1].color, 0);
        }
        return std::move(out_);
    }

  private:
    // Fits the part of segment @p segment from @p from to @p to, of which @p left and @p right are the colors.
    void piece(std::size_t segment, double from, double to, Color left, Color right, int depth) {
        // SVG blends a transparent stop's color into its neighbor's, so a transparent end takes the other end's color,
        // which leaves only alpha to blend.
        if (!premultiplied_ && left.a == 0)
            left = {right.r, right.g, right.b, 0};
        if (!premultiplied_ && right.a == 0)
            right = {left.r, left.g, left.b, 0};
        bool close = true;
        for (int k = 1; k < samples && close; ++k) {
            const double u = static_cast<double>(k) / samples;
            close = difference(blend(straight(left), straight(right), u, space_, premultiplied_),
                               ramp_(segment, from + (to - from) * u)) <= svgGradientTolerance;
        }
        if (!close && depth < deepest) {
            const double middle = from + (to - from) / 2.0;
            const Color halfway = quantized(ramp_(segment, middle));
            piece(segment, from, middle, left, halfway, depth + 1);
            piece(segment, middle, to, halfway, right, depth + 1);
            return;
        }
        const double start = sorted_[segment].offset;
        const double end = sorted_[segment + 1].offset;
        const auto at = [&](double u) { return u == 1.0 ? end : start + (end - start) * u; };
        // A transparent end that changed color for this piece needs a stop of its own, at the same offset.
        if (out_.back().color != left)
            out_.push_back({at(from), left});
        out_.push_back({at(to), right});
    }

    const std::vector<GradientStop> &sorted_;
    Ramp ramp_;
    Interpolation space_;
    bool premultiplied_;
    std::vector<GradientStop> out_;
};

} // namespace

SvgError::SvgError(const std::string &message, int line, int column)
    : std::runtime_error(placed(message, line, column)), line_(line), column_(column) {}

namespace detail {

std::string_view trimmed(std::string_view text) {
    while (!text.empty() && isSvgSpace(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && isSvgSpace(text.back()))
        text.remove_suffix(1);
    return text;
}

std::optional<ScannedNumber> scanNumber(std::string_view text, std::size_t at) {
    std::size_t i = at;
    bool negative = false;
    if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
        negative = text[i] == '-';
        ++i;
    }
    std::string digits;
    while (i < text.size() && isDigit(text[i]))
        digits += text[i++];
    long long fractionDigits = 0;
    if (i + 1 < text.size() && text[i] == '.' && isDigit(text[i + 1])) {
        ++i;
        while (i < text.size() && isDigit(text[i])) {
            digits += text[i++];
            ++fractionDigits;
        }
    }
    if (digits.empty())
        return std::nullopt;
    // An e takes part only when digits follow it, with or without a sign; otherwise it starts what follows, as in 1em.
    long long exponent = 0;
    if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
        std::size_t j = i + 1;
        bool negativeExponent = false;
        if (j < text.size() && (text[j] == '+' || text[j] == '-')) {
            negativeExponent = text[j] == '-';
            ++j;
        }
        if (j < text.size() && isDigit(text[j])) {
            while (j < text.size() && isDigit(text[j])) {
                if (exponent < exponentCap)
                    exponent = exponent * 10 + (text[j] - '0');
                ++j;
            }
            if (negativeExponent)
                exponent = -exponent;
            i = j;
        }
    }
    const double magnitude = decimalValue(digits, exponent - fractionDigits);
    return ScannedNumber{negative ? -magnitude : magnitude, i};
}

void appendNumber(std::string &out, double value) {
    std::array<char, numberBuffer> buffer{};
    // Adding 0 turns negative zero into zero and leaves every other value as it is.
    const std::to_chars_result written = std::to_chars(buffer.data(), buffer.data() + buffer.size(), value + 0.0);
    out.append(buffer.data(), written.ptr);
}

PathDataReading readPathData(std::string_view data) { return PathDataReader(data).read(); }

std::uint8_t scaledAlpha(std::uint8_t alpha, double opacity) {
    const double scaled = static_cast<double>(alpha) / byteMax * std::clamp(opacity, 0.0, 1.0);
    return static_cast<std::uint8_t>(std::lround(std::clamp(scaled, 0.0, 1.0) * byteMax));
}

std::vector<GradientStop> svgStops(const std::vector<GradientStop> &stops, Interpolation interpolation,
                                   bool degenerate) {
    // linea's ramp, as its renderer shades it: a gradient along the unit interval.
    const detail::Shader shader(Paint{LinearGradient{{0.0, 0.0}, {1.0, 0.0}, stops, SpreadMethod::pad, interpolation}},
                                identity());
    if (degenerate)
        return {{0.0, quantized(straight(shader.at({0.0, 0.0})))}};
    std::vector<GradientStop> sorted = stops;
    for (GradientStop &stop : sorted)
        stop.offset = std::clamp(stop.offset, 0.0, 1.0);
    std::ranges::stable_sort(sorted, {}, &GradientStop::offset);
    const auto ramp = [&](std::size_t segment, double u) {
        if (u == 0.0)
            return straight(sorted[segment].color);
        if (u == 1.0)
            return straight(sorted[segment + 1].color);
        const double start = sorted[segment].offset;
        return straight(shader.at({start + (sorted[segment + 1].offset - start) * u, 0.0}));
    };
    // A browser blends the stops in sRGB, which is SVG's default color-interpolation, and the document sets no other.
    return StopFitter(sorted, ramp, Interpolation::srgb, false).fit();
}

std::vector<GradientStop> lineaStops(const std::vector<GradientStop> &stops, Interpolation space) {
    const auto ramp = [&](std::size_t segment, double u) {
        return blend(straight(stops[segment].color), straight(stops[segment + 1].color), u, space, false);
    };
    return StopFitter(stops, ramp, space, true).fit();
}

StrokeStyle svgInitialStroke() { return {.width = 1.0, .join = Join::miter, .cap = Cap::butt, .miterLimit = 4.0}; }

TextPosition positionOf(std::string_view text, std::size_t offset) {
    TextPosition position{1, 1};
    // A byte order mark marks the encoding, and takes no column.
    const std::size_t start = text.starts_with(byteOrderMark) ? byteOrderMark.size() : 0;
    for (std::size_t i = start; i < offset && i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n' || (c == '\r' && (i + 1 >= text.size() || text[i + 1] != '\n'))) {
            ++position.line;
            position.column = 1;
        } else if (c != '\r' && (static_cast<unsigned char>(c) & utf8ContinuationMask) != utf8Continuation) {
            // A UTF-8 continuation byte belongs to the character before it.
            ++position.column;
        }
    }
    return position;
}

} // namespace detail

std::string toSvgPathData(const Path &path) {
    std::string data;
    std::size_t at = 0;
    const std::vector<Vec2> &points = path.points();
    for (const Verb verb : path.verbs()) {
        if (!data.empty())
            data += ' ';
        data += verbLetter(verb);
        for (int i = 0; i < pointCount(verb); ++i) {
            const Vec2 p = points[at++];
            if (!std::isfinite(p.x) || !std::isfinite(p.y))
                throw SvgError("a path has a point that is not finite, which SVG cannot write");
            if (i > 0)
                data += ' ';
            detail::appendNumber(data, p.x);
            data += ' ';
            detail::appendNumber(data, p.y);
        }
    }
    return data;
}

SvgPathData fromSvgPathData(std::string_view data) {
    detail::PathDataReading reading = detail::readPathData(data);
    SvgPathData result{std::move(reading.path), std::nullopt};
    if (reading.errorAt) {
        const detail::TextPosition at = detail::positionOf(data, *reading.errorAt);
        result.error = SvgError(reading.error, at.line, at.column);
    }
    return result;
}

} // namespace linea
