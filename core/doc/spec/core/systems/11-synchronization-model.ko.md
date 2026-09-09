---
title: "Synchronization model"
---

[English](https://zlink-systems.github.io/zlink/spec/core/systems/11-synchronization-model/) | 한국어

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [Core 스펙 목차](../README.ko.md) | [이전: Core hot path](10-hot-path.ko.md)
<!-- zlink-nav:end -->

# Synchronization model

> **이 장이 정의하는 것** — Core 구현이 따라야 하는 동기화 규칙. 어떤 상태를 함께 지켜야
> 하는지, 그 상태를 누가 언제 바꾸는지, 소유자 사이를 어떤 장치로 잇는지, 그리고 message마다
> 실행되는 경로(hot path)에 lock을 두어도 되는 조건. 코드가 이 규칙과 다르면 코드를 고친다.
> 현재 코드가 어디에서 아직 다르고 어떤 순서로 맞추는지는 이 장이 적지 않는다 — 진행
> 기록은 계획 문서가 소유한다.

## 1. Synchronization 개요

Core의 hot path는 socket을 부르는 application thread가 queue에 message를 넣고, connection을
처리하는 I/O thread가 그 queue에서 꺼내 보내거나 그 반대로 넣어 주는 일이다. inproc처럼 I/O
thread 없이 두 socket이 pipe 하나로 마주 보는 경우와, connection의 memory 상태를 읽는
context 관측자도 같은 상태를 만진다. 이 장은 그 상태를 여러 실행 주체가 함께 만질 때 누가
무엇을 소유하는지, 소유가 갈리는 지점마다 어떤 장치 — lock, single-producer queue, atomic
값 — 를 쓰는지, 그리고 그 장치를 어떻게 다루는지 정한다.

Caller가 의존하는 계약은 바뀌지 않는다. 공개 API의 동시 호출 허용 범위와 close 결과는
[Socket 공통 §2 스레드 안전성](../socket/README.ko.md#2-스레드-안전성)과 함수별 계약이 소유하고,
readiness와 대기자 진행은 [Polling](../05-polling.ko.md)이, memory 회계는
[Auto-HWM](06-auto-hwm.ko.md)이 소유한다. [Thread safety](04-thread-safety.ko.md)는 현재 구현의
설명이며, 이 장은 그 구현이 따라야 할 보호 규칙이다. 이 장은 그 계약들을 지키는 데 **정확히
어떤 배타 장치가 필요하고, 그 이상은 왜 두지 않는지**를 정한다.

socket 상태를 한 번에 한 실행 주체만 바꾸도록 부여하는 권한을 **socket turn**이라 한다. 이 장의
규칙은 모두 "같은 불변식을 지키는 상태에는 소유자가 하나"라는 원칙에서 나온다. 모델은
[framework의 state lane](../../../../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md)이
쓰는 단일 소유 원칙과 같지만, 두 문서는 같은 장치가 아니다. framework의 lane은 FIFO 대기열과
재진입 즉시 거부를 제공하는 실행 단위이고, Core의 turn은 배타권만 제공한다. 작업을 어느
thread에서 실행할지 예약하는 일은 [I/O thread](03-io-thread.ko.md)와 mailbox의 비동기 소유자가
맡으며 turn이 대신하지 않는다.

| 주체 | 이 장에서 정하는 것 |
|---|---|
| socket을 부르는 application thread | 허용된 송수신·control 연산이 socket 상태를 바꾸는 동안 socket turn을 쥔다 |
| connection을 처리하는 I/O thread | connection의 engine 상태와 pipe의 session 쪽 끝을 소유하고, socket 쪽 상태는 command와 발행된 atomic 값으로만 바꾼다 |
| context 관측자(memory snapshot, HWM 재계획) | 발행된 값을 읽고, 일관된 조회가 필요할 때만 registry lock을 잡는다 |
| Core 유지보수자 | lock을 추가·제거·범위 변경할 때 보호할 불변식과 함께 접근하는 주체를 밝히고 [§8](#8-검증-요구)의 검증을 제출한다 |

## 2. 함께 지켜야 할 조건과 상태 분류

변경 가능한 상태는 "무엇을 함께 지켜야 하는가"로 분류하고, 분류가 정해지면 지키는 장치도
정해진다. 분류의 단위는 물리적 객체가 아니라 **함께 유지해야 하는 불변식의 범위**다. 한
객체 안에 여러 범위가 있으면 범위마다 따로 분류한다.

| 함께 지켜야 할 조건 | 분류 | 적용할 장치 | Core의 예 |
|---|---|---|---|
| 하나의 표에 조회·추가·삭제만 하고, 다른 상태와 함께 지킬 조건이 없다 | **C1 — 조회 표** | 변경은 lock 하나 아래에서. hot path의 조회는 lock 없는 스냅샷으로 읽되, 스냅샷이 가리키는 객체의 수명은 참조 보유나 generation으로 보장한다 | route shard의 RID→pipe 표, public poller의 handle 표 |
| 여러 field가 함께 바뀌어야 맞거나, 결정을 내린 뒤 그 결정으로 비동기 동작을 이어간다 | **C2 — 교차 불변식** | 실행 소유자 하나가 직렬로 실행하므로 소유자 안에서는 그 field들을 잠그지 않는다 | socket이 수신 가능한 pipe의 집합과 다음 수신 대상을 정하는 상태, 보낼 pipe를 고르는 상태, multipart 진행, 지연 종료 큐의 연결 구조; pipe 송신 끝의 queue·활성 여부·generation 묶음; context socket registry의 socket 집합·slot·빈 slot 목록 |
| 다른 상태와 독립적으로 관측해도 되는 값 하나 — 정수 증가, 단조 max, 플래그 확인, 참조 하나 교체 | **C3 — 발행 값** | atomic 연산. 다른 thread가 읽는 값은 release로 발행하고 acquire로 읽는다 | pipe의 `_state`·`_in_active`, peer가 소비한 message·byte 수, auto-HWM의 planned/applied, 지연 종료 큐의 "비어 있는가" 판정용 head |

같은 불변식에 참여하는 field는 하나의 소유자가 가진다. 그 일부만 별도 lock이나 atomic으로
떼어 내면 떼어 낸 경계에서 조건이 깨진다 — atomic 타입으로 바꾸는 것만으로 C3가 되지는
않는다. 반대로 C1·C3는 C2의 소유자 아래로 끌어들일 필요가 없다. 지연 종료 큐처럼 "비어
있는가"만 atomic head로 관측하고 연결 구조 변경은 소유자 아래에서 하는 것이 그 예다.

## 3. 소유자와 접근 규칙

### 3.1 socket turn

**무엇을 소유하는가.** socket의 C2 상태 전부. turn 안에서는 그 상태를 잠그지 않는다.

**누가 언제 쥐는가.**

| 주체 | 언제 | 그래서 |
|---|---|---|
| application thread | 허용된 send·recv·request·reply·control 연산이 socket 상태를 바꾸는 동안 | 다른 application thread의 같은 종류 연산은 그 socket에서 기다린다 — 이것이 동시 호출을 허용하는 socket 계약의 실현이다 |
| command owner(application thread, 또는 비동기 실행자인 I/O thread) | 그 socket의 mailbox에서 command 묶음을 꺼내 적용하는 동안 | command 적용과 공개 연산이 같은 상태를 동시에 만지지 않는다 |

command 적용은 socket type이나 command 종류에 관계없이 turn 안에서 한다. 이미 turn을 쥔
thread가 command를 처리할 때는 다시 잡지 않고 같은 turn 안에서 처리한다. "자주 실행되니
lock 없이 둔다"는 예외는 두지 않는다.

close는 turn을 기다리는 연산이 아니다. close는 진행 중인 API가 있으면 거부하는 lifecycle
gate이며, 그 결과(`EBUSY`)와 순서는 [Socket 공통 §2](../socket/README.ko.md#2-스레드-안전성)가
소유한다. receive의 single-consumer 제약과 multipart owner 제약도 turn이 대체하지 않는다 —
그 제약은 자기 계약을 따른다.

**어떻게 쥐고 놓는가.** turn은 공개 API 진입 상태어의 한 bit다. 상태어는 진행 중 호출 수와
close 상태를 같은 word에 담으므로, 획득과 해제는 **다른 bit를 보존하는 read-modify-write**로
한다. 단순 store로 해제하면 그 사이 다른 thread가 바꾼 admission·close 상태를 덮어쓴다. 획득이
실패하면 socket의 C2 상태를 읽지 않고 진입 상태어만 다시 읽으며 짧게 물러섰다 재시도한다.

**turn 안에서 하지 않는 것.** 같은 socket의 공개 API를 다시 부르지 않고, 다른 socket의 turn을
기다리지 않으며, 대기를 시작하지 않는다. 대기가 필요하면 §3.4대로 turn을 놓는다.

내부 확인 조건: socket의 C2 상태를 읽고 쓰는 코드는 turn을 쥔 실행 주체 하나뿐이다.

**현재 구현.** turn과 공개 API 진입 상태는 하나의 상태어에 들어 있다.

| 비트 | 의미 |
|---|---|
| 63 | close gate |
| 62 | socket turn |
| 61 | 진행 중인 multipart lease |
| 32..60 | complete-record admission 수(단위 `1 << 32`) |
| 0..31 | 실행 중 public API 수 |

- 무경합 진입은 상태어 `0`을 `1 | turn`으로 바꾸는 strong CAS 하나로 admission과 turn을 함께
  얻는다(성공 `acq_rel`, 실패 `acquire`). 실패하면 admission을 먼저 얻고 turn을 따로 CAS한다.
  send admission은 acquire load 뒤 weak CAS로 admission과 multipart 표지 또는 complete-record
  수를 함께 올리며, 가능하면 같은 CAS에서 turn까지 세운다.
- 반납은 turn 비트에 대한 `fetch_and(release)`이고, admission까지 함께 놓는 경로는
  `fetch_sub(turn | 1, acq_rel)` 하나다. multipart 호출이 대기로 물러날 때는 multipart 표지만
  남기고 admission과 turn을 내린다.
- 경합 backoff는 spin 64회 → `yield`(1024회 미만) → 100 µs sleep 순이다.
- 같은 thread가 이미 그 socket의 turn을 쥐고 있으면 내부 재진입은 새 turn을 얻지 않는다. 소유
  여부는 thread-local 목록으로 판정하고, 반납은 실제로 획득한 scope만 한다.
- command drain은 batch 시작에서 turn을 얻어 mailbox dequeue, command 적용, 지연된 pipe 종료,
  readiness·submit progress 발행까지 같은 turn 안에서 끝낸다.
- close는 turn을 기다리지 않는다. 실행 중 public API 수가 0일 때 close 비트를 CAS로 세운다.
  수가 0이 아니면 public poller의 짧은 readiness sample이 끝날 기회를 주기 위해 최대 1024회
  backoff한 뒤에도 남아 있으면 `EBUSY`를 반환한다. poller 등록이 잡은 lifetime pin 자체는
  admission을 즉시 반납하므로 등록이 close를 막지 않는다.

### 3.2 pipe의 두 끝과 그 사이

socket과 session 사이에서 message를 나르는 pipe([Architecture](01-architecture.ko.md))는 양
끝의 **동시 실행 주체가 각각 하나**다. socket 쪽 끝은 그 시점에 socket turn을 쥔 주체가, session
쪽 끝은 connection의 I/O thread가 실행한다([Threading model §3](02-threading-model.ko.md#3-thread-간-통신)).
socket 쪽 끝을 실행하는 application thread는 매번 다를 수 있다 — 고정되는 것은 thread가 아니라
"한 번에 하나"라는 조건이다. inproc pipe는 session 끝 대신 상대 socket의 turn이 반대쪽 끝이다.

**queue.** 두 끝 사이의 queue는 넣는 쪽 하나와 꺼내는 쪽 하나만 있는 single-producer
single-consumer 구조다. producer·consumer 상태에 별도 mutex를 두지 않는다. 넣는 쪽은 flush에서
"꺼내는 쪽이 잠들어 있었는가"를 atomic 교환으로 판정해 잠들어 있었을 때만 `activate_read`
command를 보내고, 꺼내는 쪽은 queue가 비면 같은 교환으로 자신을 잠든 상태로 표시한다.

**끝 하나가 소유하는 값.** 그 끝의 queue 위치, 쓴 message·byte 수, 활성 여부, generation은 그
끝의 실행 주체만 바꾼다. 반대쪽 끝이나 제3의 주체(monitor, context 관측자)가 읽어야 하는 값은
C3로 발행한다 — 쓰는 쪽은 release, 읽는 쪽은 acquire. 발행하지 않은 값을 다른 주체가 읽으려면
소유자와 같은 lock 아래에서만 읽는다. **reader가 lock을 유지한다고 writer의 lock을 뺄 수 있는
것이 아니다** — writer가 lock을 빼려면 그 값을 발행하거나 reader를 없애야 한다.

**두 끝이 공유하는 값.** peer 수명 링크와 head 재분류 표시처럼 양 끝이 모두 쓰는 값은 끝 하나가
소유하는 값이 아니다. 이 값은 자기 lock 아래에서 바꾸거나 CAS로 전이를 확정하며, 그 lock은
message당 0회인 경로(pipe 분리, peer 식별, 종료)만 잡는다. hot path의 write·read·flush가 그
lock을 잡으면 안 된다.

**credit.** 보낼 수 있는 양의 계산, 후보·확정·반환의 경계, 대기자를 깨우는 조건(LWM)은
[Auto-HWM](06-auto-hwm.ko.md)과 [Socket 공통](../socket/README.ko.md)이 소유한다. 이 장이 정하는
것은 접근 규칙뿐이다: 각 끝이 자기 counter를 쓰고 상대 counter는 발행된 값으로 읽으며, 어느
끝도 상대의 counter를 잠그지 않는다.

내부 확인 조건: pipe 끝의 값마다 writer가 하나이고, 그 값을 읽는 다른 주체가 있으면 발행돼
있다.

**현재 구현.** 정상 송수신 경로에서 pipe가 잡는 mutex는 없다.

- ypipe는 reader 하나·writer 하나의 SPSC이고 두 끝이 공유하는 것은 발행된 pointer 하나다.
  writer의 flush는 그 CAS가 "reader가 잠들었다"를 관측했을 때만 `activate_read` command를
  만든다. reader는 큐가 비면 같은 pointer에 잠듦을 발행한다.
- 두 끝이 서로의 진행을 보는 누계(메시지 수·byte 수)는 C3 ledger로 발행한다. 유일한 writer는
  짝수 sequence를 홀수로 `exchange(acq_rel)`한 뒤 두 counter를 release store하고 다음 짝수를
  release store한다. reader는 sequence를 acquire load해 홀수면 재시도하고, 두 counter를 읽은 뒤
  sequence를 다시 읽어 같을 때만 그 쌍을 채택한다.
- 남은 endpoint lock은 hot path가 아니라 cold·control 구간이다: 재연결로 큐를 교체할 때,
  종료·delimiter로 lifecycle 상태를 바꿀 때, route blob 발행, HWM 적용과 credit 회수,
  transport pair hold. 두 끝이 공유하는 transport lock은 재진입하지 않는다.
- 상대 끝을 떼어낼 때는 자기 endpoint lock을 놓은 뒤 상대의 것을 잡는다. 두 끝의 lock을 동시에
  쥐지 않는다.

### 3.3 mailbox와 깨어남

thread 사이의 command 전달 채널인 mailbox는 여러 thread가 넣고 한 번에 하나의 소유자가 꺼낸다.
socket mailbox의 소유자는 공개 API를 쥔 thread와 비동기 실행자 사이에서 바뀌며, 그 전환은
[I/O thread](03-io-thread.ko.md)가 정한다.

**삽입.** 넣는 쪽이 여럿이므로 삽입점에 lock 하나가 있다. 이 lock은 command 발행과 그에 딸린
알림 상태(잠듦 판정, 대기자 등록, poller 알림)를 함께 보호한다. hot path에서 "여러 producer"를
이유로 허용되는 lock은 이것뿐이다.

**깨어남.** 소비자를 깨우는 원인은 하나가 아니다. queue가 "잠듦→깸"으로 전이할 때, 처리한
command가 public readiness를 바꿔 poller를 다시 깨워야 할 때, 그리고 command 없이 상태만
바꾼 뒤 명시적으로 신호할 때다. 규칙은 원인마다 같다 — **깨울 이유가 생긴 그 전이에서 한 번
알리고, 이미 깨어 있는 소비자에게는 알리지 않는다.** 소비자는 mailbox를 다 비운 뒤에야 잠들고,
잠들기 직전에 queue를 한 번 더 확인해 그 사이의 command를 놓치지 않는다. 같은 신호를 public
poller와 command owner가 나눠 소비할 때 누가 먼저 소비하고 누가 다시 무장(re-arm)하는지는
[Polling](../05-polling.ko.md)이 소유한다.

**꺼냄.** queue에서 꺼내는 동작 자체에는 lock이 없다. socket의 C2 상태를 바꾸는 command는
[§3.1](#31-socket-turn)의 socket turn 안에서 적용하고, connection의 engine 상태를 바꾸는 command는
그 connection의 I/O thread가 적용한다([I/O thread](03-io-thread.ko.md)). 각 mailbox의 command는
목적지 상태의 소유자에서 적용한다.

내부 확인 조건: 대기 등록과 알림 발행 사이에서 깨어남을 잃지 않는다 — 등록 뒤 잠들기 전에
queue를 재확인하고, 알림은 등록을 본 뒤 발행한다.

**현재 구현.** producer는 command 하나당 mailbox lock을 한 번 잡는다. 그 구간에서 command
기록과 flush, 관찰자 epoch·대기자 갱신, pending hint, 등록된 poller signal, Asio 예약 여부를
모두 결정한다.

- 깨움은 "비어 있고 잠든 receiver"에서 "일감 있음"으로 바뀌는 전이에서만 낸다. 이미 깨어 있는
  receiver 뒤에 붙는 command는 신호를 만들지 않는다.
- Asio executor가 설치돼 있으면 예약 플래그를 `exchange(true)`로 접어 callback 하나만 post하고,
  drain 뒤 lock 아래에서 예약을 내리며 큐를 다시 확인해 남은 일감이 있으면 예약을 유지한다.
  공개 FD 사용자가 등록돼 있으면 primary signaler에도 신호하고, 없으면 post만으로 깨운다.
- signaler는 신호 상태를 `exchange(true)`로 접어 이미 신호된 상태면 syscall을 생략한다.
- 대기 등록과 command 도착의 경합은 lock 아래의 command epoch·pending hint·대기자 수와 조건
  변수로 닫는다. 관찰자도 대기자도 없는 평범한 전송은 그 비용을 내지 않는다.

### 3.4 대기와 재획득

공개 연산이 admission이나 message를 기다려야 할 때의 순서는 다음과 같다.

```text
application thread                          command owner / I/O thread
  turn 획득 → 조건 검사 → 실패
  대기자 등록 (turn 안)
  turn 해제
  signaler·CV 대기  ◄──── 알림 ─────────  message·credit 전달 → 대기자 있음 → 알림
  turn 재획득 → 조건 재검사 → 진행 또는 다시 대기
```

- 대기 중에는 turn을 쥐지 않는다. 대기를 끝내 줄 주체가 같은 turn을 필요로 하면 대기가 끝나지
  않는다.
- condition variable 대기는 진입 시 mutex를 쥐고 대기 중 놓았다가 복귀 때 다시 쥔다. 이 형태는
  허용된다. 금지되는 것은 대기 중에도 놓지 않는 것이다.
- timeout이 있는 대기는 만료 시 재획득 뒤 조건을 한 번 더 검사하고 결과를 정한다.

내부 확인 조건: 대기자 등록은 turn 안에서, 대기는 turn 밖에서 일어난다.

### 3.5 context 수준 상태

물리 queue registry, socket registry, auto-HWM plan은 context 수준 상태다. 정상 message
경로에서 context 전체 mutex를 잡지 않는다는 비용 규칙은 [Auto-HWM](06-auto-hwm.ko.md)이
소유한다. 이 장이 정하는 것은 다음이다.

- hot path의 회계는 connection에 캐시된 handle과 발행된 값으로 한다.
- 등록·해제와 일관된 조회(memory snapshot, 재계획)는 registry lock 아래에서 한다. 조회가 pipe
  끝의 값을 읽을 때는 §3.2의 규칙을 따른다 — 발행된 값을 읽거나, 소유자와 같은 lock을 잡는다.
- 지연된 HWM 적용은 계획값과 적용값이 같으면 lock을 잡지 않고 끝낸다.

## 4. Lock을 두어도 되는 조건

hot path에 lock을 두려면 다음 질문에 답할 수 있어야 하고, 답이 "없다"면 그 lock은 없앤다.

> **이 lock이 없으면 같은 상태를 동시에 쓰거나 읽는 두 번째 실행 주체가 이 경로에 실제로
> 있는가? 있다면 누구이고, 무엇을 함께 지켜야 하는가?**

| 동시에 접근하는 주체와 조건 | 함께 지켜야 할 상태 | 적용할 장치 |
|---|---|---|
| 다른 application thread가 같은 socket의 C2 상태를 바꾼다 | socket 상태 전부 | socket turn. 별도 lock을 더 두지 않는다 |
| 반대쪽 pipe 끝이 같은 값을 쓴다 | 두 끝이 공유하는 수명·표시 값 | 그 값의 lock 또는 CAS 전이. cold 경로만 잡는다 |
| 반대쪽 pipe 끝·monitor·context 관측자가 값을 **읽기만** 한다 | 그 값 하나 | C3 발행(release/acquire). lock 없음 |
| 여러 thread가 같은 채널에 넣는다 | 삽입 순서와 알림 상태 | 삽입점 lock 하나 |
| 읽는 주체만 둘 이상이고 쓰는 주체가 없다 | 없음 | 장치 없음 |

hot path에 새 lock을 들이는 변경은 이 질문의 답과 [§8](#8-검증-요구)의 검증을 함께 낸다.

## 5. 위반 형태와 따를 규칙

| 위반 조건 | 발생하는 문제 | 따를 규칙 |
|---|---|---|
| 지키는 조건이 없는 lock이 hot path에 있다 | 경합이 없어도 획득·해제 명령과 상태어 갱신 비용이 message마다 든다 | [§4](#4-lock을-두어도-되는-조건)의 질문에 답이 없으면 없앤다 |
| 같은 불변식에 참여하는 상태를 여러 lock으로 나눴다 | 나눈 경계에서 조건이 깨진다 | [§2](#2-함께-지켜야-할-조건과-상태-분류): 불변식 하나에 소유자 하나 |
| socket type이나 빈도를 이유로 command를 turn 밖에서 적용한다 | 공개 연산과 command가 같은 상태를 동시에 만진다 | [§3.1](#31-socket-turn) |
| reader만 lock을 유지한 채 writer의 lock을 뺐다 | reader와 writer가 동기화되지 않는다 | [§3.2](#32-pipe의-두-끝과-그-사이): 값을 발행하거나 reader를 없앤다 |
| 대기를 끝내 줄 주체가 필요로 하는 turn·lock을 쥔 채 기다린다 | 대기가 끝나지 않는다 | [§3.4](#34-대기와-재획득) |
| 재진입하는 lock을 condition variable과 함께 쓴다 | 재진입 횟수가 둘 이상이면 대기가 mutex를 실제로 놓지 않아 진행이 멈춘다 | [§6](#6-lock의-종류순서memory-ordering): CV에는 재진입하지 않는 lock만 |
| message당 0회인 경로 때문에 hot path가 lock을 잡는다 | cold 경로가 message마다 비용을 강제한다 | [§3.2](#32-pipe의-두-끝과-그-사이): cold 경로 쪽만 잡는다 |

lock을 semaphore나 `try_lock` 재시도로 바꾸는 것은 형태를 바꾸는 것이 아니다. 같은 자리에 같은
이유로 남아 있으면 같은 규칙에 걸린다. socket turn 자체의 짧은 재시도는 이 규칙의 대상이
아니다 — 그것은 소유자 하나를 정하는 장치이지 상태를 지키는 별도 lock이 아니다.

## 6. Lock의 종류·순서·memory ordering

- **종류.** 기본은 재진입하지 않는 `mutex_t`다. 같은 thread가 같은 lock을 다시 잡아야 하는
  자리만 `recursive_mutex_t`를 쓰고, 그 재진입 경로를 선언부 주석에 적는다. condition variable과
  함께 쓰는 lock은 `mutex_t`뿐이다. POSIX backend의 debug·sanitizer 빌드는 `mutex_t`를 재진입을
  즉시 잡는 모드(`ERRORCHECK`)로 만든다. 다른 backend는 이 검출을 제공하지 않으므로 재진입
  금지는 리뷰와 §8의 검증으로 지킨다.
- **순서.** 설계 규칙으로서 lock은 socket turn → 그 socket이 소유한 pipe 끝 → context registry
  순서로만 잡는다. 역순으로 잡지 않고, 한 socket의 turn을 쥔 채 다른 socket의 turn이나 pipe 끝을
  잡지 않는다. 상대 socket에 영향을 주는 변경은 command로 보낸다. registry가 pipe 끝의 값을
  조회할 때는 registry lock을 놓고 조회하거나 발행된 값만 읽는다.
- **memory ordering.** 다른 thread에 값을 발행할 때는 release store, 읽을 때는 acquire load를
  쓴다. 대기자 등록과 알림 발행처럼 두 갱신 중 어느 하나도 놓치면 안 되는 자리에는 양쪽에
  `seq_cst` fence를 둔다. relaxed는 통계처럼 순서가 의미 없는 값에만 쓴다.

### 6.1 구현 lock 목록과 획득 순서

| 구간 | 순서 |
|---|---|
| command drain | socket turn → mailbox lock(짧은 snapshot) → 대기자가 있으면 submit progress lock |
| attach 최초 게시 | socket turn → monitor lock |
| Auto-HWM 전체 재계산 | 재계산 lock → 상태 lock / socket pin lock(pin 뒤 즉시 반납) / option lock → socket Auto-HWM lock → monitor lock → pipe endpoint lock |
| Auto-HWM 증분 확장 | 재계산 lock → 상태 lock → option lock → registry lock → (registry 반납) pipe endpoint lock → socket Auto-HWM lock → 상태 lock |
| pipe 상대 끝 분리 | 자기 endpoint lock 반납 → 상대 endpoint lock |

메시지당 hot path 비용은 다음과 같다. 정상 public send는 pipe mutex 0회, socket 상태어 CAS
1회(획득)와 atomic 1회(반납)를 쓴다. 정상 public receive도 pipe와 fair queue에서 mutex 0회이며,
multipart physical record는 record 전체가 같은 turn을 유지한다. 메시지 경계마다 C3 ledger writer가
sequence exchange 1회와 release store 3회를 낸다. mailbox producer는 command당 lock 1회이고
실제 신호는 위의 전이에서만 낸다.

## 7. 변경 절차

- hot path에 상태를 더하는 서브시스템은 먼저 [§2](#2-함께-지켜야-할-조건과-상태-분류)로
  분류한다. C2면 socket turn 아래에, 다른 주체가 읽어야 하는 값이면 C3 발행으로 설계한다.
  "일단 lock으로 두고 나중에 최적화"는 하지 않는다.
- lock을 추가·제거·범위 변경하는 변경은 보호할 불변식, 함께 접근하는 실행 주체, 적용한 장치를
  적고 [Core hot path §6](10-hot-path.ko.md#6-변경-절차)의 절차에 [§8](#8-검증-요구)의 검증을 더해
  제출한다.
- 이 장만으로 caller가 관측하는 동작을 바꿀 수 없다. 관측 동작을 바꾸려면
  [Socket 공통 §2](../socket/README.ko.md#2-스레드-안전성), [Polling](../05-polling.ko.md),
  [Auto-HWM](06-auto-hwm.ko.md)의 계약 절차를 먼저 따른다.

## 8. 검증 요구

lock을 추가·제거·범위 변경한 변경은 다음 관찰 결과를 지킨다. 각 규칙의 내부 확인 조건은 그
규칙 옆에 있으며 여기서 반복하지 않는다.

1. **readiness와 대기자 진행** — [Polling](../05-polling.ko.md)이 정한 level 결과와 lost-wake
   방지가 그대로다. wake 경로의 회귀 테스트(`test_wake_invariants` 계열)를 반복 실행해 실패가
   없어야 하며, 반복 횟수와 목록은 [Core hot path §5](10-hot-path.ko.md#5-성능-gate)의 gate 절차가
   정한다.
2. **회계 값** — [Auto-HWM](06-auto-hwm.ko.md)이 정의한 charge의 값과 반환 경계가 그대로다. 관측
   시점을 바꾸는 변경은 그 문서의 계약 절차를 먼저 따른다.
3. **data race** — 변경 전후 TSan 경고 집합의 차이가 없다. 새 경고를 성능을 이유로 넘기지 않는다.
4. **명령 수** — [Core hot path §5](10-hot-path.ko.md#5-성능-gate)의 `hotpath_gate` 셀이 기준 안에
   있고, lock별 획득 횟수 표가 제출돼 있다.

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [Core 스펙 목차](../README.ko.md) | [이전: Core hot path](10-hot-path.ko.md)
<!-- zlink-nav:end -->
