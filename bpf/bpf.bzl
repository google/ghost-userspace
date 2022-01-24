"""The open source build rules for eBPF programs and skeleton headers."""

def bpf_program(name, src, hdrs, bpf_object, **kwargs):
    """Generates an eBPF object file from .c source code.

    Args:
      name: target name for eBPF program.
      src: eBPF program source code in C.
      hdrs: list of header files depended on by src.
      bpf_object: name of generated eBPF object file.
      **kwargs: additional arguments.
    """
    native.genrule(
        name = name,
        srcs = ["@linux//:libbpf"] + [src] + hdrs,
        outs = [bpf_object],
        cmd = (
            "clang-12 -g -O2 -target bpf -D__TARGET_ARCH_x86 " +
            # The `.` directory is the project root, so we pass it with the `-I`
            # flag so that #includes work in the source files.
            #
            # `$(BINDIR)/external/linux` contains the outputs of the targets in
            # linux.BUILD. Thus, the headers for libbpf are within that
            # directory at libbpf/include/*
            # (i.e., $(BINDIR)/external/linux/libbpf/include/*).
            #
            # `$@` is the location to write the eBPF object file.
            "-I . -I $(BINDIR)/external/linux/libbpf/include -c $(location " + src + ") -o $@ && " +
            "llvm-strip -g $@"
        ),
        **kwargs
    )

def bpf_skeleton(name, bpf_object, skel_hdr, **kwargs):
    """Generates eBPF skeleton from object file to .c source code.

    Args:
      name: target name for eBPF program.
      bpf_object: built eBPF program.
      skel_hdr: name of generated skeleton header file.
      **kwargs: additional arguments.
    """
    native.genrule(
        name = name,
        # bpftool does not seem to be compiled when I include it in the `tools`
        # attribute list instead.
        srcs = ["@linux//:bpftool", bpf_object],
        outs = [skel_hdr],
        cmd = (
            "$(BINDIR)/external/linux/bpftool/bin/bpftool gen skeleton $(location " + bpf_object + ") > $@ && " +
            # The libbpf headers are located in `libbpf` rather than `bpf`.
            "sed -i 's/#include <bpf/#include <libbpf/g' $@"
        ),
        **kwargs
    )
