"""Helpers to generate abi-specific ghost targets"""

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
        ],
        hdrs = [
            "lib/ghost_uapi.h",
        ],
        defines = defines,
        visibility = [
        ],
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
        hdrs = hdrs,
        copts = copts,
        linkopts = linkopts,
        visibility = visibility,
        deps = massage_abi_deps(deps, abi),
    )
