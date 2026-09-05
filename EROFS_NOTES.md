# rvlinux EROFS build and container notes

This tree can build `rvlinux` with a read-only guest root backed by liberofs. The
host opens and memory-maps one EROFS image; guest path operations are resolved
inside that image instead of against the host filesystem.

## Host build dependencies

On a Debian/Ubuntu builder:

```dockerfile
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git autoconf automake libtool pkg-config \
    erofs-utils ca-certificates \
 && rm -rf /var/lib/apt/lists/*
```

`autoconf`, `automake`, `libtool`, and `pkg-config` are needed because CMake
builds the pinned liberofs source with its autotools build. `erofs-utils`
provides `mkfs.erofs`, `dump.erofs`, and `fsck.erofs` for creating and checking
images.

## Clone and build

The liberofs source is a pinned Git submodule, so initialize submodules:

```dockerfile
WORKDIR /src
RUN git clone --recurse-submodules https://github.com/directionallyai/libriscv.git
WORKDIR /src/libriscv

RUN cmake -S emulator -B emulator/build \
      -DCMAKE_BUILD_TYPE=Release \
      -DSTATIC_BUILD=OFF \
      -DRISCV_VIRTUAL_PAGING=ON \
      -DRISCV_VERBOSE_SYSCALLS=ON \
 && cmake --build emulator/build -j"$(nproc)"
```

If the repository was cloned without `--recurse-submodules`:

```sh
git submodule update --init --recursive
```

The binary is `emulator/build/rvlinux`. It is dynamically linked by default,
so the runtime container needs the host C++ runtime (`libstdc++6`, `libgcc-s1`,
and libc on Debian). liberofs itself is linked into `rvlinux` as a static
library.

## Build an uncompressed guest image

Prepare a RISC-V root directory containing the dynamic loader, libraries, and
programs required by the guest. Build the image without a `-z` option:

```dockerfile
RUN mkfs.erofs -T 0 /opt/rootfs.erofs /opt/riscv-root
```

`-T 0` makes image timestamps deterministic. Omitting `-z` produces the
uncompressed extents required by the direct-page mapping path. Verify it with:

```sh
dump.erofs -s /opt/rootfs.erofs
```

A currently expected feature line is similar to:

```text
Filesystem features: sb_csum mtime
```

Compression features such as LZ4 should not appear.

## Run a guest

```sh
rvlinux --erofs /opt/rootfs.erofs /usr/bin/python3 -- /test.py
```

Arguments after `--` belong to the guest executable. Useful diagnostic options
include:

```sh
rvlinux --erofs /opt/rootfs.erofs \
  --verbose-syscalls --non-interactive -n -1 \
  /usr/bin/python3 -- /test.py
```

Use `--no-translate` (`-n`) when native translation is not wanted. Native
translation is not intended as a security boundary for hostile guest code.

Do not combine `--erofs` with `--proxy`: proxy mode deliberately exposes the
host filesystem and the CLI treats the modes as mutually exclusive.

## Filesystem and socket behavior

In EROFS mode:

- Guest pathname lookup is confined to the EROFS image.
- Regular files and directories are readable.
- Write-capable opens and filesystem mutations fail with `EROFS`.
- Guest regular-file descriptors are backed by sealed read-only memfds.
- Eligible full, uncompressed pages are ultimately backed directly by the
  host's read-only EROFS image mapping. A final partial page remains private so
  bytes beyond EOF are zero-filled safely.
- Socket syscalls are enabled by default, while filesystem access remains
  image-scoped and read-only.
- Passing `--sandbox` disables sockets as well.
- Standard input, output, and error remain connected to the rvlinux process.

The current direct-page implementation first uses the generic file mapping and
then replaces eligible pages with image-backed pages. It provides shared,
read-only steady-state backing across VMs, but it does not yet eliminate the
initial file-copy bandwidth.

## Readiness timestamp

EROFS mode provides a synthetic read-only `/dev/ready`. Its first open prints
the elapsed time since guest simulation began and returns a file containing
`ok\n`. For example:

```python
with open("/dev/ready") as ready:
    print(ready.read().strip())
print("hello world")
```

Typical output is:

```text
Ready: 248.410 ms
ok
hello world
```

This is useful for distinguishing dynamic-loader/interpreter initialization
from work performed after the application declares itself ready.

## HTTP proxy environment variables

Python's `urllib`, Requests, and many other HTTP clients honor conventional
proxy variables:

```text
HTTP_PROXY=http://proxy.example:8080
HTTPS_PROXY=http://proxy.example:8080
NO_PROXY=localhost,127.0.0.1
```

Lowercase forms are also commonly supported. rvlinux currently constructs a
small fixed guest environment and does **not** automatically forward these
variables from the container environment. A Docker `ENV HTTP_PROXY=...` alone
therefore does not make the value visible to guest Python; rvlinux must first
be changed to explicitly forward the desired variables, or the guest program
must receive/configure the proxy by another mechanism.

## Minimal multi-stage Dockerfile outline

```dockerfile
FROM debian:stable-slim AS builder
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake git autoconf automake libtool pkg-config \
    erofs-utils ca-certificates \
 && rm -rf /var/lib/apt/lists/*
WORKDIR /src
RUN git clone --recurse-submodules https://github.com/directionallyai/libriscv.git
WORKDIR /src/libriscv
RUN cmake -S emulator -B emulator/build \
      -DCMAKE_BUILD_TYPE=Release -DSTATIC_BUILD=OFF \
      -DRISCV_VIRTUAL_PAGING=ON -DRISCV_VERBOSE_SYSCALLS=ON \
 && cmake --build emulator/build -j"$(nproc)"

# Populate this directory in a project-specific earlier step.
COPY riscv-root/ /opt/riscv-root/
RUN mkfs.erofs -T 0 /opt/rootfs.erofs /opt/riscv-root

FROM debian:stable-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    libstdc++6 libgcc-s1 \
 && rm -rf /var/lib/apt/lists/*
COPY --from=builder /src/libriscv/emulator/build/rvlinux /usr/local/bin/rvlinux
COPY --from=builder /opt/rootfs.erofs /opt/rootfs.erofs
ENTRYPOINT ["/usr/local/bin/rvlinux", "--erofs", "/opt/rootfs.erofs"]
CMD ["/usr/bin/python3", "--", "/test.py"]
```

The construction of `riscv-root/` is intentionally project-specific. It must
contain RISC-V binaries and their RISC-V dynamic loader/libraries; the runtime
container itself remains the host architecture.
