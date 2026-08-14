#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

namespace hud {

struct SPresentationSnapshot {
    uint64_t panelSerial = 0;
    uint64_t frameSerial = 0;
    uint64_t streakStart = 0;
};

// Pure bookkeeper for panel-specific presentation acknowledgements. A frame advances only
// after a successful xrEndFrame with shouldRender=true. A panel is current iff its serial
// equals the global frame serial; queued, budget-dropped, or otherwise omitted panels are
// therefore distinguishable from panels that reached the submitted layer array.
class CPresentationTracker {
  public:
    void recordFrame(bool submitted, const std::vector<uint32_t>& panelIds);
    void dismiss(uint32_t panelId);
    void reset();
    SPresentationSnapshot snapshot(uint32_t panelId) const;

  private:
    uint64_t m_frameSerial = 0;
    struct SPanelState {
        uint64_t last = 0;
        uint64_t streakStart = 0;
    };
    std::unordered_map<uint32_t, SPanelState> m_panelSerials;
};

} // namespace hud
