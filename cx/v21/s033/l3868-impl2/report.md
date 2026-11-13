# C++ L2/L3 구현

실험 branch `exp/3868-cpp-ladder-next`, 시작 HEAD `a67753ede4`. 측정·benchmark·measure.lock·push·PR·GitHub·sub-agent·internal-docs 실행 없음. 기존 `l3868/build-l3868`은 사용하거나 수정하지 않았다.

| 단계 | 복원 기능 | 로컬 commit | 검증 |
|---|---|---|---|
| L2 #4352 | production envelope 생성·header 1회 parse·새 response header·completion parse | 빌드 후 기록 | Release 빌드 중 |
| L3 #4353 | L2 검증·커밋 뒤 진행 | 미구현 | — |

L2 production: `framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp:1652,1822`, `runtime/mesh/mesh_node_host_service.cpp:148`, `runtime/channels/channel_packet_dispatcher.cpp:248`(이하 runtime 경로는 같은 src 아래). Bench: `framework/bench/grpc/cpp/ladder/ladder_wire.hpp:114,210,237,286`, `ladder_driver.inc:118,135`. [네 경로 대조](call-shape-L2.md).

L2는 header 생성/파싱만 추가한다. L1 permit·blocking recv·protobuf parse·metrics·fresh reply body·2-part submit·binding completion은 유지한다. Header는 target에서 원본 bytes를 빌리며 reply까지 보존한다. Request header를 reply에서 다시 parse하지 않는다. Java/.NET 참고 L2와 달리 response encode·completion parse도 L2 범위에 포함했다.

검증 raw: [configure](raw/configure.log), [build](raw/build.log). 완료 후 test·바이너리 freshness·commit 근거를 추가한다.

소유 계층·spec: Framework envelope codec/reply writer; `02-channel-transport/06-wire-protocol.ko.md` §4 ClientServer application record, §2 Framework multipart application profile. Production runtime 동작은 바꾸지 않는다.
교차언어 대조: Java `bf83275257`, .NET `2590b2a030`; source encode/target parse는 공통, response/completion 복원 범위 차이는 위에 명시했다.
변경 분류: A(실험의 기존 production codec 경로 연결).
규칙 수: envelope 형식 codec 1→1, response header writer 1→1; 수신·응답 합계 중복 header parse 2→1. 추가 정책 상태·queue·lock 없음.
점검: 미복원 typed serializer 초기화와 수동 protobuf completion shortcut 제거. 남은 항목은 검증 뒤 기록한다.

측정 재개 시: 같은 최종 build에서 `ZLINK_CPP_LADDER_LEVEL=1` 대 `2`, L3 완료 시 `2` 대 `3`. 각 비교는 send-saturation·request-serial·request-backpressure × 1024/4096 B의 6셀만 측정한다. 기본 각 1회이며 경계 반복은 common-rules.md를 따른다. 이번 작업에서 성능 수치는 만들지 않는다.
