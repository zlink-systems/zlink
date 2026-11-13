# Node typed JSON — Buffer.toJSON 제거와 native Base64 decode

Node의 일반 data graph에서 bytes를 공용 JSON 정책으로 먼저 변환하여, `Buffer.toJSON`의 버려지는 Array(N)과 wrapper를 만들지 않는다. 수신 bytes schema는 기존 canonical 검증 뒤 Node native decoder를 사용한다. Wire·DTO·codec 선택·공용 finite/64-bit/Date/toJSON 정책은 유지했다.

## 범위와 판정 근거

- 작업 브랜치 `perf/4354-node-typed-json`, 기준 commit `6e48bdf5fbfe7252b029b405f514693a91f2a92d`. 시작 시 clean이었다. 앞 작업의 [보고서](../n4json/report.md)와 [감독 메모](../n4json/SUPERVISOR-NOTE.md)를 먼저 읽었다.
- 지정 primary와 `/root/worktree/zlink-v21-s033/n4json-scratch`만 사용했다. Scratch는 앞 작업의 `ae8a5178b5` archive와 설치·공식 build를 재사용했다. Baseline의 두 runtime TS 파일이 6e48bdf5fb와 같은지 [SHA 검증](raw/baseline-source-integrity.json)했다.
- 모든 profile·build·test와 하위 process는 `taskset -c 6-7,14-15`를 상속했다. L4의 모든 thread affinity는 각 cell의 `scheduling.txt`에 보존했다. 외부 hold로 정지한 build/test에는 재개 신호를 보내지 않았다.
- measure.lock, GitHub, push, PR, sub-agent, 다른 AI 도구, internal-docs.sh 사용 없음. Raw는 실패 자료까지 보존했다. .NET STJ source는 공식 source.dot.net에서 읽고 HTML/text 원본을 보존했다.

## 결정 소유와 교차언어 대조

아래 Node 기준 line은 baseline source 기준이다. 네 언어에서 byte sequence를 padded Base64 JSON string으로 쓰는 계약은 같다. Java와 Kotlin은 같은 JVM JSON 구현을 사용한다. 이번 수정은 Node 구현 비용만 바꾸며, 다른 언어의 decoder accepted set을 변경하거나 같다고 판정하지 않는다.

| 결정 / spec | Node baseline | .NET | Java / Kotlin | C++ | 수정 뒤 소유자 |
|---|---|---|---|---|---|
| 기본 typed JSON 선택 / message-model §5 | `channel-envelope.ts:498-508` | `ZLinkFrameworkJsonPayloadCodec.cs:26-33` | `ZLinkJsonMessageSerializer.java:54-76` | `contracts/codecs/serializer.hpp:345-375` | 기존 Framework codec 선택 유지 |
| bytes 표현 / message-model `05-message-model.ko.md:153` | 공용 profile `:72-74`, Node adapter `:8-11`; 그 전에 `node:buffer:1154-1161`의 임시 표현 생성 | codec `:26-33` → STJ ByteArrayConverter.Write | mapper `writeValueAsBytes`, serializer `:76` | `json_profile.hpp:258-273,518-519` | 공용 `frameworkJsonV1Value`; Node adapter는 primitive와 data graph 준비만 소유 |
| canonical bytes 검증과 복원 / message-model §5, payload ownership §5 | 공용 profile `:137-144,386-406` | codec `:10-23` → STJ ByteArrayConverter.Read | `ZLinkFrameworkJsonProfile.java:352-375` | `json_profile.hpp:275-300,805-807` | 기존 공용 validator, Node adapter의 native byte primitive |

Java 파일은 `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/runtime/messaging/`와 `zlink-framework-json-internal/src/main/java/systems/zlink/framework/runtime/internal/json/`, C++ profile은 `framework/languages/cpp/common/include/zlink/json_profile.hpp`에 있다. Node 경로는 `framework/languages/node/packages/` 아래다. .NET local 경로는 아래 비교표에 명시했다.

소유 계층: **Framework JSON codec / Node runtime adapter**. Core·binding·transport·execution gate 결정은 변경하지 않았다.

스펙: **message-model §5의 padded RFC 4648 byte sequence**, **payload ownership §2/§5의 저장소·객체·view 복사 회계**.

교차언어 대조: **기존 네 구현의 Base64 wire 표현 유지**. Node portable 구현과 decoded 값·원문 JSON을 대조했다. 네 runtime 동시 실행 suite는 이 범위에서 수행하지 않았다.

변경 분류: **B — 기존 성능 결함**. 이번 사용자 요청과 감독 메모가 임시 배열 제거와 native decode 구현을 승인했다. Spec/public root API 변경 없음.

## 구현과 의미 보존

- 공용 stringifier factory가 기존 `frameworkJsonV1Value`를 data graph 준비 함수에 제공한다. Node adapter는 descriptor를 읽고 byte field와 그 상위 객체/배열만 얕게 복제한다. 변환이 없는 subtree는 원본을 유지한다. 사용자 객체와 byte 저장소는 바꾸지 않는다.
- 공용 정책을 이미 적용한 data graph는 native JSON.stringify로 끝낸다. 같은 정책을 replacer에서 한 번 더 확인하지 않는다. Schema 검증은 기존처럼 encoded JSON을 parse하여 같은 validator로 수행한다. Schema를 추론하거나 별도 serializer를 등록하지 않는다.
- Accessor, proxy, callable/custom toJSON, bigint/function, boxed primitive, sparse array, 비표준 byte prototype/byte metadata와 cycle은 원래 stringify/replacer 경로를 유지한다. Custom replacer도 원래 경로다. 따라서 getter의 두 번 읽기, 첫 item/두 번째 source 차이, toJSON 부작용, replacer holder와 cycle 거부를 보존한다. **이 fallback graph의 Buffer.toJSON 비용까지 없앴다고 주장하지 않는다.**
- 기존 parse 진입점을 유지하여 Node adapter가 decoder 함수만 전달한다. 공용 Base64 regex와 pad-bit 검사 코드는 그대로다. 이 검사가 정의하는 canonical 집합은 두 backend 모두 decode할 수 있으므로 허용/거부 집합을 늘리지 않는다. 이 검사를 통과한 문자열만 `Buffer.from(text, 'base64')`에 들어간다. 결과는 `new Uint8Array(buffer, byteOffset, byteLength)`로 노출한다. Byte 저장소 사본은 추가하지 않고 정확한 범위를 가진 Uint8Array 타입을 유지한다.
- Browser/portable backend는 기존 Base64 encode/decode를 사용한다. Node platform 선택을 메시지별 flag나 registry로 추가하지 않았다.

규칙 수(같은 결정을 내리는 위치): **JSON 값/byte 표현 정책 1 → 1**, **canonical Base64 허용 판정 1 → 1**. 일반 data graph에서 버려지는 표현을 만드는 단계는 **Buffer.toJSON + 공용 byte 정책 2 → 공용 정책 1**이다. 준비 가능성 판정은 Node adapter 한 곳에 있으며 wire나 schema 규칙을 추가하지 않는다. Traversal용 Set/descriptor map/얕은 객체는 새 지역 작업 저장소이며, 제거한 Array(N)/wrapper와 아래 할당 값으로 비교한다. 권위 상태·cache·queue·lock·timer는 추가하지 않았다.

리팩토링 점검: native decoder 경로 누락과 Buffer 임시 숫자 배열 **2건 수정**. JSON 표현·검증 소유 중복, control/gate 동기화 추가 **0건**, 별도 이슈로 넘긴 항목 **0건**. Schema writer를 별도 runtime 정책으로 남기지 않았다.

## .NET 비용 비교

.NET은 이 작업에서 성능을 다시 측정하지 않았다. 아래는 **실제 local codec와 공식 STJ 소스 비교**다. STJ에 copy/allocation이 전혀 없다고 해석하지 않는다.

| 비용 | Node baseline → 이번 구현 | .NET local 진입점과 STJ equivalent / 더 적게 하는 일 |
|---|---|---|
| Encode 준비 | Buffer마다 Array(N)과 wrapper → 일반 data graph에서 둘 다 0; bytes가 바뀐 상위 객체 descriptor와 얕은 사본은 생긴다 | `framework/languages/dotnet/src/Zlink.Framework/Runtime/Messaging/ZLinkFrameworkJsonPayloadCodec.cs:26` → [ByteArrayConverter.Write](https://source.dot.net/System.Text.Json/System/Text/Json/Serialization/Converters/Value/ByteArrayConverter.cs.html). bytes를 writer에 바로 넘겨 숫자 배열과 JS graph 준비가 없다. |
| Base64 encode | 앞 작업에서 이미 native view→base64 string. 이번에도 byte 입력 복사는 0; B-char 문자열은 남는다 | 같은 codec `:26-33` → [Utf8JsonWriter.WriteBase64StringValue](https://source.dot.net/System.Text.Json/System/Text/Json/Writer/Utf8JsonWriter.WriteValues.Bytes.cs.html). ReadOnlySpan<byte>에서 writer의 UTF-8 output span으로 쓰므로 별도 Base64 JS string이 없다. |
| JSON encode / UTF-8 | J-char JSON string, 이후 최초 UTF-8 encode가 남는다. Schema가 있으면 JSON.parse와 기존 validator도 남는다 | 같은 codec `:26-33` → [JsonSerializer.WriteBytes](https://source.dot.net/System.Text.Json/System/Text/Json/Serialization/JsonSerializer.Write.ByteArray.cs.html). pooled writer/buffer에 serialize한 뒤 WrittenSpan.ToArray로 소유 결과를 반환한다. 전체 JSON 문자열과 그 문자열의 UTF-8 재변환이 없다. |
| Receive parse / bytes decode | UTF-8→text와 native JSON.parse는 유지. Schema bytes는 atob binary string + N번 charCodeAt → native Buffer decode + 정확한 Uint8Array view | 같은 codec `:10-23,43-47` → [ByteArrayConverter.Read](https://source.dot.net/System.Text.Json/System/Text/Json/Serialization/Converters/Value/ByteArrayConverter.cs.html), [Utf8JsonReader.GetBytesFromBase64](https://source.dot.net/System.Text.Json/System/Text/Json/Reader/Utf8JsonReader.TryGet.cs.html). UTF-8 token span을 쓰므로 전체 JSON/base64 JS 문자열과 JS per-char loop가 없다. |
| Byte copy / 소유 할당 | 새 decoded byte 저장소 1회 + Buffer 객체/Uint8Array view. Pool의 backing buffer 전체 크기와 view 길이는 구분한다. Envelope/binding 복사는 이번 변경 밖이다 | 같은 codec `:40-47`가 owned payload/ReadOnlySpan을 연결한다. [JsonReaderHelper.TryDecodeBase64](https://source.dot.net/System.Text.Json/System/Text/Json/Reader/JsonReaderHelper.Unescaping.cs.html)는 stack/pooled scratch로 decode한 뒤 필요한 길이를 ToArray한다. 최종 byte[] 할당/복사는 존재하지만 binary string과 N개의 JS scalar 처리가 없다. |

## 후보 비교와 측정 조건

후보 도구는 [candidate-costs.cjs](tools/candidate-costs.cjs), [run-candidates-final.py](tools/run-candidates-final.py)다. (a) descriptor 기반 준비와 (b) schema writer를 CPU sample 구성과 allocation으로 비교했다. (b)는 benchmark의 `{body:bytes}` schema만 쓰는 **최소 비용 실험**이며 전체 JSON writer가 아니다. Schema가 없는 입력은 기준 구현을 사용한다. 따라서 그 typed 결과는 전체 JSON 의미를 보존하는 writer의 성능 증명이 아니며, 추가해야 할 getter/toJSON/array/extra property 정책 비용을 포함하지 않는다.

선택: **(a)**. L4는 schema를 제공하지 않아 (b)가 지정 cell의 encode를 바꾸지 못한다. (a)는 공용 JSON 정책 한 곳을 재사용하며 schema 유무에 모두 적용한다. (b)를 production에 넣으면 기존 native JSON 동작과 schema writer의 두 판정 경로가 생긴다. 전체 writer를 Node의 두 번째 JSON 정책으로 추가하지 않는다.

Profiler는 앞 작업의 `--cpu-prof` + Inspector HeapProfiler 16 KiB sampling을 재사용했다. minor/major GC로 회수된 객체를 포함한다. L4는 3 s warmup + 4 s active이며 CPU 표는 active sample 비중이다. Allocation은 startup+warmup+active의 function subtree bytes / warmup+active 실제 message 수다. Micro는 1 s warmup + 2.5 s active이며 CPU 표는 전체 process sample 구성, allocation은 전체 호출 수로 나눴다. 서로 겹치는 subtree를 합산하지 않는다. JS 문자열의 실제 one-byte/two-byte 저장 형식은 V8의 선택이며 문자 수를 고정 byte 수로 주장하지 않는다. Native helper가 inline되어 부모 이름이 사라질 수 있어 Base64 CPU는 encoder/decoder 함수와 native base64Slice/base64Write의 **합집합**으로 센다. Wall time·throughput 개선 배수를 판정하지 않는다.

초기 candidate-v1 raw도 보존했다. 최종 구현/비교는 `candidate-final-*`, 실제 L4는 `before-*` / `after-*`를 사용한다. 각 process lifetime은 20 s 이내인지 확인한다. L4 source/target의 start·mid-active thread affinity, raw stats, native artifact SHA는 각 cell과 `progress.md`에 남긴다.

__TABLES__

## 검증과 남은 비용

__VALIDATION__

남은 비용: Base64/JSON output string, 최초 UTF-8 encode, data graph descriptor/얕은 사본, 기존 schema encode 검증의 JSON.parse, receive text/DTO 생성, 기존 envelope/binding/bench 복사다. 관찰 가능한 fallback graph와 custom replacer에서는 Buffer.toJSON을 유지한다. 기존 의미를 바꾸어 이 비용을 숨기지 않는다.

바꾼 파일은 Node adapter, 공용 JSON profile, `test/contract/node-typed-json.test.js` 세 개다. 공식 browser build가 갱신한 Unity 생성물은 전/후/patch를 raw에 보존하고 이 작업 전 값으로 복원했다. Raw·scratch·설치/build output은 요청대로 보존하고, 이 세션이 시작한 검사·profile process의 종료를 확인했다.

__COMMIT__
