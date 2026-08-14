#include "Presentation.hpp"

namespace hud {

void CPresentationTracker::recordFrame(bool submitted, const std::vector<uint32_t>& panelIds) {
    if (!submitted)
        return;
    ++m_frameSerial;
    for (const uint32_t id : panelIds) {
        auto& state = m_panelSerials[id];
        if (state.last + 1 != m_frameSerial)
            state.streakStart = m_frameSerial;
        if (state.streakStart == 0)
            state.streakStart = m_frameSerial;
        state.last = m_frameSerial;
    }
}

void CPresentationTracker::dismiss(uint32_t panelId) {
    m_panelSerials.erase(panelId);
}

void CPresentationTracker::reset() {
    m_frameSerial = 0;
    m_panelSerials.clear();
}

SPresentationSnapshot CPresentationTracker::snapshot(uint32_t panelId) const {
    const auto found = m_panelSerials.find(panelId);
    return {
        .panelSerial = found == m_panelSerials.end() ? 0 : found->second.last,
        .frameSerial = m_frameSerial,
        .streakStart = found == m_panelSerials.end() ? 0 : found->second.streakStart,
    };
}

} // namespace hud
