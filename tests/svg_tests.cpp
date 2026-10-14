#include "linea/Shapes.hpp"
#include "linea/Svg.hpp"

#include <doctest/doctest.h>

#include <array>
#include <bit>
#include <clocale>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <numbers>
#include <optional>
#include <ostream>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

using namespace linea;

namespace {

constexpr Color black{0, 0, 0, 255};
constexpr Color red{255, 0, 0, 255};

// The path that @p data describes, which must hold no error.
Path pathOf(std::string_view data) {
    const SvgPathData read = fromSvgPathData(data);
    CAPTURE(data);
    CHECK_FALSE(read.error);
    return read.path;
}

// A document of @p width by @p height px around @p body.
std::string document(std::string_view body, std::string_view size = "width=\"100\" height=\"100\"") {
    return "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" " +
           std::string(size) + ">" + std::string(body) + "</svg>";
}

// The drawing that @p body describes in a 100 by 100 document, which must read without error.
Drawing read(std::string_view body) {
    const SvgDocument svg = fromSvg(document(body));
    CHECK(svg.errors.empty());
    return svg.drawing;
}

const Layer &layerAt(const std::vector<Node> &nodes, std::size_t i) {
    REQUIRE(i < nodes.size());
    REQUIRE(std::holds_alternative<Layer>(nodes[i].content));
    return std::get<Layer>(nodes[i].content);
}

const Group &groupAt(const std::vector<Node> &nodes, std::size_t i) {
    REQUIRE(i < nodes.size());
    REQUIRE(std::holds_alternative<Indirect<Group>>(nodes[i].content));
    return *std::get<Indirect<Group>>(nodes[i].content);
}

// The only layer that @p body draws.
Layer onlyLayer(std::string_view body) {
    const Drawing drawing = read(body);
    REQUIRE(drawing.nodes.size() == 1);
    return layerAt(drawing.nodes, 0);
}

// The first layer that @p body draws, inside groups or not.
Layer firstLayer(std::string_view body) {
    const Drawing drawing = read(body);
    const std::vector<Node> *nodes = &drawing.nodes;
    while (true) {
        REQUIRE_FALSE(nodes->empty());
        if (std::holds_alternative<Layer>(nodes->front().content))
            return std::get<Layer>(nodes->front().content);
        nodes = &std::get<Indirect<Group>>(nodes->front().content)->children;
    }
}

Color fillColor(std::string_view body) {
    const Layer layer = firstLayer(body);
    REQUIRE(layer.fill);
    REQUIRE(std::holds_alternative<Color>(*layer.fill));
    return std::get<Color>(*layer.fill);
}

Color colorOf(std::string_view fill) { return fillColor("<path d=\"M0 0\" fill=\"" + std::string(fill) + "\"/>"); }

void checkRefused(std::string_view body, std::string_view message) {
    CAPTURE(body);
    CHECK_THROWS_WITH_AS(static_cast<void>(fromSvg(document(body))), doctest::Contains(message.data()), SvgError);
}

void checkSameDrawing(const Drawing &drawing) {
    const std::string svg = toSvg(drawing);
    CAPTURE(svg);
    const SvgDocument back = fromSvg(svg);
    CHECK(back.errors.empty());
    CHECK(back.drawing == drawing);
    CHECK(toSvg(back.drawing) == svg);
}

} // namespace

// Path data.

TEST_CASE("Path data writes each verb as an absolute command and reads back as the same path") {
    Path path;
    path.moveTo({1.5, -2.0})
        .lineTo({0.1, 0.2})
        .quadTo({1e-10, 1e300}, {-0.0, 5e-324})
        .cubicTo({1.0 / 3.0, 2.0 / 3.0}, {0.1 + 0.2, 123456789.125}, {-7.0, 8.0})
        .close()
        .lineTo({9.0, 9.0})
        .moveTo({4.0, 4.0})
        .moveTo({5.0, 5.0});
    const std::string data = toSvgPathData(path);
    CHECK(data == "M1.5 -2 L0.1 0.2 Q1e-10 1e+300 0 5e-324 C0.3333333333333333 0.6666666666666666 0.30000000000000004 "
                  "123456789.125 -7 8 Z M1.5 -2 L9 9 M4 4 M5 5");
    CHECK(pathOf(data) == path);
    CHECK(toSvgPathData(Path{}).empty());
    Path infinite;
    infinite.moveTo({std::numeric_limits<double>::infinity(), 0.0});
    CHECK_THROWS_AS(static_cast<void>(toSvgPathData(infinite)), SvgError);
}

TEST_CASE("Numbers read correctly rounded, whatever the C locale's decimal point") {
    CHECK(pathOf("M0.1 2.2250738585072011e-308").points()[0] == Vec2{0.1, 2.2250738585072011e-308});
    // Halfway between two doubles, rounded to the even one.
    CHECK(pathOf("M9007199254740993 0").points()[0].x == 9007199254740992.0);
    CHECK(pathOf("M4.9406564584124654e-324 1e-400").points()[0] == Vec2{4.9406564584124654e-324, 0.0});
    CHECK(pathOf("M0.000000000000000000000000000001e30 1000000000000000000000e-21").points()[0] == Vec2{1.0, 1.0});
    // A locale that writes decimal commas changes strtod's decimal point, if the system has one.
    const std::string saved = std::setlocale(LC_NUMERIC, nullptr);
    for (const char *locale : {"de_DE.UTF-8", "de_DE.utf8", "fr_FR.UTF-8", "German_Germany.1252"}) {
        if (std::setlocale(LC_NUMERIC, locale) != nullptr) {
            CHECK(pathOf("M1.5 0.25").points()[0] == Vec2{1.5, 0.25});
            break;
        }
    }
    std::setlocale(LC_NUMERIC, saved.c_str());
}

TEST_CASE("A command repeats for more numbers without its letter, and a moveto's further pairs are linetos") {
    Path expected;
    expected.moveTo({0, 0}).lineTo({10, 0}).lineTo({10, 10}).lineTo({20, 10});
    CHECK(pathOf("M0 0 10 0 10 10 20 10") == expected);
    CHECK(pathOf("M0 0 L10 0 10 10 20 10") == expected);
    CHECK(pathOf("m0 0 10 0 0 10 10 0") == expected);
    CHECK(pathOf("M0,0,10,0,10,10,20,10") == expected);
    CHECK(pathOf("M0 0l10 0 0 10 10 0") == expected);
    // A relative moveto first is absolute, and its further pairs are relative.
    Path relative;
    relative.moveTo({5, 5}).lineTo({15, 15});
    CHECK(pathOf("m5 5 10 10") == relative);
    Path cubics;
    cubics.moveTo({0, 0}).cubicTo({1, 1}, {2, 2}, {3, 3}).cubicTo({4, 4}, {5, 5}, {6, 6});
    CHECK(pathOf("M0 0 C1 1 2 2 3 3 4 4 5 5 6 6") == cubics);
    CHECK(pathOf("M0 0 c1 1 2 2 3 3 1 1 2 2 3 3") == cubics);
}

TEST_CASE("Numbers take as many characters as still form a number") {
    CHECK(pathOf("M100-200").points()[0] == Vec2{100, -200});
    CHECK(pathOf("M0.6.5").points()[0] == Vec2{0.6, 0.5});
    CHECK(pathOf("M-.5-.5").points()[0] == Vec2{-0.5, -0.5});
    CHECK(pathOf("M1e-5 1E+2").points()[0] == Vec2{1e-5, 100.0});
    CHECK(pathOf("M+5 +.5e1").points()[0] == Vec2{5, 5});
    CHECK(pathOf("M1.5.5.5.5").points() == std::vector<Vec2>{{1.5, 0.5}, {0.5, 0.5}});
    CHECK(pathOf("M1e2.5 0 0").points() == std::vector<Vec2>{{100, 0.5}, {0, 0}});
    CHECK(pathOf("\t\n\f\r M 1 2 \t\n\f\r ").points()[0] == Vec2{1, 2});
}

TEST_CASE("Arc flags need no separators, and arcs follow Path::arcTo") {
    Path expected;
    expected.moveTo({0, 0}).arcTo({1, 1}, 0.0, false, false, {1, 1}).arcTo({2, 1}, 0.0, true, true, {5, 1});
    CHECK(pathOf("M0 0 a1 1 0 00 1 1 A2 1 0 115 1") == expected);
    CHECK(pathOf("M0 0 a1,1,0,0,0,1,1 2,1,0,1,1,4,0") == expected);
    // Negative radii count as their absolute values, as SVG 2 section 9.5.1 says.
    CHECK(pathOf("M0 0 a-1 -1 0 00 1 1 A-2 1 0 115 1") == expected);
    Path rotated;
    rotated.moveTo({0, 0}).arcTo({3, 1}, -30.0 * std::numbers::pi / 180.0, false, true, {4, 2});
    CHECK(pathOf("M0 0A3 1-30 0 1 4 2") == rotated);
    // A zero radius draws a line, and an arc to the current point draws nothing.
    Path line;
    line.moveTo({0, 0}).lineTo({4, 4});
    CHECK(pathOf("M0 0 A0 2 0 0 1 4 4 A2 2 0 0 1 4 4") == line);
}

TEST_CASE("Every command, absolute and relative, and S and T reflect the previous curve's control point") {
    Path lines;
    lines.moveTo({1, 1}).lineTo({5, 1}).lineTo({5, 4}).lineTo({7, 4}).lineTo({7, 2}).lineTo({8, 3});
    CHECK(pathOf("M1 1 H5 V4 h2 v-2 l1 1") == lines);
    // S after C reflects C's second control point; after anything else it starts at the current point.
    Path smooth;
    smooth.moveTo({0, 0}).cubicTo({0, 10}, {10, 10}, {10, 0}).cubicTo({10, -10}, {20, -10}, {20, 0});
    CHECK(pathOf("M0 0 C0 10 10 10 10 0 S20 -10 20 0") == smooth);
    CHECK(pathOf("M0 0 C0 10 10 10 10 0 s10 -10 10 0") == smooth);
    // A chain of Ss reflects each one's second control point in turn.
    Path chain = smooth;
    chain.cubicTo({20, 10}, {30, 10}, {30, 0});
    CHECK(pathOf("M0 0 C0 10 10 10 10 0 S20 -10 20 0 S30 10 30 0") == chain);
    Path unreflected;
    unreflected.moveTo({0, 0}).quadTo({5, 5}, {10, 0}).cubicTo({10, 0}, {20, -10}, {20, 0});
    CHECK(pathOf("M0 0 Q5 5 10 0 S20 -10 20 0") == unreflected);
    // T reflects the previous quadratic's control point, through a chain of Ts.
    Path quads;
    quads.moveTo({0, 0}).quadTo({5, 5}, {10, 0}).quadTo({15, -5}, {20, 0}).quadTo({25, 5}, {30, 0});
    CHECK(pathOf("M0 0 Q5 5 10 0 T20 0 T30 0") == quads);
    CHECK(pathOf("M0 0 q5 5 10 0 t10 0 10 0") == quads);
    Path plain;
    plain.moveTo({0, 0}).lineTo({1, 1}).quadTo({1, 1}, {2, 0});
    CHECK(pathOf("M0 0 L1 1 T2 0") == plain);
    // After closepath, a relative command counts from the subpath's start, where the next subpath starts.
    Path closed;
    closed.moveTo({1, 1}).lineTo({5, 1}).close().lineTo({1, 5}).close().moveTo({3, 3}).lineTo({4, 4});
    CHECK(pathOf("M1 1 L5 1 Z l0 4 z m2 2 1 1") == closed);
}

TEST_CASE("Path data draws up to the last whole segment before its first error, and reports the error") {
    const auto check = [](std::string_view data, std::string_view drawn, std::string_view problem, int column) {
        CAPTURE(data);
        const SvgPathData read = fromSvgPathData(data);
        CHECK(read.path == pathOf(drawn));
        REQUIRE(read.error);
        CHECK(std::string(read.error->what()).find(problem) != std::string::npos);
        CHECK(read.error->line() == 1);
        CHECK(read.error->column() == column);
    };
    // SVG 2 section 9.5.4's example: the line to (20,20) is drawn and the lone 30 is the error.
    check("M 10,10 L 20,20,30", "M10 10 L20 20", "expected a number", 19);
    check("L 10 10", "", "must start with a moveto", 1);
    check("M0 0 L10 10,", "M0 0 L10 10", "after the comma", 13);
    check("M0 0 L,10 10", "M0 0", "expected a number", 7);
    check("M0 0 L10 10 Z 5 5", "M0 0 L10 10 Z", "expected a command letter", 15);
    check("M0 0 L10 10 X", "M0 0 L10 10", "expected a command letter", 13);
    check("M0 0 A1 1 0 2 1 5 5", "M0 0", "arc flag", 13);
    check("M0 0 L1e999 0", "M0 0", "too large", 7);
    check("M5. 5", "", "expected a number", 3);
    check("M0 0,L1 1", "M0 0", "after the comma", 6);
    check("M0 0 C1 1 2 2 3 3 4 4", "M0 0 C1 1 2 2 3 3", "expected a number", 22);
    CHECK(fromSvgPathData("").path.empty());
    CHECK_FALSE(fromSvgPathData("").error);
    CHECK_FALSE(fromSvgPathData(" \n ").error);
    const SvgPathData multiline = fromSvgPathData("M0 0\nL1 1\r\n  L2");
    REQUIRE(multiline.error);
    CHECK(multiline.error->line() == 3);
    CHECK(multiline.error->column() == 5);
}

// Writing.

TEST_CASE("A drawing writes as path and g elements, with only what differs from SVG's initial values") {
    Layer square;
    square.path = rectangle({10, 10}, {30, 30});
    square.fill = Paint{black};
    Layer outline;
    outline.path = circle({50, 50}, 20);
    outline.fill = std::nullopt;
    outline.stroke = Paint{Color{0, 128, 255, 128}};
    outline.strokeStyle = {.width = 2.5, .join = Join::miter, .cap = Cap::butt, .dashArray = {4, 2}};
    Layer star;
    star.path = rectangle({0, 0}, {1, 1});
    star.fill = Paint{Color{255, 0, 0, 0}};
    star.fillRule = FillRule::evenOdd;
    star.strokeStyle = {.width = 3.0, .join = Join::bevel};
    star.transform = translate({5, 6});
    star.opacity = 0.25;
    Group group;
    group.children = {star, Group{}};
    group.transform = scale(2, 3);
    group.opacity = 0.5;
    Group turned;
    turned.transform = rotate(std::numbers::pi / 6.0);
    const Drawing drawing{100, 50.5, {square, outline, group, turned}};
    CHECK(toSvg(drawing) ==
          "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"100\" height=\"50.5\" viewBox=\"0 0 100 50.5\" "
          "preserveAspectRatio=\"none\">\n"
          "  <path d=\"M10 10 L30 10 L30 30 L10 30 Z\"/>\n"
          "  <path d=\"" +
              toSvgPathData(outline.path) +
              "\" fill=\"none\" stroke=\"#0080ff\" stroke-opacity=\"0.5\" stroke-width=\"2.5\" "
              "stroke-dasharray=\"4 2\"/>\n"
              "  <g transform=\"scale(2 3)\" opacity=\"0.5\">\n"
              "    <path transform=\"translate(5 6)\" opacity=\"0.25\" d=\"M0 0 L1 0 L1 1 L0 1 Z\" fill=\"#ff0000\" "
              "fill-opacity=\"0\" fill-rule=\"evenodd\" stroke-width=\"3\" stroke-linejoin=\"bevel\"/>\n"
              "    <g/>\n"
              "  </g>\n"
              "  <g transform=\"matrix(0.8660254037844387 0.49999999999999994 -0.49999999999999994 "
              "0.8660254037844387 0 0)\"/>\n"
              "</svg>\n");
    CHECK(toSvg(Drawing{1, 2, {}}) == "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"2\" "
                                      "viewBox=\"0 0 1 2\" preserveAspectRatio=\"none\"/>\n");
}

TEST_CASE("A translucent color writes its alpha as the shortest opacity that reads back as the same byte") {
    for (int alpha = 0; alpha <= 255; ++alpha) {
        Layer layer;
        layer.path = rectangle({0, 0}, {1, 1});
        layer.fill = Paint{Color{1, 2, 3, static_cast<std::uint8_t>(alpha)}};
        layer.stroke = Paint{Color{4, 5, 6, static_cast<std::uint8_t>(255 - alpha)}};
        const Drawing drawing{1, 1, {layer}};
        CAPTURE(alpha);
        checkSameDrawing(drawing);
        const std::string svg = toSvg(drawing);
        const std::size_t at = svg.find("fill-opacity=\"");
        if (alpha == 255) {
            CHECK(at == std::string::npos);
        } else {
            // Three decimals always suffice, so an opacity is at most 0.xyz.
            REQUIRE(at != std::string::npos);
            CHECK(svg.find('"', at + 14) - (at + 14) <= 5);
        }
    }
    CHECK(toSvg(Drawing{1, 1, {Layer{.path = {}, .fill = Paint{Color{0, 0, 0, 128}}}}}).find("fill-opacity=\"0.5\"") !=
          std::string::npos);
}

TEST_CASE("Writing refuses numbers that are not finite and values that SVG holds invalid") {
    const double nan = std::numeric_limits<double>::quiet_NaN();
    const double infinity = std::numeric_limits<double>::infinity();
    const auto refused = [](const Drawing &drawing, std::string_view message) {
        CHECK_THROWS_WITH_AS(static_cast<void>(toSvg(drawing)), doctest::Contains(message.data()), SvgError);
    };
    refused(Drawing{nan, 1, {}}, "width is not finite");
    refused(Drawing{1, infinity, {}}, "height is not finite");
    refused(Drawing{-1, 1, {}}, "must not be negative");
    Layer layer;
    layer.path = rectangle({0, 0}, {1, 1});
    const auto with = [&](auto change) {
        Layer changed = layer;
        change(changed);
        return Drawing{1, 1, {changed}};
    };
    refused(with([&](Layer &l) { l.opacity = nan; }), "opacity is not finite");
    refused(with([&](Layer &l) { l.transform.e = infinity; }), "transform is not finite");
    refused(with([&](Layer &l) { l.path.lineTo({nan, 0}); }), "not finite");
    refused(with([&](Layer &l) { l.strokeStyle.width = -1; }), "stroke width is negative");
    refused(with([&](Layer &l) { l.strokeStyle.width = nan; }), "stroke width is not finite");
    refused(with([&](Layer &l) { l.strokeStyle.miterLimit = 0.5; }), "miter limit is below 1");
    refused(with([&](Layer &l) { l.strokeStyle.dashArray = {1, -1}; }), "dash length is negative");
    refused(with([&](Layer &l) { l.strokeStyle.dashArray = {1, infinity}; }), "dash length is not finite");
    refused(with([&](Layer &l) { l.strokeStyle.dashOffset = nan; }), "dash offset is not finite");
    Group group;
    group.opacity = nan;
    refused(Drawing{1, 1, {group}}, "opacity is not finite");
}

TEST_CASE("Writing refuses each effect it does not write, naming the field, rather than drop it") {
    const auto refused = [](const Node &node, std::string_view field) {
        CHECK_THROWS_WITH_AS(static_cast<void>(toSvg(Drawing{10, 10, {node}})), doctest::Contains(field.data()),
                             SvgError);
    };
    const DropShadow shadow{{1, 1}, 2.0, black};
    const Clip clip{rectangle({0, 0}, {5, 5}), FillRule::nonZero};
    Layer layer;
    layer.path = rectangle({0, 0}, {10, 10});
    refused(Layer{.path = layer.path, .blur = 1.0}, "Layer::blur");
    refused(Layer{.path = layer.path, .shadow = shadow}, "Layer::shadow");
    refused(Layer{.path = layer.path, .clip = clip}, "Layer::clip");
    refused(Group{.children = {layer}, .blur = 0.5}, "Group::blur");
    refused(Group{.children = {layer}, .shadow = shadow}, "Group::shadow");
    refused(Group{.children = {layer}, .clip = clip}, "Group::clip");
    refused(Group{.children = {layer}, .mask = Mask{{layer}, MaskMode::luminance}}, "Group::mask");
    // A node deep inside a group is refused too.
    refused(Group{.children = {Group{.children = {Layer{.path = layer.path, .blur = 2.0}}}}}, "Layer::blur");
}

TEST_CASE("A written document is ASCII from a fixed set of characters, and the same for the same drawing") {
    Layer layer;
    layer.path = star({50, 50}, 40, 20, 5, -std::numbers::pi / 2.0);
    layer.fill = Paint{Color{12, 34, 56, 78}};
    layer.stroke = Paint{red};
    layer.transform = rotateAround(0.3, {50, 50});
    const Drawing drawing{100, 100, {layer, Group{{layer}, scale(0.5), 0.75}}};
    const std::string svg = toSvg(drawing);
    CHECK(svg == toSvg(drawing));
    for (const char c : svg) {
        const bool allowed = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
                             std::string_view(" \n<>/=\"#.:-+()").find(c) != std::string_view::npos;
        CAPTURE(c);
        CHECK(allowed);
    }
}

// Writing and reading back.

TEST_CASE("Writing a drawing and reading it back gives the same drawing, exactly") {
    Layer filled;
    filled.path = star({50, 50}, 40, 15, 7, 0.1);
    filled.fill = Paint{Color{200, 100, 50, 255}};
    Layer stroked;
    stroked.path.moveTo({1, 2}).quadTo({3, 4}, {5, 6}).cubicTo({7, 8}, {9, 10}, {11, 12});
    stroked.fill = std::nullopt;
    stroked.stroke = Paint{Color{0, 0, 0, 1}};
    stroked.strokeStyle = {.width = 0.1,
                           .join = Join::miter,
                           .cap = Cap::square,
                           .miterLimit = 10,
                           .dashArray = {1, 0.5, 2},
                           .dashOffset = -3.25};
    // A layer without a stroke keeps a stroke style of its own, unlike linea's default.
    Layer styled;
    styled.path = rectangle({0, 0}, {3, 3});
    styled.strokeStyle = {
        .width = 7, .join = Join::miter, .cap = Cap::butt, .miterLimit = 2, .dashArray = {1}, .dashOffset = 1};
    Layer empty;
    Group nested;
    nested.children = {filled, Group{{stroked, Group{}}, skewX(0.2) * rotate(1.0), 0.3}, styled};
    nested.transform = {1.5, -0.25, 0.75, 2.0, -10.0, 20.0};
    nested.opacity = 1.5;
    checkSameDrawing(Drawing{120, 80, {nested, empty, filled}});
    checkSameDrawing(Drawing{0, 0, {filled}});
    checkSameDrawing(Drawing{});
    for (const Join join : {Join::miter, Join::round, Join::bevel})
        for (const Cap cap : {Cap::butt, Cap::round, Cap::square})
            for (const bool withStroke : {false, true}) {
                // Set after the layer is built: GCC 13 at -O3 warns, falsely, that a layer built with a conditional
                // stroke may be moved from uninitialized.
                Layer layer{.path = rectangle({1, 1}, {9, 9}), .strokeStyle = {.join = join, .cap = cap}};
                if (withStroke)
                    layer.stroke = Paint{red};
                checkSameDrawing(Drawing{10, 10, {layer}});
            }
}

TEST_CASE("Drawings of random numbers write and read back exactly") {
    std::mt19937_64 random(20261014);
    // Any finite double, from its bits, and doubles of everyday size.
    const auto anyDouble = [&] {
        while (true) {
            const double value = std::bit_cast<double>(random());
            if (std::isfinite(value))
                return value;
        }
    };
    const auto everyday = [&] { return std::uniform_real_distribution<double>(-1000.0, 1000.0)(random); };
    const auto number = [&] { return random() % 2 == 0 ? anyDouble() : everyday(); };
    const auto color = [&] {
        const std::uint64_t bits = random();
        return Color{static_cast<std::uint8_t>(bits), static_cast<std::uint8_t>(bits >> 8),
                     static_cast<std::uint8_t>(bits >> 16), static_cast<std::uint8_t>(bits >> 24)};
    };
    for (int round = 0; round < 200; ++round) {
        Layer layer;
        layer.path.moveTo({number(), number()})
            .lineTo({number(), number()})
            .quadTo({number(), number()}, {number(), number()})
            .cubicTo({number(), number()}, {number(), number()}, {number(), number()});
        if (random() % 2 == 0)
            layer.path.close();
        layer.fill = random() % 3 == 0 ? std::nullopt : std::optional<Paint>(Paint{color()});
        layer.stroke = random() % 3 == 0 ? std::nullopt : std::optional<Paint>(Paint{color()});
        layer.strokeStyle.width = std::abs(number());
        layer.strokeStyle.miterLimit = 1.0 + std::abs(everyday());
        layer.strokeStyle.dashArray = {std::abs(number()), std::abs(number())};
        layer.strokeStyle.dashOffset = number();
        layer.transform = {number(), number(), number(), number(), number(), number()};
        layer.opacity = number();
        Group group{{layer}, {number(), number(), number(), number(), number(), number()}, number()};
        CAPTURE(round);
        checkSameDrawing(Drawing{std::abs(number()), std::abs(number()), {group, layer}});
    }
}

// Reading: the document.

TEST_CASE("The svg element's width, height, viewBox and preserveAspectRatio size the drawing and place its content") {
    const auto contentTransform = [](std::string_view size) {
        const SvgDocument svg = fromSvg(document("<path d=\"M0 0\"/>", size));
        REQUIRE(svg.drawing.nodes.size() == 1);
        if (std::holds_alternative<Layer>(svg.drawing.nodes[0].content))
            return identity();
        return groupAt(svg.drawing.nodes, 0).transform;
    };
    const Drawing plain = fromSvg(document("", "width=\"20\" height=\"1in\"")).drawing;
    CHECK(plain.width == 20.0);
    CHECK(plain.height == 96.0);
    CHECK(contentTransform("width=\"100\" height=\"50\" viewBox=\"0 0 100 50\"") == identity());
    CHECK(contentTransform("width=\"200\" height=\"100\" viewBox=\"10 20 100 50\"") ==
          translate({-20, -40}) * scale(2.0));
    // Uneven scales meet in the middle by default, or slice, or stretch with none.
    CHECK(contentTransform("width=\"200\" height=\"100\" viewBox=\"0 0 100 100\"") == translate({50, 0}) * scale(1.0));
    CHECK(contentTransform("width=\"200\" height=\"100\" viewBox=\"0 0 100 100\" preserveAspectRatio=\"xMinYMax\"") ==
          scale(1.0));
    CHECK(contentTransform("width=\"200\" height=\"100\" viewBox=\"0 0 100 100\" "
                           "preserveAspectRatio=\"xMaxYMin meet\"") == translate({100, 0}) * scale(1.0));
    CHECK(contentTransform("width=\"200\" height=\"100\" viewBox=\"0 0 100 100\" "
                           "preserveAspectRatio=\"xMidYMid slice\"") == translate({0, -50}) * scale(2.0));
    CHECK(contentTransform("width=\"200\" height=\"100\" viewBox=\"0 0 100 100\" preserveAspectRatio=\"none\"") ==
          scale(2.0, 1.0));
    // Without a width or height, or with a percentage, the viewBox gives the size, keeping its aspect ratio.
    const Drawing boxed = fromSvg(document("", "viewBox=\"0 0 40 30\"")).drawing;
    CHECK((boxed.width == 40.0 && boxed.height == 30.0));
    const Drawing half = fromSvg(document("", "width=\"80\" height=\"100%\" viewBox=\"0 0 40 30\"")).drawing;
    CHECK((half.width == 80.0 && half.height == 60.0));
    CHECK_THROWS_WITH_AS(static_cast<void>(fromSvg(document("", "width=\"1\""))),
                         doctest::Contains("needs a width and a height, or a viewBox"), SvgError);
    CHECK_THROWS_WITH_AS(static_cast<void>(fromSvg(document("", "width=\"-1\" height=\"1\""))),
                         doctest::Contains("width"), SvgError);
    CHECK_THROWS_WITH_AS(static_cast<void>(fromSvg(document("", "viewBox=\"0 0 1\""))), doctest::Contains("viewBox"),
                         SvgError);
    CHECK(fromSvg(document("<path d=\"M0 0\"/>", "width=\"1\" height=\"1\" display=\"none\"")).drawing.nodes.empty());
    // The root's opacity fades the whole drawing.
    const Drawing faded = fromSvg(document("<path d=\"M0 0\"/>", "width=\"1\" height=\"1\" opacity=\"0.5\"")).drawing;
    CHECK(groupAt(faded.nodes, 0).opacity == 0.5);
}

TEST_CASE("The root must be svg in the SVG namespace") {
    CHECK_THROWS_WITH_AS(static_cast<void>(fromSvg("<svg width=\"1\" height=\"1\"/>")),
                         doctest::Contains("SVG namespace"), SvgError);
    CHECK_THROWS_WITH_AS(static_cast<void>(fromSvg("<g xmlns=\"http://www.w3.org/2000/svg\"/>")),
                         doctest::Contains("root element must be svg"), SvgError);
    const Drawing prefixed =
        fromSvg("<s:svg xmlns:s=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\"><s:path d=\"M0 0 L1 1\"/>"
                "</s:svg>")
            .drawing;
    CHECK(prefixed.nodes.size() == 1);
}

// Reading: shapes.

TEST_CASE("Basic shapes read as the paths SVG 2 makes of them") {
    Path rect;
    rect.moveTo({10, 20}).lineTo({40, 20}).lineTo({40, 60}).lineTo({10, 60}).lineTo({10, 20}).close();
    CHECK(onlyLayer("<rect x=\"10\" y=\"20\" width=\"30\" height=\"40\"/>").path == rect);
    Path rounded;
    rounded.moveTo({12, 20})
        .lineTo({38, 20})
        .arcTo({2, 3}, 0.0, false, true, {40, 23})
        .lineTo({40, 57})
        .arcTo({2, 3}, 0.0, false, true, {38, 60})
        .lineTo({12, 60})
        .arcTo({2, 3}, 0.0, false, true, {10, 57})
        .lineTo({10, 23})
        .arcTo({2, 3}, 0.0, false, true, {12, 20})
        .close();
    CHECK(onlyLayer("<rect x=\"10\" y=\"20\" width=\"30\" height=\"40\" rx=\"2\" ry=\"3\"/>").path == rounded);
    // One radius given stands for both, and each is at most half its own side, so the ends are half ellipses.
    Path even;
    even.moveTo({5, 0})
        .arcTo({5, 2}, 0.0, false, true, {10, 2})
        .arcTo({5, 2}, 0.0, false, true, {5, 4})
        .arcTo({5, 2}, 0.0, false, true, {0, 2})
        .arcTo({5, 2}, 0.0, false, true, {5, 0})
        .close();
    CHECK(onlyLayer("<rect width=\"10\" height=\"4\" ry=\"5\"/>").path == even);
    CHECK(onlyLayer("<rect width=\"10\" height=\"4\" rx=\"auto\" ry=\"5\"/>").path == even);
    CHECK(onlyLayer("<rect width=\"0\" height=\"4\"/>").path.empty());
    CHECK(onlyLayer("<rect width=\"10\"/>").path.empty());
    Path circlePath;
    circlePath.moveTo({15, 20})
        .arcTo({5, 5}, 0.0, false, true, {10, 25})
        .arcTo({5, 5}, 0.0, false, true, {5, 20})
        .arcTo({5, 5}, 0.0, false, true, {10, 15})
        .arcTo({5, 5}, 0.0, false, true, {15, 20})
        .close();
    CHECK(onlyLayer("<circle cx=\"10\" cy=\"20\" r=\"5\"/>").path == circlePath);
    CHECK(onlyLayer("<ellipse cx=\"10\" cy=\"20\" rx=\"5\"/>").path == circlePath);
    CHECK(onlyLayer("<ellipse cx=\"10\" cy=\"20\" rx=\"auto\" ry=\"5\"/>").path == circlePath);
    CHECK(onlyLayer("<circle r=\"0\"/>").path.empty());
    Path line;
    line.moveTo({1, 2}).lineTo({3, 4});
    CHECK(onlyLayer("<line x1=\"1\" y1=\"2\" x2=\"3\" y2=\"4\"/>").path == line);
    Path polyline;
    polyline.moveTo({1, 2}).lineTo({3, 4}).lineTo({5, 6});
    CHECK(onlyLayer("<polyline points=\"1,2 3,4 5-6e0\"/>").path == pathOf("M1 2 L3 4 L5 -6"));
    CHECK(onlyLayer("<polyline points=\"1 2,3 4 5 6\"/>").path == polyline);
    CHECK(onlyLayer("<polygon points=\"1 2 3 4 5 6\"/>").path == Path(polyline).close());
    CHECK(onlyLayer("<path/>").path.empty());
    CHECK(onlyLayer("<path d=\"none\"/>").path.empty());
    checkRefused("<rect width=\"-1\" height=\"1\"/>", "width");
    checkRefused("<circle r=\"-1\"/>", "r:");
}

TEST_CASE("Errors in path data and points are drawn past as SVG specifies, and reported in place") {
    const SvgDocument svg = fromSvg(document("<path d=\"M10 10 L20 20 L30\"/>\n<polygon points=\"1 2 3 4 5\"/>"
                                             "<polyline points=\"1 2 3 x\"/>"));
    REQUIRE(svg.errors.size() == 3);
    CHECK(layerAt(svg.drawing.nodes, 0).path == pathOf("M10 10 L20 20"));
    CHECK(layerAt(svg.drawing.nodes, 1).path == pathOf("M1 2 L3 4 Z"));
    CHECK(layerAt(svg.drawing.nodes, 2).path == pathOf("M1 2"));
    CHECK(std::string(svg.errors[0].what()).find("<path>: d: expected a number, at character 18") != std::string::npos);
    CHECK(std::string(svg.errors[1].what()).find("<polygon>: points: an odd number of coordinates") !=
          std::string::npos);
    CHECK(svg.errors[1].line() == 2);
    CHECK(svg.errors[1].column() == 1);
    CHECK(std::string(svg.errors[2].what()).find("<polyline>: points: expected a number") != std::string::npos);
}

TEST_CASE("Lengths take absolute units at 96 px to the inch, and percentages of the viewport, and refuse em") {
    const auto width = [](std::string_view value) {
        const Layer layer = onlyLayer("<rect width=\"" + std::string(value) + "\" height=\"1\"/>");
        return layer.path.points()[1].x;
    };
    CHECK(width("3") == 3.0);
    CHECK(width("3px") == 3.0);
    CHECK(width("1in") == 96.0);
    CHECK(width("2.54cm") == doctest::Approx(96.0).epsilon(1e-15));
    CHECK(width("25.4mm") == doctest::Approx(96.0).epsilon(1e-15));
    CHECK(width("101.6Q") == doctest::Approx(96.0).epsilon(1e-15));
    CHECK(width("72pt") == 96.0);
    CHECK(width("6pc") == 96.0);
    CHECK(width("1IN") == 96.0);
    CHECK(width("25%") == 25.0);
    checkRefused("<rect width=\"2em\" height=\"1\"/>", "em units are not supported");
    checkRefused("<rect width=\"2ex\" height=\"1\"/>", "ex units are not supported");
    checkRefused("<rect width=\"calc(1px)\" height=\"1\"/>", "width");
    // Percentages are of the viewBox: x and widths of its width, y and heights of its height, and other lengths of its
    // normalized diagonal.
    const Drawing drawing = fromSvg(document("<rect x=\"10%\" y=\"10%\" width=\"50%\" height=\"50%\"/>"
                                             "<circle r=\"10%\"/><path d=\"M0 0\" stroke=\"red\" stroke-width=\"1%\"/>",
                                             "width=\"1\" height=\"1\" viewBox=\"0 0 300 400\""))
                                .drawing;
    const Group &content = groupAt(drawing.nodes, 0);
    CHECK(layerAt(content.children, 0).path.points()[0] == Vec2{30, 40});
    CHECK(layerAt(content.children, 0).path.points()[2] == Vec2{180, 240});
    const double diagonal = std::sqrt((300.0 * 300.0 + 400.0 * 400.0) / 2.0);
    CHECK(layerAt(content.children, 1).path.points()[0].x == doctest::Approx(diagonal / 10.0));
    CHECK(layerAt(content.children, 2).strokeStyle.width == doctest::Approx(diagonal / 100.0));
}

TEST_CASE("Transforms compose in the order they are written, the first applied last") {
    const auto transformOf = [](std::string_view value) {
        return onlyLayer("<path d=\"M0 0\" transform=\"" + std::string(value) + "\"/>").transform;
    };
    CHECK(transformOf("translate(10,20) scale(2)") == translate({10, 20}) * scale(2.0));
    CHECK(transformOf("translate(10) , scale(2 3)") == translate({10, 0}) * scale(2.0, 3.0));
    CHECK(transformOf("matrix(1 2 3 4 5 6)rotate(90)") == Affine{1, 2, 3, 4, 5, 6} * rotate(std::numbers::pi / 2.0));
    CHECK(transformOf("rotate(45 10 20)") == rotateAround(std::numbers::pi / 4.0, {10, 20}));
    CHECK(transformOf("skewX(30) skewY(-30)") == skewX(std::numbers::pi / 6.0) * skewY(-std::numbers::pi / 6.0));
    CHECK(transformOf("translate(.5.5)") == translate({0.5, 0.5}));
    CHECK(transformOf("") == identity());
    checkRefused("<path d=\"M0 0\" transform=\"rotate(45deg)\"/>", "transform");
    checkRefused("<path d=\"M0 0\" transform=\"translate(1 2 3)\"/>", "transform");
    checkRefused("<path d=\"M0 0\" transform=\"scale(1),\"/>", "transform");
    checkRefused("<path d=\"M0 0\" transform=\"perspective(10)\"/>", "transform");
}

// Reading: paint and style.

TEST_CASE("Every CSS Color 4 syntax for an sRGB color reads as its bytes") {
    CHECK(colorOf("#f80") == Color{255, 136, 0, 255});
    CHECK(colorOf("#f808") == Color{255, 136, 0, 136});
    CHECK(colorOf("#FF8000") == Color{255, 128, 0, 255});
    CHECK(colorOf("#ff800080") == Color{255, 128, 0, 128});
    CHECK(colorOf("RebeccaPurple") == Color{102, 51, 153, 255});
    CHECK(colorOf("transparent") == Color{0, 0, 0, 0});
    CHECK(colorOf("rgb(255, 128, 0)") == Color{255, 128, 0, 255});
    CHECK(colorOf("rgb(100%, 50%, 0%)") == Color{255, 128, 0, 255});
    CHECK(colorOf("rgba(255, 128, 0, 0.5)") == Color{255, 128, 0, 128});
    CHECK(colorOf("rgb(255,128,0,50%)") == Color{255, 128, 0, 128});
    CHECK(colorOf("RGB(300, -5, 127.5)") == Color{255, 0, 128, 255});
    CHECK(colorOf("rgb(255 128 0)") == Color{255, 128, 0, 255});
    CHECK(colorOf("rgb(100% 128 none / 25%)") == Color{255, 128, 0, 64});
    CHECK(colorOf("rgba(255 128 0 / .5)") == Color{255, 128, 0, 128});
    CHECK(colorOf("hsl(120, 100%, 25%)") == Color{0, 128, 0, 255});
    CHECK(colorOf("hsla(120, 100%, 25%, 0.5)") == Color{0, 128, 0, 128});
    CHECK(colorOf("hsl(120deg 100% 25%)") == Color{0, 128, 0, 255});
    CHECK(colorOf("hsl(0.33333333333turn 100 25 / 1)") == Color{0, 128, 0, 255});
    CHECK(colorOf("hsl(133.33333333grad 100% 25%)") == Color{0, 128, 0, 255});
    CHECK(colorOf("hsl(2.0943951023931953rad 100% 25%)") == Color{0, 128, 0, 255});
    CHECK(colorOf("hwb(120 0% 49.8%)") == Color{0, 128, 0, 255});
    CHECK(colorOf("hwb(0 100% 100%)") == Color{128, 128, 128, 255});
    // hwb(30 20% 10%) mixes white and black into hsl(30 100% 50%), which is (1, 0.5, 0): 0.7 of it plus 0.2.
    CHECK(colorOf("hwb(30 20% 10% / 0.5)") == Color{230, 140, 51, 128});
    CHECK(colorOf("color(srgb 1 0.5 0)") == Color{255, 128, 0, 255});
    CHECK(colorOf("color(srgb 100% 50% 0% / 0.5)") == Color{255, 128, 0, 128});
    CHECK(colorOf("color(srgb 2 -1 none)") == Color{255, 0, 0, 255});
    for (const std::string_view invalid :
         {"#ff000", "rgb(255, 50%, 0)", "rgb(255 128 0, 1)", "rgb(255, 128)", "hsl(120 100% 25% 1)", "hwb(0, 0%, 0%)",
          "notacolor", "rgb(255 128 0 / 1 / 1)"}) {
        CAPTURE(invalid);
        checkRefused("<path d=\"M0 0\" fill=\"" + std::string(invalid) + "\"/>", "is not a value that linea reads");
    }
    checkRefused("<path d=\"M0 0\" fill=\"oklch(0.7 0.1 30)\"/>", "oklch() colors are not supported");
    checkRefused("<path d=\"M0 0\" fill=\"lab(50 20 30)\"/>", "lab() colors are not supported");
    checkRefused("<path d=\"M0 0\" fill=\"color(display-p3 1 0 0)\"/>", "color(display-p3 ...) is not supported");
    checkRefused("<path d=\"M0 0\" fill=\"rgb(from red r g b)\"/>", "relative colors");
    checkRefused("<path d=\"M0 0\" style=\"fill: var(--accent)\"/>", "var() is not supported");
}

TEST_CASE("currentColor takes the color property, which inherits, and fill-opacity and stroke-opacity apply") {
    CHECK(fillColor("<g color=\"red\"><path d=\"M0 0\" fill=\"currentColor\"/></g>") == red);
    // currentColor inherits as a keyword, so each element takes its own color.
    CHECK(fillColor("<g fill=\"currentColor\" color=\"red\"><path d=\"M0 0\" color=\"#00f\"/></g>") ==
          Color{0, 0, 255, 255});
    // currentColor as the color property's own value is the parent's color.
    CHECK(fillColor("<g color=\"#0f0\"><path d=\"M0 0\" color=\"currentcolor\" fill=\"currentColor\"/></g>") ==
          Color{0, 255, 0, 255});
    CHECK(fillColor("<path d=\"M0 0\" fill=\"currentColor\"/>") == black);
    CHECK(fillColor("<path d=\"M0 0\" fill=\"red\" fill-opacity=\"0.5\"/>") == Color{255, 0, 0, 128});
    CHECK(fillColor("<path d=\"M0 0\" fill=\"#ff000080\" fill-opacity=\"50%\"/>") == Color{255, 0, 0, 64});
    CHECK(fillColor("<path d=\"M0 0\" fill=\"red\" fill-opacity=\"2\"/>") == red);
    const Layer stroked = onlyLayer("<path d=\"M0 0\" stroke=\"blue\" stroke-opacity=\"0.25\"/>");
    REQUIRE(stroked.stroke);
    CHECK(std::get<Color>(*stroked.stroke) == Color{0, 0, 255, 64});
    // A reference that does not resolve falls back to its fallback, or to none.
    CHECK(fillColor("<path d=\"M0 0\" fill=\"url(#missing) red\"/>") == red);
    CHECK_FALSE(onlyLayer("<path d=\"M0 0\" fill=\"url(#missing)\"/>").fill);
    CHECK(fillColor("<path id=\"p\" d=\"M0 0\" fill=\"url('#p') currentColor\" color=\"red\"/>") == red);
    checkRefused("<path d=\"M0 0\" fill=\"url(other.svg#a)\"/>", "references to other documents");
}

TEST_CASE("Properties inherit as SVG inherits them, and a style attribute's declarations take precedence") {
    const Drawing drawing =
        read("<g fill=\"red\" stroke=\"blue\" stroke-width=\"3\" stroke-linejoin=\"bevel\" fill-rule=\"evenodd\" "
             "opacity=\"0.5\"><path d=\"M0 0\"/><path d=\"M0 0\" style=\"fill:#0f0 !important;stroke-width:4px; "
             "/* a comment; with a semicolon */ stroke: inherit\" fill=\"black\"/><path d=\"M0 0\" fill=\"initial\" "
             "fill-rule=\"unset\" opacity=\"inherit\"/></g>");
    const Group &group = groupAt(drawing.nodes, 0);
    CHECK(group.opacity == 0.5);
    const Layer &plain = layerAt(group.children, 0);
    CHECK(std::get<Color>(*plain.fill) == red);
    CHECK(std::get<Color>(*plain.stroke) == Color{0, 0, 255, 255});
    CHECK(plain.strokeStyle.width == 3.0);
    CHECK(plain.strokeStyle.join == Join::bevel);
    CHECK(plain.strokeStyle.cap == Cap::butt);
    CHECK(plain.fillRule == FillRule::evenOdd);
    // Opacity is not inherited.
    CHECK(plain.opacity == 1.0);
    const Layer &styled = layerAt(group.children, 1);
    CHECK(std::get<Color>(*styled.fill) == Color{0, 255, 0, 255});
    CHECK(styled.strokeStyle.width == 4.0);
    CHECK(std::get<Color>(*styled.stroke) == Color{0, 0, 255, 255});
    const Layer &keywords = layerAt(group.children, 2);
    CHECK(std::get<Color>(*keywords.fill) == black);
    CHECK(keywords.fillRule == FillRule::evenOdd);
    CHECK(keywords.opacity == 0.5);
    // A layer without a stroke keeps linea's default style, but for what the document sets.
    CHECK(onlyLayer("<path d=\"M0 0\"/>").strokeStyle == StrokeStyle{});
    CHECK(onlyLayer("<path d=\"M0 0\" stroke-linecap=\"square\"/>").strokeStyle == StrokeStyle{.cap = Cap::square});
    checkRefused("<path d=\"M0 0\" style=\"fill\"/>", "is not a declaration");
    checkRefused("<path d=\"M0 0\" style=\"fill: url(&quot;#a)\"/>", "not closed");
    checkRefused("<path d=\"M0 0\" stroke-width=\"-1\"/>", "stroke-width");
    checkRefused("<path d=\"M0 0\" stroke-miterlimit=\"0.5\"/>", "stroke-miterlimit");
    checkRefused("<path d=\"M0 0\" stroke-dasharray=\"1 -1\"/>", "stroke-dasharray");
    checkRefused("<path d=\"M0 0\" stroke-linejoin=\"arcs\"/>", "arcs is not supported");
    checkRefused("<path d=\"M0 0\" opacity=\"half\"/>", "opacity");
}

TEST_CASE("Stroke properties read as SVG writes them") {
    const Layer layer = onlyLayer("<path d=\"M0 0\" stroke=\"red\" stroke-width=\"2\" stroke-linejoin=\"round\" "
                                  "stroke-linecap=\"square\" stroke-miterlimit=\"8\" stroke-dasharray=\"1,2 3%\" "
                                  "stroke-dashoffset=\"-1in\"/>");
    CHECK(layer.strokeStyle.width == 2.0);
    CHECK(layer.strokeStyle.join == Join::round);
    CHECK(layer.strokeStyle.cap == Cap::square);
    CHECK(layer.strokeStyle.miterLimit == 8.0);
    REQUIRE(layer.strokeStyle.dashArray.size() == 3);
    CHECK(layer.strokeStyle.dashArray[2] == doctest::Approx(3.0));
    CHECK(layer.strokeStyle.dashOffset == -96.0);
    CHECK(onlyLayer("<path d=\"M0 0\" stroke=\"red\" stroke-dasharray=\"none\"/>").strokeStyle ==
          StrokeStyle{.width = 1.0, .join = Join::miter, .cap = Cap::butt});
}

TEST_CASE("display none and visibility hidden draw nothing, and visibility can show a child again") {
    const Drawing drawing =
        read("<g display=\"none\"><path d=\"M0 0\"/></g><path d=\"M0 0\" style=\"display:none\"/>"
             "<g visibility=\"hidden\"><path d=\"M1 1\"/><path d=\"M2 2\" visibility=\"visible\"/></g>"
             "<path d=\"M3 3\" display=\"inline\"/>");
    REQUIRE(drawing.nodes.size() == 2);
    const Group &hidden = groupAt(drawing.nodes, 0);
    REQUIRE(hidden.children.size() == 1);
    CHECK(layerAt(hidden.children, 0).path == pathOf("M2 2"));
    CHECK(layerAt(drawing.nodes, 1).path == pathOf("M3 3"));
}

TEST_CASE("use places a copy of what it refers to, which inherits from the use element") {
    const Drawing drawing =
        read("<defs><path id=\"dot\" d=\"M0 0 L1 1\"/></defs><g fill=\"red\"><path id=\"shape\" d=\"M5 5\"/></g>"
             "<use href=\"#dot\" x=\"10\" y=\"20\" transform=\"scale(2)\" fill=\"blue\" opacity=\"0.5\"/>"
             "<use xlink:href=\"#shape\"/><use href=\"#missing\"/><use/>");
    REQUIRE(drawing.nodes.size() == 5);
    const Group &placed = groupAt(drawing.nodes, 1);
    CHECK(placed.transform == scale(2.0) * translate({10, 20}));
    CHECK(placed.opacity == 0.5);
    REQUIRE(placed.children.size() == 1);
    CHECK(layerAt(placed.children, 0).path == pathOf("M0 0 L1 1"));
    CHECK(std::get<Color>(*layerAt(placed.children, 0).fill) == Color{0, 0, 255, 255});
    // The copy inherits from the use element, not from the group around the original.
    const Group &copied = groupAt(drawing.nodes, 2);
    CHECK(std::get<Color>(*layerAt(copied.children, 0).fill) == black);
    CHECK(groupAt(drawing.nodes, 3).children.empty());
    CHECK(groupAt(drawing.nodes, 4).children.empty());
    checkRefused("<use id=\"u\" href=\"#u\"/>", "use refers to an element that contains it");
    checkRefused("<g id=\"g\"><use href=\"#g\"/></g>", "use refers to an element that contains it");
    checkRefused("<g id=\"a\"><use href=\"#b\"/></g><g id=\"b\"><use href=\"#a\"/></g>", "contains it");
    checkRefused("<symbol id=\"s\"/><use href=\"#s\"/>", "symbol");
    checkRefused("<use href=\"other.svg#a\"/>", "other documents");
}

TEST_CASE("Non-rendering elements, and elements and attributes in other namespaces, are ignored") {
    const Drawing drawing =
        fromSvg(
            "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"no\"?>\n"
            "<!-- Created with Inkscape -->\n"
            "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:inkscape=\"http://www.inkscape.org/namespaces/inkscape\" "
            "xmlns:sodipodi=\"http://sodipodi.sourceforge.net/DTD/sodipodi-0.dtd\" "
            "xmlns:rdf=\"http://www.w3.org/1999/02/22-rdf-syntax-ns#\" width=\"10\" height=\"10\" "
            "inkscape:version=\"1.3\" sodipodi:docname=\"a.svg\">"
            "<title>A title</title><desc>A description</desc><metadata><rdf:RDF><text/></rdf:RDF></metadata>"
            "<sodipodi:namedview pagecolor=\"#ffffff\"><inkscape:page x=\"0\"/></sodipodi:namedview>"
            "<defs><linearGradient id=\"unused\"/><clipPath id=\"c\"><text>unused</text></clipPath><filter id=\"f\"/>"
            "<marker id=\"m\"/><pattern id=\"p\"/><mask id=\"k\"/></defs>"
            "<g inkscape:groupmode=\"layer\" inkscape:label=\"Layer 1\"><path d=\"M0 0 L1 1\" "
            "sodipodi:nodetypes=\"cc\" inkscape:original-d=\"M0 0\" style=\"fill:#ff0000;-inkscape-stroke:none;"
            "font-variation-settings:normal;shape-rendering:crispEdges\"/></g></svg>")
            .drawing;
    REQUIRE(drawing.nodes.size() == 1);
    CHECK(std::get<Color>(*layerAt(groupAt(drawing.nodes, 0).children, 0).fill) == red);
}

TEST_CASE("Rendering features that linea cannot draw are refused, naming the element or property") {
    checkRefused("<text>Hi</text>", "<text>: text is not supported");
    checkRefused("<image href=\"a.png\"/>", "image is not supported");
    checkRefused("<foreignObject/>", "foreignObject is not supported");
    checkRefused("<switch/>", "switch is not supported");
    checkRefused("<svg/>", "nested svg elements are not supported");
    checkRefused("<blink/>", "unknown element blink");
    checkRefused("<pattern id=\"p\"/><path d=\"M0 0\" fill=\"url(#p)\"/>", "pattern paints are not supported");
    checkRefused("<path d=\"M0 0\" clip-path=\"url(#c)\"/>", "clip-path is not supported");
    checkRefused("<g mask=\"url(#m)\"/>", "mask is not supported");
    checkRefused("<g filter=\"url(#f)\"/>", "filter is not supported");
    checkRefused("<path d=\"M0 0\" style=\"filter: blur(2px)\"/>", "filter is not supported");
    checkRefused("<path d=\"M0 0\" style=\"mix-blend-mode: multiply\"/>", "mix-blend-mode is not supported");
    checkRefused("<g marker-end=\"url(#m)\"><line x2=\"1\"/></g>", "markers are not supported");
    checkRefused("<path d=\"M0 0\" style=\"marker: url(#m)\"/>", "markers are not supported");
    checkRefused("<path d=\"M0 0\" stroke=\"red\" paint-order=\"stroke\"/>", "paint-order");
    checkRefused("<path d=\"M0 0\" stroke=\"red\" vector-effect=\"non-scaling-stroke\"/>", "vector-effect");
    checkRefused("<path d=\"M0 0\" style=\"transform: rotate(45deg)\"/>", "the transform property in a style");
    checkRefused("<path d=\"M0 0\" style=\"offset-path: path('M0 0 L9 9')\"/>", "the offset-path property");
    checkRefused("<path d=\"M0 0\" transform=\"scale(2)\" transform-origin=\"5 5\"/>", "transform-origin");
    checkRefused("<style>path { fill: red }</style>", "style sheets and scripts are not supported");
    checkRefused("<defs><style>path { fill: red }</style></defs>", "style sheets");
    checkRefused("<script>alert(1)</script>", "style sheets and scripts are not supported");
    checkRefused("<rect width=\"1\" height=\"1\"><animate attributeName=\"x\" to=\"5\"/></rect>",
                 "animation is not supported");
    checkRefused("<defs><linearGradient id=\"g\"><stop><set attributeName=\"offset\" to=\"1\"/></stop>"
                 "</linearGradient></defs>",
                 "animation is not supported");
    checkRefused("<path d=\"M0 0\" fill=\"context-fill\"/>", "context-fill is not supported");
    // Each is harmless where it changes nothing: markers on a rect, paint-order without both paints, and
    // vector-effect without a stroke, an empty style element and a clip path that nothing uses.
    CHECK(read("<rect width=\"1\" height=\"1\" marker-end=\"url(#m)\"/><path d=\"M0 0\" paint-order=\"stroke\"/>"
               "<path d=\"M0 0\" vector-effect=\"non-scaling-stroke\"/><style/><style> </style>"
               "<path d=\"M0 0\" stroke=\"red\" paint-order=\"fill markers\"/>")
              .nodes.size() == 4);
    // A refusal names its place in the document.
    try {
        static_cast<void>(fromSvg(document("\n\n   <text/>")));
        FAIL("text was read");
    } catch (const SvgError &error) {
        CHECK(error.line() == 3);
        CHECK(error.column() == 4);
        CHECK(std::string(error.what()).starts_with("line 3, column 4: <text>:"));
    }
}

// Reading: XML.

TEST_CASE("XML reads entities, character references, CDATA, comments, processing instructions and a DOCTYPE") {
    const SvgDocument svg =
        fromSvg("\xEF\xBB\xBF<?xml version='1.0'?><!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" "
                "\"http://www.w3.org/Graphics/SVG/1.1/DTD/svg11.dtd\">\r\n<?xml-stylesheet href=\"none\"?>"
                "<svg xmlns='http://www.w3.org/2000/svg' width='1' height='1'><!-- a > b -->"
                "<desc><![CDATA[ <not> & markup ]]> &lt;&gt;&amp;&apos;&quot;&#169;&#x1F600;</desc>"
                "<path d='M&#48; 0&#x20;L1\n1' fill='&#x23;f00'/><?pi data?></svg><!-- after -->\n");
    REQUIRE(svg.drawing.nodes.size() == 1);
    CHECK(layerAt(svg.drawing.nodes, 0).path == pathOf("M0 0 L1 1"));
    CHECK(std::get<Color>(*layerAt(svg.drawing.nodes, 0).fill) == red);
}

TEST_CASE("XML that is not well-formed, or that declares entities, is refused at its line and column") {
    const auto refused = [](std::string_view text, std::string_view message, int line, int column) {
        CAPTURE(text);
        try {
            static_cast<void>(fromSvg(text));
            FAIL("read without error");
        } catch (const SvgError &error) {
            CHECK(std::string(error.what()).find(message) != std::string::npos);
            CHECK(error.line() == line);
            CHECK(error.column() == column);
        }
    };
    const std::string open = "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"1\" height=\"1\">";
    refused("<!DOCTYPE svg [<!ENTITY lol \"lol\">]><svg/>", "internal subset", 1, 1);
    refused(open + "&lol;</svg>", "the entity &lol; is not defined", 1, 62);
    refused(open + "<g></svg>", "</g> was expected here", 1, 65);
    refused(open + "<g>", "<g> is not closed", 1, 62);
    refused(open + "\n<path d=\"1\" d=\"2\"/></svg>", "the attribute d appears twice", 2, 13);
    refused(open + "<a:b/></svg>", "the namespace prefix a is not declared", 1, 62);
    refused(open + "<g x=\"<\"/></svg>", "'<' may not appear", 1, 68);
    refused(open + "<!-- a -- b --></svg>", "'--' may only end a comment", 1, 69);
    refused(open + "]]></svg>", "']]>' may not appear", 1, 62);
    refused(open + "\x01</svg>", "a character that XML does not allow", 1, 62);
    refused(open + "\xC3\x28</svg>", "not well-formed UTF-8", 1, 62);
    refused(open + "&#0;</svg>", "a character reference", 1, 62);
    refused(open + "</svg><g/>", "may follow the root element", 1, 68);
    refused("", "no root element", 1, 1);
    refused("\xFF\xFE<", "UTF-16", 1, 1);
    refused("<?xml version=\"1.0\" encoding=\"ISO-8859-1\"?><svg/>", "only UTF-8", 1, 1);
    refused(" <?xml version=\"1.0\"?><svg/>", "XML declaration may only start", 1, 2);
    refused("<svg xmlns:x=\"\"/>", "may not be bound to no namespace", 1, 6);
    refused("<svg xmlns:a=\"u\" xmlns:b=\"u\" a:x=\"1\" b:x=\"2\"/>", "the attribute b:x appears twice", 1, 38);
}

TEST_CASE("Nesting is bounded, counting use elements, and so is what use elements and references copy") {
    std::string deep;
    for (int i = 0; i < deepestSvgNesting; ++i)
        deep += "<g>";
    for (int i = 0; i < deepestSvgNesting; ++i)
        deep += "</g>";
    checkRefused(deep, "elements nest more than 256 deep");
    // The XML reader bounds nesting too: in another namespace, which SVG ignores, and far deeper than any stack holds.
    std::string foreign = "<x:a xmlns:x=\"urn:x\">";
    for (int i = 0; i < deepestSvgNesting; ++i)
        foreign += "<x:a>";
    checkRefused(foreign, "elements nest more than 256 deep");
    std::string deepest(1'000'000, ' ');
    for (std::size_t i = 0; i + 3 <= deepest.size(); i += 3)
        deepest.replace(i, 3, "<g>");
    checkRefused(deepest, "elements nest more than 256 deep");
    // Use elements that place one another reach the bound through copies.
    std::string chain = "<path id=\"u0\" d=\"M0 0\"/>";
    for (int i = 1; i < deepestSvgNesting; ++i)
        chain += "<use id=\"u" + std::to_string(i) + "\" href=\"#u" + std::to_string(i - 1) + "\"/>";
    checkRefused(chain, "counting those that use elements place");
    // Ten use elements of ten use elements of ... a path of a thousand points: a few kilobytes asking for terabytes.
    std::string laughs = "<defs><path id=\"l0\" d=\"M0 0";
    for (int i = 0; i < 1000; ++i)
        laughs += " 1 1";
    laughs += "\"/>";
    for (int level = 1; level <= 9; ++level) {
        laughs += "<g id=\"l" + std::to_string(level) + "\">";
        for (int copy = 0; copy < 10; ++copy)
            laughs += "<use href=\"#l" + std::to_string(level - 1) + "\"/>";
        laughs += "</g>";
    }
    laughs += "</defs><use href=\"#l9\"/>";
    checkRefused(laughs, "the drawing would take more than");
}

// Gradients.

namespace {

// The stops of the first gradient element in @p svg, as SVG reads them: offset, stop-color and stop-opacity.
std::vector<GradientStop> writtenStops(const std::string &svg) {
    std::vector<GradientStop> stops;
    const auto value = [&](std::size_t from, std::string_view name) -> std::string {
        const std::size_t at = svg.find(" " + std::string(name) + "=\"", from);
        const std::size_t end = svg.find("/>", from);
        if (at == std::string::npos || at > end)
            return {};
        const std::size_t start = at + name.size() + 3;
        return svg.substr(start, svg.find('"', start) - start);
    };
    const std::size_t close = svg.find("</linearGradient>");
    for (std::size_t at = svg.find("<stop"); at != std::string::npos && at < close; at = svg.find("<stop", at + 1)) {
        Color color = *fromHex(value(at, "stop-color"));
        const std::string opacity = value(at, "stop-opacity");
        if (!opacity.empty())
            color.a = static_cast<std::uint8_t>(std::lround(std::stod(opacity) * 255.0));
        stops.push_back({std::stod(value(at, "offset")), color});
    }
    return stops;
}

// SVG's color at offset @p t of @p stops, which are sorted: each channel and alpha blended in proportion,
// unpremultiplied, as RGBA from 0 to 1, each channel multiplied by alpha.
std::array<double, 4> svgRamp(const std::vector<GradientStop> &stops, double t) {
    const auto premultiplied = [](double r, double g, double b, double a) {
        return std::array<double, 4>{r * a, g * a, b * a, a};
    };
    const auto at = [&](std::size_t i) {
        const Color c = stops[i].color;
        return std::array<double, 4>{c.r / 255.0, c.g / 255.0, c.b / 255.0, c.a / 255.0};
    };
    std::size_t right = 0;
    while (right < stops.size() && stops[right].offset <= t)
        ++right;
    if (right == 0 || right == stops.size()) {
        const auto c = at(right == 0 ? 0 : stops.size() - 1);
        return premultiplied(c[0], c[1], c[2], c[3]);
    }
    const auto x = at(right - 1);
    const auto y = at(right);
    const double u = (t - stops[right - 1].offset) / (stops[right].offset - stops[right - 1].offset);
    const auto mix = [&](std::size_t k) { return x[k] + (y[k] - x[k]) * u; };
    return premultiplied(mix(0), mix(1), mix(2), mix(3));
}

// linea's srgb blend at offset @p t of @p stops, which are sorted: alpha, and each channel times alpha, in
// proportion, as RGBA from 0 to 1, each channel multiplied by alpha.
std::array<double, 4> premultipliedRamp(const std::vector<GradientStop> &stops, double t) {
    const auto at = [&](std::size_t i) {
        const Color c = stops[i].color;
        const double a = c.a / 255.0;
        return std::array<double, 4>{c.r / 255.0 * a, c.g / 255.0 * a, c.b / 255.0 * a, a};
    };
    std::size_t right = 0;
    while (right < stops.size() && stops[right].offset <= t)
        ++right;
    if (right == 0 || right == stops.size())
        return at(right == 0 ? 0 : stops.size() - 1);
    const auto x = at(right - 1);
    const auto y = at(right);
    const double u = (t - stops[right - 1].offset) / (stops[right].offset - stops[right - 1].offset);
    return {x[0] + (y[0] - x[0]) * u, x[1] + (y[1] - x[1]) * u, x[2] + (y[2] - x[2]) * u, x[3] + (y[3] - x[3]) * u};
}

// The greatest difference between two premultiplied colors on any channel or alpha, in 8-bit steps.
double rampGap(const std::array<double, 4> &x, const std::array<double, 4> &y) {
    double gap = 0.0;
    for (std::size_t k = 0; k < 4; ++k)
        gap = std::max(gap, std::abs(x[k] - y[k]) * 255.0);
    return gap;
}

constexpr int rampPixels = 1000;

// The greatest difference, in 8-bit steps on any premultiplied channel or alpha, between what linea draws for a
// horizontal @p paint over 0 to 1000 and what SVG draws for @p stops along the same line.
double rampDifference(const Paint &paint, const std::vector<GradientStop> &stops) {
    Layer layer;
    layer.path = rectangle({0, 0}, {rampPixels, 1});
    layer.fill = paint;
    const Image image = render(Drawing{rampPixels, 1, {layer}}, rampPixels, 1);
    double worst = 0.0;
    for (int x = 0; x < rampPixels; ++x) {
        const std::array<double, 4> expected = svgRamp(stops, (x + 0.5) / rampPixels);
        const std::uint8_t *pixel = image.rgba.data() + static_cast<std::size_t>(x) * 4;
        const double alpha = pixel[3] / 255.0;
        for (std::size_t k = 0; k < 4; ++k) {
            const double drawn = k == 3 ? alpha : pixel[k] / 255.0 * alpha;
            worst = std::max(worst, std::abs(drawn - expected[k]) * 255.0);
        }
    }
    return worst;
}

LinearGradient ramp(std::vector<GradientStop> stops, Interpolation interpolation) {
    return {{0, 0}, {rampPixels, 0}, std::move(stops), SpreadMethod::pad, interpolation};
}

} // namespace

TEST_CASE("Gradients of every kind write and read back exactly") {
    const std::vector<GradientStop> stops{{0.0, {255, 0, 0, 255}}, {0.4, {0, 128, 0, 128}}, {1.0, {0, 0, 255, 255}}};
    const std::vector<GradientStop> awkward{
        {1.5, {9, 8, 7, 255}}, {-0.25, {1, 2, 3, 0}}, {0.5, {4, 5, 6, 200}}, {0.5, {200, 100, 0, 255}}};
    std::vector<Node> nodes;
    const auto add = [&](Paint fill, std::optional<Paint> stroke = std::nullopt) {
        nodes.emplace_back(
            Layer{.path = rectangle({10, 10}, {90, 60}), .fill = std::move(fill), .stroke = std::move(stroke)});
    };
    for (const Interpolation interpolation : {Interpolation::oklab, Interpolation::srgb, Interpolation::srgbLinear}) {
        for (const SpreadMethod spread : {SpreadMethod::pad, SpreadMethod::repeat, SpreadMethod::reflect}) {
            add(LinearGradient{{10, 20}, {80.5, 40}, stops, spread, interpolation});
            add(RadialGradient{{50, 30}, 25, stops, std::nullopt, 0.0, spread, interpolation},
                Paint{LinearGradient{{0, 0}, {1, 1}, awkward, spread, interpolation}});
            add(RadialGradient{{50, 30}, 25, awkward, Vec2{60, 35}, 5.0, spread, interpolation});
        }
        // Ends that coincide, a radius of 0, a focal point at the center, and one or no stops.
        add(LinearGradient{{5, 5}, {5, 5}, stops, SpreadMethod::pad, interpolation});
        add(RadialGradient{{5, 5}, 0.0, stops, std::nullopt, 0.0, SpreadMethod::pad, interpolation});
        add(RadialGradient{{5, 5}, 10.0, stops, Vec2{5, 5}, 0.0, SpreadMethod::pad, interpolation});
        add(LinearGradient{{0, 0}, {10, 0}, {{0.3, {1, 2, 3, 4}}}, SpreadMethod::pad, interpolation});
        add(LinearGradient{{0, 0}, {10, 0}, {}, SpreadMethod::pad, interpolation});
    }
    nodes.emplace_back(Group{{nodes.front(), nodes.back()}, rotate(0.5), 0.5});
    checkSameDrawing(Drawing{100, 70, nodes});
}

TEST_CASE("Identical gradients share one element, and the linea namespace appears only where it is needed") {
    const LinearGradient srgb{{0, 0}, {10, 0}, evenStops({red, black}), SpreadMethod::pad, Interpolation::srgb};
    Layer layer{.path = rectangle({0, 0}, {10, 10}), .fill = Paint{srgb}, .stroke = Paint{srgb}};
    const std::string plain = toSvg(Drawing{10, 10, {layer, layer}});
    CHECK(plain.find("id=\"gradient1\"") != std::string::npos);
    CHECK(plain.find("gradient2") == std::string::npos);
    CHECK(plain.find("linea:") == std::string::npos);
    layer.stroke = Paint{LinearGradient{{0, 0}, {10, 0}, evenStops({red, black})}};
    const std::string oklab = toSvg(Drawing{10, 10, {layer}});
    CHECK(oklab.find("xmlns:linea=\"https://github.com/Gondola-Bros-Entertainment/linea\"") != std::string::npos);
    CHECK(oklab.find("linea:interpolation=\"oklab\" linea:stops=\"0 #ff0000 1 #000000\"") != std::string::npos);
}

TEST_CASE("A written gradient draws in SVG, which blends unpremultiplied in sRGB, within the tolerance of linea's") {
    constexpr double steps = svgGradientTolerance * 255.0;
    // The fitter measures each piece at 15 points; between them a blend may stray a little further.
    constexpr double betweenSamples = 0.05;
    const std::vector<std::vector<GradientStop>> cases{
        evenStops({red, {0, 0, 255, 255}}),
        evenStops({{255, 0, 0, 255}, {255, 255, 0, 255}, {0, 255, 0, 255}, {0, 255, 255, 255}, {0, 0, 255, 255}}),
        evenStops({black, {255, 255, 255, 255}}),
        evenStops({{255, 0, 0, 255}, {0, 0, 0, 0}, {0, 0, 255, 128}}),
        {{0.2, {255, 128, 0, 30}}, {0.2, {0, 128, 255, 255}}, {0.9, {20, 200, 20, 100}}},
    };
    for (const auto &stops : cases) {
        // linea's srgb blend, premultiplied in sRGB, can be computed here exactly and compared at every offset.
        const LinearGradient srgb = ramp(stops, Interpolation::srgb);
        const std::vector<GradientStop> written =
            writtenStops(toSvg(Drawing{rampPixels, 1, {Layer{.path = {}, .fill = Paint{srgb}}}}));
        double worst = 0.0;
        for (int i = 0; i <= 10 * rampPixels; ++i) {
            const double t = i / (10.0 * rampPixels);
            worst = std::max(worst, rampGap(premultipliedRamp(stops, t), svgRamp(written, t)));
        }
        CHECK(worst <= steps + betweenSamples);
        // Oklab and linear light are compared with what linea renders, whose pixels round straight color and alpha
        // each to the nearest step, so a channel times alpha by up to one step more.
        for (const Interpolation interpolation : {Interpolation::oklab, Interpolation::srgbLinear}) {
            const LinearGradient gradient = ramp(stops, interpolation);
            const std::string svg = toSvg(Drawing{rampPixels, 1, {Layer{.path = {}, .fill = Paint{gradient}}}});
            CHECK(rampDifference(Paint{gradient}, writtenStops(svg)) <= steps + betweenSamples + 1.0);
        }
    }
}

TEST_CASE("A gradient read from SVG draws in linea within the tolerance of SVG's unpremultiplied blend") {
    constexpr double steps = svgGradientTolerance * 255.0;
    constexpr double betweenSamples = 0.05;
    // Neighbors that differ in color and in alpha blend differently premultiplied, so reading adds stops between them;
    // where neighbors share a color or an alpha, the stops are read exactly as written.
    const std::string body = "<linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" x2=\"1000\">"
                             "<stop stop-color=\"red\"/><stop offset=\"0.3\" stop-color=\"red\" stop-opacity=\"0\"/>"
                             "<stop offset=\"0.6\" stop-color=\"blue\" stop-opacity=\"0.5\"/>"
                             "<stop offset=\"1\" stop-color=\"lime\" stop-opacity=\"0.5\"/>"
                             "<stop offset=\"1\" stop-color=\"black\" stop-opacity=\"0\"/></linearGradient>"
                             "<rect width=\"1000\" height=\"1\" fill=\"url(#g)\"/>";
    const Layer layer = layerAt(fromSvg(document(body, "width=\"1000\" height=\"1\"")).drawing.nodes, 0);
    REQUIRE(layer.fill);
    const LinearGradient gradient = std::get<LinearGradient>(*layer.fill);
    CHECK(gradient.interpolation == Interpolation::srgb);
    const std::vector<GradientStop> written{
        {0.0, red}, {0.3, {255, 0, 0, 0}}, {0.6, {0, 0, 255, 128}}, {1.0, {0, 255, 0, 128}}, {1.0, {0, 0, 0, 0}}};
    CHECK(gradient.stops.size() > written.size());
    double worst = 0.0;
    for (int i = 0; i <= 10 * rampPixels; ++i) {
        const double t = i / (10.0 * rampPixels);
        worst = std::max(worst, rampGap(premultipliedRamp(gradient.stops, t), svgRamp(written, t)));
    }
    CHECK(worst <= steps + betweenSamples);
    const Layer opaque =
        onlyLayer("<linearGradient id=\"g\"><stop stop-color=\"red\"/><stop offset=\"1\" "
                  "stop-color=\"blue\"/></linearGradient><path d=\"M0 0 H10 V10 Z\" fill=\"url(#g)\"/>");
    CHECK(std::get<LinearGradient>(*opaque.fill).stops == evenStops({red, {0, 0, 255, 255}}));
}

TEST_CASE("color-interpolation linearRGB, which Chrome honors, reads as a gradient blended in linear light") {
    // Opaque neighbors blend alike premultiplied or not, so the stops read as written, whether the property is set on
    // the gradient, inherited from an ancestor, or declared in a style attribute.
    for (const std::string_view gradient : {"<linearGradient id=\"g\" color-interpolation=\"linearRGB\">",
                                            "<g color-interpolation=\"linearRGB\"><linearGradient id=\"g\">",
                                            "<linearGradient id=\"g\" style=\"color-interpolation: linearrgb\">"}) {
        const bool grouped = gradient.starts_with("<g");
        const Drawing drawing = read(std::string(gradient) +
                                     "<stop stop-color=\"red\"/><stop offset=\"1\" "
                                     "stop-color=\"lime\"/></linearGradient>" +
                                     (grouped ? "</g>" : "") + "<rect width=\"100\" height=\"1\" fill=\"url(#g)\"/>");
        const Layer &layer = layerAt(drawing.nodes, grouped ? 1 : 0);
        const LinearGradient read = std::get<LinearGradient>(*layer.fill);
        CHECK(read.interpolation == Interpolation::srgbLinear);
        CHECK(read.stops == evenStops({red, {0, 255, 0, 255}}));
        // Halfway, each channel is half its light, which sRGB encodes as 188, as Chrome draws it.
        const Image image = render(drawing, 100, 1);
        CHECK(image.rgba[49 * 4] == doctest::Approx(188).epsilon(0.02));
        CHECK(image.rgba[49 * 4 + 1] == doctest::Approx(188).epsilon(0.02));
    }
    // Translucent neighbors that differ in color too take more stops, as in sRGB.
    const std::vector<GradientStop> written{{0.0, {255, 0, 0, 0}}, {1.0, {0, 0, 255, 255}}};
    const Layer translucent = onlyLayer(
        "<linearGradient id=\"g\" color-interpolation=\"linearRGB\"><stop stop-color=\"red\" stop-opacity=\"0\"/>"
        "<stop offset=\"1\" stop-color=\"blue\"/></linearGradient><path d=\"M0 0 H1 V1 Z\" fill=\"url(#g)\"/>");
    const LinearGradient fitted = std::get<LinearGradient>(*translucent.fill);
    CHECK(fitted.interpolation == Interpolation::srgbLinear);
    CHECK(fitted.stops.size() > written.size());
    // Both ramps blend linear light, SVG's straight and linea's premultiplied, and are compared encoded.
    const auto encode = [](double light) {
        return light <= 0.0031308 ? light * 12.92 : 1.055 * std::pow(light, 1.0 / 2.4) - 0.055;
    };
    const auto linearRamp = [&](const std::vector<GradientStop> &stops, double t, bool premultiplied) {
        std::size_t right = 0;
        while (right < stops.size() && stops[right].offset <= t)
            ++right;
        const std::size_t first = right == 0 ? 0 : right - 1;
        const std::size_t second = right == stops.size() ? stops.size() - 1 : right;
        const double span = stops[second].offset - stops[first].offset;
        const double u = span > 0.0 ? (t - stops[first].offset) / span : 0.0;
        const Color x = stops[first].color;
        const Color y = stops[second].color;
        const double xa = x.a / 255.0;
        const double ya = y.a / 255.0;
        const double a = xa + (ya - xa) * u;
        std::array<double, 4> out{0, 0, 0, a};
        if (!(a > 0.0))
            return out;
        const std::array<std::uint8_t, 3> xs{x.r, x.g, x.b};
        const std::array<std::uint8_t, 3> ys{y.r, y.g, y.b};
        for (std::size_t k = 0; k < 3; ++k) {
            const double from = srgbToLinear(xs[k]) * (premultiplied ? xa : 1.0);
            const double to = srgbToLinear(ys[k]) * (premultiplied ? ya : 1.0);
            out[k] = encode((from + (to - from) * u) / (premultiplied ? a : 1.0)) * a;
        }
        return out;
    };
    double worst = 0.0;
    for (int i = 0; i <= 10 * rampPixels; ++i) {
        const double t = i / (10.0 * rampPixels);
        worst = std::max(worst, rampGap(linearRamp(fitted.stops, t, true), linearRamp(written, t, false)));
    }
    CHECK(worst <= svgGradientTolerance * 255.0 + 0.05);
    // Originals that toSvg() recorded for an sRGB blend are not restored once the document blends in linear light.
    std::string svg = toSvg(Drawing{10,
                                    10,
                                    {Layer{.path = rectangle({0, 0}, {10, 10}),
                                           .fill = Paint{LinearGradient{{0, 0}, {10, 0}, evenStops({red, black})}}}}});
    svg.replace(svg.find("<linearGradient "), 16, "<linearGradient color-interpolation=\"linearRGB\" ");
    CHECK(std::get<LinearGradient>(*layerAt(fromSvg(svg).drawing.nodes, 0).fill).interpolation ==
          Interpolation::srgbLinear);
    checkRefused("<linearGradient id=\"g\" color-interpolation=\"lab\"/><path d=\"M0 0\" fill=\"url(#g)\"/>",
                 "color-interpolation");
}

TEST_CASE("Recorded originals are restored only while the stops SVG draws are the ones they regenerate") {
    const LinearGradient original{
        {0, 0}, {10, 0}, evenStops({red, {0, 0, 255, 255}}), SpreadMethod::pad, Interpolation::oklab};
    const std::string svg =
        toSvg(Drawing{10, 10, {Layer{.path = rectangle({0, 0}, {10, 10}), .fill = Paint{original}}}});
    CHECK(std::get<LinearGradient>(*layerAt(fromSvg(svg).drawing.nodes, 0).fill) == original);
    // Another tool changed a stop's color: the document now draws what it shows.
    std::string edited = svg;
    const std::size_t at = edited.find("stop-color=\"#8c53a2\"");
    REQUIRE(at != std::string::npos);
    edited.replace(at, 20, "stop-color=\"#00ff00\"");
    const LinearGradient read = std::get<LinearGradient>(*layerAt(fromSvg(edited).drawing.nodes, 0).fill);
    CHECK(read.interpolation == Interpolation::srgb);
    CHECK(read.stops == writtenStops(edited));
    // Originals that do not parse are ignored.
    std::string garbled = svg;
    garbled.replace(garbled.find("linea:stops=\"0"), 14, "linea:stops=\"x");
    CHECK(std::get<LinearGradient>(*layerAt(fromSvg(garbled).drawing.nodes, 0).fill).interpolation ==
          Interpolation::srgb);
}

TEST_CASE("Gradient elements lend their stops and attributes through href, in either kind") {
    const Drawing drawing =
        read("<defs><linearGradient id=\"base\" gradientUnits=\"userSpaceOnUse\" x1=\"1\" x2=\"9\" "
             "spreadMethod=\"reflect\" gradientTransform=\"translate(5)\"><stop stop-color=\"red\"/>"
             "<stop offset=\"1\" stop-color=\"blue\"/></linearGradient>"
             "<linearGradient id=\"child\" xlink:href=\"#base\" y2=\"4\"><title>no content</title></linearGradient>"
             "<radialGradient id=\"round\" href=\"#child\" cx=\"3\" cy=\"4\" r=\"2\"/></defs>"
             "<path d=\"M0 0 H10 V10 Z\" fill=\"url(#child)\"/><path d=\"M0 0 H10 V10 Z\" fill=\"url(#round)\"/>");
    const auto &linear = std::get<LinearGradient>(*layerAt(drawing.nodes, 0).fill);
    CHECK(linear.from == Vec2{6, 0});
    CHECK(linear.to.x == doctest::Approx(14.0));
    CHECK(linear.to.y == doctest::Approx(4.0));
    CHECK(linear.spread == SpreadMethod::reflect);
    CHECK(linear.stops == evenStops({red, {0, 0, 255, 255}}));
    // The radial gradient takes the units, transform, spread and stops, but not the linear gradient's x1 and x2.
    const auto &radial = std::get<RadialGradient>(*layerAt(drawing.nodes, 1).fill);
    CHECK(radial.center == Vec2{8, 4});
    CHECK(radial.radius == 2.0);
    CHECK_FALSE(radial.focal);
    CHECK(radial.spread == SpreadMethod::reflect);
    CHECK(radial.stops.size() == 2);
    checkRefused("<linearGradient id=\"a\" href=\"#b\"/><linearGradient id=\"b\" href=\"#a\"/>"
                 "<path d=\"M0 0\" fill=\"url(#a)\"/>",
                 "cycle");
}

TEST_CASE("objectBoundingBox units, the default, map the gradient onto the shape's bounding box") {
    const LinearGradient linear =
        std::get<LinearGradient>(*onlyLayer("<linearGradient id=\"g\" x1=\"10%\" x2=\".5\"><stop/></linearGradient>"
                                            "<rect x=\"20\" y=\"10\" width=\"40\" height=\"40\" fill=\"url(#g)\"/>")
                                      .fill);
    CHECK(linear.from == Vec2{24, 10});
    CHECK(linear.to == Vec2{40, 10});
    // gradientTransform acts in the box's units, before the box maps them onto the shape.
    const LinearGradient scaled = std::get<LinearGradient>(
        *onlyLayer("<linearGradient id=\"g\" gradientTransform=\"translate(0.5) scale(0.5)\"><stop/></linearGradient>"
                   "<rect x=\"20\" y=\"10\" width=\"40\" height=\"40\" fill=\"url(#g)\"/>")
             .fill);
    CHECK(scaled.from == Vec2{40, 10});
    CHECK(scaled.to == Vec2{60, 10});
    // A radial gradient on a square box stays circular.
    const RadialGradient radial =
        std::get<RadialGradient>(*onlyLayer("<radialGradient id=\"g\" fx=\"0.25\" fr=\"10%\"><stop/></radialGradient>"
                                            "<rect x=\"20\" y=\"10\" width=\"40\" height=\"40\" fill=\"url(#g)\"/>")
                                      .fill);
    CHECK(radial.center == Vec2{40, 30});
    CHECK(radial.radius == 20.0);
    CHECK(*radial.focal == Vec2{30, 30});
    CHECK(radial.focalRadius == doctest::Approx(4.0));
    // A box without area, as a horizontal line has, leaves nothing to paint.
    CHECK_FALSE(
        onlyLayer("<linearGradient id=\"g\"><stop/></linearGradient><line x2=\"10\" stroke=\"url(#g)\"/>").stroke);
    // A linear gradient stays linear under any transform: a skewed one rises along the inverse's transpose.
    const LinearGradient skewed = std::get<LinearGradient>(
        *onlyLayer("<linearGradient id=\"g\" gradientUnits=\"userSpaceOnUse\" x2=\"10\" "
                   "gradientTransform=\"skewX(45)\"><stop/></linearGradient><path d=\"M0 0 H10 V10 Z\" "
                   "fill=\"url(#g)\"/>")
             .fill);
    // skewX(45) moves (x, y) to (x + y, y), so a point's offset is (x - y) / 10, which rises toward (5, -5).
    CHECK(skewed.from == Vec2{0, 0});
    CHECK(skewed.to.x == doctest::Approx(5.0));
    CHECK(skewed.to.y == doctest::Approx(-5.0));
}

TEST_CASE("A radial gradient stretched into an ellipse fills as a layer in its own coordinates") {
    const Drawing drawing =
        read("<radialGradient id=\"g\"><stop stop-color=\"red\"/><stop offset=\"1\" stop-color=\"blue\"/>"
             "</radialGradient><rect x=\"0\" y=\"0\" width=\"100\" height=\"50\" fill=\"url(#g)\" stroke=\"black\" "
             "opacity=\"0.5\"/>");
    const Group &together = groupAt(drawing.nodes, 0);
    CHECK(together.opacity == 0.5);
    const Layer &filled = layerAt(together.children, 0);
    CHECK(filled.transform == scale(100.0, 50.0));
    CHECK(filled.path == transformed(pathOf("M0 0 L100 0 L100 50 L0 50 L0 0 Z"), scale(0.01, 0.02)));
    CHECK(std::get<RadialGradient>(*filled.fill).center == Vec2{0.5, 0.5});
    CHECK_FALSE(filled.stroke);
    const Layer &stroked = layerAt(together.children, 1);
    CHECK_FALSE(stroked.fill);
    CHECK(stroked.stroke);
    // The ellipse's edge meets the box's sides: the middle of the left side is the last stop's blue.
    const Image image = render(drawing, 100, 100);
    CHECK(image.rgba[(25 * 100 + 2) * 4 + 2] > 200);
    CHECK(image.rgba[(25 * 100 + 50) * 4 + 0] > 200);
    checkRefused("<radialGradient id=\"g\"><stop/></radialGradient><rect width=\"100\" height=\"50\" "
                 "stroke=\"url(#g)\"/>",
                 "stretched unevenly");
}

TEST_CASE("Stops read their offsets, colors and opacities as SVG fixes them up") {
    // Offsets clamp and rise to the largest before them; stop-opacity, the stop color's own alpha and the element's
    // fill-opacity multiply; and currentColor is the color where the stop stands, not where the gradient is used.
    // Neighbors here share a color or an offset, so no stops are added between them.
    const Layer layer = firstLayer(
        "<g color=\"lime\"><linearGradient id=\"g\" style=\"color:blue\"><stop offset=\"50%\" "
        "stop-color=\"currentColor\"/><stop offset=\"0.2\" style=\"stop-color:red;stop-opacity:.5\"/>"
        "<stop offset=\"2\" stop-color=\"rgba(255, 0, 0, 0.5)\" stop-opacity=\"0.5\"/><desc/><stop/></linearGradient>"
        "<path d=\"M0 0 H10 V10 Z\" fill=\"url(#g)\" fill-opacity=\"0.5\"/></g>");
    const auto &stops = std::get<LinearGradient>(*layer.fill).stops;
    REQUIRE(stops.size() == 4);
    CHECK(stops[0] == GradientStop{0.5, {0, 0, 255, 128}});
    CHECK(stops[1] == GradientStop{0.5, {255, 0, 0, 64}});
    CHECK(stops[2] == GradientStop{1.0, {255, 0, 0, 32}});
    CHECK(stops[3] == GradientStop{1.0, {0, 0, 0, 128}});
    // A linear gradient whose ends coincide paints its last stop's color, and one without stops nothing.
    CHECK(fillColor("<linearGradient id=\"g\" x2=\"0\"><stop stop-color=\"red\"/><stop stop-color=\"blue\"/>"
                    "</linearGradient><path d=\"M0 0 H10 V10 Z\" fill=\"url(#g)\"/>") == Color{0, 0, 255, 255});
    CHECK(std::get<LinearGradient>(
              *onlyLayer("<linearGradient id=\"g\"/><path d=\"M0 0 H1 V1 Z\" fill=\"url(#g)\"/>").fill)
              .stops.empty());
    checkRefused("<linearGradient id=\"g\"><stop offset=\"half\"/></linearGradient><path d=\"M0 0\" fill=\"url(#g)\"/>",
                 "offset");
    checkRefused("<linearGradient id=\"g\" gradientUnits=\"pixels\"/><path d=\"M0 0\" fill=\"url(#g)\"/>",
                 "gradientUnits");
    checkRefused("<linearGradient id=\"g\" spreadMethod=\"wrap\"/><path d=\"M0 0\" fill=\"url(#g)\"/>", "spreadMethod");
    checkRefused("<radialGradient id=\"g\" r=\"-1\"/><path d=\"M0 0\" fill=\"url(#g)\"/>", "r:");
}

TEST_CASE("Writing refuses paints that SVG has no element for, and gradients SVG holds invalid") {
    const auto refused = [](Paint paint, std::string_view message) {
        CHECK_THROWS_WITH_AS(static_cast<void>(toSvg(
                                 Drawing{1, 1, {Layer{.path = rectangle({0, 0}, {1, 1}), .fill = std::move(paint)}}})),
                             doctest::Contains(message.data()), SvgError);
    };
    refused(ConicGradient{{0, 0}, 0.0, evenStops({red, black})}, "conic gradient cannot be written");
    refused(Pattern{std::make_shared<const Drawing>(Drawing{1, 1, {}}), identity()}, "pattern paint is not written");
    refused(RadialGradient{{0, 0}, -1.0, {}}, "radius is negative");
    refused(RadialGradient{{0, 0}, 1.0, {}, std::nullopt, -1.0}, "radius is negative");
    refused(LinearGradient{{0, 0}, {std::numeric_limits<double>::infinity(), 0}, {}}, "not finite");
    refused(LinearGradient{{0, 0}, {1, 0}, {{std::numeric_limits<double>::quiet_NaN(), red}}}, "offset is not finite");
}

TEST_CASE("Numbers that overflow a double on the way in are refused, never read as infinities") {
    // A relative coordinate or an arc can reach past the largest double from finite numbers; the path ends before it.
    for (const std::string_view data :
         {"M1e308 0 l1e308 0", "M1e308 0 a1 1 0 0 1 1e308 0", "M1e308 0 h1e308", "M1e308 1e308 c1e308 0 0 0 0 0"}) {
        CAPTURE(data);
        const SvgPathData read = fromSvgPathData(data);
        REQUIRE(read.error);
        CHECK(std::string(read.error->what()).find("too large for a double") != std::string::npos);
        CHECK(std::ranges::all_of(read.path.points(), [](Vec2 p) { return std::isfinite(p.x) && std::isfinite(p.y); }));
    }
    checkRefused("<rect width=\"1e308in\" height=\"1\"/>", "width");
    checkRefused("<rect x=\"1e308\" width=\"1e308\" height=\"1\"/>", "past the largest double");
    checkRefused("<path d=\"M0 0\" transform=\"scale(1e200) scale(1e200)\"/>", "transform");
    checkRefused("<path id=\"p\" d=\"M0 0\"/><use href=\"#p\" x=\"1e308\" transform=\"translate(1e308)\"/>",
                 "past the largest double");
    CHECK_THROWS_WITH_AS(
        static_cast<void>(fromSvg(
            document("", "width=\"1e300\" height=\"1\" viewBox=\"0 0 1e-300 1\" preserveAspectRatio=\"none\""))),
        doctest::Contains("past the largest double"), SvgError);
}

// Corruption.

namespace {

// Whether every number in @p nodes is finite.
bool allFinite(const std::vector<Node> &nodes) {
    const auto finite = [](double value) { return std::isfinite(value); };
    const auto finitePoint = [&](Vec2 p) { return finite(p.x) && finite(p.y); };
    const auto finiteAffine = [&](const Affine &m) {
        return finite(m.a) && finite(m.b) && finite(m.c) && finite(m.d) && finite(m.e) && finite(m.f);
    };
    const auto finitePaint = [&](const std::optional<Paint> &paint) {
        if (!paint)
            return true;
        if (const auto *linear = std::get_if<LinearGradient>(&*paint))
            return finitePoint(linear->from) && finitePoint(linear->to);
        if (const auto *radial = std::get_if<RadialGradient>(&*paint))
            return finitePoint(radial->center) && finite(radial->radius) && finite(radial->focalRadius) &&
                   finitePoint(radial->focal.value_or(Vec2{}));
        return true;
    };
    return std::ranges::all_of(nodes, [&](const Node &node) {
        if (const auto *group = std::get_if<Indirect<Group>>(&node.content))
            return finiteAffine((*group)->transform) && finite((*group)->opacity) && allFinite((*group)->children);
        const Layer &layer = std::get<Layer>(node.content);
        return std::ranges::all_of(layer.path.points(), finitePoint) && finiteAffine(layer.transform) &&
               finite(layer.opacity) && finite(layer.strokeStyle.width) && finite(layer.strokeStyle.dashOffset) &&
               std::ranges::all_of(layer.strokeStyle.dashArray, finite) && finitePaint(layer.fill) &&
               finitePaint(layer.stroke);
    });
}

// Whether @p text reads as a drawing of finite numbers that writes and reads back as itself, or is refused with an
// SvgError; anything else, such as another exception, is a failure, and the sanitizers catch what this cannot.
bool readsSafely(std::string_view text) {
    SvgDocument read;
    try {
        read = fromSvg(text);
    } catch (const SvgError &) {
        return true;
    }
    if (!allFinite(read.drawing.nodes) || !std::isfinite(read.drawing.width) || !std::isfinite(read.drawing.height))
        return false;
    const SvgDocument again = fromSvg(toSvg(read.drawing));
    return again.drawing == read.drawing && again.errors.empty();
}

// A document as linea writes one: groups, transforms, strokes and gradients in Oklab and sRGB.
std::string writtenFixture() {
    Layer gem;
    gem.path = star({50, 50}, 44, 20, 5, -std::numbers::pi / 2.0);
    gem.fill = Paint{RadialGradient{{50, 40}, 48, evenStops({{255, 236, 160, 255}, {214, 120, 24, 255}})}};
    gem.stroke = Paint{Color{40, 24, 12, 200}};
    gem.strokeStyle = {.width = 4.0, .join = Join::miter, .dashArray = {3, 1}};
    Layer band;
    band.path = rectangle({0, 80}, {100, 100});
    band.fill = Paint{LinearGradient{
        {0, 0}, {100, 0}, evenStops({red, {0, 0, 255, 128}}), SpreadMethod::reflect, Interpolation::srgb}};
    band.fillRule = FillRule::evenOdd;
    return toSvg(Drawing{100, 100, {gem, Group{{band}, rotateAround(0.2, {50, 50}), 0.75}}});
}

// A document as other tools write them, with most of what fromSvg() reads.
constexpr std::string_view handFixture =
    "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE svg PUBLIC \"-//W3C//DTD SVG 1.1//EN\" \"svg11.dtd\">\n"
    "<svg xmlns=\"http://www.w3.org/2000/svg\" xmlns:xlink=\"http://www.w3.org/1999/xlink\" "
    "xmlns:i=\"http://www.inkscape.org/namespaces/inkscape\" width=\"2in\" height=\"50%\" viewBox=\"0 0 200 100\" "
    "preserveAspectRatio=\"xMidYMax slice\"><title>A &amp; B</title><!-- comment -->"
    "<defs><linearGradient id=\"a\" x2=\"50%\" gradientTransform=\"rotate(30 .5 .5)\"><stop offset=\"0\" "
    "style=\"stop-color:hsl(30 80% 50%)\"/><stop offset=\"1\" stop-color=\"rgb(10 20 30 / 50%)\"/></linearGradient>"
    "<radialGradient id=\"b\" xlink:href=\"#a\" fx=\"0.3\" fr=\"5%\" spreadMethod=\"repeat\"/>"
    "<path id=\"p\" d=\"m10,10 20,0 0,20z M40 40c5 0 10 5 10 10s-5 10-10 10-10-5-10-10 5-10 10-10\"/></defs>"
    "<g fill=\"url(#b) #f00\" stroke=\"currentColor\" color=\"#123\" style=\"stroke-width:2px;opacity:.9\" "
    "i:label=\"x\"><rect x=\"5\" y=\"5\" width=\"40\" height=\"30\" rx=\"4\"/><circle cx=\"80\" cy=\"20\" r=\"10%\"/>"
    "<ellipse cx=\"120\" cy=\"20\" rx=\"15\" ry=\"8\" fill=\"url(#a)\"/><line x1=\"0\" y1=\"90\" x2=\"200\" y2=\"90\" "
    "stroke-dasharray=\"4,2\" stroke-linecap=\"round\"/><polyline points=\"0,0 10,10 20,0\" fill=\"none\"/>"
    "<polygon points=\"150,50 170,90 130,90\" fill-rule=\"evenodd\" fill-opacity=\".5\"/>"
    "<use xlink:href=\"#p\" x=\"100\" transform=\"scale(1.5)\"/><path d=\"M0 0 Q 10 20 20 0 T 40 0 A 5 5 0 1 0 50 0\" "
    "fill=\"none\" stroke-linejoin=\"bevel\"/></g><![CDATA[ ]]></svg>\n";

void checkCorruptions(std::string_view clean) {
    REQUIRE(readsSafely(clean));
    REQUIRE_NOTHROW(static_cast<void>(fromSvg(clean)));
    for (std::size_t length = 0; length < clean.size(); ++length) {
        CAPTURE(length);
        CHECK(readsSafely(clean.substr(0, length)));
    }
    for (std::size_t offset = 0; offset < clean.size(); ++offset) {
        const auto original = static_cast<unsigned char>(clean[offset]);
        for (const unsigned value : {0x00u, 0xFFu, original ^ 0x01u, original ^ 0x80u, unsigned{'<'}, unsigned{'"'},
                                     unsigned{'-'}, unsigned{'9'}}) {
            std::string corrupted(clean);
            corrupted[offset] = static_cast<char>(value);
            CAPTURE(offset);
            CAPTURE(value);
            CHECK(readsSafely(corrupted));
        }
    }
}

} // namespace

TEST_CASE("Every truncation and single-byte corruption of a document is read or refused, never misread") {
    SUBCASE("written by linea") { checkCorruptions(writtenFixture()); }
    SUBCASE("written by hand") { checkCorruptions(handFixture); }
}
