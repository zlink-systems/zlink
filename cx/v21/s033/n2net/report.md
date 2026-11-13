# Node L2 header 비용 대조와 production 수정안

| L2 비용 | Node (`framework/languages/node/packages/framework/src/` 기준, 5f3a59bff6) | .NET (`framework/languages/dotnet/src/` 기준, 5f3a59bff6) | .NET이 덜 하는 작업 / 판정 |
|---|---|---|---|
| source request header encode | `runtime/channels/channel-envelope.ts:126,158,463,570,668`; caller `runtime/channels/channel-outbound-operations.ts:212-222` | `Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs:157,188,1195`; caller `ZLinkClientCallCodec.cs:7-36` | 둘 다 prepared bytes 사용. Node는 deadline Date·ISO 문자열·ASCII byte 재변환을 추가함. .NET은 stack buffer에 직접 format |
| target request decode | `channel-envelope.ts:889,926,967,1052,1634`; dispatcher `channel-dispatchers.ts:145,268` | `ZLinkEnvelopeCodec.cs:377,380,414,637,933`; `Runtime/Channels/ZLinkClientServerDispatcher.cs:140,247` | Node canonical도 bytes에서 읽으며 전체 header 문자열/JSON.parse 재검사 없음. 필요한 값은 양쪽 모두 문자열 생성. Node deadline은 문자열로 반환; .NET은 TryGetDateTimeOffset으로 value type |
| target reply encode | `channel-envelope.ts:217,463,668`; `channel-dispatchers.ts:409-412` | `ZLinkEnvelopeCodec.cs:157,188,1195`; `Runtime/Channels/ZLinkChannelReplyWriter.cs:14,34-60` | correlation 재사용, deadline 없음. Node의 full-template copy, .NET의 prefix copy+dynamic write. protobuf encode를 순수 header CPU로 계산하지 않음 |
| source reply decode | `channel-envelope.ts:322,889`; `channel-outbound-operations.ts:243-248` | `ZLinkEnvelopeCodec.cs:377,414,933`; `ZLinkClientCallCodec.cs:60-91,125-126`의 reply decode 경로 | Node는 caller에 전달할 4개 문자열(channel/message/content-type/correlation)을 생성. .NET도 같은 네 값 생성. Node escape가 있는 문자열은 token별 JSON.parse, 전체 recheck는 없음 |
| correlation 생성·검증 | `channel-envelope.ts:110-123,1674,1695,1724-1744` | `Systems.Zlink.Stream.Connector/Runtime/ZlinkStreamCorrelation.cs:9-13`; `ZLinkEnvelopeCodec.cs:1097-1106` | Node는 suffix hex+zero slice+두 번 연결 후 Buffer.write에서 flatten 가능. .NET은 counter의 hex 문자열 1개. 외부 ID 1..64 ASCII 검증은 둘 다 유지 |
| deadline format·parse | `channel-envelope.ts:521,700,723,983,1651`; L2 decode는 deadline 문자열을 반환하며 재parse하지 않음 | `ZLinkClientCallCodec.cs:33`; `ZLinkEnvelopeCodec.cs:497-506,1229-1230,1363-1380` | Node request마다 Date 객체 1개+ISO 문자열 1개. .NET DateTimeOffset/stackalloc/Utf8Formatter로 heap timestamp 객체·문자열 없음. .NET도 length/write 두 번 format함 |
| metadata·flow | `channel-envelope.ts:505,523,1010-1018,1116,1759-1853` | `ZLinkEnvelopeCodec.cs:518-537,588,943-1023,1251` | 빈 metadata Node 공유 객체, .NET wire에 metadata object가 있으면 빈 Dictionary 1개. Node가 항상 더 많이 할당하지 않음. 둘 다 flow 문자열/validation 필요. Off이면 Node는 flow value 문자열 생성을 생략 |
| header buffers·copies/message | prepared `channel-envelope.ts:711-728`; 일반 `:626-665` | `ZLinkEnvelopeCodec.cs:1195-1216,1226`; command `:225` | Node prepared request/reply: Buffer view 1개(작은 header의 backing은 slab 공유), template 전체 1회 copy+동적 필드 덮어쓰기. command(no flow): 공유 buffer 반환, 0 alloc/copy. 일반: Buffer 1개+writer 2개. .NET request/reply: Message 1개+native header storage 1개, prefix 1회 copy+필드 직접 쓰기. native Message 생성 backing은 managed 할당 수와 구분 |
| 문자열/message (wire encode/decode, payload 제외) | 생성 request: ID suffix/slice/concatenation 최대4개+ISO1개; decode request: 비-null 문자열 5개, reply4개, command3개. flow +1, metadata entry +2, escape token의 JSON source +1 | 생성 request ID1개; timestamp 문자열0개. decode request/reply4개, command3개. flow +1, metadata entry +2; known field name stackalloc (`:641-646`) | Node 문자열 수는 V8 rope/slice 최적화에 따라 달라지므로 정적 생성 위치와 실제 allocation trace를 별도 보고. prepared encoding은 고정 field/name 문자열을 매번 만들지 않음 |

공통 prefix: 위 Node 약칭은 `runtime/channels/`, .NET codec/call 약칭은 `Zlink.Framework/Runtime/Messaging/`. .NET 읽기 기준도 이번 branch의 코드이며 ladder .NET 기준 69891f4446과 해당 codec 차이는 별도 확인한다. Node ladder는 2bef18aa94의 bench를 git archive로 이 작업의 scratch에 만들고 이 branch production package를 연결한다. 다른 worktree나 그 build output은 읽거나 쓰지 않는다.

## 원인과 제안 (구현 전)

l2bp의 Node inclusive envelope CPU는 source 15.51%, target 13.09%, source request encode 11.58%, target reply encode 7.29%(protobuf 포함)다. 근거: `/root/jobs/fx/cx/v21/s033/l2bp/raw/node/baseline-L2/cpu-summary.json`. 이 값은 이번 branch의 전/후 측정을 대체하지 않는다. L2 요청 encode가 source의 event-loop 동기 작업을 늘리며 binding 완료 slot 누수나 새 Promise hop 증거는 없다. ladder source는 `shared/raw-wire.js:89-116`, target reply도 production codec을 부른다.

소유 계층은 Framework channel-envelope codec이다. ClientServer §5.3 (`framework/doc/framework/common/spec/server/02-channel-transport/03-client-server-channel.ko.md:385-426`)의 UTF-8 JSON/metadata, Messaging hot path §3 E1·§4.1 I3·§4.3 W4 (`01-execution/08-messaging-hot-path.ko.md`)의 encode/decode 경계에 속한다. Core/binding admission·completion·queue·gate는 변경하지 않는다. 분류 **B: 기존 내부 변환 비용 수정**이며 public 동작 차이 A/D가 아니다.

| 결정 | spec 소유 | 현재 판정 위치 (각 언어) | 수정 후 소유자 |
|---|---|---|---|
| header encode/decode/검증 | ClientServer §5.3, hot path E1/I3/W4 | Node `channel-envelope.ts:570,889,1634`; .NET `ZLinkEnvelopeCodec.cs:188,377,933`; C++ `framework/languages/cpp/framework/src/runtime/messaging/envelope_codec.cpp:168,527,1293-1328`; Java/Kotlin 공유 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkChannelEnvelope.java:502,634,846,1213` | 기존 각 언어 codec, 1→1 |
| correlation 생성 | correlation/flow spec §3 | Node `channel-envelope.ts:115`; .NET `ZlinkStreamCorrelation.cs:13`; C++ client_call_codec generator; Java/Kotlin `ZLinkServiceOperationIds.java` | Node 기존 generator 한 곳, 1→1 |
| deadline의 wire 표현 | ClientServer §5.3 | Node Date→ISO (`:521`)와 writer (`:700,723`); .NET `FormatHeaderDeadline:1363`; Java/Kotlin/C++ 기존 codec | Node numeric deadline→final bytes 한 곳, 변환 판정 위치 2→1 |

선택안: 기존 process random prefix의 Buffer를 보관하고 counter를 같은 Buffer의 뒤 8 bytes에 써서 `.toString('hex')` 한 번으로 기존 32자 ID를 만든다. 기존 prefix 문자열과 zero-slice/연결을 제거한다. Deadline은 Unix milliseconds를 Gregorian date/time 숫자로 나누어 최종 header buffer에 ISO와 같은 bytes를 직접 쓴다. 4자리/확장 6자리 year, millisecond, Date의 허용 최대 범위를 기존 출력과 맞춘다. 준비된 header와 일반 writer 모두 같은 formatter를 쓴다. decoder 반환 deadline은 기존 문자열로 유지한다.

대안: Date/ISO를 계속 만들고 기존 full-template cache를 확장하는 방법은 timestamp allocation을 남기고 cache 조건을 늘려 제외한다. 전체 decoder 재작성/indexOf·RegExp reader 후보는 l2bp에서 기각됐고 이번 수정에 사용하지 않는다. 필수 문자열을 cache로 공유하거나 수신 buffer를 retain하는 방법도 새 수명 규칙 때문에 제외한다.

규칙 수(판정 위치): correlation uniqueness 1→1, correlation representation 1→1, deadline representation 2→1, wire validation 1→1. 추가 queue/lock/flag 없음. process prefix Buffer는 기존 prefix string을 대체하며 counter 소유는 그대로다. 새 스펙·wire·public API가 필요하면 구현을 중단한다.

## 검증 예정

고정 correlation·deadline으로 request/reply/command의 byte-identical output과 decoded values를 전/후 모두 검증한다. no deadline/flow/metadata, flow와 metadata, escape, 모든 ASCII 및 64-byte correlation, 잘못된 65-byte/비ASCII, ISO 최대 날짜/확장 year를 포함한다. Node format/lint와 touched-path tests를 실행한다. 모든 프로세스 CPU 6–7,14–15, profile ≤20초, measure.lock 미사용이다. CPU shape만 비교하며 throughput 개선/7% 통과로 판정하지 않는다. raw는 모두 보존한다.

## .NET 정적 allocation 계수

payload codec·binding 제출을 제외한 정상 request encode: caller Header record 1개(`ZLinkClientCallCodec.cs:28-36`), ID string 1개(`ZlinkStreamCorrelation.cs:13`), header Message wrapper 1개+native storage 1개(`ZLinkEnvelopeCodec.cs:1203`), TwoMessageParts 1개(`ZLinkMessageParts.cs:25-29,69`). Timestamp는 DateTimeOffset value type이며 heap Date/string 0개. Reply encode는 Header record 1개(`ZLinkChannelReplyWriter.cs:41-60`), Message wrapper+native storage 각1개, parts wrapper1개, 새 correlation/deadline string 0개. Decode request/reply는 final Header record1개(`ZLinkEnvelopeCodec.cs:548`)+필수 값 string4개(`:486-495,578-582`), metadata object가 있으면 빈 경우도 Dictionary1개(`:599`). Reader/field-name/deadline scratch는 heap allocation 0개. Nonempty metadata는 Dictionary storage와 entry당 key/value string 각1개를 더한다. Native allocator 내부의 세부 allocation 수까지 managed code만으로 확정하지 않는다.

일반 .NET request/reply encode는 prefix만 한 번 복사하고 dynamic field를 최종 storage에 쓴다(`:1226-1256`); 길이 계산 pass는 write하지 않는다(`:1286-1288`). Decode는 AsReadOnlySpan으로 읽어 전체 header byte copy 0회(`:377-378`). Body serializer 비용과 binding의 native copy는 양쪽 header 계수에서 제외한다. .NET codec은 69891f4446→5f3a59bff6 diff 0이다. Reference loss 1K 12.48%, 4K 9.57%는 `lad-dotnet-s1/progress.md:133-177`에 기록된 L1/L2 71,957.2→62,976.2 및 83,673.6→75,668.6의 비다. 이번 제한 CPU profile의 throughput 판정에는 사용하지 않는다.

## 구현 전 측정 보강

이번 branch의 유효 L2 inclusive envelope는 source **17.400%**, target **7.225%**다. Source encodeChannelEnvelopeParts 13.998%, target encodeChannelReplyParts 4.451%(protobuf 포함), request/reply decode 2.773%/3.345%다. raw: `raw/node/before-L2-final/cpu-summary.json`. L1 두 process는 envelope 0%(`raw/node/before-L1-final/cpu-summary.json`). 모든 child exit0, 9.48s/11.09s. 이 수치는 제한된 CPU의 shape 진단이며 throughput 판정이 아니다.

Exact allocation trace(--jitless, 2000 calls)는 request encode 21.101개/903.50B, reply encode 9.081개/422.05B, request decode 17.012개/744.49B, reply decode 14.011개/632.47B, command encode 2개/80B다(`raw/alloc-before/allocation-summary.json`). 문자열/Date와 final parts 외에 Node builtin의 hideStackFrames(...args)와 Buffer.toString arguments object도 포함된다. `node:internal/errors:535-537`, `node:buffer:1114-1146`에 해당 wrapper 경로가 있다. --jitless 수치를 최적화된 live runtime의 실제 count로 단정하지 않으며, 같은 probe의 기본 V8 최적화 모드와 실제 ladder의 collected-object allocation sampling을 함께 기록한다.

.NET ladder 69891f4446의 source encode는 `framework/bench/grpc/dotnet/Client/Program.cs:436-449`, target decode/reply는 `ZLinkServer/Program.cs:226-243,283-304`다. PrepareResponse(`ZLinkEnvelopeCodec.cs:259-274`)는 reply messageName을 빈 문자열로 준비하고 metadata는 null이면 wire에서 생략한다. Node는 request messageName과 `metadata:{}`를 유지한다. 그러므로 실제 .NET L2 request decode는 header1+string4=managed5개, reply는 header1+비어 있지 않은 string3=managed4개이며 metadata Dictionary가 없다. Node reply의 messageName bytes/string은 실제 비용 차이다. 이를 제거하면 이번에 고정한 Node wire bytes/decoded values가 바뀌므로 수정 범위에서 제외한다. 다른 언어의 wire 표현을 Node에 그대로 덮어쓰지 않는다.

## 최종 제안 개정 (반영 전)

직접 Gregorian 계산 후보는 optimized V8에서 10.101→4.237 allocations/message였으나 interpreter에서는 숫자 boxing 때문에 21.101→48.100으로 증가했고 기존 roundtrip gate(13.5/module-owned allocations)를 실패했다. 정수 cast 후보도 같아 제외했다. raw `alloc-after/`, `int-probe/`는 기각 후보이며 최종 결과가 아니다.

최종 수정은 native ISO formatter를 유지한다. 동기 codec의 Date scratch 1개를 매번 setTime 후 toISOString으로 쓰고 per-message Date 객체를 제거한다. Gregorian/year/유효 범위는 Node Date가 계속 소유하므로 날짜 규칙을 추가하지 않는다. Correlation은 prefix와 counter를 같은 16-byte buffer에 두고 뒤 8 bytes를 carry로 증가시킨다. Number counter와 zero-slice/문자열 연결을 없앤다. Prepared correlation/deadline/flow는 고정 길이를 이미 검증하므로 Buffer.write(string, offset, 'ascii') 공개 overload로 length 중복 검증을 줄인다. wire bytes는 같다.

Date scratch는 per-call Date를 대체하며 deadline cache나 authoritative deadline state가 아니다. Scalar counter를 삭제하고 prefix storage에 counter를 합친다. queue/lock/flag 추가 없음. Rule count: deadline의 값/ISO 소유자 1→1(native Date), correlation 생성 1→1(codec), 외부 correlation·wire validation 1→1(codec), prepared-field length 검증 위치 2→1(eligibility만 판단; Buffer length 인자 제거).

새 allocation 회귀 예산은 final array+element storage(2), interpreter native setTime boxed result(1), ISO string(1)의 4개/호출이다. 기존 roundtrip assertion은 변경하지 않는다. 초기 새 테스트의 'ISO까지 없앰' 예산은 기각된 직접 날짜 계산안에 해당하므로 최종 representation의 회귀 예산으로 대체한다. baseline은 이 최종 예산에서도 실패해야 한다.
