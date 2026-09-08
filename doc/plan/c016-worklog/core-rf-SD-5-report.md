# core-rf-SD-5 결과 보고

## 1. 결과

B-SD4-1을 해소했다. WS/WSS bounded gather는 첫 pointer body를 복사하지 않고 그 위치만 `gather_split_offset`에 기록한 뒤, 이미 준비된 후속 frame을 같은 encoder batch 뒤쪽에 계속 모은다. 제출은 `[batch 앞부분, pointer body, batch 뒷부분]` buffer sequence를 한 번의 `async_writev()`와 한 번의 Beast binary `async_write()`로 수행한다.

커밋은 만들지 않았다. base `84d25131a6` 누적 patch에서 `SUPERVISOR-NOTE.md`와 `core/tests/perf/hotpath_reference.json`을 제외했다.

## 2. 규칙 전후

- 수정 전: pointer gather가 시작되면 `[header, body]`를 즉시 닫아, 합계가 128 KiB target보다 작아도 이미 준비된 다음 small frame은 별도 write였다.
- 수정 후: bounded batch의 유일한 추가 상태는 split offset 하나다. 합계 `앞부분 + body + 뒷부분`에 target과 `zmp_send_batch_max_size()`를 적용하고, 첫 pointer body 뒤의 준비된 frame까지 모아 한 write로 제출한다. 두 번째 pointer body는 현재 batch를 닫고 다음 batch에서 처리한다. max를 넘는 body는 기존 분리 규칙을 유지한다.
- 상태 규칙 수: `pointer 발견 즉시 batch 종료`와 후속 재제출이라는 별도 규칙을 제거하고, `합계 상한까지 하나의 bounded batch`라는 한 규칙으로 통합했다. 새 영속 필드는 split offset 하나뿐이며 pending 표시는 그 필드의 예약 비트를 사용한다.
- 대안 검토: pointer body를 batch buffer에 복사하는 방법은 zero-copy 계약을 깨므로 기각했다. pointer body별 queue/descriptor 상태를 추가하는 방법은 동일 사실의 복수 소유와 다중 pointer 규칙을 늘리므로 기각했다.

## 3. 구현

- `asio_engine.cpp`, `asio_engine_pipeline.hpp`: pointer body 소유권을 write completion까지 유지하고 split 전후 encoder byte를 모은다. pending 두 번째 body도 split offset의 예약 비트로 표현한다.
- `i_asio_transport.hpp`, TCP/IPC/WS/WSS transport: 고정된 두 buffer 인자를 array/count 계약으로 일반화했다. WS/WSS 공통 경로는 전달받은 sequence 전체를 한 Beast binary write에 넘긴다. TCP ZMP와 RAW STREAM의 byte 경로는 기존 1/2-buffer 동작을 유지한다.
- `contract_zmp_engine_fixture.hpp`, 관련 unit: 실제 ZMP encoder와 fake transport를 연결해 제출 횟수, buffer 수와 wire frame 순서를 관찰한다.
- public API, ABI, 새 환경변수와 옵션은 추가하지 않았다. 보호된 spec 문서는 변경하지 않았다.

## 4. 반례 테스트

target은 128 KiB이며 body는 64 KiB다.

| 실제 encoder 입력 | 관찰 결과 | 판정 |
|---|---|---|
| `64 KiB body -> small` | 3 buffers, write 1회, frame 순서 일치 | PASS |
| `small -> 64 KiB body -> small` | 3 buffers, write 1회, frame 순서 일치 | PASS |
| 합계가 max 초과 | 실제 encoder write 2회 | PASS |
| pointer body 2개 | 각 2 buffers, write 2회 | PASS |

## 5. 검증

| 검증 | 결과 |
|---|---|
| `JOBS=4 scripts/build-core.sh dev` | 성공 |
| `test_zmp_ws_wss`, `test_asio_ws`, policy/transport/encoder unit | 5/5 성공 |
| 관련 `stream\|asio\|decoder\|ws` 27-suite 3회 | 27/27 x 3 성공, 14.91/14.92/15.00 s |
| `ctest --test-dir core/build-dev -E hotpath_gate` | 211/211 성공, 246.16 s |
| GCC TSan build 및 관련 27-suite | 27/27 성공, 22.39 s, TSan 보고 0건 |
| Release 및 release-gate build | 성공, 각 시작 시 ninja 0, `JOBS=4`, foreground |
| `git diff --check` | 성공 |

첫 전체 gate에서 `test_pubsub_churn_dist`가 pending body 판별 오류를 재현했다. message 크기로 pending을 추정하던 구현을 제거하고 split offset 예약 비트로 소유 사실을 표현한 뒤, 해당 테스트와 전체 211개를 다시 실행해 모두 통과했다.

## 6. WS/WSS gate

Release library, 100 clients, server/client I/O thread 4, TCP/WS/WSS, 1/64 KiB, 세 pattern의 18셀을 매회 단일 report에 기록했다. 측정은 `/tmp/claude-1000/PERF_LOCK` 아래 수행했다.

| run | 시작 load1 | 완료 | ws Q64/Q1 | wss Q64/Q1 | 판정 |
|---:|---:|---|---:|---:|---|
| 1 | 1.31 | 18/18 | 0.967902 | 0.832113 | PASS |
| 2 | 3.78 | 18/18 | 0.830596 | 1.358819 | PASS |
| 3 | 3.99 | 17/18 | 산출 불가 | 산출 불가 | FAIL |

run 3은 gate 대상 외 추가 셀 `MULTI_ROUTER_ROUTER_SENDSEND/ws/65536`의 endpoint 오류로 report가 partial이었다. 원인 변화 없이 반복 측정하지 않았다. 원자료는 다음과 같다.

- `all-artifacts/SD-5-ws-gate-r1/multi/report/perf_c_multi_linux_20260909_022954_SD-5-r1.txt`
- `all-artifacts/SD-5-ws-gate-r2/multi/report/perf_c_multi_linux_20260909_023219_SD-5-r2.txt`
- `all-artifacts/SD-5-ws-gate-r3/multi/report/perf_c_multi_linux_20260909_023439_SD-5-r3.txt`

## 7. hotpath 5셀

release-gate build 뒤 같은 lock으로 한 번 측정했다. 작업 지시대로 reqrep 기준은 `15609.7872`를 적용했고 reference 파일은 수정하지 않았다.

| cell | reference | measured | ratio | 판정 |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3230.922 | 3105.3438 | 0.9611 | PASS |
| dealer_router_reqrep_inproc | 15609.7872 | 15624.6704 | 1.0010 | PASS |
| pair_inproc | 2348.457 | 2456.7270 | 1.0461 | PASS |
| router_router_tcp | 2972.532 | 3049.9692 | 1.0261 | PASS |
| stream_tcp | 13969.806 | 14058.2220 | 1.0063 | PASS |

## 8. 소유 계층·스펙·교차언어·분류

- 소유 계층: Core Asio ZMP encoder가 준비된 byte의 bounded batch와 split을 소유하고, transport는 한 buffer sequence를 한 operation으로 제출한다.
- 스펙 조항: `core/doc/spec/core/protocol/01-zmp.ko.md` §9 :489-492의 현재 준비 byte를 target까지 한 Beast write로 제출하는 규칙과 :411-414의 header payload-copy 금지 규칙을 따른다.
- 교차언어 대조: C/C++/.NET/Java/Kotlin은 동일 native Core engine/transport를 사용하므로 언어별 runtime 보상이나 별도 변경이 없다. TCP ZMP와 RAW STREAM wire byte도 변경하지 않았다.
- 변경 분류: B — SD-4 bounded batching의 기존 결함을 결정을 소유한 Core engine/transport에서 수정했다.

## 9. 남은 실패

- WS gate 3회 중 세 번째 18-cell report가 `MULTI_ROUTER_ROUTER_SENDSEND/ws/65536` endpoint 오류 1건으로 partial이다. 앞선 두 단일-report gate는 모두 기준을 통과했다.
