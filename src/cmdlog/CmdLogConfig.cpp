#include "CmdLogConfig.hpp"

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace hudcmd {
namespace {
    std::string trim(const std::string& value) {
        const size_t first = value.find_first_not_of(" \t\r\n");
        if (first == std::string::npos)
            return {};
        return value.substr(first, value.find_last_not_of(" \t\r\n") - first + 1);
    }

    std::string stripComment(const std::string& value) {
        bool quoted = false;
        for (size_t i = 0; i < value.size(); ++i) {
            if (value[i] == '"')
                quoted = !quoted;
            else if (value[i] == '#' && !quoted)
                return value.substr(0, i);
        }
        return value;
    }

    bool asString(const std::string& value, std::string& out) {
        if (value.size() < 2 || value.front() != '"' || value.back() != '"')
            return false;
        out = value.substr(1, value.size() - 2);
        return true;
    }

    bool asInt(const std::string& value, int& out) {
        if (value.empty())
            return false;
        char* end = nullptr;
        errno = 0;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (end == value.c_str() || *end != '\0' || errno == ERANGE || parsed < INT_MIN || parsed > INT_MAX)
            return false;
        out = static_cast<int>(parsed);
        return true;
    }

    bool asDouble(const std::string& value, double& out) {
        if (value.empty())
            return false;
        char* end = nullptr;
        errno = 0;
        out = std::strtod(value.c_str(), &end);
        return end != value.c_str() && *end == '\0' && errno != ERANGE && std::isfinite(out);
    }
}

bool parseCmdLogConfig(const std::string& text, SCmdLogConfig& out,
                       std::vector<std::string>& errors, std::vector<std::string>& warnings) {
    std::istringstream input(text);
    std::string        line, section;
    int                lineNo = 0;

    auto error   = [&](const std::string& message) { errors.push_back("line " + std::to_string(lineNo) + ": " + message); };
    auto warning = [&](const std::string& message) { warnings.push_back("line " + std::to_string(lineNo) + ": " + message); };
    auto setString = [&](std::string& target, const std::string& value) {
        if (!asString(value, target))
            error("expected a quoted string");
    };
    auto setInt = [&](int& target, const std::string& value) {
        if (!asInt(value, target))
            error("expected an integer");
    };

    while (std::getline(input, line)) {
        ++lineNo;
        const std::string clean = trim(stripComment(line));
        if (clean.empty())
            continue;
        if (clean.front() == '[') {
            if (clean.back() != ']') {
                error("malformed section header");
                continue;
            }
            section = trim(clean.substr(1, clean.size() - 2));
            continue;
        }

        const size_t equals = clean.find('=');
        if (equals == std::string::npos) {
            error("expected key = value");
            continue;
        }
        const std::string key   = trim(clean.substr(0, equals));
        const std::string value = trim(clean.substr(equals + 1));
        if (section != "cmd") {
            warning("unknown section '[" + section + "]' key '" + key + "' (ignored)");
            continue;
        }

        if (key == "slot") setString(out.slot, value);
        else if (key == "history") setInt(out.history, value);
        else if (key == "ttl_ms") setInt(out.ttlMs, value);
        else if (key == "max_chars") setInt(out.maxChars, value);
        else if (key == "coalesce_ms") setInt(out.coalesceMs, value);
        else if (key == "rise_ms") setInt(out.riseMs, value);
        else if (key == "fade_ms") setInt(out.fadeMs, value);
        else if (key == "opacity") {
            if (!asDouble(value, out.opacity))
                error("expected a number");
        } else
            warning("unknown key 'cmd." + key + "' (ignored)");
    }

    if (out.slot.empty()) errors.push_back("cmd.slot must not be empty");
    if (out.history < 1 || out.history > 8) errors.push_back("cmd.history must be in [1,8]");
    if (out.ttlMs < 500 || out.ttlMs > 60000) errors.push_back("cmd.ttl_ms must be in [500,60000]");
    if (out.maxChars < 16 || out.maxChars > 200) errors.push_back("cmd.max_chars must be in [16,200]");
    if (out.coalesceMs < 0 || out.coalesceMs > 10000) errors.push_back("cmd.coalesce_ms must be in [0,10000]");
    if (out.riseMs < 0 || out.riseMs > 5000 || out.fadeMs < 0 || out.fadeMs > 5000)
        errors.push_back("cmd rise_ms/fade_ms must be in [0,5000]");
    if (out.opacity < 0.2 || out.opacity > 1.0) errors.push_back("cmd.opacity must be in [0.2,1]");
    return errors.empty();
}

bool loadCmdLogConfigFile(const std::string& path, SCmdLogConfig& out,
                          std::vector<std::string>& errors, std::vector<std::string>& warnings,
                          bool allowMissing) {
    std::error_code fsError;
    const bool      exists = std::filesystem::exists(path, fsError);
    if (fsError) {
        errors.push_back("cannot inspect config file '" + path + "': " + fsError.message());
        return false;
    }
    if (!exists) {
        if (allowMissing) {
            warnings.push_back("config file '" + path + "' not found; using defaults");
            return true;
        }
        errors.push_back("config file '" + path + "' does not exist");
        return false;
    }
    if (!std::filesystem::is_regular_file(path, fsError) || fsError) {
        errors.push_back("config path '" + path + "' is not a readable regular file");
        return false;
    }

    errno = 0;
    std::ifstream file(path);
    if (!file) {
        errors.push_back("cannot open config file '" + path + "': " + std::strerror(errno ? errno : EIO));
        return false;
    }
    std::stringstream contents;
    contents << file.rdbuf();
    if (file.bad()) {
        errors.push_back("cannot read config file '" + path + "'");
        return false;
    }
    return parseCmdLogConfig(contents.str(), out, errors, warnings);
}

std::string defaultCmdLogConfigPath() {
    if (const char* xdg = std::getenv("XDG_CONFIG_HOME"); xdg && *xdg)
        return std::string(xdg) + "/hypxrhud/cmd.toml";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::string(home) + "/.config/hypxrhud/cmd.toml";
    return "cmd.toml";
}

} // namespace hudcmd
