# Python binding 계약 B 독립 리뷰 결과

## 결론

Core 0.17.0 계약 B의 SEND/WRITABLE 동작은 공개 API 형태를 바꾸지 않고 충족한다. 코드 리뷰에서 성능 결함 3건과 multi perf runner 결함 2건을 찾아 수정했다. 최종 test, sample, single perf smoke, multi perf smoke가 모두 통과했다.

## 계약 항목 판정

| 항목 | 판정 | 근거 |
|---|---|---|
| (a) DONTWAIT 한 번, OK/ID 0 즉시 완료 | PASS | `routed_async.py:914-978`: 시도마다 `ZLINK_DONTWAIT`로 한 번 제출하고 OK/ID 0이면 SEND entry를 즉시 끝낸다. 후속 completion을 기다리지 않는다. |
| (b) payload 보유와 정확한 WRITABLE 재제출 | PASS | `routed_async.py:359-403,748-780,914-963`: binding이 native message snapshot과 target을 보유한다. token/context/RID가 모두 맞는 WRITABLE만 소비하고, 다시 막히면 새 nonzero token을 등록한다. |
| (c) WRITABLE TERMINAL 전달 | PASS | `routed_async.py:733-779`: ENOENT는 `SubmitResult.NOT_FOUND`, ESHUTDOWN/ETERM은 `SubmitResult.TERMINATED`인 `SubmitError`로 대기자에게 전달한다. |
| (d) ROUTER/STREAM route 없음 | PASS | `routed_async.py:908-958`: `NOT_CONNECTED`/EHOSTUNREACH/ID 0을 즉시 `SubmitError`로 전달하고 token을 등록하지 않는다. 공개 통합 test가 통과했다. |
| (e) completion queue 단일 drain과 혼합 분배 | PASS | `routed_async.py:795-854`, `poller.py:158-220`: `_drain_lock`이 한 owner만 허용하고 NO_DATA까지 비운다. ID/context로 REQUEST와 WRITABLE을 나누며, WRITABLE-only POLLCOMPLETION은 숨기고 REQUEST 알림은 유지한다. level event를 drain한 뒤 deadline 안에서 다시 기다린다. |
| (f) close/term 정리와 thread safety | PASS | `routed_async.py:1105-1131`, `socket_base_impl.py:371-383`: shutdown이 token map을 비우고 waiter와 retained message를 한 번만 끝낸다. native socket close가 끝날 때까지 context root를 유지한다. lock 순서는 drain/poll wait/owner 경계로 분리돼 있으며 관련 lifecycle test가 통과했다. |
| (g) 오류 mapping과 예외 타입 | PASS | `routed_async.py:725-779`: submit 실패는 `SubmitError`, request completion 실패는 `RequestError`로 유지하며 Core result/errno를 보존한다. |
| (h) REQUEST/blocking/PUB 회귀 | PASS | blocking SEND는 flags 0 경로, REQUEST는 기존 REQUEST completion, PUB/XPUB은 publish 경로를 그대로 쓴다. 전체 157 test와 7 sample이 통과했다. |

## 발견 버그

| 파일:행 | 증상 | 수정 |
|---|---|---|
| `bindings/python/src/zlink/_runtime/messaging/routed_async.py:359` | async SEND 성공 경로에서도 payload를 Python `bytes`로 먼저 복사한 뒤 `zlink_msg`로 다시 복사했다. 1024B payload마다 불필요한 전량 복사가 생겼다. | 최초에 native `zlink_msg` snapshot을 하나 만들고, 매 admission 시 `zlink_msg_copy`로 공유 복사한다. 64B 초과 payload는 refcount만 늘며 payload bytes를 다시 복사하지 않는다. 성공·실패·shutdown에서 snapshot을 정확히 한 번 닫는다. |
| `bindings/python/src/zlink/_runtime/messaging/routed_async.py:630` | event가 없어도 timeout 0 poll 뒤 `call_soon`을 계속 등록해 event loop가 spin했다. | wake FD가 포함된 Core poller를 daemon worker가 timeout -1로 blocking wait한다. public poller ownership 변경과 shutdown은 wake FD로 즉시 깨운다. 고정 sleep과 zero-time poll을 제거했다. |
| `bindings/python/src/zlink/_runtime/messaging/routed_async.py:446` | entry 해제마다 live completion ID map 전체를 훑어 O(n)이었다. pending 수가 많으면 전체 정리가 O(n²)까지 커질 수 있었다. | entry의 단일 현재 completion ID만 조회·삭제해 O(1)로 바꿨다. |
| `bindings/python/perf/multi/run_benchmarks.py:1089` | PUBSUB server는 control `STOP`을 기다리지만 runner가 보내지 않아 case마다 shutdown timeout 30초를 소모했다. | client가 끝난 뒤 live PUBSUB server에 `STOP`을 보내고 기다린다. tcp 2-case smoke가 약 60초에서 4초로 줄었다. |
| `bindings/python/perf/multi/run_benchmarks.py:727,1417,1598` | process timeout이 발생해도 예외 문자열에 partial `RESULT`가 있으면 case와 전체 smoke를 성공으로 잘못 판정했다. | `_run_pattern_captured`가 process-control 실패를 별도 flag로 보존한다. 실패 case의 partial RESULT를 성공 집계에서 제외하고 `fail_count != 0`이면 전체 상태를 partial로 만든다. |

## 성능 검토

| 항목 | 판정 | 수정 | 측정/검증 |
|---|---|---|---|
| 성공 경로 payload snapshot | FAIL → 수정 | Python bytes 전량 복사를 없애고 native message 공유 복사로 교체 | 공개 mutable-buffer WRITABLE 재전송 test 및 retained resource 1회 해제 test 5/5 green |
| event 없는 completion progress | FAIL → 수정 | timeout -1 blocking Core poller + wake FD | source guard와 pre-attach async SEND test 5/5 green; sleep/timer/zero-time poll 없음 |
| completion 대기 자료구조 | FAIL → 수정 | context map과 completion-ID map 모두 평균 O(1), 해제도 단일 key 처리 | optimization guard green |
| 성공 async SEND의 나머지 비용 | 적합 | operation/Condition, native snapshot list, owner lock, context map 등록, Core submit은 정확한 token 상관관계와 close 직렬화에 필요해 유지 | 추가 payload bytes 복사는 없음. worker/thread/poller 생성은 BACKPRESSURED 또는 REQUEST 대기 때만 발생 |
| poll/drain | 적합 | public/runtime owner 모두 NO_DATA까지 drain하고 event가 없으면 blocking wait | spin·고정 sleep 없음 |

### 1024B DEALER_ROUTER tcp 수정 전/후

동일한 공식 single runner(`duration=3`, `runs=1`) 결과다. 이 runner는 blocking `submit_sync()` 송신 경로를 측정하므로 async snapshot 수정의 직접 효과 측정이 아니라 전체 binding 비회귀 확인값이다. 다른 job과 병행한 단일 실행이라 차이를 인과로 해석하지 않는다.

| 상태 | Throughput (msg/s) | Latency mean (ms) | p95 (ms) | p99 (ms) |
|---|---:|---:|---:|---:|
| 수정 전 | 7,855 | 1.104 | 5.230 | 7.183 |
| 수정 후 | 20,021 | 0.218 | 0.389 | 3.963 |

## 스모크 결과

### Test와 sample

| 실행 | 결과 |
|---|---|
| `bash bindings/python/tests/run_tests.sh` | 157 passed, 4 subtests passed |
| sample gate | 7/7 passed |
| 핵심 public WRITABLE/pre-attach/resource-release 회귀 | 3 tests × 5회, 모두 green, test 내 sleep 없음 |

### Single perf smoke — 1024B, duration 2, runs 1

| Pattern | Transport | Throughput (msg/s) | Mean latency (ms) |
|---|---|---:|---:|
| PAIR | tcp | 17,028 | 0.136 |
| PAIR | inproc | 19,584 | 0.467 |
| DEALER_ROUTER | tcp | 21,746 | 0.199 |
| DEALER_ROUTER | inproc | 11,008.5 | 0.321 |
| PUBSUB | tcp | 50,893 | 0.490 |
| PUBSUB | inproc | 32,299 | 0.473 |

결과: 6/6 case, 30/30 RESULT 줄, status complete.

### Multi perf smoke — clients 8, duration 2, runs 1

각 셀은 `1024B / 65536B` 순서다.

| Pattern | Transport | Throughput (msg/s) | Mean latency (ms) |
|---|---|---:|---:|
| DEALER_DEALER | tcp | 17,319 / 14,443.5 | 0.114 / 0.193 |
| DEALER_DEALER | tls | 14,911.5 / 18,882.5 | 0.308 / 15.376 |
| DEALER_DEALER | ws | 18,200 / 14,771.5 | 0.172 / 0.585 |
| DEALER_DEALER | wss | 13,786.5 / 9,795.5 | 0.318 / 65.105 |
| DEALER_ROUTER_SENDSEND | tcp | 13,770 / 9,272.5 | 166.890 / 63.636 |
| DEALER_ROUTER_SENDSEND | tls | 10,356.5 / 7,224.5 | 166.775 / 105.459 |
| DEALER_ROUTER_SENDSEND | ws | 11,210 / 12,955 | 199.232 / 48.638 |
| DEALER_ROUTER_SENDSEND | wss | 10,752 / 5,784 | 198.537 / 123.951 |
| PUBSUB | tcp | 145,558.5 / 62,292 | 27.228 / 15.474 |
| PUBSUB | tls | 67,687.5 / 10,242.5 | 139.123 / 53.733 |
| PUBSUB | ws | 82,697.5 / 37,396.5 | 118.838 / 24.916 |
| PUBSUB | wss | 123,089.5 / 10,292 | 82.983 / 70.078 |

결과: 24/24 case, 120/120 RESULT 줄, status complete, 0 throughput 없음, 총 93초.

## 변경 파일

- `bindings/python/src/zlink/_runtime/messaging/routed_async.py`
- `bindings/python/perf/multi/run_benchmarks.py`
- `bindings/python/perf/multi/perf_multi_dealer_router_server.py`
- `bindings/python/perf/multi/perf_multi_router_router_server.py`
- `bindings/python/tests/test_completion_contract.py`
- `bindings/python/tests/test_optimization_guard.py`
- `bindings/python/tests/test_perf_multi_runner.py`
- `bindings/python/README.md`

`git diff --check -- bindings/python` 통과. Python 범위에 별도 mirror cmp 대상은 없다. Core는 재빌드·clean하지 않고 `core/build/lib/libzlink.so.0.17.0`만 사용했다.

## BLOCKERS

없음.
