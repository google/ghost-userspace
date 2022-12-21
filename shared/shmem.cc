// Copyright 2021 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "shared/shmem.h"

#include <fcntl.h>
#include <linux/memfd.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <atomic>
#include <filesystem>
#include <string>

#include "absl/strings/match.h"
#include "absl/strings/str_cat.h"

#ifndef F_LINUX_SPECIFIC_BASE
#define F_LINUX_SPECIFIC_BASE 1024
#endif
#ifndef F_ADD_SEALS
#define F_ADD_SEALS (F_LINUX_SPECIFIC_BASE + 9)
#endif
#ifndef F_SEAL_SEAL
#define F_SEAL_SEAL 0x0001 /* prevent further seals from being set */
#endif
#ifndef F_SEAL_SHRINK
#define F_SEAL_SHRINK 0x0002 /* prevent file from shrinking */
#endif
#ifndef F_SEAL_GROW
#define F_SEAL_GROW 0x0004 /* prevent file from growing */
#endif

namespace fs = std::filesystem;

namespace ghost {

constexpr size_t kHugepageSize = 2 * 1024 * 1024;
static const char* kMemFdPrefix = "ghost-shmem-";

// Please don't use "0" as a header version, it's not distinguishable from
// an uninitialized header.
static constexpr int64_t kHeaderVersion = 1;

// This currently occupies the first page of every mapping (from offset zero).
struct GhostShmem::InternalHeader {
  int64_t header_version;

  size_t mapping_size;
  size_t header_size;
  size_t client_size;

  std::atomic<bool> ready, finished;

  pid_t owning_pid;
  int64_t client_version;
};

GhostShmem::GhostShmem(int64_t client_version, const char* name, size_t size) {
  CreateShmem(client_version, name, size);
}

bool GhostShmem::Attach(int64_t client_version, const char* name, pid_t pid) {
  return ConnectShmem(client_version, name, pid);
}

GhostShmem::~GhostShmem() {
  if (hdr_) {
    hdr_->finished.store(true);
  }
  if (shmem_) {
    munmap(shmem_, map_size_);
  }
  if (memfd_ >= 0) {
    close(memfd_);
  }
}

void GhostShmem::MarkReady() { hdr_->ready.store(true); }

void GhostShmem::WaitForReady() {
  // TODO: Use a shared futex here.
  while (!hdr_->ready.load()) {
  }
}

size_t GhostShmem::size() {
  // We apply internal adjustments, e.g. our header, hugepages, etc.
  return hdr_->client_size;
}

void GhostShmem::CreateShmem(int64_t client_version, const char* suffix,
                             size_t size) {
  int MFD_FLAGS = MFD_CLOEXEC | MFD_ALLOW_SEALING;
  const int MFD_SEALS = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL;
  std::string name;

  // Suffixes must currently be unique for the hosting process.
  CHECK_EQ(OpenGhostShmemFd(suffix, Gtid::Current().tid()), -1);

  name = kMemFdPrefix;
  name.append(suffix);
  memfd_ = memfd_create(name.c_str(), MFD_FLAGS);
  CHECK_GE(memfd_, 0);

  // Prepend our header to the mapping.
  map_size_ = roundup2(size + kHeaderReservedBytes, kHugepageSize);
  CHECK_LE(map_size_, UINT32_MAX);
  CHECK_EQ(ftruncate(memfd_, map_size_), 0);
  CHECK_EQ(fcntl(memfd_, F_ADD_SEALS, MFD_SEALS), 0);

  shmem_ =
      mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, memfd_, 0);
  CHECK_NE(shmem_, MAP_FAILED);

  // At this point the shmem_ is created, our header is initialized, but the
  // region is not yet ready.  Clients must call MarkReady() before we'll allow
  // connections against it to proceed.
  hdr_ = static_cast<InternalHeader*>(shmem_);
  char* bytes = static_cast<char*>(shmem_);
  data_ = bytes + kHeaderReservedBytes;

  // We can safely initialize InternalHeader data fields after this point, as
  // MarkReady() cannot yet proceed.
  hdr_->header_version = kHeaderVersion;
  hdr_->mapping_size = map_size_;
  hdr_->client_size = map_size_ - kHeaderReservedBytes;
  hdr_->header_size = kHeaderReservedBytes;
  hdr_->owning_pid = getpid();  // Should probably be process.
}

bool GhostShmem::ConnectShmem(int64_t client_version, const char* suffix,
                              pid_t pid) {
  memfd_ = OpenGhostShmemFd(suffix, pid);
  if (memfd_ < 0) {
    return false;
  }

  struct stat sb;
  CHECK_EQ(fstat(memfd_, &sb), 0);

  map_size_ = sb.st_size;
  shmem_ =
      mmap(nullptr, map_size_, PROT_READ | PROT_WRITE, MAP_SHARED, memfd_, 0);
  CHECK_NE(shmem_, MAP_FAILED);

  // Avoid deadlock between agent and the task it is scheduling. This happens
  // if both tasks (agent and non-agent) fault on the same page in shared mem
  // concurrently. Subsequently when the page is ready then it is possible that
  // the non-agent task is woken up first but doesn't get a chance to run
  // because the agent (that is responsible for scheduling it) is also blocked
  // on the same page.
  //
  // See b/173811264 for details.
  CHECK_EQ(mlock(shmem_, map_size_), 0);

  // Setup internal fields.
  hdr_ = static_cast<InternalHeader*>(shmem_);
  char* bytes = static_cast<char*>(shmem_);
  data_ = bytes + kHeaderReservedBytes;

  // Ensure we synchronize on the remote side marking that content is ready
  // before trying to validate.
  WaitForReady();

  CHECK_EQ(hdr_->header_version, kHeaderVersion);
  CHECK_EQ(hdr_->client_version, client_version);
  CHECK_EQ(hdr_->mapping_size, map_size_);
  CHECK_EQ(hdr_->header_size, kHeaderReservedBytes);
  CHECK_EQ(hdr_->owning_pid, pid);
  return true;
}

// static
int GhostShmem::OpenGhostShmemFd(const char* suffix, pid_t pid) {
  std::string path = "/proc/" + std::to_string(pid) + "/fd";
  std::string needle("/memfd:");
  needle.append(kMemFdPrefix);
  needle.append(suffix);

  std::error_code dir_error;
  auto f = fs::directory_iterator(path, dir_error);
  auto end = fs::directory_iterator();

  for (/* f */; !dir_error && f != end; f.increment(dir_error)) {
    // It's possible for f to disappear at any moment if the file is closed.
    std::error_code ec;
    std::string p = fs::read_symlink(f->path(), ec);
    if (ec) {
      continue;
    }
    if (absl::StartsWith(p, needle)) {
      std::string path = f->path();
      int fd = open(path.c_str(), O_RDWR | O_CLOEXEC);
      if (fd < 0) {
        continue;
      }
      return fd;
    }
  }
  return -1;
}

pid_t GhostShmem::Owner() const {
  return hdr_ ? hdr_->owning_pid : 0;
}

// static
GhostShmem* GhostShmem::GetShmemBlob(size_t size) {
  static std::atomic<int> unique = 0;
  std::string blob = absl::StrCat(
      "blob-", std::to_string(unique.fetch_add(1, std::memory_order_relaxed)));
  // GhostShmem needs a unique name per process for the memfd
  GhostShmem* shmem = new GhostShmem(/*client_version=*/0, blob.c_str(), size);
  shmem->MarkReady();

  return shmem;
}

}  // namespace ghost
