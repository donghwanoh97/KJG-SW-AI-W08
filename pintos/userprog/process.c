#include "userprog/process.h"
#include <debug.h>
#include <inttypes.h>
#include <round.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "userprog/gdt.h"
#include "userprog/tss.h"
#include "filesys/directory.h"
#include "filesys/file.h"
#include "filesys/filesys.h"
#include "threads/flags.h"
#include "threads/init.h"
#include "threads/interrupt.h"
#include "threads/palloc.h"
#include "threads/thread.h"
#include "threads/mmu.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#include "threads/synch.h"
#include "userprog/syscall.h"

#ifdef VM
#include "vm/vm.h"
#endif

#include "devices/timer.h"
#define MAX_ARGS 64

#define ALIGN sizeof(double)

static void process_cleanup (void);
static bool load (const char *file_name, struct intr_frame *if_);
static void initd (void *f_name);
static void __do_fork (void *);

/* initd 및 다른 프로세스를 위한 일반 프로세스 초기화 함수. */
static void
process_init (void) {
	struct thread *current = thread_current ();
}

/* FILE_NAME에서 로드한 첫 번째 유저랜드(userland) 프로그램 "initd"를
 * 시작함.
 * 새 스레드는 process_create_initd()가 반환되기 전에 스케줄될 수 있고
 * (심지어 종료될 수도 있음).  initd의 스레드 id를 반환하며, 스레드를
 * 생성할 수 없으면 TID_ERROR를 반환함.
 * 이 함수는 반드시 한 번만 호출되어야 함에 주의할 것. */
tid_t
process_create_initd (const char *file_name) {
	char *fn_copy;
	tid_t tid;

	/* FILE_NAME의 복사본을 만듦.
	 * 그렇지 않으면 호출자와 load() 사이에 경쟁 상태(race)가 생김. */
	fn_copy = palloc_get_page (0);
	if (fn_copy == NULL)
		return TID_ERROR;
	strlcpy (fn_copy, file_name, PGSIZE);

	/**
	 * strtok_r로 실행파일 이름용 복사본 만들기
	 * 실행과 인자 전달 사용할 전체 명렬줄 fn_copy는 유지
	 * 현재 구현은 스레드 이름 크기에 맞춰 최대 15문자만 보관중(임시)
	 */
	char name_copy[16];
	char *save_ptr;

	strlcpy(name_copy, file_name, sizeof name_copy);
	char *program_name = strtok_r(name_copy, " ", &save_ptr);	/* 명령줄 첫 단어를 최초 프로세스 이름으로 */

	if (program_name == NULL) {		/* 이름을 얻지 못했다면 실행에 사용할 복사본을 해제하고 실패처리 */
		palloc_free_page(fn_copy);
		return TID_ERROR;
	}
	

	/* FILE_NAME을 실행할 새 스레드를 생성함. */
	tid = thread_create (program_name, PRI_DEFAULT, initd, fn_copy);
	if (tid == TID_ERROR)
		palloc_free_page (fn_copy);
	return tid;
}

/* 첫 번째 유저 프로세스를 실행하는 스레드 함수. */
static void
initd (void *f_name) {
#ifdef VM
	supplemental_page_table_init (&thread_current ()->spt);
#endif

	process_init ();
	fd_table_create ();

	if (process_exec (f_name) < 0)
		PANIC("Fail to launch initd\n");
	NOT_REACHED ();
}

/* 현재 프로세스를 `name`이라는 이름으로 복제함. 새 프로세스의 스레드 id를
 * 반환하며, 스레드를 생성할 수 없으면 TID_ERROR를 반환함. */
tid_t
process_fork (const char *name, struct intr_frame *if_) {
	/* 현재 스레드를 새 스레드로 복제함. */
	struct thread *t = thread_current();
	t->fork_if = if_;
	tid_t tid = thread_create (name, PRI_DEFAULT, __do_fork, t);
	
	struct list_elem *e = list_begin(&t->children);

	while (e != list_end(&t->children)) {
		struct child_info *info = list_entry(e, struct child_info, elem);
		if (info->tid == tid) {
			sema_down(&info->fork_sema);
			if (!info->fork_success) {
				return TID_ERROR;
			}
			break;
		}
		e = list_next(e);		
	}
	return tid;
}

#ifndef VM
/* 이 함수를 pml4_for_each에 넘겨서 부모의 주소 공간을 복제함.
 * project 2 전용임. */
static bool
duplicate_pte (uint64_t *pte, void *va, void *aux) {
	struct thread *current = thread_current ();
	struct thread *parent = (struct thread *) aux;
	void *parent_page;
	void *newpage;
	bool writable;

	/* 1. TODO: parent_page가 커널 페이지이면 즉시 반환할 것. */
	if (is_kernel_vaddr(va)) return true;

	/* 2. 부모의 페이지 맵 레벨 4(page map level 4)에서 VA를 찾음. */
	parent_page = pml4_get_page(parent->pml4, va);


	/* 3. TODO: 자식을 위한 새 PAL_USER 페이지를 할당하고 결과를
	 *    TODO: NEWPAGE에 저장할 것. */
	newpage = palloc_get_page(PAL_USER);
	if(newpage == NULL) return false;
	
	/* 4. TODO: 부모의 페이지를 새 페이지로 복제하고,
	 *    TODO: 부모의 페이지가 쓰기 가능한지 확인할 것 (그 결과에 따라
	 *    TODO: WRITABLE을 설정). */
	memcpy(newpage, parent_page, PGSIZE);

	writable = is_writable(pte);

	/* 5. 새 페이지를 WRITABLE 권한으로 자식의 페이지 테이블의
	 *    주소 VA에 추가함. */
	if (!pml4_set_page (current->pml4, va, newpage, writable)) {
		/* 6. TODO: 페이지 삽입에 실패하면 오류 처리를 할 것. */
		palloc_free_page(newpage);
		return false;
	}
	return true;
}
#endif

/* 부모의 실행 컨텍스트(execution context)를 복사하는 스레드 함수.
 * 힌트) parent->tf는 프로세스의 유저랜드 컨텍스트를 가지고 있지 않음.
 *       즉, process_fork의 두 번째 인자를 이 함수에 넘겨줘야 함. */
static void
__do_fork (void *aux) {
	struct intr_frame if_;
	struct thread *parent = (struct thread *) aux;
	struct thread *current = thread_current ();
	/* TODO: 어떻게든 parent_if를 넘겨줄 것. (즉, process_fork()의 if_) */
	struct intr_frame *parent_if = parent->fork_if;
	bool succ = true;

	/* 1. CPU 컨텍스트를 로컬 스택으로 읽어 옴. */
	memcpy (&if_, parent_if, sizeof (struct intr_frame));

	/* 2. 페이지 테이블(PT)을 복제함 */
	current->pml4 = pml4_create();
	if (current->pml4 == NULL)
		goto error;

	process_activate (current);
#ifdef VM
	supplemental_page_table_init (&current->spt);
	if (!supplemental_page_table_copy (&current->spt, &parent->spt))
		goto error;
#else
	if (!pml4_for_each (parent->pml4, duplicate_pte, parent))
		goto error;
#endif

	/* TODO: 여기에 코드를 작성할 것.
	 * TODO: 힌트) 파일 객체를 복제하려면 include/filesys/file.h의
	 * TODO:       `file_duplicate`를 사용할 것. 이 함수가 부모의 리소스를
	 * TODO:       성공적으로 복제하기 전까지는 부모가 fork()에서 반환하면
	 * TODO:       안 된다는 점에 주의할 것. */
	fd_table_create();
	for (int i = 2; i < FD_MAX; i++) {
		if(!parent->fd_table[i]) continue;

		struct file *f = file_duplicate(parent->fd_table[i]);
		if(f == NULL) { goto error; }
		current->fd_table[i] = f;
	}
	

	process_init ();

	/* 마지막으로 새로 생성된 프로세스로 전환함. */
	if (succ) {
		current->self_info->fork_success = true;
		sema_up(&current->self_info->fork_sema);
		if_.R.rax = 0;
		do_iret (&if_);
	}
error:
	current->self_info->fork_success = false;
	sema_up(&current->self_info->fork_sema);
	thread_exit ();
}

/* 현재 실행 컨텍스트를 f_name으로 전환함.
 * 실패하면 -1을 반환함. */
int
process_exec (void *f_name) {
	char *file_name = f_name;
	bool success;

	/* thread 구조체 안의 intr_frame은 사용할 수 없음.
	 * 현재 스레드가 다시 스케줄될 때 그 멤버에
	 * 실행 정보를 저장하기 때문임. */
	struct intr_frame _if;
	_if.ds = _if.es = _if.ss = SEL_UDSEG;
	_if.cs = SEL_UCSEG;
	_if.eflags = FLAG_IF | FLAG_MBS;

	/* 먼저 현재 컨텍스트를 제거함 */
	process_cleanup();

	/* 그런 다음 바이너리를 로드함 */

	success = load(file_name, &_if);

	/* 로드에 실패하면 종료함. */
	palloc_free_page (file_name);
	if (!success)
		return -1;
	
	/* 전환된 프로세스를 시작함. */
	do_iret (&_if);
	NOT_REACHED ();
}


/* 스레드 TID가 종료되기를 기다렸다가 그 종료 상태(exit status)를
 * 반환함.  커널에 의해 종료된 경우(즉, 예외로 인해 kill된 경우)
 * -1을 반환함.  TID가 유효하지 않거나, 호출한 프로세스의 자식이
 * 아니거나, 주어진 TID에 대해 process_wait()가 이미 성공적으로
 * 호출된 적이 있으면, 기다리지 않고 즉시 -1을 반환함.
 *
 * 이 함수는 problem 2-2에서 구현할 것임.  지금은 아무 일도
 * 하지 않음. */
int
process_wait (tid_t child_tid) {
	/* XXX: 힌트) process_wait (initd)를 하면 pintos가 종료되므로,
	 * XXX:       process_wait를 구현하기 전에 여기에 무한 루프를
	 * XXX:       추가해 두기를 권장함. */
	
	struct thread *curr = thread_current();
	struct list_elem *e = list_begin(&curr->children);
	
	/* chilredn 리스트를 순회하며 대상 child_info를 검색 */
	while (e != list_end(&curr->children)) {
		struct child_info *info = list_entry(e, struct child_info, elem);
		if (info->tid == child_tid) {
			sema_down(&info->wait_sema);
			
			/* 자식이 종료된 이후에 실행되는 코드 */
			int exit_status = info->exit_status;
			list_remove(&info->elem); // 이미 종료된 pid에 대한 wait 방지
			free(info);
			return exit_status; // todo: 커널에 의해 죽었을 때 exit(-1) 처리 확인 필요
		}
		e = list_next(e);
	}

	return -1; // 직계자식이 아닌 경우
}

/* 프로세스를 종료함. 이 함수는 thread_exit()가 호출함. */
void
process_exit (void) {
	struct thread *curr = thread_current ();
	bool user_process = curr->pml4 != NULL;
	int status = -1;

	if (curr->exit_info != NULL)
		status = curr->exit_info->status;
	else if (curr->self_info != NULL)
		status = curr->self_info->exit_status;

	/* 메시지는 이곳에서만 출력하며 커널 스레드에는 출력하지 않음. */
	if (user_process)
		printf ("%s: exit(%d)\n", curr->name, status);
	free (curr->exit_info);
	curr->exit_info = NULL;

	/* 부모가 wait에서 깨어나기 전에 열린 파일과 주소 공간을 정리함. */
	fd_table_close_all();
	process_cleanup ();

	/* 부모 프로세스로서의 마무리 */
	if (!list_empty(&curr->children)) {
		// children 순회
		struct list_elem *e = list_begin(&curr->children);
		while (e != list_end(&curr->children)) {
			struct child_info *info = list_entry(e, struct child_info, elem);
			bool should_free;

			enum intr_level old_level = intr_disable();
			should_free = info->child_exited; // 자식이 이미 종료한 경우 1, 자식이 아직 종료되지 않은 경우 0

			if (!should_free) {
				info->parent_exited = true;
			}
			e = list_remove(e); // children에서 제거한 뒤 다음 노트 탐색
			intr_set_level(old_level);

			if (should_free) {
				free(info);
			}
		}
	}

	/* 자식 프로세스로서의 마무리 */
	if (curr->self_info != NULL) {
		struct child_info *info = curr->self_info;
		curr->self_info = NULL;

		/* 부모 종료와 상태 게시가 엇갈리지 않도록 함께 보호함. */
		enum intr_level old_level = intr_disable ();
		bool parent_exited = info->parent_exited;
		info->exit_status = status;
		info->child_exited = true;
		if (!parent_exited)
			sema_up (&info->wait_sema);
		/* sema_up 이후 부모가 info를 해제할 수 있으므로 다시 접근하지 않음. */
		intr_set_level (old_level);

		if (parent_exited)
			free (info);
	}

	file_close(curr->using_file);
}

/* 현재 프로세스의 리소스를 해제함. */
static void
process_cleanup (void) {
	struct thread *curr = thread_current ();

#ifdef VM
	supplemental_page_table_kill (&curr->spt);
#endif

	uint64_t *pml4;
	/* 현재 프로세스의 페이지 디렉터리(page directory)를 파괴하고
	 * 커널 전용 페이지 디렉터리로 되돌아감. */
	pml4 = curr->pml4;
	if (pml4 != NULL) {
		/* 여기서 순서를 올바르게 지키는 것이 중요함.  타이머 인터럽트가
		 * 프로세스의 페이지 디렉터리로 다시 전환하지 못하도록, 페이지
		 * 디렉터리를 전환하기 전에 cur->pagedir를 NULL로 설정해야 함.
		 * 또 프로세스의 페이지 디렉터리를 파괴하기 전에 기본(base) 페이지
		 * 디렉터리를 활성화해야 함.  그렇지 않으면 활성 페이지 디렉터리가
		 * 이미 해제된(그리고 비워진) 것이 되어 버림. */
		curr->pml4 = NULL;
		pml4_activate (NULL);
		pml4_destroy (pml4);
	}
}

/* 다음(next) 스레드에서 유저 코드를 실행할 수 있도록 CPU를 설정함.
 * 이 함수는 매 컨텍스트 스위치(context switch)마다 호출됨. */
void
process_activate (struct thread *next) {
	/* 스레드의 페이지 테이블을 활성화함. */
	pml4_activate (next->pml4);

	/* 인터럽트 처리에 사용할 스레드의 커널 스택을 설정함. */
	tss_update (next);
}

/* ELF 바이너리를 로드함.  아래 정의들은 ELF 명세 [ELF1]에서
 * 거의 그대로 가져온 것임.  */

/* ELF 타입(types).  [ELF1] 1-2 참고. */
#define EI_NIDENT 16

#define PT_NULL    0            /* 무시. */
#define PT_LOAD    1            /* 로드 가능한 세그먼트. */
#define PT_DYNAMIC 2            /* 동적 링킹(dynamic linking) 정보. */
#define PT_INTERP  3            /* 동적 로더(dynamic loader)의 이름. */
#define PT_NOTE    4            /* 보조 정보. */
#define PT_SHLIB   5            /* 예약됨. */
#define PT_PHDR    6            /* 프로그램 헤더 테이블(Program header table). */
#define PT_STACK   0x6474e551   /* 스택 세그먼트. */

#define PF_X 1          /* 실행 가능. */
#define PF_W 2          /* 쓰기 가능. */
#define PF_R 4          /* 읽기 가능. */

/* 실행 파일 헤더(Executable header).  [ELF1] 1-4부터 1-8까지 참고.
 * ELF 바이너리의 맨 앞에 나타남. */
struct ELF64_hdr {
	unsigned char e_ident[EI_NIDENT];
	uint16_t e_type;
	uint16_t e_machine;
	uint32_t e_version;
	uint64_t e_entry;
	uint64_t e_phoff;
	uint64_t e_shoff;
	uint32_t e_flags;
	uint16_t e_ehsize;
	uint16_t e_phentsize;
	uint16_t e_phnum;
	uint16_t e_shentsize;
	uint16_t e_shnum;
	uint16_t e_shstrndx;
};

struct ELF64_PHDR {
	uint32_t p_type;
	uint32_t p_flags;
	uint64_t p_offset;
	uint64_t p_vaddr;
	uint64_t p_paddr;
	uint64_t p_filesz;
	uint64_t p_memsz;
	uint64_t p_align;
};

/* 약어 */
#define ELF ELF64_hdr
#define Phdr ELF64_PHDR

static bool setup_stack (struct intr_frame *if_);
static bool validate_segment (const struct Phdr *, struct file *);
static bool load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes,
		bool writable);

/* FILE_NAME의 ELF 실행 파일을 현재 스레드에 로드함.
 * 실행 파일의 진입점(entry point)을 *RIP에,
 * 초기 스택 포인터를 *RSP에 저장함.
 * 성공하면 true를, 그렇지 않으면 false를 반환함. */
static bool
load (const char *file_name, struct intr_frame *if_) {
	struct thread *t = thread_current ();
	struct ELF ehdr;
	struct file *file = NULL;
	off_t file_ofs;
	bool success = false;
	int i;

	/* 페이지 디렉터리를 할당하고 활성화함. */
	t->pml4 = pml4_create ();
	if (t->pml4 == NULL)
		goto done;
	process_activate (thread_current ());
	
	char *argv[MAX_ARGS];
	int argc = 0;

	char *token, *save_ptr;
	for (token = strtok_r (file_name, " ", &save_ptr); token != NULL && argc < MAX_ARGS-1; token = strtok_r (NULL, " ", &save_ptr)) {
		argv[argc++] = token;
	}
	argv[argc] = NULL;

	if (argc <= 0 || argc > MAX_ARGS) {
		goto done;
	}

	/* 실행 파일을 엶. */
	file = filesys_open (argv[0]);
	if (file == NULL) {
		printf ("load: %s: open failed\n", argv[0]);
		goto done;
	}
	file_deny_write(file);
	t->using_file = file;

	/* 실행 파일 헤더를 읽고 검증함. */
	if (file_read (file, &ehdr, sizeof ehdr) != sizeof ehdr
			|| memcmp (ehdr.e_ident, "\177ELF\2\1\1", 7)
			|| ehdr.e_type != 2
			|| ehdr.e_machine != 0x3E // amd64
			|| ehdr.e_version != 1
			|| ehdr.e_phentsize != sizeof (struct Phdr)
			|| ehdr.e_phnum > 1024) {
		printf ("load: %s: error loading executable\n", argv[0]);
		goto done;
	}

	/* 프로그램 헤더를 읽음. */
	file_ofs = ehdr.e_phoff;
	for (i = 0; i < ehdr.e_phnum; i++) {
		struct Phdr phdr;

		if (file_ofs < 0 || file_ofs > file_length (file))
			goto done;
		file_seek (file, file_ofs);

		if (file_read (file, &phdr, sizeof phdr) != sizeof phdr)
			goto done;
		file_ofs += sizeof phdr;
		switch (phdr.p_type) {
			case PT_NULL:
			case PT_NOTE:
			case PT_PHDR:
			case PT_STACK:
			default:
				/* 이 세그먼트를 무시함. */
				break;
			case PT_DYNAMIC:
			case PT_INTERP:
			case PT_SHLIB:
				goto done;
			case PT_LOAD:
				if (validate_segment (&phdr, file)) {
					bool writable = (phdr.p_flags & PF_W) != 0;
					uint64_t file_page = phdr.p_offset & ~PGMASK;
					uint64_t mem_page = phdr.p_vaddr & ~PGMASK;
					uint64_t page_offset = phdr.p_vaddr & PGMASK;
					uint32_t read_bytes, zero_bytes;
					if (phdr.p_filesz > 0) {
						/* 일반 세그먼트.
						 * 앞부분은 디스크에서 읽고 나머지는 0으로 채움. */
						read_bytes = page_offset + phdr.p_filesz;
						zero_bytes = (ROUND_UP (page_offset + phdr.p_memsz, PGSIZE)
								- read_bytes);
					} else {
						/* 전부 0임.
						 * 디스크에서 아무것도 읽지 않음. */
						read_bytes = 0;
						zero_bytes = ROUND_UP (page_offset + phdr.p_memsz, PGSIZE);
					}
					if (!load_segment (file, file_page, (void *) mem_page,
								read_bytes, zero_bytes, writable))
						goto done;
				}
				else
					goto done;
				break;
		}
	}

	/* 스택을 설정함. */
	if (!setup_stack(if_))
		goto done;

	/* 시작 주소. */
	if_->rip = ehdr.e_entry;

	uint8_t *ptr = (uint8_t *) if_->rsp;

	for (int i = argc-1; i >= 0; i--) {
		size_t len = strlen(argv[i]) + 1;
		ptr -= len;
		memcpy(ptr, argv[i], len);
		argv[i] = (char *)ptr; // 커널 메모리를 스택 메모리로 변경
	}

	// 8의 배수의 주소로 내림
	ptr = (void *) ((uintptr_t) ptr & ~(uintptr_t) 7);

	ptr -= (argc + 1) * sizeof(char *);
	memcpy(ptr, argv, (argc + 1) * sizeof(char *));

    uint8_t *argv_addr = ptr;
    ptr -= sizeof(void *);
    memset(ptr, 0, sizeof(void *));

    if_->R.rdi = argc;
    if_->R.rsi = (uint64_t) argv_addr;
    if_->rsp = (uintptr_t) ptr;

	success = true;

done:
	/* 로드가 성공하든 실패하든 여기에 도달함. */
	return success;
}


/* PHDR이 FILE 안의 유효하고 로드 가능한 세그먼트를 기술하는지
 * 확인하여, 그렇다면 true를, 아니면 false를 반환함. */
static bool
validate_segment (const struct Phdr *phdr, struct file *file) {
	/* p_offset과 p_vaddr는 페이지 오프셋이 같아야 함. */
	if ((phdr->p_offset & PGMASK) != (phdr->p_vaddr & PGMASK))
		return false;

	/* p_offset은 FILE 내부를 가리켜야 함. */
	if (phdr->p_offset > (uint64_t) file_length (file))
		return false;

	/* p_memsz는 최소한 p_filesz만큼은 커야 함. */
	if (phdr->p_memsz < phdr->p_filesz)
		return false;

	/* 세그먼트가 비어 있으면 안 됨. */
	if (phdr->p_memsz == 0)
		return false;

	/* 가상 메모리 영역은 시작과 끝이 모두 유저 주소 공간
	   범위 안에 있어야 함. */
	if (!is_user_vaddr ((void *) phdr->p_vaddr))
		return false;
	if (!is_user_vaddr ((void *) (phdr->p_vaddr + phdr->p_memsz)))
		return false;

	/* 이 영역은 커널 가상 주소 공간을 가로질러 "한 바퀴 돌아서"
	   (wrap around) 이어질 수 없음. */
	if (phdr->p_vaddr + phdr->p_memsz < phdr->p_vaddr)
		return false;

	/* 페이지 0의 매핑을 허용하지 않음.
	   페이지 0을 매핑하는 것 자체가 좋지 않은 방식일 뿐 아니라, 이를
	   허용하면 시스템 콜(system call)에 널 포인터를 넘기는 유저 코드가
	   memcpy() 등의 널 포인터 assertion을 통해 커널을 패닉(panic)
	   시킬 가능성이 꽤 높음. */
	if (phdr->p_vaddr < PGSIZE)
		return false;

	/* 문제 없음. */
	return true;
}

#ifndef VM
/* 이 블록의 코드는 project 2 동안에만 사용됨.
 * project 2 전체에 걸쳐 쓸 함수를 구현하려면 #ifndef 매크로
 * 바깥에 구현할 것. */

/* load() 도우미 함수들. */
static bool install_page (void *upage, void *kpage, bool writable);

/* FILE의 오프셋 OFS에서 시작하는 세그먼트를 주소 UPAGE에 로드함.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 다음과 같이
 * 초기화됨.
 *
 * - UPAGE의 READ_BYTES 바이트는 FILE의 오프셋 OFS부터 읽어 와야 함.
 *
 * - UPAGE + READ_BYTES의 ZERO_BYTES 바이트는 0으로 채워야 함.
 *
 * 이 함수로 초기화한 페이지는 WRITABLE이 true이면 유저 프로세스가
 * 쓸 수 있어야 하고, 아니면 읽기 전용이어야 함.
 *
 * 성공하면 true를, 메모리 할당 오류나 디스크 읽기 오류가 발생하면
 * false를 반환함. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	file_seek (file, ofs);
	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산함.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고
		 * 마지막 PAGE_ZERO_BYTES 바이트는 0으로 채울 것임. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* 메모리 페이지 하나를 가져옴. */
		uint8_t *kpage = palloc_get_page (PAL_USER);
		if (kpage == NULL)
			return false;

		/* 이 페이지를 로드함. */
		if (file_read (file, kpage, page_read_bytes) != (int) page_read_bytes) {
			palloc_free_page (kpage);
			return false;
		}
		memset (kpage + page_read_bytes, 0, page_zero_bytes);

		/* 페이지를 프로세스의 주소 공간에 추가함. */
		if (!install_page (upage, kpage, writable)) {
			printf("fail\n");
			palloc_free_page (kpage);
			return false;
		}

		/* 다음으로 넘어감. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK 위치에 0으로 채운 페이지를 매핑하여 최소한의 스택을 생성함 */
static bool
setup_stack (struct intr_frame *if_) {
	uint8_t *kpage;
	bool success = false;

	kpage = palloc_get_page (PAL_USER | PAL_ZERO);
	if (kpage != NULL) {
		success = install_page (((uint8_t *) USER_STACK) - PGSIZE, kpage, true);
		if (success)
			if_->rsp = USER_STACK;
		else
			palloc_free_page (kpage);
	}
	return success;
}

/* 유저 가상 주소 UPAGE에서 커널 가상 주소 KPAGE로의 매핑을
 * 페이지 테이블에 추가함.
 * WRITABLE이 true이면 유저 프로세스가 페이지를 수정할 수 있고,
 * 아니면 읽기 전용임.
 * UPAGE는 이미 매핑되어 있으면 안 됨.
 * KPAGE는 아마도 palloc_get_page()로 유저 풀(user pool)에서
 * 얻은 페이지여야 함.
 * 성공하면 true를, UPAGE가 이미 매핑되어 있거나 메모리 할당에
 * 실패하면 false를 반환함. */
static bool
install_page (void *upage, void *kpage, bool writable) {
	struct thread *t = thread_current ();

	/* 그 가상 주소에 이미 페이지가 없는지 확인한 다음,
	 * 우리 페이지를 거기에 매핑함. */
	return (pml4_get_page (t->pml4, upage) == NULL
			&& pml4_set_page (t->pml4, upage, kpage, writable));
}
#else
/* 여기서부터의 코드는 project 3 이후에 사용됨.
 * project 2 전용 함수를 구현하려면 위쪽 블록에 구현할 것. */

static bool
lazy_load_segment (struct page *page, void *aux) {
	/* TODO: 파일에서 세그먼트를 로드할 것 */
	/* TODO: 주소 VA에서 첫 페이지 폴트(page fault)가 발생하면 이 함수가 호출됨. */
	/* TODO: 이 함수를 호출할 때 VA를 사용할 수 있음. */
}

/* FILE의 오프셋 OFS에서 시작하는 세그먼트를 주소 UPAGE에 로드함.
 * 총 READ_BYTES + ZERO_BYTES 바이트의 가상 메모리가 다음과 같이
 * 초기화됨.
 *
 * - UPAGE의 READ_BYTES 바이트는 FILE의 오프셋 OFS부터 읽어 와야 함.
 *
 * - UPAGE + READ_BYTES의 ZERO_BYTES 바이트는 0으로 채워야 함.
 *
 * 이 함수로 초기화한 페이지는 WRITABLE이 true이면 유저 프로세스가
 * 쓸 수 있어야 하고, 아니면 읽기 전용이어야 함.
 *
 * 성공하면 true를, 메모리 할당 오류나 디스크 읽기 오류가 발생하면
 * false를 반환함. */
static bool
load_segment (struct file *file, off_t ofs, uint8_t *upage,
		uint32_t read_bytes, uint32_t zero_bytes, bool writable) {
	ASSERT ((read_bytes + zero_bytes) % PGSIZE == 0);
	ASSERT (pg_ofs (upage) == 0);
	ASSERT (ofs % PGSIZE == 0);

	while (read_bytes > 0 || zero_bytes > 0) {
		/* 이 페이지를 어떻게 채울지 계산함.
		 * FILE에서 PAGE_READ_BYTES 바이트를 읽고
		 * 마지막 PAGE_ZERO_BYTES 바이트는 0으로 채울 것임. */
		size_t page_read_bytes = read_bytes < PGSIZE ? read_bytes : PGSIZE;
		size_t page_zero_bytes = PGSIZE - page_read_bytes;

		/* TODO: lazy_load_segment에 정보를 넘기기 위해 aux를 설정할 것. */
		void *aux = NULL;
		if (!vm_alloc_page_with_initializer (VM_ANON, upage,
					writable, lazy_load_segment, aux))
			return false;

		/* 다음으로 넘어감. */
		read_bytes -= page_read_bytes;
		zero_bytes -= page_zero_bytes;
		upage += PGSIZE;
	}
	return true;
}

/* USER_STACK 위치에 스택 페이지(PAGE) 하나를 생성함. 성공하면 true를 반환함. */
static bool
setup_stack (struct intr_frame *if_) {
	bool success = false;
	void *stack_bottom = (void *) (((uint8_t *) USER_STACK) - PGSIZE);

	/* TODO: stack_bottom에 스택을 매핑하고 즉시 해당 페이지를 claim할 것.
	 * TODO: 성공하면 rsp를 그에 맞게 설정할 것.
	 * TODO: 해당 페이지가 스택임을 표시해야 함. */
	/* TODO: 여기에 코드를 작성할 것 */

	return success;
}
#endif /* VM */
