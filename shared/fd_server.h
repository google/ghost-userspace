// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#ifndef GHOST_LIB_FD_SERVER_H_
#define GHOST_LIB_FD_SERVER_H_

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/string_view.h"

#include <string>
#include <thread>

namespace ghost {

// Serves a file descriptor over an AF_UNIX socket when presented with a nonce.
//
// You must pass the socket path and nonce to the client via some out-of-band
// mechanism, such as gRPC or a bash script.
//
// Notes:
// - Uses the unix domain socket abstract namespace
// - Picks its own path in the abstract namespace for the socket.
// - Shared FDs are essentially duped, and they point to the same struct file:
// they share offsets and whatnot.
//
// Options:
// - Serves once and shuts down (default)
// - Cancels itself after a timeout (default 1 sec)
//
// Usage:
//
//   FdServer foo(fd_to_share, "some_nonce");
//   StatusOr<string> path = foo.Init());
//   CHECK(path.ok());
//
//   // Pass *path and some_nonce to the client via an out of band mechanism
//
//   foo.Serve(); // Blocks until the server is done
//
class FdServer {
 public:
  FdServer(int fd, absl::string_view nonce,
           absl::Duration timeout = absl::Milliseconds(1000),
           bool serve_forever = false) :
    fd_to_share_(fd),
    nonce_(nonce),
    timeout_(timeout),
    serve_forever_(serve_forever) {}

  ~FdServer() {
    if (sock_fd_ != -1) close(sock_fd_);
    if (dup_fd_ != -1) close(dup_fd_);
    if (ep_fd_ != -1) close(ep_fd_);
  }

  FdServer(const FdServer&) = delete;
  FdServer& operator=(const FdServer&) = delete;

  // Initializes the server.  Returns the path to the unix domain socket that
  // the client must connect to.  Follow up with Serve().
  absl::StatusOr<std::string> Init();

  // Runs the FD server, blocks until complete.
  absl::Status Serve();

  // Client method: returns the FD shared at sock_path.
  static absl::StatusOr<int> GetSharedFd(absl::string_view sock_path,
                                         absl::string_view nonce);

 private:
  int fd_to_share_;
  std::string nonce_;
  absl::Duration timeout_;
  bool serve_forever_;

  int sock_fd_ = -1;
  int dup_fd_ = -1;
  int ep_fd_ = -1;

  // Convert timeout_ to an argument for epoll.
  // For epoll: -1 == no timeout, 0 == poll.
  // We never want to actually poll - either block forever or timeout.
  int64_t EpollTimeout() {
    if (timeout_ == absl::InfiniteDuration()) {
      return -1;
    }
    // User might think 0 == no timeout.
    if (timeout_ == absl::ZeroDuration()) {
      return -1;
    }
    // epoll wants the time in ms.  ToInt64Milliseconds rounds down to 0.
    // Rounding up a timeout is always safe.
    int64_t to = absl::ToInt64Milliseconds(timeout_);
    if (to == 0) {
      return 1;
    }
    return to;
  }

  absl::StatusOr<int> WaitForFdCmd(int fd, std::function<int()> cmd);
  absl::Status HandleConnection(int conn_fd);
};

// Helper class for an asynchronous FdServer, with its own internal threading.
//
// Usage similar to FdServer:
//
//   AsyncFdServer async_foo(fd_to_share, "some_nonce");
//   StatusOr<string> path = async_foo.InitAndServe());
//   CHECK(path.ok());
//
//   // Pass *path and some_nonce to the client via an out of band mechanism
//
// Notes:
// - You can destruct async_foo while its thread is still running.  Fire and
// forget.
// - Will limit the number of outstanding threads to avoid DoS problems.
//
class AsyncFdServer {
 public:
  AsyncFdServer(int fd, absl::string_view nonce,
                absl::Duration timeout = absl::Milliseconds(1000)) :
    fd_to_share_(fd),
    nonce_(nonce),
    timeout_(timeout) {}
  ~AsyncFdServer();

  AsyncFdServer(const AsyncFdServer&) = delete;
  AsyncFdServer& operator=(const AsyncFdServer&) = delete;

  // Spawns a thread to listen and serve the FD.
  // Returns the path to the unix domain socket that the client must connect to.
  absl::StatusOr<std::string> InitAndServe();

 private:
  int fd_to_share_;
  std::string nonce_;
  absl::Duration timeout_;
  std::thread thread_;

  static constexpr int kMaxNrThreads = 42;
  static std::atomic<int> nr_threads_;
};

}  // namespace ghost

#endif  // GHOST_LIB_FD_SERVER_H_
