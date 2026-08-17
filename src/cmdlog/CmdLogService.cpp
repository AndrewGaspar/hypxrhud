#include "CmdLogService.hpp"

#include "Log.hpp"

#include <cstring>
#include <systemd/sd-bus.h>

namespace hudcmd {

CCmdLogService::~CCmdLogService() {
    if (m_vtable)
        sd_bus_slot_unref(m_vtable);
    if (m_bus && m_haveName)
        sd_bus_release_name(m_bus, kCmdLogBusName);
}

bool CCmdLogService::init() {
    if (!m_bus)
        return false;

    static const sd_bus_vtable vtable[] = {
        SD_BUS_VTABLE_START(0),
        SD_BUS_METHOD_WITH_ARGS("Publish",
                                SD_BUS_ARGS("s", commandLine),
                                SD_BUS_NO_RESULT,
                                CCmdLogService::onPublish, SD_BUS_VTABLE_UNPRIVILEGED),
        SD_BUS_PROPERTY("Rows", "as", CCmdLogService::propRows, 0, 0),
        SD_BUS_VTABLE_END,
    };

    const int objectResult = sd_bus_add_object_vtable(m_bus, &m_vtable, kCmdLogObjPath,
                                                      kCmdLogIface, vtable, this);
    if (objectResult < 0) {
        Log::log(Log::ERR, "[cmdlog] cannot publish the ticker object: {}", std::strerror(-objectResult));
        return false;
    }
    const int nameResult = sd_bus_request_name(m_bus, kCmdLogBusName, 0);
    if (nameResult < 0) {
        Log::log(Log::ERR, "[cmdlog] cannot take {} ({}) — is another ticker already running?",
                 kCmdLogBusName, std::strerror(-nameResult));
        return false;
    }
    m_haveName = true;
    return true;
}

int CCmdLogService::onPublish(sd_bus_message* message, void* userdata, sd_bus_error*) {
    auto*       self    = static_cast<CCmdLogService*>(userdata);
    const char* command = nullptr;
    if (sd_bus_message_read(message, "s", &command) < 0 || !command) {
        // Malformed input from an untrusted caller is ignored, never fatal.
        return sd_bus_reply_method_return(message, "");
    }
    std::string line(command);
    if (line.size() > kMaxPublishBytes)
        line.resize(kMaxPublishBytes);
    if (self->m_onPublish)
        self->m_onPublish(line);
    return sd_bus_reply_method_return(message, "");
}

int CCmdLogService::propRows(sd_bus*, const char*, const char*, const char*,
                             sd_bus_message* reply, void* userdata, sd_bus_error*) {
    auto* self = static_cast<CCmdLogService*>(userdata);
    int   result = sd_bus_message_open_container(reply, 'a', "s");
    if (result < 0)
        return result;
    if (self->m_rows) {
        for (const auto& row : self->m_rows()) {
            result = sd_bus_message_append(reply, "s", row.c_str());
            if (result < 0)
                return result;
        }
    }
    return sd_bus_message_close_container(reply);
}

} // namespace hudcmd
