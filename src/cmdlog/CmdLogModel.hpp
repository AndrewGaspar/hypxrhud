#pragma once

#include "CmdLogConfig.hpp"
#include "Panel.hpp"

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

// hypxrhud-cmdlog — the PURE command-ticker model (no sd-bus, no XR). It holds the last N
// published command lines, ages each one out on its own TTL, and renders the visible rows
// newest-first for the HUD. Everything policy-ish (sanitising, truncation, coalescing,
// expiry) lives here so it unit-tests with the rest of the pure core, exactly like
// hudkeys::CKeysModel and hudbat's value/diff model.

namespace hudcmd {

// Fold control characters (a quoted newline in an argument) into spaces, collapse runs of
// whitespace, trim, and bound the absolute length. The HUD rasteriser draws one line per
// row; a raw command line from the shim must never smuggle layout into it.
std::string sanitizeCommand(const std::string& raw);

// Head-preserving truncation to `maxChars` CODEPOINTS (never splits a UTF-8 sequence).
// The head is what identifies a command, so the tail is what goes.
std::string truncateHead(const std::string& text, int maxChars);

class CCmdLogModel {
  public:
    explicit CCmdLogModel(SCmdLogConfig config) : m_config(std::move(config)) {}

    // Record a command line. Returns true when the visible rows changed (an empty or
    // all-control-character command is dropped and returns false).
    bool publish(const std::string& command, int64_t nowMs);

    // Drop rows whose TTL elapsed. Returns true when the visible rows changed.
    bool expire(int64_t nowMs);

    bool   empty() const { return m_entries.empty(); }
    size_t size() const { return m_entries.size(); }

    // The HUD panel body: newest first, newest big+Accent, older rows Dim.
    std::vector<hud::SLine> lines() const;
    // The same rows as plain strings (the `Rows` D-Bus property + tests).
    std::vector<std::string> rows() const;

    // Absolute ms at which the next row expires, or -1 when there is nothing to expire.
    int64_t nextExpiryMs() const;

  private:
    struct SEntry {
        std::string text;   // sanitised (untruncated) command line.
        int         count  = 1;
        int64_t     lastMs = 0;
    };

    SCmdLogConfig      m_config;
    std::deque<SEntry> m_entries; // newest first.
};

} // namespace hudcmd
