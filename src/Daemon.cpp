#include "Daemon.hpp"

#include "Backoff.hpp"
#include "Log.hpp"
#include "Wire.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

extern std::atomic<bool> g_stopRequested;

namespace hud {

namespace {
    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }
    // Cap on how long we wait for the runtime to deliver EXITING after we asked for a clean
    // session exit — beyond this the daemon exits anyway (a dead runtime never replies).
    constexpr int64_t kExitGraceMs = 2000;
    // Idle poll cap when nothing else schedules a wake, so reapExpired still animates fades.
    constexpr int64_t kIdlePollMs = 1000;
}

CDaemon::CDaemon(const SConfig& cfg, bool xrEnabled, bool stdinFeed)
    : m_cfg(cfg), m_xrEnabled(xrEnabled), m_stdinFeed(stdinFeed), m_scene(cfg.perClientCap) {
    m_cfg.applySlots(m_scene.slots());
}

const char* CDaemon::stateName(ERuntime s) const {
    switch (s) {
        case ERuntime::Live:       return "live";
        case ERuntime::Connecting: return "connecting";
        default:                   return "absent";
    }
}

void CDaemon::setState(ERuntime s) {
    if (m_state == s)
        return;
    m_state = s;
    m_bus.setRuntimeState(stateName(s));
}

int CDaemon::computeTimeoutMs() const {
#ifdef HAVE_XR
    if (m_state == ERuntime::Live && m_session.sessionRunning() && !m_stop)
        return 0; // xrWaitFrame paces us; just drain the bus non-blocking and render.
    if (m_state == ERuntime::Live)
        return 16; // live but not yet running (waiting for READY) — poll gently for events.
    if (m_xrEnabled) {
        int64_t until = m_nextProbeAt - nowMs();
        return (int)std::clamp<int64_t>(until, 0, kIdlePollMs);
    }
#endif
    return (int)kIdlePollMs; // absent / no-XR: block on the bus fd, wake to reap fades.
}

void CDaemon::reapAndEmit(int64_t now) {
    std::vector<SDismissal> expired;
    m_scene.reapExpired(now, &expired);
    m_bus.emitDismissed(expired);
}

void CDaemon::drainStdin(int64_t now) {
    if (!m_stdinFeed || m_stdinEof)
        return;
    char    buf[4096];
    ssize_t r;
    while ((r = read(STDIN_FILENO, buf, sizeof(buf))) > 0)
        m_stdinBuf.append(buf, (size_t)r);
    if (r == 0)
        m_stdinEof = true;
    size_t pos;
    while ((pos = m_stdinBuf.find('\n')) != std::string::npos) {
        std::string line = m_stdinBuf.substr(0, pos);
        m_stdinBuf.erase(0, pos + 1);
        SWireMsg msg;
        if (!Wire::parse(line, msg))
            continue;
        std::vector<SDismissal> d;
        if (msg.action == SWireMsg::EAction::Dismiss) {
            if (m_scene.dismiss(msg.dismissId, "client", now))
                m_bus.emitDismissed({{msg.dismissId, "client"}});
        } else {
            m_scene.upsert(msg.upsert, now, &d);
            m_bus.emitDismissed(d);
        }
    }
}

void CDaemon::reloadPalette() {
    std::vector<std::string> warns;
    m_palette = resolvePalette(m_cfg.themeFollow, m_cfg.themeFile, m_cfg.colorOverrides, warns);
    for (const auto& w : warns)
        Log::log(Log::DEBUG, "[theme] {}", w);
#ifdef HAVE_XR
    m_session.setPalette(m_palette);
#endif
    // Force every panel to re-raster so the new colours take effect without a content change.
    m_scene.forceRedrawAll();
    Log::log(Log::INFO, "[theme] palette reloaded ({} panel(s) re-rastering)", m_scene.panels().size());
}

#ifdef HAVE_XR
void CDaemon::tryBringUp(int64_t now) {
    CSession::EBringUp res = m_session.bringUp(m_egl, m_cfg, m_scene);
    if (res == CSession::EBringUp::Live) {
        m_attempt = 0;
        m_session.setPalette(m_palette); // a fresh session starts on the default palette.
        m_bus.setRuntimeInfo(m_session.runtimeName(), m_session.maxLayerCount(), m_session.budget());
        setState(ERuntime::Live); // on reconnect the fresh session re-rasters every panel (memo §6.2).
        Log::log(Log::INFO, "[daemon] XR session live (runtime: {}, budget {}) — {} panel(s) pending",
                 m_session.runtimeName().empty() ? "?" : m_session.runtimeName(), m_session.budget(),
                 m_scene.panels().size());
        return;
    }

    // Not live — schedule the next probe. HEADSET wait (runtime up, undonned) uses a gentle
    // FIXED cadence; RUNTIME wait grows the backoff so a permanently-absent runtime is cheap.
    if (res == CSession::EBringUp::NoHeadset) {
        m_attempt     = 0;
        m_nextProbeAt = now + std::max<int64_t>(250, m_cfg.reprobeBaseMs);
    } else {
        int64_t d     = reprobeBackoffMs(m_attempt, m_cfg.reprobeBaseMs, m_cfg.reprobeCapMs);
        m_nextProbeAt = now + d;
        m_attempt++;
    }
    setState(ERuntime::Connecting);
}

void CDaemon::onSessionLost(int64_t now) {
    Log::log(Log::WARN, "[daemon] XR session lost — tearing down; re-probing (panels kept)");
    m_session.teardown();
    m_bus.setRuntimeInfo("", 0, 0);
    m_attempt     = 0;                                  // a dropped live session: retry promptly.
    m_nextProbeAt = now + std::max<int64_t>(250, m_cfg.reprobeBaseMs);
    setState(ERuntime::Connecting);
}
#endif

int CDaemon::run() {
    if (!m_bus.init(m_scene, m_cfg)) {
        Log::log(Log::ERR, "[daemon] could not own the D-Bus name — exiting");
        return 4;
    }

#ifdef HAVE_XR
    if (m_xrEnabled) {
        m_eglOk = m_egl.init(m_cfg.gpu);
        if (!m_eglOk) {
            Log::log(Log::ERR, "[daemon] EGL init failed (check --gpu/render node) — serving D-Bus only");
            m_xrEnabled = false;
        }
    }
#endif

    // Resolve the theming palette (WP-H6) once up-front so the first session bring-up renders
    // in the right colours. Arm the Omarchy theme watch only when we can actually render.
    {
        std::vector<std::string> warns;
        m_palette = resolvePalette(m_cfg.themeFollow, m_cfg.themeFile, m_cfg.colorOverrides, warns);
        for (const auto& w : warns)
            Log::log(Log::DEBUG, "[theme] {}", w);
    }
    if (m_xrEnabled && m_cfg.themeFollow)
        m_themeWatchOk = m_themeWatch.init();

    // Initial state: connecting if we'll probe a runtime, else permanently absent.
    m_state = m_xrEnabled ? ERuntime::Connecting : ERuntime::Absent;
    m_bus.setRuntimeState(stateName(m_state));
    m_nextProbeAt = nowMs(); // probe immediately on the first tick.
    Log::log(Log::INFO, "[daemon] up — {} ({} feed)", stateName(m_state),
             m_stdinFeed ? "D-Bus + stdin" : "D-Bus");

    m_bus.process(); // flush the name-acquired / initial signals.

    while (true) {
        const int timeout = computeTimeoutMs();

        // Fold the bus fd (+ optional stdin + the theme-watch inotify fd) into one poll().
        // sd-bus's own timer bounds the wait so its timeouts fire on schedule.
        struct pollfd fds[3];
        int           nfds  = 0;
        const int     busFd = m_bus.fd();
        if (busFd >= 0) {
            fds[nfds].fd      = busFd;
            fds[nfds].events  = (short)m_bus.events();
            fds[nfds].revents = 0;
            nfds++;
        }
        if (m_stdinFeed && !m_stdinEof) {
            fds[nfds].fd      = STDIN_FILENO;
            fds[nfds].events  = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        const int themeFd = m_themeWatchOk ? m_themeWatch.fd() : -1;
        if (themeFd >= 0) {
            fds[nfds].fd      = themeFd;
            fds[nfds].events  = POLLIN;
            fds[nfds].revents = 0;
            nfds++;
        }
        int busTimeout = m_bus.timeoutMs();
        int pollTo     = timeout;
        if (busTimeout >= 0)
            pollTo = (pollTo < 0) ? busTimeout : std::min(pollTo, busTimeout);
        poll(fds, nfds, pollTo);

        m_bus.process(); // drain method calls -> mutate the scene / emit signals.

        // Omarchy theme switched? Re-resolve the palette + re-raster every panel (WP-H6).
        if (m_themeWatchOk && m_themeWatch.drain())
            reloadPalette();

        const int64_t now = nowMs();
        drainStdin(now);
        reapAndEmit(now);

        // ---- clean-shutdown latch ----
        if (g_stopRequested.load())
            m_stop = true;
        if (m_stop && !m_exitAsked) {
#ifdef HAVE_XR
            if (m_state == ERuntime::Live && m_session.sessionRunning()) {
                m_session.requestExit(); // ask the runtime to end the session cleanly.
                m_exitAsked   = true;
                m_exitAskedAt = now;
            } else
#endif
            {
                break; // nothing live to wind down — exit now.
            }
        }

#ifdef HAVE_XR
        if (m_state == ERuntime::Live) {
            m_session.pollEvents();
            if (m_session.lost()) {
                if (m_stop)
                    break; // our requested clean exit completed.
                onSessionLost(now);
                continue;
            }
            if (m_session.sessionRunning() && !m_stop)
                m_session.renderFrame(now);
            // While stopping, keep pumping events (above) until EXITING; bail if it never comes.
            if (m_stop && m_exitAsked && now - m_exitAskedAt > kExitGraceMs)
                break;
        } else if (m_xrEnabled && !m_stop) {
            if (now >= m_nextProbeAt)
                tryBringUp(now);
        }
#endif
    }

    Log::log(Log::INFO, "[daemon] shutting down");
    m_themeWatch.shutdown();
#ifdef HAVE_XR
    m_session.teardown();
    if (m_eglOk)
        m_egl.destroy();
#endif
    m_bus.shutdown();
    return 0;
}

} // namespace hud
