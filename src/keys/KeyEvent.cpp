#include "KeyEvent.hpp"

#include <jansson.h>
#include <linux/input-event-codes.h>

#include <memory>

namespace hudkeys {
namespace {
    using JPtr = std::unique_ptr<json_t, decltype(&json_decref)>;

    bool integerField(json_t* root, const char* name, json_int_t& out) {
        json_t* value = json_object_get(root, name);
        if (!value || !json_is_integer(value))
            return false;
        out = json_integer_value(value);
        return true;
    }

    bool stringField(json_t* root, const char* name, const char*& out) {
        json_t* value = json_object_get(root, name);
        if (!value || !json_is_string(value))
            return false;
        out = json_string_value(value);
        return out != nullptr;
    }
}

SParseResult parseShowMeTheKeyEvent(const std::string& line) {
    if (line.empty() || line.size() > 65536)
        return {.status = EParseStatus::Error, .error = "record length out of range"};

    json_error_t jsonError = {};
    JPtr root(json_loadb(line.data(), line.size(), JSON_REJECT_DUPLICATES, &jsonError), json_decref);
    if (!root || !json_is_object(root))
        return {.status = EParseStatus::Error, .error = "invalid JSON object"};

    const char* eventName = nullptr;
    if (!stringField(root.get(), "event_name", eventName))
        return {.status = EParseStatus::Error, .error = "missing event_name"};
    if (std::string(eventName) != "KEYBOARD_KEY")
        return {.status = EParseStatus::Ignored};

    json_int_t eventType = 0, timestamp = 0, keyCode = 0, stateCode = 0;
    const char *keyName = nullptr, *stateName = nullptr;
    if (!integerField(root.get(), "event_type", eventType) ||
        !integerField(root.get(), "time_stamp", timestamp) ||
        !stringField(root.get(), "key_name", keyName) ||
        !integerField(root.get(), "key_code", keyCode) ||
        !stringField(root.get(), "state_name", stateName) ||
        !integerField(root.get(), "state_code", stateCode))
        return {.status = EParseStatus::Error, .error = "keyboard record has missing or mistyped fields"};

    if (eventType != 300 || timestamp < 0 || keyCode < 0 || keyCode > KEY_MAX ||
        std::string(keyName).rfind("KEY_", 0) != 0)
        return {.status = EParseStatus::Error, .error = "keyboard record has an invalid value"};

    EKeyState state = EKeyState::Released;
    if (std::string(stateName) == "PRESSED" && stateCode == 1)
        state = EKeyState::Pressed;
    else if (std::string(stateName) == "RELEASED" && stateCode == 0)
        state = EKeyState::Released;
    else
        return {.status = EParseStatus::Error, .error = "keyboard state fields disagree"};

    return {
        .status = EParseStatus::Event,
        .event = {.code = static_cast<uint16_t>(keyCode), .state = state, .sourceTimestamp = timestamp},
    };
}

} // namespace hudkeys
