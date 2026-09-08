# Single REQREP 러너 C 모델 정합 — Python (D-BP40 admission window)

- 일자: 2026-09-08
- branch: `main` (작업 시작 시 `cf05e74a93`)
- Core source: `release`
- Core package prefix: `/home/hep7/.cache/zlink/core-pinned/0.17.3`
  (`lib/libzlink.so.0.17.3`, SHA-256 `4779e33380356d469ec3b0c30b12bb6c828c8710a4cdd71b44d0669cafa1a1af`)
- 변경 파일: `bindings/python/perf/single/perf_single_reqrep.py` (그 외 없음.
  Multi 파일과 binding 라이브러리 `bindings/python/src`는 건드리지 않았다)

## 1. 무엇이 잘못돼 있었나 (원인, 파일:줄)

수정 전 `bindings/python/perf/single/perf_single_reqrep.py`의 requester는 세 곳에서
C 기준(`bindings/c/perf/single/common/perf_single_reqrep.hpp` `run_request_phase`
396-443)과 어긋났다.

| # | 위치(수정 전) | 증상 |
|---|---|---|
| 1 | `perf_single_reqrep.py:157-172` | turn마다 **1건 제출 → `wait(events, 0)` → `await asyncio.sleep(0)`**. reply를 기다리지는 않지만 **제출을 멈추는 경계가 없다**. 유일한 제동이 event-loop turn 비용이라, 작은 크기에서는 제출이 turn 비용에 묶여 깊이 8에 머물고(§4) 큰 크기에서는 반대로 깊이 107~136·latency 92~140 ms까지 밀린다(수정 전 Node와 같은 모양, D-BP40). |
| 2 | `perf_single_reqrep.py:135` | `latency.add(float(now_ns - header["sent_ts_ns"]) / 2.0)` — 왕복 시간을 **2로 나눠** 담았다. 정책 §1.1은 request-reply latency를 "request 제출부터 reply completion까지의 왕복"으로 정의하고, C(`record_request_completion` 192-198)와 C++·.NET·Node는 왕복 전체를 담는다. 즉 Python의 REQREP latency는 지금까지 **절반으로 보고**되고 있었다. |
| 3 | `perf_single_reqrep.py:165-177`, `201-216` | 측정 구간의 진행이 `asyncio.create_task` + `await asyncio.sleep(0)` + 전용 thread의 private event loop로 이루어진다. §1.1.5의 판정 기준(진행 주체 = 러너가 만든 전용 OS thread)은 만족하지만, 이번 지시가 금지한 event-loop yield 자체는 남는다. **공개 API로 제거할 수 없다(§5).** |

`asyncio.sleep(0)` 자체는 D-BP40이 지적한 손실의 원인이 아니다. 손실의 원인은 1번,
즉 admission 경계가 없다는 것이다.

## 2. 차이 표 (C 기준)

| 항목 | C | Python (before → after) |
|---|---|---|
| 제출 gate | `BACKPRESSURED`까지 연속 제출 | 1건/turn, 경계 없음 → **admission window까지 연속 제출** |
| 미완료 상한 | 없음(Core admission 경계) | 실효 무제한 → **applied SNDHWM bytes ÷ wire size** |
| 제출 중 progress | 64건마다 `poll_completion_once(0)` | 매 건 `wait(0)` → **64건마다 `wait(0)` + turn 1회** |
| 경계 도달 시 | `poll(50)` 블로킹 | 없음(항상 `wait(0)`) → **`wait(0 if 제출했으면 else 50)`** |
| reply drain | `zlink_completion_recv`를 `NO_DATA`까지 | `Poller.wait` 내부 `CompletionOwner.drain` 전량 + future settle ✓ |
| 진행 주체 | 전용 requester OS thread | 전용 `threading.Thread` + 그 thread만 도는 private loop (§5) |
| timestamp | 제출 직전 stamp / completion 시각 | 동일 ✓ |
| latency | 왕복 전체 | **/2.0 → 왕복 전체** |
| active 유효 조건 | `completed_at < deadline` + run/phase/size | 동일 ✓ |
| throughput | `completed / duration` | 동일 ✓ |
| request timeout knob | `PERF_SINGLE_REQREP_TIMEOUT_MS`(200) | 동일 ✓ |
| completion drain bound | `PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS`(10000) | 동일 ✓ |

## 3. 수정 요지 (diff)

정책 §1.1.3(D-BP40): awaitable terminal 러너는 미완료 집합을 **Core가 그 소켓에 적용한
SNDHWM bytes ÷ 메시지 wire size**로 묶는다. 고정 숫자 상한이 아니라 C가
`BACKPRESSURED`를 받는 admission 창 자체다.

- `_applied_send_hwm_bytes(monitor)` / `_admission_window_requests(applied, socket_sndhwm, wire_size)`
  추가. 값의 출처는 requester 소켓의 auto-HWM snapshot
  (`monitor.status().auto_hwm_applied_sndhwm_bytes`)이고, snapshot이 0일 때만
  `requester.options.send_high_water_mark`로 되읽는다. 둘 다 0이면 상한을 임의로 정하지
  않고 `RuntimeError`로 실패시킨다. 창은 CONNECTION_READY 게이트를 통과한 직후,
  requester monitor가 열려 있는 동안 1회 읽는다.
- requester turn을 C 모양으로 바꿨다.
  ```python
  while time.perf_counter() < active_end and not failures:
      submitted_any = False; submitted_since_progress = 0
      while (time.perf_counter() < active_end and not failures
             and len(pending) < admission_window):
          submit_one()                       # await 하지 않는다
          submitted_any = True
          if (submitted_since_progress := submitted_since_progress + 1) >= 64:
              submitted_since_progress = 0
              completion_poller.wait(completion_events, 0); await asyncio.sleep(0)
      completion_poller.wait(completion_events, 0 if submitted_any else 50)
      await asyncio.sleep(0)
  ```
  (실제 코드는 walrus 없이 같은 구조다.) 창이 찼고 이번 turn에 제출한 것이 없을 때만
  50 ms 블로킹 progress를 한다 — C가 backpressure 뒤에 하는 `poll(50)`과 같은 자리다.
- 종료 drain도 busy loop가 되지 않게 같은 규칙을 쓴다: 미완료 수가 줄면 `wait(0)`,
  한 turn 동안 하나도 줄지 않으면 `wait(50)`. 새 request는 제출하지 않고 bound는 그대로
  `PERF_SINGLE_REQREP_DRAIN_TIMEOUT_MS`다.
- latency `/2.0` 제거(왕복 전체). C·C++·.NET·Node와 같은 정의가 됐다.
- `PERF_DEBUG=1`일 때만 `single_reqrep_debug:admission_window=...:wire_size=...`를
  stderr로 남긴다(기존 공통 knob, 새 env 아님). RESULT 출력 형식은 그대로다.

넣지 않은 것: 고정 숫자 상한, 새 환경 변수, 새 timeout, sleep, client 수·크기·HWM 변경,
binding 라이브러리 변경.

### 3.1 창 크기 (applied SNDHWM 1,048,576 B 기준)

| size(B) | 64 | 256 | 1024 | 65536 | 131072 | 262144 |
|---|---|---|---|---|---|---|
| admission window | 16384 | 4096 | 1024 | 16 | 8 | 4 |

.NET·Node와 같은 값이다(같은 auto-HWM balanced 프로파일, 같은 wire size 정의).

## 4. 깊이(throughput × latency) — before / after

Little's law로 실효 미완료 깊이 = throughput(ops/s) × latency(s)를 계산했다.
**before의 report latency는 왕복의 절반이므로**(§1의 2번) 아래 before 깊이는
보고값을 2배로 되돌린 실제 왕복 기준이다. after와 C는 보고값 그대로다.

**DEALER_ROUTER_REQREP (tcp, 5 s, 1 run)**

| size | C tp | C lat(ms) | C 깊이 | py before tp | lat(ms, 왕복) | 깊이 | py after tp | lat(ms) | 깊이 | after %C |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 663,767.6 | 5.252 | 3,486 | 10,702.4 | 0.812 | 8.7 | 22,322.0 | 5.917 | **132.1** | 3.4% |
| 256 | (FAIL) | — | — | 10,795.6 | 0.745 | 8.0 | 22,466.8 | 5.940 | **133.5** | — |
| 1024 | 702,512.6 | 1.053 | 740 | 12,205.8 | 0.674 | 8.2 | 20,977.2 | 6.278 | **131.7** | 3.0% |
| 65536 | 9,946.0 | 0.195 | 1.94 | 1,216.6 | 92.271 | 112.3 | 3,280.4 | 4.691 | **15.4** | 33.0% |
| 131072 | 7,967.4 | 0.243 | 1.94 | 1,054.2 | 120.908 | 127.5 | 3,449.2 | 2.133 | **7.4** | 43.3% |
| 262144 | 6,429.8 | 0.300 | 1.93 | 974.0 | 139.811 | 136.2 | 3,032.2 | 1.104 | **3.3** | 47.2% |

**ROUTER_ROUTER_REQREP**

| size | C tp | C lat(ms) | C 깊이 | py before tp | lat(ms, 왕복) | 깊이 | py after tp | lat(ms) | 깊이 | after %C |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 64 | 840,318.2 | 0.157 | 131.9 | 11,883.2 | 0.655 | 7.8 | 21,987.4 | 5.909 | **129.9** | 2.6% |
| 256 | 790,297.6 | 0.215 | 169.5 | 10,245.2 | 0.734 | 7.5 | 22,382.2 | 5.627 | **126.0** | 2.8% |
| 1024 | 652,905.0 | 6.307 | 4,118 | 11,431.6 | 0.694 | 7.9 | 21,766.8 | 5.761 | **125.4** | 3.3% |
| 65536 | 10,179.8 | 0.191 | 1.94 | 1,163.2 | 91.792 | 106.8 | 3,366.6 | 4.582 | **15.4** | 33.1% |
| 131072 | 8,468.0 | 0.229 | 1.94 | 986.8 | 131.367 | 129.6 | 3,526.8 | 2.088 | **7.4** | 41.6% |
| 262144 | 6,600.6 | 0.292 | 1.93 | 941.6 | 132.917 | 125.2 | 3,251.4 | 1.018 | **3.3** | 49.3% |

읽는 법:

- **작은 크기**: before는 turn당 1건 제출 + event-loop turn 비용이 제동이라 깊이가
  7.5~8.7에 머물렀다. after는 창(16384/4096/1024) 안에서 연속 제출하므로 깊이가
  126~133이 됐다. C의 RR 64 B 깊이 131.9와 **같은 자릿수**다(D-BP40의 판정 지표).
  처리량은 **1.7~2.2배**로 올랐다(예: RR 256 B 10,245 → 22,382).
- **큰 크기**: before는 반대로 어긋나 있었다 — 경계가 없어 깊이 107~136, latency
  92~140 ms까지 밀렸고 deadline 뒤 완료는 집계에서 빠져 처리량이 ~1,000 ops/s로
  떨어졌다. after는 창(16/8/4)이 그대로 상한이 되어 깊이 3.3~15.4, latency 1.0~4.7 ms,
  처리량 **2.7~3.6배**(974 → 3,032, 941.6 → 3,251)다. C 깊이 1.93~1.94보다는 조금 크다
  — 창이 C의 실측 깊이보다 크기 때문이며 .NET·Node와 같은 성질이다.
- C 대비 비율은 작은 크기 2.6~3.4%, 큰 크기 33~49%다. 남은 격차는 깊이가 아니라
  Python per-op 비용이다(비용 지도 `2026-09-08-python-cost-map.ko.md` 참조).
- C `DEALER_ROUTER_REQREP tcp 256 B` 셀은 이번에도 C 자체 종료 결함으로 실패했다
  (`retained=1 replier_fatal=1`, `perf_c_single_linux_20260908_161418_sgfix-python2.txt`
  `## Failures`). Python과 무관하며 다른 언어 담당 sub-agent도 같은 셀에서 같은 실패를
  봤다.

### 4.1 report 경로

| 구분 | 경로 | status |
|---|---|---|
| before (수정 전 러너) | `bindings/python/perf/results/single/report/perf_python_single_linux_20260908_145739_sgfix-python-before.txt` | complete |
| after C (짝) | `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_161418_sgfix-python2.txt` | partial (C 256 B 셀 실패) |
| after Python | `bindings/python/perf/results/single/report/perf_python_single_linux_20260908_161651_sgfix-python2.txt` | complete |
| 회귀 PAIR 64 B | `bindings/python/perf/results/single/report/perf_python_single_linux_20260908_152957_sgfix-python-pair.txt` | complete |

before/after 모두 같은 고정 Core 런타임이다
(`0.17.3/lib/libzlink.so.0.17.3`, SHA-256 `4779e333...a1af`).

## 5. binding 한계 — Python 공개 request terminal로는 event loop를 뺄 수 없다

지시대로 "동기 API로 계속 제출"이 가능한지부터 확인했다. **불가능하다.**
`bindings/python/src/zlink/contracts/sockets/operations.py:44-60`의 `RequestOp`에는
terminal이 둘뿐이다.

| 공개 terminal | 구현 | Single 정책 결과 |
|---|---|---|
| `submit()` | `_RequestOp.submit` → `CompletionOwner.submit_request` (`_runtime/messaging/routed_async.py:1166`, `async def`) | admission과 reply를 합친 **asyncio awaitable**. 본문이 시작하자마자 `asyncio.get_running_loop()`을 부른다(native 확장 경로·순수 Python 경로 모두). running loop 없이는 시작조차 못 한다 |
| `submit_sync()` | `CompletionOwner.submit_request_sync` (`:1223`) → `_wait_request` (`:1246`) | **reply까지 블로킹**. 연속 제출이 끊겨 in-flight 1 RTT loop가 된다 |

"awaitable을 전용 thread에서 블로킹 없이 poll한다"도 성립하지 않는다. 실측으로
확인했다(고정 prefix 0.17.3 런타임, DEALER requester):

```python
coro = requester.request().messages(payload, b"").timeout(0.2).submit()
coro.send(None)   # -> RuntimeError: no running event loop
```

반환값은 coroutine이고, 러너가 직접 `send(None)`으로 밀면 binding 내부의
`asyncio.get_running_loop()`에서 즉시 실패한다. 설령 loop 객체를 만들어 넘겨도
`_CompletionEntry.wait_async`가 `await asyncio.shield(self.future)`이고
(`routed_async.py:338-350`), future의 완료 콜백은 `loop.call_soon`으로 큐에 들어가므로
**loop의 ready queue를 도는 주체가 반드시 있어야 한다**. 즉 awaitable을 "poll"하는
형태로 바꿀 여지가 공개 계약 안에 없다.

그래서 이번 러너는 §1.1.5의 판정 기준("측정 구간에서 그 역할의 진행을 실제로 밀고 있는
것이 러너가 만든 전용 OS thread인가")에 맞춰 **러너가 만든 private loop를 전용 requester
thread 안에서만 돌린다**. 제출·completion drain·settle이 모두 그 thread에서 일어나고,
공유 pool·런타임 executor·다른 thread의 loop는 관여하지 않는다. 이는 정책이 Node
worker loop에 명시적으로 허용한 것과 같은 형태이지만, Python asyncio에는 그런 명시
허용 문구가 없다. **감독자 판단이 필요한 지점이다.**

### 5.1 무엇이 있으면 해소되는가 (계약 제안, 이번 작업 범위 밖)

binding 내부에는 이미 필요한 것이 다 있다. `CompletionOwner.drain()`은 공개 poller가
POLLCOMPLETION 소유자일 때 **호출 thread에서 동기적으로** entry를 settle시키고
(`_runtime/eventing/poller.py:222`), `_CompletionEntry`에는 `settled` 속성과
블로킹 없는 결과 인수 경로(`wait_request`)가 이미 있다(`routed_async.py:352-365`).
빠진 것은 "admission만 마치고 reply 전에 돌아오는 공개 terminal"과 "그 결과를
non-blocking으로 관측하는 공개 handle" 하나뿐이다(예: `submit_nowait()` →
`done`/`result()`를 가진 handle). 그것이 생기면 Python Single REQREP은 asyncio 없이
C와 같은 turn을 그대로 표현할 수 있다.

## 6. 검증

| 항목 | 결과 |
|---|---|
| `2-1788845119-15408-...-sgfix-python-before` (before, python DR·RR REQREP tcp 6 sizes 1-run) | rc=0, status=complete |
| `2-1788845124-16617-...-sgfix-python` (짝 C→python, `&&`) | **rc=1** — C 256 B 셀 실패로 `&&`가 python leg을 막았다. 러너 코드와 무관 |
| `2-1788848896-84897-...-sgfix-python2` (짝 C→python, C rc를 보고만 하고 gate하지 않음) | rc=0 (`PAIRED_RC c=1 python=0`), python status=complete |
| `2-1788845128-16741-...-sgfix-python-pair` (회귀 PAIR 64 B 1-run) | rc=0, 155,165 msg/s — 같은 날 0.17.3 다른 run 159,753 msg/s와 같은 수준(PAIR는 이 파일을 쓰지 않는다) |
| `pytest` (perf) | `tests/test_perf_runner.py`, `tests/test_perf_monitor_hwm.py` 전부 통과. `tests/test_perf_multi_runner.py` 2건 실패(`PYTHON_MULTI_DEFAULT_IO_THREADS 4 != 1`, multi client 기본값) — **수정 전부터 있던 Multi 쪽 실패**이며 이 작업과 무관하다(`bindings/python`에서 수정된 파일은 이 러너 하나뿐이다) |

측정 중 다른 sub-agent가 같은 디렉터리의 one-way 파일(`perf_common.py`의
`run_one_way_receiver`, `perf_pair.py` 등)을 고쳤다. REQREP 러너는 그 함수를 쓰지
않으므로 before/after 비교에는 영향이 없다.

before 측정은 수정본을 작업 트리에서 잠시 들어내지 않기 위해, 티켓 스크립트 자신이
실행 직전 수정 전 파일을 넣고 종료 시(trap) 수정본을 되돌리는 방식으로 냈다. 티켓
종료 후 작업 트리 파일이 수정본과 byte 동일함을 확인했다.

## 7. 이번에 고치지 않은 잔여 차이

- Python **Multi** reqrep/one-way client 3개가 latency를 `/2.0`으로 담는다
  (`perf/multi/perf_multi_reqrep_client.py:145`,
  `perf_multi_dealer_router_client.py:113`, `perf_multi_router_router_client.py:112`).
  이번 과제 범위(Multi 파일 금지)라 손대지 않았다. Multi 정책의 latency 정의와 대조가
  필요하다.
- Go single REQREP도 같은 `/2.0`을 쓴다(`bindings/go/perf/single/perf_reqrep.go:88`,
  dotnet-node 로그 §7이 이미 지적).
- Python single runner는 `## Auto-HWM Detail` 표를 빈 채로 낸다
  (`perf/single/run_benchmarks.py:539`가 `rows` 없이 호출). 창 계산의 입력인 applied SNDHWM이
  리포트에 남지 않으므로, 이번에는 `PERF_DEBUG=1` stderr 줄로만 확인할 수 있다.
  (C·.NET 리포트에는 표가 있다.)
