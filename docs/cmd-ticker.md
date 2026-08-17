# Command ticker (`hypxrhud-cmdlog`)

`hypxrhud-cmdlog` is a **filming aid**: it puts the `hyprctl` commands you run on the box
into the headset HUD for a few seconds each, so a viewer of the recording sees the commands
that are driving the compositor as they take effect. It is two pieces:

- a `hyprctl` **PATH shim** (`shim/hyprctl`, installed by hand at `~/.local/bin/hyprctl`)
  that publishes the command line and then execs the real `hyprctl`, and
- the **producer daemon** (`hypxrhud-cmdlog`) that owns the ticker's D-Bus endpoint, keeps
  the last few commands, and renders them into one hypxrhud slot.

## What it shows

The **last three** `hyprctl` command lines, newest first (`history`), each on screen for
about **six seconds** from when it was last published (`ttl_ms`). The newest row is the big
accent line; older rows are dimmed below it. A command longer than **60 characters**
(`max_chars`) keeps its **head** and ends in `...` — the head is what identifies a command
on camera. A command repeated inside the coalesce window becomes `… x2` instead of a second
row. When the last row ages out, the panel is **dismissed**, not blanked: an idle session
carries no ticker quad at all.

The default slot is `status` (bottom-right), which keeps the ticker clear of `voice` and
`keys` — the two slots a filmed session is most likely to also be using — and of `battery`
(bottom-left).

## What it deliberately does not show

- **Keybind-dispatched actions never appear.** A `bind = SUPER, Q, killactive` runs inside
  the compositor; it never spawns `hyprctl`, so there is nothing for the shim to see. What
  you film with this is the *typed-command* half of a demo. (`hypxrhud-keys` is the tool for
  the keystroke half.)
- **Direct IPC users bypass it.** Anything that talks to the Hyprland socket itself —
  `hyprctl` invoked by absolute path, a plugin, `hyprland-ipc` bindings in a script, another
  compositor client — never passes through the PATH shim.
- **Read-only queries are skipped by default.** `hyprctl -j …` (any JSON query) and the
  bare query verbs (`version monitors clients workspaces activewindow layers devices binds
  configerrors` and friends) are filming noise. The full list is one variable at the top of
  the shim (`HYPXR_CMD_HUD_SKIP`); trim or extend it freely, or override it in the
  environment.
- **It is not an audit log.** Nothing is written to disk, the rows live only in the
  producer's memory, and they are gone six seconds later. Equally, it is not a security
  boundary: with the ticker running, anyone looking at the headset (or the recording) sees
  the command lines, including any argument you typed.

## How it works, and why a producer daemon

```
~/.local/bin/hyprctl  ──(background, ≤100 ms, fire-and-forget)──▶  hypxrhud-cmdlog
        │                     Publish(s "hyprctl dispatch …")            │
        └── exec /the/real/hyprctl "$@"  (args/stdin/stdout/exit code)   │
                                                              CreatePanel/UpdatePanel
                                                                         ▼
                                                                     hypxrhud
```

hypxrhud keys every panel to its creator's **unique bus name** and auto-dismisses that
client's panels when it disconnects (`client-gone`). A one-shot `busctl CreatePanel` from
the shim would therefore have its panel torn down the instant the shim exited — which is
why the ticker is a long-lived producer, and why the history/expiry/truncation model lives
there rather than in the shim.

The producer is a plain hypxrhud client, so it follows the same lifecycle rules as
`hypxrhud-battery`: it creates the panel on the first row, sends updates
`NO_REPLY_EXPECTED`, dismisses on empty, and treats an absent/unreachable daemon as a
retry, not an error (a `CreatePanel` failure is logged at DEBUG and retried on the next
publish or in two seconds). It watches `PanelDismissed` and `NameOwnerChanged`, so a
preemption or a HUD restart just drops the stale id and the next command re-creates the
panel. Unlike `hypxrhud-keys` there is **no presentation gate**: the ticker is cosmetic,
carries no capture privilege, and is happy to accept rows while the headset is doffed (they
simply expire unseen).

Its D-Bus surface is two members on `io.github.andrewgaspar.hypxrhud.cmdlog`, object
`/io/github/andrewgaspar/hypxrhud/cmdlog`, interface
`io.github.andrewgaspar.hypxrhud.cmdlog1`:

```
Publish(s commandLine) -> ()   # designed for NO_REPLY_EXPECTED; the shim never waits
property as Rows (read)        # the rows currently on screen, newest first
```

## The shim's contract

`shim/hyprctl` is a bash script whose whole job is to be invisible:

- It **execs** the real `hyprctl`, so argv, stdin, stdout, stderr, and the exit code are the
  real command's, untouched.
- The publish happens **before** the exec, **backgrounded**, with stdin/stdout/stderr
  redirected to `/dev/null` (so a `hyprctl … | head` pipeline is never held open by it) and
  a hard `timeout` of 100 ms. `busctl --expect-reply=no --auto-start=no` means an absent
  ticker costs one dropped bus message and prints nothing; a missing `busctl`, a missing
  bus, or a missing session is likewise a silent no-op.
- Nothing in the publish path can change the real command's behaviour or status.

Environment knobs (all optional):

| variable | effect |
|---|---|
| `HYPXR_CMD_HUD=0` | skip publishing entirely (the kill switch) |
| `HYPXR_CMD_HUD_SKIP` | override the skipped-verb list |
| `HYPXR_CMD_HUD_REAL` | pin the real `hyprctl` (e.g. `/usr/bin/hyprctl`) |
| `HYPXR_CMD_HUD_TIMEOUT` | publish timeout in seconds (default `0.1`) |
| `HYPXR_CMD_HUD_MAXLEN` | cap on the published string (default 400) |

**Which `hyprctl` it execs:** `HYPXR_CMD_HUD_REAL` if set, otherwise the next `hyprctl` in
`PATH` that is not the shim itself (resolved by realpath, so the shim can never exec
itself), otherwise `/usr/bin/hyprctl`. This deliberately preserves whatever you would have
run without the shim — on the development host `~/code/hypxrland/build/bin/hyprctl` comes
later in `PATH` than `~/.local/bin`, and that build-tree binary is what the shim keeps
using. Set `HYPXR_CMD_HUD_REAL` if you want a fixed one.

## Config

`$XDG_CONFIG_HOME/hypxrhud/cmd.toml` (fallback `~/.config/hypxrhud/cmd.toml`); a missing
implicit file uses the compiled defaults, while an explicitly requested missing/unreadable
file fails closed. The commented reference is `examples/cmd.toml`:
`slot` (default `status`), `history` (3), `ttl_ms` (6000), `max_chars` (60), `coalesce_ms`
(1500), `rise_ms` (90), `fade_ms` (300), `opacity` (0.92). Unknown keys and sections warn
and are ignored; out-of-range values are errors.

## Install (integrator)

Build and install the project as usual — the producer is built and installed with the other
clients:

```sh
cmake -S . -B build-user \
  -DCMAKE_BUILD_TYPE=RelWithDebInfo \
  -DCMAKE_INSTALL_PREFIX="$HOME/.local"
cmake --build build-user -j
ctest --test-dir build-user --output-on-failure
cmake --install build-user
```

That installs `~/.local/bin/hypxrhud-cmdlog`, the example config, and — deliberately **not**
into `bin/` — the shim at `~/.local/share/hypxrhud/shim/hyprctl`. The shim is a file named
`hyprctl`; installing it into a `bin/` directory would shadow the real `hyprctl` for anyone
who merely installed this project, so putting it on `PATH` is an explicit, separate step:

```sh
# 1. the PATH shim (this is the step that changes what `hyprctl` means in your shells)
install -Dm755 ~/.local/share/hypxrhud/shim/hyprctl ~/.local/bin/hyprctl
hash -r                     # forget the old hyprctl in already-open shells
command -v hyprctl          # expect ~/.local/bin/hyprctl

# 2. the producer's user unit (static/manual — a filming aid, never enabled at login)
install -Dm644 build-user/services/hypxrhud-cmdlog.service \
  "$HOME/.config/systemd/user/hypxrhud-cmdlog.service"
systemctl --user daemon-reload

# 3. optional: your own config
install -Dm644 examples/cmd.toml ~/.config/hypxrhud/cmd.toml
```

The HUD daemon itself does **not** need restarting for this feature — the ticker is an
ordinary client of the interface it already serves. (Restart it only if you are also
deploying a new `hypxrhud` binary from the same build.)

Start the ticker when you are about to film, and stop it afterwards:

```sh
systemctl --user start hypxrhud-cmdlog.service    # or: ~/.local/bin/hypxrhud-cmdlog --verbose
systemctl --user stop  hypxrhud-cmdlog.service
```

### Verify without a headset

Use a harmless but publishable command — `notify` is ideal, since its only effect is a
short toast on the flat desktop:

```sh
hyprctl notify 1 2000 0 "ticker check" >/dev/null; hypxrhud-cmdlog --rows
# expect: hyprctl notify 1 2000 0 'ticker check'

HYPXR_CMD_HUD=0 hyprctl notify 1 1000 0 hidden >/dev/null   # kill switch: rows unchanged
hyprctl monitors >/dev/null                                 # skip list:   rows unchanged
hypxrhud-cmdlog --rows
```

`hypxrhud-cmdlog --publish "hyprctl reload"` injects a row without running anything, and
`busctl --user get-property io.github.andrewgaspar.hypxrhud.cmdlog
/io/github/andrewgaspar/hypxrhud/cmdlog io.github.andrewgaspar.hypxrhud.cmdlog1 Rows` is the
same read over plain `busctl`. With the ticker stopped, both report the name as
unavailable — which is exactly what the shim sees, and why it stays silent.

### Uninstall / disable

Stop the unit and the shim is inert. To remove the shim itself, `rm ~/.local/bin/hyprctl`
(and `hash -r`); `HYPXR_CMD_HUD=0` in the environment disables publishing without removing
anything.

## Test boundaries

- `tests/test_cmdlog.cpp` — the pure half: control-character/whitespace sanitising, UTF-8-safe
  head truncation, newest-first history with the `history` bound, coalescing, per-row TTL
  expiry, and the config parser's ranges/warnings.
- `tests/cmdlog_integration.cpp` — a private `dbus-run-session` bus with the REAL daemon
  (`--no-xr`), the REAL producer, and the REAL shim script: publish → rows → one panel,
  truncation over the wire, the skip list (verb and `-j`), the `HYPXR_CMD_HUD=0` kill switch
  (with the real command still running and its exit code/stdout/stdin/argv intact), expiry
  dismissing the panel, and recovery across a HUD restart.

No automated test ever reaches a real compositor: every shim invocation runs with `PATH`
pointing at a stub `hyprctl` and `HYPRLAND_INSTANCE_SIGNATURE` unset.

## Live checks left for the user

- [ ] With the ticker running and the headset on, `hyprctl dispatch exec kitty` shows the
      command bottom-right within a frame or two, and it fades out ~6 s later.
- [ ] Three commands in quick succession stack newest-first; the fourth pushes the oldest off.
- [ ] A long command is readable at a glance (tune `max_chars` for your capture resolution).
- [ ] Stopping the unit removes the panel immediately and leaves `hyprctl` behaving normally.
