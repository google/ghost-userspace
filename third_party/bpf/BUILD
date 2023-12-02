# Note: If you modify this BUILD file, please contact jhumphri@ first to ensure
# that you are not breaking the Copybara script.

load("//:bpf/bpf.bzl", "bpf_program")

package(
    default_applicable_licenses = ["//:license"],
    default_visibility = [
        "//:__subpackages__",
    ],
)

# We use the GPLv2 license for the eBPF code so that we can access kernel
# functionality restricted to eBPF programs that are licensed under GPLv2. That
# being said, keep in mind that all of this eBPF code is authored and owned by
# Google.
licenses(["restricted"])

exports_files(
    [
        "biff_bpf.h",
        "cfs_bpf.h",
        "common.bpf.h",
        "edf.h",
        "flux_bpf.h",
        "pntring.bpf.h",
        "pntring_funcs.bpf.h",
        "schedfair.h",
        "schedlat.h",
        "schedrun.h",
        "topology.bpf.h",
        "schedghostidle.bpf.c",
    ],
)

filegroup(
    name = "flux_infra",
    srcs = [
        "flux_api.bpf.c",
        "flux_dispatch.bpf.c",
        "flux_header_bpf.h",
    ],
)

filegroup(
    name = "flux_scheds",
    srcs = [
        "biff_flux.bpf.c",
        "biff_flux_bpf.h",
        "idle_flux.bpf.c",
        "idle_flux_bpf.h",
        "prov_flux.bpf.c",
        "prov_flux_bpf.h",
        "roci_flux.bpf.c",
        "roci_flux_bpf.h",
    ],
)

bpf_program(
    name = "biff_bpf",
    src = "biff.bpf.c",
    hdrs = [
        "biff_bpf.h",
        "common.bpf.h",
        "topology.bpf.h",
        "//:abi/latest/kernel/ghost.h",
        "//:lib/ghost_uapi.h",
    ],
    bpf_object = "biff_bpf.o",
)

bpf_program(
    name = "cfs_bpf",
    src = "cfs.bpf.c",
    hdrs = [
        "cfs_bpf.h",
        "common.bpf.h",
        "//:abi/latest/kernel/ghost.h",
        "//:arr_structs",
        "//:lib/ghost_uapi.h",
    ],
    bpf_object = "cfs_bpf.o",
)

bpf_program(
    name = "edf_bpf",
    src = "edf.bpf.c",
    hdrs = [
        "common.bpf.h",
        "edf.h",
        "//:abi/latest/kernel/ghost.h",
        "//:lib/ghost_uapi.h",
    ],
    bpf_object = "edf_bpf.o",
)

bpf_program(
    name = "flux_bpf",
    src = "flux.bpf.c",
    hdrs = [
        "common.bpf.h",
        "flux_bpf.h",
        ":flux_infra",
        ":flux_scheds",
        "//:abi/latest/kernel/ghost.h",
        "//:arr_structs",
        "//:lib/ghost_uapi.h",
    ],
    bpf_object = "flux_bpf.o",
)

bpf_program(
    name = "schedclasstop_bpf",
    src = "schedclasstop.bpf.c",
    hdrs = [
        "common.bpf.h",
    ],
    bpf_object = "schedclasstop_bpf.o",
)

bpf_program(
    name = "schedfair_bpf",
    src = "schedfair.bpf.c",
    hdrs = [
        "common.bpf.h",
        "schedfair.h",
        "//third_party:iovisor_bcc/bits.bpf.h",
    ],
    bpf_object = "schedfair_bpf.o",
)

bpf_program(
    name = "schedlat_bpf",
    src = "schedlat.bpf.c",
    hdrs = [
        "common.bpf.h",
        "schedlat.h",
        "//third_party:iovisor_bcc/bits.bpf.h",
    ],
    bpf_object = "schedlat_bpf.o",
)

bpf_program(
    name = "schedrun_bpf",
    src = "schedrun.bpf.c",
    hdrs = [
        "common.bpf.h",
        "schedrun.h",
        "//third_party:iovisor_bcc/bits.bpf.h",
    ],
    bpf_object = "schedrun_bpf.o",
)

bpf_program(
    name = "test_bpf",
    src = "test.bpf.c",
    hdrs = [
        "common.bpf.h",
        "//:abi/latest/kernel/ghost.h",
        "//:lib/ghost_uapi.h",
    ],
    bpf_object = "test_bpf.o",
)
