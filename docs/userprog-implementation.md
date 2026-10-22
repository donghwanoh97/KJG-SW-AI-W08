# Project 2 구현 방식

[← README로 돌아가기](../README.md)

함수 하나하나가 왜 그렇게 짜였는지는 소스의 Doxygen 주석(`@details`, `@note`)에 적어 뒀습니다. 이 문서는 함수들이 어떻게 맞물리는지를 정리합니다.

수정한 파일은 `pintos/userprog/process.c`, `pintos/userprog/syscall.c`, `pintos/userprog/exception.c`, `pintos/threads/thread.c`, `pintos/include/threads/thread.h`, `pintos/include/userprog/process.h`, `pintos/include/userprog/syscall.h`입니다.

## 1. `struct thread`에 추가한 필드 (`include/threads/thread.h`)

| 필드 | 의미 |
| --- | --- |
| `children` | 내가 만든 자식들의 `struct child_info` 리스트 (부모 → 자식) |
| `self_info` | 부모의 `children`에 들어 있는 내 `child_info` (자식 → 부모) |
| `fork_if` | `fork` 중인 부모의 유저 컨텍스트(`intr_frame`) 주소. `__do_fork()`가 읽어 감 |
| `exit_info` | `exit(status)`가 남긴 종료 상태 기록 (`malloc`) |
| `fd_table` | fd 테이블. `calloc`으로 `FD_MAX`(64)칸, 커널 스레드는 `NULL` |
| `using_file` | 실행 중인 실행 파일. `file_deny_write()`를 걸어 둔 채 종료 때 닫음 |

`children`, `self_info`, `fork_if`는 `#ifdef USERPROG` 밖에 있습니다.

## 2. 부모-자식 기록: `struct child_info` (`include/userprog/process.h`)

자식이 먼저 죽든 부모가 먼저 죽든 종료 상태를 안전하게 넘기기 위해, 상태를 `struct thread`가 아니라 **따로 `malloc`한 `child_info`**에 둡니다. `struct thread`는 자식이 죽으면 페이지째 해제되기 때문입니다.

| 필드 | 의미 |
| --- | --- |
| `tid` | 자식 tid |
| `exit_status` | 자식의 종료 상태 (기본값 -1) |
| `child_exited` / `parent_exited` | 누가 먼저 끝났는지. 나중에 끝난 쪽이 `free`함 |
| `wait_sema` | 자식이 종료 상태를 쓰면 `sema_up`, 부모의 `wait`가 `sema_down` |
| `fork_sema` / `fork_success` | `fork`에서 자식이 복제를 끝냈는지와 성공 여부 |

- 생성: `thread_create()`가 새 스레드마다 `child_info`를 만들고 현재 스레드의 `children`에 넣습니다.
- 해제 규칙: `process_exit()`에서 부모 역할(남은 `children` 정리)과 자식 역할(내 `self_info`에 상태 게시)을 둘 다 처리하고, 각각 인터럽트를 끈 채 `child_exited`/`parent_exited`를 보고 마지막 쪽이 `free`합니다. `wait`로 회수된 기록은 `process_wait()`가 `list_remove` 후 `free`합니다(같은 tid를 두 번 `wait`하면 -1).

## 3. 시스템 콜별 동작 (`userprog/syscall.c`, `userprog/process.c`)

### Argument Passing (`load()`)

`strtok_r`로 명령줄을 자르고, `setup_stack()` 뒤에 문자열을 스택 위에서부터 역순으로 복사한 다음 8바이트 정렬, `argv[]` 포인터 배열(끝은 `NULL`), 가짜 return address 순으로 쌓습니다. `rdi = argc`, `rsi = argv`, `rsp`는 가짜 return address를 가리킵니다. 인자는 최대 `MAX_ARGS - 1`개입니다.

### `exit` / 종료 메시지

- `sys_exit(status)`가 `self_info->exit_status`와 `exit_info`에 상태를 쓰고 `thread_exit()`를 부릅니다. 잘못된 포인터, 유저 모드 page fault도 모두 `sys_exit(-1)`로 들어옵니다.
- `name: exit(status)` 메시지는 **`process_exit()`에서만** 출력하고, `pml4`가 없는 커널 스레드는 출력하지 않습니다.
- `process_exit()` 순서: 메시지 출력 → `fd_table_close_all()` → `process_cleanup()`(주소 공간 해제) → 자식 기록 정리 → 내 상태 게시(`sema_up`) → `using_file` 닫기.

### `fork`

1. 부모가 `fork_if`에 자기 `intr_frame` 주소를 남기고 `thread_create(..., __do_fork, parent)` 후 자식의 `fork_sema`에서 기다립니다. 부모가 기다리는 동안 부모 커널 스택의 `intr_frame`이 살아 있으므로 주소만 넘겨도 안전합니다.
2. 자식은 `__do_fork()`에서 `intr_frame`을 복사하고, `pml4_for_each(duplicate_pte)`로 유저 페이지를 하나씩 복제하고, `file_duplicate()`로 fd 테이블을 복제합니다.
3. 성공하면 `fork_success = true`, `sema_up`, `rax = 0`으로 `do_iret`. 실패하면 `fork_success = false`, `sema_up` 뒤 `thread_exit()`. 부모는 실패 시 `TID_ERROR`를 반환합니다.

### `exec`

유저 문자열을 `palloc_get_page`로 받은 커널 페이지에 복사한 뒤 `process_exec()`를 부릅니다. `process_exec()`는 현재 주소 공간을 `process_cleanup()`으로 버리고 `load()`합니다. 실패하면 `sys_exit(-1)`입니다.

### `wait`

`children`에서 tid를 찾아 `wait_sema`에서 기다린 뒤 `exit_status`를 돌려주고 기록을 지웁니다. 직계 자식이 아니면 바로 -1입니다.

### deny-write (`rox-*`)

`load()`가 실행 파일을 연 직후 `file_deny_write()`를 걸고 `using_file`에 보관합니다. `process_exit()` 마지막의 `file_close()`가 쓰기 금지를 풀어 줍니다.

## 4. fd 테이블과 파일 시스템 콜

- 테이블: `fd_table_create()`가 `calloc(FD_MAX, ...)`. 0, 1은 stdin/stdout 예약이라 테이블에는 2번부터 들어갑니다. `fd_add()`는 비어 있는 가장 작은 번호를 줍니다.
- 범위 검사는 `fd_get()` 한 곳에서만 합니다. 잘못된 fd는 **exit하지 않고** 실패값(-1 또는 무시)을 돌려줍니다.
- `fork`는 `file_duplicate()`로 같은 위치(offset)를 가진 새 `struct file`을 만듭니다. 부모와 자식이 `struct file`을 공유하지 않습니다.
- 종료 때 `fd_table_close_all()`이 전부 닫고 테이블을 `free`합니다.

## 5. 락 규칙과 유저 메모리 접근

- 전역 `filesys_lock` 하나만 씁니다. `file_*`, `filesys_*`를 부르는 곳만 감싸고, 재귀로 잡지 않습니다.
- **락을 쥔 채 유저 메모리를 건드리지 않습니다.**
  - 파일 이름(`create` `remove` `open`)은 `copy_name()`으로 128바이트(`NAME_BUF`) 커널 버퍼에 먼저 복사합니다. 더 긴 이름은 실패 처리합니다.
  - `read`/`write`는 512바이트(`IO_CHUNK`) 커널 스택 버퍼를 거칩니다. 락 안에서는 커널 버퍼만 만지고, 유저 버퍼와의 복사는 락 밖에서 합니다.
  - 이유: Project 3에서 lazy loading이나 mmap 때문에 유저 버퍼 접근 중 page fault가 나고, fault 처리가 파일을 읽느라 `filesys_lock`을 잡으면, 이미 락을 쥔 상태라 패닉이 납니다. P2에서는 필요 없지만 미리 이렇게 만들어 뒀습니다.
- 유저 포인터 검증: `check_address()`(주소 하나), `check_buffer()`(버퍼가 걸친 모든 페이지), `copy_name()`(문자열을 복사하며 새 페이지마다 검사). 모두 `is_user_vaddr()` + `pml4_get_page()`로 확인하고 실패하면 `sys_exit(-1)`입니다.

## 6. 알려진 한계

| 항목 | 내용 |
| --- | --- |
| `dup2` | 미구현. `SYS_DUP2` case가 비어 있고 `tests/userprog/dup2`는 `userprog/Make.vars`에서 주석 처리됨 |
| `SYS_EXIT` case | `sys_exit()`와 같은 일을 handler 안에서 다시 함. `sys_exit()` 호출로 합쳐도 됨 |
| `SYS_EXEC` | `palloc_get_page()`에 플래그 자리로 문자열 길이를 넘김. 첫 페이지만 검사한 뒤 `strlen`을 하므로 페이지 경계를 넘는 잘못된 문자열, 4KB보다 긴 문자열을 막지 못함 |
| `thread_create()` | 모든 스레드(커널 스레드 포함)에 `child_info`를 `malloc`하고 NULL 검사가 없음 |
| `seek` | `position`이 `INT32_MAX`보다 크면 무시함. `off_t`(32비트)로 바뀌며 음수가 되어 `file_seek()`의 ASSERT로 패닉이 나는 것을 막음 |
| 유저 포인터 검증 | `pml4_get_page()` 기반이라 Project 3의 lazy 페이지와 맞지 않음. [인수인계 문서](project3-handoff.md) 참고 |
