# C++ L4 typed JSON — 원인과 수정

기본 typed JSON의 **Base64 임시 string·중복 UTF-8/escape 순회·문자 append**, decoder의 **문자마다 alphabet 검색·byte vector 반복 확장**을 제거했다. padded RFC 4648, JSON 출력, public API와 bench DTO는 유지한다. 변경 분류는 **B(기존 codec의 불필요한 처리 비용)**다.

수정 전/후 production 출력과 독립 fixture가 **0, 1, 2, 3, 127, 128, 1024, 4096 bytes 모두 byte-identical**이며 decoded value도 같다. 관련 C++ runtime test **7/7**, C++ format과 diff check가 통과했다. 로컬 commit: **846aa1d27f50882d075904d91ea47c3ff63c8ed5**.

원인·제안을 먼저 기록한 [구현 전 보고서](raw/report-before-implementation.md)를 보존했다. 아래 J의 수정 전 line은 `df42a4df40`, 수정 후 line은 현재 변경 기준이다. `J=framework/languages/cpp/common/include/zlink/json_profile.hpp`, `I=framework/languages/cpp/framework/include/zlink/framework`.

## 원인과 production 수정

| 확인한 원인 | 수정 전 production file:line | 수정 |
|---|---|---|
| 이미 reserve한 Base64 string을 만든 뒤 일반 string writer가 UTF-8 검사·escape 검사·문자 append를 다시 수행 | `J:260-274,369-408,519` | `J:371-404`가 최종 JSON storage에 Base64를 직접 쓴다. byte_output도 같은 writer를 사용한다 |
| 각 Base64 문자를 64자 alphabet의 find로 찾음 | `J:275-300`, 특히 `:285` | `J:262-269`의 constexpr 역조회는 기존 alphabet에서 파생한다. 별도 수작업 매핑은 없다 |
| decoded vector에 reserve 없이 push_back | `J:294-298` | `J:277`에서 padding을 반영한 크기를 한 번 정하고 직접 기록한다. 기존 padding 위치·잔여 bit 검사를 유지한다 |
| 최종 JSON buffer의 문자별 증가 | `J:912,375-404` | byte field 길이에 맞춰 capacity를 확보한다. Base64 임시 string과 전체 body 성장 복사를 제거한다 |

기본 registry entry는 `I/contracts/codecs/serializer.hpp:345-375`, 특히 `:365-374`다. 이 entry가 호출하는 공통 production owner인 **J**를 수정했다. Framework entry에 두 번째 codec이나 bench 전용 경로를 만들지 않았다. 기존 encoder는 production에서 제거하고 이전 padding acceptance와 대조하는 test oracle로 옮겼다.

[원문 J](raw/cpp-profile-source.txt)·[registry](raw/cpp-serializer-source.txt). 이 DTO 경로는 이미 typed writer/SAX이며 **DOM/tree·ostream/stringstream을 사용하지 않는다**. encode_bytes 자체에는 이미 reserve가 있었다. std::vector<byte>를 raw string으로 옮기는 우회도 없다.

대안은 ① reserve·lookup만 고쳐 작은 diff를 만드는 방법과 ② 최종 output에 직접 쓰는 방법이었다. ①은 임시 string·추가 복사·일반 string 검사를 남긴다. ②는 기존 byte_output에 storage 접근자를 추가하는 대신 중간 표현과 중복 순회를 제거하므로 ②를 선택했다.

## 실제 L3/L4 CPU profile

Scratch는 `62e766f88d`의 git archive이며 위치는 `/root/worktree/zlink-v21-s033/c4json-scratch`다. production과 scratch의 J 및 registry entry는 수정 전 [동일 SHA256](raw/codec-baseline-identity.json)이다. [호출 대조](call-shape.md)에서 L3 framing과 L4 default typed JSON을 구분한다.

최종 **v5 네 셀**은 active submit worker가 생성된 뒤 source/target의 모든 live TID에 `perf record -g -e cpu-clock:u -F 499 --call-graph fp`를 연결했다. profile window는 **3.5초**, 전체 process run은 **5.50–8.16초**, 모든 thread는 시작·중간 모두 **CPUs 6-7,14-15 / SCHED_OTHER**다. 모두 `submitted=completed=received`, errors=0이다. [검증 수치·조건](raw/valid-profile-summary.json), 각 run의 `before.json`·`perf-task-map.json`·`*-scheduling.json`에 binary SHA256·freshness·TID를 남겼다. measure.lock을 사용하지 않았다. **처리량·단계 손실·7% 예산은 판정하지 않는다.**

| 셀 | 관측한 self sample / call shape | raw |
|---|---|---|
| L3 send-saturation 4K | protobuf/frame copy 중심. typed payload JSON/Base64 writer·decoder가 없음 | [self](raw/baseline-v5-L3-send-saturation-4096/perf-self.txt), [stack](raw/baseline-v5-L3-send-saturation-4096/perf-callgraph.txt) |
| L4 send-saturation 4K | source bench-submit-0: string push_back **16.33%**, write_value **11.79%**, write_string **6.57%**. target memchr **16.10%**, decode_bytes **5.26%** | [self](raw/baseline-v5-L4-send-saturation-4096/perf-self.txt), [stack](raw/baseline-v5-L4-send-saturation-4096/perf-callgraph.txt) |
| L3 request-serial 1K | protobuf encode/decode와 binding completion 경로. typed JSON/Base64가 없음 | [self](raw/baseline-v5-L3-request-serial-1024/perf-self.txt), [stack](raw/baseline-v5-L3-request-serial-1024/perf-callgraph.txt) |
| L4 request-serial 1K | source encode write_string **2.09%**, target reply encode **1.10%**. target/source completion의 memchr **4.29% / 2.45%**. 왕복의 encode·decode 두 쌍이 모두 잡힘 | [self](raw/baseline-v5-L4-request-serial-1024/perf-self.txt), [stack](raw/baseline-v5-L4-request-serial-1024/perf-callgraph.txt) |

동일 DTO·0xab 입력의 분리 production JSON bulk probe는 전/후 각각 encode/decode × 1K/4K, run 4초로 수집했다. 4K encode의 push_back **47.03%**, write_string 자체 **23.24%**가 수정 후 각각 **0.68%**, 주요 sample에서 사라지는 형태로 바뀌고, 직접 writer가 **93.69%**를 차지한다. decode의 memchr **26.76%**도 수정 후 주요 sample에서 사라졌다. 이는 **shape 변화**이며 sample 비율을 처리량 개선율로 환산하지 않는다. [전 encode](raw/baseline-bulk-encode-4096.self.txt)·[후 encode](raw/fixed-bulk-encode-4096.self.txt)·[전 decode](raw/baseline-bulk-decode-4096.self.txt)·[후 decode](raw/fixed-bulk-decode-4096.self.txt).

## 메시지 한 번의 allocation·copy

아래 단위는 warmed default `serializer_registry_t.get<DTO>()`의 **body serialize 또는 deserialize 1회**다. input DTO·input message는 이미 존재하며 registry 초기화, framing, bench metrics는 제외한다. source/target 두 번의 encode·decode가 있는 request는 이 비용을 각각 두 번 수행한다.

C++ new는 기존 allocation_counter의 stack 기록으로, native malloc/calloc/realloc은 LD_PRELOAD로 따로 셌다. stack은 non-PIE probe와 addr2line으로 production 및 nlohmann file:line에 연결했다. [요약](raw/allocation-summary.json)·[전 raw](raw/baseline-registry-native-allocations.txt)·[후 raw](raw/fixed-registry-native-allocations.txt)·[전 stack](raw/baseline-allocation-addresses.txt)·[후 stack](raw/fixed-allocation-addresses.txt).

| logical bytes / 작업 | C++ new 전→후 | native malloc 전→후 | 할당 요청 bytes 합계 전→후 |
|---|---:|---:|---:|
| 1K encode | 6→3 | **7→4** | 5,285→1,964 |
| 1K decode | 32→22 | **32→22** | 10,447→9,424 |
| 4K encode | 8→3 | **9→4** | 21,671→6,060 |
| 4K decode | 38→26 | **38→26** | 40,401→36,306 |

Encoder의 구성은 initial output 1회, Base64 임시 string **1→0회**, output 성장 **3/5→1회**, owned string 객체 1회(32 B), Core external message metadata 1회(40 B)다. owner는 `I/contracts/codecs/serializer.hpp:163-178`, Core 연결은 binding `message.cpp:283-297`, metadata 할당은 `core/src/runtime/core/msg.cpp:356`이다. Core나 binding을 수정하지 않았다.

| decoder allocation owner / 최초 관측 위치 | 1K 전→후 | 4K 전→후 |
|---|---:|---:|
| SAX frame reserve, J 전 `:852` / 후 `:888` | 1→1 | 1→1 |
| parser state, nlohmann `parser.hpp:233` | 1→1 | 1→1 |
| raw token vector, nlohmann `lexer.hpp:1358` | 12→12 | 14→14 |
| unescaped token string, nlohmann `lexer.hpp:1408` | 7→7 | 9→9 |
| decoded byte vector, J 전 `:294-298` / 후 `:277` | **11→1** | **13→1** |

[각 event의 요청 크기·stack](raw/baseline-allocation-events.json)와 [수정 후 event](raw/fixed-allocation-events.json)가 이 합계를 뒷받침한다. nlohmann의 두 token buffer 및 JSON 문법·UTF-8 처리 비용은 남는다. 이를 없애기 위해 parser나 wire 계약을 바꾸지 않는다.

Copy 계수는 실제 allocation capacity와 code의 byte range를 연결해 계산했다. memcpy interposition 수가 아니라 **논리 data copy**다. 변환의 read/write와 resize의 zero-fill은 별도다.

| body byte copy / 위치 | 수정 전 | 수정 후 |
|---|---|---|
| Base64 임시 text → 최종 JSON, J 전 `:519,375-404` | 1회: 1K 1,368 B / 4K 5,464 B | 0회 |
| 최종 JSON 성장 prefix 복사 | 1K 3회·1,792 B / 4K 5회·7,936 B | 1회·8 B (`{"body":` prefix), J `:377` |
| decoded byte vector 성장 prefix, J 전 `:294-298` | 1K 10회·1,023 B / 4K 12회·4,095 B | 0회 |
| SAX raw/unescaped token | body를 두 buffer에 수집하며 성장 복사도 발생 | 동일. 위 lexer 두 위치 |
| default registry → zlink message, I `:163-178,205-223` | 전체 JSON copy 0회: owned string 이전 | 0회 |
| outgoing service frame, `service_wire_codec.cpp:83-89,3196-3205,3240-3246` | JSON body 1회 | 1회 |
| incoming framing, 같은 파일 `:3400-3424` | retained slice, body copy 0회 | 0회 |

별도 public `message_t::from_json` utility는 `I/codecs/json.hpp:21-25` → binding `message.cpp:108-115`에서 JSON을 1회 복사한다. default registry 경로와 구분한다. bench의 새 reply DTO body 복사도 그대로다. [경계 원문](raw/copy-boundaries.txt).

## 결정 소유·계약

| 결정 / spec 조항 | C++ | Java | .NET | Node | 수정의 소유자 |
|---|---|---|---|---|---|
| byte field → padded RFC 4648, message-model §5 `:153` | J 전 `:260-274,519` / 후 `:371-404` | `ZLinkJsonMessageSerializer.java:74-76` → mapper | `ZLinkFrameworkJsonPayloadCodec.cs:26-33` → System.Text.Json | `framework-json-v1.ts:53-65,394-399` | 각 언어 기존 typed JSON codec |
| field decode·validation, 같은 §5 | J 전 `:275-300,806` / 후 `:269-299,842` | `ZLinkFrameworkJsonProfile.java:353-375` | `ZLinkFrameworkJsonPayloadCodec.cs:10-23` | `framework-json-v1.ts:137-144,386-405` | 각 언어 기존 typed JSON codec |

[Java serializer](raw/java-serializer-source.txt)·[Java profile](raw/java-profile-source.txt)·[.NET codec](raw/dotnet-codec-source.txt)·[Node profile](raw/node-profile-source.txt)는 git show로 대조했다. byte field의 padded Base64 표현을 유지하며 C++의 관찰 가능한 계약을 변경하지 않는다. C++ decode acceptance는 이전 encoder를 독립 oracle로 삼아 padding 조합과 모든 256 문자 위치를 대조한다. Spec 원문은 `framework/doc/framework/common/spec/server/00-foundation/05-message-model.ko.md:134-153`([사본](raw/message-model-source.txt))이다.

소유 계층: **Framework typed JSON codec**. Spec: **message-model §5:153**. 교차언어: **기존 padded Base64 field 계약 유지**. 변경 분류: **B**.

규칙 수(이 byte 경로의 판정 위치): Base64 표현 1→1, 이미 생성한 Base64의 UTF-8/escape 검사 2→0, canonical decode 1→1, 합계 **4→2**. 새 flag·queue·lock·timer 0. 파생 constexpr 역조회는 독립 매핑 규칙을 추가하지 않는다. 점검: 불필요한 처리 3건 수정, 중복 production encoder 제거, execution gate 우회·Core/binding 제어 보상 0건, 이관 0건.

## 검증·변경 파일

| 검증 | 결과 / raw |
|---|---|
| Release/source bindings/Core 1.25.0/-j3/25 GiB 제한 | [baseline build](raw/build-baseline.log)·[main build](raw/build-unit.log)·[actual Core link](raw/ldd-baseline.log)·[CPU mask](raw/build-affinity-snapshot.json). 모든 build dir는 이 worktree 안에 생성 |
| typed JSON / allocation / connector core·extension / MessagePack / noexception | **6/6** [CTest](raw/ctest-typed-json.log) |
| serializer registry / codec selection·ownership | **1/1** [CTest](raw/ctest-serializer-registry.log) |
| byte-identical JSON + decoded value, 지정 8개 크기 | old=new=독립 fixture **8/8** [SHA256](raw/final-wire-comparison.json) |
| byte 출력 검사의 음성 대조 | 의미가 같은 JSON에 공백만 추가하면 assertion으로 거부(-6), fixture 복원 후 통과(0), 원본 SHA256 복원 [결과](raw/wire-bidirectional-check.json) |
| allocation의 양성 대조 | DOM control new=34, 실제 encode/decode DOM=0; byte decode는 positive size당 1회 [로그](raw/test-allocations-first.log) |
| 전체 C++ format / 공통 header 별도 format / diff check | **0/0/0** [format](raw/format-final.log)·[common](raw/format-common-final.log)·[diff](raw/diff-check-final.log) |

변경 파일은 `framework/languages/cpp/`의 `common/include/zlink/json_profile.hpp`, `CMakeLists.txt`, `tests/Zlink.Framework.UnitTests/`의 `test_cpp_framework_typed_json.cpp`, `test_cpp_framework_typed_json_allocations.cpp`, `test_cpp_framework_typed_json_noexception.cpp`, `typed_json_bytes_golden.jsonl`이다. Framework default serializer·message JSON utility·connector를 같은 fixture로 검증한다. bench DTO·wire·public API·spec 문서 변경은 없다.

남은 codec 관련 실패는 없다. parser token 처리와 기존 framing copy의 비용은 남으며 처리량 재측정은 이 보고서의 판정 범위 밖이다. 초기 scratch profile의 고정 HTTP 포트 재사용에 따른 bind/start 실패(뒤따른 source SIGSEGV 포함), admitted send를 receiver drain 전 집계한 snapshot, worker 생성 전 attach한 sample은 최종 v5 판정에서 제외했다. 원래 raw는 모두 보존하며, HTTP start 실패 뒤 cleanup의 근본 원인은 이번 typed JSON 수정 범위에서 조사하지 않았다. [기동 오류](raw/baseline-v2-L3-request-serial-1024/source.log)·[상태](raw/baseline-v2-L3-request-serial-1024/startup-events.json).

로컬 commit만 수행한다. push·PR·GitHub·sub-agent/다른 AI·internal-docs.sh·measure.lock 사용은 없다. Raw와 scratch source는 보존하며, 종료 시 이 worktree의 build dir만 정리한다. 정리 결과: build-c4json-ladder / build-c4json-unit / build-c4json-bulk 3개 삭제 완료; raw·scratch source 유지 ([최종 상태](raw/final-repository-state.json)).
