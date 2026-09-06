# .NET binding R3 수정 결과

F-R3-1, F-R3-13, F-R3-15를 수정했다. 공개 API는 그대로이며, 전체 binding 테스트 **232/232**, samples **7/7**, 신규 회귀 테스트 **10건 × 5회**가 통과했다. 남은 실패와 BLOCKERS는 없다. Commit은 하지 않았다.

원인 위치와 diff의 삭제 측 행 번호는 수정 전 `53c49da5d6e6ff45c49498e1458c394d7a7dd791`의 `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs`를 기준으로 한다. 아래에서 `CompletionOwner.cs`는 이 파일을 뜻한다. 감독자가 확인한 `spec-review/R3-bindings-summary.md`와 D-098·D-109·D-111을 적용했다.

## F-R3-1 — WRITABLE RID 재검증 제거

- **소유 계층:** Core socket이 submit RID echo를 보장하고, binding은 socket-local context·token으로 waiter를 연결한다.
- **스펙 조항:** `core/doc/spec/core/socket/README.ko.md:989`의 part send WRITABLE echo, 같은 문서 `:1149`의 completion pull record 계약. Binding 투영은 `bindings/doc/spec/async-execution-model.ko.md` §4·§5와 D-109의 WRITABLE 전달 문장을 따른다.
- **원인:** `CompletionOwner.cs:610`의 `TargetMatches`, `:984`의 SEND capture와 `:1295`의 REQUEST writable capture가 target RID를 다시 비교했다. 비교용 `ToBytes()` 배열도 매번 만들었다.
- **수정:** `TargetMatches`와 두 호출을 삭제했다. Completion kind·context·token 검증, terminal errno 투영과 재제출 target 보관은 기존 소유자가 계속 처리한다. 수정 후 capture 위치는 `CompletionOwner.cs:928`, `:1238`이다.
- **교차언어 대조:** `bindings/c/include/zlink/socket/api.h:225`와 `:335`의 C raw ABI는 Core WRITABLE record를 그대로 전달한다. R3 진단에 열거된 일곱 고수준 binding의 중복 판정 중 이번 diff는 .NET 판정만 제거한다.
- **변경 분류:** B — 기존 중복 판정 결함. 계약을 만족하는 Core에서는 공개 동작이 같다.
- **수정 전/후 규칙 수:** .NET 수정 범위에서 2 → 1(Core echo 보장 + .NET 동일성 재판정 → Core echo 보장). 다른 언어의 수정 완료를 포함한 수치가 아니다.
- **회귀 테스트:** `tests/Zlink.Tests/test_writable_token_delivery.cs`의 `writable_delivers_core_result_to_matching_token` 4개 case. SEND·REQUEST 각각에서 일치하는 RID와 의도적으로 다른 RID를 주입하고, 같은 token·context의 Core terminal `ENOENT`가 `NotFound`로 전달되며 registry가 정리되는지 확인한다. 다른 RID 주입은 제거한 binding 정책을 검증하는 test-only 입력이며 정상 Core 동작의 예시가 아니다. 일치하는 RID 2개 case는 원래 구현에서도 통과했다.
- **검증:** 수정 전에는 다른 RID 2개 case가 `NotFound` 대신 `InternalError`로 실패했다. 수정 후 4/4 × 5회 통과했으며 전체 gate에서도 통과했다.
- **BLOCKERS:** 없음.

## F-R3-13 — tokenless BACKPRESSURED 보존

- **소유 계층:** Core submit result·errno 계약이 오류 분류를 소유한다. Binding은 nonzero token이 있는 BACKPRESSURED만 WRITABLE 대기에 연결한다.
- **스펙 조항:** `core/doc/spec/core/socket/README.ko.md:977`의 unified reservation 65,536개 제한과 REQUEST의 `BACKPRESSURED/EAGAIN/ID 0`, 같은 문서 §6의 SEND·REQUEST 공유 slot 포화 표. Blocking SEND의 `SNDTIMEO` 만료는 `:964`에 규정되어 있다. 정확한 submit error 전달은 `bindings/doc/spec/async-execution-model.ko.md` §5·§6 및 `bindings/doc/spec/async-coroutine-policy.ko.md` §3을 따른다.
- **원인:** `CompletionOwner.cs:103`, `:157`, `:216`, `:1079`, `:1431`이 tokenless BACKPRESSURED를 protocol failure로 바꾸었다. 최초 Request 외에 SendAsync·TrySend와 SEND·REQUEST 재제출에도 같은 판정이 있었다.
- **수정:** 위 다섯 경로에서 기존 submit exception을 그대로 전달한다. 성공 결과의 잘못된 completion ID 조합만 protocol failure로 남는다. Nonzero backpressure token을 대기시키는 기존 조건은 유지한다. 수정 후 관련 위치는 `CompletionOwner.cs:103`, `:151`, `:207`, `:1047`, `:1402`이다.
- **교차언어 대조:** `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:225`는 nonzero token일 때만 writable 대기로 연결하며, 나머지는 원래 result·errno를 던진다. `bindings/go/internal/native/dealer_router_request.go`의 request 실패 경로도 원래 error를 전달한다. .NET의 오류 재분류를 이 계약에 맞췄다.
- **변경 분류:** B — 기존 Core 결과 오분류 결함.
- **수정 전/후 규칙 수:** 2 → 1(Core의 합법적인 tokenless BACKPRESSURED + binding의 INTERNAL_ERROR 재분류 → Core result 분류).
- **회귀 테스트:** `tests/Zlink.Tests/test_tokenless_backpressure.cs`의 `full_completion_reservations_preserve_request_backpressure`. Public poller가 completion owner인 DEALER에서 public Request Async 65,536개로 실제 Core reservation을 채운다. 다음 async·blocking Request의 `Backpressured`와 EAGAIN, 실패 시 caller message 보존을 검증한다. Close 뒤 예약했던 65,536개 waiter도 모두 typed Terminated로 정리되는지 확인한다.
- **검증:** 수정 전에는 async overflow가 `InternalError`로 실패했다. 수정 후 1/1 × 5회 통과했으며 전체 gate에서도 통과했다.
- **BLOCKERS:** 없음.

## F-R3-15 — blocking native submit의 공용 잠금 제거

- **소유 계층:** Core가 동시 part sequence의 수락·거절과 native admission을 결정한다. Binding registry의 `_sync`가 등록을, 기존 request entry monitor가 결과 공개와 completion 합류를 소유한다.
- **스펙 조항:** `core/doc/spec/core/socket/README.ko.md` §2의 동시 send·lifecycle 계약, `bindings/doc/spec/README.ko.md:1336`의 binding 송신 lock·gate 금지. 등록·결과 공개·선행 completion 합류는 `bindings/doc/spec/async-execution-model.ko.md` §5, 단일 completion owner는 §4, reply의 synchronous NONE admission은 `bindings/doc/spec/async-coroutine-policy.ko.md` §1·§5를 따른다.
- **원인:** `CompletionOwner.cs:111`, `:224`, `:253`의 blocking Send·Request·Reply가 `_submitSync`를 잡은 채 native 호출을 실행했다. Core의 `SNDTIMEO` 대기가 다른 sender의 binding 진입도 막았다.
- **수정:** 세 blocking terminal에서 공용 submit 잠금을 제거했다. Request는 기존 registry 등록과 entry publication의 짧은 동기화를 사용하고 native submit은 그 밖에서 실행한다. Context 발급은 기존 counter의 `Interlocked.Increment` 하나로 통합했다. 선행 REQUEST completion은 기존 Registered 상태가 끝날 때까지 해당 entry의 `Monitor.Wait`로 publication과 합류한다. `PublishRequest`와 abort가 같은 monitor를 깨우며, lifecycle로 이미 끝난 entry는 다시 공개하지 않는다. Native record는 기존 drain의 지역 변수와 `finally`가 계속 소유하므로 completion owner 이전과 queue drain 직렬화도 유지된다. 수정 후 위치는 `CompletionOwner.cs:108`, `:212`, `:237`, `:441`, `:1185`, `:1238`, `:1424`이다.
- **대안 비교:** 선행 completion을 별도 저장소와 상태로 옮기는 방식은 새로운 raw ownership·cleanup 경로가 필요하다. 기존 entry monitor로 publication과 합류하는 방식을 선택하여 새로운 상태 필드·queue·timer·poller 없이 기존 등록 상태와 drain ownership을 재사용했다. 이 합류 대기는 native admission의 재시도나 timeout 연장이 아니다.
- **교차언어 대조:** C++ `bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:145`의 raw request는 runtime 수명을 확보한 뒤 binding 송신 mutex 없이 native part sequence를 호출한다. Rust `bindings/rust/src/internal/completion_owner.rs:357`의 shared lifecycle read guard도 sender끼리 상호 배제하는 lock과 다르다. .NET은 기존 공용 `_submitSync`에 의존했던 publication 합류만 entry monitor로 옮겨야 했다.
- **변경 분류:** B — 기존 Core 제출 경합 직렬화 결함.
- **수정 전/후 규칙 수:** .NET 수정 범위에서 2 → 1(Core sequence 경합 판정 + .NET native admission 직렬화 → Core 판정). Binding의 등록·publication 합류 계약은 새 정책이 아니라 기존 계약의 동기화 위치를 바꾼 것이다.
- **회귀 테스트:** `tests/Zlink.Tests/test_blocking_submit_concurrency.cs`의 5개 case. `blocking_admission_does_not_serialize_other_target`은 실제 Core의 receive-flow pause로 Request·Send를 막고 다른 target의 public Request·Reply가 먼저 끝나는지 4개 조합으로 확인한다. 최초 제출 진입의 순서는 test-only caller payload barrier로 고정하며 native 호출은 실제 NONE 경로다. `prepublication_reply_joins_blocking_request_once`는 Core admission 뒤 caller payload 소비를 잠시 멈추고 reply를 먼저 drain하여, publication 전에는 poller progress와 Request가 끝나지 않고 합류 뒤 한 번만 완료되는지 검증한다. Test-only reflection은 생산 코드나 공개 API에 hook을 추가하지 않는다.
- **검증:** 수정 전에는 동시 제출 4개 case가 첫 admission의 timeout까지 직렬화되어 실패했고 선행 completion case도 공용 lock 보유를 검출했다. 수정 후 5/5 × 5회 통과했으며 전체 gate에서도 통과했다.
- **BLOCKERS:** 없음.

## Diff 분리

생산 코드 변경은 `bindings/dotnet/src/Zlink/Runtime/Messaging/CompletionOwner.cs` 하나에 있다. 아래 symbol·삭제 측 hunk로 원인별 분리가 가능하며 기존 테스트 assertion은 수정하지 않았다.

| 순서·원인 | 생산 코드 hunk(수정 전 행) | 함께 포함할 .NET 테스트 파일 |
|---|---|---|
| 1. F-R3-1 | `TargetMatches` 삭제 610–622, SEND capture 985–987, REQUEST writable capture 1296–1298 | `tests/Zlink.Tests/test_writable_token_delivery.cs`, `tests/Zlink.Tests/CompletionOwnerTestAccess.cs` |
| 2. F-R3-13 | SendAsync 103–107, TrySend 157–160, RequestAsync 216–220, Send retry 1079–1082, Request retry 1431–1434 | `tests/Zlink.Tests/test_tokenless_backpressure.cs` |
| 3. F-R3-15 | Send 113–122, Request 227–250, Reply 256–266, Drain 주석 326–327, NextContext 459, PublishRequest 1225·1230, Capture 1276, AbortBeforeNativeWait 1455 | `tests/Zlink.Tests/test_blocking_submit_concurrency.cs` |

`CompletionOwnerTestAccess.cs`는 1번 diff에 포함하는 test-only 공통 접근 도구이며 3번 테스트도 재사용한다. 생산 코드에서 세 원인은 독립적이고, 테스트를 위 순서로 적용하면 공통 도구를 복제할 필요가 없다. 이 요약 파일은 세 원인의 공통 검증 기록이다.

## Gate 결과

검증 Core는 `core/build-dev/lib/libzlink.so.0.17.0`이다. SHA-256은 `64567f1715b3f1527afbc1c290e2b262d02d722768e160227a6f9815bdd4bb43`이다. Local runtime helper의 기본 `core/build` 디렉터리가 없어 `ZLINK_LIBRARY_PATH`와 `LD_LIBRARY_PATH`를 `core/build-dev`로 명시했다. Core 파일이나 공통 gate script는 변경하지 않았다.

전체 gate를 한 번 실행한 명령:

```bash
flock /tmp/zlink-samples-gate.lock env \
  ZLINK_CORE_SOURCE=local \
  ZLINK_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib/libzlink.so \
  LD_LIBRARY_PATH=/home/hep7/project/zlink/core/build-dev/lib \
  bindings/dotnet/tests/run_tests.sh
```

| 검증 | 결과 | 로그 |
|---|---|---|
| 관련 completion·request·admission·lifecycle 테스트 | 39/39, 실패 0 | `/tmp/zlink-dotnet-r3-related.log` |
| 수정 전 구현에 신규 회귀 테스트 적용 | 예상한 실패 8, 기존 정상 동작 통과 2 | `/tmp/zlink-dotnet-r3-original-regressions.log` |
| 수정본 신규 회귀 5회 | 매회 10/10, 합계 50/50, 실패·skip 0 | `/tmp/zlink-dotnet-r3-regressions-1.log` … `-5.log` |
| `tests/run_tests.sh` 전체 테스트 | 232/232, 실패·skip 0 | `/tmp/zlink-dotnet-r3-full-gate.log` |
| 같은 script의 samples, flock 보유 | 7/7, 실패 0, 최종 `[dotnet-tests] PASS` | 같은 전체 gate 로그 |
| `git diff --check` | 통과 | 별도 로그 없음 |

신규 회귀 반복은 `dotnet test`에서 `test_writable_token_delivery`, `test_tokenless_backpressure`, `test_blocking_submit_concurrency`만 filter로 선택했다. 첫 회에 수정본을 다시 빌드했고 이후 네 회는 `--no-build`로 실행했다. 전체 gate는 samples `DealerRouterRecv`, `MonitorRecv`, `PairRecv`, `PubSubRecv`, `RequestReplyAsync`, `StreamPacketCallback`, `StreamRecv`를 실행했다. Perf benchmark는 실행하지 않았다.

## 변경 범위와 BLOCKERS

변경 파일은 `CompletionOwner.cs`, 위 테스트 파일 4개와 이 보고서다. `core/**`, 모든 `doc/spec/**`, `framework/**`, `doc/site/**`, 다른 binding은 변경하지 않았다. 기존 사용자 변경을 유지했으며 branch 전환·commit·push를 하지 않았다.

**BLOCKERS: 없음. 남은 실패: 없음.**
