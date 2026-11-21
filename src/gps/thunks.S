/* thunks.S — entradas chamadas pelo código do gps.dll (ARMv4, convenção do
 * Windows CE) e adaptadores das funções de ponto flutuante do CE.
 * SPDX-License-Identifier: GPL-3.0-or-later */
	.syntax unified
	.arm
	.text

/*
 * Cada import tem um thunk gerado em tempo de execução (gpsce.c):
 *	ldr r12, [pc]	@ índice
 *	ldr pc, [pc]	@ bf_thunk_common
 * Aqui os argumentos viram um vetor de palavras, como o CE os passa:
 * a[0..3] = r0..r3, a[4..] = os que o chamador deixou na pilha. Assim um
 * printf do CE (doubles sem alinhamento de 8 bytes) é lido palavra a palavra.
 * A pilha do CE vem alinhada em 4: alinha em 8 antes de entrar no C.
 * Retorno em r0:r1 (uint64_t).
 */
	.global	bf_thunk_common
	.type	bf_thunk_common, %function
bf_thunk_common:
	push	{r0-r3}
	mov	r1, sp
	push	{r4, lr}
	mov	r4, sp
	bic	sp, sp, #7
	mov	r0, r12
	bl	bf_dispatch
	mov	sp, r4
	pop	{r4, lr}
	add	sp, sp, #16
	bx	lr
	.size	bf_thunk_common, . - bf_thunk_common

/* __negd / __negs: troca o sinal (double em r0:r1, palavra alta em r1) */
	.global	ce_negd
	.type	ce_negd, %function
ce_negd:
	eor	r1, r1, #0x80000000
	bx	lr
	.global	ce_negs
	.type	ce_negs, %function
ce_negs:
	eor	r0, r0, #0x80000000
	bx	lr

/* __ned / __nes: "diferente" (verdadeiro também com NaN) */
	.global	ce_ned
	.type	ce_ned, %function
ce_ned:
	push	{r4, lr}
	bl	__aeabi_dcmpeq
	eor	r0, r0, #1
	pop	{r4, pc}
	.global	ce_nes
	.type	ce_nes, %function
ce_nes:
	push	{r4, lr}
	bl	__aeabi_fcmpeq
	eor	r0, r0, #1
	pop	{r4, pc}

/* __rt_sdiv / __rt_udiv do CE: divisor em r0, dividendo em r1;
 * quociente em r0, resto em r1 (o __aeabi_*divmod recebe na outra ordem) */
	.global	ce_rt_sdiv
	.type	ce_rt_sdiv, %function
ce_rt_sdiv:
	mov	r2, r0
	mov	r0, r1
	mov	r1, r2
	b	__aeabi_idivmod
	.global	ce_rt_udiv
	.type	ce_rt_udiv, %function
ce_rt_udiv:
	mov	r2, r0
	mov	r0, r1
	mov	r1, r2
	b	__aeabi_uidivmod

/* chama uma função do DLL com até 6 argumentos (ponteiro, a0..a5) */
	.global	ce_call6
	.type	ce_call6, %function
ce_call6:
	push	{r4-r6, lr}
	mov	r12, r0
	mov	r0, r1
	mov	r1, r2
	mov	r2, r3
	ldr	r3, [sp, #16]		@ a3
	ldr	r4, [sp, #20]		@ a4
	ldr	r5, [sp, #24]		@ a5
	sub	sp, sp, #8
	str	r4, [sp]
	str	r5, [sp, #4]
	mov	lr, pc
	mov	pc, r12			@ o código do CE volta com "mov pc, lr"
	add	sp, sp, #8
	pop	{r4-r6, pc}
	.size	ce_call6, . - ce_call6

	.section .note.GNU-stack, "", %progbits
