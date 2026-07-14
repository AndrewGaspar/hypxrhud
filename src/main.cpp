#include "Config.hpp"
#include "Log.hpp"
#include "PngWrite.hpp"
#include "Preview.hpp"
#include "Scene.hpp"

#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
#include <cstring>
#include <string>

// hypxrhud — the shared XR HUD daemon. Owns ONE OpenXR overlay session and renders an
// N-panel, slot-based head-locked HUD above HypXRland's monitors. XR utilities push
// panels to it (voice feedback, screenkey, toasts, media, battery) instead of each
// re-implementing session/EGL/swapchain/fade machinery. WP-H1+H2: the render core +
// multi-panel scene; the transport is an interim stdin NDJSON feed (WP-H3 replaces it
// with a D-Bus front end), and runtime-loss handling exits (WP-H4 makes it persistent).
//
// SAFETY: only ONE XR runtime per box. Do not start this alongside another live XR
// session by hand. `--preview <png>` renders the six-slot composite offline (no XR) and
// is the review path.

// The XR session lives behind HAVE_XR: when openxr/egl/gbm are absent at build time the
// daemon still builds and --preview works; running it just reports no runtime.
#ifdef HAVE_XR
#include "Egl.hpp"
#include "Session.hpp"
#endif

std::atomic<bool> g_stopRequested{false};
static void       onSignal(int) { g_stopRequested.store(true); }

static constexpr int kExitOk        = 0;
static constexpr int kExitUsage     = 2;
static constexpr int kExitNoRuntime = 3; // XR runtime unavailable — caller degrades.

static void printUsage(const char* a0) {
    std::fprintf(stderr,
                 "hypxrhud — shared XR HUD daemon for HypXRland\n"
                 "\nUsage: %s [options]   (reads panel NDJSON on stdin; see README)\n"
                 "  --config <path>   Config file (default $XDG_CONFIG_HOME/hypxrhud/hypxrhud.toml)\n"
                 "  --preview <png>   Render the six-slot composite to a PNG and exit (no XR)\n"
                 "  --gpu <path>      DRM render node (must match the runtime); overrides config\n"
                 "  --z <int>         Overlay sessionLayersPlacement (default 20); overrides config\n"
                 "  --verbose         Debug logging\n"
                 "  -h, --help\n",
                 a0);
}

int main(int argc, char** argv) {
    std::string configPath = hud::defaultConfigPath();
    std::string previewOut;
    std::string gpuOverride;
    bool        haveZ = false;
    int         zOverride = 0;

    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto        need = [&](const char* n) -> const char* {
            if (i + 1 >= argc) { Log::log(Log::ERR, "{} requires a value", n); std::exit(kExitUsage); }
            return argv[++i];
        };
        if (a == "-h" || a == "--help") { printUsage(argv[0]); return kExitOk; }
        else if (a == "--config")  configPath = need("--config");
        else if (a == "--preview") previewOut = need("--preview");
        else if (a == "--gpu")     gpuOverride = need("--gpu");
        else if (a == "--z")       { zOverride = std::atoi(need("--z")); haveZ = true; }
        else if (a == "--verbose") Log::setLevel(Log::DEBUG);
        else { Log::log(Log::ERR, "unknown option: {}", a); printUsage(argv[0]); return kExitUsage; }
    }

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
        hud::SImage composite = hud::renderPreview(scene, 1600, 1000, cfg.texW, cfg.texH);
        if (!hud::Png::write(composite, previewOut)) {
            Log::log(Log::ERR, "failed to write preview PNG: {}", previewOut);
            return kExitUsage;
        }
        Log::log(Log::INFO, "wrote six-slot preview: {} ({} panels)", previewOut, scene.panels().size());
        return kExitOk;
    }

    // ---- signals: handler only sets a flag (SA_RESTART so it never EINTRs Monado's
    // blocking IPC inside xrEndFrame -> INSTANCE_LOST; hypxrvoice/hypxrpaper lesson) ----
    struct sigaction sa = {};
    sa.sa_handler       = onSignal;
    sa.sa_flags         = SA_RESTART;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

#ifdef HAVE_XR
    hud::CEgl egl;
    if (!egl.init(cfg.gpu)) {
        Log::log(Log::ERR, "EGL init failed — no HUD (check --gpu / render node)");
        return kExitNoRuntime;
    }
    {
        hud::CSession session;
        if (!session.init(egl, cfg)) {
            Log::log(Log::ERR, "XR session init failed — no runtime; HUD unavailable");
            session.destroy();
            egl.destroy();
            return kExitNoRuntime;
        }
        Log::log(Log::INFO, "hypxrhud up (runtime present); reading panel feed on stdin");
        session.run();
        session.destroy();
    }
    egl.destroy();
    Log::log(Log::INFO, "hypxrhud exited");
    return kExitOk;
#else
    Log::log(Log::ERR, "hypxrhud built without XR support (openxr/egl/gbm missing); only --preview works");
    return kExitNoRuntime;
#endif
}
