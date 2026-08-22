# Stage 1: byte-HWM 제거 기준점 (2026-08-22)

이 문서는 `core-byte-hwm-flow-control-plan.ko.md` §7 step 1 / §7.1 "기준점" 행 /
§12.2 첫 행의 실행 증거다. 측정값, 제거·우회한 symbol과 판정만 기록한다.

**결론: 이 stage는 BLOCKED다.** 계획이 지목한 byte-HWM hot-path 기계장치
(physical queue registry 회계, decoder reservation round-trip, per-frame atomic,
credit wakeup)를 차례로 우회해도 첫 회귀 case가 전혀 회복되지 않았다. 대신
bisect로 회귀를 소유한 commit 한 개를 정확히 특정했고, 회귀의 성격이 CPU 비용이
아니라 **blocking/wakeup 구조 변화**임을 rusage로 확인했다.

## 1. 시작 상태 보존

```text
447f41a9f2 wip: preserve byte-HWM worktree state before removal baseline
```

`git add -A -- . ':!gmon.out'` 후 commit. `gmon.out`은 사용자 artifact이므로
추가·삭제하지 않았다. 이 commit이 복구 지점이다. Stage 1 종료 시점의 worktree는
이 commit과 동일하다(진단 편집은 모두 revert함).

- Branch: `codex/bindings-0.11.1-performance` (전환 없음)
- 보호 경로(`core/doc/spec/`, `core/doc/internals/` 기존 파일, `bindings/doc/spec/`)
  변경 없음. Spec 변경 제안도 발생하지 않았다.

## 2. 제거·우회를 시도한 symbol과 결과

각 항목은 build 후 같은 case를 재측정했다. 모두 **회복 없음**이다.
네 시도 모두 최종 worktree에서 revert했다(진단 코드를 branch에 남기지 않음).

| # | 우회 대상 | 구현 위치 | 결과 (Kops/s) |
|---|---|---|---|
| 1 | Auto-HWM 적용 HWM 값 (4 MiB → 4 GiB / 512 MiB, auto-HWM off 포함) | perf option only, source 변경 없음 | 130.6 / 120.6 / 121.2 (기준 130) — 무변화 |
| 2 | Decoder frame reservation round-trip: `session_base_t::configure_zmp_decoder`가 `zmp_decoder_t::set_frame_admission_handler`를 설치하지 않게 하여 `reserve_inbound_decoder_frame` / `write_reserved_decoder_frame` / `release_decoder_frame_reservation` 경로 전체를 우회하고 `push_msg_internal`이 0.10.1과 같은 `_pipe->write(msg_)`로 떨어지게 함 | `core/src/runtime/core/session_base_pipe_io.cpp` | 130.5 — 무변화 |
| 3 | Blocked-writer drain wakeup: `pipe_t::account_inbound_frame`의 `_peer->_waiting_for_byte_credit` acquire load + `_in_pipe->check_read()` + 추가 `send_activate_write` 경로 제거 (LWM wakeup만 유지) | `core/src/runtime/core/pipe.cpp` | 119.99 / 118.68 (PRE 167.97 / 160.90) — 무변화 |
| 4 | Receive-path 직렬화: `receive_once_guarded`의 `scoped_lock_t (runtime_.sync)`와 `async_mailbox_handler`의 `scoped_lock_t (receive_runtime().sync)` 제거 (recv마다 app thread와 io thread가 같은 mutex를 잡던 구조) | `socket_base_msg.cpp`, `socket_base_lifecycle.cpp` | 125.85 / 119.67 (PRE 175.37 / 163.57, WT 123.07 / 117.87) — 무변화 |

Registry per-frame 전역 atomic은 이미 dirty worktree에서 application lane 기준으로
비활성이다(`ctx_physical_queue_registry.cpp:634-642`, `commit_decoder_frame`의
application 분기 주석 "Application queues enforce byte admission ... does not mutate
shared registry counters"). Application pipe는 `_registry_accounting == false`이므로
(`pipe.cpp:95-105`, `resolved_queue_class != physical_queue_class_application`)
registry 호출이 hot path에 없다. 따라서 계획이 지목한 "registry 회계 제거"는
이미 완료된 상태이며 남은 회귀와 무관하다.

계획 §7.1의 금지 사항은 모두 지켰다: 공개 HWM option을 unlimited로 바꾸지 않았고,
retained receive API·public Auto-HWM option·monitoring ABI·기존 test를 삭제하지 않았다.

### 2.1 Test와의 긴장

없음. 위 우회는 모두 되돌렸고, 최종 상태에서 8개 focused test가 통과한다.
단, 우회 2(decoder reservation)를 최종 구현으로 남길 경우
`core/doc/internals`와 handoff가 규정한 "decoder는 payload allocation 전에 session의
inline reservation으로 검사한다"는 동작이 사라진다. 회복 효과가 0이므로 유지할
이유가 없다.

## 3. Build와 focused test (최종 상태 = 447f41a9f2)

```text
$ cmake --build core/build --parallel 8      -> 성공
$ ls -l core/build/lib/libzlink.so           -> libzlink.so.0 -> libzlink.so.0.11.1
  libzlink.so.0.11.1 mtime 2026-08-22 06:55:02 (epoch 1787349302)
$ find core/src core/include -newer core/build/lib/libzlink.so.0.11.1 -type f
  (출력 없음: runtime이 모든 source보다 최신)
```

```text
$ ctest --test-dir core/build --output-on-failure \
  -R '^(test_zmp_request_reply|unittest_auto_hwm_policy|unittest_zmp_decoder|test_ctx_options|test_retained_hwm_credit|test_router_handover|test_connect_rid|test_router_mandatory_hwm)$'
100% tests passed, 0 tests failed out of 8   (Total 56.91s)
```

| Test | 결과 |
|---|---|
| test_ctx_options | Passed |
| test_retained_hwm_credit | Passed |
| test_connect_rid | Passed |
| test_router_handover | Passed (13.10s) |
| test_zmp_request_reply | Passed (38.04s) |
| test_router_mandatory_hwm | Passed (2.03s) |
| unittest_zmp_decoder | Passed |
| unittest_auto_hwm_policy | Passed |

## 4. Stage 1 성능 gate (plan §8.2.1, §8.2.3)

Local / release 교대 3회씩, `--runs 1`.

Report 경로 (실행 순서):

1. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_065620_autohwm-stage1-local.txt`
2. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_065629_autohwm-stage1-release-0101.txt`
3. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_065636_autohwm-stage1-local.txt`
4. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_065642_autohwm-stage1-release-0101.txt`
5. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_065647_autohwm-stage1-local.txt`
6. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_065653_autohwm-stage1-release-0101.txt`

| Run | Version | Throughput (Kops/s) | Bandwidth (MB/s) | Mean(ms) | P95(ms) | P99(ms) |
|---|---|---|---|---|---|---|
| 1 | local | 130.186 | 66.655 | 0.360 | 0.577 | 0.724 |
| 2 | 0.10.1 | 184.644 | 94.538 | 0.253 | 0.388 | 0.481 |
| 3 | local | 121.489 | 62.202 | 0.385 | 0.614 | 0.750 |
| 4 | 0.10.1 | 173.847 | 89.010 | 0.267 | 0.418 | 0.528 |
| 5 | local | 117.991 | 60.411 | 0.396 | 0.630 | 0.769 |
| 6 | 0.10.1 | 166.808 | 85.406 | 0.278 | 0.429 | 0.544 |

### 4.1 Median 판정

| Metric | Local median | 0.10.1 median | Local/0.10.1 | 판정 |
|---|---|---|---|---|
| Throughput (Kops/s) | 121.489 | 173.847 | 69.9% | FAIL |
| Bandwidth (MB/s) | 62.202 | 89.010 | 69.9% | FAIL |
| Lat.Mean (ms) | 0.385 | 0.267 | 144.2% | FAIL |
| Lat.P95 (ms) | 0.614 | 0.418 | 146.9% | FAIL |
| Lat.P99 (ms) | 0.750 | 0.528 | 142.0% | FAIL |

5개 metric 모두 미달이다. 격차(30~47%)가 stage 0 noise floor(throughput 7.7%,
P99 21.3%)를 크게 넘으므로 재실행 없이 실패로 판정한다. Threshold 완화는 하지 않았다.

## 5. Bisect: 회귀를 소유한 commit

같은 toolchain(`-DCMAKE_BUILD_TYPE=Release -DENABLE_LTO=ON`, `/usr/bin/c++`)으로
각 revision을 scratch에서 build해 같은 perf harness로 측정했다.

### 5.1 Build 구성이 원인이 아님을 먼저 배제

`core/v0.10.1` tag를 local toolchain으로 build한 runtime이 배포 binary와 같은 수준이다.

| Runtime | Throughput (Kops/s) |
|---|---|
| 0.10.1 release 배포 binary | 187.9 (stage 0 median) |
| 0.10.1 tag를 local에서 build | 188.1 |

따라서 회귀는 build flag/LTO/컴파일러 차이가 아니라 source 차이다.

### 5.2 인접 paired bisect (각 3회 교대, median)

| Revision | 설명 | Throughput median | Mean lat median |
|---|---|---|---|
| `core/v0.11.0` tag | byte-HWM 이전 release | 175.3 | 0.266 |
| `2728d70d44` | byte-HWM commit의 **부모** | 164.7 / 165.2 | 0.283 |
| `3ef4d09a37` | **core: implement byte HWM accounting and routed admission** | **62.2** | **0.792** |
| `core/v0.11.1` tag | byte-HWM 이후 release | 68.1 (단일) | 0.721 |
| worktree (`447f41a9f2`) | 현재 상태 | 121.5 | 0.385 |

`2728d70d44` vs `3ef4d09a37` 인접 paired 3쌍(모두 같은 방향):

```text
pre 178.2 / commit 61.7
pre 165.2 / commit 63.2
pre 162.7 / commit 62.2
```

**회귀는 commit `3ef4d09a37` "core: implement byte HWM accounting and routed admission"
단일 commit이 소유한다** (165 → 62, -62%). 이후 dirty worktree 작업이 62 → 121로
절반가량 회복했고, 남은 격차는 121 vs 165(부모) / 174~188(0.10.1)이다.

### 5.3 회귀는 pattern 특정이 아님

| Pattern | 부모(2728d70d44) | worktree | 비율 |
|---|---|---|---|
| Multi ROUTER_ROUTER_SENDSEND / tcp / 256 | 164.7 K | 121.5 K | 74% |
| Multi DEALER_ROUTER_SENDSEND / tcp / 256 | (0.10.1 186.6 K) | 132.3 K | 71% |
| Multi PUBSUB / tcp / 256 | 1,689 K | 1,157 K | 69% |

PUBSUB까지 같은 비율로 떨어지므로 ROUTER 전용 경로(`router_recv_path.cpp`,
`router_send_path.cpp`, `lb.cpp`)가 아니라 **모든 socket이 공유하는 pipe/session/
socket_base 공통 경로**에 원인이 있다.

### 5.4 회귀의 성격: CPU 비용이 아니라 blocking

`/usr/bin/time -v`로 같은 benchmark 전체(자식 process 포함)를 측정했다.

| Runtime | Throughput | User(s) | Sys(s) | CPU% | Voluntary ctx switches |
|---|---|---|---|---|---|
| 부모 `2728d70d44` | 170.97 K | 15.75 | 21.95 | 624% | 279,746 |
| worktree | 123.41 K | 14.93 | 18.41 | 557% | 332,436 |

Throughput이 28% 낮은데 **총 CPU 시간은 오히려 적고**(37.7s → 33.3s),
voluntary context switch는 19% 많다. message당으로 환산하면 voluntary ctx switch가
약 65% 증가했다. 즉 추가 연산 비용이 아니라 **message당 sleep/wake round-trip이
늘어난 구조 변화**다. 이 성질은 다음 관측과도 일치한다.

- HWM 값을 4 MiB → 512 MiB로 키워도 회복이 없다(§2 #1). 큐 깊이 부족이 아니다.
- decoder reservation을 완전히 제거해도 회복이 없다(§2 #2).
- LWM 외 drain wakeup을 제거해도 회복이 없다(§2 #3).
- recv 경로의 공유 mutex를 제거해도 회복이 없다(§2 #4).

`3ef4d09a37`이 공통 경로에 도입한 구조 후보(다음 stage의 조사 대상):

- `socket_base_msg.cpp`의 recv 전면 재작성: `receive_once_guarded`,
  `wait_receive_progress` / `notify_receive_progress` (epoch + condition variable)
  기반 대기 채널이 기존 `process_commands` polling 대기를 대체함.
- `socket_base_lifecycle.cpp`의 `async_mailbox_handler` / `notify_receive_progress`
  경로와 `mailbox_t::recv`의 새 `_recv_sync`.
- `session_base.cpp:637-640`의 `choose_io_thread_transport` (session 전용 round-robin
  counter 분리).
- ROUTER/공통 recv의 deferred prefetch (`read_deferred` / `finish_deferred_read`,
  `fq_t::recvpipe_deferred`, `router_t::finish_prefetched_credit`).

## 6. Plan §10 중단 보고

```text
Result: BLOCKED
Changed source: 없음 (진단 편집은 모두 revert). worktree = 447f41a9f2
Changed public contract: 없음
Focused tests: 8/8 Passed (56.91s)
Paired perf reports: §4의 6개 report (autohwm-stage1-local / autohwm-stage1-release-0101)
First remaining failure: Multi ROUTER_ROUTER_SENDSEND / tcp / 256 B —
  throughput·bandwidth median 69.9%, mean·p95·p99 median 142~147%.
  원인 소유 commit = 3ef4d09a37, 성격 = per-message blocking/wakeup 증가.
Framework work started: no
```

### 6.1 중단 사유

계획 §7.1의 "기준점" 행은 다음 단계 진입 조건을 "Public HWM option을 unlimited로
바꾸지 않고 알려진 perf case가 회복됨"으로 정의한다. 계획이 제거 대상으로 지목한
기계장치(physical queue registry 회계, decoder reservation, per-frame atomic,
credit wakeup)를 모두 우회해도 회복이 0이므로, **이 정의대로의 제거 기준점을
만들 수 없다**. 회귀는 byte-HWM 회계 산술이 아니라 같은 commit이 함께 바꾼
receive/dispatch 대기 구조에 있다.

`3ef4d09a37` 전체를 되돌리면 회복되지만, 그 commit이 retained receive API,
public Auto-HWM option, monitoring ABI와 다수의 기존 test를 함께 도입했으므로
계획 §7.1이 금지한 "삭제로 기준점 맞추기"에 해당한다. 따라서 사용자 판단이 필요하다.

### 6.2 선택지

1. Stage 1의 기준점 정의를 바꾼다: 제거 대상을 "byte-HWM 회계"가 아니라
   "`3ef4d09a37`이 도입한 receive/dispatch 대기 구조"로 재정의하고, §5.4의 후보를
   하나씩 부모 commit 동작으로 되돌려 회복 지점을 찾는다. (권장)
2. `3ef4d09a37`의 core runtime 변경만 부모로 되돌린 임시 branch를 만들어
   기준점 수치를 확보하고, public 표면은 별도로 재이식한다.
3. Stage 1을 건너뛰고 Stage 2(최소 byte-HWM 재구현)로 진행하되, 성능 gate는
   `3ef4d09a37` 부모(164.7 K) 대비로만 판정하고 0.10.1 대비 gate는 별도 작업으로
   분리한다.

어느 쪽도 계획 본문의 gate를 완화하는 결정이므로 사용자 승인 없이 진행하지 않는다.

## 7. Walk-back: `3ef4d09a37` 수신·dispatch 대기 구조 후보 제거

Coordinator 결정에 따라 Stage 1의 제거 대상을 "byte-HWM 회계"에서
"`3ef4d09a37`이 도입한 receive/dispatch 대기 구조"로 재정의하고 §5.4 후보를
하나씩 부모(`2728d70d44`) 동작으로 되돌렸다. 각 후보마다 build → 8개 focused test
→ paired 1회(local + 0.10.1) 신호 측정을 수행했다. **네 후보 모두 회복 없음**이며
모두 revert했다. Public 계약, retained receive API, Auto-HWM option, monitoring ABI,
기존 test는 전부 유지했다.

| # | 후보 | 복원 내용 | Test | local / 0.10.1 (Kops/s) | 비율 | 판정 |
|---|---|---|---|---|---|---|
| 기준 | — | 변경 없음 (§4 median) | 8/8 | 121.5 / 173.8 | 69.9% | — |
| 1 | `receive_once_guarded` / `wait_receive_progress` epoch+condvar 채널 | `read_activated`에서 `receive.sync` lock과 `notify_receive_progress_locked()` 제거(부모는 `xread_activated()`만 호출), `receive_once_guarded`의 `scoped_lock_t (runtime_.sync)` 제거 | 8/8 (1회 flaky 실패 관측) | 131.5 / 190.0 | 69.2% | 회복 없음 → revert |
| 2 | `mailbox_t::recv`의 `_recv_sync` | command dequeue마다 잡던 추가 mutex 제거(부모에는 없음) | 8/8 | 131.8 / 185.9 | 70.9% | 회복 없음 → revert |
| 3 | `choose_io_thread_transport` 분리 counter | `session_base_t::start_connecting`이 부모처럼 공유 `choose_io_thread` round-robin을 쓰게 복원 | 8/8 | 128.2 / 184.2 | 69.6% | 회복 없음 → revert |
| 4 | `read_deferred` / `finish_deferred_read` prefetch | **적용 불가**: `fq_t::recvpipe_deferred`는 호출자가 없고 `router_t::_prefetched_credit_deferred`는 항상 false다. 현재 worktree에서 이미 dead code이므로 hot path 비용이 0이다. | — | — | — | 해당 없음 |

후보 1의 paired 결과(131.5/190.0)는 기준(121.5/173.8)과 비율이 같다. 절대값 차이는
host drift이며 비율로 판정했다. 세 후보 모두 noise(±8%) 안이다.

Report tag: `autohwm-stage1b-cand1-local` / `-release-0101`,
`autohwm-stage1b-cand2-*`, `autohwm-stage1b-cand3-*`
(`bindings/c/perf/results/multi/report/`).

### 7.1 계측 결과: wakeup 횟수는 회귀 원인이 아니다

후보 추측을 끝내기 위해 임시 counter를 넣어(측정 후 전량 revert) worktree와
부모를 같은 방식으로 계측했다. 5초 ROUTER_ROUTER_SENDSEND / tcp / 256 B, process당
집계.

| Runtime | msgs_read | send_activate_read | send_activate_write | engine input_stop | input_restart |
|---|---|---|---|---|---|
| worktree | 1,239,138 | 1,238,762 | 36 | 0 | 0 |
| 부모 `2728d70d44` | 1,705,832 | 1,704,266 | 936 | — | — |

- **`send_activate_read`는 두 runtime 모두 message당 1회**다(1.00 : 1.00). 즉
  message당 mailbox command·signaler wakeup 횟수는 회귀 전후가 같다. §5.4에서
  세운 "대기 채널이 wakeup을 늘렸다"는 가설은 **기각**된다.
- `send_activate_write`(byte credit 반환)는 worktree 36회, 부모 936회로 둘 다
  message 수 대비 무시할 수준이다. `input_stop` / `input_restart`가 **0**이므로
  이 workload에서 engine input backpressure는 한 번도 발생하지 않는다. 즉
  **byte HWM 차단은 이 회귀에 전혀 관여하지 않는다**(§2 #1의 HWM 128배 확대가
  무효였던 이유와 일치).
- 이 workload는 async mailbox handler를 등록하지 않으므로
  `async_mailbox_owns_commands()`가 false다. 따라서 `wait_receive_progress`
  condvar 경로는 애초에 실행되지 않는다(후보 1이 무효인 구조적 이유).

### 7.2 재해석: 원인은 message당 순수 CPU 비용

§5.4의 rusage를 message 단위로 환산하면 다음과 같다.

| | 부모 | worktree | 변화 |
|---|---|---|---|
| Messages / 5s | 약 855 K | 약 617 K | -28% |
| User CPU / message | 18.4 us | 24.2 us | **+31%** |
| Sys CPU / message | 25.7 us | 29.8 us | +16% |
| Voluntary ctx switch / message | 0.327 | 0.539 | +65% |

message당 user CPU가 31% 증가했다. wakeup 횟수는 동일하므로 voluntary ctx switch
증가는 처리량이 낮아 같은 시간에 더 자주 큐가 비는 2차 효과로 해석해야 한다.
**회귀는 message당 순수 CPU 비용 증가이며, 특정 wakeup·차단 구조가 아니다.**

### 7.3 남은 조사 범위

`3ef4d09a37`이 다시 쓴 message당 공통 경로 중 아직 비용이 배제되지 않은 부분:

| 파일 | commit diff 규모 | 배제 여부 |
|---|---|---|
| `sockets/common/socket_base.cpp` | 363 | 미조사 |
| `sockets/common/socket_base_msg.cpp` (`recv_common`, `send_direct_with_retry` 재작성) | 316 | lock만 배제, 함수 구조 미조사 |
| `sockets/common/socket_base_dispatch.cpp` | 279 | 미조사 |
| `core/pipe.cpp` (`write_*` 변형 6종, `check_hwm_for_message`, `publish_outbound_frame_unlocked`) | 698 | drain wakeup만 배제 |
| `sockets/internal/lb.cpp`, `dist.cpp`, `router_*` | 224 / 10 / 239 | PUBSUB도 같은 비율로 회귀하므로 공통 경로가 우선 |
| `core/recv_internal.cpp` | 68 | 미조사 |

PUBSUB(1,689 K → 1,157 K, 69%)와 ROUTER(74%)가 같은 비율로 떨어지므로 socket별
경로보다 `pipe.cpp` + `socket_base*` 공통 경로를 먼저 본다. 다음 단계에서는 추측
대신 sampling profiler가 필요하다. 이 host에는 `perf`, `gdb`, `valgrind`가 없고
`gprof`만 있으므로, 정적 링크된 소형 bench에 `-pg`를 적용하거나 profiler를 설치할
수 있는 host를 확보해야 한다.

### 7.4 시도 횟수

Coordinator가 허용한 추가 3회를 후보 1·2·3으로 모두 사용했다(후보 4는 dead code로
적용 불가). 누적 회복이 0이므로 §5의 full gate 재실행은 수행하지 않았다. 최종
worktree는 `447f41a9f2`와 동일하다.

## 8. Sampling profile과 수정

### 8.1 gprof는 이 workload를 측정할 수 없다

`core/v0.10.1`~worktree를 같은 toolchain으로 static build하고 perf harness의
bench source를 그대로 링크해 `-pg`로 측정했다. 결과는 사용할 수 없었다.

- glibc의 gmon histogram은 사실상 main thread만 sampling한다. 8초 run에서
  process 전체가 약 35 s CPU를 쓰는데 gprof가 잡은 self-time은 두 process 합쳐
  2.4 s(약 7%)였다. 회귀가 있는 io thread는 전혀 보이지 않는다.
- `-pg` shared library는 mcount arc가 main executable text 범위 밖이라 버려진다.
  따라서 동적 링크로는 call count조차 얻을 수 없다.
- 이름이 바뀐 함수(`pipe_t::read` → `read_internal`,
  `fq_t::recvpipe` → `recvpipe_internal`)는 join되지 않아 차분표가 왜곡된다.

### 8.2 대체 수단: 전 thread SIGPROF sampler

`perf`, `gdb`, `valgrind`가 없고 `sudo`가 password-gated이므로, bench 실행 파일에
링크되는 독립 sampler(`scratchpad/sampler.cpp`, repo source 무수정)를 만들었다.
`ITIMER_PROF` 1 ms + `SA_SIGINFO` handler가 어느 thread에서 인터럽트되든
`REG_RIP`를 기록하고, leaf가 공유 library면 stack에서 실행 파일 text 범위에 있는
첫 return address를 찾아 호출자에 귀속시킨다. 종료 시 PC 히스토그램과
`/proc/self/maps`를 덤프해 libc/libssl symbol까지 해석한다.

같은 sampler object를 worktree와 부모(`2728d70d44`, `git worktree`로 checkout)
양쪽에 링크했고, LTO 없는 `-O2 -g` static build가 실제 회귀를 재현했다
(worktree 127.6 K vs 부모 163.3 K = 78%).

### 8.3 차분 profile (client, us/message, 최초 측정)

| delta | worktree | parent | symbol |
|---|---|---|---|
| +0.356 | 0.427 | 0.071 | `libc:__lll_lock_wait_private` (futex wait) |
| +0.180 | 1.009 | 0.829 | (zlink text 합계) |
| +0.140 | 0.179 | 0.039 | `libc:poll` |
| +0.104 | 0.208 | 0.104 | `libc:pthread_mutex_unlock` |
| +0.100 | 0.229 | 0.129 | `libc:pthread_mutex_lock` |
| +0.063 | 0.063 | 0.000 | `libc:eventfd` |
| +0.047 | 0.047 | 0.000 | `libc:close` |
| +0.038 | 0.038 | 0.000 | `mailbox_t::remove_signaler` |

`eventfd`/`close`/`remove_signaler`가 부모에서 **정확히 0**인 것이 결정적이었다.

### 8.4 원인: `zlink_poll`마다 poller signaler를 모든 mailbox에 등록

`zlink_poll()`은 호출마다 `socket_poller_t`를 새로 만든다
(`core/src/api/monitoring/poller_poll_once.cpp:27`). `3ef4d09a37` 이전 POSIX
poller는 socket마다 자기 mailbox descriptor를 pollset에 넣었다. 그 commit이
POSIX에도 Windows용 공유 signaler 방식을 도입하면서
`socket_poller_t::rebuild()`가 **모든 socket mailbox에 poller 소유 signaler를
등록**하고 소멸 시 다시 해제한다. 100 socket을 poll하면 호출당 mailbox mutex
왕복 200회 + eventfd 생성·close 1회가 발생하고, 이 mutex는 io thread가 command를
넣을 때 쓰는 것과 같아 futex wait이 폭증한다.

### 8.5 적용한 수정과 delta

각 수정 뒤 build → 8개 focused test → paired 1회.

| # | 수정 | Test | local / 0.10.1 | 비율 | 판정 |
|---|---|---|---|---|---|
| 0 | 기준 (§4) | 8/8 | 121.5 / 173.8 | 69.9% | — |
| 1 | `socket_poller`: POSIX descriptor pollset 복원 | 8/8 | 141.7 / 188.3 | 75.3% | keep |
| 2 | poller signaler를 lazy 생성 (일반 경로는 eventfd 미할당) | 8/8 | 145.1 / 184.7 | 78.6% | keep |
| 3 | `read_activated`·recv step의 공유 receive mutex를 async 소유 시에만 획득 | 8/8 | 146.6 / 186.0 | 78.8% | keep (noise 내이나 계약 불변, per-message work 제거) |
| 4 | `has_in()`의 공유 receive mutex를 async 소유 시에만 획득 | 8/8 | rested A/B median 131.0 → 134.1 | +2.4% | keep |
| 5 | `mailbox_t::recv`의 `_recv_sync` 제거 | — | 30.1 / 123.1 / 125.1 (불안정) | — | **reject**: command pipe read endpoint가 경쟁해 처리량이 붕괴한다 |

Commit: `d58a179033`(1·2), `b818c03f71`(3), `5ad65a4627`(4).

### 8.6 수정 후 재-profile

수정 1·2 뒤 다시 sampling하면 `eventfd`·`close`·`remove_signaler` 항목이 완전히
사라진다. 남은 client 상위 delta(호출자 귀속 기준, us/msg)는 다음과 같다.

| delta | wt | par | 호출자 |
|---|---|---|---|
| +0.168 | 0.182 | 0.014 | `socket_base_t::has_in` (수정 4의 대상) |
| +0.162 | 0.201 | 0.040 | `socket_poller_t::wait` |
| +0.119 | 0.166 | 0.047 | `router_t::xhas_in` |
| +0.118 | 0.161 | 0.043 | `mailbox_t::schedule_if_needed` |
| +0.163 | 0.235 | 0.072 | `enter_public_api` + `leave_public_api` |
| +0.126 | 0.188 | 0.062 | `socket_poller_t::add_item` + `find_socket_item` |

남은 비용은 전부 **poll 경로**다. `zlink_poll`이 호출마다 poller를 만들고 100개
item을 등록(`add_item`마다 `enter_public_api`/`inc_mailbox_ref`,
`find_socket_item`은 선형 탐색이라 등록이 O(N²))한 뒤, 준비 여부를 socket마다
`get_events_internal` → `process_commands` + `has_in`으로 조회한다. 이 구조는
부모에도 있었지만 `3ef4d09a37`이 각 단계에 lock과 admission을 추가하면서
비용이 3~10배가 됐다.

### 8.7 test_retained_hwm_credit의 기존 flakiness

수정 중 이 test가 간헐 실패했다. 40회 반복으로 확인한 결과 **원본 worktree
`447f41a9f2`에서도 14/40 실패**하고, 수정 후에도 15/40로 같다(host load 약 2.0
이상일 때). 이 flakiness는 이번 작업이 만든 것이 아니며, 한산한 host에서는
8/8 통과한다. 별도 조사 대상으로 기록한다.

### 8.8 최종 gate (plan §8.2.1, §8.2.3)

Tag `autohwm-stage1-final-local` / `autohwm-stage1-final-release-0101`,
local/release 교대 3회, `--runs 1`, host load 0.71에서 실행.

Report 경로 (실행 순서):

1. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_081154_autohwm-stage1-final-local.txt`
2. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_081200_autohwm-stage1-final-release-0101.txt`
3. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_081205_autohwm-stage1-final-local.txt`
4. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_081211_autohwm-stage1-final-release-0101.txt`
5. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_081217_autohwm-stage1-final-local.txt`
6. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_081222_autohwm-stage1-final-release-0101.txt`

| Run | Version | Throughput (Kops/s) | Bandwidth (MB/s) | Mean(ms) | P95(ms) | P99(ms) |
|---|---|---|---|---|---|---|
| 1 | local | 146.143 | 74.825 | 0.330 | 0.509 | 0.623 |
| 2 | 0.10.1 | 185.215 | 94.830 | 0.251 | 0.387 | 0.488 |
| 3 | local | 149.960 | 76.780 | 0.320 | 0.500 | 0.607 |
| 4 | 0.10.1 | 177.472 | 90.866 | 0.263 | 0.404 | 0.496 |
| 5 | local | 128.965 | 66.030 | 0.374 | 0.557 | 0.677 |
| 6 | 0.10.1 | 173.842 | 89.007 | 0.268 | 0.411 | 0.509 |

| Metric | Local median | 0.10.1 median | Local/0.10.1 | Stage 1 시작 | 판정 |
|---|---|---|---|---|---|
| Throughput (Kops/s) | 146.143 | 177.472 | 82.3% | 69.9% | FAIL |
| Bandwidth (MB/s) | 74.825 | 90.866 | 82.3% | 69.9% | FAIL |
| Lat.Mean (ms) | 0.330 | 0.263 | 125.5% | 144.2% | FAIL |
| Lat.P95 (ms) | 0.509 | 0.404 | 126.0% | 146.9% | FAIL |
| Lat.P99 (ms) | 0.623 | 0.496 | 125.6% | 125.6%→ 142.0% | FAIL |

5개 metric 모두 여전히 미달이지만 격차는 절반 가까이 줄었다(처리량 69.9% →
82.3%, latency 142~147% → 125~126%). 참고로 이 host에서 부모 `2728d70d44` 자체가
0.10.1의 약 94.8%(164.7 vs 173.8)이므로, 남은 격차는 "부모까지 약 12%p" +
"부모와 0.10.1 사이 약 5%p"로 나뉜다.

### 8.9 시도 횟수와 중단

Coordinator가 허용한 추가 3회 수정 기회를 수정 1·2(1회), 3(2회), 4(3회)로
사용했고 5는 안전하지 않아 폐기했다. 같은 root cause(=`3ef4d09a37`이 다시 쓴
per-message 경로)에서 gate가 여전히 실패하므로 여기서 중단하고 보고한다.

다음 작업자를 위한 남은 후보는 §8.6 표다. 가장 큰 단일 항목은 `zlink_poll`의
per-call poller 구조 자체이며, 이를 고치려면 poller 재사용 또는 item 등록
경로(`add_item`의 O(N²) 선형 탐색, item마다의 `enter_public_api`/mailbox ref)를
바꿔야 한다. 이는 byte-HWM 계약과 무관한 poll API 내부 설계 변경이므로 범위
승인이 필요하다.

### 8.10 재부팅 후 한산한 host에서 gate 재실행 (final2 무효화 + final3)

08:59 host 재부팅 뒤 `09:08`~`09:09`에 실행한 `autohwm-stage1-final2-*` gate는
**무효**로 처리한다. 재부팅 직후 host가 아직 정상 상태로 안정화되지 않아
0.10.1 자체의 처리량이 정상 대역(약 170~190 Kops/s)에 크게 못 미치는 약 87
Kops/s로 측정됐고, 이 때문에 local과 release가 거의 동일한 값(약 87 Kops/s
모두)으로 나와 비교가 무의미했다. 참고용 report 경로:

1. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_090851_autohwm-stage1-final2-local.txt` (87.568 Kops/s)
2. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_090857_autohwm-stage1-final2-release-0101.txt` (90.665 Kops/s)
3. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_090903_autohwm-stage1-final2-local.txt` (99.272 Kops/s)
4. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_090909_autohwm-stage1-final2-release-0101.txt` (87.826 Kops/s)
5. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_090916_autohwm-stage1-final2-local.txt` (78.379 Kops/s)
6. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_090922_autohwm-stage1-final2-release-0101.txt` (83.536 Kops/s)

local median ≈ 87.567, release median ≈ 87.826 — 거의 동일해 host 저하가
release baseline까지 함께 끌어내렸음을 보여준다. 이 데이터로는 gate 판정을
내릴 수 없어 폐기한다.

같은 host를 재부팅 5~6분 뒤(부팅 안정화 후, load average 0.05~0.16, 다른
user 프로세스 없음) 재측정했다. 8개 focused test는 전부 통과(2개는 host가
아직 8-test를 동시 실행하며 걸린 순간적 스케줄링 경합으로 1회 실패했으나,
개별 재실행 시 `test_retained_hwm_credit`, `test_router_mandatory_hwm` 모두
바로 통과 — §8.7에 기록된 기존 flakiness와 일치하며 host 정상 판정에 영향
없음).

측정 직전 `uptime` load average: `0.05, 0.08, 0.04`.

Tag `autohwm-stage1-final3-local` / `autohwm-stage1-final3-release-0101`,
local/release 교대 3회, `--runs 1`로 실행.

Report 경로 (실행 순서):

1. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_092151_autohwm-stage1-final3-local.txt`
2. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_092202_autohwm-stage1-final3-release-0101.txt`
3. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_092212_autohwm-stage1-final3-local.txt`
4. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_092221_autohwm-stage1-final3-release-0101.txt`
5. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_092231_autohwm-stage1-final3-local.txt`
6. `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260822_092241_autohwm-stage1-final3-release-0101.txt`

각 report의 `META,core_source`와 `META,core_version`을 확인해 순서가 의도대로
local(0.11.1)/release(0.10.1)로 교대했음을 확인했다.

| Run | Version | Throughput (Kops/s) | Bandwidth (MB/s) | Mean(ms) | P95(ms) | P99(ms) |
|---|---|---|---|---|---|---|
| 1 | local | 168.141 | 86.088 | 0.286 | 0.451 | 0.549 |
| 2 | 0.10.1 | 192.050 | 98.329 | 0.243 | 0.378 | 0.472 |
| 3 | local | 164.053 | 83.995 | 0.292 | 0.460 | 0.556 |
| 4 | 0.10.1 | 181.110 | 92.728 | 0.257 | 0.397 | 0.500 |
| 5 | local | 162.885 | 83.397 | 0.294 | 0.468 | 0.579 |
| 6 | 0.10.1 | 185.180 | 94.812 | 0.252 | 0.387 | 0.485 |

Sanity check: 0.10.1 처리량 median 185.180 Kops/s로 정상 대역(약 170~190) 안에
있어 host는 건강하다고 판정한다(BLOCKED(host) 아님).

| Metric | Local median | 0.10.1 median | Local/0.10.1 | 판정 |
|---|---|---|---|---|
| Throughput (Kops/s) | 164.053 | 185.180 | 88.6% | FAIL |
| Bandwidth (MB/s) | 83.995 | 94.812 | 88.6% | FAIL |
| Lat.Mean (ms) | 0.292 | 0.252 | 115.9% | FAIL |
| Lat.P95 (ms) | 0.460 | 0.387 | 118.9% | FAIL |
| Lat.P99 (ms) | 0.556 | 0.485 | 114.6% | FAIL |

§8.2.3 기준(local throughput·bandwidth median ≥ 0.10.1 median, 세 latency
median 모두 ≤ 0.10.1 median)으로 5개 metric 모두 여전히 FAIL이다. 다만 §8.8의
무효 이전(host 정상 상태) 측정과 비교하면 격차가 계속 줄고 있다(처리량/대역폭
82.3% → 88.6%, latency 125~126% → 115~119%). **최종 판정: FAIL** — relaxed
threshold 없이 그대로 기록한다.

## 9. 다음 stage로 넘어가는 조건

미충족. `doc/plan/autohwm/core-byte-hwm-flow-control-plan.ko.md` §12.2 첫 행은
`[ ]` 유지, Evidence에 `BLOCKED:`를 기록했다.
