#pragma once

// hypxrhud — the `--self-test` one-command health check (WP-H7). Spins a PRIVATE session
// bus (dbus-run-session, the same isolation the integration test uses — never the user's
// real bus), starts the daemon in runtime-absent mode (--no-xr), and drives a
// create / update / dismiss round-trip plus a capabilities check against it, then exits
// 0 (healthy) or nonzero (a clear failure line on stderr). Safe to run while a headset is
// in use: it touches no XR runtime and no shared bus. Documented in the README.

namespace hud {

// Run the self-test. Re-execs itself once under dbus-run-session for the private bus, then
// on the second entry spawns + drives the daemon. Returns the process exit code (0 = pass).
int runSelfTest();

} // namespace hud
