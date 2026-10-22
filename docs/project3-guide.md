# Project 3 시작 가이드 (Virtual Memory)

[← README로 돌아가기](../README.md)

Project 3을 시작하는 팀이 처음 하루 동안 볼 것을 정리합니다. 공식 문서의 [Project 3 소개](https://casys-kaist.github.io/pintos-kaist/project3/introduction.html)를 먼저 읽고 이 문서로 저장소 위치를 맞춰 보세요. 이 문서는 코드 대신 무엇을 어디서 해야 하는지만 적습니다.

## 1. 범위와 배점 (`pintos/tests/vm/Grading`)

| 비중 | 묶음 |
| --- | --- |
| 60% | `tests/vm/Rubric.functionality` (스택 성장, 페이징, mmap, swap, lazy loading) |
| 20% | `tests/vm/Rubric.robustness` (잘못된 주소, 코드 영역 쓰기, mmap 예외) |
| 8% + 5% | `tests/userprog` functionality / robustness (**P2 테스트를 VM 빌드에서 다시 통과해야 함**) |
| 5% | `tests/filesys/base` |
| 1% + 1% | threads alarm / priority |
| +25% (extra) | `tests/vm/cow` (Copy-on-Write, `cow-simple`) |

빌드와 테스트는 `pintos/vm`에서 합니다. `vm/Make.vars`가 `-DUSERPROG -DFILESYS -DVM`을 켜므로 `process.c`의 `#ifdef VM` 블록과 `exception.c`의 `vm_try_handle_fault()` 호출이 살아납니다.

```bash
cd pintos/vm
make check
```

**처음 상태**: `vm_alloc_page_with_initializer()`, `setup_stack()` 등이 비어 있어서, VM 빌드에서는 P2에서 통과하던 테스트도 실패하는 상태로 시작합니다. 예를 들어 `args-none`은 `load()`가 실패해 `initd()`의 `PANIC("Fail to launch initd")`에서 멈춥니다. lazy loading까지 끝내면 P2 테스트가 다시 통과해야 정상입니다(공식 문서 기준).

## 2. 테스트 묶음

| 묶음 | 테스트 | 필요한 기능 |
| --- | --- | --- |
| Lazy loading | `lazy-anon` `lazy-file` | SPT, uninit 페이지, page fault에서 claim |
| 스택 성장 | `pt-grow-stack` `pt-grow-stk-sc` `pt-big-stk-obj` `pt-grow-bad` | fault 주소가 스택 근처면 새 anon 페이지 |
| 잘못된 접근 | `pt-bad-addr` `pt-bad-read` `pt-write-code` `pt-write-code2` | SPT에 없는 주소, 읽기 전용 페이지 쓰기 → `exit(-1)` |
| 페이징 | `page-linear` `page-parallel` `page-shuffle` `page-merge-*` | 프레임 관리, 여러 프로세스 |
| mmap | `mmap-*` | `SYS_MMAP`/`SYS_MUNMAP`, file-backed 페이지, write-back |
| swap | `swap-anon` `swap-file` `swap-iter` `swap-fork` | 프레임 테이블, 희생자 선택, swap disk |
| COW (extra) | `cow-simple` | fork 때 페이지 공유, 쓰기 fault에서 복사 |

`child-*`, `qsort`, `parallel-merge`는 다른 테스트가 실행하는 보조 프로그램입니다.

## 3. 추천 구현 순서

공식 문서의 순서를 따르되, 각 단계가 끝날 때 확인할 테스트를 붙였습니다.

1. **SPT (Supplemental Page Table)**: `include/vm/vm.h`의 `struct supplemental_page_table`(지금 비어 있음)과 `struct page`의 `/* Your implementation */` 자리를 채우고, `vm/vm.c`의 `supplemental_page_table_init` / `spt_find_page` / `spt_insert_page` / `spt_remove_page`를 구현합니다. 자료구조는 `include/lib/kernel/hash.h`의 해시 테이블이 공식 문서의 추천입니다.
2. **프레임**: `vm_get_frame` / `vm_claim_page` / `vm_do_claim_page`. 이 단계에서는 eviction 없이 `palloc_get_page(PAL_USER)` 실패 시 `PANIC`으로 둬도 됩니다(공식 문서 허용).
3. **Anonymous page와 lazy loading**: `vm_alloc_page_with_initializer`, `vm/uninit.c`, `vm/anon.c`의 initializer, `process.c` VM 블록의 `load_segment` / `lazy_load_segment` / `setup_stack`, 그리고 `vm_try_handle_fault`. 끝나면 **P2 테스트 전체**와 `lazy-*`를 돌려 봅니다.
4. **fork와 exit의 SPT**: `supplemental_page_table_copy`(`__do_fork()`가 VM일 때 부름)와 `supplemental_page_table_kill`(`process_cleanup()`이 부름). `fork-*`, `multi-*`가 기준입니다.
5. **스택 성장**: `vm_stack_growth`와 fault 판정. 스택 최대 크기는 공식 문서 기준 1MB입니다. `pt-grow-*`.
6. **mmap**: `vm/file.c`의 `do_mmap` / `do_munmap`와 file-backed 페이지, `syscall.c`에 `SYS_MMAP` / `SYS_MUNMAP` case 추가. `mmap-*`.
7. **swap**: `vm/anon.c`의 `swap_disk`(지금 `NULL`), `anon_swap_in/out`, `vm/file.c`의 `file_backed_swap_in/out`, `vm_get_victim` / `vm_evict_frame`. `swap-*`, `page-merge-*`.
8. **COW (extra)**: `vm_handle_wp`와 fork 경로.

## 4. 스켈레톤 위치

| 파일 | 할 일 |
| --- | --- |
| `pintos/include/vm/vm.h` | `struct page`, `struct frame`, `struct supplemental_page_table` 필드 |
| `pintos/vm/vm.c` | SPT, 프레임, fault 처리, 스택 성장, SPT 복사/해제 |
| `pintos/vm/uninit.c` | 첫 fault 때 실제 타입으로 바뀌는 페이지 (대부분 이미 있음, `uninit_destroy`만 TODO) |
| `pintos/vm/anon.c`, `include/vm/anon.h` | 익명 페이지, swap 슬롯 |
| `pintos/vm/file.c`, `include/vm/file.h` | file-backed 페이지, mmap |
| `pintos/userprog/process.c` (`#ifdef VM` 블록, 파일 끝부분) | `lazy_load_segment`, `load_segment`, `setup_stack` |
| `pintos/userprog/exception.c` | `page_fault()`가 이미 `vm_try_handle_fault()`를 먼저 부름 |
| `pintos/userprog/syscall.c` | mmap 시스템 콜, 유저 포인터 검증 수정 |

`DO NOT MODIFY` 주석이 달린 함수(`vm_dealloc_page` 등)는 건드리지 않습니다.

## 5. 역할 분담 전에 합의할 것

P2에서 `struct thread` 필드를 먼저 합의하지 않아 겹친 적이 있습니다. P3에서는 아래를 먼저 정해 두는 것을 추천합니다.

- `struct page`에 넣을 공통 필드(쓰기 가능 여부, SPT용 hash elem 등)와 `aux`로 넘길 정보의 구조체 이름
- 프레임 테이블을 전역 리스트로 둘지, 그 락은 무엇인지
- page fault 처리 경로가 `filesys_lock`을 잡을지 여부 (P2의 `read`/`write`는 잡아도 안전하게 만들어 둠, [인수인계 문서](project3-handoff.md) 참고)
- 유저 포인터 검증 함수를 SPT 기반으로 바꾸는 담당

## 6. 참고

- 공식 문서: [Memory Management](https://casys-kaist.github.io/pintos-kaist/project3/vm_management.html), [Anonymous Page](https://casys-kaist.github.io/pintos-kaist/project3/anon.html), [Stack Growth](https://casys-kaist.github.io/pintos-kaist/project3/stack_growth.html), [Memory Mapped Files](https://casys-kaist.github.io/pintos-kaist/project3/memory_mapped_files.html), [Swap In/Out](https://casys-kaist.github.io/pintos-kaist/project3/swapping.html)
- Stanford Pintos의 VM 자료는 페이지 테이블(`pagedir`)과 함수 이름이 다릅니다. 이 저장소는 `pml4_*`를 씁니다.
