// hypxrhud-cmdlog — D-Bus integration test. Spawns the REAL hypxrhud daemon (--no-xr) AND
// the REAL hypxrhud-cmdlog producer on a PRIVATE session bus (dbus-run-session), then drives
// the REAL `hyprctl` shim script against them. It asserts the whole filming path:
// shim -> Publish -> ticker rows -> one HUD panel, plus truncation, the skip list, per-row
// expiry (panel dismissed, not merely blank), the HYPXR_CMD_HUD=0 kill switch, and survival
// of a HUD restart.
//
// SAFETY: the shim is never allowed to reach a real compositor. Every invocation runs with
// PATH pointing at a stub `hyprctl` (and HYPRLAND_INSTANCE_SIGNATURE unset), so the exec
// target is always the stub; the stub also proves args/stdin/stdout/exit-code passthrough.
//
// Mirrors tests/battery_integration.cpp and tests/keys_integration.cpp: never registers a
// name on the user's real bus (the CMake wrapper always launches it under dbus-run-session
// with HYPXRHUD_DBUS_TEST_OK=1).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "Dbus.hpp" // kBusName / kObjPath / kIface
#include "cmdlog/CmdLogService.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <signal.h>
#include <spawn.h>
#include <sstream>
#include <string>
#include <sys/wait.h>
#include <systemd/sd-bus.h>
#include <thread>
#include <unistd.h>
#include <vector>

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
            sd_bus_error    error = SD_BUS_ERROR_NULL;
            sd_bus_message* reply = nullptr;
            const int       result = sd_bus_call_method(bus, hud::kBusName, hud::kObjPath, hud::kIface,
                                                        "GetCapabilities", &error, &reply, "");
            if (reply)
                sd_bus_message_unref(reply);
            sd_bus_error_free(&error);
            if (result >= 0)
                return true;
            std::this_thread::sleep_for(30ms);
        }
        return false;
    }

    uint32_t panelCount(sd_bus* bus) {
        sd_bus_error error = SD_BUS_ERROR_NULL;
        uint32_t     count = 0;
        sd_bus_get_property_trivial(bus, hud::kBusName, hud::kObjPath, hud::kIface,
                                    "PanelCount", &error, 'u', &count);
        sd_bus_error_free(&error);
        return count;
    }

    // The ticker's own introspection: exactly the rows the HUD panel is showing.
    bool readRows(sd_bus* bus, std::vector<std::string>& out) {
        out.clear();
        sd_bus_error    error = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        const int result = sd_bus_get_property(bus, hudcmd::kCmdLogBusName, hudcmd::kCmdLogObjPath,
                                               hudcmd::kCmdLogIface, "Rows", &error, &reply, "as");
        if (result >= 0) {
            const char* row = nullptr;
            sd_bus_message_enter_container(reply, 'a', "s");
            while (sd_bus_message_read(reply, "s", &row) > 0)
                out.emplace_back(row);
            sd_bus_message_exit_container(reply);
        }
        if (reply)
            sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        return result >= 0;
    }

    bool waitForTicker(sd_bus* bus, int timeoutMs) {
        const int64_t            deadline = nowMs() + timeoutMs;
        std::vector<std::string> rows;
        while (nowMs() < deadline) {
            if (readRows(bus, rows))
                return true;
            std::this_thread::sleep_for(30ms);
        }
        return false;
    }

    // Timing-robust waits: assert on the NEWEST row (or emptiness), never on a row count
    // that a TTL could change under the test.
    bool waitForFirstRow(sd_bus* bus, const std::string& expected, int timeoutMs) {
        const int64_t            deadline = nowMs() + timeoutMs;
        std::vector<std::string> rows;
        while (nowMs() < deadline) {
            if (readRows(bus, rows) && !rows.empty() && rows.front() == expected)
                return true;
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }

    bool waitForNoRows(sd_bus* bus, int timeoutMs) {
        const int64_t            deadline = nowMs() + timeoutMs;
        std::vector<std::string> rows;
        while (nowMs() < deadline) {
            if (readRows(bus, rows) && rows.empty())
                return true;
            std::this_thread::sleep_for(20ms);
        }
        return false;
    }

    bool waitForPanelCount(sd_bus* bus, uint32_t want, int timeoutMs) {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            if (panelCount(bus) == want)
                return true;
            std::this_thread::sleep_for(20ms);
        }
        return panelCount(bus) == want;
    }

    bool anyRowContains(const std::vector<std::string>& rows, const std::string& needle) {
        for (const auto& row : rows)
            if (row.find(needle) != std::string::npos)
                return true;
        return false;
    }

    struct SShimRun {
        int         exitCode = -1;
        std::string stdoutText;
    };

    // Run the shim exactly as a user's shell would, with the exec target forced to a stub.
    SShimRun runShim(const std::string& shim, const std::string& stubDir, const std::string& stubLog,
                     const std::string& extraEnv, const std::string& args) {
        // The stub dir comes FIRST in PATH, so the shim's "next hyprctl in PATH that is not
        // me" resolution lands on the stub and never on a real one; /usr/bin:/bin follow
        // only so the shim's own `#!/usr/bin/env bash` and `busctl` still resolve.
        std::ostringstream command;
        command << "printf 'STDIN-OK' | env -u HYPRLAND_INSTANCE_SIGNATURE "
                << "PATH=" << stubDir << ":/usr/bin:/bin STUB_LOG=" << stubLog << " " << extraEnv << " "
                << shim << " " << args << " 2>/dev/null";
        SShimRun run;
        FILE*    pipe = popen(command.str().c_str(), "r");
        if (!pipe)
            return run;
        char buffer[256];
        while (std::fgets(buffer, sizeof(buffer), pipe))
            run.stdoutText += buffer;
        const int status = pclose(pipe);
        run.exitCode = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
        return run;
    }

    std::string readFile(const std::string& path) {
        std::ifstream     file(path);
        std::stringstream contents;
        contents << file.rdbuf();
        return contents.str();
    }
}

TEST_CASE("the hyprctl shim drives the ticker end to end on a private bus") {
    const char* daemonBin = std::getenv("HYPXRHUD_BIN");
    const char* tickerBin = std::getenv("HYPXRHUD_CMDLOG_BIN");
    const char* shimPath  = std::getenv("HYPXRHUD_CMDLOG_SHIM");
    if (!std::getenv("HYPXRHUD_DBUS_TEST_OK") || !daemonBin || !tickerBin || !shimPath) {
        MESSAGE("SKIP: not under the private-bus CTest wrapper");
        return;
    }
    REQUIRE(std::getenv("DBUS_SESSION_BUS_ADDRESS") != nullptr);

    // ---- a scratch dir: the stub hyprctl, its log, and the ticker's config ----
    const std::string workDir = (std::filesystem::temp_directory_path() /
                                 ("hypxrhud-cmdlog-test-" + std::to_string(getpid()))).string();
    std::filesystem::create_directories(workDir + "/bin");
    const std::string stubDir = workDir + "/bin";
    const std::string stubLog = workDir + "/stub.log";
    {
        std::ofstream stub(stubDir + "/hyprctl");
        stub << "#!/bin/sh\n"
             << "{ printf 'ARGS:'; for a in \"$@\"; do printf ' [%s]' \"$a\"; done; printf '\\n'; } >> \"$STUB_LOG\"\n"
             << "cat >> \"$STUB_LOG\"; printf '\\n' >> \"$STUB_LOG\"\n"
             << "echo STUB-STDOUT\n"
             << "exit 7\n";
    }
    std::filesystem::permissions(stubDir + "/hyprctl", std::filesystem::perms::owner_all);
    const std::string configPath = workDir + "/cmd.toml";
    {
        std::ofstream config(configPath);
        config << "[cmd]\nslot = \"status\"\nhistory = 3\nttl_ms = 3000\nmax_chars = 24\n"
                  "coalesce_ms = 0\nrise_ms = 0\nfade_ms = 0\nopacity = 0.92\n";
    }

    // ---- the real daemon + the real ticker, both on the private bus ----
    pid_t       daemonPid = -1, tickerPid = -1;
    const char* daemonArgv[] = {daemonBin, "--no-xr", nullptr};
    REQUIRE(posix_spawn(&daemonPid, daemonBin, nullptr, nullptr,
                        const_cast<char* const*>(daemonArgv), environ) == 0);

    sd_bus* observer = nullptr;
    REQUIRE(sd_bus_open_user(&observer) >= 0);
    struct SGuard {
        pid_t       daemonPid = -1, tickerPid = -1;
        sd_bus*     bus = nullptr;
        std::string workDir;
        ~SGuard() {
            if (bus)
                sd_bus_flush_close_unref(bus);
            for (pid_t pid : {tickerPid, daemonPid}) {
                if (pid > 0) {
                    kill(pid, SIGTERM);
                    int status = 0;
                    waitpid(pid, &status, 0);
                }
            }
            std::error_code ignored;
            std::filesystem::remove_all(workDir, ignored);
        }
    } guard{daemonPid, -1, observer, workDir};

    REQUIRE(waitForDaemon(observer, 8000));
    CHECK(panelCount(observer) == 0);

    const char* tickerArgv[] = {tickerBin, "--config", configPath.c_str(), "--verbose", nullptr};
    REQUIRE(posix_spawn(&tickerPid, tickerBin, nullptr, nullptr,
                        const_cast<char* const*>(tickerArgv), environ) == 0);
    guard.tickerPid = tickerPid;
    REQUIRE(waitForTicker(observer, 8000));

    std::vector<std::string> rows;
    REQUIRE(readRows(observer, rows));
    CHECK(rows.empty());
    CHECK(panelCount(observer) == 0); // nothing published yet: no ticker panel at all.

    // ---- 1. a state-changing command: published, rendered, and passed straight through ----
    SShimRun run = runShim(shimPath, stubDir, stubLog, "", "reload");
    CHECK(run.exitCode == 7);                                   // the real command's status.
    CHECK(run.stdoutText == "STUB-STDOUT\n");                   // the real command's stdout.
    CHECK(waitForFirstRow(observer, "hyprctl reload", 3000));
    CHECK(waitForPanelCount(observer, 1, 2000));
    const std::string stubbed = readFile(stubLog);
    CHECK(stubbed.find("ARGS: [reload]") != std::string::npos); // untouched argv.
    CHECK(stubbed.find("STDIN-OK") != std::string::npos);       // untouched stdin.

    // ---- 2. a long command keeps its head and is truncated for display ----
    run = runShim(shimPath, stubDir, stubLog, "", "dispatch exec kitty --title demo");
    CHECK(run.exitCode == 7);
    CHECK(waitForFirstRow(observer, "hyprctl dispatch exec...", 3000)); // max_chars = 24.
    REQUIRE(readRows(observer, rows));
    REQUIRE(rows.size() >= 2);
    CHECK(rows[1] == "hyprctl reload"); // newest first, older below.

    // ---- 3. read-only queries are skipped (both by verb and by -j) ----
    run = runShim(shimPath, stubDir, stubLog, "", "monitors");
    CHECK(run.exitCode == 7);
    run = runShim(shimPath, stubDir, stubLog, "", "-j clients");
    CHECK(run.exitCode == 7);
    std::this_thread::sleep_for(400ms);
    REQUIRE(readRows(observer, rows));
    CHECK_FALSE(anyRowContains(rows, "monitors"));
    CHECK_FALSE(anyRowContains(rows, "clients"));

    // ---- 4. the kill switch skips publishing without touching the real command ----
    run = runShim(shimPath, stubDir, stubLog, "HYPXR_CMD_HUD=0", "keyword general:gaps_in 8");
    CHECK(run.exitCode == 7);
    CHECK(run.stdoutText == "STUB-STDOUT\n");
    std::this_thread::sleep_for(400ms);
    REQUIRE(readRows(observer, rows));
    CHECK_FALSE(anyRowContains(rows, "gaps_in"));
    CHECK(readFile(stubLog).find("[keyword] [general:gaps_in] [8]") != std::string::npos);

    // An explicit real-binary override takes the same path (what a packaged install pins).
    run = runShim(shimPath, stubDir, stubLog,
                  "HYPXR_CMD_HUD_REAL=" + stubDir + "/hyprctl", "dispatch killactive");
    CHECK(run.exitCode == 7);
    CHECK(waitForFirstRow(observer, "hyprctl dispatch kill...", 3000));

    // ---- 5. every row expires and the panel is DISMISSED, not left blank ----
    CHECK(waitForNoRows(observer, 8000));
    CHECK(waitForPanelCount(observer, 0, 3000));

    // ---- 6. the ticker survives a HUD restart (stale panel id, non-fatal) ----
    kill(daemonPid, SIGTERM);
    int daemonStatus = 0;
    REQUIRE(waitpid(daemonPid, &daemonStatus, 0) == daemonPid);
    guard.daemonPid = -1;
    run = runShim(shimPath, stubDir, stubLog, "", "reload");
    CHECK(run.exitCode == 7); // the real command never depends on the HUD being up.

    pid_t restartedPid = -1;
    REQUIRE(posix_spawn(&restartedPid, daemonBin, nullptr, nullptr,
                        const_cast<char* const*>(daemonArgv), environ) == 0);
    guard.daemonPid = restartedPid;
    REQUIRE(waitForDaemon(observer, 8000));
    run = runShim(shimPath, stubDir, stubLog, "", "dispatch workspace 2");
    CHECK(run.exitCode == 7);
    CHECK(waitForFirstRow(observer, "hyprctl dispatch work...", 4000));
    CHECK(waitForPanelCount(observer, 1, 3000)); // re-created against the new daemon.
}
