# Node L3 service wire 원인과 운영 수정안

## 판정

운영 Node 송신 경로는 multipart를 JS Buffer에 완성한 뒤 binding이 같은 frame을 native message로 다시 복사한다. `service-wire-m6a-codec.ts:216-229,258-274`의 inner part→frame 복사는 framing에 필요하지만, `node-raw-mesh-backend.ts:2951-2958` → `raw-service-mesh-runtime.ts:1945-1946` → raw binding submit으로 이어지는 완성 frame의 두 번째 복사는 public `Message.allocate`에 직접 기록하여 없앨 수 있다. C++·Java·.NET은 이미 이 방법을 제공한다. 분류는 **B(기존 구현의 불필요한 복사)**이며 wire·public API·spec 변경은 필요하지 않다.

수신 codec은 이미 원본 frame의 view를 반환한다. `decodeMultipartApplicationParts`/`decodeMultipartParts` (`service-wire-m6a-codec.ts:350-362,409-449`)에 payload 복사가 있다고 진단하지 않는다. `Buffer.concat`, base64, payload 전체 UTF-8 변환도 이 codec의 정상 multipart 경로에는 없다.

`R=framework/languages/node/packages/framework/src/runtime`, 아래 운영 file:line은 시작 HEAD `df42a4df40` 기준이다. Scratch `20598f57a2`의 codec과 시작 HEAD codec은 동일하다 (`git diff 20598f57a2 HEAD -- .../service-wire-m6a-codec.ts` 차이 0).

## 재현·프로파일 근거

Scratch `/root/worktree/zlink-v21-s033/n3sw-scratch`에서 의존성·HTTP client archive·Framework dist를 새로 빌드했다. 측정 worktree와 그 build output은 사용하지 않았다. [호출 대조](call-shape.md), 각 run의 `build.json`에 HEAD·base·dirty·artifact SHA256·freshness를 기록했다. 모든 자식 process는 CPU 6–7,14–15에 고정했고 각 프로파일은 20초 이내다. measure.lock은 사용하지 않았다. 이 CPU 영역은 다른 측정의 보조 process도 사용하므로 절대 msg/s나 개선율 판정에 사용하지 않는다.

각 디렉터리에는 source/target의 `.cpuprofile`, `.heapprofile`, `*-summary.json`, `events.json`, thread affinity/policy, counters와 log가 있다.

| 프로파일 | source 활성 sample 형태 | target 활성 sample 형태 | raw |
|---|---|---|---|
| L2 send 4K | service codec 0%, binding 38.28%, GC 6.33% | service codec 0%, binding 45.97%, GC 5.02% | [L2 send](raw/baseline-L2-send-4K/) |
| L3 send 4K | service codec 7.61%, binding 38.45%, GC 6.13% | service codec 6.81%, binding 39.55%, GC 4.86% | [L3 send](raw/baseline-L3-send-4K/) |
| L2 serial 1K | completion poll 89.19% | idle 14.48%, service codec 0% | [L2 serial](raw/baseline-L2-serial-1K/) |
| L3 serial 1K | completion poll 89.02%, service codec 0.93% | idle 14.95%, service codec 2.16% | [L3 serial](raw/baseline-L3-serial-1K/) |

L3 send source에서는 `encodeMultipartApplicationPayload` 186/3428 samples, `allocateApplicationPayload` 93, `writeMultipartPayload` 65였다. 완성 frame을 받는 binding `submitSend` self는 L2 305/1928 → L3 704/3428 samples, native body `data` self는 L3 225/3428 samples였다. Native call 안의 memcpy와 다른 native 작업은 JS CPU profile만으로 정확히 분리할 수 없으므로 copy의 존재는 아래 source 경로와 독립 계측으로 확인했다. Serial에서 poll 대기가 유지되며 framing이 새 wait 정책을 추가한다는 증거는 없다.

각 run은 errors=0, source submitted=completed=target received를 확인했다. Thread의 policy와 affinity는 `threads-ready.json`, `threads-active.json`에 보존했다. CLI heap profile은 종료 시 살아 있는 sampled allocation이며 누적 allocation 수로 해석하지 않는다. Native/external bytes는 V8 heap sample에 완전히 포함되지 않는다.

기존 측정의 손실은 사용자 제공값(send 1K/4K 30.8%/35.1%, serial 15.8%/13.9%, backpressure 19.0%/19.3%)이며 이번 프로파일의 throughput과 혼합하지 않는다. `lad-node-s1/progress.md`의 counted run들과 각각의 raw 경로를 읽었다. 그 파일에는 재시도와 다른 session도 있으므로 임의 run을 섞어 새 손실률을 만들지 않는다.

## message당 복사·할당

독립 계측 [baseline-copy-counts.json](raw/baseline-copy-counts.json)은 288-byte inner header와 1K/4K body를 사용한다. [copy-counts.cjs](tools/copy-counts.cjs)는 Buffer allocation·set·copy·write·subarray를 구분하고 roundtrip과 원본 backing store identity를 함께 검사한다.

| 경로 | byte 복사·쓰기 | view·객체 allocation | production 위치 |
|---|---|---|---|
| service send/request/reply header | 새 11/19/21-byte Buffer; channel text 5 bytes 쓰기 | header Buffer 1 | `R/foundation/service-wire-m6a-codec.ts:93-114,143-161,641-648` |
| multipart pack | inner header와 body 각 1회 `.set`: 총 1312/4384 bytes. 고정 profile text 52 bytes 쓰기 | 완성 frame 1387/4459 bytes의 slab view 1; slab 확장 때만 backing allocation | `:216-229,258-274,278-305`; `contracts/Common/payload-slab.ts:9-23` |
| 완성 frame submit | JS Buffer→native 완성 frame 1회 추가: 위 fixture의 1387/4459 bytes | native message storage + binding operation 객체 | `R/backend/node/node-raw-binding-port.ts:327-340,343-377`; binding `runtime/buffers/message_conversion.ts:23-37`, native `addon_core.cc` Buffer 입력 분기 |
| direct receive decode | 추가 byte copy 0 | part당 Buffer view 1 + `ZLinkBufferMessage` wrapper 1, 결과 배열 1; 2-part는 각각 2/2/1 | `R/foundation/service-wire-m6a-codec.ts:350-362,409-449`; `R/backend/runtime-message.ts:16-17` |
| ladder receive decode | 추가 byte copy 0 | direct와 동일 + intermediate multipart view 1 + profile 객체 1; service header Reader·string·result 별도 | `shared/raw-wire.js:151-195`; codec `:337-346` |
| native receive→JS | binding이 managed storage를 제공할 때 1회; Framework part decode와 별도 | binding receive facade·Buffer | binding `contracts/messaging/message.ts:309-328`, `runtime/messaging/message_materializer.ts` |

L3 source ladder는 먼저 `encodeBenchPayloadMessage`로 native body를 만든 뒤 pack을 위해 `body.data()`를 읽는다 (`shared/raw-wire.js:202-203,137`). 이 native body의 Buffer 노출/GC 비용도 L3 profile에 포함된다. 운영 serializer는 owned encoded payload를 쓸 수 있으므로 이 비용 전체를 운영 framing codec의 비용이라고 주장하지 않는다. Bench만 바꾸어 손실을 없애는 수정은 하지 않는다.

Request 왕복에는 pack 2회, 수신 unpack 2회가 있다. 불필요한 완성 frame copy는 요청·응답 leg마다 1회다. Inner payload JSON/protobuf 직렬화와 binding receive copy는 framing copy 수와 합산하지 않는다.

## 결정 소유 표·교차언어 대조

| 결정 | 소유 spec | 현재 판정 위치(언어별) | 수정 뒤 소유자 |
|---|---|---|---|
| multipart byte layout·길이 검증 | `02-channel-transport/06-wire-protocol.ko.md:123-165,185-215` §2, schema `framework/runtime/protocol/service-wire-v1.schema.json` | Node `service-wire-m6a-codec.ts:216-305,409-449`; C++ `service_wire_codec.cpp:3020-3060,3224-3228,3349-3418`; Java `ZLinkServiceM6AWireCodec.java:375-409,443-463,508-550`; .NET `ZLinkApplicationPayloadEnvelopeCodec.cs:153-170,211-260,350-434` | 각 언어 codec 한 곳 유지; Node는 기존 writer를 재사용 |
| outbound 완성 frame storage | `01-execution/05-payload-ownership-and-codec.ko.md` §1–5 (Framework 추가 copy 0 목표, view·객체와 별도 회계) | Node codec JS frame + raw port의 Buffer submit; C++ native writer `service_wire_codec.cpp:3224-3228`; Java native `Message.allocate` `ZLinkServiceM6AWireCodec.java:383-394` (`fea12ea97a` 확인); .NET `Message.Allocate` `ZLinkApplicationPayloadEnvelopeCodec.cs:153-170` | Node도 codec에서 최종 native Message를 작성하고 raw port가 public binding API로 제출; 중간 완성 JS frame 제거 |
| 수신 part backing lifetime | 같은 ownership spec §4–5·§8 | Node `node-raw-mesh-backend.ts:2635-2639` + `runtime-message.ts:16-23`; C++ retained owner `service_wire_codec.cpp:3383-3431`; Java `decodeFrameworkMultipartFrameView(Message)` `:499-506`와 `ZLinkFrameworkMultipartView`; .NET multipart view `ZLinkApplicationPayloadEnvelopeCodec.cs:211-260` | 수신 retained/view 경로 유지 |
| submit·backpressure·completion | Core/binding의 기존 submit 계약, Framework는 결과 관찰 | Node `node-raw-binding-port.ts:327-377`, C++/Java/.NET도 각 public binding submit | 새 retry·poller·queue·timeout·정책 상태 없음 |

C++ 사다리는 [l3868-impl2/report.md](../l3868-impl2/report.md), [call-shape-L3.md](../l3868-impl2/call-shape-L3.md)의 retained-frame decoder를 대조했다. Java `fea12ea97a`의 bench와 production codec을 `git show`로 확인했다. 언어 간 wire·public 동작 차이가 아니라 저장소와 할당 경로의 차이이므로 D(spec gap)가 아니다. 다른 언어 파일은 수정하지 않는다.

## 운영 수정안

대안 1은 고정 profile text의 재검증·UTF-8 쓰기와 중간 decoder 객체만 줄이는 것이다. frame 크기에 비례하는 Buffer→native 전체 복사가 남는다. 대안 2는 기존 multipart writer가 최종 native Message에 직접 기록하게 하고 기존 raw port에서 public Message submit으로 넘기는 것이다. **대안 2를 적용한다.** 필요하면 고정 profile field도 codec 안의 기존 상수에서 준비하되 형식이나 validation을 분산하지 않는다.

- 적용 범위: `framework/languages/node`의 운영 service wire codec, node/channel backend, raw runtime/port의 내부 outbound frame 전달과 그 focused contract test. 기존 Buffer encoder는 내부 byte 소비자와 기존 테스트에 필요한 계약을 유지한다. 공통 writer는 하나다.
- 한 outbound frame의 snapshot·storage owner를 하나로 둔다. 선택 실패·로컬 전달·동기 submit 실패·정상 submit·backpressure·deferred route 대기에서 소유권을 명확히 종료한다. Caller bytes를 나중에 다시 읽는 lazy serializer로 snapshot 의미를 바꾸지 않는다.
- 완성 JS frame을 보관했다가 native frame을 다시 만드는 대신, native frame 하나를 만들어 전송한다. Local/Buffer 소비자는 기존 bytes 계약을 유지한다. 수신 decoder는 기존 view를 보존한다.
- 공개 binding `Message.allocate/data/close`와 submit을 사용한다. Binding private/native addon API, caller/sample의 수동 framing, 숨은 Buffer metadata/WeakMap 매핑을 추가하지 않는다.
- 규칙 수(판정 위치): wire 형식·검증 owner **1→1**; outbound 완성 frame 저장소를 생성하는 위치 **2(JS codec allocation + binding Buffer copy)→1(native codec allocation)**. 추가 queue·lock·timer·retry·routing 판단 0. 새 소유 표현이 필요하면 중간 JS frame 표현을 대체하며 중복 저장하지 않는다.
- 검증: 독립 byte fixture와 offset/empty/string/1K/4K/large parts, encode snapshot, decode backing identity, native frame 직접 제출과 실패 시 release를 검사한다. 비용 검사는 legacy Buffer 경로를 음성 대조군으로 삼아 실제 copy가 있을 때 실패해야 한다. Node format check와 touched framework tests를 실행한다.

이 보고서를 먼저 저장한 뒤 사용자 지시에 따라 구현을 진행한다. Spec 또는 public API 변경이 필요해지면 구현을 중단하고 보고한다. 최초 보고서와 작성 시점/production git status는 `raw/report-first.md`, `raw/report-first-evidence.json`에 보존한다.
