#pragma once

#include "KeysConfig.hpp"
#include "Panel.hpp"
#include "Presentation.hpp"

#include <cstdint>
#include <optional>
#include <vector>

struct sd_bus;
struct sd_bus_error;
struct sd_bus_message;
struct sd_bus_slot;

namespace hudkeys {

class CKeysClient {
  public:
    CKeysClient(sd_bus* bus, SKeysConfig config);
    ~CKeysClient();
    CKeysClient(const CKeysClient&) = delete;
    CKeysClient& operator=(const CKeysClient&) = delete;

    bool init();
    bool showPrivacyIndicator();
    bool sync(const std::vector<hud::SLine>& lines);
    bool process();
    bool runtimeLive() const;
    std::optional<hud::SPresentationSnapshot> presentation(uint32_t panelId) const;
    bool healthy() const { return !m_ownerLost && !m_processFailed; }
    int fd() const;
    int events() const;
    int timeoutMs() const;
    uint32_t panelId() const { return m_panelId; }

  private:
    static int onPanelDismissed(sd_bus_message* message, void* userdata, sd_bus_error* error);
    static int onNameOwnerChanged(sd_bus_message* message, void* userdata, sd_bus_error* error);
    bool create(const std::vector<hud::SLine>& lines, int riseMs, int holdMs);
    bool update(const std::vector<hud::SLine>& lines, int riseMs, int holdMs);
    hud::SLine privacyLine() const;
    void dismiss();

    sd_bus*      m_bus = nullptr;
    sd_bus_slot* m_match = nullptr;
    sd_bus_slot* m_ownerMatch = nullptr;
    SKeysConfig  m_config;
    uint32_t     m_panelId = 0;
    bool         m_ownerLost = false;
    bool         m_processFailed = false;
};

} // namespace hudkeys
