#include "Config.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace hud {
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
    bool asFloat(const std::string& v, double& out) {
        if (v.empty()) return false;
        char*  end = nullptr;
        double r   = std::strtod(v.c_str(), &end);
        if (end == v.c_str() || *end != '\0') return false;
        out = r;
        return true;
    }
    bool parseVec3(const std::string& s, float& x, float& y, float& z) {
        return std::sscanf(s.c_str(), "%f,%f,%f", &x, &y, &z) == 3;
    }
    bool asBool(const std::string& v, bool& out) {
        if (v == "true" || v == "1" || v == "yes" || v == "on")   { out = true;  return true; }
        if (v == "false" || v == "0" || v == "no" || v == "off")  { out = false; return true; }
        return false;
    }
    // The theme-override role keys accepted under [theme] (per-role hex colours, WP-H6).
    bool isThemeRole(const std::string& k) {
        return k == "normal" || k == "dim" || k == "accent" || k == "good" || k == "warn" ||
               k == "bad" || k == "panel_bg" || k == "bar_track";
    }
}

void SConfig::applySlots(CSlots& registry) const {
    for (const auto& [name, ov] : slots) {
        SSlot* s = registry.findMut(name);
        if (!s)
            continue; // unknown slot name; ignored (warned at parse time).
        if (ov.hasPose)  { s->px = ov.px; s->py = ov.py; s->pz = ov.pz; }
        if (ov.hasSize)  s->sizeW = ov.sizeW;
        if (ov.hasSpace) s->space = ov.space;
        if (ov.hasMax)   s->maxStack = ov.max;
        if (ov.hasRefuse) s->onRefuse = ov.refuseQueue ? ERefusePolicy::Queue : ERefusePolicy::Refuse;
    }
}

bool parseConfig(const std::string& text, SConfig& out, std::vector<std::string>& errors,
                 std::vector<std::string>& warnings) {
    std::istringstream in(text);
    std::string        line, section;
    int                lineNo = 0;

    auto err  = [&](const std::string& m) { errors.push_back("line " + std::to_string(lineNo) + ": " + m); };
    auto warn = [&](const std::string& m) { warnings.push_back("line " + std::to_string(lineNo) + ": " + m); };

    auto setStr = [&](std::string& dst, const std::string& v) {
        std::string s;
        if (asString(v, s)) { dst = s; return true; }
        err("expected a quoted string");
        return false;
    };
    auto setInt = [&](int& dst, const std::string& v) {
        long l;
        if (asInt(v, l)) { dst = static_cast<int>(l); return true; }
        err("expected an integer");
        return false;
    };
    auto setFloat = [&](float& dst, const std::string& v) {
        double d;
        if (asFloat(v, d)) { dst = static_cast<float>(d); return true; }
        err("expected a number");
        return false;
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

        if (section == "hud") {
            if (key == "z")                   setInt(out.hudZ, val);
            else if (key == "gpu")            setStr(out.gpu, val);
            else if (key == "opacity")        setFloat(out.opacity, val);
            else if (key == "blend_mode") {
                std::string v;
                if (!setStr(v, val)) continue;
                if (v == "opaque" || v == "alpha" || v == "additive") out.blendMode = v;
                else err("hud.blend_mode must be opaque|alpha|additive");
            }
            else if (key == "per_client_cap") setInt(out.perClientCap, val);
            else if (key == "tex_w")          setInt(out.texW, val);
            else if (key == "tex_h")          setInt(out.texH, val);
            else if (key == "rise_ms")        setInt(out.riseMs, val);
            else if (key == "hold_ms")        setInt(out.holdMs, val);
            else if (key == "fade_ms")        setInt(out.fadeMs, val);
            else if (key == "reprobe_base_ms") setInt(out.reprobeBaseMs, val);
            else if (key == "reprobe_cap_ms")  setInt(out.reprobeCapMs, val);
            else warn("unknown key 'hud." + key + "' (ignored)");
        } else if (section == "theme") {
            if (key == "follow") {
                // Accept a bare or quoted boolean: follow = true  /  follow = "false".
                std::string v = val;
                asString(val, v); // strip quotes if present (leaves v unchanged otherwise).
                if (!asBool(v, out.themeFollow))
                    err("theme.follow must be true|false");
            } else if (key == "file") {
                setStr(out.themeFile, val);
            } else if (isThemeRole(key)) {
                std::string v;
                if (setStr(v, val)) out.colorOverrides[key] = v; // hex validated at resolve time.
            } else {
                warn("unknown key 'theme." + key + "' (ignored)");
            }
        } else if (section.rfind("slot.", 0) == 0) {
            std::string name = section.substr(5);
            auto&       ov   = out.slots[name];
            if (key == "pose") {
                std::string v;
                if (!setStr(v, val)) continue;
                if (parseVec3(v, ov.px, ov.py, ov.pz)) ov.hasPose = true;
                else err("slot pose must be \"x,y,z\"");
            } else if (key == "size") {
                if (setFloat(ov.sizeW, val)) ov.hasSize = true;
            } else if (key == "space") {
                if (setStr(ov.space, val)) ov.hasSpace = true;
            } else if (key == "max") {
                if (setInt(ov.max, val)) ov.hasMax = true;
            } else if (key == "on_refuse") {
                std::string v;
                if (!setStr(v, val)) continue;
                if (v == "queue")       { ov.hasRefuse = true; ov.refuseQueue = true; }
                else if (v == "refuse") { ov.hasRefuse = true; ov.refuseQueue = false; }
                else err("slot on_refuse must be refuse|queue");
            } else if (key == "opacity") {
                if (setFloat(ov.opacity, val)) ov.hasOpacity = true;
            } else {
                warn("unknown key '" + section + "." + key + "' (ignored)");
            }
        } else {
            warn("unknown section '[" + section + "]' key '" + key + "' (ignored)");
        }
    }
    return errors.empty();
}

bool loadConfigFile(const std::string& path, SConfig& out, std::vector<std::string>& errors,
                    std::vector<std::string>& warnings) {
    std::ifstream f(path);
    if (!f) {
        warnings.push_back("config file '" + path + "' not found; using defaults");
        return true;
    }
    std::stringstream ss;
    ss << f.rdbuf();
    return parseConfig(ss.str(), out, errors, warnings);
}

std::string defaultConfigPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/hypxrhud/hypxrhud.toml";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config/hypxrhud/hypxrhud.toml";
    return "hypxrhud.toml";
}

} // namespace hud
