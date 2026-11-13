# C++ L4 typed JSON 조사·수정안

분리 production CPU·allocation 진단과 수정안을 먼저 보고한 뒤 구현·검증을 시작했다. 구현 전 보고서는 [raw 사본](raw/report-before-implementation.md)에 보존했다. 아래 내부 byte 처리 비용은 분리한 production JSON bulk probe의 CPU·allocation profile로 확인했다. 전체 ladder profile과 registry/native 계수는 build 완료 뒤 추가한다. Scratch Release 빌드는 다른 작업의 측정 guard에 의해 일시 정지되어 있다. `measure.lock`을 사용하거나 guard를 우회하지 않는다.

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

Codec 경계의 복사와 service wire 복사는 구분한다. [경계 원문](raw/copy-boundaries.txt): default registry의 `from_text`는 external message로 storage를 이전한다. `service_wire_codec.cpp:3153-3229`는 native message를 한 번 배정하고 `:83-89,3196-3205,3240-3246`에서 body를 wire frame에 **1회 복사**한다. 수신 `:3400-3424`는 retained external slice이며 body byte copy는 0회다. 이 L3 framing 동작을 유지한다. 별도 public `message_t::from_json` utility는 `codecs/json.hpp:21-25` → binding `message.cpp:108-115`에서 JSON을 1회 복사하며, default registry의 encode 경로와 다르다.

## 결정 소유 표

| 결정·spec 소유 조항 | C++ | Java | .NET | Node | 모을 소유자 |
|---|---|---|---|---|---|
| byte field → padded Base64, message-model §5 `05-message-model.ko.md:153` | `J:260-274,518-519` | `ZLinkJsonMessageSerializer.java:74-76` → profile mapper | `ZLinkFrameworkJsonPayloadCodec.cs:26-33` → System.Text.Json | `framework-json-v1.ts:53-65,394-399` | 각 언어 기존 typed JSON codec |
| Base64 field 검증·decode, 같은 §5 | `J:275-300,809-811` | `ZLinkFrameworkJsonProfile.java:353-375` | `ZLinkFrameworkJsonPayloadCodec.cs:10-23` → System.Text.Json | `framework-json-v1.ts:137-144,386-405` | 각 언어 기존 typed JSON codec |

대조 원문: [Java serializer](raw/java-serializer-source.txt), [Java profile](raw/java-profile-source.txt), [.NET codec](raw/dotnet-codec-source.txt), [Node profile](raw/node-profile-source.txt). 이번 수정은 byte field의 관찰 가능한 동작을 바꾸지 않고 C++ 내부의 변환 비용을 줄인다. 다른 언어의 구현 비용이 다른 것은 spec gap으로 분류하지 않는다.

## 확보한 CPU·allocation 근거

분리 probe는 production과 동일한 scratch `J`의 `encode/try_decode`를 호출한다. 입력은 bench와 같은 0xab byte field다. 각 perf run은 4초, CPUs 6-7,14-15이며 처리량 판정을 하지 않는다. Registry의 string owner 및 Core metadata는 이 probe 범위 밖이다.

| 크기 | encode C++ new / 요청 bytes | decode C++ new / 요청 bytes | raw |
|---|---:|---:|---|
| 1K | 5 / 5,213 | 32 / 10,447 | [allocation](raw/baseline-bulk-allocations.txt) |
| 4K | 7 / 21,599 | 38 / 40,401 | 같은 raw |

| CPU self sample | 1K | 4K | 근거 |
|---|---:|---:|---|
| encode string push_back | 46.94% | 47.03% | [1K](raw/baseline-bulk-encode-1024.self.txt), [4K](raw/baseline-bulk-encode-4096.self.txt) |
| encode write_string 자체 | 21.06% | 23.24% | 같은 raw. 4K push_back 중 42.02%p는 write_string 아래다 |
| decode memchr | 25.78% | 26.76% | [1K](raw/baseline-bulk-decode-1024.self.txt), [4K](raw/baseline-bulk-decode-4096.self.txt) |
| decode decode_bytes 자체 | 9.24% | 9.09% | 같은 raw |
| decode lexer get 자체 | 26.71% | 29.79% | 같은 raw. JSON token 처리 비용도 남는다 |

CPU 원인은 encode의 Base64 임시 string과 일반 string writer의 중복 순회·문자 append, decode의 alphabet 선형 검색과 output vector 반복 확장이다. Nlohmann의 SAX lexer token buffer 비용도 관측됐다. Library parser를 교체하거나 wire를 바꿔 이 비용을 숨기지 않는다. 이 수치는 분리 probe의 shape이며 전체 ladder의 손실 기여율로 환산하지 않는다.

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
- L3/L4 send-saturation 4K·request-serial 1K 전체 profile, registry의 per-message C++ new 및 native malloc 계수, copy 계수는 build 완료 뒤 이 보고서에 추가한다.
- 공통 production byte writer/decoder와 byte-identical JSON·decode·allocation 회귀 test를 구현했다. Release build·test·전체 ladder profile·local commit은 진행 중이다. Raw는 보존한다.
