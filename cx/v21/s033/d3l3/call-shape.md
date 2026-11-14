# .NET L3 호출 경로

Production 기준 `6350275fac`, bench 기준 `29d9fca82b`. `D=framework/languages/dotnet/src/Zlink.Framework`, `B=framework/bench/grpc/dotnet`, `Bind=bindings/dotnet/src/Zlink`. Production 파일 변경 없음. Scratch에서는 기존 Framework test friend assembly 이름으로 내부 codec에 접근하며 binding public API만 사용한다.

| 경계 | L2 | L3 |
|---|---|---|
| source send | `B/Client/Program.cs:686-697,749-789`: envelope header와 protobuf body 생성 | `:700-718`: service head 생성 → multipart pack `[header,body]` → 원본 두 Message dispose |
| source request | `B/Client/Program.cs:618-630,652-663` | `:631-650`: correlation·service head·multipart pack 추가 |
| binding submit | `B/Client/Program.cs:721,957-985` | 같은 public `SubmitSendAsync`/`SubmitRequest`, socket·completion 정책 동일 |
| target receive | `B/ZLinkServer/Program.cs:254-280,300-308`: permit → blocking Recv → envelope parse | `:283-294,375-410`: service head parse → `TryDecodeFrameworkMultipartView` → envelope parse |
| target body/handler | protobuf span decode | `B/ZLinkServer/Program.cs:414-437`: 동일 protobuf decode; multipart GetSpan은 view |
| target reply | `B/ZLinkServer/Program.cs:343-365` response header+protobuf body | `:438-457`: reply body+response header → multipart pack, service reply header와 submit |
| source completion | `B/Client/Program.cs:792-810,854-870`: envelope parse+protobuf decode | `:811-854`: reply head parse → **복사형** `TryDecodeFrameworkMultipart(Message,out Message[])` → protobuf decode; service branch에서는 inner response envelope parse를 하지 않음 |

## Codec 경로

- 서비스 head encode: `D/Runtime/Service/ZLinkServiceWireCodec.cs:185-204` → `PrepareApplication:206-237`의 channel UTF8 byte[] → native `Message.Allocate` → `WriteApplication:242-262`. Bench `B/Client/Program.cs:636,703`은 반환 Message를 `Message.From(Message)`로 다시 공유 복제하고 원본을 dispose하지 않는다. Production `D/Runtime/Service/ZLinkManagedMeshNode.cs:10917-10933`은 반환 Message를 직접 넘긴다. 이 차이를 production 비용으로 합산하지 않는다.
- Multipart pack: `D/Runtime/Spots/ZLinkApplicationPayloadEnvelopeCodec.cs:153-169` → length 계산 `:171-175,280-287` → native Allocate → writer `:329-344` → **같은 length 계산 재호출** `:341` → count·length·part copy `:388-409`. 2 parts 기준 size loop 2회+write loop 1회, inner bytes 각각 1회 최종 native frame으로 복사. 완성 managed frame 재복사 없음.
- Target unpack: 위 codec `:211-257,490-540`. 고정 profile bytes 직접 비교, count·length validation 1회. 2 parts는 inline offset/length를 가진 `D/Runtime/Messaging/ZLinkEnvelopeCodec.cs:14-85`의 value struct이며 배열·Message·body copy 없음. `GetSpan:49-53`은 요청 때마다 frame.AsReadOnlySpan을 다시 얻는다.
- Bench completion unpack: 위 codec `:201-209,436-487`. profile validation 뒤 part bounds 1회 순회, 결과 배열 생성, 2차 순회에서 `Message.From(span)`으로 part마다 native storage를 만들고 bytes를 복사한다. `Bind/Contracts/Messaging/Message.cs:214-219`, `Bind/Runtime/Messaging/Message.Native.cs:328-336`.

## Production 연결과 소유

- source: `D/Runtime/Service/ZLinkManagedMeshNode.cs:10917-10970` CreateApplicationWire가 native service head+native packed payload를 wire 배열에 보관한다. 실패는 생성된 Message를 dispose한다. Direct send `:10973` 이후 binding public submit으로 전달한다.
- target: 같은 파일 `:7354-7379`가 서비스 head와 multipart view를 decode하며 `:7407` 이후 receive ownership과 함께 enqueue한다. View 자체는 frame을 dispose하지 않는다.
- reply: 같은 파일 `:12365-12390` PrepareNativeTerminalReply가 service reply+packed payload를 생성한다.
- production completion: 같은 파일 `:11442-11499` DecodeDirectApplicationReply는 **view형** decoder를 사용한다. `ZLinkBackendRouteReceived`가 원본 reply parts를 유지한다. Bench의 복사형 completion과 다르다.
- binding Message lifetime: `Bind/Contracts/Messaging/Message.cs:90-110,141-155,248-253`; native `Message.Native.cs:85-97,135-158,164-171`. `From(Message)`는 Copy()이며 대형 native payload는 공유한다. 이를 전체 byte deep-copy로 세지 않는다. 서비스 head는 작은 inline native bytes라 값 복제가 일어난다.

## 프로파일 범위

CPU 6–7,14–15를 source·target·trace collector 전체에 상속한다. source/target 각각 hard timeout 20초 이하. `measure.lock`은 읽어서 다른 측정 여부만 확인하며 취득하지 않는다. Guard가 멈춘 process에 CONT를 보내지 않는다. CPU/allocation trace와 active 구간의 thread CPU delta를 함께 보존한다. 공유 CPU의 결과를 공식 throughput 재측정으로 사용하지 않는다.

## 결정 소유 표

`C=framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp`, `J=framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/internal/service/ZLinkServiceM6AWireCodec.java`, `N=ec6aa8ea5c:framework/languages/node/packages/framework/src/runtime/foundation/service-wire-m6a-codec.ts`.

| 결정 | 소유 조항 | 현재 판정 위치 | 제안 뒤 소유자 |
|---|---|---|---|
| count·length·opaque part bytes·profile | `02-channel-transport/06-wire-protocol.ko.md:185-215`, schema가 유일 wire source | D payload codec `:153-175,329-409,490-540`; C `:3154-3228,3349-3428`; J `:383-463,508-551`; N `:282-340,433-451,492-529` | 각 언어 기존 codec. .NET의 재합산만 제거 |
| 완성 outbound frame 저장소 | `01-execution/05-payload-ownership-and-codec.ko.md` §1–2,4–5 | D payload codec `:158`; C `:59-106,3185`; J `:385`; N `:83-105,286` | 모두 codec의 native Message 하나 유지 |
| frame 생성 실패의 close | 같은 ownership §4 | D payload codec `:164-168`; C native writer RAII `:59-106`; J `:392-395`; N `:295-297` | 기존 close owner 유지 |
| 수신 frame 수명과 part view | 같은 ownership §4–5 | D mesh `:7371,11479-11491` + multipart view `D/Runtime/Messaging/ZLinkEnvelopeCodec.cs:14-85`; C `:3383-3428`; J codec `:499-505` + `ZLinkFrameworkMultipartView.java:19-28,196` 이후; N `:433-451,517` + backend `node-raw-mesh-backend.ts:2635-2639` | binding/record owner가 수명 유지. view는 byte copy와 분리 |
| submit·backpressure·completion | Core/binding submit 계약; Framework ownership §6 및 `01-submit-and-completion.ko.md` §9 | 각 언어 public binding submit. D `Bind/Runtime/Messaging/CompletionOwner.cs:443-485`; C raw owner `runtime/mesh/raw_mesh_node_owner.cpp:2151-2155`; Java raw node `runtime/binding/ZLinkJavaRawMeshNode.java:5415-5430`; Node raw port `runtime/backend/node/node-raw-binding-port.ts:520-536` | 변경 없음. 새 재시도·poller·queue·timer 없음 |

## 사다리와 production의 차이

- L3 source의 `Message.From(EncodeApplicationMessage(...))`는 production `CreateApplicationWire`에 없다. 작은 service head의 shared-copy를 새 wrapper로 만들고 원래 Message를 finalizer까지 남긴다. 원본은 native Message이므로 JS Buffer→native 같은 완성 payload 복사가 아니다.
- L3 reply는 `B/ZLinkServer/Program.cs:451-457`의 `RawEnvelopeHeaders.Response`를 사용한다. `B/Shared/RawEnvelopeHeaders.cs:11-13`에는 formatMarker·correlation이 없다. L2 `ReplyL2:344-365`의 동적 response encode와 다르다.
- L3 completion `B/Client/Program.cs:824-854`는 복사형 unpack 뒤 protobuf만 decode한다. L2 `:807-808`의 response header parse를 수행하지 않는다. Production direct completion은 view형이며, 이후 정상 envelope decoder로 전달된다. 따라서 request-backpressure의 L2→L3 수치를 순수 framing 차이로 정량 해석할 수 없다.
- Scratch의 유효 production command header는 252 B였다. protobuf body는 1K/4K body에 tag+varint 3 B를 더하므로 1,027/4,099 B다. 실제 send pack의 inner byte 복사는 2회, 총 1,279/4,351 B이며 최종 frame은 1,354/4,426 B다. Multipart 자체 overhead는 2-part에서 75 B다. Exact allocation probe의 fixture는 body를 1,024/4,096 B로 주므로 frame이 1,351/4,423 B인 점을 구분한다.
