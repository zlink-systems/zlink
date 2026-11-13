# #1639 C++ chained wake — mailbox notify 상한 실패

PM의 2026-11-10 16:24Z 결정을 구현했다. 기존 `scheduler_drain` 본체와 negative controls 6개는 각각 50회 통과했다. `mailbox_owner_dispatch` 본체는 28회째 **worker notify 8회 > 기존 상한 1회**로 실패했다. Hang은 아니며 exit 1이다. 재실행으로 실패를 지우지 않았고, 테스트·등록·상한을 변경하지 않았다. **완료 조건과 머지 조건 미충족**이다.

50회 전체 통과라는 선행 조건을 충족하지 못했으므로 전체 framework unit, Release bench build, base/chained 3회 비교와 chained Spot 단계 측정은 실행하지 않았다. 성능 개선 또는 머지 가능 판정은 하지 않는다.

## 작업 범위

- Branch/worktree: `perf/1639-cpp-worker-wake`, `/root/worktree/zlink-v21-s033/psched5`. 시작 HEAD는 `ecc0354996f2d1dfbbf3ed0524eec565e1300def`다.
- 로컬 커밋: **`bc25ac287c7843d54d5eb3cd7cce6524c54fb380`**. Runtime 후보 두 파일만 포함한다. 기존 bench pacer 4개 파일은 uncommitted로 유지했다. `pacer-before.patch`와 `pacer-after.patch`의 `cmp`가 통과했다.
- 테스트 source·CMake 등록·negative-control 판정 script의 diff는 없다. Hash는 `unchanged-tests.json`에 있다. Spec·다른 branch/worktree는 수정하지 않았다. Push·PR·GitHub·sub-agent·다른 AI 도구 호출은 없다.

## 결정 소유 표

| 결정 | 소유 조항 | 현재 판정 위치 | 유지할 소유자 |
|---|---|---|---|
| Node·Channel record별 독립 진행 | S02 §2 | C++ `runtime/dispatch/offload_executor.cpp:148,185,472`; .NET `Runtime/Execution/ZLinkProcessExecutionPool.cs:307,438`; Java·Kotlin 공용 `execution/ZLinkWorkerPool.java:59,80`; Node `runtime/backend/mesh-dispatch-pump.ts:146` | 언어별 application scheduler |
| worker 예약·깨움·확장 | S02 §10 | C++ `offload_executor.cpp:185`; 다른 언어는 위 실행 자원 소유 위치 | C++ `reserve_workers_locked` 한 곳 |
| worker별 연속 drain 예산 | S02 §9 | C++ `runtime/mesh/mesh_node_host_service.cpp:3956–4002`; .NET ApplicationSource; Java·Kotlin JVM executor; Node pump | 기존 owner drain |

원인: `ecc0354996`의 entry당 demand 1만으로는 단일 publication 뒤 첫 handler가 기다릴 때 다음 record를 실행할 worker를 예약하지 못한다. `burst`와 `independent`의 기존 진행 계약을 충족하려면 take가 다음 예약을 요청해야 한다.

대안 비교: source별 예약 flag는 worker phase와 예약 사실을 중복한다. 기존 reserved phase를 사용하고 예약 이유를 기존 grow 입력 대신 전달하는 방식을 선택한다. 새 persistent 상태 없이 예약 결정이 한 곳에 남는다. 다른 언어는 실행 자원 구현이 다르며, OS worker별 chained notify를 public contract로 요구하지 않는다. S02 §2의 독립 진행 결과는 유지한다.

## 구현과 판정 입력

- Runtime 변경 파일은 `framework/languages/cpp/framework/src/runtime/dispatch/offload_executor.{cpp,hpp}` 두 개다. `take_drain_locked`에서 record가 남으면 기존 예약 함수로 요청하며, 최초 worker 선택과 W5 연속 take가 같은 경로를 사용한다. Notify는 mutex 밖에서 한다.
- Publication demand는 `min(max_workers, roles + queue.size())`를 유지한다. Take demand만 최대 1로 제한하고 기존 reserved 수를 차감한다. 따라서 reserved worker가 하나라도 있으면 take는 추가 예약하지 않는다. 이 전역 보수적 확인은 source별 중복 예약도 막는다.
- 수정 전/후 규칙 수(예약 판정 위치)는 **1 → 1**, `reserve_workers_locked`다. 직접 입력 종류는 **7 → 7**: 최대 worker 수, role 수, queue entry 수, worker phase, 예약 이유, live worker 수, current worker. 기존 `grow` bool을 `publication/take/wait` enum으로 교체했다(값 종류 2 → 3). Demand 입력은 **2 → 2**(role 수·entry 수; 최대 worker 공통 상한). `record.more`는 기존 queue membership 판단 결과를 그대로 사용한다.
- Persistent flag·counter·queue·lock·timer 추가는 **0**이다. `try_take_drain`에 기존 notifications 자료형의 호출별 임시 저장소를 사용한다. Source별 marker는 추가하지 않았다.
- `pending()`은 기존 scheduler negative-terminal source의 호출(`test_cpp_framework_scheduler_drain.cpp:127`)이 남아 있으므로 virtual 선언과 필수 override를 유지했다. 테스트·등록·spec은 수정하지 않는다.
- 수정 전 재현: 8.318초 watchdog timeout, SIGTERM exit -15. `before-focused-results.json`, `before-test_cpp_framework_scheduler_drain-1-2107406-stacks.txt`에 burst handler future 대기와 main의 destructor → drain_until을 보존했다.

## 검증 결과

| 기존 등록 | 실행 결과 | 원자료 |
|---|---|---|
| scheduler_drain 본체 | 50/50 통과 | `test_cpp_framework_scheduler_drain-{1..50}.log` |
| scheduler negative items / wakes / stream-wakes / terminal / spot-gate / actor-gate | 각각 50/50 통과, 합계 300회 | `test_cpp_framework_scheduler_drain_negative_<mode>-{1..50}.log` |
| mailbox_owner_dispatch 본체 | 27회 통과, 28회째 exit 1로 중단 | `test_cpp_framework_mailbox_owner_dispatch-{1..28}.log` |
| mailbox negative extra-notify | 선행 실패로 미실행 | — |
| 전체 framework-unit | 미실행 | 250개 등록 목록은 `unit-registrations.json` |
| Release bench / base·chained 3회 / chained Spot stage | 미실행 | — |

전체 반복 결과는 `focused-results.json`, 등록별 실제 command는 `focused-registrations.json`, 실행기는 `run-focused.py`다. 총 **378회 실행: 377회 통과, 1회 실패**다. 수정 후 timeout은 0건이다. Negative controls의 통과는 기존 CMake oracle이 mutant exit 1을 확인한 결과이며 timeout·예외를 통과로 취급하지 않는다. Watchdog는 기존과 같은 8초이고, timeout 시 gdb stack을 먼저 보존하고 SIGTERM만 사용한다.

Scheduler 본체의 첫 실행은 다음 값을 기록했다.

- `burst records=1024 items=1 wakes=4 queued=1 turns=4 bound=1/4`
- `steady_stream records=1000 worker_notifications=4 ... workers_participating=4`
- Node·Channel 모두 `B_before_A_terminal=1 different_worker=1`
- Spot·Actor 모두 `gate_overlap=0`; shutdown의 permit 잔류는 0.

## 첫 실패의 경계

실패 원자료는 [mailbox 28회 로그](test_cpp_framework_mailbox_owner_dispatch-28.log)다.

```text
worker_notifications_for_129_records=8
workers_participating=16
blocked_handler_same_node_progress=1
blocked_handler=1 independent_owner=1 independent_completion=1
queued_records=128 handler_on_receive_thread=0
```

`framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_mailbox_owner_dispatch.cpp:303–306`은 129-record busy window 전체의 notify 상한을 **1**로 정한다. `:325`가 이 상한과 participant 수를 판정한다. 이번 실패에서는 같은 Node의 진행, 독립 owner·infrastructure completion, 수신 thread와 handler 분리가 모두 통과했다. Notify 상한만 초과했다. 첫 27회는 notify 0회가 25회, 1회가 2회였고, 28회째 8회였다. 집계는 `mailbox-counts.json`이다.

Chained 후보의 wake 결정은 `runtime/dispatch/offload_executor.cpp:190–224` 한 곳이다. `:196–197`에서 take 수요를 1로 제한하고 `:199–210`에서 이미 reserved인 worker를 차감하므로, **동시에 남은 예약 하나**와 **take당 추가 notify 최대 하나**를 유지한다. 그러나 worker가 reserved에서 running으로 넘어가면 다음 take는 다시 한 worker를 예약할 수 있다. 이 제한은 busy window 전체 notify가 하나라는 제한과 다르다. 관측된 8회는 기존 window oracle을 만족하지 못한 직접 증거다.

이 로그는 publication과 take의 notify 횟수를 따로 분류하지 않으므로 8회를 각 호출 종류에 배분하지 않는다. 정확한 interleaving stack은 수집하지 않았다. 실패는 정상 종료의 oracle rejection이어서 hang stack은 없으며, 위의 수정 전 burst 재현 stack만 존재한다. Timeout 증가, 재시도, 선행 순서 강제, 추가 상태 또는 assertion 완화는 적용하지 않았다. PM의 최종 규칙을 벗어난 후속 수정도 하지 않았다.

## Build와 검증 명령

- Configure: `bash /root/jobs/fx/cx/v21/s033/psched7/configure-tests.sh`.
- 수정 전·후 build: `/root/jobs/fx/cx/cpp-build-slot.sh cmake --build .psched5-tests --parallel 3 --target test_cpp_framework_scheduler_drain test_cpp_framework_mailbox_owner_dispatch`.
- 수정 전 재현: `python3 /root/jobs/fx/cx/v21/s033/psched7/run-focused.py --before`.
- 수정 후 반복: `python3 /root/jobs/fx/cx/v21/s033/psched7/run-focused.py`.
- Release `-O3 -DNDEBUG`, test probes ON, install OFF, 추가 `-g` 없음. 빌드는 모두 bare build-slot wrapper를 거쳤다. Build directory 크기는 최종 326 MiB로 25 GB 미만이다.
- 설정과 binary hash/mtime: `test-CMakeCache.txt`, `test-binaries.json`, `test-registrations.cmake`. 로그: `configure-tests.log`, `build-tests-before.log`, `build-tests.log`.
- `git diff --check`, 테스트·등록 파일 diff 없음, bench pacer `cmp`, JOB shell 문법·Python compile 검사는 통과했다. Scheduler 이름을 찾는 문자열 gate 참조는 관련 tests/cmake에서 발견하지 않았다.

## Base 대비 머지 조건

이번 작업의 **새 base 3회 값과 chained 값은 모두 없다**. 과거 psmeas 수치를 이번 결과로 재사용하지 않는다.

| 고정 조건 | 이번 base / chained | 판정 |
|---|---|---|
| target I/O sendto/msg ≤ base | 미측정 / 미측정 | 판정 불가 |
| Spot p99가 base 자체 3회 범위 이내 | 미측정 / 미측정 | 판정 불가 |
| send 1 KiB 처리량이 base 범위 이내 또는 개선 | 미측정 / 미측정 | 판정 불가 |
| request 1 KiB 처리량이 base 범위 이내 또는 개선 | 미측정 / 미측정 | 판정 불가 |
| app-worker µs/msg ≤ base | 미측정 / 미측정 | 판정 불가 |
| target voluntary wakes/msg ≤ base | 미측정 / 미측정 | 판정 불가 |

제공된 base build `/root/worktree/zlink-v21-s033/psmeas-base/.psmeas-build`의 5개 binary 존재·hash·mtime과 cache는 `base-binaries.json`, `base-CMakeCache.txt`, `base-head.txt`에 보존했다. Base build는 변경하거나 삭제하지 않았다.

후속 실행용 harness는 `measure.sh`, `summarize.py`, `configure-branch.sh`에 준비했다. 부하·readiness wait·source 0–7 / target 8–15·3회 순서는 기존 psmeas와 같다. 실제 측정을 실행하지 않았으므로 측정 raw 경로도 없다. `summarize.py`는 chained의 변동 폭을 기준 확대에 쓰지 않고 base 자체 범위만 쓴다.

Spot 분석 방법은 기존 psched5 pm-queue 원자료로 확인했다. 기존 239개 요청의 p50/p99를 동일하게 재현했다: HTTP → gate submit 1,192/5,489 µs, submit → run 33/2,710 µs, run → handler 40/67 µs, handler → HTTP resume 134/3,503 µs. 이는 **이전 빌드의 방법 확인값**이며 chained 성능이 아니다. 보존 경로는 `stage-method-check/{stages.txt,events.json,request-stages.json,stage-summary.json}`다. 이번 chained에서 새 uprobe를 설치하거나 단계 측정을 실행하지 않았다.

## 소유·분류·남은 실패

- 소유 계층: Framework application scheduler, C++ `reserve_workers_locked` 한 곳.
- Spec 조항: S02 §2 record별 독립 진행, §9 worker별 연속 drain 예산, §10 공유 실행 자원. Public 계약 변경은 없다.
- 교차언어 대조: 위 결정 소유 표의 .NET, Java·Kotlin 공용, Node 실행 경로를 읽어 확인했다. 실행 자원별 구현이 다르며 동일 OS-worker wake 알고리즘을 강제하는 계약은 없다. 이번 수정은 C++ 내부 예약 규칙만 다루며 다른 언어 동작을 변경하지 않는다.
- 변경 분류: **B(기존 결함 수정 후보)**. 단일 publication 뒤 burst 진행 부족은 해소했으나 기존 mailbox notify oracle에 회귀가 있어 완료·머지 승인 불가다.
- Runtime·호출 경로 점검: 발견 2건, 수정 1건(단일 publication 뒤 진행), 미해결 1건(mailbox busy-window notify 상한), 별도 이슈로 넘긴 항목 0건. Execution gate 우회 동기화나 새 판정 상태는 추가하지 않았다.
- 남은 실패: `mailbox_owner_dispatch` 28회째 notify 8 > 1. 승인된 규칙과 기존 oracle을 유지한 채 이 회귀를 해결해야 나머지 검증과 측정을 진행할 수 있다.

## 정리

Raw 출력·실패 로그·수정 전 stack·patch·hash와 JOB harness는 모두 보존한다. 시험 build의 Testing 자료를 JOB에 복사한 뒤 이번 `.psched5-tests`만 제거한다. 새 bench build는 만들지 않았다. Branch와 worktree, 기존 uncommitted pacer, 제공된 base build는 유지한다. 최종 정리 확인은 아래에 기록한다.

정리 확인: `test-build-evidence/`에 남은 build 검증 자료를 복사했다. `.psched5-tests`를 제거했으며 `.psched7-build`는 생성하지 않았다. 최종 git status는 기존 bench pacer 4개 파일만 modified다.
