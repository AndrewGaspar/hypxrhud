# hypxrhud

**A shared XR HUD daemon for the HypXRland ecosystem.** It owns **one** OpenXR
overlay session and renders an **N-panel, slot-based, head-locked HUD** above
HypXRland's monitor quads. XR utilities — [hypxrvoice](../hypxrvoice)'s feedback
HUD, the planned `hypxrkeys` screenkey overlay, in-headset notification toasts, a
now-playing widget, a battery readout — push panels to hypxrhud instead of each
re-implementing session lifecycle, EGL, swapchains, and fade machinery (and each
re-risking Monado's GL-fence contract).

It pairs with the family: **hypxrpaper** owns the *primary* session (the
environment), **HypXRland** composites monitor quads as an overlay, and
**hypxrhud** sits on top as a second overlay carrying the HUD.

> Status: **WP-H1 … WP-H7** complete. The render core + multi-panel slot scene
> (H1/H2), the **D-Bus front end** (H3, `io.github.andrewgaspar.hypxrhud`), the
> **persistent lifecycle + re-probe backoff** (H4), the **slot arbiter polish**
> (H5 — queueing + occupancy introspection), **Omarchy theming** (H6 — live palette
> from the current theme), and **packaging + `--self-test`** (H7) are implemented and
> tested (unit + a private-bus integration suite + a one-command health check). The
> interim stdin NDJSON feed survives behind a `--stdin` debug flag. Live in-headset
> validation (checklist below) is deferred to the user. Full design memo:
> `HYPXRHUD.md` in the HypXRland tree (`docs/openxr/research/`).

## Architecture

```
   ┌──────────────────────────────────────────────────────────────┐
   │        Monado / WiVRn multi-system compositor                 │
   │  composites every active+visible overlay session each frame   │
   └──────▲───────────────────▲───────────────────────▲────────────┘
  z_order │ INT64_MIN         │ overlay_z = 1          │ hud_z = 20 (top)
          │                   │                        │
   ┌──────┴─────┐     ┌───────┴────────┐     ┌─────────┴──────────────┐
   │ hypxrpaper │     │  HypXRland     │     │  hypxrhud (THIS DAEMON)│
   │  primary   │     │  monitor quads │     │  ONE overlay session   │
   │  bg / glTF │     └────────────────┘     │  N head-locked quads   │
   └────────────┘                            │  one per panel         │
                                             └─────────┬──────────────┘
   panel producers ───────────────────────────────────┘
   (D-Bus CreatePanel/UpdatePanel/DismissPanel; --stdin NDJSON for debug)
```

Three layers, mirroring hypxrvoice's clean split:

1. **Pure model + render core** (no OpenXR/EGL/GL) — unit-tested, offline-renderable:
   - `Panel` — the panel content model (`SPanelContent`: coloured text lines, a
     confidence bar, **named gauges**) and the fade **envelope** (`SFade` +
     `envelopeOpacity`).
   - `Slots` — the six named VIEW-space anchors with stacking + occupancy rules.
   - `Scene` — the **panel table** (`id → SPanel`), the slot **arbiter**
     (collision policy), per-client budgets, and the layer-budget math.
   - `PanelText` — the stb_truetype rasteriser → premultiplied RGBA (bundled OFL
     font baked into the binary; no runtime font dependency). Renders text panels
     *and* the dual-gauge battery panel.
   - `Config` — a deliberately thin TOML loader (one struct, swappable for Lua).
   - `Props` — the pure D-Bus `a{sv}` props → `SUpsert` mapping (the front end reads
     the wire into a typed prop map, then this maps it; unit-tested with no bus).
   - `Backoff` — the runtime re-probe schedule (vendored from the compositor).
   - `Wire` — the `--stdin` NDJSON debug transport (kept for tests/scripting).
   - `PngWrite` + `Preview` — the offline `--preview` composite.
2. **XR overlay session** (`Session`, `Egl`) — one `XrSessionCreateInfoOverlayEXTX`
   session, single-threaded with the EGL context held current the whole time
   (Monado's fence contract by construction). It **mirrors** the pure scene into
   GPU swapchains, uploading a panel **only when its content epoch changes**
   (a static panel re-submits its swapchain for free — no re-raster, no upload),
   and submits **one `XrCompositionLayerQuad` per visible panel** with a per-panel
   `XR_KHR_composition_layer_color_scale_bias` fade, in one `xrEndFrame`.
3. **Transport** — the **D-Bus front end** (`Dbus`, sd-bus) and the persistent
   **event loop + lifecycle** (`Daemon`). The sd-bus fd is folded into the same
   single `poll()` loop that paces frames — no threads, so the EGL context stays
   current (Monado's fence contract). The `--stdin` NDJSON path (`Wire`) remains
   for debug/scripting and drives the *same* scene API.

### One quad per panel

Each visible panel contributes its own quad, so fades are free (`color_scale_bias.a`,
no re-upload) and only the panel that changed re-rasters. At session start the
daemon queries `maxLayerCount` and logs the effective budget = `min(maxLayerCount,
16)` (the OpenXR floor is 16; our stack reports 128). If more panels are visible
than the budget, the lowest-priority/oldest are coalesced out of submission rather
than overflowing `xrEndFrame`.

### Slots

Six named VIEW-space slots (all re-posable in config):

| slot | default pose (m) | placement | occupancy |
|---|---|---|---|
| `voice`   | `0,-0.28,-1.0`   | bottom-centre | singleton, last-writer-wins |
| `keys`    | `0,-0.14,-1.0`   | above voice   | singleton, last-writer-wins |
| `toast`   | `0,+0.30,-1.1`   | top-centre    | **stack** (newest lowest, N=3) |
| `status`  | `+0.55,-0.20,-1.2` | bottom-right | singleton, pinned |
| `media`   | `-0.55,+0.20,-1.2` | top-left    | singleton |
| `battery` | `-0.55,-0.20,-1.2` | bottom-left | singleton |

**Collision policy** (singleton slots): higher `urgency` preempts (loser dismissed
`preempted`); equal urgency is last-writer-wins. A **strictly-lower-urgency**
newcomer is, by default, **refused** (`CreatePanel` returns the
`…Error.Rejected` D-Bus error) — or, when the slot is configured
`on_refuse = "queue"` (WP-H5), **held pending**: the create succeeds and returns an
id, the panel occupies no head-space and is never rendered or expired, and it is
**promoted** into the slot (highest urgency, then oldest) the moment the occupant is
dismissed, expires, or its client disconnects. The `toast` slot is a bounded stack —
new toasts offset older ones upward, and the oldest is evicted past `max` (the
per-slot stack depth). A panel may set `slot = ""` + an explicit `pose` to bypass
slotting (free placement). `GetCapabilities` reports live per-slot occupancy
(`slotOccupancy`: active + queued counts) for introspection.

The **battery** panel carries **two** gauges — headset battery (from WiVRn) and
laptop battery (from upower) — each `{label, percent, charging}`. The first-party
client that fills it is [`hypxrhud-battery`](#hypxrhud-battery-battery-gauges) (below).

### `hypxrhud-battery` (battery gauges)

A small standalone sd-bus client (`src/battery/`) that feeds the `battery` slot:

- **Laptop** battery from UPower's `DisplayDevice` (`Percentage`/`State`), on the system
  bus; omitted on a desktop with no battery. Works out of the box.
- **Headset** battery from WiVRn. As of WiVRn **v26.6.1** the headset charge is not
  exposed on any external interface — it lives only inside the Monado HMD device — so the
  headset gauge is gated behind a forward-compatible seam that lights up the moment WiVRn
  publishes a `Battery` D-Bus property. See [`docs/battery-wivrn.md`](docs/battery-wivrn.md)
  for the full mechanism, source citations, and the minimal WiVRn patch.

It polls on a config interval (default 30 s), also wakes early on a UPower/WiVRn
`PropertiesChanged`, and only calls `UpdatePanel` when the rounded gauges change
(zero-cost-when-static). A configurable low-battery threshold (default 15 %) posts a
one-shot toast per source per discharge cycle. Config: `examples/battery.toml`
(`[battery]` section, `~/.config/hypxrhud/battery.toml`); unit:
`systemd/hypxrhud-battery.service`. Run `hypxrhud-battery --once --verbose` to print a
one-shot source read without looping.

## Building

Requirements: a C++23 compiler, CMake ≥ 3.20, `jansson` (interim wire format), and
— for the XR session — an OpenXR runtime plus `egl`, `glesv2`, `gbm`, `libdrm`
(Monado / WiVRn expose `XR_MNDX_egl_enable`, `XR_KHR_opengl_es_enable`,
`XR_EXTX_overlay`). When the XR deps are absent the daemon still builds and
`--preview` works; running it just reports no runtime.

```sh
cmake -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build -j
ctest --test-dir build --output-on-failure
```

## Running

```sh
# Offline six-slot review composite — no XR, safe to run anywhere:
build/hypxrhud --preview hud.png

# The daemon (D-Bus service). It owns io.github.andrewgaspar.hypxrhud and serves
# panels immediately, whether or not an XR runtime is present yet:
build/hypxrhud --config ~/.config/hypxrhud/hypxrhud.toml

# Drive it from any client (busctl example — a voice panel):
busctl --user call io.github.andrewgaspar.hypxrhud \
  /io/github/andrewgaspar/hypxrhud io.github.andrewgaspar.hypxrhud1 \
  CreatePanel 'a{sv}' 2 slot s voice urgency u 1     # -> u <id>

# Debug / scripting: also accept the interim NDJSON feed on stdin.
build/hypxrhud --stdin < panels.ndjson
# Headless / CI: never probe a runtime, serve D-Bus only.
build/hypxrhud --no-xr

# One-command health check: spins a PRIVATE bus, spawns the daemon (--no-xr), runs a
# create/update/dismiss + capabilities round-trip, exits 0 (healthy) or nonzero.
build/hypxrhud --self-test

# Themed offline preview (no headset), using a specific Omarchy theme dir:
build/hypxrhud --preview hud.png --theme ~/.config/omarchy/current/theme
```

### Health check — `--self-test`

`hypxrhud --self-test` is a self-contained, side-effect-free health check (WP-H7). It
re-execs itself once under `dbus-run-session` to obtain a **private** session bus (it
never touches your real one, and never starts an XR session — safe to run while a
headset is in use), spawns the daemon in `--no-xr` mode on that bus, and drives:
name acquisition → `GetCapabilities` (key shape) → `CreatePanel` (nonzero id) →
`PanelCount` → `UpdatePanel` (ack) → `DismissPanel` → `PanelCount` back to 0. It
prints a per-step `[ok]/[FAIL]` line and exits `0` on success, nonzero on any failure
— suitable for a packaging smoke test or a `systemd` `ExecStartPre` sanity gate.

**Persistent by design (WP-H4):** the daemon does **not** exit when no runtime is
present. It owns the bus name, accepts panels into the scene, and probes
`xrCreateInstance` on a growing backoff (2s → 30s while the runtime is absent; a
gentle fixed cadence while it's up but the headset is undonned). When the runtime
appears it rebuilds GPU state and re-uploads every live panel; a WiVRn disconnect
returns it to probing rather than killing it. `SIGTERM` ends the session cleanly
and releases the bus name.

### Config keys

See `examples/hypxrhud.toml` for the fully-commented reference. Config path:
`$XDG_CONFIG_HOME/hypxrhud/hypxrhud.toml` (a thin TOML subset; a missing file is not
an error). All keys are optional.

**`[hud]`** — `z` (overlay placement, default 20), `gpu` (DRM render node, empty =
auto; `--gpu`/`--z` override), `opacity` (default per-panel ceiling, 0.92),
`blend_mode` (`opaque|alpha|additive`), `per_client_cap` (default 4), `tex_w`/`tex_h`
(per-panel raster size, 768×384), the default `rise_ms`/`hold_ms`/`fade_ms` envelope
(110/2600/450), and the re-probe schedule `reprobe_base_ms` (2000) / `reprobe_cap_ms`
(30000).

**`[theme]`** (WP-H6) — `follow` (follow the Omarchy current theme, default `true`),
`file` (override the palette file path; empty = the theme's `mako.ini`), and per-role
hex overrides `normal` / `dim` / `accent` / `good` / `warn` / `bad` / `panel_bg` /
`bar_track` (each `"#RRGGBB"`, applied on top of the theme).

**`[slot.<name>]`** — override any of the six slots: `pose = "x,y,z"`, `size` (metres),
`space` (`view|local`), `max` (stack depth, toast), `on_refuse` (`refuse|queue`, WP-H5).

### Theming (WP-H6)

By default the panel palette **follows the Omarchy current theme**. hypxrhud resolves
the `~/.config/omarchy/current/theme` symlink and reads the theme's `mako.ini`,
mapping `text-color` → foreground, `border-color` → accent, `background-color` → the
panel fill, and deriving the muted hint text + bar tracks from that contrast. When no
Omarchy theme is present the built-in palette (a dark scheme) is used verbatim.

**Live reload:** an `inotify` watch on the theme symlink's parent directory catches
`omarchy-theme-set` (which atomically flips the symlink) and re-resolves the palette
**without a restart**, force-dirtying every panel so they re-raster in the new colours
on the next frame. The watch folds into the same single `poll()` loop as the bus and
frame pacing (no threads).

**Overrides:** `[theme] follow = false` pins the built-in palette; `[theme] file =
"…"` points at a different palette file; and per-role hex keys
(`normal`/`dim`/`accent`/`good`/`warn`/`bad`/`panel_bg`/`bar_track`) win over the
theme. Note that `mako.ini` carries only foreground/accent/background, so the status
roles (`good`/`warn`/`bad`) keep tuned defaults unless overridden — on a **light**
theme you will likely want to set darker `good`/`warn`/`bad` for contrast.

Offline evidence: `--preview <png> --theme <dir>` renders the six-slot composite using
a given theme dir's `mako.ini`, so you can review a theme's HUD colours without a
headset.

## D-Bus API (WP-H3)

Bus name **`io.github.andrewgaspar.hypxrhud`**, object
**`/io/github/andrewgaspar/hypxrhud`**, interface
**`io.github.andrewgaspar.hypxrhud1`**:

```
CreatePanel(a{sv} props) -> u id      # 0 never returned; a refusal is a D-Bus error
UpdatePanel(u id, a{sv} props) -> ()  # designed for NO_REPLY_EXPECTED (fire-and-forget)
DismissPanel(u id) -> ()
GetCapabilities() -> a{sv}            # version, maxLayerCount, budget, perClientCap,
                                      #  slots[], slotOccupancy a(suu) (name,active,queued),
                                      #  spaces[], runtimeState, runtimeName
signal PanelDismissed(u id, s reason) # "expired" | "client" | "preempted" | "client-gone"
signal RuntimeStateChanged(s state)   # "absent" | "connecting" | "live"
property s RuntimeState  (read, emits-change)
property s RuntimeName   (read, emits-change)
property u PanelCount    (read)
property u MaxPanels     (read, emits-change)   # = the effective per-frame layer budget
```

The `props` `a{sv}` (all keys optional, extensible mako/notify style) maps 1:1 onto
the panel model: `slot` s, `space` s (`view|local`), `urgency` (y/u/i/x),
`pose` (ddd), `size` d, `rise_ms`/`hold_ms`/`fade_ms` i, `opacity` d, `kind` s
(`text|gauges`), `title` s (a big accent line), `lines` `a(sub)` = (text,
colorRole 0–5, big), `gauges` `a(sdb)` = (label, percent, charging), `confidence` d.
Unknown keys are ignored (forward-compatible). Every panel is keyed to the caller's
**unique bus name**: when a client disconnects, all its panels are auto-dismissed
(`client-gone`) via `NameOwnerChanged`; a client may hold at most `per_client_cap`
(default 4) panels, and a create past that is refused with
`io.github.andrewgaspar.hypxrhud1.Error.Rejected`.

**Activation.** `dbus/io.github.andrewgaspar.hypxrhud.service` +
`systemd/hypxrhud.service` (`Type=dbus`, `BusName=…`) make the daemon
**bus-activated** on the first call; CMake installs them to
`share/dbus-1/services/` and `lib/systemd/user/`. `systemctl --user enable --now
hypxrhud.service` starts it eagerly instead.

### `--stdin` NDJSON debug feed

With `--stdin`, one JSON object per line drives the *same* scene API (useful for
tests/scripting). It deserialises into the same `SUpsert` the D-Bus props build.

```jsonc
// create/update a panel (id 0 allocates a new one; a known id updates in place)
{"action":"upsert","id":0,"owner":"voice","slot":"voice","urgency":1,
 "rise":110,"hold":-1,"fade":450,"opacity":0.92,
 "content":{"kind":"text","confidence":0.82,
            "lines":[{"t":"listening","c":"accent","big":true},
                     {"t":"open the browser","c":"normal"}]}}

// a dual-gauge battery panel
{"action":"upsert","slot":"battery",
 "content":{"kind":"gauges",
            "gauges":[{"label":"headset","percent":83,"charging":true},
                      {"label":"laptop","percent":47,"charging":false}]}}

// remove a panel
{"action":"dismiss","id":3}
```

Optional keys: `space` (`view|local`), `pose` (`[x,y,z]`, overrides slot),
`size` (quad width, metres). Colour roles: `normal|dim|accent|good|warn|bad`.

## Client-author quickstart

hypxrhud is driven entirely over D-Bus — a client needs no XR/EGL code. Minimal flow:

1. On start, `GetCapabilities()` (optional) and read the `RuntimeState` property /
   subscribe `RuntimeStateChanged`. While it is not `"live"`, fall back to your 2D
   path (e.g. `notify-send`) — panels are still *accepted* and shown when the headset
   comes up, but you may prefer the 2D surface while doffed.
2. `CreatePanel(props)` when you have something to show; **keep the returned `id`**.
3. `UpdatePanel(id, props)` for changes — send it `NO_REPLY_EXPECTED` (fire-and-forget)
   on hot paths (e.g. a per-word transcript); it is a one-way message with no round-trip.
4. `DismissPanel(id)` when done. If your process crashes, hypxrhud auto-dismisses your
   panels (`client-gone`) via `NameOwnerChanged` — no stale panels leak.

`busctl` example (a voice panel with a title + a body line + a confidence bar):

```sh
busctl --user call io.github.andrewgaspar.hypxrhud \
  /io/github/andrewgaspar/hypxrhud io.github.andrewgaspar.hypxrhud1 \
  CreatePanel 'a{sv}' 4 \
    slot s voice \
    urgency u 1 \
    confidence d 0.82 \
    title s listening \
    lines 'a(sub)' 1 'open the browser' 0 0        # -> u <id>

# inspect live per-slot occupancy:
busctl --user call io.github.andrewgaspar.hypxrhud \
  /io/github/andrewgaspar/hypxrhud io.github.andrewgaspar.hypxrhud1 GetCapabilities
```

## Live in-headset validation checklist (for the user)

Unit + integration + `--self-test` cover the pure logic and the bus surface offline;
the following need a real runtime + headset (run `preview-xr.sh --wivrn` or a full
session, and drive panels with `busctl` as above):

- [ ] Daemon reaches an overlay session over a running primary; a `CreatePanel` shows
      a head-locked quad at the slot pose; `DismissPanel` removes it; a transient panel
      fades on its envelope.
- [ ] Two panels fade independently (only the changed one re-rasters — a static panel
      costs nothing per frame).
- [ ] All six slots place correctly (voice/keys centred low, toast top-centre stack,
      status bottom-right, media top-left, battery bottom-left dual gauge).
- [ ] Toast stacking: three toasts stack upward; a fourth evicts the oldest.
- [ ] Queueing (`on_refuse = queue` on a slot): a lower-urgency panel is held, then
      appears the instant the occupant is dismissed.
- [ ] Theming: `omarchy-theme-set <other>` recolours the HUD **without a restart**
      (foreground/accent/panel background follow the theme).
- [ ] Lifecycle: kill/restart the runtime → panels survive the gap and reappear;
      compositor (primary) restart → the HUD keeps rendering.
- [ ] `SIGTERM` ends the session cleanly and releases the bus name.

## Seams left for later milestones

- **WP-H8 — hypxrvoice migration (client-side call mapping).** hypxrvoice becomes a
  pure client of this interface:
  `onListeningStart` → `CreatePanel({slot:"voice", …})` (keep the returned `id`);
  per-word transcript → `UpdatePanel(id, {lines/confidence})` sent
  **`NO_REPLY_EXPECTED`** (fire-and-forget, no round-trip); `onListeningStop` →
  `DismissPanel(id)`; the notify-send fallback triggers on `RuntimeState != "live"`
  or the daemon being absent (watch `NameOwnerChanged` / `RuntimeStateChanged`).
  Delete hypxrvoice's `hud/`, `HudMessage`, and render core (now provided here).
- **WP-H9 (hypxrkeys)** and **WP-H10 (presence-gated Notifications mirror)** follow
  as their own milestones.

## Licence

BSD 3-Clause (`LICENSE`). Bundles LiberationMono under the SIL Open Font License
(`third_party/fonts/LiberationMono-OFL.txt`) and the public-domain `stb_truetype` /
`stb_image_write`. The render core, XR session bring-up, and EGL helper were lifted
from [hypxrvoice](../hypxrvoice)'s WP-V5 HUD and generalised.
```
