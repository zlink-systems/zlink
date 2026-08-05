---
title: "STREAM 서버 session"
---

# STREAM 서버 session

[스펙 목차](README.ko.md) · [이전: Spot·Actor routing](18-object-routing.ko.md) · [다음: Session Actor dispatch](20-session-actor-dispatch.ko.md)

> **이 장이 정의하는 것** — 서버 쪽 STREAM session(연결 하나를 수락한 때부터 닫을
> 때까지 packet 처리와 request correlation을 유지하는 실행 단위)의 공개 계약.


이 문서는 ZLink Framework의 서버 쪽 STREAM session(연결 하나를 수락한 때부터
닫을 때까지 packet 처리와 request correlation을 유지하는 실행 단위) 공개 계약을 정의한다. 대상 독자는 서버 session
표면, dispatch, 등록, codec과 오류 경계를 구현하는 Framework 개발자다. Client 쪽
계약은 [Stream Connector 공통 스펙](stream-connector/32-stream-connector.ko.md)이
정의하며 두 문서는 같은 wire 계약을 공유한다. 언어별 type과 signature는
[언어별 Server interface 목차](server/languages/README.ko.md)의 STREAM 문서가
고정한다.

## 1. 목적

STREAM은 일반 request-response와 성격이 다르다. 다음이 훨씬 중요한 축이 된다.

- 연결 수명
- peer 식별
- packet framing
- session lifecycle

Framework는 STREAM을 header 기반 packet session으로 처리한다. Raw byte stream을
application에 그대로 넘기지 않는다.

## 2. 기본 방향

- Framework는 stream header를 decode하여 packet name과 metadata를 dispatch context에
  넣고, 아직 업무 객체로 변환하지 않은 payload와 함께 session callback에 전달한다.
- Framework는 모든 언어에서 STREAM transport ingress를 `recv` mode로 운영한다. Framework는
  Core의 STREAM packet callback 또는 raw receive callback을 등록해 application packet을
  받지 않는다. 이 규칙은 언어별 binding의 표현이 달라도 동일하게 적용한다.
- Framework 내부의 `recv loop`가 raw part를 읽고 header framing을 조립한다. 이 loop가
  managed queue에 packet을 넘길 수 없으면 다음 receive를 수행하지 않아 Core receive pipe의
  HWM이 backpressure 경계로 동작하게 한다.
- Application은 [packet name](01-glossary.ko.md#packet-name)으로 처리할 packet을 구분하고 Framework의 공통 decoder
  표면을 사용한다. 이 표면은 등록된 codec registry로 payload를 업무 객체로
  변환하므로 handler가 codec별 helper를 직접 선택하지 않는다(§5).
- Transport 본체는 header framing까지만 처리하며 payload의 업무 객체 변환을
  담당하지 않는다.
- Session 연결과 연결 해제 lifecycle callback을 기본 표면으로 제공한다.
- 오류 callback에는 application 예외가 아니라 해당 session에 귀속되는 transport 오류만
  전달한다(§6).

다음 기능은 이 계약의 범위에 포함하지 않는다.

- Application이 직접 실행하는 recv loop(§4)
- Raw chunk 직접 처리
- 사용자 정의 header framing. Header binary 형식은 Framework와 connector가 공유하는
  내부 protocol이며 application이 이 형식을 바꾸는 설정은 제공하지 않는다.

## 3. Dispatch 모델

Framework 내부 `recv loop`는 application session callback을 바로 실행하지 않는다.
Framework가 packet을 관리 queue에 넣은 뒤 session callback을 실행한다. 이 queue
경계에서 Framework의 dispatch, DI와 logging을 일관되게 적용한다.

[STREAM session dispatch](01-glossary.ko.md#stream-session)에는 Handler filter를 적용하지 않는다.
다른 dispatch의 filter 적용 범위와 실행 규칙은
[Framework API §8.1](06-framework-api.ko.md#81-handler-filter)이 정한다.

- Session callback은 packet name, metadata와 request 정보를 담은 dispatch context와
  payload를 받는다.
- Runtime은 request header 값을 dispatch context 안에 보존한다. Application이 header 객체를
  만들거나 relay 호출에 다시 넘기지 않는다.
- `recv` 결과에서 얻은 peer 식별 값인 routing ID는 session dispatch까지 정보 손실 없이
  전달된다.

### 3.1 reply 상관관계

Session이 만드는 `Response`와 `Error`는 원본 request의 request sequence를 그대로
반환한다. Client는 이 sequence만으로 pending request를 찾는다.

- `Response`·`Error` header에 packet name을 담지 않는다. 응답은 handler를 고르지 않으므로 그
  필드가 필요 없고, 언어마다 다른 값을 채워 넣으면 진단만 어긋난다.
- typed reply의 decode 타입은 client가 호출 시 지정한 타입이다. 이름으로 고르지 않는다.
- `Error`도 같은 sequence로 되돌린다.

전체 규칙은 [메시지 모델](04-message-model.ko.md)의 reply correlation 계약이
정의한다.

## 4. Framework 내부 recv loop와 application 표면

Framework는 내부에서 `recv loop`를 소유하지만 raw receive loop를 application 공개 표면으로
노출하지 않는다. Application은 session callback만 사용하고, 수신 순서·취소·backpressure와
header framing은 Framework가 관리한다.

모든 Framework 언어의 transport ingress는 다음 경계를 따른다.

```text
Core receive pipe
    -> Framework recv loop
    -> header framing and queue admission
    -> session callback
```

Core packet callback이나 raw receive callback을 사용해 queue admission을 우회하면
Core receive pipe의 HWM이 application queue를 제한하지 못하므로 Framework contract를
만족하지 못한다. Framework는 queue admission이 실패한 동안 새 packet을 읽지 않으며,
이미 받은 packet을 버리거나 같은 packet을 callback으로 재전달하지 않는다.

## 5. Codec 계층 분리

Framework 기본 표면은 session, session context, stream과 message까지만 제공한다.

- 객체 변환은 raw transport message가 아니라 framework message와 별도 codec extension이
  맡는다.
- raw transport나 framework 기본 runtime에 특정 codec 구현을 직접 섞지 않는다.
- session handler는 codec별 helper를 직접 호출하지 않는다. JSON·Protobuf·MessagePack·custom
  codec을 바꿔도 업무 코드는 같은 decode 표면을 쓴다.
- server framework, HTTP client host와 stream connector는 codec 번호, content-type과 typed payload 선택
  계약을 공유하지만 registry instance는 공유하지 않는다. Server는 server root별 registry, HTTP client는
  host별 registry([HTTP Client §5](http-client/12-http-client.ko.md#5-codec)),
  connector는 connector instance별 typed codec option
  ([Stream Connector §5.4](stream-connector/32-stream-connector.ko.md#54-codec))을
  소유한다.

## 6. 오류 경계

| 오류 | 어디로 가는가 |
|---|---|
| 해당 session에 귀속되는 transport 오류 | Session 오류 callback으로 전달한다. |
| Handshake 실패 | Session이 만들어지기 전의 실패이므로 session callback을 실행할 대상이 없다. Runtime monitoring에만 기록한다. |
| Socket·node 단위 오류 | 특정 session 하나의 오류로 확정할 수 없으므로 session callback을 실행하지 않고 Runtime monitoring에 기록한다. |
| Application handler 예외 | Transport 오류가 아니므로 session 오류 callback을 실행하지 않고 handler 예외 처리 경로를 사용한다. |

Session 오류 callback은 monitor에서 관찰 가능한 transport 오류를 session 단위로 다시 올려주는
축으로만 제한한다.

세션이 닫힐 때의 종료 사유는 [Stream Connector §6.3](stream-connector/32-stream-connector.ko.md#63-종료-사유)의 닫힌 집합과
정합하며, 계기는 [runtime-metrics §4](25-runtime-metrics.ko.md#4-object와-stream)가 소유한다.

## 7. 등록 모델

Stream node는 명시적으로 등록한다. Attribute나 decorator 기반의 암시적 등록은
제공하지 않는다.

다음 .NET 발췌는 bind endpoint, TLS와 session type을 한 Stream node에 등록하는
방법을 보여준다. 다른 언어에 같은 signature를 요구하지 않으며, 정확한 .NET 계약은
[.NET configuration interface](server/languages/dotnet/interfaces/03-configuration-topology.ko.md)가
정의한다.

```csharp
public interface IZLinkStreamNodeBuilder
{
    IZLinkStreamNodeBuilder Bind(string endpoint);
    IZLinkStreamNodeBuilder Bind(int port = 0);
    IZLinkStreamNodeBuilder SetBindHost(string bindHost);
    IZLinkStreamNodeBuilder SetAdvertiseHost(string advertiseHost);
    IZLinkSocketConfig ConfigureSocket();
    IZLinkStreamNodeBuilder SetTlsServer(
        string certificatePath,
        string keyPath,
        bool requireClientCertificate = false);
    IZLinkStreamNodeBuilder AddSession<TSession>()
        where TSession : class, IZLinkSession;
}
```

```csharp
options
    .AddStreamNode("gateway")
    .Bind(7400)
    .SetBindHost("0.0.0.0")
    .SetAdvertiseHost("node-a.example.net")
    .ConfigureSocket().MaxMessageSize = 64 * 1024; // client에서 server로 받는 complete STREAM message의 기본 상한이다.
    .SetTlsServer(
        "server.crt",
        "server.key",
        requireClientCertificate: true)
    .AddSession<GatewaySession>(); // 이 node에서 사용할 session type 하나를 등록한다.
```

`ConfigureSocket().MaxMessageSize`는 이 StreamNode의 Core STREAM inbound option이다. 기본값은
`64 KiB`이며 complete message의 크기를 6-byte prefix를 제외한 header byte와 payload byte의 합으로
계산한다. 이 상한은 client에서 server로 들어오는 message에만 적용하고 server에서 client로 보내는
message에는 적용하지 않는다. `0`은 별도 Framework 상한을 사용하지 않도록 Core에 `-1`을 전달하며,
양수는 유한한 상한이다. 음수는 startup configuration error다. 상한을 넘은 message는 session handler에
일부도 전달하지 않고 server 측에 `EMSGSIZE`를 기록한 뒤 연결을 종료한다. raw client에는 error code를
보내지 않으므로 client는 연결 종료만 관찰한다.

등록 표면의 축:

| 축 | 의미 |
|---|---|
| Stream node 이름 | Node를 식별한다. |
| Bind endpoint | 반드시 지정해야 한다. |
| Session type 등록 | Stream node 하나에 session 하나만 등록한다. |

### 7.1 TLS

Stream node는 TLS를 사용할 수 있다. TLS를 켜면 인증서 경로와 key 경로를 함께 지정해야 한다.
client 인증서를 요구할지는 같은 server TLS 설정에서 선택한다. 기본값은 요구하지 않는 것이며,
요구하도록 설정하면 client certificate 검증에 실패한 연결은 session을 만들기 전에 거부한다.
Client 쪽 transport 선택은 endpoint scheme이 결정한다
([Stream Connector §3](stream-connector/32-stream-connector.ko.md)).

### 7.2 Startup validation

다음은 host 시작 전에 설정 오류로 실패한다.

| 조건 | 결과 |
|---|---|
| Stream node 이름이 비어 있다. | 설정 오류로 startup에 실패한다. |
| 같은 stream node 이름을 두 번 등록했다. | Node 이름은 runtime 식별자이므로 설정 오류로 startup에 실패한다. |
| Bind endpoint가 없다. | 설정 오류로 startup에 실패한다. |
| 같은 session type을 중복 등록했다. | 설정 오류로 startup에 실패한다. |
| 한 node에 session을 둘 이상 등록했다. | 설정 오류로 startup에 실패한다. |
| TLS를 켰지만 인증서 경로가 비어 있다. | 설정 오류로 startup에 실패한다. |
| TLS를 켰지만 key 경로가 비어 있다. | 설정 오류로 startup에 실패한다. |
| TLS server를 설정하지 않고 client 인증서를 요구했다. | 설정 오류로 startup에 실패한다. |

등록 시점에 이 node가 header 기반 packet 경로라는 사실이 분명히 드러나야 한다.

## 8. Session에서 actor로

session이 받은 packet을 actor로 넘기는 계약은
[session-actor-dispatch](20-session-actor-dispatch.ko.md)가 소유한다.

Session callback은 [Spot](01-glossary.ko.md#spot) 상태를 직접 변경하지 않는다. Actor dispatch나 Spot 호출을
제출하는 데까지만 처리한다
([Stage wrapper on Spot §3](17-stage-wrapper-on-spot.ko.md#3-spot-turn-보존)).

Actor가 다른 MeshNode에 있어도 physical STREAM socket과 session object는 현재
session owner에 유지된다. Framework는 bind control, Actor ingress와 Actor push만
[MeshNode](01-glossary.ko.md#meshnode) 사이의 raw ROUTER service record로 전달한다. Application에는 target Node
RID, binding generation(같은 session [owner](01-glossary.ko.md#owner) process lifecycle 안에서 binding이 교체된 순서),
authority fence와 command 24·36·38의 codec을 노출하지 않는다. Session을 닫을 때는
current [binding generation](01-glossary.ko.md#binding-generation)의 tombstone을 제출하므로
이전 bind에서 늦게 도착한 close가 새 binding을 해제하지 못한다. Command별 정확한
전달 계약은 [Session–Actor dispatch §4](20-session-actor-dispatch.ko.md#4-session이-actor-route를-보관하는-방법)가
정의한다.

Physical disconnect 때 Framework는 current binding snapshot의 모든 Actor에 저장 route로 자동 통지한다.
Application callback은 bound 목록을 순회하지 않는다. 한 Actor의 실패는 다른 Actor 통지와 cleanup을
막지 않으며 exact binding identity마다 Spot disconnect callback을 최대 한 번 실행한다. Public
`NotifyDisconnected`는 connection이 유지되는 동안 application이 보내는 논리적 통지로 유지한다.

Bound Actor가 relocation되면 physical STREAM socket과 Session object는 그대로
유지한다. Target Actor를 복원하고 owner·membership commit을 완료하면 Target Actor가
message를 처리하기 시작한다. 그 뒤 target runtime이
`sessionActorLocationUpdateReqMsg`를 send하여 Session owner에 저장된 해당 Actor binding
route, 즉 현재 Actor owner에 전달할 경로를 target owner로 갱신하도록 요청한다.
Route switch와 함께 bound-session current Actor location snapshot도 같은 ActorId·ObjectGeneration을
유지한 채 target MeshName·NodeRid로 갱신한다. 같은 Session에서 relocation 대상에 포함되지 않은 다른 Actor의 route와 location snapshot은
바꾸지 않는다. Session owner는 갱신한 뒤 `sessionActorLocationUpdateResMsg`를 send한다.
응답이 없으면 target runtime은 최초 send 1초 뒤부터 1초, 2초, 4초, 5초 간격으로 같은
요청을 다시 보내고 이후에는 5초 간격을 유지한다. 응답을 기다리는 동안에도 Target Actor는
message를 처리하며, 이전 route로 도착한 message는 Message Follow route가 전달한다. Route
update는 같은 ObjectGeneration에만 허용하고 application은 relocation을 알기 위해 rebind하지
않는다. 새 incarnation은 explicit bind가 필요하다.

## 9. 구현 및 contract test 검증 요구

| 항목 | 검증 |
|---|---|
| dispatch 경로 | Framework 내부 recv loop가 packet을 읽고 managed queue를 거친 뒤 session callback을 실행한다 |
| peer 식별 보존 | recv 결과의 [routing id](01-glossary.ko.md#routing-id)가 session dispatch까지 손실 없이 전달된다 |
| 등록 검증 | 같은 node에 session을 둘 이상 등록하면 startup에서 실패한다 |
| 오류 경계 | handshake·socket 오류가 session 오류 callback으로 올라오지 않는다 |
| 인증과 dispatch | connector와 session node 사이에서 인증과 packet dispatch가 완료된다 |
| 종료와 재개 | stream 종료로 pending request가 실패하고, 새 session의 인증·bind 뒤 messaging이 재개된다 |
| reply 상관관계 | `Response`·`Error` header에 packet name이 없고, client가 sequence 단독으로 매칭해 정상 완료한다 |
| Cross-node Actor 전달 | Physical STREAM은 session owner에 유지되고 command 38·24·36 record만 MeshNode 사이에서 전달된다 |
