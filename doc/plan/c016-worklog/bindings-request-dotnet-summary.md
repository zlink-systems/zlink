# bindings/dotnet REQUEST 계약 통일 결과

## 결과

REQUEST의 `Async()` 제출 경로를 D-B85 계약 B에 맞췄다. 즉시 admission되면 기존 nonzero REQUEST completion ID로 reply/timeout을 기다린다. `BACKPRESSURED`와 nonzero wait token을 받으면 그때만 binding이 request parts를 snapshot하고, 자기 token·context·RID가 일치하는 `WRITABLE`을 받은 뒤 같은 request를 다시 제출한다. 재제출이 다시 거절되면 새 token으로 다시 기다린다. Admission 전 terminal `WRITABLE`은 `ZlinkSubmitException`의 `NotFound` 또는 `Terminated`로 끝난다. Blocking `Submit()`은 기존 `NONE` 동작을 유지한다.

Public API signature와 enum 숫자는 바꾸지 않았다.

## 변경 파일

- `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs`
  - REQUEST admission ID와 WRITABLE wait token을 구분하는 state machine을 추가했다.
  - 거절 시점에만 payload snapshot을 만들고, token·context·RID가 모두 일치할 때만 다시 제출한다.
  - 재거절 token 교체, admission 뒤 REQUEST completion, cancellation, runtime/public poller 소유권 이전, close와 terminal errno 정리를 한 entry에서 처리한다.
- `bindings/dotnet/src/Zlink/Runtime/Options/SocketOption.cs`
  - `PendingMaxMsgs`와 `PendingMaxBytes`를 ABI 보존용이며 SEND/REQUEST 동작에서는 무시하는 option으로 설명했다.
- `bindings/dotnet/src/Zlink/Contracts/Messaging/OperationContracts.cs`
- `bindings/dotnet/src/Zlink/Contracts/Messaging/CompletionKind.cs`
- `bindings/dotnet/src/Zlink/Contracts/Eventing/EventEnums.cs`
- `bindings/dotnet/src/Zlink/Contracts/Sockets/MessageSocketContracts.cs`
- `bindings/dotnet/src/Zlink/Contracts/Sockets/RoutedSocketContracts.cs`
  - REQUEST도 SEND와 같은 pre-admission WRITABLE 흐름을 사용한다는 public 계약 주석으로 고쳤다.
  - Core-owned pre-admission pending payload와 bounded pending budget 가정을 제거했다.
- `bindings/dotnet/README.md`
- `bindings/dotnet/README.docfx.md`
  - REQUEST token 흐름, admission 뒤 reply timeout 시작, terminal typed failure, `PENDING_MAX_*` no-op 계약을 기록했다.
- `bindings/dotnet/tests/Zlink.Tests/test_request_writable_contract.cs` (신규)
  - HWM 거절→peer drain/reply→WRITABLE→동일 request 재제출→reply, connect-before-bind, close token 정리, route 제거 `ENOENT`, SEND/REQUEST token 혼재를 public API로 검증한다.
  - `Thread.Sleep`, `Task.Delay`, timer retry loop를 사용하지 않는다.
- `bindings/dotnet/tests/Zlink.Tests/test_optimization_guard.cs`
  - REQUEST snapshot이 첫 제출 뒤의 backpressure 분기에만 존재하고 WRITABLE 재제출 경로가 유지되는지 검사한다.
- `bindings/dotnet/perf/single/run_benchmarks.sh`
- `bindings/dotnet/perf/multi/run_benchmarks.sh`
  - 별도 source tree가 소유하는 symlink Core build에는 현재 worktree의 source mtime stale 검사를 적용하지 않도록 고쳤다. 일반 local build의 stale 검사는 유지한다.

## API 전/후

| 구분 | 변경 전 | 변경 후 |
|---|---|---|
| `RequestSubmitOperation.Async()` 즉시 admission | nonzero ID를 REQUEST completion ID로 등록 | 동일 |
| `RequestSubmitOperation.Async()` backpressure | 예외로 끝나거나 nonzero ID를 pending REQUEST 수락으로 간주 | nonzero wait token으로 대기하고 matching WRITABLE에서 같은 request를 다시 제출 |
| payload 소유권 | Core의 admission 전 pending 보관을 전제 | Core는 payload를 보관하지 않으며 binding이 거절 시점에만 snapshot 보관 |
| timeout | 제출 시점부터 REQUEST completion을 전제 | 실제 admission 뒤에만 기존 reply timeout/completion 단계 시작 |
| terminal WRITABLE | REQUEST 경로에서 처리하지 않음 | `ENOENT`→`ZlinkSubmitException.NotFound`, lifecycle errno→`ZlinkSubmitException.Terminated` |
| blocking `Submit()` | Core의 `NONE` admission과 REQUEST completion을 기다림 | 동일 |
| `PENDING_MAX_MSGS/BYTES` | REQUEST pending budget으로 설명 | 숫자와 저장은 ABI용으로 유지하되 동작에서는 무시 |

## 테스트

- REQUEST token 회귀 + source guard 5회 연속: 매회 6/6 통과, 합계 30/30.
- 기존 REQUEST/completion/lifecycle 관련 묶음: 38/38 통과.
- `ZLINK_CORE_SOURCE=local ZLINK_BUILD_JOBS=3 CARGO_BUILD_JOBS=2 bash bindings/dotnet/tests/run_tests.sh`: managed 197/197, sample 7/7, PASS.
- perf runner/optimization guard: 22/22 통과.
- `dotnet format ... --verify-no-changes`: 통과.
- `bash -n` single/multi runner: 통과.
- `git diff --check -- bindings/dotnet`: 통과.

## 스모크 수치

Single, tcp, 1024B, duration 2, runs 1: success 3, fail 0, `status: complete`.

| pattern | throughput | latency mean / p95 / p99 (ms) |
|---|---:|---:|
| DEALER_ROUTER | 118188.5 msg/s | 6.970 / 27.681 / 30.657 |
| DEALER_ROUTER_REQREP | 28885.0 ops/s | 139.462 / 190.652 / 201.837 |
| ROUTER_ROUTER_REQREP | 30899.0 ops/s | 114.250 / 159.650 / 168.145 |

결과: `bindings/dotnet/perf/results/single/report/perf_dotnet_single_linux_20260905_003819.txt`.

Multi, tcp, clients 8, duration 2, 1024/65536B, runs 1: success 6, fail 0, `status: complete`.

| pattern | 1024B throughput | 65536B throughput |
|---|---:|---:|
| MULTI_DEALER_ROUTER_REQREP | 5276.5 ops/s | 299.5 ops/s |
| MULTI_ROUTER_ROUTER_REQREP | 5612.0 ops/s | 2112.5 ops/s |
| MULTI_DEALER_DEALER | 79821.0 msg/s | 22945.0 msg/s |

결과: `bindings/dotnet/perf/results/multi/report/perf_dotnet_multi_linux_20260905_003834.txt`.

모든 throughput은 0보다 크다.

## BLOCKERS

없음.
