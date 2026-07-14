#include "BatteryClient.hpp"

#include "Dbus.hpp" // hud::kBusName / kObjPath / kIface
#include "Log.hpp"

#include <cstring>
#include <systemd/sd-bus.h>

namespace hudbat {

namespace {
    // Append a {sv} entry whose value is one gauges array a(sdb) = [(label, percent, charging)].
    void appendGauges(sd_bus_message* m, const std::vector<hud::SGauge>& gauges) {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", "gauges");
        sd_bus_message_open_container(m, 'v', "a(sdb)");
        sd_bus_message_open_container(m, 'a', "(sdb)");
        for (const auto& g : gauges)
            sd_bus_message_append(m, "(sdb)", g.label.c_str(), (double)g.percent, g.charging ? 1 : 0);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
        sd_bus_message_close_container(m);
    }
    void appendStr(sd_bus_message* m, const char* key, const char* val) {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", key);
        sd_bus_message_append(m, "v", "s", val);
        sd_bus_message_close_container(m);
    }
    void appendI(sd_bus_message* m, const char* key, int32_t val) {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", key);
        sd_bus_message_append(m, "v", "i", val);
        sd_bus_message_close_container(m);
    }
    void appendU(sd_bus_message* m, const char* key, uint32_t val) {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", key);
        sd_bus_message_append(m, "v", "u", val);
        sd_bus_message_close_container(m);
    }
}

bool CBatteryClient::createPanel(const std::vector<hud::SGauge>& gauges) {
    sd_bus_message* m = nullptr;
    sd_bus_message_new_method_call(m_bus, &m, hud::kBusName, hud::kObjPath, hud::kIface, "CreatePanel");
    sd_bus_message_open_container(m, 'a', "{sv}");
    appendStr(m, "slot", m_slot.c_str());
    appendStr(m, "kind", "gauges");
    appendI(m, "hold_ms", -1); // persist until updated/dismissed (status panel).
    appendGauges(m, gauges);
    sd_bus_message_close_container(m);

    sd_bus_error   err   = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int r = sd_bus_call(m_bus, m, 0, &err, &reply);
    if (r < 0) {
        Log::log(Log::DEBUG, "[client] CreatePanel failed ({}) — daemon not ready?",
                 err.message ? err.message : strerror(-r));
        sd_bus_error_free(&err);
        sd_bus_message_unref(m);
        return false;
    }
    uint32_t id = 0;
    sd_bus_message_read(reply, "u", &id);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    sd_bus_message_unref(m);
    m_panelId = id;
    Log::log(Log::INFO, "[client] battery panel created (id {}, {} gauge(s))", id, gauges.size());
    return id != 0;
}

bool CBatteryClient::updatePanel(const std::vector<hud::SGauge>& gauges) {
    sd_bus_message* m = nullptr;
    sd_bus_message_new_method_call(m_bus, &m, hud::kBusName, hud::kObjPath, hud::kIface, "UpdatePanel");
    sd_bus_message_append(m, "u", m_panelId);
    sd_bus_message_open_container(m, 'a', "{sv}");
    appendStr(m, "kind", "gauges");
    appendI(m, "hold_ms", -1);
    appendGauges(m, gauges);
    sd_bus_message_close_container(m);

    // Fire-and-forget: the daemon drains it from the bus fd next tick.
    sd_bus_message_set_expect_reply(m, 0);
    int r = sd_bus_send(m_bus, m, nullptr);
    sd_bus_message_unref(m);
    if (r < 0) {
        Log::log(Log::WARN, "[client] UpdatePanel send failed: {}", strerror(-r));
        return false;
    }
    return true;
}

void CBatteryClient::dismissPanel() {
    if (m_panelId == 0)
        return;
    sd_bus_error   err = SD_BUS_ERROR_NULL;
    sd_bus_message* rep = nullptr;
    sd_bus_call_method(m_bus, hud::kBusName, hud::kObjPath, hud::kIface, "DismissPanel", &err, &rep, "u", m_panelId);
    if (rep) sd_bus_message_unref(rep);
    sd_bus_error_free(&err);
    Log::log(Log::INFO, "[client] battery panel dismissed (id {}) — all sources absent", m_panelId);
    m_panelId = 0;
}

bool CBatteryClient::syncPanel(const std::vector<hud::SGauge>& gauges) {
    if (gauges.empty()) {
        dismissPanel();
        return true;
    }
    if (m_panelId == 0)
        return createPanel(gauges);
    return updatePanel(gauges);
}

void CBatteryClient::postToast(const std::string& text) {
    sd_bus_message* m = nullptr;
    sd_bus_message_new_method_call(m_bus, &m, hud::kBusName, hud::kObjPath, hud::kIface, "CreatePanel");
    sd_bus_message_open_container(m, 'a', "{sv}");
    appendStr(m, "slot", "toast");
    appendU(m, "urgency", 2); // critical.
    // lines: a(sub) = [(text, colorRole=4 Warn, big=0)]
    sd_bus_message_open_container(m, 'e', "sv");
    sd_bus_message_append(m, "s", "lines");
    sd_bus_message_open_container(m, 'v', "a(sub)");
    sd_bus_message_open_container(m, 'a', "(sub)");
    sd_bus_message_append(m, "(sub)", text.c_str(), (uint32_t)4, (int)0);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);
    sd_bus_message_close_container(m);

    sd_bus_message_set_expect_reply(m, 0); // fire-and-forget; the toast auto-expires.
    int r = sd_bus_send(m_bus, m, nullptr);
    sd_bus_message_unref(m);
    if (r < 0)
        Log::log(Log::WARN, "[client] toast send failed: {}", strerror(-r));
    else
        Log::log(Log::INFO, "[client] low-battery toast: {}", text);
}

} // namespace hudbat
