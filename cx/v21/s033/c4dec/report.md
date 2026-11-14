# C++ typed JSON decode 구조 비교

**직접 span reader(B)를 production에 적용했다.** 1K/4K decode는 **22회·9,424 B / 26회·36,306 B → 2회·1,504 B / 2회·4,576 B**다. nlohmann의 token buffer와 parser stack을 없애고 기존 typed 변환·필수 필드·중복 key 검사를 유지한다. [실제 registry 측정](raw/registry-summary.json)·[후보/CPU 요약](raw/candidate-summary.json).

`J=framework/languages/cpp/common/include/zlink/json_profile.hpp`, `N=nlohmann/detail/input/`, `D=framework/languages/dotnet/src/`. C++ 전은 `aae182694c`, 후는 이번 commit이다. 단위는 준비된 JSON body의 decode 1회이며 초기 registry/입력 생성·framing·handler copy는 제외한다. C++ B는 allocation **요청 크기 합계**, .NET B는 managed allocation이다.

## 비용·검증 위치

| decode 항목 | C++ 전 → 후 file:line / 1K·4K 비용 | .NET file:line / 같은 byte DTO |
|---|---|---|
| 입력·metadata | J 전 `:954` → 후 `:1157`: contiguous 입력 copy 0; dto_traits/static metadata | `D/Zlink.Framework/Runtime/Messaging/ZLinkFrameworkJsonPayloadCodec.cs:10-13`: input span copy 0, options/metadata cache |
| raw token | N `lexer.hpp:1358`: **12/14회, 4,095/16,383 B**, 본문 1회 수집+성장 copy → **0** | `D/Shared/ZLinkFrameworkJsonDocument.cs:20`: Utf8JsonReader가 input span의 token을 읽는다. 긴 value의 raw-token buffer 없음 |
| unescaped token | N `lexer.hpp:1407`: **7/9회, 3,817/15,339 B**, 본문 1회 수집+성장 copy → J `:932`: unescaped view, **0** | codec `:13` → [BCL ByteArrayConverter:5-11](raw/dotnet-stj-bytearrayconverter.cs): value string을 만들지 않고 GetBytesFromBase64 |
| 문법·frame | N `parser.hpp:233` **1회/8 B** + J 전 `:888` **1회/480 B** → J 후 `:1107,1109` **1회/480 B** | `D/Shared/ZLinkFrameworkJsonDocument.cs:36-61`: 검증 scan 뒤 STJ typed scan. object당 HashSet·bucket·entry·key string **4회/208 B**. DTO/constructor/required 관리 **200 B**(회수 계측 없음) |
| byte 결과 | J 전 `:269` → 후 `:273`: **1회/1,024 또는 4,096 B**, 결과 크기를 계산하고 직접 기록 | [JsonReaderHelper:295-319](raw/dotnet-stj-jsonreaderhelper.cs): pooled 2K/8K buffer에 decode → 결과 array로 **1,024/4,096 B copy 1회**. 결과 array **1회/1,048 또는 4,120 managed B** |
| 수용 검증 | J 후 `:1111` BOM, `:932,305` escape/Unicode/UTF-8, `:1060` finite, `:754` typed range·enum·64-bit 문자열·Base64, `:1132` duplicate, `:730` required | `D/Shared/ZLinkFrameworkJsonDocument.cs:13-61` BOM/duplicate/finite; `D/Shared/ZLinkJsonSerializerOptions.cs:54-127,194-357` required/nullability/typed 값; STJ reader/byte converter 문법·Base64 |

C++ 전 항목별 합계는 [c4json stack 재집계](raw/c4json-decode-costs.json), .NET 비교값은 **수정 전 STJ 8.0.31**의 [기존 raw](raw/dotnet-allocations-before.txt)다. .NET 총 decode는 1K **1,456 B**, 4K **4,528 B**이며 전체 allocation 회수는 계측하지 않았다. 이 버퍼·frame·HashSet/constructor 비용은 구현 선택이다.

실제 L4 request/reply는 같은 `typed_payload_t`다([정의](raw/ladder_payload.hpp):15-22,37-38). Target request decode는 [ladder_wire.hpp](raw/ladder_wire.hpp):263-265, source reply decode는 `:347-348`에서 기본 registry를 쓴다. 따라서 위 1K/4K decode 비용이 양쪽에 각각 적용된다. 새 reply의 `set_body` copy(`:283-284`)는 codec 밖의 1K/4K copy 1회이며 유지했다.

## 후보·선택

| 후보 | 판정 위치 수 전→후 | 1K allocation / B | 4K allocation / B | 기존 accept/reject·값 대조 |
|---|---:|---:|---:|---|
| 기존 nlohmann SAX | 9→9 | 22 / 9,424 | 26 / 36,306 | 기준 |
| A: document 크기로 두 token buffer reserve, finite 중복 제거 | 9→8 | 5 / 4,271 | 5 / 15,535 | **55,749건 동일** |
| **B: 직접 span reader** | **9→7** | **2 / 1,504** | **2 / 4,576** | **55,749건 동일** |

[할당 원본·perf 요약](raw/candidate-summary.json), [수용 대조](raw/candidate-tests-before.json), [판정 위치별 표](raw/decision-owners-before.md). 판정 수는 BOM·문법 진행·UTF-8·전체 finite·typed 변환·duplicate·required의 owner entry를 센다. B는 parser/typed dispatch의 container 진행 판정 2→1, 전체 finite 판정 2→1이다.

A는 nlohmann public SAX API가 lexer reserve/reset이나 string_view token을 제공하지 않으므로 **private lexer reserve 실험**으로 비용을 확인했다. public callback은 `std::string&`여서 본문 token copy 2회를 남긴다. parser 재사용에는 private reset/lifetime 상태가 추가로 필요하다. B는 vendor 수정 없이 token copy와 parser stack을 제거하며 할당·판정 위치가 모두 적어 선택했다. Production에는 reader 하나만 있고 SAX fallback은 없다. 이전 decoder와 A/B 실험은 job raw/tools에만 보존한다.

B는 escape가 없는 token을 input view로 전달하고 escape에만 scratch를 쓴다. 기존 `strtod` rounding·overflow promotion·signed underflow와 root 뒤 NUL terminator 수용도 유지했다. 본문 token copy는 2→0회이며 byte vector의 기존 resize zero-fill은 유지한다. .NET pooled buffer는 decode 뒤 2K/8K를 clear한다. parser states, `started`·`healthy`를 제거하고 기존 frame의 `pending_key`를 문법 진행 상태 `after_value`로 대체했다. queue·lock·timer 추가 0.

## CPU shape

4K self sample은 기존 lexer `get` **49.13%**, string `push_back` **28.59%**, `scan_string` **10.39%**; A는 각각 **43.09/28.36/17.81%**다. B는 `string_token` **77.40%**, Base64 typed 변환 **20.90%**다. token 수집·성장 대신 입력 검사와 결과 변환이 남는다. [전](raw/baseline-decode-4096.self.txt)·[A](raw/candidate-a-decode-4096.self.txt)·[B](raw/candidate-b-decode-4096.self.txt). 1K도 [같은 형태](raw/candidate-summary.json)다.

각 profile은 4초, timeout 20초, 시작·중간 모두 **SCHED_OTHER / CPUs 6-7,14-15**다([조건 검증](raw/profile-validation.json)). 모든 binary는 이 worktree에서 빌드했고 commit/dirty/base·SHA256·source freshness를 [progress.md](progress.md)와 metadata에 기록했다. measure.lock·throughput 판정은 없다.

## 검증·commit

- 공유 `framework-json-v1.json`, required-presence golden, c4json의 8개 byte fixture와 malformed/escape/Unicode/duplicate/number-edge/random 입력 **55,749건**: A/B 모두 기존 수용·거부 및 decoded 값 동일.
- 새 allocation budget: **기존 코드에서 22회/9,424 B로 실패(-6)**. [실패 원본](raw/test_cpp_framework_typed_json_allocations-before.txt).
- Production runtime **7/7 통과**(typed JSON·allocation·noexception·connector core/extension·MessagePack·registry): [CTest](raw/ctest-production.log). 실제 L4 DTO의 native malloc과 C++ new가 위 표의 모든 후보·크기에서 일치한다([registry 결과](raw/registry-summary.json), [production stack](raw/production-registry-stacks.txt)).
- encode/decode allocation 주입 음성 대조는 각각 **-6으로 검출**; 실제 body budget은 1K/4K 모두 통과([양방향 결과](raw/verification-summary.json), [전후 budget](raw/budget-before-after.json)). 긴 escaped 문자열·비연속 iterator·5,000단계 unknown array도 통과했다.
- Release, source binding, Core `/root/.cache/zlink/core/1.25.0/linux-x64`, `-j3`, 25 GiB 제한; [빌드](raw/build-production.log)·[실제 Core link](raw/ldd-production.txt)·[최종 binary SHA256/freshness](raw/production-test-metadata.json).
- C++ 전체 format / 공통 header / diff check **0/0/0**: [결과](raw/format-summary.json).

변경 파일: `J`, `tests/Zlink.Framework.UnitTests/test_cpp_framework_typed_json.cpp`, `test_cpp_framework_typed_json_allocations.cpp`. 보호 문서·spec·public API·wire·bench DTO 변경 없음.

소유 계층: Framework 공통 typed JSON codec. Spec: message-model §5(`framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md:134-153`). 교차언어: .NET/JVM/Node의 기존 typed 표현과 공통 golden을 유지하며 C++ 기존 수용 집합도 보존한다([원문](raw/dotnet-profile-options.cs)·[JVM](raw/java-profile.java)·[Node](raw/node-profile.ts)). 분류 **B**.

점검: token 저장 중복·parser 상태 중복·finite 검사 중복 **3건 수정**, execution gate 우회·Core/binding 결정 보상 **0건**, 이관 **0건**. 로컬 commit: **대기**. 남은 관련 실패는 없다.
