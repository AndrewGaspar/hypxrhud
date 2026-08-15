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
- `mods_only` is a display/model filter, not narrower device capture: ShowMeTheKey still reads
  the keyboard event stream before the producer discards ordinary text. The producer cannot
  identify password fields, the lock screen, or other sensitive surfaces. Stop it before
  typing secrets; all-key mode should be treated as visibly disclosed keylogging.

ShowMeTheKey needs permission to read keyboard input devices. The development account is a
member of the local `input` and `wheel` groups, so direct access is expected, but the direct
backend has **not yet been exercised by this client in a live session**. The client
intentionally has no privilege-escalation fallback. If direct execution fails, fix the
account/device permission policy explicitly; do not make the HUD client passwordlessly
elevate an arbitrary command. The upstream GTK frontend's `pkexec` path is not used here,
and the supplied service has `NoNewPrivileges=yes`.

## Why this backend, and what it does not provide

A normal Wayland toplevel receives keyboard events only while the compositor routes focus
to it; there is no general protocol for passively observing every key. The GlobalShortcuts
portal reports activations for shortcuts the application registers, not an arbitrary key
stream. The InputCapture portal is designed for compositor-triggered, exclusive remote-input
capture (currently activated through pointer barriers), not a passive ShowMeTheKey overlay.
The RemoteDesktop portal's keyboard methods inject events; they do not observe them.

ShowMeTheKey instead reads evdev through libinput and emits newline-delimited JSON. Its CLI
backend is deliberately separated from its GTK frontend because reading `/dev/input/event*`
requires device permission. `hypxrhud-keys` consumes only the CLI JSON and renders through
hypxrhud; it never launches ShowMeTheKey's GTK window.

One subtle but important libinput boundary: `libinput_udev_assign_seat()` filters and
enumerates devices for a seat. It does **not** ask logind for input-device file descriptors.
libinput calls the embedding application's `libinput_interface.open_restricted` callback for
every device path, and seat assignment can report success even when those opens fail. Thus a
future native backend would still need an explicit device-access broker (for example a
logind `TakeDevice` integration) or compositor cooperation; merely assigning `seat0` is not
a privilege solution.

Primary references:

- [ShowMeTheKey architecture and JSON format](https://github.com/AlynxZhou/showmethekey#project-structure)
- [libinput context and `open_restricted` API](https://wayland.freedesktop.org/libinput/doc/latest/api/group__base.html)
- [XDG GlobalShortcuts portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.GlobalShortcuts.html)
- [XDG InputCapture portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.InputCapture.html)
- [XDG RemoteDesktop portal](https://flatpak.github.io/xdg-desktop-portal/docs/doc-org.freedesktop.portal.RemoteDesktop.html)

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

The implicit config is `$XDG_CONFIG_HOME/hypxrhud/keys.toml`, falling back to
`~/.config/hypxrhud/keys.toml`. If that implicit file is absent, compiled defaults are used.
Command-line RMLVO options override the file, and `--mods-only` can only tighten the file's
privacy setting. There is no command-line switch that changes `mods_only = true` back to
false. The complete, commented key set and validation ranges live in `examples/keys.toml`.

## Manual demo run

Do this only when you are ready to display keys:

```sh
# Safest shortcut-only mode:
~/.local/bin/hypxrhud-keys --mods-only --verbose

# In a build tree, the equivalent is:
build/hypxrhud-keys --mods-only --verbose

# Or, after installation, start/stop the static unit explicitly:
systemctl --user start hypxrhud-keys.service
systemctl --user stop hypxrhud-keys.service
```

Prefer the direct command for the first attended film test: its lifecycle-only diagnostics
stay visible, and `--mods-only` overrides a less restrictive config. The installed unit uses
the config default and has no command-line privacy override; set `mods_only = true` in
`~/.config/hypxrhud/keys.toml` before using the unit for shortcut-only filming.

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

## User-local installation

```sh
cmake -S . -B build-user \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-user -j
ctest --test-dir build-user --output-on-failure
cmake --install build-user
```

CMake writes absolute executable paths into the generated D-Bus and systemd files. The
D-Bus activation file lands in `~/.local/share/dbus-1/services`, which is searched on the
development host. The generated units land in `~/.local/lib/systemd/user`; that directory
is **not** in this host's `systemd-analyze --user unit-paths` output. For this host, copy the
generated units into the searched, higher-precedence directory:

```sh
install -Dm644 build-user/services/hypxrhud.service \
  "$HOME/.config/systemd/user/hypxrhud.service"
install -Dm644 build-user/services/hypxrhud-battery.service \
  "$HOME/.config/systemd/user/hypxrhud-battery.service"
install -Dm644 build-user/services/hypxrhud-keys.service \
  "$HOME/.config/systemd/user/hypxrhud-keys.service"
systemctl --user daemon-reload
systemctl --user restart hypxrhud.service
```

Do not enable `hypxrhud-keys.service`: it is intentionally static. Confirm the files that
systemd actually resolved rather than assuming an install prefix won:

```sh
systemctl --user show hypxrhud.service hypxrhud-keys.service \
  -p FragmentPath -p UnitFileState -p ActiveState -p SubState -p ExecStart
systemctl --user cat hypxrhud.service
systemctl --user cat hypxrhud-keys.service
```

The user manager needs the same XR runtime selection as the interactive session. Importing
it with `systemctl --user import-environment XR_RUNTIME_JSON` affects the current user
manager only; it is not a persistent unit setting. Re-import or configure the session
environment after login as appropriate.

## Current development-host handoff (2026-08-14)

This is a point-in-time handoff record, not a claim that later builds have these hashes:

- Repository `master` is at `c9b1a78` (`feat: add privacy-safe keystroke HUD producer`).
- `~/.local/bin/hypxrhud` is SHA-256
  `ea15cba96383051d96f62be2c4d067a9fbb499cf070f9c6f824fb926a9140fd1`.
- `~/.local/bin/hypxrhud-keys` is SHA-256
  `7511867664afc125a4818a2dc3248aa7dbaf9772ea9087a6ba48160cc90e6c09`.
- `~/.local/bin/hypxrhud-battery` is SHA-256
  `dacbdc8ce967ac199ede0fb50dd837c51c1ac5ac3b046de514872bd04dfc55e8`.
- `~/.config/systemd/user/hypxrhud.service` wins unit precedence and starts the exact
  user-local daemon. It is enabled and was active at handoff.
- Its host-only `hypxrhud.service.d/no-nvidia.conf` drop-in pins Mesa EGL and the AMD Vulkan
  ICD so HUD probing does not wake the NVIDIA dGPU; this drop-in is not part of this repo.
- `~/.config/systemd/user/hypxrhud-battery.service` likewise wins unit precedence,
  starts the user-local battery producer, and was enabled and active at handoff.
- `~/.config/systemd/user/hypxrhud-keys.service` starts the exact user-local producer. It
  is static, inactive, and has never been started for live key capture.
- The current user-manager environment contains
  `XR_RUNTIME_JSON=/usr/share/openxr/1/openxr_wivrn.json`; this was imported, not persisted
  in a unit drop-in.
- The installed backend is ShowMeTheKey 1.21.0 at `/usr/bin/showmethekey-cli`; the installed
  xkbcommon is 1.13.2.
- The complete automated suite passed at integration: 90 doctest cases / 675 assertions,
  plus all private-bus CTest targets. These tests never opened an input device.
- With the headset disconnected, the installed daemon entered WiVRn runtime IPC and a live
  D-Bus probe timed out. This does not invalidate the private-bus tests, but it means the
  installed daemon/producer pair still needs attended validation with a connected headset.

The next agent should connect WiVRn, confirm the daemon is responsive and rendering, then
run exactly:

```sh
~/.local/bin/hypxrhud-keys --mods-only --verbose
```

Accept no privilege escalation added by this project. Confirm the disclosure appears and
dwells before the backend child starts; exercise only harmless shortcuts; confirm plain text
is absent; then stop the producer and verify both child and panel disappear. Also test the
capture-fatal paths by preempting/dismissing the exact panel and by disconnecting the XR
runtime. Until those checks pass, describe this as an installed, offline-tested prototype,
not a live-validated film tool.
