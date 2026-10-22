#include "userprog/syscall.h"
#include <stdio.h>
#include <string.h>
#include <syscall-nr.h>
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "threads/loader.h"
#include "userprog/gdt.h"
#include "threads/flags.h"
#include "intrinsic.h"
#include "userprog/process.h"
#include "threads/malloc.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/palloc.h"
#include "threads/vaddr.h"
#include "devices/input.h"
#include "threads/init.h"

void syscall_entry (void);
void syscall_handler (struct intr_frame *);


void check_address (const void *addr); 

/* 시스템 콜(System call).
 *
 * 예전에는 시스템 콜 서비스를 인터럽트 핸들러(interrupt handler)가 처리했음
 * (예: linux의 int 0x80). 하지만 x86-64에서는 제조사가 시스템 콜을 요청하는
 * 효율적인 경로인 `syscall` 명령어를 제공함.
 *
 * syscall 명령어는 모델 특정 레지스터(Model Specific Register, MSR)의
 * 값을 읽어서 동작함. 자세한 내용은 매뉴얼을 참고할 것. */

#define MSR_STAR 0xc0000081         /* 세그먼트 셀렉터(Segment selector) msr */
#define MSR_LSTAR 0xc0000082        /* 롱 모드(Long mode) SYSCALL 대상 */
#define MSR_SYSCALL_MASK 0xc0000084 /* eflags용 마스크 */

#define IO_CHUNK 512                /* read/write가 커널 버퍼를 거치는 단위 (디스크 섹터 크기).
                                       프로젝트 2에는 필수 아님. 프로젝트 3에서 fault 처리(lazy load, mmap write-back)가
                                       filesys_lock을 잡는다면, 락을 쥔 채 유저 메모리를 건드릴 때 같은 락을 다시 잡아 패닉이 남.
                                       그래서 락 안에서는 커널 버퍼만 만지는 구조로 미리 만들어 둠.
                                       fault 경로가 락을 전혀 안 잡기로 정해지면 이 청킹은 직접 읽기/쓰기로 되돌려도 됨. */
#define NAME_BUF 128                /* 파일 이름을 커널로 복사해 두는 버퍼 크기 */
struct lock filesys_lock;           /* 전역 파일 시스템 락 */

/**
 * @brief syscall 명령어용 MSR을 설정하고 전역 filesys_lock을 초기화함
 *
 * @details
 * Pintos의 파일 시스템(filesys_*, file_*)은 내부 동기화가 없으므로,
 * 모든 파일 시스템 호출을 락 하나로 직렬화하기로 정함. 락을 여러 개로 쪼개면
 * 락 순서 문제가 생기고 P2 범위에서는 성능 이득도 없어서 전역 락 하나만 씀.
 * 시스템 콜이 처음 들어오기 전에 락이 준비되어 있어야 하므로 여기서 초기화함.
 *
 * @note threads/init.c의 main()에서 부팅 중 한 번만 호출됨
 */
void
syscall_init (void) {
	lock_init(&filesys_lock);
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* 인터럽트 서비스 루틴(interrupt service routine)은 syscall_entry가
	 * 유저랜드 스택을 커널 모드 스택으로 바꿀 때까지 어떤 인터럽트도
	 * 처리하면 안 됨. 따라서 FLAG_FL을 마스크함. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}

/**
 * @brief 종료 상태 STATUS를 기록하고 현재 프로세스를 종료함
 *
 * @details
 * exit 시스템 콜뿐 아니라 잘못된 유저 포인터(check_address),
 * 유저 모드 page fault(exception.c의 page_fault)도 모두 sys_exit(-1)로 들어옴.
 * 종료 경로를 이 함수 하나로 모아서, 어떤 이유로 죽든 같은 방식으로 상태가 남게 함.
 *
 * 상태를 두 곳에 씀.
 * - self_info->exit_status: 부모의 process_wait()가 읽어 갈 값
 * - exit_info->status: process_exit()가 종료 메시지에 쓸 값
 *
 * exit_info는 malloc으로 받으므로 실패할 수 있음. 그래서 self_info에 먼저 써 두어,
 * 할당이 실패해도 process_exit()가 self_info 쪽 값으로 메시지와 wait 결과를 맞추게 함.
 *
 * @param[in] status 부모에게 전달하고 종료 메시지에 찍을 종료 상태
 *
 * @note 반환하지 않음. thread_exit()가 process_exit()를 거쳐 자원을 정리함
 * @see process_exit(), process_wait()
 */
void sys_exit (int status) {
	struct thread *cur = thread_current ();

	/* 종료 기록 할당에 실패해도 부모에게 전달할 상태는 보존함. */
	if (cur->self_info != NULL)
		cur->self_info->exit_status = status;
	if (cur->exit_info == NULL)
		cur->exit_info = malloc (sizeof *cur->exit_info);
	if (cur->exit_info != NULL)
		cur->exit_info->status = status;

	thread_exit ();
}

/**
 * @brief 유저가 넘긴 주소 ADDR 하나가 접근 가능한 유저 페이지인지 검사함
 *
 * @details
 * 세 가지를 차례로 봄.
 * 1. NULL이 아님
 * 2. 유저 영역 주소임(is_user_vaddr). 커널 주소를 넘겨 커널 메모리를 읽거나 쓰는 것을 막음
 * 3. 현재 프로세스의 페이지 테이블에 실제로 매핑되어 있음(pml4_get_page)
 *
 * 하나라도 어기면 커널이 그 주소를 건드리기 전에 sys_exit(-1)로 프로세스를 끝냄.
 * page fault가 난 뒤 수습하는 방식 대신 "먼저 검사하고 쓰는" 방식을 택함.
 * 검사 위치가 시스템 콜 입구로 모여 있어서 흐름을 따라가기 쉽기 때문임.
 *
 * @param[in] addr 검사할 유저 가상 주소
 *
 * @note 이 주소가 속한 한 페이지만 봄. 여러 페이지에 걸친 버퍼는 check_buffer(),
 *       문자열은 copy_name()을 씀
 */
void check_address(const void *addr) {
	if (addr == NULL || !is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL) {
		sys_exit(-1);
	}
}

/**
 * @brief 유저 버퍼 [buf, buf + size)가 걸친 모든 페이지를 검사함
 *
 * @details
 * read/write 버퍼는 시작 주소만 멀쩡하고 중간이나 끝이 매핑 안 된 페이지일 수 있음
 * (예: 스택 끝에 걸친 버퍼). 시작 주소만 보면 커널이 memcpy하다 page fault로 죽으므로,
 * 버퍼가 걸친 페이지마다 한 번씩 검사함. 바이트 단위가 아니라 페이지 단위로 도는 이유는
 * 매핑 여부가 페이지 단위로 정해지기 때문임.
 *
 * @param[in] buf 유저 버퍼 시작 주소
 * @param[in] size 버퍼 크기(바이트). 0이면 시작 주소만 검사함
 *
 * @note 첫 페이지는 check_address(buf)가, 그 뒤 페이지는 각 페이지의 시작 주소로 검사함.
 * @note
 * 임시: 포인터 검증 함수가 나오면 그 호출로 바꿀 것.
 * check_address()가 pml4_get_page()를 쓰므로 프로젝트 3의 lazy 페이지에는 맞지 않음.
 */
static void check_buffer(const void *buf, size_t size) {
	check_address(buf);
	if (size == 0) {
		return;
	}
	const char *end = (const char *)buf + size - 1;
	for (const char *p = (const char *)pg_round_down(buf) + PGSIZE; p <= end; p += PGSIZE) {
		check_address(p);
	}
}

/**
 * @brief 유저 문자열을 커널 버퍼로 복사함
 *
 * @details
 * 문자열은 길이를 미리 알 수 없어서 check_buffer()를 쓸 수 없음. 그래서 한 글자씩
 * 복사하면서 새 페이지에 들어설 때만 check_address()로 검사함.
 * 복사본을 커널 스택 버퍼에 두면 두 가지가 해결됨.
 * - filesys_lock을 잡은 동안에는 유저 메모리를 건드리지 않게 됨 (아래 note)
 * - 검사한 뒤 다른 스레드가 유저 문자열을 바꿔도 커널은 복사본만 씀
 *
 * @param[out] kdst 복사해 둘 커널 버퍼
 * @param[in] usrc 유저가 넘긴 문자열
 * @param[in] max kdst 크기 (NUL 포함)
 * @return NUL까지 max 안에 복사했으면 true, 그보다 길면 false
 *
 * @note 
 * 락을 잡기 전에 불러야 함. 프로젝트 2의 page_fault()는 filesys_lock을 안 잡아서 지금은 문제없음.
 * 프로젝트 3에서 fault 처리(lazy load, mmap)가 이 락을 잡는다면, 락을 쥔 채 유저 메모리를 읽다가 fault가 날 때
 * 같은 락을 다시 잡으려 해서 패닉이 남(락은 재귀 불가). 그때를 대비한 설계임.
 * @note 
 * 첫 페이지는 호출한 쪽의 check_address()가 검사하고, 여기서는 새 페이지에 들어설 때마다 검사함.
 * 임시: 문자열 검증 함수가 나오면 바꿀 것.
 */
static bool copy_name(char *kdst, const char *usrc, size_t max) {
	for (size_t i = 0; i < max; i++) {
		if (i > 0 && pg_ofs(usrc + i) == 0) {
			check_address(usrc + i);
		}
		kdst[i] = usrc[i];
		if (kdst[i] == '\0') {
			return true;
		}
	}
	return false;
}

/**
 * @brief 현재 스레드의 fd 테이블을 할당함
 *
 * @details
 * calloc으로 FD_MAX칸(64칸 = 512바이트)을 받아 전부 NULL인 상태로 시작함.
 * fd 번호를 그대로 배열 인덱스로 쓰는 고정 크기 배열이라 조회가 O(1)이고 구현이 단순함.
 * 처음에는 palloc으로 4KB 페이지(512칸)를 통째로 받았지만, 칸 하나가 포인터 8바이트라
 * 대부분이 비어 있는 낭비여서 64칸 calloc으로 줄임. 테스트가 동시에 여는 파일 수는 64칸 안에 들어옴.
 *
 * @note 할당에 실패하면 fd_table은 NULL로 남고, 이후 fd_add()가 -1을 반환함.
 * @note 유저 프로세스가 생기는 두 곳(initd(), __do_fork())에서만 호출함.
 *       커널 스레드는 테이블이 없음(NULL)
 */
void fd_table_create(void) {
	thread_current()->fd_table = calloc(FD_MAX, sizeof(struct file *));
}

/**
 * @brief fd에 연결된 열린 파일을 찾음
 *
 * @details
 * fd는 유저가 마음대로 넘기는 정수라 음수나 아주 큰 값이 올 수 있음.
 * 배열 범위를 벗어난 읽기를 막으려고, fd로 테이블을 여는 곳은 모두 이 함수를 거치게 함.
 * 0, 1은 stdin/stdout 예약 번호라 테이블에서 찾지 않고 NULL을 돌려줌.
 * 호출하는 쪽에서 0, 1을 따로 처리함(SYS_READ, SYS_WRITE).
 *
 * @param[in] fd 유저가 넘긴 파일 디스크립터
 * @return 연결된 struct file *. 테이블이 없거나, fd가 2 미만·FD_MAX 이상이거나, 빈 칸이면 NULL
 *
 * @note 잘못된 fd여도 exit하지 않음. 범위 검사는 이 함수 한 곳에서만 함.
 *       잘못된 fd는 커널 메모리를 위협하지 않는 유저의 실수라서, 프로세스를 죽이지 않고
 *       시스템 콜이 실패값(-1)을 돌려주게 함
 */
static struct file *fd_get(int fd) {
	struct file **table = thread_current()->fd_table;

	if (table == NULL || fd < 2 || fd >= (int)FD_MAX) {
		return NULL;
	}
	return table[fd];
}

/**
 * @brief 열린 파일을 fd 테이블의 빈 칸에 등록함
 *
 * @details
 * POSIX처럼 비어 있는 가장 작은 번호를 줌. 닫힌 번호를 다시 쓰므로 open/close를
 * 반복해도 번호가 바닥나지 않음. 64칸뿐이라 앞에서부터 훑는 선형 탐색으로 충분함.
 *
 * @param[in] file 등록할 열린 파일
 * @return 배정된 fd (2번부터 비어 있는 가장 작은 번호). 테이블이 없거나 꽉 차면 -1
 *
 * @note 실패해도 file을 닫지 않음. 호출한 쪽이 닫아야 함(SYS_OPEN 참고).
 *       file을 연 쪽이 실패 처리까지 책임지게 해서 소유권을 한 곳에 둠
 */
int fd_add(struct file *file) {
	struct file **table = thread_current()->fd_table;

	if (table == NULL) {
		return -1;
	}
	for (int fd = 2; fd < (int)FD_MAX; fd++) {
		if (table[fd] == NULL) {
			table[fd] = file;
			return fd;
		}
	}
	return -1;
}

/**
 * @brief fd에 연결된 파일을 닫고 칸을 비움
 *
 * @details
 * 칸을 먼저 NULL로 비운 뒤 파일을 닫음. 닫힌 struct file을 가리키는 칸이
 * 테이블에 남는 순간이 없게 하려는 순서임.
 *
 * @param[in] fd 닫을 파일 디스크립터
 *
 * @note 잘못된 fd나 이미 닫은 fd면 아무것도 하지 않음(close-twice, close-bad-fd).
 * @note file_close() 호출 부분만 filesys_lock을 잡음. 락 구간을 짧게 둬서
 *       다른 프로세스의 파일 시스템 호출이 오래 기다리지 않게 함
 */
void fd_close(int fd) {
	struct file *file = fd_get(fd);

	if (file == NULL) {
		return;
	}
	thread_current()->fd_table[fd] = NULL;

	lock_acquire(&filesys_lock);
	file_close(file);
	lock_release(&filesys_lock);
}

/**
 * @brief 열린 파일을 모두 닫고 fd 테이블을 해제함
 *
 * @details
 * 프로세스 종료 시 process_exit()에서 호출함.
 * 유저가 close하지 않고 끝낸 파일도 여기서 닫아야 inode 참조 수가 줄어,
 * 나중에 remove된 파일이 실제로 지워지고 메모리도 새지 않음.
 * 칸마다 fd_close()를 다시 써서 닫는 방식(칸 비우기, 락 잡기)을 한 곳에 둠.
 *
 * @note 테이블이 없으면(커널 스레드, fork 실패) 아무것도 하지 않음.
 * @note 해제 뒤 fd_table을 NULL로 둬서, 혹시 다시 불려도 이중 해제가 나지 않음
 */
void fd_table_close_all(void) {
	struct thread *cur = thread_current();

	if (cur->fd_table == NULL) {
		return;
	}
	for (int fd = 2; fd < (int)FD_MAX; fd++) { // 0,1은 예약이라고 가정
		fd_close(fd);
	}
	free(cur->fd_table);
	cur->fd_table = NULL;
}



/**
 * @brief 시스템 콜 번호에 따라 각 시스템 콜을 처리하는 디스패처
 *
 * @details
 * x86-64 syscall 규약을 그대로 따름. 번호는 rax, 인자는 rdi, rsi, rdx 순서로 오고,
 * 반환값은 f->R.rax에 써 두면 syscall_entry가 유저로 돌아갈 때 rax로 복원함.
 * 첫 인자가 fd인 시스템 콜이 많아서 rdi를 미리 fd로 꺼내 둠.
 *
 * 파일 시스템 콜은 모두 같은 순서를 지킴.
 * 1. 유저 포인터 검증 (check_address, check_buffer)
 * 2. 유저 데이터를 커널 버퍼로 복사 (copy_name, IO_CHUNK 버퍼)
 * 3. filesys_lock을 잡고 파일 시스템 함수 호출, 바로 해제
 * 4. 결과를 rax에 씀
 * 락을 쥔 채 유저 메모리를 건드리지 않는 것이 핵심 규칙임. 프로젝트 3에서 page fault 처리가
 * 같은 락을 잡으면 재귀 획득으로 패닉이 나기 때문임 (IO_CHUNK, copy_name 주석 참고).
 *
 * 잘못된 포인터는 프로세스를 끝내고(sys_exit(-1)), 잘못된 fd는 실패값만 돌려줌.
 * 포인터는 커널 메모리를 위협하지만 fd는 테이블 범위 검사만으로 안전하기 때문임.
 *
 * @param[in,out] f 유저 레지스터가 저장된 인터럽트 프레임. rax에 반환값을 씀
 */
void
syscall_handler (struct intr_frame *f UNUSED) {
	int fd = f->R.rdi;
	switch (f->R.rax)
	{
		case SYS_HALT:
			power_off();
			break;
		case SYS_EXIT: {		/* exit(status) 첫 인자는 rdi로 전달 */
			int status = (int)f->R.rdi;
			struct thread *cur = thread_current();

			struct exit_info *record = malloc(sizeof(struct exit_info));		/* 종료 상태 저장할 실제 메모리 공간 할당 */
			if (record == NULL) {		/* 할당 실패하면 기록에 접근하지 않고 현재 스레드 종료 */
				thread_exit();
			}
			
			record->status = status;		// 구조체 안에 종료 상태 저장

			cur->exit_info = record;		// process_exit()에서도 같은 기록 찾을 수 있도록 현재 스레드에 구조체 주소 저장

			thread_exit();		/* 내부에서 process_exit() 호출해 자원 정리하고 현재 스레드 종료 */
		}
		case SYS_FORK:
			f->R.rax = process_fork(f->R.rdi, f);
			break;
		case SYS_EXEC:
			check_address(f->R.rdi);	
			size_t len = strlen((char *)f->R.rdi) + 1;
			char *k = palloc_get_page(len);
			if (k == NULL) {
				sys_exit(-1);
			}
			strlcpy(k, f->R.rdi, len);
			
			f->R.rax = process_exec(k);
			sys_exit(-1);
			break;
		case SYS_WAIT:
			f->R.rax = process_wait(f->R.rdi);
			break;
		case SYS_CREATE: {
			const char *create_file_name = (const char *)f->R.rdi;
			unsigned initial_size = (unsigned)f->R.rsi;

			check_address(create_file_name);

			char kname[NAME_BUF];
			if (!copy_name(kname, create_file_name, sizeof kname)) {	// 너무 긴 이름은 실패
				f->R.rax = false;
				break;
			}

			lock_acquire(&filesys_lock);
			f->R.rax = filesys_create(kname, initial_size);
			lock_release(&filesys_lock);

			break;
		}
		case SYS_REMOVE: {
			const char *remove_file_name = (const char *)f->R.rdi;

			check_address(remove_file_name);

			char kname[NAME_BUF];
			if (!copy_name(kname, remove_file_name, sizeof kname)) {
				f->R.rax = false;
				break;
			}

			lock_acquire(&filesys_lock);
			f->R.rax = filesys_remove(kname);
			lock_release(&filesys_lock);

			break;
		}
		case SYS_OPEN: {
			const char *open_file_name = (const char *)f->R.rdi;

			check_address(open_file_name);

			char kname[NAME_BUF];
			if (!copy_name(kname, open_file_name, sizeof kname)) {
				f->R.rax = -1;
				break;
			}

			lock_acquire(&filesys_lock);
			struct file *file = filesys_open(kname);
			lock_release(&filesys_lock);

			if (file == NULL) {	// 없는 파일이거나 빈 이름이면 실패(-1)
				f->R.rax = -1;
				break;
			}

			int new_fd = fd_add(file);
			if (new_fd == -1) {	// fd 테이블이 꽉 차면 열어 둔 파일을 닫고 실패
				lock_acquire(&filesys_lock);
				file_close(file);
				lock_release(&filesys_lock);
			}
			f->R.rax = new_fd;

			break;
		}
		case SYS_FILESIZE: {
			struct file *file = fd_get(fd); // 실제 파일 조회는 이후 open 구현 후 fd 관리 코드와 연결

			if (file == NULL) {	// fd에 연결된 파일 없으면 실패(-1) 반환
				f->R.rax = -1;
				break;
			}

			// 지금은 길이가 안 바뀌어 락 없이도 안전하지만, 락 규칙을 예외 없이 지키고 파일 확장(프로젝트 4)에 대비해 잡음
			lock_acquire(&filesys_lock);
			f->R.rax = file_length(file);
			lock_release(&filesys_lock);

			break;
		}
		case SYS_READ: {
			void * buffer = (void *)f->R.rsi;
			unsigned size = (unsigned)f->R.rdx;

			check_buffer(buffer, size);

			if (fd == 0) {
				uint8_t *buf = buffer;
				for (unsigned i = 0; i < size; i++) {
					buf[i] = input_getc(); // 키보드에서 한 글자씩 읽어 버퍼에 저장
				}
				f->R.rax = size;
			}
			else if(fd == 1) {
				f->R.rax = -1;
			}
			else {
				struct file* fd_file =fd_get(fd);
				if (fd_file == NULL) {
					f->R.rax = -1;
					break;
				}

				// 락 안에서는 커널 버퍼에만 읽고, 유저 버퍼로의 복사는 락 밖에서 함
				// 단점: 청크마다 락을 다시 잡고, 시작 위치가 섹터 경계가 아니면 경계 섹터를 두 번 읽음
				char kbuf[IO_CHUNK];
				unsigned done = 0;
				while (done < size) {
					unsigned n = size - done < sizeof kbuf ? size - done : sizeof kbuf;

					lock_acquire(&filesys_lock);
					int got = file_read(fd_file, kbuf, n);
					lock_release(&filesys_lock);

					if (got <= 0) {
						break;
					}
					memcpy((char *)buffer + done, kbuf, got);
					done += got;
					if (got < (int)n) {	// 파일 끝에서 짧게 읽힘
						break;
					}
				}
				f->R.rax = done; // 실제로 읽은 바이트 수 반환
			}

			break;
		}
		case SYS_WRITE: {
			const void *buffer = (const void *)f->R.rsi;
			unsigned size = (unsigned)f->R.rdx;

			check_buffer(buffer, size);

			if (fd == 1) {
				putbuf(buffer, size);
				f->R.rax = size;
			} 
			else {
				struct file* fd_file =fd_get(fd);
				if (fd_file == NULL) {	// fd 0이나 잘못된 fd면 실패(-1), exit 안 함
					f->R.rax = -1;
					break;
				}
				// 유저 버퍼는 락 밖에서 커널 버퍼로 복사해 두고, 락 안에서는 커널 버퍼만 씀
				char kbuf[IO_CHUNK];
				unsigned done = 0;
				while (done < size) {
					unsigned n = size - done < sizeof kbuf ? size - done : sizeof kbuf;
					memcpy(kbuf, (const char *)buffer + done, n);

					lock_acquire(&filesys_lock);
					int put = file_write(fd_file, kbuf, n);
					lock_release(&filesys_lock);

					done += put;
					if (put < (int)n) {	// 파일 끝이거나 쓰기가 막혀 더 못 씀
						break;
					}
				}
				f->R.rax = done; // 실제로 쓴 바이트 수 반환
			}
			break;
		}
		case SYS_SEEK: {
			unsigned position = (unsigned)f->R.rsi;
			struct file *file = fd_get(fd);

			if (file == NULL) {	// 잘못된 fd면 아무것도 안 함
				break;
			}
			// off_t(int32)로 바꾸면 음수가 되어 file_seek의 ASSERT에서 커널 패닉이 나므로 무시
			if (position > INT32_MAX) {	
				break;
			}

			lock_acquire(&filesys_lock);
			file_seek(file, position);
			lock_release(&filesys_lock);

			break;
		}
		case SYS_TELL: {
			struct file *file = fd_get(fd);

			if (file == NULL) {
				f->R.rax = -1;
				break;
			}

			lock_acquire(&filesys_lock);
			f->R.rax = file_tell(file);
			lock_release(&filesys_lock);

			break;
		}
		case SYS_CLOSE:
			fd_close(fd);	// 잘못된 fd나 이미 닫은 fd는 그냥 무시
			break;
		case SYS_DUP2:
			break;
		default:
			break;
	}
}
