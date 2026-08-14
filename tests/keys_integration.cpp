#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Dbus.hpp"
#include "keys/KeysClient.hpp"

#include <chrono>
#include <cstdlib>
#include <signal.h>
#include <spawn.h>
#include <systemd/sd-bus.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

using namespace std::chrono_literals;
extern char** environ;

namespace {
    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    bool waitForDaemon(sd_bus* bus, int timeoutMs) {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            sd_bus_error error = SD_BUS_ERROR_NULL;
            sd_bus_message* reply = nullptr;
            const int result = sd_bus_call_method(bus, hud::kBusName, hud::kObjPath, hud::kIface,
                                                  "GetCapabilities", &error, &reply, "");
            if (reply) sd_bus_message_unref(reply);
            sd_bus_error_free(&error);
            if (result >= 0)
                return true;
            std::this_thread::sleep_for(30ms);
        }
        return false;
    }

    uint32_t panelCount(sd_bus* bus) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        uint32_t count = 0;
        sd_bus_get_property_trivial(bus, hud::kBusName, hud::kObjPath, hud::kIface,
                                    "PanelCount", &error, 'u', &count);
        sd_bus_error_free(&error);
        return count;
    }

    uint32_t createCompetingKeysPanel(sd_bus* bus) {
        sd_bus_message* call = nullptr;
        sd_bus_message_new_method_call(bus, &call, hud::kBusName, hud::kObjPath, hud::kIface, "CreatePanel");
        sd_bus_message_open_container(call, 'a', "{sv}");
        auto appendString = [&](const char* key, const char* value) {
            sd_bus_message_open_container(call, 'e', "sv");
            sd_bus_message_append(call, "s", key);
            sd_bus_message_append(call, "v", "s", value);
            sd_bus_message_close_container(call);
        };
        appendString("slot", "keys");
        appendString("title", "synthetic competitor");
        sd_bus_message_open_container(call, 'e', "sv");
        sd_bus_message_append(call, "s", "urgency");
        sd_bus_message_append(call, "v", "u", static_cast<uint32_t>(2));
        sd_bus_message_close_container(call);
        sd_bus_message_close_container(call);

        sd_bus_error error = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        const int result = sd_bus_call(bus, call, 0, &error, &reply);
        uint32_t id = 0;
        if (result >= 0)
            sd_bus_message_read(reply, "u", &id);
        if (reply) sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        sd_bus_message_unref(call);
        return id;
    }
}

TEST_CASE("keys client discloses capture and recreates after PanelDismissed on a private bus") {
    const char* binary = std::getenv("HYPXRHUD_BIN");
    if (!std::getenv("HYPXRHUD_DBUS_TEST_OK") || !binary) {
        MESSAGE("SKIP: not under the private-bus CTest wrapper");
        return;
    }
    REQUIRE(std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr);

    pid_t daemonPid = -1;
    const char* argv[] = {binary, "--no-xr", nullptr};
    REQUIRE(posix_spawn(&daemonPid, binary, nullptr, nullptr, const_cast<char* const*>(argv), environ) == 0);

    sd_bus* observer = nullptr;
    REQUIRE(sd_bus_open_user(&observer) >= 0);
    struct SGuard {
        pid_t pid = -1;
        sd_bus* bus = nullptr;
        ~SGuard() {
            if (bus) sd_bus_flush_close_unref(bus);
            if (pid > 0) {
                kill(pid, SIGTERM);
                int status = 0;
                waitpid(pid, &status, 0);
            }
        }
    } guard{daemonPid, observer};

    REQUIRE(waitForDaemon(observer, 8000));
    sd_bus* producerBus = nullptr;
    REQUIRE(sd_bus_open_user(&producerBus) >= 0);
    {
        hudkeys::SKeysConfig config;
        config.privacyHoldMs = 20;
        config.riseMs = 0;
        hudkeys::CKeysClient client(producerBus, config);
        REQUIRE(client.init());
        CHECK_FALSE(client.runtimeLive()); // --no-xr must never satisfy main's capture gate.
        REQUIRE(client.showPrivacyIndicator());
        const uint32_t indicatorId = client.panelId();
        REQUIRE(indicatorId != 0);
        CHECK(panelCount(observer) == 1);
        const auto neverPresented = client.presentation(indicatorId);
        REQUIRE(neverPresented);
        CHECK(neverPresented->panelSerial == 0);
        CHECK(neverPresented->frameSerial == 0);
        CHECK(neverPresented->streakStart == 0);

        // Equal urgency is last-writer-wins: a competing client preempts the disclosure and
        // drives the real PanelDismissed callback without relying on transient expiry.
        REQUIRE(createCompetingKeysPanel(observer) != 0);
        const int64_t deadline = nowMs() + 2000;
        while (nowMs() < deadline && client.panelId() != 0) {
            REQUIRE(client.process());
            std::this_thread::sleep_for(20ms);
        }
        CHECK(client.panelId() == 0);

        REQUIRE(client.sync({hud::SLine{"synthetic shortcut", hud::EColor::Accent, true}}));
        CHECK(client.panelId() != 0);
        CHECK(client.panelId() != indicatorId);
        CHECK(panelCount(observer) == 1);

        // Losing the well-known HUD owner is capture-fatal, even though the session bus
        // itself remains connected. Main observes this and stops its exact source child.
        kill(daemonPid, SIGTERM);
        int daemonStatus = 0;
        REQUIRE(waitpid(daemonPid, &daemonStatus, 0) == daemonPid);
        guard.pid = -1;
        const int64_t ownerDeadline = nowMs() + 2000;
        while (nowMs() < ownerDeadline && client.healthy()) {
            client.process();
            std::this_thread::sleep_for(20ms);
        }
        CHECK_FALSE(client.healthy());
        CHECK(client.panelId() == 0);
    }
    sd_bus_flush_close_unref(producerBus);
}
