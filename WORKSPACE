workspace(name = "com_google_ghost")

load("@bazel_tools//tools/build_defs/repo:git.bzl", "new_git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# We clone the ghOSt Linux kernel so that we can use libbpf and bpftool when
# compiling our eBPF programs, eBPF skeleton headers, and eBPF userspace
# programs.
new_git_repository(
    name = "linux",
    remote = "https://github.com/google/ghost-kernel",
    branch = "ghost-v5.11",
    build_file = "//third_party:linux.BUILD",
)

# Abseil depends on this.
http_archive(
  name = "rules_cc",
  url = "https://github.com/bazelbuild/rules_cc/archive/262ebec3c2296296526740db4aefce68c80de7fa.zip",
  sha256 = "9a446e9dd9c1bb180c86977a8dc1e9e659550ae732ae58bd2e8fd51e15b2c91d",
  strip_prefix = "rules_cc-262ebec3c2296296526740db4aefce68c80de7fa",
)

http_archive(
    name = "rules_foreign_cc",
    url = "https://github.com/bazelbuild/rules_foreign_cc/archive/99ea7e75c2a48cc233ff5e7682c1a31516faa84b.tar.gz",
    sha256 = "06fb31803fe3d2552f988f3c2fee430b10d566bc77dd7688897eca5388107883",
    strip_prefix = "rules_foreign_cc-99ea7e75c2a48cc233ff5e7682c1a31516faa84b",
)

load("@rules_foreign_cc//foreign_cc:repositories.bzl", "rules_foreign_cc_dependencies")
rules_foreign_cc_dependencies()

# Abseil live HEAD (Nov 17, 2022)
http_archive(
  name = "com_google_absl",
  url = "https://github.com/abseil/abseil-cpp/archive/ebab79b5783b3298ee2f31251174c660c322d7ef.zip",
  sha256 = "c3b9d19cd38cd475f60b5756db1bdc6d10ed43e5c7ce7374eae9a57d763d0597",
  strip_prefix = "abseil-cpp-ebab79b5783b3298ee2f31251174c660c322d7ef",
)

# GoogleTest release-1.12.1 (Jun 27 2022)
http_archive(
  name = "com_google_googletest",
  url = "https://github.com/google/googletest/archive/58d77fa8070e8cec2dc1ed015d66b454c8d78850.zip",
  sha256 = "ab78fa3f912d44d38b785ec011a25f26512aaedc5291f51f3807c592b506d33a",
  strip_prefix = "googletest-58d77fa8070e8cec2dc1ed015d66b454c8d78850",
)

http_archive(
  name = "com_google_benchmark",
  url = "https://github.com/google/benchmark/archive/7d0d9061d83b663ce05d9de5da3d5865a3845b79.zip",
  sha256 = "a07789754963e3ea3a1e13fed3a4d48fac0c5f7f749c5065f6c30cd2c1529661",
  strip_prefix = "benchmark-7d0d9061d83b663ce05d9de5da3d5865a3845b79",
)

# Used by rocksdb.
http_archive(
    name = "gflags",
    url = "https://github.com/gflags/gflags/archive/827c769e5fc98e0f2a34c47cef953cc6328abced.tar.gz",
    sha256 = "ce2931dd537eaab7dab78b25bec6136a0756ca0b2acbdab9aec0266998c0d9a7",
    strip_prefix = "gflags-827c769e5fc98e0f2a34c47cef953cc6328abced",
)

# OneTBB live HEAD (Jan 2 2023)
# A dependency of rocksdb that is required for rocksdb::ClockCache.
http_archive(
    name = "tbb",
    url = "https://github.com/oneapi-src/oneTBB/archive/e6e493f96ec8b7e2e2b4d048ed49356eb54ec2a0.tar.gz",
    sha256 = "42f11cb14215043b9e49cbce6b22b5bec9f3e59deb14047100b52a1eee34c514",
    strip_prefix = "oneTBB-e6e493f96ec8b7e2e2b4d048ed49356eb54ec2a0",
)

http_archive(
    name = "rocksdb",
    url = "https://github.com/facebook/rocksdb/archive/v6.15.5.tar.gz",
    sha256 = "d7b994e1eb4dff9dfefcd51a63f86630282e1927fc42a300b93c573c853aa5d0",
    strip_prefix = "rocksdb-6.15.5",
    build_file = "//third_party:rocksdb.BUILD",
)

http_archive(
    name = "subpar",
    url = "https://github.com/google/subpar/archive/9fae6b63cfeace2e0fb93c9c1ebdc28d3991b16f.tar.gz",
    sha256 = "481233d60c547e0902d381cd4fb85b63168130379600f330821475ad234d9336",
    strip_prefix = "subpar-9fae6b63cfeace2e0fb93c9c1ebdc28d3991b16f",
)

http_archive(
    name = "rules_python",
    url = "https://github.com/bazelbuild/rules_python/archive/6f37aa9966f53e063c41b7509a386d53a9f156c3.tar.gz",
    sha256 = "ecd139e703b41ae2ea115f4f4229b4ea2d70bab908fb75a3b49640f976213009",
    strip_prefix = "rules_python-6f37aa9966f53e063c41b7509a386d53a9f156c3",
)

load("@rules_python//python:pip.bzl", "pip_install")

pip_install(
   name = "my_deps",
   requirements = "//:requirements.txt",
)
