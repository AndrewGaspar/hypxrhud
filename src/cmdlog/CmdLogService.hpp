#pragma once

#include <functional>
#include <string>
#include <vector>

// hypxrhud-cmdlog — the tiny D-Bus endpoint the `hyprctl` shim pokes with one string.
//
// Why a producer daemon at all, rather than letting the shim call the HUD's CreatePanel
// directly: hypxrhud keys every panel to its creator's UNIQUE bus name and auto-dismisses
// that client's panels on disconnect ("client-gone"). A one-shot `busctl` would therefore
// have its panel torn down the instant it exited. A long-lived owner is what lets a
// command stay on screen — and it is also where the history/expiry/truncation model lives.
//
// The surface is deliberately one method and one read-only property:
//   Publish(s commandLine) -> ()   # designed for NO_REPLY_EXPECTED (the shim never waits)
//   property as Rows (read)        # the rows the HUD is currently showing, newest first

struct sd_bus;
struct sd_bus_slot;
struct sd_bus_message;
struct sd_bus_error;

namespace hudcmd {

inline constexpr const char* kCmdLogBusName = "io.github.andrewgaspar.hypxrhud.cmdlog";
inline constexpr const char* kCmdLogObjPath = "/io/github/andrewgaspar/hypxrhud/cmdlog";
inline constexpr const char* kCmdLogIface   = "io.github.andrewgaspar.hypxrhud.cmdlog1";

// Longest command line accepted off the bus; anything past this is truncated before it
// reaches the model (which bounds it again, and then truncates for display).
inline constexpr size_t kMaxPublishBytes = 8192;

class CCmdLogService {
  public:
    using FPublish = std::function<void(const std::string&)>;
    using FRows    = std::function<std::vector<std::string>()>;

    CCmdLogService(sd_bus* bus, FPublish onPublish, FRows rows)
        : m_bus(bus), m_onPublish(std::move(onPublish)), m_rows(std::move(rows)) {}
    ~CCmdLogService();

    CCmdLogService(const CCmdLogService&)            = delete;
    CCmdLogService& operator=(const CCmdLogService&) = delete;

    // Install the vtable and take the well-known name. Returns false when the name is
    // already owned (another ticker is running) or the bus rejects the object.
    bool init();

  private:
    static int onPublish(sd_bus_message*, void*, sd_bus_error*);
    static int propRows(sd_bus*, const char*, const char*, const char*, sd_bus_message*, void*, sd_bus_error*);

    sd_bus*      m_bus    = nullptr;
    sd_bus_slot* m_vtable = nullptr;
    bool         m_haveName = false;
    FPublish     m_onPublish;
    FRows        m_rows;
};

} // namespace hudcmd
