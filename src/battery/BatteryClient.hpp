#pragma once

#include "Panel.hpp" // hud::SGauge

#include <cstdint>
#include <string>
#include <vector>

// hypxrhud-battery — the thin client to the hypxrhud daemon's D-Bus panel API
// (io.github.andrewgaspar.hypxrhud1). Owns the ONE persistent battery panel (created lazily
// once there is a gauge to show, dismissed when every source goes absent) and posts one-shot
// low-battery toasts. All the "should I send?" decisions are made by the pure model in
// main; this class only marshals a{sv} and talks the bus.

struct sd_bus;

namespace hudbat {

class CBatteryClient {
  public:
    // `session` is a borrowed connection to the user/session bus (where the hypxrhud daemon
    // lives). `slot` is the HUD slot the battery panel targets (default "battery").
    CBatteryClient(sd_bus* session, std::string slot) : m_bus(session), m_slot(std::move(slot)) {}

    // Create (if needed) or update the battery panel to show exactly `gauges`. An empty
    // list dismisses the panel (never a stale gauge). Returns false on a hard bus error;
    // a not-yet-activated daemon is a soft failure (returns false, retried next tick).
    bool syncPanel(const std::vector<hud::SGauge>& gauges);

    // Post a transient low-battery toast ("headset battery 14%"). Fire-and-forget.
    void postToast(const std::string& text);

    bool havePanel() const { return m_panelId != 0; }

  private:
    bool createPanel(const std::vector<hud::SGauge>& gauges); // -> m_panelId
    bool updatePanel(const std::vector<hud::SGauge>& gauges); // fire-and-forget
    void dismissPanel();

    sd_bus*     m_bus     = nullptr;
    std::string m_slot;
    uint32_t    m_panelId = 0;
};

} // namespace hudbat
