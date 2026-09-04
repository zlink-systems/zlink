# Python REQUEST 계약 통일 결과

## 결과

REQUEST awaitable이 Core 0.17.0 계약 B의 SEND와 같은 wait-token/WRITABLE
상태기를 사용한다. 최초 DONTWAIT admission이 거절된 시점에만 바인딩이
payload와 target snapshot을 보유하고, 자신의 token/context/RID가 일치하는
WRITABLE에서만 같은 요청을 재제출한다. admission 뒤에는 snapshot을 해제하고
새 nonzero REQUEST completion ID로 전환해 reply 또는 timeout을 기다린다.

WRITABLE의 TERMINAL ENOENT는 `SubmitResult.NOT_FOUND`, ESHUTDOWN/ETERM은
`SubmitResult.TERMINATED`인 `SubmitError`로 전달한다. runtime wake는 기존 Core
poller와 wake FD만 사용하며 spin, sleep, timer를 추가하지 않았다.

## 변경 파일

- `bindings/python/src/zlink/_runtime/messaging/routed_async.py`
  - REQUEST refusal-time snapshot, WRITABLE 상관관계 검증, 재제출, completion ID
    전환, terminal/close 정리를 구현했다.
- `bindings/python/src/zlink/_runtime/eventing/poller.py`
  - WRITABLE-only completion이 SEND뿐 아니라 pre-admission REQUEST 진행도
    나타낸다는 주석을 반영했다.
- `bindings/python/src/zlink/_runtime/native_codes.py`
  - `PENDING_MAX_MSGS/BYTES`를 ABI 유지용이며 Core가 무시하는 옵션으로 고쳤다.
- `bindings/python/src/zlink/contracts/sockets/operations.py`
  - RequestOp awaitable의 admission retry와 reply completion 순서를 명시했다.
- `bindings/python/perf/multi/perf_multi_reqrep_client.py`
  - 바깥 REQUEST retry FIFO와 BACKPRESSURED 재호출을 제거하고 public managed
    terminal 한 번으로 통일했다.
- `bindings/python/README.md`
  - REQUEST wait-token 계약과 PENDING 옵션 무시 계약을 반영했다.
- `bindings/python/tests/test_request_writable_contract.py`
  - HWM, connect-before-bind, close, SEND/REQUEST 혼재를 각각 5회 검증한다.
- `bindings/python/tests/test_completion_contract.py`
  - REQUEST TERMINAL WRITABLE의 typed 오류 mapping을 검증한다.
- `bindings/python/tests/test_perf_multi_runner.py`
  - perf가 REQUEST 외부 retry를 만들지 않는 계약으로 바꿨다.
- `bindings/python/tests/test_readme_alignment.py`
  - PENDING 옵션의 ABI 유지·무시됨을 검증한다.

## Public API 전/후

| 항목 | 이전 | 이후 |
|---|---|---|
| `RequestOp.submit()` 서명 | awaitable reply terminal | 변경 없음 |
| 최초 `OK` | nonzero ID를 REQUEST completion ID로 등록 | 변경 없음 |
| 최초 `BACKPRESSURED` | `SubmitError`로 호출자에게 반환 | nonzero wait token을 등록하고 내부 대기 |
| WRITABLE | SEND만 재제출 | SEND와 pre-admission REQUEST를 token/context/RID별 재제출 |
| REQUEST 재제출 `OK` | 경로 없음 | 새 nonzero REQUEST completion ID로 전환 후 reply/timeout 대기 |
| `RequestOp.submit_sync()` | Core `NONE` blocking 경로 | 변경 없음 |
| `PENDING_MAX_MSGS/BYTES` | DONTWAIT REQUEST admission 제한으로 기술 | enum/storage ABI 유지, 동작에는 무시됨 |

## 테스트

- 관련 회귀: `83 passed, 4 subtests passed`
- 공개 REQUEST 회귀: 4종 × 5회 = `20/20 passed`
  - test 파일에는 sleep/timer가 없으며 peer 대기는 public poller를 사용한다.
- 전체 gate: `180 passed, 4 subtests passed`
- sample gate: `7/7 passed`
- `git diff --check -- bindings/python`: 통과

## Single perf smoke

조건: tcp, 1024B, duration 2, runs 1. `3/3` case, `15/15` RESULT,
`status: complete`, 0 값 없음.

| Pattern | Throughput | Mean latency |
|---|---:|---:|
| DEALER_ROUTER_REQREP | 2,118.5 ops/s | 0.463 ms |
| ROUTER_ROUTER_REQREP | 1,940.0 ops/s | 0.506 ms |
| DEALER_ROUTER | 17,282.5 msg/s | 0.246 ms |

## Multi perf smoke

조건: clients 8, duration 2, runs 1, 1024/65536B, tcp/tls/ws/wss.
`24/24` case, `120/120` RESULT, `status: complete`, 0 값 없음.
각 셀은 1024B / 65536B 순서다.

| Pattern | Transport | Throughput | Mean latency (ms) |
|---|---|---:|---:|
| DEALER_ROUTER_REQREP | tcp | 8,584 / 1,993 | 1.259 / 10.719 |
| DEALER_ROUTER_REQREP | tls | 8,647.5 / 1,174 | 1.279 / 39.292 |
| DEALER_ROUTER_REQREP | ws | 6,978.5 / 1,563 | 1.531 / 16.086 |
| DEALER_ROUTER_REQREP | wss | 8,962 / 1,117.5 | 1.269 / 55.387 |
| ROUTER_ROUTER_REQREP | tcp | 6,612 / 2,219.5 | 1.582 / 9.824 |
| ROUTER_ROUTER_REQREP | tls | 7,207 / 1,344.5 | 1.518 / 36.406 |
| ROUTER_ROUTER_REQREP | ws | 8,771.5 / 2,931 | 1.207 / 8.868 |
| ROUTER_ROUTER_REQREP | wss | 8,559 / 1,506.5 | 1.319 / 37.371 |
| DEALER_DEALER | tcp | 14,568 / 13,728 | 0.251 / 0.416 |
| DEALER_DEALER | tls | 16,440 / 13,683 | 0.269 / 6.931 |
| DEALER_DEALER | ws | 13,754.5 / 14,375.5 | 0.266 / 0.239 |
| DEALER_DEALER | wss | 16,925.5 / 14,727.5 | 0.148 / 51.756 |

## BLOCKERS

없음.
