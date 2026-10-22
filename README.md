# SW-AI-W08 · KAIST Pintos (x86-64) Project 2: User Programs

크래프톤 정글 SW-AI Lab 13기 8주차 팀 프로젝트입니다. KAIST Pintos(x86-64)의 Project 2(User Programs)를 구현했고, Project 3(Virtual Memory)를 시작하는 팀이 이어서 작업할 수 있도록 진행 상황과 구현 방식, 넘겨줄 주의점을 문서로 남깁니다.

> 이 저장소는 **KAIST Pintos(64비트)** 기준입니다. Stanford Pintos(32비트) 자료는 함수 이름, 구조체, 레지스터 이름이 다를 수 있으니 이 저장소의 코드와 먼저 대조해 주세요.
> 공식 문서: https://casys-kaist.github.io/pintos-kaist/

## 팀 구성

<table align="center">
  <tr>
    <td align="center" width="33%">
      <a href="https://github.com/NARi3040"><img src="https://github.com/NARi3040.png?size=200" width="150" alt="박준호"></a><br><br>
      <a href="https://github.com/NARi3040"><b>박준호</b></a>
    </td>
    <td align="center" width="33%">
      <a href="https://github.com/donghwanoh97"><img src="https://github.com/donghwanoh97.png?size=200" width="150" alt="오동환"></a><br><br>
      <a href="https://github.com/donghwanoh97"><b>오동환</b></a>
    </td>
    <td align="center" width="33%">
      <a href="https://github.com/jye-1123"><img src="https://github.com/jye-1123.png?size=200" width="150" alt="지영은"></a><br><br>
      <a href="https://github.com/jye-1123"><b>지영은</b></a>
    </td>
  </tr>
</table>

### 학습 및 코드 관리 방식

- 테스트 통과보다 **공부를 우선**으로 진행했습니다.
- 역할은 1번 IO(fd 테이블, 파일 시스템 콜) / 2번 종료·검증(`exit` `wait` `exec` `halt`, 유저 포인터 검증, page fault) / 3번 fork·보호(`fork`, `load`, deny-write)로 나눴습니다.
- 코어타임에 모여 코드 리뷰를 한 뒤, 살아남은 코드만 `main`에 머지합니다.

## 현재 상태

**Project 2 필수 범위 완료.** `pintos/userprog`에서 `make check` 결과 **95개 전부 통과**입니다(userprog, filesys/base, no-vm/multi-oom, threads alarm/priority 포함).

| 영역 | 상태 |
| --- | --- |
| Argument Passing | 완료 |
| 유저 메모리 검증 | 완료 (`check_address`, `check_buffer`, `copy_name`) |
| `halt` `exit` `exec` `wait` `fork` | 완료 |
| 파일 시스템 콜 (`create` `remove` `open` `close` `read` `write` `filesize` `seek` `tell`) | 완료 |
| 종료 메시지 `name: exit(status)` | 완료 |
| 실행 중인 파일 쓰기 금지 (`rox-*`) | 완료 |
| `dup2` (extra) | **미구현** (`SYS_DUP2` case만 있음, 테스트도 `Make.vars`에서 주석 처리) |
| Project 1 MLFQS | 미구현 (P2/P3 채점에 없음) |

## 문서

Project 3을 시작한다면 위에서부터 순서대로 읽는 것을 추천합니다.

| 문서 | 내용 |
| --- | --- |
| [Project 2 구현 방식](docs/userprog-implementation.md) | 프로세스 구조체, fork/wait/exit 동기화, fd 테이블, 락 규칙, 알려진 한계 |
| [Project 3 시작 가이드](docs/project3-guide.md) | 범위와 배점, 테스트 묶음, 추천 구현 순서, 스켈레톤 위치 |
| [Project 3 인수인계: P2 코드에서 바꿔야 할 곳](docs/project3-handoff.md) | VM을 켜면 깨지거나 손봐야 하는 P2 코드 체크리스트 |
| [개발 환경과 테스트](docs/dev-environment.md) | DevContainer, 빌드와 테스트 실행, pintos-util |
| [주석 규칙](docs/comment-style.md) | Doxygen 주석 형식과 HTML 문서 생성. P2에서 작성한 함수의 설계 의도는 소스의 Doxygen 주석에 있음 |
| [스레드 사용 가이드 (P1)](docs/thread-guide.md) | 스케줄러 동작과 지켜야 할 불변식 |
| [P1 구현 방식](docs/implementation.md) | Alarm Clock, Priority Scheduling, Priority Donation |

핵심 규칙 네 가지만 먼저 기억해 주세요.

- 파일 시스템 함수(`file_*`, `filesys_*`)는 전역 `filesys_lock`을 잡고 호출합니다. **락을 쥔 채 유저 메모리를 건드리지 않습니다** (P3에서 page fault가 같은 락을 잡으면 패닉).
- 유저 포인터는 시스템 콜에서 쓰기 전에 검증하고, 잘못되면 `sys_exit(-1)`로 종료합니다.
- 종료 메시지는 `process_exit()` 한 곳에서만 출력합니다.
- `ready_list`는 항상 priority 내림차순입니다. READY 스레드의 priority를 바꾸면 `thread_requeue()`를 호출해야 합니다 (P1).

## 빠른 시작

VSCode에서 `Dev Containers: Reopen in Container`로 열고, 컨테이너 터미널에서 실행합니다. 호스트의 최신 gcc로는 빌드가 깨질 수 있으니 반드시 컨테이너 안에서 빌드해 주세요.

```bash
cd pintos/userprog         # Project 3은 pintos/vm
make check                 # 전체 테스트
```

한 테스트만 돌리거나 `pintos` 명령이 안 잡힐 때의 주의사항은 [개발 환경과 테스트](docs/dev-environment.md)에 있습니다.
