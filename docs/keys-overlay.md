# Keystroke overlay (`hypxrhud-keys`)

`hypxrhud-keys` is the first-party ShowMeTheKey producer for hypxrhud's existing
head-locked `keys` slot. It is intended for deliberate, short recording sessions—not as a
login service.

## Privacy and privilege contract

- Capture does not begin until hypxrhud reports a live XR session and the exact `KEY DISPLAY
  ON` panel ID is included in successful `shouldRender`/`xrEndFrame` layer submissions for
  the panel's rise time plus a render-frame margin. Bus acceptance, a queued panel, a
  layer-budget omission, or a zero-layer frame does not satisfy the gate.
- The installed systemd user unit is static/manual: it has no `[Install]` section and cannot
  be enabled through the normal `systemctl enable` flow.
- The producer executes only the fixed `/usr/bin/showmethekey-cli` backend. There is no shell,
  configurable command, automatic `pkexec`, or broad process-name termination.
- Stop tracks and terminates the exact child PID; parent death also signals the child.
- Loss of the HUD D-Bus owner, bus processing, or live runtime is capture-fatal: the producer
  exits and requires a deliberate restart/disclosure instead of silently reconnecting.
- The disclosure remains as a persistent first row in the exact `keys` panel throughout
  capture. Every successful frame must keep that ID in its submitted layer list; any
  omission breaks the continuous presentation streak and stops the input child.
- Raw JSON and key text are never written to application logs. `--verbose` adds lifecycle
  diagnostics only.
- `mods_only = true` (or `--mods-only`) prevents ordinary typed characters from entering HUD
  history. Ctrl/Alt/Super shortcuts and named control/navigation keys remain visible.

ShowMeTheKey needs permission to read keyboard input devices. On this machine the packaged
backend can run directly for the interactive account, so the client intentionally has no
privilege-escalation fallback. If direct execution fails, fix the account/device permission
policy explicitly; do not make the HUD client passwordlessly elevate an arbitrary command.

## Build and review

Requirements add `xkbcommon` to the daemon's existing C++/jansson/libsystemd dependencies.
The tests use fixture JSON and a private D-Bus session only; they never open an input device.

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

Review `examples/keys.toml`, especially the XKB rules/model/layout/variant/options. Keycodes
from ShowMeTheKey/libinput are evdev codes; the translator applies xkbcommon's required `+8`
offset before resolving symbols. Modifier labels have stable `Super`, `Ctrl`, `Alt`, `Shift`
ordering regardless of press order. Held-key repeats are suppressed, consecutive identical
presses are coalesced, and history is bounded to at most eight rows.

An absent implicit default config uses compiled defaults. An explicitly requested missing
config, a non-regular path, an unreadable file, or an I/O error fails closed; it never silently
falls back from an intended `mods_only = true` policy to all-key capture.

## Manual demo run

Do this only when you are ready to display keys:

```sh
# Safest shortcut-only mode:
build/hypxrhud-keys --mods-only --verbose

# Or, after installation, start/stop the static unit explicitly:
systemctl --user start hypxrhud-keys.service
systemctl --user stop hypxrhud-keys.service
```

The producer listens for `PanelDismissed`; its D-Bus adapter can recreate a stale ID, while
the privacy-sensitive main process treats a dismissal/preemption during active capture as
fatal and requires a deliberate restart. Process shutdown dismisses the persistent panel
before closing the bus connection.

## Test boundaries

- `test_keys.cpp`: strict schema, malformed/duplicate JSON, evdev-to-XKB translation,
  modifier order, privacy filtering, repeat/coalescing/history behavior, config validation,
  fragmented nonblocking fixture reads, and scoped child termination.
- `keys_integration.cpp`: real producer D-Bus adapter against a real `hypxrhud --no-xr` daemon
  on `dbus-run-session`; verifies that runtime-absent cannot pass the capture gate,
  `PanelDismissed`/recreate behavior, and capture-fatal HUD owner loss.
- `test_presentation.cpp`: pure successful/`shouldRender=false` frame tracking plus exact
  acknowledgement behavior for submitted, queued, and layer-budget-dropped panels.

No automated test or install step starts `/usr/bin/showmethekey-cli`.
