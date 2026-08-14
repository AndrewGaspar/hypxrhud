#pragma once

#include "Config.hpp"
#include "Presentation.hpp"
#include "Scene.hpp"

#include <cstdint>
#include <string>
#include <vector>

// hypxrhud — the D-Bus front end (WP-H3). Owns the well-known name
// `io.github.andrewgaspar.hypxrhud` on the session bus and exposes the declarative panel
// API (design memo §2). It drives the SAME pure CScene the interim NDJSON did — the
// transport changes, not the scene arbiter.
//
// Single-threaded by construction: the sd-bus fd is folded into CDaemon's poll() loop
// (fd()/timeoutMs()/process()), never its own thread, so the EGL context stays current
// (Monado's fence contract). `UpdatePanel` is designed for NO_REPLY_EXPECTED
// fire-and-forget — the daemon drains it from the bus fd on the next tick.
//
// Ownership/lifetime: every panel records its creator's UNIQUE bus name (§2.4); a match on
// org.freedesktop.DBus NameOwnerChanged auto-dismisses a client's panels when it drops
// (reason "client-gone"). The per-client cap (SConfig::perClientCap, default 4) is enforced
// in CScene::upsert; a rejected create returns a clear D-Bus error.
//
// The vtable reads the on-wire `a{sv}` into a typed SPropMap and hands it to the PURE
// upsertFromProps (Props.cpp) — the only sd-bus-specific code is the variant reading here.

struct sd_bus;
struct sd_bus_slot;
struct sd_bus_message;
struct sd_bus_error;

namespace hud {

// Well-known name / object path / interface (locked at triage: bus name is
// io.github.andrewgaspar.hypxrhud). Shared so the integration-test client reuses them.
inline constexpr const char* kBusName = "io.github.andrewgaspar.hypxrhud";
inline constexpr const char* kObjPath = "/io/github/andrewgaspar/hypxrhud";
inline constexpr const char* kIface   = "io.github.andrewgaspar.hypxrhud1"; // versioned suffix.

class CBus {
  public:
    CBus() = default;
    ~CBus();

    // Open the session bus, request the well-known name, install the vtable + the
    // NameOwnerChanged match. `scene` and `cfg` are borrowed for the process lifetime.
    // Returns false if the bus is unavailable or the name is already owned.
    bool init(CScene& scene, const SConfig& cfg);

    // Release the name, flush pending traffic, unref the bus. Idempotent.
    void shutdown();

    bool ok() const { return m_bus != nullptr && m_haveName; }

    // ---- poll()-loop integration (no threads) ----
    int  fd() const;         // sd_bus_get_fd, or -1.
    int  events() const;     // POLLIN/POLLOUT mask sd-bus currently wants.
    int  timeoutMs() const;  // sd-bus's own timer, in ms; -1 = infinite (block on fd).
    void process();          // drain sd_bus_process fully (handles queued in + out).

    // ---- daemon -> bus state (surfaced as a property + a signal) ----
    // state ∈ {"absent","connecting","live"}. Emits RuntimeStateChanged + a
    // PropertiesChanged for RuntimeState only when it actually changes (not spammy).
    void setRuntimeState(const std::string& state);
    void setRuntimeInfo(const std::string& runtimeName, int64_t maxLayers, int budget);
    void setRendering(bool rendering);
    void markPresented(const std::vector<uint32_t>& panelIds);

    // Emit PanelDismissed(id, reason) for a batch (reapExpired / bring-up preemptions the
    // daemon collects outside a method call).
    void emitDismissed(const std::vector<SDismissal>& dismissed);

    const std::string& runtimeState() const { return m_state; }

  private:
    // vtable trampolines (userdata = this).
    static int onCreatePanel(sd_bus_message*, void*, sd_bus_error*);
    static int onUpdatePanel(sd_bus_message*, void*, sd_bus_error*);
    static int onDismissPanel(sd_bus_message*, void*, sd_bus_error*);
    static int onGetCapabilities(sd_bus_message*, void*, sd_bus_error*);
    static int onGetPanelPresentation(sd_bus_message*, void*, sd_bus_error*);
    static int propRuntimeState(sd_bus*, const char*, const char*, const char*, sd_bus_message*, void*, sd_bus_error*);
    static int propPanelCount(sd_bus*, const char*, const char*, const char*, sd_bus_message*, void*, sd_bus_error*);
    static int propRuntimeName(sd_bus*, const char*, const char*, const char*, sd_bus_message*, void*, sd_bus_error*);
    static int propRendering(sd_bus*, const char*, const char*, const char*, sd_bus_message*, void*, sd_bus_error*);
    static int propMaxPanels(sd_bus*, const char*, const char*, const char*, sd_bus_message*, void*, sd_bus_error*);
    static int onNameOwnerChanged(sd_bus_message*, void*, sd_bus_error*);

    void emitOne(uint32_t id, const std::string& reason);

    sd_bus*      m_bus     = nullptr;
    sd_bus_slot* m_vtable  = nullptr;
    sd_bus_slot* m_nocSlot = nullptr;
    bool         m_haveName = false;

    CScene*        m_scene = nullptr;
    const SConfig* m_cfg   = nullptr;

    std::string m_state       = "absent";
    std::string m_runtimeName;
    bool        m_rendering   = false;
    int64_t     m_maxLayers   = 0;
    int         m_budget      = 0;
    CPresentationTracker m_presentations;
};

} // namespace hud
