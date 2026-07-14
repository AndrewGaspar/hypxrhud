#pragma once

#include "Panel.hpp"
#include "Slots.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

// hypxrhud — the PURE scene manager: the panel table (id -> panel), slot arbitration,
// per-client budgets, and the layer-budget math. NO OpenXR here — the XR session
// (Session) mirrors this table into GPU swapchains and submits one quad per visible
// panel. Both the interim stdin transport (Wire) and the future D-Bus front end
// (WP-H3) drive the scene through the SAME `upsert`/`dismiss`/`dropOwner` API, so H3
// swaps the transport without touching this arbiter. Unit-tested directly.

namespace hud {

// One panel in the scene. `epoch` bumps whenever the rasterisable content changes, so
// the XR side re-uploads ONLY changed panels (the upload-on-change / idle-monitor
// trick, hypxrvoice HudSession.cpp:238). `shownAtMs` starts the fade envelope.
struct SPanel {
    uint32_t      id      = 0;
    std::string   owner   = "stdin"; // creator identity (unique bus name under D-Bus).
    std::string   slot;              // named slot, or "" for free placement.
    std::string   space   = "view";  // resolved reference space.
    int           urgency = 1;       // 0 low / 1 normal / 2 critical (slot priority).

    // Explicit placement override (used for free placement, or to nudge a slotted panel).
    bool  hasPose = false;
    float px = 0.f, py = 0.f, pz = -1.0f;
    bool  hasSize = false;
    float sizeW = 0.42f;

    SFade         fade;
    SPanelContent content;

    int64_t  shownAtMs = 0; // envelope clock origin (renderer monotonic ms).
    uint64_t epoch     = 0; // content revision; XR side compares to detect a re-raster.

    // WP-H5 queueing: a lower-urgency loser to a busy `on_refuse = queue` singleton slot is
    // held pending (queued=true) — it occupies no head-space and is never submitted or
    // expired; it is promoted (queued=false, envelope clock restarted) when the slot frees.
    bool queued = false;
};

// A create-or-update request. id==0 allocates a new panel; a known id updates in place.
// This is the shape the D-Bus `CreatePanel`/`UpdatePanel` props marshal into (§2.2).
struct SUpsert {
    uint32_t      id    = 0;
    std::string   owner = "stdin";
    std::string   slot;
    std::string   space;            // "" -> inherit slot default (or "view" free).
    bool          hasPose = false;
    float         px = 0.f, py = 0.f, pz = -1.0f;
    bool          hasSize = false;
    float         sizeW = 0.42f;
    int           urgency = 1;
    SFade         fade;
    SPanelContent content;

    // MERGE presence flags (BUG-2). On an UPDATE (id != 0) ONLY supplied keys overwrite the
    // panel's current value; absent keys are preserved — a partial UpdatePanel (e.g. the
    // battery client resending only its gauges) must NOT reset the panel's slot/pose/size/
    // envelope to defaults and snap it to centre-FoV. On a CREATE every field is applied
    // (absent ones use the SUpsert defaults, seeded from config). pose/size reuse hasPose/
    // hasSize (a pose/size key is the only way those become true — there is no "clear").
    // upsertFromProps sets these per key present; the pure mapper never inspects the scene.
    bool setSlot = false, setSpace = false, setUrgency = false;
    bool setRise = false, setHold = false, setFade = false, setOpacity = false;
    bool setKind = false, setConfidence = false, setLines = false, setGauges = false;
};

// A panel that left the scene, with a reason (design memo §2.3 PanelDismissed reasons:
// "expired" | "client" | "preempted" | "client-gone"). H3 emits these as D-Bus signals.
struct SDismissal {
    uint32_t    id;
    std::string reason;
};

// Per-slot occupancy snapshot for GetCapabilities introspection (WP-H5): how many panels a
// slot currently holds active (visible / stacked) vs held-pending (queued).
struct SSlotStat {
    std::string name;
    int         active = 0;
    int         queued = 0;
};

class CScene {
  public:
    explicit CScene(int perClientCap = 4) : m_perClientCap(perClientCap) {}

    CSlots&       slots() { return m_slots; }
    const CSlots& slots() const { return m_slots; }

    // Create or update a panel. Returns the panel id, or 0 if REJECTED (per-client cap
    // reached for a new panel, or a lower-urgency newcomer refused a busy singleton
    // slot). Any panels evicted by the operation (singleton preemption / stack
    // overflow) are appended to `dismissed`.
    uint32_t upsert(const SUpsert& u, int64_t nowMs, std::vector<SDismissal>* dismissed = nullptr);

    // Remove a panel. reason is recorded for the caller (H3 signal). Returns false if
    // the id is unknown. `nowMs` lets a freed singleton slot promote a queued panel (WP-H5);
    // pass the render clock (0 = no reconcile, for pure remove in tests).
    bool dismiss(uint32_t id, const std::string& reason, int64_t nowMs = 0);

    // Auto-dismiss every panel owned by `owner` (client-gone, WP-H3 NameOwnerChanged).
    // Appends the removed ids to `dismissed` if provided; `nowMs` drives queue promotion.
    void dropOwner(const std::string& owner, std::vector<SDismissal>* dismissed = nullptr,
                   int64_t nowMs = 0);

    // Drop panels whose envelope has fully faded (transient panels time out on their
    // own). Appends removed ids with reason "expired". Call once per frame/tick. Queued
    // panels never expire (their envelope clock has not started); a freed slot promotes one.
    void reapExpired(int64_t nowMs, std::vector<SDismissal>* dismissed = nullptr);

    // Bump every panel's content epoch so the XR side re-rasters them all (WP-H6 theme
    // reload: force-dirty so the new palette takes effect without a content change).
    void forceRedrawAll();

    // Per-slot occupancy for GetCapabilities (WP-H5), in slot-registry order.
    std::vector<SSlotStat> slotStats() const;

    const std::map<uint32_t, SPanel>& panels() const { return m_panels; }
    const SPanel*                     get(uint32_t id) const;
    size_t                            ownerCount(const std::string& owner) const;

    // Resolve a panel's on-screen placement: its slot default, plus a stack offset for
    // stack slots, with any explicit pose/size override applied.
    SPlacement placementOf(const SPanel& p) const;

    // Current fade opacity for a panel (envelope over nowMs - shownAtMs).
    float alphaOf(const SPanel& p, int64_t nowMs) const;

    // Panels to actually submit this frame: those with alpha > ~0, ordered bottom→top
    // for the layers[] array (higher urgency and newer on top), capped to `budget`
    // layers. When more panels are visible than the budget allows, the lowest-priority
    // / oldest are dropped from submission (coalesced) rather than overflowing xrEndFrame.
    std::vector<uint32_t> submitOrder(int64_t nowMs, int budget) const;

  private:
    // Stack index of a stack-slot panel: 0 = newest (base pose), increasing = older,
    // pushed up. Panels beyond the slot's maxStack are not placed (they were evicted
    // at upsert time, but this is defensive).
    int stackIndex(const SPanel& p) const;

    // Promote a held-pending (queued) panel into any singleton slot with no active occupant
    // (WP-H5). Picks the highest-urgency, then oldest, queued member; restarts its envelope
    // at nowMs and bumps its epoch so the XR side rasters it. Called after any removal.
    void reconcileQueues(int64_t nowMs);

    CSlots                     m_slots;
    std::map<uint32_t, SPanel> m_panels;
    uint32_t                   m_nextId      = 1;
    int                        m_perClientCap = 4;
};

// --- pure layer-budget math (design memo §1.1) ---------------------------------------

// The internal conservative cap on simultaneous panels/layers. The OpenXR spec floors
// maxLayerCount at 16; our stack reports 128, but a dozen panels is plenty and 16 keeps
// us safe on any runtime that only guarantees the minimum.
inline constexpr int kInternalLayerCap = 16;

// Effective per-frame panel budget given the runtime's advertised maxLayerCount.
// runtimeMax<=0 (unknown) is treated as the spec minimum. Never exceeds the runtime's
// own limit, never exceeds our internal cap.
int layerBudget(int64_t runtimeMax);

} // namespace hud
