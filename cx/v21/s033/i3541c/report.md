# #3541 Core 수정 완료

**결과**
- 측정 ctx에서 connect session만 선택하게 테스트 fixture를 고쳤습니다. publisher/listener를 별도 ctx로 옮겼습니다: `core/tests/unittest/unittest_connect_io_thread_choice.cpp:62-70`.
- 판정 위치는 connect당 **2곳 → 1곳**입니다. 기존에는 endpoint의 선택 `socket_base_endpoint.cpp:533`과 connecter의 재선택 `session_base.cpp:853`이 있었고, 이제 connecter는 session의 `_io_thread`를 재사용합니다: `core/src/runtime/core/session_base.cpp:852-854`. round-robin 정책 소유자는 ctx registry이며 증가 위치는 `ctx_io_thread_registry.cpp:99`; 계약은 `core/doc/spec/core/systems/03-io-thread.ko.md:147-153`입니다.
- 분류: **B — 기존 중복 선택 결함**. Framework runtime 변경이 아니므로 언어별 Framework parity 대조 대상은 아닙니다. Core의 공통 경로로 TCP와 TLS 모두 확인했습니다.

**바꾼 것**
- `core/tests/unittest/unittest_connect_io_thread_choice.cpp:62-70,91-108,137-145`: listener ctx 분리와 측정 ctx 초기화, connect session 분포 assertion.
- `core/tests/unittest/CMakeLists.txt:60`: 새 Core unit test 등록.
- Core 수정은 connecter가 이미 배정된 session thread를 사용하도록 유지했습니다: `core/src/runtime/core/session_base.cpp:852-853`.

**검증**
- 수정 전 negative check: TCP `45/2/48/5`, TLS `50/0/50/0`; 둘 다 분산 assertion 실패. [negative.log](/root/jobs/fx/cx/v21/s033/i3541c/negative.log)
- 수정 후 TCP/TLS test 50회 전부 통과, 각 실행 `25/25/25/25`. [fixed-50.log](/root/jobs/fx/cx/v21/s033/i3541c/fixed-50.log)
- 전체 Core build 완료: [core-build-all.log](/root/jobs/fx/cx/v21/s033/i3541c/core-build-all.log)
- Core unit suite 67/67 통과: [core-unit-final.log](/root/jobs/fx/cx/v21/s033/i3541c/core-unit-final.log)
- `bash scripts/format/format.sh --check cpp` 통과: [format.log](/root/jobs/fx/cx/v21/s033/i3541c/format.log)

**리팩토링 점검**
- connect/start_connecting 경로의 thread 판정을 2곳에서 1곳으로 줄였습니다. 새 상태·옵션은 없고, 해당 경로에서 추가 중복 helper나 gate 우회 동기화는 발견하지 않았습니다.

**커밋 · 남은 문제**
- 로컬 커밋 `c889c21066135344589f8a5cc446ccd9c1b335de` (`fix(core): connecter uses its session's io thread (#3541)`).
- 남은 문제 없음. `core/build`를 제거했고 worktree는 clean입니다.
