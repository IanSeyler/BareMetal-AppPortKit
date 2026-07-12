/*
 * BareMetal-Firecracker port.
 *
 * Upstream: cancellation-point syscall path, ending in a raw
 * `syscall` instruction. BareMetal has no syscall trap (see
 * arch/x86_64/syscall_arch.h), so after the usual cancellation
 * check this instead calls __bmos_syscall(), the same C dispatcher
 * used by the non-cancellable path, with syscall arguments shuffled
 * from the incoming SysV register/stack layout into a normal SysV
 * call to a 7-argument C function.
 */
.text
.global __cp_begin
.hidden __cp_begin
.global __cp_end
.hidden __cp_end
.global __cp_cancel
.hidden __cp_cancel
.hidden __cancel
.global __syscall_cp_asm
.hidden __syscall_cp_asm
.type   __syscall_cp_asm,@function
__syscall_cp_asm:
	/* rdi=&self->cancel, rsi=nr, rdx=a1, rcx=a2, r8=a3, r9=a4,
	 * 8(%rsp)=a5, 16(%rsp)=a6 */
__cp_begin:
	mov (%rdi),%eax
	test %eax,%eax
	jnz __cp_cancel
	mov 16(%rsp),%rax	/* a6 */
	mov 8(%rsp),%r11	/* a5 */
	mov %rsi,%rdi		/* nr */
	mov %rdx,%rsi		/* a1 */
	mov %rcx,%rdx		/* a2 */
	mov %r8,%rcx		/* a3 */
	mov %r9,%r8		/* a4 */
	mov %r11,%r9		/* a5 */
	push %rax		/* a6, as __bmos_syscall's 7th (stack) arg */
	call __bmos_syscall
__cp_end:
	add $8,%rsp
	ret
__cp_cancel:
	jmp __cancel
