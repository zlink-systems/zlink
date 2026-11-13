# C++ L4 typed JSON 조사·수정안

현재 단계는 진단이다. Production 변경은 시작하지 않았다. 아래 원인 후보는 코드로 확인했으며, CPU·allocation profile 확보 뒤 기여도를 확정한다. Scratch Release 빌드는 다른 작업의 측정 guard에 의해 일시 정지되어 있다. `measure.lock`을 사용하거나 guard를 우회하지 않는다.

## 확인한 경로

`I=framework/languages/cpp/framework/include/zlink/framework`, `J=framework/languages/cpp/common/include/zlink/json_profile.hpp`. 기준은 production `df42a4df40`, scratch ladder `62e766f88d`다. 두 serializer 파일의 SHA256은 같다([identity](raw/codec-baseline-identity.json)). 호출 대조는 [call-shape.md](call-shape.md), 원문은 [CPP profile](raw/cpp-profile-source.txt)·[serializer](raw/cpp-serializer-source.txt)다.

| 항목 | production file:line | 확인 결과 |
|---|---|---|
| 기본 typed 선택 | `I/contracts/codecs/serializer.hpp:345-375` | dto_traits → 기존 JSON profile. 메시지별 codec 등록 없음 |
| byte encode | `J:260-274,518-519` | 이미 Base64 길이를 reserve한다. 임시 Base64 string을 만든 뒤 일반 string writer로 다시 처리한다 |
| 일반 string 처리 | `J:369-408` | Base64에도 UTF-8 검사 1회, escape 검사·문자 append 1회를 수행한다. 최종 JSON은 처음 256 bytes만 reserve한다(`J:950-958`) |
| byte decode | `J:275-300` | 문자마다 64자 alphabet의 find를 호출한다. 출력 vector reserve 없이 push_back하여 크기에 따라 재할당·prefix 복사가 늘어난다 |
| JSON 구조 | `J:486-571,594-948,960-980` | typed writer/SAX다. 이 DTO 경로의 DOM/tree 생성·ostream/stringstream 사용은 없다 |
| message 경계 | `I/contracts/codecs/serializer.hpp:163-178,205-223,365-374` | encode string을 external message에 소유권 이전하고 decode 입력을 span으로 빌린다. JSON 전체 복사는 없다 |

## 결정 소유 표

| 결정·spec 소유 조항 | C++ | Java | .NET | Node | 모을 소유자 |
|---|---|---|---|---|---|
| byte field → padded Base64, message-model §5 `05-message-model.ko.md:153` | `J:260-274,518-519` | `ZLinkJsonMessageSerializer.java:74-76` → profile mapper | `ZLinkFrameworkJsonPayloadCodec.cs:26-33` → System.Text.Json | `framework-json-v1.ts:53-65,394-399` | 각 언어 기존 typed JSON codec |
| Base64 field 검증·decode, 같은 §5 | `J:275-300,809-811` | `ZLinkFrameworkJsonProfile.java:353-375` | `ZLinkFrameworkJsonPayloadCodec.cs:10-23` → System.Text.Json | `framework-json-v1.ts:137-144,386-405` | 각 언어 기존 typed JSON codec |

대조 원문: [Java serializer](raw/java-serializer-source.txt), [Java profile](raw/java-profile-source.txt), [.NET codec](raw/dotnet-codec-source.txt), [Node profile](raw/node-profile-source.txt). 이번 수정은 byte field의 관찰 가능한 동작을 바꾸지 않고 C++ 내부의 변환 비용을 줄인다. 다른 언어의 구현 비용이 다른 것은 spec gap으로 분류하지 않는다.

## Production 수정 제안

**분류 B(기존 C++ codec의 불필요한 처리 비용), 소유 계층 Framework typed JSON codec.** Spec [message model §5](raw/message-model-source.txt): UTF-8 JSON, padded RFC 4648 Base64 및 기존 오류 판정을 유지한다. Public API·wire·bench DTO 변경이 필요하지 않다.

| 대안 | 판단 |
|---|---|
| reserve만 추가하고 decoder lookup만 바꾼다 | Base64 임시 string·일반 UTF-8/escape 처리·추가 복사를 남긴다 |
| 기존 byte writer가 최종 JSON buffer에 직접 Base64를 쓰고, decoder가 크기를 한 번 정한 output에 직접 쓴다 | 선택안. 알려진 ASCII alphabet을 재검사하지 않고, alphabet에서 파생한 역조회로 문자 검색을 제거한다. 일반 JSON parser와 오류 표면은 재사용한다 |

실제 owner는 `J`이며 Framework 기본 serializer와 connector가 함께 사용한다. `framework/` entry에 별도 codec을 추가하지 않고 이 공통 production 구현을 고친다. 기존 `encode_bytes`의 생산 코드 중간 표현은 제거하고, 기존 padding 대조 test가 쓰는 이전 encoder는 test oracle로 옮긴다.

규칙 수(이 byte 경로의 판정 위치): Base64 표현 1→1, 이미 생성된 Base64의 UTF-8/escape 검사 2→0, canonical decoder 1→1, 합계 **4→2**. 새 flag·queue·lock·timer 0. 역조회 표는 기존 alphabet에서 constexpr로 파생하므로 독립 매핑 규칙을 만들지 않는다.

## 검증·근거 상태

- 수정 전 C++ format check 통과: [raw](raw/format-baseline.log).
- 독립 Python RFC 4648 JSON fixture 8개 준비: [크기·SHA256](raw/independent-goldens.json). 크기는 0, 1, 2, 3, 127, 128, 1024, 4096이다. 수정 전 production 출력과 비교할 예정이다.
- CPU L3/L4 send-saturation 4K·request-serial 1K profile, per-message C++ new 및 native malloc 계수, copy 계수는 build 완료 뒤 이 보고서에 추가한다.
- Runtime 구현·unit test·local commit은 아직 없다. Raw는 보존한다.
