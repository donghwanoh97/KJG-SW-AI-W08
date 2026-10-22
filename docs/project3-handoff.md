# Project 3 인수인계: P2 코드에서 바꿔야 할 곳

[← README로 돌아가기](../README.md)

`pintos/vm`으로 빌드하면 `VM` 매크로가 켜집니다. 이때 P2 코드 중 그대로 두면 틀리거나, P3 기능과 맞물려 손봐야 하는 곳을 정리합니다. 각 항목은 이 저장소 코드를 직접 확인한 내용입니다. 구현 방법은 적지 않고, **무엇이 왜 문제인지**만 적습니다.

## 1. 유저 포인터 검증이 lazy 페이지를 거부함 (`userprog/syscall.c`)

- `check_address()`는 `pml4_get_page()`가 `NULL`이면 `sys_exit(-1)`합니다. `check_buffer()`와 `copy_name()`도 이것을 씁니다.
- P3에서는 SPT에 등록만 되고 아직 프레임이 없는 페이지(lazy, swap out)가 정상입니다. 이런 주소를 넘긴 정상 프로그램이 `exit(-1)`로 죽습니다.
- 바꿀 방향: "페이지 테이블에 있는가"가 아니라 "SPT에 있는가(또는 스택 성장 대상인가)"로 판단해야 합니다. `spt_find_page()`가 기준이 됩니다.
- `read`는 유저 버퍼에 **씁니다**. 읽기 전용 페이지(코드 영역)를 버퍼로 넘기면 `exit(-1)`이어야 합니다(`pt-write-code2`). 지금 `check_buffer()`는 쓰기 가능 여부를 보지 않습니다.

## 2. 커널 모드 page fault 처리 (`userprog/exception.c`)

- `page_fault()`는 `vm_try_handle_fault()`가 실패하면, `user`일 때만 `sys_exit(-1)`하고 나머지는 `kill()`로 커널을 멈춥니다.
- 시스템 콜 처리 중 커널이 유저 버퍼를 읽다 난 fault는 `user == false`입니다. 이 경우 fault 주소가 유저 영역이면 커널 패닉이 아니라 프로세스 종료로 가야 하는지 팀에서 정해야 합니다(1번 항목의 검증을 어디까지 할지와 같이 정하세요).
- 스택 성장 판정에는 fault 당시 **유저** `rsp`가 필요합니다. 커널 모드 fault에서는 `f->rsp`가 커널 스택을 가리키므로, 시스템 콜 진입 시점의 유저 `rsp`를 `struct thread` 어딘가에 남겨 둬야 합니다(공식 문서 Stack Growth 참고). 지금 그런 필드는 없습니다.

## 3. `filesys_lock`과 page fault (`userprog/syscall.c`)

- P2의 `read`/`write`는 512바이트 커널 버퍼(`IO_CHUNK`)를 거치고, `create`/`remove`/`open`은 이름을 128바이트 버퍼(`NAME_BUF`)에 복사한 뒤 락을 잡습니다. **락을 쥔 채 유저 메모리를 건드리는 곳은 없습니다.**
- 그래서 lazy loading이나 mmap fault 처리 안에서 `file_read()`를 위해 `filesys_lock`을 잡아도 시스템 콜 쪽과 재귀 획득이 생기지 않습니다. 이 구조를 유지해 주세요.
- 새로 추가하는 코드도 같은 규칙을 지켜야 합니다. 특히 `mmap`, `munmap`, 프로세스 종료 시 write-back에서 락 안에서 유저 주소를 읽으면 같은 문제가 생깁니다.
- `fault 경로가 락을 전혀 안 잡기로` 정하면 `IO_CHUNK` 청킹은 직접 읽기/쓰기로 되돌려도 됩니다(`syscall.c`의 `IO_CHUNK` 주석).
- 128바이트를 넘는 파일 이름은 실패 처리됩니다. P4(긴 경로)에서 다시 봐야 합니다.

## 4. 프로세스 생명주기 (`userprog/process.c`)

| 위치 | 지금 동작 | P3에서 볼 점 |
| --- | --- | --- |
| `initd()`, `__do_fork()` | VM이면 `supplemental_page_table_init()` 호출 | `__do_fork()`는 VM이면 `pml4_for_each(duplicate_pte)` 대신 `supplemental_page_table_copy()`를 부름. 이 함수가 uninit 페이지와 이미 로드된 페이지를 모두 복제해야 `fork-*`가 통과함 |
| `process_exec()` | `process_cleanup()`으로 현재 주소 공간을 버린 뒤 `load()` | `process_cleanup()`이 `supplemental_page_table_kill()`을 부름. kill 뒤 같은 SPT에 `load()`가 다시 페이지를 넣으므로, kill이 SPT를 다시 쓸 수 있는 상태로 남겨야 함 |
| `process_exit()` | `fd_table_close_all()` → `process_cleanup()` → … → `file_close(using_file)` | mmap 페이지의 write-back은 SPT kill(= `process_cleanup()`)이 부르는 `file_backed_destroy()` 경로에서 하게 됨. mmap이 `file_reopen()`으로 자기 파일을 따로 들고 있어야 fd를 먼저 닫아도 안전함(`mmap-close`, `mmap-exit`) |
| `load()` | 실행 파일을 `using_file`로 열어 둔 채 `file_deny_write()` | lazy loading은 첫 fault 때 실행 파일을 읽으므로 실행 중 파일이 열려 있어야 함. `using_file`은 종료 때까지 닫히지 않음. 세그먼트마다 `aux`에 어떤 `struct file *`을 넘길지 정할 것 |
| `load()` 인자 전달 | `setup_stack()`이 만든 스택에 바로 `memcpy` | VM의 `setup_stack()`은 첫 스택 페이지를 **즉시 claim**해야 함(스켈레톤 주석). 그래야 이 `memcpy`가 fault 없이 동작함 |

## 5. 시스템 콜 디스패처 (`userprog/syscall.c`)

- `SYS_MMAP`, `SYS_MUNMAP`은 `include/lib/syscall-nr.h`에 번호만 있고 `syscall_handler()`에 case가 없습니다. 지금은 `default: break`로 조용히 무시됩니다.
- mmap 인자 검증(fd 0/1, 길이 0, 정렬 안 된 주소, 커널 주소, 기존 매핑과 겹침)은 `mmap-*` robustness 테스트가 각각 확인합니다. 잘못된 fd 처리는 P2와 같이 `fd_get()` 한 곳을 쓰면 됩니다.
- `SYS_DUP2`는 비어 있습니다(P2 extra, 미구현).

## 6. 그 밖에

- `SYS_EXEC`는 유저 문자열의 첫 바이트만 `check_address()`로 확인한 뒤 `strlen`합니다. lazy 페이지에서도, 페이지를 넘는 잘못된 문자열에서도 안전하지 않습니다. 1번 항목을 고칠 때 같이 보는 것을 추천합니다.
- `thread_create()`가 모든 스레드에 `child_info`를 `malloc`합니다. 메모리 압박 테스트(`swap-*`, `page-merge-*`)에서 실패 시 NULL 검사가 없습니다.
