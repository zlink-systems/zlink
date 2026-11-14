# Java L2 header 작업

- 시작 branch: `perf/4352-java-l2-header`, HEAD `2125999e62`, 변경 없음. 지정 branch에서 local commit만 수행한다.
- prior `l2bp`, `lad-java-l2e`, `n2net` 보고서 확인. scratch bench는 `f3e3933567` archive이며 composite build는 이 worktree Java Framework만 참조한다.
- 프로파일은 CPU 6-7,14-15, 직접 measure.lock 사용 없음. 테스트만 지정 test-slot으로 실행한다. raw 삭제 없음.

| 단계 | Java 작업·file:line | .NET 작업·file:line | 추가 할당·복사·collection | cold JIT / steady | 변경 후보 |
|---|---|---|---|---|---|
| client request encode | Envelope create:155, writeHeader:438, encodeHeaderBytesUncached:568 | ClientCallCodec:7, EnvelopeCodec:188,1195 | Header, correlation String, byte[]; writer size+write 순회 | 수집 예정 | 준비된 값/필드 처리 |
| target request parse | Envelope decodeHeader:846, readCanonical:1213, string:1311 | EnvelopeCodec:377,414,933 | HeaderReader, CanonicalReader, String+byte[], native scratch; canonical miss 시 Jackson 재시도 | 수집 예정 | reader 비용 |
| target reply encode | DispatchReporter:378, Envelope reply:181, writeHeader:438 | ReplyWriter:34, EnvelopeCodec:1195 | reply Header, byte[]; prepared names 3회 비교 | 수집 예정 | writer 반복 |
| client reply parse/completion | CallRuntime:260, Envelope tryDecodeHeader:693; binding CompletionOwner | ClientCallCodec:60; binding CompletionOwner | parse 위와 동일; binding pending lookup/remove가 physical correlation 소유 | 수집 예정 | parser 비용만 변경 |

비교할 항목: field/value scan, Strings, byte[]/char[] copies, metadata map, cache lookup, exceptions, correlation completion, JIT tier. wire·검증·buffer 소유 결정 위치의 전후 수를 기록한다.

## 구현 전 진단과 결정 소유

- 양방향 재현: 새 allocation 검사는 기존 코드에서 native correlated parse 520 B/header(상한 440), escape parse 2,080 B/header(상한 650)로 실패했다. encode는 owned array 336 B/header. 96개 request/reply/command 조합의 새 byte/decoded-value 검사는 기존 코드에서 통과했다. `raw/tests-before.log`, `raw/{allocation,header-bytes}-before.xml`.
- steady backpressure L2 CPU sample: source envelope 8.34%, target 22.26%. 제출=완료=수신, 오류 0. CanonicalReader.consume/string와 HeaderReader.readCanonical/decode가 parse를 분리해 수행한다. correlation은 hex String 2개와 concat을 만든다.

| 결정 | 소유 spec | 현재 판정 위치(언어별) | 최종 소유자 |
|---|---|---|---|
| header bytes | ClientServer §5.3 / messaging hot path W4 | Java Envelope:438; .NET EnvelopeCodec:1217; C++ framework/src/runtime/messaging/envelope_codec.cpp encode; Node channel-envelope.ts encodePayload | 각 언어 envelope codec 한 writer |
| JSON 문법과 field 검증 | ClientServer §5.3 header grammar/structure | Java Envelope HeaderReader:918/1213 + CanonicalReader:1278 + finish:1149; .NET EnvelopeCodec:414/933; C++ envelope_codec.cpp decode; Node decodeChannelEnvelopeHeader | Java envelope의 단일 byte reader + 완성된 header 검증 |
| buffer 수명 | messaging hot path I3 / payload ownership | Java MultipartView:61, Envelope:568; .NET EnvelopeCodec:1195; C++/Node envelope codec | 각 encoder가 owned output, reader가 borrowed input·독립 String을 소유 |
| physical correlation/completion | submit-and-completion §10–11 / hot path E5 | Java binding runtime/sockets/CompletionOwner:1199–1229; .NET binding CompletionOwner; C++/Node binding completion owner | 기존 binding(변경 없음) |

교차언어: C++ retained string_view, .NET Utf8JsonReader/span, Node byte reader, Java/Kotlin은 같은 Java codec. 반환 값·검증 계약 차이를 도입하지 않는다. 분류 B(기존 내부 비용 수정), Framework envelope codec 소유. 사용자 step 4에 따라 구현한다.

대안 비교: canonical reader만 작게 고치면 ASCII/일반 JSON 두 parser와 중복 검증이 남는다. Jackson 하나만 쓰면 baseline escape 경로의 2 KB/header 비용을 모든 header에 부과한다. 단일 byte reader가 RFC JSON·UTF-8·escape·unknown 값·중복 검증을 한 번 처리하도록 통합한다. 기존 per-reader scratch만 유지하고 HeaderReader 의미 상태 객체와 CanonicalReader 객체를 제거한다. 새 cache/queue/lock/timer/ThreadLocal 없음.

| 판정 위치 | 전 | 후(계획) |
|---|---:|---:|
| header writer | 1 | 1 |
| header parser | 2 | 1 |
| field/metadata/flow 의미 검증 | 1 | 1 |
| output buffer 소유 | 1 | 1 |
| completion/correlation 결정 | binding 1 | binding 1 |

## Native writer 추가 진단과 수정

- Heap header → Message.from 경로는 Message.allocate baseline보다 **416 B/header** 추가 할당한다(`raw/tests-writer-before.log`, `raw/writer-before.xml`). .NET EnvelopeCodec:1195–1206은 최종 Message span에 직접 쓴다. Java binding Message:244/578의 allocate/mutableDataBuffer 기존 API를 사용하며 binding 변경은 필요 없다.
- writer는 하나이며 byte[] API와 native ByteBuffer 목적지만 달라진다. PreparedNames는 값뿐 아니라 field prefix까지 같은 encoding owner에서 준비한다. HeaderField 표 하나에서 writer token과 reader field 이름을 제공한다. JSON factory·일반 Jackson parser는 삭제하고 기존 StreamReadConstraints만 유지한다.
- correlation formatting 기존 184 B/header 실패를 보존했다(`raw/tests-correlation-before.log`, `raw/correlation-before.xml`). UUID 값·32자리 소문자 bytes를 유지하며 HexFormat.formatHex에 16-byte 입력을 한 번 전달한다(104 B/header). 판단 소유자인 internal/service/ZLinkServiceOperationIds만 변경한다.
- 첫 native writer의 focused 검사 4,156개 중 cached command의 service frame copy 검사 1개 실패. Cached header까지 native materialization한 것이 원인이다. 기준을 바꾸지 않고, 기존 cache hit bytes는 그대로 유지하고 uncached header만 final Message를 할당하도록 encodeHeaderPart에서 한 번 결정한다. `raw/tests-focused-native-final.log` 보존.
- MultipartView는 이미 사용하던 byte[]/Message slot 표현을 그대로 보유하며 native header의 소유권만 같은 owner로 이전한다. 기존 byte[] entry는 같은 소유 함수로 모아 constructor/cleanup 중복을 줄인다.

## CPU 비용 분리

- 첫 통합 reader는 ByteBuffer의 mutable position을 byte마다 갱신하고, field 이름마다 14개 전체 길이를 검사했다. Steady request parse sample 추정이 source 1.34→3.07 µs/제출, target 1.72→3.57 µs/수신으로 증가했다. 이 후보를 완료안으로 채택하지 않는다(`raw/after-final-L2-bp`). Native allocation이 envelope 안으로 이동했으므로 writer 전체 envelope 비중은 이전과 직접 같은 범위가 아님도 확인했다.
- 단일 reader 안의 cursor 하나로 position 갱신을 옮기고 성공 때만 입력 cursor를 consume 상태로 공개한다. 기존 HeaderField lookup을 field-name 길이로 묶은 grammar table 하나로 대체해 비교 후보를 최대 3개로 줄인다(.NET ReadHeaderField:637도 길이로 구분). 새 cache가 아니며 entry·lookup policy·eviction을 추가하지 않는다. 정상 optional-string 검사는 parse의 invalid bit가 0이면 끝낸다. 새 제어 상태/queue/lock/flag 없음.
- Native writer의 null·빈 metadata 고정 토큰도 단일 bulk copy로 쓴다. writer/parser 수와 buffer/검증 owner는 동일하다. 후보 raw와 failed/invalid run은 모두 보존한다.
