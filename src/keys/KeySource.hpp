#pragma once

#include <functional>
#include <string>
#include <vector>

#include <sys/types.h>

namespace hudkeys {

class CKeySource {
  public:
    using LineCallback = std::function<void(const std::string&)>;

    CKeySource() = default;
    ~CKeySource();
    CKeySource(const CKeySource&) = delete;
    CKeySource& operator=(const CKeySource&) = delete;

    // Production entry point: the absolute executable is intentionally not configurable.
    bool start(std::string& error);

    // Dependency-injection seam for a compiled, non-input fixture. Production code never
    // exposes this argv through its CLI or config.
    bool startForTesting(const std::vector<std::string>& argv, std::string& error);

    int  fd() const { return m_fd; }
    pid_t pid() const { return m_pid; }
    bool readAvailable(const LineCallback& callback, std::string& error);
    bool exited(int& status);
    void stop();

  private:
    bool startArgv(const std::vector<std::string>& argv, std::string& error);

    int         m_fd = -1;
    pid_t       m_pid = -1;
    std::string m_buffer;
};

} // namespace hudkeys
