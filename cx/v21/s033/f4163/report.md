**#4163 결과 — queue depth는 확인, latency 수정안은 미채택**

#4135가 이 workload의 수락 용량을 늘리거나 reader wake를 늦췄다는 근거는 확인하지 못했다. 깊어진 queue는 확인했다. 로컬 커밋 `d657f51371523f875d99b6198744d5135f028432`의 admission 통합안은 계약 테스트를 통과했지만 latency를 개선하지 못했다. **#4135가 high mode 비율을 바꾼 세부 원인과 유효한 수정은 미확정이다.**

**경로와 결정**

- 기본 runner는 routing-id 인자와 `[1024B MORE, 0B FINAL]`을 제출한다. `bindings/c/perf/common/perf_zlink_part_helpers.hpp:19,39,79`, `single/src/perf_router_router.cpp:448` 기준이다. routing-id는 대상 선택에 사용되며 이 record의 payload frame으로 pipe에 기록되지 않는다. 첫 body는 `core/src/runtime/sockets/common/socket_send_complete.cpp:361,398` → `router/router_send_path.cpp:493`, FINAL은 같은 파일 `:186`을 지난다.
- #4135는 complete scope를 `pipe_write_complete_record`로 전달한다(`common/socket_runtime.hpp:843`). `0005a4d6c0`의 `core/src/runtime/core/pipe_write.cpp:1233`은 일반 byte admission, `:1244`는 empty-queue 예외를 따로 판정한다. 새 예외는 complete record의 MORE에도 적용되지만, 이번 record의 전체 charge 1,152B는 관측 HWM 4,096,000B 및 1,048,576B보다 작다. 따라서 **이번 메시지에 새로 허용되는 oversize record는 없다.** 일반 용량식은 `core/src/runtime/core/pipe.hpp:1112,1145`가 소유한다.
- 대기 장소는 sender의 `_out_pipe` = receiver의 `_in_pipe`인 inproc physical FIFO다. FINAL만 record를 공개하고 flush한다(`pipe_write.cpp:62,1300`; FINAL flush 구현은 후보 기준 `:1426`). receiver가 `fq`/ROUTER receive 경로로 FINAL까지 dequeue해야 `_published_msgs_read`와 byte credit이 증가한다(`pipe_receive.cpp:724,742,756`). 그 전에 수천 records가 기다리는 것이 high latency다. sender/receiver loop는 `bindings/c/perf/single/common/perf_single_one_way.hpp:282,326`이다.
- FINAL flush 조건, queue 용량식, waiter/fence/recheck 및 reader의 `activate_write` 조건은 유지된다(`pipe_write.cpp:146`; `pipe_receive.cpp:767,807`). 같은 socket의 complete send scope가 writer를 직렬화하며, 첫 MORE에서 고정한 empty 자격과 현재 in-flight=0 검사를 함께 적용한다. 두 concurrent records가 empty 예외를 동시에 쓰는 정책을 추가하지 않았다.

**확인 계측**

전용 Release/LTO 빌드 M=`9dc2246dd0`, C=`0005a4d6c0`에서 각각 5회 측정했다. thread-local 임시 counter로 1,024 commits마다 `msgs_written − peer.published_msgs_read`를 표본 추출했다. 표는 대표 실행의 마지막 누적 dump이며 N=8,388,608 commits다. 최신 commit은 아직 flush 전일 수 있어 depth에는 최대 1건의 오차가 있다.

| 실행 | sampled 평균 records | 최대 records | HWM 거부 | reader activation | mean ms |
|---|---:|---:|---:|---:|---:|
| M-1 | 89.90 | 3,547 | 2 | 872 | 0.049444 |
| C-2 | 2,090.45 | 3,555 | 95 | 1 | 1.103031 |

5회 sampled depth 중앙값은 M 89.90 / C 1,610.06 records였다. 양쪽 최대는 3,554 / 3,555로 `floor(4096000/1152)=3555`와 일치한다. cached credit과 실제 dequeue의 평균 차이는 모든 실행에서 0.11 record 미만이다. flush는 양쪽 모두 record당 1회다. C-2의 depth/rate는 약 1.15ms로 측정 mean 1.10ms와 대응한다. **깊은 queue는 입증했지만, capacity 확대·credit 지연·wake 누락은 입증하지 못했다.** 처리 속도 균형이나 코드 배치가 mode를 바꾼다는 설명은 아직 가설이다. 추가 CPU profile도 특정 결함을 분리하지 못했다.

Raw: [전체 counter 표](probe-summary.csv), [C-2](runs/probe-C-2.log), [M-1](runs/probe-M-1.log), [계측 patch C](probe-C.diff)·[M](probe-M.diff), [CPU profiles](profiles/). counter는 최종 코드에서 제거했다.

**구현한 수정안과 검증**

`core/src/runtime/core/pipe_write.cpp:1233`에서 ordinary/empty 예외를 한 capacity 호출로 합쳤다(9 insertions / 17 deletions). 첫 frame의 empty 자격, MAXMSGSIZE 선검사, rollback과 waiter 등록 후 credit 재확인은 유지했다. 대안인 HWM 축소나 추가 pacing은 기존 정책을 바꾸므로 구현하지 않았다.

- 판정 호출 위치 **3→2**: 일반/예외 진입부 **2→1**, 경쟁 방지 재확인 **1→1**. 새 flag·counter·queue·lock은 0개다. 중복 판정 1건을 통합했고 범위 밖 변경은 없다.
- 소유 계층: Core pipe byte admission.
- Spec: `core/doc/spec/core/systems/06-auto-hwm.ko.md:453,470,476` — incremental MORE 제한, complete record 한 건의 empty 예외, MAXMSGSIZE 유지.
- 교차언어: Framework 변경 없음. 모든 binding이 사용하는 공통 Core 경로만 변경했다.
- 분류: **B 후보(의미 보존 중복 판정 제거)**. latency 원인 수정으로는 미채택이다.
- `test_router_complete_record_hwm` **9/9**, `unittest_router_pipe_contract` **29/29**, `test_phase3_request_reply_contract` **20/20** 통과. 작은 HWM의 정상 수락과 두 번째 record·MAXMSGSIZE 거부, concurrent sender 사례를 포함한다. [CTest 결과](logs/tests.log), [사례별 raw](tests-raw/Temporary/LastTest.log).

**Runs-10 비교**

C→F→C→F, 각 stage `--runs 5`, `256,1024`, inproc, CPU 8–15, `flock -o measure.lock` 아래 실행했다. B/M은 제공된 기존 10회다. mean/p95는 실행별 값의 중앙값이며 단위는 ms다.

| 크기 | 버전 | mean 중앙값 (범위) | p95 중앙값 | throughput Mmsg/s |
|---|---|---:|---:|---:|
| 256 | B 1.23 기존 | 1.747 (0.004–3.139) | 4.861 | — |
| 256 | M 기존 | 1.797 (0.079–3.256) | 4.790 | — |
| 256 | C 이번 | 3.332 (1.047–3.527) | 5.750 | 1.778 |
| 256 | F 수정안 | 3.233 (1.260–3.359) | 5.698 | 1.804 |
| 1024 | B 1.23 기존 | 0.290 (0.014–1.116) | 1.546 | — |
| 1024 | M 기존 | 0.088 (0.030–0.988) | 0.644 | — |
| 1024 | C 이번 | 0.141 (0.072–1.128) | 0.979 | 1.813 |
| 1024 | F 수정안 | 1.049 (0.097–1.121) | 1.878 | 1.813 |

1024B mean ≥0.9ms는 C **4/10**, F **7/10**이었다. 이전 C 중앙값 1.091ms에 비해 이번 control 자체의 mode 비율도 달랐다. **수정안의 latency 개선은 검증 실패다.** 기존 B 최대 1.116ms를 F는 2/10회 초과했다. 실패를 제외하거나 통과한 재실행으로 대체하지 않았다.

Raw: [실행별 40개 결과](runs-10.csv), [C r1](runs/C-r1-single-ROUTER_ROUTER-inproc.log)·[r2](runs/C-r2-single-ROUTER_ROUTER-inproc.log), [F r1](runs/F-r1-single-ROUTER_ROUTER-inproc.log)·[r2](runs/F-r2-single-ROUTER_ROUTER-inproc.log), [runner 저장 결과](runs/saved-results/). 기존 B/M 근거는 `/root/jobs/fx/cx/v21/s033/c4163/report.md`다.

남은 일은 mode 비율을 바꾼 CPU 실행 경로를 인과적으로 분리하는 것이다. 이 증거로 HWM 정책이나 wake 규칙을 바꾸는 수정은 제안하지 않는다. branch `fix/4163-router-inproc-queue-depth`와 `$JOB/src`를 보존했다. push·PR·GitHub·spec 변경은 없다. build 디렉터리와 보조 worktree는 정리했으며 raw와 전용 prefix는 보존했다.

자원 검사 30회 모두 통과: 최소 MemAvailable 19.94GiB, 기록된 최대 JOB 1.26GiB / 25GiB. 빌드는 지정 wrapper의 Release, `-O3 -DNDEBUG`, `-g` 없이 실행했다. [빌드 recipe](build.sh), [수정 빌드](rebuild-fix.sh), [테스트 recipe](test.sh), [측정 recipe](measure.sh), [runtime provenance](logs/fix-manifest.json).
