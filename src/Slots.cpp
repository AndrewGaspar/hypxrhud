#include "Slots.hpp"

namespace hud {

// The six locked default slots (design memo triage §5). Poses are VIEW-space metres,
// -z forward; x>0 is right, y>0 is up. Corner slots sit a little further away (-1.2 m)
// than the centred transient panels so they stay out of the way.
CSlots::CSlots() {
    m_slots = {
        // name       x      y      z      w     space   stack max  dy
        {"voice",   0.00f, -0.28f, -1.00f, 0.42f, "view", false, 1, 0.16f}, // bottom-centre
        {"keys",    0.00f, -0.14f, -1.00f, 0.42f, "view", false, 1, 0.16f}, // just above voice
        {"toast",   0.00f,  0.30f, -1.10f, 0.40f, "view", true,  3, 0.16f}, // top-centre, stack
        {"status",  0.55f, -0.20f, -1.20f, 0.30f, "view", false, 1, 0.12f}, // bottom-right
        {"media",  -0.55f,  0.20f, -1.20f, 0.34f, "view", false, 1, 0.12f}, // top-left
        {"battery",-0.55f, -0.20f, -1.20f, 0.30f, "view", false, 1, 0.12f}, // bottom-left
    };
}

const SSlot* CSlots::find(const std::string& name) const {
    for (const auto& s : m_slots)
        if (s.name == name)
            return &s;
    return nullptr;
}

SSlot* CSlots::findMut(const std::string& name) {
    for (auto& s : m_slots)
        if (s.name == name)
            return &s;
    return nullptr;
}

} // namespace hud
