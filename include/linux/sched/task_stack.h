/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_SCHED_TASK_STACK_H
#define _LINUX_SCHED_TASK_STACK_H

/*
 * task->stack (kernel stack) handling interfaces:
 */

#include <linux/sched.h>
#include <linux/magic.h>
#include <linux/refcount.h>
#include <linux/kasan.h>

#ifdef CONFIG_THREAD_INFO_IN_TASK

/*
 * When accessing the stack of a non-current task that might exit, use
 * try_get_task_stack() instead.  task_stack_page will return a pointer
 * that could get freed out from under you.
 */
static __always_inline void *task_stack_page(const struct task_struct *task)
{
	return task->stack;
}

#define setup_thread_stack(new,old)	do { } while(0)

static __always_inline unsigned long *end_of_stack(const struct task_struct *task)
{
#ifdef CONFIG_STACK_GROWSUP
	return (unsigned long *)((unsigned long)task->stack + THREAD_SIZE) - 1;
#else
	return task->stack;
#endif
}

#else

#define task_stack_page(task)	((void *)(task)->stack)

static inline void setup_thread_stack(struct task_struct *p, struct task_struct *org)
{
	*task_thread_info(p) = *task_thread_info(org);
	task_thread_info(p)->task = p;
}

/*
 * Return the address of the last usable long on the stack.
 *
 * When the stack grows down, this is just above the thread
 * info struct. Going any lower will corrupt the threadinfo.
 *
 * When the stack grows up, this is the highest address.
 * Beyond that position, we corrupt data on the next page.
 */
static inline unsigned long *end_of_stack(const struct task_struct *p) // end_of_stack 여기
{ // TODO : inline, stack, ifdef?if?
	/*
	inline : 함수 호출이 아니라 코드 삽입으로 처리해도 되는 함수, 코드 조각처럼
	주소 없어도 되고, 호출 안해도 되고, 문법적으로만 함수
	이 코드는 독립된 함수로 존재할 필요가 없고, 호출이라는 개념 없이 현재 위칭[ 직접 삽입될 수 있다
	
	아키텍쳐별로 스택이 쌓이는 방향은 이미 결정되어있음

	#ifdef는 컴파일 전에 판단
	정의돼 있으면 이 줄을 소스 코드에 남김
	정의 안 돼 있으면 이 줄을 아예 지워버림
	if는 실행 중에 판단
	코드 항상 존재, 컴파일됨, 실행 중에 조건 검사, 분기(branch) 발생 -> CPU가 판단
	*/
#ifdef CONFIG_STACK_GROWSUP // 스택이 위로 쌓임 낮은 주소 -> 높은 주소 스택의 끝
	return (unsigned long *)((unsigned long)task_thread_info(p) + THREAD_SIZE) - 1;
#else // 스택이 아래로 쌓임 높은 주소 -> 낮은 주소 thread_info 전
	return (unsigned long *)(task_thread_info(p) + 1);
#endif
}

#endif

#ifdef CONFIG_THREAD_INFO_IN_TASK //CONFIG_THREAD_INFO_IN_TASK
static inline void *try_get_task_stack(struct task_struct *tsk)
{
	return refcount_inc_not_zero(&tsk->stack_refcount) ?
		task_stack_page(tsk) : NULL;
}

extern void put_task_stack(struct task_struct *tsk);
#else
static inline void *try_get_task_stack(struct task_struct *tsk)
{
	return task_stack_page(tsk);
}

static inline void put_task_stack(struct task_struct *tsk) {}
#endif

void exit_task_stack_account(struct task_struct *tsk);

#define task_stack_end_corrupted(task) \
		(*(end_of_stack(task)) != STACK_END_MAGIC)

static inline int object_is_on_stack(const void *obj)
{
	void *stack = task_stack_page(current);

	obj = kasan_reset_tag(obj);
	return (obj >= stack) && (obj < (stack + THREAD_SIZE));
}

extern void thread_stack_cache_init(void);

#ifdef CONFIG_DEBUG_STACK_USAGE
unsigned long stack_not_used(struct task_struct *p);
#else
static inline unsigned long stack_not_used(struct task_struct *p)
{
	return 0;
}
#endif
extern void set_task_stack_end_magic(struct task_struct *tsk);

static inline int kstack_end(void *addr)
{
	/* Reliable end of stack detection:
	 * Some APM bios versions misalign the stack
	 */
	return !(((unsigned long)addr+sizeof(void*)-1) & (THREAD_SIZE-sizeof(void*)));
}

#endif /* _LINUX_SCHED_TASK_STACK_H */
