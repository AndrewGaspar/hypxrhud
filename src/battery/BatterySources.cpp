#include "BatterySources.hpp"

#include "Log.hpp"

#include <cstring>
#include <systemd/sd-bus.h>

namespace hudbat {

namespace {
    // sd_bus_get_property_trivial wrappers that return false on any error (service absent,
    // unknown object/property, wrong type) and never throw.
    bool getD(sd_bus* bus, const char* dst, const char* path, const char* iface,
              const char* prop, double& out) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        int r = sd_bus_get_property_trivial(bus, dst, path, iface, prop, &err, 'd', &out);
        sd_bus_error_free(&err);
        return r >= 0;
    }
    bool getU(sd_bus* bus, const char* dst, const char* path, const char* iface,
              const char* prop, uint32_t& out) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        int r = sd_bus_get_property_trivial(bus, dst, path, iface, prop, &err, 'u', &out);
        sd_bus_error_free(&err);
        return r >= 0;
    }
    bool getB(sd_bus* bus, const char* dst, const char* path, const char* iface,
              const char* prop, bool& out) {
        sd_bus_error err = SD_BUS_ERROR_NULL;
        int          v   = 0;
        int r = sd_bus_get_property_trivial(bus, dst, path, iface, prop, &err, 'b', &v);
        sd_bus_error_free(&err);
        if (r < 0)
            return false;
        out = v != 0;
        return true;
    }
}

SSourceReading readUpower(sd_bus* system) {
    SSourceReading r;
    if (!system)
        return r;

    uint32_t type = 0;
    bool     isPresent = false;
    // If we cannot even read Type/IsPresent, UPower is absent or has no DisplayDevice.
    if (!getU(system, kUPowerBus, kUPowerPath, kUPowerDev, "Type", type))
        return r;
    getB(system, kUPowerBus, kUPowerPath, kUPowerDev, "IsPresent", isPresent);
    if (!upowerIsLaptopBattery(type, isPresent))
        return r; // desktop / no battery -> omit the laptop gauge.

    double   pct   = -1.0;
    uint32_t state = 0;
    getD(system, kUPowerBus, kUPowerPath, kUPowerDev, "Percentage", pct);
    getU(system, kUPowerBus, kUPowerPath, kUPowerDev, "State", state);

    r.present  = true;
    r.percent  = (float)pct;
    r.charging = upowerCharging(state);
    return r;
}

bool wivrnHeadsetConnected(sd_bus* session) {
    bool connected = false;
    if (!session)
        return false;
    getB(session, kWivrnBus, kWivrnPath, kWivrnIface, "HeadsetConnected", connected);
    return connected;
}

SSourceReading readWivrn(sd_bus* session) {
    SSourceReading r;
    if (!session)
        return r;

    // A headset must be connected for any battery to be meaningful.
    bool connected = false;
    if (!getB(session, kWivrnBus, kWivrnPath, kWivrnIface, "HeadsetConnected", connected))
        return r; // WiVRn not running -> headset absent.
    if (!connected)
        return r; // no headset -> omit the gauge.

    // Forward-compatible `Battery` property probe (the documented seam). We call the raw
    // org.freedesktop.DBus.Properties.Get (reply signature "v") rather than
    // sd_bus_get_property() so we can inspect the variant's inner signature ourselves and
    // accept either `(bbd)` or a bare `d` without a type mismatch.
    sd_bus_error   err   = SD_BUS_ERROR_NULL;
    sd_bus_message* reply = nullptr;
    int rr = sd_bus_call_method(session, kWivrnBus, kWivrnPath, "org.freedesktop.DBus.Properties",
                                "Get", &err, &reply, "ss", kWivrnIface, "Battery");
    if (rr < 0 || !reply) {
        // No Battery property in this WiVRn (v26.6.1) -> headset connected but charge is
        // not externally readable. Omit the gauge (see docs/battery-wivrn.md).
        Log::log(Log::DEBUG, "[wivrn] headset connected but no Battery property ({}) — gauge omitted",
                 err.message ? err.message : "unavailable");
        sd_bus_error_free(&err);
        if (reply) sd_bus_message_unref(reply);
        return r;
    }

    char        vtype    = 0;
    const char* contents = nullptr;
    bool        present  = true;
    bool        charging = false;
    double      charge   = -1.0;
    if (sd_bus_message_peek_type(reply, &vtype, &contents) > 0 && vtype == 'v' && contents) {
        if (std::strcmp(contents, "(bbd)") == 0) {
            int p = 1, c = 0;
            if (sd_bus_message_enter_container(reply, 'v', "(bbd)") >= 0) {
                sd_bus_message_read(reply, "(bbd)", &p, &c, &charge);
                sd_bus_message_exit_container(reply);
                present = p != 0;
                charging = c != 0;
            }
        } else if (std::strcmp(contents, "d") == 0) {
            if (sd_bus_message_enter_container(reply, 'v', "d") >= 0) {
                sd_bus_message_read(reply, "d", &charge);
                sd_bus_message_exit_container(reply);
            }
        }
    }
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);

    if (!present || charge < 0.0)
        return r; // property present but reports no battery -> omit.

    r.present  = true;
    r.percent  = wivrnChargeToPercent(charge);
    r.charging = charging;
    return r;
}

} // namespace hudbat
