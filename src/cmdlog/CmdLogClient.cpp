#include "CmdLogClient.hpp"

#include "Dbus.hpp" // hud::kBusName / kObjPath / kIface
#include "Log.hpp"

#include <cstring>
#include <systemd/sd-bus.h>

namespace hudcmd {
namespace {
    void appendString(sd_bus_message* message, const char* key, const char* value) {
        sd_bus_message_open_container(message, 'e', "sv");
        sd_bus_message_append(message, "s", key);
        sd_bus_message_append(message, "v", "s", value);
        sd_bus_message_close_container(message);
    }

    void appendInteger(sd_bus_message* message, const char* key, int32_t value) {
        sd_bus_message_open_container(message, 'e', "sv");
        sd_bus_message_append(message, "s", key);
        sd_bus_message_append(message, "v", "i", value);
        sd_bus_message_close_container(message);
    }

    void appendDouble(sd_bus_message* message, const char* key, double value) {
        sd_bus_message_open_container(message, 'e', "sv");
        sd_bus_message_append(message, "s", key);
        sd_bus_message_append(message, "v", "d", value);
        sd_bus_message_close_container(message);
    }

    void appendLines(sd_bus_message* message, const std::vector<hud::SLine>& lines) {
        sd_bus_message_open_container(message, 'e', "sv");
        sd_bus_message_append(message, "s", "lines");
        sd_bus_message_open_container(message, 'v', "a(sub)");
        sd_bus_message_open_container(message, 'a', "(sub)");
        for (const auto& line : lines)
            sd_bus_message_append(message, "(sub)", line.text.c_str(),
                                  static_cast<uint32_t>(line.color), line.big ? 1 : 0);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
        sd_bus_message_close_container(message);
    }

    void appendProps(sd_bus_message* message, const SCmdLogConfig& config,
                     const std::vector<hud::SLine>& lines, bool includeSlot) {
        sd_bus_message_open_container(message, 'a', "{sv}");
        if (includeSlot) {
            appendString(message, "slot", config.slot.c_str());
            appendInteger(message, "urgency", 1); // normal: the ticker never preempts a real status.
        }
        appendString(message, "kind", "text");
        appendInteger(message, "rise_ms", config.riseMs);
        appendInteger(message, "hold_ms", -1); // the model owns expiry; the panel persists.
        appendInteger(message, "fade_ms", config.fadeMs);
        appendDouble(message, "opacity", config.opacity);
        appendLines(message, lines);
        sd_bus_message_close_container(message);
    }
}

CCmdLogClient::~CCmdLogClient() {
    dismiss();
    if (m_match)
        sd_bus_slot_unref(m_match);
    if (m_ownerMatch)
        sd_bus_slot_unref(m_ownerMatch);
}

bool CCmdLogClient::init() {
    if (!m_bus)
        return false;
    const int result = sd_bus_match_signal(m_bus, &m_match, hud::kBusName, hud::kObjPath,
                                           hud::kIface, "PanelDismissed", onPanelDismissed, this);
    if (result < 0) {
        Log::log(Log::ERR, "[cmdlog] cannot subscribe to panel lifecycle: {}", std::strerror(-result));
        return false;
    }
    const int ownerResult = sd_bus_match_signal(m_bus, &m_ownerMatch, "org.freedesktop.DBus",
                                                "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                                "NameOwnerChanged", onNameOwnerChanged, this);
    if (ownerResult < 0) {
        Log::log(Log::ERR, "[cmdlog] cannot watch HUD ownership: {}", std::strerror(-ownerResult));
        return false;
    }
    return true;
}

bool CCmdLogClient::sync(const std::vector<hud::SLine>& lines) {
    if (lines.empty()) {
        dismiss();
        return true;
    }
    if (m_panelId == 0)
        return create(lines);
    return update(lines);
}

bool CCmdLogClient::create(const std::vector<hud::SLine>& lines) {
    sd_bus_message* call = nullptr;
    if (sd_bus_message_new_method_call(m_bus, &call, hud::kBusName, hud::kObjPath,
                                       hud::kIface, "CreatePanel") < 0)
        return false;
    appendProps(call, m_config, lines, true);

    sd_bus_error    error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int             result = sd_bus_call(m_bus, call, 0, &error, &reply);
    sd_bus_message_unref(call);
    if (result < 0) {
        // The daemon may be absent, starting, or the slot may be held by a higher urgency.
        // Cosmetic: log at DEBUG and let the next published command retry.
        Log::log(Log::DEBUG, "[cmdlog] CreatePanel failed ({}) — will retry",
                 error.message ? error.message : std::strerror(-result));
        sd_bus_error_free(&error);
        return false;
    }
    uint32_t id = 0;
    result = sd_bus_message_read(reply, "u", &id);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    if (result < 0 || id == 0)
        return false;
    m_panelId = id;
    Log::log(Log::INFO, "[cmdlog] ticker panel created (id {}, slot {})", id, m_config.slot);
    return true;
}

bool CCmdLogClient::update(const std::vector<hud::SLine>& lines) {
    sd_bus_message* call = nullptr;
    if (sd_bus_message_new_method_call(m_bus, &call, hud::kBusName, hud::kObjPath,
                                       hud::kIface, "UpdatePanel") < 0)
        return false;
    sd_bus_message_append(call, "u", m_panelId);
    appendProps(call, m_config, lines, false);

    // Fire-and-forget on the hot path (a command per keystroke-of-the-demo); the daemon
    // drains it from its bus fd on the next tick.
    sd_bus_message_set_expect_reply(call, 0);
    const int result = sd_bus_send(m_bus, call, nullptr);
    sd_bus_message_unref(call);
    if (result < 0) {
        Log::log(Log::DEBUG, "[cmdlog] UpdatePanel send failed: {}", std::strerror(-result));
        return false;
    }
    return true;
}

void CCmdLogClient::dismiss() {
    if (!m_bus || m_panelId == 0)
        return;
    sd_bus_error    error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    sd_bus_call_method(m_bus, hud::kBusName, hud::kObjPath, hud::kIface, "DismissPanel",
                       &error, &reply, "u", m_panelId);
    if (reply)
        sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    Log::log(Log::DEBUG, "[cmdlog] ticker panel dismissed (id {})", m_panelId);
    m_panelId = 0;
}

int CCmdLogClient::onPanelDismissed(sd_bus_message* message, void* userdata, sd_bus_error*) {
    auto*       self   = static_cast<CCmdLogClient*>(userdata);
    uint32_t    id     = 0;
    const char* reason = nullptr;
    if (sd_bus_message_read(message, "us", &id, &reason) >= 0 && id == self->m_panelId) {
        self->m_panelId = 0;
        Log::log(Log::DEBUG, "[cmdlog] panel {} ended ({})", id, reason ? reason : "?");
    }
    return 0;
}

int CCmdLogClient::onNameOwnerChanged(sd_bus_message* message, void* userdata, sd_bus_error*) {
    auto*       self = static_cast<CCmdLogClient*>(userdata);
    const char *name = nullptr, *oldOwner = nullptr, *newOwner = nullptr;
    if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0 || !name)
        return 0;
    if (std::strcmp(name, hud::kBusName) == 0 && oldOwner && *oldOwner &&
        (!newOwner || std::strcmp(oldOwner, newOwner) != 0)) {
        // The HUD restarted: our id is stale. Not fatal — the next command re-creates.
        self->m_panelId = 0;
        Log::log(Log::DEBUG, "[cmdlog] HUD owner changed; panel id dropped");
    }
    return 0;
}

} // namespace hudcmd
