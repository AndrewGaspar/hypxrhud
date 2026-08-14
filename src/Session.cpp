#include "Session.hpp"

#include "Egl.hpp"
#include "Log.hpp"
#include "PanelText.hpp"

#include <cstring>

namespace hud {

#define XR_CHK(expr)                                                                    \
    do {                                                                                \
        XrResult _r = (expr);                                                           \
        if (XR_FAILED(_r)) {                                                            \
            Log::log(Log::ERR, "[xr] " #expr " failed: {}", (int)_r);                  \
            return false;                                                               \
        }                                                                               \
    } while (0)

namespace {
    constexpr int64_t kSRGBA = 0x8C43; // GL_SRGB8_ALPHA8
    constexpr int64_t kRGBA8 = 0x8058; // GL_RGBA8

    EBlendMode blendFromXr(XrEnvironmentBlendMode m) {
        switch (m) {
            case XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND: return EBlendMode::Alpha;
            case XR_ENVIRONMENT_BLEND_MODE_ADDITIVE:    return EBlendMode::Additive;
            default:                                    return EBlendMode::Opaque;
        }
    }
    XrEnvironmentBlendMode blendToXr(EBlendMode m) {
        switch (m) {
            case EBlendMode::Alpha:    return XR_ENVIRONMENT_BLEND_MODE_ALPHA_BLEND;
            case EBlendMode::Additive: return XR_ENVIRONMENT_BLEND_MODE_ADDITIVE;
            case EBlendMode::Opaque:   return XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
        }
        return XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
    }
}

CSession::~CSession() {
    teardown();
}

bool CSession::createInstance() {
    uint32_t extCount = 0;
    xrEnumerateInstanceExtensionProperties(nullptr, 0, &extCount, nullptr);
    std::vector<XrExtensionProperties> extProps(extCount, {XR_TYPE_EXTENSION_PROPERTIES});
    if (extCount)
        xrEnumerateInstanceExtensionProperties(nullptr, extCount, &extCount, extProps.data());
    auto hasExt = [&](const char* n) {
        for (auto& e : extProps)
            if (std::strcmp(e.extensionName, n) == 0)
                return true;
        return false;
    };

    const char* required[] = {"XR_MNDX_egl_enable", "XR_KHR_opengl_es_enable", XR_EXTX_OVERLAY_EXTENSION_NAME};
    std::vector<const char*> exts;
    for (auto* r : required) {
        if (!hasExt(r)) {
            Log::log(Log::ERR, "[xr] required extension '{}' unavailable", r);
            return false;
        }
        exts.push_back(r);
    }
    m_haveColorScale = hasExt(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);
    if (m_haveColorScale)
        exts.push_back(XR_KHR_COMPOSITION_LAYER_COLOR_SCALE_BIAS_EXTENSION_NAME);

    XrApplicationInfo appInfo = {};
    std::strncpy(appInfo.applicationName, "hypxrhud", XR_MAX_APPLICATION_NAME_SIZE - 1);
    appInfo.applicationVersion = 1;
    std::strncpy(appInfo.engineName, "hypxrhud", XR_MAX_ENGINE_NAME_SIZE - 1);
    appInfo.engineVersion = 1;
    appInfo.apiVersion    = XR_API_VERSION_1_0;

    XrInstanceCreateInfo info  = {XR_TYPE_INSTANCE_CREATE_INFO};
    info.applicationInfo       = appInfo;
    info.enabledExtensionCount = (uint32_t)exts.size();
    info.enabledExtensionNames = exts.data();
    XR_CHK(xrCreateInstance(&info, &m_instance));

    XrInstanceProperties props = {XR_TYPE_INSTANCE_PROPERTIES};
    if (XR_SUCCEEDED(xrGetInstanceProperties(m_instance, &props)))
        m_runtimeName = props.runtimeName;
    Log::log(Log::INFO, "[xr] instance created (runtime: {}, color_scale_bias: {})",
             m_runtimeName.empty() ? "?" : m_runtimeName, m_haveColorScale);
    return true;
}

XrResult CSession::getSystem() {
    XrSystemGetInfo si = {XR_TYPE_SYSTEM_GET_INFO};
    si.formFactor      = XR_FORM_FACTOR_HEAD_MOUNTED_DISPLAY;
    XrResult r         = xrGetSystem(m_instance, &si, &m_systemId);
    if (XR_FAILED(r)) {
        // XR_ERROR_FORM_FACTOR_UNAVAILABLE = runtime up, headset undonned (memo §6.1) —
        // the caller polls at the gentle fixed cadence rather than growing the backoff.
        Log::log(Log::DEBUG, "[xr] xrGetSystem failed: {}", (int)r);
        return r;
    }

    // Layer budget (design memo §1.1): query maxLayerCount and log the effective cap.
    XrSystemProperties props = {XR_TYPE_SYSTEM_PROPERTIES};
    if (XR_SUCCEEDED(xrGetSystemProperties(m_instance, m_systemId, &props))) {
        m_maxLayers = props.graphicsProperties.maxLayerCount;
        Log::log(Log::INFO, "[xr] runtime maxLayerCount={} -> panel budget {}", m_maxLayers, budget());
    } else {
        m_maxLayers = 0;
        Log::log(Log::WARN, "[xr] xrGetSystemProperties failed; assuming spec-minimum layer budget {}", budget());
    }
    selectBlendMode();
    return XR_SUCCESS;
}

void CSession::selectBlendMode() {
    // Enumerate the runtime's advertised blend modes for primary-stereo (preferred-first);
    // needs only instance+system. The pure pickBlendMode selects ours per [hud] blend_mode.
    std::vector<EBlendMode> modes;
    uint32_t                n  = 0;
    XrResult                r  = xrEnumerateEnvironmentBlendModes(
        m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 0, &n, nullptr);
    if (XR_SUCCEEDED(r) && n) {
        std::vector<XrEnvironmentBlendMode> xm(n);
        if (XR_SUCCEEDED(xrEnumerateEnvironmentBlendModes(
                m_instance, m_systemId, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, n, &n, xm.data())))
            for (auto m : xm)
                modes.push_back(blendFromXr(m));
    } else {
        Log::log(Log::WARN, "[xr] xrEnumerateEnvironmentBlendModes failed/empty ({}); assuming OPAQUE only", (int)r);
    }

    SBlendPick pick = pickBlendMode(modes, m_cfg.blendMode);
    if (pick.requestedUnsupported && !m_warnedBlend) {
        Log::log(Log::WARN, "[xr] blend_mode '{}' not advertised by runtime; falling back to '{}'",
                 m_cfg.blendMode, blendModeName(pick.mode));
        m_warnedBlend = true;
    }
    m_blendMode = blendToXr(pick.mode);
    std::string list;
    for (auto m : modes)
        list += (list.empty() ? "" : ", ") + std::string(blendModeName(m));
    Log::log(Log::INFO, "[xr] environment blend mode: {} (config '{}', runtime advertises: {})",
             blendModeName(pick.mode), m_cfg.blendMode, list.empty() ? "?" : list);
}

bool CSession::createSession(CEgl& egl) {
    PFN_xrGetOpenGLESGraphicsRequirementsKHR pfn = nullptr;
    XR_CHK(xrGetInstanceProcAddr(m_instance, "xrGetOpenGLESGraphicsRequirementsKHR",
                                 reinterpret_cast<PFN_xrVoidFunction*>(&pfn)));
    XrGraphicsRequirementsOpenGLESKHR reqs = {XR_TYPE_GRAPHICS_REQUIREMENTS_OPENGL_ES_KHR};
    XR_CHK(pfn(m_instance, m_systemId, &reqs));

    XrGraphicsBindingEGLMNDX binding = {XR_TYPE_GRAPHICS_BINDING_EGL_MNDX};
    binding.getProcAddress           = (PFNEGLGETPROCADDRESSPROC)eglGetProcAddress;
    binding.display                  = egl.m_display;
    binding.config                   = egl.m_config;
    binding.context                  = egl.m_context;

    // Overlay create-info between session info and EGL binding: our quads composite
    // above HypXRland's monitors (higher sessionLayersPlacement = top; hud_z default 20).
    XrSessionCreateInfoOverlayEXTX overlay = {XR_TYPE_SESSION_CREATE_INFO_OVERLAY_EXTX};
    overlay.createFlags            = 0;
    overlay.sessionLayersPlacement = (uint32_t)m_cfg.hudZ;
    overlay.next                   = &binding;

    XrSessionCreateInfo si = {XR_TYPE_SESSION_CREATE_INFO};
    si.systemId            = m_systemId;
    si.next                = &overlay;

    XR_CHK(xrCreateSession(m_instance, &si, &m_session));
    Log::log(Log::INFO, "[xr] overlay session created (placement {})", m_cfg.hudZ);

    // VIEW (head-locked, native/free) + LOCAL (world-fixed) reference spaces.
    XrReferenceSpaceCreateInfo vi = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    vi.poseInReferenceSpace       = {{0, 0, 0, 1}, {0, 0, 0}};
    vi.referenceSpaceType         = XR_REFERENCE_SPACE_TYPE_VIEW;
    XR_CHK(xrCreateReferenceSpace(m_session, &vi, &m_viewSpace));
    XrReferenceSpaceCreateInfo li = {XR_TYPE_REFERENCE_SPACE_CREATE_INFO};
    li.poseInReferenceSpace       = {{0, 0, 0, 1}, {0, 0, 0}};
    li.referenceSpaceType         = XR_REFERENCE_SPACE_TYPE_LOCAL;
    if (XR_FAILED(xrCreateReferenceSpace(m_session, &li, &m_localSpace)))
        m_localSpace = XR_NULL_HANDLE; // LOCAL panels fall back to VIEW if unsupported.
    return true;
}

bool CSession::chooseSwapchainFormat() {
    uint32_t n = 0;
    xrEnumerateSwapchainFormats(m_session, 0, &n, nullptr);
    std::vector<int64_t> formats(n);
    if (n)
        xrEnumerateSwapchainFormats(m_session, n, &n, formats.data());
    m_swapchainFormat = kSRGBA;
    if (!formats.empty()) {
        int64_t chosen = formats[0];
        for (auto f : formats)
            if (f == kSRGBA) { chosen = f; break; }
        if (chosen != kSRGBA)
            for (auto f : formats)
                if (f == kRGBA8) { chosen = f; break; }
        m_swapchainFormat = chosen;
    }
    return true;
}

CSession::SGpuPanel* CSession::ensureGpu(uint32_t id) {
    auto it = m_gpu.find(id);
    if (it != m_gpu.end())
        return &it->second;

    XrSwapchainCreateInfo ci = {XR_TYPE_SWAPCHAIN_CREATE_INFO};
    ci.usageFlags            = XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
    ci.format                = m_swapchainFormat;
    ci.sampleCount           = 1;
    ci.width                 = (uint32_t)m_cfg.texW;
    ci.height                = (uint32_t)m_cfg.texH;
    ci.faceCount             = 1;
    ci.arraySize             = 1;
    ci.mipCount              = 1;

    SGpuPanel g;
    if (XR_FAILED(xrCreateSwapchain(m_session, &ci, &g.swapchain))) {
        Log::log(Log::ERR, "[xr] xrCreateSwapchain failed for panel {}", id);
        return nullptr;
    }
    uint32_t c = 0;
    xrEnumerateSwapchainImages(g.swapchain, 0, &c, nullptr);
    std::vector<XrSwapchainImageOpenGLESKHR> imgs(c, {XR_TYPE_SWAPCHAIN_IMAGE_OPENGL_ES_KHR});
    xrEnumerateSwapchainImages(g.swapchain, c, &c,
                               reinterpret_cast<XrSwapchainImageBaseHeader*>(imgs.data()));
    for (auto& im : imgs)
        g.texes.push_back(im.image);

    auto [ins, ok] = m_gpu.emplace(id, std::move(g));
    return &ins->second;
}

void CSession::reconcileGpu() {
    for (auto it = m_gpu.begin(); it != m_gpu.end();) {
        if (!m_scene->get(it->first)) {
            if (it->second.swapchain != XR_NULL_HANDLE)
                xrDestroySwapchain(it->second.swapchain);
            it = m_gpu.erase(it);
        } else {
            ++it;
        }
    }
}

void CSession::uploadPanel(SGpuPanel& g, const uint8_t* rgba) {
    uint32_t                    idx = 0;
    XrSwapchainImageAcquireInfo acq = {XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
    if (XR_FAILED(xrAcquireSwapchainImage(g.swapchain, &acq, &idx)))
        return;
    XrSwapchainImageWaitInfo wait = {XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
    wait.timeout                  = XR_INFINITE_DURATION;
    if (XR_FAILED(xrWaitSwapchainImage(g.swapchain, &wait)))
        return;

    if (idx < g.texes.size()) {
        // GL texture origin is bottom-left; our raster is top-row-first, so flip rows.
        std::vector<uint8_t> flip((size_t)m_cfg.texW * m_cfg.texH * 4);
        const size_t         row = (size_t)m_cfg.texW * 4;
        for (int y = 0; y < m_cfg.texH; y++)
            std::memcpy(&flip[(size_t)(m_cfg.texH - 1 - y) * row], &rgba[(size_t)y * row], row);

        glBindTexture(GL_TEXTURE_2D, g.texes[idx]);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, m_cfg.texW, m_cfg.texH, GL_RGBA, GL_UNSIGNED_BYTE, flip.data());
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFinish();
        glBindTexture(GL_TEXTURE_2D, 0);
    }
    XrSwapchainImageReleaseInfo rel = {XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
    xrReleaseSwapchainImage(g.swapchain, &rel);
}

void CSession::pollEvents() {
    XrEventDataBuffer ev = {XR_TYPE_EVENT_DATA_BUFFER};
    XrResult          r;
    while (m_instance != XR_NULL_HANDLE && (r = xrPollEvent(m_instance, &ev)) == XR_SUCCESS) {
        if (ev.type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED) {
            auto* e = reinterpret_cast<XrEventDataSessionStateChanged*>(&ev);
            m_state = e->state;
            switch (m_state) {
                case XR_SESSION_STATE_READY: {
                    XrSessionBeginInfo bi           = {XR_TYPE_SESSION_BEGIN_INFO};
                    bi.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
                    if (XR_SUCCEEDED(xrBeginSession(m_session, &bi))) {
                        m_sessionRunning = true;
                        Log::log(Log::INFO, "[xr] session begun");
                    }
                    break;
                }
                case XR_SESSION_STATE_STOPPING:
                    xrEndSession(m_session);
                    m_sessionRunning = false;
                    break;
                case XR_SESSION_STATE_EXITING:
                case XR_SESSION_STATE_LOSS_PENDING:
                    // WP-H4: NOT a process exit — the daemon tears down + re-enters
                    // probe/backoff, keeping the panel table. A requested clean shutdown
                    // (SIGTERM -> requestExit) also lands here; the daemon distinguishes.
                    m_lost = true;
                    break;
                default: break;
            }
        } else if (ev.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING) {
            m_lost = true;
        }
        ev = {XR_TYPE_EVENT_DATA_BUFFER};
    }
    if (r == XR_ERROR_INSTANCE_LOST || r == XR_ERROR_SESSION_LOST)
        m_lost = true;
}

bool CSession::renderFrame(int64_t now, std::vector<uint32_t>* submittedIds) {
    if (submittedIds)
        submittedIds->clear();
    if (!m_sessionRunning || m_session == XR_NULL_HANDLE)
        return false;

    XrFrameWaitInfo wi = {XR_TYPE_FRAME_WAIT_INFO};
    XrFrameState    fs = {XR_TYPE_FRAME_STATE};
    XrResult        rw = xrWaitFrame(m_session, &wi, &fs);
    if (rw == XR_ERROR_SESSION_LOST || rw == XR_ERROR_INSTANCE_LOST) { m_lost = true; return false; }
    if (XR_FAILED(rw)) return false;

    XrFrameBeginInfo bi = {XR_TYPE_FRAME_BEGIN_INFO};
    XrResult         rb = xrBeginFrame(m_session, &bi);
    if (rb == XR_ERROR_SESSION_LOST || rb == XR_ERROR_INSTANCE_LOST) { m_lost = true; return false; }

    reconcileGpu();

    // Stable storage for the layer structs referenced by xrEndFrame.
    const int                                        cap = budget();
    std::vector<XrCompositionLayerQuad>              quads;
    std::vector<XrCompositionLayerColorScaleBiasKHR> csbs;
    std::vector<const XrCompositionLayerBaseHeader*> layers;
    std::vector<uint32_t>                            layerIds;
    quads.reserve(cap);
    csbs.reserve(cap);
    layers.reserve(cap);
    layerIds.reserve(cap);

    if (fs.shouldRender) {
        for (uint32_t id : m_scene->submitOrder(now, cap)) {
            const SPanel* p = m_scene->get(id);
            if (!p)
                continue;
            SGpuPanel* g = ensureGpu(id);
            if (!g)
                continue;
            // Upload-on-change: re-raster + upload ONLY when the content epoch moved.
            if (g->uploadedEpoch != p->epoch) {
                SImage img = renderPanel(p->content, m_cfg.texW, m_cfg.texH, m_palette);
                if (!img.empty())
                    uploadPanel(*g, img.rgba.data());
                g->uploadedEpoch = p->epoch;
            }

            const float alpha = m_scene->alphaOf(*p, now);
            SPlacement  place = m_scene->placementOf(*p);
            XrSpace     space = (place.space == "local" && m_localSpace != XR_NULL_HANDLE)
                                    ? m_localSpace
                                    : m_viewSpace;

            XrCompositionLayerQuad quad = {XR_TYPE_COMPOSITION_LAYER_QUAD};
            quad.layerFlags               = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
            quad.space                    = space;
            quad.eyeVisibility            = XR_EYE_VISIBILITY_BOTH;
            quad.subImage.swapchain       = g->swapchain;
            quad.subImage.imageRect       = {{0, 0}, {m_cfg.texW, m_cfg.texH}};
            quad.subImage.imageArrayIndex = 0;
            quad.pose.orientation         = {0, 0, 0, 1};
            quad.pose.position            = {place.px, place.py, place.pz};
            float h                       = place.sizeW * (float)m_cfg.texH / (float)m_cfg.texW;
            quad.size                     = {place.sizeW, h};
            quads.push_back(quad);

            if (m_haveColorScale) {
                XrCompositionLayerColorScaleBiasKHR csb = {XR_TYPE_COMPOSITION_LAYER_COLOR_SCALE_BIAS_KHR};
                csb.colorScale = {alpha, alpha, alpha, alpha}; // premultiplied -> scale all channels.
                csb.colorBias  = {0, 0, 0, 0};
                csbs.push_back(csb);
                quads.back().next = &csbs.back();
            }
            layers.push_back(reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quads.back()));
            layerIds.push_back(id);
        }
    }

    XrFrameEndInfo ei       = {XR_TYPE_FRAME_END_INFO};
    ei.displayTime          = fs.predictedDisplayTime;
    ei.environmentBlendMode = m_blendMode;
    ei.layerCount           = (uint32_t)layers.size();
    ei.layers               = layers.data();
    XrResult re             = xrEndFrame(m_session, &ei);
    if (re == XR_ERROR_SESSION_LOST || re == XR_ERROR_INSTANCE_LOST) { m_lost = true; return false; }
    if (XR_FAILED(re) || !fs.shouldRender)
        return false;
    if (submittedIds)
        *submittedIds = std::move(layerIds);
    return true;
}

CSession::EBringUp CSession::bringUp(CEgl& egl, const SConfig& cfg, CScene& scene) {
    m_egl            = &egl;
    m_cfg            = cfg;
    m_scene          = &scene;
    m_lost           = false;
    m_sessionRunning = false;
    m_state          = XR_SESSION_STATE_UNKNOWN;

    if (!createInstance()) {
        teardown();
        return EBringUp::NoRuntime;
    }
    XrResult sysr = getSystem();
    if (sysr == XR_ERROR_FORM_FACTOR_UNAVAILABLE) {
        teardown();
        return EBringUp::NoHeadset; // runtime up, headset undonned — gentle fixed cadence.
    }
    if (XR_FAILED(sysr)) {
        teardown();
        return EBringUp::NoRuntime;
    }
    egl.makeCurrent(); // held current for the whole session (fence contract, 95c541a8).
    if (!createSession(egl)) {
        teardown();
        return EBringUp::NoRuntime;
    }
    if (!chooseSwapchainFormat()) {
        teardown();
        return EBringUp::NoRuntime;
    }
    return EBringUp::Live;
}

void CSession::requestExit() {
    if (m_session != XR_NULL_HANDLE && m_sessionRunning)
        xrRequestExitSession(m_session);
}

void CSession::teardown() {
    for (auto& [id, g] : m_gpu)
        if (g.swapchain != XR_NULL_HANDLE)
            xrDestroySwapchain(g.swapchain);
    m_gpu.clear(); // a fresh session re-rasters every panel on reconnect (memo §6.2 force-dirty).
    if (m_localSpace != XR_NULL_HANDLE) { xrDestroySpace(m_localSpace); m_localSpace = XR_NULL_HANDLE; }
    if (m_viewSpace != XR_NULL_HANDLE) { xrDestroySpace(m_viewSpace); m_viewSpace = XR_NULL_HANDLE; }
    if (m_session != XR_NULL_HANDLE) { xrDestroySession(m_session); m_session = XR_NULL_HANDLE; }
    // NOTE: the EGL context is owned by CDaemon and kept current across reconnects — NOT
    // released here.
    if (m_instance != XR_NULL_HANDLE) { xrDestroyInstance(m_instance); m_instance = XR_NULL_HANDLE; }
    m_sessionRunning = false;
    m_state          = XR_SESSION_STATE_UNKNOWN;
}

} // namespace hud
