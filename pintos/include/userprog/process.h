#ifndef USERPROG_PROCESS_H
#define USERPROG_PROCESS_H

#include "threads/thread.h"

tid_t process_create_initd (const char *file_name);
tid_t process_fork (const char *name, struct intr_frame *if_);
int process_exec (void *f_name);
int process_wait (tid_t);
void process_exit (void);
void process_activate (struct thread *next);

/**
 * @brief 유저 프로그램이 exit(status)로 남긴 종료 상태 기록
 *
 * @details
 * sys_exit()가 malloc해서 thread->exit_info에 달고, process_exit()가 종료 메시지를
 * 찍은 뒤 free함. 부모에게 넘기는 값은 child_info.exit_status 쪽이고,
 * 이 구조체는 종료 메시지용임.
 */
struct exit_info {
    int status;
};

/**
 * @brief 부모와 자식이 공유하는 자식 프로세스 기록 (wait, fork 동기화용)
 *
 * @details
 * 종료 상태를 struct thread에 두지 않고 따로 malloc한 이유: 자식이 먼저 죽으면
 * struct thread는 페이지째 해제되지만, 부모는 그 뒤에 wait로 상태를 받아야 함.
 * 그래서 양쪽보다 오래 살 수 있는 별도 기록을 두고, 부모는 children 리스트로,
 * 자식은 self_info 포인터로 같은 기록을 가리킴.
 *
 * wait 대상이 되는 유저 프로세스에만 필요하므로 thread_create()가 아니라
 * process_create_initd()와 process_fork()가 만듦(child_info_create()). 커널 스레드에는 없음.
 * 해제는 다음 중 하나가 함.
 * - 부모가 wait로 회수하면 process_wait()
 * - fork가 실패하면 process_fork() (자식의 process_exit()가 끝난 뒤)
 * - 그 밖에는 부모와 자식 중 나중에 끝나는 쪽의 process_exit()
 *
 * @note child_exited, parent_exited는 인터럽트를 끈 채로 읽고 써야 함
 */
struct child_info {
    tid_t tid;                    /* 자식 tid. 부모가 children에서 찾을 때 씀 */
    int exit_status;              /* 자식 종료 상태. 기본값 -1 */
    bool child_exited;            /* 자식이 process_exit()를 마쳤는지 */
    bool parent_exited;           /* 부모가 먼저 종료했는지. true면 자식이 직접 free함 */
    struct semaphore wait_sema;   /* 자식 종료 시 up, 부모 wait에서 down */
    struct semaphore fork_sema;   /* __do_fork() 복제 완료 시 up, process_fork()에서 down */
    bool fork_success;            /* 복제 성공 여부. fork_sema를 올리기 전에 씀 */
    struct list_elem elem;        /* 부모의 children 리스트 원소 */
};

/**
 * @brief process_fork()가 thread_create()의 aux로 자식에게 넘기는 인자 묶음
 *
 * @note 부모 커널 스택의 지역 변수임. 부모는 fork_sema에서 기다리므로 자식이
 *       fork_sema를 올리기 전까지만 유효함. 그 뒤에는 접근하면 안 됨
 */
struct fork_args {
    struct thread *parent;        /* 복제할 부모 */
    struct intr_frame *parent_if; /* 시스템 콜 진입 시점의 부모 유저 레지스터 */
    struct child_info *info;      /* 자식이 self_info로 가져갈 기록 */
};

/**
 * @brief process_create_initd()가 initd에게 넘기는 인자 묶음 (malloc, initd가 free)
 *
 * @note main 스레드는 기다려 주지 않으므로 지역 변수가 아니라 heap에 둠
 */
struct initd_args {
    char *fn_copy;                /* 명령줄 페이지 (process_exec()가 해제) */
    struct child_info *info;      /* initd가 self_info로 가져갈 기록 */
};

#endif /* userprog/process.h */
