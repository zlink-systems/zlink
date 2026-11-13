# C++ Framework 경로 계수: #1639 step 0

현재 상태는 gate 대기입니다. `/root/jobs/fx/cx/v21/s033/core124b/verdict.md`가 아직 없어 빌드, 테스트, lock 획득과 측정을 시작하지 않았습니다. 기준 revision은 사용자가 지정한 `532cd83b67cdb4ec2a4ab9261d7a27459a376a37`입니다. git diff와 git status만 사용했습니다.

계측 패치는 `counters.diff`에 저장했습니다. CMake option `ZLINK_FRAMEWORK_CPP_PATH_COUNTERS`는 기본 OFF입니다. OFF에서는 계측 매크로가 빈 토큰열로 전개되며 상태, 분기와 atomic을 추가하지 않습니다. ON에서는 thread별 plain counter와 고정 크기 histogram을 쓰고 bench host와 worker가 종료·join된 뒤 프로세스 종료 시 JSON을 기록합니다. Retire한 thread의 저장소도 그때까지 보관합니다. 새 lock, atomic RMW, 갱신 중 할당과 출력은 없습니다. Thread별 첫 사용 때만 저장소를 만들고 Linux TID별 독립 slot에 게시합니다. Warmup/active selector는 기존 phase 경계에서만 store하며 카운터 갱신은 selector load만 수행합니다. gate 이후 빌드와 원시 count 대조로 확인할 예정입니다.

카운터는 target의 application executor 통지와 실제 대기 복귀를 구분합니다. 수신 회전, owner drain과 worker 작업당 건수도 따로 기록합니다. Source는 dispatcher 게시와 실행, logical registry callback 실행, application 최종 완료 통지와 admission 통지를 따로 기록합니다. 즉시 완료 send에는 completion 통지가 없다는 점을 별도 계수합니다. Admission pending은 binding이 반환한 `ZLINK_SUBMIT_BACKPRESSURED` snapshot으로 판정하며 추가 readiness 조회를 하지 않습니다.

빌드와 libzlink SHA-256, 실행별 분포와 합계, ON/OFF 처리량, 예산 재계산과 리팩토링 점검 결과는 측정 뒤 채웁니다. 현재 worktree에는 .artifacts와 build가 없어 기존 dependency cache의 읽기 허용을 사용자에게 확인 중입니다.

## 1. 빌드와 native library

빌드하지 않았으므로 libzlink SHA-256과 빌드 성공 여부는 미확인입니다. Core gate 파일의 존재를 여러 차례 확인했으나 아직 없습니다. 재개 시 기존 명령의 잔존 프로세스를 확인했으며 이 작업의 빌드·벤치 프로세스는 없었습니다. `raw/`에도 중단된 측정 파일이 없으므로 `.stopped`로 바꿀 파일은 없었습니다.

## 2. 카운터 위치

아래 경로는 worktree 기준입니다. Runtime 접두어 C는 `framework/languages/cpp/framework/src/runtime/`, bench 접두어 P는 `framework/bench/grpc/cpp/`입니다. 위치는 현재 계측 패치가 적용된 파일 기준입니다.

| 계수 | 증가·관측 위치 | 정의와의 관계 |
|---|---|---|
| Worker notification | C`dispatch/offload_executor.cpp:108,170,183` | 기존 `_ready.notify_one()` 직전입니다. `zlink-mesh-app` executor만 셉니다. |
| Worker wait/wake | C`dispatch/offload_executor.cpp:340,353` | 기존 mutex 안에서 queue가 비어 실제 대기할 때와 성공적으로 복귀했을 때를 구분합니다. Queue가 이미 있어 predicate가 즉시 참인 경우는 실제 wake로 세지 않습니다. |
| Worker record | C`mesh/mesh_node_host_service.cpp:3782` | Worker가 mailbox에서 받은 node send/request record를 application dispatch에 인도하는 지점입니다. Receive만 한 record와 구분합니다. |
| Worker turn 분포 | C`dispatch/offload_executor.cpp:386` | 각 기존 queued application work의 실행 구간에서 처리한 record 수입니다. 다음 실제 wait까지 처리한 수는 별도 wake 구간으로 기록합니다. |
| Owner 게시·drain | C`mesh/mesh_node_host_service.cpp:3361,3882,3893` | Owner 작업 게시 수와 그 drain coroutine이 인도한 record 수를 따로 셉니다. Coroutine local은 suspend 뒤에도 유지됩니다. |
| Ingress rotation | C`stateful/public_host_runtime.cpp:7544,7567` | 실제 receive 회전의 `pumped == application` record만 셉니다. 관리 작업을 포함하는 기존 반환 count는 별도 `rotation_activity`로 C`mesh/mesh_node_host_service.cpp:2354`에서 기록합니다. |
| Completion post/run | C`dispatch/provider_task_context.cpp:80,190` | 기존 `boost::asio::post` 직전과 posted callback 시작에서 각각 셉니다. Turn별 logical completion과 application 최종 완료 수도 기록합니다. |
| Logical completion | C`foundation/operation_registry.cpp:282,285` | Registry completion을 게시하는 지점과 실제 callback을 실행하는 지점입니다. |
| Application completion | P`common/bench_async.hpp:48,62,73`; P`client/bench_cpp_client.cpp:957` | 즉시 완료, admission-only 통지, 최종 완료 통지, caller가 꺼낸 completion batch와 소비한 최종 결과를 구분합니다. |
| p_s | C`backend/raw_route_port.cpp:214,216` | Binding send `.async()` 호출 수와 반환 snapshot이 `ZLINK_SUBMIT_BACKPRESSURED`인 수입니다. 이 branch가 기존 observer와 logical pending 경로로 이어집니다. |
| p_r | C`backend/raw_binding_adapter.hpp:60,62` | Binding request `.async()` 호출 수와 admission pending snapshot 수입니다. 기존 `take_submission_admission`과 같은 snapshot을 읽습니다. |
| Phase | P`client/bench_cpp_client.cpp:1204`; P`common/bench_stats_server.hpp:37` | 기존 active 시작과 target reset에서만 selector를 변경합니다. Warmup과 active 원시 count를 모두 보관합니다. |

히스토그램은 8,192개의 고정 slot을 가진 정확한 sparse histogram입니다. `count`, `sum`, `max`, 각 bin과 `lost`를 원시 JSON에 남깁니다. 측정 뒤 `lost == 0`, 게시/실행/consumption 합계와 benchmark count를 대조해야 합니다. 실제 wake 구간이 phase 경계를 넘을 수 있으므로 wake 분포는 전체 run 합계도 함께 확인해야 합니다. Pending 비율은 race가 가능한 추가 readiness 검사 대신 binding의 최초 admission snapshot을 사용합니다.

## 3. 셀별 분포와 실행 합계

6개 셀의 K, rotation batch, K_c, p_s, p_r과 numerator/denominator는 모두 미측정입니다. 0으로 대체하지 않았습니다. Source port의 control-plane 작업 포함 여부와 application 제출 수의 일치도 측정 뒤 확인해야 합니다.

## 4. ON/OFF 처리량

각 셀 ON 3회와 OFF 3회를 아직 실행하지 않았습니다. `measure.py`는 기존 bench의 phase/settle/count 검사 함수를 사용하고 source 0–7, target 8–15에 고정합니다. Python supervisor가 두 프로세스를 관리하며 셸 `&`와 SIGKILL을 사용하지 않습니다. Measurement 시작 명령은 gate 이후 `flock -o /root/jobs/fx/cx/measure.lock` 안에서 실행해야 합니다. 실행별 MemAvailable과 uptime을 보관하도록 준비했습니다.

## 5. 설계 합계

`paper_model.py`와 `design-input.json`을 p0로 복사했습니다. 복사한 모델의 입출력 경로도 p0 안으로 바꿨습니다. 측정 입력이 없으므로 기존 K=K_c=8 가정을 실측값으로 바꾸거나 새 예산 판정을 하지 않았습니다. K와 K_c의 통지 기준 numerator/denominator를 확정한 뒤 측정값을 대입하는 코드와 산술 검증이 남아 있습니다.

## 6. 리팩토링 점검

다음 3개는 전달 단계의 중복 후보이며 이번 작업에서는 수정하지 않았습니다. 계약상 필요한 단계인지와 제거 가능 여부를 확정한 결함 목록은 아닙니다.

- Mailbox owner 알림이 host callable 게시와 executor 알림으로 이어집니다. C`mesh/service_mailbox.cpp:516`, C`mesh/mesh_node_host_service.cpp:3361`, C`dispatch/offload_executor.cpp:109`입니다.
- Pending send는 binding observer의 Framework task 완료에서 logical registry 완료를 거쳐 public task를 완료합니다. C`backend/raw_route_port.cpp:248`, C`mesh/raw_mesh_node_owner.cpp:1730,1739,1711`, C`foundation/operation_registry.cpp:282`입니다.
- Completion turn에서 native continuation scheduler가 별도 handler coroutine executor에 다시 게시할 수 있습니다. C`dispatch/provider_task_context.cpp:190`, C`dispatch/coroutine_executor.cpp:218,100`입니다. Bench는 그 뒤 completion queue와 condition variable 통지를 사용합니다. P`common/bench_async.hpp:62`입니다.

Runtime 결정의 소유와 판정 위치는 바꾸지 않았습니다. 규칙 수는 수정 전/후 동일합니다. 점검 후보 3개, 수정 0개, 본 작업에서 계수만 하고 남긴 후보 3개입니다.

현재 결과는 계측 준비 단계입니다. Gate 파일 부재 때문에 빌드·검증·측정과 예산 재계산이 남아 있습니다. 프로세스가 정지한 동안에는 파일을 조회하지 않으므로 gate 이후 재개가 필요합니다.
