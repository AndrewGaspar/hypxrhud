#pragma once

#include "Scene.hpp"

#include <string>

// hypxrhud INTERIM transport — one JSON object per line on stdin (NDJSON). This is the
// throwaway feed that keeps the daemon testable headless until WP-H3 lands the D-Bus
// front end. It is the SHudView wire format (hypxrvoice HudMessage) extended with
// {panel id, slot, content-kind, gauges}: it deserialises into the SAME SUpsert the
// D-Bus `CreatePanel`/`UpdatePanel` will build, so H3 replaces the TRANSPORT, not the
// scene model. Round-trips exactly, so it is unit-tested directly.
//
// Line schema (all fields except `action` optional; unknown keys ignored):
//   {"action":"upsert","id":0,"owner":"voice","slot":"voice","space":"view",
//    "urgency":1,"pose":[x,y,z],"size":0.42,"rise":110,"hold":-1,"fade":450,
//    "opacity":0.92,
//    "content":{"kind":"text","confidence":0.8,
//               "lines":[{"t":"listening","c":"accent","big":true}],
//               "gauges":[{"label":"headset","percent":83,"charging":true}]}}
//   {"action":"dismiss","id":3}

namespace hud {

struct SWireMsg {
    enum class EAction { Upsert, Dismiss } action = EAction::Upsert;
    SUpsert  upsert;         // meaningful for Upsert.
    uint32_t dismissId = 0;  // meaningful for Dismiss.
};

namespace Wire {
    // Parse one line into a message. Returns false on malformed input (the caller then
    // ignores the line rather than dying).
    bool parse(const std::string& line, SWireMsg& out);

    // Serialise a message to a single '\n'-terminated line (for tests / a producer CLI).
    std::string serialize(const SWireMsg& m);
}

} // namespace hud
