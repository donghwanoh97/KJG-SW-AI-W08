# 주석 규칙 (Doxygen)

[← README로 돌아가기](../README.md)

함수와 파일에는 [Doxygen](https://www.doxygen.nl/manual/docblocks.html) 형식의 주석을 답니다. 본문은 한국어로 쓰고 문장 끝은 "~함", "~임" 체를 씁니다. 이 저장소의 `threads/thread.c`, `threads/synch.c`, `devices/timer.c`, `include/threads/*.h`와 Project 2에서 작성한 함수(`userprog/syscall.c`, `userprog/process.c`의 `process_*`/`load`/`__do_fork`/`duplicate_pte`, `userprog/exception.c`의 `page_fault`, `include/userprog/process.h`의 구조체)가 이 형식을 따릅니다.

## 기본 형식

주석은 `/** ... */`로 시작하고 함수 정의(또는 선언) 바로 위에 둡니다. 일반 `/* ... */` 주석은 Doxygen이 읽지 않습니다.

```c
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
 *
 * @see donate_priority()
 */
void
thread_requeue (struct thread *t) {
```

## 이 저장소에서 쓰는 태그

| 태그 | 쓰임 |
| --- | --- |
| `@file`, `@brief`, `@date` | 파일 맨 위. 파일 이름, 한 줄 설명, 작성일 |
| `@brief` | 함수가 하는 일을 한 줄로. 모든 함수에 답니다 |
| `@details` | 동작 방식, 호출 순서, 왜 그렇게 했는지 |
| `@param[in]`, `@param[out]`, `@param[in,out]` | 인자 설명. 읽기만 하면 `in`, 결과를 채우면 `out`, 읽고 바꾸면 `in,out` |
| `@return` | 반환값. 반환값이 없으면(`void`) 생략 |
| `@note` | 호출 조건과 제약. 예: "인터럽트가 꺼진 상태에서 호출해야 함" |
| `@see` | 같이 봐야 할 함수 |
| `@pre` | 호출 전에 만족해야 하는 조건 (드물게 사용) |

## 작성 원칙

- 코드를 읽으면 알 수 있는 것을 반복하지 말고, **왜 그렇게 했는지와 호출 조건**을 적습니다.
- 동작이 바뀌면 주석도 같이 고칩니다. 코드와 안 맞는 주석은 없는 것보다 나쁩니다.
- 인터럽트, lock, 정렬 같은 불변식 조건은 `@note`에 반드시 적습니다.
- 구조체 필드는 `/** */` 대신 필드 오른쪽에 `/* ... */`로 짧게 달아도 됩니다(`struct thread`처럼).
- 함수 본문 안의 주석은 `/* ... */`나 `//`를 씁니다. 본문 안에 `/** */`를 쓰면 Doxygen이 엉뚱한 곳에 붙입니다.
- 원본 Pintos에서 번역한 일부 함수(`load_segment`, `setup_stack`, `install_page` 등 손대지 않은 함수)는 아직 `/* ... */` 주석이라 Doxygen 형식이 아닙니다. 건드릴 때 같이 바꿔 주세요.

## HTML 문서로 보기

현재 저장소에는 `Doxyfile`이 없고 `doxygen`도 설치되어 있지 않습니다. 필요하면 직접 만들어서 쓰세요. Doxygen은 소스 파일만 읽으면 되므로 DevContainer 안이 아니라 **내 컴퓨터(호스트)에 설치해서 실행**해도 됩니다. 컨테이너 안에 설치하면 컨테이너를 다시 빌드할 때 사라집니다.

| 환경 | 설치 |
| --- | --- |
| Ubuntu (DevContainer 안 포함) | `sudo apt install doxygen graphviz` |
| macOS | `brew install doxygen graphviz` (Homebrew 필요) |
| Windows | `winget install DimitriVanHeesch.Doxygen`, `winget install Graphviz.Graphviz` 또는 [doxygen.nl/download](https://www.doxygen.nl/download.html)의 설치 파일 |

Windows에서는 설치 후 터미널을 새로 열어야 `doxygen` 명령이 잡힙니다. 아래는 저장소 루트에서 실행합니다(PowerShell, zsh, bash 모두 같습니다).

```bash
doxygen -g Doxyfile          # 설정 파일 생성
# Doxyfile에서 INPUT = pintos, RECURSIVE = YES, EXTRACT_ALL = YES 로 설정
doxygen Doxyfile             # html/index.html 생성
```

생성된 `html/index.html`은 브라우저로 열면 됩니다. `Doxyfile`과 `html/`은 커밋하지 않는 편이 좋습니다.
