#include "BatteryConfig.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hudbat {
namespace {
    std::string trim(const std::string& s) {
        size_t a = s.find_first_not_of(" \t\r\n");
        if (a == std::string::npos)
            return "";
        size_t b = s.find_last_not_of(" \t\r\n");
        return s.substr(a, b - a + 1);
    }
    std::string stripComment(const std::string& s) {
        bool inStr = false;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '"')
                inStr = !inStr;
            else if (s[i] == '#' && !inStr)
                return s.substr(0, i);
        }
        return s;
    }
    bool asString(const std::string& v, std::string& out) {
        if (v.size() >= 2 && v.front() == '"' && v.back() == '"') {
            out = v.substr(1, v.size() - 2);
            return true;
        }
        return false;
    }
    bool asInt(const std::string& v, long& out) {
        if (v.empty()) return false;
        char* end = nullptr;
        long  r   = std::strtol(v.c_str(), &end, 10);
        if (end == v.c_str() || *end != '\0') return false;
        out = r;
        return true;
    }
    bool asBool(const std::string& v, bool& out) {
        if (v == "true")  { out = true;  return true; }
        if (v == "false") { out = false; return true; }
        return false;
    }
}

bool parseBatteryConfig(const std::string& text, SBatteryConfig& out,
                        std::vector<std::string>& errors, std::vector<std::string>& warnings) {
    std::istringstream in(text);
    std::string        line, section;
    int                lineNo = 0;

    auto err  = [&](const std::string& m) { errors.push_back("line " + std::to_string(lineNo) + ": " + m); };
    auto warn = [&](const std::string& m) { warnings.push_back("line " + std::to_string(lineNo) + ": " + m); };

    auto setInt = [&](int& dst, const std::string& v) {
        long l;
        if (asInt(v, l)) { dst = (int)l; return; }
        err("expected an integer");
    };
    auto setBool = [&](bool& dst, const std::string& v) {
        bool b;
        if (asBool(v, b)) { dst = b; return; }
        err("expected true|false");
    };
    auto setStr = [&](std::string& dst, const std::string& v) {
        std::string s;
        if (asString(v, s)) { dst = s; return; }
        err("expected a quoted string");
    };

    while (std::getline(in, line)) {
        lineNo++;
        std::string s = trim(stripComment(line));
        if (s.empty())
            continue;
        if (s.front() == '[') {
            if (s.back() != ']') { err("malformed section header"); continue; }
            section = trim(s.substr(1, s.size() - 2));
            continue;
        }
        size_t eq = s.find('=');
        if (eq == std::string::npos) { err("expected key = value"); continue; }
        std::string key = trim(s.substr(0, eq));
        std::string val = trim(s.substr(eq + 1));

        if (section == "battery") {
            if (key == "poll_interval_sec")   setInt(out.pollIntervalSec, val);
            else if (key == "low_threshold")  setInt(out.lowThreshold, val);
            else if (key == "low_hysteresis") setInt(out.lowHysteresis, val);
            else if (key == "headset")        setBool(out.showHeadset, val);
            else if (key == "laptop")         setBool(out.showLaptop, val);
            else if (key == "headset_label")  setStr(out.headsetLabel, val);
            else if (key == "laptop_label")   setStr(out.laptopLabel, val);
            else if (key == "slot")           setStr(out.slot, val);
            else if (key == "toasts")         setBool(out.toasts, val);
            else warn("unknown key 'battery." + key + "' (ignored)");
        } else {
            warn("unknown section '[" + section + "]' key '" + key + "' (ignored)");
        }
    }
    return errors.empty();
}

bool loadBatteryConfigFile(const std::string& path, SBatteryConfig& out,
                           std::vector<std::string>& errors, std::vector<std::string>& warnings) {
    std::ifstream f(path);
    if (!f) {
        warnings.push_back("config file '" + path + "' not found; using defaults");
        return true;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parseBatteryConfig(ss.str(), out, errors, warnings);
}

std::string defaultBatteryConfigPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/hypxrhud/battery.toml";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config/hypxrhud/battery.toml";
    return "battery.toml";
}

} // namespace hudbat
