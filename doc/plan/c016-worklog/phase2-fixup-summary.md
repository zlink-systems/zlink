# Hot-path phase 2 기능 회귀 fixup 요약

## 결론

- 회귀 1인 ROUTER directed blocking multipart `FINAL` 재시도는 Core에서 근본 수정했다. 결정적으로 실패하던 `test_reconnect_options`는 focused 1회와 `repeat-until-fail:5`, 전체 CTest에서 모두 통과했다.
- 회귀 2의 정지 위치와 원인은 확정했지만 Core 결함이 아니다. C++ 계약 테스트가 receiver를 시작하기 전에 기본 HWM의 3.27배를 보낼 수 있고, phase 2의 정상적인 send hot-path 단축으로 timing 기반 `EINVAL` 비율이 낮아지자 실제 HWM에 도달했다. 지정된 binding source 수정 금지 때문에 `bindings/cpp` gate는 14/15이며 이 한 건을 BLOCKER로 남긴다.
- 기존 phase 2 tree와 staging 경계를 보존했다. 백업 `origin/wip/hotpath-phase2-snapshot-20260903` 대비 repository source 변경은 `core/src/runtime/sockets/common/socket_send_pending_submit.cpp` 한 파일, 17줄 추가뿐이다. `restore/reset/checkout/commit/push`는 수행하지 않았다.

## 회귀 1 — ROUTER blocking directed multipart FINAL retry

### 계약과 재현

- 테스트는 `core/tests/integration/test_reconnect_options.cpp:247-305`에서 `MORE("one")`를 staging하고, `FINAL("two")` 직전에 1회 `ENOTCONN`을 주입한 뒤 동일 RID의 완전한 두-part record 성공을 요구한다. 실패 지점은 `:292-295`의 expected `ZLINK_SUBMIT_OK(0)`, actual `ZLINK_SUBMIT_BACKPRESSURED(1)`이었다.
- `core/doc/spec/core/socket/README.ko.md:913-956`은 `MORE`를 `FINAL` 성공까지 staging하고, blocking `NONE FINAL`이 `SNDTIMEO` 안에서 동일 logical target의 reconnect와 admission을 기다리도록 정한다.
- `core/doc/spec/core/socket/07-router.ko.md:169-173,432-435`는 모든 part의 동일 target과 admission 전 동일 logical RID whole-record retry를 요구한다.

### 원인과 실증

1. 주입한 `ENOTCONN`은 `core/src/runtime/sockets/common/socket_base_msg.cpp:493-500`에서 `xsend_routed()` 호출 전에 소비된다. 따라서 pipe에 part가 하나도 공개되지 않는다.
2. `core/src/runtime/sockets/common/socket_send_complete.cpp:487-504`의 pristine multipart attempt copy는 그대로 남아 frame zero부터 재시도할 수 있다.
3. `core/src/runtime/sockets/common/socket_send_pending_submit.cpp:562-624`는 첫 실패 뒤 caller RID를 string-backed retry target으로 materialize하고 logical waiter를 등록한 다음 lifecycle sync를 놓는다.
4. 그러나 route는 이미 writable이고 failpoint가 route/HWM 상태나 submit-progress epoch를 바꾸지 않는다. 기존 코드는 곧바로 `wait_submit_progress()`(`core/src/runtime/sockets/common/socket_base_lifecycle.cpp:422-445`)에 들어가 새 edge가 없는 채 200 ms 전체 budget을 소진했다. GDB에서도 첫 `wait_submit_progress(epoch=7, timeout=200)` 이후 두 번째 `xsend_routed()`가 없음을 확인했다.
5. 비교 기준 `78a71828b07a`의 10 ms polling slice는 우연히 outer loop로 돌아가 두 번째 시도를 만들었다. 새 event-driven wait 자체는 올바르지만, 이미 writable인 raw target을 persistent target으로 전환한 직후 한 번의 revalidation이 빠져 있었다.

### 수정

- `core/src/runtime/sockets/common/socket_send_pending_submit.cpp:570-600`
  - 해당 iteration에서 transient raw target materialization에 성공했고 첫 오류가 `ENOTCONN` 또는 `EHOSTUNREACH`일 때만 `retry_committed_raw_target`을 세운다.
- 같은 파일 `:629-653`
  - terminal/non-retryable 검사를 모두 유지한 뒤 deadline이 남아 있으면 동일 logical target을 즉시 정확히 한 번 재검증한다.
  - `transient_raw_target`은 이미 false이므로 두 번째 실패에는 flag가 다시 서지 않는다. 따라서 polling/spin이 생기지 않고 기존 event-driven wait로 간다.
  - HWM `EAGAIN`은 이 branch에 들어오지 않아 실제 receiver credit/progress를 기다리는 기존 backpressure 계약을 유지한다.

이 변경은 실패 후 fallback에만 boolean과 분기를 추가한다. 정상 successful admission hot path에는 allocation, identity string, socket-table lock, polling/sleep 또는 보조 작업을 추가하지 않아 hot-path spec §3/§4 형태를 유지한다.

## 회귀 2 — C++ `test_cpp_contract_socket` timeout

### 정지 위치

- 직접 실행은 출력 없이 timeout됐고, GDB에서 `bindings/cpp/tests/contract/test_cpp_contract_socket.cpp:705-792`의 `test_concurrent_pair_multipart_exposes_core_rejection_and_returns_lvalues`로 확정했다.
- main thread는 sender join(`:769-770`)에 있었고, 8개 sender 전부 `wait_submit_progress -> send_completion_submit_blocking -> pair_t::xsend`에 있었다.
- `pair_t::xsend`는 `core/src/runtime/sockets/pair/pair.cpp:75-106`에서 `pipe_message_admission_hwm_full`을 `EAGAIN`으로 반환하고 있었다. 이는 lost wake나 lifecycle deadlock이 아니라 반복되는 유효한 blocking HWM wait다.

### HWM/용량 실증

- 포화 시 live PAIR pipe: HWM `1,048,576`, `_bytes_written=1,048,407`, peer bytes read `0`, accepted `4,913` records. 이후 accepted는 증가하지 않고 `SNDTIMEO=1,000 ms` 만료가 누적됐다.
- 테스트는 8 sender × 2,000 records × 3 frames(`test_cpp_contract_socket.cpp:722-746`)를 먼저 보낸다. 전체 charge는 `3,429,360` bytes로 balanced auto-HWM 1 MiB의 3.27배다.
- Core의 charge는 `core/src/runtime/core/pipe.cpp:3553-3562`처럼 `payload + sizeof(msg_t)`이고, `:3620-3641`에서 in-flight bytes와 HWM을 비교한다. 이 값과 포화 지점은 정확히 일치한다.
- receiver drain은 모든 sender join 뒤(`test_cpp_contract_socket.cpp:779-791`)에만 시작하므로 포화 중에는 credit을 만들 주체가 없다.
- debugger에서 해당 live pipe HWM만 `1,048,576 -> 8,388,608`로 바꾸자 동일 실행 파일의 8 sender와 후속 테스트가 모두 정상 종료했다. 저장소 파일은 바꾸지 않았다.

### pristine 비교와 phase 2 경계

- `git archive 0237a513bf941fd8c906d70f3fc589bf149d61ff`로 `/tmp/zlink-baseline-59m1QZ`에 별도 pristine main을 만들고 Core/C++ target을 빌드했다. 같은 실행 파일은 0.17초 PASS했고 임시-copy 계측은 accepted `2,896`, rejected `13,104`, unexpected `0`이었다.
- phase 2 A의 공통 send 변경, 특히 `core/src/api/socket/socket_message_send_api.cpp:533-564`의 PAIR scoped immediate complete-record path는 정상 record의 lifecycle 재진입을 제거한다. 그 결과 유효 sequence admission이 빨라져 테스트가 우연히 기대하던 contention `EINVAL` 비율이 줄고, accepted가 4,913까지 올라 실제 HWM을 채웠다.
- 공개 계약은 여러 thread의 동시 send를 허용한다(`core/doc/spec/core/socket/README.ko.md:44-55`). 어떤 thread가 몇 건을 먼저 성공시켜야 하는지 또는 일정 비율을 `EINVAL`로 만들라는 fairness/rejection 계약은 없다.
- 계약은 HWM 도달 뒤 receiver credit까지 대기하고(`README.ko.md:418-452,1229-1236`), blocking `FINAL`이 `SNDTIMEO`에 만료하면 `BACKPRESSURED/EAGAIN`을 반환하도록 명시한다(`:937-941`). 테스트는 `test_cpp_contract_socket.cpp:751-777`에서 이 정상 결과를 unexpected로 센다.
- 유사한 Core concurrency test는 이 문제를 피하려고 64 MiB 수동 HWM을 설정하고 receiver를 동시에 실행한다(`core/tests/integration/test_helper_interleave.cpp:959-1008`).

### 왜 Core를 수정하지 않았는가

Core만 바꿔 이 테스트를 통과시키는 선택지는 다음 셋뿐이며 모두 계약 또는 요청 위반이다.

1. hot path를 의도적으로 늦춰 timing 기반 `EINVAL` 비율을 높인다 — 성능 목표와 hot-path §3/§4 위반이다.
2. PAIR balanced auto-HWM/default HWM을 이 테스트 전송량보다 높인다 — 공개 memory/backpressure 정책 변경이다.
3. HWM `EAGAIN`을 `EINVAL`로 분류하거나 receiver 없이 내부 추가 buffer로 이동한다 — 공개 result와 byte-HWM 계약 위반이다.

따라서 올바른 최소 수정은 binding test에서 connection 전에 SNDHWM/RCVHWM을 `3,429,360`보다 크게 설정하거나 receiver를 sender와 동시에 drain하는 것이다. 두 방법 모두 `bindings/cpp/**` source 수정이 필요해 이번 범위에서는 적용하지 않았다.

## 변경 파일

- Repository source: `core/src/runtime/sockets/common/socket_send_pending_submit.cpp` (+17, snapshot 대비)
- 작업 기록: `/home/hep7/project/zlink-work/c016/phase2-fixup-progress.md`
- 이 요약: `/home/hep7/project/zlink-work/c016/phase2-fixup-summary.md`

`doc/**`, `core/doc/**`, `framework/**`, binding source/header mirror, `scripts/local-package/**`, `hotpath_reference.json`은 수정하지 않았다. 기존 51-file phase 2 tree와 staged/unstaged 경계를 변경하지 않았고 fixup은 기존 staged 파일에 unstaged delta로만 남아 있다.

## Gate 결과

모든 build/test/benchmark 실행 전에 `ulimit -v 16777216`을 적용했다.

| Gate | 결과 |
|---|---|
| `cmake --build core/build -j4` | PASS, 100% build 완료 |
| focused `test_reconnect_options` | PASS 1/1; 내부 9/9 |
| `--repeat until-fail:5` | PASS 5/5 |
| focused helper ownership/interleave/completion | PASS 3/3 |
| `ctest --test-dir core/build -j2` | PASS 134/134, 0 failed |
| `test_single_lane_*` repeat 1 | PASS 29/29 |
| `test_single_lane_*` repeat 2 | PASS 29/29 |
| raw header mirror `cmp` | PASS 12/12 |
| `git diff --check` + `git diff --cached --check` | PASS |
| `bash bindings/cpp/tests/run_tests.sh` | **BLOCKED 14/15**; `test_cpp_contract_socket`만 120.07 s timeout, 나머지 14 PASS |
| `ZLINK_CORE_SOURCE=local bash bindings/python/tests/run_tests.sh` | PASS: 144 tests + 4 subtests, samples 7/7 |

### C perf 1-run sanity

- 실제 runtime: `/home/hep7/project/zlink/core/build/lib/libzlink.so.0.15.1`, local dirty Core임을 runner가 확인했다.
- 결과 artifact: `/home/hep7/project/zlink/bindings/c/perf/results/single/report/perf_c_single_linux_20260903_130043.txt`, expected/actual result lines 20/20, status complete.

| Pattern/transport/1024 B | fixup | phase2 pre-fix 10:00 | ratio |
|---|---:|---:|---:|
| DEALER_ROUTER_REQREP tcp | 606.75 Kops/s | 570.48 Kops/s | 1.0636 |
| DEALER_ROUTER_REQREP inproc | 578.42 Kops/s | 634.70 Kops/s | 0.9113 |
| ROUTER_ROUTER tcp | 901.69 Kmsg/s | 974.58 Kmsg/s | 0.9252 |
| ROUTER_ROUTER inproc | 977.02 Kmsg/s | 1,015.80 Kmsg/s | 0.9618 |

4/4 cell이 이전 phase 2 1-run의 0.91–1.06배 범위여서 급격한 수준 이탈은 없었다. 요청대로 이는 sanity 관찰일 뿐 성능 합격 판정으로 사용하지 않는다.

## BLOCKERS

1. **C++ gate 15/15 미달** — `bindings/cpp/tests/contract/test_cpp_contract_socket.cpp:705-792`가 receiver credit 없이 최대 3,429,360 bytes를 1 MiB HWM에 넣고, 계약상 정상인 `EAGAIN`을 unexpected로 취급한다. live HWM만 8 MiB로 올리면 동일 binary가 정상 종료한다. binding test의 HWM 설정 또는 concurrent drain 수정이 필요하지만 binding source 수정 금지 범위다.
2. 그 외 blocker는 없다.
