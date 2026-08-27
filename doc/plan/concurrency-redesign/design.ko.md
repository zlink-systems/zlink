# ZLink 동시성 기본 설계 — lane 소유 모델

대상: `.NET`(`framework/languages/dotnet/src/Zlink.Framework`) 먼저 적용. 나머지 세 트리는
이 설계가 검증된 뒤 같은 규칙으로 옮긴다.

## 1. 지금 상태 — 실측

| 항목 | 값 |
|---|---:|
| 코드 | 490 파일 / 154,448 줄 |
| `lock` 문 | **999** (99 파일) |
| 그중 `_gate` 대상 | **620** |
| `_gate`를 가진 클래스 | **61** |
| `lock` 안에서 값을 꺼내 밖에서 쓰는 자리 | ~465 |
| 이미 쓰는 것 | `ConcurrentDictionary` 54 · `Interlocked` 250 · `Channel<>` 12 · `SemaphoreSlim` 28 |

## 2. 왜 한 줄만 고쳐도 동작이 바뀌는가

**C# `lock`은 `await`을 감쌀 수 없다.** 그래서 async 경로는 예외 없이 이렇게 된다.

```csharp
ZLinkSessionBindingEntry entry;
lock (_gate) { if (!_entries.TryGetValue(key, out entry)) return; }   // 여기서 해제
await SendAsync(entry.Route);                                         // 낡았을 수 있는 값으로 실행
```

이건 실수가 아니라 **메커니즘이 강제하는 형태**다. `lock`을 쓰는 한 async 경계마다 스냅샷이
생기고, 스냅샷은 구조적으로 낡을 수 있다. 465곳이 이 형태인 이유이고, 로직 한 줄을 추가하면
그 줄이 어떤 gate를 잡고 어느 스냅샷을 쓰는지에 따라 동작이 바뀌는 이유다.

cpp에서 오늘 잡은 결함이 정확히 이 모양이었다 — resolve는 `match=true`인데 lock 밖 write가
`unavailable`을 돌려주고, route tombstone은 실패 **뒤에** 왔다.

## 3. 경계는 이미 그어져 있다

`_gate` 620개가 61개 클래스에 몰려 있다는 것은 **"컴포넌트 하나가 자기 상태 전부를 gate
하나로 지킨다"는 관례가 이미 있다**는 뜻이다. 소유 단위는 이미 정해져 있다. 틀린 것은 그
경계를 지키는 **메커니즘**이다.

그리고 lane primitive도 이미 있다 — `Runtime/Execution/ZLinkSerialExecutionQueue.cs`
(1,118줄, `TryPost`·`TryPostApplication`·`TryPostFinal`·relocation seal). 다만 Spot/Actor
실행에만 쓰이고 61개 gate 클래스는 쓰지 않는다.

**따라서 이 작업은 "lane을 만드는 일"이 아니라 "이미 있는 경계에서 메커니즘을 바꾸는 일"이다.**

## 4. 기본 설계 — 상태를 세 종류로 가른다

모든 `lock`은 아래 셋 중 하나로 분류하고, 분류가 정해지면 전환은 기계적이다.

### C1 — 순수 조회 레지스트리 → `ConcurrentDictionary`

단일 map이고, 연산이 `TryGet`/`TryAdd`/`TryRemove`로 끝나며, **다른 필드와 걸친 불변식이
없는** 경우.

```csharp
// before
private readonly object _gate = new();
private readonly Dictionary<K, V> _map = new();
lock (_gate) { _map[k] = v; }

// after
private readonly ConcurrentDictionary<K, V> _map = new();
_map[k] = v;
```

판별 기준: 그 `lock` 블록이 **컬렉션 하나만** 건드리고, 블록을 두 개의 원자 연산으로 쪼개도
불변식이 깨지지 않으면 C1이다.

### C2 — lane 소유 상태 → 단일 소유자 + 평범한 `Dictionary`

여러 컬렉션·필드에 걸친 불변식이 있거나, 결정을 내린 뒤 async로 행동하는 경우.
**`ConcurrentDictionary`로 바꾸면 안 된다** — 원자성이 map 하나 단위로 쪼개져 불변식이 깨진다.

```csharp
// 실제 예: ZLinkSessionActorBindingTable.IsLateCanonicalRoute
lock (_entries)
{
    if (_timedOutCanonicalSeals.Contains(key)) return true;   // 컬렉션 A
    if (!TryFindCanonicalBinding(...)) return true;           // 컬렉션 B
    if (entry.AppliedCanonicalRelocationRoute is not null) return true;  // 엔트리 상태
    return false;
}
```

전환 형태: 컴포넌트가 lane 하나를 소유하고, **상태를 만지는 모든 코드가 그 lane 안에서만
돈다.** lane 안에서는 잠금이 필요 없으므로 컬렉션은 평범한 `Dictionary`로 남긴다.

```csharp
private readonly ZLinkSerialExecutionQueue _lane;
private readonly Dictionary<K, Entry> _entries = new();   // lane 소유 — 잠그지 않는다

public ValueTask<bool> IsLateCanonicalRouteAsync(Record request) =>
    _lane.RunAsync(() => { /* 위 블록 그대로. lock 없음 */ });
```

**요점:** playhouse가 `_actors`를 `ConcurrentDictionary`가 아니라 평범한 `Dictionary`로
두는 이유가 이것이다. 소유권을 lane이 보장하므로 잠글 이유가 없다.

### C3 — 원자 카운터·플래그 → `Interlocked` / `volatile`

`lock`이 정수 증가, 플래그 확인, 단일 참조 교체만 하는 경우. 이미 250곳이 그렇게 하고 있다.

## 5. 전환 규칙

1. **한 번에 클래스 하나.** `_gate` 하나가 한 단위다. 경계를 새로 긋지 않는다.
2. **분류를 먼저 적는다.** 그 클래스의 모든 `lock` 블록을 C1/C2/C3로 표시하고 시작한다.
   섞여 있으면 C2가 이긴다(가장 강한 요구가 클래스를 결정한다).
3. **C2 전환 시 재진입을 먼저 걷어낸다.** lane은 재진입이 안 된다. lane 안에서 같은
   컴포넌트의 public 메서드를 다시 부르는 자리는 내부 메서드로 분리한다.
4. **public 시그니처가 sync에서 async로 바뀌는 것을 허용한다.** 호출자가 그 값을 async로
   쓰고 있었다면 어차피 스냅샷이었다.
5. **스펙이 정한 관측 가능한 동작은 바꾸지 않는다.** 순서·타임아웃·오류 코드는 그대로 둔다.

## 6. 검증 기준

전환 하나마다 아래를 모두 통과해야 다음으로 간다.

- `dotnet test tests/Zlink.Framework.UnitTests` — **1879 통과 / 0 실패** 유지
- `framework/languages/dotnet/samples/run_samples.sh` — 6샘플 placement marker 유지
- `lock` 문 수가 줄어든 것은 **증거가 아니다.** 줄여야 하는 것은 "스냅샷을 lock 밖에서
  쓰는 자리"다. 클래스별로 그 수를 전후로 센다.

## 7. 적용 순서

`_gate` 밀도와 오늘 관측된 실패 지점을 겹쳐 정한다.

| 순서 | 대상 | lock | 비고 |
|---:|---|---:|---|
| 1 | `Runtime/Actors/ZLinkSessionActorBindingTable.cs` | 30 | cpp에서 오늘 터진 결함의 .NET 대응부. C2 표본 |
| 2 | `Runtime/Spots/ZLinkSpotNodeCatalog.cs` | 48 | 레지스트리 성격이 강해 C1 비중이 클 것으로 본다 |
| 3 | `Runtime/Actors/ZLinkActorHandoffState.cs` | 63 | |
| 4 | `Runtime/Actors/ZLinkActorRuntimeState.cs` | 35 | |
| 5 | `Runtime/Service/ZLinkManagedMeshNode.cs` | 141 | 가장 크다. 앞 넷의 규칙이 확정된 뒤 |

1번을 먼저 하는 이유는 크기가 아니라 **C2의 표본이기 때문**이다. C2 전환이 실제로 어떤
비용인지(재진입 몇 곳, 시그니처 몇 개가 async로 바뀌는지)를 여기서 확정하고 나머지를 추정한다.

## 8. 하지 않는 것

- **경계 재설계.** `_gate` 61개가 이미 소유 단위다. 합치거나 쪼개지 않는다.
- **`lock` 전면 금지.** C3와 lane 내부 구현에는 남는다. 목표는 0이 아니라
  "async 경계를 넘는 스냅샷 0"이다.
- **네 트리 동시 진행.** .NET에서 규칙이 확정된 뒤 옮긴다.
