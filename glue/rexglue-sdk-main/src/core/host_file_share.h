// Host files that the guest opens more than once (docs/switch-port/16-deko3d-devlog.md, run 50).
//
// Horizon, the Switch's file system, locks a file that is open for writing: until that handle is closed, a second open
// of the file fails, and so does stat() (libnx's stat opens the file to read its size). libnx reports both as EIO.
// Run 49 ended there at 1,355 s: the title created its save (NtCreateFile 'save0:\SGTA412', write access), opened it
// again, and the VFS's check that a cached entry still exists on the host, std::filesystem::exists() without an
// error_code, threw filesystem_error (EIO) through the title's thread: std::terminate.
// - HostFileIsGone: a cached entry is dropped only when the host says the file is not there, not on any error.
// - SharedHostFdTable: the descriptors open per path, so that a second guest handle of a locked file uses the
//   descriptor already open (reads and writes are pread/pwrite at the handle's own offsets, so sharing is safe), and
//   stat of a locked file can be answered by fstat. The descriptor is closed with the last handle.
//
// No Switch headers here: tests/deko3d/test_run50.py compiles it on the PC.

#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>

namespace rex::filesystem {

inline bool HostFileIsGone(bool exists, const std::error_code& ec) {
  if (exists) {
    return false;
  }
  return !ec || ec == std::errc::no_such_file_or_directory || ec == std::errc::not_a_directory;
}

struct SharedHostFd {
  using CloseFn = void (*)(int);
  SharedHostFd(int fd, bool read, bool write, CloseFn close_fn)
      : fd(fd), read(read), write(write), close_fn(close_fn) {}
  ~SharedHostFd() {
    if (fd >= 0 && close_fn) {
      close_fn(fd);
    }
  }
  SharedHostFd(const SharedHostFd&) = delete;
  SharedHostFd& operator=(const SharedHostFd&) = delete;

  const int fd;
  const bool read;
  const bool write;
  const CloseFn close_fn;
};

class SharedHostFdTable {
 public:
  // Registers a descriptor just opened. An open one of the same path that writes is kept as the one to share when the
  // new one does not write.
  std::shared_ptr<SharedHostFd> Add(const std::string& path, int fd, bool read, bool write,
                                    SharedHostFd::CloseFn close_fn) {
    auto shared = std::make_shared<SharedHostFd>(fd, read, write, close_fn);
    std::lock_guard<std::mutex> lock(mutex_);
    if (fds_.size() >= 64) {
      for (auto it = fds_.begin(); it != fds_.end();) {
        it = it->second.expired() ? fds_.erase(it) : std::next(it);
      }
    }
    auto& slot = fds_[path];
    const auto old = slot.lock();
    if (!old || write || !old->write) {
      slot = shared;
    }
    return shared;
  }

  // The open descriptor of path that allows the access asked for, or null.
  std::shared_ptr<SharedHostFd> Find(const std::string& path, bool read, bool write) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = fds_.find(path);
    if (it == fds_.end()) {
      return nullptr;
    }
    auto shared = it->second.lock();
    if (!shared) {
      fds_.erase(it);
      return nullptr;
    }
    if ((read && !shared->read) || (write && !shared->write)) {
      return nullptr;
    }
    return shared;
  }

  size_t size() {
    std::lock_guard<std::mutex> lock(mutex_);
    return fds_.size();
  }

 private:
  std::mutex mutex_;
  std::unordered_map<std::string, std::weak_ptr<SharedHostFd>> fds_;
};

}  // namespace rex::filesystem
