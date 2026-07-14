#include "Props.hpp"

namespace hud {

namespace {
    const SPropValue* find(const SPropMap& m, const char* key) {
        auto it = m.find(key);
        return it == m.end() ? nullptr : &it->second;
    }
}

SUpsert upsertFromProps(uint32_t id, const std::string& owner, const SPropMap& props,
                        const SConfig& defaults) {
    SUpsert u;
    u.id    = id;
    u.owner = owner;

    // Seed the fade envelope + opacity ceiling from config [hud] so an omitted key inherits
    // the operator's defaults rather than the struct hard-coded ones.
    u.fade.riseMs      = defaults.riseMs;
    u.fade.holdMs      = defaults.holdMs;
    u.fade.fadeMs      = defaults.fadeMs;
    u.fade.opacityCeil = defaults.opacity;

    // Each recognised key both fills the field AND flags it as SUPPLIED (BUG-2 merge): the
    // scene's update path applies only supplied fields so a partial UpdatePanel preserves the
    // rest. pose/size use hasPose/hasSize as their supplied-flag (a key is the only setter).
    if (const SPropValue* v = find(props, "slot"); v && v->type == SPropValue::EType::Str) {
        u.slot = v->s; u.setSlot = true;
    }
    if (const SPropValue* v = find(props, "space"); v && v->type == SPropValue::EType::Str) {
        u.space = v->s; u.setSpace = true;
    }
    if (const SPropValue* v = find(props, "urgency")) {
        u.urgency = (int)v->asInt(); u.setUrgency = true;
    }

    if (const SPropValue* v = find(props, "pose"); v && v->type == SPropValue::EType::Vec3) {
        u.hasPose = true;
        u.px = v->v3[0];
        u.py = v->v3[1];
        u.pz = v->v3[2];
    }
    if (const SPropValue* v = find(props, "size")) {
        u.hasSize = true;
        u.sizeW   = (float)v->asDouble();
    }

    if (const SPropValue* v = find(props, "rise_ms")) {
        u.fade.riseMs = (int)v->asInt(); u.setRise = true;
    }
    if (const SPropValue* v = find(props, "hold_ms")) {
        u.fade.holdMs = (int)v->asInt(); u.setHold = true;
    }
    if (const SPropValue* v = find(props, "fade_ms")) {
        u.fade.fadeMs = (int)v->asInt(); u.setFade = true;
    }
    if (const SPropValue* v = find(props, "opacity")) {
        u.fade.opacityCeil = (float)v->asDouble(); u.setOpacity = true;
    }

    // --- content ---
    if (const SPropValue* v = find(props, "kind"); v && v->type == SPropValue::EType::Str) {
        u.content.kind = panelKindFromName(v->s); u.setKind = true;
    }
    if (const SPropValue* v = find(props, "confidence")) {
        u.content.confidence = (float)v->asDouble(); u.setConfidence = true;
    }

    // A `title` is a convenience for a single big Accent line, prepended before any `lines`.
    if (const SPropValue* v = find(props, "title"); v && v->type == SPropValue::EType::Str && !v->s.empty()) {
        u.content.lines.push_back(SLine{v->s, EColor::Accent, true}); u.setLines = true;
    }

    if (const SPropValue* v = find(props, "lines"); v && v->type == SPropValue::EType::Lines) {
        for (const auto& ln : v->lines)
            u.content.lines.push_back(ln);
        u.setLines = true;
    }

    if (const SPropValue* v = find(props, "gauges"); v && v->type == SPropValue::EType::Gauges) {
        for (const auto& g : v->gauges)
            u.content.gauges.push_back(g);
        u.setGauges = true;
        // Gauges present with no explicit kind -> treat as a gauges panel (and flag the kind
        // as supplied so the inference survives a merge onto an existing panel).
        if (!find(props, "kind") && !u.content.gauges.empty() && u.content.lines.empty()) {
            u.content.kind = EPanelKind::Gauges; u.setKind = true;
        }
    }

    return u;
}

} // namespace hud
