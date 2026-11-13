기준 커밋: 1a6324724b3aa9aa52bf6679e0ee0ae4923c7090 (`git log -1 origin/main`); worktree HEAD: 213e579f52939d637ea39edd3bfc9d31e15752f6 (`git log -1`).

# Framework 서비스 wire·binding 계약 경계 설계 진단

## 1쪽 요약

**추천은 (다), binding의 native multipart에 Framework descriptor와 payload를 직접 전달하는 새 application wire다.** 일반 Node/Channel application record를 `[Framework descriptor, payload part…]`로 만든다. Descriptor가 service command·channel/message 식별·serializer 정보·Framework identity·flow·metadata를 한 번만 표현한다. Core/binding이 transport request matching, reply route/token, completion과 multipart 경계를 담당한다. Framework는 업무 대상 선택, typed dispatch, end-to-end identity와 deadline을 담당한다. 업무 payload의 기본 typed JSON serializer는 유지한다.

(가)의 **임의 request metadata 저장소는 현재 binding/Core 공통 계약에 없다.** ZMP request metadata는 Core의 kind와 내부 sequence이며 공개 수신 전에 제거된다. `Message.get_property()`는 진단용 조회다. 이것을 Framework header 저장소로 쓰려면 Core wire·공개 API와 여러 binding 계약까지 확장해야 한다. 비교군의 “201B JSON metadata”는 현재 계약의 자동 ZMP metadata로 설명할 수 없으며, benchmark source나 capture로 application part인지 확인해야 한다.

**사용자 결정이 필요한 사항**은 application wire 변경 허용, 구버전과의 동시 통신 필요 여부, 적용 범위와 version 정책이다. 추천 기본안은 다음 major의 RouteMesh application wire로 Node/Channel부터 전환하고, 호환되지 않는 peer는 admission에서 거부하는 것이다. Rolling upgrade가 필수라면 기존 handshake로 capability를 교환할 수 있는 bootstrap 계약을 먼저 설계해야 한다. ClientServer의 현재 JSON record와 Spot/Actor·relocation은 서로 다른 계약이므로 함께 전환할지 별도로 결정한다. Wire를 바꿀 수 없다면 (나)의 기존 JSON에서 허용된 optional field 생략만 검토한다.

| 효과 | 구분 | 결론 |
|---|---|---|
| 51.9k req/s, target 14.23 vs 7.19µs, source 13.78 vs 6.06µs | 사용자 제공 측정 | I/O CPU 차이는 각각 +7.04, +7.72µs/요청이다. 이 진단에서는 재측정하지 않았다. |
| Framework 요청 1,435B, 응답 1,443B; 비교군 각 1,252B | 사용자 제공 측정 | 각각 +183B(+14.6%), +191B(+15.3%)다. |
| 현 일반 request 13B / reply 21B, 자체 multipart 포장 75B | 소스·스펙으로 계산 | JSON header 296B와 payload 1,024B를 더하면 application part 합은 1,408B / 1,416B다. |
| 추천안 요청 약 1,089–1,119B, 응답 약 1,081–1,111B | 설계 추정 | 아래의 가상 이름 길이·빈 metadata·flow off·128-bit identity 보존 조건과 현재 ZMP 24B를 적용했다. 제공된 capture의 미확인 잔여 3B까지 유지하면 각 값에 +3B다. CPU 개선율은 추정하지 않는다. |
| Framework wire 해석 위치 4 → 1 | 제안 구조의 수 | 빈 metadata인 일반 record에서 service header, outer profile, packed part 경계, channel JSON 판정을 하나의 descriptor 해석으로 통합한다. Core decode·typed dispatch·payload JSON 해석은 남는다. |
| registry 1.19 + reply header 1.09 + multipart 0.45 = 2.73µs | 사용자 제공 측정 | 제거 가능한 총 CPU로 간주하지 않는다. Framework operation 의미와 새 descriptor 해석 비용이 남는다. |

현재 코드의 결함을 재현한 보고서가 아니라 **D: 새 wire·소유권 계약 결정이 필요한 설계 제안**이다. 32자리 correlation의 128-bit identity를 u64로 잘라서는 안 된다. 코드·저장소 문서는 변경하지 않았다. 빌드·실행·측정·test는 수행하지 않았다.

---

## 기준과 근거 표기

- Worktree는 detached 상태이고 조회 시 tracked/untracked 변경이 없었다. HEAD와 로컬 `origin/main`이 달랐다. 선택 질문에 답이 도착하지 않은 상태에서 명시된 “origin/main 고정”을 우선하여 위 `origin/main` 객체를 기준으로 읽었다. 원격 fetch는 하지 않았으므로 원격 서버의 최신 main임을 확인한 것은 아니다.
- 아래 `file:line`은 모두 기준 커밋의 위치다. HEAD의 파일과 섞지 않도록 Framework 파일은 `git show origin/main:<path>`로 읽었다. 비교한 Core·binding·공통 server spec·service schema에는 HEAD와 기준 커밋 사이 차이가 없었다.
- 네 runtime은 C++, .NET, JVM, Node.js다. Java/Kotlin은 JVM runtime과 codec을 공유한다. Kotlin 별도 wire 구현을 가정하지 않았다. Kotlin의 Java builder 사용 근거: `framework/languages/java/zlink-framework-kotlin/src/main/kotlin/systems/zlink/framework/kotlin/ZLinkRouteMeshExtensions.kt:3`.
- `scripts/dev/internal-docs.sh:8`의 pull, `:10`의 gh clone, `:15`의 저장소 밖 복사는 사용자 제한과 충돌하므로 실행하지 않았다. 공개 규칙과 저장소의 계약을 적용했다. Sub-agent·gh·dchat은 사용하지 않았다.

### 공통 계약 위치

이후 표의 약칭은 다음 파일을 뜻한다. 절 이름과 line을 함께 적는다.

| 약칭 | 파일 |
|---|---|
| W | `framework/doc/framework/common/spec/server/02-channel-transport/06-wire-protocol.ko.md` |
| S | `framework/runtime/protocol/service-wire-v1.schema.json` |
| CS | `framework/doc/framework/common/spec/server/02-channel-transport/03-client-server-channel.ko.md` |
| CM | `framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md` |
| F | `framework/doc/framework/common/spec/server/06-observability/04-flow-correlation.ko.md` |
| B | `bindings/doc/spec/README.ko.md` |
| BA | `bindings/doc/spec/async-execution-model.ko.md` |
| BO | `bindings/doc/spec/message-ownership.ko.md` |
| K | `core/doc/spec/core/socket/README.ko.md` |
| R | `core/doc/spec/core/socket/07-router.ko.md` |
| Z | `core/doc/spec/core/protocol/01-zmp.ko.md` |
| M | `core/doc/spec/core/02-message.ko.md` |

## 1. 현재 wire 구성과 필드 소유

RouteMesh의 일반 Node direct typed request를 application metadata가 없는 경우로 펼치면 다음과 같다. “자체 multipart”의 내부 part와 Core가 운반하는 실제 part를 구분해야 한다.

```text
Core request record
  native part 0: service prefix + service correlation                 13 B
  native part 1: application envelope + packed multipart
    application version, lengths, fixed packetName/contentType       63 B
    packed part count + 2 part lengths                               12 B
    inner part 0: channel JSON header                               296 B *
    inner part 1: application JSON payload                         1,024 B *
```

`*`는 제공된 측정의 길이다. 모든 message의 고정 길이가 아니다. Node request는 두 native part이며, inner header와 body가 다시 한 native part로 복사된다. Application metadata frame이 있으면 native part가 하나 늘어난다. 일반 reply의 service frame은 21B다. Channel request는 service header에 `text8 channelName`이 더 있어 `14 + UTF-8(channelName)`B다. 13B는 Node request와 맞는 구성이지 모든 Channel request의 길이가 아니다.

근거: W §2 `:115`, `:129`, `:186`; S `:7176`, `:7214`, `:7237`; Java 실제 frame 조립 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawMeshNode.java:5201`.

**ClientServer application record는 다른 형식이다.** `[JSON header, payload]` native 두 part를 이미 Core request/reply 위에 전달한다. Service command 18/19/20과 자체 multipart profile은 RouteMesh용이다. 모든 Framework 통신에 13B와 75B가 추가된다고 일반화하면 안 된다. 근거: W §4 `:353–359`.

### 필드별 계약과 의미

| 위치·필드 | 현재 운반 정보 | 소유 계약·근거 |
|---|---|---|
| Core transport routing identity | 현재 socket의 peer 선택·수신 source·reply 경로 | W §2 `:115`; Z §6 `:329`; R §9 `:226`. Service application frame에 다시 복사하지 않는 것이 현재 원칙이다. |
| Service `magic=ZM`, `major=1`, `command`, `flags` | Framework record 식별·operation 종류·metadata/tail 존재 | W §1 `:92`, §2 `:129–154`; S가 규범 원천이다(W `:49`). Core의 ZMP magic/version과 별개다. |
| Service `correlation:u64` | command request와 command 20 terminal의 연결 | S `:7184`, `:7222`, `:7243`; W §2 `:156`. Core 내부 sequence나 공개 reply token과 같다는 계약은 없다. |
| Service `channelName` | 수신 dispatch scope와 Channel 선택 의미 | S `:7203`, `:7227`; CM §2/3, §6 `:312`. Core RID와 다르다. |
| Service `metadata` flag와 별도 frame | Framework command에 붙는 선택 metadata bytes, 최대 1,024B | W §2 `:147`, `:166`. ZMP protocol metadata나 임의 Core message property가 아니다. |
| Reply `terminalResult:u32`, `failureCode:u32`, operation별 tail | Framework 성공·실패와 operation별 결과 | S `:7248–7258`; Java generic reply는 정확히 21B를 검사한다(`ZLinkJavaRawMeshNode.java:5245`). Core errno만으로 모든 Framework 실패를 표현하지 못한다. |
| Outer application `version`, `bodyLength`, `payloadLength` | Framework typed application envelope의 경계 | S의 `application-payload-envelope-v1`; Java `ZLinkServiceM6AWireCodec.java:394`; C++ `service_wire_codec.cpp:3145`. |
| Outer `packetName=ZLinkFrameworkMultipart`(23B), `contentType=application/x-zlink-multipart`(29B) | 실제 application 타입이 아니라 고정 multipart profile 식별 | W §2 `:188–192`; S `:1447–1462`. |
| Packed multipart `u32 count`, 각 `u32 length` | 원래 Framework part 순서·길이·opaque bytes | W §2 `:197–209`; S `:1457`. 두 part면 12B이며, outer envelope와 합쳐 75B다. |
| JSON `formatMarker=242` | channel envelope 문법 식별 | CS §5.3 `:417`. Core/ZM version을 대신하지 않는다. |
| JSON `kind` | request/response/command/publish/error | CS §5.3 `:418`. Core는 request/reply만 식별하며 typed application 오류·publish 의미는 모른다. |
| JSON `channelName`, `messageName` | handler namespace와 packet 식별 | CS §5.3 `:419`; CM §6 `:312`. Outer multipart packetName과 실제 messageName은 다른 값이다. |
| JSON `contentType` | typed payload serializer 선택 | CS §5.3 `:419`; W §6의 기본 typed JSON profile. 고정 outer contentType과 구분한다. |
| JSON `correlationId` | Framework request의 terminal identity·reply 연결·진단 context | F §2 `:34–55`, §3 `:61`; CS §7 `:464`. 1..64B opaque ASCII가 계약이며 “항상 u64”가 아니다. Java는 128-bit operation ID의 32자리 hex를 쓴다. |
| JSON `deadline` | Framework 요청의 남은 처리·전달 예산을 결정할 시간 정보 | CS §5.3 `:420`; Framework의 operation deadline과 Core의 admission 후 hop timeout은 동일하지 않다(K `:1242–1245`). |
| JSON `flowId`, `flowOrigin` | 여러 hop·branch의 같은 업무 흐름; UUIDv7 36 ASCII bytes와 origin enum | F §1 `:19–28`, §3 `:62–66`, §4 `:80–109`, §5 `:132`. Tracing off에서는 생성·검사·전파를 생략하며 correlation은 유지한다. |
| JSON `metadata` | 업무용 string→string metadata snapshot | CS §5.3 `:387–395`; CM §9 `:673–690`. 최대 1,024B이며 일반 reply에 요청 metadata를 자동 복사하지 않는다. |
| JSON `topic`, `source` | publish topic, 관측·상위 dispatch source 정보 | CS §5.3 `:420`; CM §7 `:454`, §6의 handler context `:374`. Ordinary request에서 null이라고 필드 의미 전체를 삭제하지 않는다. |
| JSON `errorCode`, `errorMessage` | Framework/application 오류 내용 | CS §5.3 `:420`; JSON kind `error`와 함께 해석한다. Core request terminal 결과만으로 대체하지 않는다. |
| Stateful command의 `OperationId`, route fence, reply route | deduplication, Message Follow·relocation의 원래 operation/terminal 전달 | W §11 `:764–775`; 단일 hop의 Core correlation을 재사용할 수 없는 정보다. |

Service metadata와 JSON metadata는 서로 다른 frame에 실릴 수 있다. **Java Node request에서는 같은 application metadata가 inner header와 outer metadata 인자로 모두 전달되는 경로가 확인된다.** `ZLinkChannelRouteCalls.java:463`은 `metadata.values()`로 inner parts를 만들고 `:489–494`는 `metadata.encode()`도 하위 request에 전달한다. `:556–565`가 envelope 생성 인자를 확인한다. `ZLinkJavaRawMeshNode.java:5207–5213`은 nonempty metadata일 때 별도 native frame을 만든다. 제공된 빈 metadata 측정의 추가 바이트 원인이라고 단정하지 않는다. 전환 설계에서는 application metadata의 wire 소유자를 하나로 정해야 한다.

### 네 runtime 구현 위치

모든 경로는 저장소 root 기준이다. 각 셀의 line이 해당 구조를 생성하거나 읽는 위치다.

| runtime | service header·reply | 자체 multipart·outer profile | channel JSON header |
|---|---|---|---|
| JVM(Java/Kotlin) | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java:213` Node u64, `:259` Channel, `:284` reply | 같은 파일 `:355` native buffer에 포장, `:394` outer fields, `:481` profile decode, `:514` count/length decode | `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:90` field 전부, `:438` JSON write, `:878` general read |
| .NET | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkServiceWireCodec.cs:206` 준비, `:242` service write, `:463` reply write | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Spots/ZLinkApplicationPayloadEnvelopeCodec.cs:153` native write, `:211` view decode, `:224` profile 검증 | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs:97` field 전부, `:379` header reader, `:834` JSON serializer, `:1136` 기본 header encode |
| C++ | `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:480` Node, `:518` Channel, `:3791` reply | 같은 파일 `:3113` count/length·outer profile write, `:3201` profile read, `:3308` parts decode | `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.hpp:138` field 전부; `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:357` static fields, `:382` dynamic fields, `:1091` decode |
| Node.js | `framework/languages/node/packages/framework/src/runtime/foundation/service-wire-m6a-codec.ts:78` Node, `:110` Channel, `:143` reply | 같은 파일 `:216` outer+multipart write, `:243` length 계산, `:256` parts write, `:325` outer read | `framework/languages/node/packages/framework/src/runtime/channels/channel-envelope.ts:53` field 전부, `:560` encode, `:745` fields write, `:392` envelope decode |

Java correlation의 원래 값: `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceOperationIds.java:17`은 process prefix와 counter로 128-bit UUID를 만들고, `:30–33`이 두 u64를 모두 hex로 표현한다. `ZLinkChannelEnvelope.java:169`가 이 값을 header에 넣는다. W §11 `:767–768`도 durable OperationId를 한 word로 줄이지 못하게 한다.

Optional JSON 표현은 byte 단위로 같지 않다. Java writer는 `flowId:null`, `flowOrigin:null`, `metadata:{}`를 쓸 수 있다. .NET은 null metadata를 생략한다(`ZLinkEnvelopeCodec.cs:117–123`). Node는 flow가 없으면 두 field를 생략한다(`channel-envelope.ts:774–781`). 이는 현재의 optional 값 허용 범위 안에서 읽는 방식의 차이다. 296B를 모든 언어에 고정하여 계산하지 않는다.

## 2. Binding/Core로 대체 가능한 정보

### 기존 계약의 제공 범위

| 항목 | 이미 제공하는 수단 | 대체 여부와 경계 | 근거 |
|---|---|---|---|
| 물리 peer·source routing | ROUTER target RID, 수신 `Received` source와 reply context | 사용 가능. Framework의 논리 Channel/Actor/Spot 선택은 남긴다. | K `:1188–1200`; B `:879–900`; Z §6 `:329` |
| 한 transport hop의 request/reply matching | Core kind + 내부 u64 sequence, completion ID/context, binding reply stage | 사용 가능. Framework가 sequence를 해석·복제하지 않고 해당 request의 completion 결과를 받는다. | Z §3 `:84–100`, §5 `:275–303`; K `:1211–1219`; BA §4/5 `:61`, `:102` |
| 수신 request에 대한 reply 경로 | `Received.reply()` 또는 ROUTER source RID + opaque reply token | 사용 가능. Token을 wire에 싣거나 operation ID로 쓰지 않는다. Token은 responder-local capability다. | B `:1000–1006`; R §9 `:226–233`; K `:1256–1275` |
| native multipart 경계 | builder `.message(...)` 반복, whole-message arrays; 수신 parts | packed count/length와 outer fixed profile을 제거할 기반이 된다. 여러 독립 record를 batch하는 권한은 아니다. | B `:436–462`, `:961–973`; M §4/수신; BO `:201`, `:401–419` |
| 임의 Framework metadata/header | application part로 opaque bytes 운반 | 가능. **전용 request metadata API는 없다.** 첫 ordinary part에 넣는 방식은 (다)다. | B `:449–462`, `:998`; M `:27–30` |
| `Message` 속성 | `get_property(name)`의 native metadata 조회 | 전송용 저장소로 사용 불가. 공통 쓰기 계약도 없고 per-message 임의 값을 wire에 보존하는 계약도 없다. | B `:1705`, `:1745–1748`; M `:27–30` |
| ZMP metadata 옵션·handshake metadata | READY peer metadata와 Core protocol metadata | request별 header로 사용 불가. Connection metadata와 per-operation 정보는 수명이 다르다. | Z §4 `:123`, §10 `:518–522`; B `:449–462` |
| `user_context` | native completion에 되돌리는 local pointer | local operation 객체를 completion에 연결하는 데 사용 가능. 원격 전파 불가이며 binding이 수명을 소유한다. | K `:1195`, `:1214–1217`; BA §5 `:104` |
| transport timeout·late reply 정리 | Core request admission 이후 timeout 및 completion | binding에 맡길 수 있다. Framework 전체 deadline, forwarding·durable terminal 수명까지 없애지는 않는다. | K `:1242–1254`; BA §6 `:129`; F §5 `:132` |
| message storage·views | native payload 소유 `Message`, read view, explicit move | 자체 payload 재포장의 복사를 줄일 수 있다. 공개 borrowed constructor나 무제한 zero-copy 보장은 없다. | BO `:206–245`, `:363–384`; B `:1732–1748` |

`bindings/doc/spec/message-ownership.ko.md:413–419`에는 Core 0.10.1 `*_part` adapter 설명이 남아 있고, `bindings/doc/spec/README.ko.md:436–462`는 whole-message ABI를 요구한다. 두 문서의 native substrate 서술은 일치하지 않는다. 현재 Java의 배열 기반 호출은 `bindings/java/src/main/java/systems/zlink/runtime/nativeapi/Native.java:744–764`로 확인했다. 이 진단에서는 실제 whole-message ABI와 README의 규정을 기준으로 하며, 별도 계약 정리가 필요한 문서 불일치로 기록한다. API 확장 근거로 오래된 part 설명을 쓰지 않는다.

### Framework 필드의 이관 판정

| Framework 필드 | Core/binding 자체 정보로 제거 가능 여부 | 추천 배치 |
|---|---|---|
| service u64 hop correlation | 일반 Node/Channel의 transport matching 목적은 제거 가능. 현재 Framework wire의 echo/검증 계약 개정 필요 | binding reply stage·reply context 사용 |
| JSON correlation / 128-bit OperationId | 일괄 제거·u64 축소 불가. Framework end-to-end identity, 진단, durable relay는 보존 | descriptor에 필요한 원래 identity를 무손실 표현. 생성된 32자리 hex는 16B로 표현 가능 |
| `flowId`, `flowOrigin` | Core가 모르는 Framework 흐름이므로 자동 대체 불가 | descriptor의 선택 field. UUIDv7는 16B + origin 1B로 무손실 표현 가능. Off의 생략 규칙 유지 |
| application metadata | Core protocol metadata로 대체 불가 | descriptor에 한 번만 표현. 문자열·1,024B 제한·snapshot 의미·reply 비복사 유지 |
| `channelName`, 실제 packet/message name | RID로 대체 불가. 같은 peer에 handler namespace·타입이 여럿일 수 있음 | descriptor에 한 번 표현. ClientServer에서 connection에 고정된 channel은 계약 변경 후 생략 검토 |
| inner `contentType` | Core가 opaque payload의 codec을 알지 못함 | 기본 JSON을 schema enum/default로 표현하고 custom serializer 식별은 보존 |
| outer 고정 packetName/contentType | native multipart를 쓰면 의미를 profile/version으로 통합 가능 | 문자열 전송 삭제. 실제 application packet name은 유지 |
| outer lengths·packed count/length | Core의 native part 경계와 중복되는 저장 형식 | native parts 사용. Descriptor와 payload part 개수의 유효성은 새 schema에서 검사 |
| `kind`, `formatMarker`, service command | 일부 transport kind는 중복이나 Framework command/error 의미는 필요 | 하나의 Framework discriminator/version으로 통합 |
| deadline | Core의 hop timeout 인자만으로 전체 의미 대체 불가 | end-to-end 의미가 필요한 경우 descriptor에 보존. 시간 단위·정밀도·범위는 새 계약에서 결정 |
| terminalResult/failureCode, errorCode/message | Framework/application 실패는 Core errno에 모두 대응하지 않음 | 하나의 Framework terminal 표현으로 통합. 성공 시 기본값 생략 가능 |
| topic/source·route fence·bound-session·reply relay | 일부 source는 `Received`로 복구 가능. 나머지 Framework 의미는 그대로 필요 | command별로 필요한 정보만 descriptor/control record에 보존. Core 속성으로 추정하지 않음 |

### 현재 binding 사용과 결정 소유 표

이미 Framework는 binding request/reply를 쓰고 있다. 추천은 “request API를 처음 도입”하는 것이 아니라, 그 위에서 다시 운반·검증하는 정보를 줄이는 것이다.

| 결정 | 현재 판정·전달 위치 | 단일 소유자와 제안 |
|---|---|---|
| peer 선택·수신 source·reply routing | JVM `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/binding/ZLinkJavaRawServicePort.java:444–450`; .NET `framework/languages/dotnet/src/Zlink.Framework/Runtime/Service/ZLinkManagedMeshNode.cs:7371–7391`; C++ `framework/languages/cpp/framework/src/runtime/mesh/raw_mesh_node_owner.cpp:1324–1366`; Node `framework/languages/node/packages/framework/src/runtime/foundation/raw-service-mesh-runtime.ts:1872–1905` | 물리 route·reply는 Core/binding(K §Request와 reply). 논리 Channel 선택은 Framework(CM §2/3). 경계를 유지한다. |
| Framework service correlation echo | JVM `ZLinkServiceM6AWireCodec.java:213`, `ZLinkJavaRawMeshNode.java:5249–5253`; .NET `ZLinkServiceWireCodec.cs:258`, `:469`; C++ `service_wire_codec.cpp:488`, Node `service-wire-m6a-codec.ts:156–159` | 일반 application hop matching은 Core completion 하나. Framework operation identity는 다른 목적으로 보존한다. 위 파일들의 전체 경로는 구현 표에 있다. |
| native part 밖의 자체 multipart 저장·검증 | JVM `ZLinkServiceM6AWireCodec.java:355`, `:514`; .NET `ZLinkApplicationPayloadEnvelopeCodec.cs:153`, `:224`; C++ `service_wire_codec.cpp:3123–3159`; Node `service-wire-m6a-codec.ts:243–269` | Core/binding native part 경계. Framework는 descriptor·payload 계약만 검사한다. |
| typed handler·serializer 결정 | 네 runtime의 channel JSON codec 위치는 구현 표 | Framework registry·기본 typed JSON serializer를 그대로 사용한다. Core에 packet registry를 추가하지 않는다. |
| Framework identity·flow·metadata | 네 runtime의 header field 선언과 encode 위치는 구현 표; Java `ZLinkServiceOperationIds.java:17–33` | Framework descriptor 한 곳. F §2/3, CM §9 의미를 유지한다. |
| completion drain·DONTWAIT 재제출 | binding BA §4/5, B `:1166`; Framework는 위 public request 결과를 관찰 | Core/binding 소유. 새 Framework poller·retry queue·completion matching table을 만들지 않는다. |

진단 분류는 **D(새 application wire와 matching 소유 계약)**다. 현재 스펙이 명시한 포장을 삭제하는 것을 B(구현 버그 수정)라고 주장하지 않는다. 기존 공통 API 재사용은 설계 방향이며, 새 wire를 현 계약에 무단 적용하는 것은 허용되지 않는다.

## 3. 대안 비교와 바이트 추정

### 계산 경계

제공된 수치는 다음처럼 분해된다.

- 요청: `1,435 = 13 + 75 + 296 + 1,024 + 27`.
- 응답: `1,443 = 21 + 75 + 296 + 1,024 + 27`.
- 비교군: `1,252 = 201 + 1,024 + 27`.
- 고정 outer profile은 `1(version)+4(bodyLength)+1+23(packetName)+1+29(contentType)+4(payloadLength)=63B`다. Packed 두 part의 count/length 12B를 더해 75B다.
- **현재 ZMP spec만 계산하면** native 두 part의 공통 header `8×2`와 request/reply sequence `8`은 24B다(Z §3/5). 위 잔여 27B와 3B가 다르다. 이 3B를 TCP/IP header나 routing identity라고 단정하지 않는다. Capture 집계 단위·사용 Core 버전·payload 실제 길이·추가 framing을 kairos가 확인해야 한다.

아래 숫자는 우선 현재 ZMP 24B를 포함한다. 제공된 capture와 같은 미확인 잔여 3B가 계속 있으면 3B를 더한다. TCP/IP/Ethernet/TLS 및 connection handshake·control 비용은 별도다.

설계한 compact descriptor의 **예시**는 `version:u8 + operation:u8 + presence:u16`, `text8 channel`, `text8 messageName`, 기본 JSON `codec:u8`, 원래 128-bit identity 16B, request deadline 8B다. 빈 metadata·flow off이면 request `H=31+C+N`, response `H=23+C+N`B다. `C=0..8`, `N=10..32`라는 가상 이름 길이를 적용하여 `Hreq=41..71`, `Hresp=33..63`으로 계산했다. 실제 benchmark 이름과 deadline 필드 사용 여부는 제공되지 않았다. 모든 이름을 code table로 바꾸거나 registry 협상 상태를 추가한 추정이 아니다.

Flow on은 UUID 16B+origin 1B로 +17B를 예상한다. Metadata를 현재 의미의 canonical JSON bytes로 보존하면 길이용 u16과 내용 `A`B가 추가된다. 예시의 8B deadline은 단위·정밀도가 확정되어야 하며 기존 값을 잘라 저장하자는 제안이 아니다. Custom content type, 긴 이름, error text·route fence가 있으면 그 길이를 별도로 더한다.

| 대안 | 바이트 추정(요청 / 응답) | Framework 구조 해석 단계 | 네 runtime·계약·호환성·위험 |
|---|---|---|---|
| 현 구조 | 소스 계산 1,432 / 1,440B; 제공 측정 1,435 / 1,443B | 4: service → outer profile → packed part 경계 → channel JSON | 네 runtime 공통 구조. Core decode와 payload JSON 해석은 별도다. |
| (가) 전용 binding/Core metadata에 전부 이관 | **현재 API로는 구현 불가.** 가상 새 extension에 descriptor H를 싣고 payload가 한 part이면 `1,024+H+16+E`: 약 1,081–1,111 / 1,073–1,103B + extension 자체 비용 E | Framework metadata decode 1회; Core extension decode가 추가됨 | Core ZMP/header·message/socket API와 공통 binding, 각 언어 공개 계약까지 변경. E 형식이 없으므로 수치는 가정이다. Core가 Framework policy를 알지 않는 opaque extension이어도 새 범용 기능·소유권·버전 비용을 부담한다. |
| (나-1) 같은 JSON wire의 허용된 optional 값 생략 | Java의 빈 `metadata:{}` 14B, null flow fields 32B를 모두 생략할 수 있다면 1,386 / 1,394B(측정 기준 1,389 / 1,397B) | 4 유지 | 구버전이 missing=empty/absent를 이미 허용하는 field에 한정. 네 decoder의 missing-field 판정을 정적으로·contract test로 확인해야 한다. Marker/kind/실제 이름·correlation을 무조건 생략하면 안 된다. .NET/Node는 일부를 이미 생략한다. |
| (나-2) 현재 record 계층을 유지하고 inner header만 binary H로 변경 | `현재값−296+H`: 소스 기준 약 1,177–1,207 / 1,177–1,207B(측정 잔여 포함 각 1,180–1,210B) | 4 유지, 마지막 JSON parse를 binary로 교체 | **binary는 기존 JSON decoder와 wire 호환되지 않는다.** 새 profile/version 필요. 네 codec 변경·공통 golden 필요. 자체 multipart 복사와 두 header의 의미 중복이 남는다. |
| (다-1) binding native multipart + 현재 JSON header | `1,024+296+24 = 1,344B` 양방향(측정 잔여 포함 1,347B) | 1: channel JSON | RouteMesh service application header와 75B 포장을 제거. ClientServer는 이미 이 형태에 가깝다. 새 RouteMesh 계약·command 분류/terminal 표현 필요. Payload JSON과 기존 header 해석 비용은 유지한다. |
| **(다-2) native multipart + 단일 compact descriptor(추천)** | `1,024+H+24`: 약 **1,089–1,119 / 1,081–1,111B**(측정 잔여 포함 1,092–1,122 / 1,084–1,114B) | **1: descriptor** | Core/binding API 변경 없이 네 Framework runtime의 application wire·serializer 연결·receive ownership을 변경. 기본 payload serializer는 유지. 새 major/capability와 admission 범위 결정 필요. |

단계 수는 빈 metadata인 일반 Node/Channel의 Framework 구조 판정 위치를 센 값이다. Java에서는 service header decoder, `ZLinkServiceM6AWireCodec.java:481`의 outer profile 검사, `:507`의 packed count/length 검사, `ZLinkChannelEnvelope.java:877`의 JSON reader가 해당한다. 함수 호출 수·전체 byte 순회 횟수·mesh thread에서 수행되는 단계 수와는 다르다. Metadata 자체가 JSON이면 nonempty metadata의 해석 비용을 별도로 더해야 한다.

(가)의 “binding metadata”가 사실 ordinary first application part를 의미한다면, 그 구현은 (다)와 같다. 이름만 metadata로 바꿔도 protocol header 처리 비용이 없어지는 것은 아니다.

(나)의 hex→u64는 값을 구분해야 한다. Service correlation은 이미 8B binary다. 현재 Java의 32자리 hex는 128-bit이며 16B로 무손실 표현할 수 있다. 임의 1..64B ASCII correlation의 축소, process prefix 삭제, OperationId 한 word 생략은 별도 identity 계약 변경이다. 무손실 압축으로 취급하지 않는다.

(다-1)과 (다-2)는 대안 비교용이다. 추천 구현에 두 wire를 message마다 추측해서 decode하는 fallback을 추가하지 않는다. Message마다 동적 dictionary/packet-ID 협상이나 새 cache를 도입하지 않으며, schema가 descriptor와 codec enum을 한 곳에서 정의하고 생성한다.

### 예상 효과의 한계

추천안은 제공된 Framework 메시지에 대해 대략 요청 22–24%, 응답 23–25%의 바이트 감소를 예상한다. 가상 H와 미확인 3B 조건을 적용한 범위이며 실제 byte 목표가 아니다. 비교군의 201B JSON보다 H가 작으면 binding 비교군보다도 작아질 수 있지만, 비교군 metadata의 의미·flow·deadline·name이 같은지 먼저 확인해야 한다.

Core I/O CPU를 절반으로 줄인다는 결론은 없다. 바이트 감소는 TCP 작업량의 한 요인이며, write 묶음·read 경계·HWM 흐름은 비선형이다. 제공된 mesh 비용 중 header 1.09와 multipart 0.45µs의 작업은 제거 대상으로 볼 수 있으나, 대체 descriptor decode 비용과 남는 Framework 검증을 빼기 전의 수치다. Registry 1.19µs는 어떤 registry인지 call stack이 없으므로 제거 대상으로 확정할 수 없다.

## 4. TCP CPU 증가의 소스상 가설과 kairos 확인 항목

요청·응답 바이트가 약 15% 늘어난 데 비해 target CPU는 약 1.98배, source는 약 2.27배다. 호출·깨우기가 같거나 적다는 측정은 “호출이 많아서 느리다”는 설명을 지지하지 않는다. 같은 호출 수라도 buffer 크기, partial completion, copy, packet 처리량과 cache 비용이 다를 수 있다. 각 항목은 확인 전 가설이다.

| 우선순위·가설 | 소스 근거·현재 판단 | kairos가 확인할 측정·반증 조건 |
|---|---|---|
| 높음: record 크기와 write batch 충전률 | ROUTER output batch는 16KiB(`core/src/runtime/sockets/router/router.cpp:22`, `:46`). 기본 input batch는 8KiB(`core/src/runtime/core/options.cpp:76`, `:129`). Engine은 대기 중인 frame을 target batch까지 채운다(`core/src/runtime/engine/asio/asio_engine.cpp:1348–1364`, `:1440–1457`). | 요청/응답 connection별 실제 bytes/write와 bytes/read의 분포, records/batch, partial write, sync/async 완료 비율. 같은 req/s여도 Framework 도착 간격이 batch를 덜 채우는지 비교. 16KiB에 단순 계산하면 약 13개 vs 11개 record로, 이것만으로 2배는 설명되지 않는다. |
| 높음: native frame 크기 분포·receive zero-copy 성공률 | Framework는 두 native part가 약 13B/1,395B이며 비교군을 metadata/body 두 part라 가정하면 201B/1,024B다. 수신 decoder는 payload 전체가 allocator buffer의 남은 영역 안에 들어오면 공유 view를 만들고, 그렇지 않으면 새 message storage를 할당한다(`core/src/runtime/protocol/zmp_decoder.cpp:275–295`). | frame 크기별 allocator view 성공/실패, 새 allocation과 copy bytes, read buffer 잔여 길이, split-frame 비율. Binding 비교군 실제 part 수·크기를 먼저 확인. view 성공률이 같고 copy도 같으면 이 가설의 설명력은 낮다. |
| 높음: payload 배치가 send copy·buffer 경계에 미치는 영향 | 일반 encoder는 buffer로 memcpy하며, 특정 큰 chunk는 pointer를 직접 반환한다(`core/src/runtime/protocol/encoder.hpp:73–95`). Framework는 inner header/body를 native 한 frame에 미리 포장한다(`ZLinkServiceM6AWireCodec.java:355–379`). | I/O stack의 memcpy/allocator cycles와 kernel TCP cycles를 분리. 포장 복사는 Framework thread 비용이므로 I/O CPU 증가에 그대로 합산하지 않음. 실제 native library·commit·옵션을 양쪽에서 확인. |
| 중간: Nagle·TCP_NODELAY·socket buffer 차이 | Core 기본 `tcp_nodelay=1`(`core/src/runtime/core/options.cpp:116`), TCP tuning은 실제 setsockopt 호출(`core/src/runtime/transports/tcp/tcp.cpp:29–35`; `core/src/runtime/transports/asio/asio_tcp_tuning.hpp:21`). Java binding perf는 TCP_NODELAY를 명시 설정한다(`bindings/java/perf/common/src/main/java/systems/zlink/perf/PerfTransport.java:80–90`). Framework 경로에서 반대값을 설정한다는 증거는 찾지 못했다. | 양쪽 실제 accepted/outbound socket의 TCP_NODELAY, SO_SNDBUF/RCVBUF, MSS, RTT, delayed ACK와 retransmit, syscall flags. 기본값이나 comment만으로 옵션 차이를 단정하지 않음. |
| 중간: TCP segmentation·GRO/GSO·SKB 경계 | ZMP는 stream read 경계와 무관하게 header/payload를 조립한다(Z §8 `:375`). TCP packet 처리 비용은 byte 수와 선형 관계가 아닐 수 있다. 두 record가 보통 MTU의 payload 한계보다 작다는 사실만으로 packet 수가 같다고 할 수 없다. | 실제 MSS/MTU/loopback 여부, segments/req, SKB/GRO 크기와 offload 설정, ACK 비율. 단일 record가 1,435B라는 이유만으로 MSS 초과를 주장하지 않음. |
| 중간: Core request-reply lane·HWM control traffic 차이 | ROUTER-ROUTER reply는 Completion lane이고 DEALER-ROUTER는 Application lane이다(Z §4.1 `:176`, §5 `:283`). Application 호출 수와 제어 frame·두 connection의 작업량은 다르다. | 비교군 peer type과 physical Application/Completion connection 수, lane별 bytes·request/reply/control frame 수, PAUSED/RUNNING·HWM 상태, reply window occupancy. 동일 topology·pacing이면 그때 이 차이를 제외. |
| 낮음(기본값에서는): gather/zero-copy threshold 분기 | Gather는 `ZLINK_ASIO_GATHER_WRITE`로 켜며 기본 threshold는 65,536B(`core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp:14–26`). `asio_engine.cpp:793`은 body 크기로 분기한다. 1KiB와 1.4KiB는 기본 threshold 아래다. | 실제 env/options와 gather 경로 횟수 확인. Threshold가 기본이면 “1KiB에서 gather 경로가 달라졌다”는 설명은 배제한다. Custom threshold가 두 frame 크기 사이면 별도 비교. |
| 공통: sampling·thread CPU 집계의 경계 | 제공 측정의 원본 stack/집계 방법은 이 저장소에 없다. “TCP send/receive”가 kernel symbol인지 상위 transport subtree인지에 따라 copy·poll·allocator 포함 범위가 달라진다. | 같은 구간·51.9k 완료 req/s·warmup·CPU affinity·주파수로 user/system cycles와 instructions, cache misses, cycles/byte·cycles/segment, thread별 stack을 비교. Control/idle CPU 포함 여부와 계수 중복을 명시. |

우선 확보할 입력은 비교 benchmark의 source/commit, 실제 loaded Core·binding package, request/response의 native part 목록과 길이, TCP socket 옵션, wire capture의 27B 잔여 분해다. 이후 같은 payload 의미로 “크기만 맞춘 비교”와 “part 배치만 맞춘 비교”를 분리하면 byte 증가와 framing 증가의 효과를 구분할 수 있다. 이는 kairos의 후속 측정 제안이며 이 진단에서 실행하지 않았다.

## 5. 추천안의 소유 경계·호환성·승인 항목

### 추천안의 범위

다음 major의 RouteMesh Node/Channel application record를 native `[descriptor, payload…]`로 정한다. Descriptor는 현재 서비스 header·inner envelope에 필요한 정보를 한 번만 표현한다. 고정 multipart packetName/contentType과 packed length table은 전송하지 않는다. 실제 application packet name·custom codec 식별·metadata·flow와 오류 의미는 유지한다. Handler와 sample의 public API에 새 codec 조립 책임을 추가하지 않는다.

Framework는 Node/Channel/Actor/Spot의 논리 대상 선택과 operation 정책을 담당한다. Transport request/reply는 기존 binding public operation을 통해 제출하고 원래 reply context로 회신한다. One-way에 correlation을 새로 만들지 않는다. Native completion ID·token은 원격 identity로 전파하지 않는다. Stateful relay·durable operation의 128-bit identity, 원래 deadline, route fence·reply route는 Framework 계약대로 보존한다.

Framework operation registry 전체를 삭제하는 제안은 아니다. 일반 hop matching에 필요한 별도 wire echo를 없애고 binding의 원래 request completion으로 결과를 돌려준다. Durable terminal delivery와 Framework end-to-end 의미를 위한 registry는 해당 owner 안에 남긴다. Core error completion과 application error descriptor도 구분한다.

Native multipart는 payload가 여러 part일 때 Core header와 wrapper 수가 늘 수 있다. 현재는 packed payload 전체가 native 한 part다. 일반 typed payload 한 part는 descriptor와 합쳐 native 두 part로 유지되지만, payload가 n개이면 새 record는 n+1개다. 이 경우 ZMP overhead는 `8×(n+1)+8`B이고, old packed count/length 절감과 비교해야 한다. Native view와 explicit move가 있어도 Node의 managed Buffer materialization이나 builder의 원본 보존 복사는 남을 수 있으므로 “전 구간 zero-copy”를 목표 수치에 넣지 않는다.

### Version과 혼합 배포

현재 규범은 wire major 1과 required capability `framework-service-v13`이다(W §1 `:92`). Capability는 admitted connection 중 변경할 수 없다(W §4 `:343`). 따라서 새 descriptor를 기존 v1 command ID의 payload로 조용히 넣거나, JSON parse가 실패하면 binary로 재시도해서는 안 된다.

| 선택 | 사용자 결정 | 설계 결과 |
|---|---|---|
| 추천 기본값: 새 major, 동시 업그레이드 | 혼합 버전 peer가 일시적으로 통신하지 못해도 되는가 | 새 capability를 지정하고 admission에서 불일치를 거부한다. 연결별로 wire가 고정되며 runtime별 message fallback이 없다. |
| 구·신 버전 동시 통신/rolling upgrade | 지원해야 하는 이전 버전과 기간은 무엇인가 | 구 peer가 읽을 수 있는 bootstrap admission과 connection 단위 profile 선택을 정의해야 한다. Advertise만 추가해도 old required-capability 판정에서 거절될 수 있으므로 현재 admission/schema를 함께 개정한다. |
| RouteMesh 일반 application 우선 | Node/Channel만 이번 wire 변경 대상으로 할 것인가 | Spot/Actor·relocation command는 보존한다. 같은 connection에서 control/application을 구분하는 새 framing 계약을 schema에 명시한다. |
| ClientServer·수동 client 포함 | 기존 JSON wire를 사용하는 client도 업그레이드 가능한가 | CS §5.3과 application admission profile을 함께 개정한다. 기존 arbitrary ASCII correlation을 지원할지, v2의 runtime identity 표현으로 제한할지도 결정한다. |
| 외부 wire 형식 | Binary descriptor, JSON default 생략, 또는 현 형식 유지 중 무엇을 허용하는가 | Binary는 byte·parse 감소가 크고 계약 변경 범위도 크다. 호환 유지가 필수면 검증된 optional field 생략으로 범위를 제한한다. |

**기술적으로 사전 승인 없이 선택할 수 없는 부분**은 새 wire major/capability 이름, 구버전 지원 정책, ClientServer까지의 적용 범위, correlation/operation identity 표현과 deadline 정밀도다. 설계의 기본 방향과 이 결정들을 분리한다.

### 스펙 변경 범위

| 범위 | 추천 (다)에 필요한 변경 | (가)의 추가 변경 |
|---|---|---|
| `framework/runtime/protocol/service-wire-v1.schema.json`과 차기 schema | application record/descriptor/terminal, flags·bound, 새 major/capability와 bootstrap. 기존 이름의 schema를 무단 재해석하지 않음 | 동일 |
| W §1/2/3/4, CS §5.3(포함 시) | service/application framing, native parts, admission에서 형식 결정 | 동일 |
| F §2/3/5, W §11 | hop matching과 Framework identity 소유를 분리. 원래 operation·flow 의미와 durable 두 u64는 보존 | Core metadata에 넣더라도 의미 소유는 Framework로 유지 |
| CM §6/9, payload ownership·error model | dispatch scope·metadata 단일 표현·terminal·part 소유권을 새 형식과 대조. 공개 의미가 같으면 불필요한 재서술 없이 소유 절만 개정 | 동일 |
| 생성기·golden·네 runtime codecs | 새 descriptor를 단일 schema에서 생성하고 같은 bytes와 failure를 검증 | 동일 |
| B·BA·BO, Core M/K/Z | **새 API·wire 확장 불필요.** BO의 오래된 part-substrate 서술 불일치는 별도 계약 정리로 제안 | 임의 per-request metadata storage, propagation, ownership, new API·ABI, ZMP extension negotiation을 새로 정의. 네 Framework runtime 밖의 binding에도 영향 |

### 후속 검증의 완료 조건

후속 구현을 승인하면 네 runtime의 cross-language golden을 먼저 고정한다. 정상 request/reply·one-way, metadata의 empty/Unicode/1,024B 경계, flow off/on, custom codec, application error, out-of-order reply, late reply, cancellation과 native view 소유권을 같은 계약으로 확인해야 한다. Kotlin은 JVM codec 결과와 coroutine 호출 경계를 확인한다.

검사는 실패 입력도 포함해야 한다. 잘못된 version/capability, 누락·과잉 part, 잘린 descriptor·문자열, 잘못된 enum/identity, 과한 metadata, 잘못된 flow, request kind와 native request context 불일치, ClientServer/RouteMesh 방향 혼용을 reject하는지 확인한다. 호환 정책을 채택하면 old/new 조합의 성공과 거부 결과를 함께 고정한다. 성능은 동일 의미·동일 topology·동일 pacing에서 bytes/part, allocations/copies, mesh CPU와 Core I/O CPU를 각각 비교한다.

**정적 점검 결과:** application wire 중복 표현·해석 구조를 확인했고, Java nonempty metadata의 중복 전달 경로와 binding 문서의 substrate 불일치를 기록했다. 수정 0건, 런타임 결함 확정 0건이다. 설계 제안과 미확인 성능 가설은 이 산출물에 남겼으며 새 Issue·PR을 만들지 않았다.

**규칙 수(제안, 빈 metadata인 일반 Node/Channel application wire의 형식 판정 위치):** Framework 4(service / outer application profile / packed part 경계 / channel JSON) → 1(descriptor); Core의 transport matching owner는 1 → 1로 유지한다. End-to-end identity와 durable terminal owner는 삭제 대상이 아니다. 새 상태·queue·lock·poller를 추가하지 않는 방향이다.

**소유 계층:** Core/binding은 transport route·request matching·multipart·reply token·completion; Framework는 논리 대상·typed dispatch·end-to-end identity·flow·metadata·operation deadline.

**스펙 조항:** B request-reply metadata 경계·BA §4/5·K Request와 reply·Z §3/5, W §1/2/4/11·CS §5.3·F §2/3/4/5·CM §6/9.

**교차언어 대조:** C++/.NET/JVM/Node의 현 service·outer multipart·JSON envelope 구조를 읽어 대조했다. Optional JSON 출력 차이를 확인했다. 실행 기반 parity 검증은 수행하지 않았다.

**변경 분류:** D(새 application wire·identity/matching 경계의 계약 결정). 코드와 정식 스펙을 변경하지 않은 읽기 전용 진단이며, 구현 전에 사용자 wire·호환성 결정과 감독자의 스펙 승인이 필요하다.
