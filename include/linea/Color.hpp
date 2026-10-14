// linea - colors: sRGB bytes, hex notation, HSL, Oklab and Oklch, the CSS named colors, and adjustments that step
// evenly to the eye.
#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace linea {

// An sRGB color with straight alpha, as images store it.
struct Color {
    std::uint8_t r{};
    std::uint8_t g{};
    std::uint8_t b{};
    std::uint8_t a = 255;
    friend constexpr bool operator==(Color, Color) = default;
};

// sRGB-encoded 0 to 255 and linear light 0 to 1, as the sRGB standard defines them.
double srgbToLinear(std::uint8_t value);
std::uint8_t linearToSrgb(double value);

namespace detail {

// The value of the hex digit @p c, in either case, or nothing when @p c is not one.
constexpr std::optional<int> hexDigitValue(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if (c >= 'a' && c <= 'f')
        return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
        return c - 'A' + 10;
    return std::nullopt;
}

// @p c in lower case when it is an ASCII capital letter, and @p c itself otherwise.
constexpr char lowerAscii(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

// Whether @p left sorts before @p right, ignoring ASCII case.
constexpr bool beforeIgnoringCase(std::string_view left, std::string_view right) {
    return std::ranges::lexicographical_compare(left, right, {}, lowerAscii, lowerAscii);
}

} // namespace detail

// The color that @p text writes in CSS hex notation: '#' and then 3, 4, 6 or 8 hex digits in either case, as #rgb,
// #rgba, #rrggbb or #rrggbbaa. A short form's digit stands for itself twice, so #f80 is #ff8800, and a color written
// without alpha is opaque. Nothing for any other text, surrounding spaces included.
constexpr std::optional<Color> fromHex(std::string_view text) {
    if (!text.starts_with('#'))
        return std::nullopt;
    text.remove_prefix(1);
    const bool shortForm = text.size() == 3 || text.size() == 4;
    if (!shortForm && text.size() != 6 && text.size() != 8)
        return std::nullopt;
    const std::size_t width = shortForm ? 1 : 2;
    std::array<std::uint8_t, 4> channels{0, 0, 0, 255};
    for (std::size_t i = 0; i < text.size() / width; ++i) {
        int value = 0;
        for (const char digit : text.substr(i * width, width)) {
            const std::optional<int> digitValue = detail::hexDigitValue(digit);
            if (!digitValue)
                return std::nullopt;
            value = value * 16 + *digitValue;
        }
        channels[i] = static_cast<std::uint8_t>(shortForm ? value * 16 + value : value);
    }
    return Color{channels[0], channels[1], channels[2], channels[3]};
}

// @p c in lowercase CSS hex notation: #rrggbb when it is opaque and #rrggbbaa otherwise.
std::string toHex(Color c);

// A color in HSL, the cylindrical form of sRGB that CSS's hsl() writes: hue @p h in degrees around the color wheel,
// red at 0, green at 120 and blue at 240; saturation @p s and lightness @p l from 0 to 1, where lightness 0 is black
// and 1 is white at any saturation; and @p alpha from 0, transparent, to 1, opaque.
struct Hsl {
    double h{};
    double s{};
    double l{};
    double alpha = 1.0;
};

// @p c in HSL, as CSS Color 4 converts it, with hue in [0, 360). A grey has no hue: it takes hue 0 and saturation 0.
Hsl toHsl(Color c);
// The color @p c describes, as CSS Color 4 converts it, each channel rounded to the nearest byte. Hue wraps around the
// circle, and saturation, lightness and alpha clamp to 0 to 1.
Color fromHsl(const Hsl &c);

// A color in Oklab, Bjorn Ottosson's perceptual color space (2020), where equal distances look like equal differences:
// lightness @p l from 0, black, to 1, white; @p a from green (negative) to red (positive); @p b from blue (negative)
// to yellow (positive); and @p alpha from 0, transparent, to 1, opaque.
struct Oklab {
    double l{};
    double a{};
    double b{};
    double alpha = 1.0;
};

// A color in Oklch, the polar form of Oklab that CSS's oklch() writes: lightness @p l as Oklab's; chroma @p c, the
// distance sqrt(a^2 + b^2) from the grey of the same lightness, 0 for greys and about 0.32 at most in sRGB, at
// #ff00ff; hue @p h in degrees, atan2(b, a), near 29 for red, 110 for yellow, 142 for green and 264 for blue; and
// @p alpha from 0, transparent, to 1, opaque.
struct Oklch {
    double l{};
    double c{};
    double h{};
    double alpha = 1.0;
};

// @p c in Oklab.
Oklab toOklab(Color c);
// @p c in Oklch, with hue in [0, 360). A grey has no hue: it takes chroma 0 and hue 0.
Oklch toOklch(Color c);
// The color @p c describes, each channel rounded to the nearest byte. Lightness 1 or more gives white and 0 or less
// black, and alpha clamps to 0 to 1. A color outside the sRGB gamut keeps its lightness and hue and gives up only as
// much chroma as it must for every channel to round to a byte without clipping.
Color fromOklab(const Oklab &c);
// The color @p c describes, made as fromOklab() makes it. Hue wraps around the circle, and negative chroma counts as 0.
Color fromOklch(const Oklch &c);

// The adjustments below work in Oklab, where equal distances look like equal differences, so an amount is the same
// visible step whatever the color. Each keeps alpha, and a result outside the sRGB gamut loses chroma as fromOklab()
// describes.

// @p c with its Oklab lightness raised by @p amount, keeping its hue and chroma. Lightness runs from 0, black, to 1,
// white, so lighten(c, 0.1) moves any color a tenth of the way from black to white. A negative @p amount darkens.
// Lightness stops at 0 and 1.
Color lighten(Color c, double amount);
// lighten(c, -amount): @p c with its Oklab lightness lowered by @p amount.
Color darken(Color c, double amount);
// @p c with its Oklch chroma raised by @p amount, keeping its lightness and hue. Chroma runs from 0 for greys to about
// 0.32 for the most vivid sRGB colors, so saturate(c, 0.05) is one modest step for any color. A negative @p amount
// desaturates. Chroma stops at 0 and at the edge of the sRGB gamut. A grey has no hue to strengthen and comes back
// unchanged.
Color saturate(Color c, double amount);
// saturate(c, -amount): @p c with its Oklch chroma lowered by @p amount, down to the grey of the same lightness.
Color desaturate(Color c, double amount);
// The color @p t of the way from @p from to @p to, blended in Oklab with premultiplied alpha as CSS's
// color-mix(in oklab, ...) blends: alpha, and each Oklab coordinate times alpha, move in proportion to @p t, so a
// transparent color lends no hue. @p t clamps to 0 to 1, and a mix whose alpha comes to 0 is transparent black.
Color mix(Color from, Color to, double t);

// A color that CSS names.
struct NamedColor {
    std::string_view name{};
    Color color{};
};

// The 148 named colors of CSS Color 4, in lower case and in alphabetical order. Some share a value: aqua and cyan,
// fuchsia and magenta, and each gray with its grey.
inline constexpr auto namedColors = std::to_array<NamedColor>({
    {"aliceblue", {240, 248, 255, 255}},
    {"antiquewhite", {250, 235, 215, 255}},
    {"aqua", {0, 255, 255, 255}},
    {"aquamarine", {127, 255, 212, 255}},
    {"azure", {240, 255, 255, 255}},
    {"beige", {245, 245, 220, 255}},
    {"bisque", {255, 228, 196, 255}},
    {"black", {0, 0, 0, 255}},
    {"blanchedalmond", {255, 235, 205, 255}},
    {"blue", {0, 0, 255, 255}},
    {"blueviolet", {138, 43, 226, 255}},
    {"brown", {165, 42, 42, 255}},
    {"burlywood", {222, 184, 135, 255}},
    {"cadetblue", {95, 158, 160, 255}},
    {"chartreuse", {127, 255, 0, 255}},
    {"chocolate", {210, 105, 30, 255}},
    {"coral", {255, 127, 80, 255}},
    {"cornflowerblue", {100, 149, 237, 255}},
    {"cornsilk", {255, 248, 220, 255}},
    {"crimson", {220, 20, 60, 255}},
    {"cyan", {0, 255, 255, 255}},
    {"darkblue", {0, 0, 139, 255}},
    {"darkcyan", {0, 139, 139, 255}},
    {"darkgoldenrod", {184, 134, 11, 255}},
    {"darkgray", {169, 169, 169, 255}},
    {"darkgreen", {0, 100, 0, 255}},
    {"darkgrey", {169, 169, 169, 255}},
    {"darkkhaki", {189, 183, 107, 255}},
    {"darkmagenta", {139, 0, 139, 255}},
    {"darkolivegreen", {85, 107, 47, 255}},
    {"darkorange", {255, 140, 0, 255}},
    {"darkorchid", {153, 50, 204, 255}},
    {"darkred", {139, 0, 0, 255}},
    {"darksalmon", {233, 150, 122, 255}},
    {"darkseagreen", {143, 188, 143, 255}},
    {"darkslateblue", {72, 61, 139, 255}},
    {"darkslategray", {47, 79, 79, 255}},
    {"darkslategrey", {47, 79, 79, 255}},
    {"darkturquoise", {0, 206, 209, 255}},
    {"darkviolet", {148, 0, 211, 255}},
    {"deeppink", {255, 20, 147, 255}},
    {"deepskyblue", {0, 191, 255, 255}},
    {"dimgray", {105, 105, 105, 255}},
    {"dimgrey", {105, 105, 105, 255}},
    {"dodgerblue", {30, 144, 255, 255}},
    {"firebrick", {178, 34, 34, 255}},
    {"floralwhite", {255, 250, 240, 255}},
    {"forestgreen", {34, 139, 34, 255}},
    {"fuchsia", {255, 0, 255, 255}},
    {"gainsboro", {220, 220, 220, 255}},
    {"ghostwhite", {248, 248, 255, 255}},
    {"gold", {255, 215, 0, 255}},
    {"goldenrod", {218, 165, 32, 255}},
    {"gray", {128, 128, 128, 255}},
    {"green", {0, 128, 0, 255}},
    {"greenyellow", {173, 255, 47, 255}},
    {"grey", {128, 128, 128, 255}},
    {"honeydew", {240, 255, 240, 255}},
    {"hotpink", {255, 105, 180, 255}},
    {"indianred", {205, 92, 92, 255}},
    {"indigo", {75, 0, 130, 255}},
    {"ivory", {255, 255, 240, 255}},
    {"khaki", {240, 230, 140, 255}},
    {"lavender", {230, 230, 250, 255}},
    {"lavenderblush", {255, 240, 245, 255}},
    {"lawngreen", {124, 252, 0, 255}},
    {"lemonchiffon", {255, 250, 205, 255}},
    {"lightblue", {173, 216, 230, 255}},
    {"lightcoral", {240, 128, 128, 255}},
    {"lightcyan", {224, 255, 255, 255}},
    {"lightgoldenrodyellow", {250, 250, 210, 255}},
    {"lightgray", {211, 211, 211, 255}},
    {"lightgreen", {144, 238, 144, 255}},
    {"lightgrey", {211, 211, 211, 255}},
    {"lightpink", {255, 182, 193, 255}},
    {"lightsalmon", {255, 160, 122, 255}},
    {"lightseagreen", {32, 178, 170, 255}},
    {"lightskyblue", {135, 206, 250, 255}},
    {"lightslategray", {119, 136, 153, 255}},
    {"lightslategrey", {119, 136, 153, 255}},
    {"lightsteelblue", {176, 196, 222, 255}},
    {"lightyellow", {255, 255, 224, 255}},
    {"lime", {0, 255, 0, 255}},
    {"limegreen", {50, 205, 50, 255}},
    {"linen", {250, 240, 230, 255}},
    {"magenta", {255, 0, 255, 255}},
    {"maroon", {128, 0, 0, 255}},
    {"mediumaquamarine", {102, 205, 170, 255}},
    {"mediumblue", {0, 0, 205, 255}},
    {"mediumorchid", {186, 85, 211, 255}},
    {"mediumpurple", {147, 112, 219, 255}},
    {"mediumseagreen", {60, 179, 113, 255}},
    {"mediumslateblue", {123, 104, 238, 255}},
    {"mediumspringgreen", {0, 250, 154, 255}},
    {"mediumturquoise", {72, 209, 204, 255}},
    {"mediumvioletred", {199, 21, 133, 255}},
    {"midnightblue", {25, 25, 112, 255}},
    {"mintcream", {245, 255, 250, 255}},
    {"mistyrose", {255, 228, 225, 255}},
    {"moccasin", {255, 228, 181, 255}},
    {"navajowhite", {255, 222, 173, 255}},
    {"navy", {0, 0, 128, 255}},
    {"oldlace", {253, 245, 230, 255}},
    {"olive", {128, 128, 0, 255}},
    {"olivedrab", {107, 142, 35, 255}},
    {"orange", {255, 165, 0, 255}},
    {"orangered", {255, 69, 0, 255}},
    {"orchid", {218, 112, 214, 255}},
    {"palegoldenrod", {238, 232, 170, 255}},
    {"palegreen", {152, 251, 152, 255}},
    {"paleturquoise", {175, 238, 238, 255}},
    {"palevioletred", {219, 112, 147, 255}},
    {"papayawhip", {255, 239, 213, 255}},
    {"peachpuff", {255, 218, 185, 255}},
    {"peru", {205, 133, 63, 255}},
    {"pink", {255, 192, 203, 255}},
    {"plum", {221, 160, 221, 255}},
    {"powderblue", {176, 224, 230, 255}},
    {"purple", {128, 0, 128, 255}},
    {"rebeccapurple", {102, 51, 153, 255}},
    {"red", {255, 0, 0, 255}},
    {"rosybrown", {188, 143, 143, 255}},
    {"royalblue", {65, 105, 225, 255}},
    {"saddlebrown", {139, 69, 19, 255}},
    {"salmon", {250, 128, 114, 255}},
    {"sandybrown", {244, 164, 96, 255}},
    {"seagreen", {46, 139, 87, 255}},
    {"seashell", {255, 245, 238, 255}},
    {"sienna", {160, 82, 45, 255}},
    {"silver", {192, 192, 192, 255}},
    {"skyblue", {135, 206, 235, 255}},
    {"slateblue", {106, 90, 205, 255}},
    {"slategray", {112, 128, 144, 255}},
    {"slategrey", {112, 128, 144, 255}},
    {"snow", {255, 250, 250, 255}},
    {"springgreen", {0, 255, 127, 255}},
    {"steelblue", {70, 130, 180, 255}},
    {"tan", {210, 180, 140, 255}},
    {"teal", {0, 128, 128, 255}},
    {"thistle", {216, 191, 216, 255}},
    {"tomato", {255, 99, 71, 255}},
    {"turquoise", {64, 224, 208, 255}},
    {"violet", {238, 130, 238, 255}},
    {"wheat", {245, 222, 179, 255}},
    {"white", {255, 255, 255, 255}},
    {"whitesmoke", {245, 245, 245, 255}},
    {"yellow", {255, 255, 0, 255}},
    {"yellowgreen", {154, 205, 50, 255}},
});

// The named color @p name, which matches ignoring ASCII case as CSS keywords do, or nothing for any other name. CSS's
// transparent and currentcolor are keywords of their own, not named colors, and give nothing.
constexpr std::optional<Color> namedColor(std::string_view name) {
    const auto found = std::ranges::lower_bound(namedColors, name, detail::beforeIgnoringCase, &NamedColor::name);
    if (found == namedColors.end() || detail::beforeIgnoringCase(name, found->name))
        return std::nullopt;
    return found->color;
}

} // namespace linea
