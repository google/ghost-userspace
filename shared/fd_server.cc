// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "absl/cleanup/cleanup.h"
#include "absl/synchronization/notification.h"
#include "shared/fd_server.h"

#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/un.h>

namespace ghost {

absl::StatusOr<std::string> FdServer::Init() {
  int ret;

  if (nonce_.length() == 0) {
    return absl::InvalidArgumentError("nonce must not be empty");
  }
  if (sock_fd_ != -1) {
    return absl::InvalidArgumentError("do not reinitialize FdServer");
  }

  // Dup our own copy of FD.  Async callers may close their original FD.
  dup_fd_ = dup(fd_to_share_);
  if (dup_fd_ < 0) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "dup");
  }
  sock_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd_ < 0) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "socket");
  }

  ret = fcntl(sock_fd_, F_GETFL, 0);
  if (ret < 0) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "f_getfl");
  }
  ret = fcntl(sock_fd_, F_SETFL, ret | O_NONBLOCK);
  if (ret < 0) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "f_setfl");
  }

  // Autobind: If a bind(2) call specifies addrlen as sizeof(sa_family_t), the
  // kernel will pick a path in the abstract namespace, e.g. @00001.
  //
  // Also note that abstract sockets don't need to be removed: they are
  // removed when their last fd closes.
  struct sockaddr_un sa_un = {.sun_family = AF_UNIX};
  ret = bind(sock_fd_, reinterpret_cast<struct sockaddr*>(&sa_un),
             sizeof(sa_family_t));
  if (ret < 0) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "bind");
  }
  ret = listen(sock_fd_, 0);
  if (ret < 0) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "listen");
  }

  socklen_t sa_size = sizeof(sa_un);
  ret = getsockname(sock_fd_, reinterpret_cast<struct sockaddr*>(&sa_un),
                    &sa_size);
  if (ret < 0) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "getsockname");
  }
  if (sa_size > sizeof(sa_un)) {
    return absl::InvalidArgumentError("getsockname truncation");
  }
  // Byte 0 is '\0' (null-prefixed).  '@' is the convention for the abstract
  // namespace
  sa_un.sun_path[0] = '@';

  ep_fd_ = epoll_create1(0);
  if (ep_fd_ < 0) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "epoll create");
  }

  return std::string(sa_un.sun_path, sa_size -
                     offsetof(struct sockaddr_un, sun_path));
}

// Run cmd on fd and if it would block, epoll.  fd should be O_NONBLOCK.
//
// cmd must return a signed int.  Negative is failure.  0 or positive is
// success.
//
// Returns cmd's value on success or a StatusCode on error.
absl::StatusOr<int> FdServer::WaitForFdCmd(int fd, std::function<int()> cmd) {
  constexpr int kNrResults = 1;
  struct epoll_event results[kNrResults];
  struct epoll_event ep_ev;
  ep_ev.events = EPOLLIN | EPOLLET;

  if (epoll_ctl(ep_fd_, EPOLL_CTL_ADD, fd, &ep_ev)) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "epoll_ctl_add");
  }
  auto deregister_ep_fd = absl::MakeCleanup([&](){
    epoll_ctl(ep_fd_, EPOLL_CTL_DEL, fd, &ep_ev);
  });

  for (;;) {
    int ret = cmd();
    if (ret >= 0) {
      return ret;
    }
    if (errno == EINTR) {
      continue;
    }
    if (errno != EAGAIN) {
      return absl::Status(absl::ErrnoToStatusCode(errno), "cmd error");
    }
    ret = epoll_wait(ep_fd_, results, kNrResults, EpollTimeout());
    if (ret == 0) {
      return absl::DeadlineExceededError("epoll timeout");
    }
    if (ret < 0) {
      // We could epoll_pwait and block all signals, but we want signals.
      // consider a simple FdServer that is blocked and gets a SIGINT: we don't
      // want to block that handler (which default kills the process).  We also
      // might be part of a bigger program that uses signals, so we might
      // legitimately get e.g. SIGUSR1.
      if (errno == EINTR) {
        continue;
      }
      return absl::Status(absl::ErrnoToStatusCode(errno), "epoll wait");
    }
  }
}

// Returns kOk for successfully shared, kUnauthenticated for didn't share due to
// client error (bad nonce), or other statuses for server errors.
absl::Status FdServer::HandleConnection(int conn_fd) {
  char buf[4096];
  struct iovec iov = {0};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
  struct cmsghdr *cmsg;

  iov.iov_base = buf;
  iov.iov_len = sizeof(buf);
  absl::StatusOr<int> result = WaitForFdCmd(conn_fd, [&]() {
    return recvmsg(conn_fd, &msg, 0);
  });
  if (!result.ok()) {
    return result.status();
  }
  const int bytes_read = *result;
  std::string query(buf, bytes_read);
  if (query != nonce_) {
    char bad[] = "BAD NONCE";
    iov.iov_base = bad;
    iov.iov_len = sizeof(bad);
    // MSG_NOSIGNAL: avoid SIGPIPE if the remote end is closed.
    (void)sendmsg(conn_fd, &msg, MSG_NOSIGNAL);
    return absl::UnauthenticatedError("bad nonce");
  }
  // We have to pass some data with the FD.  GOOD/BAD helps with debugging.
  char good[] = "GOOD NONCE";
  iov.iov_base = good;
  iov.iov_len = sizeof(good);

  union {
      char buf[CMSG_SPACE(sizeof(int))];
      struct cmsghdr align;
  } u;
  msg.msg_control = u.buf;
  msg.msg_controllen = sizeof(u.buf);
  cmsg = CMSG_FIRSTHDR(&msg);
  cmsg->cmsg_level = SOL_SOCKET;
  cmsg->cmsg_type = SCM_RIGHTS;
  cmsg->cmsg_len = CMSG_LEN(sizeof(int));
  memcpy(CMSG_DATA(cmsg), &dup_fd_, sizeof(int));

  // MSG_NOSIGNAL: avoid SIGPIPE if the remote end is closed.
  int ret = sendmsg(conn_fd, &msg, MSG_NOSIGNAL);
  if (ret != iov.iov_len) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "sendmsg FD");
  }

  return absl::OkStatus();
}

// Runs the FD server, blocks until complete.
absl::Status FdServer::Serve() {
  if (sock_fd_ == -1) {
    return absl::InvalidArgumentError("server was not initialized");
  }

  for (;;) {
    // Return the socket of a new connection, or negative errno.
    absl::StatusOr<int> conn_fd = WaitForFdCmd(sock_fd_, [&]() {
      return accept4(sock_fd_, nullptr, nullptr, SOCK_NONBLOCK);
    });
    if (!conn_fd.ok()) {
      return conn_fd.status();
    }
    absl::Status status = HandleConnection(*conn_fd);
    close(*conn_fd);
    if (absl::IsUnauthenticated(status)) {
      // We had a connection with a bad nonce, which is a client error, not a
      // server error.  Don't tear down the server until we get a good
      // connection (or timeout).
      // 
      // Two cases:
      // 1) it wasn't our client: some other task sent us garbage.  Don't stop
      // serving, since our client still expects to connect.
      // 2) client messed up.  It's unlikely they will try again, but we already
      // need to handle cases where the client messes up: e.g. the timeout.
      continue;
    }
    if (status.ok() && serve_forever_) {
      continue;
    }
    return status;
  }
}

// static
absl::StatusOr<int> FdServer::GetSharedFd(absl::string_view sock_path,
                                          absl::string_view nonce) {
  int shared_fd, sock_fd, ret;
  struct sockaddr_un sa_un = {.sun_family = AF_UNIX};
  char buf[4096];
  struct iovec iov = {0};
  struct msghdr msg = {.msg_iov = &iov, .msg_iovlen = 1};
  struct cmsghdr *cmsg;
  // string_view.data() is const.  std::string isn't.
  std::string nonce_str(nonce);

  // Not explicitly null-terminating sun_path.  Nulls aren't special for
  // abstract sockets, so the addrlen tells the kernel how big the path is.
  if (sock_path.length() > sizeof(sa_un.sun_path)) {
    return absl::InvalidArgumentError("sock_path too big");
  }
  memcpy(sa_un.sun_path, sock_path.data(), sock_path.length());
  if (sa_un.sun_path[0] == '@') {
    sa_un.sun_path[0] = '\0';
  }

  sock_fd = socket(AF_UNIX, SOCK_STREAM, 0);
  if (sock_fd < 0) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "socket");
  }
  auto close_sock_fd = absl::MakeCleanup([&](){
    close(sock_fd);
  });

  ret = connect(sock_fd, reinterpret_cast<struct sockaddr*>(&sa_un),
                offsetof(struct sockaddr_un, sun_path) + sock_path.length());
  if (ret < 0) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "connect");
  }

  iov.iov_base = nonce_str.data();
  iov.iov_len = nonce_str.length();
  ret = sendmsg(sock_fd, &msg, MSG_NOSIGNAL);
  // If you don't send at least a byte, the server won't recvmsg.  This is a
  // Linux UDS SOCK_STREAM thing.  It's possible that nonce is "", and iov_len
  // == 0.  We could check earlier for an empty string, but test code uses this
  // method to make sure the server can handle an empty nonce (BadEmptyNonce).
  if (ret == 0) {
    return absl::InvalidArgumentError("sendmsg empty write: maybe no nonce");
  }
  if (ret < 0) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "sendmsg");
  }
  if (ret != iov.iov_len) {
    return absl::InvalidArgumentError("sendmsg short write");
  }

  iov.iov_base = buf;
  iov.iov_len = sizeof(buf);

  union {
      char buf[CMSG_SPACE(sizeof(int))];
      struct cmsghdr align;
  } u;
  msg.msg_control = u.buf;
  msg.msg_controllen = sizeof(u.buf);

  ret = recvmsg(sock_fd, &msg, 0);
  if (ret < 0) {
    return absl::Status(absl::ErrnoToStatusCode(errno), "recvmsg");
  }
  if (ret == 0) {
    return absl::InvalidArgumentError("recvmsg short read");
  }

  cmsg = CMSG_FIRSTHDR(&msg);
  if (!cmsg ||
      cmsg->cmsg_level != SOL_SOCKET ||
      cmsg->cmsg_type != SCM_RIGHTS ||
      cmsg->cmsg_len != CMSG_LEN(sizeof(int))) {
    return absl::InvalidArgumentError("recvmsg no fd");
  }
  memcpy(&shared_fd, CMSG_DATA(cmsg), sizeof(int));

  return shared_fd;
}

absl::StatusOr<std::string> AsyncFdServer::InitAndServe() {
  std::unique_ptr<FdServer> fds = std::make_unique<FdServer>(
                                    fd_to_share_, nonce_, timeout_,
                                    /*serve_forever=*/false);
  absl::StatusOr<std::string> path = fds->Init();
  if (!path.ok()) {
    return path.status();
  }

  if (nr_threads_.load() >= kMaxNrThreads) {
    return absl::ResourceExhaustedError("too many threads");
  }
  nr_threads_++;

  // Passing ownership of fds
  thread_ = std::thread([](std::unique_ptr<FdServer> fds) {
    absl::Status status = fds->Serve();
    if (!status.ok() && !absl::IsDeadlineExceeded(status)) {
      std::cerr << "AsyncFdServer thread failure: " << status << std::endl;
    }
    nr_threads_--;
  }, std::move(fds));

  return *path;
}

AsyncFdServer::~AsyncFdServer() {
  if (thread_.joinable()) {
    thread_.detach();
  }
}

std::atomic<int> AsyncFdServer::nr_threads_ = 0;

}  // namespace ghost
