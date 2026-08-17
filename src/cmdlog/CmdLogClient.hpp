#pragma once

#include "CmdLogConfig.hpp"
#include "Panel.hpp"

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// hypxrhud-cmdlog — the HUD panel adapter (the battery client's shape: create on the first
// row, fire-and-forget updates, dismiss the moment the model empties so the slot is not
// merely blank but GONE). It also mirrors the keys client's lifecycle matches — a
// PanelDismissed for our id or a HUD owner change clears the id so the next row re-creates
// the panel — but a missing daemon is NOT fatal here: the ticker is cosmetic, so it keeps
// running and retries.

struct sd_bus;
struct sd_bus_slot;
struct sd_bus_message;
struct sd_bus_error;

namespace hudcmd {

class CCmdLogClient {
  public:
    CCmdLogClient(sd_bus* bus, SCmdLogConfig config) : m_bus(bus), m_config(std::move(config)) {}
    ~CCmdLogClient();

    CCmdLogClient(const CCmdLogClient&)            = delete;
    CCmdLogClient& operator=(const CCmdLogClient&) = delete;

    // Subscribe to PanelDismissed + NameOwnerChanged. Returns false only if the matches
    // cannot be installed (a broken bus connection).
    bool init();

    // Create/update/dismiss so the HUD shows exactly `lines` (empty = no panel at all).
    // Returns false when the daemon could not be reached; the caller simply retries.
    bool sync(const std::vector<hud::SLine>& lines);

    void     dismiss();
    uint32_t panelId() const { return m_panelId; }

  private:
    bool create(const std::vector<hud::SLine>& lines);
    bool update(const std::vector<hud::SLine>& lines);

    static int onPanelDismissed(sd_bus_message*, void*, sd_bus_error*);
    static int onNameOwnerChanged(sd_bus_message*, void*, sd_bus_error*);

    sd_bus*       m_bus        = nullptr;
    SCmdLogConfig m_config;
    uint32_t      m_panelId    = 0;
    sd_bus_slot*  m_match      = nullptr;
    sd_bus_slot*  m_ownerMatch = nullptr;
};

} // namespace hudcmd
