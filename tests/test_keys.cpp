#include "doctest.h"

#include "keys/KeyEvent.hpp"
#include "keys/Disclosure.hpp"
#include "keys/KeySource.hpp"
#include "keys/KeysConfig.hpp"
#include "keys/KeysModel.hpp"

#include <chrono>
#include <cstdlib>
#include <linux/input-event-codes.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <vector>
#include <unistd.h>

using namespace hudkeys;

namespace {
    SKeyEvent press(uint16_t code) { return {.code = code, .state = EKeyState::Pressed}; }
    SKeyEvent release(uint16_t code) { return {.code = code, .state = EKeyState::Released}; }

    CKeysModel model(SKeysConfig config = {}) {
        CKeysModel result(std::move(config));
        std::string error;
        REQUIRE(result.init(error));
        return result;
    }
}

TEST_CASE("ShowMeTheKey parser accepts only a coherent keyboard schema") {
    const std::string valid =
        R"({"event_name":"KEYBOARD_KEY","event_type":300,"time_stamp":39869802,"key_name":"KEY_C","key_code":46,"state_name":"PRESSED","state_code":1})";
    const auto parsed = parseShowMeTheKeyEvent(valid);
    REQUIRE(parsed.status == EParseStatus::Event);
    CHECK(parsed.event.code == 46);
    CHECK(parsed.event.state == EKeyState::Pressed);
    CHECK(parsed.event.sourceTimestamp == 39869802);

    CHECK(parseShowMeTheKeyEvent(R"({"event_name":"POINTER_MOTION"})").status == EParseStatus::Ignored);
    CHECK(parseShowMeTheKeyEvent("not-json").status == EParseStatus::Error);
    CHECK(parseShowMeTheKeyEvent(R"({"event_name":"KEYBOARD_KEY"})").status == EParseStatus::Error);
    CHECK(parseShowMeTheKeyEvent(
        R"({"event_name":"KEYBOARD_KEY","event_type":300,"time_stamp":1,"key_name":"KEY_C","key_code":46,"state_name":"PRESSED","state_code":0})")
              .status == EParseStatus::Error);
    CHECK(parseShowMeTheKeyEvent(
        R"({"event_name":"KEYBOARD_KEY","event_name":"KEYBOARD_KEY","event_type":300,"time_stamp":1,"key_name":"KEY_C","key_code":46,"state_name":"PRESSED","state_code":1})")
              .status == EParseStatus::Error);
}

TEST_CASE("disclosure gate waits before frames and aborts on exact-panel omission") {
    CHECK(checkPresentation({.panelSerial = 0, .frameSerial = 0}) == EPresentationCheck::Waiting);
    CHECK(checkPresentation({.panelSerial = 0, .frameSerial = 4}) == EPresentationCheck::Waiting);
    CHECK(checkPresentation({.panelSerial = 4, .frameSerial = 4}) == EPresentationCheck::Current);
    CHECK(checkPresentation({.panelSerial = 3, .frameSerial = 4}) == EPresentationCheck::Omitted);
}

TEST_CASE("xkb model translates evdev+8 and orders modifiers deterministically") {
    auto keys = model();
    CHECK_FALSE(keys.handle(press(KEY_LEFTSHIFT), 1));
    CHECK_FALSE(keys.handle(press(KEY_LEFTMETA), 2));
    CHECK_FALSE(keys.handle(press(KEY_LEFTALT), 3));
    CHECK_FALSE(keys.handle(press(KEY_LEFTCTRL), 4));
    CHECK(keys.handle(press(KEY_E), 5));
    const auto lines = keys.lines();
    REQUIRE(lines.size() == 1);
    CHECK(lines[0].text == "Super+Ctrl+Alt+Shift+E");
    CHECK(lines[0].big);
}

TEST_CASE("model suppresses held repeats, coalesces distinct presses, and bounds history") {
    SKeysConfig config;
    config.history = 2;
    config.coalesceMs = 100;
    auto keys = model(config);

    CHECK(keys.handle(press(KEY_A), 0));
    CHECK_FALSE(keys.handle(press(KEY_A), 1));
    CHECK_FALSE(keys.handle(release(KEY_A), 2));
    CHECK(keys.handle(press(KEY_A), 50));
    REQUIRE(keys.lines().size() == 1);
    CHECK(keys.lines()[0].text == "A  x2");
    keys.handle(release(KEY_A), 51);

    CHECK(keys.handle(press(KEY_B), 200));
    keys.handle(release(KEY_B), 201);
    CHECK(keys.handle(press(KEY_C), 300));
    const auto lines = keys.lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text == "C");
    CHECK(lines[1].text == "B");
}

TEST_CASE("mods-only mode does not retain ordinary typed text") {
    SKeysConfig config;
    config.modsOnly = true;
    auto keys = model(config);

    CHECK_FALSE(keys.handle(press(KEY_A), 1));
    keys.handle(release(KEY_A), 2);
    keys.handle(press(KEY_LEFTSHIFT), 3);
    CHECK_FALSE(keys.handle(press(KEY_B), 4));
    keys.handle(release(KEY_B), 5);
    keys.handle(release(KEY_LEFTSHIFT), 6);

    keys.handle(press(KEY_LEFTCTRL), 7);
    CHECK(keys.handle(press(KEY_C), 8));
    keys.handle(release(KEY_C), 9);
    keys.handle(release(KEY_LEFTCTRL), 10);
    CHECK(keys.handle(press(KEY_ENTER), 11));
    const auto lines = keys.lines();
    REQUIRE(lines.size() == 2);
    CHECK(lines[0].text == "Enter");
    CHECK(lines[1].text == "Ctrl+C");
}

TEST_CASE("keys config parses RMLVO, privacy, and bounded model settings") {
    SKeysConfig config;
    std::vector<std::string> errors, warnings;
    CHECK(parseKeysConfig(
        "[keys]\n"
        "rules = \"evdev\"\nmodel = \"pc105\"\nlayout = \"de\"\nvariant = \"nodeadkeys\"\n"
        "options = \"caps:swapescape\"\nslot = \"keys\"\nhistory = 4\ncoalesce_ms = 700\n"
        "rise_ms = 20\nprivacy_hold_ms = 2000\n"
        "opacity = 0.8\nmods_only = true\n",
        config, errors, warnings));
    CHECK(errors.empty());
    CHECK(config.rules == "evdev");
    CHECK(config.model == "pc105");
    CHECK(config.layout == "de");
    CHECK(config.variant == "nodeadkeys");
    CHECK(config.options == "caps:swapescape");
    CHECK(config.history == 4);
    CHECK(config.modsOnly);
    CHECK(config.opacity == doctest::Approx(0.8));

    SKeysConfig invalid;
    errors.clear();
    warnings.clear();
    CHECK_FALSE(parseKeysConfig("[keys]\nhistory = 100\nopacity = 2\n", invalid, errors, warnings));
    CHECK(errors.size() == 2);

    SKeysConfig invisible;
    errors.clear();
    warnings.clear();
    CHECK_FALSE(parseKeysConfig("[keys]\nrise_ms = 100\nprivacy_hold_ms = 100\nopacity = 0.1\n",
                                invisible, errors, warnings));
    CHECK(errors.size() == 2);

    SUBCASE("an explicit missing or unreadable config fails closed") {
        SKeysConfig loaded;
        errors.clear();
        warnings.clear();
        CHECK_FALSE(loadKeysConfigFile("/definitely/not/a/hypxrhud-keys-config.toml", loaded,
                                       errors, warnings, false));
        CHECK_FALSE(errors.empty());

        errors.clear();
        warnings.clear();
        CHECK_FALSE(loadKeysConfigFile("/", loaded, errors, warnings, false));
        CHECK_FALSE(errors.empty());
    }

    SUBCASE("only an implicit missing default may use safe compiled defaults") {
        SKeysConfig loaded;
        errors.clear();
        warnings.clear();
        CHECK(loadKeysConfigFile("/definitely/not/a/hypxrhud-keys-config.toml", loaded,
                                 errors, warnings, true));
        CHECK(errors.empty());
        CHECK(warnings.size() == 1);
    }
}

TEST_CASE("tracked source reads fixture records and terminates only its child") {
    const char* fixture = std::getenv("HYPXRHUD_KEY_SOURCE_FIXTURE");
    if (!fixture) {
        MESSAGE("SKIP: fixture executable path not provided");
        return;
    }

    CKeySource source;
    std::string error;
    REQUIRE(source.startForTesting({fixture}, error));
    const pid_t child = source.pid();
    REQUIRE(child > 0);

    std::vector<std::string> lines;
    for (int i = 0; i < 40 && lines.size() < 2; ++i) {
        pollfd fd = {.fd = source.fd(), .events = POLLIN, .revents = 0};
        poll(&fd, 1, 50);
        REQUIRE(source.readAvailable([&](const std::string& line) { lines.push_back(line); }, error));
    }
    REQUIRE(lines.size() == 2);
    CHECK(parseShowMeTheKeyEvent(lines[0]).status == EParseStatus::Event);
    CHECK(parseShowMeTheKeyEvent(lines[1]).status == EParseStatus::Event);

    source.stop();
    CHECK(source.pid() == -1);
    CHECK(kill(child, 0) == -1);
}

TEST_CASE("tracked source reports exec failure synchronously and retains no child") {
    CKeySource source;
    std::string error;
    CHECK_FALSE(source.startForTesting({"/definitely/not/showmethekey-cli"}, error));
    CHECK_FALSE(error.empty());
    CHECK(source.pid() == -1);
    CHECK(source.fd() == -1);
}
