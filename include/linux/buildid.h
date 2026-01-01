/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _LINUX_BUILDID_H
#define _LINUX_BUILDID_H

#include <linux/types.h>

#define BUILD_ID_SIZE_MAX 20

struct vm_area_struct;
int build_id_parse(struct vm_area_struct *vma, unsigned char *build_id, __u32 *size);
int build_id_parse_nofault(struct vm_area_struct *vma, unsigned char *build_id, __u32 *size);
int build_id_parse_buf(const void *buf, unsigned char *build_id, u32 buf_size);

#if IS_ENABLED(CONFIG_STACKTRACE_BUILD_ID) || IS_ENABLED(CONFIG_VMCORE_INFO)
extern unsigned char vmlinux_build_id[BUILD_ID_SIZE_MAX];
void init_vmlinux_build_id(void);
#else
static inline void init_vmlinux_build_id(void) { }
#endif

struct freader { //freader
	/*
	build-id 같은 걸 파싱할 때, 입력을 파일에서 읽든 메모리 버퍼에서 읽든 동일한 api로 읽기 위한 reader상태 구조체
	freader 읽는 대상
	1. 파일 기반 (커널파일/페이지/folio를 매핑해서 읽기)
	2. 메모리 버퍼 기반(이미 메모리에 있는 notes 덩어리 같은거)

	?struct 와 union

	file
	역할 : 열린 파일의 인스턴스 관리
	관련 시스템 : 가상 파일 시스템(VFS), 파일 시스템
	추상화 레벨 : 논리적인 파일 접근 제어
	사용 시점 : 파일 열기(open())부터 닫기(close())까지

	folio
	역할 : 메모리 페이지 그룹 관리
	관련 시스템 : 메모리 관리(MM), 페이지 캐시
	추상화 레벨 : 물리적/가상 메모리 관리
	사용 시점 : 메모리 할당 및 페이지 캐시 작업 시

	struct file (파일 구조체)
	목적: struct file은 열린 파일의 인스턴스를 나타냅니다.
	사용자가 open() 시스템 호출을 사용하여 파일을 열면
	커널 내부에 해당 파일 인스턴스를 관리하기 위한 struct file이 생성됩니다.
	역할: 파일의 모드(읽기, 쓰기 등), 현재 파일 오프셋(파일 포인터), 관련 inode 포인터 등
	열린 파일의 상태 정보를 저장합니다.
	범위: 사용자 공간 프로그램에서는 직접 접근할 수 없으며, 커널 공간에서만 사용됩니다.
	파일 작업(읽기, 쓰기 등)을 수행하는 커널 함수들은 이 struct file 포인터를 인자로 받습니다.

	struct folio (폴리오 구조체)
	목적: struct folio는 커널의 메모리 관리 하위 시스템에서
	하나 이상의 물리적으로 연속된 메모리 페이지 그룹을 나타내는 새로운 추상화 개념입니다.
	역할: 기존의 struct page는 하나의 기본 페이지(일반적으로 4KB) 또는
	더 큰 복합 페이지(compound page)의 일부(head 또는 tail)를 나타낼 수 있어 혼란과 오류를 유발했습니다.
	struct folio는 항상 페이지 그룹의 '헤드(head)'를 나타내도록 보장하여,
	메모리 관리 코드의 명확성과 타입 안정성을 향상시킵니다.
	또한, 큰 페이지(large pages)를 효율적으로 관리하여 메모리 오버헤드를 줄이는 데 도움을 줍니다.
	범위: 주로 페이지 캐시 및 메모리 관리(MM) 하위 시스템 내에서 사용됩니다.
	*/
	void *buf; // 현재 읽고 있는 데이터의 베이스 포인터 / 현재 매핑된 볼록의 시작 주소
	u32 buf_sz; // buf 가 가리키는 전체 입력 버퍼 범위 크기 / u32 : 32비트 부호없는 정수
	int err; // 리더 동작 중 생긴 에러 저장 / 실패시 음수
	union {
		struct { // 파일 모드 블록
			struct file *file; // include/linux/fs.h
			struct folio *folio; // include/linux/mm_types.h
			void *addr;
			loff_t folio_off;
			bool may_fault;
		};
		/*
		struct file *file;
		커널 내부에서 열린 파일을 나타내는 객체 포인터
		이 파일에서 데이터를 읽는다는 입력 소스 핸들

		struct folio *folio;
		파일 내용을 메모리에 올릴 때, page 대신 folio 단위를 씀
		folio = 하나 이상의 연속된 페이지를 대표하는 메모리 관리 객체
		현재 읽고 있는 위치가 속한 folio를 들고 있으면서 필요할 때 다음 folio로 이동하거나 매핑/해제 가능

		void *addr;
		folio를 실제로 CPU가 읽을 수 있도록 커널 가상주소로 매핑한 포인터
		현재 folio의 내용을 여기 주소로 접근 가능한 상태
		파일에서의 읽기 모드
		(1)folio 확보/로딩 -> (2)folio를 커널 매핑 -> (3)add로 읽기 -> (4)unmap/put

		loff_t folio_off;
		folio가 커버하는 범위 중에서 우리가 보려는 시작 오프셋
		loff_t는 커널에서 파일 오프셋에 쓰는 타입

		bool may_fault;
		읽는 과정에서 페이지 폴트가 발생해도 되는 상황인지 같은 플래그로 쓰임
		*/
		struct { // 메모리 모드 블록
			const char *data;
			u64 data_sz;
		};
		/*
		const char *data;
		이미 메모리에 존재하는 입력 데이터의 시작 주소
		
		u64 data_sz;
		입력 데이터 전체 크기
		u64 : 64비트 크기의 부호없는 정수
		buf_size를 그대로 넣어두는 역할
		*/
	};
};

void freader_init_from_file(struct freader *r, void *buf, u32 buf_sz,
			    struct file *file, bool may_fault);
void freader_init_from_mem(struct freader *r, const char *data, u64 data_sz);
const void *freader_fetch(struct freader *r, loff_t file_off, size_t sz);
void freader_cleanup(struct freader *r);

#endif
