#pragma once

#include "KeyEvent.hpp"
#include "KeysConfig.hpp"
#include "Panel.hpp"

#include <cstdint>
#include <deque>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

struct xkb_context;
struct xkb_keymap;
struct xkb_state;

namespace hudkeys {

class CXkbState {
  public:
    CXkbState() = default;
    ~CXkbState();
    CXkbState(const CXkbState&) = delete;
    CXkbState& operator=(const CXkbState&) = delete;
    CXkbState(CXkbState&& other) noexcept;
    CXkbState& operator=(CXkbState&& other) noexcept;

    bool init(const SKeysConfig& config, std::string& error);
    void update(uint16_t evdevCode, EKeyState state);
    bool isModifier(uint16_t evdevCode) const;
    bool isTextKey(uint16_t evdevCode) const;
    std::string keyLabel(uint16_t evdevCode) const;
    std::string chordPrefix() const;
    bool shortcutModifiersActive() const;

  private:
    bool modActive(const char* name) const;

    xkb_context* m_context = nullptr;
    xkb_keymap*  m_keymap = nullptr;
    xkb_state*   m_state = nullptr;
};

class CKeysModel {
  public:
    explicit CKeysModel(SKeysConfig config) : m_config(std::move(config)) {}

    bool init(std::string& error) { return m_xkb.init(m_config, error); }

    // Returns true only when the visible history changed. Releases, held-key repeats,
    // standalone modifiers, and privacy-filtered text still update keyboard state but do
    // not produce a HUD row.
    bool handle(const SKeyEvent& event, int64_t nowMs);
    std::vector<hud::SLine> lines() const;

  private:
    struct SHistory {
        std::string text;
        int         count = 1;
        int64_t     lastMs = 0;
    };

    SKeysConfig         m_config;
    CXkbState           m_xkb;
    std::set<uint16_t>  m_pressed;
    std::deque<SHistory> m_history;
};

} // namespace hudkeys
