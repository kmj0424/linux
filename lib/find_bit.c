// SPDX-License-Identifier: GPL-2.0-or-later
/* bit search implementation
 *
 * Copyright (C) 2004 Red Hat, Inc. All Rights Reserved.
 * Written by David Howells (dhowells@redhat.com)
 *
 * Copyright (C) 2008 IBM Corporation
 * 'find_last_bit' is written by Rusty Russell <rusty@rustcorp.com.au>
 * (Inspired by David Howell's find_next_bit implementation)
 *
 * Rewritten by Yury Norov <yury.norov@gmail.com> to decrease
 * size and improve performance, 2015.
 */

#include <linux/bitops.h>
#include <linux/bitmap.h>
#include <linux/export.h>
#include <linux/math.h>
#include <linux/minmax.h>
#include <linux/swab.h>
#include <linux/random.h>

/*
 * Common helper for find_bit() function family
 * @FETCH: The expression that fetches and pre-processes each word of bitmap(s)
 * @MUNGE: The expression that post-processes a word containing found bit (may be empty)
 * @size: The bitmap size in bits
 */
#define FIND_FIRST_BIT(FETCH, MUNGE, size)					\
({										\
	unsigned long idx, val, sz = (size);					\
										\
	for (idx = 0; idx * BITS_PER_LONG < sz; idx++) {			\
		val = (FETCH);							\
		if (val) {							\
			sz = min(idx * BITS_PER_LONG + __ffs(MUNGE(val)), sz);	\
			break;							\
		}								\
	}									\
										\
	sz;									\
})

/*
 * Common helper for find_next_bit() function family
 * @FETCH: The expression that fetches and pre-processes each word of bitmap(s)
 * @MUNGE: The expression that post-processes a word containing found bit (may be empty)
 * @size: The bitmap size in bits
 * @start: The bitnumber to start searching at
 */
#define FIND_NEXT_BIT(FETCH, MUNGE, size, start)				\
({										\
	unsigned long mask, idx, tmp, sz = (size), __start = (start);		\
										\
	if (unlikely(__start >= sz))						\
		goto out;							\
										\
	mask = MUNGE(BITMAP_FIRST_WORD_MASK(__start));				\
	idx = __start / BITS_PER_LONG;						\
										\
	for (tmp = (FETCH) & mask; !tmp; tmp = (FETCH)) {			\
		if ((idx + 1) * BITS_PER_LONG >= sz)				\
			goto out;						\
		idx++;								\
	}									\
										\
	sz = min(idx * BITS_PER_LONG + __ffs(MUNGE(tmp)), sz);			\
out:										\
	sz;									\
})

#define FIND_NTH_BIT(FETCH, size, num)						\
({										\
	unsigned long sz = (size), nr = (num), idx, w, tmp;			\
										\
	for (idx = 0; (idx + 1) * BITS_PER_LONG <= sz; idx++) {			\
		if (idx * BITS_PER_LONG + nr >= sz)				\
			goto out;						\
										\
		tmp = (FETCH);							\
		w = hweight_long(tmp);						\
		if (w > nr)							\
			goto found;						\
										\
		nr -= w;							\
	}									\
										\
	if (sz % BITS_PER_LONG)							\
		tmp = (FETCH) & BITMAP_LAST_WORD_MASK(sz);			\
found:										\
	sz = idx * BITS_PER_LONG + fns(tmp, nr);				\
out:										\
	sz;									\
})

#ifndef find_first_bit
/*
 * Find the first set bit in a memory region.
 */
unsigned long _find_first_bit(const unsigned long *addr, unsigned long size)
{
	return FIND_FIRST_BIT(addr[idx], /* nop */, size);
}
EXPORT_SYMBOL(_find_first_bit);
#endif

#ifndef find_first_and_bit
/*
 * Find the first set bit in two memory regions.
 */
unsigned long _find_first_and_bit(const unsigned long *addr1,
				  const unsigned long *addr2,
				  unsigned long size)
{
	return FIND_FIRST_BIT(addr1[idx] & addr2[idx], /* nop */, size);
}
EXPORT_SYMBOL(_find_first_and_bit);
#endif

/*
 * Find the first bit set in 1st memory region and unset in 2nd.
 */
unsigned long _find_first_andnot_bit(const unsigned long *addr1,
				  const unsigned long *addr2,
				  unsigned long size)
{
	return FIND_FIRST_BIT(addr1[idx] & ~addr2[idx], /* nop */, size);
}
EXPORT_SYMBOL(_find_first_andnot_bit);

/*
 * Find the first set bit in three memory regions.
 */
unsigned long _find_first_and_and_bit(const unsigned long *addr1,
				      const unsigned long *addr2,
				      const unsigned long *addr3,
				      unsigned long size)
{
	return FIND_FIRST_BIT(addr1[idx] & addr2[idx] & addr3[idx], /* nop */, size);
}
EXPORT_SYMBOL(_find_first_and_and_bit);

#ifndef find_first_zero_bit
/*
 * Find the first cleared bit in a memory region.
 */
unsigned long _find_first_zero_bit(const unsigned long *addr, unsigned long size)
{
	return FIND_FIRST_BIT(~addr[idx], /* nop */, size);
}
EXPORT_SYMBOL(_find_first_zero_bit);
#endif

#ifndef find_next_bit
unsigned long _find_next_bit(const unsigned long *addr, unsigned long nbits, unsigned long start)
{
	return FIND_NEXT_BIT(addr[idx], /* nop */, nbits, start);
}
EXPORT_SYMBOL(_find_next_bit);
#endif

unsigned long __find_nth_bit(const unsigned long *addr, unsigned long size, unsigned long n)
{
	return FIND_NTH_BIT(addr[idx], size, n);
}
EXPORT_SYMBOL(__find_nth_bit);

unsigned long __find_nth_and_bit(const unsigned long *addr1, const unsigned long *addr2,
				 unsigned long size, unsigned long n)
{
	return FIND_NTH_BIT(addr1[idx] & addr2[idx], size, n);
}
EXPORT_SYMBOL(__find_nth_and_bit);

unsigned long __find_nth_andnot_bit(const unsigned long *addr1, const unsigned long *addr2,
				 unsigned long size, unsigned long n)
{
	return FIND_NTH_BIT(addr1[idx] & ~addr2[idx], size, n);
}
EXPORT_SYMBOL(__find_nth_andnot_bit);

unsigned long __find_nth_and_andnot_bit(const unsigned long *addr1,
					const unsigned long *addr2,
					const unsigned long *addr3,
					unsigned long size, unsigned long n)
{
	return FIND_NTH_BIT(addr1[idx] & addr2[idx] & ~addr3[idx], size, n);
}
EXPORT_SYMBOL(__find_nth_and_andnot_bit);

#ifndef find_next_and_bit
unsigned long _find_next_and_bit(const unsigned long *addr1, const unsigned long *addr2,
					unsigned long nbits, unsigned long start)
{
	return FIND_NEXT_BIT(addr1[idx] & addr2[idx], /* nop */, nbits, start);
}
EXPORT_SYMBOL(_find_next_and_bit);
#endif

#ifndef find_next_andnot_bit
unsigned long _find_next_andnot_bit(const unsigned long *addr1, const unsigned long *addr2,
					unsigned long nbits, unsigned long start)
{
	return FIND_NEXT_BIT(addr1[idx] & ~addr2[idx], /* nop */, nbits, start);
}
EXPORT_SYMBOL(_find_next_andnot_bit);
#endif

#ifndef find_next_or_bit
unsigned long _find_next_or_bit(const unsigned long *addr1, const unsigned long *addr2,
					unsigned long nbits, unsigned long start)
{
	return FIND_NEXT_BIT(addr1[idx] | addr2[idx], /* nop */, nbits, start);
}
EXPORT_SYMBOL(_find_next_or_bit);
#endif

#ifndef find_next_zero_bit
unsigned long _find_next_zero_bit(const unsigned long *addr, unsigned long nbits,
					 unsigned long start)
{
	return FIND_NEXT_BIT(~addr[idx], /* nop */, nbits, start);
}
EXPORT_SYMBOL(_find_next_zero_bit);
#endif

#ifndef find_last_bit // find_last_bit
unsigned long _find_last_bit(const unsigned long *addr, unsigned long size)
/*
비트맵은 unsigned long 배열로 표현, addr는 unsigned long 단위로 저장된 비트 묶음의 시작 주소
addr가 가리키는 비트맵에서 0..size-1 범위 안의 마지막 인덱스 1비트를 찾는다.
찾으면 그 비트의 인덱스를 반환하고, 없으면 size를 반환한다.
#ifndef find_last_bit
어떤 아키텍처는 find_last_bit를 자체 최적화(asm 등)로 제공.
그 경우 find_last_bit가 이미 정의되어 있을 수 있으므로, 중복 정의를 피하기 위해 빌드에서 제외.
이 코드는 최적화 버전이 없을 때 쓰는 fallback 구현.
addr : unsigned long 배열로 표현된 비트맵의 시작 주소.
예: cpumask_bits(...) 같은 매크로가 넘겨주는 raw 비트 배열 포인터.
size : 검사할 비트의 개수(상한). 유효한 비트 인덱스 범위는 0..size-1.
워드 : unsigned long 하나, 워드 크기는 아키텍처에 따라 다름.
크기는 BITS_PER_LONG 매크로로 표현
BITS_PER_LONG : unsigned long 이 몇 비트인지 나타내는 값(예 : 64비트면 64)
반환값
마지막 1비트를 찾으면 그 비트의 인덱스(0 기반)를 반환.
1비트가 하나도 없으면 size를 반환.
*/
{
	if (size) { // size가 존재하면 마지막 워드부터 거꾸로 검사.
		/*
		val : 마지막 워드에서 size에 해당하는 유효 비트만 남기기 위한 마스크
		비트맵은 unsigned long 단위로 저장되는데, size가 BITS_PER_LONG의 배수가 아니면 마지막 워드에는 남는 비트(범위 밖)가 존재.
		그 범위 밖 비트가 우연히 1이면 오탐이 되므로, 마지막 워드에서는 유효 비트만 보도록 마스킹.
		BITS_PER_LONG = 64, size = 130이면 마지막 워드는 3번째 워드(idx=2)이고
		마지막 워드에서는 0..1(2개 비트)만 유효하므로 그 부분만 1로 남기는 마스크가 생성된다.
		*/
		unsigned long val = BITMAP_LAST_WORD_MASK(size);
		/*
		idx : size-1 이 속한 워드의 인덱스, 비트맵의 마지막 워드 번호
		BITS_PERLONG = 64, size = 1 => idx = (0) / 64 = 0
		size = 64 => idx = (63) / 64 = 0
		size = 65 => idx = (64) / 64 = 1
		*/
		unsigned long idx = (size-1) / BITS_PER_LONG;

		do { // idx 워드부터 시작해서 0 워드까지 역순으로 내려가며 검사, 각 워드에서 1비트가 발견되면 즉시 반환
			val &= addr[idx]; // 현재 검사 중인 허용 마스크 & 현재 워드의 실제 비트 값
			/*
			현재 워드에 1비트가 하나라도 남아있으면(0이 아니면) 마지막 1비트를 찾은 것.
			__fls(val) : val에서 가장 높은 위치의 1비트 인덱스(0 기반, 워드 내부 기준)를 반환.
			예 : val = 0b00101000이면 __fls(val)=5 같은 식(최상위 1비트 위치).
			(현재 워드의 시작 비트 인덱스) + (워드 내부 최상위 1비트 위치) = idx * BITS_PER_LONG + __fls(val)
			전체 비트맵 기준의 비트 인덱스
			*/
			if (val)
				return idx * BITS_PER_LONG + __fls(val);

			val = ~0ul; // ~ : NOT 연산자 -> 모두 1인 unsigned long
		} while (idx--);
	}
	return size;
}
EXPORT_SYMBOL(_find_last_bit);
#endif

unsigned long find_next_clump8(unsigned long *clump, const unsigned long *addr,
			       unsigned long size, unsigned long offset)
{
	offset = find_next_bit(addr, size, offset);
	if (offset == size)
		return size;

	offset = round_down(offset, 8);
	*clump = bitmap_get_value8(addr, offset);

	return offset;
}
EXPORT_SYMBOL(find_next_clump8);

#ifdef __BIG_ENDIAN

#ifndef find_first_zero_bit_le
/*
 * Find the first cleared bit in an LE memory region.
 */
unsigned long _find_first_zero_bit_le(const unsigned long *addr, unsigned long size)
{
	return FIND_FIRST_BIT(~addr[idx], swab, size);
}
EXPORT_SYMBOL(_find_first_zero_bit_le);

#endif

#ifndef find_next_zero_bit_le
unsigned long _find_next_zero_bit_le(const unsigned long *addr,
					unsigned long size, unsigned long offset)
{
	return FIND_NEXT_BIT(~addr[idx], swab, size, offset);
}
EXPORT_SYMBOL(_find_next_zero_bit_le);
#endif

#ifndef find_next_bit_le
unsigned long _find_next_bit_le(const unsigned long *addr,
				unsigned long size, unsigned long offset)
{
	return FIND_NEXT_BIT(addr[idx], swab, size, offset);
}
EXPORT_SYMBOL(_find_next_bit_le);

#endif

#endif /* __BIG_ENDIAN */

/**
 * find_random_bit - find a set bit at random position
 * @addr: The address to base the search on
 * @size: The bitmap size in bits
 *
 * Returns: a position of a random set bit; >= @size otherwise
 */
unsigned long find_random_bit(const unsigned long *addr, unsigned long size)
{
	int w = bitmap_weight(addr, size);

	switch (w) {
	case 0:
		return size;
	case 1:
		/* Performance trick for single-bit bitmaps */
		return find_first_bit(addr, size);
	default:
		return find_nth_bit(addr, size, get_random_u32_below(w));
	}
}
EXPORT_SYMBOL(find_random_bit);
