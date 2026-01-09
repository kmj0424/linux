// SPDX-License-Identifier: GPL-2.0

#include <linux/buildid.h>
#include <linux/cache.h>
#include <linux/elf.h>
#include <linux/kernel.h>
#include <linux/pagemap.h>
#include <linux/secretmem.h>

#define BUILD_ID 3

#define MAX_PHDR_CNT 256

void freader_init_from_file(struct freader *r, void *buf, u32 buf_sz,
			    struct file *file, bool may_fault)
{
	memset(r, 0, sizeof(*r));
	r->buf = buf;
	r->buf_sz = buf_sz;
	r->file = file;
	r->may_fault = may_fault;
}

void freader_init_from_mem(struct freader *r, const char *data, u64 data_sz)
{
	memset(r, 0, sizeof(*r));
	r->data = data;
	r->data_sz = data_sz;
}

static void freader_put_folio(struct freader *r) //freader_put_folio
{
	/*
	재사용이 안 되는 상황이면 이전 folio의 매핑(r->addr)을 해제하고
	folio refcount를 내려서(put) 누수 방지해야 함.
	*/
	if (!r->folio)
		return;
	kunmap_local(r->addr);
	folio_put(r->folio);
	r->folio = NULL;
}

static int freader_get_folio(struct freader *r, loff_t file_off) //freader_get_folio
{
	/* 파일에서 file_off 오프셋을 읽기 위해, 그 오프셋이 포함된 folio를 페이지 캐시에서
	가져오고(필요 시에 읽어오고), folio를 매핑해 r->addr로 접근 가능하게 만든다
	페이지 캐시에 없으면 디스크에서 읽어와 채운 뒤(uptodate), kmap_local로 매핑
	r->folio : 현재 선택된 folio 객체
	r->folio_off : 이 folio가 파일에서 시작하는 오프셋(= folio가 커버하는 시작 위치)
	r->addr : folio의 내용에 접근 가능한 커널 가상주소(매핑된 주소)
	성공 시 r에 세팅되는 값:
	r->folio : file_off가 속한 folio (페이지캐시에서 확보)
	r->folio_off : 이 folio가 파일에서 시작하는 바이트 오프셋 (folio_pos)
	r->addr : folio 내용을 직접 읽을 수 있는 커널 가상주소 (kmap_local_folio)
	folio의 내용을 파일 데이터로 채우고, 그 folio가 유효하다는 상태(uptodate)를 세팅
	*/
	/* check if we can just reuse current folio */
	if (r->folio && file_off >= r->folio_off &&
	    file_off < r->folio_off + folio_size(r->folio))
	/*
	r->folio : 이미 확보된 folio가 존재하고
	file_off >= r->folio_off : 읽고 싶은 위치가 그 folio의 시작보다 같거나 뒤에 있으며
	file_off < r->folio_off + folio_size(r->folio) : 읽고 싶은 위치가 그 folio의 끝보다 앞에 있다
	지금 읽고 싶은 file_off가 이미 확보해 둔 folio가 담당하는 파일 구간 안에 있다면
	*/
		return 0;

	freader_put_folio(r);
	/*
	r->folio가 없거나(처음이거나) 있더라도 file_off가 현재 folio 범위 밖이라 재사용이 안 되는 상태
	따라서 기존 folio가 있으면 정리하고 file_off가 속한 새 folio를 준비
	*/

	/* reject secretmem folios created with memfd_secret() */
	if (secretmem_mapping(r->file->f_mapping)) // 그냥 리턴 false?
	/*
	secretmem_mapping(r->file->f_mapping) : 이 파일 매핑은 memfd_secret()로 만든 secretmem 기반
	secretmem은 보안 목적상 일반적인 페이지캐시/매핑 경로로 내용을 읽거나 매핑해 접근하는 걸 허용하지 않음
	*/
		return -EFAULT;

	r->folio = filemap_get_folio(r->file->f_mapping, file_off >> PAGE_SHIFT);
	/*
	r->folio 에 담기는 상태
	- 이미 캐시에 있고 uptodate인 folio : 바로 읽기 가능
	- 캐시에 있지만, 아직 uptodate 아님 : 내용 없음 / 불완전
	- 에러 : 바로 실패 처리
	*/

	/* if sleeping is allowed, wait for the page, if necessary */
	if (r->may_fault && (IS_ERR(r->folio) || !folio_test_uptodate(r->folio))) {
	/*
	r->may_fault : 현재 컨텍스트에서 sleep/fault/디스크 IO 대기가 허용되는지 여부
	IS_ERR(r->folio) : 페이지 캐시에서 folio를 얻는 데 실패하여 r->folio가 ERR_PTR(-errno) 상태인 경우
	!folio_test_uptodate(r->folio) : folio 포인터는 정상이나, 데이터가 아직 디스크에서 읽혀 신뢰 가능한 최신 상태(uptodate)로 준비되지 않은 경우
	folio가 없거나/에러이거나/내용이 준비되지 않았고,
	지금 컨텍스트가 잠들 수 있다면,
	read_cache_folio()를 통해 실제 IO를 수행해 folio를 uptodate 상태로 만들 기회를 준다.
	*/
		filemap_invalidate_lock_shared(r->file->f_mapping); // 이 mapping의 페이지 캐시를 건드릴 공유(shared) 락을 잡는 것
		r->folio = read_cache_folio(r->file->f_mapping, file_off >> PAGE_SHIFT,
					    NULL, r->file);
		/*
		성공: 해당 index(file_off>>PAGE_SHIFT)를 커버하는 struct folio * (페이지캐시 folio)
        이 경로는 필요 시 디스크 IO까지 해서 folio 내용을 채워(uptodate) 반환하려고 시도함
		실패: ERR_PTR(-errno) (IS_ERR(r->folio)로 판별)
		*/
		filemap_invalidate_unlock_shared(r->file->f_mapping); // shared 락 해제
	}

	if (IS_ERR(r->folio) || !folio_test_uptodate(r->folio)) { // folio를 얻는 데 실패하거나 데이터가 유효하지 않으면
		if (!IS_ERR(r->folio)) // folio는 얻었는데 데이터가 유효하지 않은 경우
			folio_put(r->folio); // folio의 참조 카운트(refcount)를 1 감소
		r->folio = NULL; // 이 reader에는 현재 유효한 folio가 없다는 상태를 명확히 표시
		return -EFAULT;
	}
	/*
	folio = 여러 개의 page를 묶은 메모리 객체
	파일 캐시(page cache)에서 데이터 단위로 관리됨
	참조 카운트(refcount)로 누가 쓰고 있나를 관리
	*/

	r->folio_off = folio_pos(r->folio); // folio_pos(r->folio) : 현재 확보한 folio가 파일에서 커버하는 구간의 시작 바이트 오프셋
	r->addr = kmap_local_folio(r->folio, 0); //kmap_local_folio(r->folio, 0) : folio 내용을 커널 가상주소로 로컬 매핑한 포인터를 얻음

	return 0;
}

const void *freader_fetch(struct freader *r, loff_t file_off, size_t sz) // freader_fetch
{
	/*
	r : reader 상태(메모리 모드면 r->data/r->data_sz 사용, 파일 모드면 folio 사용)
	file_off : 이번에 읽으려고 하는 시작 오프셋(절대 위치 요청)
	sz : 읽고 싶은 바이트 수

	입력(메모리 버퍼 or 파일)에서 file_Off(입력 시작 기준 오프셋) 위치부터 sz 바이트를 연속된 메모리로 접근하여 포인터를 반환

	성공하면 그 데이터가 있는 메모리 주소 반환
	실패시 NULL 반환 + r->err에 코드 저장
	*/
	size_t folio_sz;

	/* provided internal temporary buffer should be sized correctly 
	제공된 내부 임시 버퍼는 요청 크기를 감당할 수 있을 만큼 충분히 커야 한다 */
	if (WARN_ON(r->buf && sz > r->buf_sz)) {
		/*
		r->buf : 내부 임시 버퍼 포인터(파일 모드에서 경계 걸치면 복사할 때 사용)
		-> 존재한다는 건 복사 버퍼를 쓰는 파일 모드일 가능성이 큼
		r->buf_sz : 그 임시 버퍼의 크기
		WARN_ON : true면 커널 경고를 한 번 띄우고 true를 반환하는 매크로
		파일 모드에서는 경계를 넘는 읽기 요청이 오면 r->buf에 복사해서 이어붙임
		이떄 요청 크기가 버퍼보다 크면 메모리 오버런 위험 -> 에러처리
		*/
		r->err = -E2BIG; // error too big
		return NULL;
	}

	if (unlikely(file_off + sz < file_off)) { // 오버플로우 검사
	/*
	file_off + sz < file_off 라는 상황은 거의 발생하지 않겠지만 발생한다면 error
	file_off + sz 계산 중 정수 오버플로우 발생 시 이후 범위 체크가 무의미해짐 -> 에러 처리
	- 예: file_off가 매우 큰 값인데 sz를 더해 0 근처로 돌아오는 경우
	*/
	r->err = -EOVERFLOW; // error overflow
		return NULL;
	}

	/* working with memory buffer is much more straightforward
	메모리 버퍼를 대상으로 작업하는 경우는 훨씬 단순하다 */
	if (!r->buf) { // 메모리 모드(입력이 이미 메모리에 있는 경우)
		if (file_off + sz > r->data_sz) { // 요청 끝(file_off+sz)이 메모리 버퍼 크기(data_sz)를 넘으면 out-of-range
			r->err = -ERANGE;
			return NULL;
		}
		return r->data + file_off;
	}

	/* fetch or reuse folio for given file offset
	주어진 파일 오프셋에 해당하는 folio를 가져오거나 재사용한다 
	여기부턴 파일 모드 */
	r->err = freader_get_folio(r, file_off); // file_off가 속한 folio를 가져오거나 재사용 -> 실패하면 r->err 세팅되고 NULL 반환
	if (r->err)
		return NULL;

	/* if requested data is crossing folio boundaries, we have to copy
	 * everything into our local buffer to keep a simple linear memory
	 * access interface
	 * 요청한 데이터가 folio 경계를 넘어가는 경우,
	 * 단순한 선형 메모리 접근 인터페이스를 유지하기 위해
	 * 모든 데이터를 로컬 버퍼로 복사해야 한다
	 */
	folio_sz = folio_size(r->folio); // 현재 folio가 커버하는 파일 범위가 어디까지인지
	if (file_off + sz > r->folio_off + folio_sz) {
	/*
	r->folio_off : 현재 folio가 파일에서 시작하는 오프셋
	folio_sz : 현재 folio의 바이트 크기
	file_off + sz : 내가 읽고 싶은 요청의 끝
	요청 끝이 현재 folio 끝을 넘어간다 -> 경계 넘음
	*/
		u64 part_sz = r->folio_off + folio_sz - file_off, off;
		/*
		현재 folio 끝 오프셋: r->folio_off + folio_sz
		요청 시작 오프셋: file_off
		둘의 차이 = 요청 시작부터 folio 끝까지 남은 바이트 수
		part_sz : 요청한 데이터 중에서 현재 folio 안에 들어있는 부분의 길이
		*/
		memcpy(r->buf, r->addr + file_off - r->folio_off, part_sz); // 현재 folio를 r->buf로 복사
		off = part_sz; // 지금까지 채운 길이

		while (off < sz) {
			/* fetch next folio 
			첫 folio에서 가능한 만큼(part_sz) 복사해둔 뒤, 남은(sz-off) 만큼을 다음 folio들에서 계속 이어붙이는 루프 */
			r->err = freader_get_folio(r, r->folio_off + folio_sz);
			/*
			r->folio_off는 현재 folio의 파일 시작 오프셋
			folio_sz는 현재 folio 크기
			r->folio_off + folio_sz는 현재 folio의 끝(= 다음 folio의 시작 오프셋)
			freader_get_folio()가 내부 상태를 갱신
			r->folio_off → 새 folio의 시작 오프셋으로 바뀜
			r->addr → 새 folio의 메모리 시작 주소로 바뀜
			r->folio → 새 folio 객체로 바뀜
			*/
			if (r->err)
				return NULL;
			folio_sz = folio_size(r->folio); // 새 folio 크기 갱신
			part_sz = min_t(u64, sz - off, folio_sz);
			/*
			이번 folio에서 복사할 크기(part_sz) 결정
			sz - off : 아직 남은 요청 바이트 수
			folio_sz : 이번 folio 전체 크기
			그 둘 중 작은 값을 복사
			min_t(u64, ...)는 타입 안전하게 최소값 구하는 커널 매크로
			*/
			memcpy(r->buf + off, r->addr, part_sz); // 새 folio의 앞부분부터 r->buf 뒤에 이어붙임
			off += part_sz; // 채운 길이 갱신
		}

		return r->buf;
	}

	/* if data fits in a single folio, just return direct pointer
	데이터가 하나의 folio 안에 모두 들어가면, 직접 포인터를 반환한다 */
	return r->addr + (file_off - r->folio_off);
}

void freader_cleanup(struct freader *r) // freader_cleanup 마지막으로 잡고 있던 folio가 남아 있으면 깨끗이 풀고 끝내라
{
	if (!r->buf) // 메모리 모드
		return; /* non-file-backed mode */ // folio 매핑 같은 게 없으니 정리할 게 없음

	freader_put_folio(r);
	/*
	kunmap_local(r->addr) : 매핑 해제
	folio_put(r->folio) : refcount(참조카운트) 감소
	r->folio = NULL : 상태 리셋
	*/
}

/*
 * Parse build id from the note segment. This logic can be shared between
 * 32-bit and 64-bit system, because Elf32_Nhdr and Elf64_Nhdr are
 * identical.
 */
static int parse_build_id(struct freader *r, unsigned char *build_id, __u32 *size,
			  loff_t note_off, Elf32_Word note_size) // parse_build_id
{
	/*
	메모리 버퍼 모드 : 이미 notes가 메모리에 있음 (r->data, r->data_sz)
	파일 모드 : 파일에서 notes를 읽어야 함(페이지/folio를 가져오고 매핑)
	freader_fetch()가 내부적으로 folio 매핑/복사까지 처리

	notes 섹션은 여러 개의 note 엔트리가 연속으로 들어있는 컨테이너
	이 함수는 그 엔트리들을 순회하며 build-id note를 찾는다.

	r : 입력 읽기 준비된 reader 상태
	build_id : 결과 저장할 버퍼 주소, vmlinux_build_id[]
	size : 결과 길이 저장할 곳
	note_off : notes 섹션 안에서의 현재 위치
	note_size : 파싱할 전체 범위 크기(notes 전체 크기)
	*/
	const char note_name[] = "GNU"; // note name : GNU에서 type=BUILD_ID인 note를 찾기 위함
	const size_t note_name_sz = sizeof(note_name); // G N U \0 4바이트
	u32 build_id_off, new_off, note_end, name_sz, desc_sz;
	const Elf32_Nhdr *nhdr;
	const char *data;
	/*
	note_end : notes 섹션 끝
	name_sz : 현재 note의 n_namesz
	desc_sz : 현재 note의 n_descz
	new_off : 다음 note로 넘어갈 오프셋 계산 결과
	build_id_off : build-id desc 가 시작되는 오프셋 / build-id 데이터 위치 계산값
	nhdr : note 헤더 포인터(가져온 버퍼를 Elf32_Nhdr로 해석)
	data : desc 바이트들 포인터 / 실제 build-id 데이터 포인터
	
	Elf32_Nhdr(ELF note header 구조체)
	n_namesz : name 길이
	n_descsz : desc 길이
	n_type : note type (build-id 같은 종류 식별)
	
	Elf32_Word(ELF에서 쓰는 32-bit unsigned 정수 타입 - u32랑 동일 계열)
	64비트 ELF에서도 Nhdr 자체는 동일한 구조(32-bit 필드)를 쓴다
	-> note 구조는 대개 32-bit 필드로 고정된 포맷이라서
	*/
	
	if (check_add_overflow(note_off, note_size, &note_end)) // 오버플로우면 true, 아니면 결과를 note_end에 저장
		return -EINVAL; // 인자가 잘못됐나는 의미의 표준 errno(커널에서 음수로 리턴)

	while (note_end - note_off > sizeof(Elf32_Nhdr) + note_name_sz) {
		/*
		note_off부터 note_end까지 남은 바이트가 최소한 헤더 12바이트 + name 4바이트 보다 클 떄만 검사
		note에 헤더는 12바이트 고정 사이즈
		왜 >= 가 아닌가
		desc에 들은 값을 어차피 봐야해서 볼려면 16보다 커야하니까 build-id 찾기 위해
		
		현재 오프셋에서 최소한 헤더(12) + name 크기(4)를 읽을 수 있을 때만 루프 진입.	
		name/desc의 실제 길이는 4배수일 필요는 없고, 다음 필드로 이동할 때 4바이트 정렬을 맞추기 위해 padding을 포함한 이동량을 ALIGN()로 계산한다.
		*/
		nhdr = freader_fetch(r, note_off, sizeof(Elf32_Nhdr) + note_name_sz); // 현재 note의 헤더 읽기
		if (!nhdr)
			return r->err;

		name_sz = READ_ONCE(nhdr->n_namesz); // note가 실제로 가진 name 길이
		desc_sz = READ_ONCE(nhdr->n_descsz); // note가 가진 데이터 길이
		/*
		READ_ONCE : 컴파일러가 값을 한번만 메모리에서 읽도록 강제
		메모리 맵/동시성/최적화 상황에서 값이 이상하게 보이는 걸 막고 파서가 일관된 값으로 계산하게 하려는 방어적 습관
		*/
		new_off = note_off + sizeof(Elf32_Nhdr); // 헤더 건너뛰고 name으로
		if (check_add_overflow(new_off, ALIGN(name_sz, 4), &new_off) ||
		    check_add_overflow(new_off, ALIGN(desc_sz, 4), &new_off) ||
		    new_off > note_end)
		/*
		check_add_overflow(new_off, ALIGN(name_sz, 4), &new_off)
		-name 크기(name_sz)가 정상 범위인지 간접적으로 확인하는 단계 -> 확인 후 다음 주소 반환
		check_add_overflow(new_off, ALIGN(desc_sz, 4), &new_off)
		-desc 크기(desc_sz)가 정상 범위인지 간접적으로 확인하는 단계
		new_off > note_end : 계산된 note 전체 크기가 notes 컨테이너 범위를 넘는지 확인
		ALIGN(sz, 4) : 길이를 4바이트 경계로 올림해라
		끝나면 다음 오프셋
		*/
			break;

		if (nhdr->n_type == BUILD_ID && // note의 타입이 build-id 인지
		    name_sz == note_name_sz && // note가 가진 name 길이가 우리가 기대하는 길이(4)와 같은지
		    memcmp(nhdr + 1, note_name, note_name_sz) == 0 && // nhdr + 1은 헤더 바로 뒤, name이 GNU인지 확인
		    desc_sz > 0 && desc_sz <= BUILD_ID_SIZE_MAX) { // build-id는 실제 데이터(desc)가 있어야 하니까 >0
			// 결과 버퍼 최대치보다 크면 복사하면 터지니까 상한 검사
			build_id_off = note_off + sizeof(Elf32_Nhdr) + ALIGN(note_name_sz, 4); // build-id 데이터(desc)가 시작하는 위치 계산

			/* freader_fetch() will invalidate nhdr pointer */
			data = freader_fetch(r, build_id_off, desc_sz); // desc(build-id 바이트) 실제로 읽어오기
			if (!data) // ?가능한가 위에서 사이즈 체크했는데 \0?
				return r->err;

			memcpy(build_id, data, desc_sz); // build_id(= vmlinux_build_id[])에 build-id 바이트를 복사
			memset(build_id + desc_sz, 0, BUILD_ID_SIZE_MAX - desc_sz); // 남는 부분은 0으로 채워서 항상 고정 크기 배열 형태 유지
			if (size) // 성공 경로일때, build-id 잘 찾고 모든 검사 통과했을 떄
				*size = desc_sz; // build_id_len = desc_sz;
			return 0;
		}

		note_off = new_off; // 다음 note 엔트리 시작으로 이동
	}
	// 범위 안에서 GNU build-id note를 못 찾았거나 중간에 데이터가 이상해서 break로 나왔거나
	return -EINVAL; // 끝까지 없으면 에러
}

/* Parse build ID from 32-bit ELF */
static int get_build_id_32(struct freader *r, unsigned char *build_id, __u32 *size)
{
	const Elf32_Ehdr *ehdr;
	const Elf32_Phdr *phdr;
	__u32 phnum, phoff, i;

	ehdr = freader_fetch(r, 0, sizeof(Elf32_Ehdr));
	if (!ehdr)
		return r->err;

	/* subsequent freader_fetch() calls invalidate pointers, so remember locally */
	phnum = READ_ONCE(ehdr->e_phnum);
	phoff = READ_ONCE(ehdr->e_phoff);

	/* set upper bound on amount of segments (phdrs) we iterate */
	if (phnum > MAX_PHDR_CNT)
		phnum = MAX_PHDR_CNT;

	/* check that phoff is not large enough to cause an overflow */
	if (phoff + phnum * sizeof(Elf32_Phdr) < phoff)
		return -EINVAL;

	for (i = 0; i < phnum; ++i) {
		phdr = freader_fetch(r, phoff + i * sizeof(Elf32_Phdr), sizeof(Elf32_Phdr));
		if (!phdr)
			return r->err;

		if (phdr->p_type == PT_NOTE &&
		    !parse_build_id(r, build_id, size, READ_ONCE(phdr->p_offset),
				    READ_ONCE(phdr->p_filesz)))
			return 0;
	}
	return -EINVAL;
}

/* Parse build ID from 64-bit ELF */
static int get_build_id_64(struct freader *r, unsigned char *build_id, __u32 *size)
{
	const Elf64_Ehdr *ehdr;
	const Elf64_Phdr *phdr;
	__u32 phnum, i;
	__u64 phoff;

	ehdr = freader_fetch(r, 0, sizeof(Elf64_Ehdr));
	if (!ehdr)
		return r->err;

	/* subsequent freader_fetch() calls invalidate pointers, so remember locally */
	phnum = READ_ONCE(ehdr->e_phnum);
	phoff = READ_ONCE(ehdr->e_phoff);

	/* set upper bound on amount of segments (phdrs) we iterate */
	if (phnum > MAX_PHDR_CNT)
		phnum = MAX_PHDR_CNT;

	/* check that phoff is not large enough to cause an overflow */
	if (phoff + phnum * sizeof(Elf64_Phdr) < phoff)
		return -EINVAL;

	for (i = 0; i < phnum; ++i) {
		phdr = freader_fetch(r, phoff + i * sizeof(Elf64_Phdr), sizeof(Elf64_Phdr));
		if (!phdr)
			return r->err;

		if (phdr->p_type == PT_NOTE &&
		    !parse_build_id(r, build_id, size, READ_ONCE(phdr->p_offset),
				    READ_ONCE(phdr->p_filesz)))
			return 0;
	}

	return -EINVAL;
}

/* enough for Elf64_Ehdr, Elf64_Phdr, and all the smaller requests */
#define MAX_FREADER_BUF_SZ 64

static int __build_id_parse(struct vm_area_struct *vma, unsigned char *build_id,
			    __u32 *size, bool may_fault)
{
	const Elf32_Ehdr *ehdr;
	struct freader r;
	char buf[MAX_FREADER_BUF_SZ];
	int ret;

	/* only works for page backed storage  */
	if (!vma->vm_file)
		return -EINVAL;

	freader_init_from_file(&r, buf, sizeof(buf), vma->vm_file, may_fault);

	/* fetch first 18 bytes of ELF header for checks */
	ehdr = freader_fetch(&r, 0, offsetofend(Elf32_Ehdr, e_type));
	if (!ehdr) {
		ret = r.err;
		goto out;
	}

	ret = -EINVAL;

	/* compare magic x7f "ELF" */
	if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0)
		goto out;

	/* only support executable file and shared object file */
	if (ehdr->e_type != ET_EXEC && ehdr->e_type != ET_DYN)
		goto out;

	if (ehdr->e_ident[EI_CLASS] == ELFCLASS32)
		ret = get_build_id_32(&r, build_id, size);
	else if (ehdr->e_ident[EI_CLASS] == ELFCLASS64)
		ret = get_build_id_64(&r, build_id, size);
out:
	freader_cleanup(&r);
	return ret;
}

/*
 * Parse build ID of ELF file mapped to vma
 * @vma:      vma object
 * @build_id: buffer to store build id, at least BUILD_ID_SIZE long
 * @size:     returns actual build id size in case of success
 *
 * Assumes no page fault can be taken, so if relevant portions of ELF file are
 * not already paged in, fetching of build ID fails.
 *
 * Return: 0 on success; negative error, otherwise
 */
int build_id_parse_nofault(struct vm_area_struct *vma, unsigned char *build_id, __u32 *size)
{
	return __build_id_parse(vma, build_id, size, false /* !may_fault */);
}

/*
 * Parse build ID of ELF file mapped to VMA
 * @vma:      vma object
 * @build_id: buffer to store build id, at least BUILD_ID_SIZE long
 * @size:     returns actual build id size in case of success
 *
 * Assumes faultable context and can cause page faults to bring in file data
 * into page cache.
 *
 * Return: 0 on success; negative error, otherwise
 */
int build_id_parse(struct vm_area_struct *vma, unsigned char *build_id, __u32 *size)
{
	return __build_id_parse(vma, build_id, size, true /* may_fault */);
}

/**
 * build_id_parse_buf - Get build ID from a buffer
 * @buf:      ELF note section(s) to parse
 * @buf_size: Size of @buf in bytes
 * @build_id: Build ID parsed from @buf, at least BUILD_ID_SIZE_MAX long
 *
 * Return: 0 on success, -EINVAL otherwise
 */
int build_id_parse_buf(const void *buf, unsigned char *build_id, u32 buf_size)
{
	struct freader r; // include/linux/buildid.h
	int err;

	/*
	파일이 아니라 메모리 버퍼 buf를 입력으로 삼고, 읽을 수 있는 최대 범위는 buf_size까지
	현재 읽기 위치, 남은 바이트 수, 버퍼 끝 경계를 freader 내부에 세팅
	*/
	freader_init_from_mem(&r, buf, buf_size); // 메모리 모드로 초기화

	err = parse_build_id(&r, build_id, NULL, 0, buf_size);

	freader_cleanup(&r);
	return err;
}

#if IS_ENABLED(CONFIG_STACKTRACE_BUILD_ID) || IS_ENABLED(CONFIG_VMCORE_INFO)
// 스택 트레이스용이든, 크래시 덤프용이든 build-id가 필요한 기능이 하나라도 켜져 있으면 아래 변수를 만든다
unsigned char vmlinux_build_id[BUILD_ID_SIZE_MAX] __ro_after_init;
/*
CONFIG_STACKTRACE_BUILD_ID : 커널이 스택 트레이스 출력할 떄 Build ID 정보를 함께 활용할 수 있게 하는 옵션
->크래시 로그를 분석할 때, 이 스택 트레이스가 정확히 어떤 vmlinux와 매칭되는지를 Build ID로 확인 가능
스택 트레이스 : 호출 경로 스택

CONFIG_VMCORE_INFO : kdump 같은 크래시 덤프(vmcore) 분석을 위해 커널 메모리 덤프에 필요한 메타 정보(vmcoreinfo)를 제공하는 옵션
vmcore 분석에서도 어떤 커널 빌드냐가 중요해서 Build ID가 유용함

IS_ENABLED(x) : 옵션이 y로 빌트인(y)이거나, 경우에 따라 모듈(m)로 활성화되었는지를 C 전처리 단계에서 안전하게 판정하려고 쓰는 매크로

vmlinux_build_id : 커널(vmlinux)의 Build ID 바이트들을 저장해두는 버퍼
unsigned char[]인 이유 : Build ID는 문자열이 아니라 바이너리 바이트 시퀸스, 바이트를 저장하는 배열
바이너리 바이트 시퀸스 : 컴퓨터가 데이터를 처리하는 기본 단위인 0과 1로 이루어진 바이트들이 순서대로 나열된 형태

BUILD_ID_SIZE_MAX : Build ID가 어떤 길이든 담을 수 있게 최대 크기로 만들어둔 배열
Build ID는 길이가 고정이 아닐 수 있는데(노트 형식/해시 종류에 따라) 커널에서는 최대 길이를 상수로 잡아서 그만큼 버퍼를 확보함

__ro_after_init : init 끝난 뒤에 read-only로 취급
이 값은 부팅때 한 번 계산해서 저장하면 끝이라서, 이후엔 수정될 이유가 없음
이후에 읽기 전용으로 바꾸면 버그나 공격으로 값이 변조될 가능성이 줄어듦
-> init 코드에서만 채워지고 init이후에는 커널이 이 메모리를 쓰기 금지로 보호할 수 있음(커널의 rodata보호 메커니즘과 연결)
?rodata보호 메커니즘?
*/
/**
 * init_vmlinux_build_id - Compute and stash the running kernel's build ID
 */
void __init init_vmlinux_build_id(void) // init_vmlinux_build_id
{
	extern const void __start_notes;
	extern const void __stop_notes;
	/*
	경계 심볼 : 링크 스트립트가 notes 섹션의 시작/끝 주소를 심볼로 박아주는 형태
	-> extren 으로 그런 심볼이 어딘가에 있으니 주소를 가져다 쓰겠다는 뜻
	Build ID는 ELF note 영역에 들어있음
	커널 이미지(vmlinux)애도 note 섹션들이 있고, 그 중에 build-id 노트가 있음
	커널은 부팅 시점에 그 노트 섹션 범위를 메모리에서 찾아서 파싱하려 함
	note들이 모여 있는 메모리 범위의 시작과 끝
	*/
	unsigned int size = &__stop_notes - &__start_notes;
	/*
	size 계산 : &__stop_notes ~ &__start_notes 사이가 notes 데이터 덩어리라고 보고 그 길이를 size로 구해서 파서에 넘김
	*/

	build_id_parse_buf(&__start_notes, vmlinux_build_id, size);
	/*
	&__start_notes : notes 영역의 시작 주소(여기부터 파싱 시작)
	&__stop_notes : notes 영역 끝 주소
	vmlinux_build_id : 파싱해서 얻은 Build ID 바이트를 여기에 복사해서 저장할 목적지 버퍼
	size : notes 영역의 총 크기 / 파서는 이 범위를 넘어가지 않게 안전하게 탐색하면서 build-id note를 찾음

	notes 전체를 스캔해서 build-id note를 찾아라
	notes 버퍼를 note 포멧(ELF note)단위로 순회하면서 이 노트가 build-id 인지 확인하고 맞으면 desc(payload)부분을 꺼내서 vmlinux_build_id로 복사
	실패하면(해당 노트가 없거나 손상) 보통은 0으로 남거나, 어떤 방식으로든 없음 상태로 처리
	*/
}
#endif
