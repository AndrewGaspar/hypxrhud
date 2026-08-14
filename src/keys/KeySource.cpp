#include "KeySource.hpp"

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <cstdint>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

namespace hudkeys {
namespace {
    bool moveAboveStdio(int& fd) {
        if (fd > STDERR_FILENO)
            return true;
        const int moved = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        if (moved < 0)
            return false;
        close(fd);
        fd = moved;
        return true;
    }
}

CKeySource::~CKeySource() {
    stop();
}

bool CKeySource::start(std::string& error) {
    return startArgv({"/usr/bin/showmethekey-cli"}, error);
}

bool CKeySource::startForTesting(const std::vector<std::string>& argv, std::string& error) {
    return startArgv(argv, error);
}

bool CKeySource::startArgv(const std::vector<std::string>& argv, std::string& error) {
    if (m_pid > 0 || m_fd >= 0) {
        error = "key source already started";
        return false;
    }
    if (argv.empty() || argv.front().empty() || argv.front().front() != '/') {
        error = "key source executable must be an absolute path";
        return false;
    }

    int pipeFds[2] = {-1, -1};
    if (pipe2(pipeFds, O_CLOEXEC) < 0) {
        error = std::string("cannot create source pipe: ") + std::strerror(errno);
        return false;
    }
    const int readFlags = fcntl(pipeFds[0], F_GETFL);
    if (readFlags < 0 || fcntl(pipeFds[0], F_SETFL, readFlags | O_NONBLOCK) < 0) {
        error = std::string("cannot make source pipe nonblocking: ") + std::strerror(errno);
        close(pipeFds[0]);
        close(pipeFds[1]);
        return false;
    }
    if (!moveAboveStdio(pipeFds[0]) || !moveAboveStdio(pipeFds[1])) {
        error = std::string("cannot isolate source pipe descriptors: ") + std::strerror(errno);
        close(pipeFds[0]);
        close(pipeFds[1]);
        return false;
    }

    int statusFds[2] = {-1, -1};
    if (pipe2(statusFds, O_CLOEXEC) < 0) {
        error = std::string("cannot create source status pipe: ") + std::strerror(errno);
        close(pipeFds[0]);
        close(pipeFds[1]);
        return false;
    }
    if (!moveAboveStdio(statusFds[0]) || !moveAboveStdio(statusFds[1])) {
        error = std::string("cannot isolate source status descriptors: ") + std::strerror(errno);
        close(pipeFds[0]);
        close(pipeFds[1]);
        close(statusFds[0]);
        close(statusFds[1]);
        return false;
    }

    const pid_t expectedParent = getpid();

    const pid_t child = fork();
    if (child < 0) {
        error = std::string("cannot fork key source: ") + std::strerror(errno);
        close(pipeFds[0]);
        close(pipeFds[1]);
        close(statusFds[0]);
        close(statusFds[1]);
        return false;
    }

    if (child == 0) {
        close(statusFds[0]);
        auto failSetup = [&](uint8_t stage) {
            while (write(statusFds[1], &stage, sizeof(stage)) < 0 && errno == EINTR) { /* report */ }
            _exit(127);
        };
        // The source belongs to exactly this client. Parent death and explicit termination
        // cannot leave an input-capturing orphan behind.
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) < 0)
            failSetup(1);
        if (getppid() != expectedParent)
            failSetup(2);
        if (dup2(pipeFds[1], STDOUT_FILENO) < 0)
            failSetup(3);
        const int nullFd = open("/dev/null", O_RDWR | O_CLOEXEC);
        if (nullFd < 0)
            failSetup(4);
        if (dup2(nullFd, STDIN_FILENO) < 0 || dup2(nullFd, STDERR_FILENO) < 0)
            failSetup(5);
        close(pipeFds[0]);
        if (pipeFds[1] != STDOUT_FILENO)
            close(pipeFds[1]);
        if (nullFd > STDERR_FILENO)
            close(nullFd);

        std::vector<char*> execArgv;
        execArgv.reserve(argv.size() + 1);
        for (const auto& value : argv)
            execArgv.push_back(const_cast<char*>(value.c_str()));
        execArgv.push_back(nullptr);
        execv(argv.front().c_str(), execArgv.data());
        failSetup(6);
    }

    close(statusFds[1]);
    close(pipeFds[1]);

    pollfd statusPoll = {.fd = statusFds[0], .events = POLLIN | POLLHUP, .revents = 0};
    int pollResult = -1;
    do {
        pollResult = poll(&statusPoll, 1, 2000);
    } while (pollResult < 0 && errno == EINTR);

    uint8_t stage = 0;
    const ssize_t statusBytes = pollResult > 0 ? read(statusFds[0], &stage, sizeof(stage)) : -1;
    close(statusFds[0]);
    if (pollResult <= 0 || statusBytes != 0) {
        if (pollResult == 0)
            error = "key source setup timed out";
        else if (statusBytes > 0)
            error = "key source containment or exec setup failed (stage " + std::to_string(stage) + ")";
        else
            error = std::string("cannot confirm key source setup: ") + std::strerror(errno);
        close(pipeFds[0]);
        kill(child, SIGKILL);
        int status = 0;
        while (waitpid(child, &status, 0) < 0 && errno == EINTR) { /* reap */ }
        return false;
    }

    m_fd = pipeFds[0];
    m_pid = child;
    return true;
}

bool CKeySource::readAvailable(const LineCallback& callback, std::string& error) {
    if (m_fd < 0)
        return false;

    char chunk[4096];
    for (;;) {
        const ssize_t bytes = read(m_fd, chunk, sizeof(chunk));
        if (bytes > 0) {
            m_buffer.append(chunk, static_cast<size_t>(bytes));
            if (m_buffer.size() > 65536) {
                error = "key source record exceeds 64 KiB";
                m_buffer.clear();
                return false;
            }
            size_t newline = 0;
            while ((newline = m_buffer.find('\n')) != std::string::npos) {
                std::string line = m_buffer.substr(0, newline);
                m_buffer.erase(0, newline + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (!line.empty())
                    callback(line);
            }
            continue;
        }
        if (bytes == 0) {
            if (!m_buffer.empty()) {
                error = "key source ended with a truncated record";
                m_buffer.clear();
                return false;
            }
            return true;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return true;
        if (errno == EINTR)
            continue;
        error = std::string("cannot read key source: ") + std::strerror(errno);
        return false;
    }
}

bool CKeySource::exited(int& status) {
    if (m_pid <= 0)
        return true;
    const pid_t result = waitpid(m_pid, &status, WNOHANG);
    if (result == 0)
        return false;
    if (result == m_pid || (result < 0 && errno == ECHILD)) {
        m_pid = -1;
        return true;
    }
    return false;
}

void CKeySource::stop() {
    if (m_fd >= 0) {
        close(m_fd);
        m_fd = -1;
    }
    m_buffer.clear();
    if (m_pid <= 0)
        return;

    kill(m_pid, SIGTERM);
    int status = 0;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
    while (std::chrono::steady_clock::now() < deadline) {
        const pid_t result = waitpid(m_pid, &status, WNOHANG);
        if (result == m_pid || (result < 0 && errno == ECHILD)) {
            m_pid = -1;
            return;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    kill(m_pid, SIGKILL);
    while (waitpid(m_pid, &status, 0) < 0 && errno == EINTR) { /* reap exact child */ }
    m_pid = -1;
}

} // namespace hudkeys
