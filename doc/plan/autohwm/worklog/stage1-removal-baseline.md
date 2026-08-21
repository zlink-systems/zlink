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

## 8. 다음 stage로 넘어가는 조건

미충족. `doc/plan/autohwm/core-byte-hwm-flow-control-plan.ko.md` §12.2 첫 행은
`[ ]` 유지, Evidence에 `BLOCKED:`를 기록했다.
