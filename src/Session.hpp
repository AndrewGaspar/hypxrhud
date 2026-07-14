#pragma once

// Platform macros must precede EGL/GLES, which must precede openxr_platform.h — same
// include contract as hypxrvoice's HudSession / hypxrpaper's Session / HypXRland's
// XRSession.
#define XR_USE_PLATFORM_EGL
#define XR_USE_GRAPHICS_API_OPENGL_ES

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl32.h>

#include <openxr/openxr.h>
#include <openxr/openxr_platform.h>

#include "BlendMode.hpp"
#include "Config.hpp"
#include "Scene.hpp"
#include "Theme.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// hypxrhud — the multi-panel OpenXR overlay session. Promoted from hypxrvoice's
// single-quad HUD subprocess (src/hud/HudSession.cpp @ 200a80e): one overlay session
// (XrSessionCreateInfoOverlayEXTX, configurable hud_z well above HypXRland's monitors),
// but now driving an N-panel scene — one XrCompositionLayerQuad per visible panel, with
// its own swapchain and a per-panel XR_KHR_composition_layer_color_scale_bias fade, one
// xrEndFrame with layerCount = N. Single-threaded, EGL context held current the whole
// session (Monado's GL-fence contract by construction, HypXRland commit 95c541a8).
//
// The scene (pure Scene) is the single source of truth; this class MIRRORS it into GPU
// swapchains, uploading a panel ONLY when its content epoch changes (the upload-once
// idle-monitor trick — a static panel re-submits its swapchain for free, no re-raster).
//
// WP-H4: the scene is owned by CDaemon (it SURVIVES a session teardown/rebuild); this
// class holds only a pointer and MIRRORS the scene into GPU swapchains. Runtime loss no
// longer exits the process — bringUp/teardown + the daemon's probe/backoff let the daemon
// survive a WiVRn disconnect. `bringUp` distinguishes "no runtime" (grow the backoff) from
// "runtime up, headset undonned" (gentle fixed cadence), matching the compositor (§6.1).

namespace hud {

class CEgl;

class CSession {
  public:
    CSession() = default;
    ~CSession();

    // Result of a bring-up attempt (drives the daemon's backoff cadence, memo §6.1).
    enum class EBringUp {
        Live,      // instance+system+session up; render away.
        NoHeadset, // runtime present but XR_ERROR_FORM_FACTOR_UNAVAILABLE (headset undonned).
        NoRuntime, // no runtime / missing extensions / session-create failure.
    };

    // Bring up instance/system/session/swapchain-format against `scene` (owned by the
    // caller). Cleans up any partial state on a non-Live result. Idempotent: safe to call
    // again after teardown() to reconnect. A fresh session starts with empty GPU state, so
    // every live panel re-rasters on reconnect (the force-dirty of memo §6.2, for free).
    EBringUp bringUp(CEgl& egl, const SConfig& cfg, CScene& scene);

    // Destroy the XR handles (instance/session/swapchains/spaces) but NOT the EGL context —
    // the daemon keeps EGL current across reconnects. Safe to call repeatedly.
    void teardown();

    // Pump XR events (session state machine). On any loss/exit event sets lost() so the
    // daemon tears down and re-enters probe/backoff instead of exiting.
    void pollEvents();

    // Submit one frame (one quad per visible panel). xrWaitFrame paces the caller. Returns
    // false if nothing was submitted (session not running / loss); sets lost() on loss.
    bool renderFrame(int64_t nowMs);

    // Ask the runtime to end the session cleanly (SIGTERM path). The subsequent EXITING
    // event sets lost(); the daemon distinguishes a requested exit from a surprise loss.
    void requestExit();

    // Set the active theming palette (WP-H6). The daemon calls this at bring-up and on a
    // live theme reload; the caller force-dirties the scene so every panel re-rasters in the
    // new colours on the next frame.
    void setPalette(const SPalette& pal) { m_palette = pal; }

    bool          lost() const { return m_lost; }
    bool          sessionRunning() const { return m_sessionRunning; }
    CScene&       scene() { return *m_scene; }
    const CScene& scene() const { return *m_scene; }
    const std::string& runtimeName() const { return m_runtimeName; }
    int64_t       maxLayerCount() const { return m_maxLayers; }
    int           budget() const { return layerBudget(m_maxLayers); }

  private:
    // Per-panel GPU resources — the swapchain half of the panel table (memo §1.1).
    struct SGpuPanel {
        XrSwapchain           swapchain = XR_NULL_HANDLE;
        std::vector<uint32_t> texes;              // GLES texture ids of the swapchain images.
        uint64_t              uploadedEpoch = 0;  // last content epoch uploaded (0 = never).
    };

    bool     createInstance();
    XrResult getSystem();                          // returns xrGetSystem's result (for NoHeadset).
    bool     createSession(CEgl& egl);
    bool     chooseSwapchainFormat();

    SGpuPanel* ensureGpu(uint32_t id);            // create-on-demand swapchain for a panel.
    void       reconcileGpu();                    // destroy swapchains for vanished panels.
    void       uploadPanel(SGpuPanel& g, const uint8_t* rgba);

    // Enumerate the runtime's advertised environment blend modes (needs instance+system,
    // no session) and select ours via the pure pickBlendMode against [hud] blend_mode.
    void selectBlendMode();

    CEgl*   m_egl = nullptr;
    SConfig m_cfg;
    CScene* m_scene = nullptr;                     // owned by CDaemon; survives teardown.

    XrInstance m_instance  = XR_NULL_HANDLE;
    XrSystemId m_systemId  = XR_NULL_SYSTEM_ID;
    XrSession  m_session   = XR_NULL_HANDLE;
    XrSpace    m_viewSpace = XR_NULL_HANDLE;
    XrSpace    m_localSpace = XR_NULL_HANDLE;

    int64_t m_swapchainFormat = 0;
    int64_t m_maxLayers       = 0;

    XrSessionState m_state          = XR_SESSION_STATE_UNKNOWN;
    bool           m_sessionRunning = false;
    bool           m_lost           = false;
    bool           m_haveColorScale = false;

    // BUG-1: the selected environment blend mode. Default OPAQUE, but for an overlay HUD over
    // passthrough "auto" picks ALPHA_BLEND when the runtime advertises it (WiVRn does) — an
    // OPAQUE overlay paints the whole view black over passthrough. Selected once in getSystem()
    // (single-threaded session; the frame thread only reads it). m_warnedBlend keeps the
    // explicit-unsupported fallback warning to once across reconnects.
    XrEnvironmentBlendMode m_blendMode   = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    bool                   m_warnedBlend = false;

    std::string                    m_runtimeName;
    std::map<uint32_t, SGpuPanel>  m_gpu;
    SPalette                       m_palette; // active theming palette (WP-H6).
};

} // namespace hud
