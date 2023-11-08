"""Helpers to generate abi-specific ghost targets"""

load("@bazel_skylib//lib:paths.bzl", "paths")
load("//:bpf/bpf.bzl", "bpf_program", "bpf_skeleton")

def massage_abi_deps(deps, abi = "latest", additional_targets = []):
    """Given a list of dependencies returns their abi-specific variants

    By default it will look for dependencies ending with ':agent', ':ghost' and ':ghost_uapi'
    but the caller can specify additional dependencies as well. Once an abi-specific dependency
    is identified it will change it to <orig_dep>_<abi_num> (for e.g. ':agent' -> ':agent_84').

    This macro is a no-op if abi = 'latest'

    Args:
      deps: list of dependencies
      abi: 'latest' or a specific abi number
      additional_targets: list of additional abi-specific targets to massage

    Returns:
      Modified list of abi-specific dependencies.
    """

    if abi == "latest":
        return deps

    uapi_targets = [
        ":agent",
        ":ghost",
        ":ghost_uapi",
    ]

    if additional_targets:
        uapi_targets.extend(additional_targets)

    newdeps = []
    for d in deps:
        if d.endswith(tuple(uapi_targets)):
            newdeps.append(d + "_" + str(abi))
        else:
            newdeps.append(d)
    return newdeps

def massage_bpf_skel_hdrs(hdrs, abi = "latest"):
    """Identify bpf skeleton headers and replace them with their abi-specific variants.

    This macro is a no-op if abi = 'latest'

    Otherwise this function looks for headers ending with '.skel.h' and then changes the path
    to include the <abi>.

    For e.g. if abi=84 then schedulers/fifo/bpf_fifo.skel.h will be rewritten to
    schedulers/fifo/84/bpf_fifo.skel.h

    Args:
      hdrs: list of headers
      abi: 'latest' or a specific abi number

    Returns:
      Modified list of abi-specific headers
    """

    if abi == "latest":
        return hdrs

    newhdrs = []
    for d in hdrs:
        if d.endswith(".skel.h"):
            newhdrs.append(paths.dirname(d) + "/" + str(abi) + "/" + paths.basename(d))
        else:
            newhdrs.append(d)
    return newhdrs

def define_ghost_uapi(name, abi):
    """Creates a ghost_uapi cc_library target for the specific abi.

    define_ghost_uapi(name = 'ghost_uapi', abi = 'latest') will create the ':ghost_uapi' target
    for the latest abi.

    define_ghost_uapi(name = 'ghost_uapi', abi = 84) will create the ':ghost_uapi_84' target
    with GHOST_SELECT_ABI=84 added to 'defines'. The use of 'defines' versus 'local_defines'
    is intentional because we want all targets that depend on this abi-specific target to be
    able to tell exactly what ABI they are being compiled against.

    Args:
      name: base name for the cc_library target
      abi: 'latest' or a specific abi number
    """

    defines = []
    if abi != "latest":
        name = name + "_" + str(abi)
        defines.append("GHOST_SELECT_ABI=" + str(abi))

    native.cc_library(
        name = name,
        srcs = [
            "abi/" + str(abi) + "/kernel/ghost.h",
            "lib/ghost_uapi.cc",
        ],
        hdrs = [
            "lib/ghost_uapi.h",
        ],
        defines = defines,
        visibility = [
        ],
        alwayslink = 1,
    )

def cc_library_ghost(name, abi, srcs, hdrs, deps, visibility = [], copts = [], linkopts = []):
    """Creates a cc_library target for the specific abi.

    If abi == 'latest' then this macro behaves exactly like cc_library and
    it creates a cc_library target called <name>.

    If abi != 'latest' then the cc_library target is called <name>_<abi>.
    Additionally uapi dependent targets in 'deps' are also modified as
    described in massage_abi_deps().

    Args:
      name: base name for the cc_library target
      abi: 'latest' or a specific abi number
      srcs: source files
      hdrs: headers
      deps: dependencies
      visibility: target visibility
      copts: compiler options
      linkopts: linker options
    """

    if abi != "latest":
        name = name + "_" + str(abi)

    native.cc_library(
        name = name,
        srcs = srcs,
        hdrs = massage_bpf_skel_hdrs(hdrs, abi),
        copts = copts,
        linkopts = linkopts,
        visibility = visibility,
        deps = massage_abi_deps(deps, abi),
    )

def bpf_skel_ghost(name, src, hdrs, abi = "latest", objdir = ""):
    """Creates bpf_program and bpf_skeleton targets for an abi-specific bpf program

    bpf_skel_ghost("fifo", "bpf_fifo.bpf.c", bpf_fifo_hdrs) will generate:
    - bpf object at "schedulers/fifo/bpf_fifo_bpf.o"
    - skeleton header at "schedulers/fifo/bpf_fifo.skel.h"
    The variant above is designed for agents that bundle a bpf program in their cc_binary.

    bpf_skel_ghost("fifo", bpf_fifo.bpf.c, bpf_fifo_hdrs, abi = 84, objdir = "bpf/user") will generate:
    - bpf object at "bpf/user/84/bpf_fifo_bpf.o
    - skeleton header at "bpf/user/84/bpf_fifo.skel.h"
    The variant above is less common and currently used to compile the schedghostidle bpf program
    bundled with the agent.

    Args:
      name: base name for the bpf_program and bpf_skeleton targets
      src: bpf source file
      hdrs: list of headers to compile the bpf source file
      abi: 'latest' or a specific abi number
      objdir: optional directory to generate the bpf object and skeleton (default is "schedulers/<name>")
    """

    bpf_name = "bpf_" + name
    prog_name = bpf_name
    skel_name = bpf_name + "_skel"
    macros = []
    hdrs = list(hdrs)  # make a local copy

    if not objdir:
        objdir = "schedulers/" + name

    if abi != "latest":
        prog_name += "_" + str(abi)
        skel_name += "_" + str(abi)
        objdir = objdir + "/" + str(abi)
        macros.append("GHOST_SELECT_ABI=" + str(abi))

    hdrs.append("//:lib/ghost_uapi.h")
    hdrs.append("//:abi/" + str(abi) + "/kernel/ghost.h")

    bpf_object = objdir + "/" + bpf_name + "_bpf.o"
    skel_hdr = objdir + "/" + bpf_name + ".skel.h"

    bpf_program(
        name = prog_name,
        src = src,
        hdrs = hdrs,
        bpf_object = bpf_object,
        macros = macros,
    )

    bpf_skeleton(
        name = skel_name,
        bpf_object = prog_name,
        skel_hdr = skel_hdr,
    )
