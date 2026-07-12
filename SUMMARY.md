# Summary: musl libc port to BareMetal-Firecracker

What this covers: porting musl libc to the BareMetal exokernel (the
`app/` build, previously just a hand-written "hello world" in C
calling `libBareMetal.h` directly), then building a POSIX file I/O
layer on top of it (ext4, replacing an earlier custom BMFS-based
layer), then TCP/IP networking (lwIP). Everything below was verified
by actually booting the result in Firecracker and checking real
output — not just "it compiled" — except the ext4 rewrite itself,
verified on the host instead (see "Verification" below).

See `OPENISSUES.md` for what's still missing/limited.

## Why this isn't a normal port

BareMetal-Firecracker apps are flat binaries loaded at a fixed
virtual address (`0xFFFF800000000000`), running in **ring 0**, in the
**same flat address space as the kernel** — no MMU-enforced
user/kernel boundary, no syscall trap, no ELF loader. An app calls the
kernel by `call`ing fixed absolute addresses (`b_output`, `b_input`,
`b_nvs_read`, etc. — `app/libBareMetal.h`). This shaped nearly every
decision below: there's no `syscall` instruction to intercept, no
processes, no real threads, and (as discovered partway through) no
room for position-independent codegen either.

## Core musl port (`crt0.c`, `posix_shim.c`, musl patches)

**Syscall transport.** musl's `arch/x86_64/syscall_arch.h` normally
does the `syscall` instruction. Patched it (and
`src/thread/x86_64/syscall_cp.s`, the cancellation-point path) to call
a C dispatcher, `__bmos_syscall()`, instead — syscall numbers are used
purely as dispatch keys into `posix_shim.c`, never executed as real
traps.

**TLS bootstrap.** `arch_prctl(ARCH_SET_FS)` is a real Linux syscall;
here it's replaced with a direct `wrmsr` to `IA32_FS_BASE`
(`src/thread/x86_64/__set_thread_area.s`) — legal because the app
already runs at ring 0, no privilege transition needed.

**Startup (`crt0.c`).** BareMetal just calls `_start()` with no
argv/envp/auxv. `crt0.c` fabricates a minimal fake initial stack
(`argc=0`, empty envp, an auxv carrying only `AT_PAGESZ` and
`AT_RANDOM`) and hands it to musl's *real* `__libc_start_main()`,
rather than reimplementing startup by hand.

**Two real bugs found via live crashes, not inspection:**
1. Host `gcc` defaults to PIE. Taking a function's address as a
   *value* (not calling it directly — e.g. passing `main` as a
   function pointer) compiled to a GOT-relative load, which read
   garbage because the flat-binary linker script has no real `.got`
   section to resolve it against. Direct calls worked; indirect calls
   didn't — that contrast is what isolated it. Fixed with `-fno-pic
   -fno-pie`.
2. That alone then broke on `0xFFFF800000000000` being unreachable via
   32-bit-relative/absolute relocations (the default "small" code
   model assumes addresses fit in 32 bits). Fixed with
   `-mcmodel=large` (full 64-bit immediates, no range assumptions).

**Linker script (`c.ld`).** Added `.init_array`/`.fini_array` (real,
if empty, sections — musl's startup/exit path walks these bounds) and
`__image_base`/`_DYNAMIC` symbols, all needed to stop
"relocation truncated to fit" errors caused by weak/undefined symbols
defaulting to address 0, which is unreachable from code sitting at
`0xFFFF800000000000` within a 32-bit displacement.

## Heap (`posix_shim.c`)

`brk()` and anonymous `mmap()` share one bump allocator starting at
`__bss_stop`, sized once from `b_system(FREE_MEMORY, 0, 0)` (actual
configured microVM RAM) rather than a guessed constant. `mmap()` was
added because mallocng routes any single allocation ≥128KB through it
regardless of `brk()` — without it, large `malloc()`s would fail
outright. `munmap()` is a no-op (documented limitation — no real
paging to reclaim into).

## ext4 file I/O (`ext4.c`, `ext4.h`)

The kernel has no filesystem, only raw 4096-byte sector I/O
(`b_nvs_read`/`b_nvs_write`), and originally this layer was BMFS (a
small, custom, reverse-engineered format). It was replaced with a
from-scratch minimal ext4 reader/writer so app-produced disk images
are ordinary ext4, readable/writable with stock Linux tooling — at the
cost of a much more involved on-disk format: superblock and block
group descriptors, inode table records, extent trees mapping a file's
logical blocks to physical ones, and directory entries.

This is not a general-purpose ext4 driver; it implements exactly the
subset `posix_shim.c` needs: `open`/`read`/`write`/`close`/`lseek`/
`unlink`, plus path-based `stat()`, against a single flat root
directory, with real block/inode bitmap allocation so files can grow
past their initial size (unlike BMFS's fixed 2MiB-per-file
reservation). It requires filesystem images built with a constrained
feature set — see the file-level comment in `ext4.c` for the exact
`mkfs.ext4` invocation (4096-byte blocks so an ext4 block and a disk
sector are the same unit, 256-byte inodes, `64bit`/`metadata_csum`/
`meta_bg` disabled) — and caps extent trees at depth 1 (up to 4
in-inode extents, or up to 4 index blocks of ~340 extents each). See
OPENISSUES.md for the full list of what's deliberately not
implemented (subdirectories, htree creation, directory listing,
journal replay, checksums).

`stat()`'s path was carried over unchanged from the BMFS-era work:
x86_64 musl doesn't call `statx()` for a plain `stat()` — disassembling
the actual compiled call site showed it takes a `SYS_stat`/`SYS_lstat`
fast path (falling back to `SYS_newfstatat` only for non-default
dirfd/flags), which needed matching musl's internal `struct kstat`
layout, not the `statx` ABI initially assumed.

Also carried over: `O_APPEND` forces the write position to the
current file size on every `write()` call (rather than seeking to
end-of-file once, at `open()` time), so a subsequent `lseek()` on the
same fd can't make a `write()` silently overwrite instead of
appending.

## Networking (`net_glue.c`, `net_shim.c`, vendored `lwip-2.2.0/`)

lwIP's own socket layer (`sockets.c`) needs a real OS thread
(`tcpip.c`) pumping a mailbox — not available here (single-threaded,
no preemption). Instead: lwIP is configured `NO_SYS=1`
(`lwip_port/lwipopts.h`), driven directly by lwIP's raw
callback-based `tcp.h`/`udp.h` API, and `net_shim.c` wraps that in a
thin blocking BSD-socket-shaped layer of its own — every blocking call
(`connect`/`accept`/`send`/`recv`) runs its own synchronous loop
(drain the NIC via `b_net_rx`, service lwIP's timers, check if the
operation completed, repeat) with a 30s timeout rather than infinite
POSIX blocking, since there's no way to recover a truly stuck call in
a unikernel VM.

`net_glue.c` is the netif driver: raw Ethernet frames in/out via
`b_net_tx`/`b_net_rx` (interface id 0, the only NIC this kernel
supports), MAC address from `b_system(NET_STATUS)`, `sys_now()` from
`b_system(TIMECOUNTER)` (nanoseconds → ms), DHCP client for address
configuration.

Scope (agreed upfront): IPv4 TCP client + server, DHCP for addressing.
No UDP/raw sockets exposed to apps, no IPv6/DNS, no non-blocking mode.

## Verification

Every phase was checked by actually booting in Firecracker
(`./build.sh helloc.app` → `./baremetal.sh start` → real serial
output), not just compiling:
- musl startup + `printf` — clean "Hello, World!" with no faults.
- Heap — small (`brk`-backed) and 200KB (`mmap`-backed, crosses
  mallocng's threshold) allocations, both fully written/read back.
- Networking — DHCP-bound a real address from the host's `dnsmasq`;
  guest TCP client connected out to a host `nc -l` listener (message
  received on the host side); host `nc` connected in to the guest's
  TCP server and got back the expected echoed reply.

ext4 (`ext4.c`) replaced BMFS without a Firecracker boot available in
this checkout, so it was instead verified on the host: `ext4.c` was
compiled as-is against a small shim backing `b_nvs_read`/`b_nvs_write`
with `pread`/`pwrite` on a loop file, then exercised against a real
`mkfs.ext4`-produced image (built with the required feature flags —
see `ext4.c`'s file-level comment) pre-seeded with a file via
`debugfs`. Covered: reading a pre-existing file byte-for-byte;
create/write/close/reopen/read-back; `stat()`/`fstatat()`; a ~20MiB
sequential file; `O_APPEND` ignoring an intervening `lseek()`;
`O_TRUNC`; a sparse write (seek past EOF, write, confirm the gap
reads back as zero); `unlink()` followed by ENOENT and by
recreation; 500 files spanning multiple directory blocks; and,
by interleaving single-block writes across two files so each file's
own blocks land non-contiguously, forcing its extent tree through the
depth-0→depth-1 promotion and then a second leaf block. The resulting
image was independently checked with real `e2fsprogs` tooling —
`e2fsck -f` reported zero errors, and `debugfs`'s own extent-tree
parser and file dumps agreed byte-for-byte with what the driver wrote
(confirming the extent tree, not just the data, is really ext4-
compatible). A real Firecracker boot exercising this path (as the
BMFS work above was) is still open — see OPENISSUES.md.

## Files

New/vendored under `app/`: `posix_shim.{c,h}`, `ext4.{c,h}`,
`net_glue.{c,h}`, `net_shim.{c,h}`, `lwip_port/` (port config),
`musl-1.2.6/` and `lwip-2.2.0/` (vendored source, fetched via
`scripts/get-musl.sh` / `scripts/get-lwip.sh`). Modified:
`crt0.c`, `c.ld`, `build-app.sh`, `helloc.c` (kept as the minimal
"Hello, World!" example — every feature above was verified with
temporary test code in `helloc.c` that was reverted afterward, not
left as the permanent example).
