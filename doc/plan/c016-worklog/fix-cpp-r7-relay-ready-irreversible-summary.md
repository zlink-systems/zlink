# C++ F-R7-5: relay-ready 수락 뒤 source dispatch 재개 제거

## 결과와 계약

Cutover 결과를 알 수 없는 source는 reconciliation deadline에 target commit이 확인되면
target route를 채택한다. 확인되지 않으면 보류한 request에 `Unavailable` terminal을 보내고
dispatch 차단을 유지한다. Store가 source를 가리키거나 commit 기록이 없어도 같은 규칙을 적용한다.

- 소유 계층: C++ Framework의 `spot_node_runtime_t` relocation reconciliation.
- Spec 조항: `framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:568–578`,
  §9 실패 표 뒤 문단. 비가역 경계는 relay-ready reply accepted이며, source snapshot은
  target의 Restore 유효시간 안 CAS 가능성을 부정하지 못한다. 감독 판정은 D-107/F-R7-5.
- 변경 분류: **B — 기존 결함 수정**. 사용자 작업 지시에서 확인·승인한 원인을 수정했다.
- 교차언어 대조: Node `service-relocation-host-runtime.ts:2079–2082,2200–2204`는
  `readyReceived` 뒤 `abortSource()` 호출을 차단한다. Java
  `ZLinkCanonicalRelocationStateMachine.java:262–269`는 cutover submit 성공·실패 모두 source
  attempt를 제거하고 재전송용 복사본을 유지한다. 이 job에서 Java의 전체 relocation 실패 경로를
  재검증하지는 않았다. C++에만 있던 source snapshot 기반 재개 예외를 제거하며, 언어 구조상
  필요한 차이는 아니다.
- 수정 전/후 규칙 수: Store 조회 결과에 따른 **3가지 처리 → 2가지 처리**
  (target 채택 / source 재개 / indeterminate 실패 → target 채택 / 미확인 실패).

## 원인과 diff 분리

변경 전 `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp:6231–6247`은
commit 기록 없음 또는 source fence 일치를 `source_owns`로 판정했다. 같은 파일의
`:6288–6289`는 이 결과를 `replay_actor_handoff_until_move_closed()`에 연결해 move를 닫았다.
이후 source에서 dispatch할 수 있어 target의 늦은 CAS와 경쟁할 수 있었다.

이번 diff는 **F-R7-5 한 원인**으로 묶을 수 있다. 다른 원인의 runtime 수정은 섞지 않았다.

| 변경 파일 | 내용 |
|---|---|
| `framework/languages/cpp/framework/src/runtime/spots/spot_runtime.cpp` | `source_owns` 판정·분기·재개 marker 삭제. target commit 확인 여부만 남기고 미확인 결과를 기존 `fast_fail_reconcile_backlog()`로 통합. |
| `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_actor_gateway.cpp` | 옛 local-service 복원 기대를 §9에 맞게 교체. source 기록·commit 기록 없음·Store 미설정에 공통 fixture 사용. 실제 error reply와 handler 미실행, dispatch 차단 유지 검증. target 채택 테스트 유지·deadline 전후 검증 추가. |
| `framework/languages/cpp/CMakeLists.txt` | 네 reconciliation 사례를 개별 CTest로 등록. |
| 이 요약 파일 | 계약 근거, 검증 결과와 감독자 확인 항목. |

`replay_actor_handoff_until_move_closed()`는 local Join 실패·완료, remote transfer의 명시적 실패,
target context 없는 reconciliation에서도 호출하므로 삭제하지 않았다. 이번 원인의
source snapshot 비교와 그에 따른 재개 경로만 제거했다. 새 runtime 상태·timeout·retry·poller·
catch-all은 추가하지 않았다. Core, binding, 다른 언어, 보호 문서는 수정하지 않았고 commit하지 않았다.

## 회귀 검증

테스트는 relay-ready 이후 cutover 결과 불명 상태를 기존 coordinator의 `mark_reconcile()`로
구성하고 실제 production sweep·forward-drain·terminal reply 경로를 실행한다.
별도 프로세스 두 개의 CAS 경쟁을 실행하는 E2E는 아니다.

| CTest | 검증 |
|---|---|
| `test_cpp_framework_reconcile_source` | source fence와 일치하고 target commit이 아닌 Store 기록에서도 deadline 전 request 보류, deadline 뒤 `unavailable` error reply 1회, pending reply 제거, handler 0회, dispatch 차단 유지, local route 미채택. 후속 send와 다음 sweep에서도 source를 재개하지 않음. |
| `test_cpp_framework_reconcile_no-commit` | commit 기록이 없어도 위와 같은 terminal·차단 규칙 적용. |
| `test_cpp_framework_reconcile_indeterminate` | Store 미설정에서도 같은 규칙 적용. |
| `test_cpp_framework_reconcile_target` | deadline 전 dispatch 차단, deadline 뒤 target node·Spot route 채택, backlog forward 1회, move 종료. |

Deadline 전후는 `steady_clock` 시각을 기존 `cleanup_expired_actor_admissions_at()`에 주입한다.
기존 30 ms·100 ms sleep을 사용하지 않으며 runtime 시간 예산은 변경하지 않았다.

- 수정 전 runtime + 새 테스트: source/no-commit **2 FAIL**, target/indeterminate **2 PASS**.
- 수정 후: 네 회귀 테스트 각각 **5회 PASS — 20/20**, 0 failures.
- 기존 `test_cpp_framework_actor_gateway`: **PASS**, 0 failures.
- `git diff --check`: **PASS**.

## Gate 결과

`linux-ninja-debug` 전체 preset 빌드: **PASS** (112 build steps, exit 0).
최종 CTest: **69 PASS / 2 FAIL / 71 실행**, 130.22초, exit 8.
새 reconciliation 테스트 4개와 기존 actor gateway는 최종 gate에서도 통과했다.
Sample runner 7개를 제외했으며, 아래 두 실패 때문에 요청된 0 failures 조건은 충족하지 못했다.

실행 명령:

```bash
# framework/languages/cpp에서 전체 preset 빌드
cmake --build --preset linux-ninja-debug -j2

# 저장소 root에서 회귀 테스트 각각 5회
flock -w7200 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build/linux-ninja-debug -R '^test_cpp_framework_reconcile_' --repeat until-fail:5 --output-on-failure

# 저장소 root에서 최종 gate 1회 (감독자 전용 sample runner 제외)
flock -w7200 /tmp/zlink-cpp-gate.lock ctest --test-dir framework/languages/cpp/build/linux-ninja-debug --output-on-failure -LE '^framework-sample-smoke$'
```

테스트 등록의 label은 기존 taxonomy의 `relocation`을 사용한다. 최종 gate 전에
`cmake --preset linux-ninja-debug`로 등록을 갱신했다.

로그 디렉터리: `/tmp/zlink-cpp-r7-relay-ready-ChfTdN/`.
회귀 로그는 `reconcile-before-fix.log`, `reconcile-5x.log`, `actor-gateway.log`에 보존했다.
전체 빌드 로그는 `build-preset.log`, 최종 gate 로그는 `ctest-final-no-sample-runners.log`다.

## BLOCKERS

1. **남은 gate 실패 — 이번 변경 밖의 두 원인.**
   `test_cpp_framework_common_e2e_inventory`는 14 configs / 361 scenarios에서
   feature-map-missing=94, source-missing=122, incomplete-status=62, 총 **278건**으로 실패했다.
   작업 시작 때 이미 있던 `doc/plan/c016-worklog/cpp-common-e2e-inventory-278-summary.md:8–10`의
   baseline 수치와 일치한다. `test_cpp_framework_label_contract`는
   `framework/languages/cpp/tests/Zlink.Framework.ContractTests/verify_ctest_label_contract.cmake:189`에서
   **`framework-r6-parity` 미등록**으로 실패했다. 해당 label은 작업 전 HEAD의
   `framework/languages/cpp/CMakeLists.txt:1112,1117`에도 있고, HEAD의 taxonomy에도 등록돼 있지
   않음을 확인했다. 두 원인은 수정 범위에 포함하지 않았고 gate를 반복하지 않았다.
2. 요청된 제외 없는 `ctest --output-on-failure`에는 `samples/*/run_sample.sh` 7개가 등록돼 있다.
   같은 작업 지시가 sample runner를 감독자 gate로 제한하므로 이 job에서는 실행하지 않는다.
   최종 gate에는 `-LE '^framework-sample-smoke$'`를 사용했으며, 제외 없는 전체 gate는 감독자에게 남긴다.
3. 보호 문서 `framework/doc/framework/common/spec/server/05-location-relocation/04-relocation-flow.ko.md:566`의
   실패 표에는 아직 “Store가 여전히 source를 owner로 보이면 local dispatch를 복원”한다는 옛 문장이
   남아 있다. `:573–578`과 D-107에 모순되므로 감독자가 해당 표를 같은 비가역 규칙으로 맞춰야 한다.
   이 job은 지정된 표 뒤 조항을 따랐으며 문서를 수정하지 않았다.
