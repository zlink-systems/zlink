# C binding REQUEST 계약 통일 결과

## 결과

- C public API의 D-B85 REQUEST 계약을 고정하는 회귀 테스트를 추가했다.
- 공개 ABI와 함수 시그니처는 변경하지 않았다. raw header mirror에는 이미 REQUEST wait-token/WRITABLE 계약과 `PENDING_MAX`의 "ABI 유지, 저장·조회 외 무시" 주석이 반영되어 있었다.
- 허용된 소스 변경은 `bindings/c/**`의 3개 파일뿐이며 `bindings/c/perf/**`는 수정하지 않았다.

## 변경 파일

- `bindings/c/CMakeLists.txt`: `test_c_request_writable_contract` 등록.
- `bindings/c/tests/test_c_request_writable_contract.c`: 5회 반복 public API 회귀 추가.
  - HWM 포화 후 `BACKPRESSURED` + nonzero wait token 확인.
  - 거절된 logical payload를 호출자가 보유하고, 자기 token의 `WRITABLE`에서 같은 bytes로 재제출한 뒤 REQUEST reply completion까지 확인.
  - accepted filler REQUEST completion과 WRITABLE이 섞여도 kind/token/context로 분리.
  - connect-before-bind, SEND/REQUEST token 혼재 및 독립성, close 시 live token 정리.
  - routed target 제거 시 동일 token/context/RID의 typed terminal `ENOENT` 확인.
  - `PENDING_MAX_MSGS/BYTES=1`이 저장·조회되면서 REQUEST/SEND token 발급을 제한하지 않는 ABI-only 동작 확인.
- `bindings/c/tests/test_c_completion_poller_contract.c`: completion 전달 자체를 검사하는 기존 테스트를 blocking admission(`ZLINK_SEND_FLAGS_NONE`)으로 변경해 connect 직후 DONTWAIT가 곧 pending 수락이라는 옛 가정을 제거.

## API 전/후

- 전: 기존 completion 테스트가 connect 직후 DONTWAIT REQUEST의 즉시 admission을 전제해, pre-admission backpressure 계약과 분리되지 않았다. REQUEST 전용 public 회귀가 없었다.
- 후: DONTWAIT REQUEST는 즉시 admission이면 nonzero REQUEST completion ID, 거절이면 EAGAIN과 payload-free WRITABLE wait token이라는 계약을 테스트한다. wait token과 admission 이후 REQUEST completion ID를 구별하며, own WRITABLE 이후에만 동일 요청을 재제출한다.
- ABI/API surface 변경 없음. blocking REQUEST는 admission을 기다린 뒤 기존 비동기 REQUEST completion으로 완료됨을 기존 테스트에서 명시적으로 검증한다.

## 테스트

- `ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=3 bash bindings/c/tests/run_tests.sh`
  - contract: 10/10 통과.
  - sample smoke: 6/6 통과.
- `bindings/c/build/test_c_request_writable_contract` 독립 실행 5/5 통과. 실행 파일 내부에서도 네 시나리오를 각각 5회 반복한다.
- `git diff --check`: 통과.
- `clang-format`: 환경에 실행 파일이 없어 수행하지 못했으며, 컴파일과 diff whitespace 검사는 통과했다.

## 스모크 수치

Single, tcp, 1024 B, duration 2 s, runs 1: status complete 3/3, zero 없음.

| Pattern | Throughput | Mean / p95 / p99 latency (ms) |
|---|---:|---:|
| DEALER_ROUTER_REQREP | 316343 ops/s | 0.135232 / 0.237744 / 0.323436 |
| ROUTER_ROUTER_REQREP | 299993 ops/s | 0.142282 / 0.247071 / 0.337925 |
| DEALER_ROUTER | 617230.5 msg/s | 0.072992 / 0.132219 / 0.190479 |

Multi, tcp, clients 8, duration 2 s, runs 1: status complete 6/6, zero 없음.

| Pattern | 1024 B | 65536 B |
|---|---:|---:|
| DEALER_ROUTER_REQREP | 284569.5 ops/s | 19028 ops/s |
| ROUTER_ROUTER_REQREP | 175624.5 ops/s | 18195 ops/s |
| DEALER_DEALER | 949958 msg/s | 115134 msg/s |

## BLOCKERS

- 구현 및 검증 blocker 없음.
- 별도 perf 작업 범위를 침범하지 않기 위해 수정하지 않은 `bindings/c/perf/README.md:169-170`에는 `PENDING_MAX`가 pending REQUEST를 제한한다는 옛 설명이 남아 있다. perf 소유 작업에서 정정이 필요하다.
- 첫 single perf 실행 때 runner가 runtime을 stale로 판정해 `--reuse-build` 없이 symlink 대상 `/home/hep7hep7/project/zlink/core/build`를 자동 incremental build했다. configure/clean 및 Core 소스 수정은 없었고, 이후 모든 perf 실행은 `--reuse-build`로 고정했다.
