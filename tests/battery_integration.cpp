// hypxrhud-battery — D-Bus integration test. Spawns the REAL hypxrhud daemon in
// runtime-absent mode (--no-xr) on a PRIVATE session bus (dbus-run-session) and drives the
// REAL CBatteryClient against it with SYNTHETIC gauge readings — so the test is
// deterministic regardless of whether this box has a UPower battery or a WiVRn headset. It
// asserts the battery panel appears on CreatePanel, survives an update, and is dismissed
// when the gauge list goes empty, plus that a low-battery toast lands as a second panel.
//
// Mirrors tests/dbus_integration.cpp: never registers a name on the user's real bus (the
// CMake wrapper always launches it under dbus-run-session with HYPXRHUD_DBUS_TEST_OK=1).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Dbus.hpp"                    // kBusName / kObjPath / kIface
#include "battery/BatteryClient.hpp"   // the real client under test

#include <chrono>
#include <cstdlib>
#include <spawn.h>
#include <string>
#include <systemd/sd-bus.h>
#include <thread>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

using namespace std::chrono_literals;
extern char** environ;

namespace {
    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    uint32_t panelCount(sd_bus* bus) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        uint32_t     n   = 0;
        sd_bus_get_property_trivial(bus, hud::kBusName, hud::kObjPath, hud::kIface, "PanelCount", &err, 'u', &n);
        sd_bus_error_free(&err);
        return n;
    }
    bool waitForDaemon(sd_bus* bus, int timeoutMs) {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            sd_bus_error err = SD_BUS_ERROR_NULL;
            sd_bus_message* reply = nullptr;
            int r = sd_bus_call_method(bus, hud::kBusName, hud::kObjPath, hud::kIface, "GetCapabilities", &err, &reply, "");
            bool ok = r >= 0;
            sd_bus_error_free(&err);
            if (reply) sd_bus_message_unref(reply);
            if (ok) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    }
    bool waitPanelCount(sd_bus* bus, uint32_t want, int timeoutMs) {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            if (panelCount(bus) == want) return true;
            std::this_thread::sleep_for(30ms);
        }
        return panelCount(bus) == want;
    }
    bool waitPanelAtLeast(sd_bus* bus, uint32_t want, int timeoutMs) {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            if (panelCount(bus) >= want) return true;
            std::this_thread::sleep_for(30ms);
        }
        return panelCount(bus) >= want;
    }
    std::vector<hud::SGauge> g2(float headsetPct, bool headsetChg, float laptopPct, bool laptopChg) {
        return {hud::SGauge{"headset", headsetPct, headsetChg}, hud::SGauge{"laptop", laptopPct, laptopChg}};
    }
}

TEST_CASE("battery client: panel appears, updates, and is dismissed against the live daemon") {
    const char* bin = std::getenv("HYPXRHUD_BIN");
    if (!std::getenv("HYPXRHUD_DBUS_TEST_OK") || !bin) {
        MESSAGE("SKIP: not under the ctest dbus-run-session wrapper (HYPXRHUD_DBUS_TEST_OK/HYPXRHUD_BIN unset)");
        return;
    }
    REQUIRE(std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr);

    pid_t       pid    = -1;
    const char* argv[] = {bin, "--no-xr", nullptr};
    REQUIRE(posix_spawn(&pid, bin, nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0);
    REQUIRE(pid > 0);

    sd_bus* bus = nullptr;
    REQUIRE(sd_bus_open_user(&bus) >= 0);
    struct Guard {
        pid_t pid; sd_bus* bus;
        ~Guard() {
            if (bus) sd_bus_flush_close_unref(bus);
            if (pid > 0) { kill(pid, SIGTERM); int st = 0; waitpid(pid, &st, 0); }
        }
    } guard{pid, bus};

    REQUIRE(waitForDaemon(bus, 8000));
    CHECK(panelCount(bus) == 0);

    // The client uses its OWN connection (as it would in production).
    sd_bus* clientBus = nullptr;
    REQUIRE(sd_bus_open_user(&clientBus) >= 0);
    hudbat::CBatteryClient client(clientBus, "battery");

    SUBCASE("create -> update -> dismiss lifecycle") {
        // First sync creates the panel.
        CHECK(client.syncPanel(g2(55, false, 83, true)));
        CHECK(client.havePanel());
        CHECK(waitPanelCount(bus, 1, 2000));

        // An update keeps the count at 1.
        CHECK(client.syncPanel(g2(54, false, 82, true)));
        std::this_thread::sleep_for(150ms);
        CHECK(panelCount(bus) == 1);

        // A headset-only list (laptop gone) still one panel.
        CHECK(client.syncPanel({hud::SGauge{"headset", 40, false}}));
        std::this_thread::sleep_for(150ms);
        CHECK(panelCount(bus) == 1);

        // Empty gauge list -> the client dismisses the panel (never a stale gauge).
        CHECK(client.syncPanel({}));
        CHECK_FALSE(client.havePanel());
        CHECK(waitPanelCount(bus, 0, 2000));
    }

    SUBCASE("low-battery toast lands as a second panel") {
        CHECK(client.syncPanel(g2(10, false, 83, false)));
        CHECK(waitPanelCount(bus, 1, 2000));
        client.postToast("headset battery 10%");
        // battery panel + toast panel.
        CHECK(waitPanelAtLeast(bus, 2, 2000));

        // cleanup
        client.syncPanel({});
    }

    sd_bus_flush_close_unref(clientBus);
}
