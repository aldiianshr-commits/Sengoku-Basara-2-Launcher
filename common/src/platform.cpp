#include "sb2/platform.hpp"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <system_error>
#include <thread>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/sysinfo.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace fs = std::filesystem;

namespace sb2 {

namespace {
fs::path P(const std::string& s) {
#if defined(_WIN32)
    return fs::path(std::u8string(s.begin(), s.end()));
#else
    return fs::path(s);
#endif
}
std::string S(const fs::path& p) {
    const std::u8string u = p.generic_u8string();
    return std::string(u.begin(), u.end());
}
#if defined(_WIN32)
std::wstring W(const std::string& s) {
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w((size_t)n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
std::string U(const std::wstring& w) {
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s((size_t)n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
#endif
} // namespace

std::string exeDir() {
#if defined(_WIN32)
    std::wstring buf(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, buf.data(), (DWORD)buf.size());
    buf.resize(n);
    return S(fs::path(buf).parent_path());
#else
    std::error_code ec;
    fs::path p = fs::read_symlink("/proc/self/exe", ec);
    if (ec) return ".";
    return S(p.parent_path());
#endif
}

std::string userDataDir() {
    static std::string cached;
    if (!cached.empty()) return cached;
    const std::string exe = exeDir();
    std::string dir;
    if (fileExists(joinPath(exe, "portable.txt"))) {
        dir = joinPath(exe, "user");
    } else {
#if defined(_WIN32)
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &path))) {
            dir = S(fs::path(path) / L"SB2HeroesRecompiled");
            CoTaskMemFree(path);
        } else {
            dir = joinPath(exe, "user");
        }
#else
        const char* xdg = std::getenv("XDG_DATA_HOME");
        const char* home = std::getenv("HOME");
        if (xdg && *xdg) dir = joinPath(xdg, "sb2heroes-recompiled");
        else if (home && *home) dir = joinPath(home, ".local/share/sb2heroes-recompiled");
        else dir = joinPath(exe, "user");
#endif
    }
    makeDirs(dir);
    cached = dir;
    return cached;
}

std::string joinPath(const std::string& a, const std::string& b) {
    if (a.empty()) return b;
    if (b.empty()) return a;
    const char last = a.back();
    if (last == '/' || last == '\\') return a + b;
    return a + "/" + b;
}

bool fileExists(const std::string& path) {
    std::error_code ec;
    return fs::exists(P(path), ec);
}

bool isDirectory(const std::string& path) {
    std::error_code ec;
    return fs::is_directory(P(path), ec);
}

bool makeDirs(const std::string& path) {
    std::error_code ec;
    fs::create_directories(P(path), ec);
    return fs::is_directory(P(path), ec);
}

uint64_t fileSize(const std::string& path) {
    std::error_code ec;
    const auto n = fs::file_size(P(path), ec);
    return ec ? 0 : (uint64_t)n;
}

int64_t fileMTime(const std::string& path) {
    std::error_code ec;
    const auto t = fs::last_write_time(P(path), ec);
    if (ec) return 0;
    return (int64_t)std::chrono::duration_cast<std::chrono::seconds>(t.time_since_epoch()).count();
}

bool readFile(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(P(path), std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n < 0) return false;
    f.seekg(0);
    out.resize((size_t)n);
    if (n) f.read((char*)out.data(), n);
    return (bool)f;
}

bool readTextFile(const std::string& path, std::string& out) {
    std::vector<uint8_t> d;
    if (!readFile(path, d)) return false;
    out.assign(d.begin(), d.end());
    return true;
}

bool writeFile(const std::string& path, const void* data, size_t size) {
    const fs::path p = P(path);
    std::error_code ec;
    if (p.has_parent_path()) fs::create_directories(p.parent_path(), ec);
    const fs::path tmp = p.string() + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) return false;
        if (size) f.write((const char*)data, (std::streamsize)size);
        if (!f) return false;
    }
    fs::rename(tmp, p, ec);
    if (ec) {
        fs::remove(p, ec);
        fs::rename(tmp, p, ec);
    }
    return !ec;
}

bool writeTextFile(const std::string& path, const std::string& text) { return writeFile(path, text.data(), text.size()); }

bool copyFile(const std::string& from, const std::string& to) {
    std::error_code ec;
    const fs::path t = P(to);
    if (t.has_parent_path()) fs::create_directories(t.parent_path(), ec);
    fs::copy_file(P(from), t, fs::copy_options::overwrite_existing, ec);
    return !ec;
}

bool removeTree(const std::string& path) {
    std::error_code ec;
    fs::remove_all(P(path), ec);
    return !ec;
}

int cpuThreads() {
    const unsigned n = std::thread::hardware_concurrency();
    return n ? (int)n : 2;
}

uint64_t totalMemoryBytes() {
#if defined(_WIN32)
    MEMORYSTATUSEX ms{};
    ms.dwLength = sizeof ms;
    if (GlobalMemoryStatusEx(&ms)) return ms.ullTotalPhys;
    return 0;
#else
    struct sysinfo si {};
    if (sysinfo(&si) == 0) return (uint64_t)si.totalram * si.mem_unit;
    return 0;
#endif
}

std::string osDescription() {
#if defined(_WIN32)
    // RtlGetVersion reports the real version regardless of manifest.
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW vi{};
    vi.dwOSVersionInfoSize = sizeof vi;
    if (HMODULE nt = GetModuleHandleW(L"ntdll.dll")) {
        if (auto fn = (RtlGetVersionFn)(void*)GetProcAddress(nt, "RtlGetVersion")) fn(&vi);
    }
    char b[96];
    std::snprintf(b, sizeof b, "Windows %s (build %lu)", vi.dwBuildNumber >= 22000 ? "11" : "10",
                  (unsigned long)vi.dwBuildNumber);
    return b;
#else
    struct utsname u {};
    if (uname(&u) == 0) return std::string(u.sysname) + " " + u.release;
    return "unknown";
#endif
}

void openInShell(const std::string& pathOrUrl) {
#if defined(_WIN32)
    ShellExecuteW(nullptr, L"open", W(pathOrUrl).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string out;
    runProcess({"xdg-open", pathOrUrl}, out);
#endif
}

// ------------------------------------------------------------------ Process

#if defined(_WIN32)

struct Process::Impl {
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE outRead = nullptr;
    HANDLE inWrite = nullptr;
    DWORD pid = 0;
    bool exited = false;
    int exitCode = -1;
};

static std::wstring quoteArg(const std::wstring& a) {
    if (!a.empty() && a.find_first_of(L" \t\n\v\"") == std::wstring::npos) return a;
    std::wstring r = L"\"";
    for (size_t i = 0;; ++i) {
        size_t bs = 0;
        while (i < a.size() && a[i] == L'\\') {
            ++i;
            ++bs;
        }
        if (i == a.size()) {
            r.append(bs * 2, L'\\');
            break;
        }
        if (a[i] == L'"') {
            r.append(bs * 2 + 1, L'\\');
            r.push_back(L'"');
        } else {
            r.append(bs, L'\\');
            r.push_back(a[i]);
        }
    }
    r.push_back(L'"');
    return r;
}

Process::~Process() {
    if (!impl_) return;
    if (impl_->inWrite) CloseHandle(impl_->inWrite);
    if (impl_->outRead) CloseHandle(impl_->outRead);
    if (impl_->thread) CloseHandle(impl_->thread);
    if (impl_->process) CloseHandle(impl_->process);
}

std::unique_ptr<Process> Process::start(const Options& o, std::string& err) {
    if (o.args.empty()) {
        err = "no program";
        return nullptr;
    }
    std::unique_ptr<Process> p(new Process());
    p->impl_ = std::make_unique<Impl>();
    SECURITY_ATTRIBUTES sa{sizeof sa, nullptr, TRUE};
    HANDLE outWrite = nullptr, inRead = nullptr, nulIn = nullptr;
    if (o.captureOutput) {
        if (!CreatePipe(&p->impl_->outRead, &outWrite, &sa, 1 << 16)) {
            err = "CreatePipe failed";
            return nullptr;
        }
        SetHandleInformation(p->impl_->outRead, HANDLE_FLAG_INHERIT, 0);
    }
    if (o.pipeStdin) {
        if (!CreatePipe(&inRead, &p->impl_->inWrite, &sa, 1 << 12)) {
            err = "CreatePipe failed";
            if (outWrite) CloseHandle(outWrite);
            return nullptr;
        }
        SetHandleInformation(p->impl_->inWrite, HANDLE_FLAG_INHERIT, 0);
    } else {
        nulIn = CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &sa, OPEN_EXISTING, 0, nullptr);
    }
    std::wstring cmd;
    for (size_t i = 0; i < o.args.size(); ++i) {
        if (i) cmd.push_back(L' ');
        cmd += quoteArg(W(o.args[i]));
    }
    STARTUPINFOEXW si{};
    si.StartupInfo.cb = sizeof si;
    HANDLE inherit[3];
    DWORD nInherit = 0;
    si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    si.StartupInfo.hStdInput = o.pipeStdin ? inRead : nulIn;
    si.StartupInfo.hStdOutput = o.captureOutput ? outWrite : GetStdHandle(STD_OUTPUT_HANDLE);
    si.StartupInfo.hStdError = o.captureOutput ? outWrite : GetStdHandle(STD_ERROR_HANDLE);
    if (si.StartupInfo.hStdInput) inherit[nInherit++] = si.StartupInfo.hStdInput;
    if (o.captureOutput) inherit[nInherit++] = outWrite;
    // Restrict inheritance to exactly these handles so that concurrently
    // started children never keep each other's pipes open.
    SIZE_T attrSize = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attrSize);
    std::vector<uint8_t> attrBuf(attrSize);
    si.lpAttributeList = (LPPROC_THREAD_ATTRIBUTE_LIST)attrBuf.data();
    bool attrOk = InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attrSize) &&
                  (nInherit == 0 || UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                                              inherit, nInherit * sizeof(HANDLE), nullptr, nullptr));
    std::wstring envBlock;
    if (!o.env.empty()) {
        LPWCH cur = GetEnvironmentStringsW();
        for (LPWCH e = cur; *e; e += wcslen(e) + 1) {
            envBlock.append(e);
            envBlock.push_back(L'\0');
        }
        FreeEnvironmentStringsW(cur);
        for (auto& [k, v] : o.env) {
            envBlock += W(k) + L"=" + W(v);
            envBlock.push_back(L'\0');
        }
        envBlock.push_back(L'\0');
    }
    DWORD flags = CREATE_UNICODE_ENVIRONMENT | (attrOk ? EXTENDED_STARTUPINFO_PRESENT : 0);
    if (o.hideWindow) flags |= CREATE_NO_WINDOW;
    PROCESS_INFORMATION pi{};
    const std::wstring wd = W(o.workDir);
    const BOOL ok = CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, nInherit ? TRUE : FALSE, flags,
                                   envBlock.empty() ? nullptr : (LPVOID)envBlock.data(), wd.empty() ? nullptr : wd.c_str(),
                                   &si.StartupInfo, &pi);
    if (attrOk) DeleteProcThreadAttributeList(si.lpAttributeList);
    if (outWrite) CloseHandle(outWrite);
    if (inRead) CloseHandle(inRead);
    if (nulIn) CloseHandle(nulIn);
    if (!ok) {
        char b[64];
        std::snprintf(b, sizeof b, "CreateProcess failed (error %lu)", (unsigned long)GetLastError());
        err = std::string(b) + " for " + o.args[0];
        return nullptr;
    }
    p->impl_->process = pi.hProcess;
    p->impl_->thread = pi.hThread;
    p->impl_->pid = pi.dwProcessId;
    return p;
}

bool Process::readSome(std::string& out) {
    if (!impl_->outRead) return false;
    char buf[4096];
    DWORD n = 0;
    if (!ReadFile(impl_->outRead, buf, sizeof buf, &n, nullptr) || n == 0) return false;
    out.append(buf, n);
    return true;
}

bool Process::writeStdin(const std::string& s) {
    if (!impl_->inWrite) return false;
    DWORD n = 0;
    return WriteFile(impl_->inWrite, s.data(), (DWORD)s.size(), &n, nullptr) && n == s.size();
}

void Process::closeStdin() {
    if (impl_->inWrite) CloseHandle(impl_->inWrite);
    impl_->inWrite = nullptr;
}

bool Process::tryWait(int& code) {
    if (impl_->exited) {
        code = impl_->exitCode;
        return true;
    }
    if (WaitForSingleObject(impl_->process, 0) != WAIT_OBJECT_0) return false;
    DWORD c = 0;
    GetExitCodeProcess(impl_->process, &c);
    impl_->exited = true;
    impl_->exitCode = (int)c;
    code = (int)c;
    return true;
}

int Process::wait() {
    if (!impl_->exited) {
        WaitForSingleObject(impl_->process, INFINITE);
        int c;
        tryWait(c);
    }
    return impl_->exitCode;
}

void Process::kill() {
    if (!impl_->exited) TerminateProcess(impl_->process, 1);
}

uint32_t Process::pid() const { return impl_->pid; }

#else

struct Process::Impl {
    pid_t pid = -1;
    int outFd = -1;
    int inFd = -1;
    bool exited = false;
    int exitCode = -1;
};

Process::~Process() {
    if (!impl_) return;
    if (impl_->outFd >= 0) close(impl_->outFd);
    if (impl_->inFd >= 0) close(impl_->inFd);
    if (!impl_->exited && impl_->pid > 0) {
        int st;
        waitpid(impl_->pid, &st, WNOHANG);
    }
}

std::unique_ptr<Process> Process::start(const Options& o, std::string& err) {
    if (o.args.empty()) {
        err = "no program";
        return nullptr;
    }
    std::unique_ptr<Process> p(new Process());
    p->impl_ = std::make_unique<Impl>();
    int outPipe[2] = {-1, -1}, inPipe[2] = {-1, -1}, errPipe[2] = {-1, -1};
    if (o.captureOutput && pipe2(outPipe, O_CLOEXEC) != 0) {
        err = "pipe failed";
        return nullptr;
    }
    if (o.pipeStdin && pipe2(inPipe, O_CLOEXEC) != 0) {
        err = "pipe failed";
        return nullptr;
    }
    if (pipe2(errPipe, O_CLOEXEC) != 0) {
        err = "pipe failed";
        return nullptr;
    }
    const pid_t pid = fork();
    if (pid < 0) {
        err = "fork failed";
        return nullptr;
    }
    if (pid == 0) {
        if (o.captureOutput) {
            dup2(outPipe[1], 1);
            dup2(outPipe[1], 2);
        }
        if (o.pipeStdin) dup2(inPipe[0], 0);
        else {
            const int nul = open("/dev/null", O_RDONLY);
            if (nul >= 0) dup2(nul, 0);
        }
        if (!o.workDir.empty() && chdir(o.workDir.c_str()) != 0) _exit(127);
        for (auto& [k, v] : o.env) setenv(k.c_str(), v.c_str(), 1);
        std::vector<char*> argv;
        for (auto& a : o.args) argv.push_back(const_cast<char*>(a.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        const int e = errno;
        ssize_t w = write(errPipe[1], &e, sizeof e);
        (void)w;
        _exit(127);
    }
    close(errPipe[1]);
    int childErr = 0;
    if (read(errPipe[0], &childErr, sizeof childErr) == (ssize_t)sizeof childErr) {
        close(errPipe[0]);
        err = "cannot execute " + o.args[0] + ": " + std::strerror(childErr);
        int st;
        waitpid(pid, &st, 0);
        if (outPipe[0] >= 0) close(outPipe[0]), close(outPipe[1]);
        if (inPipe[0] >= 0) close(inPipe[0]), close(inPipe[1]);
        return nullptr;
    }
    close(errPipe[0]);
    if (o.captureOutput) {
        close(outPipe[1]);
        p->impl_->outFd = outPipe[0];
    }
    if (o.pipeStdin) {
        close(inPipe[0]);
        p->impl_->inFd = inPipe[1];
    }
    p->impl_->pid = pid;
    return p;
}

bool Process::readSome(std::string& out) {
    if (impl_->outFd < 0) return false;
    char buf[4096];
    for (;;) {
        const ssize_t n = read(impl_->outFd, buf, sizeof buf);
        if (n > 0) {
            out.append(buf, (size_t)n);
            return true;
        }
        if (n < 0 && errno == EINTR) continue;
        return false;
    }
}

bool Process::writeStdin(const std::string& s) {
    if (impl_->inFd < 0) return false;
    signal(SIGPIPE, SIG_IGN);
    return write(impl_->inFd, s.data(), s.size()) == (ssize_t)s.size();
}

void Process::closeStdin() {
    if (impl_->inFd >= 0) close(impl_->inFd);
    impl_->inFd = -1;
}

bool Process::tryWait(int& code) {
    if (impl_->exited) {
        code = impl_->exitCode;
        return true;
    }
    int st = 0;
    const pid_t r = waitpid(impl_->pid, &st, WNOHANG);
    if (r != impl_->pid) return false;
    impl_->exited = true;
    impl_->exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    code = impl_->exitCode;
    return true;
}

int Process::wait() {
    if (!impl_->exited) {
        int st = 0;
        while (waitpid(impl_->pid, &st, 0) < 0 && errno == EINTR) {
        }
        impl_->exited = true;
        impl_->exitCode = WIFEXITED(st) ? WEXITSTATUS(st) : 128 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
    }
    return impl_->exitCode;
}

void Process::kill() {
    if (!impl_->exited && impl_->pid > 0) ::kill(impl_->pid, SIGKILL);
}

uint32_t Process::pid() const { return (uint32_t)impl_->pid; }

#endif

bool Process::running() {
    int c;
    return !tryWait(c);
}

std::string Process::readAll() {
    std::string out;
    while (readSome(out)) {
    }
    return out;
}

int runProcess(const std::vector<std::string>& args, std::string& output, const std::string& workDir) {
    Process::Options o;
    o.args = args;
    o.workDir = workDir;
    std::string err;
    auto p = Process::start(o, err);
    if (!p) {
        output = err;
        return -1;
    }
    output = p->readAll();
    return p->wait();
}

} // namespace sb2
