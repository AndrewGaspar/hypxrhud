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

> Status: **WP-H1 + WP-H2** (this milestone). The render core and multi-panel
> slot scene are implemented and tested; the transport is an interim **stdin
> NDJSON** feed. The **D-Bus front end (WP-H3)** and **lifecycle/backoff
> (WP-H4)** are designed-for but not yet built — see *What lands in H3/H4* below.
> Full design memo: `HYPXRHUD.md` in the HypXRland tree
> (`docs/openxr/research/`).

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
   (interim: stdin NDJSON  →  WP-H3: D-Bus CreatePanel/UpdatePanel/Toast)
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
   - `Wire` — the interim stdin NDJSON transport (retired when D-Bus lands).
   - `PngWrite` + `Preview` — the offline `--preview` composite.
2. **XR overlay session** (`Session`, `Egl`) — one `XrSessionCreateInfoOverlayEXTX`
   session, single-threaded with the EGL context held current the whole time
   (Monado's fence contract by construction). It **mirrors** the pure scene into
   GPU swapchains, uploading a panel **only when its content epoch changes**
   (a static panel re-submits its swapchain for free — no re-raster, no upload),
   and submits **one `XrCompositionLayerQuad` per visible panel** with a per-panel
   `XR_KHR_composition_layer_color_scale_bias` fade, in one `xrEndFrame`.
3. **Transport** — currently stdin NDJSON (`Wire`), a seam the D-Bus front end
   replaces without touching the scene model.

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
`preempted`); equal urgency is last-writer-wins; a lower-urgency newcomer is
refused. The `toast` slot is a bounded stack — new toasts offset older ones upward,
and the oldest is evicted past `max`. A panel may set `slot = ""` + an explicit
`pose` to bypass slotting (free placement).

The **battery** panel carries **two** gauges — headset battery (from WiVRn) and
laptop battery (from upower) — each `{label, percent, charging}`. The battery
*client* is a later WP; the content model and preview already support it.

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

# Live (needs an XR runtime + a headset session; ONE runtime per box):
build/hypxrhud --config ~/.config/hypxrhud/hypxrhud.toml
#   then feed it panels, one JSON object per line, on stdin (see "Interim wire").
```

If no XR runtime is available, the daemon logs a clear error and exits **3**
(`kExitNoRuntime`) — the same degrade convention hypxrvoice uses to fall back to
`notify-send`.

### Config keys

See `examples/hypxrhud.toml`. Under `[hud]`: `z` (overlay placement, default 20),
`gpu` (DRM render node, empty = auto; `--gpu`/`--z` override), `opacity`,
`blend_mode` (`opaque|alpha|additive`), `per_client_cap` (default 4), `tex_w`/
`tex_h`, and the default `rise_ms`/`hold_ms`/`fade_ms` envelope. Under
`[slot.<name>]`: `pose = "x,y,z"`, `size`, `space` (`view|local`), `max` (toast
stack cap). Config path: `$XDG_CONFIG_HOME/hypxrhud/hypxrhud.toml`.

### Interim wire (stdin NDJSON) — will be replaced by D-Bus in H3

One JSON object per line. This is the SHudView format extended with `{id, slot,
content}`; it deserialises into the **same** request the D-Bus `CreatePanel`/
`UpdatePanel` will build, so H3 replaces the transport, not the model.

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

## What lands in H3/H4 (seams already in place)

- **WP-H3 — D-Bus front end.** An sd-bus vtable
  (`io.github.andrewgaspar.hypxrhud`) for `CreatePanel`/`UpdatePanel`/
  `DismissPanel`/`Toast`/`Capabilities`, with `NameOwnerChanged` auto-dismiss and
  the bus fd folded into the frame `poll()` loop (no threads). It drives the
  **same** `CScene::upsert`/`dismiss`/`dropOwner` API this milestone already
  exposes; `Wire`/stdin is deleted. `SPanel` records an `owner` (the creator's
  unique bus name) precisely so `dropOwner("<name>")` implements client-gone
  auto-dismiss. `SDismissal{id, reason}` values returned by the arbiter become the
  `PanelDismissed(id, reason)` signal.
- **WP-H4 — lifecycle + backoff.** The daemon must **not** exit on runtime loss
  (today `Session::pollEvents` sets an exit flag, mirroring the disposable wp-v5
  subprocess). H4 turns that into HypXRland's `xrReprobeBackoffMs` probe/backoff:
  tear the session down, keep the panel table, re-raster every live panel on
  reconnect, and surface a `RuntimeStateChanged` signal around the gap. A
  runtime-absent start already accepts panels into the pure scene; H4 makes them
  render when the runtime appears.

Later milestones: slot theming from the Omarchy current theme (H6), packaging as a
bus-activated `Type=dbus` unit (H7), migrating hypxrvoice to a pure client (H8),
`hypxrkeys` as the second client (H9), and the presence-gated notification mirror
(H10).

## Licence

BSD 3-Clause (`LICENSE`). Bundles LiberationMono under the SIL Open Font License
(`third_party/fonts/LiberationMono-OFL.txt`) and the public-domain `stb_truetype` /
`stb_image_write`. The render core, XR session bring-up, and EGL helper were lifted
from [hypxrvoice](../hypxrvoice)'s WP-V5 HUD and generalised.
```
