#include "userprog/exception.h"
#include <inttypes.h>
#include <stdio.h>
#include "userprog/gdt.h"
#include "threads/interrupt.h"
#include "threads/thread.h"
#include "intrinsic.h"

/* 처리한 페이지 폴트(page fault)의 수. */
static long long page_fault_cnt;

static void kill (struct intr_frame *);
static void page_fault (struct intr_frame *);

/* 유저 프로그램이 일으킬 수 있는 인터럽트의 핸들러를 등록함.

   실제 Unix 계열 OS에서는 이런 인터럽트 대부분이 [SV-386] 3-24와
   3-25에 설명된 대로 시그널(signal) 형태로 유저 프로세스에 전달되지만,
   여기서는 시그널을 구현하지 않음.  대신 단순히 유저 프로세스를
   종료시키도록 함.

   페이지 폴트(page fault)는 예외임.  여기서는 다른 예외와 똑같이
   처리하지만, 가상 메모리(virtual memory)를 구현하려면 이 부분을
   바꿔야 함.

   각 예외에 대한 설명은 [IA32-v3a] 5.15절 "Exception and Interrupt
   Reference"를 참고할 것. */
void
exception_init (void) {
	/* 이 예외들은 유저 프로그램이 INT, INT3, INTO, BOUND 명령어 등으로
	   명시적으로 발생시킬 수 있음.  따라서 DPL==3으로 설정하며, 이는
	   유저 프로그램이 이 명령어들을 통해 예외를 호출하는 것을
	   허용한다는 뜻임. */
	intr_register_int (3, 3, INTR_ON, kill, "#BP Breakpoint Exception");
	intr_register_int (4, 3, INTR_ON, kill, "#OF Overflow Exception");
	intr_register_int (5, 3, INTR_ON, kill,
			"#BR BOUND Range Exceeded Exception");

	/* 이 예외들은 DPL==0이므로 유저 프로세스가 INT 명령어로 직접
	   호출할 수 없음.  하지만 간접적으로는 여전히 발생할 수 있음.
	   예를 들어 #DE는 0으로 나누면 발생할 수 있음.  */
	intr_register_int (0, 0, INTR_ON, kill, "#DE Divide Error");
	intr_register_int (1, 0, INTR_ON, kill, "#DB Debug Exception");
	intr_register_int (6, 0, INTR_ON, kill, "#UD Invalid Opcode Exception");
	intr_register_int (7, 0, INTR_ON, kill,
			"#NM Device Not Available Exception");
	intr_register_int (11, 0, INTR_ON, kill, "#NP Segment Not Present");
	intr_register_int (12, 0, INTR_ON, kill, "#SS Stack Fault Exception");
	intr_register_int (13, 0, INTR_ON, kill, "#GP General Protection Exception");
	intr_register_int (16, 0, INTR_ON, kill, "#MF x87 FPU Floating-Point Error");
	intr_register_int (19, 0, INTR_ON, kill,
			"#XF SIMD Floating-Point Exception");

	/* 대부분의 예외는 인터럽트를 켠 상태로 처리할 수 있음.
	   페이지 폴트는 폴트 주소가 CR2에 저장되어 있고 이를 보존해야
	   하므로 인터럽트를 꺼야 함. */
	intr_register_int (14, 0, INTR_OFF, page_fault, "#PF Page-Fault Exception");
}

/* 예외 통계를 출력함. */
void
exception_print_stats (void) {
	printf ("Exception: %lld page faults\n", page_fault_cnt);
}

/* 유저 프로세스가 (아마도) 일으킨 예외의 핸들러. */
static void
kill (struct intr_frame *f) {
	/* 이 인터럽트는 (아마도) 유저 프로세스가 일으킨 것임.
	   예를 들어 프로세스가 매핑되지 않은 가상 메모리에 접근하려 했을
	   수 있음(페이지 폴트).  지금은 단순히 유저 프로세스를 종료시킴.
	   나중에는 커널에서 페이지 폴트를 처리하도록 해야 함.  실제 Unix
	   계열 운영체제는 대부분의 예외를 시그널(signal)로 프로세스에
	   되돌려 보내지만, 여기서는 시그널을 구현하지 않음. */

	/* 인터럽트 프레임(intr_frame)의 코드 세그먼트 값으로 예외가 어디서
	   발생했는지 알 수 있음. */
	switch (f->cs) {
		case SEL_UCSEG:
			/* 유저의 코드 세그먼트이므로 예상대로 유저 예외임.
			   유저 프로세스를 종료시킴.  */
			printf ("%s: dying due to interrupt %#04llx (%s).\n",
					thread_name (), f->vec_no, intr_name (f->vec_no));
			intr_dump_frame (f);
			thread_exit ();

		case SEL_KCSEG:
			/* 커널의 코드 세그먼트이며, 이는 커널 버그를 의미함.
			   커널 코드는 예외를 일으키면 안 됨.  (페이지 폴트는
			   커널 예외를 일으킬 수 있지만, 여기까지 도달하면 안 됨.)
			   이를 분명히 하기 위해 커널을 패닉(panic) 시킴.  */
			intr_dump_frame (f);
			PANIC ("Kernel bug - unexpected interrupt in kernel");

		default:
			/* 그 밖의 다른 코드 세그먼트?  일어나면 안 되는 일임.
			   커널을 패닉(panic) 시킴. */
			printf ("Interrupt %#04llx (%s) in unknown segment %04x\n",
					f->vec_no, intr_name (f->vec_no), f->cs);
			thread_exit ();
	}
}

/* 페이지 폴트(page fault) 핸들러.  가상 메모리를 구현하려면 채워
   넣어야 하는 뼈대 코드임.  project 2의 일부 해법에서도 이 코드를
   수정해야 할 수 있음.

   진입 시점에 폴트가 난 주소는 CR2(Control Register 2)에 있고,
   exception.h의 PF_* 매크로에 설명된 형식의 폴트 정보는 F의
   error_code 멤버에 있음.  여기 있는 예제 코드는 그 정보를 파싱하는
   방법을 보여줌.  둘 모두에 대한 더 자세한 정보는 [IA32-v3a] 5.15절
   "Exception and Interrupt Reference"의 "Interrupt 14--Page Fault
   Exception (#PF)" 설명에서 찾을 수 있음. */
static void
page_fault (struct intr_frame *f) {
	bool not_present;  /* True: 존재하지 않는 페이지, false: 읽기 전용 페이지에 쓰기. */
	bool write;        /* True: 쓰기 접근, false: 읽기 접근. */
	bool user;         /* True: 유저의 접근, false: 커널의 접근. */
	void *fault_addr;  /* 폴트 주소. */

	/* 폴트가 난 주소, 즉 폴트를 일으킨 접근 대상 가상 주소를 가져옴.
	   코드를 가리킬 수도, 데이터를 가리킬 수도 있음.  폴트를 일으킨
	   명령어의 주소와 반드시 같지는 않음 (그건 f->rip임). */

	fault_addr = (void *) rcr2();

	/* 인터럽트를 다시 켬 (CR2가 바뀌기 전에 확실히 읽으려고
	   잠시 꺼 둔 것뿐이었음). */
	intr_enable ();


	/* 원인을 판별함. */
	not_present = (f->error_code & PF_P) == 0;
	write = (f->error_code & PF_W) != 0;
	user = (f->error_code & PF_U) != 0;

#ifdef VM
	/* project 3 이후용. */
	if (vm_try_handle_fault (f, fault_addr, user, write, not_present))
		return;
#endif

	/* 페이지 폴트 횟수를 셈. */
	page_fault_cnt++;
	if(user){
		sys_exit(-1);
	}
	/* 진짜 폴트라면 정보를 출력하고 종료함. */
	printf ("Page fault at %p: %s error %s page in %s context.\n",
			fault_addr,
			not_present ? "not present" : "rights violation",
			write ? "writing" : "reading",
			user ? "user" : "kernel");
	kill (f);
}

