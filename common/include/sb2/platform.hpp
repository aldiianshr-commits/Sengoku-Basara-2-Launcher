// Small platform layer: well-known directories and child processes with
// redirected standard streams (used by the launcher to run the bundled
// compiler and to control the game process).
#pragma once
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace sb2 {

// Directory containing the running executable (UTF-8, no trailing slash).
std::string exeDir();
// Per-user writable data directory.  Portable mode (a file named
// "portable.txt" next to the executable) keeps everything in <exe>/user.
std::string userDataDir();
// Join with '/' (accepted by Windows APIs as well).
std::string joinPath(const std::string& a, const std::string& b);
bool fileExists(const std::string& path);
bool isDirectory(const std::string& path);
bool makeDirs(const std::string& path);
uint64_t fileSize(const std::string& path);
int64_t fileMTime(const std::string& path);
bool readFile(const std::string& path, std::vector<uint8_t>& out);
bool readTextFile(const std::string& path, std::string& out);
bool writeFile(const std::string& path, const void* data, size_t size);
bool writeTextFile(const std::string& path, const std::string& text);
bool copyFile(const std::string& from, const std::string& to);
bool removeTree(const std::string& path);
int cpuThreads();
uint64_t totalMemoryBytes();
std::string osDescription();
// Open a folder or URL with the shell.
void openInShell(const std::string& pathOrUrl);

class Process {
public:
    struct Options {
        std::vector<std::string> args; // args[0] = program path
        std::string workDir;
        bool captureOutput = true;     // stdout+stderr merged into a pipe
        bool pipeStdin = false;        // writable stdin
        bool hideWindow = true;        // no console window for console programs (Windows)
        std::vector<std::pair<std::string, std::string>> env; // extra variables
    };
    ~Process();
    static std::unique_ptr<Process> start(const Options& o, std::string& err);

    // Reads available output (blocks until some data or EOF). Returns false at EOF.
    bool readSome(std::string& out);
    // Reads everything until EOF.
    std::string readAll();
    bool writeStdin(const std::string& s);
    void closeStdin();
    bool running();
    // Waits for exit; returns exit code (or -1).
    int wait();
    // Returns true and the exit code if the process has exited.
    bool tryWait(int& code);
    void kill();
    uint32_t pid() const;

private:
    Process() = default;
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Runs a program to completion and captures its output.
int runProcess(const std::vector<std::string>& args, std::string& output, const std::string& workDir = {});

} // namespace sb2
