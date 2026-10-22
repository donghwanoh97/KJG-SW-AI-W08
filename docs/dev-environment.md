# 개발 환경과 테스트

[← README로 돌아가기](../README.md)

개발 환경은 Docker + VSCode DevContainer(`ubuntu:22.04`, x86-64, QEMU)를 씁니다. VSCode에서 `Dev Containers: Reopen in Container`로 열면 되고, 9~13주차 동안 같은 환경입니다.

```bash
cd pintos
source ./activate          # pintos 명령을 PATH에 추가 (DevContainer 터미널에서는 자동)
cd threads                 # Project 2는 userprog, Project 3은 vm
make                       # 빌드
make check                 # 전체 테스트
```

한 테스트만 돌리려면 `build` 디렉터리에서 실행합니다.

```bash
cd build
make tests/threads/priority-donate-one.result
```

## 테스트 실행/디버깅 도구: pintos-util

[jacti/pintos-util](https://github.com/jacti/pintos-util)은 Pintos 프로젝트 폴더에 복사해서 쓰는 테스트 실행/디버깅 유틸 모음입니다. 테스트를 메뉴에서 골라 실행하고, VS Code + GDB 디버깅을 쉽게 해 줍니다. 이 저장소의 `pintos/threads`, `userprog`, `vm`, `filesys`에는 같은 이름의 `select_test.sh`와 `.test_config`가 이미 들어 있습니다.

```bash
cd pintos/threads          # userprog, vm, filesys도 같은 방식
./select_test.sh -q -r     # 테스트 메뉴, clean 후 재빌드
```

- 옵션: `-q` 일반 실행, `-g` GDB 스텁(빌드 생략), `-r` clean 후 재빌드
- VS Code 디버깅은 pintos-util의 `launch.json`을 `.vscode/launch.json`으로 복사하고 `program`, `cwd`, `miDebuggerPath`를 본인 환경에 맞게 고쳐서 씁니다.
- `.test_config`는 `generate_test_config.py`로 `make check` 출력에서 만들 수 있습니다. 자세한 사용법은 pintos-util의 문서(`docs/usage.md`, `docs/test-config.md`)를 참고해 주세요.
- 스크립트는 상위 경로의 `activate`를 `source`하므로 `pintos/activate`가 있어야 합니다.

**주의**: `pintos` 명령이 PATH에 없는 상태에서 `make check`를 돌리면 빈 `.output`과 `.result`가 남아, 이후에 PATH를 고쳐도 해당 테스트가 FAIL로 보일 수 있습니다. 이때는 `build/tests/threads/<테스트>.{output,errors,result}`를 지우고 다시 실행해 주세요.
