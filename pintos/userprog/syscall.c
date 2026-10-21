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

/* 임시로 주소 확인 */
void check_address(const void *addr) {
	if (addr == NULL || !is_user_vaddr(addr) || pml4_get_page(thread_current()->pml4, addr) == NULL) {
		sys_exit(-1);
	}
}

/**
 * @brief 유저 버퍼 [buf, buf + size)가 걸친 모든 페이지를 검사함
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
 * @param kdst 복사해 둘 커널 버퍼
 * @param usrc 유저가 넘긴 문자열
 * @param max kdst 크기 (NUL 포함)
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
 * @details calloc으로 FD_MAX칸(64칸 = 512바이트)을 받아 전부 NULL인 상태로 시작함.
 *
 * @note 할당에 실패하면 fd_table은 NULL로 남고, 이후 fd_add()가 -1을 반환함.
 */
void fd_table_create(void) {
	thread_current()->fd_table = calloc(FD_MAX, sizeof(struct file *));
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
	free(cur->fd_table);
	cur->fd_table = NULL;
}



/* 시스템 콜의 메인 인터페이스 */
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
