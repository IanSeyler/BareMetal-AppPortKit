# Open Issues

Known gaps and limitations in the musl libc port (`app/`), ext4 file
I/O, and lwIP-based networking. Most of these are deliberate scope
cuts made while getting each phase working end to end, not bugs —
they're listed here so they're easy to find again when someone hits
one.

## Process model

- **No `fork`/`vfork`/`clone`/`execve`/`wait4`.** Not wired into
  `posix_shim.c`'s dispatcher at all (falls through to the default
  `-ENOSYS` case). There is exactly one process, ever — matches
  BareMetal's model (an app is `call`ed directly by the kernel/monitor,
  there's no process table), but any program that tries to spawn a
  child (a shell, a `system()` call, a forking server) will fail.
- **No real command-line args or environment.** `crt0.c` fabricates a
  minimal fake initial stack for musl's startup path with `argc=0` and
  an empty `envp`. Programs that read `argv`/`getenv()` will always see
  nothing, regardless of how the app was invoked.
- **`getpid`/`getppid`/`kill`/`uname`/`sysinfo`/`times` are unimplemented.**
  Anything that queries "what process am I" or sends itself a signal
  will get `-ENOSYS`.
- **No signal delivery.** `rt_sigaction`, `rt_sigprocmask`, and
  `sigaltstack` are accepted and silently ignored (`posix_shim.c`) —
  handlers can be registered but will never actually fire, since
  BareMetal has no mechanism to deliver a signal into running app code.
- **Single-threaded only.** `pthread_create` isn't wired up (would need
  a `clone` implementation); `__set_thread_area`/TLS bootstrap works
  and reports `can_do_threads=1` to musl since the FS-base wrmsr
  genuinely succeeds, but there is nowhere to run a second thread.

## Missing common syscalls

Not implemented (all fall through to `-ENOSYS`):
- `nanosleep`/`clock_nanosleep` — no way to block for a relative
  duration; `sleep()`/`usleep()` will fail or misbehave.
- `clock_gettime`/`gettimeofday`/`time()` only support
  `CLOCK_REALTIME`/`CLOCK_REALTIME_COARSE`, backed by
  `b_system(WALLCLOCK)` (seconds since the Unix epoch, recorded at
  boot; `tv_nsec` is always 0 -- no sub-second component is wired up).
  `CLOCK_MONOTONIC` and other clock ids return `-EINVAL`; they would
  need `b_system(TIMECOUNTER)` (nanoseconds since boot, already used
  internally by the heap/TLS/lwIP code) wired up as a separate case.
- `getrandom` — nothing backs `/dev/urandom`-equivalent randomness for
  application code (musl's own internal entropy needs, e.g. the stack
  canary and mallocng's hardening secret, are seeded via `crt0.c`'s
  `fill_random()` using `rdrand`/`rdtsc`, but that path isn't exposed
  as a syscall).
- `poll`/`select`/`epoll_*` — no way to multiplex across multiple fds
  (stdin, an ext4 file, a socket) in one blocking call. Combined with
  every blocking socket/file op in this port already being a
  synchronous spin-loop internally, this means a program can't
  currently wait on "whichever of these fds is ready first."
- `pipe`/`pipe2`/`dup`/`dup2`/`dup3` — no in-process fd duplication or
  pipes between fds.
- `getcwd`/`chdir`/`mkdir`/`rmdir`/`chmod`/`chown`/`umask` — no
  concept of a working directory or permissions above the on-disk mode
  bits (`ext4.c` only ever looks up paths in the root directory).

## Heap (`posix_shim.c`)

- **`mmap()` never really unmaps.** `munmap()` is a documented no-op —
  it returns success but the memory is never reclaimed. A program that
  mmaps and unmaps in a loop will exhaust the heap arena.
- **No growth beyond the initial `b_system(FREE_MEMORY)` ceiling.** The
  heap size is fixed once, at first use; there's no mechanism to claim
  more RAM even if more becomes available (e.g. if BareMetal's own
  memory management changes).
- **Large (≥128KB) `malloc()`s share the same bump arena as `brk()`.**
  Works, but means a single big allocation can exhaust room that
  smaller `brk()`-backed allocations would otherwise have used, with
  no way to trade space back.

## ext4 file I/O (`ext4.c`)

This is a from-scratch, minimal ext4 implementation, not a general-
purpose one — it supports exactly the read/write/create/grow/delete
operations `posix_shim.c` needs against a single flat directory, and
requires filesystem images built with a constrained feature set (see
the file-level comment in `ext4.c` for the exact `mkfs.ext4` flags:
4096-byte blocks, 256-byte inodes, `64bit`/`metadata_csum`/`meta_bg`
disabled). Verified against real `e2fsprogs` tooling (`e2fsck -f`,
`debugfs`) — see SUMMARY.md — but still a much smaller surface than a
real ext4 driver:

- **Flat namespace only — no subdirectories.** Paths are looked up
  directly in the root directory (inode 2); anything with an embedded
  `/` is rejected with `-ENOENT`. ext4 itself supports real
  subdirectories, but nothing above this layer needs them yet.
- **Directory entries are only ever scanned/inserted linearly.**
  htree-indexed directories are still read correctly (see the comment
  in `ext4.c` on why a linear scan is safe there), but this driver
  never builds an htree itself, and clears a directory's htree flag
  the first time it inserts into it — a directory this driver
  modifies stays linear-only from then on.
- **Extent trees are only ever grown to depth 1** — up to 4 in-inode
  extents, or up to 4 index entries each pointing to one 4096-byte
  leaf block of up to ~340 extents. No depth-2+ trees. A hard cap,
  though a generous one for this environment's expected file sizes.
- **Deleted directory slots (tombstones) are reused whole, never
  split**, even when much larger than the new entry being inserted —
  minor directory-space waste, not a correctness issue.
- **No journal replay, no checksums.** Every operation is expected to
  complete without a crash partway through, same limitation BMFS had
  before it. `metadata_csum`/`uninit_bg` must be disabled at
  `mkfs.ext4` time — this driver doesn't compute or verify them.
- **No `access()`/`chmod()`/permission bits beyond mode bits already
  on disk.** `ext4_fstat_fd()` always reports `0644` regardless.
- **No directory listing.** `opendir`/`readdir`-equivalent enumeration
  of what's on disk isn't implemented — only look-up-by-name.
- **Small fixed open-file table** (`EXT4_MAX_OPEN` = 8 concurrent files
  across the whole process) and **fixed max image size** (128 block
  groups × 128MiB = 16GiB, from `EXT4_MAX_GROUPS`).

## Networking (`net_glue.c`, `net_shim.c`)

- **TCP only.** No UDP or raw IP sockets exposed to application code
  (`socket(AF_INET, SOCK_DGRAM, ...)` returns `-EAFNOSUPPORT`), even
  though UDP is compiled into lwIP and used internally by the DHCP
  client.
- **No IPv6, no DNS resolution** (`LWIP_IPV6=0`, `LWIP_DNS=0` in
  `lwip_port/lwipopts.h`). Programs must connect to a literal IPv4
  address; `getaddrinfo()`/`gethostbyname()` have nothing to resolve
  against.
- **All blocking socket calls (`connect`/`accept`/`send`/`recv`) have
  a hard-coded 30s timeout**, not indefinite POSIX blocking. Deliberate
  — there's no way to interrupt or recover a truly stuck call in a
  single-threaded unikernel VM — but it means a legitimately
  slow/idle connection (e.g. a server waiting hours for the next
  client) will be torn down.
- **No non-blocking mode.** `SOCK_NONBLOCK`/`O_NONBLOCK` and
  `MSG_DONTWAIT` are accepted but not honored — every socket call
  blocks (up to the timeout above) regardless.
- **`setsockopt`/`getsockopt` are accept-and-ignore stubs.** Nothing
  like `SO_REUSEADDR`, `SO_RCVTIMEO`, or `TCP_NODELAY` actually takes
  effect.
- **Unaccepted connections are leaked on listener `close()`.** If a
  listening socket is closed while connections are sitting in its
  accept queue (arrived but not yet `accept()`ed), those `tcp_pcb`s
  are never explicitly closed.
- **Single NIC, hard-coded interface id 0** — matches the current
  kernel (`init_net` only brings up one virtio-net device), so this
  isn't a shim limitation so much as a note that multi-NIC support
  would need kernel-side work first.
- **Fixed socket table** (`SOCK_MAX` = 16 concurrent sockets).

## General

- **No dynamic linking, by design** — everything is statically linked
  into one flat binary at a fixed load address. Not a gap so much as
  a permanent constraint of this environment (no ELF loader, no
  syscall trap to service `mmap`-based `dlopen`).
- **musl and lwIP are vendored at different freshness policies.**
  `scripts/get-musl.sh` always fetches latest; `scripts/get-lwip.sh` is
  pinned to 2.2.0 because the port glue (`lwip_port/*`, `net_glue.c`,
  `net_shim.c`) was written against that release's exact file layout
  and config surface. Bumping the musl version is low-risk; bumping
  lwIP needs re-checking the port glue against whatever changed.
