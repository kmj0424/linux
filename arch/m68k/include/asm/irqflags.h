/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _M68K_IRQFLAGS_H
#define _M68K_IRQFLAGS_H

#include <linux/types.h>
#include <linux/preempt.h>
#include <asm/thread_info.h>
#include <asm/entry.h>

static inline unsigned long arch_local_save_flags(void)
{
	unsigned long flags;
	asm volatile ("movew %%sr,%0" : "=d" (flags) : : "memory");
	return flags;
}

static inline void arch_local_irq_disable(void)
{
#ifdef CONFIG_COLDFIRE
	asm volatile (
		"move	%/sr,%%d0	\n\t"
		"ori.l	#0x0700,%%d0	\n\t"
		"move	%%d0,%/sr	\n"
		: /* no outputs */
		:
		: "cc", "%d0", "memory");
#else
	asm volatile ("oriw  #0x0700,%%sr" : : : "memory");
#endif
}

static inline void arch_local_irq_enable(void)
{
#if defined(CONFIG_COLDFIRE)
	asm volatile (
		"move	%/sr,%%d0	\n\t"
		"andi.l	#0xf8ff,%%d0	\n\t"
		"move	%%d0,%/sr	\n"
		: /* no outputs */
		:
		: "cc", "%d0", "memory");
#else
# if defined(CONFIG_MMU)
	if (MACH_IS_Q40 || !hardirq_count())
# endif
		asm volatile (
			"andiw %0,%%sr"
			:
			: "i" (ALLOWINT)
			: "memory");
#endif
}

static inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags = arch_local_save_flags();
	arch_local_irq_disable();
	return flags;
}

static inline void arch_local_irq_restore(unsigned long flags) // arch_local_irq_restore
{
	/*
	static inline : 호출되지 않고 호출 위치에 코드가 직접 삽입
	flags : 이전에 저장해 둔 CPU 상태 값
	보통 arch_local_irq_save()에서 얻은 값
	flags에 저장돼 있던 CPU 상태 값을 현재 CPU의 상태 레지스터(SR)에 그대로 복원

	movew : word(16비트) 이동 명령
	%0 : 첫 번째 입력 오퍼랜드 (flags)
	%%sr : CPU 상태 레지스터(Status Register)
	flags 값을 SR 레지스터에 직접 쓰기

	"d" : 데이터 레지스터에 넣어서 전달
	(flags) : C 변수 flags
	flags 값을 데이터 레지스터에 넣어서 사용해라
	
	volatile : 이 명령을 절대 제거하거나 이동하지 마라
	CPU 상태를 바꾸는 명령이므로 최적화 금지
	
	"memory" : 이 명령은 메모리 상태에 영향을 줄 수 있다
	이 명령 앞뒤로 메모리 접근 재배치 금지

	
	*/
	asm volatile ("movew %0,%%sr" : : "d" (flags) : "memory");
}

static inline bool arch_irqs_disabled_flags(unsigned long flags)
{
	if (MACH_IS_ATARI) {
		/* Ignore HSYNC = ipl 2 on Atari */
		return (flags & ~(ALLOWINT | 0x200)) != 0;
	}
	return (flags & ~ALLOWINT) != 0;
}

static inline bool arch_irqs_disabled(void)
{
	return arch_irqs_disabled_flags(arch_local_save_flags());
}

#endif /* _M68K_IRQFLAGS_H */
