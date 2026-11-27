cmake_minimum_required(VERSION 3.16)
project(hope C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(HOPE_CORE_SOURCES
    src/base/hpe_util.c
    src/render/hpe_pixmap.c
    src/render/hpe_raster.c
    src/render/hpe_image.c
    src/render/hpe_font.c
    src/render/hpe_emoji.c
    src/dom/hpe_dom.c
    src/html/hpe_html.c
    src/html/hpe_encoding.c
    src/css/hpe_css.c
    src/layout/hpe_layout.c
    src/paint/hpe_paint.c
    src/paint/hpe_svg.c
    src/js/hpe_js_value.c
    src/js/hpe_js_parser.c
    src/js/hpe_js_interp.c
    src/js/hpe_js_dom.c
    src/js/hpe_js_mutation.c
    src/js/hpe_js_timers.c
    src/js/hpe_js_regex.c
    src/js/hpe_js_date.c
    src/js/hpe_js_intl.c
    src/js/hpe_js_bigint.c
    src/js/hpe_js_buffer.c
    src/js/hpe_js_symbol.c
    src/js/hpe_js_gen.c
    src/js/hpe_js_async.c
    src/js/hpe_js_gc.c
    src/hope.c
    src/hope_chrome.c
    src/hope_sel.c
)

set(HOPE_PLATFORM_SOURCES
    src/platform/win32/hpe_win32.c
)

add_executable(hope
    src/main.c
    ${HOPE_CORE_SOURCES}
    ${HOPE_PLATFORM_SOURCES}
)

target_include_directories(hope PRIVATE src)

# --- Unit tests: base / css / dom / html only (no platform backend) ---
add_executable(hope_tests
    tests/test_main.c
    tests/test_base.c
    tests/test_css.c
    tests/test_html.c
    src/base/hpe_util.c
    src/css/hpe_css.c
    src/dom/hpe_dom.c
    src/html/hpe_html.c
    src/html/hpe_encoding.c
)
target_include_directories(hope_tests PRIVATE src tests)

if(MSVC)
    target_compile_options(hope PRIVATE /W3 /utf-8)
    target_link_libraries(hope PRIVATE shell32)
    target_compile_options(hope_tests PRIVATE /W3 /utf-8)
else()
    target_compile_options(hope PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_definitions(hope PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(hope PRIVATE gdi32 user32 shell32 imm32)
    target_compile_options(hope_tests PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_definitions(hope_tests PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()

enable_testing()
add_test(NAME hope_tests COMMAND hope_tests)
