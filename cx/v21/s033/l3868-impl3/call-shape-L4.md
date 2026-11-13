# C++ L4 (#4354) 호출 대조

기준 L3 `dd83e75df4`, 실험 branch `exp/3868-cpp-ladder-next`. 측정 없음.
`B=framework/bench/grpc/cpp`, `R=framework/languages/cpp/framework/src/runtime`,
`I=framework/languages/cpp/framework/include/zlink/framework`.
L3 line은 기준 commit, L4 line은 L4 구현 commit 기준이다.

| 경로 | 이전 L3 file:line | 새 L4 file:line | 실제 production 호출·차이의 소유 기능 |
|---|---|---|---|
| source body 생성·stamp | `B/ladder/ladder_wire.hpp:127-132`: protobuf body | `B/ladder/ladder_payload.hpp:18-23`, `B/ladder/ladder_wire.hpp:132-136`: 같은 binary body를 DTO에 생성·stamp, 기본 serializer로 JSON encode | `I/contracts/channels/channel.hpp:835-837,900-907` → `I/contracts/codecs/serializer.hpp:320-332,345-375`. L4 typed codec; `dto_traits`만 선언, 메시지별 codec 등록 없음 |
| source header·framing·submit | `B/ladder/ladder_wire.hpp:136-150`, `B/ladder/ladder_driver.inc:121-142` | `B/ladder/ladder_wire.hpp:146-160`, `B/ladder/ladder_driver.inc:121-147` | 기존 L2 envelope와 L3 service framing 유지. Content type만 serializer의 `application/json` 사용(L4). 실제 binding send/request, observer, timeout·part 수 유지 |
| target wait·permit·frame decode | `R/mesh/raw_mesh_node_owner.cpp:526-541`, `B/ladder/ladder_wire.hpp:235-241,384` | 같은 `R` 호출, `B/ladder/ladder_wire.hpp:253-260,412` | L1 permit, blocking recv(NONE), drain 0, retained frame·header 수명 유지. 추가 worker·scope 없음 |
| target typed deserialize·handler | `B/ladder/ladder_wire.hpp:247-250`: protobuf ParseFromArray, metrics | `B/ladder/ladder_wire.hpp:264-269`: `deserialize_typed_payload`, 같은 metrics | `R/channels/route_handler_registry.hpp:144-146` → `I/contracts/codecs/serializer.hpp:478-485,504-510`. L4 JSON decode와 content-type 검증. 고정 handler 직접 실행 유지 |
| target reply 새 body·encode | `B/ladder/ladder_wire.hpp:263-272`: 새 protobuf body 복사·encode | `B/ladder/ladder_wire.hpp:278-296`: 새 DTO body 복사·typed serialize, 기존 reply writer | `R/channels/route_handler_registry.hpp:147`, `I/contracts/detail/handler_invocation.hpp`의 `serialize_handler_result` → `I/contracts/codecs/serializer.hpp:513-515`; `R/channels/route_packet_dispatcher.cpp:444`. L4 새 JSON body encode. Request parts 재사용 없음 |
| target reply frame·submit | `B/ladder/ladder_wire.hpp:288-292` | `B/ladder/ladder_wire.hpp:311-315` | 기존 L3 reply header/correlation·multipart pack·같은 2-part binding reply submit 유지 |
| source completion wait·parse | `B/ladder/ladder_driver.inc:40,132-142`, `B/ladder/ladder_wire.hpp:301-323` | 같은 binding pollcompletion wait/observers, `B/ladder/ladder_wire.hpp:324-345` | 기존 service correlation/terminal 검사·envelope parse 1회·error mapper 유지. Registry·dispatcher·deadline queue 없음 |
| source completion typed body·counter | `B/ladder/ladder_wire.hpp:324`, `B/ladder/ladder_driver.inc:135-136`: protobuf decode→finish | `B/ladder/ladder_wire.hpp:347-348`, `B/ladder/ladder_driver.inc:136-138`: typed decode→finish_bytes | `I/contracts/channels/call.hpp:246` → `I/contracts/codecs/serializer.hpp:345-375`. L4 JSON decode. `finish`이 원래 호출하던 동일 `finish_bytes`를 사용해 protobuf 중간 객체/추가 body copy 없이 같은 검증·counter 수행 |

DTO의 `vector<byte>`는 Framework의 기존 JSON profile을 통해 base64 문자열이 된다. 수동 JSON/base64 codec이나 protobuf↔DTO adapter는 없다. L0~L3는 protobuf 그대로다. L4는 body allocation·decode/encode를 JSON 경로로 바꾸고 JSON wire 크기가 늘어난다. 추가 wait·drain·metrics counter·queue·lock은 없다. Serializer registry의 기본 타입 선택과 immutable request plan은 typed codec 소유다. L6 activation을 복원하기 전에는 unhosted serializer registry 경로를 쓰며, L5_7에서는 production handler의 immutable codec plan을 host 시작 시 binding한다.

비교 대안: protobuf extension을 기존 placeholder에 연결하면 기본 JSON 경로를 복원하지 못한다. DTO metadata로 기본 serializer를 고르면 production serializer 선택·오류 변환·binary field 규칙을 한 곳에서 사용한다. 후자를 구현했다.

교차언어: Java `18a25db605`, Node `33b6c9b030`은 기본 typed JSON을 사용한다. .NET 참고 commit `a7fd48d1b1`은 typed protobuf extension을 사용한다. 복원 기능은 모두 typed payload codec이며, C++은 이번 지시의 기본 typed JSON 조건을 따른다. 다른 branch/worktree는 변경하지 않았다.

소유 계층: Framework typed codec. Spec `00-foundation/05-message-model.ko.md`, `00-foundation/06-framework-api.ko.md`의 기본 typed JSON 및 serializer 계약. Production runtime 동작은 변경하지 않는다. 분류 A(기존 계약 경로를 실험 단계에 연결). 판정 위치 수: JSON encode/decode/type 검증 각 1→1; 신규 codec·정책 상태 없음. 복원 범위 밖 차이 0.

검증 근거와 commit은 [report.md](report.md)에 기록한다. 다음 비교는 L3 대 L4, send-saturation·request-serial·request-backpressure × 1024/4096 B의 6셀이다.
