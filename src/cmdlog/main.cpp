#include "CmdLogClient.hpp"
#include "CmdLogConfig.hpp"
#include "CmdLogModel.hpp"
#include "CmdLogService.hpp"
#include "Log.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <string>
#include <systemd/sd-bus.h>
#include <vector>

// hypxrhud-cmdlog — the command ticker producer. It owns
// io.github.andrewgaspar.hypxrhud.cmdlog, takes one string per `Publish` (the `hyprctl`
// PATH shim fires one per command), keeps the last few in a pure model that ages each row
// out on its own TTL, and mirrors the visible rows into ONE hypxrhud text panel — created
// on the first row, dismissed the moment the last one expires, so an idle session shows no
// ticker at all.
//
// Single-threaded, one poll() loop over the single session-bus fd (the same connection
// serves the ticker name AND talks to the HUD), like hypxrhud-battery.

using namespace hudcmd;

namespace {
    std::atomic<bool> g_stop{false};
    void              onSignal(int) { g_stop.store(true); }

    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void usage(const char* argv0) {
        std::fprintf(stderr,
                     "hypxrhud-cmdlog — hyprctl command ticker for hypxrhud (filming aid)\n"
                     "\nUsage: %s [options]\n"
                     "  --config <path>    Config (default $XDG_CONFIG_HOME/hypxrhud/cmd.toml)\n"
                     "  --publish <line>   Send one command line to the RUNNING ticker and exit\n"
                     "  --rows             Print the rows the running ticker is showing and exit\n"
                     "  --verbose          Debug lifecycle logging\n"
                     "  -h, --help\n",
                     argv0);
    }

    bool takeValue(int& index, int argc, char** argv, std::string& out) {
        if (index + 1 >= argc)
            return false;
        out = argv[++index];
        return true;
    }

    int busTimeoutMs(sd_bus* bus) {
        uint64_t usec = 0;
        if (!bus || sd_bus_get_timeout(bus, &usec) < 0 || usec == UINT64_MAX)
            return -1;
        return static_cast<int>(usec / 1000);
    }

    int busEvents(sd_bus* bus) {
        if (!bus)
            return 0;
        const int events = sd_bus_get_events(bus);
        return events < 0 ? POLLIN : events;
    }

    // --publish: a one-shot client call, so a live check needs no busctl incantation.
    int publishOnce(const std::string& line) {
        sd_bus* bus = nullptr;
        if (sd_bus_open_user(&bus) < 0 || !bus) {
            Log::log(Log::ERR, "cannot open the session bus");
            return 1;
        }
        sd_bus_error    error = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        const int       result = sd_bus_call_method(bus, kCmdLogBusName, kCmdLogObjPath, kCmdLogIface,
                                                    "Publish", &error, &reply, "s", line.c_str());
        if (reply)
            sd_bus_message_unref(reply);
        if (result < 0)
            Log::log(Log::ERR, "publish failed ({}) — is hypxrhud-cmdlog running?",
                     error.message ? error.message : std::strerror(-result));
        sd_bus_error_free(&error);
        sd_bus_flush_close_unref(bus);
        return result < 0 ? 1 : 0;
    }

    // --rows: read the running ticker's visible rows (a headset-free verification).
    int printRows() {
        sd_bus* bus = nullptr;
        if (sd_bus_open_user(&bus) < 0 || !bus) {
            Log::log(Log::ERR, "cannot open the session bus");
            return 1;
        }
        sd_bus_error    error = SD_BUS_ERROR_NULL;
        sd_bus_message* reply = nullptr;
        int             result = sd_bus_get_property(bus, kCmdLogBusName, kCmdLogObjPath, kCmdLogIface,
                                                     "Rows", &error, &reply, "as");
        if (result >= 0) {
            const char* row = nullptr;
            sd_bus_message_enter_container(reply, 'a', "s");
            while (sd_bus_message_read(reply, "s", &row) > 0)
                std::fprintf(stdout, "%s\n", row);
            sd_bus_message_exit_container(reply);
        } else
            Log::log(Log::ERR, "cannot read rows ({}) — is hypxrhud-cmdlog running?",
                     error.message ? error.message : std::strerror(-result));
        if (reply)
            sd_bus_message_unref(reply);
        sd_bus_error_free(&error);
        sd_bus_flush_close_unref(bus);
        return result < 0 ? 1 : 0;
    }
}

int main(int argc, char** argv) {
    std::string configPath     = defaultCmdLogConfigPath();
    bool        configExplicit = false;
    std::string publishLine;
    bool        publishMode = false, rowsMode = false;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        }
        if (arg == "--verbose") {
            Log::setLevel(Log::DEBUG);
            continue;
        }
        if (arg == "--rows") {
            rowsMode = true;
            continue;
        }
        if (arg == "--config") {
            if (!takeValue(i, argc, argv, configPath)) {
                Log::log(Log::ERR, "--config requires a value");
                return 2;
            }
            configExplicit = true;
            continue;
        }
        if (arg == "--publish") {
            if (!takeValue(i, argc, argv, publishLine)) {
                Log::log(Log::ERR, "--publish requires a value");
                return 2;
            }
            publishMode = true;
            continue;
        }
        Log::log(Log::ERR, "unknown option: {}", arg);
        usage(argv[0]);
        return 2;
    }

    if (rowsMode)
        return printRows();
    if (publishMode)
        return publishOnce(publishLine);

    SCmdLogConfig            config;
    std::vector<std::string> errors, warnings;
    if (!loadCmdLogConfigFile(configPath, config, errors, warnings, !configExplicit)) {
        for (const auto& error : errors)
            Log::log(Log::ERR, "config: {}", error);
        return 2;
    }
    for (const auto& warning : warnings)
        Log::log(Log::DEBUG, "config: {}", warning);

    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0 || !bus) {
        Log::log(Log::ERR, "cannot open the session bus");
        return 1;
    }
    struct SBusGuard {
        sd_bus* bus = nullptr;
        ~SBusGuard() {
            if (bus)
                sd_bus_flush_close_unref(bus);
        }
    } busGuard{bus};

    int returnCode = 0;
    {
        CCmdLogModel  model(config);
        CCmdLogClient client(bus, config);
        bool          dirty = false;

        if (!client.init()) {
            Log::log(Log::ERR, "cannot watch HUD panel lifecycle");
            return 1;
        }

        CCmdLogService service(
            bus,
            [&](const std::string& line) {
                if (model.publish(line, nowMs()))
                    dirty = true;
            },
            [&]() { return model.rows(); });
        if (!service.init())
            return 1;

        struct sigaction action = {};
        action.sa_handler = onSignal;
        sigaction(SIGINT, &action, nullptr);
        sigaction(SIGTERM, &action, nullptr);

        Log::log(Log::INFO, "hypxrhud-cmdlog ready on {} (slot {}, {} rows, {} ms)",
                 kCmdLogBusName, config.slot, config.history, config.ttlMs);

        int64_t retryAt = -1; // set when the HUD could not be reached.
        while (!g_stop.load()) {
            const int64_t now = nowMs();
            if (model.expire(now))
                dirty = true;

            if (dirty || (retryAt > 0 && now >= retryAt)) {
                if (client.sync(model.lines())) {
                    dirty   = false;
                    retryAt = -1;
                } else {
                    dirty   = false;
                    retryAt = now + 2000; // daemon absent/slot busy: retry without spinning.
                }
            }

            int64_t deadline = -1;
            if (const int64_t expiry = model.nextExpiryMs(); expiry >= 0)
                deadline = expiry;
            if (retryAt > 0 && (deadline < 0 || retryAt < deadline))
                deadline = retryAt;

            int timeout = deadline < 0 ? -1 : static_cast<int>(std::max<int64_t>(0, deadline - nowMs()));
            if (const int busTimeout = busTimeoutMs(bus); busTimeout >= 0)
                timeout = timeout < 0 ? busTimeout : std::min(timeout, busTimeout);

            pollfd fd = {.fd = sd_bus_get_fd(bus), .events = static_cast<short>(busEvents(bus)), .revents = 0};
            const int pollResult = poll(&fd, 1, timeout);
            if (pollResult < 0 && errno != EINTR) {
                Log::log(Log::ERR, "poll failed: {}", std::strerror(errno));
                returnCode = 1;
                break;
            }
            if (fd.revents & (POLLHUP | POLLERR | POLLNVAL)) {
                Log::log(Log::ERR, "session bus connection lost");
                returnCode = 1;
                break;
            }

            int processResult = 0;
            while ((processResult = sd_bus_process(bus, nullptr)) > 0) { /* drain in + out */ }
            if (processResult < 0) {
                Log::log(Log::ERR, "bus processing failed: {}", std::strerror(-processResult));
                returnCode = 1;
                break;
            }
        }

        Log::log(Log::INFO, "hypxrhud-cmdlog exiting");
        // ~CCmdLogService releases the name; ~CCmdLogClient dismisses the panel, so a stop
        // never leaves a frozen command on screen.
    }

    return returnCode;
}
