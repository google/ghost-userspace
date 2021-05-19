workspace(name = "com_google_ghost")

load("@bazel_tools//tools/build_defs/repo:git.bzl", "git_repository")
load("@bazel_tools//tools/build_defs/repo:http.bzl", "http_archive")

# TODO: Add SHA-256 for each http_archive once we settle on final versions.

# Abseil depends on this.
http_archive(
  name = "rules_cc",
  urls = ["https://github.com/bazelbuild/rules_cc/archive/262ebec3c2296296526740db4aefce68c80de7fa.zip"],
  strip_prefix = "rules_cc-262ebec3c2296296526740db4aefce68c80de7fa",
)

http_archive(
  name = "com_google_absl",
  sha256 = "542dee3a6692cf7851329f4f9f4de463bb6305c7e0439946d4ba750852e4d71c",
  strip_prefix = "abseil-cpp-2e9532cc6c701a8323d0cffb468999ab804095ab",
  urls = ["https://github.com/abseil/abseil-cpp/archive/2e9532cc6c701a8323d0cffb468999ab804095ab.zip"],
)

http_archive(
  name = "com_google_googletest",
  urls = ["https://github.com/google/googletest/archive/011959aafddcd30611003de96cfd8d7a7685c700.zip"],
  strip_prefix = "googletest-011959aafddcd30611003de96cfd8d7a7685c700",
)

http_archive(
  name = "com_google_benchmark",
  urls = ["https://github.com/google/benchmark/archive/7d0d9061d83b663ce05d9de5da3d5865a3845b79.zip"],
  strip_prefix = "benchmark-7d0d9061d83b663ce05d9de5da3d5865a3845b79",
)

# Used by rocksdb.
http_archive(
    name = "gflags",
    sha256 = "ce2931dd537eaab7dab78b25bec6136a0756ca0b2acbdab9aec0266998c0d9a7",
    strip_prefix = "gflags-827c769e5fc98e0f2a34c47cef953cc6328abced",
    url = "https://github.com/gflags/gflags/archive/827c769e5fc98e0f2a34c47cef953cc6328abced.tar.gz",
)

# A dependency of rocksdb that is required for rocksdb::ClockCache.
http_archive(
    name = "tbb",
    build_file = "//third_party:tbb.BUILD",
    sha256 = "b182c73caaaabc44ddc5ad13113aca7e453af73c1690e4061f71dfe4935d74e8",
    strip_prefix = "oneTBB-2021.1.1",
    url = "https://github.com/oneapi-src/oneTBB/archive/v2021.1.1.tar.gz",
)

http_archive(
    name = "rocksdb",
    build_file = "//third_party:rocksdb.BUILD",
    sha256 = "d7b994e1eb4dff9dfefcd51a63f86630282e1927fc42a300b93c573c853aa5d0",
    strip_prefix = "rocksdb-6.15.5",
    url = "https://github.com/facebook/rocksdb/archive/v6.15.5.tar.gz",
)

git_repository(
    name = "subpar",
    remote = "https://github.com/google/subpar",
    tag = "2.0.0",
)

http_archive(
    name = "rules_python",
    sha256 = "ecd139e703b41ae2ea115f4f4229b4ea2d70bab908fb75a3b49640f976213009",
    strip_prefix = "rules_python-6f37aa9966f53e063c41b7509a386d53a9f156c3",
    urls = [
        "https://github.com/bazelbuild/rules_python/archive/6f37aa9966f53e063c41b7509a386d53a9f156c3.tar.gz",
    ],
)

load("@rules_python//python:pip.bzl", "pip_install")

pip_install(
   name = "my_deps",
   requirements = "//:requirements.txt",
)
