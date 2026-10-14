// linea - Color implementation: the sRGB transfer function, hex notation, HSL, Oklab and Oklch, and adjustments in
// Oklab.
#include "linea/Color.hpp"

#include "ColorSpace.hpp"
#include "Roots.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <functional>
#include <numbers>
#include <numeric>
#include <optional>
#include <vector>

namespace linea {
namespace {

// The sRGB transfer function's constants, IEC 61966-2-1.
constexpr double linearLimit = 0.04045;
constexpr double encodedLimit = 0.0031308;
constexpr double linearSlope = 12.92;
constexpr double curveOffset = 0.055;
constexpr double curveGamma = 2.4;
constexpr double byteMax = 255.0;
// A byte at its most: opaque alpha, or a channel at full strength.
constexpr std::uint8_t full = 255;

constexpr double fullTurnDegrees = 360.0;
constexpr double degreesPerRadian = 180.0 / std::numbers::pi;
// HSL measures hue from rgb in sixths of a turn, and CSS Color 4 turns it back into rgb in twelfths.
constexpr double degreesPerSextant = fullTurnDegrees / 6.0;
constexpr double twelfthsPerTurn = 12.0;
constexpr double degreesPerTwelfth = fullTurnDegrees / twelfthsPerTurn;

// Chroma at or below this is rounding noise on a grey, whose hue means nothing, as CSS Color 4's sample conversion
// code treats it. The least chromatic 8-bit color that is not a grey, #feffff, has chroma 0.00106.
constexpr double greyChroma = 0.000004;
// Gamut mapping finds the share of a color's chroma that fits to within a billionth, far finer than a byte can show.
constexpr double chromaShareTolerance = 1e-9;
// A channel fits when it rounds to a byte without clipping: encoded, it lies within half an 8-bit step of 0 to 1.
constexpr double halfStep = 0.5 / byteMax;

// Linear sRGB to and from Oklab, as Bjorn Ottosson defined it (2020), with the matrices at the double precision CSS
// Color 4 publishes, so that white is L = 1 and a = b = 0 to within rounding.
using Vector3 = std::array<double, 3>;
using Matrix3 = std::array<Vector3, 3>;

// Linear sRGB to the cone responses Oklab starts from: CSS Color 4's XYZ to LMS matrix times its exact sRGB to XYZ
// matrix.
constexpr Matrix3 srgbToLms{{{0.412221469470763, 0.5363325372617348, 0.0514459932675022},
                             {0.21190349581782522, 0.6806995506452344, 0.10739695353694055},
                             {0.08830245919005643, 0.2817188391361215, 0.6299787016738221}}};
// The cube roots of the cone responses to Oklab.
constexpr Matrix3 lmsToOklab{{{0.2104542683093140, 0.7936177747023054, -0.0040720430116193},
                              {1.9779985324311684, -2.4285922420485799, 0.4505937096174110},
                              {0.0259040424655478, 0.7827717124575296, -0.8086757549230774}}};
// The inverse of lmsToOklab.
constexpr Matrix3 oklabToLms{{{1.0000000000000000, 0.3963377773761749, 0.2158037573099136},
                              {1.0000000000000000, -0.1055613458156586, -0.0638541728258133},
                              {1.0000000000000000, -0.0894841775298119, -1.2914855480194092}}};
// The inverse of srgbToLms.
constexpr Matrix3 lmsToSrgb{{{4.076741636075958, -3.3077115392580625, 0.23096990318210447},
                             {-1.268437973285032, 2.609757349287689, -0.3413193760026573},
                             {-0.00419607613867548, -0.7034186179359363, 1.7076146940746117}}};

constexpr Vector3 multiply(const Matrix3 &m, const Vector3 &v) {
    return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
            m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
}

std::array<double, 256> decodeTable() {
    std::array<double, 256> table{};
    for (std::size_t i = 0; i < table.size(); ++i) {
        const double v = static_cast<double>(i) / byteMax;
        table[i] = v <= linearLimit ? v / linearSlope : std::pow((v + curveOffset) / (1.0 + curveOffset), curveGamma);
    }
    return table;
}

// @p linear encoded by the sRGB transfer function, which continues its straight segment below 0 and its curve above 1.
double encodeSrgb(double linear) {
    return linear <= encodedLimit ? linear * linearSlope
                                  : (1.0 + curveOffset) * std::pow(linear, 1.0 / curveGamma) - curveOffset;
}

// @p value, from 0 to 1, as the nearest byte, clamped.
std::uint8_t toByte(double value) {
    return static_cast<std::uint8_t>(std::lround(std::clamp(value, 0.0, 1.0) * byteMax));
}

// @p degrees moved onto [0, 360) by whole turns.
double wrapDegrees(double degrees) {
    return std::fmod(std::fmod(degrees, fullTurnDegrees) + fullTurnDegrees, fullTurnDegrees);
}

// Where the linear-light channel @p linear lies against the values that fit: -1 below them, 1 above them and 0 among
// them.
int placement(double linear) {
    const double encoded = encodeSrgb(linear);
    if (encoded <= -halfStep)
        return -1;
    if (encoded >= 1.0 + halfStep)
        return 1;
    return 0;
}

using Channels = std::array<double, 3>;

bool fits(const Channels &channels) {
    return std::ranges::all_of(channels, [](double linear) { return placement(linear) == 0; });
}

// The share between @p on, where @p off is false, and @p past, where it is true, at which @p off turns true, taken
// from the @p on side.
double boundary(const auto &off, double on, double past) {
    while (std::abs(past - on) > chromaShareTolerance) {
        const double middle = std::midpoint(on, past);
        if (off(middle))
            past = middle;
        else
            on = middle;
    }
    return on;
}

// The highest share from @p low to @p high at which every channel that @p levels gives fits, when each changes
// monotonically between them, or nothing when no share there fits.
std::optional<double> highestFit(const auto &levels, double low, double high) {
    double lowest = low;
    double highest = high;
    for (std::size_t channel = 0; channel < std::tuple_size_v<Channels>; ++channel) {
        for (const int side : {-1, 1}) {
            // A monotonic channel lies off this side over a start of the stretch, an end of it, all of it or none.
            const auto off = [&](double share) { return placement(levels(share)[channel]) == side; };
            const bool offAtLow = off(low);
            const bool offAtHigh = off(high);
            if (offAtLow && offAtHigh)
                return std::nullopt;
            if (offAtLow)
                lowest = std::max(lowest, boundary(off, high, low));
            else if (offAtHigh)
                highest = std::min(highest, boundary(off, low, high));
        }
    }
    if (lowest > highest)
        return std::nullopt;
    return highest;
}

// The largest share s from 0 to 1 at which the Oklab color (@p l, s @p a, s @p b) fits, for @p l between 0 and 1.
// Scaling a and b together changes chroma alone, so lightness and hue stay exact, and grey, at s = 0, always fits.
// Paths of constant hue near blue leave the gamut and come back, so a bisection over the whole path could stop at an
// inner edge. Each channel is a cubic in s, though, and between the turns of the three cubics every channel is
// monotonic, so the highest stretch where some share fits holds the answer.
double fittingShare(double l, double a, double b) {
    const auto levels = [&](double share) {
        const detail::LinearColor c = detail::oklabToLinear({l, a * share, b * share});
        return Channels{c.r, c.g, c.b};
    };
    if (fits(levels(1.0)))
        return 1.0;
    // Each cone root is base + s slope, and each channel sums its weights times the roots' cubes, so the channel turns
    // where its derivative over 3, the sum of weight slope (base + s slope)^2, is 0.
    const Vector3 base = multiply(oklabToLms, {l, 0.0, 0.0});
    const Vector3 slope = multiply(oklabToLms, {0.0, a, b});
    std::vector<double> breaks{0.0, 1.0};
    for (const Vector3 &weights : lmsToSrgb) {
        double c2 = 0.0;
        double c1 = 0.0;
        double c0 = 0.0;
        for (std::size_t i = 0; i < weights.size(); ++i) {
            c2 += weights[i] * slope[i] * slope[i] * slope[i];
            c1 += 2.0 * weights[i] * slope[i] * slope[i] * base[i];
            c0 += weights[i] * slope[i] * base[i] * base[i];
        }
        detail::forEachUnitRoot(c2, c1, c0, [&](double share) { breaks.push_back(share); });
    }
    std::ranges::sort(breaks, std::ranges::greater{});
    for (std::size_t i = 1; i < breaks.size(); ++i)
        if (const std::optional<double> fit = highestFit(levels, breaks[i], breaks[i - 1]))
            return *fit;
    return 0.0;
}

} // namespace

namespace detail {

LinearColor linear(Color c) {
    return {srgbToLinear(c.r), srgbToLinear(c.g), srgbToLinear(c.b), static_cast<double>(c.a) / byteMax};
}

Oklab linearToOklab(const LinearColor &c) {
    const Vector3 cone = multiply(srgbToLms, {c.r, c.g, c.b});
    const Vector3 lab = multiply(lmsToOklab, {std::cbrt(cone[0]), std::cbrt(cone[1]), std::cbrt(cone[2])});
    return {lab[0], lab[1], lab[2], c.a};
}

LinearColor oklabToLinear(const Oklab &c) {
    const Vector3 root = multiply(oklabToLms, {c.l, c.a, c.b});
    const Vector3 rgb =
        multiply(lmsToSrgb, {root[0] * root[0] * root[0], root[1] * root[1] * root[1], root[2] * root[2] * root[2]});
    return {rgb[0], rgb[1], rgb[2], c.alpha};
}

} // namespace detail

double srgbToLinear(std::uint8_t value) {
    static const std::array<double, 256> table = decodeTable();
    return table[value];
}

std::uint8_t linearToSrgb(double value) { return toByte(encodeSrgb(std::clamp(value, 0.0, 1.0))); }

std::string toHex(Color c) {
    constexpr std::string_view digits = "0123456789abcdef";
    std::string text = "#";
    const auto put = [&](std::uint8_t channel) {
        const auto value = static_cast<std::size_t>(channel);
        text += digits[value / 16];
        text += digits[value % 16];
    };
    put(c.r);
    put(c.g);
    put(c.b);
    if (c.a != full)
        put(c.a);
    return text;
}

Hsl toHsl(Color c) {
    const double alpha = static_cast<double>(c.a) / byteMax;
    const auto [low, high] = std::minmax({c.r, c.g, c.b});
    const double l = (low + high) / (2.0 * byteMax);
    if (low == high)
        return {0.0, 0.0, l, alpha};
    const double r = c.r / byteMax;
    const double g = c.g / byteMax;
    const double b = c.b / byteMax;
    const double spread = (high - low) / byteMax;
    double sextant = 0.0;
    if (high == c.r)
        sextant = (g - b) / spread + (c.g < c.b ? 6.0 : 0.0);
    else if (high == c.g)
        sextant = (b - r) / spread + 2.0;
    else
        sextant = (r - g) / spread + 4.0;
    return {sextant * degreesPerSextant, (high / byteMax - l) / std::min(l, 1.0 - l), l, alpha};
}

Color fromHsl(const Hsl &c) {
    const double s = std::clamp(c.s, 0.0, 1.0);
    const double l = std::clamp(c.l, 0.0, 1.0);
    const double hue = wrapDegrees(c.h) / degreesPerTwelfth;
    const double reach = s * std::min(l, 1.0 - l);
    // Every channel follows the same trapezoid around the hue circle, red's offset by 0 twelfths of a turn, green's by
    // 8 and blue's by 4.
    const auto channel = [&](double offset) {
        const double k = std::fmod(offset + hue, twelfthsPerTurn);
        return toByte(l - reach * std::clamp(std::min(k - 3.0, 9.0 - k), -1.0, 1.0));
    };
    return {channel(0.0), channel(8.0), channel(4.0), toByte(c.alpha)};
}

Oklab toOklab(Color c) { return detail::linearToOklab(detail::linear(c)); }

Oklch toOklch(Color c) {
    const Oklab lab = toOklab(c);
    const double chroma = std::hypot(lab.a, lab.b);
    if (chroma <= greyChroma)
        return {lab.l, 0.0, 0.0, lab.alpha};
    return {lab.l, chroma, wrapDegrees(std::atan2(lab.b, lab.a) * degreesPerRadian), lab.alpha};
}

Color fromOklab(const Oklab &c) {
    const std::uint8_t alpha = toByte(c.alpha);
    if (c.l >= 1.0)
        return {full, full, full, alpha};
    if (c.l <= 0.0)
        return {0, 0, 0, alpha};
    const double share = fittingShare(c.l, c.a, c.b);
    const detail::LinearColor fitted = detail::oklabToLinear({c.l, c.a * share, c.b * share});
    return {linearToSrgb(fitted.r), linearToSrgb(fitted.g), linearToSrgb(fitted.b), alpha};
}

Color fromOklch(const Oklch &c) {
    const double chroma = std::max(0.0, c.c);
    const double hue = c.h / degreesPerRadian;
    return fromOklab({c.l, chroma * std::cos(hue), chroma * std::sin(hue), c.alpha});
}

Color lighten(Color c, double amount) {
    Oklab lab = toOklab(c);
    lab.l += amount;
    return fromOklab(lab);
}

Color darken(Color c, double amount) { return lighten(c, -amount); }

Color saturate(Color c, double amount) {
    Oklab lab = toOklab(c);
    const double chroma = std::hypot(lab.a, lab.b);
    if (chroma <= greyChroma)
        return c;
    const double factor = std::max(0.0, chroma + amount) / chroma;
    lab.a *= factor;
    lab.b *= factor;
    return fromOklab(lab);
}

Color desaturate(Color c, double amount) { return saturate(c, -amount); }

Color mix(Color from, Color to, double t) {
    const double u = std::clamp(t, 0.0, 1.0);
    const Oklab p = toOklab(from);
    const Oklab q = toOklab(to);
    const double alpha = std::lerp(p.alpha, q.alpha, u);
    if (alpha <= 0.0)
        return {0, 0, 0, 0};
    const auto blend = [&](double first, double second) {
        return std::lerp(first * p.alpha, second * q.alpha, u) / alpha;
    };
    return fromOklab({blend(p.l, q.l), blend(p.a, q.a), blend(p.b, q.b), alpha});
}

} // namespace linea
