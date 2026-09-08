# core-rf-SD-4 결과 보고서

## 1. 결론

review-SD-2의 차단 항목 B-SD2-1을 Core Asio encoder의 기존 bounded batch 경로 안에서 해소했다. SD-2가 추가했던 WebSocket message-boundary 기본 gather 선택은 되돌렸고, `prepare_output_buffer()`가 준비한 batch 뒤에 pointer 전달 큰 body가 오면 합계가 WebSocket max 128 KiB 이내일 때만 `[batch, body]`를 하나의 `async_writev()`로 제출한다. 합계가 max를 넘으면 기존 encoder 분리 경로를 그대로 탄다.

- 누적 patch: `/home/hep7hep7/project/zlink-work/all-artifacts/SD-4.patch`
- base: `84d25131a6424031149ab7321e0678b65409bd75`
- commit은 만들지 않았다.
- 사용자 소유 `SUPERVISOR-NOTE.md`와 `core/tests/perf/hotpath_reference.json`은 patch에서 제외했다.

## 2. 되돌린 SD-2 hunk

`core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp:139-143`의 `use_gather_write_for()`를 `protocol header 지원 && transport gather 지원 && ZLINK_ASIO_GATHER_WRITE`로 복원했다. transport가 message boundary라는 이유만으로 connection 전체가 `prepare_gather_output()` 단일-message 경로를 기본 선택하지 않는다. 생성자와 connection policy에서 message-boundary 인자도 제거했다.

따라서 TCP ZMP의 기존 env opt-in gather는 유지되고 기본값은 꺼져 있다. RAW STREAM은 ZMP gather header를 만들지 않으므로 계속 비활성이다.

## 3. bounded batch 수정

| 파일:행 | 변경 |
|---|---|
| `core/src/runtime/transports/ws/ws_batch_policy.hpp:28-44` | 현재 copy batch와 다음 큰 frame의 header/body가 target 및 128 KiB max에 들어오는지 판단하는 순수 policy를 추가했다. |
| `core/src/runtime/engine/asio/asio_engine.cpp:1239-1332` | 정상 owner인 `prepare_output_buffer()`에서 작은 frame/header를 계속 모은다. 다음 body가 pointer 전달 대상이고 합계가 max 이내면 header를 현재 batch 끝에 붙이고 body pointer를 두 번째 buffer로 보존한다. max 초과면 손대지 않고 기존 encoder 경로가 분리한다. |
| `core/src/runtime/engine/asio/asio_engine.cpp:776-793` | 기존 `async_writev()`와 기존 gather lifetime 상태를 공통 제출 helper로 재사용한다. env opt-in 단일-message gather와 bounded batch closure가 같은 제출 코드만 공유한다. |
| `core/src/runtime/engine/asio/asio_engine.cpp:1479-1484` | 중복되어 있던 `process_output()`의 batch 조립을 제거하고 `prepare_output_buffer()` 한 owner를 호출한다. |
| `core/src/runtime/engine/asio/asio_engine.hpp:244` | 공통 제출 helper 선언만 추가했다. public API/ABI 변경은 없다. |

새 옵션, env, timer, persistent 상태는 추가하지 않았다. 기존 `async_gather`, `tx_msg`, header/body pointer lifetime과 transport capability만 사용했다.

### 규칙 전후

- 전: bounded multi-frame batch와 message-boundary 기본 single-message gather라는 batch owner 2개가 있었다.
- 후: `prepare_output_buffer()`의 bounded batch owner 1개가 copy write 또는 `[batch, body]` 2-buffer 제출 형태를 고른다.
- 규칙 수: 2 → 1. 128 KiB max와 기존 target 검사는 그대로 한 곳에서 적용된다.

### 대안 비교

| 대안 | 판정 |
|---|---|
| WS/WSS에서 SD-2의 기본 single-message gather 유지 | multi-frame target과 128 KiB max owner를 우회하므로 제외했다. |
| bounded batch owner가 큰 body 도착 시 copy 또는 기존 2-buffer 제출을 선택 | 기존 소유권과 max를 유지하고 header/body Beast write 분리만 없애므로 선택했다. |

## 4. 영향 경계

bounded pointer gather 조건은 `zmp_transport_has_message_boundaries()`와 `supports_gather_write()`를 동시에 요구한다. 따라서 TCP ZMP는 이 분기에 들어오지 않으며 기존 env opt-in `prepare_gather_output()`만 유지한다. RAW STREAM은 `options.type == ZLINK_CORE_SOCKET_STREAM`일 때 ZMP message-boundary가 false이므로 들어오지 않는다. 두 경로의 byte와 기본 write 선택은 변하지 않는다.

## 5. 반례 테스트

`core/tests/unittest/unittest_asio_write_turn_policy.cpp:329-353`에 다음 두 반례를 고정했다.

1. target 128 KiB에서 이미 준비된 작은 frame 뒤의 64 KiB frame header/body가 max 안에 들어오면 한 `[batch, body]` operation으로 허용한다.
2. 64 KiB body는 초기 target을 넘어도 128 KiB max 안에서 같은 operation을 닫지만, body가 max보다 크면 거부해 기존 분리 경로로 보낸다.

같은 unit에서 WS 기본 gather가 꺼져 있고 TCP ZMP gather는 env opt-in일 때만 켜지며 RAW는 꺼지는 것도 검증한다.

## 6. 기능·동시성 검증

| 검증 | 결과 |
|---|---|
| `JOBS=4 scripts/build-core.sh dev` | 성공. 시작 ninja 0, available 10.6 GiB. |
| `test_zmp_ws_wss`, `test_asio_ws`, `unittest_ws_transport_config`, `unittest_asio_write_turn_policy` | 4/4 성공. |
| `ctest -R 'stream\|asio\|decoder\|ws'` 3회 | 27/27 × 3 성공, 14.89/14.91/14.96 s. |
| `ctest -E hotpath_gate` | 211/211 성공, 240.51 s. |
| GCC TSan 증분 build | 성공. `-fsanitize=thread -fno-omit-frame-pointer -fPIE` 확인. |
| `TSAN_OPTIONS=halt_on_error=1 setarch x86_64 -R ctest -R 'stream\|asio\|decoder\|ws'` | 27/27 성공, 22.61 s, TSan 보고 0건. |
| Release 및 release-gate build | 성공. 각 시작 시 ninja 0, `JOBS=4`, foreground. |
| `git diff --check` | 성공. |

## 7. WS/WSS gate 3회

고정된 최종 Release library로 idle 상태에서 매회 `/tmp/claude-1000/PERF_LOCK`을 잡았다. 조건은 100 clients, I/O thread 4, TCP/WS/WSS, 1/64 KiB, DEALER-DEALER 단방향과 DEALER-ROUTER 왕복이며 각 run은 12/12 cell 완료, skip/fail/unsupported 0이다.

| run | load1 | ws Q1 | ws Q64 | ws Q64/Q1 | wss Q1 | wss Q64 | wss Q64/Q1 | 판정 |
|---:|---:|---:|---:|---:|---:|---:|---:|---|
| 1 | 0.98 | 0.729025 | 0.854316 | 1.171861 | 0.636813 | 0.593151 | 0.931437 | PASS |
| 2 | 1.10 | 0.726069 | 0.912894 | 1.257310 | 0.644907 | 0.688525 | 1.067634 | PASS |
| 3 | 0.44 | 0.846423 | 0.992671 | 1.172784 | 0.599293 | 0.648844 | 1.082682 | PASS |

기준은 각 transport의 Q64/Q1 ≥ 0.80이다. 원자료는 다음에 있다.

- `all-artifacts/SD-4-ws-gate-final3-r1/multi/report/perf_c_multi_linux_20260909_005525_SD-4-final3-r1.txt`
- `all-artifacts/SD-4-ws-gate-final3-r2/multi/report/perf_c_multi_linux_20260909_005804_SD-4-final3-r2.txt`
- `all-artifacts/SD-4-ws-gate-final3-r3/multi/report/perf_c_multi_linux_20260909_010222_SD-4-final3-r3.txt`

## 8. hotpath 5셀

release-gate build 뒤 load1 1.41에서 같은 lock으로 한 번 측정했다. reference 파일은 수정하지 않았다.

| cell | reference | measured | ratio | 판정 |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3230.922 | 3104.855 | 0.9610 | PASS |
| dealer_router_reqrep_inproc | 16455.383 | 15625.600 | 0.9496 | FAIL |
| pair_inproc | 2348.457 | 2457.121 | 1.0463 | PASS |
| router_router_tcp | 2972.532 | 3040.816 | 1.0230 | PASS |
| stream_tcp | 13969.806 | 13990.218 | 1.0015 | PASS |

잔여 실패는 `dealer_router_reqrep_inproc` 한 셀의 경계 미달이다. 이번 변경의 WS/WSS transport 및 bounded batch 분기와 무관한 inproc 셀이며, 원인 변화 없이 전체 gate를 반복하지 않았다.

## 9. 소유 계층·스펙·교차언어·분류

- 소유 계층: Core Asio ZMP encoder의 `prepare_output_buffer()`가 준비된 byte의 bounded batch와 제출 단위를 소유하고, WS/WSS transport는 기존 gather capability와 `async_writev()`만 제공한다.
- 스펙 조항: `core/doc/spec/core/protocol/01-zmp.ko.md` §8(:381-387)의 WebSocket byte carrier와 §9(:481-492)의 “현재 준비된 ZMP byte를 target까지 모으는 bounded batch”, frame마다 Beast write를 시작하지 않는 규칙을 따른다. spec 문장은 변경하지 않았다.
- 교차언어 대조: C/C++/.NET/Java/Kotlin binding이 동일한 native Core engine/transport를 사용하므로 언어별 runtime 보상이나 별도 변경이 없다. C benchmark app 변경은 누적 SD-3 계측 구성이다.
- 변경 분류: B — wire 위반이 아닌 기존 WS/WSS batching 성능 결함을 소유 모듈에서 수정했다.
- 공개면: `core/include/**`, `core/src/libzlink.vers`, public API/ABI와 보호된 spec 문서는 변경하지 않았다.
