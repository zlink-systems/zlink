# TCP 단일 smoke hang 회귀 조사·수정 기록 (2026-08-23)

> 선행 기록: `doc/perf/perf/bindings-0.12.0/log/2026-08-23-cpp-single-smoke.md`
> 대상 branch/commit: `codex/bindings-0.12.0-performance` / `9855b8f57d`
> 용의 commit: `5d2bf1e84f` "perf: finalize byte-HWM regression work and handoff"
> 관련 설계 문서: `doc/plan/core-byte-hwm-performance-regression-handoff.ko.md`

## 1. 증상

`bindings/c/perf/run_benchmarks.sh --pattern PAIR --transports tcp --msg-sizes 64`
가 `status: partial`(`PAIR current tcp 64B: timeout`)로 끝난다. `perf_pair`를 직접
실행하면 `Connected to tcp://127.0.0.1:<port>` 이후 아무 출력 없이 무기한 hang한다
(90초까지 확인, 프로세스는 CPU를 전혀 쓰지 않는 완전 정지 상태).

`inproc`/`ipc`는 같은 바이너리로 정상 동작한다. OS 수준 loopback TCP도 정상이다.

### 1.1 선행 기록의 진단 정정

선행 기록은 `setup_connected_pair()`의
`wait_for_socket_monitor_event(..., ZLINK_EVENT_CONNECTION_READY, 1000)`이
자기 timeout을 지키지 못하고 무기한 block한다고 추정했다. **이 추정은 틀렸다.**

`bindings/c/perf/single/common/perf_single_monitor.hpp`의 wait helper에 임시 계측
로그를 넣어 확인한 결과는 다음과 같다.

```
[DBG] enter wait ev=4096
[DBG] pre poller_wait to=999
[DBG] post poller_wait rc=1
[DBG] pre destroy ready=1
[DBG] post destroy      <- bind_monitor,  ready=1
[DBG] enter wait ev=4096
... 동일 ...
[DBG] post destroy      <- connect_monitor, ready=1
```

즉 `ZLINK_EVENT_CONNECTION_READY`는 두 socket 모두 1초 안에 정상 수신되고
handshake도 성공한다. hang은 그 뒤 **데이터 구간(`run_active_phase`)**에서
발생한다. perf 러너는 setup 성공 시 아무 로그도 남기지 않기 때문에 겉으로는
setup에서 멈춘 것처럼 보였을 뿐이다. C 러너 wait helper 자체에는 결함이 없다.

## 2. 회귀 구간 증거

| 대상 | libzlink | PAIR/tcp/64B |
|------|----------|--------------|
| 부모 commit `fdcaca94b4` (별도 worktree + 신규 Release 빌드) | `.../parent/core/build/lib` | **통과** — `RESULT,current,PAIR,tcp,64,throughput,2905276.200`, `latency,0.360` |
| HEAD `9855b8f57d` (= `5d2bf1e84f` 포함) | `core/build/lib` | **hang** (`exit=124`) |
| HEAD + `ZLINK_ASIO_TCP_DISABLE_SYNC_WRITE=1` | `core/build/lib` | 통과 — `throughput,3279428.400`, `latency,0.110` |
| 부모 `fdcaca94b4` + `ZLINK_ASIO_TCP_SYNC_WRITE=1` | `.../parent/core/build/lib` | **hang** (`exit=124`) |

두 binary는 동일한 `perf_pair`이고 `LD_LIBRARY_PATH`로 runtime만 교체했다.

결론:
- 결함 코드 자체(§3)는 **`5d2bf1e84f` 이전부터 존재**했다. 부모 commit에서도
  `ZLINK_ASIO_TCP_SYNC_WRITE=1`로 opt-in하면 똑같이 hang한다.
- `5d2bf1e84f`가 그 경로를 **기본값으로 바꾸면서** 잠재 결함이 전면 노출됐다.
  따라서 회귀를 도입한 commit은 `5d2bf1e84f`가 맞다.

`5d2bf1e84f`의 해당 diff (`core/src/runtime/transports/tcp/tcp_transport.cpp`):

```diff
-const bool tcp_allow_sync_write_on = env::flag_enabled ("ZLINK_ASIO_TCP_SYNC_WRITE");
+const bool tcp_allow_sync_write_on =
+  !env::flag_enabled ("ZLINK_ASIO_TCP_DISABLE_SYNC_WRITE");
```

같은 commit의 `socket_base_monitor.cpp` / `mailbox.cpp` / `control_runtime.cpp` /
`pipe.cpp` 변경은 이 hang과 무관함을 계측으로 확인했다
(`detach_monitor_socket`은 정상 반환, `wait_async_quiesced(10000)`도 즉시 만족,
main thread의 `mailbox_t::recv`는 200ms `RCVTIMEO`로 정상 순환).

## 3. 근본 원인

### 3.1 결함 지점

`core/src/runtime/transports/tcp/tcp_transport.cpp:133`(수정 전)

```cpp
//  The fd is already set to non-blocking; inform Asio to avoid blocking
//  synchronous write_some/read_some paths.
_socket->native_non_blocking (true, ec);
```

주석이 밝히는 의도("동기 write_some/read_some이 block하지 않게 한다")와 실제
호출 API가 어긋나 있다. Boost.Asio는 socket state에 **두 개의 독립된 non-blocking
비트**를 둔다.

- `native_non_blocking(true)` → `socket_ops::set_internal_non_blocking()` →
  `internal_non_blocking` 비트. Asio가 **자기 reactor 기반 비동기 연산**에 쓴다.
- `non_blocking(true)` → `socket_ops::set_user_non_blocking()` →
  `user_set_non_blocking` 비트. **동기 연산**이 참조하는 비트다.

`basic_stream_socket::write_some()`이 최종적으로 부르는
`socket_ops::sync_send()`는 다음 구조다.

```cpp
for (;;) {
    bytes = socket_ops::send (s, bufs, count, flags, ec);
    if (bytes >= 0) return bytes;
    if ((state & user_set_non_blocking) || (ec != would_block && ec != try_again))
        return 0;
    if (socket_ops::poll_write (s, 0, -1, ec) < 0)   // <-- timeout -1
        return 0;
}
```

`user_set_non_blocking`이 꺼져 있으므로 `EAGAIN`이 나면 **`poll(fd, POLLOUT, -1)`로
무기한 block**한다.

### 3.2 deadlock 성립 경로

1. `5d2bf1e84f` 이후 `tcp_transport_t::supports_speculative_write()`
   (`tcp_transport.cpp:732`)가 기본 `true`가 되고,
   `asio_engine_t::use_stream_speculative_write()` (`asio_engine.cpp:830`)는
   STREAM 이외의 일반 socket type(PAIR/ROUTER/…)에서도 그 값을 그대로 쓴다.
   → 일반 데이터 경로가 동기 write로 전환된다.
2. `asio_engine_t::speculative_write()`의
   `while (prepare_output_buffer ())` 루프(`asio_engine.cpp:1382`)와
   `start_async_write()`의 선행 동기 write(`asio_engine.cpp:666`)가
   `tcp_transport_t::write_some()` → `_socket->write_some()`
   (`tcp_transport.cpp:698`)을 반복 호출한다.
3. sender의 TCP send buffer가 가득 차면 `send()`가 `EAGAIN`을 돌려주고,
   §3.1 때문에 **IO thread가 `poll(fd, -1)`에서 그대로 잠든다.**
4. IO thread는 하나뿐이고(`ZLINKbg/IO/0`) 같은
   `asio_poller_t::loop()`(`asio_poller.cpp:445/460`)가 **두 peer engine을 모두**
   구동한다. 그 thread가 block되면 receiver engine이 이미 걸어둔
   `async_read_some`이 영원히 dispatch되지 않는다.
5. receiver가 읽지 않으므로 sender의 send buffer는 절대 비지 않는다 →
   **영구 self-deadlock**.

### 3.3 계측 증거

`asio_engine.cpp`/`asio_poller.cpp`에 임시 로그를 넣어 확인한 사실(로그는 수정 후
모두 원복했다):

- engine과 poller는 모두 같은 tid에서 돈다(단일 IO thread).
- IO thread의 poller loop은 `iter=2`에서 `run_for(100ms)`에 진입한 뒤
  **다시 돌아오지 않는다.** 그 사이 12,000줄이 넘는 engine 로그가 찍히고,
  마지막 줄은 항상 `start_async_write: outsize=NNN`이다. 즉
  `start_async_write()`의 선행 동기 write 안에서 멈춘다.
- 반면 다른 poller thread는 `iter=121`까지 `poll=0`으로 100ms마다 정상 순환한다.
- `on_read_complete`는 handshake 구간 4회 이후 **한 번도** 발생하지 않는다
  (정상 실행에서는 26,905회).
- `/proc/<pid>/task/*/wchan`: main thread `futex_wait_queue`(200ms `RCVTIMEO`
  순환), `ZLINKbg/IO/0` **`do_sys_poll`** — `poll()`에서 잠든 상태로,
  §3.1의 `poll_write(s, 0, -1)`과 정확히 일치한다.
- `ss -tnp` 스냅샷: sender fd `Send-Q=4159232`, receiver fd `Recv-Q=117912`.
  커널에 데이터가 쌓여 있는데 아무도 읽지 않는 전형적인 정지 상태다.

### 3.4 왜 inproc/ipc는 멀쩡했나

`ipc_transport.cpp:43`의 `ipc_allow_sync_write_on`은 여전히 opt-in
(`ZLINK_ASIO_IPC_SYNC_WRITE`)이라 동기 write 경로를 타지 않는다. 다만 같은 파일
115행이 TCP와 **완전히 동일한 `native_non_blocking()` 오용**을 하고 있어 잠재
결함은 동일하다. inproc은 transport socket 자체가 없다.

## 4. 적용한 수정

설계 문서 `doc/plan/core-byte-hwm-performance-regression-handoff.ko.md` §유지한 구현은
"TCP speculative synchronous write는 기본 경로로 유지하되 환경 변수로 진단할 수
있다"를 명시하고, §폐기한 실험은 "speculative sync write 비활성"을 이미 폐기 항목으로
기록한다. 따라서 **flag 기본값을 되돌리지 않고**(= commit 전체 revert 없이) 실제
결함인 non-blocking 비트만 고쳤다.

### 4.1 변경 파일

| 파일 | 변경 |
|------|------|
| `core/src/runtime/transports/tcp/tcp_transport.cpp:144` | `_socket->native_non_blocking (true, ec)` → `_socket->non_blocking (true, ec)` + 근거 주석 |
| `core/src/runtime/transports/ipc/ipc_transport.cpp:119` | 동일 (동일한 잠재 결함 제거) |

기능 코드 변경은 이 2줄이 전부다.

### 4.2 왜 이 수정이 옳은가

- `non_blocking(true)`는 `user_set_non_blocking` 비트를 세우므로
  `sync_send()`/`sync_recv()`가 `EAGAIN`을 **`would_block` 오류로 즉시 반환**한다.
  engine은 이미 그 경로를 처리한다(`asio_engine.cpp:672-676`,
  `asio_engine.cpp:1340-1348` → `start_async_write()`로 fallback).
  결과적으로 IO thread가 절대 block되지 않고 reactor로 복귀한다.
- 비동기 경로는 영향받지 않는다. Asio의 reactive socket service는 async 연산
  개시 시 `user_set_non_blocking | internal_non_blocking` 중 하나만 서 있으면
  되며, `non_blocking(true)`는 fd의 `O_NONBLOCK`도 함께 보장한다.
- speculative sync write 기본값(`tcp_transport.cpp:87`)과 byte-HWM 설계
  (pipe-local byte credit, decoder credit 예약 등)는 **그대로 유지**된다.
- 주석과 실제 API가 일치하게 되어 원저자 의도를 복원한다.

## 5. 검증

### 5.1 공식 smoke (`ZLINK_CORE_SOURCE=local`)

release asset은 immutable하므로 로컬 빌드(`core/build`)로 검증했다.

```bash
PERF_FAIL_FAST=1 bash bindings/c/perf/run_benchmarks.sh \
  --pattern PAIR --transports tcp --msg-sizes 64 --duration 1 --runs 1 \
  --results-tag tcp-fix-final-20260823
```

```
META,core_source,local
META,core_runtime,/home/hep7hep7/project/zlink/core/build/lib/libzlink.so.0.12.0
RESULT,current,PAIR,tcp,64,throughput,2563754.000
RESULT,current,PAIR,tcp,64,bandwidth,164.080
RESULT,current,PAIR,tcp,64,latency,52.184
RESULT,current,PAIR,tcp,64,latency_p95,54.769
RESULT,current,PAIR,tcp,64,latency_p99,55.779
- status: complete
- actual_result_lines: 5
```

결과 파일:
`bindings/c/perf/results/single/report/perf_c_single_linux_20260823_122902_tcp-fix-final-20260823.txt`

### 5.2 다른 transport / pattern

| 실행 | 결과 파일 | status |
|------|-----------|--------|
| PAIR / tcp | `..._20260823_122629_tcp-fix-verify-20260823.txt` | complete |
| PAIR / inproc + ipc | `..._20260823_122642_tcp-fix-verify-inproc-ipc-20260823.txt` | complete |
| ROUTER_ROUTER / tcp | `..._20260823_122904_tcp-fix-router-20260823.txt` | complete |

`inproc` `latency,0.118` / `ipc` `latency,0.069`로 수정 전과 동등하다
(비TCP 경로 무영향 확인).

### 5.3 Core unittest / integration test

`core/build-tests`(BUILD_TESTS=ON, Release) 재빌드 후 변경 파일과 관련된
subset 실행:

```bash
ctest -R "tcp|ipc|monitor|pipe|hwm|control_runtime|mailbox|pair|router|reqrep" \
  --output-on-failure -j4 --timeout 180
```

18개 중 16개 통과. 통과 항목에는
`unittest_pipe_byte_charge`, `unittest_auto_hwm_policy`,
`unittest_control_runtime`, `unittest_ypipe`, `unittest_ipc_address`,
`test_retained_hwm_credit`, `test_monitor_enhanced`, `test_router_mandatory`,
`test_probe_router`, `test_router_auto_id_format` 등이 포함된다.

실패 2건은 **수정과 무관한 기존 실패**다. 수정을 `git stash`로 잠시 제거하고
`core/build-tests`를 다시 빌드해 같은 2건을 재현·확인했다.

| 테스트 | 수정 전(HEAD) | 수정 후 |
|--------|---------------|---------|
| `test_router_concurrent_routed_recv` | Failed | Failed (동일) |
| `test_router_mandatory_hwm` | Timeout | Subprocess aborted (`test_routed_send_ready_isolated_by_exact_target_and_terminal_cause:FAIL: Expected 0 Was 204`) |

`test_router_mandatory_hwm`은 수정 전에는 hang(Timeout)했고 수정 후에는 기존
assertion으로 즉시 실패한다. 같은 근본 실패가 §3의 deadlock에 가려 timeout으로
보이던 것이 드러난 것이며, 회귀가 아니라 **가시화**다. 두 항목 모두 별도
후속 과제다.

## 6. 남은 관측: 기본 sync write의 성능 특성

deadlock은 사라졌지만, PAIR/tcp/64B에서 기본 sync write는 비활성 대비 여전히
불리하다(각 3회, 같은 빌드):

| 설정 | throughput | latency (µs) |
|------|-----------|--------------|
| 기본(sync write ON) | 2.59–2.69 M/s | 51.5 / 52.7 / 53.7 |
| `ZLINK_ASIO_TCP_DISABLE_SYNC_WRITE=1` | 2.89–3.20 M/s | 0.106 / 0.115 / 0.140 |

원인은 `speculative_write()`의 루프(`asio_engine.cpp:1382`)가 STREAM 이외
socket type에서는 write budget(`asio_stream_spec_write_budget_bytes`)의 제한을
받지 않아, pipe가 빌 때까지 reactor로 복귀하지 않고 깊게 batching하기 때문으로
보인다. 정확성 문제는 아니므로 이번 수정 범위에서 제외했고, 설계 문서의
기본값 유지 방침과 충돌하므로 **후속 측정 과제로 남긴다**(일반 socket type에도
write budget을 적용할지 검토 필요).

## 7. 0.12.0 release asset 영향

`core/v0.12.0` release asset
(`/home/hep7hep7/.cache/zlink/core/0.12.0/linux-x64/lib/libzlink.so.0.12.0`,
sha256 `aaff83d34ca1833d566feb39141ab5b4660d22429a551067de71b4f8967ba365`)은
결함 commit `5d2bf1e84f` 기준으로 빌드됐고, 본 조사에서 로컬 빌드와 **동일하게
hang**함을 확인했다.

따라서 **이 release asset은 무효이며 재릴리스가 필요하다.** 근거:

- 결함은 특정 pattern이 아니라 TCP transport 데이터 평면 전체에 작용한다
  (PAIR·ROUTER_ROUTER 모두 재현).
- `ws`/`wss`/`tls`는 `supports_speculative_write()`를 `false`로 override하므로
  (`ws_transport.hpp:73`, `wss_transport.hpp:81`, `ssl_transport.hpp:66`)
  동기 write 경로를 타지 않는다. 영향 범위는 `tcp`(및 opt-in 시 `ipc`)다.
- 다만 `tcp`는 bindings 성능 측정의 기준 transport이므로 실사용 불가 상태다.
- 수정 없이는 어떤 TCP 부하도 peer의 수신 window가 찰 때 영구 정지한다.

수정 반영 후 `core/v0.12.0`을 재빌드·재게시하고, `release-provenance.txt` /
`core-package-provenance.json`의 sha256을 갱신해야 한다. 그 전까지 bindings
0.12.0 perf 본 측정은 release runtime으로 진행할 수 없다.

## 8. 작업 범위/보호 사항

- commit·push는 수행하지 않았다.
- 조사용 임시 계측(`asio_engine.cpp`, `asio_poller.cpp`, `mailbox.cpp`,
  `socket_base_monitor.cpp`, `socket_lifecycle_runtime.cpp`,
  `perf_single_monitor.hpp`)은 모두 원복했다. `git diff` 상 core 변경은
  §4.1의 2개 파일뿐이다.
- 회귀 구간 확인용 worktree(`fdcaca94b4`)는 제거했다.
- 기존 tree의 다른 변경(C++/dotnet/java/node/rust perf runner 등)은 건드리지
  않았다.
