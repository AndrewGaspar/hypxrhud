#include "KeysClient.hpp"

#include "Dbus.hpp"
#include "Log.hpp"

#include <cerrno>
#include <cstring>
#include <poll.h>
#include <systemd/sd-bus.h>
#include <utility>

namespace hudkeys {
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

    void appendProps(sd_bus_message* message, const SKeysConfig& config,
                     const std::vector<hud::SLine>& lines, int riseMs, int holdMs, bool includeSlot) {
        sd_bus_message_open_container(message, 'a', "{sv}");
        if (includeSlot) {
            appendString(message, "slot", config.slot.c_str());
            appendInteger(message, "urgency", 2); // disclosure wins budget/slot ties.
        }
        appendString(message, "kind", "text");
        appendInteger(message, "rise_ms", riseMs);
        appendInteger(message, "hold_ms", holdMs);
        appendDouble(message, "opacity", config.opacity);
        appendLines(message, lines);
        sd_bus_message_close_container(message);
    }
}

CKeysClient::CKeysClient(sd_bus* bus, SKeysConfig config) : m_bus(bus), m_config(std::move(config)) {}

CKeysClient::~CKeysClient() {
    dismiss();
    if (m_match)
        sd_bus_slot_unref(m_match);
    if (m_ownerMatch)
        sd_bus_slot_unref(m_ownerMatch);
}

bool CKeysClient::init() {
    if (!m_bus)
        return false;
    const int result = sd_bus_match_signal(m_bus, &m_match, hud::kBusName, hud::kObjPath,
                                           hud::kIface, "PanelDismissed", onPanelDismissed, this);
    if (result < 0) {
        Log::log(Log::ERR, "[keys] cannot subscribe to panel lifecycle: {}", std::strerror(-result));
        return false;
    }
    const int ownerResult = sd_bus_match_signal(m_bus, &m_ownerMatch, "org.freedesktop.DBus",
                                                "/org/freedesktop/DBus", "org.freedesktop.DBus",
                                                "NameOwnerChanged", onNameOwnerChanged, this);
    if (ownerResult < 0) {
        Log::log(Log::ERR, "[keys] cannot watch HUD ownership: {}", std::strerror(-ownerResult));
        return false;
    }
    return true;
}

bool CKeysClient::showPrivacyIndicator() {
    return create({privacyLine()}, m_config.riseMs, -1);
}

bool CKeysClient::sync(const std::vector<hud::SLine>& lines) {
    if (lines.empty())
        return true;
    if (!process())
        return false;
    std::vector<hud::SLine> visible;
    visible.reserve(lines.size() + 1);
    visible.push_back(privacyLine());
    visible.insert(visible.end(), lines.begin(), lines.end());
    if (m_panelId == 0)
        return create(visible, 0, -1);
    return update(visible, 0, -1);
}

hud::SLine CKeysClient::privacyLine() const {
    const std::string mode = m_config.modsOnly ? "KEY DISPLAY ON  -  SHORTCUTS ONLY" : "KEY DISPLAY ON  -  ALL KEYS";
    return {mode, hud::EColor::Warn, false};
}

bool CKeysClient::create(const std::vector<hud::SLine>& lines, int riseMs, int holdMs) {
    sd_bus_message* call = nullptr;
    int result = sd_bus_message_new_method_call(m_bus, &call, hud::kBusName, hud::kObjPath,
                                                hud::kIface, "CreatePanel");
    if (result < 0)
        return false;
    appendProps(call, m_config, lines, riseMs, holdMs, true);

    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    result = sd_bus_call(m_bus, call, 0, &error, &reply);
    sd_bus_message_unref(call);
    if (result < 0) {
        Log::log(Log::WARN, "[keys] HUD panel create failed: {}",
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
    Log::log(Log::INFO, "[keys] HUD panel active (id {})", id);
    return true;
}

bool CKeysClient::update(const std::vector<hud::SLine>& lines, int riseMs, int holdMs) {
    sd_bus_message* call = nullptr;
    int result = sd_bus_message_new_method_call(m_bus, &call, hud::kBusName, hud::kObjPath,
                                                hud::kIface, "UpdatePanel");
    if (result < 0)
        return false;
    sd_bus_message_append(call, "u", m_panelId);
    appendProps(call, m_config, lines, riseMs, holdMs, false);
    // Keep this acknowledged so transport failures are visible. PanelDismissed is drained
    // before each sync; once observed, the next event takes the CreatePanel path.
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    result = sd_bus_call(m_bus, call, 0, &error, &reply);
    sd_bus_message_unref(call);
    if (reply) sd_bus_message_unref(reply);
    if (result < 0)
        Log::log(Log::WARN, "[keys] HUD panel update failed: {}",
                 error.message ? error.message : std::strerror(-result));
    sd_bus_error_free(&error);
    return result >= 0;
}

void CKeysClient::dismiss() {
    if (!m_bus || m_panelId == 0)
        return;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    sd_bus_call_method(m_bus, hud::kBusName, hud::kObjPath, hud::kIface, "DismissPanel",
                       &error, &reply, "u", m_panelId);
    if (reply) sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    m_panelId = 0;
}

int CKeysClient::onPanelDismissed(sd_bus_message* message, void* userdata, sd_bus_error*) {
    auto* self = static_cast<CKeysClient*>(userdata);
    uint32_t id = 0;
    const char* reason = nullptr;
    if (sd_bus_message_read(message, "us", &id, &reason) >= 0 && id == self->m_panelId) {
        self->m_panelId = 0;
        Log::log(Log::DEBUG, "[keys] HUD panel lifecycle ended");
    }
    return 0;
}

int CKeysClient::onNameOwnerChanged(sd_bus_message* message, void* userdata, sd_bus_error*) {
    auto* self = static_cast<CKeysClient*>(userdata);
    const char *name = nullptr, *oldOwner = nullptr, *newOwner = nullptr;
    if (sd_bus_message_read(message, "sss", &name, &oldOwner, &newOwner) < 0 || !name)
        return 0;
    if (std::strcmp(name, hud::kBusName) == 0 && oldOwner && *oldOwner &&
        (!newOwner || std::strcmp(oldOwner, newOwner) != 0)) {
        self->m_panelId = 0;
        self->m_ownerLost = true;
        Log::log(Log::WARN, "[keys] HUD owner disappeared; capture must stop");
    }
    return 0;
}

bool CKeysClient::process() {
    if (!m_bus || !healthy())
        return false;
    int result = 0;
    while ((result = sd_bus_process(m_bus, nullptr)) > 0) { /* drain */ }
    if (result < 0) {
        m_processFailed = true;
        Log::log(Log::WARN, "[keys] HUD bus processing failed; capture must stop");
    }
    return healthy();
}

bool CKeysClient::runtimeLive() const {
    if (!m_bus || !healthy())
        return false;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    int rendering = 0;
    const int result = sd_bus_get_property_trivial(m_bus, hud::kBusName, hud::kObjPath,
                                                   hud::kIface, "Rendering", &error, 'b', &rendering);
    const bool live = result >= 0 && rendering != 0;
    sd_bus_error_free(&error);
    return live;
}

std::optional<hud::SPresentationSnapshot> CKeysClient::presentation(uint32_t panelId) const {
    if (!m_bus || !healthy() || panelId == 0)
        return std::nullopt;
    sd_bus_error error = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    const int result = sd_bus_call_method(m_bus, hud::kBusName, hud::kObjPath, hud::kIface,
                                          "GetPanelPresentation", &error, &reply, "u", panelId);
    uint64_t panelSerial = 0, frameSerial = 0, streakStart = 0;
    const int readResult = result >= 0 ? sd_bus_message_read(reply, "ttt", &panelSerial, &frameSerial, &streakStart) : result;
    if (reply) sd_bus_message_unref(reply);
    sd_bus_error_free(&error);
    if (readResult < 0)
        return std::nullopt;
    return hud::SPresentationSnapshot{panelSerial, frameSerial, streakStart};
}

int CKeysClient::fd() const {
    return m_bus ? sd_bus_get_fd(m_bus) : -1;
}

int CKeysClient::events() const {
    if (!m_bus)
        return 0;
    const int result = sd_bus_get_events(m_bus);
    return result < 0 ? POLLIN : result;
}

int CKeysClient::timeoutMs() const {
    if (!m_bus)
        return -1;
    uint64_t usec = 0;
    if (sd_bus_get_timeout(m_bus, &usec) < 0 || usec == UINT64_MAX)
        return -1;
    return static_cast<int>(usec / 1000);
}

} // namespace hudkeys
