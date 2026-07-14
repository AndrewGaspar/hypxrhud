#include "ThemeWatch.hpp"

#include "Log.hpp"
#include "Theme.hpp"

#include <cerrno>
#include <climits>
#include <cstring>
#include <sys/inotify.h>
#include <unistd.h>

namespace hud {

CThemeWatch::~CThemeWatch() {
    shutdown();
}

bool CThemeWatch::init() {
    const std::string link = omarchyThemeDir(); // .../omarchy/current/theme
    if (link.empty())
        return false;

    // Split into parent dir + entry name.
    size_t slash = link.find_last_of('/');
    if (slash == std::string::npos || slash + 1 >= link.size())
        return false;
    std::string parent = link.substr(0, slash);
    m_entry            = link.substr(slash + 1);

    m_fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (m_fd < 0) {
        Log::log(Log::DEBUG, "[theme] inotify_init failed: {} (live reload disabled)", strerror(errno));
        m_fd = -1;
        return false;
    }
    // The symlink flip is a create/move/delete of the `theme` entry in `parent`.
    m_wd = inotify_add_watch(m_fd, parent.c_str(),
                             IN_CREATE | IN_MOVED_TO | IN_MOVED_FROM | IN_DELETE);
    if (m_wd < 0) {
        Log::log(Log::DEBUG, "[theme] watch on '{}' failed: {} (live reload disabled)", parent,
                 strerror(errno));
        close(m_fd);
        m_fd = -1;
        return false;
    }
    Log::log(Log::INFO, "[theme] watching '{}' for theme switches", parent);
    return true;
}

bool CThemeWatch::drain() {
    if (m_fd < 0)
        return false;
    bool changed = false;
    // inotify_event is variable-length; size the buffer for several events.
    alignas(struct inotify_event) char buf[4096];
    for (;;) {
        ssize_t n = read(m_fd, buf, sizeof(buf));
        if (n <= 0)
            break; // EAGAIN (drained) or error.
        for (char* p = buf; p < buf + n;) {
            auto* ev = reinterpret_cast<struct inotify_event*>(p);
            if (ev->len > 0 && m_entry == ev->name)
                changed = true;
            p += sizeof(struct inotify_event) + ev->len;
        }
    }
    return changed;
}

void CThemeWatch::shutdown() {
    if (m_fd >= 0) {
        if (m_wd >= 0)
            inotify_rm_watch(m_fd, m_wd);
        close(m_fd);
    }
    m_fd = -1;
    m_wd = -1;
}

} // namespace hud
