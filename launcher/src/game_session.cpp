#include "game_session.hpp"

#include <chrono>
#include <cstdio>
#include <sstream>

namespace sb2 {

GameSession::~GameSession() {
    if (proc_ && proc_->running()) {
        proc_->writeStdin("stop\n");
        for (int i = 0; i < 30 && proc_->running(); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(100));
        if (proc_->running()) proc_->kill();
    }
    if (thread_.joinable()) thread_.join();
}

bool GameSession::start(const std::vector<std::string>& args, const std::string& logFile, std::string& err) {
    Process::Options o;
    o.args = args;
    o.captureOutput = true;
    o.pipeStdin = true;
    o.hideWindow = false;
    proc_ = Process::start(o, err);
    if (!proc_) return false;
    logFile_ = logFile;
    if (!logFile_.empty()) {
        std::string header = "# ";
        for (auto& a : args) header += a + " ";
        writeTextFile(logFile_, header + "\n");
    }
    thread_ = std::thread(&GameSession::reader, this);
    return true;
}

void GameSession::reader() {
    std::string buf;
    FILE* lf = nullptr;
    if (!logFile_.empty()) lf = std::fopen(logFile_.c_str(), "ab");
    while (proc_->readSome(buf)) {
        size_t pos;
        while ((pos = buf.find('\n')) != std::string::npos) {
            std::string line = buf.substr(0, pos);
            buf.erase(0, pos + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind("status: ", 0) == 0) {
                double v = 0, s = 0, f = 0;
                int p = 0;
                if (std::sscanf(line.c_str(), "status: vps=%lf speed=%lf fps=%lf paused=%d", &v, &s, &f, &p) == 4) {
                    vps_ = v;
                    speed_ = s;
                    fps_ = f;
                    paused_ = p != 0;
                }
                continue; // not interesting for the log
            }
            if (lf) {
                std::fwrite(line.data(), 1, line.size(), lf);
                std::fputc('\n', lf);
                std::fflush(lf);
            }
            std::lock_guard<std::mutex> lk(mutex_);
            lines_.push_back(std::move(line));
            if (lines_.size() > 5000) lines_.pop_front();
        }
    }
    if (lf) std::fclose(lf);
}

void GameSession::sendCommand(const std::string& cmd) {
    if (proc_ && proc_->running()) proc_->writeStdin(cmd + "\n");
}

void GameSession::pause() {
    sendCommand("pause");
    paused_ = true;
}

void GameSession::resume() {
    sendCommand("resume");
    paused_ = false;
}

void GameSession::stop() {
    if (!proc_) return;
    sendCommand("stop");
    stopRequested_ = true;
    stopAt_ = std::chrono::steady_clock::now();
}

bool GameSession::running() {
    if (!proc_) return false;
    if (stopRequested_ && proc_->running() && std::chrono::steady_clock::now() - stopAt_ > std::chrono::seconds(6)) proc_->kill();
    return proc_->running();
}

bool GameSession::finished(int& code) {
    if (!proc_) return false;
    if (exited_) {
        code = exitCode_;
        return true;
    }
    int c;
    if (!proc_->tryWait(c)) return false;
    if (thread_.joinable()) thread_.join();
    exited_ = true;
    exitCode_ = c;
    code = c;
    return true;
}

std::vector<std::string> GameSession::logTail(size_t n) {
    std::lock_guard<std::mutex> lk(mutex_);
    const size_t start = lines_.size() > n ? lines_.size() - n : 0;
    return std::vector<std::string>(lines_.begin() + (ptrdiff_t)start, lines_.end());
}

std::string GameSession::logText() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::string s;
    for (auto& l : lines_) s += l + "\n";
    return s;
}

} // namespace sb2
