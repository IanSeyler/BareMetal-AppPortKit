/*
 * BareMetal-Firecracker port.
 *
 * Upstream musl issues Linux syscalls with the x86-64 `syscall`
 * instruction. BareMetal has no syscall trap: apps run in ring 0,
 * in the same flat address space as the kernel, and call kernel
 * services as ordinary functions (see app/libBareMetal.c). So instead
 * of trapping, every syscall number/argument tuple musl would have
 * passed to the `syscall` instruction is instead handed to
 * __bmos_syscall(), a plain C function implemented in
 * app/posix_shim.c that translates it into BareMetal API calls.
 *
 * Syscall numbers (the `n` argument) are still the standard Linux
 * x86-64 __NR_* values from bits/syscall.h -- they are only used as
 * dispatch keys by the shim, never executed as real syscalls.
 */

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

long __bmos_syscall(long n, long a1, long a2, long a3, long a4, long a5, long a6);

static __inline long __syscall0(long n)
{
	return __bmos_syscall(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __syscall1(long n, long a1)
{
	return __bmos_syscall(n, a1, 0, 0, 0, 0, 0);
}

static __inline long __syscall2(long n, long a1, long a2)
{
	return __bmos_syscall(n, a1, a2, 0, 0, 0, 0);
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
	return __bmos_syscall(n, a1, a2, a3, 0, 0, 0);
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
	return __bmos_syscall(n, a1, a2, a3, a4, 0, 0);
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4, long a5)
{
	return __bmos_syscall(n, a1, a2, a3, a4, a5, 0);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4, long a5, long a6)
{
	return __bmos_syscall(n, a1, a2, a3, a4, a5, a6);
}

/* No vDSO page exists on BareMetal; leave VDSO_* undefined so musl
 * always falls through to the syscall path above. */

#define IPC_64 0
