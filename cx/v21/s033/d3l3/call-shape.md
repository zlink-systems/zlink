# .NET L3 호출 경로

Production 기준 `6350275fac`, bench 기준 `29d9fca82b`. `D=framework/languages/dotnet/src/Zlink.Framework`, `B=framework/bench/grpc/dotnet`, `N=bindings/dotnet/src/Zlink`. Production 파일 변경 없음. Scratch에서는 기존 Framework test friend assembly 이름으로 내부 codec에 접근하며 binding public API만 사용한다.

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
- Bench completion unpack: 위 codec `:201-209,436-487`. profile validation 뒤 part bounds 1회 순회, 결과 배열 생성, 2차 순회에서 `Message.From(span)`으로 part마다 native storage를 만들고 bytes를 복사한다. `N/Contracts/Messaging/Message.cs:214-219`, `N/Runtime/Messaging/Message.Native.cs:328-336`.

## Production 연결과 소유

- source: `D/Runtime/Service/ZLinkManagedMeshNode.cs:10917-10970` CreateApplicationWire가 native service head+native packed payload를 wire 배열에 보관한다. 실패는 생성된 Message를 dispose한다. Direct send `:10973` 이후 binding public submit으로 전달한다.
- target: 같은 파일 `:7354-7379`가 서비스 head와 multipart view를 decode하며 `:7407` 이후 receive ownership과 함께 enqueue한다. View 자체는 frame을 dispose하지 않는다.
- reply: 같은 파일 `:12365-12390` PrepareNativeTerminalReply가 service reply+packed payload를 생성한다.
- production completion: 같은 파일 `:11442-11499` DecodeDirectApplicationReply는 **view형** decoder를 사용한다. `ZLinkBackendRouteReceived`가 원본 reply parts를 유지한다. Bench의 복사형 completion과 다르다.
- binding Message lifetime: `N/Contracts/Messaging/Message.cs:90-110,141-155,248-253`; native `Message.Native.cs:85-97,135-158,164-171`. `From(Message)`는 Copy()이며 대형 native payload는 공유한다. 이를 전체 byte deep-copy로 세지 않는다. 서비스 head는 작은 inline native bytes라 값 복제가 일어난다.

## 프로파일 범위

CPU 6–7,14–15를 source·target·trace collector 전체에 상속한다. source/target 각각 hard timeout 20초 이하. `measure.lock`은 읽어서 다른 측정 여부만 확인하며 취득하지 않는다. Guard가 멈춘 process에 CONT를 보내지 않는다. CPU/allocation trace와 active 구간의 thread CPU delta를 함께 보존한다. 공유 CPU의 결과를 공식 throughput 재측정으로 사용하지 않는다.
