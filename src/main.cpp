#include "Config.hpp"
#include "Daemon.hpp"
#include "Log.hpp"
#include "PngWrite.hpp"
#include "Preview.hpp"
#include "Scene.hpp"
#include "SelfTest.hpp"
#include "Theme.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <unistd.h>

// hypxrhud — the shared XR HUD daemon. Owns ONE OpenXR overlay session and renders an
// N-panel, slot-based head-locked HUD above HypXRland's monitors. XR utilities push panels
// over D-Bus (io.github.andrewgaspar.hypxrhud) — voice feedback, screenkey, toasts, media,
// battery — instead of each re-implementing session/EGL/swapchain/fade machinery.
//
// WP-H3 adds the sd-bus front end; WP-H4 makes the daemon PERSISTENT: it owns the bus name
// and accepts panels even when no runtime is present, probing on a backoff and rendering
// once the runtime/headset appear, surviving a WiVRn disconnect. The XR session lives
// behind HAVE_XR (built without openxr/egl/gbm the daemon still runs, serving D-Bus and
// reporting the runtime "absent"). `--preview <png>` renders the six-slot composite offline.
//
// SAFETY: only ONE XR runtime per box. Do not start this alongside another live XR session
// by hand.

std::atomic<bool> g_stopRequested{false};
static void       onSignal(int) { g_stopRequested.store(true); }

static constexpr int kExitOk    = 0;
static constexpr int kExitUsage = 2;

static void printUsage(const char* a0) {
    std::fprintf(stderr,
                 "hypxrhud — shared XR HUD daemon for HypXRland\n"
                 "\nUsage: %s [options]   (D-Bus service; see README)\n"
                 "  --config <path>   Config file (default $XDG_CONFIG_HOME/hypxrhud/hypxrhud.toml)\n"
                 "  --preview <png>   Render the six-slot composite to a PNG and exit (no XR)\n"
                 "  --theme <dir>     With --preview: render using this theme dir's palette (mako.ini)\n"
                 "  --self-test       Health check on a PRIVATE bus (spawns the daemon, round-trips); exits 0/nonzero\n"
                 "  --gpu <path>      DRM render node (must match the runtime); overrides config\n"
                 "  --z <int>         Overlay sessionLayersPlacement (default 20); overrides config\n"
                 "  --stdin           Also read the interim NDJSON panel feed on stdin (debug)\n"
                 "  --no-xr           Never probe a runtime; serve D-Bus only (tests/headless)\n"
                 "  --verbose         Debug logging\n"
                 "  -h, --help\n",
                 a0);
}

int main(int argc, char** argv) {
    std::string configPath = hud::defaultConfigPath();
    std::string previewOut;
    std::string themeArg;
    std::string gpuOverride;
    bool        haveZ = false;
    int         zOverride = 0;
    bool        stdinFeed = false;
    bool        noXr      = false;
    bool        selfTest  = false;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto        need = [&](const char* n) -> const char* {
            if (i + 1 >= argc) { Log::log(Log::ERR, "{} requires a value", n); std::exit(kExitUsage); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { printUsage(argv[0]); return kExitOk; }
        else if (a == "--config")   configPath = need("--config");
        else if (a == "--preview")  previewOut = need("--preview");
        else if (a == "--theme")    themeArg = need("--theme");
        else if (a == "--self-test") selfTest = true;
        else if (a == "--gpu")      gpuOverride = need("--gpu");
        else if (a == "--z")        { zOverride = std::atoi(need("--z")); haveZ = true; }
        else if (a == "--stdin")    stdinFeed = true;
        else if (a == "--no-xr")    noXr = true;
        else if (a == "--verbose")  Log::setLevel(Log::DEBUG);
        else { Log::log(Log::ERR, "unknown option: {}", a); printUsage(argv[0]); return kExitUsage; }
    }

    // ---- self-test (WP-H7): private-bus health check, no XR, no real session bus ----
    if (selfTest)
        return hud::runSelfTest();

    // ---- config ----
    hud::SConfig             cfg;
    std::vector<std::string> errs, warns;
    if (!hud::loadConfigFile(configPath, cfg, errs, warns)) {
        for (const auto& e : errs)
            Log::log(Log::ERR, "config: {}", e);
        return kExitUsage;
    }
    for (const auto& w : warns)
        Log::log(Log::DEBUG, "config: {}", w);
    if (!gpuOverride.empty())
        cfg.gpu = gpuOverride;
    if (haveZ)
        cfg.hudZ = zOverride;

    // ---- offline preview (no XR) ----
    if (!previewOut.empty()) {
        hud::CScene scene(cfg.perClientCap);
        cfg.applySlots(scene.slots());
        int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
        hud::buildPreviewScene(scene, now);

        // Resolve the theming palette. --theme <dir> forces that theme's mako.ini; otherwise
        // follow the config (which by default follows the Omarchy current theme).
        const bool  follow   = themeArg.empty() ? cfg.themeFollow : true;
        std::string themeFile = themeArg.empty() ? cfg.themeFile : (themeArg + "/mako.ini");
        std::vector<std::string> pwarns;
        hud::SPalette palette = hud::resolvePalette(follow, themeFile, cfg.colorOverrides, pwarns);
        for (const auto& w : pwarns)
            Log::log(Log::DEBUG, "theme: {}", w);

        hud::SImage composite = hud::renderPreview(scene, 1600, 1000, cfg.texW, cfg.texH, palette);
        if (!hud::Png::write(composite, previewOut)) {
            Log::log(Log::ERR, "failed to write preview PNG: {}", previewOut);
            return kExitUsage;
        }
        Log::log(Log::INFO, "wrote six-slot preview: {} ({} panels{})", previewOut,
                 scene.panels().size(), themeArg.empty() ? "" : ", themed");
        return kExitOk;
    }

    // ---- signals: handler only sets a flag (SA_RESTART so it never EINTRs Monado's
    // blocking IPC inside xrEndFrame -> INSTANCE_LOST; hypxrvoice/hypxrpaper lesson).
    // poll() is interrupted regardless (SA_RESTART does not resume poll), so the loop wakes
    // promptly on SIGTERM. ----
    struct sigaction sa = {};
    sa.sa_handler       = onSignal;
    sa.sa_flags         = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    if (stdinFeed) {
        // Non-blocking stdin so the poll loop is never stalled waiting for content.
        int fl = fcntl(STDIN_FILENO, F_GETFL, 0);
        if (fl >= 0)
            fcntl(STDIN_FILENO, F_SETFL, fl | O_NONBLOCK);
    }

#ifndef HAVE_XR
    if (!noXr)
        Log::log(Log::WARN, "hypxrhud built without XR support (openxr/egl/gbm missing) — D-Bus only, runtime always absent");
    noXr = true;
#endif

    hud::CDaemon daemon(cfg, /*xrEnabled=*/!noXr, /*stdinFeed=*/stdinFeed);
    int rc = daemon.run();
    Log::log(Log::INFO, "hypxrhud exited ({})", rc);
    return rc;
}
