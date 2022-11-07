// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

#include "shared/fd_server.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace ghost {
namespace {

TEST(FdServerTest, SyncPassFd) {
  int pfds[2];
  ASSERT_EQ(pipe(pfds), 0);
  ASSERT_EQ(write(pfds[1], "x", 1), 1);
  close(pfds[1]);

  FdServer foo(pfds[0], "some_nonce", absl::InfiniteDuration());
  absl::StatusOr<std::string> uds = foo.Init();
  close(pfds[0]);

  ASSERT_TRUE(uds.ok());

  std::thread t([](std::string path, std::string nonce) {
    absl::StatusOr<int> sfd = FdServer::GetSharedFd(path, nonce);
    ASSERT_TRUE(sfd.ok());
    char buf[1];
    EXPECT_EQ(read(*sfd, buf, sizeof(buf)), sizeof(buf));
    EXPECT_EQ(buf[0], 'x');
    close(*sfd);
  }, *uds, "some_nonce");

  EXPECT_TRUE(foo.Serve().ok());
  t.join();
}

TEST(FdServerTest, AsyncPassFd) {
  int pfds[2];
  ASSERT_EQ(pipe(pfds), 0);
  ASSERT_EQ(write(pfds[1], "x", 1), 1);
  close(pfds[1]);

  AsyncFdServer foo(pfds[0], "some_nonce", absl::InfiniteDuration());
  absl::StatusOr<std::string> uds = foo.InitAndServe();
  close(pfds[0]);

  ASSERT_TRUE(uds.ok());

  absl::StatusOr<int> sfd = FdServer::GetSharedFd(*uds, "some_nonce");
  ASSERT_TRUE(sfd.ok());
  char buf[1];
  EXPECT_EQ(read(*sfd, buf, sizeof(buf)), sizeof(buf));
  EXPECT_EQ(buf[0], 'x');
  close(*sfd);
}

TEST(FdServerTest, BadNonce) {
  int pfds[2];
  ASSERT_EQ(pipe(pfds), 0);
  ASSERT_EQ(write(pfds[1], "x", 1), 1);
  close(pfds[1]);

  AsyncFdServer foo(pfds[0], "some_nonce", absl::InfiniteDuration());
  absl::StatusOr<std::string> uds = foo.InitAndServe();
  close(pfds[0]);

  ASSERT_TRUE(uds.ok());

  absl::StatusOr<int> sfd = FdServer::GetSharedFd(*uds, "bad_nonce");
  EXPECT_FALSE(sfd.ok());
}

TEST(FdServerTest, BadSubsetNonce) {
  int pfds[2];
  ASSERT_EQ(pipe(pfds), 0);
  ASSERT_EQ(write(pfds[1], "x", 1), 1);
  close(pfds[1]);

  AsyncFdServer foo(pfds[0], "some_nonce", absl::InfiniteDuration());
  absl::StatusOr<std::string> uds = foo.InitAndServe();
  close(pfds[0]);

  ASSERT_TRUE(uds.ok());

  absl::StatusOr<int> sfd = FdServer::GetSharedFd(*uds, "some_non");
  EXPECT_FALSE(sfd.ok());
}

TEST(FdServerTest, BadEmptyNonce) {
  int pfds[2];
  ASSERT_EQ(pipe(pfds), 0);
  ASSERT_EQ(write(pfds[1], "x", 1), 1);
  close(pfds[1]);

  AsyncFdServer foo(pfds[0], "some_nonce", absl::InfiniteDuration());
  absl::StatusOr<std::string> uds = foo.InitAndServe();
  close(pfds[0]);

  ASSERT_TRUE(uds.ok());

  absl::StatusOr<int> sfd = FdServer::GetSharedFd(*uds, "");
  EXPECT_FALSE(sfd.ok());
}

TEST(FdServerTest, EmptyNonce) {
  int pfds[2];
  ASSERT_EQ(pipe(pfds), 0);
  ASSERT_EQ(write(pfds[1], "x", 1), 1);
  close(pfds[1]);

  FdServer foo(pfds[0], "");
  absl::StatusOr<std::string> uds = foo.Init();
  close(pfds[0]);

  EXPECT_FALSE(uds.ok());
}

// Might flake: based on timing
TEST(FdServerTest, TimeoutDoesntFire) {
  int pfds[2];
  ASSERT_EQ(pipe(pfds), 0);
  ASSERT_EQ(write(pfds[1], "x", 1), 1);
  close(pfds[1]);

  AsyncFdServer foo(pfds[0], "some_nonce", absl::Seconds(1));
  absl::StatusOr<std::string> uds = foo.InitAndServe();
  close(pfds[0]);

  ASSERT_TRUE(uds.ok());

  absl::StatusOr<int> sfd = FdServer::GetSharedFd(*uds, "some_nonce");
  ASSERT_TRUE(sfd.ok());
  char buf[1];
  EXPECT_EQ(read(*sfd, buf, sizeof(buf)), sizeof(buf));
  EXPECT_EQ(buf[0], 'x');
  close(*sfd);
}

// Might flake: based on timing
TEST(FdServerTest, TimeoutFires) {
  int pfds[2];
  ASSERT_EQ(pipe(pfds), 0);
  ASSERT_EQ(write(pfds[1], "x", 1), 1);
  close(pfds[1]);

  AsyncFdServer foo(pfds[0], "some_nonce", absl::Microseconds(1));
  absl::StatusOr<std::string> uds = foo.InitAndServe();
  close(pfds[0]);

  ASSERT_TRUE(uds.ok());

  absl::SleepFor(absl::Milliseconds(100));
  absl::StatusOr<int> sfd = FdServer::GetSharedFd(*uds, "some_nonce");
  EXPECT_FALSE(sfd.ok());
}

TEST(FdServerTest, WaitTimeout) {
  int pfds[2];
  ASSERT_EQ(pipe(pfds), 0);
  ASSERT_EQ(write(pfds[1], "x", 1), 1);
  close(pfds[1]);

  FdServer foo(pfds[0], "some_nonce", absl::Milliseconds(100));
  absl::StatusOr<std::string> uds = foo.Init();
  close(pfds[0]);

  ASSERT_TRUE(uds.ok());

  absl::Status status = foo.Serve();
  EXPECT_TRUE(absl::IsDeadlineExceeded(status));
}

}  // namespace
}  // namespace ghost
