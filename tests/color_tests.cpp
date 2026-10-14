#include "linea/Color.hpp"

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

using namespace linea;

namespace {

// Rounding each channel to the nearest byte moves a color less than oklabRounding in Oklab wherever its lightness is
// above 0.3, and its lightness less than lightnessRounding wherever its lightness is above 0.4: a half-step error in
// every channel reaches about 0.0035 and 0.0019 there.
constexpr double oklabRounding = 0.004;
constexpr double lightnessRounding = 0.002;

constexpr Color white{255, 255, 255, 255};
constexpr Color black{0, 0, 0, 255};
constexpr Color tomato{255, 99, 71, 255};
constexpr Color steelBlue{70, 130, 180, 255};
constexpr Color seaGreen{46, 139, 87, 255};
constexpr Color mediumOrchid{186, 85, 211, 255};
constexpr Color rosyBrown{188, 143, 143, 255};
constexpr Color darkSeaGreen{143, 188, 143, 255};

bool withinStep(Color a, Color b) {
    const auto within = [](int x, int y) { return std::abs(x - y) <= 1; };
    return within(a.r, b.r) && within(a.g, b.g) && within(a.b, b.b) && within(a.a, b.a);
}

// Every fifth level of each channel, both ends included, with alpha varying from color to color.
std::vector<Color> lattice() {
    constexpr int stride = 5;
    std::vector<Color> colors;
    for (int r = 0; r <= 255; r += stride)
        for (int g = 0; g <= 255; g += stride)
            for (int b = 0; b <= 255; b += stride)
                colors.push_back({static_cast<std::uint8_t>(r), static_cast<std::uint8_t>(g),
                                  static_cast<std::uint8_t>(b), static_cast<std::uint8_t>((r + 2 * g + 3 * b) % 256)});
    return colors;
}

// Whether some channel of @p c is at 0 or 255, as one is for every color on the edge of the sRGB gamut.
bool onGamutEdge(Color c) {
    const auto atEnd = [](std::uint8_t channel) { return channel == 0 || channel == 255; };
    return atEnd(c.r) || atEnd(c.g) || atEnd(c.b);
}

double distance(const Oklab &p, const Oklab &q) { return std::hypot(p.l - q.l, p.a - q.a, p.b - q.b); }

// How far @p c lies, in Oklab's a-b plane, from the ray of @p reference's hue: 0 when they share a hue.
double offHue(const Oklab &c, const Oklab &reference) {
    const double chroma = std::hypot(reference.a, reference.b);
    const double along = (c.a * reference.a + c.b * reference.b) / chroma;
    const double across = (c.b * reference.a - c.a * reference.b) / chroma;
    return along >= 0.0 ? std::abs(across) : std::hypot(c.a, c.b);
}

} // namespace

TEST_CASE("Hex notation reads all four CSS forms, in either case") {
    CHECK(fromHex("#f80") == Color{255, 136, 0, 255});
    CHECK(fromHex("#F80C") == Color{255, 136, 0, 204});
    CHECK(fromHex("#ff8800") == Color{255, 136, 0, 255});
    CHECK(fromHex("#FF880080") == Color{255, 136, 0, 128});
    CHECK(fromHex("#aBcDeF") == Color{171, 205, 239, 255});
    static_assert(fromHex("#663399") == Color{102, 51, 153, 255});
}

TEST_CASE("Text that is not CSS hex notation reads as nothing") {
    for (const std::string_view text : {"", "#", "fff", "ff8800", "#ff", "#ff880", "#ff8800f", "#ff8800ff0", "#ggg",
                                        "#ff880g", "#+ff", " #fff", "#fff ", "##fff"})
        CHECK_FALSE(fromHex(text).has_value());
}

TEST_CASE("Hex notation round-trips every color exactly, writing alpha only when it is not opaque") {
    CHECK(toHex(Color{255, 136, 0, 255}) == "#ff8800");
    CHECK(toHex(Color{18, 52, 86, 120}) == "#12345678");
    CHECK(toHex(Color{0, 0, 0, 0}) == "#00000000");
    for (const Color c : lattice())
        CHECK(fromHex(toHex(c)) == c);
}

TEST_CASE("HSL matches CSS for the primaries, greys and hues past a full turn") {
    CHECK(fromHsl({0.0, 1.0, 0.5}) == Color{255, 0, 0, 255});
    // CSS's green, #008000, is hsl(120 100% 25%).
    CHECK(fromHsl({120.0, 1.0, 0.25}) == Color{0, 128, 0, 255});
    CHECK(fromHsl({240.0, 1.0, 0.5, 0.5}) == Color{0, 0, 255, 128});
    CHECK(fromHsl({-120.0, 1.0, 0.5}) == Color{0, 0, 255, 255});
    CHECK(fromHsl({480.0, 1.0, 0.5}) == Color{0, 255, 0, 255});
    CHECK(fromHsl({30.0, 0.0, 0.5}) == Color{128, 128, 128, 255});
    CHECK(fromHsl({0.0, 2.0, 1.5}) == white);

    const Hsl green = toHsl(Color{0, 128, 0, 255});
    CHECK(green.h == doctest::Approx(120.0));
    CHECK(green.s == doctest::Approx(1.0));
    CHECK(green.l == doctest::Approx(128.0 / 510.0));
    const Hsl grey = toHsl(Color{90, 90, 90, 51});
    CHECK(grey.h == 0.0);
    CHECK(grey.s == 0.0);
    CHECK(grey.l == doctest::Approx(90.0 / 255.0));
    CHECK(grey.alpha == doctest::Approx(0.2));
}

TEST_CASE("HSL round-trips every color within one 8-bit step") {
    for (const Color c : lattice())
        CHECK(withinStep(fromHsl(toHsl(c)), c));
}

TEST_CASE("White has Oklab lightness 1 and black 0, and greys have no chroma or hue") {
    const Oklab bright = toOklab(white);
    CHECK(bright.l == doctest::Approx(1.0).epsilon(1e-12));
    CHECK(bright.a == doctest::Approx(0.0).epsilon(1e-12));
    CHECK(bright.b == doctest::Approx(0.0).epsilon(1e-12));
    const Oklab dark = toOklab(black);
    CHECK(dark.l == 0.0);
    CHECK(dark.a == 0.0);
    CHECK(dark.b == 0.0);
    for (int level = 0; level <= 255; ++level) {
        const auto channel = static_cast<std::uint8_t>(level);
        const Oklch grey = toOklch({channel, channel, channel, 255});
        CHECK(grey.c == 0.0);
        CHECK(grey.h == 0.0);
    }
}

TEST_CASE("Oklch matches the values CSS Color 4 gives for sRGB blue and yellow") {
    // CSS Color 4 writes sRGB blue as oklch(0.452 0.313 264.1) and yellow as oklch(0.968 0.211 109.8), each rounded
    // to its last place.
    const auto agrees = [](double value, double published, double lastPlace) {
        return std::abs(value - published) <= lastPlace / 2.0;
    };
    const Oklch blue = toOklch({0, 0, 255, 255});
    CHECK(agrees(blue.l, 0.452, 0.001));
    CHECK(agrees(blue.c, 0.313, 0.001));
    CHECK(agrees(blue.h, 264.1, 0.1));
    const Oklch yellow = toOklch({255, 255, 0, 255});
    CHECK(agrees(yellow.l, 0.968, 0.001));
    CHECK(agrees(yellow.c, 0.211, 0.001));
    CHECK(agrees(yellow.h, 109.8, 0.1));
}

TEST_CASE("Oklab and Oklch round-trip every color within one 8-bit step") {
    for (const Color c : lattice()) {
        CHECK(withinStep(fromOklab(toOklab(c)), c));
        CHECK(withinStep(fromOklch(toOklch(c)), c));
    }
}

TEST_CASE("A color outside sRGB keeps its lightness and hue and gives up only chroma") {
    const Oklch vivid{0.7, 0.4, 150.0};
    const Color fitted = fromOklch(vivid);
    const Oklab got = toOklab(fitted);
    CHECK(std::abs(got.l - vivid.l) <= lightnessRounding);
    const double hue = vivid.h * std::numbers::pi / 180.0;
    CHECK(offHue(got, {vivid.l, std::cos(hue), std::sin(hue)}) <= oklabRounding);
    CHECK(onGamutEdge(fitted));
    // Lightness past either end is white or black, and alpha clamps.
    CHECK(fromOklch({1.2, 0.1, 30.0, 0.5}) == Color{255, 255, 255, 128});
    CHECK(fromOklab({-0.1, 0.05, 0.05, 2.0}) == black);
}

TEST_CASE("Asking for more chroma never gives less, even where a path of constant hue leaves sRGB and comes back") {
    // At lightness 0.38, the path of hue 264, toward blue, leaves the gamut at chroma 0.224 and fits again from 0.263
    // to 0.264, so a search that stopped at the first edge would give less chroma for more. Each kept chroma is within
    // rounding of its true value, so two can differ by twice that.
    constexpr double fewest = 0.2;
    constexpr double most = 0.4;
    constexpr int steps = 2000;
    double keptSoFar = 0.0;
    for (int step = 0; step <= steps; ++step) {
        const double asked = std::lerp(fewest, most, static_cast<double>(step) / steps);
        const double kept = toOklch(fromOklch({0.38, asked, 264.0})).c;
        CHECK(kept >= keptSoFar - 2.0 * oklabRounding);
        keptSoFar = std::max(keptSoFar, kept);
    }
}

TEST_CASE("Lightening raises Oklab lightness by exactly the amount, and darkening lowers it") {
    for (const Color c : {tomato, steelBlue, seaGreen, mediumOrchid, rosyBrown}) {
        const Oklab before = toOklab(c);
        for (const double amount : {0.05, 0.1, 0.2, 0.3}) {
            const Oklab lighter = toOklab(lighten(c, amount));
            CHECK(std::abs(lighter.l - before.l - amount) <= lightnessRounding);
            CHECK(offHue(lighter, before) <= oklabRounding);
        }
        for (const double amount : {0.05, 0.1, 0.15}) {
            const Oklab darker = toOklab(darken(c, amount));
            CHECK(std::abs(before.l - darker.l - amount) <= lightnessRounding);
            CHECK(offHue(darker, before) <= oklabRounding);
        }
        CHECK(lighten(c, 1.0) == white);
        CHECK(darken(c, 1.0) == black);
    }
    CHECK(lighten(Color{70, 130, 180, 77}, 0.1).a == 77);
}

TEST_CASE("Saturating raises Oklch chroma by exactly the amount, up to the edge of the sRGB gamut") {
    for (const Color c : {rosyBrown, darkSeaGreen}) {
        const Oklch before = toOklch(c);
        for (const double amount : {0.02, 0.05, 0.1}) {
            const Oklch more = toOklch(saturate(c, amount));
            CHECK(std::abs(more.c - before.c - amount) <= oklabRounding);
            CHECK(std::abs(more.l - before.l) <= lightnessRounding);
        }
        const Oklch less = toOklch(desaturate(c, 0.02));
        CHECK(std::abs(before.c - less.c - 0.02) <= oklabRounding);

        const Color vivid = saturate(c, 1.0);
        CHECK(onGamutEdge(vivid));
        CHECK(std::abs(toOklab(vivid).l - before.l) <= lightnessRounding);
        CHECK(offHue(toOklab(vivid), toOklab(c)) <= oklabRounding);
        const Color grey = desaturate(c, 1.0);
        CHECK((grey.r == grey.g && grey.g == grey.b));
        CHECK(std::abs(toOklab(grey).l - before.l) <= lightnessRounding);
    }
    CHECK(saturate(Color{128, 128, 128, 255}, 0.1) == Color{128, 128, 128, 255});
}

TEST_CASE("Mixing moves through Oklab in equal steps") {
    const Oklab p = toOklab(tomato);
    const Oklab q = toOklab(steelBlue);
    constexpr int steps = 8;
    for (int step = 0; step <= steps; ++step) {
        const double t = static_cast<double>(step) / steps;
        const Oklab expected{std::lerp(p.l, q.l, t), std::lerp(p.a, q.a, t), std::lerp(p.b, q.b, t)};
        CHECK(distance(toOklab(mix(tomato, steelBlue, t)), expected) <= oklabRounding);
    }
    CHECK(withinStep(mix(tomato, steelBlue, 0.0), tomato));
    CHECK(withinStep(mix(tomato, steelBlue, 1.0), steelBlue));
    CHECK(mix(tomato, steelBlue, -1.0) == mix(tomato, steelBlue, 0.0));
    CHECK(mix(tomato, steelBlue, 2.0) == mix(tomato, steelBlue, 1.0));
    // Halfway from black to white in Oklab is lightness 0.5, linear 0.125, which sRGB encodes as 99: perceptually
    // halfway, where a blend in sRGB would give 128.
    CHECK(mix(black, white, 0.5) == Color{99, 99, 99, 255});
}

TEST_CASE("Mixing premultiplies alpha, so a transparent color lends no hue") {
    CHECK(withinStep(mix(tomato, Color{0, 0, 0, 0}, 0.5), Color{255, 99, 71, 128}));
    CHECK(mix(Color{255, 0, 0, 0}, Color{0, 0, 255, 0}, 0.3) == Color{0, 0, 0, 0});
}

TEST_CASE("The named colors are CSS Color 4's 148, each found by its name in any case") {
    CHECK(namedColors.size() == 148);
    for (const NamedColor &entry : namedColors) {
        CHECK(namedColor(entry.name) == entry.color);
        std::string shouted(entry.name);
        for (char &letter : shouted)
            letter = static_cast<char>(letter - 'a' + 'A');
        CHECK(namedColor(shouted) == entry.color);
    }
    CHECK(namedColor("rebeccapurple") == Color{102, 51, 153, 255});
    CHECK(namedColor("tomato") == tomato);
    CHECK(namedColor("gray") == Color{128, 128, 128, 255});
    CHECK(namedColor("grey") == namedColor("gray"));
    CHECK(namedColor("aqua") == namedColor("cyan"));
    for (const std::string_view unknown : {"transparent", "currentcolor", "", "grey ", "bluish"})
        CHECK_FALSE(namedColor(unknown).has_value());
    static_assert(namedColor("RebeccaPurple") == fromHex("#663399"));
}
