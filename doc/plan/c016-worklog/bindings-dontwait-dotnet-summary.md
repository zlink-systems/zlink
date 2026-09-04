# .NET DONTWAIT backpressure 계약 정합 결과

## 결과

`bindings/dotnet/**`의 SEND API와 비동기 상태 머신을 D-B79 확정 B 계약에 맞췄다. 정상 SEND는 completion ID 0에서 끝나며 completion을 기다리지 않는다. DONTWAIT backpressure는 EAGAIN 상태와 nonzero WRITABLE 대기 토큰을 구분하고, 비동기 SEND는 정확한 token/context/RID의 WRITABLE을 pull한 뒤 보관한 동일 패킷만 재전송한다. REQUEST completion 경로와 ABI 값은 유지했다.

바인딩 구현과 managed gate는 완료했다. 네이티브 계약 검증은 이 worktree의 진행 중 Core 스냅샷이 컴파일되지 않아 미완료다. 감독관이 최종 Core를 교체한 뒤 표준 gate를 다시 실행해야 한다.

## 직접 참조 조사

`completion ID`, `pending`, `POLLCOMPLETION`, `PENDING_MAX`, `DONTWAIT`, `BACKPRESSURED`를 `bindings/dotnet`에서 직접 검색해 다음 책임 경계를 확인했다.

| 범위 | 확인한 의미와 조치 |
|---|---|
| `Runtime/Messaging/CompletionOwner.cs` | nonzero SEND completion ID를 수락 완료로 간주해 SEND completion을 기다리던 중심 경로를 WRITABLE token 대기와 동일 패킷 재전송 상태 머신으로 교체했다. |
| `Runtime/Native/NativeTypes.cs`, `RequestReplySupport.cs` | native completion kind가 SEND/REQUEST만 표현하던 경계를 WRITABLE까지 확장하고 REQUEST 결과 처리는 유지했다. |
| `Contracts/Eventing/*`, `Runtime/Eventing/Poller.cs` | `POLLCOMPLETION`과 completion queue 소유권을 확인했다. WRITABLE-only wake는 `PollOut`으로 노출하고 queue는 NO_DATA까지 drain하며, REQUEST용 `PollCompletion` 의미는 유지했다. |
| `SinglePartSubmit.cs`, `SocketKernel.MultipartSubmit.cs`, `SocketKernel.SendCore.cs` | DONTWAIT/EAGAIN 반환과 native part 소유권을 확인해 STREAM을 포함한 payload 소비·복원 경계를 새 계약에 맞췄다. |
| `Runtime/Options/SocketOption.cs` | `PENDING_MAX_MSGS/BYTES` 숫자 ABI를 유지하면서 REQUEST 제한이며 SEND에서는 no-op임을 명시했다. 공개 SEND 옵션 노출은 없었다. |
| README와 pending 기대 테스트 | 0.16.0의 pending accepted/SEND completion 서술과 기대를 정상 ID 0 및 WRITABLE 재시도 계약으로 교체했다. |

## 공개 API 전후

| API/동작 | 전 | 후 |
|---|---|---|
| `SendSubmitOperation.Submit()` | blocking SEND를 수행했지만 성공 SEND의 ID 0/무-completion 계약을 별도로 표현하지 않았다. | blocking admission은 completion을 만들지 않는다. 실패는 기존 typed exception 규칙으로 전달한다. |
| `SendSubmitOperation.TrySubmit()` | SEND builder에 직접적인 nonblocking 상태 terminal이 없었다. | `true`는 admission 성공과 caller message 소비, `false`는 BACKPRESSURED/EAGAIN과 caller message 보존을 뜻한다. |
| `SendSubmitOperation.Async()` | nonzero ID를 payload가 Core에 pending 수락된 것으로 보고 SEND completion을 기다렸다. | 성공 ID 0은 즉시 완료한다. Backpressure token/context/RID와 패킷 사본을 기억하고 POLLOUT에서 queue를 pull해 같은 WRITABLE token을 확인한 뒤 같은 패킷을 재전송한다. 재-backpressure는 새 token으로 다시 무장한다. |
| `CompletionKind` | SEND/REQUEST 값이 native 내부에만 있었다. | 공개 enum으로 `Send=1`, `Request=2`, `Writable=3`을 노출한다. `Send`는 ABI 예약값이며 성공 SEND completion은 생성되지 않는다. |
| `Poller.Wait()` | completion wake를 일반 `PollCompletion`으로 취급했다. | WRITABLE-only wake는 내부 drain 후 `PollOut`으로 표시하고, 필터링된 wake 뒤에도 원래의 단조 deadline까지 REQUEST completion을 기다린다. 외부 poller가 `PollCompletion`을 등록하면 queue 소유권을 가져간다. |
| `ZLINK_OPT_PENDING_MAX_*` | 내부 enum에 용도 구분이 없었다. | ABI 값은 그대로 두고 REQUEST 제한/SEND no-op으로 한정했다. |
| REQUEST/reply | REQUEST completion queue를 사용했다. | 기존 REQUEST kind, 결과, reply 및 timeout 처리를 유지했다. |

비동기 SEND pump는 별도 전용 OS thread, sleep, timer를 만들지 않는다. shared managed scheduler의 event-loop turn에서 nonblocking poll을 수행하며, cancellation·close·poll/drain 오류에도 payload와 native context 수명을 분리해 awaiter가 영구 대기하지 않게 했다.

현재 Core 공개 ABI에는 poller waitable FD/HANDLE이나 readiness callback이 없고 `zlink_poller_wait`만 동기식으로 제공된다. 따라서 public poller가 queue를 소유하지 않은 autonomous backpressure 대기에서는 scheduler turn이 반복될 수 있다. 외부 event loop가 `PollOut | PollCompletion` poller를 소유하면 해당 loop가 진행을 구동한다. autonomous await의 무스핀 구현에는 Core의 비소비형 waitable/callback ABI가 필요하다.

## 변경 파일

공개 계약·문서:

- `bindings/dotnet/README.md`
- `bindings/dotnet/README.docfx.md`
- `bindings/dotnet/src/Zlink/Contracts/Errors/SubmitResult.cs`
- `bindings/dotnet/src/Zlink/Contracts/Errors/TypedExceptions.cs`
- `bindings/dotnet/src/Zlink/Contracts/Eventing/EventEnums.cs`
- `bindings/dotnet/src/Zlink/Contracts/Eventing/Poller.cs`
- `bindings/dotnet/src/Zlink/Contracts/Messaging/CompletionKind.cs` (신규)
- `bindings/dotnet/src/Zlink/Contracts/Messaging/OperationContracts.cs`
- `bindings/dotnet/src/Zlink/Contracts/Sockets/IStreamSocket.cs`
- `bindings/dotnet/src/Zlink/Contracts/Sockets/MessageSocketContracts.cs`
- `bindings/dotnet/src/Zlink/Contracts/Sockets/RoutedSocketContracts.cs`
- `bindings/dotnet/src/Zlink/Contracts/Sockets/SocketEnums.cs`

런타임:

- `bindings/dotnet/src/Zlink/Runtime/Eventing/Poller.cs`
- `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs`
- `bindings/dotnet/src/Zlink/Runtime/Messaging/Received.Operations.cs`
- `bindings/dotnet/src/Zlink/Runtime/Messaging/RequestReplySupport.cs`
- `bindings/dotnet/src/Zlink/Runtime/Messaging/SinglePartSubmit.cs`
- `bindings/dotnet/src/Zlink/Runtime/Native/NativeTypes.cs`
- `bindings/dotnet/src/Zlink/Runtime/Options/SocketOption.cs`
- `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.MultipartSubmit.cs`
- `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketKernel.SendCore.cs`
- `bindings/dotnet/src/Zlink/Runtime/Sockets/SocketOperations.Send.cs`

테스트:

- `bindings/dotnet/tests/Zlink.Tests/test_optimization_guard.cs`
- `bindings/dotnet/tests/Zlink.Tests/test_pull_completion_contract.cs`
- `bindings/dotnet/tests/Zlink.Tests/test_routed_async_admission.cs`
- `bindings/dotnet/tests/Zlink.Tests/test_socket_surface.cs`

다른 binding, Core, framework 및 보호 문서는 변경하지 않았다. worktree에 이미 있던 Core와 다른 binding의 변경은 그대로 보존했다.

## 테스트와 gate

모든 명령은 `ulimit -v 16777216` 아래에서 실행했다. .NET GC에는 16 GiB 제한 안에서 안정적으로 실행되도록 `COMPlus_GCHeapHardLimit=40000000`, workstation GC, heap 1개, processor count 2를 적용했다.

| 검증 | 결과 |
|---|---|
| `bash scripts/build-core.sh dev` | 실패. `socket_base_api.cpp:159`와 `socket_base_lifecycle.cpp:1318`에서 `fail_all_send_writable`이 선언되지 않았다. 범위 밖 Core는 수정하지 않았다. 같은 소스를 빌드하는 release lib-only 재시도는 반복하지 않았다. |
| .NET build | 성공, warning 0 / error 0. |
| 전체 .NET suite | 178/178 성공. native library가 없어 native 의존 테스트는 availability guard에서 조기 반환한다. |
| 표준 `bindings/dotnet/tests/run_tests.sh` (`ZLINK_CORE_SOURCE=local`, build-dev 경로) | 성공. tests 178/178, samples 7/7. samples도 native availability guard로 종료되어 Core 동작은 검증하지 못했다. |
| 신규 계약 필터 4건 × 5회 | 매회 4/4, 합계 20/20 성공. HWM/POLLOUT/WRITABLE 2건과 TrySubmit HWM 1건은 native guard로 조기 반환했고 `CompletionKind` 1건은 실제 실행됐다. |
| 변경 C# 파일 `dotnet format --verify-no-changes` | 성공. 전체 프로젝트 검사는 미수정 `test_socket_concurrency.cs`의 기존 whitespace 4건을 별도로 보고한다. |
| `git diff --check -- bindings/dotnet` | 성공. |
| raw header mirror | `bindings/dotnet/include`가 없어 적용 대상이 아니다. |

## 최종 Core 교체 후 재실행

최종 Core 스냅샷을 교체한 뒤 다음 순서로 네이티브 검증을 완료한다.

1. repository root에서 `ulimit -v 16777216`을 설정하고 `bash scripts/build-core.sh dev`를 실행한다.
2. `ZLINK_CORE_SOURCE=local`, `ZLINK_LIBRARY_PATH=$PWD/core/build-dev/lib/libzlink.so`, `LD_LIBRARY_PATH=$PWD/core/build-dev/lib`를 설정한다.
3. `bindings/dotnet`에서 `bash tests/run_tests.sh`를 실행한다.
4. HWM 계약 필터 4건을 5회 다시 실행해 매회 4건 모두 실제 native 경로를 통과하는지 확인한다.

금지된 `--core-version`, `scripts/local-package/**`, git commit/push/checkout은 사용하지 않았다.
