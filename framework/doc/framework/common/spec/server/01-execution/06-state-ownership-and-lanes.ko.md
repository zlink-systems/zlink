---
title: "상태 소유와 state lane"
---

# 상태 소유와 state lane

[Execution 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 05. Payload 소유권과 codec](05-payload-ownership-and-codec.ko.md)

> 이 문서는 컴포넌트가 자신의 mutable 상태를 어떤 메커니즘으로 지키는지를 정의한다 —
> 한 번에 하나의 turn만 그 상태를 만지도록 보장하는 primitive의 계약이다. 모든 언어
> runtime이 이 계약을 따라야 한다. 순서·타임아웃·오류 코드 같은 관측 가능한 동작은
> 다른 문서가 소유하며, 이 문서가 정의하는 규칙은 그 관측 가능한 동작을 바꾸지 않는다.

## 1. 상태 소유 개요

컴포넌트는 자신이 소유한 mutable 상태(field·collection)를 가진다. 이 문서는 그 상태를
읽고 쓰는 코드가 동시에 하나만 실행되도록 보장하는 메커니즘의 규칙을 정의한다 —
어떤 상태가 어느 primitive로 지켜지는지 판별하는 기준, 그 primitive가 제공해야 하는
보장, 전환할 때 지켜야 하는 규칙이다.

이 문서가 정의하지 않는 것 — Handler가 언제 실행되고 언제 다른 handler에게 자리를
내주는지는 [Handler turn과 execution gate](02-handler-turn-and-execution-gate.ko.md)가,
message가 socket에서 handler에 이르는 동안의 소유권과 복사는
[Payload ownership과 codec](05-payload-ownership-and-codec.ko.md)이 소유한다. 이 문서의
규칙은 컴포넌트 내부 상태를 보호하는 메커니즘에 관한 것이지, application이 관측하는
순서·타임아웃·오류 코드를 바꾸는 규칙이 아니다.

## 2. 용어 구분 — state lane과 Application/lifecycle lane

이 문서의 **state lane**은 컴포넌트 하나가 자신의 mutable 상태를 소유하는 실행 단위다.
그 컴포넌트의 상태를 읽고 쓰는 모든 코드는 이 lane 위에서만 실행된다.

이 용어는 [Handler turn과 execution gate 「7. Lane 분리와 우선순위
(구현)」](02-handler-turn-and-execution-gate.ko.md#7-lane-분리와-우선순위-구현)이 쓰는
"application lane"·"lifecycle lane"과 다른 개념이다. 두 문서가 같은 단어 "lane"을 쓰지만
가리키는 대상이 다르므로 반드시 구분해야 한다. Application lane·lifecycle lane은
[owner](../00-foundation/02-glossary.ko.md#owner) — 주소와 상태를 가진 논리 instance인
[Spot](../00-foundation/02-glossary.ko.md#spot)이나 그 안의 Actor를 현재 실행하는 쪽 —
하나마다 한 쌍씩 있고, state lane은 상태를 소유하는 컴포넌트마다 하나씩 있다.

| | state lane (이 문서) | application lane / lifecycle lane (02 §7) |
|---|---|---|
| 단위 | 컴포넌트 하나(예: 하나의 binding table, 하나의 catalog) | Owner(Spot·Actor) 하나 |
| 목적 | 그 컴포넌트의 상태를 한 번에 하나의 turn만 만지게 한다 | Owner가 처리할 작업을 우선순위별로 줄 세운다 |
| 담는 것 | 그 컴포넌트의 상태를 읽고 쓰는 코드 조각 | 업무 payload·timer callback(application) 또는 join·leave·relocation·lifecycle control(lifecycle) |
| admission·우선순위 | 없다 — 들어온 순서대로(FIFO) 실행할 뿐 건수·byte 한도나 lane 간 우선순위가 없다 | 있다 — 건수·byte 한도, owner 점유 시간 예산, lifecycle 우선순위 규칙 |
| 소유 문서 | 이 문서 | [02 §7](02-handler-turn-and-execution-gate.ko.md#7-lane-분리와-우선순위-구현) |

컴포넌트 하나가 state lane을 가지는 것과, 그 컴포넌트가 속한 Spot·Actor의 handler가
application lane·lifecycle lane 중 어디로 admission되는지는 서로 다른 층의 결정이다. 한
handler 실행이 application lane에서 순서를 기다리는 동안, 그 handler가 만지는 상태는 또
다른 컴포넌트의 state lane 위에서 직렬화될 수 있다.

## 3. 금지되는 형태

state lane이 막는 형태는 다음과 같다 — **lock 같은 배타적 접근 안에서 상태의 스냅샷을
꺼내고, 그 접근을 해제한 뒤, async 경계 너머에서 그 스냅샷으로 결정을 내리는 형태.**

```csharp
Entry entry;
lock (_gate) { if (!_entries.TryGetValue(key, out entry)) return; }   // 여기서 해제
await SendAsync(entry.Route);                                         // 낡았을 수 있는 값으로 실행
```

이 형태는 실수가 아니라 **메커니즘이 강제하는 결과**다. Lock은 `await`을 감쌀 수 없다.
그래서 lock으로 상태를 지키면서 그 상태를 바탕으로 비동기 작업을 해야 하는 코드는
예외 없이 "스냅샷을 꺼내고 lock을 놓은 뒤 그 값으로 행동한다"는 모양이 된다. Lock을
다시 잡을 수 있는 지점(비동기 작업 완료 이후)에 도달했을 때, 그 사이 다른 turn이 같은
상태를 이미 바꿨을 수 있으므로 스냅샷은 구조적으로 낡는다.

state lane은 이 형태 자체를 없앤다. Lane의 turn 안에는 해제 지점이 없으므로 — 상태를
읽는 코드와 그 상태에 근거해 행동하는 코드가 같은 turn 안에 있으면 — 스냅샷이 생기지
않는다.

## 4. 상태 분류와 판별 기준

컴포넌트의 mutable 상태는 다음 분류 중 하나로 분류한다. 분류가 정해지면 그 상태를
지키는 메커니즘은 기계적으로 정해진다.

| 분류 | 판별 기준 | 메커니즘 |
|---|---|---|
| **C1 — 순수 조회 레지스트리** | 단일 map이고, 연산이 조회·추가·삭제로 끝나며, 다른 field·collection과 걸친 불변식이 없다. 배타적 접근 블록을 두 개의 원자 연산으로 쪼개도 불변식이 깨지지 않는다 | concurrent map (언어별 thread-safe map 구현) |
| **C2 — 교차 불변식 상태** | 여러 collection·field에 걸친 불변식이 있거나, 하나의 결정을 내린 뒤 그 결정에 따라 비동기 행동을 이어간다 | state lane 소유 + 평범한 map(잠그지 않는다) |
| **C3 — 원자 카운터·플래그** | 정수 증가, 플래그 확인, 단일 참조 교체만 한다 | atomic 연산(언어별 원자 증가·비교교환·volatile 참조) |

한 컴포넌트 안에 세 종류가 섞여 있으면 **C2가 이긴다** — 가장 강한 요구가 그
컴포넌트 전체의 메커니즘을 결정한다. 컴포넌트를 부분적으로만 lane으로 옮기고 나머지를
concurrent map이나 별도 lock으로 남기면, C2가 막으려는 교차 불변식 위반이 그 경계에서
다시 나타난다.

**C2를 concurrent map으로 치환하지 않는다.** Concurrent map은 map 하나에 대한 개별
연산만 원자적으로 만든다. 여러 collection·field에 걸친 불변식은 map 단위의 원자성으로
지켜지지 않는다 — 원자성이 그 map 하나로 쪼개지면서 불변식 자체가 깨진다.

**C2를 다른 배타적 접근 primitive(semaphore 등)로 치환하지 않는다.** 배타적 접근을
제공하는 primitive를 lock에서 semaphore로 바꿔도 [§3](#3-금지되는-형태)이 막는 형태는
그대로 남는다 — 얻는 것은 숫자(잠금 개수)가 줄었다는 것뿐이고, "배타적 접근 해제 뒤
스냅샷으로 행동한다"는 구조는 바뀌지 않는다.

### 상태 보호와 작업 프로토콜 직렬화를 구분한다

외부 비동기 작업 전체를 하나씩 실행하기 위한 semaphore·socket gate·dispose gate는
컴포넌트의 mutable 상태를 보호하는 state lane과 책임이 다르다. 다음 조건을 모두
만족할 때만 그 gate를 작업 프로토콜 primitive로 유지할 수 있다.

- gate가 보호하는 것이 외부 resource operation의 시작·종료 또는 exact-once
  disposal이며, C2 상태의 일부를 별도로 소유하지 않는다.
- operation을 시작하기 전에 필요한 generation·identity·ownership을 gate 안에서
  확정한다.
- gate를 놓은 뒤에는 mutable 상태 스냅샷이 아니라 Task, reservation, seal token 또는
  단독 ownership을 이전받은 resource를 사용한다.
- 완료 경로는 같은 ownership을 정확히 한 번 반납한다.

작업 프로토콜 gate 안에서 state lane의 완료를 기다리는 것은 허용한다. 그러나 반대
방향 — state lane의 turn 안에서 그 gate의 획득이나 장기 operation의 완료를 기다리는
것 — 은 금지한다. 두 방향을 모두 허용하면 서로가 서로의 완료를 기다리는 역방향
deadlock이 생긴다.

이 예외는 C2 상태를 여러 lock으로 나누는 허가가 아니다. 같은 불변식에 참여하는
field와 collection은 여전히 하나의 state lane이 소유한다.

## 5. state lane의 보장과 제약

state lane은 다음을 보장한다.

- **한 번에 한 turn만 실행한다.** 같은 lane 위의 두 turn이 동시에 실행되지 않는다.
- **FIFO다.** Lane에 들어온 순서대로 실행한다. 앞쪽 삽입은 없다.
- **비재진입이다.** 이미 그 lane의 turn 위에서 실행 중인 코드가 같은 lane에 다시
  진입하려 하면 거부한다.
- **소유한 collection은 잠그지 않는다.** Lane이 소유한 상태를 담는 collection은 평범한
  구조로 둔다 — lock이 아니라 lane의 단일 실행이 배타성을 보장하므로 collection 자체가
  thread-safe일 필요가 없다.

**재진입은 데드락이 아니라 예외로 검출되어야 한다.** 왜 재진입이 처리할 방법이 없는
상태인지는 구체적인 순서로 보면 드러난다.

1. 같은 컴포넌트의 public 메서드 A가 이미 lane의 turn 위에서 실행 중이다.
2. A의 본문이 같은 컴포넌트의 public 메서드 B를 호출한다.
3. B도 상태에 접근하려고 같은 lane에 들어가야 하므로, "지금 이 lane에서 실행 중인
   turn이 끝나기"를 기다린다.
4. 그런데 지금 이 lane에서 실행 중인 turn이 바로 B를 부른 A 자신이다.

B는 A가 끝나기를 기다리고, A는 B가 끝나야 자신의 turn을 마칠 수 있다 — 결국 A가 A
자신이 끝나기를 기다리는 것과 같다. 이 대기는 다른 turn이 끼어들어도 풀리지 않으므로
영원히 끝나지 않는다.

이 상태를 그대로 두면(hang) 서버는 조용히 멈춘다. 어느 호출이 원인인지 로그나 스택
트레이스에 남지 않으므로, 원인을 찾으려면 그 순간의 실행 덤프를 통째로 뒤져야 한다.
Lane은 이 상황을 hang이 아니라 재진입이 일어난 바로 그 호출 지점에서 예외로 끝내야
한다 — 어느 lane에 재진입했는지 알려주는 오류가 그 자리에서 발생하면, 스택 트레이스가
원인 코드를 곧바로 가리킨다.

### 완료 신호와 블로킹 호환 경계

Lane work의 완료 신호는 lane의 현재 소유권 표시가 해제된 뒤에 caller continuation을
실행해야 한다. 완료 API가 dependent continuation을 완료 thread에서 inline 실행할 수
있는 언어에서는, 완료 신호를 lane-current scope 안에서 직접 완료하지 않는다. 완료를
scope 밖 scheduler에 게시하거나 continuation을 비동기로 강제한다.

기존 동기 표면이 state lane의 완료를 블로킹 대기하는 호환 경계는 다음 조건을 모두
만족할 때만 허용한다.

- 제출한 lane 항목이 현재 보유 중인 외부 gate를 다시 획득하지 않는다.
- lane 항목의 모든 완료 신호는 continuation을 비동기로 실행한다.
- 그 동기 표면이 반환되기 전에 상태 등록·캡처가 완료돼야 하는 계약이 있거나, 공개 동기
  signature를 이 전환에서 바꿀 수 없다는 사유가 기록돼 있다.

이 세 조건 중 하나라도 확인할 수 없으면 gate를 보유한 채 lane 완료를 기다리지 않는다.
호출 경로를 비동기로 전파하거나, gate와 lane의 책임을 다시 분리한다.

## 6. 재진입 제거 규칙

C2로 분류한 컴포넌트를 lane으로 전환할 때 재진입을 먼저 걷어낸다. 실제 전환에서
나타나는 재진입 유형은 다음과 같다.

**유형 ① — lane 안에서 같은 컴포넌트의 public 표면을 다시 부르는 자리.** 한 public
진입점이 lane에 들어간 turn 안에서 같은 컴포넌트의 다른 public 메서드를 호출하면, 그
메서드도 같은 lane에 들어가려 하므로 재진입이 된다. 이런 자리는 lane에 들어가지 않는
private 메서드로 분리한다. Public 진입점은 lane에 한 번만 들어가고, lane 안에서는 그
private 메서드의 본문을 직접 호출한다.

**유형 ② — lane turn 안에서 시작한 장기 비동기 작업이 lane 소유권을 상속하는 경우.**
Turn 안에서 timeout·retry·background loop 같은 작업을 시작하면, 그 작업이 실행되는
문맥이 "지금 이 lane 위에서 실행 중"이라는 표시를 그대로 물려받을 수 있다. 지연이 끝난
뒤 그 작업이 같은 lane에 다시 들어가려 하면, 실제로는 원래 turn이 이미 끝난 뒤인데도
재진입으로 검출된다.

이런 장기 작업을 시작하는 자리에서는 실행 문맥의 흐름을 끊는다. 다만 문맥 흐름을
끊는 조치는 그 비동기 작업이 실제로 thread 전환을 거친 뒤에만 효력이 있다. 호출한
async 함수의 동기 prefix가 첫 `await` 전에 같은 lane에 재진입할 수 있다면, 문맥 흐름을
끊는 것만으로는 충분하지 않다 — 이 경우 작업 시작 자체를 별도 scheduler에 게시해 동기
prefix까지 원래 turn 밖에서 실행되게 한다. 반대로 첫 동작이 실제 비동기 지연이고 그
전에 lane 재진입이 없다면, 문맥 흐름을 끊는 것만으로 충분하다.

**유형 ③ — 기존 배타적 접근 안에서 외부 callback을 호출하던 경우.** Monitor 재진입으로
동작하던 callback을 lane turn 안에서 직접 호출하면, 그 callback이 같은 컴포넌트의
public 표면에 재진입한다. 다음 세 단계로 나눈다.

1. turn A에서 검증, 결과 산출, 원본이 callback 전에 끝내던 상태 전이 전부와 placeholder
   ownership claim을 완료한다.
2. callback은 lane turn 밖에서 호출한다.
3. turn B에서는 그 placeholder를 callback 결과 Task로 정확히 교체하거나 실패를 정산한다.

원본이 callback 전에 끝내던 상태 전이를 turn B로 미루지 않는다. Callback 실행 창의
경쟁 관측자는 원본의 "배타적 접근이 끝난 뒤"와 같은 상태를 봐야 한다.

## 7. 시그니처 전환 규칙

상태 접근 메서드의 시그니처는 다음 규칙에 따라 전환한다.

- **동기 반환을 비동기 반환으로 바꾸는 것을 허용한다.** 호출자가 그 값을 이미 비동기
  경로 안에서 쓰고 있었다면 그 값은 애초에 스냅샷이었다 — 반환 방식을 비동기로 맞추는
  것은 기존 관측 가능한 동작을 바꾸지 않는다.
- **out 파라미터는 반환값으로 합친다.** 하나의 반환값에 성공 여부와 결과를 함께
  담는다.
- **실패 시에도 값을 돌려주던 out은 nullable scalar로 기계 치환하지 않는다.** 성공
  여부와 값을 하나의 스칼라로 뭉치면, "실패했지만 그 실패에 딸린 값도 함께 필요한"
  경우를 표현할 수 없다 — 예를 들어 거절되었더라도 그 시점의 현재 high-water 값을
  호출자가 여전히 받아야 하는 경우, nullable scalar 하나로는 성공 값과 실패 시 부가
  값을 동시에 담지 못한다. 이런 경우는 성공 여부와 값(그리고 실패 시 부가 값)을 함께
  담는 결과 타입을 만들어 보존한다. 기계적인 nullable 치환은 관측 가능한 동작을
  바꾸므로 허용하지 않는다.
- **반환 전 완료 보장을 보존한다.** 원본 동기 메서드가 waiter 등록, epoch·generation
  캡처, store 판독 또는 exact ownership claim을 반환 전에 완료했다면, 전환 뒤에도
  caller가 반환을 관찰하기 전에 그 작업이 완료돼 있어야 한다. 비동기 fire-and-forget
  게시로 바꾸지 않는다.
- 이 보장을 유지하기 위해 동기 호환 경계가 필요하면 [§5 「완료 신호와 블로킹 호환
  경계」](#완료-신호와-블로킹-호환-경계) 조건을 확인하고 사유를 기록한다. 완료 신호를
  기다리는 이후 단계는 비동기로 남길 수 있지만, 등록·캡처 자체를 반환 뒤로 미루지는
  않는다.
- 공개 또는 언어별 exact interface가 동기 계약이면, state lane 도입만을 이유로
  Promise나 Task 반환으로 바꾸지 않는다. 내부 호출자가 이미 비동기이고 관측 계약이
  변하지 않을 때만 async signature를 전파한다.

## 8. 전환 단위와 검증

- **전환 경계는 기존 gate가 소유하던 상태 영역을 그대로 쓴다.** 클래스 하나에 서로
  독립적인 gate가 여러 개 있었다면, 각각을 별도 ownership region으로 옮길 수 있다. 이
  경우 두 영역에 걸친 field·collection 불변식이 없고, 영역 사이의 호출 방향이
  단방향임을 기록한다.
- 교차 불변식이나 양방향 대기가 하나라도 있으면 여러 lane으로 나누지 않고 한
  ownership region으로 합친다. "클래스 하나"는 기본 작업 단위일 뿐, 한 클래스 안에
  근거 없이 여러 state lane을 만드는 허가가 아니다.
- socket·completion·worker 같은 작업 프로토콜 gate는 [§4 「상태 보호와 작업 프로토콜
  직렬화를 구분한다」](#상태-보호와-작업-프로토콜-직렬화를-구분한다) 조건을 만족할
  때만 state lane 전환 대상에서 제외한다. 제외 사유에는 ownership transfer,
  generation fence, completion 방식과 lock-order를 기록한다.
- **전환마다 검증을 통과해야 다음으로 간다.** 확인할 항목은
  [검증 요구](#10-검증-요구)가 소유한다.
- **성공 지표는 lock 개수가 아니다.** 배타적 접근 문의 개수가 줄어든 것은 증거가
  아니다. 줄여야 하는 것은 "async 경계를 넘어 쓰이는 스냅샷"의 수이며, 이 수를 컴포넌트
  단위로 전후 비교한다. 이 비교는 공개 표면이 아니라 내부 계측으로 확인하는 **내부
  확인 조건**이며, 검증 요구 절에는 두지 않는다.

Async 경계 snapshot의 계수 단위는 source의 배타적 접근 위치다. 단순 문자열 검색
결과가 아니라 실제 언어 token을 센다. 각 위치에서 배타적 접근 안에서 산출한 값·참조·
결정이 다음 중 하나를 넘어 쓰이는지 추적한다.

- `await` 또는 Task·Promise·future 반환
- detached task, queue, worker thread 또는 callback dispatcher 제출
- 비동기 continuation을 실행하는 completion signal
- nonblocking transport operation 제출

그 경계를 넘더라도 immutable completion signal, exact token, reservation 또는 단독
ownership transfer로 유효성이 고정되면, primitive/protocol 제외군으로 따로 센다.
Mutable authorization을 그대로 넘겨 쓰면 잔존 결함이다. 최종 보고는 `전체 / 제외군 /
잔존 결함` 세 값을 모두 적는다.

## 9. 언어별 매핑

.NET에서는 `Zlink.Framework.Runtime.Execution.ZLinkStateLane`이 이 문서가 정의하는
규칙의 기준 구현이다. 상태 접근은 이 lane에 제출한 작업으로 실행하며, lane이
소유한 collection은 평범한 `Dictionary`로 둔다. 재진입은 hang이 아니라 그 호출 지점에서
즉시 `InvalidOperationException`으로 검출된다.

`ZLinkStateLane`은 Spot·Actor **실행**에 쓰는 `ZLinkSerialExecutionQueue`와 별개다.
`ZLinkSerialExecutionQueue`에는 relocation seal과 lifecycle admission이 함께 있는데, 이는
상태 소유 목적에는 필요 없는 책임이다 — 상태를 지키려는 컴포넌트가 이 실행용 queue를
가져다 쓰면 그 queue가 지고 있는 relocation·lifecycle 책임까지 함께 떠안게 된다. 그래서
상태 소유에는 이 실행용 queue를 쓰지 않는다.

다른 언어로 포팅할 때는 구체적인 자료형이나 API 모양을 그대로 옮기는 대신, 이 문서가
정의하는 같은 보장 — 한 번에 한 turn만 실행, FIFO, 재진입의 즉시 예외 검출, 소유
collection의 무잠금 — 을 만족하는 그 언어의 primitive를 쓴다.

.NET은 lane 소유권 표시에 `AsyncLocal`을 쓰므로, 장기 작업을 시작하는 지점에서는
`ExecutionContext.SuppressFlow`로 그 상속을 끊는다. Async 함수의 동기 prefix가 첫
`await` 전에 lane에 재진입할 수 있으면, `Task.Run` 등으로 시작 자체를 별도
scheduler에 게시한다.

Java는 lane-current `ThreadLocal` scope 안에서 `CompletableFuture.complete`를 호출하지
않는다. `complete`는 비동기 표시가 없는 dependent를 완료 thread에서 inline 실행할 수
있다. Current scope를 해제한 뒤 완료하거나, `completeAsync`처럼 별도 scheduler에서
완료하는 API를 쓴다.

Node.js의 동기 메서드는 하나의 JavaScript turn 안에서 끝나는 동안 다른 callback과
동시에 실행되지 않는다. 따라서 `await` 전후로 상태 접근이 갈라지지 않고 public
재진입 검출도 필요 없는 동기 표면은 Promise로 바꾸지 않는다. State lane 전환 대상은
`await` 전후로 상태가 갈라지는 비동기 경로와, 재진입 검출이 필요한 표면뿐이다.

## 10. 검증 요구

공개 표면(state lane에 제출한 작업의 반환값과 예외, 재진입 호출이 받는 오류, 전환한
언어의 단위 테스트와 샘플 게이트 결과)만으로 다음을 확인한다. 각 항목은 test 하나로
이어진다.

**재진입과 실행 순서**

- 이미 그 lane의 turn 위에서 실행 중인 코드가 같은 lane에 다시 진입을 시도하면, hang이
  아니라 그 호출 지점에서 즉시 예외로 끝난다.
- 서로 다른 호출자가 동시에 같은 lane에 작업을 제출해도, lane이 소유한 잠금 없는
  평범한 collection에 대한 갱신이 유실되지 않는다.
- 같은 lane에 제출한 작업은 제출한 순서대로 실행된다.
- 닫힌 lane에 결과를 기다리는 호출을 제출하면 즉시 예외로 끝나고, 결과를 기다리지
  않는 제출은 실패를 반환한다.
- Lane turn에서 시작한 장기 작업은 원래 turn이 끝난 뒤 같은 lane에 정상 진입하며,
  거짓 재진입 예외를 만들지 않는다.
- 동기 prefix가 있는 장기 작업도 원래 turn 밖에서 시작됨을 확인한다.
- 완료 continuation은 lane-current scope 밖에서 실행된다.
- 외부 callback은 turn 밖에서 실행되며, callback이 관찰하는 상태는 원본 배타적 접근이
  끝난 시점과 같다.
- 작업 프로토콜 gate를 보유한 채 lane 완료를 기다리는 허용 사례는, lane 항목이 그
  gate를 재획득하지 않으며 모든 completion continuation이 비동기임을 확인한다.
- 반환 전 등록·캡처 계약이 있는 메서드는, caller가 반환을 관찰하는 시점에 그 등록·
  캡처가 이미 완료돼 있다.
- async 경계 snapshot 재측정에서 잔존 결함이 0이고, primitive/protocol 제외 위치에는
  각각 유효성 보존 근거가 기록돼 있다.

**전환 검증**

- 전환 전후 그 언어의 단위 테스트 전체가 그린이다.
- 전환 전후 샘플 게이트가 유지된다.
- 전환 전후 caller가 관측하는 순서·타임아웃·오류 코드가 바뀌지 않는다.

---

[Execution 주제 목차](README.ko.md) · [스펙 목차](../README.ko.md) · [이전: 05. Payload 소유권과 codec](05-payload-ownership-and-codec.ko.md)
