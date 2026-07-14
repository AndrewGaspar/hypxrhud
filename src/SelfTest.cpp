#include "SelfTest.hpp"

#include "Dbus.hpp" // kBusName / kObjPath / kIface

#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <spawn.h>
#include <string>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <thread>
#include <unistd.h>
#include <vector>

extern char** environ;

namespace hud {

namespace {
    using namespace std::chrono_literals;

    void pass(const char* step) { std::fprintf(stderr, "  [ok]   %s\n", step); }
    void fail(const char* step) { std::fprintf(stderr, "  [FAIL] %s\n", step); }

    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    std::string selfExe() {
        char buf[4096];
        ssize_t n = readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (n <= 0)
            return "";
        buf[n] = '\0';
        return buf;
    }

    // busctl-shaped CreatePanel: {slot, urgency, one big accent line}. Returns id (0 = error).
    uint32_t createPanel(sd_bus* bus, const char* slot, uint32_t urgency, const char* line) {
        sd_bus_message* m = nullptr;
        if (sd_bus_message_new_method_call(bus, &m, kBusName, kObjPath, kIface, "CreatePanel") < 0)
            return 0;
        sd_bus_message_open_container(m, 'a', "{sv}");
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "slot");
        sd_bus_message_append(m, "v", "s", slot);
        sd_bus_message_close_container(m);
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "urgency");
        sd_bus_message_append(m, "v", "u", urgency);
        sd_bus_message_close_container(m);
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "lines");
        sd_bus_message_open_container(m, 'v', "a(sub)");
        sd_bus_message_open_container(m, 'a', "(sub)");
        sd_bus_message_append(m, "(sub)", line, (uint32_t)2, (int)1);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m); // a{sv}

        sd_bus_error    err   = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        uint32_t        id    = 0;
        if (sd_bus_call(bus, m, 0, &err, &reply) >= 0)
            sd_bus_message_read(reply, "u", &id);
        sd_bus_error_free(&err);
        if (reply) sd_bus_message_unref(reply);
        sd_bus_message_unref(m);
        return id;
    }

    bool updateUrgency(sd_bus* bus, uint32_t id, uint32_t urgency) {
        sd_bus_message* m = nullptr;
        if (sd_bus_message_new_method_call(bus, &m, kBusName, kObjPath, kIface, "UpdatePanel") < 0)
            return false;
        sd_bus_message_append(m, "u", id);
        sd_bus_message_open_container(m, 'a', "{sv}");
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "urgency");
        sd_bus_message_append(m, "v", "u", urgency);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
        sd_bus_error    err   = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        int r = sd_bus_call(bus, m, 0, &err, &reply);
        sd_bus_error_free(&err);
        if (reply) sd_bus_message_unref(reply);
        sd_bus_message_unref(m);
        return r >= 0;
    }

    uint32_t panelCount(sd_bus* bus) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        uint32_t     n   = 0;
        sd_bus_get_property_trivial(bus, kBusName, kObjPath, kIface, "PanelCount", &err, 'u', &n);
        sd_bus_error_free(&err);
        return n;
    }

    // GetCapabilities must return an a{sv} carrying at least these keys.
    bool capsHaveExpectedKeys(sd_bus* bus) {
        sd_bus_error    err   = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        bool ok = false;
        if (sd_bus_call_method(bus, kBusName, kObjPath, kIface, "GetCapabilities", &err, &reply, "") >= 0) {
            std::vector<std::string> keys;
            if (sd_bus_message_enter_container(reply, 'a', "{sv}") >= 0) {
                while (sd_bus_message_enter_container(reply, 'e', "sv") > 0) {
                    const char* k = nullptr;
                    sd_bus_message_read(reply, "s", &k);
                    if (k) keys.push_back(k);
                    sd_bus_message_skip(reply, "v");
                    sd_bus_message_exit_container(reply);
                }
                sd_bus_message_exit_container(reply);
            }
            auto has = [&](const char* k) {
                for (auto& s : keys) if (s == k) return true;
                return false;
            };
            ok = has("version") && has("slots") && has("spaces") && has("perClientCap") &&
                 has("slotOccupancy");
        }
        sd_bus_error_free(&err);
        if (reply) sd_bus_message_unref(reply);
        return ok;
    }

    bool dismiss(sd_bus* bus, uint32_t id) {
        sd_bus_error    err   = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        int r = sd_bus_call_method(bus, kBusName, kObjPath, kIface, "DismissPanel", &err, &reply, "u", id);
        sd_bus_error_free(&err);
        if (reply) sd_bus_message_unref(reply);
        return r >= 0;
    }

    bool waitForDaemon(sd_bus* bus, int timeoutMs) {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            sd_bus_error    err   = SD_BUS_ERROR_NULL;
            sd_bus_message* reply = nullptr;
            int  r  = sd_bus_call_method(bus, kBusName, kObjPath, kIface, "GetCapabilities", &err, &reply, "");
            bool ok = r >= 0;
            sd_bus_error_free(&err);
            if (reply) sd_bus_message_unref(reply);
            if (ok) return true;
            std::this_thread::sleep_for(50ms);
        }
        return false;
    }

    // The actual test, run inside the private bus. Returns 0 on success.
    int runInner() {
        std::string self = selfExe();
        if (self.empty()) {
            std::fprintf(stderr, "self-test: cannot resolve /proc/self/exe\n");
            return 1;
        }
        if (!std::getenv("DBUS_SESSION_BUS_ADDRESS")) {
            std::fprintf(stderr, "self-test: no private session bus (dbus-run-session failed?)\n");
            return 1;
        }

        // Spawn the daemon in runtime-absent mode on this private bus.
        pid_t       pid    = -1;
        const char* argv[] = {self.c_str(), "--no-xr", nullptr};
        if (posix_spawn(&pid, self.c_str(), nullptr, nullptr, const_cast<char* const*>(argv), environ) != 0) {
            std::fprintf(stderr, "self-test: failed to spawn the daemon\n");
            return 1;
        }

        sd_bus* bus = nullptr;
        if (sd_bus_open_user(&bus) < 0) {
            std::fprintf(stderr, "self-test: cannot open the private session bus\n");
            kill(pid, SIGTERM); waitpid(pid, nullptr, 0);
            return 1;
        }

        int rc = 0;
        auto check = [&](bool cond, const char* step) {
            if (cond) pass(step);
            else { fail(step); rc = 1; }
            return cond;
        };

        std::fprintf(stderr, "hypxrhud --self-test (private bus, --no-xr daemon):\n");

        if (!check(waitForDaemon(bus, 8000), "daemon owns io.github.andrewgaspar.hypxrhud")) {
            sd_bus_flush_close_unref(bus);
            kill(pid, SIGTERM); waitpid(pid, nullptr, 0);
            return 1;
        }

        check(capsHaveExpectedKeys(bus), "GetCapabilities returns the expected keys");

        uint32_t id = createPanel(bus, "voice", 1, "self-test");
        check(id != 0, "CreatePanel returns a nonzero id");
        check(panelCount(bus) == 1, "PanelCount == 1 after create");
        check(updateUrgency(bus, id, 2), "UpdatePanel acks");
        check(panelCount(bus) == 1, "PanelCount still 1 after update");
        check(dismiss(bus, id), "DismissPanel acks");
        check(panelCount(bus) == 0, "PanelCount == 0 after dismiss");

        sd_bus_flush_close_unref(bus);
        kill(pid, SIGTERM);
        int st = 0;
        waitpid(pid, &st, 0);

        std::fprintf(stderr, "\nself-test: %s\n", rc == 0 ? "PASS" : "FAIL");
        return rc;
    }
}

int runSelfTest() {
    // First entry: re-exec under a private session bus so we never touch the real one.
    if (!std::getenv("HYPXRHUD_SELFTEST_BUS")) {
        std::string self = selfExe();
        if (self.empty()) {
            std::fprintf(stderr, "self-test: cannot resolve own path\n");
            return 1;
        }
        setenv("HYPXRHUD_SELFTEST_BUS", "1", 1);
        const char* argv[] = {"dbus-run-session", "--", self.c_str(), "--self-test", nullptr};
        execvp("dbus-run-session", const_cast<char* const*>(argv));
        std::fprintf(stderr, "self-test: dbus-run-session not found (%s); cannot create a private bus\n",
                     std::strerror(errno));
        return 1;
    }
    return runInner();
}

} // namespace hud
