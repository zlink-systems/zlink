# Node REQUEST 계약 통일 결과

## 결과

- 공개 TypeScript API signature는 바꾸지 않았다.
- Async REQUEST는 첫 DONTWAIT admission이 `BACKPRESSURED`+nonzero token이면 binding이 거절 시점의 payload와 target만 snapshot하고, 자기 token/context/RID의 `WRITABLE`에서만 같은 request를 재제출한다.
- 재제출 admission 뒤에는 새 nonzero REQUEST completion ID로 전환해 기존 reply/timeout completion으로 Promise를 완료한다. Reply timeout은 admission 시점부터 적용된다.
- `WRITABLE TERMINAL`과 completion drain의 lifecycle 종료는 pre-admission request에는 typed `SubmitError`(`NotFound`/`Terminated`), admission 이후 request에는 typed `RequestError`로 전달한다.
- Runtime wake는 `setImmediate`/`setTimeout` probe 대신 socket notification FD의 libuv watcher를 사용한다. Public poller ownership 전환과 socket close 때 watcher를 정리한다.
- Blocking `submit_sync()`는 기존 `ZLINK_SEND_FLAGS_NONE` admission/reply 동작을 유지한다.
- `ZLINK_OPT_PENDING_MAX_MSGS/BYTES`는 ABI 값과 Core storage만 유지되고 SEND/REQUEST가 모두 무시하는 옵션으로 주석과 README를 정정했다.
- Multi REQREP runner는 REQUEST의 application-level pre-admission 재시도 queue를 제거했다. 이미 timeout된 request의 blocking reply `EAGAIN`은 샘플 drop으로 처리하며 peer 종료 중 `ECONNABORTED`는 다음 poll progress로 넘긴다.

## 변경 파일

- Runtime/native: `src/zlink/runtime/messaging/completion_owner.ts`, `src/zlink/runtime/native/binding_socket.ts`, `native/src/addon_core.cc`, `native/src/addon_core_api.h`, `native/src/addon_exports.cc`
- Public 계약/옵션/README: `src/zlink/contracts/messaging/operations.ts`, `src/zlink/runtime/options/option_mapping.ts`, `README.md`, `README.typedoc.md`
- Perf source: `perf/multi/perf_multi_socket_reqrep.ts`, `perf/multi/perf_multi_runtime.ts`, `perf/multi/perf_multi_dealer_router_server.ts`, `perf/multi/perf_multi_router_router_server.ts`
- Tests: `tests/request_admission.test.ts`, `tests/send_completion_operation_path.test.ts`, `tests/source_layout.test.ts`, `tests/perf_multi_routed_sendsend_contract.test.ts`
- Generated dist-tools mirror: 위 perf 4개와 test 4개의 `dist-tools/**.js`

## API 전/후

| 항목 | 이전 | 이후 |
|---|---|---|
| Async REQUEST immediate admission | nonzero REQUEST ID를 등록하고 reply/timeout 대기 | 동일; payload retry snapshot 추가 없음 |
| Async REQUEST backpressure | `SubmitError.Backpressured`로 Promise 종료 또는 Core pending 수락 가정 | binding-owned snapshot, wait token 대기, matching WRITABLE에서 exact request 재제출 |
| REQUEST completion | 최초 submit ID의 reply/timeout만 처리 | admission 전에는 WRITABLE token, admission 뒤에는 REQUEST ID의 reply/timeout 처리 |
| Runtime wake | zero-time poll + adaptive JS timer | socket notification FD 기반 libuv watcher 또는 public Poller |
| Blocking REQUEST | Core NONE submit + blocking completion pull | 변경 없음 |
| PENDING_MAX | REQUEST admission 전 pending record 제한으로 설명 | ABI/storage 유지, SEND와 REQUEST 모두 무시 |
| Public signature | `request(...).message(...).timeout(...).submit()/submit_sync()` | 변경 없음 |

## 검증

- Native addon: `ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=3 CARGO_BUILD_JOBS=2 bash scripts/rebuild_native.sh` 통과.
- TypeScript: `npm run build`, `npm run typecheck` 통과.
- Public REQUEST regression: 5회 연속 통과. 각 run은 HWM request 5 rounds, connect-before-bind, close token cleanup, context shutdown terminal, SEND/REQUEST token 혼재를 검증한다. 고정 sleep/timer를 사용하지 않았다.
- 전체: `ZLINK_CORE_SOURCE=local bash bindings/node/tests/run_tests.sh` 최종 통과.
- Samples: 7/7 통과.
- `git diff --check -- bindings/node` 통과.

## Smoke 수치

Single, tcp, 1024B, duration 2, runs 1: `perf/results/single/report/perf_node_single_linux_20260905_004326_request-contract.txt`, status complete, 3/3 success, 0 수치 없음.

| pattern | throughput | mean latency |
|---|---:|---:|
| DEALER_ROUTER_REQREP | 593.5 ops/s | 1.634 ms |
| ROUTER_ROUTER_REQREP | 527.0 ops/s | 1.850 ms |
| DEALER_ROUTER | 23,347.5 msg/s | 306.218 ms |

Multi, clients 8, duration 2, sizes 1024/65536, tcp/tls/ws/wss, runs 1:

- `perf/results/multi/report/perf_node_multi_linux_20260905_005213_request-dealer-reqrep-complete.txt`: status complete, 8/8 success, fail/skip/unsupported 0.
- `perf/results/multi/report/perf_node_multi_linux_20260905_005324_request-router-dd-complete.txt`: status complete, 16/16 success, fail/skip/unsupported 0.

| pattern/transport | throughput 1024 / 65536 |
|---|---:|
| DEALER_ROUTER_REQREP tcp | 11,031.5 / 3,428.5 ops/s |
| DEALER_ROUTER_REQREP tls | 8,895.5 / 1,128.0 ops/s |
| DEALER_ROUTER_REQREP ws | 10,817.0 / 2,836.0 ops/s |
| DEALER_ROUTER_REQREP wss | 11,520.5 / 1,824.0 ops/s |
| ROUTER_ROUTER_REQREP tcp | 14,280.5 / 2,372.5 ops/s |
| ROUTER_ROUTER_REQREP tls | 10,708.5 / 2,008.0 ops/s |
| ROUTER_ROUTER_REQREP ws | 14,510.5 / 4,071.5 ops/s |
| ROUTER_ROUTER_REQREP wss | 19,417.0 / 2,675.5 ops/s |
| DEALER_DEALER tcp | 114,581.5 / 23,739.0 msg/s |
| DEALER_DEALER tls | 124,794.0 / 12,763.5 msg/s |
| DEALER_DEALER ws | 105,021.5 / 22,499.5 msg/s |
| DEALER_DEALER wss | 70,684.0 / 8,361.5 msg/s |

## BLOCKERS

없음.
