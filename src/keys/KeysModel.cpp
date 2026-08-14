#include "KeysModel.hpp"

#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon-keysyms.h>
#include <xkbcommon/xkbcommon.h>

#include <array>
#include <cctype>
#include <utility>

namespace hudkeys {
namespace {
    constexpr xkb_keycode_t xkbCode(uint16_t evdevCode) {
        // xkbcommon keycodes follow the X11 convention; libinput/evdev codes do not.
        return static_cast<xkb_keycode_t>(evdevCode) + 8;
    }

    std::string specialLabel(xkb_keysym_t symbol) {
        switch (symbol) {
            case XKB_KEY_Escape: return "Esc";
            case XKB_KEY_Return: case XKB_KEY_KP_Enter: return "Enter";
            case XKB_KEY_Tab: case XKB_KEY_ISO_Left_Tab: return "Tab";
            case XKB_KEY_BackSpace: return "Backspace";
            case XKB_KEY_space: return "Space";
            case XKB_KEY_Delete: case XKB_KEY_KP_Delete: return "Delete";
            case XKB_KEY_Insert: case XKB_KEY_KP_Insert: return "Insert";
            case XKB_KEY_Home: case XKB_KEY_KP_Home: return "Home";
            case XKB_KEY_End: case XKB_KEY_KP_End: return "End";
            case XKB_KEY_Page_Up: case XKB_KEY_KP_Page_Up: return "PgUp";
            case XKB_KEY_Page_Down: case XKB_KEY_KP_Page_Down: return "PgDown";
            case XKB_KEY_Left: case XKB_KEY_KP_Left: return "Left";
            case XKB_KEY_Right: case XKB_KEY_KP_Right: return "Right";
            case XKB_KEY_Up: case XKB_KEY_KP_Up: return "Up";
            case XKB_KEY_Down: case XKB_KEY_KP_Down: return "Down";
            case XKB_KEY_Print: return "Print";
            case XKB_KEY_Pause: return "Pause";
            default: break;
        }
        if (symbol >= XKB_KEY_F1 && symbol <= XKB_KEY_F35)
            return "F" + std::to_string(symbol - XKB_KEY_F1 + 1);
        return {};
    }

    bool validPrintable(const std::string& value) {
        if (value.empty())
            return false;
        for (const unsigned char c : value)
            if (c < 0x20 || c == 0x7f)
                return false;
        return true;
    }
}

CXkbState::~CXkbState() {
    if (m_state) xkb_state_unref(m_state);
    if (m_keymap) xkb_keymap_unref(m_keymap);
    if (m_context) xkb_context_unref(m_context);
}

CXkbState::CXkbState(CXkbState&& other) noexcept {
    *this = std::move(other);
}

CXkbState& CXkbState::operator=(CXkbState&& other) noexcept {
    if (this == &other)
        return *this;
    if (m_state) xkb_state_unref(m_state);
    if (m_keymap) xkb_keymap_unref(m_keymap);
    if (m_context) xkb_context_unref(m_context);
    m_context = std::exchange(other.m_context, nullptr);
    m_keymap = std::exchange(other.m_keymap, nullptr);
    m_state = std::exchange(other.m_state, nullptr);
    return *this;
}

bool CXkbState::init(const SKeysConfig& config, std::string& error) {
    m_context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
    if (!m_context) {
        error = "cannot create xkb context";
        return false;
    }
    xkb_rule_names names = {
        .rules = config.rules.empty() ? nullptr : config.rules.c_str(),
        .model = config.model.empty() ? nullptr : config.model.c_str(),
        .layout = config.layout.c_str(),
        .variant = config.variant.empty() ? nullptr : config.variant.c_str(),
        .options = config.options.empty() ? nullptr : config.options.c_str(),
    };
    m_keymap = xkb_keymap_new_from_names(m_context, &names, XKB_KEYMAP_COMPILE_NO_FLAGS);
    if (!m_keymap) {
        error = "cannot compile configured xkb keymap";
        return false;
    }
    m_state = xkb_state_new(m_keymap);
    if (!m_state) {
        error = "cannot create xkb state";
        return false;
    }
    return true;
}

void CXkbState::update(uint16_t evdevCode, EKeyState state) {
    xkb_state_update_key(m_state, xkbCode(evdevCode),
                         state == EKeyState::Pressed ? XKB_KEY_DOWN : XKB_KEY_UP);
}

bool CXkbState::isModifier(uint16_t evdevCode) const {
    switch (evdevCode) {
        case KEY_LEFTSHIFT: case KEY_RIGHTSHIFT:
        case KEY_LEFTCTRL: case KEY_RIGHTCTRL:
        case KEY_LEFTALT: case KEY_RIGHTALT:
        case KEY_LEFTMETA: case KEY_RIGHTMETA:
        case KEY_CAPSLOCK: case KEY_NUMLOCK: case KEY_SCROLLLOCK:
            return true;
        default:
            return false;
    }
}

bool CXkbState::isTextKey(uint16_t evdevCode) const {
    const xkb_keysym_t symbol = xkb_state_key_get_one_sym(m_state, xkbCode(evdevCode));
    std::array<char, 64> utf8 = {};
    const int bytes = xkb_keysym_to_utf8(symbol, utf8.data(), utf8.size());
    return bytes > 0 && validPrintable(std::string(utf8.data(), static_cast<size_t>(bytes - 1)));
}

bool CXkbState::modActive(const char* name) const {
    return xkb_state_mod_name_is_active(m_state, name, XKB_STATE_MODS_EFFECTIVE) > 0;
}

std::string CXkbState::chordPrefix() const {
    std::string result;
    auto append = [&](const char* label) {
        if (!result.empty())
            result += "+";
        result += label;
    };
    // Fixed UI order makes a chord stable even if the modifiers were pressed differently.
    if (modActive(XKB_MOD_NAME_LOGO)) append("Super");
    if (modActive(XKB_MOD_NAME_CTRL)) append("Ctrl");
    if (modActive(XKB_MOD_NAME_ALT)) append("Alt");
    if (modActive(XKB_MOD_NAME_SHIFT)) append("Shift");
    if (!result.empty())
        result += "+";
    return result;
}

bool CXkbState::shortcutModifiersActive() const {
    return modActive(XKB_MOD_NAME_LOGO) || modActive(XKB_MOD_NAME_CTRL) || modActive(XKB_MOD_NAME_ALT);
}

std::string CXkbState::keyLabel(uint16_t evdevCode) const {
    const xkb_keysym_t symbol = xkb_state_key_get_one_sym(m_state, xkbCode(evdevCode));
    if (const std::string special = specialLabel(symbol); !special.empty())
        return special;

    std::array<char, 64> utf8 = {};
    const int bytes = xkb_keysym_to_utf8(symbol, utf8.data(), utf8.size());
    if (bytes > 0) {
        std::string value(utf8.data(), static_cast<size_t>(bytes - 1));
        if (validPrintable(value)) {
            if (value.size() == 1)
                value[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(value[0])));
            return value;
        }
    }

    std::array<char, 64> name = {};
    if (xkb_keysym_get_name(symbol, name.data(), name.size()) > 0)
        return name.data();
    return "Key" + std::to_string(evdevCode);
}

bool CKeysModel::handle(const SKeyEvent& event, int64_t nowMs) {
    if (event.state == EKeyState::Released) {
        if (m_pressed.erase(event.code) == 0)
            return false;
        m_xkb.update(event.code, event.state);
        return false;
    }

    if (!m_pressed.insert(event.code).second)
        return false; // held-key repeat from a noisy source.
    m_xkb.update(event.code, event.state);
    if (m_xkb.isModifier(event.code))
        return false;

    const std::string label = m_xkb.keyLabel(event.code);
    if (label.empty())
        return false;

    // In privacy mode, ordinary typed characters (including Shift+letter and Space) never
    // enter the model. Shortcuts and named navigation/control keys remain visible.
    const bool printableText = m_xkb.isTextKey(event.code);
    if (m_config.modsOnly && printableText && !m_xkb.shortcutModifiersActive())
        return false;

    const std::string chord = m_xkb.chordPrefix() + label;
    if (!m_history.empty() && m_history.front().text == chord &&
        nowMs - m_history.front().lastMs <= m_config.coalesceMs) {
        ++m_history.front().count;
        m_history.front().lastMs = nowMs;
        return true;
    }

    m_history.push_front({.text = chord, .count = 1, .lastMs = nowMs});
    while (static_cast<int>(m_history.size()) > m_config.history)
        m_history.pop_back();
    return true;
}

std::vector<hud::SLine> CKeysModel::lines() const {
    std::vector<hud::SLine> result;
    result.reserve(m_history.size());
    bool newest = true;
    for (const auto& entry : m_history) {
        std::string text = entry.text;
        if (entry.count > 1)
            text += "  x" + std::to_string(entry.count);
        result.push_back({std::move(text), newest ? hud::EColor::Accent : hud::EColor::Dim, newest});
        newest = false;
    }
    return result;
}

} // namespace hudkeys
