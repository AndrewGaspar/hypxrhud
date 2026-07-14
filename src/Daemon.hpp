#pragma once

#include "Config.hpp"
#include "Dbus.hpp"
#include "Scene.hpp"

#include <cstdint>
#include <string>

#ifdef HAVE_XR
#include "Egl.hpp"
#include "Session.hpp"
#endif

// hypxrhud — the persistent daemon: the single-threaded event loop that owns the
// authoritative CScene and the D-Bus front end, and (WP-H4) manages the XR session
// lifecycle with a re-probe backoff. This is what replaces the wp-v5 subprocess's
// disposable "exit on any loss" model: the daemon SURVIVES a runtime that is absent at
// startup or lost at runtime — it keeps serving D-Bus (panels are accepted into the pure
// scene), probes xrCreateInstance on a growing backoff, and on (re)connect rebuilds GPU
// state so every live panel re-uploads.
//
// The loop folds the sd-bus fd (+ optional --stdin debug feed) and the reprobe timer into
// ONE poll(): no threads, so the EGL context stays current the whole session (Monado's
// fence contract). When a session is live, xrWaitFrame paces the loop and the bus fd is
// drained each frame (NO_REPLY_EXPECTED UpdatePanel = at most one-frame latency). When no
// session is live, the loop blocks in poll() until the bus fd or the next probe is due.

namespace hud {

class CDaemon {
  public:
    // xrEnabled=false forces permanent runtime-absent mode (the --no-xr flag / a build with
    // no OpenXR): the daemon owns the bus and serves the scene but never probes a runtime —
    // the deterministic mode the D-Bus integration tests run in.
    CDaemon(const SConfig& cfg, bool xrEnabled, bool stdinFeed);

    // Own the bus name, then run the poll loop until SIGTERM (g_stopRequested). Returns the
    // process exit code. If the bus name can't be acquired, returns nonzero without looping.
    int run();

  private:
    enum class ERuntime { Absent, Connecting, Live };

    const char* stateName(ERuntime s) const;
    void        setState(ERuntime s);
    int         computeTimeoutMs() const; // poll() timeout for this tick.
    void        reapAndEmit(int64_t nowMs);
    void        drainStdin(int64_t nowMs);

#ifdef HAVE_XR
    void tryBringUp(int64_t nowMs);   // one probe attempt; schedules the next on failure.
    void onSessionLost(int64_t nowMs); // teardown + re-enter Connecting (keep the panels).
#endif

    SConfig  m_cfg;
    bool     m_xrEnabled = false;
    bool     m_stdinFeed = false;

    CScene m_scene;
    CBus   m_bus;

    ERuntime m_state = ERuntime::Absent;

    // Re-probe schedule (WP-H4). m_nextProbeAt is a steady-clock ms deadline; m_attempt is
    // the 0-based consecutive-failure count feeding reprobeBackoffMs.
    int64_t m_nextProbeAt = 0;
    int     m_attempt     = 0;

    // Clean-shutdown bookkeeping.
    bool    m_stop      = false;
    bool    m_exitAsked = false;
    int64_t m_exitAskedAt = 0;

    // --stdin debug feed state.
    std::string m_stdinBuf;
    bool        m_stdinEof = false;

#ifdef HAVE_XR
    CEgl     m_egl;
    bool     m_eglOk = false;
    CSession m_session;
#endif
};

} // namespace hud
