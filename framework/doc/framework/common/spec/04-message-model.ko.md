---
title: "Framework 메시지 계약"
---

# Framework 메시지 계약

[스펙 목차](README.ko.md) · [이전: ZLink Framework 상호작용 모델](03-interaction-model.ko.md) · [다음: 비동기 실행과 handler turn](05-async-execution-policy.ko.md)

> **이 장이 정의하는 것** — typed 메시지, application metadata, 응답과 오류의 공개 계약.


이 문서는 ZLink Framework의 typed 메시지, application metadata, 응답과 오류 계약을 정의한다.
대상 독자는 framework 공개 계약과 언어별 service runtime을 구현하는 개발자다. Framework envelope와 내부
multipart encoding은 모든 언어가 같은 wire schema와 golden fixture를 사용하고, 각 언어 service runtime이
raw transport 위에서 처리한다. Application에는 이 형식을 노출하지 않는다.

## 1. Typed 메시지

application은 payload type과 등록된 handler를 기준으로 메시지를 보낸다. Framework는 기본 typed JSON
serializer를 사용해 payload를 encoding하고 수신 handler의 인자 type으로 decoding한다. 호출자는 메시지
type마다 codec, serializer registry, encoder 또는 decoder를 등록하지 않는다.

별도의 wire format이 필요한 package는 framework가 정의한 codec extension을 사용할 수 있다. extension은
host 단위 정책이며 업무 handler나 개별 send/request 호출에 반복해서 전달하지 않는다. raw bytes를 직접
다루는 API는 transport 검사와 codec extension 구현에만 사용한다.

## 2. 메시지 종류

| 종류 | 의미 | 완료 |
|---|---|---|
| Send | 대상 handler에 한 번 전달하는 one-way 메시지 | Source-local queue가 수락하면 반환 데이터 없이 완료하며 원격 handler 완료를 기다리지 않는다 |
| Request | 대상 handler가 reply 또는 오류를 반환하는 메시지 | reply, 오류, timeout 또는 cancellation로 한 번 완료된다 |
| Logical Multicast | target ChannelName의 각 MeshNode에서 조건에 맞는 Spot에 발행하는 메시지 | Source-local capacity를 확보해 publish를 시작하면 반환 데이터 없이 완료하며 target별 수를 monitoring에 집계하지 않는다 |
| Classic fanout publish | 독립 fanout channel의 subscriber에 발행하는 메시지 | Local publisher queue가 수락하면 반환 데이터 없이 완료하며 subscriber 수신은 확인하지 않는다 |
| STREAM send/request | 연결된 session에 보내는 one-way 메시지 또는 reply를 요구하는 메시지 | session sequence와 lifecycle 계약을 따른다 |

Request의 reply 상관관계는 transport가 발급한 operation ID 또는 [session sequence](01-glossary.ko.md#session-sequence)가 소유한다. packet name이나
application metadata를 reply matching key로 사용하지 않는다. reply는 성공 payload와 framework 오류 중 하나로
완료되며 같은 request를 두 번 완료할 수 없다.

Object creation request는 일반 Send·Request와 다른 manager operation 입력이다. Framework는 typed codec으로
encode한 최대 1 MiB payload의 immutable content reference와 hash를 placement reservation 전에 durable creation
intent에 기록한다. Factory는 logical key, ObjectGeneration과 creation attempt를 함께 받아 같은 attempt의
at-least-once 실행에도 같은 결과로 수렴해야 한다. CAS loser는 creation request를 일반 message로 보내지 않는다.
Ready commit 또는 fenced failure cleanup이 끝날 때까지 content reference를 유지한다. [ObjectGeneration](01-glossary.ko.md#objectgeneration),
AuthorityOwnerGeneration, attempt와 owner lease token은 Store fencing에만 사용하며 application message payload나
handler context에 포함하지 않는다.

### 2.1 MessageContext

일반 send·request와 Actor handler는 공통 `MessageContext`를 받는다. 이 context는 current message의
nullable MeshName, nullable ChannelName, PacketName, ContentType, immutable Metadata와 UTF-8 exact nullable
CorrelationId를 제공한다. CorrelationId는 send에서 null이고 request에서 non-null이다. MeshName은
RouteMesh와 Spot·Actor dispatch에서 non-null이며 ClientServer·STREAM에서는 null이다. Connection
cancellation은 universal context가 아니라 언어별 handler 인자나 Session 전용 context가 소유한다.
Node direct는 `RouteMessageContext`, Logical Multicast는
`PublishMessageContext`, STREAM dispatch는 `SessionMessageContext`로 각 경로의 추가 정보를 제공한다.

Send·Request·SpotActor별 marker context는 제공하지 않는다. Actor request의 context에 reply metadata·compression
option을 두지 않으며 별도 reply call도 만들지 않는다. Handler filter는 current `MessageContext` 정보와
공개 dispatch 종류를 함께 제공하는 filter 전용 context를 받는다. 이 값은 Node direct send/request, Channel
send/request와 classic fanout만 구분한다. Socket 종류, endpoint, Framework 내부 owner 분류와 dispatch
descriptor는 노출하지 않는다. Filter가 적용되지 않는 Spot·Actor·Logical Multicast·STREAM context에는
dispatch 종류를 추가하지 않는다. Object lifecycle Context와 현재 message의 MessageContext는 서로 다른
계약이다.

### 2.2 Global object reference JSON

`ActorRef`와 `SpotRef`의 typed JSON contract는 모든 언어에서 같은 property 이름과 JSON type을 사용한다. 모든
property는 required이고 property 이름은 case-sensitive다. 중복 property, `null`, unknown property와 범위를
벗어난 generation은 거부한다. Deserialization은 ID와 route string을 normalization하지 않는다.

```json
{
  "actorId": "player-42",
  "objectGeneration": "17",
  "meshName": "game",
  "nodeRid": "game-node-0123456789abcdef0123456789abcdef"
}
```

```json
{
  "spotId": "room-42",
  "objectGeneration": "9",
  "meshName": "game",
  "nodeRid": "game-node-0123456789abcdef0123456789abcdef"
}
```

`actorId`와 `spotId`는 global logical ID이고 `meshName`과 `nodeRid`는 조회 시점의 location snapshot이다.
`objectGeneration`은 `"1"`..`"9223372036854775807"`의 leading-zero 없는 decimal string이다. 숫자 token,
부호, 소수점과 exponent는 허용하지 않는다.

## 3. Application metadata

Application metadata는 업무 payload와 별도로 전달하는 작은 key-value snapshot이다. Node direct, [ChannelName](01-glossary.ko.md#channelname),
Spot direct, Actor와 STREAM send/request가 같은 계약을 사용한다.

| 항목 | 계약 |
|---|---|
| key와 value | UTF-8이며 NUL을 포함하지 않는다 |
| 전체 크기 | encoding된 key와 value 및 구조 overhead를 포함해 최대 1024 bytes다 |
| 같은 key | outbound builder에서 마지막으로 설정한 값이 적용된다 |
| 수신 | handler context가 변경할 수 없는 [snapshot](01-glossary.ko.md#snapshot)을 제공한다 |
| lifetime | handler turn이 끝날 때까지 유효하며 보관하려면 application이 복사한다 |
| malformed input | handler를 호출하지 않고 protocol 오류로 처리한다 |
| reply | request metadata를 자동으로 복사하지 않으며 일반 reply에 metadata setter를 제공하지 않는다 |

Metadata의 내부 frame 배치와 encoding은 공개 계약이 아니다. Framework는 payload와 metadata의 경계를
유지하고, relay가 필요한 경로에서도 application이 frame을 조립하거나 parsing하게 하지 않는다.

## 4. 전달 규칙

| 경로 | metadata 전달 |
|---|---|
| [Node direct](01-glossary.ko.md#node-direct)와 ChannelName | source snapshot을 선택된 [MeshNode](01-glossary.ko.md#meshnode)의 handler context에 전달한다 |
| [Spot](01-glossary.ko.md#spot) | global Spot ID의 current [Ready](01-glossary.ko.md#ready) owner에 있는 application claim에 전달한다 |
| [Logical Multicast](01-glossary.ko.md#logical-multicast) | 같은 publish snapshot을 각 matching Spot handler에 전달한다 |
| Actor | Actor handler context에 전달하며 Spot callback을 거치지 않는다 |
| STREAM session | session send/request context에 전달한다 |
| bound session에서 Actor로 relay | root metadata policy의 session-to-actor allowlist가 허용한 key만 전달한다 |
| Actor에서 bound session으로 relay | root metadata policy의 actor-to-session allowlist가 허용한 key만 전달한다 |

Framework가 새 request를 만드는 경우에는 원본 metadata를 자동 복사하지 않는다. 호출자가 현재 handler의
metadata를 명시적으로 넘긴 경우에만 새 outbound snapshot에 포함한다. 자동 전파가 필요한 trace 정보는
[메시지 흐름 상관관계](27-flow-correlation.ko.md)가 별도 framework field로 관리한다.

## 5. Ownership과 크기 제한

submit 호출이 반환되기 전까지 outbound builder와 payload는 호출자가 소유한다. Framework가 submit을
수락하면 필요한 payload와 metadata reference 또는 복사본을 operation lifetime 동안 유지한다. 호출자가
transport buffer, native message pointer 또는 multipart part의 lifetime을 관리하게 하지 않는다.

Handler에 전달된 message context, metadata와 payload view는 callback 동안 읽기 전용이다. Application이 이를
dispose하지 않으며 callback이 끝난 뒤 보관하려면 필요한 값을 복사한다. Framework가 callback completion과 함께
수신 payload storage, reply correlation과 route envelope의 lifecycle을 정리한다.

Object creation이 pending인 동안에도 같은 ownership 규칙이 적용된다. Location Store I/O와 [factory](01-glossary.ko.md#factory)가 caller의
payload object나 native buffer 수명에 의존하지 않도록 Framework service runtime이 immutable encoded payload를
content store에 고정한다. Ready 또는 fenced failure 뒤에는 해당 attempt가 소유한 payload storage를 한 번
해제한다.

payload 최대 크기는 대상 transport의 `MaxMessageSize`를 따른다. 전체 message가 제한을 넘으면 일부 part를
전달하지 않고 submit 또는 receive 전체가 실패한다. Logical Multicast의 target별 제출과 결과 집계는
[Spot 메시징](12-spot-messaging.ko.md)이 정의한다.
