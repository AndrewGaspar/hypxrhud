#pragma once

#include "Config.hpp"
#include "Panel.hpp"
#include "Scene.hpp"

#include <cstdint>
#include <map>
#include <string>
#include <vector>

// hypxrhud — the PURE D-Bus props -> SUpsert mapping (WP-H3). The D-Bus `CreatePanel` /
// `UpdatePanel` take an `a{sv}` props dict (design memo §2.2), extensible mako/notify
// style. The sd-bus front end (Dbus.cpp) deserialises the on-wire `a{sv}` into a typed
// SPropMap (a value variant per key), then hands it to `upsertFromProps` HERE — so the
// SEMANTIC mapping (props -> the same SUpsert the interim NDJSON built) is pure and
// unit-tested with no live bus (tests/test_props.cpp). The mechanical variant-reading is
// the only part that needs libsystemd; it stays a thin adapter in Dbus.cpp.
//
// This is the exact inverse of hypxrvoice's SHudView-as-a{sv} plan: the model already
// carries these fields (lines+colorRole, confidence, gauges, fade envelope), so the
// mapping is a straight copy with defaults from config.
//
// Recognised keys (all optional; unknown keys ignored -> forward-compatible):
//   slot        s        named placement slot ("" = free placement)
//   space       s        "view" | "local"
//   urgency     y|u|i|x  0 low / 1 normal / 2 critical
//   pose        (ddd)    centre override, metres  -> hasPose
//   size        d        quad width override, metres -> hasSize
//   rise_ms     i|u|x    fade envelope (else config default)
//   hold_ms     i|u|x    hold; <0 = persist until update/dismiss
//   fade_ms     i|u|x    fade-out
//   opacity     d        per-panel opacity ceiling
//   kind        s        "text" | "gauges"
//   title       s        convenience: a single big Accent line (prepended)
//   lines       list     structured text rows (text, colorRole, big)
//   gauges      list     metered rows (label, percent, charging)
//   confidence  d        [0,1] certainty bar; <0 = none

namespace hud {

// One typed props value. Mirrors the D-Bus variant types the front end accepts. The list
// members (lines/gauges) are pre-parsed by the adapter so this stays a plain value type
// the pure mapper can consume without touching sd-bus.
struct SPropValue {
    enum class EType { Str, Int, Dbl, Bool, Vec3, Lines, Gauges } type = EType::Str;

    std::string         s;
    int64_t             i = 0;
    double              d = 0.0;
    bool                b = false;
    float               v3[3] = {0.f, 0.f, 0.f};
    std::vector<SLine>  lines;
    std::vector<SGauge> gauges;

    static SPropValue str(std::string v)  { SPropValue p; p.type = EType::Str;  p.s = std::move(v); return p; }
    static SPropValue integer(int64_t v)  { SPropValue p; p.type = EType::Int;  p.i = v; return p; }
    static SPropValue dbl(double v)        { SPropValue p; p.type = EType::Dbl;  p.d = v; return p; }
    static SPropValue boolean(bool v)      { SPropValue p; p.type = EType::Bool; p.b = v; return p; }
    static SPropValue vec3(float x, float y, float z) {
        SPropValue p; p.type = EType::Vec3; p.v3[0] = x; p.v3[1] = y; p.v3[2] = z; return p;
    }
    static SPropValue lineList(std::vector<SLine> v)   { SPropValue p; p.type = EType::Lines;  p.lines  = std::move(v); return p; }
    static SPropValue gaugeList(std::vector<SGauge> v) { SPropValue p; p.type = EType::Gauges; p.gauges = std::move(v); return p; }

    // Numeric coercion helpers (a client may send urgency as y/u/i/x, sizes as d/i).
    int64_t asInt() const {
        switch (type) {
            case EType::Int:  return i;
            case EType::Dbl:  return (int64_t)d;
            case EType::Bool: return b ? 1 : 0;
            default:          return 0;
        }
    }
    double asDouble() const {
        switch (type) {
            case EType::Dbl:  return d;
            case EType::Int:  return (double)i;
            default:          return 0.0;
        }
    }
};

using SPropMap = std::map<std::string, SPropValue>;

// Build an SUpsert from a props map. `id` (0 = create) and `owner` (the caller's unique
// bus name) come from the D-Bus method context, not the props, so they are passed
// explicitly. `defaults` seeds the fade envelope / opacity ceiling (config [hud]).
SUpsert upsertFromProps(uint32_t id, const std::string& owner, const SPropMap& props,
                        const SConfig& defaults);

} // namespace hud
