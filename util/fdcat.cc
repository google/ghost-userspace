// Copyright 2022 Google LLC
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

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

