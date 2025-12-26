// SPDX-License-Identifier: GPL-2.0-only
/*
 * lib/debug_locks.c
 *
 * Generic place for common debugging facilities for various locks:
 * spinlocks, rwlocks, mutexes and rwsems.
 *
 * Started by Ingo Molnar:
 *
 *  Copyright (C) 2006 Red Hat, Inc., Ingo Molnar <mingo@redhat.com>
 */
#include <linux/rwsem.h>
#include <linux/mutex.h>
#include <linux/export.h>
#include <linux/spinlock.h>
#include <linux/debug_locks.h>

/*
 * We want to turn all lock-debugging facilities on/off at once,
 * via a global flag. The reason is that once a single bug has been
 * detected and reported, there might be cascade of followup bugs
 * that would just muddy the log. So we report the first one and
 * shut up after that.
 */
int debug_locks __read_mostly = 1;
EXPORT_SYMBOL_GPL(debug_locks);

/*
 * The locking-testsuite uses <debug_locks_silent> to get a
 * 'silent failure': nothing is printed to the console when
 * a locking bug is detected.
 */
int debug_locks_silent __read_mostly;
/*
__read_mostly : 대부분 읽기만 한다
성능 최적화용 메모리 배치
락 디버깅을 끌 때, 메시지를 출력할지 말지
0 → 경고/로그를 출력
1 → 조용히(silent) 끔
자동으로 lockdep가 꺼지는 상황(치명적 오류 후)에서는 로그 폭주를 막기 위해 silent=1로 둠
*/
EXPORT_SYMBOL_GPL(debug_locks_silent); // GPL 모듈에서 이 변수를 참조 가능

/*
 * Generic 'turn off all lock debugging' function:
 */
/*
debug_locks : lockdep 전체가 현재 활성화되어 있는지 나타내는 전역 플래그
__debug_locks_off : lockdep/락 디버깅을 실제로 불가역적으로 끄는 내부 함수
1 → 이번 호출로 처음 꺼졌다
0 → 이미 꺼져 있었음
*/
int debug_locks_off(void)
{
	if (debug_locks && __debug_locks_off()) { // 켜져 있었고, 이번에 실제로 꺼졌다면
		if (!debug_locks_silent) {
			console_verbose();
			/*
			lockdep가 꺼지는 상황은 보통 데드락 감지, 심각한 락 오용, 시스템 일관성 붕괴 직전
			이때 메세지 묻히지 않기 위해 커널 콘솔 로그 레벨을 최대(verbose) 로 올림
			*/
			return 1; // 이번 호출로 lock debugging이 꺼졌고, 메시지 출력도 허용됨
		}
	}
	return 0; // 이미 꺼져 있었거나 / silent 모드라 조용히 처리됨
}
EXPORT_SYMBOL_GPL(debug_locks_off);
