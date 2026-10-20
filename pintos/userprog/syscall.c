#include "userprog/syscall.h"
#include <stdio.h>
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

#define FD_MAX (PGSIZE / sizeof(struct file *))	 /* 페이지 한 장 = 512칸 */
struct lock filesys_lock;           /* 전역 파일 시스템 락 */

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

/*
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
fd_table 빨간줄 뜨는거 정상입니다.
@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@@
*/

/* 임시로 주소 확인 */
void check_address(const void *addr) {
	if (addr == NULL || !is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL) {
		thread_exit(); // 임시
	}
}

/**
 * @brief 현재 스레드의 fd 테이블을 할당함
 *
 * @details palloc_get_page(PAL_ZERO)로 페이지 한 장을 받아 FD_MAX칸 전부 NULL인 상태로 시작함.
 *
 * @note 할당에 실패하면 fd_table은 NULL로 남고, 이후 fd_add()가 -1을 반환함.
 */
void fd_table_create(void) {
	thread_current()->fd_table = palloc_get_page(PAL_ZERO);
}

/**
 * @brief fd에 연결된 열린 파일을 찾음
 *
 * @param fd 유저가 넘긴 파일 디스크립터
 * @return 연결된 struct file *. 테이블이 없거나, fd가 2 미만·FD_MAX 이상이거나, 빈 칸이면 NULL
 *
 * @note 잘못된 fd여도 exit하지 않음. 범위 검사는 이 함수 한 곳에서만 함.
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
 * @param file 등록할 열린 파일
 * @return 배정된 fd (2번부터 비어 있는 가장 작은 번호). 테이블이 없거나 꽉 차면 -1
 *
 * @note 실패해도 file을 닫지 않음. 호출한 쪽이 닫아야 함.
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
 * @param fd 닫을 파일 디스크립터
 *
 * @note 잘못된 fd나 이미 닫은 fd면 아무것도 하지 않음.
 * @note file_close() 호출 부분만 filesys_lock을 잡음.
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
 * @brief 열린 파일을 모두 닫고 fd 테이블 페이지를 해제함
 *
 * @details 프로세스 종료 시 process_exit()에서 호출함.
 *
 * @note 테이블이 없으면(커널 스레드, fork 실패) 아무것도 하지 않음.
 */
void fd_table_close_all(void) {
	struct thread *cur = thread_current();

	if (cur->fd_table == NULL) {
		return;
	}
	for (int fd = 2; fd < (int)FD_MAX; fd++) { // 0,1은 예약이라고 가정
		fd_close(fd);
	}
	palloc_free_page(cur->fd_table);
	cur->fd_table = NULL;
}

/* 시스템 콜의 메인 인터페이스 */
void
syscall_handler (struct intr_frame *f UNUSED) {
	int fd = f->R.rdi;
	switch (f->R.rax)
	{
		case SYS_HALT:
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
			break;
		case SYS_EXEC:
			break;
		case SYS_WAIT:
			f->R.rax = process_wait(f->R.rdi);
			break;
		case SYS_CREATE: {
			const char *create_file_name = (const char *)f->R.rdi;
			unsigned initial_size = (unsigned)f->R.rsi;

			check_address(create_file_name);

			lock_acquire(&filesys_lock);
			f->R.rax = filesys_create(create_file_name, initial_size);
			lock_release(&filesys_lock);

			break;
		}
		case SYS_REMOVE: {
			const char *remove_file_name = (const char *)f->R.rdi;

			check_address(remove_file_name);

			lock_acquire(&filesys_lock);
			f->R.rax = filesys_remove(remove_file_name);
			lock_release(&filesys_lock);

			break;
		}
		case SYS_OPEN: {
			const char *open_file_name = (const char *)f->R.rdi;

			check_address(open_file_name);

			lock_acquire(&filesys_lock);
			struct file *file = filesys_open(open_file_name);
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
			int fd = f->R.rdi;
			struct file *file = fd_get(fd); // 실제 파일 조회는 이후 open 구현 후 fd 관리 코드와 연결

			if (file == NULL) {	// fd에 연결된 파일 없으면 실패(-1) 반환
				f->R.rax = -1;
				break;
			}

			lock_acquire(&filesys_lock);
			f->R.rax = file_length(file); // 파일 크기 조회해서 리턴
			lock_release(&filesys_lock);

			break;
		}
		case SYS_READ: {
			/*
			int
			read (int fd, void *buffer, unsigned size) {
				return syscall3 (SYS_READ, fd, buffer, size);
			}
			rax rdi rsi rdx
			*/
			void * buffer = (void *)f->R.rsi;
			unsigned size = (unsigned)f->R.rdx;

			check_address(buffer);

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

				lock_acquire(&filesys_lock);
				f->R.rax = file_read(fd_file, buffer, size); // 실제로 읽은 바이트 수 반환
				lock_release(&filesys_lock);
			}

			break;
		}
		case SYS_WRITE: {
			/*
			int
			write (int fd, const void *buffer, unsigned size) {
				return syscall3 (SYS_WRITE, fd, buffer, size);
			}
			rax rdi rsi rdx
			*/
			const void *buffer = (const void *)f->R.rsi;
			unsigned size = (unsigned)f->R.rdx;

			check_address(buffer);

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
				lock_acquire(&filesys_lock);
				f->R.rax = file_write(fd_file, buffer, size); // 실제로 쓴 바이트 수 반환
				lock_release(&filesys_lock);
			}
			break;
		}
		case SYS_SEEK:
		/*
		void
		seek (int fd, unsigned position) {
			syscall2 (SYS_SEEK, fd, position);
		}
		rax rdi 
		*/
			unsigned position = f->R.rdi;

			break;
		case SYS_TELL: {
			int fd = f->R.rdi;
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
