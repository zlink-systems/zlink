# CCU-1 — STREAM 벤치 zlink 서버가 CCU 4000에서 실패하는 원인

작성 2026-09-08. 담당 job CCU-1. 시간 상한 1.5 h 내 종료.
Core 0.17.3 (`core/build/lib/libzlink.so.0.17.3`), 러너 `bindings/c/bench/with_stream/run_benchmarks.sh`,
서버 `bindings/c/bench/with_stream/stacks/zlink/test_scenario_stream_zlink.cpp`.

## 1. 결론 한 줄

**Core 결함**이다. STREAM 소켓에 애플리케이션 파이프가 붙을 때마다
`socket_base_t::attach_pipe()`가 **컨텍스트 전체 Auto-HWM 재계산을 동기로** 수행하고,
그 재계산은 컨텍스트의 모든 physical queue를 순회한다. 연결 N개를 받으면 O(N²)이 되어
N=3000 이상에서 애플리케이션 스레드가 수신 루프에 돌아오지 못한다 → 측정 창(3 s) 동안 에코가 0건.
벤치 서버·환경(fd 한도·포트·backlog)은 원인이 아니다.

동기 재계산을 debounce 예약으로 바꾼 실험 빌드에서 **CCU 4000이 263.2 kops로 정상 통과**했다(§5).

## 2. 실패 증거

원 측정 `bindings/c/bench/with_stream/results/20260908_162232`:

| 항목 | 값 |
|---|---|
| `skipped_stacks.csv` | `zlink,run_failed` |
| `benchmark.log` | `16:22:32 start` → `16:22:41 client failed ... rc=2` (9 s) |
| zlink 서버/클라이언트 로그 | 0 바이트 |
| 다른 5개 스택 | 전부 PASS (asio 248.1, asio_pull 212.5, cppserver 285.6, cppserver_pull 291.8, zmq 251.1 kops) |

재현 `results/20260908_171056` (`--stack zlink --size 64 --ccu 4000 --duration 3 --runs 1 --reuse-build`, `PERF_DEBUG=1`):

```
perf_stream_client: case_failed size=64 connect_ok=4000 connect_fail=0 send_error=0
  recv_error=0 timeout_error=1 size_mismatch=0 throughput_bps=0.000 samples=0 window_ok=0
```

- **연결은 4000개 전부 성공**(`connect_fail=0`). connect 거부·backlog·fd 한도·TIME_WAIT 문제가 아니다.
- 3 s 액티브 윈도에서 **RTT 샘플 0개, throughput 0** — 에코가 한 건도 돌아오지 않았다.
- rc=2 경로는 `perf_stream_bench_client.hpp:598` (`run_active_window` 끝에서 `outstanding_total > 0`)이다.
- 서버 로그가 빈 것은 러너 `run_benchmarks.sh:847 stop_active_server()`가 SIGINT 후 **1 s만에 SIGKILL** 하기 때문이며
  (4000 파이프 teardown이 1 s를 넘김), 행(hang)의 증거가 아니다. 수동 실행에서는 SIGINT 후 30 s를 줘도 종료하지 않았다.

### 환경 (원인 아님)

| 항목 | 값 | 판정 |
|---|---|---|
| `ulimit -n` | 1048576 (soft=hard) | 여유. 서버 fd 최대 4024 |
| `ip_local_port_range` | 32768–60999 (28231) | 여유 |
| `somaxconn` | 4096, 서버 backlog 요청 32768 | 4000 수용 |
| `ss -tan` estab (측정 중) | 8001 (양끝 4000쌍 + listener) | 커널은 전부 수용 |
| EMFILE/ENOMEM/OOM | 서버 stderr 없음, `dmesg` OOM 없음, 서버 RSS 134 MB | 아님 |
| 같은 CCU의 asio | PASS | 머신 한계 아님 |

## 3. 임계와 스레드 프로파일

수동 프로브(서버 직접 기동, `--duration 3`, 4 io-threads). `ut`는 메인(애플리케이션) 스레드 누적 utime(tick, 1 tick=10 ms), 1 s 간격:

| CCU | 클라이언트 rc | throughput | 메인 스레드 ut 추이 |
|---:|---|---:|---|
| 1000 | 0 | 263.4 kops | 0 77 153 227 231 231 … (포화) |
| 2000 | 0 | 110.6 kops | 0 78 175 261 281 282 … (포화) |
| 3000 | **2** | — | 0 66 161 259 354 448 544 580 … (5.8 s 소모 후 종료) |
| 4000 | **2** | — | 0 62 163 267 362 461 557 657 754 849 951 1048 (12 s 시점에도 계속 증가) |

같은 창의 I/O 스레드 4개는 누적 ut = 5 / 4 / 2 / 2 tick, wchan = `do_epoll_wait`로 **거의 완전히 유휴**였다.
즉 병목은 I/O가 아니라 **애플리케이션 스레드 1개가 Core 내부에서 CPU를 100 % 태우는 것**이다.
CCU 2000에서 이미 throughput이 1000 대비 42 %로 떨어지는 것도 같은 비용의 연속적인 표현이다.

## 4. 원인 — 스택 샘플 (file:line)

`backtrace()` 기반 1 Hz 샘플러를 LD_PRELOAD로 붙여 메인 스레드를 표본화했다(샘플 13개 중 12개가 동일 스택, 나머지 1개는 연결 전 `poll`).
`addr2line`로 해석한 스택:

```
zlink_poller_wait
 └ socket_poller_t::wait                     (src/runtime/.../socket_poller.cpp)
   └ socket_poller_t::check_events
     └ socket_base_t::get_events_internal
       └ socket_base_t::process_commands
         └ object_t::process_command
           └ socket_base_t::attach_pipe      ← 파이프 1개 붙일 때마다
             └ ctx_t::auto_hwm_recalculate_now
               └ ctx_physical_queue_registry_t::plan_application_queues
                 └ ctx_physical_queue_registry_t::find_locked
```

해당 코드:

| 위치 | 내용 |
|---|---|
| `core/src/runtime/sockets/common/socket_base_api.cpp:634-647` | `attach_pipe()` 끝. `recalculate_application_attach && auto_hwm_policy_enabled` 이면 debounce를 건너뛰고 `get_ctx()->auto_hwm_recalculate_now()`를 **동기 호출** |
| `core/src/runtime/sockets/common/socket_base_api.cpp:311-326` | staged application pipe 경로에도 같은 동기 호출 (주석: "immediate budget snapshot observes it") |
| `core/src/runtime/core/ctx_auto_hwm_recalc.cpp:119-198` | `auto_hwm_recalculate_now()`: 모든 소켓 수집 → `collect_auto_hwm_queue_policies`로 **모든 파이프의 정책 벡터 생성** → `plan_application_queues` → 소켓마다 `apply_physical_auto_hwm_plan` |
| `core/src/runtime/core/ctx_physical_queue_registry.cpp:749-830` | `plan_application_queues()`: 입력 정책 전부에 `find_locked`, 이어서 **`_directions` 전체(연결당 2개)를 순회**하며 `std::map<uint64_t, resolved_input_t>` 를 매번 새로 구축 |
| `core/src/runtime/sockets/stream/stream.cpp:112` | `stream_t` 생성자에서 `refresh_auto_hwm_policy()` → STREAM은 Auto-HWM 정책이 켜진 상태로 동작 |

**비용**: 연결 1개 attach 당 O(현재 큐 수)의 맵 구축 + 순회. 연결 N개를 받는 동안 총 O(N²)(맵 연산이라 실제로는 N² log N).
N=1000이면 이 비용이 연결 폭주 구간 2~3 s 안에 끝나 측정이 성립하지만, N=4000이면 10 s를 넘겨
클라이언트의 3 s 측정 창 전체가 이 재계산에 잡아먹혀 **에코가 0건**이 된다.
`schedule_auto_hwm_recalculate()`의 debounce 기본값은 3000 ms(`core/include/zlink/core/api.h:49`)인데,
attach 경로는 그 debounce를 명시적으로 우회한다.

## 5. 인과 검증 (실험 패치)

worktree `~/project/zlink-work/ccu1`에서 `socket_base_api.cpp`의 두 동기 호출을
`schedule_auto_hwm_recalculate()`(debounce 3000 ms)로 바꾸고 release lib를 빌드해 같은 프로브를 다시 돌렸다.

| CCU | 현행 0.17.3 | 실험 패치 | 메인 스레드 ut(패치) |
|---:|---:|---:|---|
| 1000 | 263.4 kops | **294.2 kops** (+11.7 %) | 0 … 포화 |
| 4000 | **실패**(rc=2, 0 kops) | **263.2 kops**, p50 7.57 ms, `recv_msgs=793457`, `send_error=0` | 0 36 125 211 257 257 … (2.57 s 후 포화) |

CCU 4000에서 실패가 사라지고, asio 248.1 / cppserver 285.6 kops와 같은 대역에 들어온다.
CCU 1000에서도 +11.7 %가 나오는데, 이는 연결 폭주 구간의 동기 재계산 비용이 저 CCU에서도
측정 창을 갉아먹고 있었다는 뜻이다.

**이 패치는 원인 증명용이며 그대로 제안하지 않는다.** `socket_base_api.cpp:311-316`의 주석이
"정책이 켜져 있으면 토폴로지를 동기로 공표해 즉시 budget snapshot이 관측하게 한다"고 못 박고 있어,
debounce로 바꾸면 `zlink_ctx_auto_hwm_budget_snapshot` 계열의 즉시 가시성 계약이 깨질 수 있다
(관련 테스트: `core/tests/unittest/unittest_auto_hwm_policy.cpp`, `unittest_auto_hwm_physical_attempt.cpp`,
`unittest_single_lane_accounting.cpp`, `tests/integration/test_ctx_options.cpp`). 계약 테스트는 돌리지 않았다.

## 6. 제안하는 수정 (Core, 이 job에서는 미구현)

두 설계를 비교했다.

| 안 | 내용 | 장점 | 단점 |
|---|---|---|---|
| **A. 증분 계획 (권장)** | attach 경로에서 **새로 붙은 그 큐 하나만** 이미 적용된 context plan에 대해 계획·적용한다. `plan_application_queues`에 "이 큐만" 입력을 받는 증분 진입점을 두고, 전체 재계산은 지금처럼 debounce 태스크가 담당 | attach가 O(1) → O(N²) 소멸. **즉시 가시성 계약 유지**(새 큐가 붙는 즉시 스냅샷에 보임). 새 옵션·플래그·상태를 추가하지 않음(POSDDD) | 재계산 로직을 전체/증분 두 경로로 나눠야 함 — 몇 줄이 아니라 registry 리팩터 |
| B. attach 경로 debounce | §5의 실험 패치 | 2~4 줄 | 즉시 가시성 계약이 깨질 수 있음(위 테스트). 계약 변경이므로 공통 규칙상 D 항목 |

A를 권장한다. B는 A가 계약상 불가능하다고 판명될 때의 대안이며, 그 경우 "attach 직후 스냅샷 즉시 가시성"이
어느 스펙 절에서 요구되는지 확인이 선행되어야 한다(= D 항목: 깨지는 절 확인 필요).

어느 쪽이든 **벤치 서버(`test_scenario_stream_zlink.cpp`)와 러너는 고칠 것이 없다.** 환경 설정 요구사항도 없다
(현 머신의 fd 한도·포트·somaxconn 모두 4000 CCU에 충분하다).

부수적으로 러너의 `stop_active_server()`(`run_benchmarks.sh:847`)가 SIGINT 후 1 s만에 SIGKILL 해
고 CCU에서 서버의 `METRIC` 줄이 유실된다. 진단 편의를 위해 유예를 늘리는 것은 별도 사안이다(이 job에서는 변경하지 않음).

## 7. 변경 파일

- 메인 체크아웃: 없음(코드 변경 없음). 보고서·진행 파일만 추가.
- worktree `~/project/zlink-work/ccu1`: `core/src/runtime/sockets/common/socket_base_api.cpp` (§5 실험 패치, 커밋하지 않음).
- 측정 산출물: `bindings/c/bench/with_stream/results/20260908_171056` (재현),
  스크래치패드의 `probe_z1000/2000/3000/4000.txt`, `probe_f1000/f4000.txt`, `srv_sample.log`(스택 샘플).

## 8. 변경 분류

**B — 기존 결함**(Core, 연결 스케일). 계약 적응·우회가 아니다.
단, 제안 B(debounce)를 택할 경우에는 D(spec gap: attach 직후 budget snapshot 즉시 가시성)로 승격된다.

## 9. 남은 것

- 제안 A의 실제 구현과 Auto-HWM 계약 테스트 재확인은 하지 않았다(범위 밖·시간 상한).
- CCU 2000의 42 % throughput 저하가 이 결함만으로 전부 설명되는지(패치 후 CCU 2000 재측정)는 재지 않았다.
