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

## 2. 두 상한은 층위가 다르다 — 섞으면 안 된다

| 층 | 자원 | 누가 소비하나 | 고갈 시 올바른 동작 |
|---|---|---|---|
| Core byte HWM | socket queue 바이트 | 송신 | send 대기. 마지노선 |
| **Application Job Queue permit** (04 §6) | 처리할 inbound job 개수 | **수신** | peer에게 `PAUSED`. "나 지금 힘드니 보내지 마" |
| **Completion 예약** (01 §11, 4,096) | 내가 낸 outbound operation | **송신** | ← **이 문서가 정하려는 것** |

**중요 — 이전 세션이 한 번 틀린 지점이다.** completion 예약을 04 §6의 receive-flow 입력에
넣자는 안은 **방향이 반대다.** completion 예약은 *내가 요청을 낼 때* 소비하고, receive-flow는
*내가 받기 버거울 때* 상대를 멈추는 수단이다. DEALER-ROUTER에서 `PAUSED`를 걸면 상대의
reply까지 막혀 내 예약 반환이 오히려 늦어진다. **자기 교착을 만든다.**

즉 completion 예약 고갈의 backpressure는 **peer가 아니라 내 호출자**를 향해야 한다.

---

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

### 결정 ③ 고갈 시 동작 — **기다렸다가 deadline** 을 제안한다

지금은 즉시 `CapacityExceeded`다. 이것이 벤치를 죽이는 직접 원인이다.

`04-application-job-queue-and-backpressure.ko.md` §8이 이미 **3단계 backpressure**(대기 →
제출 → `DeadlineExceeded`)를 규정한다. completion 예약도 같은 3단계를 따르는 것이 일관된다.

| 상황 | 제안하는 동작 |
|---|---|
| 자리 있음 | 즉시 예약하고 제출 |
| 자리 없음 + deadline 있음 | **deadline까지 예약을 기다린다.** 못 얻으면 `DeadlineExceeded` |
| 자리 없음 + deadline 없음 | `CapacityExceeded` (지금 동작 유지) |

이러면 벤치는 느려질 뿐 실패하지 않는다. 사용자가 말한 "백프레셔에 걸렸다가 풀리는" 동작이 된다.

---

## 4. 개정 문안 (§3 제안을 모두 채택했을 때)

01-submit-and-completion.ko.md §11의 해당 두 문단을 아래로 교체한다.

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
> **예약할 자리가 없으면 operation의 absolute deadline까지 자리를 기다린다.** 기다리는 동안
> request를 보내지 않는다. deadline 안에 자리를 얻으면 그대로 제출하고, 얻지 못하면
> `DeadlineExceeded`로 끝낸다. 이는
> [Application Job Queue와 backpressure §8](../../01-execution/04-application-job-queue-and-backpressure.ko.md#8-backpressure-3단계와-한도-종류)의
> 3단계 backpressure와 같은 규약이다. **deadline이 없는 operation만 `CapacityExceeded`로
> 즉시 거부한다.** 한 번 수락한 operation의 completion enqueue에는 거부하거나 버리는 경로가
> 없다.
>
> **이 상한은 peer에게 전파하지 않는다.** completion 예약은 요청을 내는 쪽이 소비하는
> 자원이므로 backpressure는 호출자를 향한다. 수신 부하에 대한 peer 제어는
> [같은 문서 §6](../../01-execution/04-application-job-queue-and-backpressure.ko.md#6-pressure-상태와-socket-제어)의
> receive-flow state가 단독으로 소유하며, 두 경로를 섞지 않는다.

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
