# C++ L4 / L5~L7 구현

실험 branch `exp/3868-cpp-ladder-next`, 시작 `dd83e75df4`. 측정·부하·measure.lock·push·PR·GitHub·sub-agent·internal-docs 실행 없음. 현재 구현·검증 진행 중이며 완료 후 아래 근거를 확정한다.

| 단계 | 복원 기능 | commit·검증 |
|---|---|---|
| L4 #4354 | 기본 typed JSON serializer로 source encode·target decode·새 reply encode·source completion decode. L0~L3 protobuf 유지 | 검증 중 |
| L5~L7 #4355~4357 | public Framework route로 owner queue/worker·handler activation/DI scope·operation registry/deadline/completion 복원 | L4 commit 후 적용 |

`B=framework/bench/grpc/cpp`, `R=framework/languages/cpp/framework/src/runtime`, `I=framework/languages/cpp/framework/include/zlink/framework`.

- L4 production: `I/contracts/channels/channel.hpp:835-837,900-907`, `R/channels/route_handler_registry.hpp:144-147` → `I/contracts/codecs/serializer.hpp:345-375,478-515`; completion `I/contracts/channels/call.hpp:246`. Bench `B/ladder/ladder_payload.hpp:21-26,38-41`, `B/ladder/ladder_wire.hpp:132-136,264-269,278-296,347-348`, `B/ladder/ladder_driver.inc:136-138`. [L4 호출 대조](call-shape-L4.md).

교차언어: Java `18a25db605`·Node `33b6c9b030`의 L4는 기본 typed JSON이다. .NET 참고 commit `a7fd48d1b1`은 typed protobuf extension이며, C++은 이번 지시대로 기본 typed JSON을 구현한다. 다른 branch/worktree는 수정하지 않는다. Node의 지정 report 경로 `lad-node-impl4c/report.md`는 없어 같은 commit을 기록한 `lad-node-impl4/report.md`를 읽었다.

측정 재개 시 같은 build에서 아래 두 비교를 각각 실행한다. 성능 수치는 이번 작업에 없다.

| 비교 | 6셀 | 예산 |
|---|---|---|
| L3(3) 대 L4(4) | send-saturation·request-serial·request-backpressure × 1024/4096 B | 단계 7% |
| L4(4) 대 L5_7(7) | 같은 3 patterns × 2 sizes | 묶음 21% |

남은 작업: 각 단계 최종 build, freshness, bench unit/contract test, 무부하 server dry start/stop, 호출 대조 확정, 로컬 commit.
