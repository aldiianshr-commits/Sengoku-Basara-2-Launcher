// Game library: scanned folders, detected disc images, persisted to
// <user data>/library.json, with an incremental scan cache.
#pragma once
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "sb2/scanner.hpp"

namespace sb2 {

class Library {
public:
    Library();
    ~Library();
    void load();
    void save();
    void addFolder(const std::string& dir);
    void removeFolder(size_t index);
    void addFile(const std::string& path);
    void startScan();
    bool scanning() const { return scanning_; }
    std::string scanStatus();
    std::vector<std::string> folders();
    std::vector<GameEntry> games();
    // Sorted: target title first, then valid known, then the rest.
    void sort();

private:
    void scanThread(std::vector<std::string> dirs, std::vector<std::string> files);
    std::string path_, cachePath_;
    std::mutex mutex_;
    std::vector<std::string> folders_;
    std::vector<std::string> files_;
    std::vector<GameEntry> games_;
    std::thread thread_;
    std::atomic<bool> scanning_{false};
    std::atomic<bool> cancel_{false};
    std::string status_;
};

} // namespace sb2
