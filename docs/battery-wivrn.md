# WiVRn headset battery — mechanism, finding, and the seam

`hypxrhud-battery` shows two gauges in the HUD `battery` slot: the **laptop** battery (via
UPower, works out of the box) and the **headset** battery (via WiVRn). This note documents
exactly how the headset battery does — and does not — reach the PC, why the headset gauge is
gated behind a documented seam today, and the minimal WiVRn change that lights it up.

## What the source says (WiVRn v26.6.1, `io.github.wivrn` @ ddb5a8ce)

The headset DOES report its battery to the PC. The data path is:

1. The Android client samples its own battery via a `BroadcastReceiver` for
   `ACTION_BATTERY_CHANGED` (`client/android/battery.cpp`) and computes
   `charge = level / scale` (a fraction in `[0,1]`), plus a `charging` flag.
2. It sends a `from_headset::battery { float charge; bool present; bool charging; }` packet
   (`common/wivrn_packets.h:507`).
3. The server ingests it: `wivrn_session::operator()(from_headset::battery&&)`
   (`server/driver/wivrn_session.cpp:718`) → `wivrn_hmd::update_battery`
   (`server/driver/wivrn_hmd.cpp:151`), stored in a `thread_safe<from_headset::battery>`.
4. The server exposes it to **Monado only**, as the HMD `xrt_device` method
   `wivrn_hmd::get_battery_status(present, charging, charge)`
   (`server/driver/wivrn_hmd.cpp:205`; wired at `wivrn_hmd.cpp:92`
   `.get_battery_status = ...`). Git history: `225bf9d3 expose battery on hmd xdev`,
   `e86ebfda auto wivrn_hmd_get_battery_status`.

That is the end of the line. The battery is **not** surfaced anywhere an out-of-process
sibling can read it:

- **Not on WiVRn's D-Bus interface.** `dbus/io.github.wivrn.Server.xml` (interface
  `io.github.wivrn.Server` at `/io/github/wivrn/Server`) exposes `HeadsetConnected`,
  `SessionRunning`, `SystemName`, refresh rates, FoV, codecs, pairing/keys, `ClientTab`, and
  bitrate — but **no battery property**. Confirmed by live introspection of the running
  server (pid owns `io.github.wivrn.Server` on the session bus) and by the git history of
  `Server.xml` (properties were added over time — `SystemName`, `ClientTab`, `Bitrate` — but
  battery never was, on `boundaryless`/`master`).
- **No OpenXR path.** There is no core or Monado OpenXR extension for HMD battery (no
  `battery` symbol under `monado/src/xrt/external/openxr_includes/` or the `oxr` state
  tracker). So `hypxrhud`, though it is itself an OpenXR overlay client of this runtime,
  cannot query it through the OpenXR API.
- **Monado IPC is a dead end for us.** Monado's IPC protocol *does* define
  `device_get_battery_status` (`monado/src/xrt/ipc/shared/proto/50-device.json:247`, handler
  `ipc_server_handler.c:3041`), but there is no generated client caller and no OpenXR entry
  point that reaches it. Opening a raw Monado IPC client to WiVRn's socket would (a) depend
  on WiVRn's fork-specific, unpinned IPC ABI, and (b) create a second Monado client session
  against a runtime the user may be **live** in — a state-changing action this project's
  safety rules forbid. Not attempted.

**Conclusion:** in WiVRn v26.6.1 the headset battery is genuinely not readable by a
separate process without patching WiVRn. So the client ships the headset gauge behind a
forward-compatible seam and the laptop gauge fully working.

## The seam (what `hypxrhud-battery` already does)

`readWivrn()` (`src/battery/BatterySources.cpp`):

1. Reads `HeadsetConnected` (b) — this property *exists* and tells us a headset is attached.
2. Probes a `Battery` property on `io.github.wivrn.Server`, tolerating either signature:
   - `(bbd)` = `(present, charging, charge)` with `charge` in `[0,1]` — **the contract**, and
   - a bare `d` = `charge` only (convenience).
3. If the property is absent (today's v26.6.1), the headset gauge is **omitted** — even
   while a headset is connected — rather than shown blank or stale.

So the moment a WiVRn build exposes `Battery`, the gauge appears with **no change to
`hypxrhud-battery`**. `wivrnChargeToPercent()` scales `charge` (`[0,1]`) to a percentage.

## Minimal WiVRn patch to enable it (optional, user-side)

Add a `Battery` property to the server D-Bus object, reading the value the HMD already
holds. Sketch against `io.github.wivrn` @ ddb5a8ce:

- `dbus/io.github.wivrn.Server.xml` — add under the `io.github.wivrn.Server` interface:

  ```xml
  <!-- present, charging, charge[0..1] -->
  <property name="Battery" type="(bbd)" access="read"/>
  ```

- `server/driver/wivrn_hmd.{h,cpp}` — expose the stored battery to the server object (the
  value is already there: `battery.lock()` / `get_battery_status`).
- `dashboard/wivrn_server.cpp` (the Qt object backing the D-Bus interface) — add the
  `Battery` property getter returning `(present, charging, charge)` from the session's HMD,
  and emit `PropertiesChanged` when `update_battery` runs (mirroring how `HeadsetConnected`
  is published) so `hypxrhud-battery` wakes immediately instead of on its poll timer.

The client already listens for `PropertiesChanged` on `io.github.wivrn.Server`, so no
client change is needed. Pin: this seam targets `io.github.wivrn.Server` as of WiVRn
v26.6.1; if upstream adds battery under a different name/signature, adjust `readWivrn()`.

## Live probe (this box, 2026-07-13)

`busctl --user get-property io.github.wivrn.Server /io/github/wivrn/Server
io.github.wivrn.Server HeadsetConnected SessionRunning SystemName` →
`b false`, `b true`, `s "Meta Quest 3"`. No `Battery` property (as expected). UPower
`DisplayDevice` reported `Percentage 83`, `State 2` (discharging), `Type 2` (Battery),
`IsPresent true` — so the laptop gauge reads correctly live.
