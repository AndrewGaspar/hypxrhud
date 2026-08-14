#include "Disclosure.hpp"

namespace hudkeys {

EPresentationCheck checkPresentation(const hud::SPresentationSnapshot& snapshot) {
    // Before this exact panel has ever reached a layer array, successful global frames are
    // expected: its rise alpha may still be below the scene's visibility threshold. The
    // caller bounds this wait. Once panelSerial is nonzero, any mismatch is a real omission.
    if (snapshot.panelSerial == 0)
        return EPresentationCheck::Waiting;
    return snapshot.panelSerial == snapshot.frameSerial ? EPresentationCheck::Current :
                                                          EPresentationCheck::Omitted;
}

} // namespace hudkeys
