# CP3 마일스톤 감사 (codex sol, 2026-08-27)

> 감독: Claude. 스냅샷 재측정·스펙 06 개정안·마일스톤 리뷰 전문.

# 판정

정적 CP3 판정은 **NOT-CLEAN**입니다.

- 문서의 `lock` 118은 문자열 검색 hit입니다. 실제 C# `lock` 문은 **114개**입니다.
- 그중 정의에 맞는 async 경계 스냅샷 source는 **48곳**입니다.
  - primitive·프로토콜 gate로 정당화 가능: **45곳**
  - 실질 잔존 결함: **3곳**
- 결함 3곳은 모두 `ZLinkActorMessageFollowLease`에 있습니다.
- 나머지 실제 lock 66곳은 값을 async 경계로 넘기지 않습니다.
- 파일 수정, git, 빌드, 테스트는 실행하지 않았습니다.
- 걸린 시간: **8분 45초**.

근거 정의는 [design.ko.md](/home/hep7/project/zlink/doc/plan/concurrency-redesign/design.ko.md:10), 금지 형태는 [스펙 06 §3](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md:54), 현재 지표는 [progress.ko.md](/home/hep7/project/zlink/doc/plan/concurrency-redesign/progress.ko.md:73)입니다.

## 1. 측정값과 방법

### 전수 결과

| 파일 | 실제 lock | async 경계 snapshot | 판정 |
|---|---:|---:|---|
| [ZLinkSerialExecutionQueue.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialExecutionQueue.cs:54) | 25 | 10 — L70, 91, 126, 317, 382, 413, 456, 572, 605, 839 | application/lifecycle queue primitive |
| [ZLinkSpotSerialExecutor.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotSerialExecutor.cs:69) | 19 | 8 — L69, 585, 925, 1108, 1163, 1180, 1192, 1237 | execution/barrier primitive |
| [ZLinkWorkerPool.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkWorkerPool.cs:60) | 11 | 3 — L89, 211, 283 | bounded worker primitive |
| [ZLinkMeshCompletionTable.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkMeshCompletionTable.cs:37) | 8 | 5 — L37, 162, 209, 215, 231 | completion ownership/admission primitive |
| [ZLinkCompletionDispatcher.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Backend/DotNet/ZLinkCompletionDispatcher.cs:49) | 4 | 1 — L94 | preallocated callback dispatcher |
| [ZLinkSerialWorkItem.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkSerialWorkItem.cs:45) | 1 | 1 — L45 | queue 하위의 payload memoization primitive |
| [ZLinkManagedMeshNode.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:2821) | 20 | 11 — L2821, 2829, 2889, 4382, 5000, 9000, 9028, 9164, 9237, 9310, 11034 | dispose/socket ownership protocol |
| [ZLinkClientServerRuntimeService.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerRuntimeService.cs:161) | 7 | 2 — L161, 188 | observer registration/exact-identity removal protocol |
| [ZLinkActorHandoffState.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:1930) | 5 | **3 — L1930, 1947, 1976** | **실질 잔존 결함** |
| [ZLinkClientServerClientRuntime.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Channels/ZLinkClientServerClientRuntime.cs:831) | 4 | 0 | socket lifecycle operation 전체를 lock 안에서 수행 |
| [ZLinkInstanceSpotMonitoring.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkInstanceSpotMonitoring.cs:127) | 3 | 0 | 동기 aggregate, async snapshot 없음 |
| [ZLinkEntrySpotDispatchPump.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkEntrySpotDispatchPump.cs:245) | 3 | 1 — L273 | terminal retirement claim 뒤 queue dispose |
| [ZLinkActorMessageFollower.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkActorMessageFollower.cs:724) | 2 | 1 — L766 | terminal retirement claim 뒤 queue dispose |
| [ZLinkFrameworkRuntimeState.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkRuntimeState.cs:172) | 1 | 1 — L172 | exact-once disposal task |
| [ZLinkSpotNodeBundleRegistry.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotNodeBundleRegistry.cs:17) | 1 | 1 — L17 | exact-once disposal task |
| **합계** | **114** | **48** | 45 제외군, 3 결함 |

118과 114의 차이인 4곳은 다음 오탐입니다.

- `Expression.Block(`: [ZLinkHandlerMethodInvoker.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Handlers/ZLinkHandlerMethodInvoker.cs:61)
- `ZLinkDeadlineClock(` 생성자: [ZLinkDeadlineClock.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkDeadlineClock.cs:14)
- `new ZLinkDeadlineClock(`: [ZLinkManagedMeshNode.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:196)
- 주석의 `try block (`: [ZLinkSpotRetireScheduler.cs](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkSpotRetireScheduler.cs:472)

### 재현 명령

```bash
cd /home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework

# 기존 문서 지표: substring 오탐 포함
grep -rhoE 'lock *\(' --include='*.cs' . | wc -l
# 118

# 실제 C# lock token: word boundary 적용
rg -o --glob '*.cs' '\block\s*\(' . | wc -l
# 114

# 파일별 실제 lock
rg -l --glob '*.cs' '\block\s*\(' . |
while IFS= read -r file; do
    count=$(rg -o '\block\s*\(' "$file" | wc -l)
    printf '%3d %s\n' "$count" "$file"
done | sort -nr

# 각 source 문맥
rg -n -C 12 --glob '*.cs' '\block\s*\(' .
```

각 실제 lock에는 다음 판정 절차를 적용했습니다.

1. lock 안에서 읽거나 산출한 참조·값·결정을 표시합니다.
2. lock 해제 뒤 첫 사용까지 추적합니다.
3. 그 사이에 `await`, `Task`/`ValueTask` 반환, `Task.Run`, queue/thread 제출, 비동기 continuation을 발생시키는 TCS 완료, nonblocking transport 제출이 있으면 한 source lock 지점을 1건으로 셉니다.
4. 다음 중 하나가 있으면 primitive/protocol gate로 분류합니다.
   - 컬렉션에서 제거하거나 필드를 null로 바꿔 단독 ownership을 이전
   - generation·exact-reference·seal token 재검증
   - 완료까지 유지되는 claim/reservation counter
   - immutable Task/TCS completion signal
   - queue/worker/dispatcher 자체가 해당 handoff의 소유 primitive
5. 위 증명이 없고 mutable authorization만 넘기면 잔존 결함입니다.

단위는 변수나 caller 수가 아니라 **source lock 문 위치**입니다.

### 잔존 결함 3곳

모두 [ZLinkActorMessageFollowLease](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkActorHandoffState.cs:1918)의 판독/claim입니다.

- L1930 `IsActive`: true라는 판독 뒤 route 또는 retry send가 실행되지만 lease generation/claim을 넘기지 않습니다.
- L1947 `IsCommitted`: true 확인 뒤 [dispatcher가 비동기 ActorQueue에 enqueue](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkActorMessageFollowDispatcher.cs:64)합니다. 실제 실행 시 [첫 시도는 `IsActive`를 우회](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkActorMessageFollower.cs:518)하므로 queue 대기 중 만료·취소돼도 한 번 전송할 수 있습니다.
- L1976 `TryBeginMessageFollowNotice`: liveness 확인과 suppression claim 뒤 nonblocking notification을 보내지만, `Cancel()`이 그 사이 실행돼도 send authorization을 철회하거나 generation으로 거부할 수 없습니다.

이는 Message Follow가 `MessageFollowDuration` 안에서만 전달되어야 한다는 [host relocation 계약](/home/hep7/project/zlink/framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md:682)과 충돌할 수 있습니다. 단순 primitive 제외로 처리할 수 없습니다.

## 2. 스펙 06 개정안 전문

아래는 파일을 수정하지 않은 제안 diff입니다. 발견 번호는 [rules.ko.md §7](/home/hep7/project/zlink/doc/plan/concurrency-redesign/rules.ko.md:183)을 그대로 유지했습니다.

```diff
--- a/framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md
+++ b/framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md

@@ §4. 상태 분류와 판별 기준 — C2의 semaphore 금지 문단 뒤

+### 상태 보호와 작업 프로토콜 직렬화를 구분한다
+
+외부 비동기 작업 전체를 하나씩 실행하기 위한 semaphore·socket gate·dispose gate는
+컴포넌트 mutable 상태를 보호하는 state lane과 다른 책임이다. 다음 조건을 모두 만족할
+때만 작업 프로토콜 primitive로 유지할 수 있다.
+
+- gate가 보호하는 것은 외부 resource operation의 시작·종료 또는 exact-once disposal이며,
+  C2 상태의 일부를 별도로 소유하지 않는다.
+- operation을 시작하기 전에 필요한 generation·identity·ownership을 gate 안에서 확정한다.
+- gate를 놓은 뒤에는 mutable 상태 snapshot이 아니라 Task, reservation, seal token 또는
+  단독 ownership을 이전받은 resource를 사용한다.
+- 완료 경로는 같은 ownership을 정확히 한 번 반납한다.
+
+작업 프로토콜 gate 안에서 state lane 완료를 기다리는 것은 허용할 수 있지만, 반대 방향인
+state lane turn 안에서 그 gate의 획득이나 장기 operation 완료를 기다리면 안 된다. 두 방향을
+모두 허용하면 서로가 서로의 완료를 기다리는 역방향 deadlock이 생긴다.
+
+이 예외는 C2 상태를 여러 lock으로 나누는 허가가 아니다. 같은 불변식에 참여하는 field와
+collection은 계속 하나의 state lane이 소유한다.

@@ §5. state lane의 보장과 제약 — 재진입 설명 뒤

+### 완료 신호와 블로킹 호환 경계
+
+Lane work의 완료 신호는 lane의 현재 소유권 표시가 해제된 뒤 caller continuation을 실행해야
+한다. 완료 API가 dependent continuation을 완료 thread에서 inline 실행할 수 있는 언어에서는
+완료 신호를 lane-current scope 안에서 직접 완료하지 않는다. 완료를 scope 밖 scheduler에
+게시하거나 비동기 continuation을 강제한다.
+
+기존 동기 표면이 state lane 완료를 블로킹 대기하는 호환 경계는 다음 조건을 모두 만족할
+때만 허용한다.
+
+- 제출한 lane 항목이 현재 보유 중인 외부 gate를 다시 획득하지 않는다.
+- lane 항목의 모든 완료 신호는 continuation을 비동기로 실행한다.
+- 해당 동기 표면이 반환되기 전에 상태 등록·캡처가 완료돼야 하는 계약이 있거나, 공개 동기
+  signature를 이 전환에서 변경할 수 없다는 사유가 기록돼 있다.
+
+한 조건이라도 확인할 수 없으면 gate를 보유한 채 lane 완료를 기다리지 않는다. 호출 경로를
+비동기로 전파하거나 gate와 lane의 책임을 다시 분리한다.

@@ §6. 재진입 제거 규칙 — 유형 ② 교체 및 유형 ③ 추가

-**유형 ② — lane turn 안에서 시작한 장기 비동기 작업이 lane 소유권을 상속하는 경우.**
-Turn 안에서 timeout처럼 지연 후 완료되는 비동기 작업을 시작하면, 그 작업이 실행되는
-문맥이 "지금 이 lane 위에서 실행 중"이라는 표시를 그대로 물려받을 수 있다. 지연이 끝난
-뒤 그 작업이 같은 lane에 다시 들어가려 하면, 실제로는 원래 turn이 이미 끝난 뒤인데도
-재진입으로 검출된다. 이런 장기 작업을 시작하는 자리에서는 실행 문맥의 흐름을 끊어 그
-작업이 새 호출자로 lane에 진입하게 만든다 — lane 소유권 표시가 비동기 문맥으로
-상속되는 언어에서는 장기 작업을 시작하는 지점에서 그 상속을 명시적으로 끊는다.
+**유형 ② — lane turn 안에서 시작한 장기 비동기 작업이 lane 소유권을 상속하는 경우.**
+Turn 안에서 timeout·retry·background loop 같은 작업을 시작하면, 그 작업이 "현재 lane 위에서
+실행 중"이라는 실행 문맥 표시를 상속할 수 있다. 장기 작업의 첫 lane 재진입은 원래 turn이
+끝난 뒤 실행되더라도 거짓 재진입으로 검출된다.
+
+장기 작업을 시작하는 지점에서는 실행 문맥의 흐름을 끊는다. 단, 문맥 흐름 억제는 비동기
+작업이 실제로 thread 전환을 한 뒤에만 효력이 있다. 호출한 async 함수의 동기 prefix가 첫
+await 전에 같은 lane에 재진입할 수 있으면 문맥 억제만으로 충분하지 않다. 이 경우 작업
+시작 자체를 별도 scheduler에 게시해 동기 prefix도 원래 turn 밖에서 실행되게 한다. 반대로
+첫 동작이 실제 비동기 지연이고 그 전에는 lane 재진입이 없으면 문맥 흐름 억제만으로 충분하다.
+
+**유형 ③ — 기존 배타적 접근 안에서 외부 callback을 호출하던 경우.**
+Monitor 재진입으로 동작하던 callback을 lane turn 안에서 직접 호출하면 callback이 같은
+컴포넌트의 public 표면에 재진입한다. 다음 세 단계로 분리한다.
+
+1. turn A에서 검증, 결과 산출, 원본이 callback 전에 끝내던 상태 전이 전부와 placeholder
+   ownership claim을 완료한다.
+2. callback은 lane turn 밖에서 호출한다.
+3. turn B에서는 exact placeholder를 callback 결과 Task로 교체하거나 실패를 정산한다.
+
+원본이 callback 전에 끝내던 상태 전이를 turn B로 미루지 않는다. Callback 실행 창의 경쟁
+관찰자는 원본의 "배타적 접근이 끝난 뒤"와 같은 상태를 봐야 한다.

@@ §7. 시그니처 전환 규칙 — 기존 out 규칙 뒤

+- **반환 전 완료 보장을 보존한다.** 원본 동기 메서드가 waiter 등록, epoch·generation 캡처,
+  store 판독 또는 exact ownership claim을 반환 전에 완료했다면, 전환 뒤에도 caller가
+  반환을 관찰하기 전에 그 작업이 완료돼야 한다. 비동기 fire-and-forget 게시로 바꾸지 않는다.
+- 위 보장을 유지하기 위해 동기 호환 경계가 필요하면 §5의 deadlock 조건을 확인하고 사유를
+  기록한다. 완료 신호를 기다리는 이후 단계는 비동기로 남길 수 있지만, 등록·캡처 자체를
+  반환 뒤로 미루지는 않는다.
+- 공개 또는 언어별 exact interface가 동기 계약인 경우, state lane 도입만을 이유로 Promise나
+  Task 반환으로 바꾸지 않는다. 내부 호출자가 이미 비동기이고 관측 계약이 변하지 않을 때만
+  async signature를 전파한다.

@@ §8. 전환 단위와 검증 — 전환 경계 문단 교체

-- **전환 경계는 기존 gate 단위를 그대로 쓴다.** 배타적 접근 하나가 지키던 범위가 이미
-  소유 단위다. 전환한다고 해서 그 경계를 넓히거나 쪼개지 않는다 — 경계 재설계는 이
-  전환의 목적이 아니다.
-- **전환 단위는 클래스 하나다.** 한 컴포넌트(그 클래스가 가진 배타적 접근 하나)를
-  한 번에 옮긴다.
+- **전환 경계는 기존 gate가 소유하던 상태 영역을 그대로 쓴다.** 클래스 하나에 서로 독립적인
+  gate가 여러 개 있었다면 각각을 별도 ownership region으로 옮길 수 있다. 이 경우 두 영역에
+  걸친 field·collection 불변식이 없고, 영역 사이의 호출 방향이 단방향임을 기록한다.
+- 교차 불변식이나 양방향 대기가 하나라도 있으면 여러 lane으로 나누지 않고 한 ownership
+  region으로 합친다. "클래스 하나"는 기본 작업 단위이지, 한 클래스 안에 근거 없이 여러
+  state lane을 만드는 허가가 아니다.
+- socket·completion·worker 같은 작업 프로토콜 gate는 §4의 조건을 만족할 때만 state lane
+  전환 대상에서 제외한다. 제외 사유에는 ownership transfer, generation fence, completion
+  방식과 lock-order를 기록한다.

@@ §8. 성공 지표 문단 뒤

+Async 경계 snapshot 계수 단위는 source의 배타적 접근 위치다. 단순 문자열 검색 결과를
+사용하지 않고 실제 언어 token을 센다. 각 위치에서 배타적 접근 안에서 산출한 값·참조·결정이
+다음 중 하나를 넘어 사용되는지 추적한다.
+
+- await 또는 Task·Promise·future 반환
+- detached task, queue, worker thread 또는 callback dispatcher 제출
+- 비동기 continuation을 실행하는 completion signal
+- nonblocking transport operation 제출
+
+그 경계를 넘더라도 immutable completion signal, exact token, reservation 또는 단독 ownership
+transfer로 유효성이 고정되면 primitive/protocol 제외군으로 따로 센다. Mutable authorization을
+그대로 사용하면 잔존 결함이다. 최종 보고는 `전체 / 제외군 / 잔존 결함` 세 값을 모두 적는다.

@@ §9. 언어별 매핑 — 기존 문단 뒤

+.NET은 lane 소유권 표시가 `AsyncLocal`을 사용하므로 장기 작업 시작 시
+`ExecutionContext.SuppressFlow`를 사용한다. Async 함수의 동기 prefix가 lane에 재진입할 수
+있으면 `Task.Run` 등으로 시작 자체를 별도 scheduler에 게시한다.
+
+Java는 lane-current `ThreadLocal` scope 안에서 `CompletableFuture.complete`를 호출하지 않는다.
+`complete`는 비동기 표시가 없는 dependent를 완료 thread에서 inline 실행할 수 있다. Current
+scope를 해제한 뒤 완료하거나 `completeAsync`와 같은 별도 scheduler 완료를 사용한다.
+
+Node.js의 동기 메서드는 하나의 JavaScript turn 안에서 끝나는 동안 다른 callback과 동시에
+실행되지 않는다. 따라서 await 전후로 상태 접근이 갈라지지 않고 public 재진입 검출도 필요
+없는 동기 surface는 Promise로 바꾸지 않는다. State lane 대상은 await 전후 상태가 갈라지는
+비동기 경로와 재진입 검출이 필요한 표면이다.

@@ §10. 검증 요구 — 재진입과 실행 순서 항목 뒤

+- Lane turn에서 시작한 장기 작업은 원래 turn이 끝난 뒤 같은 lane에 정상 진입하며 거짓
+  재진입 예외를 만들지 않는다.
+- 동기 prefix가 있는 장기 작업도 원래 turn 밖에서 시작됨을 확인한다.
+- 완료 continuation은 lane-current scope 밖에서 실행된다.
+- 외부 callback은 turn 밖에서 실행되며, callback이 관찰하는 상태는 원본 배타적 접근 종료
+  시점과 같다.
+- 작업 프로토콜 gate를 보유한 채 lane 완료를 기다리는 허용 사례는 lane 항목이 gate를
+  재획득하지 않으며 모든 completion continuation이 비동기임을 확인한다.
+- 반환 전 등록·캡처 계약이 있는 메서드는 caller가 반환을 관찰할 때 등록·캡처가 이미
+  완료돼 있다.
+- async 경계 snapshot 재측정에서 잔존 결함이 0이고, primitive/protocol 제외 위치에는 각각
+  유효성 보존 근거가 기록돼 있다.
```

## 3. 마일스톤 리뷰

### 발견 수

- `[H]` 1건
- `[M]` 4건
- `[L]` 1건

### [H] Message Follow authorization snapshot 3곳

위 3개 lease 판독/claim은 durable generation이나 실행 claim이 아닙니다. 특히 `IsCommitted` 후 enqueue와 실행 사이에는 실제 queue 대기가 있고, 첫 전송은 active 재검증을 건너뜁니다. CP3 목표인 실질 잔존 0을 충족하지 못합니다.

권장 방향은 bool 반환이 아니라 다음 중 하나입니다.

- lease generation을 포함한 immutable send claim을 만들고 실제 send 직전 exact generation을 재검증
- enqueue 시점이 아니라 ActorQueue 실행 turn에서 active lease를 claim
- 만료·취소가 outstanding claim을 철회하고 send가 그 terminal 상태를 확인

### [M] 제외 6 판정

[progress §2](/home/hep7/project/zlink/doc/plan/concurrency-redesign/progress.ko.md:62)의 제외 6은 실제 lock **68개**, snapshot 후보 **28개**입니다.

| 제외 대상 | 판정 |
|---|---|
| `ZLinkSerialExecutionQueue` | 타당. 스펙 §9가 명시한 application/lifecycle primitive |
| `ZLinkSpotSerialExecutor` | 타당하나 스펙에 범주 근거 추가 필요. 실행 queue와 relocation barrier 소유 |
| `ZLinkWorkerPool` | 타당. worker admission·queue·thread lifecycle primitive |
| `ZLinkMeshCompletionTable` | 조건부 타당. dispatcher reservation과 exact pending ownership을 한 primitive로 봐야 함 |
| `ZLinkCompletionDispatcher` | 타당. 무할당 terminal hot path와 preallocated node ownership이 핵심 |
| `ZLinkSerialWorkItem` | 타당. 독립 state-owner가 아니라 serial queue 하위의 immutable payload memoization |

다만 진행표의 `ZLinkCompletionDispatcher (5)`는 현재 실제 lock **4개**입니다. 제외를 class allowlist로만 두지 말고 위 diff처럼 ownership/fence/completion 조건으로 규범화해야 합니다.

### [M] 보류 3 판정

진행표가 “보류 3” 명단을 직접 열거하지 않아, 현재 source와 [small-class survey](/home/hep7/project/zlink/doc/plan/concurrency-redesign/small-class-survey.ko.md:139)를 대조한 해석입니다.

1. `ZLinkActorMessageFollowLease` — **보류 불가**. 위 3개 실질 결함 해소 필요.
2. `ZLinkClientServerRuntimeService` — **조건부 유지 가능**. Outer gate의 hub 등록과 exact-reference 제거는 stable subscription claim입니다. `MonitorHub`는 observer protocol primitive로 재분류하고 사유를 명시해야 합니다.
3. `ZLinkInstanceSpotMonitoring.Aggregate` — async snapshot은 0이므로 진짜 지표상 안전합니다. 하지만 현재 스펙 §4의 “C2면 lane” 규칙과 충돌합니다. 동기 metrics aggregate 예외를 명시하거나 변환해야 하며, 지금처럼 무기록 보류로 남기면 안 됩니다.

`ActorQueue`·`ActorLane`은 각각 execution queue retirement primitive로, `ManagedSpot`은 C1+C3 동기 registry로 재분류하는 것이 일관됩니다.

### [M] 한 클래스에 여러 state lane인 경계가 스펙과 어긋남

스펙은 “컴포넌트 하나에 state lane 하나”라고 하지만, [ZLinkManagedMeshNode](/home/hep7/project/zlink/framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:51)는 현재 5개 state lane을 가집니다.

이 구조가 반드시 잘못됐다는 증거는 없지만, 각각이 독립 ownership region이라는 불변식·호출 방향 증명이 스펙과 진행 기록에 없습니다. 클래스 단위 C2 판정과 기존 gate 단위 전환 규칙도 서로 충돌합니다. 스펙 diff의 §8 문안처럼 ownership region과 교차 불변식 판정이 필요합니다.

### [M] `AwaitStateLane` 656 회수 우선순위

656은 실제 bridge 수가 아닙니다.

- textual occurrence: **656**
- helper 선언: **133**
- 실제 호출: **523**
- 포함 파일: **72**

따라서 회수 기준값은 523 호출로 보정해야 합니다. 상위 7개 파일이 216개, 실제 호출의 약 41%입니다.

| 실제 호출 | 파일 |
|---:|---|
| 64 | `ZLinkActorHandoffState` |
| 40 | `ZLinkActorBoundSessionCoordinator` |
| 35 | `ZLinkFrameworkRuntime` |
| 24 | `ZLinkManagedMeshNode` |
| 20 | `ZLinkApplicationJobQueue` |
| 17 | `ZLinkStreamNodeRuntime` |
| 16 | `ZLinkRouteMeshRuntimeService` |

회수 순서는 다음이 적절합니다.

1. **P0 — hot path**
   - Actor frame route, bound-session lookup/fence, application-job admission, mesh send/selection, stream ingress, codec cache.
   - 동기 getter마다 lane을 블로킹하지 말고, 전체 caller turn을 lane으로 올리거나 lane이 immutable aggregate snapshot을 publish하도록 변경합니다.
   - 변경 전 allocation·queue wait·contention을 계측해야 합니다.

2. **P1 — 이미 async인 내부/lifecycle caller**
   - Private leaf부터 `ValueTask`를 그대로 반환하고 상위 caller가 `await`하도록 전파합니다.
   - relocation, start/stop, dispose, location/store 경로가 주 대상입니다.
   - discovery 9의 반환 전 등록·캡처가 없는 호출부터 처리합니다.

3. **P2 — 사유 기록 후 잔존**
   - exact sync public/interface 계약
   - 생성자·동기 property·callback ABI
   - 반환 전 등록·epoch capture가 필요한 discovery 9 경계
   - 외부 gate 안 blocking wait가 discovery 2 조건을 충족하는 경계

잔존 기록에는 `file:line`, 호출 계약, hot/cold, 반환 전 완료 필요성, gate 재획득 부재, `RunContinuationsAsynchronously` 증거를 포함해야 합니다. 최종 목표는 단순한 textual 0보다 **미분류·무근거 bridge 0**으로 두는 편이 발견 2·9와 모순되지 않습니다.

### [L] 지표 문서 drift

[progress 상단](/home/hep7/project/zlink/doc/plan/concurrency-redesign/progress.ko.md:23)은 `AwaitStateLane 121+`, 지표 표는 [656](/home/hep7/project/zlink/doc/plan/concurrency-redesign/progress.ko.md:81)입니다. `lock` 118의 substring 오탐과 함께, CP3 숫자는 token/call-site 기준으로 다시 정의해야 합니다.

## 최종 결론

- **lock 감축 자체는 충분히 진행됐습니다.**
- **진짜 지표는 48이며, 그중 45는 정당화 가능한 제외군입니다.**
- **Message Follow 3곳이 남아 실질 잔존 0은 아직 달성하지 못했습니다.**
- 스펙 06에는 발견 2·4·5·6·7·8·9와 ownership-region/제외군 계수 규칙을 반영해야 합니다.
- 전체 CP3 런타임 게이트는 사용자 금지 조건 때문에 검증하지 않았습니다. 현재 결론은 source·spec 정적 판정입니다.
