# Note: If you modify this BUILD file, please contact jhumphri@ first to ensure
# that you are not breaking the Copybara script.

load("@rules_license//rules:license.bzl", "license")
load("//:bpf/bpf.bzl", "bpf_skeleton")

package(
    default_applicable_licenses = ["//:license"],
    default_visibility = ["//:__pkg__"],
)

license(
    name = "license",
    package_name = "ghost",
)

# Each license covers the code below:
#
# BSD 2-clause: Just covers the IOVisor BCC code in third_party/iovisor_bcc/.
# This code was not written by Google.
#
# GPLv2: Just covers the eBPF code in third_party/bpf/. This code was written
# by Google. We need to license it under GPLv2 though so that the eBPF code
# can use kernel functionality restricted to code licensed under GPLv2.
#
# MIT: Just covers third_party/util/util.h. This code was not written by Google,
# but was modified by Google.
#
# BSD 3-clause: All other code is covered by BSD 3-clause. This includes the
# library code in lib/, the experiments, all code in bpf/user/, etc.
licenses(["notice"])

exports_files(["LICENSE"])

compiler_flags = [
    "-Wno-sign-compare",
]

bpf_linkopts = [
    "-lelf",
    "-lz",
]

cc_library(
    name = "agent",
    srcs = [
        "bpf/user/agent.c",
        "lib/agent.cc",
        "lib/channel.cc",
        "lib/enclave.cc",
    ],
    hdrs = [
        "bpf/user/agent.h",
        "bpf/user/schedghostidle_bpf.skel.h",
        "lib/agent.h",
        "lib/channel.h",
        "lib/enclave.h",
        "lib/scheduler.h",
        "//third_party:iovisor_bcc/trace_helpers.h",
    ],
    copts = compiler_flags,
    linkopts = bpf_linkopts + ["-lnuma"],
    deps = [
        ":base",
        ":ghost",
        ":shared",
        ":topology",
        ":trivial_status",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@linux//:libbpf",
    ],
)

cc_library(
    name = "trivial_status",
    srcs = ["lib/trivial_status.cc"],
    hdrs = ["lib/trivial_status.h"],
    copts = compiler_flags,
    deps = [
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:str_format",
    ],
)

cc_binary(
    name = "agent_cfs",
    srcs = [
        "schedulers/cfs/cfs_agent.cc",
        "schedulers/cfs/cfs_scheduler.cc",
        "schedulers/cfs/cfs_scheduler.h",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":base",
        ":topology",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/numeric:int128",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
    ],
)

cc_library(
    name = "cfs_scheduler",
    srcs = [
        "schedulers/cfs/cfs_scheduler.cc",
        "schedulers/cfs/cfs_scheduler.h",
    ],
    hdrs = [
        "schedulers/cfs/cfs_scheduler.h",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":base",
        ":topology",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/numeric:int128",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
    ],
)

cc_binary(
    name = "agent_exp",
    srcs = [
        "schedulers/edf/agent_exp.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":edf_scheduler",
        ":topology",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/flags:parse",
    ],
)

cc_library(
    name = "shinjuku_scheduler",
    srcs = [
        "schedulers/shinjuku/shinjuku_orchestrator.cc",
        "schedulers/shinjuku/shinjuku_scheduler.cc",
    ],
    hdrs = [
        "schedulers/shinjuku/shinjuku_orchestrator.h",
        "schedulers/shinjuku/shinjuku_scheduler.h",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":ghost",
        ":shared",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
    ],
)

cc_binary(
    name = "agent_shinjuku",
    srcs = [
        "schedulers/shinjuku/agent_shinjuku.cc",
    ],
    copts = compiler_flags,
    visibility = ["//experiments/scripts:__pkg__"],
    deps = [
        ":agent",
        ":shinjuku_scheduler",
        ":topology",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/flags:parse",
    ],
)

cc_library(
    name = "sol_scheduler",
    srcs = [
        "schedulers/sol/sol_scheduler.cc",
    ],
    hdrs = [
        "schedulers/sol/sol_scheduler.h",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
    ],
)

cc_binary(
    name = "agent_sol",
    srcs = [
        "schedulers/sol/agent_sol.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":sol_scheduler",
        ":topology",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/flags:parse",
    ],
)

cc_binary(
    name = "sol_test",
    srcs = [
        "tests/sol_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":ghost",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/synchronization",
    ],
)

cc_binary(
    name = "simple_exp",
    srcs = [
        "tests/simple_exp.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        ":ghost",
    ],
)

cc_binary(
    name = "cfs_test",
    testonly = 1,
    srcs = [
        "tests/cfs_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        ":cfs_scheduler",
        ":ghost",
        ":topology",
        "@com_google_googletest//:gtest",
    ],
)

cc_binary(
    name = "simple_edf",
    srcs = [
        "tests/simple_edf.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":ghost",
        ":shared",
        "@com_google_absl//absl/flags:parse",
    ],
)

cc_binary(
    name = "simple_cfs",
    srcs = [
        "tests/simple_cfs.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        ":cfs_scheduler",
        ":ghost",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/strings",
    ],
)

cc_test(
    name = "agent_test",
    size = "small",
    srcs = [
        "tests/agent_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/status",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "api_test",
    size = "small",
    srcs = [
        "tests/api_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":fifo_per_cpu_scheduler",
        ":ghost",
        "@com_google_absl//absl/functional:any_invocable",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/status",
        "@com_google_googletest//:gtest_main",
    ],
)

exports_files(glob([
    "kernel/vmlinux_ghost_*.h",
]) + [
    "lib/queue.bpf.h",
])

cc_library(
    name = "topology",
    srcs = [
        "lib/topology.cc",
    ],
    hdrs = [
        "lib/topology.h",
    ],
    copts = compiler_flags,
    linkopts = ["-lnuma"],
    deps = [
        ":base",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/strings:str_format",
    ],
)

cc_library(
    name = "base",
    srcs = [
        "lib/base.cc",
    ],
    hdrs = [
        "kernel/ghost_uapi.h",
        "lib/base.h",
        "lib/logging.h",
        "//third_party:util/util.h",
    ],
    copts = compiler_flags,
    linkopts = ["-lcap"],
    deps = [
        "@com_google_absl//absl/base",
        "@com_google_absl//absl/base:core_headers",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:node_hash_map",
        "@com_google_absl//absl/debugging:stacktrace",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/log:check",
        "@com_google_absl//absl/memory",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
    ],
)

cc_test(
    name = "base_test",
    size = "small",
    srcs = [
        "tests/base_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        "@com_google_absl//absl/synchronization",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_binary(
    name = "agent_biff",
    srcs = [
        "schedulers/biff/agent_biff.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":biff_scheduler",
        ":topology",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/flags:parse",
    ],
)

bpf_skeleton(
    name = "biff_bpf_skel",
    bpf_object = "//third_party/bpf:biff_bpf",
    skel_hdr = "schedulers/biff/biff_bpf.skel.h",
)

cc_library(
    name = "biff_scheduler",
    srcs = [
        "schedulers/biff/biff_scheduler.cc",
    ],
    hdrs = [
        "schedulers/biff/biff_bpf.skel.h",
        "schedulers/biff/biff_scheduler.h",
        "//third_party/bpf:biff_bpf.h",
        "//third_party/bpf:topology.bpf.h",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_absl//absl/strings:str_format",
        "@linux//:libbpf",
    ],
)

cc_test(
    name = "bpf_queue_test",
    size = "small",
    srcs = [
        "lib/queue.bpf.h",
        "tests/bpf_queue_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        "@com_google_googletest//:gtest",
    ],
)

cc_test(
    name = "biff_test",
    size = "small",
    srcs = [
        "tests/biff_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":biff_scheduler",
        "@com_google_googletest//:gtest",
    ],
)

cc_binary(
    name = "agent_cfs_bpf",
    srcs = [
        "schedulers/cfs_bpf/agent_cfs.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":cfs_bpf_scheduler",
        ":topology",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/flags:parse",
    ],
)

bpf_skeleton(
    name = "cfs_bpf_skel",
    bpf_object = "//third_party/bpf:cfs_bpf",
    skel_hdr = "schedulers/cfs_bpf/cfs_bpf.skel.h",
)

cc_library(
    name = "cfs_bpf_scheduler",
    srcs = [
        "schedulers/cfs_bpf/cfs_scheduler.cc",
    ],
    hdrs = [
        "lib/queue.bpf.h",
        "schedulers/cfs_bpf/cfs_bpf.skel.h",
        "schedulers/cfs_bpf/cfs_scheduler.h",
        "//third_party/bpf:cfs_bpf.h",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_absl//absl/strings:str_format",
        "@linux//:libbpf",
    ],
)

cc_test(
    name = "cfs_bpf_test",
    size = "small",
    srcs = [
        "tests/cfs_bpf_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":cfs_bpf_scheduler",
        "@com_google_googletest//:gtest",
    ],
)

bpf_skeleton(
    name = "test_bpf_skel",
    bpf_object = "//third_party/bpf:test_bpf",
    skel_hdr = "bpf/user/test_bpf.skel.h",
)

cc_test(
    name = "capabilities_test",
    size = "small",
    srcs = [
        "bpf/user/test_bpf.skel.h",
        "tests/capabilities_test.cc",
    ],
    copts = compiler_flags,
    linkopts = bpf_linkopts,
    deps = [
        ":agent",
        ":capabilities_test_lib",
        ":ghost",
        "@linux//:libbpf",
    ],
)

cc_library(
    name = "capabilities_test_lib",
    testonly = 1,
    hdrs = [
        "tests/capabilities_test.h",
    ],
    copts = compiler_flags,
    linkopts = ["-lcap"],
    deps = ["@com_google_googletest//:gtest_main"],
)

cc_test(
    name = "channel_test",
    size = "small",
    srcs = [
        "tests/channel_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        "@com_google_googletest//:gtest_main",
    ],
)

bpf_skeleton(
    name = "edf_bpf_skel",
    bpf_object = "//third_party/bpf:edf_bpf",
    skel_hdr = "schedulers/edf/edf_bpf.skel.h",
)

cc_library(
    name = "edf_scheduler",
    srcs = [
        "schedulers/edf/edf_scheduler.cc",
        "schedulers/edf/orchestrator.cc",
    ],
    hdrs = [
        "schedulers/edf/edf_bpf.skel.h",
        "schedulers/edf/edf_scheduler.h",
        "schedulers/edf/orchestrator.h",
        "//third_party/bpf:edf.h",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":ghost",
        ":shared",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_absl//absl/strings:str_format",
        "@linux//:libbpf",
    ],
)

cc_test(
    name = "edf_test",
    size = "small",
    srcs = [
        "tests/edf_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":edf_scheduler",
        "@com_google_googletest//:gtest",
    ],
)

cc_test(
    name = "enclave_test",
    size = "small",
    srcs = [
        "tests/enclave_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":fifo_centralized_scheduler",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_binary(
    name = "fdcat",
    srcs = [
        "util/fdcat.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":shared",
    ],
)

cc_binary(
    name = "fdsrv",
    srcs = [
        "util/fdsrv.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":shared",
    ],
)

cc_test(
    name = "fd_server_test",
    size = "small",
    srcs = [
        "tests/fd_server_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":shared",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_binary(
    name = "fifo_per_cpu_agent",
    srcs = [
        "schedulers/fifo/per_cpu/fifo_agent.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":fifo_per_cpu_scheduler",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/flags:parse",
    ],
)

cc_library(
    name = "fifo_per_cpu_scheduler",
    srcs = [
        "schedulers/fifo/per_cpu/fifo_scheduler.cc",
        "schedulers/fifo/per_cpu/fifo_scheduler.h",
        "schedulers/fifo/Metric.h",
        "schedulers/fifo/Metric.cpp",
    ],
    hdrs = [
        "schedulers/fifo/per_cpu/fifo_scheduler.h",
        "schedulers/fifo/Metric.h",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
    ],
)

cc_binary(
    name = "fifo_centralized_agent",
    srcs = [
        "schedulers/fifo/centralized/fifo_agent.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":fifo_centralized_scheduler",
        ":topology",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/flags:parse",
    ],
)

cc_library(
    name = "fifo_centralized_scheduler",
    srcs = [
        "schedulers/fifo/centralized/fifo_scheduler.cc",
        "schedulers/fifo/centralized/fifo_scheduler.h",
        "schedulers/fifo/Metric.h",
        "schedulers/fifo/Metric.cpp",
    ],
    hdrs = [
        "schedulers/fifo/centralized/fifo_scheduler.h",
         "schedulers/fifo/Metric.h",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        "@com_google_absl//absl/strings:str_format",
        "@com_google_absl//absl/time",
    ],
)

cc_binary(
    name = "agent_flux",
    srcs = [
        "schedulers/flux/agent_flux.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":flux_scheduler",
        ":topology",
        "@com_google_absl//absl/debugging:symbolize",
        "@com_google_absl//absl/flags:parse",
    ],
)

bpf_skeleton(
    name = "flux_bpf_skel",
    bpf_object = "//third_party/bpf:flux_bpf",
    skel_hdr = "schedulers/flux/flux_bpf.skel.h",
)

cc_library(
    name = "flux_scheduler",
    srcs = [
        "schedulers/flux/flux_scheduler.cc",
    ],
    hdrs = [
        "lib/queue.bpf.h",
        "schedulers/flux/flux_bpf.skel.h",
        "schedulers/flux/flux_scheduler.h",
        "//third_party/bpf:flux_bpf.h",
        "//third_party/bpf:flux_infra",
        "//third_party/bpf:flux_scheds",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_absl//absl/strings:str_format",
        "@linux//:libbpf",
    ],
)

cc_test(
    name = "flux_test",
    size = "small",
    srcs = [
        "tests/flux_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":flux_scheduler",
        "@com_google_googletest//:gtest",
    ],
)

cc_library(
    name = "ghost",
    srcs = [
        "lib/ghost.cc",
    ],
    hdrs = [
        "kernel/ghost_uapi.h",
        "lib/ghost.h",
    ],
    copts = compiler_flags,
    linkopts = ["-lnuma"],
    deps = [
        ":base",
        ":topology",
        "@com_google_absl//absl/container:flat_hash_map",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/log",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
    ],
)

cc_test(
    name = "prio_table_test",
    size = "small",
    srcs = [
        "tests/prio_table_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":shared",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_library(
    name = "shared",
    srcs = [
        "shared/fd_server.cc",
        "shared/prio_table.cc",
        "shared/shmem.cc",
    ],
    hdrs = [
        "shared/fd_server.h",
        "shared/prio_table.h",
        "shared/shmem.h",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        "@com_google_absl//absl/cleanup",
        "@com_google_absl//absl/status",
        "@com_google_absl//absl/status:statusor",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/synchronization",
    ],
)

cc_binary(
    name = "enclave_watcher",
    srcs = [
        "util/enclave_watcher.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":ghost",
        "@com_google_absl//absl/flags:parse",
    ],
)

cc_binary(
    name = "pushtosched",
    srcs = [
        "util/pushtosched.cc",
    ],
    copts = compiler_flags,
    deps = [
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/strings:str_format",
    ],
)

cc_test(
    name = "topology_test",
    size = "small",
    srcs = [
        "tests/topology_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":ghost",
        ":topology",
        "@com_google_absl//absl/container:flat_hash_set",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/strings",
        "@com_google_googletest//:gtest",
    ],
)

bpf_skeleton(
    name = "schedclasstop_bpf_skel",
    bpf_object = "//third_party/bpf:schedclasstop_bpf",
    skel_hdr = "bpf/user/schedclasstop_bpf.skel.h",
)

cc_binary(
    name = "schedclasstop",
    srcs = [
        "bpf/user/schedclasstop.c",
        "bpf/user/schedclasstop_bpf.skel.h",
        "//third_party:iovisor_bcc/trace_helpers.h",
    ],
    copts = compiler_flags,
    linkopts = bpf_linkopts,
    deps = [
        "@linux//:libbpf",
    ],
)

bpf_skeleton(
    name = "schedfair_bpf_skel",
    bpf_object = "//third_party/bpf:schedfair_bpf",
    skel_hdr = "bpf/user/schedfair_bpf.skel.h",
)

cc_binary(
    name = "schedfair",
    srcs = [
        "bpf/user/schedfair.c",
        "bpf/user/schedfair_bpf.skel.h",
        "//third_party:iovisor_bcc/trace_helpers.h",
        "//third_party/bpf:schedfair.h",
    ],
    copts = compiler_flags,
    linkopts = bpf_linkopts,
    deps = [
        "@linux//:libbpf",
    ],
)

bpf_skeleton(
    name = "schedghostidle_bpf_skel",
    bpf_object = "//third_party/bpf:schedghostidle_bpf",
    skel_hdr = "bpf/user/schedghostidle_bpf.skel.h",
)

cc_binary(
    name = "schedghostidle",
    srcs = [
        "bpf/user/schedghostidle.c",
        "bpf/user/schedghostidle_bpf.skel.h",
        "//third_party:iovisor_bcc/trace_helpers.h",
    ],
    copts = compiler_flags,
    linkopts = bpf_linkopts,
    deps = [
        "@linux//:libbpf",
    ],
)

bpf_skeleton(
    name = "schedlat_bpf_skel",
    bpf_object = "//third_party/bpf:schedlat_bpf",
    skel_hdr = "bpf/user/schedlat_bpf.skel.h",
)

cc_binary(
    name = "schedlat",
    srcs = [
        "bpf/user/schedlat.c",
        "bpf/user/schedlat_bpf.skel.h",
        "//third_party:iovisor_bcc/trace_helpers.h",
        "//third_party/bpf:schedlat.h",
    ],
    copts = compiler_flags,
    linkopts = bpf_linkopts,
    deps = [
        "@linux//:libbpf",
    ],
)

bpf_skeleton(
    name = "schedrun_bpf_skel",
    bpf_object = "//third_party/bpf:schedrun_bpf",
    skel_hdr = "bpf/user/schedrun_bpf.skel.h",
)

cc_binary(
    name = "schedrun",
    srcs = [
        "bpf/user/schedrun.c",
        "bpf/user/schedrun_bpf.skel.h",
        "//third_party:iovisor_bcc/trace_helpers.h",
        "//third_party/bpf:schedrun.h",
    ],
    copts = compiler_flags,
    linkopts = bpf_linkopts,
    deps = [
        "@linux//:libbpf",
    ],
)

# Shared library for ghOSt tests.

cc_library(
    name = "experiments_shared",
    srcs = [
        "experiments/shared/prio_table_helper.cc",
        "experiments/shared/thread_pool.cc",
        "experiments/shared/thread_wait.cc",
    ],
    hdrs = [
        "experiments/shared/prio_table_helper.h",
        "experiments/shared/thread_pool.h",
        "experiments/shared/thread_wait.h",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        ":ghost",
        ":shared",
    ],
)

cc_test(
    name = "thread_pool_test",
    size = "small",
    srcs = [
        "experiments/shared/thread_pool.cc",
        "experiments/shared/thread_pool.h",
        "experiments/shared/thread_pool_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        ":ghost",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_absl//absl/synchronization",
        "@com_google_googletest//:gtest_main",
    ],
)

# The RocksDB binary and tests.

cc_binary(
    name = "rocksdb",
    srcs = [
        "experiments/rocksdb/cfs_orchestrator.cc",
        "experiments/rocksdb/cfs_orchestrator.h",
        "experiments/rocksdb/ghost_orchestrator.cc",
        "experiments/rocksdb/ghost_orchestrator.h",
        "experiments/rocksdb/main.cc",
    ],
    copts = compiler_flags,
    visibility = ["//experiments/scripts:__pkg__"],
    deps = [
        ":experiments_shared",
        ":rocksdb_lib",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_absl//absl/synchronization",
    ],
)

cc_library(
    name = "rocksdb_lib",
    srcs = [
        "experiments/rocksdb/database.cc",
        "experiments/rocksdb/ingress.cc",
        "experiments/rocksdb/latency.cc",
        "experiments/rocksdb/orchestrator.cc",
    ],
    hdrs = [
        "experiments/rocksdb/clock.h",
        "experiments/rocksdb/database.h",
        "experiments/rocksdb/ingress.h",
        "experiments/rocksdb/latency.h",
        "experiments/rocksdb/orchestrator.h",
        "experiments/rocksdb/request.h",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        ":experiments_shared",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:bit_gen_ref",
        "@com_google_absl//absl/time",
        "@rocksdb",
    ],
)

cc_test(
    name = "latency_test",
    size = "small",
    srcs = [
        "experiments/rocksdb/latency.cc",
        "experiments/rocksdb/latency.h",
        "experiments/rocksdb/latency_test.cc",
        "experiments/rocksdb/request.h",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "rocksdb_options_test",
    size = "small",
    srcs = [
        "experiments/rocksdb/cfs_orchestrator.cc",
        "experiments/rocksdb/cfs_orchestrator.h",
        "experiments/rocksdb/clock.h",
        "experiments/rocksdb/database.cc",
        "experiments/rocksdb/database.h",
        "experiments/rocksdb/ghost_orchestrator.cc",
        "experiments/rocksdb/ghost_orchestrator.h",
        "experiments/rocksdb/ingress.cc",
        "experiments/rocksdb/ingress.h",
        "experiments/rocksdb/latency.cc",
        "experiments/rocksdb/latency.h",
        "experiments/rocksdb/options_test.cc",
        "experiments/rocksdb/orchestrator.cc",
        "experiments/rocksdb/orchestrator.h",
        "experiments/rocksdb/request.h",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        ":experiments_shared",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:bit_gen_ref",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
        "@rocksdb",
    ],
)

cc_test(
    name = "rocksdb_orchestrator_test",
    size = "small",
    srcs = [
        "experiments/rocksdb/cfs_orchestrator.cc",
        "experiments/rocksdb/cfs_orchestrator.h",
        "experiments/rocksdb/clock.h",
        "experiments/rocksdb/database.cc",
        "experiments/rocksdb/database.h",
        "experiments/rocksdb/ghost_orchestrator.cc",
        "experiments/rocksdb/ghost_orchestrator.h",
        "experiments/rocksdb/ingress.cc",
        "experiments/rocksdb/ingress.h",
        "experiments/rocksdb/latency.cc",
        "experiments/rocksdb/latency.h",
        "experiments/rocksdb/orchestrator.cc",
        "experiments/rocksdb/orchestrator.h",
        "experiments/rocksdb/orchestrator_test.cc",
        "experiments/rocksdb/request.h",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        ":experiments_shared",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:bit_gen_ref",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
        "@rocksdb",
    ],
)

cc_test(
    name = "database_test",
    size = "small",
    srcs = [
        "experiments/rocksdb/database.cc",
        "experiments/rocksdb/database.h",
        "experiments/rocksdb/database_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
        "@com_google_googletest//:gtest",
        "@rocksdb",
    ],
)

cc_test(
    name = "synthetic_network_test",
    size = "medium",
    srcs = [
        "experiments/rocksdb/clock.h",
        "experiments/rocksdb/database.h",
        "experiments/rocksdb/ingress.cc",
        "experiments/rocksdb/ingress.h",
        "experiments/rocksdb/request.h",
        "experiments/rocksdb/synthetic_network_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        "@com_google_absl//absl/random",
        "@com_google_absl//absl/random:bit_gen_ref",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
        "@rocksdb",
    ],
)

# The Antagonist binary and tests.

cc_binary(
    name = "antagonist",
    srcs = [
        "experiments/antagonist/cfs_orchestrator.cc",
        "experiments/antagonist/cfs_orchestrator.h",
        "experiments/antagonist/ghost_orchestrator.cc",
        "experiments/antagonist/ghost_orchestrator.h",
        "experiments/antagonist/main.cc",
        "experiments/antagonist/orchestrator.cc",
        "experiments/antagonist/orchestrator.h",
        "experiments/antagonist/results.cc",
        "experiments/antagonist/results.h",
    ],
    copts = compiler_flags,
    visibility = ["//experiments/scripts:__pkg__"],
    deps = [
        ":base",
        ":experiments_shared",
        "@com_google_absl//absl/flags:flag",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_absl//absl/synchronization",
        "@com_google_absl//absl/time",
    ],
)

cc_test(
    name = "antagonist_options_test",
    size = "small",
    srcs = [
        "experiments/antagonist/options_test.cc",
        "experiments/antagonist/orchestrator.cc",
        "experiments/antagonist/orchestrator.h",
        "experiments/antagonist/results.cc",
        "experiments/antagonist/results.h",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        ":experiments_shared",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "antagonist_orchestrator_test",
    size = "small",
    srcs = [
        "experiments/antagonist/orchestrator.cc",
        "experiments/antagonist/orchestrator.h",
        "experiments/antagonist/orchestrator_test.cc",
        "experiments/antagonist/results.cc",
        "experiments/antagonist/results.h",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        ":experiments_shared",
        "@com_google_absl//absl/functional:bind_front",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_test(
    name = "results_test",
    size = "small",
    srcs = [
        "experiments/antagonist/results.cc",
        "experiments/antagonist/results.h",
        "experiments/antagonist/results_test.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        "@com_google_absl//absl/strings",
        "@com_google_absl//absl/time",
        "@com_google_googletest//:gtest_main",
    ],
)

cc_binary(
    name = "global_scalability",
    srcs = [
        "experiments/microbenchmarks/global_scalability.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":edf_scheduler",
        ":shinjuku_scheduler",
        ":sol_scheduler",
        "@com_google_absl//absl/flags:parse",
        "@com_google_absl//absl/flags:usage",
    ],
)

cc_test(
    name = "ioctl_test",
    size = "small",
    srcs = ["experiments/microbenchmarks/ioctl_test.cc"],
    copts = compiler_flags,
    deps = [
        ":agent",
        ":ghost",
        ":topology",
        "@com_google_benchmark//:benchmark",
        "@com_google_googletest//:gtest",
    ],
)

# Simple workload.

cc_binary(
    name = "simple_workload",
    srcs = [
        "tests/custom/simple_workload.cc",
    ],
    copts = compiler_flags,
    deps = [
        ":base",
        ":ghost",
        ":shared",
        "@com_google_absl//absl/flags:parse",
    ],
)

# Orca agent.

# cc_binary(
#     name = "orca_agent",
#     srcs = [
#         "schedulers/orca/agent.cc",
#     ],
#     copts = compiler_flags,
#     deps = [
#         ":agent",
#         ":fifo_per_cpu_scheduler",
#         ":cfs_scheduler",
#         "@com_google_absl//absl/debugging:symbolize",
#         "@com_google_absl//absl/flags:parse",
#         ":base",
#         ":topology",
#         "@com_google_absl//absl/container:flat_hash_map",
#         "@com_google_absl//absl/functional:any_invocable",
#         "@com_google_absl//absl/numeric:int128",
#         "@com_google_absl//absl/strings:str_format",
#         "@com_google_absl//absl/synchronization",
#         "@com_google_absl//absl/time",
#     ],
# )
