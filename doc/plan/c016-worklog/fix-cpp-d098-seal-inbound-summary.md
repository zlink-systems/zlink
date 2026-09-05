# C++ D-098 inbound Hello shutdown seal

Shutdown seal을 관찰한 raw mesh는 inbound Hello를 소비하고 응답 없이 종료한다.
Admission candidate 조회·보류 queue 삽입·topology/liveness 갱신 전에 반환하므로 Hello로
admission 상태를 바꾸거나 Admit/Reject를 보내지 않는다. 이미 admitted인 peer의 Update 수신과
local Draining Update 송신은 계속 처리한다. `main`에서 작업했으며 commit은 없다.

## 진단과 변경 위치

경로 기준은 `framework/languages/cpp/`이며 행 번호는 변경 후 기준이다.

| 파일:행 | 근거와 변경 |
|---|---|
| `framework/src/runtime/mesh/raw_mesh_node_owner.cpp:2764` | 기존 Hello/Admit/Update 공통 처리에는 seal 조회가 없었다. Hello만 기존 `shutdown_admission_seal` atomic을 acquire로 조회하고 `infrastructure`로 반환한다. 정상 receive accounting은 유지한다. |
| `tests/Zlink.Framework.UnitTests/test_cpp_framework_mesh_shutdown_seal.cpp:122` | Sealed target이 실제 Hello를 소비한 뒤 source receive pump의 무응답, 양쪽 peer 부재, target 변경 알림 0회를 검증한다. Unsealed target은 양쪽 admission과 변경 알림 1회를 검증한다. |
| 같은 파일 `:165` | Admission 완료 후 seal을 설정한다. Source의 Draining Update가 target에 도착하고, target의 Update도 sealed source에서 동일 connection의 descriptor state/revision에 반영되는지 검증한다. |
| 같은 파일 `:83` | 재연결 fixture에서 admission 성공을 기대하는 상대는 unsealed로 구성한다. Sealed 상대의 무응답 조건과 기존 Serving/Draining 재admission 단언은 유지한다. D-097 fixture의 sealed 상대가 Admit을 보내던 전제가 D-098과 충돌한 부분이다. |

응답 송신부만 막는 대안은 이미 실행한 topology/liveness 변경을 남기고 `not_required`와 일반
성공 응답 경로마다 판단을 반복한다. Admission 처리 진입부에서 차단하는 대안을 선택해
모든 inbound Hello에 같은 규칙을 적용한다. 새 helper·flag·timer·poller·재시도는 없다.
Deferred Hello도 기존 pump 진입부를 다시 지나므로 처리 시점의 seal을 조회한다.

기존 seal 소유자는 `framework/src/runtime/host/app.cpp:449`의 `app_state_t::draining`이며,
`framework/src/runtime/mesh/mesh_node_runtime.cpp:800`이 동일 atomic을 raw options로 전달한다.
Descriptor의 Draining 값을 seal로 사용하지 않으므로 Relocate의 Draining은 admission을 닫지 않는다.
이 배선과 `raw_mesh_node_owner.cpp:663`의 Draining publication은 변경하지 않았다.

## 네 줄

- 소유 계층: Framework host가 shutdown seal을 소유하고 raw mesh가 logical peer admission을 결정한다. Physical connection·reconnect는 Core/binding 소유다.
- Spec 조항: `05-host-relocation-flow.ko.md:766` §14 step 1의 새 peer admission 시작·수락 금지, D-097 및 D-098 item 3. 이번 구현 요청이 해당 계약 적용의 승인이다.
- 교차언어 대조: 조사 시 .NET `ZLinkManagedMeshNode.cs`의 `ProcessAdmissionCore`와 Java `ZLinkJavaRawMeshNode.java`의 admission 처리에도 같은 수신 seal 누락이 있었다. 같은 작업 공간의 병행 수정에서는 각각 `:8088`, `:6353`에서 Hello만 기존 seal로 차단한다. C++도 동일 동작이며 언어 구조에 따른 예외는 없다. 타 언어 파일은 수정하지 않았다.
- 변경 분류: B — D-098으로 확정된 shutdown admission 계약의 기존 결함. Public API·wire schema 변경 없음.

## 수정 전/후 규칙 수

신규 peer admission의 shutdown 정책 **2→1**: outbound Hello는 seal을 따르고 inbound Hello는
무조건 처리하던 방향별 정책을, 양쪽 모두 기존 host seal을 따르는 정책으로 통일했다.
Seal 상태 소유자 **1→1**, 새 상태 **0개**. 코드상 Hello 진입 검사 분기는 1개 추가되지만
별도 정책이나 seal 사본은 추가하지 않았다. 기존 peer Update와 Draining publication 규칙은 유지한다.

## 검증 결과

| 검증 | 결과 | 로그 |
|---|---|---|
| `cmake --build build -j4` | exit 0 | `/tmp/cpp-d098-seal-build.log` |
| `ctest --test-dir build -R '^test_cpp_framework_mesh_shutdown_seal$' --output-on-failure` | 1/1 passed, 1.24 s | `/tmp/cpp-d098-seal-focused.log` |
| `ctest --test-dir build -L framework-unit --output-on-failure` | 42/42 passed, 48.08 s, exit 0 | `/tmp/cpp-d098-seal-unit.log` |
| `flock -w7200 /tmp/zlink-samples-gate.lock bash samples/run_samples.sh` ×1 | exit 1. 4/7 완료, GameQuest 로그 집계 실패, 나머지 2개 미실행 | `/tmp/cpp-d098-seal-samples.log` |
| `git diff --check` | passed | working tree |

전체 unit과 sample은 기존 `ZLINK_CPP_MESH_TRACE=1`,
`ZLINK_DEBUG_FRAMEWORK_SPOT_DISCOVERY=1`을 켜고 전체 출력을 파일로 보존했다.
`ldd build/test_cpp_framework_mesh_shutdown_seal`은
`.artifacts/wsl/install/zlink-core/0.17.0/lib/libzlink.so.0`을 사용한다.
Core/binding package 재빌드는 없었다.

## Sample 실패 근거

TicTacToe·Bingo·DeliveryDispatch·SupportChat은 완료했다. GameQuest의
`samples/GameQuest/run_sample.sh:380`은 `owner unavailable` 행 1회를 기대했지만 0회로 집계했다.
보존 로그 `:55`가 첫 실패이고 `:29147`에 실제 marker가 다음처럼 남아 있다.

```text
zlink mesh-host stage=gamequest-owner unavailable player=player-owner-failure
pump result=no-data pending=0
```

`samples/GameQuest/Server/GameApi/main.cpp:347`의 `std::cerr` marker와 활성화한 기존 mesh
trace가 같은 행에 섞였다. Runner의 `line_count` 완전 일치 검사에서는 이 행을 세지 못한다.
로그 `:57003–57004`에는 `gamequest-server-evidence=completed`, `gamequest=completed`가 있다.
Flow 파일들도 동일 보존 로그의 `:57005` 이후에 포함되어 있다. 첫 실패는 admission 결과가
아닌 로그 행 집계이며, 제가 켠 보조 trace가 이 실행의 간섭 조건이었다.

## BLOCKERS

- **7/7 sample gate 미달성.** GameQuest 로그 집계 실패로 ShoppingMall·ZoneWorld는 실행되지 않았다.
  사용자 지정 1회 실행을 지켰으며 gate를 재실행하거나 sample assertion을 변경하지 않았다.
  후속 gate에서는 기존 file flow를 유지하되 보조 stderr trace를 끈 실행이 필요하다.
- Build·focused·전체 unit 실패 없음. Inbound seal 구현 blocker 없음.
- 기존 Node binding 및 untracked 파일, 병행 .NET/Java 및 C++ InstanceSpot E2E 작업은 보존했다.
  이 작업의 변경은 위 C++ runtime/test와 이 요약 문서뿐이다.
