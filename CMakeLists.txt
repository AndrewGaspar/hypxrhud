cmake_minimum_required(VERSION 3.20)
project(hypxrhud LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

find_package(PkgConfig REQUIRED)
# jansson: the interim stdin NDJSON wire format (kept behind --stdin for tests/scripting).
pkg_check_modules(jansson REQUIRED IMPORTED_TARGET jansson)
# libsystemd: the sd-bus D-Bus front end (WP-H3). Present on every systemd box.
pkg_check_modules(libsystemd REQUIRED IMPORTED_TARGET libsystemd)

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
    src/Props.cpp
    src/Theme.cpp
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
# The D-Bus front end (Dbus) + persistent event loop / backoff (Daemon) are ALWAYS built —
# a box without an XR runtime still runs the daemon (owns the bus, accepts panels, reports
# the runtime "absent"). The XR overlay session (Egl + Session) is added only under HAVE_XR.
add_executable(hypxrhud src/main.cpp src/Dbus.cpp src/Daemon.cpp src/ThemeWatch.cpp src/SelfTest.cpp)
target_link_libraries(hypxrhud PRIVATE hypxrhud_core PkgConfig::libsystemd)
target_compile_options(hypxrhud PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
if(HAVE_XR)
    target_sources(hypxrhud PRIVATE src/Egl.cpp src/Session.cpp)
    target_link_libraries(hypxrhud PRIVATE PkgConfig::xrdeps)
    target_compile_definitions(hypxrhud PRIVATE HAVE_XR)
endif()

install(TARGETS hypxrhud RUNTIME DESTINATION bin)

# ---- hypxrhud-battery: the first-party battery client -------------------------------
# A small standalone sd-bus client that feeds the `battery` slot with TWO gauges — the
# WiVRn headset battery and the UPower laptop battery. The PURE parts (value/diff/latch
# model + TOML config) go in hypxrhud_battery_core (no sd-bus) so they unit-test with the
# rest; the thin bus adapters (sources + panel client) live in the executable.
add_library(hypxrhud_battery_core STATIC
    src/battery/BatteryModel.cpp
    src/battery/BatteryConfig.cpp
)
target_link_libraries(hypxrhud_battery_core PUBLIC hypxrhud_core)
target_compile_options(hypxrhud_battery_core PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

add_executable(hypxrhud-battery
    src/battery/main.cpp
    src/battery/BatterySources.cpp
    src/battery/BatteryClient.cpp
)
target_link_libraries(hypxrhud-battery PRIVATE hypxrhud_battery_core PkgConfig::libsystemd)
target_compile_options(hypxrhud-battery PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
install(TARGETS hypxrhud-battery RUNTIME DESTINATION bin)

# The battery client's systemd user unit (a plain client, enabled at login — no D-Bus
# activation of its own; its first CreatePanel bus-activates the hud daemon).
install(FILES systemd/hypxrhud-battery.service
        DESTINATION lib/systemd/user)

# ---- D-Bus service activation (WP-H3) ----
# Bus-activated on the first CreatePanel/GetCapabilities; the SystemdService= hands off to
# the user unit. See systemd/hypxrhud.service + dbus/*.service.
install(FILES dbus/io.github.andrewgaspar.hypxrhud.service
        DESTINATION share/dbus-1/services)
install(FILES systemd/hypxrhud.service
        DESTINATION lib/systemd/user)

# ---- example config + licence ----
install(FILES examples/hypxrhud.toml DESTINATION share/hypxrhud/examples)
install(FILES LICENSE                 DESTINATION share/licenses/hypxrhud)

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
    tests/test_props.cpp     # WP-H3: props -> SUpsert mapping
    tests/test_backoff.cpp   # WP-H4: re-probe backoff schedule
    tests/test_theme.cpp     # WP-H6: palette parser + resolution (fixtures)
    tests/test_arbiter.cpp   # WP-H5: slot queueing + occupancy introspection
    tests/test_battery.cpp   # battery client: model/diff/latch + config
)
target_include_directories(hypxrhud_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/third_party)
target_link_libraries(hypxrhud_tests PRIVATE hypxrhud_core hypxrhud_battery_core)
target_compile_options(hypxrhud_tests PRIVATE -Wall -Wextra)
add_test(NAME hypxrhud_tests COMMAND hypxrhud_tests)

# ---- D-Bus integration test (WP-H3) ----
# Spawns the REAL hypxrhud binary in runtime-absent mode (--no-xr) on a PRIVATE session bus
# (dbus-run-session) and a client that asserts create/update/dismiss round-trips, the
# per-client cap, NameOwnerChanged auto-dismiss, and GetCapabilities. Never touches the
# user's real session bus. Skipped with a clear message if dbus-run-session is unavailable.
add_executable(hypxrhud_dbus_tests tests/dbus_integration.cpp)
target_include_directories(hypxrhud_dbus_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hypxrhud_dbus_tests PRIVATE hypxrhud_core PkgConfig::libsystemd)
target_compile_options(hypxrhud_dbus_tests PRIVATE -Wall -Wextra)

find_program(DBUS_RUN_SESSION dbus-run-session)
if(DBUS_RUN_SESSION)
    add_test(NAME hypxrhud_dbus_tests
             COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:hypxrhud_dbus_tests>)
    set_tests_properties(hypxrhud_dbus_tests PROPERTIES
        ENVIRONMENT "HYPXRHUD_BIN=$<TARGET_FILE:hypxrhud>;HYPXRHUD_DBUS_TEST_OK=1"
        TIMEOUT 60)
else()
    add_test(NAME hypxrhud_dbus_tests
             COMMAND ${CMAKE_COMMAND} -E echo "SKIP: dbus-run-session not found; D-Bus integration test skipped")
    message(STATUS "hypxrhud: dbus-run-session not found — integration test will report SKIP")
endif()

# ---- battery client D-Bus integration test ------------------------------------------
# Drives the REAL CBatteryClient against a private-bus hypxrhud --no-xr daemon with
# synthetic gauges (no dependency on live UPower/WiVRn). Same dbus-run-session wrapper.
add_executable(hypxrhud_battery_dbus_tests
    tests/battery_integration.cpp
    src/battery/BatteryClient.cpp)
target_include_directories(hypxrhud_battery_dbus_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(hypxrhud_battery_dbus_tests PRIVATE hypxrhud_core PkgConfig::libsystemd)
target_compile_options(hypxrhud_battery_dbus_tests PRIVATE -Wall -Wextra -Wno-missing-field-initializers)

if(DBUS_RUN_SESSION)
    add_test(NAME hypxrhud_battery_dbus_tests
             COMMAND ${DBUS_RUN_SESSION} -- $<TARGET_FILE:hypxrhud_battery_dbus_tests>)
    set_tests_properties(hypxrhud_battery_dbus_tests PROPERTIES
        ENVIRONMENT "HYPXRHUD_BIN=$<TARGET_FILE:hypxrhud>;HYPXRHUD_DBUS_TEST_OK=1"
        TIMEOUT 60)
else()
    add_test(NAME hypxrhud_battery_dbus_tests
             COMMAND ${CMAKE_COMMAND} -E echo "SKIP: dbus-run-session not found; battery integration test skipped")
endif()
