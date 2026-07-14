// hypxrhud — D-Bus integration test (WP-H3). Spawns the REAL hypxrhud binary in
// runtime-absent mode (--no-xr) on a PRIVATE session bus (dbus-run-session provides it via
// $DBUS_SESSION_BUS_ADDRESS) and drives it as a client, asserting the full method/signal/
// property/lifetime surface. It NEVER registers a name on the user's real bus — the CMake
// wrapper always launches it under dbus-run-session and sets HYPXRHUD_DBUS_TEST_OK=1; the
// test refuses to run without that marker.
//
// Matrix:
//   - GetCapabilities returns the expected a{sv} shape (version/budget/slots/spaces)
//   - CreatePanel (string + a(sub) lines variants) returns a nonzero id; PanelCount tracks
//   - UpdatePanel round-trips (fire-and-forget still acks)
//   - RuntimeState property == "absent" under --no-xr
//   - per-client cap (default 4): the 5th create is refused with a clear error
//   - DismissPanel emits PanelDismissed(id,"client") and drops PanelCount
//   - NameOwnerChanged auto-dismiss: a second client's panels vanish when it disconnects

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Dbus.hpp" // kBusName / kObjPath / kIface

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <string>
#include <systemd/sd-bus.h>
#include <thread>
#include <vector>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

using namespace hud;
using namespace std::chrono_literals;

extern char** environ;

namespace {

    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // Build a CreatePanel props message: slot(s) + urgency(u) + one a(sub) line.
    // Returns the panel id (0 on error) and fills `err` string.
    uint32_t createPanel(sd_bus* bus, const char* slot, uint32_t urgency, const char* line,
                         std::string& errOut) {
        sd_bus_message* m = nullptr;
        sd_bus_message_new_method_call(bus, &m, kBusName, kObjPath, kIface, "CreatePanel");
        sd_bus_message_open_container(m, 'a', "{sv}");

        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "slot");
        sd_bus_message_append(m, "v", "s", slot);
        sd_bus_message_close_container(m);

        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "urgency");
        sd_bus_message_append(m, "v", "u", urgency);
        sd_bus_message_close_container(m);

        // lines: a(sub) = [(text, colorRole=2 Accent, big=true)]
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "lines");
        sd_bus_message_open_container(m, 'v', "a(sub)");
        sd_bus_message_open_container(m, 'a', "(sub)");
        sd_bus_message_append(m, "(sub)", line, (uint32_t)2, (int)1);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);

        sd_bus_message_close_container(m); // a{sv}

        sd_bus_error   err   = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        int r = sd_bus_call(bus, m, 0, &err, &reply);
        uint32_t id = 0;
        if (r < 0) {
            errOut = err.name ? err.name : "call-failed";
        } else {
            sd_bus_message_read(reply, "u", &id);
        }
        sd_bus_error_free(&err);
        if (reply) sd_bus_message_unref(reply);
        sd_bus_message_unref(m);
        return id;
    }

    uint32_t panelCount(sd_bus* bus) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        uint32_t     n   = 0;
        sd_bus_get_property_trivial(bus, kBusName, kObjPath, kIface, "PanelCount", &err, 'u', &n);
        sd_bus_error_free(&err);
        return n;
    }

    std::string runtimeState(sd_bus* bus) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        char*        s   = nullptr;
        std::string  out;
        if (sd_bus_get_property_string(bus, kBusName, kObjPath, kIface, "RuntimeState", &err, &s) >= 0 && s)
            out = s;
        free(s);
        sd_bus_error_free(&err);
        return out;
    }

    // Wait until the daemon owns the name (its GetCapabilities answers) or timeout.
    bool waitForDaemon(sd_bus* bus, int timeoutMs) {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            sd_bus_error   err   = SD_BUS_ERROR_NULL;
            sd_bus_message* reply = nullptr;
            int r = sd_bus_call_method(bus, kBusName, kObjPath, kIface, "GetCapabilities", &err, &reply, "");
            bool ok = r >= 0;
            sd_bus_error_free(&err);
            if (reply) sd_bus_message_unref(reply);
            if (ok)
                return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    }

    // Poll PanelCount until it equals `want` or timeout (auto-dismiss is asynchronous).
    bool waitPanelCount(sd_bus* bus, uint32_t want, int timeoutMs) {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            if (panelCount(bus) == want)
                return true;
            std::this_thread::sleep_for(30ms);
        }
        return panelCount(bus) == want;
    }

} // namespace

TEST_CASE("dbus: full create/update/dismiss + cap + NameOwnerChanged auto-dismiss") {
    const char* bin = std::getenv("HYPXRHUD_BIN");
    if (!std::getenv("HYPXRHUD_DBUS_TEST_OK") || !bin) {
        MESSAGE("SKIP: not under the ctest dbus-run-session wrapper (HYPXRHUD_DBUS_TEST_OK/HYPXRHUD_BIN unset)");
        return;
    }
    REQUIRE(std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr); // private bus from dbus-run-session.

    // ---- spawn the daemon in runtime-absent mode on this private bus ----
    pid_t       pid    = -1;
    const char* argv[] = {bin, "--no-xr", nullptr};
    int         sr     = posix_spawn(&pid, bin, nullptr, nullptr, const_cast<char* const*>(argv), environ);
    REQUIRE(sr == 0);
    REQUIRE(pid > 0);

    sd_bus* bus = nullptr;
    REQUIRE(sd_bus_open_user(&bus) >= 0);

    // Ensure the daemon is torn down no matter how the test exits.
    struct Guard {
        pid_t pid; sd_bus* bus;
        ~Guard() {
            if (bus) sd_bus_flush_close_unref(bus);
            if (pid > 0) { kill(pid, SIGTERM); int st = 0; waitpid(pid, &st, 0); }
        }
    } guard{pid, bus};

    REQUIRE(waitForDaemon(bus, 8000));

    SUBCASE("GetCapabilities and runtime-absent state") {
        sd_bus_error   err   = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        REQUIRE(sd_bus_call_method(bus, kBusName, kObjPath, kIface, "GetCapabilities", &err, &reply, "") >= 0);
        // Walk the a{sv} and collect the keys present.
        std::vector<std::string> keys;
        REQUIRE(sd_bus_message_enter_container(reply, 'a', "{sv}") >= 0);
        while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
            const char* k = nullptr;
            sd_bus_message_read(reply, "s", &k);
            if (k) keys.push_back(k);
            sd_bus_message_skip(reply, "v");
            sd_bus_message_exit_container(reply);
        }
        sd_bus_message_exit_container(reply);
        sd_bus_message_unref(reply);
        sd_bus_error_free(&err);

        auto has = [&](const char* k) {
            for (auto& s : keys) if (s == k) return true;
            return false;
        };
        CHECK(has("version"));
        CHECK(has("budget"));
        CHECK(has("slots"));
        CHECK(has("spaces"));
        CHECK(has("perClientCap"));

        CHECK(runtimeState(bus) == "absent"); // --no-xr never probes a runtime.
    }

    SUBCASE("create / update / dismiss round-trip with signal") {
        CHECK(panelCount(bus) == 0);

        std::string err;
        uint32_t    id = createPanel(bus, "voice", 1, "listening", err);
        CHECK(id != 0);
        CHECK(err.empty());
        CHECK(waitPanelCount(bus, 1, 1000));

        // UpdatePanel (fire-and-forget shape, but we call it expecting a reply -> still acks).
        {
            sd_bus_message* m = nullptr;
            sd_bus_message_new_method_call(bus, &m, kBusName, kObjPath, kIface, "UpdatePanel");
            sd_bus_message_append(m, "u", id);
            sd_bus_message_open_container(m, 'a', "{sv}");
            sd_bus_message_open_container(m, 'e', "sv");
            sd_bus_message_append(m, "s", "urgency");
            sd_bus_message_append(m, "v", "u", (uint32_t)2);
            sd_bus_message_close_container(m);
            sd_bus_message_close_container(m);
            sd_bus_error   e2 = SD_BUS_ERROR_NULL;
            sd_bus_message* rep = nullptr;
            int r = sd_bus_call(bus, m, 0, &e2, &rep);
            CHECK(r >= 0);
            if (rep) sd_bus_message_unref(rep);
            sd_bus_error_free(&e2);
            sd_bus_message_unref(m);
        }
        CHECK(panelCount(bus) == 1); // an update never changes the count.

        // Subscribe for PanelDismissed, then dismiss and confirm the signal + count drop.
        std::vector<std::pair<uint32_t, std::string>> got;
        auto cb = [](sd_bus_message* msg, void* ud, sd_bus_error*) -> int {
            auto* v = static_cast<std::vector<std::pair<uint32_t, std::string>>*>(ud);
            uint32_t did = 0; const char* reason = nullptr;
            if (sd_bus_message_read(msg, "us", &did, &reason) >= 0)
                v->emplace_back(did, reason ? reason : "");
            return 0;
        };
        sd_bus_slot* slot = nullptr;
        sd_bus_match_signal(bus, &slot, kBusName, kObjPath, kIface, "PanelDismissed", cb, &got);

        sd_bus_error   de  = SD_BUS_ERROR_NULL;
        sd_bus_message* dr = nullptr;
        CHECK(sd_bus_call_method(bus, kBusName, kObjPath, kIface, "DismissPanel", &de, &dr, "u", id) >= 0);
        if (dr) sd_bus_message_unref(dr);
        sd_bus_error_free(&de);

        // Pump the client bus to receive the broadcast signal.
        const int64_t deadline = nowMs() + 1000;
        while (nowMs() < deadline && got.empty()) {
            sd_bus_process(bus, nullptr);
            sd_bus_wait(bus, 50 * 1000);
        }
        REQUIRE(got.size() >= 1);
        CHECK(got[0].first == id);
        CHECK(got[0].second == "client");
        CHECK(waitPanelCount(bus, 0, 1000));
        sd_bus_slot_unref(slot);
    }

    SUBCASE("per-client cap refuses the 5th panel with a clear error") {
        std::string err;
        std::vector<uint32_t> ids;
        for (int i = 0; i < 4; i++) {
            // distinct free-placement panels (slot "" bypasses singleton arbitration).
            uint32_t id = createPanel(bus, "", 1, "p", err);
            CHECK(id != 0);
            ids.push_back(id);
        }
        CHECK(waitPanelCount(bus, 4, 1000));
        // 5th create -> rejected.
        uint32_t id5 = createPanel(bus, "", 1, "overflow", err);
        CHECK(id5 == 0);
        CHECK(err.find("Rejected") != std::string::npos);
        CHECK(panelCount(bus) == 4);

        // clean up so the next subcase starts empty.
        for (uint32_t id : ids) {
            sd_bus_error de = SD_BUS_ERROR_NULL; sd_bus_message* dr = nullptr;
            sd_bus_call_method(bus, kBusName, kObjPath, kIface, "DismissPanel", &de, &dr, "u", id);
            if (dr) sd_bus_message_unref(dr);
            sd_bus_error_free(&de);
        }
    }

    SUBCASE("NameOwnerChanged: a client's panels vanish when it disconnects") {
        CHECK(waitPanelCount(bus, 0, 1000));

        // A SECOND client connection creates panels, then drops.
        sd_bus* client2 = nullptr;
        REQUIRE(sd_bus_open_user(&client2) >= 0);
        std::string err;
        CHECK(createPanel(client2, "media", 1, "now playing", err) != 0);
        CHECK(createPanel(client2, "status", 1, "status", err) != 0);
        CHECK(waitPanelCount(bus, 2, 1500));

        // Disconnect client2 -> the DBus daemon emits NameOwnerChanged(new_owner="") ->
        // hypxrhud auto-dismisses both panels (reason "client-gone").
        sd_bus_flush_close_unref(client2);
        CHECK(waitPanelCount(bus, 0, 3000));
    }
}
