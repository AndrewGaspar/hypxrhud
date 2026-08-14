#pragma once

#include "Presentation.hpp"

namespace hudkeys {

enum class EPresentationCheck {
    Waiting,
    Current,
    Omitted,
};

// Classify the exact-panel acknowledgement returned by the daemon. Once the daemon has
// submitted any shouldRender frame, a non-current panel is a fail-closed omission rather
// than something the capture client may wait through.
EPresentationCheck checkPresentation(const hud::SPresentationSnapshot& snapshot);

} // namespace hudkeys
