---
title: "Synchronization model"
---

[English](https://zlink-systems.github.io/zlink/spec/core/systems/11-synchronization-model/) | 한국어

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [이전: Core hot path](10-hot-path.ko.md)
<!-- zlink-nav:end -->

# Synchronization model

> **이 장이 정의하는 것** — Core가 변경 가능한 상태를 어떤 단위로 소유하고, 소유 단위 사이를
> 어떤 장치로 잇는지, 그리고 message마다 실행되는 경로(hot path)에 lock을 두어도 되는 조건.
> 이 장의 문장은 구현 서술이다 — 코드가 사실이고 코드가 바뀌면 이 장을 고친다. 단
> [§4](#4-lock을-두어도-되는-조건)와 [§5](#5-금지되는-형태)는 Core의 모든 변경이 따라야 하는
> 구조 규칙이며, 코드가 다르면 코드를 고친다.

## 1. Synchronization 개요

Core의 hot path는 application thread가 queue에 message를 넣고, I/O thread가 그 queue에서
꺼내 보내거나 그 반대로 넣어 주는 일이다. 이 장은 그 queue와 queue 주변 상태를 여러 thread가
함께 만질 때 어느 thread가 무엇을 소유하는지, 그리고 소유가 갈리는 지점마다 어떤 장치 —
lock, single-producer queue, atomic 값 — 를 쓰는지 정한다.

Caller가 의존하는 계약은 바뀌지 않는다. socket 하나를 여러 application thread에서 동시에 써도
된다는 계약은 [Socket 공통 §2 스레드 안전성](../socket/README.ko.md#2-스레드-안전성)이 소유하고,
어느 API가 직렬화되는지는 [Thread safety](04-thread-safety.ko.md)가 소유한다. 이 장은 그 계약을
지키는 데 **정확히 어떤 배타 장치가 필요하고, 그 이상은 왜 두지 않는지**를 정한다.

| 주체 | 이 장에서 정하는 것 |
|---|---|
| socket을 부르는 application thread | 공개 연산 하나가 socket turn 하나를 잡고 그 안에서 socket 상태를 만진다 |
| connection을 처리하는 I/O thread | pipe의 session 쪽 끝과 engine 상태를 혼자 소유하고, socket 쪽에는 command와 atomic 값으로만 말한다 |
| Core 유지보수자 | hot path에 lock을 더하거나 뺄 때 답해야 하는 질문과 제출해야 하는 측정 |

이 장은 일관되고 효과적인 동기화 구현을 위한 규칙을 적는다. 모델은
[framework의 state lane](../../../../framework/doc/framework/common/spec/server/01-execution/06-state-ownership-and-lanes.ko.md)과
같고, 용어는 그 문서를 따르되 Core에는 lane을 실행하는 별도 executor가 없고 socket turn이
그 역할을 한다.

## 2. 상태 분류

변경 가능한 상태는 무엇을 지켜야 하는지에 따라 셋으로 나뉘고, 분류가 정해지면 지키는 장치도
정해진다. 다른 장치로 바꾸지 않는다.

| 분류 | 언제 이 분류인가 | 그래서 어떤 장치로 지키는가 | Core에서 해당하는 것 |
|---|---|---|---|
| **C1 — 조회 레지스트리** | map 하나에 조회·추가·삭제만 하고, 다른 상태와 함께 지켜야 하는 조건이 없다 | 변경은 lock 하나 아래에서, hot path의 조회는 lock 없이 스냅샷으로 읽는다 | route shard의 RID→pipe 표, public poller의 handle 표, context의 socket registry |
| **C2 — 교차 불변식 상태** | 여러 field가 함께 바뀌어야 맞거나, 결정을 내린 뒤 그 결정으로 비동기 동작을 이어간다 | **소유 turn 하나**가 직렬로 실행하므로 turn 안의 field는 잠그지 않는다 | socket semantic 계층의 상태 전부 — 활성 pipe 집합, receive partition, lb·fq·dist 상태, multipart 진행, 지연 종료 큐 |
| **C3 — atomic 값** | 정수 증가, 단조 max, 플래그 확인, 참조 하나 교체만 한다 | release/acquire atomic 연산 | pipe의 `_state`·`_in_active`·`_out_active`, peer가 읽은 byte 수, auto-HWM의 planned/applied, 지연 종료 큐의 head |

한 객체에 셋이 섞이면 **C2가 이긴다** — 가장 강한 요구가 그 객체 전체의 장치를 정한다. C2
상태의 일부만 별도 lock이나 atomic으로 떼어 내면 떼어 낸 경계에서 함께 지켜야 할 조건이
깨진다. 반대로 C1·C3를 C2의 turn 아래로 끌어들일 필요는 없다 — 두 분류는 자기 장치만으로
완결된다.

## 3. 소유 단위

### 3.1 socket turn

공개 연산 하나가 시작될 때 그 socket의 실행 권한을 하나 잡고, 끝날 때 놓는다. 이 권한을
**socket turn**이라 부른다. socket의 C2 상태는 전부 이 turn이 소유하므로 turn 안에서는 그
상태를 잠그지 않는다. 구현은 공개 API 진입 상태어의 `public_api_sync` bit이며, 잡는 동작은
compare-and-swap 한 번이다. 경합이 없으면 비용은 atomic 연산 한 쌍이고, 경합하면
[Thread safety §3](04-thread-safety.ko.md#3-내부-규칙)의 backoff 규칙을 따른다.

turn을 잡는 주체는 둘이다.

| 주체 | 언제 잡는가 | 그래서 |
|---|---|---|
| application thread | send·recv·request·reply·option·bind·connect·close를 실행하는 동안 | 다른 application thread는 같은 socket에서 기다린다 — 이것이 thread-safe socket 계약의 실현이다 |
| command owner(application thread 또는 비동기 실행자인 I/O thread) | 그 socket의 mailbox에서 command를 꺼내 적용하는 동안 | command 적용과 공개 연산이 같은 상태를 동시에 만지지 않는다 |

두 주체 모두 **항상** 잡는다. socket type이나 command 종류에 따른 예외를 두지 않는다. 예외를
두려면 그 command가 C2 상태를 만지지 않는다는 것을 §6의 표로 보여야 하며, "자주 실행되니
lock-free로 둔다"는 이유는 예외의 근거가 아니다 — 그 가정은 측정 대상이다. turn을 쥔
코드는 같은 socket의 공개 API를 다시 부르지 않는다.

### 3.2 pipe의 두 끝

socket과 session 사이에서 message를 나르는 pipe([Architecture](01-architecture.ko.md))는 양
끝이 각각 thread 하나에 고정된다. socket 쪽 끝은 socket turn이, session 쪽 끝은 그
connection의 I/O thread가 소유한다([Threading model §3](02-threading-model.ko.md#3-thread-간-통신)).
두 끝 사이의 queue(`ypipe_t`)는 넣는 쪽 하나와 꺼내는 쪽 하나만 있는 single-producer
single-consumer 구조라 lock이 없다. 한쪽 끝이 다른 쪽 끝을 깨워야 하면 command
(`activate_read`, `activate_write`)를 보낸다.

그래서 pipe 끝의 상태는 "그 끝을 소유한 thread만 쓴다"가 기본이고, 반대쪽 끝이 읽어야 하는
값 — 예를 들어 peer가 소비한 byte 수 — 만 C3 atomic으로 발행한다. pipe 안에 lock을 두는 것은
[§4](#4-lock을-두어도-되는-조건)의 조건을 만족하는 경우뿐이다.

내부 확인 조건: `ypipe_t`의 sleep/awake 전이와 `activate_read`/`activate_write`를 보내는 조건은
[Polling](../05-polling.ko.md)의 level 규칙을 실현한다. 이 조건이 바뀌면 POLLIN·POLLOUT이
켜지는 시점이 바뀌므로, pipe 쪽 변경은 변경 전후의 그 조건이 같음을 함께 낸다.

### 3.3 mailbox

thread 사이의 command 전달 채널인 mailbox는 여러 thread가 동시에 넣고 소유 thread 하나가
꺼내는 multi-producer single-consumer 구조다. 넣는 쪽이 여럿이므로 삽입점에 lock 하나가
정당하며, 이것이 hot path에서 허용되는 유일한 "여러 producer" lock이다. 꺼내는 쪽은 소유
thread 하나라 lock이 없다.

### 3.4 context 수준 레지스트리

물리 queue registry, socket registry, auto-HWM plan 같은 context 수준 상태는 C1 또는 C3다.
hot path는 이들을 잠그지 않고 atomic load 또는 handle에 캐시된 값으로 읽는다. 등록·해제·plan
갱신 같은 변경만 lock 아래에서 일어나며, 그 변경은 message마다가 아니라 connection이나 plan
사건마다 한 번이다.

## 4. Lock을 두어도 되는 조건

hot path에 lock을 두려면 다음 질문에 답할 수 있어야 하고, 답이 "없다"면 그 lock은 없앤다.

> **이 lock이 없으면 같은 상태를 동시에 쓰거나 읽는 두 번째 thread가 이 경로에 실제로 있는가?**

| 두 번째 thread가 누구인가 | 그래서 무엇을 쓰는가 |
|---|---|
| 다른 application thread | socket turn. 별도 lock을 더 두지 않는다 |
| 반대쪽 pipe 끝(I/O thread ↔ socket) | single-producer queue와 C3 atomic. lock을 두지 않는다 |
| 여러 producer가 한 채널에 넣는다(mailbox) | 삽입점 lock 하나 |
| message당 0회인 경로(pipe 분리, peer 식별 같은 teardown·identity) | cold 경로 쪽이 lock을 잡고 hot path는 잡지 않는 구조로 만든다 |

hot path에 새 lock을 들이는 변경은 이 질문의 답과 [§6](#6-검증-요구)의 측정을 함께 낸다.

## 5. 금지되는 형태

| 형태 | 왜 안 되는가 | 실제로 있었고 제거된 예 |
|---|---|---|
| 지키는 조건이 없는 lock | 경합이 없어도 lock 한 쌍마다 명령 수와 cache line 왕복이 든다 | `activate_read` 처리의 `_out_sync`, 항상 비어 있던 지연 종료 큐의 context lock, planned와 applied가 같을 때도 잡던 registry lock |
| 함께 지켜야 할 조건을 여러 lock으로 나눔 | 나눈 경계에서 조건이 깨진다(C2 규칙) | socket 직렬화가 `public_api_sync`·command owner·command마다 `receive.sync`의 3겹이던 구조 |
| socket type이나 command 종류별 turn 예외 | "자주 실행된다"는 가정은 측정 대상이지 규칙의 근거가 아니다 | PAIR command를 turn 없이 적용하던 예외 |
| 반대쪽 pipe 끝이 쓰는 값을 lock으로 읽음 | single-producer 구조를 lock으로 다시 감싼다 | session 쪽 `_out_sync` 아래에서 읽던 peer 소비 byte 수 |
| 이름과 성질이 다른 장치 | 재귀 mutex를 `pthread_cond_wait`와 함께 쓰면 정의되지 않은 동작이다 | `fast_mutex_t`가 실제로는 재귀 mutex였던 것 — plain `mutex_t`와 `recursive_mutex_t`로 분리, 디버그 빌드는 재진입을 즉시 잡는다 |
| cold 경로 때문에 hot path가 lock을 짊어짐 | message당 0회인 경로가 message당 lock을 강제한다 | pipe 분리·peer 식별을 위해 hot path의 write가 잡던 `_out_sync` |

lock을 semaphore·spin·`try_lock` 재시도로 바꾸는 것은 형태를 바꾸는 것이 아니다. 같은 자리에
같은 이유로 남아 있으면 같은 금지에 걸린다.

## 6. 검증 요구

hot path의 lock을 추가·제거·범위 변경하는 변경은 다음을 같이 낸다. 앞의 넷은 내부 확인
조건이고, 마지막 하나는 공개 표면에서 관찰된다.

1. **lock별 표** — 획득 지점, message당 획득 횟수(callgrind), 지키는 조건, 두 번째 thread,
   조치. 측정 도구는 [Core hot path §5](10-hot-path.ko.md#5-성능-gate)의 축소 셀과 `hotpath_gate`
   다섯 셀이다.
2. **깨어남 조건 불변** — [§3.2](#32-pipe의-두-끝)의 내부 확인 조건. 변경 전후 코드가 같거나,
   다르면 왜 같은 조건인지.
3. **TSan** — 변경 전후 경고 집합의 차이가 0. 새 data race를 "성능상 무시"로 넘기지 않는다.
4. **lost-wake 시험** — `test_wake_invariants` 계열을 `--repeat until-fail:10` 이상, wake 경로를
   만졌으면 20. 알려진 lost-wake 수정의 회귀 테스트를 포함한다.
5. **회계 값 불변**(공개 관찰) — [Auto-HWM](06-auto-hwm.ko.md)과
   [Connection별 memory](05-connection-memory.ko.md)가 정의한 charge 값은 그대로다. 관측
   시점이 앞당겨지는 것은 허용하되, 시점을 command 경계에 못박은 문장이 있다면 그 변경은
   계약 결정 절차를 먼저 따른다.

## 7. 현재 인벤토리

STREAM tcp 1024 B 셀(CCU 20, callgrind)에서 message당 mutex 획득 횟수. Core 밖(boost.asio
reactor 내부)은 이 장의 대상이 아니지만 비교를 위해 적는다. 이 표는 lock 변경이 착지할 때마다
갱신한다.

| lock | 분류 | 두 번째 thread | 2026-09-07 | 목표 |
|---|---|---|---|---|
| mailbox 삽입점 `_sync` | §3.3 여러 producer | 여러 thread | 2.7 | 2.7 (구조) |
| socket 직렬화: `public_api_sync` + command owner + command마다 `receive.sync` | C2 → turn 하나 | application thread와 command owner | 1.47 | turn의 CAS만 |
| `read_activated` / `has_in`의 receive partition | C2 | 위와 같은 클러스터 | 1.28 | 0 |
| session 쪽 `pipe_t::write`/`flush`의 `_out_sync` | §3.2 SPSC + C3 | I/O thread 하나뿐 | 2.0 | 0 (peer 소비 byte를 atomic으로, `_out_active`를 CAS로) |
| socket 쪽 `_out_sync` | C2 → turn | application thread | 1.0 | 0 (cold 경로는 유지) |
| route shard `sync` | C1 | 조회만 hot | 1.0 | 0 (스냅샷 조회) |
| public poller handle 표 | C1 | 조회만 hot | 0.56 | 0 |
| boost.asio 내부 | Core 밖 | — | 4.0 | — |
| **합계** | | | **15.1** | **≈ 8.3 (Core 소유 10.1 → 3.3)** |

libzmq는 socket lock 하나 안에서 command 처리까지 하고 pipe에 mutex가 없어 mailbox의 1.7만
남는다. Core가 같은 수에 이르지 못하는 나머지는 asio reactor의 내부 lock이며, 그것은
[I/O thread](03-io-thread.ko.md)의 reactor 선택에 속한다.

## 8. 변경 절차

- hot path에 상태를 더하는 서브시스템은 먼저 [§2](#2-상태-분류)로 분류한다. C2면 socket turn
  아래에, pipe 양 끝에 걸치면 C3 atomic으로 설계한다. "일단 lock으로 두고 나중에 최적화"는
  하지 않는다.
- lock 변경은 [Core hot path §6](10-hot-path.ko.md#6-변경-절차)의 절차에 [§6](#6-검증-요구)의
  다섯 항목을 더해 제출한다.
- 이 장만으로 caller가 관측하는 동작을 바꿀 수 없다. 관측 동작을 바꾸려면
  [Socket 공통 §2](../socket/README.ko.md#2-스레드-안전성)와 [Polling](../05-polling.ko.md)의 계약
  절차를 먼저 따른다.

<!-- zlink-nav:start -->
[시스템 목차](README.ko.md) | [이전: Core hot path](10-hot-path.ko.md)
<!-- zlink-nav:end -->
