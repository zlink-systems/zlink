# Java typed JSON byte[] 비용 조사·수정

**wire는 유지한다.** Java decode의 `getText → Base64.decode(String) → encodeToString → equals`가 byte[] field마다 전체 길이의 중간 buffer를 만든다. encode는 이미 Jackson UTF-8 streaming 경로다. 수정 대상은 shared JVM profile의 bytes decoder이며 application DTO·bench·spec·public API는 바꾸지 않는다.

## 구현 전 원인·소유 판정

기준 branch `perf/4354-java-typed-json`, 시작 HEAD `df42a4df4098d543be8dc0b84510cf6d961576d8`, clean. scratch는 `9d9901eb2c`의 detached worktree `/root/worktree/zlink-v21-s033/j4json-scratch`다. 사용자 지시대로 internal-docs, GitHub, push/PR, sub-agent, measure.lock을 사용하지 않는다. measuring worktree와 build output은 읽거나 수정하지 않는다. 다른 worktree의 code가 필요할 때에는 local object의 `git show`만 사용한다. raw는 보존한다.

아래 Java 경로의 prefix: `framework/languages/java/`.

| 결정 | spec 소유 조항 | 현재 판정 위치 | 모을 소유자 |
|---|---|---|---|
| byte sequence wire 및 padded canonical Base64 | `framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md:134-153`, 특히 :153 | Java `zlink-framework-json-internal/src/main/java/systems/zlink/framework/runtime/internal/json/ZLinkFrameworkJsonProfile.java:352-375`; C++ `common/include/zlink/json_profile.hpp:275-300`; .NET `src/Zlink.Framework/Runtime/Messaging/ZLinkFrameworkJsonPayloadCodec.cs:10-33` → STJ byte[] converter; Node `packages/stream-wire/src/internal/framework-json-v1.ts:137-144,377-406` | 각 언어 기본 typed serializer. Java는 기존 shared JSON profile 안에서만 수정 |
| JSON→typed bytes | 같은 §5 | Java `ZLinkFrameworkJsonProfile.java:362-369`의 JDK decode와 재encode 비교; C++ :280-298의 decode 중 padding bit 검사; .NET STJ `ByteArrayConverter.cs:17`; Node :138-144의 문법·padding 검사와 decoder | Java의 검증하며 decode하는 한 경로 |
| payload 소유권·native Message copy | 기존 serializer/binding 경계 | Java `zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/ZLinkJsonMessageSerializer.java:49-76,98-116`, `ZLinkMessagePayloads.java:20-21` | 기존 owner 유지 |

**분류 B: 기존 구현의 불필요한 변환·할당 제거.** 사용자 지시의 Java production 최적화 범위로 진행한다. spec/public API 변경은 필요하지 않다. 언어별 accepted/rejected 값을 새로 바꾸는 작업이 아니다.

### 원인과 대안

`ZLinkJsonMessageSerializer.java:74-76`은 보관된 mapper의 `writeValueAsBytes`를 사용한다. mapper는 생성자(:39-46)에서 한 번 만들고, serializer/deserializer metadata를 cache한다. per-message mapper, tree model, StringBuilder 또는 encode의 JSON String→UTF-8 재encode는 없다. Jackson 2.17.2 `ByteArraySerializer.java:48-49`는 `UTF8JsonGenerator.java:891-905,1903-1941`에 bytes를 직접 전달한다.

반면 shared profile :362에서 Base64 token의 String을 만들고, :368에서 JDK decoder가 그 String을 Latin-1 byte[]로 복사한다. :369에서 decoded bytes를 다시 encode한 byte[]와 String backing byte[]를 만든다. 정상 경로의 field당 source-level 비용은 Base64 길이 B=4×ceil(N/3)에 대해 **byte[B] 네 개 + byte[N] 한 개 + String 두 개**, parser의 char buffer는 별도다. token이 여러 char segment에 걸치면 Jackson `TextBuffer.java:494-505`의 StringBuilder와 backing byte[]도 추가된다. 수정안의 `getTextCharacters()`도 segmented token에서는 `TextBuffer.java:431-444,518-523`의 합쳐진 char[]를 요구한다. 이 합침 비용을 0으로 계산하지 않는다. JVM의 String 객체 scalar replacement 가능성은 실제 allocation 결과와 구분한다. 근거는 [profile source](raw/java-profile-before.txt), [JDK decoder bytecode](raw/jdk-base64-decoder-bytecode.txt), [JDK encoder bytecode](raw/jdk-base64-encoder-bytecode.txt).

| 대안 | 비용·판정 |
|---|---|
| JDK decode 유지 + 재encode 비교만 padding bit 검사로 교체 | 기존 intrinsic decoder를 유지하지만 token String과 String→Latin-1 copy가 남는다 |
| parser의 char range에서 기존 Jackson Base64 alphabet을 사용해 검증과 decode를 한 번에 수행 | decoded byte[] 하나를 정확한 크기로 할당한다. 정상 경로의 token String·Latin-1 input·재encode 결과를 제거한다. 선택안. 잘못된 입력의 기존 error category/message를 보존한다 |

규칙 수는 Base64 유효성을 결정하는 algorithm 위치를 센다. **수정 전 2(JDK decode + roundtrip 비교) → 후 1(검증하며 decode)**. 길이·alphabet·마지막 quartet의 padding 및 zero pad bits는 같은 RFC 4648 규칙을 이 owner 한 곳에서 확인한다. 새 flag·queue·lock·runtime cache 없음. 별도의 Base64 alphabet mapping도 추가하지 않고 Jackson의 기존 mapping을 재사용한다.

### Java의 copy·allocation 경계

N은 byte[] field의 길이, B=4×ceil(N/3), J는 `{\"body\":\"…\"}`의 UTF-8 길이 B+11이다. 제어 객체와 payload 크기에 비례하는 storage를 구분한다. 아래는 단일 bytes field를 가진 DTO의 정상 경로다. recycler가 반환하는 실제 buffer capacity는 호출 이력에 따라 달라지므로 cold 조건과 warmed 조건을 구분한다.

| 경계 | per-message 저장소·작업 | production/library 근거 |
|---|---|---|
| encode field | 입력 N을 generator의 재사용 UTF-8 buffer로 바로 Base64 변환 1회. Base64 String·char[]·StringBuilder 0 | `ZLinkJsonMessageSerializer.java:76`; Jackson `ByteArraySerializer.java:48-49`, `UTF8JsonGenerator.java:1903-1941` |
| encode JSON output | generator→builder copy J, builder→정확한 byte[] copy J. builder 첫 buffer는 최소 2000을 recycler에서 빌리지만 builder/LinkedList/generator/provider/context는 호출마다 새 객체 | Jackson `ObjectMapper.java:4063-4074`, `ByteArrayBuilder.java:46,64,158-174`, `BufferRecycler.java:95` |
| encode output growth | 1K body(J=1379): warmed 추가 segment 0. 4K body(J=5475): 첫 capacity=2000이면 1000·1500·2250 byte segment 3개와 node 3개. release가 마지막 block을 반환하므로 반복 호출에서는 첫 block 2000→2250→2531→(1898은 최소치 미달로 2000 재할당) 순환, 추가 segment/node 수 3→3→2가 된다. 기존 segment를 늘려 복사하지 않고 마지막 coalesce 때 모은다 | `ByteArrayBuilder.java:100-104,310-337`, `BufferRecycler.java:145-157`의 재사용·block 크기 계산. 산술 값은 raw/library source에서 유도했으며 throughput 결과가 아님 |
| encoded payload ownership | `fromOwned` wrapper 1개, payload bytes clone 0. `serializeOwned`는 Message.from으로 native body allocation·J copy 1회. bench의 encodeMessage는 serializeRetained로 heap bytes를 유지해 body Message를 만들지 않고 최종 service frame에 J를 1회 쓴다 | `ZLinkJsonMessageSerializer.java:58,61-66,70-71`; `ZLinkPayloadEncoding.java:49-64`; `ZLinkFrameworkMultipartView.java:62-71,122-130`; `ZLinkServiceM6AWireCodec.java:383-390,459-463`; Message.from은 `bindings/java/src/main/java/systems/zlink/contracts/messaging/Message.java:265-281` |
| borrowed decode input | native/partial ByteBuffer이면 heap byte[J] 1개와 J copy 1회. 전체 heap array이면 이 copy 0. public `deserialize(ZLinkEncodedPayload)`는 `.bytes()` snapshot J copy 1회 | `ZLinkJsonMessageSerializer.java:89,102-116`; `ZLinkEncodedPayload.java:35-36` |
| JSON parser | UTF-8→token char buffer 변환, parser/context/duplicate detector/property buffer/record 생성. tree model 0. metadata lookup은 cache를 쓰지만 record constructor/accessor의 reflective invocation 자체는 남는다 | `ZLinkJsonMessageSerializer.java:39-46,116,128-133`; Jackson mapper/parser와 record binding |
| bytes decode (수정 전) | 결과 byte[N] + byte[B] 4개 + String 2개. 여러 token segment면 StringBuilder와 backing byte[B] 추가. bytes→Base64 재encode 1회 포함 | shared profile :362-369; `TextBuffer.java:461-505`; JDK bytecode raw |
| bytes decode (제안) | 결과 byte[N] 1개. contiguous token char buffer borrow; 여러 segment면 char[B] coalesce 1개. String·Latin-1 copy·재encode 없음 | 구현 전 제안. profile과 unit test 뒤 확인 |

L4 bench 출입 경계도 별도로 센다. source와 reply encode는 retained payload를 바로 native service frame에 쓰므로 generator→builder, builder→exact heap byte[], heap→native frame의 **J copy 총 3회**이며 Base64 transform은 1회다. 이 경로에서 body를 별도 Message로 먼저 materialize하는 copy를 추가로 계산하지 않는다. 반면 receive의 `parts.getLast().dataBuffer()`(target `ZLinkFrameworkBenchServer.java:315-317`, source `RawStack.java:469-477`)는 `ZLinkFrameworkMultipartView.get:32-43`에서 native part snapshot J copy 1회를 먼저 만든다. 이후 serializer의 native→heap J snapshot 1회가 별도다. 앞의 part materialization은 service framing/caller 비용이고 allocation probe의 deserializeBorrowed 범위 밖이다. 이 호출 조건과 bench source는 바꾸지 않았다. 관련 production 네 파일은 `df42a4df40`와 `9d9901eb2c`에서 동일함을 확인해 raw의 `ZLinkPayloadEncoding.java.txt`, `ZLinkChannelEnvelope.java.txt`, `ZLinkFrameworkMultipartView.java.txt`, `ZLinkServiceM6AWireCodec.java.txt`에 보존했다.

writer/generator는 per-call이지만 ObjectWriter wrapper 생성이나 per-call DTO reflection discovery로 이 경로를 설명하지 않는다. 소유권과 framing 때문에 생기는 별도 copy도 bytes field 자체의 Base64 변환 횟수에 합치지 않는다.

## Profile 증거

수정 전 production profile을 확인했다. L3 send 4K와 serial 1K는 `raw/before-sync-L3-*`, L4 send 4K는 `raw/before-overlap-L4-send-saturation-4096`, L4 serial 1K는 `raw/before-query-L4-request-serial-1024`에 있다. 각 cell의 source/target JFR, active stats, process 수명, 시작·중간 thread affinity/policy 및 artifact SHA-256을 보존했다. 모든 process는 taskset 6-7,14-15를 상속하며 JFR recording duration=18 s, JVM watchdog=19.6 s + kill grace 0.3 s다. **msg/s나 처리율 비율은 비교하지 않는다.** 네 cell 모두 errors=0·submitted=completed=received이며 모든 관측 thread affinity가 6-7,14-15이고 JVM 최대 수명은 18.63–19.03 s다. [검증 원문](raw/before-profile-validation.json). scheduling policy는 각 raw에 기록했으며 다른 측정 hold 때는 SCHED_IDLE로 실행한 profile이다. counted throughput run으로 사용하지 않는다.

| 실제 증거 | 관측 |
|---|---|
| [L4 send target JFR summary](raw/before-overlap-L4-send-saturation-4096/target.summary.json) | active window CPU/native sample 528개 중 typed decode stack에 202개. allocation stack에 StringUTF16.compress, String.encode8859_1, Arrays.copyOfRange(String backing), Base64$Decoder.decode, Base64$Encoder.encode, decodeJson:109의 whole JSON heap copy가 나타난다 |
| [L4 send source summary](raw/before-overlap-L4-send-saturation-4096/source.summary.json) | ByteArrayBuilder.toByteArray/_allocMore, UTF8JsonGenerator, DefaultSerializerProvider, IOContext, LinkedList.Node가 나타난다. 입력 bytes→JSON String→UTF-8 경로가 아니라 streaming generator와 builder copy 경로다 |
| [L3 summaries](raw/before-sync-L3-all-summaries.json) | 같은 service frame·binding 경로를 실행하지만 typed JsonMessageSerializer/profile의 CPU·allocation stack은 없다 |
| [고정 횟수 allocation counter](raw/allocation-before.csv) | ThreadMXBean으로 각 크기·operation을 5000번 warm한 뒤 5000번의 thread allocated bytes를 읽었다. 1K encode 1848 B/message, heap decode 7464 B/message, native decode 8860 B/message. 4K encode 11184.5 B/message, heap decode 26920 B/message, native decode 32360 B/message |

JFR allocation weight는 sampling 추정치이므로 site별 정확한 allocation 수나 message당 B로 나누지 않는다. per-message 값은 고정 횟수 counter이며 String/payload buffer 개수는 library source와 bytecode로 대조한다. counter의 ByteBuffer wrapper 등 control object는 JIT scalar replacement에 따라 달라질 수 있다. B=0에서는 empty String/backing 공유가 있어 일반 N>0의 buffer 개수를 그대로 적용하지 않는다.

[호출 비교](call-shape.md), [profile 설정](raw/profile.jfc), [실행 script](raw/profile-bench.py), [allocation probe](raw/TypedBytesAllocationProbe.java), [production 소스 동일성](raw/production-base-parity.json). 준비 실패·cap에 의해 잘린 자료는 유효 cell과 구분하고 삭제하지 않는다.

### Scratch L4 선택 조건

9d9901eb2c의 `framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchClient.java:158-162`는 mode selector에서 L4를 누락한다. L4 단독 flag는 ready probe가 잘못된 route 경로로 가므로 profile에 쓰지 않는다. 소스 수정 없이 L3+L4 flag를 함께 주면 mode selector가 binding-shaped 경로를 선택하고 `FrameworkStack.java:136-143`, target `ZLinkFrameworkBenchServer.java:202-207`는 L4를 우선한다. DTO·production codec·측정 조건을 바꾸지 않는다. `before-*`의 준비 실패 raw는 남기며 유효 profile과 섞지 않는다.

## 교차언어 비교

비교 단위는 단일 bytes field DTO의 정상 encode/decode 한 번이다. `N/B/J`는 위 Java 표와 같다. 표의 allocation 수는 payload 크기에 비례하는 backing storage를 센다. pool의 cold allocation과 metadata/control 객체는 별도 열에 적으며 source만으로 runtime 전체 객체 수를 고정하지 않는다.

Repository 위치의 prefix: C++ `framework/languages/cpp/`, .NET `framework/languages/dotnet/`, Node `framework/languages/node/`. 다음 파일은 공통 HEAD `df42a4df40`와 각 ladder commit(C++ `62e766f88d`, .NET `29d9fca82b`, Node `7d1afa98a9`)에서 내용이 같았다. raw의 `*-ladder-source-*.txt`에 해당 commit·line을 보존했다.

| 항목 | C++ | .NET | Node |
|---|---|---|---|
| 기본 codec 선택 | `framework/include/zlink/framework/contracts/codecs/serializer.hpp:345-375`가 typed JSON writer/reader를 선택. 등록 API 추가 없음 | `src/Zlink.Framework/Runtime/Messaging/ZLinkFrameworkJsonPayloadCodec.cs:10-33,40-47`가 STJ UTF-8 codec을 호출 | `packages/framework/src/runtime/channels/channel-envelope.ts:498-508,535-557` → shared profile |
| bytes encode 중간 저장소 | `common/include/zlink/json_profile.hpp:260-273,518-519`: Base64 `std::string(B)` 1개를 정확히 reserve한 뒤 JSON string에 복사. UTF-8 재encode 0 | STJ `ByteArrayConverter.cs:28` → `Utf8JsonWriter.WriteValues.Bytes.cs:50-84`: Base64를 writer의 byte span에 직접 기록. Base64 String·char[]·중간 byte[B] 0 | committed profile `packages/stream-wire/src/internal/framework-json-v1.ts:63-64,394-399`: Uint8Array subarray view → spread argument → binary String(N) → btoa의 Base64 String(B). 1K·4K는 chunk 1개. Buffer 입력이면 replacer 전에 Buffer.toJSON이 임시 object+N원소 Array를 생성·채움(runtime buffer.js:1154-1161). 그 임시 결과는 replacer에서 버린다. JSON.stringify로 전체 JSON String(J) 생성(:21-28). 최종 wire는 numeric JSON array가 아님 |
| JSON output storage·copy | 같은 파일 :29,909-915의 JSON `std::string` 초기 capacity 256, 성장 시 realloc/copy. :160-171의 serializer `from_text`는 string을 heap wrapper로 move하고 external native Message로 채택하므로 최종 J copy 0 | `JsonSerializer.Write.ByteArray.cs:129-157`: writer output→exact byte[J] copy 1회. framework codec의 FromOwned :40-41은 복사하지 않음. Message로 materialize하는 API에서는 `ZLinkEnvelopeCodec.cs:355,857-862`의 Message.From 및 `bindings/dotnet/src/Zlink/Runtime/Messaging/Message.Native.cs:328-335`에서 native allocation·J copy 1회 | channel encoder는 JSON String을 MessageLike로 반환(:539). binding `bindings/node/src/zlink/contracts/messaging/message.ts:67-75,105-114`의 String→Buffer.from UTF-8 변환 1회, native Message snapshot copy 1회. 이 경계 밖 service frame copy는 별도 |
| bytes decode 중간 저장소 | `json_profile.hpp:882-886,805-807`의 SAX token string을 view로 전달 → :275-300의 vector에 직접 decode. token storage 1개와 vector growth. vector는 reserve하지 않아 capacity가 늘 때 decoded prefix를 복사. Base64 재encode·JSON DOM 0 | `ZLinkFrameworkJsonPayloadCodec.cs:12-13`은 document token 검증 뒤 typed deserialize. `ByteArrayConverter.cs:17` → `Utf8JsonReader.TryGet.cs:827-842` → `JsonReaderHelper.Unescaping.cs:218-247`: UTF-8 span에서 pooled/stack scratch로 decode하고 byte[N] 결과로 copy 1회. 일반 unescaped bytes field에는 Base64 String·JSON DOM 0 | `channel-envelope.ts:413-448`의 body text decode와 shared profile :79-90의 scanner+JSON.parse. bytes schema :137-144일 때 Base64 String(B) → atob의 binary String(N) → 새 Uint8Array(N)에 loop copy(:402-405). schema가 없으면 JSON.parse 결과 String을 유지하며 typed bytes 변환을 자동 추정하지 않음 |
| canonical 검증 | :277-293에서 길이·alphabet·padding bit를 decode 중 검사. **Java 재encode 원인 없음** | STJ bytes converter 경로에는 Java의 encodeToString 비교가 없음. **Java 원인 없음**. canonical pad-bit 거부 여부를 이번 작업에서 추가하거나 완화하지 않음 | :97,137-144,386-391의 regex + 마지막 sextet bit 검사. **Java 재encode 원인 없음** |
| metadata·writer | registry serializer cache :324-332, compile-time dto_traits. 매 호출 JSON string writer와 SAX handler/frame state는 생성; runtime reflection discovery 없음 | shared options `src/Shared/ZLinkJsonSerializerOptions.cs:22,33-50`와 STJ type metadata cache. document validator는 object마다 HashSet·property name을 만든다(`ZLinkFrameworkJsonDocument.cs:38-52`). writer/pool 정책은 STJ version에 따름 | module-level replacer(:42-50), scanner object와 per-object Set(:88,266). JSON tree는 JSON.parse로 생성. DTO metadata를 message마다 reflection으로 조사하는 경로 아님 |
| native receive copy | serializer :370-373은 payload span을 직접 SAX parser에 전달. whole JSON snapshot 0 | `ZLinkEnvelopeCodec.cs:795-809,833-836`는 기본 span deserializer로 native payload를 읽는다. whole JSON snapshot 0 | channel payload는 borrowed Buffer(:409-410), text conversion은 decode의 UTF-8→String 1회. 이후 bytes field decode는 위와 같음 |

C++의 동적 storage 수는 allocator/container 구현에 의존한다. 이 소스의 `std::vector` doubling을 사용하는 libstdc++에서는 새 DTO의 N=1024/4096 decode가 각각 11/13번 storage allocation, 성장 때 합계 1023/4095 bytes prefix copy를 요구한다. JSON string doubling은 J=1379/5475에 각각 4/6 storage allocation(초기 256 포함), 3/5 성장 copy다. Base64 임시 string allocation 1개, external string wrapper allocation 1개가 별도다. 이는 source에서 유도한 container 비용이며 이 작업에서 C++ profile이나 실행으로 측정한 값은 아니다.

**Node Buffer 경로 확인:** 위 committed 운영 코드는 `Buffer.toString('base64')` / `Buffer.from(text,'base64')`를 직접 호출하지 않는다. Node runtime의 Buffer base64 dispatch는 [runtime source snapshot](raw/node-runtime-buffer.js.txt):688-689, btoa/atob는 :1283-1316에서 native `_btoa/_atob`로 간다. 해당 runtime은 그 내부에 JS Buffer copy가 있다고 단정할 수 없다. 직접 Buffer base64를 사용하는 경로와 현재 framework의 binary String 경로는 구분해야 한다. Node 최적화는 이번 작업에서 수행하지 않는다.

**Node ladder의 Buffer Base64 경로:** `7d1afa98a9:framework/bench/grpc/node/shared/raw-wire.js:119-138`은 production channel encoder가 만든 JSON String을 `Buffer.from(...,'utf8')`로 바꿔 Message.from에 넘긴다(J encode 1회 + native J copy 1회). :141-148은 먼저 `Buffer.from(encoded)`로 whole JSON J copy 1회, production decode에서 UTF-8→JSON String 변환 1회 후, schema가 없는 body String을 **`Buffer.from(payload.body,'base64')`로 N bytes에 직접 decode**한다. reply도 :151-154에서 같은 Buffer Base64 decode를 쓴다. 이 decode의 payload 크기 storage는 JSON String(J), parsed Base64 String(B), output Buffer(N)이며 binary String(N)이나 재encode 검증은 없다. runtime `buffer.js:451-481,623-631,684-689`가 padded 길이를 계산하고 base64Write를 output storage에 직접 수행한다. Buffer의 slab allocation과 wrapper/view 수는 runtime pool 상태에 따라 다르며 매 message 독립 heap backing N개라고 세지 않는다. 이 bench helper는 production schema의 atob→Uint8Array typed bytes path와 다르다. wire 표현이 같아도 Java bytes deserializer와 allocation shape가 동일하지 않다. 원문은 [Node bench](raw/node-bench-raw-wire.txt)에 보존했다.


.NET library `file:line`은 [source.dot.net snapshot](https://source.dot.net/System.Text.Json/System/Text/Json/Serialization/Converters/Value/ByteArrayConverter.cs.html)의 code 첫 줄을 1로 세어 raw의 `dotnet-*.txt`에 보존했다. repository 기본 target은 net8.0이다(`Directory.Build.props:3`). upstream current source의 writer cache 객체 수를 installed .NET 8의 실제 allocation 수라고 제시하지 않는다. 표에서 확정한 것은 직접 UTF-8 bytes 처리, scratch/result의 copy 경계, framework의 span/Message 경계와 재encode 부재다.

## 구현·검증 결과

구현 전 보고서 snapshot은 raw/report-before-implementation.md에 남겼다. scratch production decoder 수정 및 작업 branch의 회귀 test 추가 후, quiet guard로 build/test가 정지된 상태다. 최종 검증에서는 unit test로 0,1,2,3,127,128,1024,4096 bytes의 JSON byte-identical 결과·roundtrip·invalid Base64 rejection 및 token String 미생성을 검증한다. Java format check와 touched path framework tests를 수행한 뒤 결과와 local commit을 기입한다.
