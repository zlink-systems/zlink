# 스펙 개정안 — #259 completion 예약 상한

작성 2026-09-12. **사용자 결정이 필요한 항목이 3개다(§3).** 결정 뒤 codex 리뷰 → 4언어 적용.

---

## 1. 무엇이 문제인가

`framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md` §11의
마지막 두 문단이 현재 이렇다.

> Terminal winner가 진행 중 호출 표의 항목을 꺼낸 뒤 dispatcher admission에 실패하면
> application completion을 잃는다. 따라서 operation을 수락할 때 completion dispatcher
> 자리도 함께 예약한다. 이 예약은 callback이 반환할 때까지 유지한다. **진행 중 operation과
> dispatcher에서 대기·실행 중인 callback을 합친 수는 4,096개를 넘지 않으므로** callback
> queue가 제한 없이 증가하지 않는다.
>
> **예약할 자리가 없으면 request를 보내기 전에 `CapacityExceeded`로 거부한다.** 한 번
> 수락한 operation의 completion enqueue에는 거부하거나 버리는 경로가 없다.

이 문단이 정하지 않은 것이 셋이다. 구현이 갈라진 원인이다.

1. **4,096의 소유 단위** — host 전체인가, MeshNode당인가, connection당인가.
2. **적용 범위** — completion을 등록하는 모든 Messaging Request인가, RouteMesh request만인가.
3. **자리가 없을 때의 동작** — 즉시 거부인가, 기다렸다가 deadline인가.

### 관측된 결과

`framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1389`

```cpp
if (!start->registered) {
    throw framework_exception_t (
      framework_error_kind_t::capacity_exceeded,
      "raw mesh request completion capacity is exhausted");
}
```

같은 패턴이 send 경로 `:1533`, `:1565`에도 있다. **C++ request 벤치가 전부 이 예외로 죽는다.**

벤치는 밀어넣을 수 있는 만큼 밀어넣는 것이 규약이고 흐름 제어는 Framework가 한다.
스펙대로라면 throughput이 낮거나 latency가 늘 뿐 **측정이 실패해서는 안 된다.**
지금의 측정 실패가 곧 스펙 미준수의 증거다.

---

## 2. 두 상한은 층위가 다르다

| 층 | 자원 | 누가 소비하나 | 고갈 시 동작 |
|---|---|---|---|
| Core byte HWM | socket queue 바이트 | 송신 | send 대기. 마지노선 |
| **Application Job Queue permit** (04 §6) | 처리할 inbound job 개수 | **수신** | peer에게 `PAUSED` |
| **Completion 예약** (01 §11, 4,096) | 내가 낸 outbound operation | **송신 측 등록** | ← 이 문서가 정하려는 것 |

### 2.1 reply는 막히지 않는다 — 분리 lane이 그래서 있다

**이전 세션이 "completion 예약을 receive-flow로 전파하면 자기 교착"이라고 적었는데 틀렸다.**
근거 없는 일반화였다. 정정한다.

reply의 전송 경로는 **source peer의 종류로 갈린다.** `core/doc/spec/core/socket/07-router.ko.md:271-274`

> Source peer가 **DEALER이면 현재 ready Application pipe**를, **ROUTER이면 현재 ready
> completion progress lane의 Completion pipe**를 고른다.

admission 규칙도 다르다. 같은 문서 `:439-441`

> **DEALER peer에 대한 reply**는 현재 ready Application pipe를 사용하고 **HWM·PAUSED와
> `SNDTIMEO` admission을 적용**하므로 `BACKPRESSURED`+`EAGAIN`이 될 수 있다.
> **ROUTER peer에 대한 reply**는 현재 ready Completion pipe를 사용하며 **HWM-free** admission을
> 적용한다.

**ROUTER peer 쪽에는 `PAUSED`가 admission 목록에 아예 없다.** 분리 lane이 바로 이것을 막으려고
존재한다.

#### RouteMesh (ROUTER-ROUTER) — 교착 없음

node A가 completion 예약 고갈로 자기 ROUTER에 `PAUSED`를 걸면 B는 A에게 **새 요청을** 못 보낸다.
그러나 B가 A에게 보내는 **reply는 Completion pipe로 HWM-free** 진입하므로 계속 도착하고,
**그 reply가 A의 예약을 반환시킨다.** 압력이 풀린다. **#259가 실제로 터지는 곳이 여기다.**

#### ClientServer (DEALER-ROUTER) — DEALER는 애초에 pause할 이유가 없다

이 topology는 **요청 방향이 하나다. DEALER → ROUTER 요청만 있고 ROUTER는 DEALER에게 요청을
보낼 수 없다.** ROUTER가 DEALER에게 보내는 것은 **reply뿐**이다.

따라서 DEALER의 inbound는 전부 *자기가 낸 요청의 응답*이고, 그 개수는 자기 미완료 요청 수로
이미 제한된다. **DEALER에는 보호할 inbound job 압력이 없으므로 `PAUSED`를 걸 이유가 없다.**

`:439`의 "DEALER peer에 대한 reply에 HWM·PAUSED 적용"은 *DEALER가 `PAUSED`를 걸었을 때*의
규칙인데, 위 이유로 그 상태가 생기지 않는다. **Framework 사용에서 발생하지 않는 경로다.**

receive-flow를 거는 쪽은 job queue를 가진 **서버(ROUTER)뿐**이고, 서버의 `PAUSED`는 클라이언트의
요청을 멈출 뿐 **서버 자신이 내보내는 reply에는 영향이 없다** — `PAUSED`는 내가 *받는* 것을
막는 상태이지 내가 *보내는* 것을 막는 상태가 아니기 때문이다.

**결론 — 두 topology 모두에서 receive-flow 전파는 안전하다.**

## 3. 결정이 필요한 3항목

### 결정 ① 소유 단위 — **host(process) 공통** 을 제안한다

**사용자 의견(2026-09-12):** "이 4096 개수 제약은 한 서버 공통이야? … 내 생각에는 공통 queue
사이즈 제약이 되어야 맞을것 같은데. 서버에서 처리해야할 메시지가 쌓이고 있으면 서버로 들어
오는 요청에 백프레셔가 걸려야 하는게 맞는거 같아서"

MeshNode당으로 두면 node 2개인 host가 8,192개를 쓴다. connection당으로 두면 상한이 사실상
없다. 04 §6도 이미 **"Host queue owner"** 를 쓰므로 host 단위가 두 문서에서 일관된다.

구현도 이미 host 단위다 — `operation_registry.cpp:139-147`의
`_state->reserved >= default_operation_capacity`가 **process-shared** 카운터다.
**스펙이 그 사실을 말하지 않을 뿐이다.**

### 결정 ② 적용 범위 — **completion을 등록하는 모든 Messaging Request** 를 제안한다

RouteMesh request·send와 ClientServer request를 모두 포함한다. 한쪽만 집계하면 host 총량
보장이 깨진다. 현재 ClientServer 경로가 세 언어에서 이 집계 밖에 있다.

### 결정 ③ 고갈 시 동작 — **receive-flow 전파** 를 제안한다

지금은 즉시 `CapacityExceeded`다. 이것이 벤치를 죽이는 직접 원인이다.

**사용자 설계(2026-09-12):** "framework hwm 은 job 개수를 조절하는거야. 4096 은 쌓인 job 의 개수
마지노선 이고 그개수가 차면 보내는쪽으로 '나 지금 힘든 상태니 잠시 보내지마' 하는거고 그럼
보내는쪽에서는 메시지를 받아서 core socket 백프레셔 설정을 하는거고"

즉 상한에 닿으면 **예외를 던지는 것이 아니라 peer에게 `PAUSED`를 보낸다.** §2.1에서 확인했듯
reply는 계속 들어오므로 압력이 스스로 풀린다.

| 상황 | 제안하는 동작 |
|---|---|
| 자리 있음 | 즉시 예약하고 제출 |
| pause 경계 도달 | 04 §6과 같은 방식으로 `PAUSED` 전이. **예외를 던지지 않는다** |
| resume 경계 복귀 | `RUNNING` 전이 |
| 그래도 자리가 없이 제출 시도 | operation deadline까지 대기 → `DeadlineExceeded` |
| deadline 없는 operation | `CapacityExceeded` (현행 유지) |

**04 §6의 pause 80 % / resume 60 % 경계를 그대로 쓴다.** 새 제어 지점을 만들지 않는다.
04 §6이 "이 receive-flow state API가 Framework pressure와 Core send flow 사이의 **유일한** runtime
제어 지점"이라고 이미 규정했으므로, completion 예약도 같은 지점을 통해야 규칙이 늘지 않는다.

이러면 벤치는 느려질 뿐 실패하지 않는다.

## 4. 개정 문안 (§3 제안을 모두 채택했을 때)

`01-submit-and-completion.ko.md` §11의 해당 두 문단을 아래로 교체한다.

> Terminal winner가 진행 중 호출 표의 항목을 꺼낸 뒤 dispatcher admission에 실패하면
> application completion을 잃는다. 따라서 operation을 수락할 때 completion dispatcher
> 자리도 함께 예약한다. 이 예약은 callback이 반환할 때까지 유지한다.
>
> **이 예약 상한은 host가 하나로 소유한다.** host 안의 MeshNode 수, ClientServer channel 수,
> connection 수와 무관하게 하나의 카운터를 공유한다. MeshNode별이나 connection별로 나누어
> 갖지 않는다. **집계 대상은 completion을 등록하는 모든 Messaging Request다** — RouteMesh의
> request와 send, ClientServer의 request를 모두 포함한다. 진행 중 operation과 dispatcher에서
> 대기·실행 중인 callback을 합친 수는 이 상한을 넘지 않으므로 callback queue가 제한 없이
> 증가하지 않는다. 기본값은 4,096이다.
>
> **상한에 가까워지면 예외를 던지지 않고 receive-flow 상태로 전이한다.** Host queue owner는
> 이 예약 사용량을
> [Application Job Queue와 backpressure §6](../../01-execution/04-application-job-queue-and-backpressure.ko.md#6-pressure-상태와-socket-제어)의
> pressure 계산에 함께 넣는다. 같은 pause·resume 경계(기본 80 %·60 %)와 같은 절대 상태 적용
> 규칙을 쓰며, **새로운 제어 지점을 만들지 않는다.** `PAUSED`는 peer가 이 host로 보내는 새
> 요청을 멈추게 하고, 이미 진행 중인 operation의 reply는 계속 도착해 예약을 반환한다.
> ROUTER peer에 대한 reply는 Completion pipe의 HWM-free admission을 사용하므로 `PAUSED`가
> 적용되지 않는다는 계약을
> [ROUTER §13](../../../../../../../core/doc/spec/core/socket/07-router.ko.md)이 소유한다.
>
> **그럼에도 자리가 없는 상태에서 제출을 시도하면** operation의 absolute deadline까지 자리를
> 기다린다. 기다리는 동안 request를 보내지 않는다. deadline 안에 자리를 얻으면 그대로
> 제출하고, 얻지 못하면 `DeadlineExceeded`로 끝낸다. 이는 같은 문서 §8의 3단계 backpressure와
> 같은 규약이다. **deadline이 없는 operation만 `CapacityExceeded`로 즉시 거부한다.** 한 번
> 수락한 operation의 completion enqueue에는 거부하거나 버리는 경로가 없다.

`04-application-job-queue-and-backpressure.ko.md` §6의 pressure 입력에 다음을 더한다.

> Host queue owner는 **ordinary job permit 사용량과 completion 예약 사용량 중 더 높은 비율**로
> pressure 상태를 계산한다. 두 자원은 각자의 상한을 가지며, 어느 한쪽이 pause 경계에 닿으면
> 전체가 `PAUSED`로 전이하고 **둘 다** resume 경계 아래일 때 `RUNNING`으로 돌아온다.

---

## 5. 적용 범위 — 4언어

| 언어 | 현재 | 해야 할 일 |
|---|---|---|
| C++ | `raw_mesh_node_owner.cpp:1389,1533,1565`에서 `capacity_exceeded` throw | deadline 대기로 교체 |
| .NET | 확인 필요 | host 단위 집계 + deadline 대기 |
| Java | 확인 필요 | 〃 |
| Node | 확인 필요 | 〃 |

ClientServer 경로가 집계에 들어가 있는지를 4언어 모두 확인한다.

---

## 6. 절차

1. **사용자가 §3의 ①②③을 결정한다** ← 지금 여기
2. 결정에 맞춰 §4 문안을 확정하고 `scripts/verify-framework-doc-contracts.sh`를 돌린다
3. codex 리뷰
4. 4언어 적용 + 회귀 테스트
5. C++ request 벤치 5쌍 완주 확인 → #259 해소
6. gRPC 포함 전체 비교표
