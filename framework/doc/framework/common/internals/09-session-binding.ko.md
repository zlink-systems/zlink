---
title: "9. Session과 Actor 연결"
---

# 9. Session과 Actor 연결

[내부 구조 목차](README.ko.md) · [이전: 8. 객체 종류와 활성화](08-object-lifecycle.ko.md) · [다음: 10. Liveness와 상태 공개](10-liveness-and-state.ko.md)

> **이 장이 답하는 것** — 외부 client 연결 하나를 Actor에 연결하고, 연결을 교체하는 동안
> message 유입을 어떻게 제어하는가.
>
> **계약 소유** — binding 계약은 [Session Actor dispatch](../spec/20-session-actor-dispatch.ko.md)가 소유한다.
> 이 장은 그 계약을 만족시키는 **구조**와, binding 교체에서 나타나는 실패를 다룬다.

Relocation 전체 상태 전이와 Session이 관여하는 정확한 구간은
[13. Relocation handoff 상태 전이](13-relocation-handoff.ko.md)가 소유한다. 이 장은 physical
Session과 Actor binding의 일반 구조만 설명한다.

외부 client 연결 하나를 Actor에 연결하는 구조다. 연결을 교체하는 동안에도 Actor owner가
current binding을 하나만 갖도록 보장해야 한다.

## 1. Session gate와 Actor gate를 나눈다

**결정 — session의 실행 권한과 Actor의 실행 권한은 서로 다른 권한이다.**

session callback을 실행하는 문맥이 Actor handler를 실행하지 않는다
([Session Actor dispatch 「3. Inbound dispatch와 reply」](../spec/20-session-actor-dispatch.ko.md#3-inbound-dispatch와-reply)).

나누지 않으면 두 방향으로 문제가 생긴다. 한 client가 보낸 packet 처리가 그 Actor가
속한 실행 단위인 [Spot](../spec/01-glossary.ko.md#spot) 전체를 잡거나, 반대로 Spot이 바쁠 때
그 연결의 keepalive 처리까지 밀린다.
연결 수명 관리와 업무 처리는 빈도도 지연 요구도 다르다.

**결정 — runtime이 쓰는 제어 record는 application queue에 넣지 않는다**
([Session Actor dispatch 「4. Session이 Actor route를 보관하는 방법」](../spec/20-session-actor-dispatch.ko.md#4-session이-actor-route를-보관하는-방법)). 연결 유지
신호가 업무 message와 같은 queue에서 기다리면, 업무가 밀릴 때 연결이 끊긴 것으로 오판할 수
있다.

## 2. 직렬 실행 원시 타입을 종류마다 새로 만들지 않는다

Spot, session, Actor 전달과 두 도메인 mailbox마다 직렬 실행 원시 타입을 따로 만들면 순서,
admission과 ready set을 관리하는 규칙도 각 타입으로 흩어진다.

**결정 — 순서, admission과 ready set을 관리하는 실행 engine은 하나만 둔다.** 타입을 따로
만들면 [7. 수신과 dispatch 루프](07-dispatch-loop.ko.md)의 한도 처리와 ready set 관리를
각 타입에서 다시 구현해야 한다. 그러면 같은 문제를 여러 곳에서 고쳐야 한다.

**결정 — 적용 위치별 차이는 여러 boolean 설정이 아니라 lane 정책 타입으로 표현한다.**
세 lane이 표현해야 하는 상태는 다음과 같다.

| 자리 | 갖는 상태 | 갖지 않는 상태 |
|---|---|---|
| Spot lane | 반납 대기, 이동 봉인 | 연결 닫힘 |
| session lane | 연결 닫힘 | 반납 대기, 이동 봉인 |
| Actor 전달 lane | — | 반납 대기, 이동 봉인, 연결 닫힘 |

**참·거짓 두세 개로 표현하면 안 되는 이유**는 조합의 대부분이 의미가 없기 때문이다.
"session에서 이동 봉인 켜짐", "Actor 전달에서 반납 대기 켜짐" 같은 조합은 존재할 수 없는
상태인데 타입이 그것을 허용하면 호출하는 쪽이 유효한 조합을 알고 있어야 한다. 봉인·반납
대기·닫힘은 각각 **다른 lifecycle과 다른 전이 규칙**을 가진 도메인 개념이지 기능 스위치가
아니다.

각 lane에서 유효한 상태만 표현하는 정책 타입을 공통 engine에 넘긴다. 언어에 따라 sealed
계층이든 tagged union이든 상관없다 — 판정 기준은 **의미 없는 조합을 만들 수 없는가**이다.

## 3. 연결을 교체하는 순서

이미 다른 session에 bind된 Actor를 새 session에 연결하면 physical connection 두 개가 잠시 유지될 수 있다.
그러나 Actor owner의 current binding은 항상 하나여야 한다. 새 binding을 current로 바꾼 뒤 이전 binding
generation의 ingress를 거부하면 message의 목적지는 새 session 하나로 정해진다.

**결정 — 새 연결을 즉시 확정하고 이전 exact session에는 one-way로 교체를 통지한다**
([Session Actor dispatch 「4. Session이 Actor route를 보관하는 방법」](../spec/20-session-actor-dispatch.ko.md#4-session이-actor-route를-보관하는-방법)).

```mermaid
sequenceDiagram
    participant SO as Session Owner
    participant AO as Actor Owner
    participant PO as Previous Session Owner

    SO->>AO: Bind exact Actor to new session
    AO->>AO: Install new current binding
    AO-->>SO: Return bind terminal
    SO->>SO: Switch to new route
    AO-)PO: Notify exact retired binding
    PO->>PO: Run replacement callback
    PO->>PO: Wait 100 ms after callback terminal
    PO->>PO: Close previous connection
```

통지에는 이전 binding을 정확히 식별할 수 있도록 다음 값을 함께 넣는다.

- Actor authority source fence
- 이전 session owner lifecycle
- session RID
- retired binding generation

이전 owner는 이 identity가 모두 일치할 때만 callback을 실행한다. Callback은 client에 중복
연결을 알릴 수 있는 마지막 application turn이다. Callback을 시작하기 전에 session을
closing으로 바꾸므로, 새로운 inbound application dispatch는 받지 않고 callback의 outbound
send만 허용한다.

Callback이 성공하거나 실패하여 terminal에 도달하면 Framework는 100 ms 뒤 connection을
닫는다. Outbound queue가 먼저 비어도 이 시간을 줄이지 않는다. Session lane이나 worker를
멈추지 않도록 infrastructure timer를 예약하고 callback turn은 즉시 반환한다. Timer는 retired
identity가 여전히 일치하는지 다시 확인한 뒤 connection을 닫는다. 통지, callback 또는 close가
실패해도 새 bind는 rollback하지 않는다.

**결정 — 연결 관계는 값 하나가 아니라 `(연결 식별자, 교체 순번)` 쌍으로 식별한다**
([Session Actor dispatch 「4. Session이 Actor route를 보관하는 방법」](../spec/20-session-actor-dispatch.ko.md#4-session이-actor-route를-보관하는-방법)). 교체 중에
이전 연결로 보낸 응답이 늦게 도착할 수 있고, 순번을 비교해야 그 응답이 지금 연결에
대한 것인지 판단할 수 있다.

## 4. 재접속과 이동을 구분한다

이 둘은 겉보기에 비슷하지만 정반대로 처리한다.

| 상황 | 연결 관계 | Application이 할 일 |
|---|---|---|
| client 재접속 | **새로 만든다** | 인증과 연결을 다시 수행한다 |
| Actor가 다른 node로 이동 | **유지한다** | 없다. runtime이 경로만 갱신한다 |

재접속은 새 session을 만들고, 이전 연결의 응답과 갱신은 새 session에 적용하지 않는다
([장애 대응과 failover 범위 「7. Store 장애」](../spec/31-failure-failover-policy.ko.md#7-store-장애)).
재접속 시도 자체는 client 라이브러리의 몫이다.

**결정 — 이전 연결 관계를 새 session으로 옮기려 시도하지 않는다.** 이전 연결 정보를
보관했다가 새 session에 복원하면 인증을 거치지 않은 연결이 이전 권한을 이어받을 수 있고,
정식 계약과도 어긋난다.

이동한 Actor에 연결을 유지하려면 옛 주소로 온 message를 새 owner로 넘기는 경로를
유지해야 한다([5. 이동 중 연속성](05-relocation-continuity.ko.md)). 그 경로가 없으면
이동 자체는 성공해도 session이 조용히 끊긴다.

### Relocation seal과 이전 binding 거부의 구분

**결정 — relocation seal과 retired binding 거부는 서로 다른 전이다.** Retired binding 거부는
current binding을 새 Session으로 교체한 뒤 이전 generation의 ingress를 막는다. Relocation
seal은 같은 binding의 Actor route를 옮기는 동안 Session message를 보관한다.

Session owner가 seal에서 하는 일은 세 가지뿐이다.

- current physical Session과 binding generation이 일치하는지 확인한다.
- 해당 binding의 request와 push를 application route로 제출하지 않고 보관한다.
- relocation 결과가 정해지면 source 또는 target route로 보관한 message를 제출하고 seal을 푼다.

### Session 검증의 단일 소유자

`SessionBindingAggregate`가 Session identity, binding generation, ActorId·ObjectGeneration,
relocation identity와 route 변경을 같은 직렬 실행 구간에서 다룬다. 이 값은 Session이 실제로
소유하는 연결 관계만 설명한다. Aggregate는 relocation target을 선택하지 않고 Location Store를
읽거나 쓰지 않으며 Actor authority를 다시 검증하지 않는다.

검증 책임은 다음과 같이 한 번씩만 둔다.

- Transport adapter는 authenticated peer, node generation과 frame 형식을 확인한다.
- Target relocation runtime은 temporary queue 설치와 Restore를 끝내고, cutover `[send]`를
  받거나 Restore 준비 응답을 보낸 뒤 1,000 ms가 지나면 예상 source owner로 Location Store
  CAS를 실행한다.
- Session aggregate는 current Session과 binding이 같은 relocation의 route 변경 대상인지 확인한다.
- Actor join, host relocation, Message Follow와 callback 경로는 위 판정을 반복하지 않는다.

Seal에는 numeric high-water를 사용하지 않는다. Source와 target 사이의 일반 message relay는
같은 TCP connection의 순서와 재전송에 의존한다. Session seal 중 도착한 message는 aggregate가
보관하지만 relocation 전용 record 수나 byte 상한을 두지 않는다. 개별 message 크기,
transport, deadline과 cancellation 제한은 그대로 적용한다.

Target만 Location Store owner CAS를 실행한다. Cutover가 1,000 ms 안에 오지 않으면
`cutover_timeout` Warning을 기록하고 같은 CAS 절차를 시작한다. CAS가 성공하면 target queue를
열고 target runtime이 command 44를 Session owner에게 `[send]`로 전달한다. Aggregate는 route와
current `ActorRef` snapshot을 target으로 바꾸고, 보관한 Session message를 새 route로 제출한 뒤
seal을 해제한다. Command 44에 대한 reply나 command 45는 사용하지 않는다.

Session owner는 seal 설치 시점부터 설정 가능한 `SessionRelocationSealTimeout`을 적용한다.
기본값은 3,000 ms다. 그 안에 exact command 44가 오지 않으면 physical Session을 닫고 binding,
held message와 seal state를 정리한다. Timeout과 command 44 처리는 같은 직렬 실행 구간에서 먼저
처리된 하나만 유효하다. 뒤늦은 command 44는 `late_session_route_update` Warning만 기록하고
무시한다.

```mermaid
sequenceDiagram
    participant C as Relocation coordinator
    participant S as Session owner
    participant A as Source runtime
    participant B as Target runtime
    participant L as Location Store

    C->>S: [request] command 42 · exact binding route 고정과 이후 message 보관
    S-->>C: [reply] command 43 · exact binding seal 설치 완료
    A->>B: [request] temporary queue 설치·Restore 후 dispatch 없이 relay 준비
    B-->>A: [reply] temporary queue·Restore 준비 완료 · source owner 유지
    A->>B: [send/request relay] cached queue와 ingress hold
    alt cutover arrives within 1,000 ms
        A->>B: [send] cutover · boundary 전 relay 완료
    else cutover timeout
        B->>B: [local] cutover_timeout Warning · fallback 진행
    end
    B->>L: [request] source fence가 같으면 owner를 target으로 CAS
    L-->>B: [reply] target owner CAS 결과
    B->>B: [local] queue 병합과 dispatch 개방
    B->>S: [send] command 44 · exact target route 적용·held 제출·seal 해제
    alt command 44 arrives within SessionRelocationSealTimeout
        S->>S: [local] route 전환 · held message 제출 · seal 해제
    else Session seal timeout
        S->>S: [local] physical Session 종료 · binding과 held state 정리
    end
```

CAS 성공 뒤 도착한 late 또는 duplicate cutover는 `late_cutover` Warning만 기록하고 무시한다.
Late 또는 duplicate command 44도 Warning만 기록하고 state를 다시 변경하지 않는다. Target이
cutover 전에 명시적으로 실패하면 matching seal만 해제하고 보관한 message를 source route로
제출한다. Cutover 뒤 failure는 source route를 다시 열지 않고 seal timeout으로 정리한다.

이 구조는 Session binding 검증을 한 곳에 모으지만 relocation 전체를 Session에 맡기지 않는다.
Actor·Spot 준비, server 간 relay, cutover `[send]`와 Location Store CAS는 공통 relocation runtime의
책임이다. §3의 retired binding 거부 규칙은 그대로 적용하며 relocation seal의 대기 상태와
합치지 않는다.

## 5. 확인할 결과

- 같은 연결의 두 session callback이 동시에 실행되지 않는다.
- session callback을 실행하는 문맥에서 Actor handler가 실행되지 않는다.
- 연결 유지 신호가 업무 message 뒤에 밀리지 않는다.
- 직렬 실행 원시 타입이 runtime 안에서 하나다.
- 연결 교체는 새 binding을 current로 등록하면 완료되며 이전 owner의 ACK를 기다리지 않는다.
- 이전 exact session callback terminal 100 ms 뒤 Framework가 connection을 닫는다.
- 완료 응답을 받기 전까지 session 소유자가 기존 경로로 message를 보낸다.
- 이전 연결로 늦게 도착한 응답이 교체 순번 비교로 걸러진다.
- client가 재접속하면 이전 연결 관계가 복원되지 않는다.
- Actor가 다른 node로 이동한 경우에는 연결을 다시 만들지 않고 경로만 갱신된다.
- Command 42와 43이 current binding의 seal 설치 결과만 전달한다.
- `SessionBindingAggregate` 한 곳에서 current Session과 binding route를 검증하며 다른 구성
  요소가 Store나 local mirror로 재검증하지 않는다.
- Target만 Restore 뒤 cutover를 받거나 1,000 ms가 지나면 Location Store owner CAS를 수행한다.
- CAS 성공 뒤 target이 command 44를 `[send]`로 전달하면 Session owner가 route를 바꾸고 held
  Session message를 target route에 제출한 다음 seal을 해제한다.
- `SessionRelocationSealTimeout`의 기본값은 3,000 ms이며, timeout이면 physical Session과 관련
  state를 정리한다.
- Late 또는 duplicate cutover와 command 44는 Warning만 남기고 state를 다시 변경하지 않는다.

---

[내부 구조 목차](README.ko.md) · [이전: 8. 객체 종류와 활성화](08-object-lifecycle.ko.md) · [다음: 10. Liveness와 상태 공개](10-liveness-and-state.ko.md)
