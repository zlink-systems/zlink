# G-10 — 즉시 성공 send의 `clock_gettime` 지연 평가

worktree는 `/home/hep7hep7/project/zlink-work/g10`, 기준은 detached `origin/main` `a2b087ff0c`다. 커밋하지 않았다.

## 1. 결과

STREAM zlink 축소셀(tcp, CCU 20, 1024 B, 10 s, server callgrind, idle 차감)의 결과다. 일반 판은
before 60,873 msg / after 58,246 msg이며, `--dump-instr=yes` 판은 인라인된 `rdtsc` 호출자 분해에만 썼다.
모든 valgrind 실행은 `pgrep -x ninja`가 빈 상태에서 지정된 `PERF_LOCK`으로 직렬화했다.

| 지표 | before | after | 변화 |
|---|---:|---:|---:|
| 전체 Ir/msg | 9,354.276 | 9,314.626 | **-39.650 (-0.42 %)** |
| 전체 `clock_gettime`/msg | 1.7195 | 1.4850 | **-0.2346 (-13.6 %)** |
| Core `clock_t` 기인 `clock_gettime`/msg | 0.5344 | 0.3052 | **-0.2291 (-42.9 %)** |
| Boost/Asio `steady_clock` 기인 `clock_gettime`/msg | 1.1852 | 1.1798 | -0.0054 |
| 즉시 send의 `submit_public_send_record -> clock_t::now_ms`/msg | **1.0000** | **0** | **-1.0000** |
| `clock_t::now_ms -> now_us` cache miss/msg | 0.2375 | 0.0031 | **-0.2344** |

G-A의 2.025/msg와 이번 pristine 1.720/msg 차이는 HEAD와 실행당 처리량 차이다. 이번 호출 그래프는 전체 값 중
약 1.18/msg가 Core의 timeout helper가 아니라 Boost/Asio `std::chrono::steady_clock::now()`임을 분리했다.

### 1.1 호출자별 횟수/msg

일반 callgrind의 함수 edge와 instruction-address dump를 함께 사용했다. `rdtsc`는 Release+LTO에서 인라인되므로
별도 dump 판(before 21,506 msg / after 59,321 msg)에서 실제 opcode 실행 수를 셌다.

| 호출자 -> 피호출자 | before | after | 판정 |
|---|---:|---:|---|
| public STREAM send -> `clock_t::now_ms` | **1.0000** | **0** | 즉시 admission에는 불필요, 제거 |
| `zlink_poller_wait` -> `clock_t::now_ms` | 0.2951 | 0.3003 | finite poll timeout 계산에 필요, 유지 |
| `socket_poller_t::wait` -> `clock_t::now_ms` | 0.1451 | 0.1476 | finite poll timeout 계산에 필요, 유지 |
| Auto-HWM schedule/apply -> `clock_t::now_ms` | 0.0010 미만/호출자 | 0.0010 미만/호출자 | debounce·진단 시각, 유지 |
| `clock_t::clock_t` 안의 `rdtsc` | 0.3855 | 0.2912 | cache 기준점, 유지 |
| `clock_t::now_ms` 안의 `rdtsc` | 1.5729 | 0.4347 | cache hit 판정, 즉시 send 1회가 사라짐 |
| `process_commands` throttle의 직접 `rdtsc` | 0.0494 | 0.0464 | command poll 최대 지연 제어, 유지 |
| **`rdtsc` 합계** | **2.0078** | **0.7722** | **-1.2356/msg** |

dump 판은 처리량 차이 때문에 poller 횟수/msg가 달라졌지만, 변경 대상 row는 caller edge와 opcode 모두 정확히
1.0000/msg에서 0으로 사라졌다. auto-HWM은 메시지별 회계에서 시간을 읽지 않고 topology/option 재계산 때만 읽었다.
`socket_base_monitor.cpp`에는 generic event timestamp용 clock 호출이 없었다.

## 2. 변경 파일

| 파일 | 변경 |
|---|---|
| `core/src/runtime/sockets/common/socket_send_submit.cpp` | 기존 `deadline_ms`를 0 sentinel로 재사용하고 positive `SNDTIMEO` deadline을 최초 wait 직전에 계산 |
| `doc/plan/c016-worklog/core-rf-G-10-summary.md` | 이 보고서 |
| main worktree의 `doc/plan/c016-worklog/progress-G-10.md` | 진행 기록 |

`core/include/**`, `core/src/libzlink.vers`, 지정 충돌 파일 3개와 스펙 문서는 수정하지 않았다.

## 3. 설계 비교와 선택

- **선택: 기존 timeout budget의 deadline만 지연 평가.** 생성자는 호출 진입 시 `options.sndtimeo` 값을 그대로
  복사한다. 첫 admission이 실패해 실제 wait가 필요할 때 `refresh_timeout()`이 기존 `deadline_ms == 0`을
  sentinel로 사용해 deadline을 한 번 만든다. 새 옵션·flag·상태·public API가 없다.
- **기각: 진입 시 `rdtsc`를 별도 보관하거나 poller/clock에 공유 cache state 추가.** API 진입부터의 TSC 변환,
  wrap/migration 처리와 새 상태 소유 규칙이 필요하고 즉시 성공에도 `rdtsc` 1회가 남는다. poller timeout,
  Auto-HWM debounce와 monitor duration처럼 실제 시간이 필요한 경로까지 한 변경에 섞는다.

수정 전/후 규칙 수: **3(값 snapshot + wait 없는 deadline 선계산 + wait 중 갱신) -> 2(값 snapshot + 최초 wait에서 deadline 생성·갱신)**.

## 4. 재확인한 계약과 판정

- `core/doc/spec/core/socket/README.ko.md:969-973`: “`NONE FINAL`은 호출 진입 시 `SNDTIMEO`를 snapshot하고
  local send queue admission까지 기다린다”, `DONTWAIT`은 한 번만 시도하고 즉시 admission은 completion이 없다.
- `core/doc/spec/core/socket/08-stream.ko.md:122-129`: STREAM `NONE FINAL`은 같은 RID의 admission/reconnect를 기다리고,
  즉시 admission이면 ID 0·completion 없음이다.
- `core/doc/spec/core/05-polling.ko.md:248-252`: finite poller wait는 event/timeout을 구분해야 하므로 poller의 시각 계산은 유지했다.
- `core/doc/spec/core/systems/06-auto-hwm.ko.md:103-106`: option setter는 설정을 저장하고 debounce 경로로 재계산을 예약한다.
  따라서 schedule/due의 시각은 필요하지만 메시지별 admission 회계에는 필요하지 않다.
- `core/doc/spec/core/06-monitoring.ko.md:154-160,407-408`: `auto_hwm_last_recalc_ms`와 완료된
  `flow_pause_duration_ms`를 노출하므로 해당 전이의 시각은 필요하다. generic monitor event timestamp 문장은 없고
  구현에도 그런 clock 호출은 없다.
- `process_commands` throttle을 특정 주기로 요구하는 public spec 문장은 없다. 내부 정책은
  `core/src/runtime/utils/config.hpp:44-48`의 “Maximal delay to process command ... 3,000,000 ticks”이며,
  이 경로는 이미 `clock_gettime`이 아닌 `rdtsc`만 사용하므로 유지했다.

“snapshot” 대상은 configured `SNDTIMEO` 값이며 생성자에서 계속 고정한다. 첫 admission 성공은 기다리지 않으므로
deadline을 관찰하지 않는다. 실패하면 첫 wait 전에 deadline을 만들고 이후 같은 absolute deadline에서 남은 값을
계산한다. `0` 즉시·`-1` 무한, admission 결과, ID/completion, reconnect/target, HWM·flow 조건은 건드리지 않았다.
따라서 **어느 인용 문장도 다른 동작이 되지 않았다**.

소유 계층: blocking SEND/REQUEST admission timeout budget을 소유한 Core `socket_send_submit`.

교차언어 대조: C/C++/각 언어 binding은 같은 native Core 경로를 사용해 별도 언어 runtime 구현이 없다. 비교 대상
zmq의 G-A 값은 0.073/msg였지만 이번 attribution으로 zlink 잔여 1.485/msg 중 1.180/msg가 Boost/Asio임을 확인했다.

변경 분류: **B — 기존 결함(실제 wait가 없는 성공 경로에서 deadline을 선계산한 불필요한 hot-path 작업)**.

## 5. 검증

| 항목 | 결과 |
|---|---|
| `JOBS=4 scripts/build-core.sh dev` | 성공 |
| `ctest --test-dir core/build-dev -R 'timeout\|sndtimeo\|rcvtimeo\|hwm\|monitor\|send\|recv\|stream' --output-on-failure` x5 | **49/49 x5, 245/245 PASS** |
| `JOBS=4 scripts/build-core.sh release --lib-only` | 성공, 최종 patch 상태로 재링크 |
| `core/build-gate` Release+LTO, `hotpath_bench` target만 `-j4` | before/after 빌드 성공 |
| 축소 STREAM callgrind full/idle before/after | 성공, parse/protocol/send error 0 |
| `git diff --check` | 성공 |
| 공개 헤더/export diff | 없음 |

### 5.1 hotpath 5셀 — 주 판정

| 셀 | before Ir/msg | after Ir/msg | 변화 | reference 판정(after) |
|---|---:|---:|---:|---|
| `dealer_dealer_inproc` | 3271.778 | 3231.215 | **-40.563 (-1.24 %)** | ratio 0.9438, 개선 과다 FAIL |
| `dealer_router_reqrep_inproc` | 18861.654 | 18734.949 | **-126.705 (-0.67 %)** | 0.9519 PASS |
| `pair_inproc` | 2330.398 | 2287.624 | **-42.774 (-1.84 %)** | 0.9741 PASS |
| `router_router_tcp` | 2916.505 | 2874.050 | **-42.455 (-1.46 %)** | 0.9669 PASS |
| `stream_tcp` | 14457.626 | 14421.722 | **-35.904 (-0.25 %)** | 0.9862 PASS |

5셀 모두 before보다 개선됐다. `dealer_dealer_inproc` 한 건은 회귀가 아니라 reference보다 5.62 % 좋아져 양방향
5 % gate가 FAIL로 표시한 것이다. reference 파일은 수정하지 않았다.

남은 실패: 기능 테스트 없음. 성능 gate의 개선 과다 1건만 남았다.

멈춘 지점: 없음. 요청 범위 구현·검증·보고를 완료했다.
