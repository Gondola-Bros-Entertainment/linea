// linea - SVG reading: an XML tree read by Xml.cpp, styled as SVG cascades presentation attributes and style
// declarations, and drawn into layers and groups. Section numbers cite SVG 2 unless they name another specification.
#include "linea/Svg.hpp"

#include "SvgSyntax.hpp"
#include "Xml.hpp"
#include "linea/Color.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <variant>
#include <vector>

namespace linea {
namespace {

using detail::XmlElement;
using detail::XmlNamespace;

// Absolute lengths in px, at 96 px to the inch, as CSS Values and Units 4 fixes them.
constexpr double pxPerInch = 96.0;
constexpr double pxPerCm = pxPerInch / 2.54;
constexpr double pxPerMm = pxPerInch / 25.4;
constexpr double pxPerQuarterMm = pxPerMm / 4.0;
constexpr double pxPerPt = pxPerInch / 72.0;
constexpr double pxPerPc = pxPerInch / 6.0;

constexpr double percent = 100.0;
constexpr double byteMax = 255.0;
constexpr double radiansPerDegree = std::numbers::pi / 180.0;
// Angles in CSS: a turn is 360 degrees and 400 gradians.
constexpr double degreesPerTurn = 360.0;
constexpr double degreesPerGradian = degreesPerTurn / 400.0;
constexpr double degreesPerRadian = 180.0 / std::numbers::pi;

constexpr std::uint8_t opaque = 255;
constexpr Color black{0, 0, 0, opaque};
constexpr Color transparentBlack{0, 0, 0, 0};
// CSS counts every character beyond ASCII as one that may stand in an identifier.
constexpr unsigned char asciiEnd = 0x80;
// The memory a group takes beside its children's: the node, and the group that the node holds apart from itself.
constexpr std::size_t groupBytes = sizeof(Node) + sizeof(Group);

std::string lowered(std::string_view text) {
    std::string result(text);
    std::ranges::transform(result, result.begin(), detail::lowerAscii);
    return result;
}

bool isIdentifierCharacter(char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || detail::isDigit(c) || c == '-' || c == '_' ||
           static_cast<unsigned char>(c) >= asciiEnd;
}

bool isFinite(Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }

bool isFinite(const Affine &m) {
    return std::isfinite(m.a) && std::isfinite(m.b) && std::isfinite(m.c) && std::isfinite(m.d) && std::isfinite(m.e) &&
           std::isfinite(m.f);
}

bool isFinite(const Path &path) {
    return std::ranges::all_of(path.points(), [](Vec2 p) { return isFinite(p); });
}

// The element and document that a value comes from, to place the errors it causes.
class Place {
  public:
    Place(std::string_view text, const XmlElement &element) : text_(text), element_(element) {}

    // An error about this element that says @p problem.
    [[nodiscard]] SvgError error(const std::string &problem) const {
        const detail::TextPosition at = detail::positionOf(text_, element_.offset);
        return SvgError("<" + element_.name + ">: " + problem, at.line, at.column);
    }
    [[noreturn]] void fail(const std::string &problem) const { throw error(problem); }

  private:
    std::string_view text_;
    const XmlElement &element_;
};

// The box that percentages of lengths are percentages of: the root's viewBox, or its size without one, or for a
// gradient in objectBoundingBox units, the unit square.
struct Viewport {
    double width{};
    double height{};
};

// Which side of the viewport a length measures, as SVG 2 section 8.9 assigns them: widths and x coordinates along x,
// heights and y coordinates along y, and other lengths along the viewport's normalized diagonal.
enum class Axis : std::uint8_t { x, y, other };

double reference(const Viewport &viewport, Axis axis) {
    switch (axis) {
    case Axis::x:
        return viewport.width;
    case Axis::y:
        return viewport.height;
    case Axis::other:
        break;
    }
    return std::sqrt((viewport.width * viewport.width + viewport.height * viewport.height) / 2.0);
}

// The length @p text writes, in px, or nothing when it writes none. A number alone is in px, as presentation
// attributes allow. Throws for units that are not absolute, such as em.
std::optional<double> parseLength(std::string_view text, Axis axis, const Viewport &viewport, const Place &where) {
    text = detail::trimmed(text);
    const std::optional<detail::ScannedNumber> number = detail::scanNumber(text, 0);
    if (!number || !std::isfinite(number->value))
        return std::nullopt;
    const std::string unit = lowered(text.substr(number->end));
    const double value = number->value;
    // A length that units or a percentage take past the largest double is not one.
    const auto finite = [](double length) { return std::isfinite(length) ? std::optional(length) : std::nullopt; };
    if (unit.empty() || unit == "px")
        return value;
    if (unit == "%")
        return finite(value / percent * reference(viewport, axis));
    static constexpr std::array<std::pair<std::string_view, double>, 6> absolute{
        {{"in", pxPerInch}, {"cm", pxPerCm}, {"mm", pxPerMm}, {"q", pxPerQuarterMm}, {"pt", pxPerPt}, {"pc", pxPerPc}}};
    for (const auto &[name, scale] : absolute)
        if (unit == name)
            return finite(value * scale);
    if (std::ranges::all_of(unit, [](char c) { return c >= 'a' && c <= 'z'; }))
        where.fail(unit + " units are not supported; lengths may be in px, in, cm, mm, Q, pt or pc, or percentages");
    return std::nullopt;
}

// Splits @p text at SVG's comma_wsp: white space with at most one comma among it. Nothing when an item is empty, as
// between two commas or after a trailing one.
std::optional<std::vector<std::string_view>> splitList(std::string_view text) {
    std::vector<std::string_view> items;
    text = detail::trimmed(text);
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t start = i;
        while (i < text.size() && !detail::isSvgSpace(text[i]) && text[i] != ',')
            ++i;
        if (i == start)
            return std::nullopt;
        items.push_back(text.substr(start, i - start));
        while (i < text.size() && detail::isSvgSpace(text[i]))
            ++i;
        if (i < text.size() && text[i] == ',') {
            ++i;
            while (i < text.size() && detail::isSvgSpace(text[i]))
                ++i;
            if (i == text.size())
                return std::nullopt;
        }
    }
    return items;
}

// The numbers @p text lists, separated by comma_wsp, or nothing when it lists anything else. Numbers may follow one
// another without a separator where the grammar allows it, as in 1-2 or .5.5.
std::optional<std::vector<double>> parseNumbers(std::string_view text) {
    std::vector<double> numbers;
    std::size_t i = 0;
    const auto skipSpace = [&] {
        while (i < text.size() && detail::isSvgSpace(text[i]))
            ++i;
    };
    skipSpace();
    while (i < text.size()) {
        const std::optional<detail::ScannedNumber> number = detail::scanNumber(text, i);
        if (!number || !std::isfinite(number->value))
            return std::nullopt;
        numbers.push_back(number->value);
        i = number->end;
        skipSpace();
        if (i < text.size() && text[i] == ',') {
            ++i;
            skipSpace();
            if (i == text.size())
                return std::nullopt;
        }
    }
    return numbers;
}

// A transform list as the transform attribute writes one (CSS Transforms 1, section 7): matrix, translate, scale,
// rotate, skewX and skewY with unitless arguments, the first applied last. Nothing for any other text.
std::optional<Affine> parseTransform(std::string_view text) {
    text = detail::trimmed(text);
    if (lowered(text) == "none")
        return identity();
    Affine result = identity();
    std::size_t i = 0;
    while (i < text.size()) {
        const std::size_t nameStart = i;
        while (i < text.size() && ((text[i] >= 'a' && text[i] <= 'z') || (text[i] >= 'A' && text[i] <= 'Z')))
            ++i;
        const std::string name = lowered(text.substr(nameStart, i - nameStart));
        while (i < text.size() && detail::isSvgSpace(text[i]))
            ++i;
        if (i >= text.size() || text[i] != '(')
            return std::nullopt;
        const std::size_t close = text.find(')', i);
        if (close == std::string_view::npos)
            return std::nullopt;
        const std::optional<std::vector<double>> arguments = parseNumbers(text.substr(i + 1, close - i - 1));
        if (!arguments)
            return std::nullopt;
        const std::vector<double> &a = *arguments;
        const std::size_t count = a.size();
        Affine step;
        if (name == "matrix" && count == 6)
            step = {a[0], a[1], a[2], a[3], a[4], a[5]};
        else if (name == "translate" && (count == 1 || count == 2))
            step = translate({a[0], count == 2 ? a[1] : 0.0});
        else if (name == "scale" && (count == 1 || count == 2))
            step = scale(a[0], count == 2 ? a[1] : a[0]);
        else if (name == "rotate" && count == 1)
            step = rotate(a[0] * radiansPerDegree);
        else if (name == "rotate" && count == 3)
            step = rotateAround(a[0] * radiansPerDegree, {a[1], a[2]});
        else if (name == "skewx" && count == 1)
            step = skewX(a[0] * radiansPerDegree);
        else if (name == "skewy" && count == 1)
            step = skewY(a[0] * radiansPerDegree);
        else
            return std::nullopt;
        result = result * step;
        if (!isFinite(result))
            return std::nullopt;
        i = close + 1;
        while (i < text.size() && detail::isSvgSpace(text[i]))
            ++i;
        if (i < text.size() && text[i] == ',') {
            ++i;
            while (i < text.size() && detail::isSvgSpace(text[i]))
                ++i;
            if (i == text.size())
                return std::nullopt;
        }
    }
    return result;
}

// A token of a CSS function's arguments, as CSS Syntax 3 would read it, for the color functions.
struct Token {
    enum class Kind : std::uint8_t { number, percentage, dimension, identifier, comma, slash };
    Kind kind{};
    double value{};
    // A dimension's unit or an identifier, in lower case.
    std::string text;
};

// The tokens of @p text, or nothing when it holds something else.
std::optional<std::vector<Token>> tokenize(std::string_view text) {
    std::vector<Token> tokens;
    std::size_t i = 0;
    const auto identifierEnd = [&](std::size_t from) {
        while (from < text.size() && isIdentifierCharacter(text[from]))
            ++from;
        return from;
    };
    while (true) {
        while (i < text.size() && detail::isSvgSpace(text[i]))
            ++i;
        if (i == text.size())
            return tokens;
        if (text[i] == ',' || text[i] == '/') {
            tokens.push_back({text[i] == ',' ? Token::Kind::comma : Token::Kind::slash, 0.0, {}});
            ++i;
            continue;
        }
        if (const std::optional<detail::ScannedNumber> number = detail::scanNumber(text, i)) {
            if (!std::isfinite(number->value))
                return std::nullopt;
            i = number->end;
            if (i < text.size() && text[i] == '%') {
                tokens.push_back({Token::Kind::percentage, number->value, {}});
                ++i;
                continue;
            }
            const std::size_t end = identifierEnd(i);
            tokens.push_back({end > i ? Token::Kind::dimension : Token::Kind::number, number->value,
                              lowered(text.substr(i, end - i))});
            i = end;
            continue;
        }
        const std::size_t end = identifierEnd(i);
        if (end == i)
            return std::nullopt;
        tokens.push_back({Token::Kind::identifier, 0.0, lowered(text.substr(i, end - i))});
        i = end;
    }
}

bool isNone(const Token &token) { return token.kind == Token::Kind::identifier && token.text == "none"; }

// A color as a property gives it: a color, or currentColor, which stands for the color property's value.
struct ColorValue {
    bool current = false;
    Color color{};
};

// The arguments of a color function: three channels and an optional alpha, in CSS Color 4's modern syntax, separated
// by white space with the alpha after a slash, or its legacy one, separated by commas.
constexpr std::size_t channelCount = 3;
struct ColorArguments {
    std::array<Token, channelCount> channels;
    std::optional<Token> alpha;
    bool legacy = false;
};

std::optional<ColorArguments> colorArguments(const std::vector<Token> &tokens) {
    ColorArguments arguments;
    const bool legacy = std::ranges::any_of(tokens, [](const Token &t) { return t.kind == Token::Kind::comma; });
    arguments.legacy = legacy;
    std::vector<Token> values;
    if (legacy) {
        // v, v, v or v, v, v, a: a comma between each two.
        if (tokens.size() != 2 * channelCount - 1 && tokens.size() != 2 * channelCount + 1)
            return std::nullopt;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            const bool separator = i % 2 == 1;
            if (separator != (tokens[i].kind == Token::Kind::comma) || isNone(tokens[i]))
                return std::nullopt;
            if (!separator)
                values.push_back(tokens[i]);
        }
    } else {
        // v v v or v v v / a
        if (tokens.size() != channelCount && tokens.size() != channelCount + 2)
            return std::nullopt;
        for (std::size_t i = 0; i < tokens.size(); ++i) {
            if (i == channelCount) {
                if (tokens[i].kind != Token::Kind::slash)
                    return std::nullopt;
                continue;
            }
            if (tokens[i].kind == Token::Kind::comma || tokens[i].kind == Token::Kind::slash)
                return std::nullopt;
            values.push_back(tokens[i]);
        }
    }
    std::ranges::copy(values.begin(), values.begin() + channelCount, arguments.channels.begin());
    if (values.size() > channelCount)
        arguments.alpha = values[channelCount];
    return arguments;
}

std::uint8_t toByte(double fraction) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(fraction, 0.0, 1.0) * byteMax));
}

// An alpha value (CSS Color 4, section 4.2): a number from 0 to 1 or a percentage, clamped, or none for 0.
std::optional<std::uint8_t> alphaOf(const std::optional<Token> &token) {
    if (!token)
        return opaque;
    if (isNone(*token))
        return std::uint8_t{0};
    if (token->kind == Token::Kind::number)
        return toByte(token->value);
    if (token->kind == Token::Kind::percentage)
        return toByte(token->value / percent);
    return std::nullopt;
}

// A hue (CSS Color 4, section 4.3) in degrees: a number of degrees or an angle, or none for 0.
std::optional<double> hueOf(const Token &token) {
    if (isNone(token))
        return 0.0;
    if (token.kind == Token::Kind::number)
        return token.value;
    if (token.kind != Token::Kind::dimension)
        return std::nullopt;
    if (token.text == "deg")
        return token.value;
    if (token.text == "grad")
        return token.value * degreesPerGradian;
    if (token.text == "rad")
        return token.value * degreesPerRadian;
    if (token.text == "turn")
        return token.value * degreesPerTurn;
    return std::nullopt;
}

// A percentage, or in the modern syntax a number that stands for one, or none for 0, as a fraction.
std::optional<double> fractionOf(const Token &token, bool legacy) {
    if (token.kind == Token::Kind::percentage)
        return token.value / percent;
    if (!legacy && token.kind == Token::Kind::number)
        return token.value / percent;
    if (!legacy && isNone(token))
        return 0.0;
    return std::nullopt;
}

// rgb() and rgba() (CSS Color 4, section 5.1): channels from 0 to 255 or percentages, which the legacy syntax may not
// mix, each clamped and rounded to a byte.
std::optional<Color> rgbFunction(const ColorArguments &arguments) {
    std::array<std::uint8_t, channelCount> channels{};
    const Token::Kind firstKind = arguments.channels[0].kind;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const Token &token = arguments.channels[i];
        if (arguments.legacy && token.kind != firstKind)
            return std::nullopt;
        if (isNone(token))
            channels[i] = 0;
        else if (token.kind == Token::Kind::number)
            channels[i] = toByte(token.value / byteMax);
        else if (token.kind == Token::Kind::percentage)
            channels[i] = toByte(token.value / percent);
        else
            return std::nullopt;
    }
    const std::optional<std::uint8_t> alpha = alphaOf(arguments.alpha);
    if (!alpha)
        return std::nullopt;
    return Color{channels[0], channels[1], channels[2], *alpha};
}

// hsl() and hsla() (CSS Color 4, section 7), through fromHsl().
std::optional<Color> hslFunction(const ColorArguments &arguments) {
    const std::optional<double> hue = hueOf(arguments.channels[0]);
    const std::optional<double> saturation = fractionOf(arguments.channels[1], arguments.legacy);
    const std::optional<double> lightness = fractionOf(arguments.channels[2], arguments.legacy);
    const std::optional<std::uint8_t> alpha = alphaOf(arguments.alpha);
    if (!hue || !saturation || !lightness || !alpha)
        return std::nullopt;
    Color c = fromHsl({*hue, *saturation, *lightness, 1.0});
    c.a = *alpha;
    return c;
}

// hwb() (CSS Color 4, section 8), which has no legacy syntax. Its channels mix white and black into the pure hue,
// computed before rounding, as the sample code of section 8.1 computes them.
std::optional<Color> hwbFunction(const ColorArguments &arguments) {
    if (arguments.legacy)
        return std::nullopt;
    const std::optional<double> hue = hueOf(arguments.channels[0]);
    const std::optional<double> white = fractionOf(arguments.channels[1], false);
    const std::optional<double> blackness = fractionOf(arguments.channels[2], false);
    const std::optional<std::uint8_t> alpha = alphaOf(arguments.alpha);
    if (!hue || !white || !blackness || !alpha)
        return std::nullopt;
    const double w = std::clamp(*white, 0.0, 1.0);
    const double b = std::clamp(*blackness, 0.0, 1.0);
    if (w + b >= 1.0) {
        const std::uint8_t grey = toByte(w / (w + b));
        return Color{grey, grey, grey, *alpha};
    }
    // The pure hue, as hsl(hue 100% 50%) gives it, before rounding (CSS Color 4, section 7.1's hslToRgb).
    constexpr double twelfthsPerTurn = 12.0;
    constexpr double degreesPerTwelfth = degreesPerTurn / twelfthsPerTurn;
    const double h = std::fmod(std::fmod(*hue, degreesPerTurn) + degreesPerTurn, degreesPerTurn) / degreesPerTwelfth;
    const auto channel = [&](double offset) {
        const double k = std::fmod(offset + h, twelfthsPerTurn);
        const double pure = 0.5 - 0.5 * std::clamp(std::min(k - 3.0, 9.0 - k), -1.0, 1.0);
        return toByte(pure * (1.0 - w - b) + w);
    };
    return Color{channel(0.0), channel(8.0), channel(4.0), *alpha};
}

// color(srgb r g b) (CSS Color 4, section 10.1): channels from 0 to 1 or percentages, clamped to the gamut. Every
// other color space throws.
std::optional<Color> colorFunction(const std::vector<Token> &tokens, const Place &where) {
    if (tokens.empty() || tokens.front().kind != Token::Kind::identifier)
        return std::nullopt;
    if (tokens.front().text != "srgb")
        where.fail("color(" + tokens.front().text + " ...) is not supported; colors must be sRGB");
    const std::optional<ColorArguments> arguments =
        colorArguments(std::vector<Token>(tokens.begin() + 1, tokens.end()));
    if (!arguments || arguments->legacy)
        return std::nullopt;
    std::array<std::uint8_t, channelCount> channels{};
    for (std::size_t i = 0; i < channels.size(); ++i) {
        const Token &token = arguments->channels[i];
        if (isNone(token))
            channels[i] = 0;
        else if (token.kind == Token::Kind::number)
            channels[i] = toByte(token.value);
        else if (token.kind == Token::Kind::percentage)
            channels[i] = toByte(token.value / percent);
        else
            return std::nullopt;
    }
    const std::optional<std::uint8_t> alpha = alphaOf(arguments->alpha);
    if (!alpha)
        return std::nullopt;
    return Color{channels[0], channels[1], channels[2], *alpha};
}

// The color @p text writes in any CSS Color 4 syntax for an sRGB color, or currentColor, or nothing when it writes
// none. Throws for colors in other color spaces and for CSS functions that linea does not evaluate.
std::optional<ColorValue> parseColor(std::string_view text, const Place &where) {
    text = detail::trimmed(text);
    if (text.starts_with('#')) {
        const std::optional<Color> hex = fromHex(text);
        return hex ? std::optional<ColorValue>({false, *hex}) : std::nullopt;
    }
    const std::size_t open = text.find('(');
    if (open == std::string_view::npos) {
        const std::string name = lowered(text);
        if (name == "currentcolor")
            return ColorValue{true, {}};
        if (name == "transparent")
            return ColorValue{false, transparentBlack};
        if (const std::optional<Color> named = namedColor(name))
            return ColorValue{false, *named};
        return std::nullopt;
    }
    if (!text.ends_with(')'))
        return std::nullopt;
    const std::string function = lowered(text.substr(0, open));
    const std::optional<std::vector<Token>> tokens = tokenize(text.substr(open + 1, text.size() - open - 2));
    static constexpr std::array<std::string_view, 8> otherSpaces{
        "lab", "lch", "oklab", "oklch", "color-mix", "device-cmyk", "light-dark", "contrast-color"};
    if (std::ranges::find(otherSpaces, function) != otherSpaces.end())
        where.fail(function + "() colors are not supported; colors must be sRGB");
    if (function == "var" || function == "calc" || function == "env" || function == "attr")
        where.fail("CSS " + function + "() is not supported");
    if (!tokens)
        return std::nullopt;
    if (function == "color") {
        const std::optional<Color> c = colorFunction(*tokens, where);
        return c ? std::optional<ColorValue>({false, *c}) : std::nullopt;
    }
    if (!tokens->empty() && tokens->front().kind == Token::Kind::identifier && tokens->front().text == "from")
        where.fail("relative colors are not supported");
    const std::optional<ColorArguments> arguments = colorArguments(*tokens);
    if (!arguments)
        return std::nullopt;
    std::optional<Color> c;
    if (function == "rgb" || function == "rgba")
        c = rgbFunction(*arguments);
    else if (function == "hsl" || function == "hsla")
        c = hslFunction(*arguments);
    else if (function == "hwb")
        c = hwbFunction(*arguments);
    return c ? std::optional<ColorValue>({false, *c}) : std::nullopt;
}

// A paint as the fill and stroke properties give it (section 13.2): none, a color, or a reference to a paint server
// with an optional fallback for when the reference does not resolve.
struct PaintValue {
    // The color, or the fallback when there is a reference; nothing for none.
    std::optional<ColorValue> color;
    // The id of the paint server referred to, or empty for none.
    std::string reference;
};

// The id that a local url() reference names: url(#id), with the id optionally quoted. Throws for a reference to
// another document; nothing for text that is not a url().
std::optional<std::string> urlReference(std::string_view text, const Place &where) {
    text = detail::trimmed(text);
    if (!lowered(text.substr(0, 4)).starts_with("url(") || !text.ends_with(')'))
        return std::nullopt;
    std::string_view inside = detail::trimmed(text.substr(4, text.size() - 5));
    if (inside.size() >= 2 && (inside.front() == '"' || inside.front() == '\'') && inside.back() == inside.front())
        inside = inside.substr(1, inside.size() - 2);
    if (!inside.starts_with('#'))
        where.fail("references to other documents are not supported: url(" + std::string(inside) + ")");
    return std::string(inside.substr(1));
}

std::optional<PaintValue> parsePaint(std::string_view text, const Place &where) {
    text = detail::trimmed(text);
    const std::string word = lowered(text);
    if (word == "none")
        return PaintValue{};
    if (word == "context-fill" || word == "context-stroke")
        where.fail(word + " is not supported");
    if (word.starts_with("url(")) {
        const std::size_t close = text.find(')');
        if (close == std::string_view::npos)
            return std::nullopt;
        const std::optional<std::string> id = urlReference(text.substr(0, close + 1), where);
        if (!id)
            return std::nullopt;
        const std::string_view fallback = detail::trimmed(text.substr(close + 1));
        if (fallback.empty() || lowered(fallback) == "none")
            return PaintValue{std::nullopt, *id};
        const std::optional<ColorValue> color = parseColor(fallback, where);
        if (!color)
            return std::nullopt;
        return PaintValue{color, *id};
    }
    const std::optional<ColorValue> color = parseColor(text, where);
    if (!color)
        return std::nullopt;
    return PaintValue{color, {}};
}

// The computed values of the properties that linea reads, for one element.
struct Style {
    // Inherited properties.
    PaintValue fill{ColorValue{false, black}, {}};
    double fillOpacity = 1.0;
    FillRule fillRule = FillRule::nonZero;
    PaintValue stroke{};
    double strokeOpacity = 1.0;
    // The stroke properties, each nothing until the document sets it, so that a layer without a stroke keeps linea's
    // defaults for those it does not set.
    std::optional<double> strokeWidth;
    std::optional<Join> strokeLinejoin;
    std::optional<Cap> strokeLinecap;
    std::optional<double> strokeMiterlimit;
    std::optional<std::vector<double>> strokeDasharray;
    std::optional<double> strokeDashoffset;
    Color color = black;
    bool visible = true;
    // Whether color-interpolation is linearRGB, which a gradient blends its stops in.
    bool linearRgb = false;
    // Whether paint-order puts the stroke before the fill.
    bool strokeFirst = false;
    // Whether marker-start, marker-mid and marker-end are each other than none.
    std::array<bool, 3> markers{};

    // Properties that are not inherited.
    double opacity = 1.0;
    bool displayed = true;
    ColorValue stopColor{false, black};
    double stopOpacity = 1.0;
    // Whether vector-effect is other than none.
    bool vectorEffect = false;
    // Whether transform-origin is set, which moves the transform attribute's origin.
    bool transformOrigin = false;
    // The first property set to an effect that linea cannot draw, such as a clip path or a filter, or empty.
    std::string unsupported;
};

// What a property's value is read against.
struct ValueContext {
    const Place &where;
    const Viewport &viewport;
    const Style &parent;
};

using Copy = void (*)(Style &to, const Style &from);
using Parse = bool (*)(Style &style, std::string_view value, const ValueContext &context);

// A property that linea reads: whether it is inherited, whether an attribute of the same name sets it as a
// presentation attribute, how to copy its computed value, and how to read a value for it, which gives false for a
// value that is not one.
struct Property {
    std::string_view name;
    bool inherited = false;
    bool presentation = true;
    Copy copy{};
    Parse parse{};
};

std::optional<double> parseNumber(std::string_view text) {
    text = detail::trimmed(text);
    const std::optional<detail::ScannedNumber> number = detail::scanNumber(text, 0);
    if (!number || number->end != text.size() || !std::isfinite(number->value))
        return std::nullopt;
    return number->value;
}

// An alpha value as opacity and its kin take it: a number or a percentage, kept as written, beyond 0 and 1 too, since
// both SVG and linea clamp them where they use them.
std::optional<double> parseOpacity(std::string_view text) {
    text = detail::trimmed(text);
    if (text.ends_with('%')) {
        const std::optional<double> value = parseNumber(text.substr(0, text.size() - 1));
        return value ? std::optional<double>(*value / percent) : std::nullopt;
    }
    return parseNumber(text);
}

// The marker property's index for marker-start, marker-mid and marker-end.
constexpr std::size_t markerStart = 0;
constexpr std::size_t markerMid = 1;
constexpr std::size_t markerEnd = 2;

bool parseMarker(std::string_view value, bool &set, const Place &where) {
    if (lowered(detail::trimmed(value)) == "none") {
        set = false;
        return true;
    }
    if (!urlReference(value, where))
        return false;
    set = true;
    return true;
}

// Records @p name as an effect linea cannot draw unless @p value is @p neutral, the value that turns it off.
bool parseEffect(Style &style, std::string_view name, std::string_view value, std::string_view neutral) {
    if (lowered(detail::trimmed(value)) != neutral && style.unsupported.empty())
        style.unsupported = name;
    return true;
}

// Every property that linea reads, and those it refuses. Unknown properties are ignored, as CSS ignores them.
std::span<const Property> properties() {
    static const auto table = std::to_array<Property>({
        {"fill", true, true, [](Style &to, const Style &from) { to.fill = from.fill; },
         [](Style &style, std::string_view value, const ValueContext &context) {
             const std::optional<PaintValue> paint = parsePaint(value, context.where);
             if (paint)
                 style.fill = *paint;
             return paint.has_value();
         }},
        {"fill-opacity", true, true, [](Style &to, const Style &from) { to.fillOpacity = from.fillOpacity; },
         [](Style &style, std::string_view value, const ValueContext &) {
             const std::optional<double> opacity = parseOpacity(value);
             style.fillOpacity = opacity.value_or(style.fillOpacity);
             return opacity.has_value();
         }},
        {"fill-rule", true, true, [](Style &to, const Style &from) { to.fillRule = from.fillRule; },
         [](Style &style, std::string_view value, const ValueContext &) {
             const std::string rule = lowered(detail::trimmed(value));
             if (rule != "nonzero" && rule != "evenodd")
                 return false;
             style.fillRule = rule == "evenodd" ? FillRule::evenOdd : FillRule::nonZero;
             return true;
         }},
        {"stroke", true, true, [](Style &to, const Style &from) { to.stroke = from.stroke; },
         [](Style &style, std::string_view value, const ValueContext &context) {
             const std::optional<PaintValue> paint = parsePaint(value, context.where);
             if (paint)
                 style.stroke = *paint;
             return paint.has_value();
         }},
        {"stroke-opacity", true, true, [](Style &to, const Style &from) { to.strokeOpacity = from.strokeOpacity; },
         [](Style &style, std::string_view value, const ValueContext &) {
             const std::optional<double> opacity = parseOpacity(value);
             style.strokeOpacity = opacity.value_or(style.strokeOpacity);
             return opacity.has_value();
         }},
        {"stroke-width", true, true, [](Style &to, const Style &from) { to.strokeWidth = from.strokeWidth; },
         [](Style &style, std::string_view value, const ValueContext &context) {
             const std::optional<double> width = parseLength(value, Axis::other, context.viewport, context.where);
             if (!width || *width < 0.0)
                 return false;
             style.strokeWidth = width;
             return true;
         }},
        {"stroke-linejoin", true, true, [](Style &to, const Style &from) { to.strokeLinejoin = from.strokeLinejoin; },
         [](Style &style, std::string_view value, const ValueContext &context) {
             const std::string join = lowered(detail::trimmed(value));
             if (join == "miter-clip" || join == "arcs")
                 context.where.fail("stroke-linejoin " + join + " is not supported");
             if (join == "miter")
                 style.strokeLinejoin = Join::miter;
             else if (join == "round")
                 style.strokeLinejoin = Join::round;
             else if (join == "bevel")
                 style.strokeLinejoin = Join::bevel;
             else
                 return false;
             return true;
         }},
        {"stroke-linecap", true, true, [](Style &to, const Style &from) { to.strokeLinecap = from.strokeLinecap; },
         [](Style &style, std::string_view value, const ValueContext &) {
             const std::string cap = lowered(detail::trimmed(value));
             if (cap == "butt")
                 style.strokeLinecap = Cap::butt;
             else if (cap == "round")
                 style.strokeLinecap = Cap::round;
             else if (cap == "square")
                 style.strokeLinecap = Cap::square;
             else
                 return false;
             return true;
         }},
        {"stroke-miterlimit", true, true,
         [](Style &to, const Style &from) { to.strokeMiterlimit = from.strokeMiterlimit; },
         [](Style &style, std::string_view value, const ValueContext &) {
             const std::optional<double> limit = parseNumber(value);
             if (!limit || *limit < 1.0)
                 return false;
             style.strokeMiterlimit = limit;
             return true;
         }},
        {"stroke-dasharray", true, true,
         [](Style &to, const Style &from) { to.strokeDasharray = from.strokeDasharray; },
         [](Style &style, std::string_view value, const ValueContext &context) {
             if (lowered(detail::trimmed(value)) == "none") {
                 style.strokeDasharray = std::vector<double>{};
                 return true;
             }
             const std::optional<std::vector<std::string_view>> items = splitList(value);
             if (!items)
                 return false;
             std::vector<double> dashes;
             for (const std::string_view item : *items) {
                 const std::optional<double> dash = parseLength(item, Axis::other, context.viewport, context.where);
                 if (!dash || *dash < 0.0)
                     return false;
                 dashes.push_back(*dash);
             }
             style.strokeDasharray = std::move(dashes);
             return true;
         }},
        {"stroke-dashoffset", true, true,
         [](Style &to, const Style &from) { to.strokeDashoffset = from.strokeDashoffset; },
         [](Style &style, std::string_view value, const ValueContext &context) {
             const std::optional<double> offset = parseLength(value, Axis::other, context.viewport, context.where);
             if (offset)
                 style.strokeDashoffset = offset;
             return offset.has_value();
         }},
        {"color", true, true, [](Style &to, const Style &from) { to.color = from.color; },
         [](Style &style, std::string_view value, const ValueContext &context) {
             const std::optional<ColorValue> color = parseColor(value, context.where);
             if (color)
                 // currentColor as the color property's own value is its parent's color (CSS Color 4, section 6.4).
                 style.color = color->current ? context.parent.color : color->color;
             return color.has_value();
         }},
        {"color-interpolation", true, true, [](Style &to, const Style &from) { to.linearRgb = from.linearRgb; },
         [](Style &style, std::string_view value, const ValueContext &) {
             const std::string space = lowered(detail::trimmed(value));
             if (space != "auto" && space != "srgb" && space != "linearrgb")
                 return false;
             style.linearRgb = space == "linearrgb";
             return true;
         }},
        {"visibility", true, true, [](Style &to, const Style &from) { to.visible = from.visible; },
         [](Style &style, std::string_view value, const ValueContext &) {
             const std::string visibility = lowered(detail::trimmed(value));
             if (visibility != "visible" && visibility != "hidden" && visibility != "collapse")
                 return false;
             style.visible = visibility == "visible";
             return true;
         }},
        {"paint-order", true, true, [](Style &to, const Style &from) { to.strokeFirst = from.strokeFirst; },
         [](Style &style, std::string_view value, const ValueContext &) {
             // normal, or fill, stroke and markers each at most once; those left out follow in that order.
             const std::string order = lowered(detail::trimmed(value));
             if (order == "normal") {
                 style.strokeFirst = false;
                 return true;
             }
             const std::optional<std::vector<std::string_view>> items = splitList(order);
             if (!items || items->size() > 3)
                 return false;
             std::vector<std::string_view> seen;
             for (const std::string_view item : *items) {
                 if ((item != "fill" && item != "stroke" && item != "markers") ||
                     std::ranges::find(seen, item) != seen.end())
                     return false;
                 seen.push_back(item);
             }
             const auto at = [&](std::string_view item) {
                 const auto found = std::ranges::find(seen, item);
                 return found == seen.end() ? seen.size() + (item == "fill" ? 0 : 1)
                                            : static_cast<std::size_t>(found - seen.begin());
             };
             style.strokeFirst = at("stroke") < at("fill");
             return true;
         }},
        {"marker-start", true, true,
         [](Style &to, const Style &from) { to.markers[markerStart] = from.markers[markerStart]; },
         [](Style &style, std::string_view value, const ValueContext &context) {
             return parseMarker(value, style.markers[markerStart], context.where);
         }},
        {"marker-mid", true, true,
         [](Style &to, const Style &from) { to.markers[markerMid] = from.markers[markerMid]; },
         [](Style &style, std::string_view value, const ValueContext &context) {
             return parseMarker(value, style.markers[markerMid], context.where);
         }},
        {"marker-end", true, true,
         [](Style &to, const Style &from) { to.markers[markerEnd] = from.markers[markerEnd]; },
         [](Style &style, std::string_view value, const ValueContext &context) {
             return parseMarker(value, style.markers[markerEnd], context.where);
         }},
        {"marker", true, false, [](Style &to, const Style &from) { to.markers = from.markers; },
         [](Style &style, std::string_view value, const ValueContext &context) {
             bool set = false;
             if (!parseMarker(value, set, context.where))
                 return false;
             style.markers = {set, set, set};
             return true;
         }},
        {"opacity", false, true, [](Style &to, const Style &from) { to.opacity = from.opacity; },
         [](Style &style, std::string_view value, const ValueContext &) {
             const std::optional<double> opacity = parseOpacity(value);
             style.opacity = opacity.value_or(style.opacity);
             return opacity.has_value();
         }},
        {"display", false, true, [](Style &to, const Style &from) { to.displayed = from.displayed; },
         [](Style &style, std::string_view value, const ValueContext &) {
             // Every display value but none renders an SVG element.
             const std::optional<std::vector<std::string_view>> words = splitList(value);
             if (!words || !std::ranges::all_of(*words, [](std::string_view word) {
                     return std::ranges::all_of(word, isIdentifierCharacter);
                 }))
                 return false;
             style.displayed = lowered(detail::trimmed(value)) != "none";
             return true;
         }},
        {"stop-color", false, true, [](Style &to, const Style &from) { to.stopColor = from.stopColor; },
         [](Style &style, std::string_view value, const ValueContext &context) {
             const std::optional<ColorValue> color = parseColor(value, context.where);
             if (color)
                 style.stopColor = *color;
             return color.has_value();
         }},
        {"stop-opacity", false, true, [](Style &to, const Style &from) { to.stopOpacity = from.stopOpacity; },
         [](Style &style, std::string_view value, const ValueContext &) {
             const std::optional<double> opacity = parseOpacity(value);
             style.stopOpacity = opacity.value_or(style.stopOpacity);
             return opacity.has_value();
         }},
        {"vector-effect", false, true, [](Style &to, const Style &from) { to.vectorEffect = from.vectorEffect; },
         [](Style &style, std::string_view value, const ValueContext &) {
             const std::string effect = lowered(detail::trimmed(value));
             if (effect != "none" && effect != "non-scaling-stroke" && effect != "non-scaling-size" &&
                 effect != "non-rotation" && effect != "fixed-position")
                 return false;
             style.vectorEffect = effect != "none";
             return true;
         }},
        {"transform-origin", false, true,
         [](Style &to, const Style &from) { to.transformOrigin = from.transformOrigin; },
         [](Style &style, std::string_view, const ValueContext &) {
             style.transformOrigin = true;
             return true;
         }},
        {"clip-path", false, true, [](Style &to, const Style &from) { to.unsupported = from.unsupported; },
         [](Style &style, std::string_view value, const ValueContext &) {
             return parseEffect(style, "clip-path", value, "none");
         }},
        {"mask", false, true, [](Style &to, const Style &from) { to.unsupported = from.unsupported; },
         [](Style &style, std::string_view value, const ValueContext &) {
             return parseEffect(style, "mask", value, "none");
         }},
        {"mask-image", false, false, [](Style &to, const Style &from) { to.unsupported = from.unsupported; },
         [](Style &style, std::string_view value, const ValueContext &) {
             return parseEffect(style, "mask-image", value, "none");
         }},
        {"filter", false, true, [](Style &to, const Style &from) { to.unsupported = from.unsupported; },
         [](Style &style, std::string_view value, const ValueContext &) {
             return parseEffect(style, "filter", value, "none");
         }},
        {"mix-blend-mode", false, false, [](Style &to, const Style &from) { to.unsupported = from.unsupported; },
         [](Style &style, std::string_view value, const ValueContext &) {
             return parseEffect(style, "mix-blend-mode", value, "normal");
         }},
        {"clip", false, true, [](Style &to, const Style &from) { to.unsupported = from.unsupported; },
         [](Style &style, std::string_view value, const ValueContext &) {
             return parseEffect(style, "clip", value, "auto");
         }},
        {"backdrop-filter", false, false, [](Style &to, const Style &from) { to.unsupported = from.unsupported; },
         [](Style &style, std::string_view value, const ValueContext &) {
             return parseEffect(style, "backdrop-filter", value, "none");
         }},
    });
    return table;
}

const Property *propertyNamed(std::string_view name) {
    const auto &table = properties();
    const auto found = std::ranges::find(table, name, &Property::name);
    return found == table.end() ? nullptr : &*found;
}

// Properties that a browser reads from a style attribute and linea does not: CSS transforms and motion paths, and
// SVG 2's geometry properties, which linea reads only as attributes.
constexpr auto styleOnlyRefused =
    std::to_array<std::string_view>({"transform", "translate", "rotate", "scale", "transform-box", "offset",
                                     "offset-path", "d", "x", "y", "width", "height", "cx", "cy", "r", "rx", "ry"});

// A style attribute's declarations, as name and value, in order (CSS Style Attributes, and CSS Syntax 3). Comments are
// dropped, names are in lower case, and !important, which matters only against style sheets, is dropped.
std::vector<std::pair<std::string, std::string>> declarations(std::string_view text, const Place &where) {
    std::vector<std::pair<std::string, std::string>> result;
    std::string current;
    std::vector<std::string> pieces;
    char quote = 0;
    int depth = 0;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (quote != 0) {
            current += c;
            if (c == '\\' && i + 1 < text.size())
                current += text[++i];
            else if (c == quote)
                quote = 0;
            continue;
        }
        if (c == '/' && i + 1 < text.size() && text[i + 1] == '*') {
            const std::size_t end = text.find("*/", i + 2);
            i = end == std::string_view::npos ? text.size() : end + 1;
            current += ' ';
            continue;
        }
        if (c == '"' || c == '\'')
            quote = c;
        else if (c == '(')
            ++depth;
        else if (c == ')')
            --depth;
        if (c == ';' && depth == 0) {
            pieces.push_back(std::move(current));
            current.clear();
            continue;
        }
        current += c;
    }
    if (quote != 0 || depth != 0)
        where.fail("style: a string or a parenthesis is not closed");
    pieces.push_back(std::move(current));
    for (const std::string &piece : pieces) {
        const std::string_view declaration = detail::trimmed(piece);
        if (declaration.empty())
            continue;
        const std::size_t colon = declaration.find(':');
        if (colon == std::string_view::npos)
            where.fail("style: '" + std::string(declaration) + "' is not a declaration");
        std::string_view value = detail::trimmed(declaration.substr(colon + 1));
        if (const std::size_t bang = value.rfind('!');
            bang != std::string_view::npos && lowered(detail::trimmed(value.substr(bang + 1))) == "important")
            value = detail::trimmed(value.substr(0, bang));
        result.emplace_back(lowered(detail::trimmed(declaration.substr(0, colon))), std::string(value));
    }
    return result;
}

// Sets @p property on @p style from @p value, or throws for a value that is not one.
void apply(const Property &property, Style &style, std::string_view value, const ValueContext &context) {
    const std::string keyword = lowered(detail::trimmed(value));
    if (keyword.find("var(") != std::string::npos)
        context.where.fail(std::string(property.name) + ": CSS var() is not supported");
    if (keyword == "inherit" || (keyword == "unset" && property.inherited)) {
        property.copy(style, context.parent);
        return;
    }
    if (keyword == "initial" || keyword == "unset") {
        property.copy(style, Style{});
        return;
    }
    if (!property.parse(style, value, context))
        context.where.fail(std::string(property.name) + ": '" + std::string(detail::trimmed(value)) +
                           "' is not a value that linea reads");
}

// The stroke style of a layer: SVG's initial values for one that strokes, linea's defaults for one that does not, and
// the stroke properties that the document sets over either.
StrokeStyle strokeStyleOf(const Style &style, bool stroked) {
    StrokeStyle result = stroked ? detail::svgInitialStroke() : StrokeStyle{};
    if (style.strokeWidth)
        result.width = *style.strokeWidth;
    if (style.strokeLinejoin)
        result.join = *style.strokeLinejoin;
    if (style.strokeLinecap)
        result.cap = *style.strokeLinecap;
    if (style.strokeMiterlimit)
        result.miterLimit = *style.strokeMiterlimit;
    if (style.strokeDasharray)
        result.dashArray = *style.strokeDasharray;
    if (style.strokeDashoffset)
        result.dashOffset = *style.strokeDashoffset;
    return result;
}

bool isAnimation(std::string_view name) {
    return name == "animate" || name == "animateMotion" || name == "animateTransform" || name == "animateColor" ||
           name == "set" || name == "discard";
}

// Elements that SVG never draws where they stand, but only through a reference, if at all.
bool isNeverRendered(std::string_view name) {
    static constexpr std::array<std::string_view, 21> names{
        "defs",    "linearGradient", "radialGradient", "stop",  "clipPath",  "mask",     "marker",
        "pattern", "filter",         "symbol",         "title", "desc",      "metadata", "style",
        "script",  "view",           "cursor",         "font",  "font-face", "hatch",    "solidcolor"};
    return std::ranges::find(names, name) != names.end();
}

// Elements that SVG draws and linea cannot.
bool isUnsupportedGraphic(std::string_view name) {
    static constexpr std::array<std::string_view, 11> names{
        "text", "image", "foreignObject", "switch", "svg", "textPath", "tspan", "video", "audio", "iframe", "canvas"};
    return std::ranges::find(names, name) != names.end();
}

constexpr bool isMarkable(std::string_view name) {
    return name == "path" || name == "line" || name == "polyline" || name == "polygon";
}

// A gradient element's attributes and stops, resolved through the templates its href attributes name, ready for each
// element that paints with it.
struct GradientTemplate {
    bool radial = false;
    bool userSpace = false;
    SpreadMethod spread = SpreadMethod::pad;
    Affine transform{};
    // A linear gradient's ends, in the gradient's units.
    Vec2 start{};
    Vec2 end{};
    // A radial gradient's end circle and focal circle, in the gradient's units.
    Vec2 center{};
    double radius{};
    std::optional<Vec2> focal;
    double focalRadius{};
    std::vector<GradientStop> stops;
    Interpolation interpolation = Interpolation::srgb;
    // Whether the stops are the originals that toSvg() recorded, rather than those SVG draws.
    bool restored = false;
};

// A paint, and for a radial gradient stretched unevenly, the transform from the coordinates in which it is circular,
// in which it is given, to the element's.
struct ResolvedPaint {
    std::optional<Paint> paint;
    Affine stretch{};
};

// Whether @p m maps circles to circles: a turn, a reflection and an even scale, with any translation.
bool keepsCircles(const Affine &m) { return (m.a == m.d && m.b == -m.c) || (m.a == -m.d && m.b == m.c); }

// Whether the stops @p regenerated match those @p read from the document: the same offsets, and each channel within
// the one step that rounding on another platform may move it.
bool sameStops(const std::vector<GradientStop> &regenerated, const std::vector<GradientStop> &read) {
    return std::ranges::equal(regenerated, read, [](const GradientStop &x, const GradientStop &y) {
        const auto near = [](std::uint8_t first, std::uint8_t second) { return std::abs(first - second) <= 1; };
        return x.offset == y.offset && near(x.color.r, y.color.r) && near(x.color.g, y.color.g) &&
               near(x.color.b, y.color.b) && near(x.color.a, y.color.a);
    });
}

// The originals that toSvg() records in linea:stops, offset and color by turn, or nothing when the text is not that.
std::optional<std::vector<GradientStop>> originalStops(std::string_view text) {
    const std::optional<std::vector<std::string_view>> items = splitList(text);
    if (!items || items->size() % 2 != 0)
        return std::nullopt;
    std::vector<GradientStop> stops;
    for (std::size_t i = 0; i < items->size(); i += 2) {
        const std::optional<double> offset = parseNumber((*items)[i]);
        const std::optional<Color> color = fromHex((*items)[i + 1]);
        if (!offset || !color)
            return std::nullopt;
        stops.push_back({*offset, *color});
    }
    return stops;
}

std::optional<Interpolation> interpolationNamed(std::string_view name) {
    if (name == "oklab")
        return Interpolation::oklab;
    if (name == "srgb")
        return Interpolation::srgb;
    if (name == "srgb-linear")
        return Interpolation::srgbLinear;
    return std::nullopt;
}

// Reads a document's elements into nodes.
class Reader {
  public:
    explicit Reader(std::string_view text) : text_(text) {}

    SvgDocument read() {
        const XmlElement root = detail::parseXml(text_);
        if (root.space != XmlNamespace::svg || root.name != "svg")
            fail(root, "the root element must be svg in the SVG namespace, " + std::string(detail::svgNamespaceUri));
        survey(root);
        SvgDocument document;
        const Affine toViewport = sizeDrawing(root, document.drawing);
        const Style style = styleOf(root, Style{});
        refuseEffects(root, style);
        if (root.attribute("transform"))
            fail(root, "a transform on the root svg element is not supported");
        std::vector<Node> nodes;
        // A root that is not displayed draws nothing.
        if (style.displayed)
            readChildren(root, style, nodes, 1);
        if (toViewport == identity() && style.opacity == 1.0) {
            document.drawing.nodes = std::move(nodes);
        } else {
            Group content;
            content.children = std::move(nodes);
            content.transform = toViewport;
            content.opacity = style.opacity;
            document.drawing.nodes.emplace_back(std::move(content));
        }
        document.errors = std::move(errors_);
        return document;
    }

  private:
    Place place(const XmlElement &element) const { return {text_, element}; }

    [[noreturn]] void fail(const XmlElement &element, const std::string &problem) const {
        place(element).fail(problem);
    }

    // Records every element's id and parent, and refuses what changes how a document draws wherever it stands: style
    // sheets, scripts and animation.
    void survey(const XmlElement &element) {
        if (const std::string *id = element.attribute("id"))
            ids_.try_emplace(*id, &element);
        if (element.space == XmlNamespace::svg) {
            if ((element.name == "style" || element.name == "script") &&
                (element.hasText || element.attribute("href") || element.attribute("href", XmlNamespace::xlink)))
                fail(element, "style sheets and scripts are not supported");
            if (isAnimation(element.name))
                fail(element, "animation is not supported");
        }
        if (element.space == XmlNamespace::xhtml && (element.name == "style" || element.name == "link"))
            fail(element, "style sheets are not supported");
        for (const XmlElement &child : element.children) {
            parents_[&child] = &element;
            survey(child);
        }
    }

    // Sets @p drawing's size from the root's width, height and viewBox, and gives the transform that its
    // preserveAspectRatio makes of the viewBox (section 8.2). A width or height that is auto, or a percentage of a
    // viewport the document cannot know, comes from the viewBox, keeping its aspect ratio.
    Affine sizeDrawing(const XmlElement &root, Drawing &drawing) {
        std::optional<std::array<double, 4>> viewBox;
        if (const std::string *text = root.attribute("viewBox")) {
            const std::optional<std::vector<double>> numbers = parseNumbers(*text);
            if (!numbers || numbers->size() != 4 || (*numbers)[2] < 0.0 || (*numbers)[3] < 0.0)
                fail(root, "viewBox: '" + *text + "' is not four numbers with a width and height not negative");
            viewBox = std::array<double, 4>{(*numbers)[0], (*numbers)[1], (*numbers)[2], (*numbers)[3]};
        }
        const auto size = [&](std::string_view name) -> std::optional<double> {
            const std::string *text = root.attribute(name);
            if (!text || lowered(detail::trimmed(*text)) == "auto" || detail::trimmed(*text).ends_with('%'))
                return std::nullopt;
            const std::optional<double> length = parseLength(*text, Axis::other, {}, place(root));
            if (!length || *length < 0.0)
                fail(root, std::string(name) + ": '" + *text + "' is not a length that is not negative");
            return length;
        };
        std::optional<double> width = size("width");
        std::optional<double> height = size("height");
        if (!width || !height) {
            if (!viewBox)
                fail(root, "the svg element needs a width and a height, or a viewBox, to give the drawing a size");
            const double boxWidth = (*viewBox)[2];
            const double boxHeight = (*viewBox)[3];
            if (!width && !height) {
                width = boxWidth;
                height = boxHeight;
            } else if (!width) {
                width = boxHeight > 0.0 ? *height * boxWidth / boxHeight : 0.0;
            } else {
                height = boxWidth > 0.0 ? *width * boxHeight / boxWidth : 0.0;
            }
        }
        drawing.width = *width;
        drawing.height = *height;
        viewport_ = {*width, *height};
        if (!viewBox)
            return identity();
        const auto [x, y, boxWidth, boxHeight] = *viewBox;
        viewport_ = {boxWidth, boxHeight};
        // A viewBox without area disables rendering, as a drawing without area draws nothing.
        if (!(boxWidth > 0.0) || !(boxHeight > 0.0))
            return identity();
        double scaleX = *width / boxWidth;
        double scaleY = *height / boxHeight;
        std::string_view align = "xMidYMid";
        bool slice = false;
        if (const std::string *text = root.attribute("preserveAspectRatio")) {
            const std::optional<std::vector<std::string_view>> words = splitList(*text);
            std::size_t at = 0;
            if (words && !words->empty() && (*words)[0] == "defer")
                ++at;
            if (!words || at >= words->size() || words->size() > at + 2 ||
                (words->size() == at + 2 && (*words)[at + 1] != "meet" && (*words)[at + 1] != "slice"))
                fail(root, "preserveAspectRatio: '" + *text + "' is not a value that linea reads");
            align = (*words)[at];
            slice = words->size() == at + 2 && (*words)[at + 1] == "slice";
            static constexpr std::array<std::string_view, 10> aligns{"none",     "xMinYMin", "xMidYMin", "xMaxYMin",
                                                                     "xMinYMid", "xMidYMid", "xMaxYMid", "xMinYMax",
                                                                     "xMidYMax", "xMaxYMax"};
            if (std::ranges::find(aligns, align) == aligns.end())
                fail(root, "preserveAspectRatio: '" + *text + "' is not a value that linea reads");
        }
        double translateX = -x * scaleX;
        double translateY = -y * scaleY;
        if (align != "none") {
            scaleX = scaleY = slice ? std::max(scaleX, scaleY) : std::min(scaleX, scaleY);
            translateX = -x * scaleX;
            translateY = -y * scaleY;
            const double spareX = *width - boxWidth * scaleX;
            const double spareY = *height - boxHeight * scaleY;
            const std::string_view horizontal = align.substr(1, 3);
            const std::string_view vertical = align.substr(5, 3);
            translateX += horizontal == "Mid" ? spareX / 2.0 : horizontal == "Max" ? spareX : 0.0;
            translateY += vertical == "Mid" ? spareY / 2.0 : vertical == "Max" ? spareY : 0.0;
        }
        const Affine toViewport = translate({translateX, translateY}) * scale(scaleX, scaleY);
        if (!isFinite(toViewport))
            fail(root, "the viewBox scales its content past the largest double");
        return toViewport;
    }

    // @p element's computed style, inheriting from @p parent: its presentation attributes, then its style attribute's
    // declarations, which take precedence (section 6.6).
    Style styleOf(const XmlElement &element, const Style &parent) const {
        Style style = parent;
        static const Style initial{};
        for (const Property &property : properties())
            if (!property.inherited)
                property.copy(style, initial);
        const Place where = place(element);
        const ValueContext context{where, viewport_, parent};
        for (const detail::XmlAttribute &attribute : element.attributes) {
            if (attribute.space != XmlNamespace::none)
                continue;
            if (const Property *property = propertyNamed(attribute.name); property && property->presentation)
                apply(*property, style, attribute.value, context);
        }
        if (const std::string *text = element.attribute("style")) {
            for (const auto &[name, value] : declarations(*text, where)) {
                if (const Property *property = propertyNamed(name))
                    apply(*property, style, value, context);
                else if (std::ranges::find(styleOnlyRefused, name) != styleOnlyRefused.end() &&
                         style.unsupported.empty())
                    style.unsupported = "the " + name + " property in a style attribute";
            }
        }
        return style;
    }

    // Throws for the effects linea cannot draw that @p style sets on @p element.
    void refuseEffects(const XmlElement &element, const Style &style) const {
        if (!style.unsupported.empty())
            fail(element, style.unsupported + " is not supported");
        if (style.transformOrigin && element.attribute("transform"))
            fail(element, "transform-origin is not supported");
    }

    // @p element's transform attribute, or the identity without one.
    Affine transformOf(const XmlElement &element) const {
        const std::string *text = element.attribute("transform");
        if (!text)
            return identity();
        const std::optional<Affine> transform = parseTransform(*text);
        if (!transform)
            fail(element, "transform: '" + *text + "' is not a transform list that linea reads");
        return *transform;
    }

    // The length that @p element's attribute @p name gives, or @p fallback without one. Throws for a value that is not
    // a length, and for a negative one when @p allowNegative is false.
    double lengthOf(const XmlElement &element, std::string_view name, Axis axis, double fallback,
                    bool allowNegative = true) const {
        const std::string *text = element.attribute(name);
        if (!text)
            return fallback;
        const std::optional<double> length = parseLength(*text, axis, viewport_, place(element));
        if (!length || (!allowNegative && *length < 0.0))
            fail(element, std::string(name) + ": '" + *text + "' is not a length that linea reads" +
                              (allowNegative ? "" : " and is not negative"));
        return *length;
    }

    // A size that may be auto: nothing for auto or when absent.
    std::optional<double> autoLengthOf(const XmlElement &element, std::string_view name, Axis axis) const {
        const std::string *text = element.attribute(name);
        if (!text || lowered(detail::trimmed(*text)) == "auto")
            return std::nullopt;
        return lengthOf(element, name, axis, 0.0, false);
    }

    void checkDepth(const XmlElement &element, int depth) const {
        if (depth > deepestSvgNesting)
            fail(element, "elements nest more than " + std::to_string(deepestSvgNesting) +
                              " deep, counting those that use elements place");
    }

    // Counts @p bytes more of the drawing's memory against mostSvgDrawingBytes.
    void charge(const XmlElement &element, std::size_t bytes) {
        bytes_ += bytes;
        if (bytes_ > mostSvgDrawingBytes)
            fail(element, "the drawing would take more than " + std::to_string(mostSvgDrawingBytes) +
                              " bytes, counting the copies that use elements and gradient references make");
    }

    void readChildren(const XmlElement &parent, const Style &style, std::vector<Node> &out, int depth) {
        for (const XmlElement &child : parent.children)
            readNode(child, style, out, depth + 1);
    }

    // Appends the node that @p element draws, if it draws one, to @p out.
    void readNode(const XmlElement &element, const Style &parentStyle, std::vector<Node> &out, int depth) {
        // Elements in other namespaces are not SVG's to draw, and SVG ignores them.
        if (element.space != XmlNamespace::svg)
            return;
        checkDepth(element, depth);
        const std::string &name = element.name;
        if (isNeverRendered(name))
            return;
        if (isUnsupportedGraphic(name))
            fail(element, name == "svg" ? "nested svg elements are not supported" : name + " is not supported");
        const bool group = name == "g" || name == "a";
        const bool use = name == "use";
        if (!group && !use && !isShape(name))
            fail(element, "unknown element " + name);
        const Style style = styleOf(element, parentStyle);
        if (!style.displayed)
            return;
        refuseEffects(element, style);
        if (group || use) {
            Group node;
            node.transform = transformOf(element);
            node.opacity = style.opacity;
            if (use)
                readUse(element, style, node, depth);
            else
                readChildren(element, style, node.children, depth);
            charge(element, groupBytes);
            out.emplace_back(std::move(node));
            return;
        }
        if (!style.visible)
            return;
        readShape(element, style, out);
    }

    static bool isShape(std::string_view name) {
        return name == "path" || name == "rect" || name == "circle" || name == "ellipse" || name == "line" ||
               name == "polyline" || name == "polygon";
    }

    // A use element draws as a group, moved by its x and y after its transform, around a copy of the element it refers
    // to, which inherits from the use element rather than from where it stands (section 5.5.3).
    void readUse(const XmlElement &element, const Style &style, Group &node, int depth) {
        const std::string *href = element.attribute("href");
        if (!href)
            href = element.attribute("href", XmlNamespace::xlink);
        if (!href)
            return;
        const std::string_view reference = detail::trimmed(*href);
        if (!reference.starts_with('#'))
            fail(element, "references to other documents are not supported: " + std::string(reference));
        const auto found = ids_.find(std::string(reference.substr(1)));
        node.transform =
            node.transform * translate({lengthOf(element, "x", Axis::x, 0.0), lengthOf(element, "y", Axis::y, 0.0)});
        if (!isFinite(node.transform))
            fail(element, "x and y move the transform past the largest double");
        if (found == ids_.end())
            return;
        const XmlElement &target = *found->second;
        if (target.space == XmlNamespace::svg && target.name == "symbol")
            fail(element, "use of a symbol is not supported");
        if (std::ranges::find(expanding_, &target) != expanding_.end() || &target == &element)
            fail(element, "use refers to an element that contains it");
        expanding_.push_back(&target);
        readNode(target, style, node.children, depth + 1);
        expanding_.pop_back();
    }

    // Appends the layer that the shape @p element draws.
    void readShape(const XmlElement &element, const Style &style, std::vector<Node> &out) {
        Layer layer;
        layer.path = shapePath(element);
        if (!isFinite(layer.path))
            fail(element, "the shape reaches past the largest double");
        layer.transform = transformOf(element);
        layer.opacity = style.opacity;
        layer.fillRule = style.fillRule;
        const ResolvedPaint fill = paintOf(element, style.fill, style.fillOpacity, style, layer.path);
        const ResolvedPaint stroke = paintOf(element, style.stroke, style.strokeOpacity, style, layer.path);
        if (stroke.stretch != identity())
            fail(element, "a stroke painted with a radial gradient stretched unevenly, by a gradientTransform or by "
                          "objectBoundingBox units on a box that is not square, is not supported");
        layer.fill = fill.paint;
        layer.stroke = stroke.paint;
        layer.strokeStyle = strokeStyleOf(style, layer.stroke.has_value());
        if (isMarkable(element.name) && std::ranges::any_of(style.markers, [](bool set) { return set; }))
            fail(element, "markers are not supported");
        if (style.strokeFirst && layer.fill && layer.stroke)
            fail(element, "paint-order with the stroke before the fill is not supported");
        if (style.vectorEffect && layer.stroke)
            fail(element, "vector-effect is not supported");
        if (fill.stretch == identity()) {
            charge(element, layerBytes(layer));
            out.emplace_back(std::move(layer));
            return;
        }
        // A radial gradient stretched into an ellipse is circular in its own coordinates, so the fill draws as a layer
        // of its own in them: its path moved into them, and its transform moving them back out. The stroke, whose width
        // a stretch would distort, stays a layer in the element's coordinates, and a group fades the two together.
        Layer filled = layer;
        filled.path = transformed(layer.path, inverse(fill.stretch));
        if (!isFinite(filled.path))
            fail(element, "the gradient's stretch takes the shape past the largest double");
        filled.transform = layer.transform * fill.stretch;
        filled.stroke = std::nullopt;
        filled.strokeStyle = StrokeStyle{};
        if (!layer.stroke) {
            charge(element, layerBytes(filled));
            out.emplace_back(std::move(filled));
            return;
        }
        Layer stroked = std::move(layer);
        stroked.fill = std::nullopt;
        filled.opacity = 1.0;
        Group together{{}, identity(), stroked.opacity};
        stroked.opacity = 1.0;
        charge(element, layerBytes(filled) + layerBytes(stroked) + groupBytes);
        together.children.emplace_back(std::move(filled));
        together.children.emplace_back(std::move(stroked));
        if (together.opacity == 1.0) {
            for (Node &node : together.children)
                out.push_back(std::move(node));
        } else {
            out.emplace_back(std::move(together));
        }
    }

    static std::size_t paintBytes(const std::optional<Paint> &paint) {
        if (!paint)
            return 0;
        if (const auto *linear = std::get_if<LinearGradient>(&*paint))
            return linear->stops.size() * sizeof(GradientStop);
        if (const auto *radial = std::get_if<RadialGradient>(&*paint))
            return radial->stops.size() * sizeof(GradientStop);
        return 0;
    }

    static std::size_t layerBytes(const Layer &layer) {
        return sizeof(Node) + layer.path.verbs().size() * sizeof(Verb) + layer.path.points().size() * sizeof(Vec2) +
               paintBytes(layer.fill) + paintBytes(layer.stroke) + layer.strokeStyle.dashArray.size() * sizeof(double);
    }

    // The paint that @p value gives @p element, whose path is @p path, its alpha scaled by @p opacity. A reference to
    // a gradient gives the gradient, and one that does not resolve gives the fallback, or none (section 13.2).
    ResolvedPaint paintOf(const XmlElement &element, const PaintValue &value, double opacity, const Style &style,
                          const Path &path) {
        if (!value.reference.empty()) {
            const auto found = ids_.find(value.reference);
            if (found != ids_.end() && found->second->space == XmlNamespace::svg) {
                const std::string &target = found->second->name;
                if (target == "pattern")
                    fail(element, "pattern paints are not supported");
                if (target == "linearGradient" || target == "radialGradient")
                    return gradientPaint(*found->second, opacity, path);
            }
        }
        if (!value.color)
            return {};
        Color color = value.color->current ? style.color : value.color->color;
        color.a = detail::scaledAlpha(color.a, opacity);
        return {Paint{color}, identity()};
    }

    // The paint that the gradient element @p gradient gives an element whose path is @p path. Its units and
    // gradientTransform map it into the element's coordinates, where a linear gradient stays linear and a radial one
    // stays circular under a turn, a reflection and an even scale. A gradient whose units leave it no area, or whose
    // transform has no inverse, paints nothing. SVG paints a linear gradient whose ends coincide its last stop's color.
    ResolvedPaint gradientPaint(const XmlElement &gradient, double opacity, const Path &path) {
        const GradientTemplate &source = gradientTemplate(gradient);
        Affine toElement = source.transform;
        if (!source.userSpace) {
            const Bounds box = tightBounds(path);
            const Vec2 size = box.max - box.min;
            if (!(size.x > 0.0) || !(size.y > 0.0))
                return {};
            toElement = translate(box.min) * scale(size.x, size.y) * toElement;
        }
        const double det = determinant(toElement);
        if (det == 0.0 || !std::isfinite(det))
            return {};
        std::vector<GradientStop> stops = source.stops;
        for (GradientStop &stop : stops)
            stop.color.a = detail::scaledAlpha(stop.color.a, opacity);
        if (!source.radial) {
            if (source.start == source.end && !source.restored) {
                if (stops.empty())
                    return {};
                return {Paint{stops.back().color}, identity()};
            }
            Vec2 from = source.start;
            Vec2 to = source.end;
            if (toElement != identity() && source.start == source.end) {
                from = to = apply(toElement, source.start);
            } else if (toElement != identity()) {
                // An offset is an affine function of the point, which keeps a gradient linear under any affine
                // transform: it rises along the direction that the inverse's transpose makes of the gradient's own.
                const Vec2 along = source.end - source.start;
                const Affine back = inverse(toElement);
                const Vec2 rise =
                    Vec2{back.a * along.x + back.b * along.y, back.c * along.x + back.d * along.y} / dot(along, along);
                from = apply(toElement, source.start);
                to = from + rise / dot(rise, rise);
            }
            if (!isFinite(from) || !isFinite(to))
                fail(gradient, "the gradient's transform takes it past the largest double");
            return {Paint{LinearGradient{from, to, std::move(stops), source.spread, source.interpolation}}, identity()};
        }
        RadialGradient radial{source.center,      source.radius, std::move(stops),    source.focal,
                              source.focalRadius, source.spread, source.interpolation};
        if (toElement == identity())
            return {Paint{std::move(radial)}, identity()};
        if (!keepsCircles(toElement))
            return {Paint{std::move(radial)}, toElement};
        const double stretch = std::hypot(toElement.a, toElement.b);
        radial.center = apply(toElement, radial.center);
        radial.radius *= stretch;
        if (radial.focal)
            radial.focal = apply(toElement, *radial.focal);
        radial.focalRadius *= stretch;
        if (!isFinite(radial.center) || !std::isfinite(radial.radius) || !isFinite(radial.focal.value_or(Vec2{})) ||
            !std::isfinite(radial.focalRadius))
            fail(gradient, "the gradient's transform takes it past the largest double");
        return {Paint{std::move(radial)}, identity()};
    }

    // The computed style of @p element where it stands, inheriting from its ancestors, as paint servers and their
    // stops inherit (section 14.1).
    const Style &standingStyle(const XmlElement &element) {
        if (const auto found = standingStyles_.find(&element); found != standingStyles_.end())
            return found->second;
        const auto parent = parents_.find(&element);
        const Style inherited = parent == parents_.end() ? Style{} : standingStyle(*parent->second);
        return standingStyles_.emplace(&element, styleOf(element, inherited)).first->second;
    }

    // The gradient element @p gradient with what its href templates lend it (section 14.1.1): each attribute from the
    // first element of the chain that has it, those of one kind of gradient only from that kind, and the stops of the
    // first element that has content.
    const GradientTemplate &gradientTemplate(const XmlElement &gradient) {
        if (const auto found = gradients_.find(&gradient); found != gradients_.end())
            return found->second;
        if (!standingStyle(gradient).unsupported.empty())
            fail(gradient, standingStyle(gradient).unsupported + " is not supported");
        std::vector<const XmlElement *> chain{&gradient};
        while (true) {
            const XmlElement &last = *chain.back();
            const std::string *href = last.attribute("href");
            if (!href)
                href = last.attribute("href", XmlNamespace::xlink);
            if (!href)
                break;
            const std::string_view reference = detail::trimmed(*href);
            if (!reference.starts_with('#'))
                fail(last, "references to other documents are not supported: " + std::string(reference));
            const auto found = ids_.find(std::string(reference.substr(1)));
            if (found == ids_.end() || found->second->space != XmlNamespace::svg ||
                (found->second->name != "linearGradient" && found->second->name != "radialGradient"))
                break;
            if (std::ranges::find(chain, found->second) != chain.end())
                fail(last, "gradient templates refer to one another in a cycle");
            if (chain.size() >= static_cast<std::size_t>(deepestSvgNesting))
                fail(last, "gradient templates chain more than " + std::to_string(deepestSvgNesting) + " deep");
            chain.push_back(found->second);
        }
        // The value of the attribute @p name, and its element, from the first element of the chain that has it, of
        // the kind @p kind when given.
        const auto lookUp = [&](std::string_view name,
                                std::string_view kind = {}) -> std::pair<const std::string *, const XmlElement *> {
            for (const XmlElement *element : chain)
                if (kind.empty() || element->name == kind)
                    if (const std::string *value = element->attribute(name))
                        return {value, element};
            return {nullptr, nullptr};
        };
        GradientTemplate result;
        result.radial = gradient.name == "radialGradient";
        if (const auto [units, element] = lookUp("gradientUnits"); units) {
            if (*units != "userSpaceOnUse" && *units != "objectBoundingBox")
                fail(*element, "gradientUnits: '" + *units + "' is not userSpaceOnUse or objectBoundingBox");
            result.userSpace = *units == "userSpaceOnUse";
        }
        if (const auto [spread, element] = lookUp("spreadMethod"); spread) {
            if (*spread == "reflect")
                result.spread = SpreadMethod::reflect;
            else if (*spread == "repeat")
                result.spread = SpreadMethod::repeat;
            else if (*spread != "pad")
                fail(*element, "spreadMethod: '" + *spread + "' is not pad, reflect or repeat");
        }
        if (const auto [transform, element] = lookUp("gradientTransform"); transform) {
            const std::optional<Affine> parsed = parseTransform(*transform);
            if (!parsed)
                fail(*element, "gradientTransform: '" + *transform + "' is not a transform list that linea reads");
            result.transform = *parsed;
        }
        // Lengths in objectBoundingBox units are fractions of the box, and percentages percentages of it.
        const Viewport units = result.userSpace ? viewport_ : Viewport{1.0, 1.0};
        const auto length = [&](std::string_view name, std::string_view kind, Axis axis, std::string_view fallback,
                                bool allowNegative = true) {
            const auto [text, element] = lookUp(name, kind);
            const std::string_view written = text ? std::string_view(*text) : fallback;
            const XmlElement &at = element ? *element : gradient;
            const std::optional<double> value = parseLength(written, axis, units, place(at));
            if (!value || (!allowNegative && *value < 0.0))
                fail(at, std::string(name) + ": '" + std::string(written) + "' is not a length that linea reads" +
                             (allowNegative ? "" : " and is not negative"));
            return *value;
        };
        if (result.radial) {
            result.center = {length("cx", "radialGradient", Axis::x, "50%"),
                             length("cy", "radialGradient", Axis::y, "50%")};
            result.radius = length("r", "radialGradient", Axis::other, "50%", false);
            // The focal point defaults to the center (section 14.2.3.1).
            const bool fx = lookUp("fx", "radialGradient").first != nullptr;
            const bool fy = lookUp("fy", "radialGradient").first != nullptr;
            if (fx || fy)
                result.focal = Vec2{fx ? length("fx", "radialGradient", Axis::x, "0") : result.center.x,
                                    fy ? length("fy", "radialGradient", Axis::y, "0") : result.center.y};
            result.focalRadius = length("fr", "radialGradient", Axis::other, "0%", false);
        } else {
            result.start = {length("x1", "linearGradient", Axis::x, "0%"),
                            length("y1", "linearGradient", Axis::y, "0%")};
            result.end = {length("x2", "linearGradient", Axis::x, "100%"),
                          length("y2", "linearGradient", Axis::y, "0%")};
        }
        // A gradient blends its stops in the color-interpolation of its own element, as Chrome does, whichever element
        // of the chain lends the stops.
        readStops(chain, standingStyle(gradient).linearRgb ? Interpolation::srgbLinear : Interpolation::srgb, result);
        return gradients_.emplace(&gradient, std::move(result)).first->second;
    }

    // Reads the stops of the first element of @p chain with content, as SVG fixes their offsets up: each clamped to 0
    // to 1 and raised to the largest before it (section 14.2.4.3), for a blend in @p space. Where the element holds the
    // originals that toSvg() recorded, and they regenerate these stops for the sRGB blend toSvg() wrote them for, they
    // are restored; otherwise the stops become linea's stops for the ramp SVG draws in @p space.
    void readStops(const std::vector<const XmlElement *> &chain, Interpolation space, GradientTemplate &result) {
        const auto hasContent = [](const XmlElement *element) {
            return std::ranges::any_of(element->children, [](const XmlElement &child) {
                return child.space == XmlNamespace::svg && child.name != "title" && child.name != "desc" &&
                       child.name != "metadata";
            });
        };
        // Without content anywhere in the chain, the gradient has no stops, and its own element holds any originals.
        const auto source = std::ranges::find_if(chain, hasContent);
        const XmlElement &owner = source == chain.end() ? *chain.front() : **source;
        const Style &ownerStyle = standingStyle(owner);
        std::vector<GradientStop> stops;
        double largest = 0.0;
        for (const XmlElement &child : owner.children) {
            if (child.space != XmlNamespace::svg || child.name != "stop")
                continue;
            double offset = 0.0;
            if (const std::string *text = child.attribute("offset")) {
                const std::optional<double> value = parseOpacity(*text);
                if (!value)
                    fail(child, "offset: '" + *text + "' is not a number or a percentage");
                offset = std::clamp(*value, 0.0, 1.0);
            }
            largest = std::max(largest, offset);
            const Style style = styleOf(child, ownerStyle);
            Color color = style.stopColor.current ? style.color : style.stopColor.color;
            color.a = detail::scaledAlpha(color.a, style.stopOpacity);
            stops.push_back({largest, color});
        }
        const std::string *originals = owner.attribute("stops", XmlNamespace::linea);
        const std::string *interpolation = owner.attribute("interpolation", XmlNamespace::linea);
        if (originals && interpolation && space == Interpolation::srgb) {
            const std::optional<std::vector<GradientStop>> original = originalStops(*originals);
            const std::optional<Interpolation> recorded = interpolationNamed(*interpolation);
            const bool degenerate = !result.radial && result.start == result.end;
            if (original && recorded &&
                std::ranges::all_of(*original, [](const GradientStop &stop) { return std::isfinite(stop.offset); }) &&
                sameStops(detail::svgStops(*original, *recorded, degenerate), stops)) {
                result.stops = *original;
                result.interpolation = *recorded;
                result.restored = true;
                return;
            }
        }
        result.stops = detail::lineaStops(stops, space);
        result.interpolation = space;
    }

    // The path that the shape @p element describes, as section 10 specifies each one's equivalent path. Errors in path
    // data and points are recorded, and the path drawn up to them.
    Path shapePath(const XmlElement &element) {
        const std::string &name = element.name;
        Path path;
        if (name == "path") {
            const std::string *d = element.attribute("d");
            if (!d || lowered(detail::trimmed(*d)) == "none")
                return path;
            detail::PathDataReading reading = detail::readPathData(*d);
            if (reading.errorAt)
                errors_.push_back(place(element).error("d: " + reading.error + ", at character " +
                                                       std::to_string(*reading.errorAt + 1) +
                                                       "; the path is drawn up to there"));
            return std::move(reading.path);
        }
        if (name == "rect")
            return rectPath(element);
        if (name == "circle" || name == "ellipse") {
            const double cx = lengthOf(element, "cx", Axis::x, 0.0);
            const double cy = lengthOf(element, "cy", Axis::y, 0.0);
            std::optional<double> rx;
            std::optional<double> ry;
            if (name == "circle") {
                rx = ry = lengthOf(element, "r", Axis::other, 0.0, false);
            } else {
                // A radius that is auto takes the other's value (section 10.4).
                rx = autoLengthOf(element, "rx", Axis::x);
                ry = autoLengthOf(element, "ry", Axis::y);
                if (!rx)
                    rx = ry;
                if (!ry)
                    ry = rx;
            }
            if (!rx || !ry || !(*rx > 0.0) || !(*ry > 0.0))
                return path;
            const Vec2 radii{*rx, *ry};
            path.moveTo({cx + *rx, cy})
                .arcTo(radii, 0.0, false, true, {cx, cy + *ry})
                .arcTo(radii, 0.0, false, true, {cx - *rx, cy})
                .arcTo(radii, 0.0, false, true, {cx, cy - *ry})
                .arcTo(radii, 0.0, false, true, {cx + *rx, cy})
                .close();
            return path;
        }
        if (name == "line") {
            path.moveTo({lengthOf(element, "x1", Axis::x, 0.0), lengthOf(element, "y1", Axis::y, 0.0)})
                .lineTo({lengthOf(element, "x2", Axis::x, 0.0), lengthOf(element, "y2", Axis::y, 0.0)});
            return path;
        }
        return pointsPath(element);
    }

    // A rect's path (section 10.2): from the end of the top left corner's curve, clockwise, with each corner rounded
    // by rx and ry, of which one that is auto takes the other's value, each at most half the side it lies along.
    Path rectPath(const XmlElement &element) const {
        const double x = lengthOf(element, "x", Axis::x, 0.0);
        const double y = lengthOf(element, "y", Axis::y, 0.0);
        const double width = autoLengthOf(element, "width", Axis::x).value_or(0.0);
        const double height = autoLengthOf(element, "height", Axis::y).value_or(0.0);
        std::optional<double> rx = autoLengthOf(element, "rx", Axis::x);
        std::optional<double> ry = autoLengthOf(element, "ry", Axis::y);
        if (!rx)
            rx = ry.value_or(0.0);
        if (!ry)
            ry = rx;
        Path path;
        if (!(width > 0.0) || !(height > 0.0))
            return path;
        double cornerX = std::min(*rx, width / 2.0);
        double cornerY = std::min(*ry, height / 2.0);
        if (!(cornerX > 0.0) || !(cornerY > 0.0))
            cornerX = cornerY = 0.0;
        const Vec2 radii{cornerX, cornerY};
        const auto corner = [&](Vec2 to) {
            if (cornerX > 0.0)
                path.arcTo(radii, 0.0, false, true, to);
        };
        // Sides of no length, where a corner's curve takes the whole side, are left out.
        const auto side = [&](Vec2 to) {
            if (to != path.current())
                path.lineTo(to);
        };
        path.moveTo({x + cornerX, y});
        side({x + width - cornerX, y});
        corner({x + width, y + cornerY});
        side({x + width, y + height - cornerY});
        corner({x + width - cornerX, y + height});
        side({x + cornerX, y + height});
        corner({x, y + height - cornerY});
        side({x, y + cornerY});
        corner({x + cornerX, y});
        path.close();
        return path;
    }

    // A polyline's or polygon's path through its points (section 10.6), a polygon's closed. An odd number of
    // coordinates, or one that is not a number, is an error that SVG draws past, up to the last whole pair.
    Path pointsPath(const XmlElement &element) {
        Path path;
        const std::string *text = element.attribute("points");
        if (!text)
            return path;
        std::vector<double> numbers;
        std::size_t i = 0;
        const std::string_view points = *text;
        bool failed = false;
        const auto skipSpace = [&] {
            while (i < points.size() && detail::isSvgSpace(points[i]))
                ++i;
        };
        skipSpace();
        while (i < points.size()) {
            const std::optional<detail::ScannedNumber> number = detail::scanNumber(points, i);
            if (!number || !std::isfinite(number->value)) {
                failed = true;
                break;
            }
            numbers.push_back(number->value);
            i = number->end;
            skipSpace();
            if (i < points.size() && points[i] == ',') {
                ++i;
                skipSpace();
                if (i == points.size())
                    failed = true;
            }
        }
        if (failed || numbers.size() % 2 == 1)
            errors_.push_back(place(element).error("points: " +
                                                   std::string(failed ? "expected a number"
                                                                      : "an odd number of "
                                                                        "coordinates") +
                                                   ", at character " + std::to_string(i + 1) +
                                                   "; the shape is drawn up to its last whole pair"));
        for (std::size_t k = 0; k + 1 < numbers.size(); k += 2) {
            if (k == 0)
                path.moveTo({numbers[k], numbers[k + 1]});
            else
                path.lineTo({numbers[k], numbers[k + 1]});
        }
        if (element.name == "polygon" && !path.empty())
            path.close();
        return path;
    }

    std::string_view text_;
    Viewport viewport_{};
    std::unordered_map<std::string, const XmlElement *> ids_;
    std::unordered_map<const XmlElement *, const XmlElement *> parents_;
    // The elements that use elements are placing copies of, outermost first, to find use elements that refer to
    // their own ancestors.
    std::vector<const XmlElement *> expanding_;
    std::unordered_map<const XmlElement *, Style> standingStyles_;
    std::unordered_map<const XmlElement *, GradientTemplate> gradients_;
    std::size_t bytes_ = 0;
    std::vector<SvgError> errors_;
};

} // namespace

SvgDocument fromSvg(std::string_view text) { return Reader(text).read(); }

} // namespace linea
