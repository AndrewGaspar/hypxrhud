#pragma once

#include <string>
#include <vector>

// hypxrhud — the slot model (design memo §4). A SLOT is a named VIEW-space anchor with
// a default pose, a stacking rule, and an occupancy policy. Panels request a slot by
// name; the scene arbiter (Scene) resolves collisions. All poses are metres in the
// panel's reference space, -z forward. Every field is config-overridable
// (examples/hypxrhud.toml), so this file only defines the DEFAULT six-slot layout
// locked at triage:
//
//   voice   bottom-centre   (single, last-writer-wins)   — hypxrvoice feedback
//   keys    just above voice (single, last-writer-wins)  — hypxrkeys screenkey
//   toast   top-centre       (STACK, newest lowest, N=3) — notification mirror
//   status  bottom-right     (single, pinned)            — status badges
//   media   top-left         (single)                    — now-playing widget
//   battery bottom-left      (single)                    — headset + laptop gauges
//
// Nothing here touches OpenXR; it is pure geometry + policy, unit-tested directly.

namespace hud {

// Where a panel ends up: a resolved centre pose + width in its reference space.
struct SPlacement {
    float       px = 0.f, py = 0.f, pz = -1.0f; // centre, metres.
    float       sizeW = 0.42f;                  // quad width, metres (height from aspect).
    std::string space = "view";                 // "view" (head-locked) | "local" (world-fixed).
};

struct SSlot {
    std::string name;
    float       px = 0.f, py = 0.f, pz = -1.0f;
    float       sizeW = 0.42f;
    std::string space = "view";
    bool        stack    = false; // true = bounded vertical stack (toast); false = singleton.
    int         maxStack = 3;      // stack cap; oldest evicted past this.
    float       stackDy  = 0.16f;  // per-entry vertical offset for a stack, metres (+ = up).
};

// The slot registry: the six defaults, each overridable. `find` returns nullptr for an
// unknown slot name (a panel with no/unknown slot uses free placement instead).
class CSlots {
  public:
    CSlots(); // installs the six locked defaults.

    const SSlot* find(const std::string& name) const;
    SSlot*       findMut(const std::string& name);
    const std::vector<SSlot>& all() const { return m_slots; }

  private:
    std::vector<SSlot> m_slots;
};

} // namespace hud
