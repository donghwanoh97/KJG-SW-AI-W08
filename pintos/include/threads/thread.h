#ifndef THREADS_THREAD_H
#define THREADS_THREAD_H

#include <debug.h>
#include <list.h>
#include <stdint.h>
#include "threads/interrupt.h"
#include "threads/synch.h"
#ifdef VM
#include "vm/vm.h"
#endif


/* 스레드 생명주기의 상태. */
enum thread_status {
	THREAD_RUNNING,     /* 실행 중인 스레드. */
	THREAD_READY,       /* 실행 중은 아니지만 실행할 준비가 됨. */
	THREAD_BLOCKED,     /* 이벤트가 발생하기를 기다리는 중. */
	THREAD_DYING        /* 곧 제거될 예정. */
};

/* 스레드 식별자 타입.
   원하는 타입으로 다시 정의해도 됨. */
typedef int tid_t;
#define TID_ERROR ((tid_t) -1)          /* tid_t의 오류 값. */

/* 스레드 우선순위. */
#define PRI_MIN 0                       /* 최저 우선순위. */
#define PRI_DEFAULT 31                  /* 기본 우선순위. */
#define PRI_MAX 63                      /* 최고 우선순위. */

/* 커널 스레드 또는 유저 프로세스.
 *
 * 각 thread 구조체는 자신만의 4 kB 페이지에 저장됨.  thread
 * 구조체 자체는 페이지의 맨 아래(offset 0)에 위치함.  페이지의
 * 나머지 부분은 스레드의 커널 스택용으로 예약되어 있으며, 커널
 * 스택은 페이지의 맨 위(offset 4 kB)에서 아래쪽으로 자람.
 * 그림으로 나타내면 다음과 같음:
 *
 *      4 kB +---------------------------------+
 *           |            커널 스택            |
 *           |                |                |
 *           |                |                |
 *           |                V                |
 *           |           아래로 자람           |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           |                                 |
 *           +---------------------------------+
 *           |              magic              |
 *           |            intr_frame           |
 *           |                :                |
 *           |                :                |
 *           |               name              |
 *           |              status             |
 *      0 kB +---------------------------------+
 *
 * 이것이 의미하는 바는 두 가지임:
 *
 *    1. 첫째, `struct thread'가 너무 커지면 안 됨.  너무
 *       커지면 커널 스택을 위한 공간이 부족해짐.  기본
 *       `struct thread'는 겨우 수 바이트 크기임.  아마
 *       1 kB를 한참 밑도는 수준으로 유지하는 것이 좋음.
 *
 *    2. 둘째, 커널 스택이 너무 커지면 안 됨.  스택이
 *       오버플로하면 스레드 상태가 손상됨.  따라서 커널
 *       함수는 큰 구조체나 배열을 non-static 지역 변수로
 *       할당하면 안 됨.  대신 malloc()이나
 *       palloc_get_page()로 동적 할당을 사용할 것.
 *
 * 이 두 문제 중 어느 쪽이든 첫 증상은 아마 thread_current()의
 * assertion 실패일 것임.  이 함수는 실행 중인 스레드의
 * `struct thread'의 `magic' 멤버가 THREAD_MAGIC으로
 * 설정되어 있는지 확인함.  스택 오버플로는 보통 이 값을
 * 바꿔 assertion을 일으킴. */
/* `elem' 멤버는 두 가지 용도가 있음.  run queue(thread.c)의
 * 요소가 될 수도 있고, 세마포어 대기 리스트(synch.c)의 요소가
 * 될 수도 있음.  이 두 가지 용도로 쓸 수 있는 이유는 둘이 서로
 * 배타적이기 때문임: ready 상태의 스레드만 run queue에 있고,
 * blocked 상태의 스레드만 세마포어 대기 리스트에 있음. */

 struct exit_info;		// 종료 기록의 구조체 이름 선언

/**
 * @brief 커널 스레드 또는 유저 프로세스를 나타내는 구조체 (TCB)
 *
 * @details 
 * 스레드마다 4 kB 페이지 하나를 차지함. 이 구조체는 페이지 맨 아래(offset 0)에,
 * 커널 스택은 맨 위에서 아래쪽으로 자람. 그래서 구조체가 커지면 스택 공간이 줄어듦.
 * thread_current()는 현재 rsp를 페이지 경계로 내려 이 구조체의 주소를 구함.
 *
 * @note elem은 ready_list(thread.c)와 세마포어 waiters(synch.c)가 공유함.
 *       THREAD_READY와 THREAD_BLOCKED는 동시에 성립하지 않으므로 둘 중 한 리스트에만 속함.
 * @note magic이 THREAD_MAGIC과 다르면 스택 오버플로로 보고 thread_current()에서 assertion 실패함.
 */
struct thread {
	/* thread.c가 소유함. */
	tid_t tid;                          /* 스레드 식별자. */
	enum thread_status status;          /* 스레드 상태. */
	char name[16];                      /* 이름 (디버깅용). */
	int priority;                       /* 우선순위. */
	int wakeup_tick;					/* 깨어날 틱 */

	/* thread.c와 synch.c가 공유함. */
	struct list_elem elem;              /* 리스트 요소. */
	
	/* priority donation용. */
	int init_priority;                  /* donation 받기 전 원래 우선순위. */
	struct lock *wait_on_lock;          /* 현재 획득을 기다리고 있는 락의 주소 */
	struct list donations;              /* 자신에게 우선순위를 기부한 스레드들의 리스트 */
	struct list_elem donation_elem;     /* 기부자 리스트(donations)에 삽입될 리스트 엘리먼트 */
	
	/* user program */	
	struct list children; // 부모 -> 자식 연결용 리스트 (struct child_info)
	struct child_info* self_info; // 자식 -> 부모 연결용 포인터 (부모 children에 있는 내 기록)
	struct intr_frame* fork_if; // fork 중인 부모의 유저 레지스터 주소. __do_fork()가 읽어 감

#ifdef USERPROG
	/* userprog/process.c가 소유함. */
	uint64_t *pml4;                     /* 페이지 맵 레벨 4(Page map level 4) */
	struct exit_info *exit_info;	/* 내 종료 기록 주소 */
	struct file **fd_table;		/* fd 테이블 (calloc, FD_MAX칸, 커널 스레드는 NULL) */
	struct file *using_file ;	/* 실행 중인 실행 파일. deny-write를 건 채 종료 때 닫음 */
#endif
#ifdef VM
	/* 스레드가 소유한 전체 가상 메모리의 테이블. */
	struct supplemental_page_table spt;
#endif

	/* thread.c가 소유함. */
	struct intr_frame tf;               /* 컨텍스트 전환용 정보 */
	unsigned magic;                     /* 스택 오버플로를 감지함. */
};

/* false(기본값)이면 round-robin 스케줄러를 사용함.
   true이면 multi-level feedback queue 스케줄러를 사용함.
   커널 커맨드라인 옵션 "-o mlfqs"로 제어함. */
extern bool thread_mlfqs;

bool thread_priority_higher (const struct list_elem *a, const struct list_elem *b, void *aux);
bool thread_priority_lower (const struct list_elem *a, const struct list_elem *b, void *aux);

void thread_test_preemption (void);
void thread_requeue (struct thread *t);
void refresh_priority (void);

void thread_init (void);
void thread_start (void);

void thread_tick (void);
void thread_print_stats (void);

typedef void thread_func (void *aux);
tid_t thread_create (const char *name, int priority, thread_func *, void *);

void thread_block (void);
void thread_unblock (struct thread *);

struct thread *thread_current (void);
tid_t thread_tid (void);
const char *thread_name (void);

void thread_exit (void) NO_RETURN;
void thread_yield (void);

int thread_get_priority (void);
void thread_set_priority (int);

int thread_get_nice (void);
void thread_set_nice (int);
int thread_get_recent_cpu (void);
int thread_get_load_avg (void);

void do_iret (struct intr_frame *tf);

/**
 * @brief sleep에 들어간 쓰레드 목록
 * 
 * 이중 연결 리스트로 구현 되있음.
 */
extern struct list sleep_list;

#endif /* threads/thread.h */
