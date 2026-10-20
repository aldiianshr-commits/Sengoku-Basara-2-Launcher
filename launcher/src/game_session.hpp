// A running game executable started by the launcher: output capture, status
// parsing and Pause / Resume / Stop over the stdin control channel.
#pragma once
#include <atomic>
#include <chrono>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sb2/platform.hpp"

namespace sb2 {

class GameSession {
public:
    ~GameSession();
    bool start(const std::vector<std::string>& args, const std::string& logFile, std::string& err);
    void pause();
    void resume();
    void stop();
    void sendCommand(const std::string& cmd);
    bool running();
    bool finished(int& exitCode);
    bool paused() const { return paused_; }
    double vps() const { return vps_; }
    double speed() const { return speed_; }
    double fps() const { return fps_; }
    std::vector<std::string> logTail(size_t n);
    std::string logText();

private:
    void reader();
    std::unique_ptr<Process> proc_;
    std::thread thread_;
    std::mutex mutex_;
    std::deque<std::string> lines_;
    std::string logFile_;
    std::atomic<bool> paused_{false};
    std::atomic<double> vps_{0}, speed_{0}, fps_{0};
    bool stopRequested_ = false;
    std::chrono::steady_clock::time_point stopAt_{};
    bool exited_ = false;
    int exitCode_ = 0;
};

} // namespace sb2
