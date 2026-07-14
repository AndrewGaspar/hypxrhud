#pragma once

#include <string>

// hypxrhud — the Omarchy theme live-reload watch (WP-H6, daemon-only). Omarchy switches
// themes by atomically flipping the `~/.config/omarchy/current/theme` SYMLINK to point at
// the new theme dir (omarchy-theme-set). We can't watch the symlink target directly (it
// changes), so — per the memo's triage decision — we watch the symlink's PARENT directory
// with inotify and react when the `theme` entry is (re)created / moved / deleted.
//
// The inotify fd folds into the daemon's single poll() loop (no threads, so the EGL context
// stays current — Monado's fence contract). On a detected flip the daemon re-resolves the
// palette and force-dirties every panel so they re-raster in the new colours. If inotify or
// the Omarchy dir is unavailable, init() returns false and the daemon simply runs without
// live theme reload (a static palette resolved once at start).

namespace hud {

class CThemeWatch {
  public:
    CThemeWatch() = default;
    ~CThemeWatch();

    // Arm an inotify watch on the parent of ~/.config/omarchy/current/theme. Returns false
    // (and leaves fd() == -1) if the environment/dir is absent or inotify is unavailable.
    bool init();

    int  fd() const { return m_fd; } // for the poll set, or -1.

    // Consume all pending inotify events. Returns true if the watched `theme` entry changed
    // (a theme switch) — the daemon should then reload the palette. Never blocks.
    bool drain();

    void shutdown();

  private:
    int         m_fd    = -1;
    int         m_wd    = -1;
    std::string m_entry;      // the symlink's basename ("theme") we filter events on.
};

} // namespace hud
