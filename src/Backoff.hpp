#pragma once

#include <algorithm>
#include <cstdint>

// hypxrhud — the dormant re-probe backoff schedule (WP-H4). Vendored VERBATIM from
// HypXRland's OpenXR::xrReprobeBackoffMs (src/openxr/XRMonitorConfig.cpp) so the daemon's
// runtime re-probe grows exactly as the compositor's does: doubling from `baseMs`, clamped
// to `capMs`. With base=2000, cap=30000 -> 2s, 4s, 8s, 16s, 30s (cap), 30s...
//
// Two cadences (design memo §6.1), same as the compositor:
//   - RUNTIME wait (no runtime/server yet): GROW the backoff (this function) so a
//     permanently-absent runtime is cheap.
//   - HEADSET wait (runtime up, headset undonned): a gentle FIXED cadence (just `baseMs`),
//     so donning never feels laggy. The daemon picks which to use; this fn is the growth.
//
// Pure -> unit-tested directly (tests/test_backoff.cpp).

namespace hud {

// attempt is the 0-based count of consecutive failed probes.
inline int64_t reprobeBackoffMs(int attempt, int64_t baseMs, int64_t capMs) {
    if (baseMs <= 0)
        baseMs = 2000;
    if (capMs < baseMs)
        capMs = baseMs;
    int64_t v = baseMs;
    for (int i = 0; i < attempt && v < capMs; ++i)
        v = std::min<int64_t>(v * 2, capMs);
    return v;
}

// Defaults matching the compositor's re-probe (memo §6.1).
inline constexpr int64_t kReprobeBaseMs = 2000;
inline constexpr int64_t kReprobeCapMs  = 30000;

} // namespace hud
