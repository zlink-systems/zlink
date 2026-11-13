# #1639 narrowed worker reservation — 진행 시험 실패로 중단

결정된 `pending()` 수요 항 제거를 local commit **`ecc0354996f2d1dfbbf3ed0524eec565e1300def`**에 구현했다. 수정 후 첫 `test_cpp_framework_scheduler_drain` 실행이 `burst` 진행 조건에서 멈췄다. 사용자의 중단 조건에 따라 50회 반복의 나머지, 전체 framework unit 실행, bench build, base/narrowed 3회 측정과 Spot 단계 재측정은 실행하지 않았다. **머지 조건 미충족**이다.

## 변경과 판정 입력

- Branch/worktree: `perf/1639-cpp-worker-wake`, `/root/worktree/zlink-v21-s033/psched5`. Parent는 `37f78cba87`이다.
- 유일한 commit 파일: `framework/languages/cpp/framework/src/runtime/dispatch/offload_executor.cpp:189–190`. 1줄 추가, 8줄 삭제. Diff: [scheduler.patch](scheduler.patch).
- 이전: `min(max_workers, roles + Σ max(1, source.pending()))` (일반 work entry는 1). 이후: `min(max_workers, roles + queue.size())`.
- 수요 입력 종류 **3 → 2**: role 수, queue entry 수, source별 pending record 수 → role 수, queue entry 수. 최대 worker 수는 양쪽의 공통 상한이다.
- 예약 판정 전체의 외부 입력 종류 **8 → 7**: 최대 worker 수, role 수, queue entry 수, pending 수, 기존 worker phase, grow, live worker 수, current worker → pending 수 제거.
- 수정 전/후 규칙 수(판정 위치): **1 → 1**, `reserve_workers_locked:185`. 기존 idle/reserved/running phase를 유지했다. 새 flag·counter·queue·lock·timer는 없다.
- Queue에 남은 running source도 최대 1로 세는 이 최소 축소부터 실패했다. Running source를 0으로 제외하는 추가 판정은 구현하지 않았다. 그것은 아래 단일 publication의 진행 부족을 해결하지 못한다.
- Bench pacer 4개 파일의 uncommitted diff는 commit 전후 `cmp`로 동일함을 확인했다: `pacer-before.patch`, `pacer-after.patch`. 테스트·spec·다른 branch/worktree는 수정하지 않았다. Push·PR·GitHub 작업은 없다.

## 실패에 필요한 owner / entry 구성

`tests/Zlink.Framework.UnitTests/test_cpp_framework_scheduler_drain.cpp:168–203`의 `burst`는 **서로 다른 owner 네 개가 아니다.** Mailbox 하나, owner `"node"` 하나, drain source/queue entry 하나에 record 1,024개를 넣고 `try_submit_drain(source)`를 한 번 호출한다. Worker는 4개이고 첫 네 handler가 모두 진입해야 release한다. 후속 publication은 그 네 handler 진입 뒤인 `:204–208`에서만 일어난다.

Entry당 수요 1이면 최초 publication은 worker 하나만 예약한다. 그 worker가 첫 handler의 `released.wait()`에 들어간 뒤 나머지 세 worker를 예약할 publication이 없다. `:203`의 기존 5초 await가 충족되지 않고 예외 정리 중 executor drain도 대기한다. 8초 watchdog 시점의 native stack에 `burst` handler의 future wait와 main의 `burst [clone .cold] → offload_executor_t::~offload_executor_t → drain_until`이 확인된다. SIGTERM으로 종료했으며 exit는 `-15`다. Assertion을 바꾸거나 timeout을 늘리지 않았다.

원자료:

- [focused-results.json](focused-results.json): 첫 실행 8.375초, timeout/SIGTERM. 남은 49회는 실행하지 않았다.
- [scheduler-timeout-stacks.txt](scheduler-timeout-stacks.txt), [scheduler-timeout-threads.json](scheduler-timeout-threads.json).
- [test_cpp_framework_scheduler_drain-1.log](test_cpp_framework_scheduler_drain-1.log): 출력 없음. 최종 catch에 도달하기 전에 destructor에서 대기했다.
- [run-focused.py](run-focused.py): 기존 시험 binary 실행과 SIGTERM watchdog만 담당한다. 시험 source 변경이나 별도 통과 조건은 없다.

`independent(:279–312)`도 owner 하나/entry 하나에 A/B record 두 개를 넣은 단일 publication이다. A의 shared_future wait 중 B가 다른 worker에서 실행되어야 한다. 이 구간은 첫 `burst` 실패 때문에 **도달하지 않았으며 실행 실패로 집계하지 않는다.** 두 구성 모두 running handler가 있는 동일 owner entry에서 추가 record를 독립 worker가 실행할 수 있어야 한다.

## 시험과 build

| 검증 | 결과 | 원자료 |
|---|---|---|
| 수정 전 두 binary와 negative-control 등록 전체 | 9/9 통과, 5.36초 | [tests-before.log](tests-before.log) |
| 수정 후 Release configure | 통과 | [configure-tests.log](configure-tests.log) |
| 수정 후 두 target build | 4/4 build step 통과 | [build-tests.log](build-tests.log) |
| 수정 후 scheduler_drain 50회 | 1회째 진행 실패로 중단 | [focused-results.json](focused-results.json) |
| 수정 후 mailbox_owner_dispatch·negative controls 50회 | 중단 조건으로 미실행 | — |
| 수정 후 전체 framework unit 1회 | 중단 조건으로 미실행 | — |

수정 전 9개 등록은 scheduler 본체, scheduler negative `items/wakes/stream-wakes/terminal/spot-gate/actor-gate`, mailbox_owner_dispatch 본체와 negative `extra-notify`다. 수정 전 negative controls의 통과는 각 mutant의 exit 1을 확인한 것이며 timeout을 통과로 처리하지 않는다.

Configure는 psched5의 `configure-tests.sh` 사본을 그대로 사용했다. Build는 `/root/jobs/fx/cx/cpp-build-slot.sh cmake --build .psched5-tests --parallel 3 --target test_cpp_framework_scheduler_drain test_cpp_framework_mailbox_owner_dispatch`로 실행했다. Release `-O3 -DNDEBUG`, test probes ON이며 `-g`는 추가하지 않았다. Build 크기는 약 326 MiB로 25 GB 미만이다. Cache·등록·binary hash/mtime은 `test-CMakeCache.txt`, `test-registrations.cmake`, `test-binaries.sha256`, `test-binaries-stat.txt`에 보존했다.

## Base / narrowed merge 조건

새 성능 수치는 없다. 아래 base 범위는 **이전 psmeas의 역사 자료**이며 이번 작업에서 다시 측정한 값이 아니다. Narrowed 미측정을 통과로 판정하지 않는다.

| 조건 | 이전 base 3회 범위 / 중앙값 | 이번 narrowed | 판정 |
|---|---:|---|---|
| Target I/O sendto/msg ≤ base | 0.800–0.820 / 0.811 | 미측정 | 판정 불가 |
| Spot p99 µs가 base 범위 이내 | 560.561–680.402 / 603.360 | 미측정 | 판정 불가 |
| Send 1 KiB msg/s가 base 범위 이내 또는 개선 | 179,445.8–187,554.3 / 183,257.7 | 미측정 | 판정 불가 |
| Request 1 KiB msg/s가 base 범위 이내 또는 개선 | 53,746.6–54,145.1 / 53,768.4 | 미측정 | 판정 불가 |
| App-worker µs/msg ≤ base | 22.077–22.542 / 22.221 | 미측정 | 판정 불가 |
| Target voluntary wakes/msg ≤ base | 1.346–1.389 / 1.385 | 미측정 | 판정 불가 |

역사 자료 경로: `/root/jobs/fx/cx/v21/s033/psmeas/measure/{1,2,3}/` 아래 send/request는 `{send-saturation,request-backpressure}/base/run1/zlink-framework-cpp-<pattern>-1024/results.json`, Spot은 `pm/base/run1/zlink-framework-cpp-send-saturation-1024/capture/pm-latencies.json`, 나머지는 `22k/base-zlink-framework-cpp/runner/run1/zlink-framework-cpp-request-backpressure-1024/capture/{A-perfstat.csv,A-target-before.tsv,A-target-after.tsv,windows.json}`다.

Spot stage timing도 narrowed에서는 미측정이다. 이전 psched5 pm-queue의 p50/p99는 HTTP entry → gate submit **1,192/5,489 µs**, submit → gate run **33/2,710 µs**, handler → resume **134/3,503 µs**다. 원자료는 `/root/jobs/fx/cx/v21/s033/psched5/diagnosis/pm-queue/run1/zlink-framework-cpp-send-saturation-1024/capture/request-stages.json`이다. Pre-gate 대기가 이동했는지는 판정할 수 없다.

## 소유·대조·정리

- 소유 계층: Framework application scheduler, 단일 `reserve_workers_locked`에서 worker 예약을 결정한다.
- Spec 조항: S02 `02-handler-turn-and-execution-gate.ko.md` §2의 Node/Channel record별 독립 실행, §9의 worker별 연속 drain, §10의 공유 실행 자원. 수정 후보는 §2의 기존 진행 시험을 충족하지 못했다.
- 교차언어 대조: psched5의 결정 소유 표를 재사용하고 해당 경로를 확인했다. .NET `Runtime/Execution/ZLinkProcessExecutionPool.cs:307,438`, Java/Kotlin 공용 `execution/ZLinkWorkerPool.java:59,80`, Node `runtime/backend/mesh-dispatch-pump.ts:146`이 각 실행 자원을 소유한다. 동일 OS worker wake 알고리즘을 강제하는 public 계약은 없다. 다른 언어의 구현·계약은 변경하지 않았다.
- 변경 분류: **B(기존 과다 예약 결함) 대상의 후보**이나 correctness 회귀로 완료·머지 승인 불가다. Block 감지나 새 phase를 추가하는 대안은 사용자 결정에 따라 제외했다.
- Runtime 점검: 진행 회귀 1건 확인, 해결 0건, 별도 이슈로 넘긴 항목 0건. Scheduler의 pending 호출 삭제 뒤 남은 virtual pending 선언/구현도 있으나 실패 시 중단 지시에 따라 추가 리팩토링하지 않았다.
- Raw와 이전 diagnosis를 보존했다. 시험 build의 `Testing/`도 이 JOB의 `test-build-Testing/`에 보존한 뒤 재사용한 `.psched5-tests` build dir를 제거했다. 새 bench build는 만들지 않았다. 제공된 base build는 유지했다. Branch와 worktree는 유지했다.
