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

void syscall_entry (void);
void syscall_handler (struct intr_frame *);

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

void
syscall_init (void) {
	write_msr(MSR_STAR, ((uint64_t)SEL_UCSEG - 0x10) << 48  |
			((uint64_t)SEL_KCSEG) << 32);
	write_msr(MSR_LSTAR, (uint64_t) syscall_entry);

	/* 인터럽트 서비스 루틴(interrupt service routine)은 syscall_entry가
	 * 유저랜드 스택을 커널 모드 스택으로 바꿀 때까지 어떤 인터럽트도
	 * 처리하면 안 됨. 따라서 FLAG_FL을 마스크함. */
	write_msr(MSR_SYSCALL_MASK,
			FLAG_IF | FLAG_TF | FLAG_DF | FLAG_IOPL | FLAG_AC | FLAG_NT);
}
//putbuf (const char *buffer, size_t n)
/* 시스템 콜의 메인 인터페이스 */
void
syscall_handler (struct intr_frame *f UNUSED) {
	// TODO: 여기에 구현을 작성할 것.
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
			break;
		case SYS_CREATE:
			break;
		case SYS_REMOVE:
			break;
		case SYS_OPEN:
			break;
		case SYS_FILESIZE:
			break;
		case SYS_READ:
			break;
		case SYS_WRITE:
			if (fd == 1) {
				putbuf(f->R.rsi, f->R.rdx);
			}
			break;
		case SYS_SEEK:
			break;
		case SYS_TELL:
			break;
		case SYS_CLOSE:
			break;
		case SYS_DUP2:
			break;
		default:
			break;
	}
}
