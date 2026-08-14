#pragma once

#include <string>
#include <vector>

namespace hudkeys {

struct SKeysConfig {
    std::string rules;
    std::string model;
    std::string layout = "us";
    std::string variant;
    std::string options;
    std::string slot = "keys";
    int         history = 3;
    int         coalesceMs = 900;
    int         riseMs = 50;
    int         privacyHoldMs = 1800;
    double      opacity = 0.92;
    bool        modsOnly = false;
};

bool parseKeysConfig(const std::string& text, SKeysConfig& out,
                     std::vector<std::string>& errors, std::vector<std::string>& warnings);
bool loadKeysConfigFile(const std::string& path, SKeysConfig& out,
                        std::vector<std::string>& errors, std::vector<std::string>& warnings,
                        bool allowMissing = true);
std::string defaultKeysConfigPath();

} // namespace hudkeys
