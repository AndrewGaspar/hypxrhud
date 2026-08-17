#include "doctest.h"

#include "cmdlog/CmdLogConfig.hpp"
#include "cmdlog/CmdLogModel.hpp"

#include <string>
#include <vector>

// The command ticker's PURE half: sanitising, head-truncation, history/coalescing, and
// per-row expiry, plus the config parser's ranges. No bus, no XR — the same split the
// battery/keys clients use (tests/test_battery.cpp, tests/test_keys.cpp).

using namespace hudcmd;

namespace {
    SCmdLogConfig cfg(int history = 3, int ttlMs = 6000, int maxChars = 60, int coalesceMs = 1500) {
        SCmdLogConfig config;
        config.history    = history;
        config.ttlMs      = ttlMs;
        config.maxChars   = maxChars;
        config.coalesceMs = coalesceMs;
        return config;
    }
}

TEST_CASE("sanitise folds control characters and collapses whitespace") {
    CHECK(sanitizeCommand("hyprctl dispatch exec kitty") == "hyprctl dispatch exec kitty");
    CHECK(sanitizeCommand("  hyprctl   keyword  general:gaps_in 8  ") == "hyprctl keyword general:gaps_in 8");
    // A quoted newline/tab in an argument must never become a second HUD row.
    CHECK(sanitizeCommand("hyprctl dispatch exec 'a\nb\tc'") == "hyprctl dispatch exec 'a b c'");
    CHECK(sanitizeCommand("").empty());
    CHECK(sanitizeCommand(" \n\t ").empty());

    // Bounded regardless of what a caller sends.
    const std::string huge = "hyprctl " + std::string(4000, 'x');
    CHECK(sanitizeCommand(huge).size() <= 1024);
}

TEST_CASE("truncation keeps the head and never splits a UTF-8 sequence") {
    CHECK(truncateHead("hyprctl reload", 60) == "hyprctl reload");
    CHECK(truncateHead("hyprctl dispatch exec kitty --title demo", 20) == "hyprctl dispatch ...");
    CHECK(truncateHead("hyprctl dispatch exec kitty --title demo", 20).size() == 20);

    // Nine two-byte codepoints: truncating to 6 keeps 3 codepoints (6 bytes) + "...".
    std::string wide;
    for (int i = 0; i < 9; ++i)
        wide += "α";
    const std::string cut = truncateHead(wide, 6);
    CHECK(cut == "ααα...");
    CHECK(cut.size() == 9); // 3 * 2 bytes + 3 ASCII dots — no half codepoint.
}

TEST_CASE("model keeps the newest commands first and bounds history") {
    CCmdLogModel model(cfg());
    CHECK(model.empty());
    CHECK_FALSE(model.publish("   ", 1000)); // nothing to show; no row.
    CHECK(model.empty());

    CHECK(model.publish("hyprctl dispatch exec kitty", 1000));
    CHECK(model.publish("hyprctl keyword general:gaps_in 8", 1100));
    CHECK(model.publish("hyprctl reload", 1200));
    CHECK(model.publish("hyprctl openxr enable", 1300));

    const auto rows = model.rows();
    REQUIRE(rows.size() == 3); // history = 3; the oldest fell off.
    CHECK(rows[0] == "hyprctl openxr enable");
    CHECK(rows[1] == "hyprctl reload");
    CHECK(rows[2] == "hyprctl keyword general:gaps_in 8");

    const auto lines = model.lines();
    REQUIRE(lines.size() == 3);
    CHECK(lines[0].text == rows[0]);
    CHECK(lines[0].big);
    CHECK(lines[0].color == hud::EColor::Accent);
    CHECK_FALSE(lines[1].big);
    CHECK(lines[1].color == hud::EColor::Dim);
}

TEST_CASE("a repeat inside the coalesce window counts instead of duplicating") {
    CCmdLogModel model(cfg());
    CHECK(model.publish("hyprctl reload", 1000));
    CHECK(model.publish("hyprctl reload", 1500));
    REQUIRE(model.size() == 1);
    CHECK(model.rows()[0] == "hyprctl reload  x2");

    // Outside the window it becomes its own row again.
    CHECK(model.publish("hyprctl reload", 4000));
    REQUIRE(model.size() == 2);
    CHECK(model.rows()[0] == "hyprctl reload");
    CHECK(model.rows()[1] == "hyprctl reload  x2");

    // coalesce_ms = 0 disables it entirely.
    CCmdLogModel plain(cfg(3, 6000, 60, 0));
    CHECK(plain.publish("hyprctl reload", 1000));
    CHECK(plain.publish("hyprctl reload", 1001));
    CHECK(plain.size() == 2);
}

TEST_CASE("each row expires on its own TTL and the ticker empties completely") {
    CCmdLogModel model(cfg(3, 6000));
    model.publish("hyprctl dispatch exec kitty", 1000);
    model.publish("hyprctl reload", 3000);

    CHECK(model.nextExpiryMs() == 7000); // the older row's deadline.
    CHECK_FALSE(model.expire(6999));
    CHECK(model.size() == 2);

    CHECK(model.expire(7000)); // the first row aged out.
    REQUIRE(model.size() == 1);
    CHECK(model.rows()[0] == "hyprctl reload");
    CHECK(model.nextExpiryMs() == 9000);

    CHECK(model.expire(9500));
    CHECK(model.empty());
    CHECK(model.nextExpiryMs() == -1);
    CHECK(model.lines().empty()); // empty lines == the client dismisses the panel.

    // A coalesced repeat refreshes the row's own deadline.
    CCmdLogModel refreshed(cfg(3, 6000));
    refreshed.publish("hyprctl reload", 1000);
    refreshed.publish("hyprctl reload", 2000);
    CHECK(refreshed.nextExpiryMs() == 8000);
}

TEST_CASE("long commands are truncated for display but still coalesce on the full text") {
    CCmdLogModel model(cfg(3, 6000, 24));
    model.publish("hyprctl dispatch exec kitty --title one", 1000);
    model.publish("hyprctl dispatch exec kitty --title two", 1100);
    REQUIRE(model.size() == 2); // same first 24 chars, different commands.
    CHECK(model.rows()[0] == "hyprctl dispatch exec...");
    CHECK(model.rows()[0].size() == 24);
}

TEST_CASE("config parses the [cmd] section and rejects out-of-range values") {
    SCmdLogConfig            config;
    std::vector<std::string> errors, warnings;
    const std::string        text = R"(
[cmd]
slot = "media"     # bottom-left is the battery's
history = 5
ttl_ms = 4000
max_chars = 48
coalesce_ms = 0
rise_ms = 40
fade_ms = 200
opacity = 0.8
)";
    REQUIRE(parseCmdLogConfig(text, config, errors, warnings));
    CHECK(errors.empty());
    CHECK(config.slot == "media");
    CHECK(config.history == 5);
    CHECK(config.ttlMs == 4000);
    CHECK(config.maxChars == 48);
    CHECK(config.coalesceMs == 0);
    CHECK(config.riseMs == 40);
    CHECK(config.fadeMs == 200);
    CHECK(config.opacity == doctest::Approx(0.8));

    SUBCASE("unknown keys and sections warn instead of failing") {
        SCmdLogConfig            other;
        std::vector<std::string> otherErrors, otherWarnings;
        CHECK(parseCmdLogConfig("[cmd]\nfuture_key = 1\n[other]\nx = 1\n", other, otherErrors, otherWarnings));
        CHECK(otherErrors.empty());
        CHECK(otherWarnings.size() == 2);
    }

    SUBCASE("ranges are enforced") {
        SCmdLogConfig            bad;
        std::vector<std::string> badErrors, badWarnings;
        CHECK_FALSE(parseCmdLogConfig("[cmd]\nhistory = 99\nttl_ms = 10\nmax_chars = 4\nopacity = 2.0\n",
                                      bad, badErrors, badWarnings));
        CHECK(badErrors.size() == 4);
    }

    SUBCASE("a malformed value is an error, not a silent default") {
        SCmdLogConfig            bad;
        std::vector<std::string> badErrors, badWarnings;
        CHECK_FALSE(parseCmdLogConfig("[cmd]\nslot = status\n", bad, badErrors, badWarnings));
        CHECK_FALSE(badErrors.empty());
    }
}

TEST_CASE("an implicit missing config uses defaults; an explicit one fails closed") {
    SCmdLogConfig            config;
    std::vector<std::string> errors, warnings;
    CHECK(loadCmdLogConfigFile("/nonexistent/hypxrhud/cmd.toml", config, errors, warnings, true));
    CHECK(errors.empty());
    CHECK(config.slot == "status");
    CHECK(config.history == 3);
    CHECK(config.ttlMs == 6000);

    errors.clear();
    warnings.clear();
    CHECK_FALSE(loadCmdLogConfigFile("/nonexistent/hypxrhud/cmd.toml", config, errors, warnings, false));
    CHECK_FALSE(errors.empty());
}
