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

http_archive(
  name = "com_google_absl",
  url = "https://github.com/abseil/abseil-cpp/archive/2e9532cc6c701a8323d0cffb468999ab804095ab.zip",
  sha256 = "542dee3a6692cf7851329f4f9f4de463bb6305c7e0439946d4ba750852e4d71c",
  strip_prefix = "abseil-cpp-2e9532cc6c701a8323d0cffb468999ab804095ab",
)

http_archive(
  name = "com_google_googletest",
  url = "https://github.com/google/googletest/archive/011959aafddcd30611003de96cfd8d7a7685c700.zip",
  sha256 = "6a5d7d63cd6e0ad2a7130471105a3b83799a7a2b14ef7ec8d742b54f01a4833c",
  strip_prefix = "googletest-011959aafddcd30611003de96cfd8d7a7685c700",
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

# A dependency of rocksdb that is required for rocksdb::ClockCache.
http_archive(
    name = "tbb",
    url = "https://github.com/oneapi-src/oneTBB/archive/d1667d514df697f05d771602b268e92560c434c4.tar.gz",
    sha256 = "b5936aac1ffbc3767cae2377199647d663159a597f8e5958c5979bbf3b8d6384",
    strip_prefix = "oneTBB-d1667d514df697f05d771602b268e92560c434c4",
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
