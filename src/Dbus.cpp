#include "Dbus.hpp"

#include "Log.hpp"
#include "Props.hpp"

#include <chrono>
#include <cstring>
#include <poll.h>
#include <systemd/sd-bus.h>

namespace hud {

namespace {
    int64_t nowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    // colorRole (uint) <-> EColor. Order matches Panel.hpp's EColor so a client can send a
    // small integer role (design memo §2.2 `a(su)` = (text, colorRole)).
    EColor colorFromRole(uint32_t r) {
        switch (r) {
            case 1:  return EColor::Dim;
            case 2:  return EColor::Accent;
            case 3:  return EColor::Good;
            case 4:  return EColor::Warn;
            case 5:  return EColor::Bad;
            default: return EColor::Normal;
        }
    }

    // Read one `a(sub)` (or `a(su)`) into SLine list: (text, colorRole, [big]).
    bool readLines(sd_bus_message* m, const char* contents, std::vector<SLine>& out) {
        const bool hasBig = std::strcmp(contents, "a(sub)") == 0;
        const char* elem  = hasBig ? "(sub)" : "(su)";
        if (sd_bus_message_enter_container(m, 'a', elem) < 0)
            return false;
        while (sd_bus_message_enter_container(m, 'r', hasBig ? "sub" : "su") > 0) {
            const char* t = nullptr;
            uint32_t    c = 0;
            int         big = 0;
            if (hasBig)
                sd_bus_message_read(m, "sub", &t, &c, &big);
            else
                sd_bus_message_read(m, "su", &t, &c);
            out.push_back(SLine{t ? t : "", colorFromRole(c), big != 0});
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
        return true;
    }

    // Read one `a(sdb)` into SGauge list: (label, percent, charging).
    bool readGauges(sd_bus_message* m, std::vector<SGauge>& out) {
        if (sd_bus_message_enter_container(m, 'a', "(sdb)") < 0)
            return false;
        while (sd_bus_message_enter_container(m, 'r', "sdb") > 0) {
            const char* label = nullptr;
            double      pct    = -1.0;
            int         charging = 0;
            sd_bus_message_read(m, "sdb", &label, &pct, &charging);
            out.push_back(SGauge{label ? label : "", (float)pct, charging != 0});
            sd_bus_message_exit_container(m);
        }
        sd_bus_message_exit_container(m);
        return true;
    }

    // Read the current variant (already positioned at a 'v') into an SPropValue by its
    // inner signature `contents`. Returns false on an unhandled signature (skipped).
    bool readVariant(sd_bus_message* m, const char* contents, SPropValue& out) {
        if (sd_bus_message_enter_container(m, 'v', contents) < 0)
            return false;
        bool ok = true;
        if (std::strcmp(contents, "s") == 0) {
            const char* s = nullptr;
            sd_bus_message_read(m, "s", &s);
            out = SPropValue::str(s ? s : "");
        } else if (std::strcmp(contents, "d") == 0) {
            double d = 0;
            sd_bus_message_read(m, "d", &d);
            out = SPropValue::dbl(d);
        } else if (std::strcmp(contents, "i") == 0) {
            int32_t v = 0;
            sd_bus_message_read(m, "i", &v);
            out = SPropValue::integer(v);
        } else if (std::strcmp(contents, "u") == 0) {
            uint32_t v = 0;
            sd_bus_message_read(m, "u", &v);
            out = SPropValue::integer(v);
        } else if (std::strcmp(contents, "x") == 0) {
            int64_t v = 0;
            sd_bus_message_read(m, "x", &v);
            out = SPropValue::integer(v);
        } else if (std::strcmp(contents, "t") == 0) {
            uint64_t v = 0;
            sd_bus_message_read(m, "t", &v);
            out = SPropValue::integer((int64_t)v);
        } else if (std::strcmp(contents, "y") == 0) {
            uint8_t v = 0;
            sd_bus_message_read(m, "y", &v);
            out = SPropValue::integer(v);
        } else if (std::strcmp(contents, "b") == 0) {
            int v = 0;
            sd_bus_message_read(m, "b", &v);
            out = SPropValue::boolean(v != 0);
        } else if (std::strcmp(contents, "(ddd)") == 0) {
            double x = 0, y = 0, z = 0;
            sd_bus_message_read(m, "(ddd)", &x, &y, &z);
            out = SPropValue::vec3((float)x, (float)y, (float)z);
        } else if (std::strcmp(contents, "a(sub)") == 0 || std::strcmp(contents, "a(su)") == 0) {
            std::vector<SLine> lines;
            readLines(m, contents, lines);
            out = SPropValue::lineList(std::move(lines));
        } else if (std::strcmp(contents, "a(sdb)") == 0) {
            std::vector<SGauge> gauges;
            readGauges(m, gauges);
            out = SPropValue::gaugeList(std::move(gauges));
        } else {
            // Unhandled signature — skip its body so the parse can continue.
            sd_bus_message_skip(m, contents);
            ok = false;
        }
        sd_bus_message_exit_container(m); // variant
        return ok;
    }

    // Read a whole `a{sv}` (positioned at the array) into an SPropMap. Unknown-typed values
    // are skipped, not fatal (forward-compatible).
    bool readPropMap(sd_bus_message* m, SPropMap& out) {
        if (sd_bus_message_enter_container(m, 'a', "{sv}") < 0)
            return false;
        while (sd_bus_message_enter_container(m, 'e', "sv") > 0) {
            const char* key = nullptr;
            if (sd_bus_message_read(m, "s", &key) < 0 || !key) {
                sd_bus_message_exit_container(m);
                continue;
            }
            char        vtype    = 0;
            const char* contents = nullptr;
            if (sd_bus_message_peek_type(m, &vtype, &contents) > 0 && vtype == 'v' && contents) {
                SPropValue val;
                if (readVariant(m, contents, val))
                    out[key] = std::move(val);
            }
            sd_bus_message_exit_container(m); // dict entry
        }
        sd_bus_message_exit_container(m); // array
        return true;
    }

    // Append one `{sv}` entry with a string value to an open `a{sv}` array.
    void appendStr(sd_bus_message* m, const char* key, const char* val) {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", key);
        sd_bus_message_append(m, "v", "s", val);
        sd_bus_message_close_container(m);
    }
    void appendU(sd_bus_message* m, const char* key, uint32_t val) {
        sd_bus_message_open_container(m, 'e', "sv");
        sd_bus_message_append(m, "s", key);
        sd_bus_message_append(m, "v", "u", val);
        sd_bus_message_close_container(m);
    }
}

CBus::~CBus() {
    shutdown();
}

bool CBus::init(CScene& scene, const SConfig& cfg) {
    m_scene = &scene;
    m_cfg   = &cfg;

    int r = sd_bus_open_user(&m_bus);
    if (r < 0 || !m_bus) {
        Log::log(Log::ERR, "[dbus] sd_bus_open_user failed: {}", strerror(-r));
        m_bus = nullptr;
        return false;
    }

    static const sd_bus_vtable vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD_WITH_ARGS("CreatePanel",
                                SD_BUS_ARGS("a{sv}", props),
                                SD_BUS_RESULT("u", id),
                                CBus::onCreatePanel, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("UpdatePanel",
                                SD_BUS_ARGS("u", id, "a{sv}", props),
                                SD_BUS_NO_RESULT,
                                CBus::onUpdatePanel, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("DismissPanel",
                                SD_BUS_ARGS("u", id),
                                SD_BUS_NO_RESULT,
                                CBus::onDismissPanel, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_METHOD_WITH_ARGS("GetCapabilities",
                                SD_BUS_NO_ARGS,
                                SD_BUS_RESULT("a{sv}", caps),
                                CBus::onGetCapabilities, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_SIGNAL_WITH_NAMES("PanelDismissed", "us", SD_BUS_PARAM(id) SD_BUS_PARAM(reason), 0),
        SD_BUS_SIGNAL_WITH_NAMES("RuntimeStateChanged", "s", SD_BUS_PARAM(state), 0),
        SD_BUS_PROPERTY("RuntimeState", "s", CBus::propRuntimeState, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("RuntimeName", "s", CBus::propRuntimeName, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_PROPERTY("PanelCount", "u", CBus::propPanelCount, 0, 0),
        SD_BUS_PROPERTY("MaxPanels", "u", CBus::propMaxPanels, 0, SD_BUS_VTABLE_PROPERTY_EMITS_CHANGE),
        SD_BUS_VTABLE_END,
    };

    r = sd_bus_add_object_vtable(m_bus, &m_vtable, kObjPath, kIface, vtable, this);
    if (r < 0) {
        Log::log(Log::ERR, "[dbus] add_object_vtable failed: {}", strerror(-r));
        shutdown();
        return false;
    }

    // Auto-dismiss a client's panels when its unique name drops (§2.4).
    r = sd_bus_match_signal(m_bus, &m_nocSlot, "org.freedesktop.DBus", "/org/freedesktop/DBus",
                            "org.freedesktop.DBus", "NameOwnerChanged", CBus::onNameOwnerChanged, this);
    if (r < 0)
        Log::log(Log::WARN, "[dbus] NameOwnerChanged match failed: {} (client-gone auto-dismiss disabled)", strerror(-r));

    // Request the well-known name. SD_BUS_NAME_REPLACE is NOT set — if another instance
    // already owns it we fail rather than steal it.
    r = sd_bus_request_name(m_bus, kBusName, 0);
    if (r < 0) {
        Log::log(Log::ERR, "[dbus] request_name '{}' failed: {} (already running?)", kBusName, strerror(-r));
        shutdown();
        return false;
    }
    m_haveName = true;
    Log::log(Log::INFO, "[dbus] owning {} at {}", kBusName, kObjPath);
    return true;
}

void CBus::shutdown() {
    if (m_bus) {
        if (m_haveName)
            sd_bus_release_name(m_bus, kBusName);
        sd_bus_flush(m_bus);
    }
    if (m_nocSlot) { sd_bus_slot_unref(m_nocSlot); m_nocSlot = nullptr; }
    if (m_vtable)  { sd_bus_slot_unref(m_vtable);  m_vtable  = nullptr; }
    if (m_bus)     { sd_bus_flush_close_unref(m_bus); m_bus = nullptr; }
    m_haveName = false;
}

int CBus::fd() const { return m_bus ? sd_bus_get_fd(m_bus) : -1; }

int CBus::events() const {
    if (!m_bus)
        return 0;
    int e = sd_bus_get_events(m_bus);
    return e < 0 ? POLLIN : e;
}

int CBus::timeoutMs() const {
    if (!m_bus)
        return -1;
    uint64_t usec = 0;
    int      r    = sd_bus_get_timeout(m_bus, &usec);
    if (r < 0)
        return -1;
    if (usec == UINT64_MAX)
        return -1; // infinite — block on the fd.
    return (int)(usec / 1000);
}

void CBus::process() {
    if (!m_bus)
        return;
    while (sd_bus_process(m_bus, nullptr) > 0) { /* drain queued in + out */ }
}

// ---- daemon -> bus state --------------------------------------------------------------

void CBus::setRuntimeState(const std::string& state) {
    if (state == m_state)
        return;
    m_state = state;
    Log::log(Log::INFO, "[dbus] runtime state -> {}", m_state);
    if (m_bus && m_haveName) {
        sd_bus_emit_signal(m_bus, kObjPath, kIface, "RuntimeStateChanged", "s", m_state.c_str());
        sd_bus_emit_properties_changed(m_bus, kObjPath, kIface, "RuntimeState", nullptr);
    }
}

void CBus::setRuntimeInfo(const std::string& runtimeName, int64_t maxLayers, int budget) {
    const bool changed = runtimeName != m_runtimeName || maxLayers != m_maxLayers || budget != m_budget;
    m_runtimeName = runtimeName;
    m_maxLayers   = maxLayers;
    m_budget      = budget;
    if (changed && m_bus && m_haveName) {
        sd_bus_emit_properties_changed(m_bus, kObjPath, kIface, "RuntimeName", nullptr);
        sd_bus_emit_properties_changed(m_bus, kObjPath, kIface, "MaxPanels", nullptr);
    }
}

void CBus::emitOne(uint32_t id, const std::string& reason) {
    if (m_bus && m_haveName)
        sd_bus_emit_signal(m_bus, kObjPath, kIface, "PanelDismissed", "us", id, reason.c_str());
}

void CBus::emitDismissed(const std::vector<SDismissal>& dismissed) {
    for (const auto& d : dismissed)
        emitOne(d.id, d.reason);
}

// ---- method handlers ------------------------------------------------------------------

int CBus::onCreatePanel(sd_bus_message* m, void* userdata, sd_bus_error* err) {
    auto* self = static_cast<CBus*>(userdata);
    const char* sender = sd_bus_message_get_sender(m);
    SPropMap props;
    if (!readPropMap(m, props)) {
        sd_bus_error_set(err, "io.github.andrewgaspar.hypxrhud1.Error.BadProps", "malformed a{sv} props");
        return -EINVAL;
    }
    SUpsert u = upsertFromProps(0, sender ? sender : "", props, *self->m_cfg);

    std::vector<SDismissal> dismissed;
    uint32_t                id = self->m_scene->upsert(u, nowMs(), &dismissed);
    if (id == 0) {
        sd_bus_error_set(err, "io.github.andrewgaspar.hypxrhud1.Error.Rejected",
                         "panel refused: per-client cap reached or the slot is held by a higher-urgency panel");
        return -EPERM;
    }
    self->emitDismissed(dismissed); // any preemptions this create caused.
    sd_bus_reply_method_return(m, "u", id);
    return 1;
}

int CBus::onUpdatePanel(sd_bus_message* m, void* userdata, sd_bus_error* err) {
    auto*    self   = static_cast<CBus*>(userdata);
    const char* sender = sd_bus_message_get_sender(m);
    uint32_t id     = 0;
    if (sd_bus_message_read(m, "u", &id) < 0) {
        sd_bus_error_set(err, "io.github.andrewgaspar.hypxrhud1.Error.BadArgs", "expected (u id, a{sv})");
        return -EINVAL;
    }
    SPropMap props;
    readPropMap(m, props);

    // Ownership: only the creator may update an existing panel; an unknown id is a best-effort
    // create (lossy transport). A cross-client update is ignored (still ack the fire-and-forget).
    const SPanel* ex = self->m_scene->get(id);
    if (id != 0 && ex && sender && ex->owner != sender) {
        Log::log(Log::WARN, "[dbus] UpdatePanel {} from {} ignored (owned by {})", id, sender, ex->owner);
        sd_bus_reply_method_return(m, "");
        return 1;
    }
    const std::string owner = (id != 0 && ex) ? ex->owner : (sender ? sender : "");
    SUpsert           u     = upsertFromProps(id, owner, props, *self->m_cfg);

    std::vector<SDismissal> dismissed;
    self->m_scene->upsert(u, nowMs(), &dismissed);
    self->emitDismissed(dismissed);
    sd_bus_reply_method_return(m, ""); // no-op if the caller set NO_REPLY_EXPECTED.
    return 1;
}

int CBus::onDismissPanel(sd_bus_message* m, void* userdata, sd_bus_error* err) {
    auto*       self   = static_cast<CBus*>(userdata);
    const char* sender = sd_bus_message_get_sender(m);
    uint32_t    id     = 0;
    if (sd_bus_message_read(m, "u", &id) < 0) {
        sd_bus_error_set(err, "io.github.andrewgaspar.hypxrhud1.Error.BadArgs", "expected (u id)");
        return -EINVAL;
    }
    const SPanel* ex = self->m_scene->get(id);
    if (!ex) {
        sd_bus_error_set(err, "io.github.andrewgaspar.hypxrhud1.Error.UnknownPanel", "no such panel id");
        return -ENOENT;
    }
    if (sender && ex->owner != sender) {
        sd_bus_error_set(err, "io.github.andrewgaspar.hypxrhud1.Error.NotOwner", "panel is owned by another client");
        return -EPERM;
    }
    self->m_scene->dismiss(id, "client");
    self->emitOne(id, "client");
    sd_bus_reply_method_return(m, "");
    return 1;
}

int CBus::onGetCapabilities(sd_bus_message* m, void* userdata, sd_bus_error* /*err*/) {
    auto* self = static_cast<CBus*>(userdata);

    sd_bus_message* reply = nullptr;
    sd_bus_message_new_method_return(m, &reply);
    sd_bus_message_open_container(reply, 'a', "{sv}");

    appendStr(reply, "version", "1");
    appendU(reply, "maxLayerCount", (uint32_t)(self->m_maxLayers < 0 ? 0 : self->m_maxLayers));
    appendU(reply, "budget", (uint32_t)(self->m_budget < 0 ? 0 : self->m_budget));
    appendU(reply, "perClientCap", (uint32_t)(self->m_cfg ? self->m_cfg->perClientCap : 4));

    // slots (as)
    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append(reply, "s", "slots");
    sd_bus_message_open_container(reply, 'v', "as");
    sd_bus_message_open_container(reply, 'a', "s");
    for (const auto& s : self->m_scene->slots().all())
        sd_bus_message_append(reply, "s", s.name.c_str());
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);

    // spaces (as)
    sd_bus_message_open_container(reply, 'e', "sv");
    sd_bus_message_append(reply, "s", "spaces");
    sd_bus_message_open_container(reply, 'v', "as");
    sd_bus_message_open_container(reply, 'a', "s");
    sd_bus_message_append(reply, "s", "view");
    sd_bus_message_append(reply, "s", "local");
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);
    sd_bus_message_close_container(reply);

    appendStr(reply, "runtimeState", self->m_state.c_str());
    appendStr(reply, "runtimeName", self->m_runtimeName.c_str());

    sd_bus_message_close_container(reply); // a{sv}
    int r = sd_bus_send(nullptr, reply, nullptr);
    sd_bus_message_unref(reply);
    return r < 0 ? r : 1;
}

// ---- properties -----------------------------------------------------------------------

int CBus::propRuntimeState(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
                           void* userdata, sd_bus_error*) {
    auto* self = static_cast<CBus*>(userdata);
    return sd_bus_message_append(reply, "s", self->m_state.c_str());
}
int CBus::propRuntimeName(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
                          void* userdata, sd_bus_error*) {
    auto* self = static_cast<CBus*>(userdata);
    return sd_bus_message_append(reply, "s", self->m_runtimeName.c_str());
}
int CBus::propPanelCount(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
                         void* userdata, sd_bus_error*) {
    auto* self = static_cast<CBus*>(userdata);
    return sd_bus_message_append(reply, "u", (uint32_t)self->m_scene->panels().size());
}
int CBus::propMaxPanels(sd_bus*, const char*, const char*, const char*, sd_bus_message* reply,
                        void* userdata, sd_bus_error*) {
    auto* self = static_cast<CBus*>(userdata);
    return sd_bus_message_append(reply, "u", (uint32_t)(self->m_budget < 0 ? 0 : self->m_budget));
}

// ---- NameOwnerChanged: client-gone auto-dismiss (§2.4) --------------------------------

int CBus::onNameOwnerChanged(sd_bus_message* m, void* userdata, sd_bus_error*) {
    auto*       self     = static_cast<CBus*>(userdata);
    const char* name     = nullptr;
    const char* oldOwner = nullptr;
    const char* newOwner = nullptr;
    if (sd_bus_message_read(m, "sss", &name, &oldOwner, &newOwner) < 0)
        return 0;
    // A client dropped iff its (unique) name now has no owner.
    if (!name || !newOwner || newOwner[0] != '\0')
        return 0;
    if (name[0] != ':') // only unique names identify a client connection.
        return 0;
    if (self->m_scene->ownerCount(name) == 0)
        return 0;

    std::vector<SDismissal> dismissed;
    self->m_scene->dropOwner(name, &dismissed);
    Log::log(Log::INFO, "[dbus] client {} gone — auto-dismissed {} panel(s)", name, dismissed.size());
    self->emitDismissed(dismissed);
    return 0;
}

} // namespace hud
