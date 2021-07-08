load("@rules_foreign_cc//foreign_cc:defs.bzl", "make")

# The libbpf source code. This is encompassed by the `:source` filegroup, but
# the `make` rule below wants just the library source passed via the
# `lib_source` parameter.
filegroup(
    name = "libbpf_source",
    srcs = glob(["tools/lib/bpf/**"]),
    visibility = ["//visibility:private"],
)

# The bpftool source code. This is encompassed by the `:source` filegroup, but
# the `make` rule below wants just the library source passed via the
# `lib_source` parameter.
filegroup(
    name = "bpftool_source",
    srcs = glob(["tools/bpf/bpftool/**"]),
    visibility = ["//visibility:private"],
)

# The Linux source code.
filegroup(
    name = "source",
    srcs = glob(["**"]),
    visibility = ["//visibility:private"],
)

# Compiles the libbpf static library.
make(
    name = "libbpf",
    # This is the library source. This filegroup includes the Makefile.
    lib_source = ":libbpf_source",
    # The Makefile uses other files in the Linux kernel tree outside of its
    # directory during the build process (e.g.,
    # `tools/scripts/Makefile.include`).
    build_data = [":source"],
    # This is the target passed to `make` (i.e., `make libbpf.a`).
    targets = ["libbpf.a"],
    # This copy should be done automatically by the rules_foreign_cc tool, yet
    # it is not. This may happen because the libbpf library is not at the root
    # of the Linux kernel tree. Perhaps the rules_foreign_cc tool makes an
    # assumption that the library source is at the root of the kernel tree,
    # which causes its copy of libbpf.a to fail since it cannot find the static
    # library at the root of the kernel tree.
    #
    # Note: The values of the environment variables below are written to
    # GNUMake.log, so look at that file to inspect them. You can also look at
    # that log to see which other environment variables exist.
    postfix_script = "cp $EXT_BUILD_ROOT/external/linux/tools/lib/bpf/libbpf.a $INSTALLDIR/lib/libbpf.a; " +
    # By making the `libbpf` directory and copying the libbpf header files into
    # it, we can have the #include paths in the project prefixed by `libbpf`. In
    # other words, we can do `#include "libbpf/header.h"` instead of
    # `#include "header.h"`. With the latter, it is more confusing to figure out
    # where the header file is and could cause conflicts if a header file in the
    # project has the same name as a header file in libbpf.
    "mkdir $INSTALLDIR/include/libbpf; " +
    "cp $EXT_BUILD_ROOT/external/linux/tools/lib/bpf/*.h $INSTALLDIR/include/libbpf",
    visibility = ["//visibility:public"],
)

# Compiles the bpftool binary.
make(
    name = "bpftool",
    lib_source = ":bpftool_source",
    # This attribute specifies that the output is a binary. Otherwise, the
    # rules_foreign_cc tool expects to find a static library (i.e., `bpftool.a`)
    # and fails when the static library is not produced.
    out_binaries = ["bpftool"],
    build_data = [":source"],
    # The default targets are `` and `install`, but we do not want the `install`
    # target. Thus, specify that the only target is `` (i.e., just `make`).
    targets = [""],
    # See the comment in the `:libbpf` target for an explanation of why this
    # copy is necessary.
    postfix_script = "cp $EXT_BUILD_ROOT/external/linux/tools/bpf/bpftool/bpftool $INSTALLDIR/bin/bpftool",
    visibility = ["//visibility:public"],
)
