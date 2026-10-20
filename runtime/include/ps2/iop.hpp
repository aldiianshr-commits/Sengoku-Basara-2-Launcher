// High-level IOP: virtual file system and the native RPC services that the
// standard IOP modules (FILEIO, CDVD, PADMAN, MCSERV, LOADFILE ...) provide.
#pragma once
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ps2 {

class Machine;

struct VfsStat {
    uint32_t mode = 0;  // FIO_S_IFREG 0x2000 / FIO_S_IFDIR 0x1000 + permissions
    uint32_t attr = 0;
    uint64_t size = 0;
};

class Vfs {
public:
    explicit Vfs(Machine& m);
    ~Vfs();
    int open(const std::string& path, int mode);
    int close(int fd);
    int read(int fd, uint8_t* buf, int size);
    int write(int fd, const uint8_t* buf, int size);
    int64_t lseek(int fd, int64_t off, int whence);
    int getstat(const std::string& path, VfsStat& st);
    int mkdir(const std::string& path);
    int remove(const std::string& path);
    int rmdir(const std::string& path);
    int dopen(const std::string& path);
    // Returns 1 and fills name/stat for the next entry, 0 at end, <0 on error.
    int dread(int fd, std::string& name, VfsStat& st);
    int dclose(int fd);
    // Map a PS2 device path to a host path (host0:, mc0:, ...); empty if not host-backed.
    std::string hostPath(const std::string& path) const;

    struct Handle;

private:
    Machine& m_;
    std::map<int, std::unique_ptr<Handle>> handles_;
    int nextFd_ = 3;
};

class Iop {
public:
    explicit Iop(Machine& m);
    ~Iop();
    void boot(); // register RPC services
    Vfs& vfs() { return *vfs_; }
    std::vector<std::string> loadedModules;

private:
    Machine& m_;
    std::unique_ptr<Vfs> vfs_;
};

} // namespace ps2
