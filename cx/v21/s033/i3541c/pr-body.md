Closes #3541

## 요지

connect 한 번에 I/O thread를 두 번 고르던 것을 한 번으로 줄였습니다. endpoint가 session의 thread를 고른 뒤(`socket_base_endpoint.cpp:533`), `session_base_t::start_connecting`이 connecter용 thread를 다시 골라(`choose_io_thread_transport`) 선택기가 connect당 두 칸씩 움직였습니다. 그래서 I/O thread 4개에서 일부 thread가 비거나 치우쳤습니다. 이제 connecter는 session의 `_io_thread`를 씁니다. engine도 원래 이 thread에 plug됩니다(`session_base.cpp:485`).

## 바꾼 것

- `core/src/runtime/core/session_base.cpp`: connecter thread 선택을 session의 `_io_thread` 재사용으로 바꿨습니다. connect당 판정 위치 2→1, 새 상태·옵션 없음
- `core/tests/unittest/unittest_connect_io_thread_choice.cpp`(새 파일)·`CMakeLists.txt`: I/O thread 4개 ctx에서 connect session 100개의 thread 분포를 TCP·TLS로 확인합니다. listener는 별도 ctx에 둬 측정 ctx에는 connect session만 잡히게 했습니다

## 검증

- 수정 전(제품 파일만 main으로 되돌림): TCP 분포 45/2/48/5, TLS 50/0/50/0으로 두 시험 모두 실패
- 수정 후: TCP·TLS 각 50회 반복 전부 통과, 매 실행 25/25/25/25
- Core 전체 빌드, Core unit 67/67 통과
- 로컬 전체 시험(`pre-pr.sh`, Core 1.25.0 소스 빌드): C++ 302/302, .NET Debug·Release 271/271, Java core test+integrationTest, Node `verify:ci`·Chromium E2E·cross-language smoke, `format.sh --check cpp dotnet java node` 모두 통과
- Core 수정이므로 다음 Core minor 릴리스에 들어갑니다(패치 릴리스 없음)

🤖 Generated with [Claude Code](https://claude.com/claude-code)

https://claude.ai/code/session_01D9jfewPT9LTVRsk1VFWTdu
