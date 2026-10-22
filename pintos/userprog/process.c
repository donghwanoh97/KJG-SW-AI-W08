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

/**
 * @brief 명령줄 FILE_NAME으로 첫 번째 유저 프로그램 "initd"를 시작함
 *
 * @details
 * 명령줄을 두 벌로 나눠 씀.
 * - fn_copy: 명령줄 전체를 담은 palloc 페이지. initd → process_exec() → load()로 넘어가
 *   인자 전달에 쓰이고 process_exec()가 해제함. 호출자의 file_name은 이 함수가 반환되면
 *   사라질 수 있어서(경쟁 상태) 페이지로 복사해 둠
 * - name_copy: 스레드 이름용. 종료 메시지가 "args-single: exit(0)"처럼 프로그램 이름만
 *   찍어야 하므로, 명령줄의 첫 단어만 잘라 thread_create()의 이름으로 씀
 *
 * name_copy를 따로 둔 이유는 strtok_r가 원본 문자열에 NUL을 써 넣기 때문임.
 * fn_copy를 자르면 load()가 인자를 못 읽게 됨.
 *
 * @param[in] file_name 커널 명령줄에서 받은 실행 명령 (예: "args-single onearg")
 * @return initd의 tid. 페이지 할당, 이름 추출, 스레드 생성 중 하나라도 실패하면 TID_ERROR
 *
 * @note 새 스레드는 이 함수가 반환되기 전에 스케줄될 수 있고, 종료될 수도 있음
 * @note 반드시 한 번만 호출해야 함 (threads/init.c의 run_task())
 * @note name_copy는 struct thread의 name 크기(16)에 맞춰 15글자까지만 담음
 */
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

	/* strtok_r로 실행 파일 이름용 복사본을 만듦.
	 * 실행과 인자 전달에 쓸 전체 명령줄 fn_copy는 그대로 유지함.
	 * 현재 구현은 스레드 이름 크기에 맞춰 최대 15문자만 보관함(임시). */
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

/**
 * @brief 첫 번째 유저 프로세스를 실행하는 스레드 함수
 *
 * @details
 * fd 테이블은 유저 프로세스만 가지므로 thread_create()가 아니라 여기서 만듦.
 * 다른 유저 프로세스는 모두 fork로 생기고 __do_fork()에서 테이블을 만들기 때문에,
 * fd_table_create() 호출 지점은 이곳과 __do_fork() 두 곳뿐임.
 *
 * @param[in] f_name process_create_initd()가 만든 명령줄 페이지 (소유권 넘겨받음)
 *
 * @note 반환하지 않음. 로드에 실패하면 실행할 유저 프로그램이 없으므로 PANIC
 */
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

/**
 * @brief 현재 프로세스를 NAME이라는 이름의 자식 프로세스로 복제함
 *
 * @details
 * 자식이 복제할 유저 컨텍스트(유저 레지스터)는 parent->tf에 없음. tf는 커널 스레드끼리
 * 전환할 때 쓰는 프레임이라 시스템 콜에 들어온 시점의 유저 레지스터가 아니기 때문임.
 * 그래서 시스템 콜 진입 때 받은 IF_ 주소를 fork_if에 남기고, 자식 스레드에 부모 포인터를
 * aux로 넘겨 __do_fork()가 읽어 가게 함.
 *
 * 그다음 자식의 fork_sema에서 기다림. 이유는 두 가지임.
 * - fork()의 반환값을 정하려면 자식의 복제 성공 여부(fork_success)를 알아야 함
 * - IF_는 부모 커널 스택에 있는 프레임이라, 부모가 먼저 유저로 돌아가면 덮어써짐.
 *   자식이 복사를 끝낼 때까지 부모를 붙잡아 둬야 주소만 넘겨도 안전함
 *
 * 자식의 child_info는 thread_create()가 children 맨 뒤에 넣어 둠. tid로 찾아서 그 기록의
 * 세마포어를 씀.
 *
 * @param[in] name 자식 스레드 이름 (유저가 넘긴 문자열)
 * @param[in] if_ 시스템 콜 진입 시점의 부모 유저 레지스터
 * @return 자식의 tid. 스레드 생성이나 자식 쪽 복제가 실패하면 TID_ERROR
 *
 * @note 부모는 0이 아닌 tid를, 자식은 __do_fork()에서 rax = 0으로 fork()에서 돌아감
 * @see __do_fork(), duplicate_pte()
 */
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
/**
 * @brief 부모 페이지 테이블의 항목 하나를 자식 주소 공간으로 복사함 (pml4_for_each 콜백)
 *
 * @details
 * 부모와 자식은 같은 가상 주소를 쓰지만 서로 다른 물리 페이지를 가져야 함.
 * 한쪽이 쓴 값이 다른 쪽에 보이면 안 되기 때문임. 그래서 매핑을 공유하지 않고
 * 새 유저 페이지를 받아 내용을 통째로 복사한 뒤 자식 pml4의 같은 VA에 붙임.
 *
 * - 커널 영역은 건너뜀. pml4_create()가 커널 매핑을 이미 모든 프로세스에 공통으로 넣어 두므로
 *   복사할 대상이 아님
 * - 쓰기 권한은 부모 PTE에서 그대로 가져옴. 코드 영역이 자식에서 쓰기 가능해지면 안 되기 때문임
 *
 * @param[in] pte 부모의 페이지 테이블 항목
 * @param[in] va pte가 매핑하는 가상 주소
 * @param[in] aux 부모 struct thread *
 * @return 복사했거나 건너뛰었으면 true. 할당이나 매핑에 실패하면 false여서 순회가 멈춤
 *
 * @note 실패 시 방금 받은 페이지는 여기서 해제함. 이미 붙인 페이지들은 자식 종료 때
 *       process_cleanup()의 pml4_destroy()가 해제함
 * @note project 2 전용. VM 빌드에서는 supplemental_page_table_copy()가 대신함
 */
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

/**
 * @brief 자식 스레드에서 부모의 유저 컨텍스트, 주소 공간, fd 테이블을 복제하는 스레드 함수
 *
 * @details
 * 순서는 다음과 같음.
 * 1. parent->fork_if를 로컬 if_로 복사. 부모가 깨어나면 원본이 사라지므로 먼저 복사함
 * 2. 새 pml4를 만들고 활성화한 뒤 duplicate_pte()로 유저 페이지를 하나씩 복제
 * 3. fd 테이블을 새로 만들고 열린 파일마다 file_duplicate()
 *    - file_reopen()이 아니라 file_duplicate()를 쓰는 이유: 파일 위치(pos)와 deny_write까지
 *      그대로 이어받아야 fork 직후 자식이 부모와 같은 위치에서 읽고 쓸 수 있음
 *    - struct file을 공유하지 않고 복제하는 이유: 한쪽이 close해도 다른 쪽 파일이 살아 있어야 하고,
 *      공유하면 누가 마지막에 닫을지 참조 수를 따로 세야 함
 *    - 같은 fd 번호 칸에 넣어서 자식도 부모와 같은 fd 번호로 파일을 씀
 * 4. fork_success를 쓰고 fork_sema를 올려 부모를 깨움
 * 5. rax = 0으로 바꿔 do_iret. 자식 쪽 fork()는 0을 반환함
 *
 * 실패하면 fork_success = false로 부모를 깨운 뒤 thread_exit()함. 반쯤 복제된 자원은
 * process_exit()가 정리함(fd_table_close_all, process_cleanup).
 *
 * @param[in] aux 부모 struct thread * (process_fork()가 넘김)
 *
 * @note 반환하지 않음. 성공하면 유저 모드로, 실패하면 thread_exit()로 감
 * @note fork_sema를 올리기 전에 성공 여부를 써야 함. 부모는 깨어나자마자 fork_success를 읽음
 * @see process_fork(), duplicate_pte()
 */
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

/**
 * @brief 현재 프로세스의 주소 공간을 버리고 명령줄 F_NAME의 프로그램으로 바꿔 실행함
 *
 * @details
 * 같은 스레드(같은 tid, 같은 fd 테이블, 같은 부모-자식 관계)를 유지한 채 유저 메모리만
 * 새 프로그램으로 갈아 끼움. exec 뒤에도 부모의 wait가 이 프로세스를 그대로 기다릴 수 있음.
 *
 * - intr_frame을 struct thread의 tf가 아니라 로컬 변수에 만드는 이유: tf는 컨텍스트 스위치
 *   때마다 덮어써지므로, load()가 채우는 도중 스케줄되면 값이 깨짐
 * - load()가 끝나면 명령줄 문자열은 이미 새 유저 스택에 복사되어 있으므로 f_name 페이지를 해제함
 *
 * @param[in] f_name palloc으로 받은 명령줄 페이지 (소유권 넘겨받고, 성공·실패 모두 여기서 해제)
 * @return 성공하면 반환하지 않음. 로드에 실패하면 -1
 *
 * @note 실패해도 기존 주소 공간은 process_cleanup()으로 이미 사라진 상태임.
 *       유저로 돌아갈 곳이 없으므로 호출한 쪽(SYS_EXEC)이 sys_exit(-1)로 끝내야 함
 */
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


/**
 * @brief 자식 CHILD_TID가 종료되기를 기다렸다가 그 종료 상태를 반환함
 *
 * @details
 * 종료 상태를 자식의 struct thread가 아니라 child_info에서 읽음. 자식이 먼저 죽으면
 * struct thread는 페이지째 해제되지만 child_info는 부모가 회수할 때까지 남아 있기 때문임.
 * 그래서 자식이 이미 죽은 뒤에 wait해도 상태를 받을 수 있음.
 *
 * - 자식이 아직 살아 있으면 wait_sema에서 잠듦. 자식은 process_exit()에서 상태를 쓴 뒤 sema_up함
 * - 자식이 이미 죽었으면 wait_sema 값이 1이라 바로 통과함
 * - 회수한 기록은 children에서 빼고 free함. 같은 tid를 다시 wait하면 찾지 못해 -1이 됨(wait-twice)
 * - children은 직계 자식만 담으므로, 손자나 남의 tid는 찾지 못해 -1이 됨(wait-bad-pid)
 *
 * @param[in] child_tid 기다릴 자식의 tid
 * @return 자식의 종료 상태. 직계 자식이 아니거나 이미 wait한 tid면 기다리지 않고 -1
 *
 * @note 커널이 죽인 경우(잘못된 포인터, page fault)는 sys_exit(-1)을 거치므로 -1이 됨.
 *       child_info의 exit_status 기본값도 -1임
 * @see process_exit()
 */
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

/**
 * @brief 현재 프로세스를 종료하며 종료 메시지 출력, 자원 해제, 부모-자식 기록 정리를 함
 *
 * @details
 * exit 시스템 콜, 잘못된 포인터, page fault, fork 실패 등 모든 종료 경로가 thread_exit()를
 * 거쳐 여기로 모임. 그래서 종료 메시지를 이 함수 한 곳에서만 출력해 중복이나 누락이 없게 함.
 * pml4가 없는 커널 스레드는 유저 프로세스가 아니므로 메시지를 찍지 않음.
 *
 * 순서와 이유는 다음과 같음.
 * 1. 종료 상태 결정: exit_info → self_info → -1 순으로 찾음. exit_info 할당이 실패해도 상태를 잃지 않음
 * 2. fd_table_close_all(), process_cleanup(): 부모가 wait에서 깨어나기 전에 파일과 주소 공간을 놓음
 * 3. 부모 역할 정리: 남은 자식 기록마다 자식이 이미 죽었으면 free, 살아 있으면 parent_exited만 표시.
 *    살아 있는 자식은 나중에 자기 기록을 직접 free함
 * 4. 자식 역할 정리: 내 기록에 상태를 쓰고 부모가 살아 있으면 sema_up, 부모가 이미 죽었으면 직접 free
 * 5. 실행 파일(using_file)을 닫아 deny-write를 풀어 줌
 *
 * child_info는 부모와 자식이 공유하므로 "나중에 끝난 쪽이 free한다"는 규칙으로 해제함.
 * 부모와 자식이 동시에 종료할 수 있으므로, 상대 플래그를 읽고 내 플래그를 쓰는 구간을
 * 인터럽트를 끈 채로 묶어 둘 다 free하거나 둘 다 안 하는 경우를 막음.
 * 락 대신 인터럽트를 끈 이유는 구간이 짧고, 단일 CPU에서는 이것만으로 원자성이 보장되기 때문임.
 *
 * @note thread_exit()에서만 호출됨
 * @note sema_up 이후에는 부모가 info를 free할 수 있으므로 info에 다시 접근하지 않음
 * @see sys_exit(), process_wait()
 */
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

/**
 * @brief 명령줄 FILE_NAME의 ELF 실행 파일을 현재 스레드에 로드하고 인자를 유저 스택에 쌓음
 *
 * @details
 * 1. 명령줄 분리: strtok_r로 공백 기준으로 잘라 argv[]에 담음. 연속 공백은 strtok_r가
 *    알아서 건너뜀(args-dbl-space). argv[0]이 실행 파일 이름임
 * 2. 실행 파일 열기와 deny-write: 연 직후 file_deny_write()를 걸고 using_file에 보관함.
 *    실행 중인 코드를 누가 덮어쓰면 안 되기 때문임(rox-*). 파일을 여기서 닫지 않고
 *    process_exit()까지 열어 두는 이유는, 닫으면 쓰기 금지도 같이 풀리기 때문임
 * 3. ELF 헤더와 세그먼트 로드, setup_stack()
 * 4. 인자 전달: x86-64 호출 규약에 맞춰 유저 스택에 위에서 아래로 쌓음
 *    - 문자열들을 뒤에서부터 복사하고 argv[i]를 유저 스택 주소로 바꿔 씀
 *    - 8바이트 정렬로 내림. 이후 쌓는 포인터들이 정렬된 주소에 놓이게 함
 *    - argv[] 포인터 배열(argv[argc] = NULL 포함)을 한 번에 복사
 *    - 가짜 return address(0)를 넣음. main이 보통 함수처럼 호출된 것과 같은 스택 모양이 됨
 *    - rdi = argc, rsi = argv 배열 주소, rsp = 가짜 return address 위치
 *
 * 문자열을 스택에 바로 memcpy할 수 있는 이유는 load() 앞부분에서 새 pml4를 활성화해 두어
 * 유저 주소가 현재 스레드의 주소 공간으로 해석되기 때문임.
 *
 * @param[in,out] file_name 명령줄 문자열. strtok_r가 공백 자리에 NUL을 써 넣으므로 내용이 바뀜
 * @param[out] if_ 진입점(rip), 스택(rsp), 인자 레지스터(rdi, rsi)를 채울 프레임
 * @return 성공하면 true, 파일 열기, ELF 검증, 메모리 할당 중 하나라도 실패하면 false
 *
 * @note 실패해도 using_file은 여기서 닫지 않음. process_exit()가 닫음
 * @note 인자는 최대 MAX_ARGS - 1개. 넘는 인자는 버림
 */
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
