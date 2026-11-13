# .NET L4 typed JSON — 원인과 production 수정 제안

**완성된 JSON byte[]의 불필요한 materialization과 Base64 decode의 임시 버퍼·복사가 확인됐다.** wire는 기존 padded RFC 4648 Base64를 유지한다. 아래는 production 수정 전의 진단과 제안이며, 구현 결과는 뒤에 덧붙인다. throughput 손실 중 이 두 비용의 기여율이나 7% 통과 여부는 이번 profile로 판정하지 않는다.

## 범위와 실행 조건

- 작업 branch `perf/4354-dotnet-typed-json`, 시작 HEAD/base `3bd8756f993ee1f003321f46b68ad21d82ea5dec`, clean. 로컬 commit만 한다.
- 지정 ladder tip `29d9fca82b`를 scratch에 archive하여 L4 symbol로 source build했다. runtime level을 L3/L4로 선택했다. 두 단계의 production serializer 소스는 작업 base와 같다.
- scratch `/root/worktree/zlink-v21-s033/d4json-scratch`, 전 프로세스와 thread 허용 CPU는 6-7,14-15의 부분집합이다. 각 target/source 프로세스에는 19초 watchdog을 뒀다. dotnet-trace CPU sampling + GC verbose allocation ticks는 역할별 6초다. warmup 2초·active 5초는 호출 형태 확인용이며 ladder throughput 측정 조건과 다르다.
- measure.lock, GitHub, internal-docs.sh, 다른 worktree/build output, sub-agent 및 다른 AI 도구를 사용하지 않았다. raw를 삭제하지 않았다. [호출 대조](call-shape.md), [build client](raw/build-client-l4.log), [build target](raw/build-server-l4.log), [진행·artifact hash](progress.md).

## 1. profile 증거

CPU 표는 EventPipe `Type=Managed` sample의 inclusive stack 출현 수다. 외부/native 대기 sample을 CPU 시간으로 합산하지 않았다. GC tick의 bytes는 sampling 추정치이며 메시지당 exact allocation은 별도 probe에서 측정했다. 원본 `.nettrace`, event 분석 JSON, thread affinity·scheduling, artifact SHA-256, 결과 counter를 각 행의 raw 디렉터리에 보존했다.

| 셀 | source / target 실행 초 | source: JSON bytes materialization / Base64 decode | target: JSON bytes materialization / Base64 decode | 검증 |
| L3 send-saturation 4K | 공통 run 16.42 | 0 / 0 | 0 / 0 | errors 0, submitted=completed=received ([raw](raw/baseline-3/L3-send-saturation/manifest.json)) |
| L4 send-saturation 4K | 공통 run 15.11 | 90 / 0 | 0 / 163 | errors 0, submitted=completed=received ([raw](raw/baseline-3/L4-send-saturation/manifest.json)) |
| L3 request-backpressure 4K | 공통 run 14.08 | 0 / 0 | 0 / 0 | errors 0, submitted=completed=received ([raw](raw/baseline-3/L3-request-backpressure/manifest.json)) |
| L4 request-backpressure 4K | 공통 run 14.40 | 33 / 79 | 65 / 106 | errors 0, submitted=completed=received ([raw](raw/baseline-3/L4-request-backpressure/manifest.json)) |

L3에도 bench 제어 HTTP·결과 DTO의 JSON 처리는 있다. L4에서 추가된 stack은 production `ZLinkFrameworkJsonPayloadCodec` → `JsonSerializer.WriteBytesAsObject`, 그리고 typed body decode의 `JsonReaderHelper.TryDecodeBase64`다. allocation tick stack은 같은 메서드에서 `System.Byte[]`가 생성됨을 확인한다. [L4 send source](raw/baseline-3/L4-send-saturation/source-managed-profile.json), [L4 send target](raw/baseline-3/L4-send-saturation/target-managed-profile.json), [L4 backpressure source](raw/baseline-3/L4-request-backpressure/source-managed-profile.json), [L4 backpressure target](raw/baseline-3/L4-request-backpressure/target-managed-profile.json).

### 메시지당 allocation·copy

사전 생성한 `record Payload(byte[] Body)`로 실제 production 메서드를 호출했다. thread-local allocated bytes를 warmup 뒤 2,000회 차이로 나누었고, payload/JSON 준비와 loop closure는 측정 밖이다. 아래 값은 동일 thread의 managed allocation이다. native Message storage와 pool의 기존 버퍼는 포함하지 않는다. 실행 runtime/STJ는 8.0.31이다. [전체 원본](raw/allocations-before.txt), [probe source](tools/codec-probe.cs).

| 논리 bytes | JSON bytes | Serialize bytes B/op | JSON part B/op | typed decode B/op | 사전 검증 B/op |
|---:|---:|---:|---:|---:|---:|
| 0 | 11 | 40 | 72 | 408 | 208 |
| 1,2,3 | 15 | 40 | 72 | 440 | 208 |
| 127,128 | 183 | 208 | 240 | 560 | 208 |
| 1024 | 1379 | 1408 | 1440 | 1456 | 208 |
| 4096 | 5475 | 5504 | 5536 | 4528 | 208 |

4K JSON part의 5,536 B/op 중 완성된 JSON array가 5,504 B다. typed decode는 결과 byte[] 4,120 B, DTO·constructor 처리·검증을 합쳐 4,528 B다. 검증은 object당 HashSet, bucket/entry 배열, property name string에 208 B를 쓴다. 이는 유지할 계약 검증 비용이며 이번 수정에서 중복 property 검사를 없애지 않는다.

| 단계 | production 위치 — 수정 전 | 4K당 allocation / copy |
|---|---|---|
| options·metadata | `src/Shared/ZLinkJsonSerializerOptions.cs:22,33-51` | 정적 options 1개, resolver metadata는 STJ cache. 메시지마다 options/resolver를 만들지 않는다. reflection 기반 계약이지만 profile은 매 호출 reflection 생성을 원인으로 지지하지 않는다. |
| Base64 encode | `src/Zlink.Framework/Runtime/Messaging/ZLinkFrameworkJsonPayloadCodec.cs:26-34` → STJ ByteArrayConverter | `Utf8JsonWriter.WriteBase64StringValue`가 UTF-8 writer 버퍼에 직접 기록한다. Convert.ToBase64String, 중간 string/char[] 없음. [사용 runtime decompile](raw/stj-bytearrayconverter.cs) |
| JSON materialization | 같은 codec `:26-34`, envelope `ZLinkEnvelopeCodec.cs:855-872` | STJ의 pooled writer → 새 5,475-byte JSON array 복사 1회(5,504 managed B), 다음 Message.From에서 native storage로 5,475 bytes 복사 1회. payload 전체 복사 2회. writer 성장 시 기존 prefix 복사도 가능하다. |
| JSON typed decode 진입 | codec `:10-23`, envelope `:793-810,831-836` | native span을 직접 읽는다. default span deserializer 경로에서 body.ToArray는 호출하지 않는다. |
| 사전 검증 | `src/Shared/ZLinkFrameworkJsonDocument.cs:13-64` | JSON token scan 1회 뒤 STJ parse 1회. 중복 property·finite number 계약을 검증한다. |
| Base64 decode | codec `:13,19-23` → shared options `:33-51`의 BCL byte[] converter | 5,464 encoded bytes 때문에 pooled byte[8192]를 빌린다. 그 버퍼에 4,096 bytes decode → 결과 byte[4096]로 복사 1회 → 임시 span 8,192 bytes clear → pool 반환. 중간 string/Convert.FromBase64String 없음. [runtime 구현](/root/jobs/fx/cx/v21/s033/d4json/raw/stj-jsonreaderhelper.cs:295) |
| native Message 소유 | binding public Message.From → `bindings/dotnet/src/Zlink/Runtime/Messaging/Message.Native.cs:328-336` | native snapshot 복사 1회. binding의 snapshot 계약이며 제거하거나 우회하지 않는다. |

production 경로의 시작 상태는 [file:line 발췌](raw/production-path-before.txt)에 고정했다. L4 bench source의 `Body.ToByteArray()`와 reply의 `ByteString.CopyFrom()`은 각각 4K array 복사다. 이는 serializer 밖의 DTO bridge 비용이며 그대로 둔다. service-wire framing의 추가 복사도 L3/L4 공통이다. Base64로 body가 커지는 4,096→5,464 bytes와 DTO JSON syntax는 wire 계약의 비용이다.

## 2. 결정 소유 표와 교차언어 확인

다른 언어는 현재 base commit `3bd8756f99`를 `git show`로만 읽었다. 아래 경로는 `framework/languages/` 기준이다. 구현 방식·allocation 수의 차이는 wire 동작 차이가 아니다.

| 결정 / spec | .NET의 판정 위치 | C++ | Java | Node | 하나로 유지할 소유자 |
|---|---|---|---|---|---|
| bytes ↔ padded Base64, message-model §5 `:136-153` | `dotnet/src/Shared/ZLinkJsonSerializerOptions.cs:33-51` → BCL byte[] converter, codec `:26-34` | `cpp/common/include/zlink/json_profile.hpp:260-310,519,805-807` | `java/zlink-framework-json-internal/.../ZLinkFrameworkJsonProfile.java:352-378` | `node/packages/stream-wire/src/internal/framework-json-v1.ts:63-64,394-406` | Framework typed JSON options/serializer. byte[] field contract를 application/bench로 내보내지 않는다. |
| 기본 typed serializer 선택, 같은 §5 | .NET codec `:5-8`, envelope `:339-358,839-852` | `cpp/framework/include/zlink/framework/contracts/codecs/serializer.hpp:345-375` | `java/zlink-framework-core/.../ZLinkJsonMessageSerializer.java:40-46,74-81` | 위 `framework-json-v1.ts:20-38,85-98` | 각 언어 기본 serializer. 등록 codec·DTO 변경 없음. |
| 중복 property·finite JSON 검증, 같은 §5 `:145,152` | `dotnet/src/Shared/ZLinkFrameworkJsonDocument.cs:36-63` | 공통 json_profile parser | 위 Java profile `:157-180` | 위 Node profile `:85-98,225-295` | 기존 JSON profile 검증을 유지한다. |
| native Message snapshot storage | envelope `:855-862` → binding Message.From | 각 binding Message 소유 | 각 binding Message 소유 | 각 binding Message 소유 | binding public Message API. Framework는 temporary JSON 버퍼만 소유한다. |

스펙은 `framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md:136-153`이다. 네 언어의 encode 결과 의미는 padded RFC 4648 Base64이며, 이번 변경은 출력 bytes와 기존 .NET decoder의 입력 수락·거부 결과를 보존한다. 새 wire/spec/public API가 필요하지 않다. 언어별로 새로운 payload 규칙을 추가하지 않는다.

## 3. production 수정 제안 — 분류 B

**소유 계층:** Framework typed JSON serializer. Core·binding·transport·application의 결정은 바꾸지 않는다.

1. native JSON part가 필요한 `ZLinkEnvelopeCodec.EncodeJsonPart`를 기존 codec 소유의 `Utf8JsonWriter` + 호출별 pooled IBufferWriter로 연결한다. writer의 기존 기본 JSON options·선언 type·encoder·depth를 유지하고, 완성된 buffer span을 public Message.From에 한 번 전달한다. 최종 managed JSON array를 생성하지 않는다. bytes 반환 API는 기존 owned-byte[] 계약이 필요하므로 유지한다. pool의 버퍼는 성공·실패 모두 반환하고 사용 구간을 지운다.
2. shared FrameworkPayload options의 byte[] converter를 같은 byte 표현의 converter로 교체한다. Write는 기존 WriteBase64StringValue 그대로다. contiguous·unescaped Base64의 정확한 결과 크기를 계산해 최종 array에 BCL Base64.DecodeFromUtf8로 직접 기록한다. encoded 길이·실제 consumed/written이 정확히 맞지 않거나 escaped/segmented input이면 기존 reader.GetBytesFromBase64로 위임한다. 이는 메모리 배치에 따른 처리 경로이며 새로운 입력 수락 정책이 아니다. 기존 BCL과 differential test로 입력 수락·거부 결과를 비교한다.
3. required/nullability·중복 property·finite number 검증, bench DTO와 source bridge, service wire, binding Message copy는 유지한다. 임시 buffer owner는 기존 managed JSON 중간 결과를 대체하며, 영구 cache·flag·queue·lock·타이머를 추가하지 않는다.

| 대안 | 판정 |
|---|---|
| source generation / DTO 전용 codec / bench 수동 Base64 | cache가 이미 있는 resolver를 바꾸는 근거가 약하다. 메시지별 계약·등록이나 DTO 특수 경로가 필요하므로 채택하지 않는다. |
| 기본 serializer 내부의 출력 storage와 Base64 decode destination 수정 | typed JSON과 BCL Base64 알고리즘을 재사용하고, 불필요한 소유 결과·복사만 제거한다. 채택한다. |

**규칙 수:** native JSON part의 소유 payload materialization 판정 위치 2(STJ owned JSON array, binding Message)→1(binding Message). byte 표현 판정 위치 1(default typed JSON options)→1(같은 options). 합계 3→2. 임시 writer 버퍼는 반환할 storage이며 별도 payload 소유 결과가 아니다. 입력 형태에 따른 decoder 처리 경로는 기존 BCL의 수락·거부 규칙을 변경하지 않는다.

**변경 분류 B:** wire·API·계약 적응이 아니라 기존 production 경로의 불필요한 allocation/copy 제거다. 새 spec가 필요한 상태(D)나 application 보상 경로(C)가 아니다. 사용자 지시의 “원인·수정 제안 먼저, 이후 구현”에 따라 이 보고서를 먼저 기록하고 위 범위에서 구현한다.

## 4. 구현·검증 예정

- byte-array field 0,1,2,3,127,128,1024,4096 bytes의 기존 JSON과 byte-identical output 및 decoded value를 unit test로 고정한다. bytes API, native part API와 envelope/default serializer 경로를 비교한다.
- escaped/segmented/whitespace/invalid Base64를 BCL과 대조하고, malformed/중복 field·required/nullability 거부가 그대로인지 검사한다.
- pool growth·serializer exception·snapshot lifetime을 검사한다. 수정 전후 동일 allocation probe와 L4 두 셀 profile 형태를 비교한다.
- .NET format check와 touched path를 덮는 Framework JSON·connector JSON·envelope·codec·message ownership unit tests를 실행한다. 최종 결과와 남은 실패를 이 보고서에 덧붙인다.

리팩토링 사전 점검: 불필요한 중간 storage 2건을 확인했다(JSON output array, Base64 temporary decode output). 같은 작업에서 고친다. 중복 property 검증과 owned byte[] 반환은 각각 다른 계약을 소유하므로 제거하지 않는다.

## 5. 구현 결과

production 수정은 아래 세 파일에만 적용했다. bench와 DTO는 변경하지 않았다.

| 파일 | 변경 |
|---|---|
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkFrameworkJsonPayloadCodec.cs:37-111` | native part용 serialization은 호출별 pooled writer → public Message.From 한 번으로 끝낸다. 사용한 버퍼 구간은 성공·실패 모두 지우고 반환한다. BCL과 같은 writer encoder·indent·depth·skip-validation 값을 사용한다. |
| `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkEnvelopeCodec.cs:855-862` | 두 EncodeJsonPart overload를 codec의 native-part 경로로 연결한다. custom serializer 선택과 owned bytes 반환 경로는 그대로다. |
| `framework/languages/dotnet/src/Shared/ZLinkJsonSerializerOptions.cs:284-327` | 기본 byte[] converter가 정확한 길이의 결과 array에 직접 decode한다. output은 WriteBase64StringValue 그대로이며, 최적화 경로가 처리하지 못하는 입력은 기존 BCL reader로 넘긴다. 이 shared options를 쓰는 connector에도 같은 경로가 적용된다. |
| `framework/languages/dotnet/tests/Zlink.Framework.UnitTests/Runtime/TypedJsonPayloadStorageTests.cs` 및 같은 project의 csproj | 요청한 8개 크기, generic/declared type bytes와 native part, default envelope, connector의 byte-identical JSON·decoded bytes를 검증한다. BCL differential 입력 26개, segmented JSON 3개, pool growth·snapshot·exception 및 allocation 회귀를 추가했다. 총 40개 사례다. |

### 실제 allocation·copy 변화

같은 probe binary의 dependencies만 전후 production assemblies로 교체했다. [before](raw/allocations-before.txt), [after](raw/allocations-after.txt), [assembly hashes](raw/probe-assembly-hashes.json). 아래는 native JSON part의 managed B/op이며, decode allocation은 전후가 같다.

| 논리 bytes | 수정 전 B/op | 수정 후 B/op | typed decode 전후 B/op |
|---:|---:|---:|---:|
| 0 | 72 | 184 | 408 |
| 1 | 72 | 184 | 440 |
| 2 | 72 | 184 | 440 |
| 3 | 72 | 184 | 440 |
| 127 | 240 | 184 | 560 |
| 128 | 240 | 184 | 560 |
| 1024 | 1440 | 184 | 1456 |
| 4096 | 5536 | 184 | 4528 |

4K part는 **5,536→184 B/op, 96.7% 감소**다. steady-state encode의 object 수는 JSON array + Message wrapper 2개에서 pooled writer owner + Utf8JsonWriter + Message wrapper 3개로 늘지만, payload 크기에 비례하는 managed array를 제거했다. 작은 0~3-byte payload는 72→184 B/op로 증가한다. 이 trade-off는 숨기지 않으며 1K/4K L4의 per-byte 비용을 줄이는 이번 범위의 한계다. owned byte[]를 반환해야 하는 Serialize/ToJson API의 allocation은 유지한다.

4K 단일 positional-record body의 typed decode는 전후 **10개 managed object**다: 결과 byte[] 1개, DTO 1개, ArgumentState 1개, constructor argument cache(Arguments<T0,T1,T2,T3>) 1개, required-property BitArray와 backing int[] 2개, document 검증의 HashSet/bucket/entry/property-name string 4개다. 4,120 + 24 + 64 + 48 + 64 + 208 = 4,528 B/op이며 exact probe와 맞는다. empty body에서는 Array.Empty를 쓰므로 결과 array allocation 1개가 빠진다. [ArgumentState](raw/stj-argumentstate.cs), [constructor argument cache](raw/stj-small-constructor-converter.cs), [required BitArray](raw/stj-readstackframe.cs), [validation](raw/production-path-before.txt).

4K당 완성 JSON 복사는 5,475 bytes 두 번→한 번이다. pooled writer 성장 시 작은 prefix 복사는 남는다. Base64 transform 자체는 전후 같지만 일반 contiguous·unescaped 입력의 decode는 임시 8K 버퍼 rent/clear와 decoded 4K 결과 복사를 없앴다. 최종 byte[4096] 4,120 B와 DTO·문서 검증 비용은 남긴다.

### 수정 후 profile

각 run은 CPU 6-7,14-15, 19초 watchdog, 역할별 6초 trace, 같은 4K payload와 bench args다. 모든 source/target에서 errors=0과 submitted=completed=received를 확인했다. trace는 throughput 판정에 사용하지 않는다.

| 셀 | run 초 | source JSON array materialization / BCL temporary Base64 decode stack | target 같은 stack | raw |
| L4 send-saturation | 11.70 | 0 / 0 | 0 / 0 | [manifest](raw/after-1/L4-send-saturation/manifest.json), [source](raw/after-1/L4-send-saturation/source-managed-profile.json), [target](raw/after-1/L4-send-saturation/target-managed-profile.json) |
| L4 request-backpressure | 14.66 | 0 / 0 | 0 / 0 | [manifest](raw/after-1/L4-request-backpressure/manifest.json), [source](raw/after-1/L4-request-backpressure/source-managed-profile.json), [target](raw/after-1/L4-request-backpressure/target-managed-profile.json) |

BCL Base64.EncodeToUtf8 / DecodeFromUtf8 primitive stack은 남아 있고, JSON bytes materialization allocation stack과 임시 Base64 decode helper stack만 사라졌다. inline된 converter가 sample에 이름으로 나타나지 않는 경우에도 direct Base64 primitive와 payload decode 경로를 확인했다. 7% step budget과 전체 손실 기여율은 측정하지 않았다.

## 6. 검증과 남은 문제

| 검증 | 결과 | 증거 |
|---|---|---|
| allocation 회귀 검사의 양방향 확인 | 수정 전 5,536 B/call로 실제 실패, 수정 후 184 B/call로 통과. 경계 <1,024 B/call. | [before 탐색 실행](raw/test-baseline-regression.log), [after 탐색 실행](raw/test-focused-first.log), [최종 suite](raw/test-framework-serializer.log) |
| 요청한 bytes 크기의 wire/value | 0,1,2,3,127,128,1024,4096 모두 전후 byte-identical output·decoded bytes 통과 | 같은 before/after 탐색 log와 최종 suite |
| touched path Framework tests | **321/321 통과**, 새 performance regression 40개 포함. FrameworkJsonProfileTests, ConnectorJsonProfileTests, EnvelopeCodecTests, CustomSerializerEnvelopeTests, EnvelopeCodecAllocationRegressionTests, MessagePayloadOwnershipConvergenceTests, EnvelopeHeaderCacheHotPathTests, RouteCodecTests, ChannelEnvelopeHandoffTests | [test-framework-serializer.log](raw/test-framework-serializer.log) |
| shared connector build | net8.0·netstandard2.1, errors 0 | [build-connector-targets.log](raw/build-connector-targets.log) |
| .NET 전체 format check | **1,281 files 통과**, exit 0 | [format-check.log](raw/format-check.log) |
| whitespace/error check | git diff --check 통과 | 최종 commit 전 확인 |

**기존 결함 1건:** non-nullable positional record `BytePayload(byte[] Body)`에 `{"body":null}`을 주어도 예외가 나지 않는다. 수정 전·후 확장 탐색에서 같은 실패다. 원인은 `src/Shared/ZLinkJsonSerializerOptions.cs:140-145`의 non-null 검사가 property.Set에만 적용되지만 positional record는 constructor argument로 값을 받는 경계에 있다. 이번 수정은 그 경계를 바꾸지 않는다. 추가 탐색 suite 44개 중 before는 allocation + nullability 두 사례 실패, after는 nullability 한 사례만 실패했다. [unchanged 탐색 source](raw/TypedJsonPayloadExplorationTests.cs), [실패 사례·해석](raw/record-nullability-exploration.txt), 위 before/after log에 보존했다.

이 확장 validation probe는 요청한 성능 회귀 suite 밖의 raw에 그대로 보존했다. 기존 repository test assertion/fixture는 변경하지 않았고, nullability 거부로 expectation을 낮추거나 입력을 바꾸지 않았다. 최종 Framework suite에는 기존 required/nullability·중복 property·finite number 검사도 그대로 포함한다. wire 수락 정책을 바꾸지 않는 이번 작업에서 이 record nullability 결함은 미수정 문제로 남긴다. GitHub 접근 금지에 따라 별도 Issue를 열지 않고 #4354 작업 보고서에만 기록했다.

소유 계층: Framework 기본 typed JSON serializer. binding의 native Message snapshot 계약을 그대로 사용한다.

스펙 조항: message-model §5, `05-message-model.ko.md:136-153` — UTF-8·typed payload·padded RFC 4648 Base64 유지.

교차언어 대조: C++·.NET·Java·Node의 byte sequence 표현을 확인했고, encode wire 의미와 기존 .NET 입력 수락·거부 결과는 변경하지 않았다.

변경 분류: **B — 기존 production 경로의 불필요한 buffer materialization/copy 제거.** spec/wire/public API 변경 없음.

규칙 수: owned JSON payload materialization 판정 위치 2→1, byte 표현 판정 위치 1→1, 합계 **3→2**. writer의 임시 storage·written count는 삭제한 중간 JSON array를 대체하며 영구 상태·flag·queue·lock은 추가하지 않았다.

리팩토링 점검: 발견 3건, 수정 2건(JSON owned array, Base64 temporary output), 이월 1건(기존 positional record nullability). gate/lifecycle/transport 제어의 새 판정이나 중복 상태는 없다.
