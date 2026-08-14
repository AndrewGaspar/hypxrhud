#pragma once

#include <cstdint>
#include <string>

namespace hudkeys {

enum class EKeyState {
    Pressed,
    Released,
};

struct SKeyEvent {
    uint16_t    code = 0;
    EKeyState   state = EKeyState::Released;
    int64_t     sourceTimestamp = 0;
};

enum class EParseStatus {
    Event,
    Ignored,
    Error,
};

struct SParseResult {
    EParseStatus status = EParseStatus::Error;
    SKeyEvent    event;
    std::string  error;
};

// Parse one complete ShowMeTheKey CLI JSON record. The parser deliberately returns only a
// structural error category: callers must never log the source line, because it may contain
// private key data.
SParseResult parseShowMeTheKeyEvent(const std::string& line);

} // namespace hudkeys
