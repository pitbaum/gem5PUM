	.file	"pattern.c"
	.text
	.section	.rodata.str1.1,"aMS",@progbits,1
.LC0:
	.string	"Address of a: %p\n"
.LC1:
	.string	"Value of A: %d \n"
	.section	.text.startup,"ax",@progbits
	.p2align 4
	.globl	main
	.type	main, @function
main:
.LFB23:
	.cfi_startproc
	endbr64
	pushq	%r12
	.cfi_def_cfa_offset 16
	.cfi_offset 12, -16
	movl	$2, %edi
	pushq	%rbp
	.cfi_def_cfa_offset 24
	.cfi_offset 6, -24
	leaq	.LC0(%rip), %rbp
	pushq	%rbx
	.cfi_def_cfa_offset 32
	.cfi_offset 3, -32
	movq	%rbp, %rsi
	leaq	.LC1(%rip), %rbx
	subq	$16, %rsp
	.cfi_def_cfa_offset 48
	movq	%fs:40, %rax
	movq	%rax, 8(%rsp)
	xorl	%eax, %eax
	leaq	4(%rsp), %r12
	movl	$2, 4(%rsp)
	movq	%r12, %rdx
	call	__printf_chk@PLT
	movl	4(%rsp), %edx
	movq	%rbx, %rsi
	xorl	%eax, %eax
	movl	$2, %edi
	call	__printf_chk@PLT
	movl	$15, %edx
#APP
# 12 "pattern.c" 1
	mov %rdx, %rax
	.byte 0x15, 0x28

# 0 "" 2
#NO_APP
	movl	4(%rsp), %eax
	movq	%rbx, %rsi
	movl	$2, %edi
	leal	1(%rax), %edx
	xorl	%eax, %eax
	movl	%edx, 4(%rsp)
	call	__printf_chk@PLT
	movl	4(%rsp), %edx
	movq	%rbx, %rsi
	xorl	%eax, %eax
	movl	$2, %edi
	call	__printf_chk@PLT
	xorl	%eax, %eax
	movq	%r12, %rdx
	movq	%rbp, %rsi
	movl	$2, %edi
	call	__printf_chk@PLT
	movq	8(%rsp), %rax
	subq	%fs:40, %rax
	jne	.L5
	addq	$16, %rsp
	.cfi_remember_state
	.cfi_def_cfa_offset 32
	xorl	%eax, %eax
	popq	%rbx
	.cfi_def_cfa_offset 24
	popq	%rbp
	.cfi_def_cfa_offset 16
	popq	%r12
	.cfi_def_cfa_offset 8
	ret
.L5:
	.cfi_restore_state
	call	__stack_chk_fail@PLT
	.cfi_endproc
.LFE23:
	.size	main, .-main
	.ident	"GCC: (Ubuntu 13.3.0-6ubuntu2~24.04) 13.3.0"
	.section	.note.GNU-stack,"",@progbits
	.section	.note.gnu.property,"a"
	.align 8
	.long	1f - 0f
	.long	4f - 1f
	.long	5
0:
	.string	"GNU"
1:
	.align 8
	.long	0xc0000002
	.long	3f - 2f
2:
	.long	0x3
3:
	.align 8
4:
