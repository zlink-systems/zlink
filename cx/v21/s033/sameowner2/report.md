# #1639 same-owner 결정 소유와 수정 설계

## 결과

- 새 공개 API 재현도 **같은 Channel B `0/20`, 다른 Channel B `20/20`, gated Actor B `0/20`**이다. 마지막 경우는 A 해제 후 두 reply 검사까지 통과했다. 회귀 모드는 `independent` assertion에서 실패한다(exit 134). **B(기존 계약 결함)**이며 제품 구현은 감독 승인 대상이다(`final-regression.log:1`–`:3`).
- 이전 Actor readiness 실패를 재현하고 원인을 확인했다. fixed RID의 초기 publication은 기본 설정에서 최소 15초를 기다리므로 fixture의 ready 대기 10초보다 길다. 현재 fixture는 테스트용 lease 설정으로 준비를 마친다. 자세한 근거는 아래 readiness 절에 있다.
- 제품·spec 변경 0. 기존 미커밋 재현을 보존하고 테스트 파일 하나만 보완했다. diff는 `repro2.diff`다. 기준은 detached `76a4d7850664c2cb32e8d35837dac45d9d2a4057`이다(`repository-final.log`).

경로 약어: `C/` = 이 worktree의 `framework/languages/cpp/framework/src/runtime/`, `CT/` = `framework/languages/cpp/tests/Zlink.Framework.UnitTests/`, `D/` = `framework/languages/dotnet/src/Zlink.Framework/Runtime/`, `J/` = `framework/languages/java/zlink-framework-core/src/main/java/systems/zlink/framework/`. `S` = `framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md`. 제품 줄 번호는 위 HEAD 기준이며 원문 발췌는 `source-evidence.log`에 보존했다.

## 결정 소유 표 — 전/후 수

질문한 결정의 근거는 **S §2:97–105**다. Node/Channel에는 gate가 없고 FIFO take 뒤 각 pipeline은 다른 worker에서 독립 실행할 수 있다. 같은 gate 배제는 **S:106**, Actor claim과 Yield의 구별은 **S:122–130**이 소유한다.

직접 claim 허가식 **2→1**, ready/drain 판정 **7→1**, 합계 판정 위치 **9→2**를 제안한다. 수정 후 단일 소유 모듈은 **`service_mailbox_t`**다. 허가식 하나를 readiness 전이도 재사용하며, 실제 application gate는 별개 결정이다.

| 현재 판정 위치 | 현재 효과 | 수정 후 |
|---|---|---|
| `C/mesh/service_mailbox.cpp:212`, `:235` | active claim이 있으면 두 곳에서 거부 | 단일 eligibility 식: 독립 owner는 다음 원본, serial owner는 다음 원본과 active prefix 부재를 확인. **2→1** |
| `C/mesh/service_mailbox.cpp:161` | idle enqueue만 ready | 아래 일곱 위치의 ready/drain 판단을 한 전이에 모음. **7→1** |
| `C/mesh/service_mailbox.cpp:186` | ready phase가 아니면 claim 후보 제외 | 공통 전이가 만든 예약 소비 |
| `C/mesh/service_mailbox.cpp:218` | 지정 owner claim 뒤 ready 항목 제거 | 이전 예약 소비와 다음 예약 갱신을 원자적으로 처리 |
| `C/mesh/service_mailbox.cpp:278` | claim을 retained 상태에 묶음 | 독립 owner는 transfer 뒤 다음 record 공개 |
| `C/mesh/service_mailbox.cpp:471` | 마지막 settlement 후 다음 ready 생성 | 자기 claim 정리 후 공통 전이 사용 |
| `C/mesh/service_mailbox.cpp:511` | draining owner 전체를 한 worker가 독점 | 독립 owner의 예약은 한 record의 FIFO transfer까지 |
| `C/mesh/service_mailbox.cpp:525` | active가 남으면 retained | 과거 독립 drain의 종료가 새 예약을 덮어쓰지 않음 |

전체 production 진입점과 gated 의존 경로는 다음과 같다. 호출자별 동시 실행 flag는 추가하지 않는다.

| 경로 | 현재 위치·의존 | 보존할 소유 |
|---|---|---|
| ClientServer | `C/channels/channel_host_service.cpp:151`(거부 정리), `:389`(정상); `C/client_server/client_server_location_runtime.cpp:1629` | Channel로 한 번 등록. 동기 pipeline 전에 다음 ready 공개 |
| Mesh/standalone application | `C/stateful/public_host_runtime.cpp:8549`, `:7599`; generic claim 연결 `C/mesh/service_mailbox.cpp:191` | Node/Channel만 독립 claim. Spot/Actor ingress는 serial 유지 |
| Mesh owner drain | `C/mesh/mesh_node_host_service.cpp:3877`, `:3895`, `:3916` | mailbox 정책 재사용 |
| Spot | `C/stateful/public_host_runtime.cpp:7970`, `:8062`; gate 구성·결합 `C/spots/spot_runtime.cpp:995`, `:15060`; gate 취득 `C/execution/serial_execution_queue.cpp:1919` | 서로 다른 Actor owner도 shared Spot gate가 배제. 같은-owner mailbox 검사는 이 gate를 대신하지 않음 |
| Actor/raw stateful | `C/stateful/public_host_runtime.cpp:7721`, `:7688` → `C/stateful/raw_stateful_dispatch.cpp:869` → `C/stateful/stateful_object_runtime.cpp:1175` | native object claim과 실제 queue 유지. mailbox serial ingress도 유지해야 비동기 authority 준비 중 후속 record가 먼저 native queue에 도착하지 않음 |
| Infrastructure | `C/stateful/raw_stateful_dispatch.cpp:1646`; `C/stateful/public_host_runtime.cpp:6183`, `:7428` | application gate 없는 control 경로의 기존 serial claim 유지 |

## 설계안 — 파일:줄별 변경

1. **등록 시 owner 성질 결정.** `C/mesh/service_mailbox.cpp:105`, `:149`에서 Node/Channel은 독립 실행, Spot/Actor와 infrastructure는 serial로 등록한다. raw ClientServer는 생성 시 Channel 성질을 정한다(`C/client_server/raw_client_server_owner.cpp:197`). 임의 ChannelName을 key prefix로 추측하지 않는다. Mesh key 해석은 mailbox에 모으고 기존 `C/stateful/public_host_runtime.cpp:703`의 해석도 같은 소유자를 재사용한다.
2. **claim은 독립 owner에서 정확히 1 record.** `C/mesh/service_mailbox.cpp:245`의 limit을 이 정책으로 제한한다. serial owner budget과 첫 record의 기존 byte-budget 규칙은 유지한다(`:250`). 독립 claim의 callback은 mutex 안에서 원본을 consumer storage로 옮긴다. 그 뒤 다음 ready를 공개하고 handler에 진입한다. 따라서 take는 FIFO이며 pipeline/handler entry 순서는 고정하지 않는다. 기존 transfer는 `C/stateful/public_host_runtime.cpp:734`, `C/client_server/client_server_location_runtime.cpp:1631`에 있다. Channel host의 두 경로도 이 계약에 맞춘다. 독립 owner의 callback 없는 claim은 허용하지 않는다.
3. **원본과 settlement 수명 분리.** 기존 list는 `[claimed slots][unclaimed FIFO]`로 유지하고 첫 unclaimed cursor를 둔다. claim의 pointer vector를 list iterator vector로 바꾼다(`C/mesh/service_mailbox.hpp:439`, `:503`). B가 먼저 끝나면 B iterator만 지우며 A slot·reply capability는 유지한다. 현재처럼 list의 첫 N개를 지우지 않는다(`C/mesh/service_mailbox.cpp:465`). `_settled[index]`의 1회 결정과 `_remaining` 마지막-credit 판정은 유지한다(`:426`, `:432`). cut은 cursor를 경계로 기존 FIFO 원본을 가져가고 claimed slot은 credit 종료까지 보존한다(`:339`).
4. **한 readiness 전이.** 기존 예약 소비 → claim/transfer → 다음 ready 갱신을 같은 lock에서 처리한다. 후행 pop/erase가 새 예약을 지우지 않게 한다(`C/mesh/service_mailbox.cpp:193`, `:218`). 이미 ready 예약이 있으면 중복 공개하지 않는다. 독립 owner의 end는 다른 worker의 phase를 변경하지 않으며 serial owner는 기존 draining/retained 수명을 유지한다(`:507`, `:517`). application handler를 mailbox lock 안에서 실행하지 않는다.
5. **상태와 회계 정리.** 새 immutable owner 성질·cursor 2개 대신 counter 정의 6개를 제거한다. owner의 `messages/bytes/active_bytes`와 domain의 `active_messages/active_bytes`는 읽는 곳 없이 증감만 한다. owner `active_messages`는 `records.begin()!=cursor`가 대체한다(`owner-counter-uses.log`, `active-counter-uses.log`). pending은 `cursor!=end()`이며 domain pending `messages/bytes`는 유지한다. credit의 권위는 기존 claim별 `_remaining`이다. 실제 종료 대기용 application accounting도 유지한다(`C/client_server/client_server_location_runtime.cpp:1638`, `:1651`; `C/stateful/public_host_runtime.cpp:8541`). 새 queue·lock·mutable flag는 없다.

W5 drain budget은 유지한다(`C/channels/channel_host_service.cpp:383`; `C/client_server/client_server_location_runtime.cpp:1623`; `C/mesh/mesh_node_host_service.cpp:3879`). 빠른 handler는 같은 drain에서 여러 claim을 처리하며 record별 executor wrapper를 만들지 않는다. 대안인 모든 application active 검사 삭제는 gated authority 준비·FIFO·cut까지 다른 계층으로 옮겨야 한다. 호출자별 flag는 정책을 복제한다. 등록 시 성질을 정하는 안이 판정 위치가 가장 적다(위 진입점 표, `C/stateful/public_host_runtime.cpp:8130`).

## trole2와의 접점

승인된 `../trole/design.md:123`의 handoff는 자원 배치, 이 설계는 take/claim eligibility를 소유한다. 공유 경계는 `C/mesh/mesh_node_host_service.cpp:2125`, `:3353`, `:3861`이다. **local A pipeline 전에 같은 독립 owner B의 work도 공개**해야 한다. role credit·실패 배치·W5를 유지하며 `C/dispatch/offload_executor.cpp`는 이 설계의 수정 대상이 아니다. 같은 Node/FIFO/gate 결합 시험은 승인 설계 T05와 함께 검증한다(`../trole/design.md:181`).

ready callback의 pop-before-callback도 공유 경계다(`C/mesh/service_mailbox.cpp:570`, `:574`; `../trole/design.md:134`). 수락 실패 때 원본·기존 예약을 보존하는 처리를 한 곳에 두며 별도 pending flag·fallback queue·재시도를 만들지 않는다.

## .NET·Java 대조

- **.NET도 같은 결함 경로가 있다.** Channel별 source(`D/Channels/ZLinkChannelReceiveLoop.cs:25`)의 기본 동시 turn 상한은 1(`D/Execution/ZLinkProcessExecutionPool.cs:266`, 배제 `:443`). `_dispatch` 직접 호출(`D/Channels/ZLinkChannelApplicationDispatchQueue.cs:137`) → unfiltered dispatcher(`D/Handlers/ZLinkHandlerDispatcher.cs:126`) → invoker(`D/Handlers/ZLinkHandlerInvocationEngine.cs:62`)가 동기 A에 묶인다. idle lane의 permit 반환은 동기 완료할 수 있다(`D/Execution/ZLinkStateLane.cs:111`, `:122`). 먼저 suspend하는 실행까지 항상 0/20이라고 단정하지는 않는다. mailbox 후보는 `D/Service/ZLinkManagedMeshNode.cs:2833`, `D/Service/ZLinkMeshOwnedMailbox.cs:104`, `:128`이다.
- **Java도 동기 A가 다음 같은 Channel entry를 막는다.** Channel별 serial queue 등록(`J/runtime/channels/ZLinkChannelDispatchRegistry.java:48`)과 active 배제(`J/execution/ZLinkSerialExecutionQueue.java:1955`), 직접 operation 호출(`:2595`; `J/runtime/channels/ZLinkHandlerDispatch.java:49`)이 결정한다. `STARTED` 반환(`:60`)은 동기 호출이 끝난 뒤다. 별도의 mailbox claim 배제는 `J/runtime/internal/service/ZLinkServiceMailbox.java:60`, `:97`이다. `ZLinkHandlerDispatch.java:65`의 Claim은 mailbox가 아니라 drain accounting token이다(`J/runtime/internal/drain/ZLinkMeshDrainCoordinator.java:22`).

두 언어는 소스 판정이며 실행하지 않았다. 기존 S §2와 충돌하는 B로 보고, 같은 #1639에서 교차언어 회귀를 검증한다.

## 시험안과 readiness 원인

현재 회귀는 공개 Channel API의 A latch → B 제출 → A 해제 전 진입 횟수를 검사한다. 다른 Channel은 `20/20`, 같은 gated Actor는 `0/20`과 두 reply를 assertion으로 확인한다(`CT/test_cpp_framework_client_server_runtime.cpp:394`, `:440`, `:535`, `:2752`). 승인 뒤 Node/Channel 사전 backlog·FIFO take/역순 entry·B→A 역순/중복 settlement·enqueue/cut/rollback/종료 경쟁·다른 gate 양성 대조·SpotWide/PerActor/Yield를 추가한다. ready 유실·중복 공개·record별 wrapper 변형을 검출해야 한다(위 설계 변경 위치).

**readiness는 gate 판정 전에 fixture에서 실패했다.** 명시적 RID(`CT/test_cpp_framework_client_server_runtime.cpp:462`)는 `fixed_rid_new_claim`을 선택한다(`C/mesh/mesh_node_host_service.cpp:63`). 빈 store도 reclaim 관찰을 기다리고(`C/locations/provider_location_repository.hpp:3313`), 조건은 `elapsed/3 >= margin`이다(`C/locations/location_repository.hpp:889`). 기본 margin 5000ms이므로 최소 15초다(`framework/languages/cpp/framework/include/zlink/framework/contracts/locations/options.hpp:20`). fixture의 ready 대기는 10초다(`CT/test_cpp_framework_client_server_runtime.cpp:477`). Store ready/lease 갱신만으로 serving이 되지 않고 descriptor publication 완료를 기다린다(`C/host/app.cpp:338`, `:353`; `C/locations/location_auto_connect_host_service.hpp:657`).

동일한 Channel→Actor 실행 순서에서 기본값은 preparing(state 0)에 머물며 readiness assertion을 재현했다(`default-locations.log:3`–`:5`, `default-locations.flow.actor:1`). 기존 테스트용 설정(renew/poll 10ms, margin 1ms)을 사용하면 정상 준비 후 Actor 대조군 20회가 통과했다(`CT/test_cpp_framework_client_server_runtime.cpp:451`; `repro2.log:485`). ready/요청 timeout과 gate assertion은 완화하지 않았다. Store 미등록은 준비 시간과 별개로 구성 단계에서 거부된다(`missing-store.log:4`; `C/host/app.cpp:202`).

## 검증 — 명령·로그

`P=/root/.cache/zlink/core/1.23.0/linux-x64`. Core 1.24.0 구성은 필수 Core 1.23.0 검사에서 실패했고 허용된 prefix로 전환했다(`configure.log:486`, `configure-core123.log`). 실제 `libzlink.so.0` 로드 경로도 1.23.0이다(`ldd.log:2`). 전체 명령은 `commands.txt`에 있다.

| 실행 | 결과·원시 로그 |
|---|---|
| 지정 preset/toolchain/options 구성 | 성공: `configure-core123.log` |
| 지정 wrapper, 해당 target, `--parallel 3` | full 112/112 및 test-only 2/2 성공: `build.log`, `build-readiness.log` |
| `LD_LIBRARY_PATH=$P/lib ... --same-owner-blocking-repro` | exit 0인 진단 실행. same `0/20`, different `20/20`, gated `0/20`: `repro2.log:2`, `:485` |
| `LD_LIBRARY_PATH=$P/lib ... --same-owner-regression` | 최종 source에서 **기대된 계약 실패**, exit 134: `final-regression.log:1`–`:3` |
| `... --same-owner-gated-default-locations` | readiness 음성 대조, exit 134: `default-locations.log:4`–`:5` |
| `... --same-owner-gated-missing-store` | Store 구성 음성 대조, exit 134: `missing-store.log:3`–`:4` |
| `git diff --check` | 성공: `diff-check.log` |

일반 gate 전체·cross-language·성능 측정은 실행하지 않았다. flow 파일, 바이너리/Core/source hash(`final-sha256.log`), guard 대기·25GiB 상한 기록도 JOB에 보존했다. build 디렉터리는 경로·CMake source·사용 중 process를 확인한 뒤 삭제했다. 삭제 전 크기는 1,546,149,121 bytes로 25GB 한도 안이었다(`cleanup.log:2`, `:3`).

## 리팩토링 점검

리팩토링 점검: 성능 1건, POSDDD 1건, 불필요 코드 5건 발견. 고친 항목 0, 넘긴 항목 #1639 감독 승인 대상 설계의 같은-owner 직렬 실행, 중복 claim 허가식, 읽지 않는 counter 5개(`C/mesh/service_mailbox.cpp:212`, `:235`, `:157`, `:158`, `:274`, `:276`, `:277`). 수정 전/후 판정 위치 수 **9→2**. 추가 상태 2개 대신 counter 정의 6개 제거를 제안한다.

## 남은 문제

제품 결함은 그대로이며 회귀 assertion은 실패한다. 감독 승인 뒤 구현·수정 후 통과·교차언어 실행·trole2 결합 검증이 필요하다. worktree와 기존 미커밋 테스트는 남겼다(`cleanup.log:3`, `repository-final.log`).
