# C++ L2/L3 구현

실험 branch `exp/3868-cpp-ladder-next`, 시작 `a67753ede4`. 측정·benchmark·measure.lock·push·PR·GitHub·sub-agent·internal-docs 실행 없음. 기존 `l3868/build-l3868`은 수정하지 않았다.

| 단계 | 복원 기능 | 로컬 commit | 빌드·테스트 |
|---|---|---|---|
| L2 #4352 | 요청·응답 envelope 생성, target/completion header 각 1회 parse | `fbdcb16e26` | Release, bench 9/9, 바이너리 5개 freshness 통과 |
| L3 #4353 | node send/request/reply header, multipart pack/unpack, retained frame | 최종 검증 대기 | 초기 Release 빌드 통과; quiet guard 대기 |

`R=framework/languages/cpp/framework/src/runtime`, `B=framework/bench/grpc/cpp`. L2 line은 해당 커밋 기준이다.

- L2 production: `R/channels/channel_runtime.cpp:1898,1920,1822` → `R/messaging/client_call_codec.cpp:73,94`; target `R/mesh/mesh_node_host_service.cpp:148`; reply `R/channels/route_packet_dispatcher.cpp:444`. Bench `B/ladder/ladder_wire.hpp:123,226,253,301`, `ladder_driver.inc:135`. [호출 대조](call-shape-L2.md).
- L3 production: `R/messaging/client_call_codec.cpp:83`, `R/mesh/raw_mesh_node_owner.cpp:1412,1466,2182`, `R/protocol/service_wire_codec.cpp:3400`, `R/mesh/mesh_record_dispatcher.cpp:47`, `R/mesh/mesh_node_runtime.cpp:4406`. Bench `B/ladder/ladder_wire.hpp:85,144,148,235,253,288,301`. [호출 대조](call-shape-L3.md).

두 단계 모두 L1 permit·blocking recv·drain 없음·protobuf parse/새 reply encode·동일 metrics·2-part submit·binding completion을 유지한다. L2는 target header/payload를 빌린다. L3는 production retained Message decoder로 frame을 move하고 기존 `retained_header`에 header slice를 보존한다. Target payload와 source completion의 추가 byte copy는 없다. Service pack이 쓰는 bytes는 L3 비용이다. 미복원 L4~L7은 켜지 않는다.

L2 근거: [최종 build](raw/build-L2-final.log), [test 9/9](raw/ctest-L2.log), [freshness·sha256](raw/build-L2-evidence.json). 1K/4K command·request/reply와 correlation 보존, 잘못된 envelope/protobuf/error 거부를 socket 계약 테스트로 확인했다.

L3 최종 근거는 guard 해제 후 추가한다. 서버에 부하를 보내는 bench smoke는 실행하지 않았다.

소유 계층·spec: Framework envelope codec/reply writer와 service wire codec; `02-channel-transport/06-wire-protocol.ko.md` §2 multipart profile·§4 application record. Production runtime 동작은 바꾸지 않는다.
교차언어 대조: Java `bf83275257`, .NET `2590b2a030`의 source encode/target parse와 대조했다. C++ L2는 response encode·completion parse도 포함하며 참고 언어의 raw reply 유지 범위와 다르다.
변경 분류: A(실험 경로에 기존 production codec 연결).
규칙 수(판정 위치): envelope/service 형식 소유 codec은 각 1→1; L3 target header decoder 분기는 2→1. 추가 정책 queue·lock·flag 없음. Correlation stack 값은 reply 재파싱을 대신하며 header 수명은 기존 필드를 재사용한다.
점검: 4건 수정(type serializer 초기화, 수동 protobuf completion, generic request encoder, service prefix/correlation 중복 parse), 이관 0건.

측정 재개 시 같은 build의 `ZLINK_CPP_LADDER_LEVEL=1` 대 `2`, 이어서 `2` 대 `3`을 비교한다. 비교마다 send-saturation·request-serial·request-backpressure × 1024/4096 B의 6셀만 측정한다. 각 1회와 경계 반복은 common-rules.md를 따른다. 이번 작업의 성능 수치는 없다.
