// Copyright 2022 Google LLC
//
// Use of this source code is governed by a BSD-style
// license that can be found in the LICENSE file or at
// https://developers.google.com/open-source/licenses/bsd

// Cats the contents of some file shared by fdsrv
//
// Example:
//      $ echo foo | fdsrv NONCE
//      @074e0 NONCE
//      $ fdcat @074e0 NONCE
//      foo

#include "shared/fd_server.h"

int main(int argc, char* argv[]) {
  if (argc < 3) {
    std::cerr << "Usage: fdcat PATH NONCE" << std::endl;
    exit(1);
  }
  auto fd = ghost::FdServer::GetSharedFd(argv[1], argv[2]);
  if (!fd.ok()) {
    std::cerr << "Failed: " << fd.status() << std::endl;
    return 1;
  }
  char buf[4096];
  ssize_t ret;
  while ((ret = read(*fd, buf, sizeof(buf))) > 0) {
    std::string s(buf, ret);
    std::cout << s;
  }
  return 0;
}

