#include "KeyEvent.hpp"
#include "KeySource.hpp"
#include "Disclosure.hpp"
#include "KeysClient.hpp"
#include "KeysConfig.hpp"
#include "KeysModel.hpp"
#include "Log.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <signal.h>
#include <string>
#include <systemd/sd-bus.h>
#include <sys/signalfd.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

using namespace hudkeys;

namespace {
    struct SBusGuard {
        sd_bus* bus = nullptr;
        ~SBusGuard() {
            if (bus)
                sd_bus_flush_close_unref(bus);
        }
    };

    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    void usage(const char* argv0) {
        std::fprintf(stderr,
                     "hypxrhud-keys — privacy-visible ShowMeTheKey overlay for hypxrhud\n"
                     "\nUsage: %s [options]\n"
                     "  --config <path>   Config (default $XDG_CONFIG_HOME/hypxrhud/keys.toml)\n"
                     "  --mods-only       Hide ordinary typed text; show shortcuts/control keys\n"
                     "  --rules <name>    XKB rules override\n"
                     "  --model <name>    XKB keyboard model override\n"
                     "  --layout <name>   XKB layout override\n"
                     "  --variant <name>  XKB variant override\n"
                     "  --options <list>  XKB options override\n"
                     "  --verbose         Debug lifecycle logging (never logs key content)\n"
                     "  -h, --help\n",
                     argv0);
    }

    bool takeValue(int& index, int argc, char** argv, std::string& out) {
        if (index + 1 >= argc)
            return false;
        out = argv[++index];
        return true;
    }

    bool waitForRuntimeLive(CKeysClient& client, int timeoutMs) {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            if (!client.process())
                return false;
            if (client.runtimeLive())
                return true;
            pollfd fd = {.fd = client.fd(), .events = static_cast<short>(client.events()), .revents = 0};
            const int result = poll(&fd, 1, 100);
            if (result < 0 && errno != EINTR)
                return false;
            if (fd.revents & (POLLHUP | POLLERR | POLLNVAL))
                return false;
        }
        return false;
    }

    bool waitForPresentation(CKeysClient& client, uint32_t panelId, int timeoutMs,
                             uint64_t& frameSerial, uint64_t& streakStart) {
        const int64_t deadline = nowMs() + timeoutMs;
        while (nowMs() < deadline) {
            if (!client.process() || !client.runtimeLive() || client.panelId() != panelId)
                return false;
            const auto presented = client.presentation(panelId);
            if (!presented)
                return false;
            const EPresentationCheck check = checkPresentation(*presented);
            if (check == EPresentationCheck::Current) {
                frameSerial = presented->frameSerial;
                streakStart = presented->streakStart;
                return true;
            }
            if (check == EPresentationCheck::Omitted)
                return false;
            pollfd fd = {.fd = client.fd(), .events = static_cast<short>(client.events()), .revents = 0};
            const int result = poll(&fd, 1, 50);
            if ((result < 0 && errno != EINTR) || (fd.revents & (POLLHUP | POLLERR | POLLNVAL)))
                return false;
        }
        return false;
    }

    bool holdDisclosure(CKeysClient& client, uint32_t panelId, uint64_t& lastFrame,
                        uint64_t firstStreak, int dwellMs) {
        const int64_t deadline = nowMs() + dwellMs;
        const uint64_t firstFrame = lastFrame;
        while (nowMs() < deadline) {
            pollfd fd = {.fd = client.fd(), .events = static_cast<short>(client.events()), .revents = 0};
            const int remaining = static_cast<int>(std::max<int64_t>(0, deadline - nowMs()));
            const int result = poll(&fd, 1, std::min(100, remaining));
            if ((result < 0 && errno != EINTR) || (fd.revents & (POLLHUP | POLLERR | POLLNVAL)))
                return false;
            if (!client.process() || !client.runtimeLive() || client.panelId() != panelId)
                return false;
            const auto presented = client.presentation(panelId);
            if (!presented || checkPresentation(*presented) != EPresentationCheck::Current ||
                presented->streakStart != firstStreak)
                return false;
            lastFrame = presented->frameSerial;
        }
        return client.panelId() == panelId && lastFrame > firstFrame;
    }
}

int main(int argc, char** argv) {
    std::string configPath = defaultKeysConfigPath();
    std::vector<std::pair<std::string, std::string>> overrides;
    bool forceModsOnly = false;
    bool configExplicit = false;

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
        if (arg == "--mods-only") {
            forceModsOnly = true;
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
        if (arg == "--rules" || arg == "--model" || arg == "--layout" ||
            arg == "--variant" || arg == "--options") {
            std::string value;
            if (!takeValue(i, argc, argv, value)) {
                Log::log(Log::ERR, "{} requires a value", arg);
                return 2;
            }
            overrides.emplace_back(arg, std::move(value));
            continue;
        }
        Log::log(Log::ERR, "unknown option: {}", arg);
        usage(argv[0]);
        return 2;
    }

    SKeysConfig config;
    std::vector<std::string> errors, warnings;
    if (!loadKeysConfigFile(configPath, config, errors, warnings, !configExplicit)) {
        for (const auto& error : errors)
            Log::log(Log::ERR, "config: {}", error);
        return 2;
    }
    for (const auto& warning : warnings)
        Log::log(Log::DEBUG, "config: {}", warning);
    for (const auto& [name, value] : overrides) {
        if (name == "--rules") config.rules = value;
        else if (name == "--model") config.model = value;
        else if (name == "--layout") config.layout = value;
        else if (name == "--variant") config.variant = value;
        else if (name == "--options") config.options = value;
    }
    if (forceModsOnly)
        config.modsOnly = true;

    CKeysModel model(config);
    std::string error;
    if (!model.init(error)) {
        Log::log(Log::ERR, "xkb initialization failed: {}", error);
        return 2;
    }

    sd_bus* bus = nullptr;
    if (sd_bus_open_user(&bus) < 0 || !bus) {
        Log::log(Log::ERR, "cannot open the session bus");
        return 1;
    }
    SBusGuard busGuard{bus};

    int returnCode = 0;
    {
        CKeysClient client(bus, config);
        if (!client.init() || !waitForRuntimeLive(client, 5000)) {
            Log::log(Log::ERR, "HUD runtime is not live; capture was not started");
            return 1;
        }
        if (!client.showPrivacyIndicator()) {
            Log::log(Log::ERR, "cannot establish the key-capture disclosure; capture was not started");
            return 1;
        }
        const uint32_t disclosureId = client.panelId();
        uint64_t firstPresentedFrame = 0;
        uint64_t presentationStreak = 0;
        if (!waitForPresentation(client, disclosureId, std::max(2000, config.riseMs + 1000),
                                 firstPresentedFrame, presentationStreak) ||
            !holdDisclosure(client, disclosureId, firstPresentedFrame, presentationStreak,
                            config.privacyHoldMs)) {
            // Require a live render-capable session for a full rise + frame margin before
            // opening an input device. The exact disclosure ID must remain in each observed
            // successful shouldRender frame; a merely bus-accepted panel is not enough.
            Log::log(Log::ERR, "cannot establish the visible key-capture disclosure; capture was not started");
            return 1;
        }

        CKeySource source;
        if (!source.start(error)) {
            Log::log(Log::ERR, "cannot start the fixed ShowMeTheKey source: {}", error);
            return 1;
        }
        Log::log(Log::INFO, "[keys] capture source active after HUD disclosure dwell");
        int64_t presentationDeadline = nowMs() + 500;

        sigset_t signalSet;
        sigemptyset(&signalSet);
        sigaddset(&signalSet, SIGINT);
        sigaddset(&signalSet, SIGTERM);
        sigprocmask(SIG_BLOCK, &signalSet, nullptr);
        const int signalFd = signalfd(-1, &signalSet, SFD_CLOEXEC | SFD_NONBLOCK);
        if (signalFd < 0) {
            Log::log(Log::ERR, "cannot create signal fd: {}", std::strerror(errno));
            source.stop();
            return 1;
        }

        bool stop = false;
        size_t rejectedRecords = 0;
        while (!stop) {
            pollfd fds[3] = {
                {.fd = source.fd(), .events = POLLIN | POLLHUP, .revents = 0},
                {.fd = client.fd(), .events = static_cast<short>(client.events()), .revents = 0},
                {.fd = signalFd, .events = POLLIN, .revents = 0},
            };
            int timeout = 250;
            if (const int busTimeout = client.timeoutMs(); busTimeout >= 0)
                timeout = std::min(timeout, busTimeout);

            const int pollResult = poll(fds, 3, timeout);
            if (pollResult < 0 && errno != EINTR) {
                Log::log(Log::ERR, "poll failed: {}", std::strerror(errno));
                returnCode = 1;
                break;
            }
            if ((fds[1].revents & (POLLHUP | POLLERR | POLLNVAL)) ||
                !client.process() || !client.runtimeLive() || client.panelId() != disclosureId) {
                Log::log(Log::ERR, "HUD connection/runtime was lost; stopping capture immediately");
                returnCode = 1;
                break;
            }
            const auto currentPresentation = client.presentation(disclosureId);
            if (!currentPresentation ||
                checkPresentation(*currentPresentation) != EPresentationCheck::Current ||
                currentPresentation->streakStart != presentationStreak) {
                Log::log(Log::ERR, "keys disclosure left the submitted layer list; stopping capture immediately");
                returnCode = 1;
                break;
            }
            if (currentPresentation->frameSerial > firstPresentedFrame) {
                firstPresentedFrame = currentPresentation->frameSerial;
                presentationDeadline = nowMs() + 500;
            } else if (nowMs() > presentationDeadline) {
                Log::log(Log::ERR, "keys disclosure presentation stopped advancing; stopping capture immediately");
                returnCode = 1;
                break;
            }

            if (fds[2].revents & POLLIN) {
                signalfd_siginfo info = {};
                read(signalFd, &info, sizeof(info));
                stop = true;
            }

            if (fds[0].revents & (POLLIN | POLLHUP)) {
                error.clear();
                if (!source.readAvailable([&](const std::string& line) {
                        const SParseResult parsed = parseShowMeTheKeyEvent(line);
                        if (parsed.status == EParseStatus::Error) {
                            ++rejectedRecords;
                            return;
                        }
                        if (parsed.status == EParseStatus::Event && model.handle(parsed.event, nowMs()) &&
                            !client.sync(model.lines()))
                            Log::log(Log::WARN, "[keys] HUD sync failed; will retry on the next visible event");
                    }, error)) {
                    Log::log(Log::ERR, "key source protocol failure: {}", error);
                    returnCode = 1;
                    break;
                }
            }

            int status = 0;
            if (source.exited(status)) {
                if (!stop) {
                    if (WIFEXITED(status))
                        Log::log(Log::ERR, "ShowMeTheKey source exited (status {}). Check executable and input-device permissions.",
                                 WEXITSTATUS(status));
                    else
                        Log::log(Log::ERR, "ShowMeTheKey source terminated unexpectedly");
                    returnCode = 1;
                }
                break;
            }
        }

        if (rejectedRecords > 0)
            Log::log(Log::WARN, "[keys] rejected {} malformed source record(s)", rejectedRecords);
        close(signalFd);
        source.stop();
        Log::log(Log::INFO, "[keys] capture source stopped");
    }

    return returnCode;
}
