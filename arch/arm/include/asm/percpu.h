/* SPDX-License-Identifier: GPL-2.0-only */
/*
 * Copyright 2012 Calxeda, Inc.
 */
#ifndef _ASM_ARM_PERCPU_H_
#define _ASM_ARM_PERCPU_H_

#include <asm/insn.h>

register unsigned long current_stack_pointer asm ("sp");

/*
 * Same as asm-generic/percpu.h, except that we store the per cpu offset
 * in the TPIDRPRW. TPIDRPRW only exists on V6K and V7
 */
#ifdef CONFIG_SMP
static inline void set_my_cpu_offset(unsigned long off) // set_my_cpu_offset
/*
현재 CPU의 per-CPU offset을 CPU 레지스터(TPIDRPRW)에 설정하는 함수
이후에 커널이 per-CPU 변수에 접근할 때 사용할 base offset을 CPU에 알려주는 역할
off : 현재 CPU가 사용할 per-CPU 메모리의 시작 offset
inline : 함수 호출 오버헤드를 줄이기 위해 컴파일 시 코드 위치에 바로 삽입
*/
{
	extern unsigned int smp_on_up; // SMP 커널이지만 실제 CPU는 1개인 환경, SMP 코드 필요 없음

	if (IS_ENABLED(CONFIG_CPU_V6) && !smp_on_up) // CPU가 ARMv6 이고 SMP 환경이 아니라면
		return;

	/* Set TPIDRPRW TPIDRPRW 레지스터에 off 값을 저장
	TPIDRPRW : Thread Process ID Register (Privileged Read Write), ARM CPU 레지스터
	volatile : 이 어셈블리는 반드시 실행되어야 하며 최적화로 제거하거나 재배치하면 안 된다
	mcr : Move to Coprocessor Register 명령어로 CPU 레지스터 값을 Coprocessor 레지스터에 기록
	p15 : ARM 시스템 제어 Coprocessor (CP15)로 MMU, cache, thread pointer 등 CPU 시스템 레지스터들이 존재하는 영역
	0 : opc1 필드로 CP15 레지스터 선택에 사용되는 opcode 값
	%0 : GCC inline assembly 입력 operand로 아래 "r"(off)로 전달된 값을 의미
	off 값이 들어있는 CPU general register를 사용
	c13 : CP15의 register group 13으로 thread ID 관련 레지스터 그룹
	c0 : 해당 register group 내부의 하위 레지스터 선택 필드
	4 : opc2 필드로 위 값들과 조합되어 실제 레지스터를 결정
	p15,0,c13,c0,4 조합은 ARM 시스템 레지스터 TPIDRPRW (Thread Process ID Register, Privileged Read/Write)를 의미
	이 명령은 off 값을 TPIDRPRW 레지스터에 기록
	Linux 커널에서는 이 레지스터에 현재 CPU의 per-CPU offset을 저장하여 per_cpu 변수 접근 시 현재 CPU의 per-CPU 영역을 빠르게 찾을 수 있도록 함
	: : "r"(off)
	GCC inline assembly 입력 operand 지정으로 off 값을 general register에 넣어 assembly 코드의 %0 위치에 전달
	: "memory" : 이 assembly가 메모리에 영향을 줄 수 있음을 컴파일러에 알리고 메모리 접근 최적화 재정렬을 방지
	*/
	asm volatile("mcr p15, 0, %0, c13, c0, 4" : : "r" (off) : "memory");
}

static __always_inline unsigned long __my_cpu_offset(void)
{
	unsigned long off;

	/*
	 * Read TPIDRPRW.
	 * We want to allow caching the value, so avoid using volatile and
	 * instead use a fake stack read to hazard against barrier().
	 */
	asm("0:	mrc p15, 0, %0, c13, c0, 4			\n\t"
#ifdef CONFIG_CPU_V6
	    "1:							\n\t"
	    "	.subsection 1					\n\t"
#if defined(CONFIG_ARM_HAS_GROUP_RELOCS) && \
    !(defined(MODULE) && defined(CONFIG_ARM_MODULE_PLTS))
	    "2: " LOAD_SYM_ARMV6(%0, __per_cpu_offset) "	\n\t"
	    "	b	1b					\n\t"
#else
	    "2: ldr	%0, 3f					\n\t"
	    "	ldr	%0, [%0]				\n\t"
	    "	b	1b					\n\t"
	    "3:	.long	__per_cpu_offset			\n\t"
#endif
	    "	.previous					\n\t"
	    "	.pushsection \".alt.smp.init\", \"a\"		\n\t"
	    "	.long	0b - .					\n\t"
	    "	b	. + (2b - 0b)				\n\t"
	    "	.popsection					\n\t"
#endif
	     : "=r" (off)
	     : "Q" (*(const unsigned long *)current_stack_pointer));

	return off;
}
#define __my_cpu_offset __my_cpu_offset()
#else
#define set_my_cpu_offset(x)	do {} while(0)

#endif /* CONFIG_SMP */

#include <asm-generic/percpu.h>

#endif /* _ASM_ARM_PERCPU_H_ */
