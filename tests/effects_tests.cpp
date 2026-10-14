#include "support.hpp"

#include "linea/Drawing.hpp"
#include "linea/Shapes.hpp"

#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <vector>

using namespace linea;
using namespace linea::test;

namespace {

constexpr Color red{255, 0, 0, 255};
constexpr Color green{0, 255, 0, 255};
constexpr Color blue{0, 0, 255, 255};
constexpr Color clear{0, 0, 0, 0};
constexpr double byteMax = 255.0;

// A kernel reaches three standard deviations, so it holds 99.7% of the Gaussian and spreads 1.2% less widely than the
// deviation asked for. Two percent allows that and the rounding of alpha to 255 levels.
constexpr double deviationTolerance = 0.02;
// Rounding alpha to 255 levels moves the centroid of an edge a few thousandths of a pixel.
constexpr double centerTolerance = 0.02;

// The alpha of every pixel of @p image added up, in levels of 255 to a pixel.
int alphaLevels(const Image &image) {
    int sum = 0;
    for (std::size_t i = 3; i < image.rgba.size(); i += 4)
        sum += image.rgba[i];
    return sum;
}

// The alpha of each pixel along row @p y of @p image.
std::vector<double> rowAlpha(const Image &image, int y) {
    std::vector<double> alpha;
    for (int x = 0; x < image.width; ++x)
        alpha.push_back(pixel(image, x, y)[3] / byteMax);
    return alpha;
}

// The alpha of each pixel down column @p x of @p image.
std::vector<double> columnAlpha(const Image &image, int x) {
    std::vector<double> alpha;
    for (int y = 0; y < image.height; ++y)
        alpha.push_back(pixel(image, x, y)[3] / byteMax);
    return alpha;
}

// Where alpha falls along a line of pixels, and how widely: the centroid and the standard deviation, in pixels, of the
// drops in alpha from each pixel to the next, each at the border the two share.
struct Spread {
    double center{};
    double deviation{};
};

Spread fall(const std::vector<double> &alpha) {
    double weight = 0.0;
    double moment = 0.0;
    for (std::size_t i = 1; i < alpha.size(); ++i) {
        const double drop = alpha[i - 1] - alpha[i];
        weight += drop;
        moment += drop * static_cast<double>(i);
    }
    const double center = moment / weight;
    double variance = 0.0;
    for (std::size_t i = 1; i < alpha.size(); ++i) {
        const double from = static_cast<double>(i) - center;
        variance += (alpha[i - 1] - alpha[i]) * from * from;
    }
    return {center, std::sqrt(variance / weight)};
}

// Checks that @p spread is centered at @p center and spreads with @p deviation, within the tolerances above.
void checkSpread(const Spread &spread, double center, double deviation) {
    CHECK(std::abs(spread.center - center) <= centerTolerance);
    CHECK(std::abs(spread.deviation - deviation) <= deviationTolerance * deviation);
}

} // namespace

TEST_CASE("A clipped layer covers exactly the intersection of its shape and its clip") {
    // A triangle and a square clip, both in the layer's own coordinates, which its transform moves 2 pixels right and
    // down.
    const Vec2 corners[] = {{0.0, 0.0}, {20.0, 0.0}, {0.0, 20.0}};
    Layer whole = filled(polygon(corners), red);
    whole.transform = translate({2.0, 2.0});
    Layer clipped = whole;
    clipped.clip = Clip{rectangle({4.0, 4.0}, {24.0, 24.0})};
    const Image shape = render({32.0, 32.0, {whole}}, 32, 32);
    const Image cut = render({32.0, 32.0, {clipped}}, 32, 32);
    // Moved with the layer, the clip covers the whole pixels from 6 to 26 each way: inside them the clipped layer is
    // the triangle, pixel for pixel, and outside them it shows nothing.
    for (int y = 0; y < 32; ++y)
        for (int x = 0; x < 32; ++x)
            CHECK(colorAt(cut, x, y) == (x >= 6 && x < 26 && y >= 6 && y < 26 ? colorAt(shape, x, y) : clear));
    // What shows is the triangle's corner within the clip, with legs of 12 pixels and an area of 72, 72 * 255 levels
    // of alpha. Each of the 12 pixels its long side halves rounds by at most half a level, 6 levels in all.
    CHECK(std::abs(alphaLevels(cut) - 72 * 255) <= 6);
}

TEST_CASE("A clip covers what its fill rule fills, and cuts a layer's fill and stroke as one image") {
    // Two nested squares that wind the same way: nonzero fills both, and even-odd only the band between them.
    Path frame = rectangle({0.0, 0.0}, {16.0, 16.0});
    frame.append(rectangle({4.0, 4.0}, {12.0, 12.0}));
    Layer square = filled(rectangle({0.0, 0.0}, {16.0, 16.0}), red);
    square.clip = Clip{frame, FillRule::nonZero};
    const Image nonZero = render({16.0, 16.0, {square}}, 16, 16);
    CHECK(colorAt(nonZero, 8, 8) == red);
    square.clip = Clip{frame, FillRule::evenOdd};
    const Image evenOdd = render({16.0, 16.0, {square}}, 16, 16);
    CHECK(colorAt(evenOdd, 8, 8) == clear);
    CHECK(colorAt(evenOdd, 2, 2) == red);

    // A clip edge through the middle of column 8, where a blue stroke covers a red fill whole. Cut as one image, the
    // pixel is half of blue; the fill and the stroke cut one by one would leave three quarters of a red and blue mix.
    Layer badge = filled(rectangle({2.0, 2.0}, {14.0, 14.0}), red);
    badge.stroke = Paint{blue};
    badge.strokeStyle = {.width = 2.0, .join = Join::miter};
    badge.clip = Clip{rectangle({0.0, 0.0}, {8.5, 16.0})};
    const Image image = render({16.0, 16.0, {badge}}, 16, 16);
    CHECK(colorAt(image, 8, 2) == Color{0, 0, 255, 128});
    CHECK(colorAt(image, 7, 2) == blue);
    CHECK(colorAt(image, 7, 8) == red);
    CHECK(colorAt(image, 9, 8) == clear);
}

TEST_CASE("A clipped group cuts its children together, in the group's own coordinates") {
    // Children side by side from 0 to 16 and a clip from 4 to 12, all moved 8 pixels right by the group.
    const Group pair{
        .children = {filled(rectangle({0.0, 0.0}, {8.0, 4.0}), red), filled(rectangle({8.0, 0.0}, {16.0, 4.0}), blue)},
        .transform = translate({8.0, 0.0}),
        .clip = Clip{rectangle({4.0, 0.0}, {12.0, 4.0})}};
    const Image image = render({32.0, 4.0, {pair}}, 32, 4);
    for (int x = 0; x < 32; ++x)
        CHECK(colorAt(image, x, 2) == (x >= 12 && x < 16 ? red : x >= 16 && x < 20 ? blue : clear));
}

TEST_CASE("An alpha mask scales a group's alpha by the mask's alpha, exactly, in the group's own coordinates") {
    // Under a mask of black at a quarter alpha, then opaque black, then nothing, all moved 2 pixels right with the
    // group. Only the mask's alpha counts, not its color.
    Group masked{.children = {filled(rectangle({0.0, 0.0}, {12.0, 2.0}), red)}, .transform = translate({2.0, 0.0})};
    masked.mask = Mask{{filled(rectangle({0.0, 0.0}, {4.0, 2.0}), Color{0, 0, 0, 64}),
                        filled(rectangle({4.0, 0.0}, {8.0, 2.0}), Color{0, 0, 0, 255})}};
    const Image image = render({14.0, 2.0, {masked}}, 14, 2);
    for (int x = 0; x < 14; ++x)
        CHECK(colorAt(image, x, 1) == (x < 2 ? clear : x < 6 ? Color{255, 0, 0, 64} : x < 10 ? red : clear));
    // The mask multiplies with the group's opacity: half of a quarter is an eighth.
    masked.opacity = 0.5;
    const Image faded = render({14.0, 2.0, {masked}}, 14, 2);
    CHECK(colorAt(faded, 3, 1) == Color{255, 0, 0, 32});
    CHECK(colorAt(faded, 7, 1) == Color{255, 0, 0, 128});
}

TEST_CASE("A luminance mask scales a group's alpha by the mask's luminance in linear light, times its alpha") {
    // Red, green and blue give CSS Masking's coefficients, 0.2125, 0.7154 and 0.0721 of 255, which round to 54, 182
    // and 18. sRGB grey 188 is linear 0.503, which gives 128 where a luminance of sRGB values would give 188. White at
    // half alpha gives half, and opaque black gives nothing.
    const Color shades[] = {red, green, blue, {188, 188, 188, 255}, {255, 255, 255, 128}, {0, 0, 0, 255}};
    const std::uint8_t expected[] = {54, 182, 18, 128, 128, 0};
    Mask mask{.mode = MaskMode::luminance};
    for (int i = 0; i < 6; ++i) {
        const double x = i;
        mask.layers.push_back(filled(rectangle({x, 0.0}, {x + 1.0, 1.0}), shades[i]));
    }
    const Group masked{.children = {filled(rectangle({0.0, 0.0}, {6.0, 1.0}), blue)}, .mask = mask};
    const Image image = render({6.0, 1.0, {masked}}, 6, 1);
    for (int i = 0; i < 6; ++i)
        CHECK(pixel(image, i, 0)[3] == expected[i]);
    // The mask scales alpha only, so the group keeps its color wherever it shows.
    CHECK(colorAt(image, 1, 0) == Color{0, 0, 255, 182});
}

TEST_CASE("A blur keeps total alpha, and draws in what lies just outside the image") {
    // A blur of 2 pixels reaches 6 beyond a shape's edges. Each pixel it reaches rounds to the nearest of 255 levels of
    // alpha, so the total strays at most half a level for each: this square's blur reaches 28 by 28 pixels.
    Layer square = filled(rectangle({12.0, 12.0}, {28.0, 28.0}), red);
    square.blur = 2.0;
    CHECK(std::abs(alphaLevels(render({40.0, 40.0, {square}}, 40, 40)) - 256 * 255) <= 28 * 28 / 2);
    // Half of this square lies beyond the image's left edge. The blur carries as much of that half into the image as it
    // carries of the other half out, so the image holds half of the square, give or take the rounding of the 14 by 28
    // pixels the blur reaches within it.
    Layer straddling = filled(rectangle({-8.0, 12.0}, {8.0, 28.0}), red);
    straddling.blur = 2.0;
    CHECK(std::abs(alphaLevels(render({40.0, 40.0, {straddling}}, 40, 40)) - 128 * 255) <= 14 * 28 / 2);
    // A shape that covers the image and more stays whole, to the last level: the kernel sums to 1, and what lies
    // beyond the image blurs in as fast as what lies within blurs out.
    Layer cover = filled(rectangle({-10.0, -10.0}, {50.0, 50.0}), red);
    cover.blur = 2.0;
    CHECK(alphaLevels(render({40.0, 40.0, {cover}}, 40, 40)) == 40 * 40 * 255);
}

TEST_CASE("A blurred edge falls with the blur's standard deviation, carried into pixels by the transform") {
    // A deviation of 1 unit, at 4 pixels a unit, under a transform that stretches x twice as far as y spreads 8 pixels
    // across x and 4 down y. Turned a quarter turn after stretching, it spreads 4 across x and 8 down y.
    struct Placement {
        Affine transform;
        double acrossX;
        double downY;
    };
    const Placement placements[] = {{scale(2.0, 1.0), 8.0, 4.0}, {quarterTurn * scale(2.0, 1.0), 4.0, 8.0}};
    for (const Placement &placement : placements) {
        // A square that covers the image up to pixel 64 across and down, written in the layer's own coordinates.
        Layer corner = filled(transformed(rectangle({-40.0, -40.0}, {16.0, 16.0}), inverse(placement.transform)), red);
        corner.transform = placement.transform;
        corner.blur = 1.0;
        const Image image = render({32.0, 32.0, {corner}}, 128, 128);
        checkSpread(fall(rowAlpha(image, 16)), 64.0, placement.acrossX);
        checkSpread(fall(columnAlpha(image, 16)), 64.0, placement.downY);
    }
}

TEST_CASE("A drop shadow falls at its offset in its color, beneath the content") {
    // A square and a shadow 1.5 units right and 1 down, in the layer's own coordinates, which its transform scales by
    // 2: the shadow falls 3 pixels right and 2 down.
    Layer square = filled(rectangle({1.0, 1.0}, {4.0, 4.0}), red);
    square.transform = scale(2.0);
    square.shadow = DropShadow{.offset = {1.5, 1.0}, .color = blue};
    const Image image = render({16.0, 16.0, {square}}, 16, 16);
    for (int y = 0; y < 16; ++y) {
        for (int x = 0; x < 16; ++x) {
            const bool content = x >= 2 && x < 8 && y >= 2 && y < 8;
            const bool shadow = x >= 5 && x < 11 && y >= 4 && y < 10;
            CHECK(colorAt(image, x, y) == (content ? red : shadow ? blue : clear));
        }
    }
    // The shadow color's alpha scales the shadow's.
    square.shadow->color = Color{0, 0, 255, 128};
    const Image faint = render({16.0, 16.0, {square}}, 16, 16);
    CHECK(colorAt(faint, 9, 9) == Color{0, 0, 255, 128});
    CHECK(colorAt(faint, 6, 6) == red);
    // A square wholly left of the image casts its shadow into it.
    Layer outside = filled(rectangle({-6.0, 2.0}, {-1.0, 6.0}), red);
    outside.shadow = DropShadow{.offset = {4.0, 0.0}, .color = blue};
    const Image cast = render({16.0, 16.0, {outside}}, 16, 16);
    for (int x = 0; x < 16; ++x)
        CHECK(colorAt(cast, x, 4) == (x < 3 ? blue : clear));
}

TEST_CASE("A group's blur and shadow apply to its children's image as a whole") {
    // Two halves of a square, blurred and shadowed as a group, match the whole square blurred and shadowed alone. Each
    // half blurred alone would leave a seam where their blurred edges overlap.
    Layer whole = filled(rectangle({12.0, 12.0}, {28.0, 28.0}), red);
    whole.blur = 2.0;
    whole.shadow = DropShadow{.offset = {3.0, 2.0}, .deviation = 1.0, .color = blue};
    const Group halves{.children = {filled(rectangle({12.0, 12.0}, {20.0, 28.0}), red),
                                    filled(rectangle({20.0, 12.0}, {28.0, 28.0}), red)},
                       .blur = whole.blur,
                       .shadow = whole.shadow};
    const Image grouped = render({40.0, 40.0, {halves}}, 40, 40);
    CHECK(grouped.rgba == render({40.0, 40.0, {whole}}, 40, 40).rgba);
    // The match is of a blurred square over its shadow: whole in the middle, and more shadow than square past its
    // corner.
    CHECK(colorAt(grouped, 20, 20) == red);
    CHECK(pixel(grouped, 33, 33)[2] > pixel(grouped, 33, 33)[0]);
}

TEST_CASE("A node on a group's own surface reaches as far as its own blur and shadow carry it") {
    // A clip around the whole image puts the group on a surface of its own and leaves every pixel as it is, so the
    // group draws exactly what its blurred, shadowed square draws alone: its surface reaches the square's shadow, cast
    // far from the square, and its blurred edges, through the plain group between them.
    Layer square = filled(rectangle({4.0, 4.0}, {10.0, 10.0}), red);
    square.blur = 1.0;
    square.shadow = DropShadow{.offset = {14.0, 6.0}, .deviation = 1.0, .color = blue};
    const Group clipped{.children = {Group{.children = {square}}}, .clip = Clip{rectangle({0.0, 0.0}, {32.0, 24.0})}};
    const Image alone = render({32.0, 24.0, {square}}, 32, 24);
    CHECK(render({32.0, 24.0, {clipped}}, 32, 24).rgba == alone.rgba);
    // The match is of the square over its shadow: the shadow, blurred twice, shows nearly opaque at its center.
    CHECK(pixel(alone, 21, 13)[2] == 255);
    CHECK(pixel(alone, 21, 13)[3] > 200);
}

TEST_CASE("A drop shadow's edge falls with its standard deviation, centered at its offset") {
    // Content over the image left of pixel 32, at 4 pixels a unit, casts a shadow 5 units right with a deviation of 1
    // unit: an edge centered at pixel 52 that falls with a deviation of 4 pixels. The opaque content covers the rest
    // of the shadow, so this is the only edge in the row.
    Layer half = filled(rectangle({-40.0, -40.0}, {8.0, 72.0}), red);
    half.shadow = DropShadow{.offset = {5.0, 0.0}, .deviation = 1.0, .color = blue};
    const Image image = render({32.0, 32.0, {half}}, 128, 128);
    checkSpread(fall(rowAlpha(image, 64)), 52.0, 4.0);
    CHECK(colorAt(image, 20, 64) == red);
    CHECK(colorAt(image, 36, 64) == blue);
    // An offset of a fraction of a pixel moves the shadow exactly that far: an eighth of a unit is half a pixel.
    half.shadow->offset = {5.125, 0.0};
    const Spread nudged = fall(rowAlpha(render({32.0, 32.0, {half}}, 128, 128), 64));
    CHECK(std::abs(nudged.center - 52.5) <= centerTolerance);
}

TEST_CASE("A node's shadow falls from the node blurred, and lies beneath it") {
    // Content blurred by 1 unit, at 4 pixels a unit, over a hard shadow cast straight beneath it. Where the blurred
    // content's alpha is b, the shadow's is b too, and the content over it gives 1 - (1 - b)^2. A shadow of the content
    // before the blur would give 1 left of the edge, and blurring the content and its shadow together would give b.
    Layer half = filled(rectangle({-10.0, -10.0}, {8.0, 26.0}), red);
    half.blur = 1.0;
    const std::vector<double> blurred = rowAlpha(render({16.0, 16.0, {half}}, 64, 64), 32);
    half.shadow = DropShadow{.color = blue};
    const std::vector<double> shadowed = rowAlpha(render({16.0, 16.0, {half}}, 64, 64), 32);
    // Rounding b to 255 levels moves 1 - (1 - b)^2 at most a level, and rounding that adds half a level more.
    constexpr double rounding = 1.5 / byteMax;
    for (std::size_t x = 0; x < blurred.size(); ++x) {
        const double clearOfIt = 1.0 - blurred[x];
        CHECK(std::abs(shadowed[x] - (1.0 - clearOfIt * clearOfIt)) <= rounding);
    }
}

TEST_CASE("A shadow cast farther than any pixel it could reach draws nothing, and the node still draws") {
    const Layer square = filled(rectangle({2.0, 2.0}, {6.0, 6.0}), red);
    const Image plain = render({8.0, 8.0, {square}}, 8, 8);
    for (const double far :
         {1e300, -1e300, std::numeric_limits<double>::infinity(), std::numeric_limits<double>::quiet_NaN()}) {
        Layer shadowed = square;
        shadowed.shadow = DropShadow{.offset = {far, far}, .color = blue};
        CHECK(render({8.0, 8.0, {shadowed}}, 8, 8).rgba == plain.rgba);
    }
}
