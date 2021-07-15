# Note: If you modify this BUILD file, please contact jhumphri@ first to ensure
# that you are not breaking the Copybara script.

load("//:bpf/bpf.bzl", "bpf_program")

package(default_visibility = ["//:__pkg__"])

# We use the GPLv2 license for the eBPF code so that we can access kernel
# functionality restricted to eBPF programs that are licensed under GPLv2. That
# being said, keep in mind that all of this eBPF code is authored and owned by
# Google.
licenses(["restricted"])

bpf_program(
    name = "ghost_bpf",
    src = "ghost.bpf.c",
    hdrs = [
        "ghost_shared_bpf.h",
        "//:kernel/vmlinux_ghost_5_11.h",
    ],
    bpf_object = "ghost_bpf.o",
)

bpf_program(
    name = "schedclasstop_bpf",
    src = "schedclasstop.bpf.c",
    hdrs = [
        "//:kernel/vmlinux_ghost_5_11.h",
    ],
    bpf_object = "schedclasstop_bpf.o",
)

bpf_program(
    name = "schedghostidle_bpf",
    src = "schedghostidle.bpf.c",
    hdrs = [
        "//:kernel/vmlinux_ghost_5_11.h",
        "//third_party:iovisor_bcc/bits.bpf.h",
    ],
    bpf_object = "schedghostidle_bpf.o",
)

bpf_program(
    name = "schedlat_bpf",
    src = "schedlat.bpf.c",
    hdrs = [
        "common.bpf.h",
        "schedlat_shared_bpf.h",
        "//:kernel/vmlinux_ghost_5_11.h",
        "//third_party:iovisor_bcc/bits.bpf.h",
    ],
    bpf_object = "schedlat_bpf.o",
)

bpf_program(
    name = "schedrun_bpf",
    src = "schedrun.bpf.c",
    hdrs = [
        "common.bpf.h",
        "schedrun_shared_bpf.h",
        "//:kernel/vmlinux_ghost_5_11.h",
        "//third_party:iovisor_bcc/bits.bpf.h",
    ],
    bpf_object = "schedrun_bpf.o",
)
