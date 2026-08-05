# C++ Registration/Codec E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-4-registration-codec.ko.md`

이 문서는 공통 시나리오 중 C++ framework의 공개 API로 직접 검증하는 항목을 구분한다.

최신 proof는 `logs/20260708-124643-1329498`이다. Config-4는 위치 resolve를 다루지 않아 Redis
location store를 등록하지 않고 수동 endpoint 연결만 사용한다.

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| `RC-A1` | not-supported | 공통 framework spec은 runtime reflection이 없는 C++을 assembly·module 자동 등록 원칙의 예외로 정한다. C++는 정식 public builder에서 handler 타입을 명시하므로 자동 스캔 경로를 별도 구현하지 않는다. 기존 scenario는 명시 등록한 `EchoAuto` request/send의 typed dispatch만 회귀 검증한다. |
| `RC-A2` | not-supported | 공통 E2E가 이 항목을 `.NET` attribute 등록으로 한정하고, C++ 정식 interface는 attribute 기반 자동 등록이 없다고 명시한다. 기존 scenario는 명시 등록한 `EchoAttr` request/send의 packet 이름과 context만 회귀 검증하며 attribute 등록을 구현했다고 판정하지 않는다. |
| `RC-A3` | 구현 | 수동 channel handler 등록의 명시 packet 이름으로 request와 send를 검증한다. 로그: `logs/20260708-124643-1329498`, 출력: `scenario RC-A3 passed`, `registration-codec e2e result=passed`. |
| `RC-A4` | 구현 | request handler마다 새 invocation scope가 만들어지고 scoped dependency가 요청 뒤 dispose되는지 검증한다. singleton dependency 유지도 함께 확인한다. 로그: `logs/20260708-124643-1329498`, 출력: `scenario RC-A4 passed`, `registration-codec e2e result=passed`. |
| `RC-A5` | 구현 | handler filter pipeline의 before/after 실행 순서를 검증한다. 로그: `logs/20260708-124643-1329498`, 출력: `scenario RC-A5 passed`, `registration-codec e2e result=passed`. |
| `RC-A6` | 구현 | client scenario가 invalid server process를 직접 관리하며 중복 handler 등록, 잘못된 handler group/channel 조합, handler group 없는 server 구성을 startup failure와 정확한 validation 오류로 검증한다. 출력: `scenario RC-A6 duplicate passed`, `scenario RC-A6 wrong-group passed`, `scenario RC-A6 unsupported-channel passed`, `scenario RC-A6 passed`. |
| `RC-B1` | 구현 | JSON request와 send를 실행하고 `application/json` content-type evidence를 확인한다. 로그: `logs/20260708-124643-1329498`, 출력: `scenario RC-B1 passed`, `registration-codec e2e result=passed`. |
| `RC-B2` | 구현 | C++ Protobuf codec extension으로 request와 send를 실행하고 `application/x-protobuf` content-type evidence를 확인한다. 로그: `logs/20260708-124643-1329498`, 출력: `scenario RC-B2 passed`, `registration-codec e2e result=passed`. |
| `RC-B3` | 구현 | C++ MessagePack codec extension으로 request와 send를 실행하고 `application/x-msgpack` content-type evidence를 확인한다. 로그: `logs/20260708-124643-1329498`, 출력: `scenario RC-B3 passed`, `registration-codec e2e result=passed`. |
| `RC-B4` | 구현 | 별도 serializer를 등록하지 않은 `json_roundtrip_req_t`가 기본 JSON 경로와 `application/json`을 사용하는지 확인한다. 같은 host에서 Protobuf, MessagePack, custom serializer도 섞어 보내 각 reply 값과 실제 inbound content-type이 서로 간섭하지 않는지 검증한다. |
| `RC-B5` | 구현 | codec requester가 Protobuf content-type request를 JSON-only peer에 보내고, 기본 JSON serializer가 payload를 해석하지 못하면 공개 `payload_decode_failed`로 끝나는지 이름으로 확인한다. 이후 같은 channel의 정상 JSON request가 계속 성공하는지도 확인한다. |

## C++ 언어 표면 차이

C++는 `.NET`의 attribute syntax나 assembly scan을 제공하지 않는다. 공통 framework spec은 C++을
자동 등록 원칙의 예외로 정하고, compile-time 타입과 명시 builder 호출을 정식 등록 표면으로 둔다.
따라서 `RC-A1`과 `RC-A2`를 서로 다른 등록 방식으로 구현했다고 판정하지 않는다. 두 scenario는
각 packet의 typed dispatch 회귀 검증으로만 유지하고, C++ 등록 계약은 `RC-A3`의 명시 등록에서
검증한다.
