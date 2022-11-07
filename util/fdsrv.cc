// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// Shares stdin with FdServer.  Outputs the path and the nonce.
//
// Examples:
//      $ echo foo | fdsrv NONCE
//      @074e0 NONCE
//      $ fdcat @074e0 NONCE
//      foo
//
//      $ fdsrv NONCE < some_file
//      @36911 NONCE
//      $ fdcat @36911 NONCE
//      contents_of_some_file

#include "shared/fd_server.h"

int main(int argc, char* argv[]) {
  if (argc < 2) {
    std::cerr << "Usage: fdsrv NONCE" << std::endl;
    exit(1);
  }
  ghost::FdServer foo(/*fd=*/0, /*nonce=*/argv[1], absl::InfiniteDuration());
  absl::StatusOr<std::string> path = foo.Init();
  if (!path.ok()) {
    std::cerr << "Failed: " << path.status() << std::endl;
    return 1;
  }
  std::cout << *path << " " << argv[1] << std::endl;
  absl::Status status = foo.Serve();
  if (!status.ok()) {
    std::cerr << "Failed: " << status << std::endl;
    return 1;
  }
  return 0;
}
