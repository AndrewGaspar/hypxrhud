#include "Scene.hpp"

#include <algorithm>
#include <climits>
#include <cmath>

namespace hud {

namespace {
    constexpr float kVisibleAlpha = 1.f / 255.f; // below this a panel contributes nothing.

    bool gaugesEqual(const std::vector<SGauge>& a, const std::vector<SGauge>& b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++)
            if (a[i].label != b[i].label || a[i].percent != b[i].percent || a[i].charging != b[i].charging)
                return false;
        return true;
    }
    bool linesEqual(const std::vector<SLine>& a, const std::vector<SLine>& b) {
        if (a.size() != b.size())
            return false;
        for (size_t i = 0; i < a.size(); i++)
            if (a[i].text != b[i].text || a[i].color != b[i].color || a[i].big != b[i].big)
                return false;
        return true;
    }
    // Do two contents rasterise identically? Used to keep a redundant UpdatePanel from
    // bumping the epoch — a static panel then costs zero (no re-raster, no upload).
    bool contentEqual(const SPanelContent& a, const SPanelContent& b) {
        return a.kind == b.kind && a.confidence == b.confidence && linesEqual(a.lines, b.lines) &&
               gaugesEqual(a.gauges, b.gauges);
    }
}

int layerBudget(int64_t runtimeMax) {
    int m = runtimeMax <= 0 ? 16 : static_cast<int>(std::min<int64_t>(runtimeMax, INT_MAX));
    return std::min(m, kInternalLayerCap);
}

const SPanel* CScene::get(uint32_t id) const {
    auto it = m_panels.find(id);
    return it == m_panels.end() ? nullptr : &it->second;
}

size_t CScene::ownerCount(const std::string& owner) const {
    size_t n = 0;
    for (const auto& [id, p] : m_panels)
        if (p.owner == owner)
            n++;
    return n;
}

uint32_t CScene::upsert(const SUpsert& u, int64_t nowMs, std::vector<SDismissal>* dismissed) {
    const SSlot* slot = u.slot.empty() ? nullptr : m_slots.find(u.slot);

    // --- update an existing panel in place ---
    if (u.id != 0) {
        auto it = m_panels.find(u.id);
        if (it != m_panels.end()) {
            SPanel& p = it->second;

            // MERGE semantics (BUG-2): a partial UpdatePanel changes ONLY the fields the
            // client supplied; every absent field keeps the panel's current value. Arbitration
            // (singleton move / urgency) only reconsiders when the relevant field was supplied.
            const int newUrgency = u.setUrgency ? u.urgency : p.urgency;

            // If the panel is (re)entering a busy singleton slot owned by someone else,
            // arbitrate exactly as a fresh create would (skip queued members + self). Only a
            // SUPPLIED slot that actually changes triggers a move.
            const bool singletonMove = u.setSlot && slot && !slot->stack && u.slot != p.slot;
            bool       makeQueued    = false;
            if (singletonMove) {
                uint32_t occ = 0;
                for (auto& [oid, op] : m_panels)
                    if (oid != u.id && op.slot == u.slot && !op.queued) { occ = oid; break; }
                if (occ) {
                    if (newUrgency < m_panels.at(occ).urgency) {
                        if (slot->onRefuse == ERefusePolicy::Queue)
                            makeQueued = true; // held pending; promoted when the slot frees.
                        else
                            return 0;          // lower-priority newcomer refused the slot.
                    } else {
                        if (dismissed)
                            dismissed->push_back({occ, "preempted"}); // equal => last-writer-wins.
                        m_panels.erase(occ);
                    }
                }
            }

            // Content merges per sub-field: absent lines/gauges/kind/confidence are preserved,
            // so an update carrying only gauges keeps any existing title/lines. Epoch bumps
            // ONLY when the merged content actually rasterises differently (zero-cost contract).
            SPanelContent merged = p.content;
            if (u.setKind)       merged.kind       = u.content.kind;
            if (u.setConfidence) merged.confidence = u.content.confidence;
            if (u.setLines)      merged.lines      = u.content.lines;
            if (u.setGauges)     merged.gauges      = u.content.gauges;
            const bool changed = !contentEqual(p.content, merged);

            p.owner = u.owner; // owner is the caller's bus name from context — always authoritative.
            if (u.setSlot)    p.slot    = u.slot;
            if (u.setSpace)   p.space   = u.space;
            if (u.setUrgency) p.urgency = u.urgency;
            if (u.hasPose)  { p.hasPose = true; p.px = u.px; p.py = u.py; p.pz = u.pz; }
            if (u.hasSize)  { p.hasSize = true; p.sizeW = u.sizeW; }
            if (u.setRise)    p.fade.riseMs      = u.fade.riseMs;
            if (u.setHold)    p.fade.holdMs      = u.fade.holdMs;
            if (u.setFade)    p.fade.fadeMs      = u.fade.fadeMs;
            if (u.setOpacity) p.fade.opacityCeil = u.fade.opacityCeil;
            if (singletonMove)
                p.queued = makeQueued; // moving into a slot resolves the panel's queued state.
            if (changed) {
                p.content   = std::move(merged);
                p.epoch++;            // triggers a single re-raster + upload on the XR side.
                p.shownAtMs = nowMs;  // refresh the dwell for the new content.
            }
            reconcileQueues(nowMs); // a vacated singleton slot may promote a queued panel.
            return u.id;
        }
        // Unknown id: fall through and create (best-effort under a lossy transport).
    }

    // --- create a new panel ---
    if (ownerCount(u.owner) >= static_cast<size_t>(m_perClientCap))
        return 0; // per-client cap reached (design memo §5 / triage #9: default 4).

    bool createQueued = false;
    if (slot && !slot->stack) {
        // Singleton slot: arbitrate against the current ACTIVE occupant (queued members don't
        // hold the slot). Equal urgency = last-writer-wins; higher preempts; a strictly-lower
        // newcomer is refused, or held-pending under on_refuse=queue (WP-H5, §4.2).
        uint32_t occ = 0;
        for (auto& [oid, op] : m_panels)
            if (op.slot == u.slot && !op.queued) { occ = oid; break; }
        if (occ) {
            if (u.urgency < m_panels.at(occ).urgency) {
                if (slot->onRefuse == ERefusePolicy::Queue)
                    createQueued = true;
                else
                    return 0; // occupant outranks the newcomer.
            } else {
                if (dismissed)
                    dismissed->push_back({occ, "preempted"});
                m_panels.erase(occ); // invariant: at most one active occupant.
            }
        }
    } else if (slot && slot->stack) {
        // Stack slot: evict the oldest while at capacity.
        std::vector<uint32_t> members;
        for (auto& [oid, op] : m_panels)
            if (op.slot == u.slot)
                members.push_back(oid);
        while (static_cast<int>(members.size()) >= slot->maxStack) {
            auto oldest = std::min_element(members.begin(), members.end(), [&](uint32_t a, uint32_t b) {
                return m_panels.at(a).shownAtMs < m_panels.at(b).shownAtMs;
            });
            if (dismissed)
                dismissed->push_back({*oldest, "preempted"});
            m_panels.erase(*oldest);
            members.erase(oldest);
        }
    }

    SPanel p;
    p.id        = m_nextId++;
    p.owner     = u.owner;
    p.slot      = u.slot;
    p.space     = u.space;
    p.urgency   = u.urgency;
    p.hasPose   = u.hasPose; p.px = u.px; p.py = u.py; p.pz = u.pz;
    p.hasSize   = u.hasSize; p.sizeW = u.sizeW;
    p.fade      = u.fade;
    p.content   = u.content;
    p.shownAtMs = nowMs;
    p.epoch     = 1;
    p.queued    = createQueued;
    uint32_t id = p.id;
    m_panels.emplace(id, std::move(p));
    return id;
}

bool CScene::dismiss(uint32_t id, const std::string&, int64_t nowMs) {
    const bool erased = m_panels.erase(id) > 0;
    if (erased && nowMs > 0)
        reconcileQueues(nowMs); // the freed slot may promote a queued panel (WP-H5).
    return erased;
}

void CScene::dropOwner(const std::string& owner, std::vector<SDismissal>* dismissed, int64_t nowMs) {
    bool removed = false;
    for (auto it = m_panels.begin(); it != m_panels.end();) {
        if (it->second.owner == owner) {
            if (dismissed)
                dismissed->push_back({it->first, "client-gone"});
            it      = m_panels.erase(it);
            removed = true;
        } else {
            ++it;
        }
    }
    if (removed && nowMs > 0)
        reconcileQueues(nowMs);
}

void CScene::reapExpired(int64_t nowMs, std::vector<SDismissal>* dismissed) {
    bool removed = false;
    for (auto it = m_panels.begin(); it != m_panels.end();) {
        const SPanel& p = it->second;
        // Queued panels have no running envelope — they never expire (WP-H5).
        if (p.queued) { ++it; continue; }
        const bool transient = p.fade.holdMs >= 0;
        if (transient && alphaOf(p, nowMs) <= kVisibleAlpha) {
            const int64_t total = std::max(0, p.fade.riseMs) + p.fade.holdMs + std::max(0, p.fade.fadeMs);
            if (nowMs - p.shownAtMs >= total) {
                if (dismissed)
                    dismissed->push_back({it->first, "expired"});
                it      = m_panels.erase(it);
                removed = true;
                continue;
            }
        }
        ++it;
    }
    if (removed)
        reconcileQueues(nowMs); // an expired singleton occupant may promote a queued panel.
}

void CScene::forceRedrawAll() {
    for (auto& [id, p] : m_panels)
        p.epoch++;
}

std::vector<SSlotStat> CScene::slotStats() const {
    std::vector<SSlotStat> out;
    for (const auto& s : m_slots.all()) {
        SSlotStat st;
        st.name = s.name;
        for (const auto& [id, p] : m_panels)
            if (p.slot == s.name) {
                if (p.queued) st.queued++;
                else          st.active++;
            }
        out.push_back(st);
    }
    return out;
}

void CScene::reconcileQueues(int64_t nowMs) {
    for (const auto& slot : m_slots.all()) {
        if (slot.stack)
            continue; // stacks never queue.
        bool hasActive = false;
        for (const auto& [id, p] : m_panels)
            if (p.slot == slot.name && !p.queued) { hasActive = true; break; }
        if (hasActive)
            continue;
        // Promote the best pending panel: highest urgency, then oldest (smallest id).
        uint32_t best = 0;
        int      bestUrg = -1;
        for (const auto& [id, p] : m_panels) {
            if (p.slot != slot.name || !p.queued)
                continue;
            if (p.urgency > bestUrg || (p.urgency == bestUrg && (best == 0 || id < best))) {
                best    = id;
                bestUrg = p.urgency;
            }
        }
        if (best) {
            SPanel& p   = m_panels.at(best);
            p.queued    = false;
            p.shownAtMs = nowMs; // start its envelope now that it is on screen.
            p.epoch++;           // force the XR side to raster it.
        }
    }
}

int CScene::stackIndex(const SPanel& p) const {
    // 0 = newest (base pose); older panels get larger indices (pushed up).
    int idx = 0;
    for (const auto& [id, op] : m_panels)
        if (op.slot == p.slot && op.id != p.id && op.shownAtMs > p.shownAtMs)
            idx++;
    return idx;
}

SPlacement CScene::placementOf(const SPanel& p) const {
    SPlacement out;
    const SSlot* slot = p.slot.empty() ? nullptr : m_slots.find(p.slot);
    if (slot) {
        out.px = slot->px; out.py = slot->py; out.pz = slot->pz;
        out.sizeW = slot->sizeW;
        out.space = slot->space;
        if (slot->stack)
            out.py += static_cast<float>(stackIndex(p)) * slot->stackDy;
    } else {
        out.px = 0.f; out.py = 0.f; out.pz = -1.0f;
        out.sizeW = 0.42f;
        out.space = "view";
    }
    // Explicit overrides win over the slot default.
    if (p.hasPose) { out.px = p.px; out.py = p.py; out.pz = p.pz; }
    if (p.hasSize) out.sizeW = p.sizeW;
    if (!p.space.empty()) out.space = p.space;
    return out;
}

float CScene::alphaOf(const SPanel& p, int64_t nowMs) const {
    return envelopeOpacity(p.fade, nowMs - p.shownAtMs);
}

std::vector<uint32_t> CScene::submitOrder(int64_t nowMs, int budget) const {
    std::vector<uint32_t> vis;
    for (const auto& [id, p] : m_panels)
        if (!p.queued && alphaOf(p, nowMs) > kVisibleAlpha) // queued panels are not submitted.
            vis.push_back(id);

    // Bottom -> top for the layers[] array (later = on top). Higher urgency and newer
    // sort LAST (on top). Tie-break on id for determinism.
    std::sort(vis.begin(), vis.end(), [&](uint32_t a, uint32_t b) {
        const SPanel& pa = m_panels.at(a);
        const SPanel& pb = m_panels.at(b);
        if (pa.urgency != pb.urgency) return pa.urgency < pb.urgency;
        if (pa.shownAtMs != pb.shownAtMs) return pa.shownAtMs < pb.shownAtMs;
        return a < b;
    });

    // Over budget: drop the lowest-priority / oldest (the FRONT of the array).
    if (budget >= 0 && static_cast<int>(vis.size()) > budget)
        vis.erase(vis.begin(), vis.end() - budget);
    return vis;
}

} // namespace hud
