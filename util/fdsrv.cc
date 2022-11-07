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
