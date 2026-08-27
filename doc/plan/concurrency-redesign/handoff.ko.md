# 이어받기 — lane 소유 동시성 전환

이 문서 하나로 새 세션이 이어받을 수 있게 쓴다. 설계 근거는
[design.ko.md](design.ko.md), 이 문서는 **지금 어디까지 됐고 다음에 무엇을 하는가**다.

작성: 2026-08-26 · 갱신: 2026-08-27 · 브랜치 `refactor/lane-ownership-concurrency` (base `3cbfbde4f9`)

## 0. 지금 이어받기 (2026-08-27 — 캠페인 꼬리)

**본체는 끝났다.** 4언어 L0·L1·L2 전부 종결, async 경계 스냅샷 실질 0(cp3-audit 전수),
dotnet lock 999→113(−89%, 잔존 전부 정당화), 스펙 06 개정(발견 1~9 반영, `078d1e22b6`),
Z0 cross-language e2e 전쌍 통과. **살아 있는 상태는 [progress.ko.md](progress.ko.md) §4
추적표(T1~T9)가 정본**이고, 규칙·게이트·요청 템플릿·STOP·발견 로그·기존 실패 목록은
[rules.ko.md](rules.ko.md)다. 새 세션은 **progress §4 → §1 매트릭스 → rules 전체** 순으로 읽는다.

### 남은 일 (progress §4와 일치해야 한다)

1. **T6/Z4 — dotnet ZoneWorld 이분 판정. 최우선.**
   조용한 환경 ×8 측정 완료: **pass 1 / fail 7**, 모드 매런 상이(ZW-B8×2·D1×2·B4·G4·C2 —
   progress T6 상세). "부하 오염" 가설 기각. **기준선 worktree가 준비돼 있다**:
   `/home/hep7/project/zlink-z4base` = base `3cbfbde4f9` checkout + `.artifacts` 심링크 완료
   (dotnet 샘플도 `.artifacts` 없으면 즉시 실패한다 — 심링크 필수).
   - 기준선 트리의 `framework/languages/dotnet/samples`에서
     `MSBUILDDISABLENODEREUSE=1 flock -w 14400 /tmp/zlink-dotnet-gate.lock timeout 1200 bash
     run_samples.sh ZoneWorld` ×8 (첫 런은 빌드 포함 — timeout 1200 필요)
   - 모드 분포를 현행(1/8)과 비교해 **기존 결함 vs 캠페인 회귀 판정**(TTT 판정 절차 `d0bb9d95a3`)
   - 회귀로 나오면 콘솔 계측 추가 말고 **기존 `.flow`(message tracking)부터** 켜서 진단.
     임시 로깅은 add-then-delete. 원인 확정 시 spec gap 3분류(스펙 근거/샘플 스펙/서버 spec gap) 기록.
   - dotnet 판정 후 cpp ZoneWorld ×8 동일 측정.
2. **T10 — CP3 감사 마무리.** node는 **보고 완료**(cp3-audit-node.ko.md — NOT CLEAN,
   결함 의심 5건 H2·M3): 의심 5건을 codex로 검증(재현 또는 반박)하고 실증되면 수정+테스트.
   **cpp·jvm 감사는 세션 종료로 미착수** — dotnet cp3-audit.ko.md와 같은 방법론으로 sol에 재요청
   (요구 4항목: ①lock 취득 전수 계수 ②async 경계 스냅샷 전수 [정당화|결함 의심] 분류표
   ③호환 경계 목록·잔존 사유 ④posddd ② 상위 10). 읽기 전용이라 Z4 측정과 병렬 가능.
3. **T5 — java·cpp 샘플 스위트 재실행.** java는 teardown 근본 수정(`e4944785a1`) 반영 확인,
   cpp는 TTT 기존 간헐(rules §4)을 감안해 TTT 외 그린 판정.
4. **T4 — java FrameworkModuleBoundaryTest ReceiveFlowState import 1건** 캠페인 원인 여부 분류.
5. **T9 — 추적표·메모리 최종 정리 + `spec/` 트리 재잠금**(§8-4) + worktree 정리
   (`git worktree remove /home/hep7/project/zlink-z4base`) 후 **main 병합 판단은 사용자**.

### 세션 운영 규칙 (요약 — 정본은 rules.ko.md)

- **Claude는 감독·리뷰·판정·게이트·커밋만.** 구현은 codex
  (`codex exec -m gpt-5.6-terra -c model_reasoning_effort="high" -s danger-full-access
  --skip-git-repo-check ... < /dev/null` — **`< /dev/null` 필수**, 조사는 `-m gpt-5.6-sol`).
- **게이트는 중앙(Claude)만 실행, 에이전트 집계 보고 맹신 금지**(부정확 보고 3회 실증 —
  집계 원문을 요구한다). 트리 락 4개(`/tmp/zlink-{dotnet,cpp,node,jvm}-gate.lock` flock),
  java·kotlin은 Gradle 트리 공유라 동시 빌드 금지. dotnet은 `MSBUILDDISABLENODEREUSE=1`,
  행 의심 시 `--blame-hang --blame-hang-timeout 8m`.
- cpp 중앙 빌드는 항상 `framework/languages/cpp/build`(에이전트 스테일 빌드 디렉터리 허위
  보고 사례 3회).

## 1. 이 작업이 왜 시작됐나

사용자 관찰: **"두 달 넘게 뭐 하나 수정하면 samples에서 flake 잡는 데 시간이 너무 많이 든다.
작은 기능 하나 추가해도 오래 걸린다."** 원인 가설이 "lock이 흩어져 있어서"였고, 세어 보니
맞았다.

| 계층 | mutex/lock 선언 | lock 취득 지점 |
|---|---:|---:|
| `core/src` (ZeroMQ 계열 transport) | 31 | 262 |
| cpp framework | 133 | 1,303 |
| **dotnet framework** | `_gate` 보유 클래스 **61** | **999** |

dotnet 999개 중 **620개가 `_gate`** 하나에 걸려 있다.

### 기계적 원인

**C# `lock`은 `await`을 감쌀 수 없다.** 그래서 async 경로는 예외 없이 이 형태가 된다.

```csharp
lock (_gate) { if (!_entries.TryGetValue(key, out entry)) return; }   // 여기서 해제
await SendAsync(entry.Route);                                         // 낡았을 수 있는 값으로 실행
```

트리에 ~465곳. **실수가 아니라 메커니즘이 강제하는 형태다.** 스냅샷은 구조적으로 낡을 수 있고,
그래서 로직 한 줄을 추가하면 그 줄이 어떤 gate를 잡고 어느 스냅샷을 쓰는지에 따라 동작이 바뀐다.

cpp에서 실제로 잡은 결함이 정확히 이 모양이었다 — resolve는 `match=true`인데 lock 밖 write가
`unavailable`을 돌려주고, route tombstone은 실패 **뒤에** 왔다(`progress.ko.md` §8.1.5).

## 2. 참조 구현 — playhouse

사용자가 예전에 만든 https://github.com/kairos-code-dev/playhouse 를 참조한다.
14,663줄에 `lock (` **10개**뿐이고 그마저 전부 transport/session 쪽이다.

핵심은 `src/PlayHouse/Core/Play/Base/BaseStage.cs`의 60줄이다.

```csharp
private readonly Dictionary<string, BaseActor> _actors = new();   // ← 평범한 Dictionary
private readonly ConcurrentQueue<StageMessage> _mailbox = new();
private int _isScheduled;

private void ScheduleExecution() {
    if (Interlocked.CompareExchange(ref _isScheduled, 1, 0) == 0)
        ThreadPool.QueueUserWorkItem(_ => _ = ExecuteAsync());
}
```

`_actors`가 `ConcurrentDictionary`가 **아니라는 것**이 전부다. lane이 소유권을 보장하므로
잠글 이유가 없다. `BaseStage`·`BaseActor`에 lock 0개.

## 3. 확정된 설계

상태를 셋으로 가르고, 분류가 정해지면 전환은 기계적이다. 자세한 판별 기준은 design.ko.md §4.

- **C1** 순수 조회 레지스트리(map 하나, 교차 불변식 없음) → `ConcurrentDictionary`, lock 삭제
- **C2** 여러 컬렉션에 걸친 불변식 → **lane 소유**, 컬렉션은 평범한 `Dictionary`로 두고 안 잠금
- **C3** 원자 카운터·플래그 → `Interlocked`

**C2에서 `ConcurrentDictionary`를 쓰면 안 된다.** 원자성이 map 하나 단위로 쪼개져 불변식이 깨진다.

### 경계를 새로 긋지 않는다

`_gate` 620개가 61개 클래스에 몰려 있다는 것은 **소유 단위가 이미 정해져 있다**는 뜻이다.
틀린 것은 경계가 아니라 메커니즘이다. 클래스를 합치거나 쪼개지 않는다.

## 4. 지금까지 만든 것 (전부 커밋됨)

| 파일 | 내용 | 상태 |
|---|---|---|
| `doc/plan/concurrency-redesign/design.ko.md` | 설계 — 분류 기준·전환 규칙·검증 기준·적용 순서 | 완료 |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Execution/ZLinkStateLane.cs` | lane primitive | 빌드 통과 |
| `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/StateLaneTests.cs` | primitive 명세 테스트 | **14/14 통과** |

### `ZLinkStateLane`이 보장하는 것

테스트가 곧 명세다. 특히 둘이 중요하다.

- `ConcurrentCallers_MutateUnsynchronizedStateWithoutLosingUpdates` — 32스레드가 **잠금 없는
  평범한 `Dictionary`**에 1,600회 써도 유실 0. "lane이 소유하면 안 잠가도 된다"의 증명이다.
- `ReenteringTheSameLane_FailsInsteadOfHanging` — 재진입이 데드락이 아니라 예외로 터진다.

**재진입 금지는 이 설계의 근본 제약이다.** 이 코드베이스는 cpp에서 `recursive_mutex`를 쓸 만큼
재진입이 있으므로, 전환 중 반드시 만난다. 그래서 primitive에 검출을 넣었다.

### 기존 `ZLinkSerialExecutionQueue`를 쓰지 않은 이유

이미 있고(1,118줄) Spot/Actor **실행**에 쓰인다. 다만 relocation seal·lifecycle admission이
얹혀 있어 **상태 소유**용으로는 과하다. 실행용은 그대로 두고 소유용만 새로 뒀다.

## 5. 표본 전환 — **완료 (2026-08-26, `3cc6f5f615`)**

결과와 실측(재진입 2곳, 호환 경계 25곳, 32분)은
[sample-conversion-report.ko.md](sample-conversion-report.ko.md). 다음 단계는 §6의
순서 재산정과 lane 소유 규칙의 `spec/server/01-execution` 승격(작성=sonnet, Fable=리뷰)이다.
아래는 표본 당시의 작업 명세를 기록으로 남긴 것이다.

**대상: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Actors/ZLinkSessionActorBindingTable.cs`**

### 이미 조사해 둔 사실 (다시 세지 말 것)

- 1,938줄, `lock` **30개 — 전부 `lock (_entries)`**
- 소유 컬렉션 **5개**: `_entries`, `_tombstones`, `_outbound`, `_canonicalSealTimeouts`,
  `_timedOutCanonicalSeals`
- **30개 블록 중 14개가 컬렉션 2개 이상을 함께 만진다** → 클래스 전체가 **C2 확정**.
  `ConcurrentDictionary` 치환은 불가.
- public/internal 메서드 **37개**
- **이 클래스를 쓰는 파일은 2개뿐** — 자기 자신과
  `Runtime/Host/ZLinkActorBoundSessionCoordinator.cs`
- 호출 지점 **35곳**(필드명 `_sessionBindings`), 그중 **22곳은 이미 async 메서드 안**.
  나머지 13곳만 시그니처를 바꾸면 된다.

즉 **파급이 한 파일에 갇혀 있다.** 표본으로 고른 이유가 이것이다.

### 절차

1. `lock (_entries)` 30곳을 C1/C2/C3로 표시한다 (위 조사대로 전부 C2)
2. **재진입을 먼저 걷어낸다.** lane 안에서 이 클래스의 public 메서드를 다시 부르는 자리를
   private 메서드로 분리한다. 안 하면 데드락이고, primitive가 예외로 잡아 준다.
3. 컬렉션 5개에서 `lock` 제거, 각 메서드 본문을 `_lane.RunAsync(...)`로 감싼다.
   **블록 본문은 한 글자도 바꾸지 않는다.**
4. `out bool` → `ValueTask<T?>` 로 시그니처를 바꾸고 호출자 35곳을 `await`으로 전파한다
5. 검증

```csharp
// before
internal bool TryGetMemoizedOutboundProof(tenure, out proof)
{
    lock (_entries) { /* 컬렉션 A + entry 상태 + 컬렉션 B */ }
}

// after
internal ValueTask<ZLinkSessionOutboundTenureProof?> GetMemoizedOutboundProofAsync(tenure) =>
    _lane.RunAsync(() => { /* 같은 본문, lock 없음 */ });
```

### 검증 기준 (하나라도 못 넘으면 다음으로 가지 않는다)

- `dotnet test tests/Zlink.Framework.UnitTests` → **1879 통과 / 0 실패** 유지
  (여기에 StateLaneTests 14개가 더해지므로 1893이 될 수 있다. 실패 0이 기준이다)
- `framework/languages/dotnet/samples/run_samples.sh` → 6샘플 placement marker 유지
  (ZoneWorld는 아래 §7의 기존 결함으로 실패한다. 이 작업과 무관)

### 이 표본에서 반드시 기록할 것

나머지 60개 클래스 추정의 유일한 근거다.

- 재진입이 실제로 몇 곳이었나
- 시그니처가 async로 번진 호출자가 몇 개였나
- 걸린 시간

## 6. 그 다음 순서

design.ko.md §7의 순서는 `_gate` 밀도로 정한 초안이다. **표본이 끝나면 다시 정한다** —
파급 범위(그 클래스를 쓰는 파일 수)를 먼저 세고 작은 것부터 하면 빠르게 누적된다.

| 후보 | lock |
|---|---:|
| `Runtime/Spots/ZLinkSpotNodeCatalog.cs` | 48 |
| `Runtime/Actors/ZLinkActorHandoffState.cs` | 63 |
| `Runtime/Actors/ZLinkActorRuntimeState.cs` | 35 |
| `Runtime/Service/ZLinkManagedMeshNode.cs` | 141 (가장 크고 파급도 클 것이다. 마지막) |

**전환 후 예상: dotnet 995 → 60 안팎(약 94% 감소).** 남는 60은 lane·큐 내부 구현이라
없애면 안 된다.

**다만 lock 개수는 성공 지표가 아니다.** 진짜 지표는 **"async 경계를 넘는 스냅샷" 465곳이
0이 되는 것**이다. `lock`을 `SemaphoreSlim`으로 바꾸기만 해도 숫자는 0이 되지만 문제는 그대로다.

## 7. 이어받을 때 알아야 할 제약

- **현황은 [progress.ko.md](progress.ko.md)(진행표 — 여기서만 상태 갱신), 규칙·프로토콜은
  [rules.ko.md](rules.ko.md)가 정본이다.** 이 문서는 배경·설계 근거·이어받기 요약(§0)만 담는다.
- **.NET이 규칙의 정본이다.** 다른 언어는 .NET에서 확정된 규칙을 옮긴다. 다만 언어별
  primitive 포팅(progress §4 L0)은 dotnet 전환과 무의존이라 **지금 병렬로 착수 가능**하다.
  빌드 트리는 4개(dotnet·cpp·node·JVM)이고 **java와 kotlin은 Gradle 트리를 공유하므로
  절대 동시에 빌드하지 않는다.**
- **스펙이 정한 관측 가능한 동작은 바꾸지 않는다.** 순서·타임아웃·오류 코드 그대로.
  전환이 관측 동작을 바꿔야만 통과된다면 그건 스펙 문제일 수 있으므로 **임의로 바꾸지 말고
  올린다**(progress §5 STOP 조건).
- **스펙 수정은 lane 규칙에 한해 열려 있다.** 이 작업이
  `spec/server/01-execution/06-state-ownership-and-lanes.{ko,en}.md`를 신설했고(`0fe461fc6c`),
  전환 중 나온 새 유형은 progress §7에 모았다가 마일스톤에서 스펙 06에 반영한다.
  **그 외 스펙 문서는 이 작업으로 수정하지 않는다.**
- **`main`에 직접 커밋하지 않는다.** 이 브랜치에서 작업한다.
- 에이전트에게 맡길 때는 스펙 수정 금지·`git add/commit/stash/reset/checkout` 금지를
  프롬프트에 넣는다. 이 세션에서 codex가 검증 없이 변경만 남기고 죽은 사례가 있었고, 그 변경이
  단위 테스트를 깨뜨려 되돌렸다.

## 8. spec-server-reorg에서 넘어온 4건 — 현재 상태 (2026-08-27)

spec-server-reorg 캠페인은 본체가 끝나 `3cbfbde4f9`로 `main`에 푸시됐다. 넘어온 4건은 이
캠페인의 Z1~Z4로 흡수됐다. 상세는 `doc/plan/spec-server-reorg/progress.ko.md` §8.1.

1. **Z1 — cpp ZoneWorld 세션 split-brain**: **수정 커밋됨.** 교체된 peer의 stale 후보 고정
   재시도가 원인 — (RID, lifecycle, epoch) operation-local 제외+후보 재조회로 수정
   (spec 03-mesh-node §5.2·§6, 08-routing 근거).
2. **Z2 — dotnet ZoneWorld mesh admission**: **수정 커밋됨**(Z1과 같은 계열,
   `ZLinkActorManagerService`+`ZLinkMeshNodeTargetAvailability`). 0/3→3/5로 개선 실증했으나
   **Z4 조용한 측정에서 다른 모드로 계속 실패 중**(§0-1) — 잔여 결함 존재.
3. **Z3 — `fast_mutex.hpp:76` abort**: `shared_ptr<unique_lock>` 형태 코드 제거, 재현 0. **종결.**
4. **`spec/` 트리 재잠금** — 아직 안 했다. 캠페인 종료 시
   `chmod -R a-w framework/doc/framework/common/spec` 한 줄. (스펙 06 개정이 끝났으므로
   T9에서 실행)
