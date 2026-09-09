# Core tests public API 분리 rebase 결과

작업 위치는 `/home/hep7/project/zlink-core-tests`이며, main checkout은 수정하지 않았다.
`core-tests-rebase` branch의 `eda6ef24e5`는 current `origin/main` `5b749308e1` 바로 위에 있다.
코드와 테스트 patch는 `/tmp/zlink-core-tests/fix-rebased.patch`에 생성했다.

## 결과

기존 public API 링크 분리 결과를 current main에 rebase했고, STREAM 단일-part 계약과
새 ctx 종료·소유권·disconnect progress 검사를 보존했다. Integration 테스트는 public
header와 shared `libzlink`를 사용하고, 내부 상태 검사는 `unittest/`에 남는다.

Clean Release+LTO와 Debug 전체 build, C smoke, public-link 감사는 통과했다. 전체 lane은
unit 57개가 통과한 뒤 `test_flow_state_paired` 한 개가 두 번 같은 public monitor timeout을
내어 요구된 0 failures를 충족하지 못했다. 나머지 integration 126개와 별도로 실행한
regression 26개는 통과했고 e2e 등록은 없다. Hotpath도 `pair_inproc` instruction 수가
reference 하한보다 낮아 두 번 실패했다. 두 실패는 아래 BLOCKERS에 재현 결과와 함께
기록한다.

수정 전/후 규칙 수: integration이 접근하는 Core API 표면 2개(공개/내부)에서 1개(공개)로
줄었고, 내부 검사마다 제품 static library를 LTO link하던 규칙을 하나의 non-LTO
`test-core` archive 규칙으로 합쳤다. 이번 rebase는 별도 예외 경로를 추가하지 않았다.

## 충돌 해결

| 파일 | Upstream 변경 | 보존·이동한 결과 |
|---|---|---|
| `core/tests/integration/monitoring/test_monitor_socket_contract.cpp` | STREAM receive를 routing-id frame과 payload frame 두 번이 아니라 `zlink_recv_part` 한 번의 `FINAL` part로 검사하고, 잘못된 결과에서 message를 다시 초기화한다. | Upstream 단일-part 검사와 실패 정리를 그대로 유지했다. 기존 private monitor queue accounting은 `unittest_monitor_queue_accounting.cpp`에만 남겼다. |
| `core/tests/integration/test_ctx_destroy.cpp` | backpressure 상태에서 receive와 send가 함께 막힌 뒤 `zlink_ctx_shutdown`이 둘 다 `ETERM`으로 해제하는 public test를 추가했다. 같은 파일에는 기존 private ctx/session/pipe lifecycle 검사가 있었다. | 새 public shutdown test와 등록을 그대로 유지했다. Private header 여섯 개와 내부 lifecycle 검사 13개는 기존 분리 목적지 `unittest_ctx_lifecycle.cpp`에만 남겼다. Upstream이 private header를 포함한 파일에 public test를 추가한 경우이므로 파일 전체를 되돌리지 않고 public case만 integration에 합쳤다. |
| `core/tests/integration/test_helper_interleave.cpp` | `begin_complete_send_scope`가 output `std::optional`을 받도록 private complete-record 검사 두 줄을 바꿨다. | Integration의 private 검사는 제거된 상태를 유지하고 `unittest_complete_record_admission.cpp`로 옮긴 검사에 같은 `begin_complete_send_scope(&_scope)`와 `std::optional` 변경을 적용했다. 최초 Release compile이 이 이동본의 누락을 잡았고 수정 후 focused build/test가 통과했다. |
| `core/tests/integration/test_stream_socket.cpp` | STREAM/RAW receive를 독립된 single `FINAL` chunk로 정의하고, STREAM `MORE` send의 `NOT_SUPPORTED`, packet peer isolation·order, no-data output 보존을 추가했다. | 새 smoke case 목록, helper의 single-part 수신, raw/packet/unsupported-send case와 모든 등록을 유지했다. 기존 private `msg`·packet-state 검사는 integration에서 제거된 상태를 유지하고 unit 범위에 남겼다. |
| `core/tests/integration/test_stream_fastpath.cpp` | 기존 raw TCP helper와 중복 fastpath case를 제거하고 public no-data `zlink_recv_part` smoke 하나로 축소했다. | Rebase 중 WIP helper hunk를 버렸다. 최종 파일은 `origin/main`과 byte-for-byte 동일하며 CMake target도 upstream 등록을 그대로 사용한다. |
| `core/tests/CMakeLists.txt` | STREAM commit은 직접 수정하지 않았고 축소된 `test_stream_fastpath` target을 유지한다. 최신 main은 새 ownership·disconnect-progress tests를 등록한다. | Upstream target과 case 등록을 모두 유지하면서 integration/shared link, public include 경계, unit case 이동, test IPO 비활성화 규칙을 합쳤다. |
| `core/CMakeLists.txt` | STREAM commit의 runtime source 변경과 최신 main의 public option 변환을 base로 받았다. Test target 추가는 없었다. | 제품 shared/static LTO 설정을 유지하고, unit이 재사용하는 non-LTO `test-core` 하나만 추가하는 기존 분리 결과를 유지했다. |

충돌 밖의 upstream `test_helper_ownership.cpp`와
`test_socket_disconnect_progress_without_app_poll.cpp`는 public header만 사용하며 변경 없이
빌드·integration lane에 포함됐다. 최신 main `4534b0705b`가 `ZLINK_OPT_TYPE`을 public
`zlink_socket_type_t`로 반환하도록 고친 뒤 남아 있던 `unittest_typed_option`의 내부 enum
expectation은 `ZLINK_SOCKET_XPUB`으로 바꿨다. 이는 새 public 계약의 정확한 값으로 맞춘
test 적응이며 expectation을 완화하지 않았다.

## Build와 검증

Release configure는 다음 옵션으로 build directory를 새로 생성했다.

```sh
cmake -S core -B core/build-gate -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=ON \
  -DWITH_TLS=ON -DBUILD_BENCHMARKS=ON
```

Release build 직전 `/proc/loadavg`의 1분 값은 `6.99`, `pgrep -c lto1`은 `0`이었다.
다른 작업의 20-worker stress gate가 종료되어 load가 10 아래로 내려갈 때까지 build를
시작하지 않았다. Clean target 실행과 `.ninja_log` 제거 뒤 최종 측정은 다음과 같다.

| 단계 | wall | user | system | 결과 |
|---|---:|---:|---:|---|
| shared `libzlink` | 24.47 s | 148.13 s | 16.33 s | PASS |
| 나머지 전체 target | 43.20 s | 332.51 s | 32.77 s | PASS |
| 합계 | 67.67 s | 480.64 s | 49.10 s | PASS |

최초 나머지 build는 이동한 complete-record unit의 upstream API 적응 누락을 compile에서
찾아 중단됐다. 이동 목적지에 upstream 변경을 적용하고 focused build를 통과시킨 뒤 clean
build를 처음부터 다시 측정했으므로 위 표에는 실패한 중간 측정을 섞지 않았다.

Debug는 `-DCMAKE_BUILD_TYPE=Debug -DENABLE_LTO=OFF`와 같은 test/TLS/benchmark 옵션으로
directory를 새로 configure했다. Configure 4.26초, 전체 578-step build 60.33초로 통과했다.

| 검증 | 결과 |
|---|---|
| `run_test_lanes.sh --include-e2e --include-regression --unittest-jobs 8` | FAIL: unit 57/57 PASS(5.94 s), integration 126/127 PASS; `test_flow_state_paired` timeout. 동일 결과 2회. Script가 integration에서 중단되어 e2e/regression은 별도 호출로 확인했다. |
| e2e lane 별도 호출 | 등록 0개 |
| regression lane 별도 호출 | 26/26 PASS, 48.76 s |
| `test_pub_monitor_close_full_mask` | PASS, 2.01 s |
| `hotpath_gate -V` | FAIL: 3/4 PASS, `pair_inproc` ratio 0.9390. 재실행도 동일. |
| Debug configure + 전체 build | PASS |
| `git diff --check origin/main` | PASS |

Hotpath 원시 값은 두 번째 실행 기준으로 다음과 같다.

| cell | reference | measured | ratio | 판정 |
|---|---:|---:|---:|---|
| `dealer_dealer_inproc` | 3455.381 | 3418.916 | 0.9894 | PASS |
| `dealer_router_reqrep_inproc` | 12054.895 | 12137.653 | 1.0069 | PASS |
| `pair_inproc` | 2681.957 | 2518.344 | 0.9390 | FAIL |
| `router_router_tcp` | 2972.882 | 2974.022 | 1.0004 | PASS |

## Public-link 감사

Release Ninja graph와 생성물을 `nm -D --undefined-only`, shared export 목록, public header
선언, `ldd`, Ninja dependency/command graph로 대조했다.

| 항목 | 결과 |
|---|---:|
| Integration/C 실행 파일 | 78 |
| `libzlink.so.0`을 load하는 실행 파일 | 78 |
| Public header에 없는 `zlink_*` undefined symbol | 0 |
| Shared export에 없는 `zlink_*` symbol | 0 |
| Private `zlink::` dynamic import | 0 |
| 실행 파일에 내장된 Core C++ 함수 | 0 |
| Integration object의 private Core header 의존 | 0/78 |
| Helper object의 private Core header 의존 | 0/3 |
| Test/test-core command의 LTO flag | 0/419 |
| Production library/hotpath LTO command | 3 |

원시 결과는 `/tmp/zlink-core-tests/logs/public-link-audit.json`,
`public-include-audit.json`, `ipo-policy-audit.json`, `final-link-summary.json`과
`/tmp/zlink-core-tests/rebased-*.log`에 보존했다.

## BLOCKERS

1. `test_flow_state_paired`의
   `test_network_peer_weight_replays_after_reconnection`이 전체 integration 순서에서 두 번,
   선택 case 반복의 첫 실행에서 한 번 `Peer weight did not reach the public monitor`로
   실패했다. 실패 assertion 후 socket teardown이 끝나지 않아 CTest 10초 timeout으로
   기록된다. 같은 전체 바이너리 단독 실행은 0.38초 PASS였고 gdb 아래 선택 case도 PASS여서
   timing-sensitive 증상이다. Timeout·재시도 횟수·순서를 바꾸지 않았고 runtime도 수정하지
   않았다. Runtime 원인 수정은 별도 진단과 감독의 A/B 승인이 필요하다.
2. `hotpath_gate`의 `pair_inproc`은 두 번 모두 ratio `0.9390`으로 ±5% 하한을 벗어났다.
   Instruction 수가 reference보다 감소한 방향이지만 gate 계약은 FAIL이다. Reference나
   허용 범위를 바꾸지 않았다.
3. 위 두 항목 때문에 요청한 전체 lane 0 failures와 hotpath PASS는 달성하지 못했다.
   Build, 나머지 lane, Debug, public-link 경계에는 blocker가 없다.

## Runtime 변경 분류

이번 rebase에서 새 runtime 변경은 추가하지 않았다. 이전 작업의 HWM 계수 수정은 그대로
포함되며 분류는 다음과 같다.

- 소유 계층: Core socket submit/admission accounting의 기존 logical wait와 counter 갱신 지점.
- Spec: `core/doc/spec/core/systems/06-auto-hwm.ko.md`의 admission 계수 규칙과 `core/doc/spec/core/06-monitoring.ko.md`의 snapshot 규칙.
- 교차언어 대조: C++·.NET·Java binding은 같은 Core snapshot을 읽으며 별도 counter나 Framework 보상은 없다.
- 변경 분류: B — public blocking FINAL이 wake 후 같은 제출을 중복 계수하던 기존 Core 결함 수정.

`doc/spec/**`는 수정하지 않았다.
