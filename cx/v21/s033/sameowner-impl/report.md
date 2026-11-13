# #1639 C++ same-owner 독립 claim — 구현·검증 완료

제품 commit `985c271c1b`, 시험 보완 commit `e7fe9fee89`·`179528ba0d`로 완료했다. 제품 7개, 시험 20개, CMake 1개 파일을 변경했다(`commit.log:1`, `final-repository.log:1`, `changed-files-count.log:1`; 목록 `changed-files.log:1`). 선택 시험 16개는 각각 **50/50 통과**, 세 실패 주입도 각각 **50/50 검출**했다(`repeat-counts.log:1`). 전체 1회에서 298/304 통과·6개 실패를 확인했고, 관련 fixture·라벨 수정 후 **관련 6개 모두 통과**했다(1차 5개·2차 m6b 1개). 남은 실패는 0건이다(`full-suite-counts.log:1`, `fixture-results.log:1`).

경로: `C/` = worktree의 `framework/languages/cpp/framework/src/runtime/`, `T/` = `framework/languages/cpp/tests/Zlink.Framework.UnitTests/`. `M` = `C/mesh/service_mailbox.cpp`, `MH` = 같은 경로의 `.hpp`, `H` = `T/test_cpp_framework_receive_role_handoff.cpp`, `U` = `T/test_cpp_framework_client_server_runtime.cpp`. `S` = `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md`. 로그 경로는 이 보고서의 디렉터리를 기준으로 한다.

## 구현과 소유

- `service_mailbox_t`가 등록 시 immutable owner kind를 정한다. raw Channel은 생성 시 명시적으로 등록하며, Mesh key 해석도 mailbox 한 곳에서 제공한다(`M:121`, `M:126`, `MH:443`, `C/client_server/raw_client_server_owner.cpp:197`, `C/stateful/public_host_runtime.cpp:730`).
- 독립 owner는 mutex 안에서 원본 하나를 consumer storage로 옮기고 다음 ready를 공개한다. 자기 claim의 iterator만 지워 B→A 역순 종료를 허용한다(`M:302`, `M:331`, `M:356`, `M:548`; Channel 소비 `C/channels/channel_host_service.cpp:156`, `:397`).
- 수정 전/후 규칙 수(판정 위치): 직접 claim 허가 **2→1**, ready/drain **7→1**, 합계 **9→2**. 전 위치는 `base-service_mailbox.cpp:212`, `:235`, `:161`, `:186`, `:218`, `:278`, `:471`, `:511`, `:525`; 후 위치는 `M:139`, `M:145`. 공통 전이 밖 phase 접근은 0곳이다(`decision-counter-audit.log:5`).
- owner/domain counter 정의는 **8→2**다. owner 4개와 domain active 2개를 제거하고 domain pending 2개를 유지했다. immutable kind와 cursor 표현 2개를 사용하며 claim별 `_remaining`·`_settled`는 유지한다(`decision-counter-audit.log:1`, `MH:443`, `MH:445`, `MH:521`, `M:509`, `M:515`). 새 persistent queue·lock·mutable flag·timer·thread는 추가하지 않았다(`final-product.diff:1`).
- W5 시간 budget과 ready callback 수락 후 제거 순서를 유지한다(`C/channels/channel_host_service.cpp:387`, `C/client_server/client_server_location_runtime.cpp:1624`, `M:626`). offload executor·role credit·receive-role handoff 구현은 변경하지 않았다(`unchanged-ownership-audit.log:1`, `changed-files.log:1`).

소유 계층: Framework의 `service_mailbox_t`가 claim과 readiness를 소유한다(`M:139`, `M:145`).
스펙 조항: Node/Channel 독립 pipeline은 `S:97`, 같은 gate 배제는 `S:106`, Actor claim과 Yield는 `S:123`이다.
교차언어 대조: 승인 보고서의 .NET·Java 소스 진단을 따른다. 이번 실행 범위는 C++이며 다른 언어 시험은 실행하지 않았다(`../sameowner2/report.md:55`, `:57`, `:58`; `validate-final.py:1`).
변경 분류: **B — 기존 계약 결함**이다(`S:97`, `fail-before-channel-01.log:1`).

## 시험과 실패 주입

각 시험은 32-record·1-worker owner-drain 관측기를 호출해 ready 유실, 중복 제출, record별 executor wrapper를 각각 주입한다(`T/same_owner_claim_controls.hpp:29`, `:94`). 최종 반복에서 정상은 제출 2회·전달 32/32, 유실은 전달 0/32, 중복은 제출 4회, wrapper는 제출 34회로 세 변형을 검출했다(`repeat-final-channel-01.log:3`). 아래 검출수 순서는 **유실/중복/wrapper**다. `P` = `repeat-counts.log`, `F` = `fail-before-classification.log`. 기준 제품의 ‘공통’은 이 관측기에서 실패했다는 뜻이다.

| 시험과 정의 | 반복 통과; 변형 검출 | 기준 제품 실패 지점 |
|---|---|---|
| Node 사전 backlog (`H:395`) | 50/50; 50/50/50 (`P:9`) | 공통 (`F:7`) |
| Channel 사전 backlog (`H:400`) | 50/50; 50/50/50 (`P:4`) | 공통 (`F:9`) |
| FIFO take·역순 entry (`H:405`) | 50/50; 50/50/50 (`P:8`) | 본체 (`F:11`) |
| B→A·중복 settlement (`H:446`) | 50/50; 50/50/50 (`P:11`) | 본체 (`F:17`) |
| enqueue·마지막 settlement 경쟁 (`H:476`) | 50/50; 50/50/50 (`P:7`) | 공통 (`F:19`) |
| cut·enqueue·claim 경쟁 (`H:505`) | 50/50; 50/50/50 (`P:6`) | 본체 (`F:21`) |
| rollback·enqueue 경쟁 (`H:555`) | 50/50; 50/50/50 (`P:12`) | 공통 (`F:25`) |
| shutdown·enqueue·settlement 경쟁 (`H:591`) | 50/50; 50/50/50 (`P:14`) | 공통 (`F:27`) |
| claim 할당 실패 (`H:622`) | 50/50; 50/50/50 (`P:5`) | 본체 (`F:29`) |
| ready 수락 실패 (`H:666`) | 50/50; 50/50/50 (`P:10`) | 본체 (`F:32`) |
| Spot/Actor/infrastructure serial budget·수명 (`H:695`) | 50/50; 50/50/50 (`P:13`) | 공통 (`F:38`) |
| 활성화한 T05 같은 Node (`H:834`) | 50/50; 50/50/50 (`P:2`) | 본체 (`F:3`) |
| 다른 gate·SpotWide/PerActor (`H:770`) | 50/50; 50/50/50 (`P:1`) | 공통, gate 본체 통과 (`F:1`) |
| ordinary await·Yield·PerActor 거부 (`H:895`) | 50/50; 50/50/50 (`P:3`) | 공통, gate 본체 통과 (`F:5`) |
| 공개 Channel (`U:385`) | 50/50; 50/50/50 (`P:15`) | 같은 Channel 0/20 (`fail-before-channel-01.log:1`) |
| 공개 gated Actor (`U:438`) | 50/50; 50/50/50 (`P:18`) | 공통, Actor 본체 통과 (`fail-before-actor-01.log:1`) |

공개 Channel 50회 반복은 매회 같은·다른 Channel 및 `actor:`로 시작하는 ChannelName 각각 B 선진입 20/20이다(`repeat-final-channel-01.log:1`, `:2`). Actor 50회 반복은 매회 0/20과 A·B reply 값 검사를 통과했다(`repeat-final-actor-01.log:1`, `U:518`). fixed RID fixture는 renew/poll 10ms, fencing margin 1ms와 location/relocation store를 사용한다(`U:447`).

Fail-before는 제품 7개 파일을 `perf/1639-cpp-receive-role-handoff`의 `3ea1463fd2`로 되돌려 실행했다. 기준 diff는 0 bytes였고 GTest 14/14 실패, 공개 시험 2개는 exit -6이었다(`fail-before-base.log:1`, `fail-before-product-diff.log`, `fail-before-driver.log:1`). 다른 Channel은 20/20, gated Actor 본체는 0/20·두 reply로 통과했다(`fail-before-channel-01.log:1`, `fail-before-actor-01.log:1`, `U:518`).

## 빌드·재검증과 한계

- 보존한 build: `/root/worktree/zlink-v21-s033/sameowner-impl/build-sameowner`, 9,728,087,534 bytes (`build-size-final.log:1`). `linux-ninja-release`, Framework tests ON, binding source ON, Core release `/root/.cache/zlink/core-release/1.24.0/linux-x64` 구성이다(`build-config-final.log:1`, `:6`, `:9`, `ldd-fail-before.log:2`).
- `cpp-build-slot.sh`, `-j3 -k0`, 주소공간 상한 26,843,545,600 bytes로 전체 빌드·최종 재링크·시험 보완 재빌드를 완료했다(`build-resource-limits.log:3`, `build-product-all.log:593`, `build-final.log:136`, `build-fixtures.exit:1`, `build-transfer.exit:1`). cpp format도 통과했다(`format-check-transfer.exit:1`).
- 기존 fixture는 한 번의 Node dispatch에 batch 전체를 기대하거나 handler 진입을 직렬로 가정했다. 3개 record FIFO는 claim 3회로, 64개 오류 회계는 record별 실패 64회로 검사해 기존 총량 검사를 유지했다(`T/test_cpp_framework_m6b_runtime.cpp:4113`, `T/test_cpp_framework_maintenance_failure.cpp:114`, `T/test_cpp_framework_relocation_seal_contract.cpp:1732`).
- m6b의 ACK marker·multicast 검사는 기존 raw consumer fixture로 원본을 mutex 안에서 옮겨 payload·routing·metadata 검사를 유지한다(`T/test_cpp_framework_m6b_runtime.cpp:4416`, `:4570`).
- Node bridge는 세 handler가 완료 0건인 동안 진입하고, 반환한 capacity를 기존 FIFO permit API로 확인한다(`T/test_cpp_framework_mesh_node_vertical.cpp:1446`, `:1485`). transport 시험은 A 해제 전 128개 각각 한 번 처리를 확인하며, coalescing은 공통 실패 주입 관측기로 검사한다(`T/test_cpp_framework_mailbox_owner_dispatch.cpp:96`, `:160`, `:292`). 이 transport 시험도 50/50 통과했고 세 변형을 각 50회 검출했다(`owner-dispatch-counts.log:1`).
- 새 CTest는 기존 등록 helper가 contract tier 하나를 부여한다. 잘못된 두 tier는 최초 label-contract 검사에서 검출했고, 수정 후 검사는 통과했다(`full-suite.log:257`, `fixture-results.log:3`, `framework/languages/cpp/CMakeLists.txt:950`).
- 시험 보완에서 제품 파일과 선택 시험 본문은 변경하지 않았다. 선택 시험 바이너리·제품 archive hash도 최초 50회 실행과 동일하다(`final-repository.log:4`, `:5`, `final-binary-verify.log:1`). 전체 suite는 요청대로 1회만 실행했고 보완한 6개를 다시 검사했다(`full-suite-counts.log:1`).
- 알려진 실패 0건이다. 검증 범위는 Linux Release/C++이며 다른 OS·교차언어 실행과 성능 측정은 포함하지 않는다(`full-suite-counts.log:3`, `validate-fixtures.py:1`, `../sameowner2/report.md:55`).

리팩토링 점검: 성능 1건, POSDDD 1건, 불필요 코드 6건 발견. 독립 owner의 불필요한 직렬 실행, 중복 판정·key 해석, 읽지 않는 counter 5개와 기존 CV wake 시험 관측기를 정리했다. 시험의 record별 중복 검사 배열을 추가하면서 관측기의 벡터·counter·flag·helper·wrap을 제거했다. 넘긴 항목 0건이다(`M:139`, `M:145`, `decision-counter-audit.log:1`, `T/test_cpp_framework_mailbox_owner_dispatch.cpp:77`, `fixture-corrections.diff:1`).
