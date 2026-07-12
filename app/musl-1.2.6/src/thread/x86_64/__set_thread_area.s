/*
 * BareMetal-Firecracker port.
 *
 * Upstream sets the thread pointer via arch_prctl(ARCH_SET_FS, ptr),
 * a real syscall on Linux. BareMetal apps run in ring 0 in the same
 * flat address space as the kernel, so the privileged instruction
 * that arch_prctl would have used on the kernel's behalf -- wrmsr on
 * IA32_FS_BASE -- can just be issued directly, with no trap needed.
 *
 * IA32_FS_BASE = 0xC0000100. wrmsr takes the MSR index in ecx and
 * the 64-bit value split as edx:eax (high:low).
 */
.text
.global __set_thread_area
.hidden __set_thread_area
.type __set_thread_area,@function
__set_thread_area:
	mov %rdi,%rax
	mov %rdi,%rdx
	shr $32,%rdx
	mov $0xC0000100,%ecx
	wrmsr
	xor %eax,%eax
	ret
