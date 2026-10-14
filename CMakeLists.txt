cmake_minimum_required(VERSION 3.21)
project(linea VERSION 0.1.0 LANGUAGES CXX)

# Tests build only when linea is the top-level project, never inside a project that adds it.
option(LINEA_BUILD_TESTS "Build linea's tests" ${PROJECT_IS_TOP_LEVEL})
option(LINEA_WARNINGS_AS_ERRORS "Fail the build on compiler warnings" ON)

function(linea_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive- /utf-8)
        if(LINEA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow)
        # GCC's -Wshadow and MSVC's /W4 flag a local that hides a namespace-scope variable; Clang needs
        # -Wshadow-all for that, so every compiler reports the same shadowing.
        target_compile_options(${target} PRIVATE $<$<CXX_COMPILER_ID:Clang,AppleClang>:-Wshadow-all>)
        if(LINEA_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# Vector graphics: paths, shapes, strokes, paints and an anti-aliased rasterizer, with no dependencies beyond the
# standard library.
add_library(linea
    src/Boolean.cpp
    src/Color.cpp
    src/Drawing.cpp
    src/Flatten.cpp
    src/Geometry.cpp
    src/Measure.cpp
    src/Noise.cpp
    src/Paint.cpp
    src/Path.cpp
    src/Patterns.cpp
    src/Raster.cpp
    src/Shading.cpp
    src/Shapes.cpp
    src/Snap.cpp
    src/Stroke.cpp
    src/Svg.cpp
    src/SvgRead.cpp
    src/SvgWrite.cpp
    src/Text.cpp
    src/Xml.cpp)
add_library(linea::linea ALIAS linea)
target_include_directories(linea PUBLIC include)
target_compile_features(linea PUBLIC cxx_std_20)
set_target_properties(linea PROPERTIES CXX_EXTENSIONS OFF)
linea_warnings(linea)

if(LINEA_BUILD_TESTS)
    enable_testing()
    add_executable(linea_tests
        tests/aggregate_tests.cpp
        tests/boolean_tests.cpp
        tests/color_tests.cpp
        tests/drawing_tests.cpp
        tests/effects_tests.cpp
        tests/flatten_tests.cpp
        tests/geometry_tests.cpp
        tests/indirect_tests.cpp
        tests/main.cpp
        tests/measure_tests.cpp
        tests/noise_tests.cpp
        tests/paint_tests.cpp
        tests/path_tests.cpp
        tests/pattern_tests.cpp
        tests/raster_tests.cpp
        tests/stroke_tests.cpp
        tests/svg_tests.cpp
        tests/text_tests.cpp)
    target_link_libraries(linea_tests PRIVATE linea)
    target_include_directories(linea_tests SYSTEM PRIVATE third_party/doctest)
    # The text tests read a vendored font from the source tree, wherever the tests run from.
    target_compile_definitions(linea_tests
        PRIVATE LINEA_TEST_FONT="${CMAKE_CURRENT_SOURCE_DIR}/tests/fonts/LatoLatin-Regular.ttf")
    linea_warnings(linea_tests)
    add_test(NAME linea_tests COMMAND linea_tests)
endif()
