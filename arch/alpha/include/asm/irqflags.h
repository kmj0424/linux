/* SPDX-License-Identifier: GPL-2.0 */
#ifndef __ALPHA_IRQFLAGS_H
#define __ALPHA_IRQFLAGS_H

#include <asm/pal.h>

#define IPL_MIN		0
#define IPL_SW0		1
#define IPL_SW1		2
#define IPL_DEV0	3
#define IPL_DEV1	4
#define IPL_TIMER	5
#define IPL_PERF	6
#define IPL_POWERFAIL	6
#define IPL_MCHECK	7
#define IPL_MAX		7

#ifdef CONFIG_ALPHA_BROKEN_IRQ_MASK
#undef IPL_MIN
#define IPL_MIN		__min_ipl
extern int __min_ipl;
#endif

#define getipl()		(rdps() & 7)
#define setipl(ipl)		((void) swpipl(ipl))

static inline unsigned long arch_local_save_flags(void)
{
	return rdps();
}

static inline void arch_local_irq_disable(void)
{
	setipl(IPL_MAX);
	barrier();
}

static inline unsigned long arch_local_irq_save(void)
{
	unsigned long flags = swpipl(IPL_MAX);
	barrier();
	return flags;
}

static inline void arch_local_irq_enable(void)
{
	barrier();
	setipl(IPL_MIN);
}

static inline void arch_local_irq_restore(unsigned long flags)
{
	barrier(); // 컴파일러 최적화 방지용 메모리 장벽, 이 줄 앞뒤 코드 순서 바꾸지 마라, IRQ 상태를 바꾸기 전에 이전 코드들이 인터럽트 상태 변경보다 뒤로 밀리지 않게
	/* CPU의 인터럽트 우선순위 레벨(IPL)을 flags 값으로 설정
	인터럽트 허용 상태 → 다시 허용, 인터럽트 차단 상태 → 계속 차단
	아키텍처마다 다르지만 보통은 상태 레지스터, 인터럽트 마스크 레지스터, PSR / SR / CPSR 같은 레지스터 중 하나를 직접 건드림. */
	setipl(flags);
	barrier();
}

static inline bool arch_irqs_disabled_flags(unsigned long flags)
{
	return flags == IPL_MAX;
}

static inline bool arch_irqs_disabled(void)
{
	return arch_irqs_disabled_flags(getipl());
}

#endif /* __ALPHA_IRQFLAGS_H */
