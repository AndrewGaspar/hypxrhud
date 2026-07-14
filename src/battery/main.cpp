#include "BatteryClient.hpp"
#include "BatteryConfig.hpp"
#include "BatteryModel.hpp"
#include "BatterySources.hpp"
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

// hypxrhud-battery — the first-party battery client. Feeds the hypxrhud `battery` slot with
// two gauges: the WiVRn headset battery and the UPower laptop battery. Single-threaded
// sd-bus client: it folds the session bus (hypxrhud + WiVRn) and the system bus (UPower)
// into one poll() loop, re-reading on a config interval AND early on a source's
// PropertiesChanged, and only UpdatePanel-ing when the rounded gauges actually change
// (zero-cost-when-static). See docs/battery-wivrn.md for the WiVRn battery mechanism.

using namespace hudbat;

namespace {
    std::atomic<bool> g_stop{false};
    void onSignal(int) { g_stop.store(true); }

    // Set by the PropertiesChanged match callbacks; makes the loop re-read before the timer.
    std::atomic<bool> g_dirty{true};

    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    int onPropsChanged(sd_bus_message*, void*, sd_bus_error*) {
        g_dirty.store(true);
        return 0;
    }

    void printUsage(const char* a0) {
        std::fprintf(stderr,
                     "hypxrhud-battery — headset (WiVRn) + laptop (UPower) battery gauges for hypxrhud\n"
                     "\nUsage: %s [options]\n"
                     "  --config <path>  Config file (default $XDG_CONFIG_HOME/hypxrhud/battery.toml)\n"
                     "  --once           Read the sources once, sync the panel, print, and exit\n"
                     "  --verbose        Debug logging\n"
                     "  -h, --help\n",
                     a0);
    }

    // sd_bus timeout in ms (or a fallback), for folding into poll().
    int busTimeoutMs(sd_bus* b) {
        if (!b) return -1;
        uint64_t usec = 0;
        if (sd_bus_get_timeout(b, &usec) < 0) return -1;
        if (usec == UINT64_MAX) return -1;
        return (int)(usec / 1000);
    }
    int busEvents(sd_bus* b) {
        if (!b) return 0;
        int e = sd_bus_get_events(b);
        return e < 0 ? POLLIN : e;
    }
    void drain(sd_bus* b) {
        if (!b) return;
        while (sd_bus_process(b, nullptr) > 0) { /* drain in + out */ }
    }
}

int main(int argc, char** argv) {
    std::string configPath = defaultBatteryConfigPath();
    bool        once = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-h" || a == "--help") { printUsage(argv[0]); return 0; }
        else if (a == "--config") {
            if (i + 1 >= argc) { Log::log(Log::ERR, "--config requires a value"); return 2; }
            configPath = argv[++i];
        }
        else if (a == "--once")    once = true;
        else if (a == "--verbose") Log::setLevel(Log::DEBUG);
        else { Log::log(Log::ERR, "unknown option: {}", a); printUsage(argv[0]); return 2; }
    }

    // ---- config ----
    SBatteryConfig           cfg;
    std::vector<std::string> errs, warns;
    if (!loadBatteryConfigFile(configPath, cfg, errs, warns)) {
        for (const auto& e : errs) Log::log(Log::ERR, "config: {}", e);
        return 2;
    }
    for (const auto& w : warns) Log::log(Log::DEBUG, "config: {}", w);

    SModelParams mp;
    mp.showHeadset   = cfg.showHeadset;
    mp.showLaptop    = cfg.showLaptop;
    mp.headsetLabel  = cfg.headsetLabel;
    mp.laptopLabel   = cfg.laptopLabel;
    mp.lowThreshold  = cfg.lowThreshold;
    mp.lowHysteresis = cfg.lowHysteresis;

    // ---- buses ----
    // Session bus: the hypxrhud daemon + WiVRn live here. Fatal if unavailable (nothing to
    // draw on). System bus: UPower. Optional — a desktop without UPower just omits the laptop
    // gauge.
    sd_bus* session = nullptr;
    if (sd_bus_open_user(&session) < 0 || !session) {
        Log::log(Log::ERR, "cannot open the session bus (no hypxrhud daemon reachable)");
        return 1;
    }
    sd_bus* system = nullptr;
    if (sd_bus_open_system(&system) < 0) {
        Log::log(Log::WARN, "cannot open the system bus — laptop (UPower) gauge disabled");
        system = nullptr;
    }

    // Early-wake matches: re-read as soon as a source's properties change.
    if (system) {
        sd_bus_match_signal(system, nullptr, kUPowerBus, kUPowerPath,
                            "org.freedesktop.DBus.Properties", "PropertiesChanged", onPropsChanged, nullptr);
    }
    sd_bus_match_signal(session, nullptr, kWivrnBus, kWivrnPath,
                        "org.freedesktop.DBus.Properties", "PropertiesChanged", onPropsChanged, nullptr);

    CBatteryClient client(session, cfg.slot);

    struct sigaction sa = {};
    sa.sa_handler = onSignal;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    std::vector<hud::SGauge> lastSent;
    bool                     haveSent = false;
    SLatch                   headsetLatch, laptopLatch;

    auto recompute = [&]() {
        SSourceReading headset = readWivrn(session);
        SSourceReading laptop  = system ? readUpower(system) : SSourceReading{};

        // One-shot low-battery toasts (gated on the source being shown).
        if (cfg.toasts) {
            if (mp.showHeadset && lowBatteryLatch(headsetLatch, headset, cfg.lowThreshold, cfg.lowHysteresis))
                client.postToast(lowBatteryToastText(mp.headsetLabel, headset.percent));
            if (mp.showLaptop && lowBatteryLatch(laptopLatch, laptop, cfg.lowThreshold, cfg.lowHysteresis))
                client.postToast(lowBatteryToastText(mp.laptopLabel, laptop.percent));
        }

        std::vector<hud::SGauge> gauges = buildGauges(headset, laptop, mp);

        // Zero-cost-when-static: only touch the panel when the rounded gauges changed, OR
        // when we still owe an initial create (haveSent flips true only on a successful sync).
        if (!haveSent || !gaugesEqual(gauges, lastSent)) {
            if (client.syncPanel(gauges)) {
                lastSent = gauges;
                haveSent = true;
            }
        }
        return std::make_pair(headset, laptop);
    };

    if (once) {
        auto [h, l] = recompute();
        std::fprintf(stderr, "headset: %s", h.present ? "" : "absent\n");
        if (h.present) std::fprintf(stderr, "%.0f%%%s\n", h.percent, h.charging ? " (charging)" : "");
        std::fprintf(stderr, "laptop:  %s", l.present ? "" : "absent\n");
        if (l.present) std::fprintf(stderr, "%.0f%%%s\n", l.percent, l.charging ? " (charging)" : "");
        std::fprintf(stderr, "headset connected: %s\n", wivrnHeadsetConnected(session) ? "yes" : "no");
        drain(session);
        if (system) drain(system);
        if (system) sd_bus_flush_close_unref(system);
        sd_bus_flush_close_unref(session);
        return 0;
    }

    Log::log(Log::INFO, "hypxrhud-battery started (poll {}s, low {}%)", cfg.pollIntervalSec, cfg.lowThreshold);

    int64_t nextPoll = 0; // fire immediately.
    while (!g_stop.load()) {
        const int64_t now = nowMs();

        if (g_dirty.exchange(false) || now >= nextPoll) {
            recompute();
            nextPoll = now + (int64_t)cfg.pollIntervalSec * 1000;
        }

        // If we owe a create but the daemon was not ready, retry sooner than the full poll.
        int64_t deadline = nextPoll;
        if (!haveSent) {
            std::vector<hud::SGauge> want = buildGauges(readWivrn(session),
                                                        system ? readUpower(system) : SSourceReading{}, mp);
            if (!want.empty())
                deadline = std::min(deadline, now + 2000);
        }

        int      tmo = (int)std::max<int64_t>(0, deadline - nowMs());
        int      bt1 = busTimeoutMs(session);
        int      bt2 = busTimeoutMs(system);
        if (bt1 >= 0) tmo = std::min(tmo, bt1);
        if (bt2 >= 0) tmo = std::min(tmo, bt2);

        struct pollfd fds[2];
        int           nfds = 0;
        fds[nfds++] = {sd_bus_get_fd(session), (short)busEvents(session), 0};
        if (system)
            fds[nfds++] = {sd_bus_get_fd(system), (short)busEvents(system), 0};

        int pr = poll(fds, nfds, tmo);
        if (pr < 0 && errno != EINTR) {
            Log::log(Log::ERR, "poll: {}", strerror(errno));
            break;
        }
        drain(session);
        if (system) drain(system);
    }

    Log::log(Log::INFO, "hypxrhud-battery exiting");
    if (system) sd_bus_flush_close_unref(system);
    sd_bus_flush_close_unref(session);
    return 0;
}
