#include "library.hpp"

#include <algorithm>

#include "sb2/json.hpp"
#include "sb2/platform.hpp"

namespace sb2 {

Library::Library() {
    path_ = joinPath(userDataDir(), "library.json");
    cachePath_ = joinPath(userDataDir(), "scan_cache.json");
}

Library::~Library() {
    cancel_ = true;
    if (thread_.joinable()) thread_.join();
}

void Library::load() {
    std::string err;
    const Json j = Json::load(path_, &err);
    std::lock_guard<std::mutex> lk(mutex_);
    folders_.clear();
    files_.clear();
    games_.clear();
    if (!err.empty() || !j.isObject()) {
        // First start: look next to the executable and in a "games" folder.
        folders_.push_back(joinPath(exeDir(), "games"));
        return;
    }
    for (const auto& f : j.get("folders").asArray()) folders_.push_back(f.asString());
    for (const auto& f : j.get("files").asArray()) files_.push_back(f.asString());
    for (const auto& g : j.get("games").asArray()) games_.push_back(GameEntry::fromJson(g));
}

void Library::save() {
    Json j;
    std::lock_guard<std::mutex> lk(mutex_);
    Json& f = j["folders"];
    f.arr();
    for (auto& s : folders_) f.push(s);
    Json& fl = j["files"];
    fl.arr();
    for (auto& s : files_) fl.push(s);
    Json& g = j["games"];
    g.arr();
    for (auto& e : games_) g.push(e.toJson());
    j.save(path_);
}

void Library::addFolder(const std::string& dir) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (std::find(folders_.begin(), folders_.end(), dir) != folders_.end()) return;
        folders_.push_back(dir);
    }
    save();
}

void Library::removeFolder(size_t i) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (i < folders_.size()) folders_.erase(folders_.begin() + (ptrdiff_t)i);
    }
    save();
}

void Library::addFile(const std::string& path) {
    {
        std::lock_guard<std::mutex> lk(mutex_);
        if (std::find(files_.begin(), files_.end(), path) == files_.end()) files_.push_back(path);
    }
    save();
}

std::vector<std::string> Library::folders() {
    std::lock_guard<std::mutex> lk(mutex_);
    return folders_;
}

std::vector<GameEntry> Library::games() {
    std::lock_guard<std::mutex> lk(mutex_);
    return games_;
}

std::string Library::scanStatus() {
    std::lock_guard<std::mutex> lk(mutex_);
    return status_;
}

void Library::sort() {
    std::lock_guard<std::mutex> lk(mutex_);
    std::stable_sort(games_.begin(), games_.end(), [](const GameEntry& a, const GameEntry& b) {
        auto rank = [](const GameEntry& g) { return g.target ? 0 : (g.valid && g.known ? 1 : (g.valid ? 2 : 3)); };
        if (rank(a) != rank(b)) return rank(a) < rank(b);
        return a.fileName < b.fileName;
    });
}

void Library::startScan() {
    if (scanning_) return;
    if (thread_.joinable()) thread_.join();
    scanning_ = true;
    cancel_ = false;
    std::vector<std::string> dirs, files;
    {
        std::lock_guard<std::mutex> lk(mutex_);
        dirs = folders_;
        files = files_;
    }
    thread_ = std::thread(&Library::scanThread, this, dirs, files);
}

void Library::scanThread(std::vector<std::string> dirs, std::vector<std::string> files) {
    ScanOptions opt;
    opt.cacheFile = cachePath_;
    opt.cancel = &cancel_;
    opt.progress = [this](const std::string& p, size_t i, size_t n) {
        std::lock_guard<std::mutex> lk(mutex_);
        status_ = std::to_string(i + 1) + "/" + std::to_string(n) + "  " + p;
    };
    std::vector<std::string> all = dirs;
    for (auto& f : files)
        if (fileExists(f)) all.push_back(f);
    std::vector<std::string> existing;
    for (auto& d : all)
        if (fileExists(d)) existing.push_back(d);
    auto found = scanFolders(existing, opt);
    {
        std::lock_guard<std::mutex> lk(mutex_);
        games_ = std::move(found);
        status_.clear();
    }
    sort();
    save();
    scanning_ = false;
}

} // namespace sb2
