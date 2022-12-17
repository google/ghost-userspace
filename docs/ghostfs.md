# ghostfs Interface

This document guides you to navigate our `ghostfs` interface. Before getting
started, note that ghOSt is not an upstream feature so you should make sure you
compile and use our open-source
[`ghost-kernel`](https://github.com/google/ghost-kernel).

## Mount ghOSt `kernfs` (a.k.a. `ghostfs`)

Before starting any interaction with ghOSt, you need to first mount the
`ghostfs`. It is implemented with `kernfs` and enables you to perform ghOSt
operations via simple file IO syscalls (e.g., `write`, `read`). You can use the
following commands to mount the`ghostfs` and check whether it exposes the top
level interfaces to start with.

```
$ mount --source ghost --target /sys/fs/ghost -t ghost
$ ls /sys/fs/ghost
ctl  version
```

### Using the C++ ghOSt Library

You can also mount `ghostfs` using `void GhostHelper()->MountGhostfs()`. Upon
failure, it raises CHECK failure, which causes the caller process to crash. If
the function succeeds or finds that the `ghostfs` has already been mounted, the
function returns without any failure.

## Top Directory (/sys/fs/ghost)

### At a glance

In the top directory `/sys/fs/ghost`, ghOSt exposes two key basic ghOSt
operations.

Operation           | Path                    | Mode | Suppored Operations
------------------- | ----------------------- | ---- | -------------------
*Get ABI version\** | `/sys/fs/ghost/version` | 0444 | `read`
*Create an Enclave* | `/sys/fs/ghost/ctl`     | 0660 | `write`

*   **ABI Version**: ABI stands for "Application Binary Interface". In ghOSt, it
    refers to the interface between the ghOSt userspace library and the ghOSt
    kernel. To guarantee that the two ghOSt counterparts work together
    correctly, it is important and required to match their ABI versions. i.e.,
    the ghOSt userspace crashes if it finds out that its ABI version does not
    match any of those supported by the ghOSt kernel. We tag each ABI version
    with a monotonically increasing integer and the number increases only when
    there is any *breaking* change in the interface. The ABI version is not
    updated every time we update the userspace or kernel ghOSt code.

### Get ABI Version

By reading `/sys/fs/ghost/version`, you can check the list of ghOSt ABI versions
supported by the current kernel. For example, if a kernel supports ABI version
`74`, you will see the following output from `cat`:

```
$ cat /sys/fs/ghost/version
74
```

`read()` always succeeds, returning the number of bytes read from the file. The
output buffer will contain each ABI version number string followed by a new line
character, for example, `75\n74\n`.

#### Using the C++ ghOSt Library

The userspace ghOSt library provides a helper function `int
GhostHelper()->GetSupportedVersions(std::vector<uint32_t>& versions)` to read
the list of ghOSt ABI versions supported by the current kernel. ABI version
strings are parsed into `uint32_t` variables and appended to `versions`. If
successful, the function returns `0`. If the function fails to parse the version
string into integers, the function returns `-1`.

### Create an Enclave

You can create a new enclave in ghOSt by writing a string command to
`/sys/fs/ghost/ctl`.

```
$ echo "<command>" > /sys/fs/ghost/ctl
```

#### Command

Callers of `write()` provide a `command` string in the following form:

```
create ${id} ${abi_version} ${extra_opts}
```

1.  `${id}`
    *   *Type*: `unsigned long (%lu)`
    *   *Required*
    *   The new enclave's ID (`unsigned long`). The new enclave will be named
        `enclave_${id}` (e.g., `enclave_1` if `id = 1`).
2.  `${abi_version}`
    *   *Type*: `unsigned int (%u)`
    *   *Required*
    *   The ABI version (`unsigned int`) to use for this API call.
3.  `${extra_opts}`
    *   *Type*: `string`
    *   Optional
    *   Extra arguments for creating the enclave. These arguments can have any
        of the following key-value pairs (`gid`) or key-only boolean flags
        (`ephemeral`) formatted in `parse_args` format such as `foo=bar,bar2
        baz=fuz wiz`.
        *   `gid=${gid}`: The group ID for the new enclave directory in
            `kernfs`.
        *   `ephemeral`: Whether to tie the new enclave's lifetime to that of
            the calling process (a.k.a., thread group), i.e., the new enclave is
            automatically destroyed when the process that created it exits.
    *   Example: `gid=1234 ephemeral`

#### Return Value & Error Codes

On success, `write()` returns the length of the command string. On error, `-1`
is returned, and `errno` is set to indicate the error.

*   `ENOENT 2 No such file or directory`
    *   Unknown option found when parsing `${extra_opts}`.
*   `ESRCH 3 No such process`
    *   Input ghOSt ABI version is not supported by the kernel, i.e., is not one
        of the kernel's supported versions.
*   `ENOMEM 12 Cannot allocate memory`
    *   Failed to create `kernfs` directory node.
    *   Failed to allocate memory for enclave/cpu data structures.
*   `EINVAL 22 Invalid argument`
    *   Failed to parse the given command (i.e., `sscanf` failure).
*   `ENOSPC 28 No space left on device`
    *   Enclave name (`enclave_${id}`) is too long, i.e., `len(name) >= 31`.

#### Using the C++ ghOSt Library

The C++ ghOSt library provides `int LocalEnclave::MakeNextEnclave()` to create a
new enclave. This function creates an enclave named `enclave_$(id)` where
`${id}` is the smallest positive integer that is not used for another existing,
active enclave. If an enclave is successfully created, the function returns the
integer file descriptor of the new enclave's directory. The function returns
`-1` if it fails to create an enclave.

### Example Workflow

**Step 1:** Check whether the ghOSt `kernfs` is mounted.

```
$ ls -all /sys/fs/ghost
-rw-rw---- 1 root root 0 ctl
-r--r--r-- 1 root root 0 version
```

**Step 2:** Get the ABI version from the ghOSt kernel.

```
# Get ABI version.
$ cat /sys/fs/ghost/version
74
```

**Step 3:** Create an enclave with `id=1` and the ABI version fetched from (2).

```
# Pass the ABI version to ctl in order to create an enclave.
$ echo "create 1 74" > /sys/fs/ghost/ctl
```

**Step 4:** Check that a new directory named `enclave_1` is created.

```
$ ls -all /sys/fs/ghost
-rw-rw---- 1 root root 0 ctl
dr-xr-xr-x 3 root root 0 enclave_1
-r--r--r-- 1 root root 0 version
```
