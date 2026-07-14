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

#include "Config.hpp"
#include "Scene.hpp"

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
// SEAMS for later WPs: the interim stdin NDJSON feed (readStdin) is what WP-H3 replaces
// with an sd-bus fd folded into the same poll loop; the run loop's runtime-loss handling
// currently exits (like the wp-v5 subprocess), which WP-H4 turns into probe/backoff so
// the daemon SURVIVES a WiVRn disconnect.

namespace hud {

class CEgl;

class CSession {
  public:
    CSession() = default;
    ~CSession();

    // Bring up instance/system/session/swapchain-format. Returns false when no XR
    // runtime is available (the caller degrades — main exits kExitNoRuntime).
    bool init(CEgl& egl, const SConfig& cfg);

    // Drive the frame loop until a clean exit (SIGTERM/SIGINT or session EXITING).
    // Reads the interim stdin NDJSON feed each tick and mirrors the scene to GPU.
    void run();

    void destroy();

    CScene&       scene() { return m_scene; }
    const CScene& scene() const { return m_scene; }
    int64_t       maxLayerCount() const { return m_maxLayers; }
    int           budget() const { return layerBudget(m_maxLayers); }

  private:
    // Per-panel GPU resources — the swapchain half of the panel table (memo §1.1).
    struct SGpuPanel {
        XrSwapchain           swapchain = XR_NULL_HANDLE;
        std::vector<uint32_t> texes;              // GLES texture ids of the swapchain images.
        uint64_t              uploadedEpoch = 0;  // last content epoch uploaded (0 = never).
    };

    bool createInstance();
    bool getSystem();
    bool createSession(CEgl& egl);
    bool chooseSwapchainFormat();

    SGpuPanel* ensureGpu(uint32_t id);            // create-on-demand swapchain for a panel.
    void       reconcileGpu();                    // destroy swapchains for vanished panels.
    void       uploadPanel(SGpuPanel& g, const uint8_t* rgba);

    void pollEvents();
    bool renderFrame(int64_t nowMs);
    bool readStdin();                             // drain stdin -> Wire -> m_scene; false on EOF.

    XrEnvironmentBlendMode blendMode() const;

    CEgl*   m_egl = nullptr;
    SConfig m_cfg;
    CScene  m_scene{4};

    XrInstance m_instance  = XR_NULL_HANDLE;
    XrSystemId m_systemId  = XR_NULL_SYSTEM_ID;
    XrSession  m_session   = XR_NULL_HANDLE;
    XrSpace    m_viewSpace = XR_NULL_HANDLE;
    XrSpace    m_localSpace = XR_NULL_HANDLE;

    int64_t m_swapchainFormat = 0;
    int64_t m_maxLayers       = 0;

    XrSessionState m_state          = XR_SESSION_STATE_UNKNOWN;
    bool           m_sessionRunning = false;
    bool           m_exit           = false;
    bool           m_exitRequested  = false;
    bool           m_haveColorScale = false;
    bool           m_stdinEof       = false;

    std::string                    m_runtimeName;
    std::string                    m_stdinBuf;
    std::map<uint32_t, SGpuPanel>  m_gpu;
};

} // namespace hud
