# C++ L4 / L5~L7 구현 완료

실험 branch `exp/3868-cpp-ladder-next`, 시작 `dd83e75df4`. 두 단계 모두 로컬 commit만 했다. 측정·benchmark·부하·measure.lock·push·PR·GitHub·sub-agent·internal-docs 실행 없음.

| 단계 | 복원 기능 | 로컬 commit | 검증 |
|---|---|---|---|
| L4 #4354 | 기본 typed JSON으로 source encode·target decode·새 reply encode·source completion decode | `276b84f248` | Release build, bench unit/contract **11/11**, 실행 파일 **11개** source/header freshness 통과, 무부하 server 시작/종료 성공 |
| L5~L7 #4355~4357 | public Framework route의 owner queue/worker·dispatch별 handler activation/DI scope·operation registry/deadline/completion | `62e766f88d` | Release build, bench unit/contract **12/12**, 실행 파일 **12개** freshness 통과, 무부하 server 시작/종료 성공 |

`B=framework/bench/grpc/cpp`, `R=framework/languages/cpp/framework/src/runtime`, `I=framework/languages/cpp/framework/include/zlink/framework`. Line은 각 단계 commit 기준이다.

- **L4 production**: source `I/contracts/channels/channel.hpp:835-837,900-907`; target/reply `R/channels/route_handler_registry.hpp:144-147` → `I/contracts/codecs/serializer.hpp:345-375,478-515`; completion `I/contracts/channels/call.hpp:246`. **Bench**: `B/ladder/ladder_payload.hpp:21-26,38-41`, `B/ladder/ladder_wire.hpp:132-136,264-269,278-296,347-348`, `B/ladder/ladder_driver.inc:136-138`.
- **L5_7 production**: source `R/channels/channel_runtime.cpp:1481,1898-1940` → `R/mesh/raw_mesh_node_owner.cpp:1487-1504,1678-1724,1779-1839`; target `R/mesh/mesh_node_host_service.cpp:2163-2220,2299-2311,3365-3371,3869-3928` → `R/dispatch/offload_executor.cpp:427-508`; activation/scope `R/channels/route_handler_invoker.cpp:37-48,70-108`; reply `R/channels/route_packet_dispatcher.cpp:426-453`; completion `R/foundation/operation_registry.cpp:270-295,382-466,735-772` → `R/dispatch/provider_task_context.cpp:172-191` → `R/mesh/mesh_node_runtime.cpp:4383-4410`. **Bench**: `B/client/bench_cpp_client.cpp:713-744,803-809`, `B/framework/bench_framework_cpp_server.cpp:23-40,91-100`.

[L4 호출 대조](call-shape-L4.md): L3의 permit·blocking recv·drain 없음·framing·metrics·새 body·2-part submit을 유지하고 기본 JSON codec만 복원했다. Binary body는 기존 DTO의 `vector<byte>` 규칙으로 base64가 된다. 메시지별 codec 등록·수동 JSON/base64·protobuf 변환 없음. 정상 1K/4K와 잘못된 JSON/base64/type/content type 거부를 함께 확인했다.

[L5~L7 호출 대조](call-shape-L5-7.md): 같은 JSON DTO·body stamp·새 reply body·bench counters를 유지한다. Readiness/receive rotation·mailbox/worker는 L5, scope/instance와 immutable codec plan은 L6, operation identity/registry/deadline/completion 및 request storage는 L7에 귀속한다. Public route 계약 test는 1K/4K send/request, dispatch별 scope 생성·정리, handler 오류, deadline 만료, 늦은 reply 이후 terminal 보존·다음 요청 성공을 확인했다. 두 대조 모두 복원 범위 밖 차이 0. 실행 자원 전환 수는 계측하지 않았다.

| 단계 | Raw 근거 |
|---|---|
| L4 | [build](raw/build-L4-first.log)·[최종 증분 build](raw/build-L4-final.log), [test 11/11](raw/ctest-L4-final.log)·[정상/거부 상세](raw/ctest-L4-final-detail.log), [freshness·SHA256](raw/freshness-L4-final.json), [무부하 시작/종료](raw/dry-L4-final.json), [commit 연결](raw/commit-L4-evidence.json) |
| L5_7 | [build](raw/build-L5-7-first.log)·[최종 증분 build](raw/build-L5-7-final.log), [test 12/12](raw/ctest-L5-7-final.log)·[정상/오류/deadline 상세](raw/ctest-L5-7-final-detail.log), [source flow](raw/contract-L5-7-final.source.flow)·[target flow](raw/contract-L5-7-final.target.flow), [freshness·SHA256](raw/freshness-L5-7-final.json), [무부하 시작/종료](raw/dry-L5-7-final.json), [commit 연결](raw/commit-L5-7-evidence.json) |

두 dry run 모두 `/ready` 성공, `received=allPhaseMessages=errors=0`, 종료 코드 0이었다. Release·source binding·Core 1.25.0·`-j3`·25 GiB 제한은 [configure copy](tools/configure-build-inner.sh)를 `cpp-build-slot.sh bash`로 실행했다. [실제 Core 연결](raw/ldd-L5-7-final.log)도 확인했다. Raw 파일은 전부 보존했다.

소유 계층·spec: 기존 Framework codec/mesh owner/host worker/handler scope/operation registry가 소유한다. `00-foundation/05-message-model.ko.md`, `06-framework-api.ko.md` §10, `01-execution/08-messaging-hot-path.ko.md` §3~§4.4, `01-submit-and-completion.ko.md` §10~§11. Production runtime 동작·public API·spec 변경 없음.
교차언어: Java `18a25db605`/`4bf0f1c4bf`, .NET `a7fd48d1b1`/`de0507ae48`, Node `33b6c9b030`/`3c50a02908`을 `git show`로 대조했다. L5~L7 public route 묶음 범위는 같다. Java·Node는 기본 JSON, .NET 참고 L4는 typed protobuf이며 C++은 이번 지시대로 기본 JSON이다. 다른 branch/worktree는 수정하지 않았다. Node 지정 report 경로가 없어 같은 commit을 기록한 `lad-node-impl4/report.md`를 읽었다.
변경 분류: 두 단계 모두 **A(기존 계약 경로를 실험 단계에 연결)**.
규칙 수(판정 위치): codec·owner queue/worker·activation/scope·registry/deadline/completion의 기존 소유자 각각 **1→1**. 추가 정책 queue·lock·timer 0; 기존 driver/handler를 template으로 재사용했다.
점검: 새 gate 우회·중복 정책·중복 driver/handler 구현 발견 0건, 이관 0건.

같은 build의 `ZLINK_CPP_LADDER_LEVEL=0/1/2/3/4/7/8`로 선택한다. `7=L5_7`, `8=기존 main`이다. 측정 재개 후 아래 비교만 실행하며 이번 작업의 성능 판정은 없다.

| 비교 | 6셀 | 예산 |
|---|---|---|
| L3(3) 대 L4(4) | send-saturation·request-serial·request-backpressure × 1024/4096 B | 단계 7% |
| L4(4) 대 L5_7(7) | 같은 3 patterns × 2 sizes | 묶음 **21%** |

변경 파일: L4는 `B/CMakeLists.txt`, `B/framework/bench_framework_cpp_server.cpp`, `B/ladder/{ladder_payload.hpp,ladder_wire.hpp,ladder_driver.inc}`, `B/tests/ladder_contract_test.cpp`, `I/detail/ablation_ladder.hpp`. L5_7은 `B/CMakeLists.txt`, `B/client/bench_cpp_client.cpp`, `B/framework/bench_framework_cpp_server.cpp`, `B/tests/ladder_route_contract_test.cpp`, 같은 switch header다.

남은 실패 없음. Common rules에 따라 이번에 만든 `build-l3868-next`를 종료 시 삭제하고, compile commands·CMake cache·Ninja header dependencies·test/flow/build 로그를 `raw/`에 보존했다. 실험 worktree와 두 로컬 commit은 유지한다.
