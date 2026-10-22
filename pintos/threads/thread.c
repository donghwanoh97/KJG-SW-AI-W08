/**
 * @file thread.c
 * @brief 스레드
 * @date 2026-10-10
 */

#include "threads/thread.h"
#include <debug.h>
#include <stddef.h>
#include <random.h>
#include <stdio.h>
#include <string.h>
#include "threads/flags.h"
#include "threads/interrupt.h"
#include "threads/intr-stubs.h"
#include "threads/palloc.h"
#include "threads/synch.h"
#include "threads/vaddr.h"
#include "intrinsic.h"
#ifdef USERPROG
#include "userprog/process.h"
#endif

/* struct thread의 `magic' 멤버에 쓰는 임의의 값.
   스택 오버플로를 감지하는 데 사용됨.  자세한 내용은 thread.h
   상단의 큰 주석 참고. */
#define THREAD_MAGIC 0xcd6abf4b

/* 기본(basic) thread용 임의의 값.
   이 값을 수정하지 말 것. */
#define THREAD_BASIC 0xd42df210

/* THREAD_READY 상태의 프로세스 목록. 즉, 실행할 준비는 됐지만
   실제로 실행 중은 아닌 프로세스들. */
static struct list ready_list;

/**
 * @brief sleep에 들어간 쓰레드 목록
 *
 * 이중 연결 리스트로 구현 되있음.
 */
struct list sleep_list;

/* idle 스레드. */
static struct thread *idle_thread;

/* 초기 스레드. init.c:main()을 실행하는 스레드. */
static struct thread *initial_thread;

/* allocate_tid()가 사용하는 락. */
static struct lock tid_lock;

/* 스레드 제거 요청 목록 */
static struct list destruction_req;

/* 통계. */
static long long idle_ticks;    /* idle 상태로 보낸 타이머 틱 수. */
static long long kernel_ticks;  /* 커널 스레드에서 보낸 타이머 틱 수. */
static long long user_ticks;    /* 유저 프로그램에서 보낸 타이머 틱 수. */

/* 스케줄링. */
#define TIME_SLICE 4            /* 각 스레드에 주는 타이머 틱 수. */
static unsigned thread_ticks;   /* 마지막 yield 이후 타이머 틱 수. */

/* false(기본값)이면 round-robin 스케줄러를 사용함.
   true이면 multi-level feedback queue 스케줄러를 사용함.
   커널 커맨드라인 옵션 "-o mlfqs"로 제어함. */
bool thread_mlfqs;

static void kernel_thread (thread_func *, void *aux);

static void idle (void *aux UNUSED);
static struct thread *next_thread_to_run (void);
static void init_thread (struct thread *, const char *name, int priority);
static void do_schedule(int status);
static void schedule (void);
static tid_t allocate_tid (void);

/* T가 유효한 스레드를 가리키는 것으로 보이면 true를 반환함. */
#define is_thread(t) ((t) != NULL && (t)->magic == THREAD_MAGIC)

/**
 * @brief 실행 중인 스레드를 반환함.
 *
 * CPU의 스택 포인터 `rsp'를 읽은 뒤 페이지 시작 주소로
 * 내림함.  `struct thread'는 항상 페이지의 맨 앞에 있고
 * 스택 포인터는 그 중간 어딘가에 있으므로, 이렇게 하면
 * 현재 스레드를 찾을 수 있음.
 *
 * @note
 * 이걸 굳이 이해해야될까?
 *
 * @see timer_ticks(), timer_elapsed(), thread_yield()
 */
#define running_thread() ((struct thread *) (pg_round_down (rrsp ())))


// thread_start용 전역 디스크립터 테이블(GDT).
// gdt는 thread_init 이후에 설정되므로, 임시 gdt를 먼저
// 설정해야 함.
static uint64_t gdt[3] = { 0, 0x00af9a000000ffff, 0x00cf92000000ffff };

/**
 * @brief main()이 호출하여 스레드 시스템을 초기화
 *
 * 현재 실행 중인 코드를 스레드로 변환해 Pintos의 초기 스레드(initial thread)를
 * 위한 struct thread를 만들고, run queue와 tid lock도 함께 초기화함.
 *
 * @details
 * 일반적으로는 불가능하며, 이 경우에만 가능한 이유는 loader.S가 스택의 맨 아래를
 * 페이지 경계에 두도록 신경 썼기 때문임. 즉 Pintos 로더(loader)가 초기 스레드의
 * 스택을 다른 모든 Pintos 스레드와 같은 위치, 즉 페이지의 맨 위에 둠.
 *
 * thread_init()이 끝나기 전에는 실행 중인 스레드의 magic 값이 올바르지 않으므로
 * thread_current()를 호출하는 것이 안전하지 않음(실패함). lock_acquire()를
 * 포함해 많은 함수가 thread_current()를 직간접적으로 호출하기 때문에,
 * thread_init()은 Pintos 초기화 과정의 아주 이른 시점에 호출됨.
 *
 * @note 이 함수를 호출한 뒤에는, thread_create()로 스레드를 만들기 전에
 *       반드시 page allocator를 초기화할 것
 */
void
thread_init (void) {
	ASSERT (intr_get_level () == INTR_OFF);

	/* 커널용 임시 gdt를 다시 로드함.
	 * 이 gdt에는 유저 컨텍스트가 포함되어 있지 않음.
	 * 커널은 gdt_init ()에서 유저 컨텍스트를 포함해 gdt를 다시 구성함. */
	struct desc_ptr gdt_ds = {
		.size = sizeof (gdt) - 1,
		.address = (uint64_t) gdt
	};
	lgdt (&gdt_ds);

	/* 전역 스레드 컨텍스트를 초기화함 */
	lock_init (&tid_lock);
	list_init (&ready_list);
	list_init (&sleep_list);
	list_init (&destruction_req);

	/* 실행 중인 스레드를 위한 thread 구조체를 설정함. */
	initial_thread = running_thread ();
	init_thread (initial_thread, "main", PRI_DEFAULT);
	initial_thread->status = THREAD_RUNNING;
	initial_thread->tid = allocate_tid ();
}

/**
 * @brief ready_list 맨 앞 스레드의 priority가 더 높으면 현재 스레드가 CPU를 양보함
 *
 * @details
 * ready_list는 priority 내림차순으로 유지되므로 list_front()가 가장 높은 priority의
 * 스레드임. 현재 스레드의 priority가 그보다 낮을 때만 양보함.
 * 외부 인터럽트 문맥이면 thread_yield()를 직접 호출할 수 없으므로
 * intr_yield_on_return()으로 핸들러 종료 시 양보하도록 예약함.
 *
 * @note ready_list가 비어 있으면 아무 것도 하지 않음
 * @note priority가 같으면 양보하지 않음
 * @note thread_create(), thread_set_priority(), sema_up()에서 호출됨
 *
 * @see thread_yield()
 * @see thread_priority_higher()
 */
void thread_test_preemption (void) {
        if (list_empty (&ready_list)) return;

		struct thread* curr = thread_current();
        struct thread* next = list_entry(list_front (&ready_list), struct thread, elem);

        if (curr->priority < next->priority) {
            if (intr_context()) { // 인터럽트 컨텍스트 분기
                intr_yield_on_return();
			}
             else {
                thread_yield();
			}
        }
}

/**
 * @brief main()이 호출하여 스케줄러를 시작함
 *
 * 인터럽트를 활성화해 preemptive 스레드 스케줄링을 시작하고, idle 스레드도 함께 생성함.
 *
 * @details
 * 유휴 스레드(idle thread), 즉 준비된 스레드가 하나도 없을 때 스케줄되는
 * 스레드를 생성함. 그다음 인터럽트를 활성화하는데, 스케줄러는 타이머
 * 인터럽트에서 복귀할 때 intr_yield_on_return()을 사용해 실행되므로
 * 이것이 부수 효과로 스케줄러도 활성화함.
 */
void
thread_start (void) {
	/* idle 스레드를 생성함. */
	struct semaphore idle_started;
	sema_init (&idle_started, 0);
	thread_create ("idle", PRI_MIN, idle, &idle_started);

	/* preemptive 스레드 스케줄링을 시작함. */
	intr_enable ();

	/* idle 스레드가 idle_thread를 초기화할 때까지 기다림. */
	sema_down (&idle_started);
}

/**
 * @brief 타이머 틱마다 타이머 인터럽트 핸들러가 호출함. 따라서 이 함수는 외부 인터럽트 컨텍스트에서 실행됨.
 *
 * @details
 * 스레드 통계를 추적하고, 타임 슬라이스(time slice)가 만료되면 스케줄러를 작동시킴.
 *
 */
void
thread_tick (void) {
	struct thread *t = thread_current ();

	/* 통계를 갱신함. */
	if (t == idle_thread)
		idle_ticks++;
#ifdef USERPROG
	else if (t->pml4 != NULL)
		user_ticks++;
#endif
	else
		kernel_ticks++;

	/* preemption을 강제함. */
	if (++thread_ticks >= TIME_SLICE)
		intr_yield_on_return ();
}

/**
 * @brief 스레드 통계를 출력함.
 *
 * @details Pintos 종료 시 호출됨.
 *
 * @note 아직 뭔지 모르곘음
 */
void
thread_print_stats (void) {
	printf ("Thread: %lld idle ticks, %lld kernel ticks, %lld user ticks\n",
			idle_ticks, kernel_ticks, user_ticks);
}


/**
 * @brief 이름이 NAME이고 초기 우선순위가 PRIORITY인 새 커널 스레드를 생성함.
 *
 * 이 스레드는 AUX를 인자로 FUNCTION을 실행하며, ready queue에 추가됨.
 *
 * @details
 * 스레드의 struct thread와 스택을 위한 페이지를 할당하고 멤버들을 초기화한 뒤,
 * 가짜 스택 프레임(fake stack frame) 묶음을 설정함. 스레드는 blocked 상태로
 * 초기화되었다가, 반환 직전에 unblock되어 새 스레드가 스케줄될 수 있게 됨.
 *
 * thread_start()가 호출된 상태라면 thread_create()가 반환되기 전에 새 스레드가
 * 스케줄될 수 있고, 심지어 반환되기 전에 종료될 수도 있음. 반대로, 새 스레드가
 * 스케줄되기 전까지 원래 스레드가 얼마든지 오래 실행될 수도 있음.
 *
 * 프로젝트 2: 새 스레드의 child_info를 만들어 현재 스레드의 children에 넣음.
 * unblock 전에 넣는 이유는, 새 스레드가 바로 스케줄되어 종료하거나 fork를 끝내도
 * 부모가 tid로 기록을 찾을 수 있어야 하기 때문임(process_fork(), process_wait()).
 * fork와 initd 모두 이 함수를 거치므로 기록 생성 지점을 한 곳으로 모음.
 *
 * @param[in] name 새 스레드의 이름
 * @param[in] priority 새 스레드의 초기 우선순위
 * @param[in] function 새 스레드가 실행할 함수 (thread_func 타입)
 * @param[in] aux function의 유일한 인자로 그대로 전달됨
 * @return 새 스레드의 스레드 식별자(tid). 생성에 실패하면 TID_ERROR
 *
 * @note 실행 순서를 보장해야 한다면 세마포어나 다른 동기화 수단을 사용할 것
 * @note 새 스레드를 unblock한 직후 thread_test_preemption()을 호출하므로,
 *       새 스레드의 priority가 현재 스레드보다 높으면 반환 전에 CPU를 선점함
 */
tid_t
thread_create (const char *name, int priority, thread_func *function, void *aux) {
	struct thread *t;
	tid_t tid;

	ASSERT (function != NULL);

	/* 스레드를 할당함. */
	t = palloc_get_page (PAL_ZERO);
	if (t == NULL)
		return TID_ERROR;

	/* 스레드를 초기화함. */
	init_thread (t, name, priority);
	tid = t->tid = allocate_tid ();

	/* 스케줄되면 kernel_thread를 호출함.
	 * 참고) rdi는 첫 번째 인자, rsi는 두 번째 인자임. */
	t->tf.rip = (uintptr_t) kernel_thread;
	t->tf.R.rdi = (uint64_t) function;
	t->tf.R.rsi = (uint64_t) aux;
	t->tf.ds = SEL_KDSEG;
	t->tf.es = SEL_KDSEG;
	t->tf.ss = SEL_KDSEG;
	t->tf.cs = SEL_KCSEG;
	t->tf.eflags = FLAG_IF;

	/* user program */
	struct thread *curr = thread_current();
	struct child_info *info = malloc(sizeof(*info));

	info->tid = t->tid;
	info->exit_status = -1;
	info->child_exited = false;
	info->parent_exited = false;
	info->fork_success = false;
	sema_init(&info->wait_sema, 0);
	sema_init(&info->fork_sema, 0);

	t->self_info = info;
	list_push_back(&curr->children, &info->elem);

	/* run queue에 추가함. */
	thread_unblock (t);

	thread_test_preemption();

	return tid;
}

/**
 * @brief 현재 스레드를 BLOCKED 상태로 전환하고 스케줄링함
 *
 * @details
 * 현재 스레드의 상태를 THREAD_BLOCKED로 변경한 뒤 schedule()를 호출
 * 이 스레드는 다른 코드가 thread_unblock()으로 깨워서 실행 가능하게 만들고
 * 스케줄러가 다시 선택한 후에 호출 지점으로 복귀
 *
 * thread_unblock()으로 깨어나기 전까지 다시 실행 대상으로 선택되지 않으므로,
 * 그렇게 되도록 미리 방법을 마련해 두어야 함. 너무 저수준(low-level)이므로,
 * 일반적으로는 synch.h의 동기화 기본 요소(synchronization primitive)를 사용하는 편이 낫음.
 * 블록된 스레드가 무엇을 기다리는지 선험적으로 알아낼 방법은 없지만,
 * 백트레이스(backtrace)가 도움이 될 수 있음.
 *
 * @note 외부 인터럽트 처리 문맥에서는 호출할 수 없음
 * @note 호출 시 인터럽트가 비활성화되어 있어야 함
 * @note 이 함수 자체는 깨울 시각을 기록하거나 시간을 확인하지 않음
 *
 * @see thread_unblock()
 * @see schedule()
 */
void thread_block (void) {
	ASSERT (!intr_context ());
	ASSERT (intr_get_level () == INTR_OFF);
	thread_current ()->status = THREAD_BLOCKED;
	schedule ();
}

/**
 * @brief 두 스레드의 priority를 비교하는 list_less_func
 *
 * @details
 * list_insert_ordered()에 넘겨 리스트를 priority 내림차순으로 유지하는 데 사용
 * 각 list_elem에서 list_entry()로 소속 스레드를 구해 priority 값을 비교
 *
 * @param[in] a 비교할 첫 번째 스레드의 list_elem (새로 삽입할 원소)
 * @param[in] b 비교할 두 번째 스레드의 list_elem (리스트에 이미 있는 원소)
 * @param[in] aux 사용하지 않음
 *
 * @return a의 priority가 b의 priority보다 높으면 true, 아니면 false
 *
 * @note priority가 같으면 false이므로 동일 priority 안에서는 먼저 들어온 스레드가 앞에 유지됨(FIFO)
 * @note 리스트 맨 앞(list_front)이 priority가 가장 높은 스레드
 * @see list_insert_ordered()
 */
bool
thread_priority_higher (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	return list_entry(a, struct thread, elem)->priority > list_entry(b, struct thread, elem)->priority;
}

/**
 * @brief 두 스레드의 priority를 비교하는 list_less_func (thread_priority_higher의 반대)
 *
 * @details list_max()/list_min()에 넘겨 priority가 가장 높은 스레드를 찾는 데 사용
 *
 * @param[in] a 비교할 첫 번째 스레드의 list_elem
 * @param[in] b 비교할 두 번째 스레드의 list_elem
 * @param[in] aux 사용하지 않음
 *
 * @return a의 priority가 b의 priority보다 낮으면 true, 아니면 false
 */
bool
thread_priority_lower (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	return list_entry(a, struct thread, elem)->priority < list_entry(b, struct thread, elem)->priority;
}

/**
 * @brief READY 스레드 T를 ready_list에서 빼서 현재 priority 위치로 다시 삽입함
 *
 * @details
 * donation으로 ready_list 안 스레드의 priority가 바뀌면 정렬이 깨지므로,
 * priority를 바꾼 직후에 호출해 정렬을 복구함. READY가 아니면 아무 것도 하지 않음.
 *
 * @param[in,out] t priority가 방금 바뀐 스레드
 *
 * @note 인터럽트가 꺼진 상태에서 호출해야 함
 */
void
thread_requeue (struct thread *t) {
	if (t->status == THREAD_READY) {
		list_remove (&t->elem);
		list_insert_ordered (&ready_list, &t->elem, thread_priority_higher, NULL);
	}
}



/**
 * @brief 대기중인(BLOCKED) 스레드를 실행 가능 상태로 전환
 *
 * @details
 * 대상 스레드의 elem을 ready_list에 priority 내림차순 위치로 삽입하고
 * status를 THREAD_READY로 변경함
 * 구조체 전체를 복사하는게 아니라 목록 연결 요소를 연결
 *
 * 스레드가 기다리던 이벤트가 발생했을 때(예: 기다리던 락이 사용 가능해졌을 때) 호출됨.
 * 이 함수는 현재 실행 중인 스레드의 CPU를 빼앗지 않음. 호출자가 이미 인터럽트를
 * 꺼둔 경우, 스레드를 깨우고 다른 데이터를 변경하는 작업을 중간에 방해받지 않는
 * 하나의 구간으로 처리하려 할 수 있기 때문에 즉시 실행을 교대하지 않는 것이 중요함.
 *
 * @param[in, out] t 깨울 BLOCKED 스레드를 가리키는 포인터
 *
 * @note t가 BLOCKED 상태가 아니라면 잘못된 호출임
 * @note 실행 중인 스레드를 READY 상태로 바꾸려면 thread_yield()를 사용
 * @note 상태 변경과 실행 후보 목록 등록이 모두 필요
 * @note 이 함수 자체는 대상 스레드를 즉시 실행하지 않음
 */
void
thread_unblock (struct thread *t) {
	enum intr_level old_level;

	ASSERT (is_thread (t));

	old_level = intr_disable ();
	ASSERT (t->status == THREAD_BLOCKED);
	list_insert_ordered(&ready_list, &t->elem, thread_priority_higher, NULL);
	t->status = THREAD_READY;
	intr_set_level (old_level);
}

/**
 * @brief 실행 중인 스레드의 이름을 반환함
 *
 * @details thread_current ()->name과 같음.
 */
const char *
thread_name (void) {
	return thread_current ()->name;
}


/**
 * @brief  실행 중인 스레드를 반환하는 함수
 *
 * running_thread()에 몇 가지 정합성 검사(sanity check)를 더한 것임
 *
 * @return 실행 중인 스레드를 반환
 *
 * @note 
 * 자세한 내용은 thread.h 상단의 큰 주석 참고. 
 *
 * @see running_thread(), thread.h
 */
struct thread *
thread_current (void) {
	struct thread *t = running_thread ();

	/* T가 정말 스레드인지 확인함.
	   이 assertion 중 하나라도 터지면 스레드의 스택이 오버플로했을
	   수 있음.  각 스레드의 스택은 4 kB 미만이므로, 큰 자동 배열 몇 개나
	   적당한 깊이의 재귀만으로도 스택 오버플로가 일어날 수 있음. */
	ASSERT (is_thread (t));
	ASSERT (t->status == THREAD_RUNNING);

	return t;
}

/**
 * @brief 실행 중인 스레드의 tid를 반환함
 *
 * @details thread_current ()->tid와 같음.
 */
tid_t
thread_tid (void) {
	return thread_current ()->tid;
}

/**
 * @brief 현재 스레드를 스케줄 대상에서 제외하고 제거함
 *
 * @note 절대 반환하지 않는다 (NO_RETURN)
 */
void
thread_exit (void) {
	ASSERT (!intr_context ());

#ifdef USERPROG
	process_exit ();
#endif

	/* 상태를 dying으로 설정하고 다른 프로세스를 스케줄하기만 함.
	   실제 제거는 schedule_tail() 호출 중에 이루어짐. */
	intr_disable ();
	do_schedule (THREAD_DYING);
	NOT_REACHED ();
}

/**
 * @brief 현재 스레드 CPU 양보 (대기 상태로 들어가지 않음)
 *
 * @details 일반 스레드는 ready_list에 priority 내림차순 위치로 삽입되어
 *          다시 실행 대상으로 선택될 수 있음 (스케줄러에게)
 *
 * @note BLOCKED 되는게 아님
 * @note CPU 양보하고 ready_list 들가기 때문에 즉시 다시 선택될 수도 있음
 * @note 새로 선택된 스레드가 현재 스레드일 수도 있으므로, 이 함수로 현재 스레드가
 *       특정 시간 동안 실행되지 않도록 만들 수 있다고 기대해서는 안 됨
 */
void
thread_yield (void) {
	struct thread *curr = thread_current ();
	enum intr_level old_level;

	ASSERT (!intr_context ());

	old_level = intr_disable ();
	if (curr != idle_thread)
		list_insert_ordered(&ready_list, &curr->elem, thread_priority_higher, NULL);
	do_schedule (THREAD_READY);
	intr_set_level (old_level);
}

/**
 * @brief 현재 스레드의 우선순위를 NEW_PRIORITY로 설정함
 *
 * @details
 * 우선순위는 PRI_MIN(0)부터 PRI_MAX(63)까지이며, 숫자가 낮을수록 우선순위가 낮음.
 * 설정 후 thread_test_preemption()을 호출해, 낮아진 priority가 ready_list의
 * 최고 priority보다 낮아졌다면 즉시 CPU를 양보함.
 *
 * @param[in] new_priority 새 우선순위
 *
 * @note 원래 priority(init_priority)만 바꾸고, donation을 반영한 유효 priority는 refresh_priority()로 다시 계산함
 * @note MLFQS(thread_mlfqs)가 켜져 있으면 무시함
 */
void
thread_set_priority (int new_priority) {
	if (thread_mlfqs)
		return;

	thread_current ()->init_priority = new_priority;
	refresh_priority ();
	thread_test_preemption();
}

/**
 * @brief donations 중 가장 높은 priority를 비교하는 list_less_func
 *
 * @details donation_elem 기준으로 두 스레드의 priority를 비교함.
 *
 * @param[in] a 비교할 첫 번째 스레드의 donation_elem
 * @param[in] b 비교할 두 번째 스레드의 donation_elem
 * @param[in] aux 사용하지 않음
 *
 * @return a의 priority가 b보다 낮으면 true, 아니면 false
 */
static bool
donation_priority_lower (const struct list_elem *a, const struct list_elem *b, void *aux UNUSED) {
	return list_entry (a, struct thread, donation_elem)->priority
		< list_entry (b, struct thread, donation_elem)->priority;
}

/**
 * @brief 현재 스레드의 priority를 init_priority와 donations 중 큰 값으로 다시 계산함
 *
 * @note lock_release()와 thread_set_priority()에서 호출됨
 * @note 현재 실행 중인 스레드만 갱신하므로 ready_list 재정렬이 필요 없음
 */
void
refresh_priority (void) {
	struct thread *curr = thread_current();

	curr->priority = curr->init_priority;
	if (!list_empty(&curr->donations)) {
		struct thread *top = list_entry (list_max (&curr->donations, donation_priority_lower, NULL), struct thread, donation_elem);
		if (top->priority > curr->priority) {
			curr->priority = top->priority;
		}
	}
}

/**
 * @brief 현재 스레드의 우선순위를 반환함
 *
 * @note donation이 반영된 유효 priority(priority)를 반환함. init_priority가 아님
 */
int
thread_get_priority (void) {
	return thread_current ()->priority;
}

/* 현재 스레드의 nice 값을 NICE로 설정함.
   (thread_set_nice, thread_get_nice, thread_get_recent_cpu, thread_get_load_avg는
   고급 스케줄러(advanced scheduler)를 위한 스텁 함수들임.) */
void
thread_set_nice (int nice UNUSED) {
	/* TODO: 여기에 구현을 작성할 것 */
}

/* 현재 스레드의 nice 값을 반환함. */
int
thread_get_nice (void) {
	/* TODO: 여기에 구현을 작성할 것 */
	return 0;
}

/* 시스템 load average의 100배 값을 반환함. */
int
thread_get_load_avg (void) {
	/* TODO: 여기에 구현을 작성할 것 */
	return 0;
}

/* 현재 스레드의 recent_cpu 값의 100배를 반환함. */
int
thread_get_recent_cpu (void) {
	/* TODO: 여기에 구현을 작성할 것 */
	return 0;
}

/* idle 스레드.  실행할 준비가 된 다른 스레드가 없을 때 실행됨.

   idle 스레드는 처음에 thread_start()에 의해 ready list에
   들어감.  처음에 한 번 스케줄되면 idle_thread를 초기화하고,
   전달받은 세마포어를 "up"해서 thread_start()가 계속 진행되게
   한 뒤, 곧바로 block됨.  그 이후로 idle 스레드는 ready list에
   나타나지 않음.  ready list가 비어 있을 때에 한해 특수한 경우로
   next_thread_to_run()이 idle 스레드를 반환함. */
static void
idle (void *idle_started_ UNUSED) {
	struct semaphore *idle_started = idle_started_;

	idle_thread = thread_current ();
	sema_up (idle_started);

	for (;;) {
		/* 다른 스레드가 실행되게 함. */
		intr_disable ();
		thread_block ();

		/* 인터럽트를 다시 활성화하고 다음 인터럽트를 기다림.

		   `sti' 명령은 다음 명령이 끝날 때까지 인터럽트를 비활성
		   상태로 유지하므로, 이 두 명령은 원자적으로 실행됨.  이
		   원자성이 중요한 이유는, 그렇지 않으면 인터럽트를 다시
		   활성화한 시점과 다음 인터럽트를 기다리는 시점 사이에
		   인터럽트가 처리되어 클럭 틱 하나만큼의 시간을 낭비할 수
		   있기 때문임.

		   [IA32-v2a] "HLT", [IA32-v2b] "STI", [IA32-v3a]
		   7.11.1 "HLT Instruction" 참고. */
		asm volatile ("sti; hlt" : : : "memory");
	}
}

/* 커널 스레드의 기반이 되는 함수. */
static void
kernel_thread (thread_func *function, void *aux) {
	ASSERT (function != NULL);

	intr_enable ();       /* 스케줄러는 인터럽트를 끈 상태로 실행됨. */
	function (aux);       /* 스레드 함수를 실행함. */
	thread_exit ();       /* function()이 반환하면 스레드를 종료함. */
}


/* T를 NAME이라는 이름의 blocked 스레드로 기본 초기화함. */
static void
init_thread (struct thread *t, const char *name, int priority) {
	ASSERT (t != NULL);
	ASSERT (PRI_MIN <= priority && priority <= PRI_MAX);
	ASSERT (name != NULL);

	memset (t, 0, sizeof *t);
	t->status = THREAD_BLOCKED;
	strlcpy (t->name, name, sizeof t->name);
	t->tf.rsp = (uint64_t) t + PGSIZE - sizeof (void *);
	t->priority = priority;
	t->init_priority = priority;
	list_init(&t->donations);

	/* user program */
	list_init(&t->children);

	t->magic = THREAD_MAGIC;
}

/**
 * @brief 다음에 실행할 스레드를 골라 반환함
 *
 * @details
 * ready_list는 priority 내림차순으로 유지되므로 맨 앞 스레드를 list_pop_front()로
 * 꺼내 반환함. ready_list가 비어 있으면 idle_thread를 반환함.
 * 실행 중인 스레드가 계속 실행할 수 있다면 그 스레드도 ready_list에 들어 있음.
 * READY 스레드의 priority가 donation으로 바뀌면 thread_requeue()가 위치를 다시 잡아줌.
 *
 * @return 다음에 실행할 스레드. ready_list가 비어 있으면 idle_thread
 *
 * @note 인터럽트가 꺼진 상태에서 호출해야 함 (schedule() 경로에서 호출됨)
 *
 * @see thread_requeue()
 * @see do_schedule()
 */
static struct thread *
next_thread_to_run (void) {
	if (list_empty (&ready_list))
		return idle_thread;
	else
		return list_entry (list_pop_front (&ready_list), struct thread, elem);
}

/* iretq를 사용해 스레드를 시작함 */
void
do_iret (struct intr_frame *tf) {
	__asm __volatile(
			"movq %0, %%rsp\n"
			"movq 0(%%rsp),%%r15\n"
			"movq 8(%%rsp),%%r14\n"
			"movq 16(%%rsp),%%r13\n"
			"movq 24(%%rsp),%%r12\n"
			"movq 32(%%rsp),%%r11\n"
			"movq 40(%%rsp),%%r10\n"
			"movq 48(%%rsp),%%r9\n"
			"movq 56(%%rsp),%%r8\n"
			"movq 64(%%rsp),%%rsi\n"
			"movq 72(%%rsp),%%rdi\n"
			"movq 80(%%rsp),%%rbp\n"
			"movq 88(%%rsp),%%rdx\n"
			"movq 96(%%rsp),%%rcx\n"
			"movq 104(%%rsp),%%rbx\n"
			"movq 112(%%rsp),%%rax\n"
			"addq $120,%%rsp\n"
			"movw 8(%%rsp),%%ds\n"
			"movw (%%rsp),%%es\n"
			"addq $32, %%rsp\n"
			"iretq"
			: : "g" ((uint64_t) tf) : "memory");
}

/* 새 스레드의 page table을 활성화해 스레드를 전환하고, 이전 스레드가
   dying 상태라면 그 스레드를 제거함.

   이 함수가 호출된 시점에는 방금 PREV 스레드에서 전환된 상태이고,
   새 스레드는 이미 실행 중이며, 인터럽트는 여전히 꺼져 있음.

   스레드 전환이 끝나기 전에는 printf()를 호출하는 것이 안전하지
   않음.  실제로는 printf()를 함수의 맨 끝에 추가해야 한다는 뜻임. */
static void
thread_launch (struct thread *th) {
	uint64_t tf_cur = (uint64_t) &running_thread ()->tf;
	uint64_t tf = (uint64_t) &th->tf;
	ASSERT (intr_get_level () == INTR_OFF);

	/* 핵심 전환 로직.
	 * 먼저 전체 실행 컨텍스트를 intr_frame에 저장하고,
	 * do_iret를 호출해 다음 스레드로 전환함.
	 * 주의: 여기서부터 전환이 끝날 때까지 어떤 스택도
	 * 사용하면 안 됨. */
	__asm __volatile (
			/* 사용할 레지스터를 저장함. */
			"push %%rax\n"
			"push %%rbx\n"
			"push %%rcx\n"
			/* 입력을 한 번만 가져옴 */
			"movq %0, %%rax\n"
			"movq %1, %%rcx\n"
			"movq %%r15, 0(%%rax)\n"
			"movq %%r14, 8(%%rax)\n"
			"movq %%r13, 16(%%rax)\n"
			"movq %%r12, 24(%%rax)\n"
			"movq %%r11, 32(%%rax)\n"
			"movq %%r10, 40(%%rax)\n"
			"movq %%r9, 48(%%rax)\n"
			"movq %%r8, 56(%%rax)\n"
			"movq %%rsi, 64(%%rax)\n"
			"movq %%rdi, 72(%%rax)\n"
			"movq %%rbp, 80(%%rax)\n"
			"movq %%rdx, 88(%%rax)\n"
			"pop %%rbx\n"              // 저장해 둔 rcx
			"movq %%rbx, 96(%%rax)\n"
			"pop %%rbx\n"              // 저장해 둔 rbx
			"movq %%rbx, 104(%%rax)\n"
			"pop %%rbx\n"              // 저장해 둔 rax
			"movq %%rbx, 112(%%rax)\n"
			"addq $120, %%rax\n"
			"movw %%es, (%%rax)\n"
			"movw %%ds, 8(%%rax)\n"
			"addq $32, %%rax\n"
			"call __next\n"         // 현재 rip를 읽음.
			"__next:\n"
			"pop %%rbx\n"
			"addq $(out_iret -  __next), %%rbx\n"
			"movq %%rbx, 0(%%rax)\n" // rip
			"movw %%cs, 8(%%rax)\n"  // cs
			"pushfq\n"
			"popq %%rbx\n"
			"mov %%rbx, 16(%%rax)\n" // eflags
			"mov %%rsp, 24(%%rax)\n" // rsp
			"movw %%ss, 32(%%rax)\n"
			"mov %%rcx, %%rdi\n"
			"call do_iret\n"
			"out_iret:\n"
			: : "g"(tf_cur), "g" (tf) : "memory"
			);
}

/* 새 프로세스를 스케줄함. 진입 시 인터럽트는 꺼져 있어야 함.
 * 이 함수는 현재 스레드의 status를 인자로 받은 status로 바꾼 뒤,
 * 실행할 다른 스레드를 찾아 전환함.
 * schedule() 안에서 printf()를 호출하는 것은 안전하지 않음. */
static void
do_schedule(int status) {
	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (thread_current()->status == THREAD_RUNNING);
	while (!list_empty (&destruction_req)) {
		struct thread *victim = list_entry (list_pop_front (&destruction_req), struct thread, elem);
		palloc_free_page(victim);
	}
	thread_current ()->status = status;
	schedule ();
}

/**
 * @brief 현재 스레드를 내보내고 next_thread_to_run()이 고른 스레드로 전환함
 *
 * @details
 * 호출 전에 현재 스레드의 status를 RUNNING이 아닌 값으로 바꿔 두어야 함.
 * 다음 스레드를 RUNNING으로 표시하고 thread_ticks를 0으로 되돌린 뒤,
 * 나가는 스레드가 THREAD_DYING이면 destruction_req에 넣고 thread_launch()로 전환함.
 *
 * @note 인터럽트가 꺼진 상태에서 호출해야 함
 *
 * @see next_thread_to_run()
 * @see thread_launch()
 */
static void
schedule (void) {
	struct thread *curr = running_thread ();
	struct thread *next = next_thread_to_run ();

	ASSERT (intr_get_level () == INTR_OFF);
	ASSERT (curr->status != THREAD_RUNNING);
	ASSERT (is_thread (next));
	/* 실행 중으로 표시함. */
	next->status = THREAD_RUNNING;

	/* 새 time slice를 시작함. */
	thread_ticks = 0;

#ifdef USERPROG
	/* 새 주소 공간을 활성화함. */
	process_activate (next);
#endif

	if (curr != next) {
		/* 전환해 나온 스레드가 dying 상태라면 그 struct thread를
		   제거함. thread_exit()가 스스로의 발밑을 허물지 않도록 이
		   작업은 늦게 일어나야 함.
		   해당 페이지는 아직 스택으로 사용 중이므로, 여기서는 페이지
		   해제 요청을 큐에 넣기만 함.
		   실제 제거 로직은 schedule()의 맨 앞에서 호출됨. */
		if (curr && curr->status == THREAD_DYING && curr != initial_thread) {
			ASSERT (curr != next);
			list_push_back (&destruction_req, &curr->elem);
		}

		/* 스레드를 전환하기 전에, 먼저 현재 실행 중인 스레드의
		 * 정보를 저장함. */
		thread_launch (next);
	}
}

/* 새 스레드에 쓸 tid를 반환함. */
static tid_t
allocate_tid (void) {
	static tid_t next_tid = 1;
	tid_t tid;

	lock_acquire (&tid_lock);
	tid = next_tid++;
	lock_release (&tid_lock);

	return tid;
}
