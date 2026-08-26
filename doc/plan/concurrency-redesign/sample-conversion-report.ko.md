# `ZLinkSessionActorBindingTable` 표본 전환 결과

작성: 2026-08-26

## 1. 재진입

실제로 분리한 재진입은 **2곳**이다.

1. `GetExactRetiredBindingAsync(string, ...)`에서
   `GetExactRetiredBindingAsync(ZLinkActorId, ...)`를 호출하던 오버로드 재진입
   - 두 진입점이 각각 lane에 한 번만 들어간 뒤 private
     `GetExactRetiredBinding(...)` 본문을 호출하도록 분리했다.
2. `ArmCanonicalSealTimeout(...)`에서 시작한 `RunCanonicalSealTimeoutAsync(...)`의 지연 후
   재진입
   - timeout 작업을 lane 안에서 시작하면 `ZLinkStateLane.Current`의 `AsyncLocal` 값이 작업에
     상속된다. 지연이 끝난 뒤 timeout 작업이 같은 lane에 들어갈 때 재진입 예외가 발생했다.
   - timeout 작업 시작 구간에서 `ExecutionContext` 흐름을 억제해, 지연 후 실행을 새 lane
     호출자로 분리했다.

## 2. async 시그니처와 호출자 전파

- 동기 메서드 **24개**를 `ValueTask` 반환과 `...Async` 이름으로 바꿨다.
- 동기 property `TombstoneCount` **1개**를 `GetTombstoneCountAsync()`로 바꿨다.
- 이미 async였던 `WaitForRouteAvailableAsync`, `SealCanonicalRouteAsync`, `SealRouteAsync`의
  시그니처는 유지하고 내부 상태 구간만 lane으로 옮겼다.
- 코디네이터의 기존 table 직접 호출 **35곳**을 새 async API로 전환했다.
- 직접 단위 테스트의 table 호출 **30곳**도 새 시그니처를 따르도록 바꿨다.

대상 파일 밖의 기존 동기 호출자를 수정하지 않는 범위 제약 때문에 코디네이터에서는 35곳 중
6곳이 직접 `await`, 4곳이 `ValueTask` 전달, 25곳이 기존 동기 표면에서 lane 완료를 기다리는
호환 경계다. 이 경계는 `AwaitStateLane(...)` 두 overload에 모았다. 상태 접근 자체는 모두 lane에서
직렬화되지만, 나머지 호출 계층까지 완전한 비동기 전파를 끝내려면 현재 금지된 대상 밖 소스 파일도
함께 바꿔야 한다.

## 3. 기존 lock 블록 본문 조정

조건, 컬렉션 변경, 오류, timeout 값과 후속 신호 순서는 바꾸지 않았다. 다만 C#의 lambda 반환과
`out` 제거를 위해 다음 구조 조정은 불가피했다.

- 기존 `out` 메서드는 nullable 결과를 반환하도록 대입과 성공 여부 반환을 합쳤다.
- `TryAccept`는 거절 시에도 기존 `acceptedHighWater`를 반환하므로 단순 `ulong?`로 바꿀 수
  없었다. `ZLinkSessionFrameAcceptance?`에 `Accepted`와 `AcceptedHighWater`를 함께 보존했다.
- `SealCanonicalRouteAsync`의 첫 상태 구간은 조기 완료 결과와 drain 작업을 함께 넘기도록
  private `PrepareCanonicalRouteSeal(...)`로 분리했다.
- `SealRouteAsync`의 첫 상태 구간은 조기 거절 결과, drain 작업과 accepted high-water를 함께
  넘기도록 private `PrepareRouteSeal(...)`로 분리했다.
- `RouteCanonicalAsync`, `AbortRouteSealAsync`, `UnsealCommittedRouteAsync`는 lane 안의 성공 여부를
  반환한 뒤, 원래 lock 밖에 있던 signal 완료와 retained frame 정산을 같은 순서로 실행한다.
- canonical seal timeout 작업에는 위 재진입을 막기 위한 `ExecutionContext.SuppressFlow()`를
  추가했다.

그 밖의 lock 블록 본문 로직 조정은 **없음**이다.

## 4. 테스트 결과

최종 필수 게이트:

```text
dotnet test tests/Zlink.Framework.UnitTests
Passed: 1893, Failed: 0, Skipped: 0, Total: 1893
```

집중 검증은 `SessionActorCoordinatorTests`, `BoundSessionReplacementLifecycleTests`,
`StateLaneTests` 합계 **67 통과 / 0 실패**였다.

첫 두 전체 실행은 각각 **1892 통과 / 1 실패**였다. 실패는 두 번 모두 변경 범위 밖의
`ClientServerChannelRuntimeTests.MalformedReservedControlFrame_DoesNotReachApplicationHandler`에서
native send 오류 코드 2로 발생했다. 해당 테스트 단독 실행과 클래스 전체 실행은 각각
1/1, 34/34로 통과했다. `dotnet clean tests/Zlink.Framework.UnitTests/Zlink.Framework.UnitTests.csproj`
후 필수 게이트를 다시 실행해 최종 **1893/1893**을 확인했다. 최종 실패 목록은 **없음**이다.

## 5. 예상과 달랐던 점

- 단순 메서드 호출 재진입 외에, lane 안에서 시작한 비동기 timeout 작업도 `AsyncLocal`을 통해
  lane 소유권을 상속했다. 장기 작업을 lane 안에서 시작하는 모든 후속 C2 전환에서 같은 검사가
  필요하다.
- 사전 조사와 달리 코디네이터의 35개 호출을 모두 `await`으로 바꾸려면 코디네이터를 호출하는
  대상 밖 runtime 파일까지 시그니처를 전파해야 했다. 이번 범위에서는 기존 동기 표면을 유지하는
  호환 경계를 사용했다.
- `TryAccept`의 `out` 값은 성공 결과만이 아니라 route seal 거절 시 현재 high-water도 전달한다.
  `out` 제거를 기계적으로 nullable scalar로 바꾸면 관측 가능한 동작이 달라진다.
- 전체 게이트에서 변경 범위 밖 native send 간헐 실패가 두 번 재현됐지만, 산출물 정리 뒤 동일한
  필수 명령이 실패 0으로 완료됐다.

## 6. 감독 독립 검증 (Claude, 2026-08-26)

작업 주체는 codex sol(reasoning high), 소요 약 **32분**(11:38–12:10), 311k 토큰.
감독 리뷰와 독립 검증 결과:

- diff 정독: lock 블록 30곳 본문 보존, `ConcurrentDictionary`/`SemaphoreSlim` 치환 없음,
  early-return이 lock 밖 signal/정산 코드를 건너뛰는 순서 신구 일치.
- 데드락 후보 1건 추적 후 무혐의: `RouteCanonicalSession`이 `lock (_outboundProofGate)` 안에서
  lane을 블로킹 대기하지만, 후처리 `SettleOutbound` → `capability.Settle`은 세션 write와
  `RunContinuationsAsynchronously` TCS뿐이라 gate를 재획득하지 않는다. 오히려 gate 보유 중
  소켓 write가 lane 스레드로 빠져 잠금 보유 중 I/O가 줄었다.
- `AwaitStateLane`의 `ValueTask.GetAwaiter().GetResult()` 블로킹은 lane이 TCS(Task) 기반이라
  안전. 단 이 호환 경계 25곳은 후속 async 전파 단계에서 제거해야 할 임시 구조다.
- `dotnet test` 독립 재실행: **1893 통과 / 0 실패**.
- `run_samples.sh` 독립 실행: **6샘플 placement marker 전부 completed**.
  ZoneWorld 실패는 기존 mesh admission 결함(handoff §8.2)으로 이 작업과 무관.
