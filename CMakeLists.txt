cmake_minimum_required(VERSION 3.20)
project(hypxrhud LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

find_package(PkgConfig REQUIRED)
# jansson: the interim stdin NDJSON wire format (retired when WP-H3 lands D-Bus).
pkg_check_modules(jansson REQUIRED IMPORTED_TARGET jansson)

# ---- optional OpenXR overlay session deps ----
# The daemon's XR session (Session.cpp + Egl.cpp) needs openxr/egl/gbm. When absent the
# daemon still builds and `--preview` works; running it just reports no runtime. This is
# the same discovery hypxrvoice/hypxrpaper use.
set(HAVE_XR FALSE)
pkg_check_modules(xrdeps IMPORTED_TARGET openxr egl glesv2 gbm libdrm)
if(xrdeps_FOUND)
    set(HAVE_XR TRUE)
    message(STATUS "hypxrhud: OpenXR overlay session ENABLED (openxr/egl/gbm found)")
else()
    message(STATUS "hypxrhud: OpenXR session disabled (openxr/egl/gbm not found) — --preview only")
endif()

# ---- bundled OFL font, baked into the binary (font_data.c) ----
set(HUD_FONT_TTF ${CMAKE_CURRENT_SOURCE_DIR}/third_party/fonts/LiberationMono-Regular.ttf)
set(HUD_FONT_C   ${CMAKE_CURRENT_BINARY_DIR}/font_data.c)
add_custom_command(
    OUTPUT ${HUD_FONT_C}
    COMMAND ${CMAKE_COMMAND} -DFONT_IN=${HUD_FONT_TTF} -DOUT_C=${HUD_FONT_C}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_font.cmake
    DEPENDS ${HUD_FONT_TTF} ${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_font.cmake
    COMMENT "Embedding HUD font (LiberationMono, OFL)"
    VERBATIM)

# ---- hud_core: the PURE render + scene library (NO OpenXR/EGL) ----
# Shared by the daemon and the tests. Depends only on jansson (interim wire) + the
# vendored stb headers + the bundled font. This is where ~all of WP-H1/H2 lives and
# where every unit test points.
add_library(hypxrhud_core STATIC
    src/Log.cpp
    src/Panel.cpp
    src/Slots.cpp
    src/Scene.cpp
    src/Config.cpp
    src/PanelText.cpp
    src/PngWrite.cpp
    src/Wire.cpp
    src/Preview.cpp
    src/stb_impl.cpp
    ${HUD_FONT_C}
)
target_include_directories(hypxrhud_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(hypxrhud_core PUBLIC PkgConfig::jansson)
target_compile_options(hypxrhud_core PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
# The stb single-header impls carry their own upstream warning style.
set_source_files_properties(src/stb_impl.cpp PROPERTIES COMPILE_OPTIONS "-w")

# ---- daemon ----
add_executable(hypxrhud src/main.cpp)
target_link_libraries(hypxrhud PRIVATE hypxrhud_core)
target_compile_options(hypxrhud PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
if(HAVE_XR)
    target_sources(hypxrhud PRIVATE src/Egl.cpp src/Session.cpp)
    target_link_libraries(hypxrhud PRIVATE PkgConfig::xrdeps)
    target_compile_definitions(hypxrhud PRIVATE HAVE_XR)
endif()

install(TARGETS hypxrhud RUNTIME DESTINATION bin)

# ---- tests (doctest) ----
enable_testing()
add_executable(hypxrhud_tests
    tests/tests_main.cpp
    tests/test_panel.cpp
    tests/test_envelope.cpp
    tests/test_raster.cpp
    tests/test_wire.cpp
    tests/test_scene.cpp
    tests/test_slots.cpp
    tests/test_config.cpp
    tests/test_preview.cpp
)
target_include_directories(hypxrhud_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(hypxrhud_tests PRIVATE hypxrhud_core)
target_compile_options(hypxrhud_tests PRIVATE -Wall -Wextra)
add_test(NAME hypxrhud_tests COMMAND hypxrhud_tests)
