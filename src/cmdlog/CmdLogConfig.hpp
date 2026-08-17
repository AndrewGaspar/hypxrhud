#pragma once

#include <string>
#include <vector>

// hypxrhud-cmdlog — the command ticker's config (the same deliberately thin TOML subset the
// daemon/battery/keys clients use: one struct, one section, quoted strings, no dependency).
// Config path: $XDG_CONFIG_HOME/hypxrhud/cmd.toml (see examples/cmd.toml).

namespace hudcmd {

struct SCmdLogConfig {
    // Which HUD slot carries the ticker. `status` (bottom-right) keeps it clear of the
    // voice/keys slots a filmed session usually also has running.
    std::string slot = "status";
    // How many commands stay on screen at once (newest first).
    int history = 3;
    // How long a row lives after it was last published, ms.
    int ttlMs = 6000;
    // Display width, in codepoints. Longer commands keep their HEAD and get an ellipsis.
    int maxChars = 60;
    // A repeat of the newest command inside this window bumps its "xN" count instead of
    // pushing a duplicate row (0 disables coalescing).
    int coalesceMs = 1500;
    // Panel envelope. The panel persists (hold = -1) and is dismissed when the last row
    // expires, so only rise/fade matter here.
    int    riseMs  = 90;
    int    fadeMs  = 300;
    double opacity = 0.92;
};

// Parse the `[cmd]` section out of `text`. Unknown sections/keys warn and are ignored
// (forward-compatible); malformed values and out-of-range settings are errors.
bool parseCmdLogConfig(const std::string& text, SCmdLogConfig& out,
                       std::vector<std::string>& errors, std::vector<std::string>& warnings);

// Load `path`. `allowMissing` (the implicit default path) turns a missing file into a
// warning + compiled defaults; an explicitly requested missing/unreadable file fails.
bool loadCmdLogConfigFile(const std::string& path, SCmdLogConfig& out,
                          std::vector<std::string>& errors, std::vector<std::string>& warnings,
                          bool allowMissing);

std::string defaultCmdLogConfigPath();

} // namespace hudcmd
