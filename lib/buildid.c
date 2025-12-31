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
	if (!r->folio)
		return;
	kunmap_local(r->addr);
	folio_put(r->folio);
	r->folio = NULL;
}

static int freader_get_folio(struct freader *r, loff_t file_off)ㅜ //freader_get_folio
{
	/* check if we can just reuse current folio */
	if (r->folio && file_off >= r->folio_off &&
	    file_off < r->folio_off + folio_size(r->folio))
		return 0;

	freader_put_folio(r);

	/* reject secretmem folios created with memfd_secret() */
	if (secretmem_mapping(r->file->f_mapping))
		return -EFAULT;

	r->folio = filemap_get_folio(r->file->f_mapping, file_off >> PAGE_SHIFT);

	/* if sleeping is allowed, wait for the page, if necessary */
	if (r->may_fault && (IS_ERR(r->folio) || !folio_test_uptodate(r->folio))) {
		filemap_invalidate_lock_shared(r->file->f_mapping);
		r->folio = read_cache_folio(r->file->f_mapping, file_off >> PAGE_SHIFT,
					    NULL, r->file);
		filemap_invalidate_unlock_shared(r->file->f_mapping);
	}

	if (IS_ERR(r->folio) || !folio_test_uptodate(r->folio)) {
		if (!IS_ERR(r->folio))
			folio_put(r->folio);
		r->folio = NULL;
		return -EFAULT;
	}

	r->folio_off = folio_pos(r->folio);
	r->addr = kmap_local_folio(r->folio, 0);

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
	*/
	r->err = -EOVERFLOW; // error overflow
		return NULL;
	}

	/* working with memory buffer is much more straightforward
	메모리 버퍼를 대상으로 작업하는 경우는 훨씬 단순하다 */
	if (!r->buf) { // 메모리 모드(입력이 이미 메모리에 있는 경우)
		if (file_off + sz > r->data_sz) {
			r->err = -ERANGE;
			return NULL;
		}
		return r->data + file_off;
	}

	/* fetch or reuse folio for given file offset
	주어진 파일 오프셋에 해당하는 folio를 가져오거나 재사용한다 */
	r->err = freader_get_folio(r, file_off);
	if (r->err)
		return NULL;

	/* if requested data is crossing folio boundaries, we have to copy
	 * everything into our local buffer to keep a simple linear memory
	 * access interface
	 * 요청한 데이터가 folio 경계를 넘어가는 경우,
	 * 단순한 선형 메모리 접근 인터페이스를 유지하기 위해
	 * 모든 데이터를 로컬 버퍼로 복사해야 한다
	 */
	folio_sz = folio_size(r->folio);
	if (file_off + sz > r->folio_off + folio_sz) {
		u64 part_sz = r->folio_off + folio_sz - file_off, off;

		memcpy(r->buf, r->addr + file_off - r->folio_off, part_sz);
		off = part_sz;

		while (off < sz) {
			/* fetch next folio */
			r->err = freader_get_folio(r, r->folio_off + folio_sz);
			if (r->err)
				return NULL;
			folio_sz = folio_size(r->folio);
			part_sz = min_t(u64, sz - off, folio_sz);
			memcpy(r->buf + off, r->addr, part_sz);
			off += part_sz;
		}

		return r->buf;
	}

	/* if data fits in a single folio, just return direct pointer
	데이터가 하나의 folio 안에 모두 들어가면, 직접 포인터를 반환한다 */
	return r->addr + (file_off - r->folio_off);
}

void freader_cleanup(struct freader *r) // freader_cleanup
{
	if (!r->buf)
		return; /* non-file-backed mode */

	freader_put_folio(r);
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
	파일 모드 : 파일에서 notes르 읽어야 함(페이지/folio를 가져오고 매핑)

	r : 입력 읽기 준비된 reader 상태
	build_id : 결과 저장할 버퍼 주소
	size : 결과 길이 저장할 곳
	note_off : 파싱 시작 위치
	note_size : 파싱할 전체 범위 크기(notes 전체 크기)
	*/
	const char note_name[] = "GNU"; // 찾고 싶은 note의 name이 GNU인지 확인하기 위한 변수
	const size_t note_name_sz = sizeof(note_name); // G N U \n 4바이트
	u32 build_id_off, new_off, note_end, name_sz, desc_sz;
	const Elf32_Nhdr *nhdr;
	const char *data;
	/*
	note_end : note_off + note_size = note 영역 끝 오프셋
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
	커널이 64비트여도 note 포맷 자체는 32-bit헤더(Elf32_Nhbr)로 쓰는 경우가 흔함
	-> note 구조는 대개 32-bit 필드로 고정된 포맷이라서
	*/
	
	if (check_add_overflow(note_off, note_size, &note_end)) // 오버플로우면 true, 아니면 결과를 note_end에 저장
		return -EINVAL; // 인자가 잘못됐나는 의미의 표준 errno(커널에서 음수로 리턴)

	while (note_end - note_off > sizeof(Elf32_Nhdr) + note_name_sz) {
		nhdr = freader_fetch(r, note_off, sizeof(Elf32_Nhdr) + note_name_sz); // 현재 note의 헤더 읽기
		if (!nhdr)
			return r->err;

		name_sz = READ_ONCE(nhdr->n_namesz); // note가 실제로 가진 name 길이
		desc_sz = READ_ONCE(nhdr->n_descsz); // note가 가진 데이터 길이
		// ?READ_ONCE

		new_off = note_off + sizeof(Elf32_Nhdr);
		if (check_add_overflow(new_off, ALIGN(name_sz, 4), &new_off) ||
		    check_add_overflow(new_off, ALIGN(desc_sz, 4), &new_off) ||
		    new_off > note_end)
			break;

		if (nhdr->n_type == BUILD_ID &&
		    name_sz == note_name_sz &&
		    memcmp(nhdr + 1, note_name, note_name_sz) == 0 &&
		    desc_sz > 0 && desc_sz <= BUILD_ID_SIZE_MAX) {
			build_id_off = note_off + sizeof(Elf32_Nhdr) + ALIGN(note_name_sz, 4);

			/* freader_fetch() will invalidate nhdr pointer */
			data = freader_fetch(r, build_id_off, desc_sz);
			if (!data)
				return r->err;

			memcpy(build_id, data, desc_sz);
			memset(build_id + desc_sz, 0, BUILD_ID_SIZE_MAX - desc_sz);
			if (size)
				*size = desc_sz;
			return 0;
		}

		note_off = new_off;
	}

	return -EINVAL;
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

	freader_init_from_mem(&r, buf, buf_size); // 메모리 모드로 초기화

	err = parse_build_id(&r, build_id, NULL, 0, buf_size);

	freader_cleanup(&r);
	return err;
}

#if IS_ENABLED(CONFIG_STACKTRACE_BUILD_ID) || IS_ENABLED(CONFIG_VMCORE_INFO)
unsigned char vmlinux_build_id[BUILD_ID_SIZE_MAX] __ro_after_init;
/*
CONFIG_STACKTRACE_BUILD_ID : 커널이 스택 트레이스 출력할 떄 Build ID 정보를 함께 활용할 수 있게 하는 옵션
->크래시 로그를 분석할 때, 이 스택 트레이스가 정확히 어떤 vmlinux와 매칭되는지를 Build ID로 확인 가능
스택 트레이스 : 호출 경로 스택

CONFIG_VMCORE_INFO : kdump 같은 크래시 덤프(vmcore) 분석을 위해 커널 메모리 덤프에 필요한 메타 정보(vmcoreinfo)를 제공하는 옵션
vmcore 분석에서도 어떤 커널 빌드냐가 중요해서 Build ID가 유용함

IS_ENABLED(x) : 옵션이 y로 빌트인(y)이거나, 경우에 따라 모듈(m)로 활성화되었는지를 C 전처리 단계에서 안전하게 판정하려고 쓰는 매크로

vmlinux_build_id : 커널(vmlinux)의 Build ID 바이트들을 저장해두는 버퍼
unsigned char[]인 이유 : Build ID는 문자열이 아니라 바이너리 바이트 시퀸스
바이너리 바이트 시퀸스 : 컴퓨터가 데이터를 처리하는 기본 단위인 0과 1로 이루어진 바이트들이 순서대로 나열된 형태
?바이트를 저장하는데 왜 unsigned char인가?

BUILD_ID_SIZE_MAX : Build ID가 어떤 길이든 담을 수 있게 최대 크기로 만들어둔 배열
Build ID는 길이가 고정이 아닐 수 있는데(노트 형식/해시 종류에 따라) 커널에서는 최대 길이를 상수로 잡아서 그만큼 버퍼를 확보함

__ro_after_init : init 끝난 뒤에 read-only로 만들겠다는 어트리뷰트
?어트리뷰트
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
