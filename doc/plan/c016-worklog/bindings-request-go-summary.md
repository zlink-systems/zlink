# Go REQUEST 계약 통일 결과

Core 0.17.0 D-B85 REQUEST 계약을 `bindings/go/**`에 적용했다. 공개 API signature 변경과 BLOCKER는 없다.

## API 전/후

| 항목 | 변경 전 | 변경 후 |
|---|---|---|
| `RequestSubmitOp.Submit(context.Context)` | 첫 DONTWAIT 제출이 admission되면 REQUEST completion을 기다렸지만, `BACKPRESSURED`는 즉시 제출 오류로 종료했다. | `BACKPRESSURED`의 nonzero token을 저장하고 binding-owned payload로 자기 `WRITABLE`에서만 같은 요청을 재제출한다. admission 뒤에는 기존 REQUEST reply/timeout completion을 기다린다. |
| payload 소유 | REQUEST pre-admission retry packet이 없었다. | 즉시 admission 경로에는 retry snapshot을 만들지 않는다. 거절된 시점에만 `zlink_msg_copy` 기반 snapshot을 만들고 admission/cancel/terminal까지 binding이 소유한다. |
| completion 분배 | REQUEST entry는 REQUEST completion만 처리했다. | socket-local owner가 REQUEST와 SEND의 WRITABLE을 context/token/RID로 구분한다. REQUEST WRITABLE 재거절은 새 token으로 계속 대기한다. |
| terminal | REQUEST WRITABLE terminal 경로가 없었다. | `ENOENT`는 `RequestNotFound`, `ESHUTDOWN`/`ETERM`은 `RequestTerminated`로 원 errno를 보존한다. close 시 token entry와 payload를 정리한다. |
| `PENDING_MAX` | README가 DONTWAIT REQUEST pending 제한으로 설명했다. | enum/ABI는 유지하지만 동작에는 영향을 주지 않는 ignored 옵션으로 설명한다. |

Go에는 별도 REQUEST blocking native 변형이 없다. 공개 `Submit(ctx)`는 DONTWAIT admission을 내부 관리한 뒤 호출자를 reply/timeout completion까지 대기시키며, 이 경로 전체가 새 token 기계를 사용한다.

## 변경 파일

- `bindings/go/internal/native/dealer_router_request.go`: REQUEST retry state, refusal-only snapshot, WRITABLE 재제출, admission 뒤 completion 전환
- `bindings/go/internal/native/completion_owner.go`: SEND/REQUEST 공용 WRITABLE waiter, token/context/RID 검증, early-WRITABLE 보류, typed terminal, cancel/close 정리
- `bindings/go/internal/native/operations.go`: request/send part staging alias로 즉시 admission의 변환 slice 할당 제거
- `bindings/go/internal/native/request_writable_retry_test.go`: HWM, connect-before-bind, close, SEND 혼재 공개 경로 회귀를 각 5회 검증
- `bindings/go/internal/native/completion_owner_test.go`: REQUEST terminal errno/result mapping 검증
- `bindings/go/internal/native/writable_retry_test.go`: 공용 WRITABLE waiter 명칭 반영
- `bindings/go/README.godoc.md`, `bindings/go/doc.go`, `bindings/go/contracts/sockets.go`: REQUEST token 계약과 ignored PENDING_MAX 설명
- `bindings/go/perf/single/perf_reqrep.go`: Core-owned pending FIFO 가정 제거
- `bindings/go/tests/raw-core11-allowlist.json`: 이미 동기화된 REQUEST/PENDING_MAX raw header hash 반영

## 테스트

- `go test -race ./internal/native -run 'TestPublicRequest|TestRequestTerminal' -count=1 -timeout=120s`: PASS
- REQUEST 공개 경로 회귀: HWM→BACKPRESSURED/token→drain/reply→WRITABLE→동일 요청 재제출→reply, connect-before-bind, close token 정리, SEND token 혼재를 각각 5회 PASS; 고정 sleep 없음
- `bash bindings/go/tests/run_tests.sh`: PASS
  - 전체 Go package test PASS
  - `go vet` PASS
  - raw contract/hot-path guard PASS
  - samples 7/7 PASS
- `git diff --check`: PASS
- 변경 Go 파일 `gofmt -l`: 출력 없음

## 성능 스모크

Core runtime: local LTO release `libzlink.so.0.17.0`, SHA-256 `a98cc793457dae04fc58aaafc9cf6fcbe70b021e59cba61b8846aab623061025`.

Single, tcp, 1024B, duration 2초, runs 1: status complete, 3/3 cases, 15/15 result rows, zero throughput 없음.

| 패턴 | 처리량 | mean / p95 / p99 latency (ms) |
|---|---:|---:|
| `DEALER_ROUTER` | 241,142.5 msg/s | 0.063 / 0.181 / 0.393 |
| `DEALER_ROUTER_REQREP` | 5,630.5 ops/s | 0.176 / 0.279 / 0.381 |
| `ROUTER_ROUTER_REQREP` | 5,986.0 ops/s | 0.165 / 0.222 / 0.301 |

Multi, clients 8, duration 2초, runs 1, 1024/65536B, tcp/tls/ws/wss: status complete, 24/24 cases, 120/120 result rows, zero throughput 없음.

| 패턴 / 크기 | tcp | tls | ws | wss |
|---|---:|---:|---:|---:|
| `MULTI_DEALER_DEALER` / 1024 | 381,154.5 | 344,618.0 | 359,920.5 | 359,503.5 |
| `MULTI_DEALER_DEALER` / 65536 | 94,242.0 | 35,225.5 | 68,653.0 | 26,474.0 |
| `MULTI_DEALER_ROUTER_REQREP` / 1024 | 4,648.0 | 3,980.0 | 4,376.0 | 3,904.0 |
| `MULTI_DEALER_ROUTER_REQREP` / 65536 | 3,272.0 | 2,272.0 | 2,672.0 | 2,192.0 |
| `MULTI_ROUTER_ROUTER_REQREP` / 1024 | 4,612.0 | 3,980.0 | 4,324.0 | 3,904.0 |
| `MULTI_ROUTER_ROUTER_REQREP` / 65536 | 3,124.0 | 2,180.0 | 2,792.0 | 2,160.0 |

수치는 DEALER_DEALER은 msg/s, REQREP은 ops/s다. 원본은 `bindings-request-go-single.txt`, `bindings-request-go-multi.txt`에 보존했다.

## BLOCKERS

없음.
