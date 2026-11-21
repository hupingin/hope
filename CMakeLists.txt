cmake_minimum_required(VERSION 3.16)
project(hope C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

set(HOPE_CORE_SOURCES
    src/base/hpe_util.c
    src/render/hpe_pixmap.c
    src/render/hpe_raster.c
    src/render/hpe_font.c
    src/dom/hpe_dom.c
    src/html/hpe_html.c
    src/css/hpe_css.c
    src/layout/hpe_layout.c
    src/paint/hpe_paint.c
    src/paint/hpe_svg.c
    src/js/hpe_js_value.c
    src/js/hpe_js_parser.c
    src/js/hpe_js_interp.c
    src/js/hpe_js_dom.c
    src/js/hpe_js_timers.c
    src/js/hpe_js_regex.c
    src/js/hpe_js_date.c
    src/hope.c
    src/hope_chrome.c
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

if(MSVC)
    target_compile_options(hope PRIVATE /W3 /utf-8)
    target_link_libraries(hope PRIVATE shell32)
else()
    target_compile_options(hope PRIVATE -Wall -Wextra -Wno-unused-parameter)
    target_compile_definitions(hope PRIVATE _CRT_SECURE_NO_WARNINGS)
    target_link_libraries(hope PRIVATE gdi32 user32 shell32)
endif()
