#ifndef __LINUX_SPINLOCK_TYPES_UP_H
#define __LINUX_SPINLOCK_TYPES_UP_H

#ifndef __LINUX_SPINLOCK_TYPES_RAW_H
# error "Please do not include this file directly."
#endif

/*
 * include/linux/spinlock_types_up.h - spinlock type definitions for UP
 *
 * portions Copyright 2005, Red Hat, Inc., Ingo Molnar
 * Released under the General Public License (GPL).
 */

#ifdef CONFIG_DEBUG_SPINLOCK
//arch_spinlock_t
typedef struct {
	volatile unsigned int slock;
	/*volatile :  컴파일러에게 이 변수의 값을 변경하는 코드가 컴파일러 자체 코드 외부에 있을 수 있음을 알림
	따라서 컴파일러는 변수를 레지스터에 캐시하거나(저장) 값을 건너뛰지 않고, 
	매번 메모리에서 직접 값을 읽어오고 쓰도록 강제
	*/
} arch_spinlock_t;

#define __ARCH_SPIN_LOCK_UNLOCKED { 1 }

#else

typedef struct { } arch_spinlock_t;

#define __ARCH_SPIN_LOCK_UNLOCKED { }

#endif

typedef struct {
	/* no debug version on UP */
} arch_rwlock_t;

#define __ARCH_RW_LOCK_UNLOCKED { }

#endif /* __LINUX_SPINLOCK_TYPES_UP_H */
