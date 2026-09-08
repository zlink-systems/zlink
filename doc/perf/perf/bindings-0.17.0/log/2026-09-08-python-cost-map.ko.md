# Python 메시지당 비용 지도 — Single 선행 조사

감독자 검토용 조사 기록이다. 결론은 두 가지다.

1. 유효한 Single `DEALER_DEALER / tcp / 64 B`에서 Python은 C의 10.72%이며,
   잔여 격차는 4,754.1 ns/message다. 계측 run에서는 완료 메시지당 Python 함수
   19.33회와 Python→binding C 확장 경계 5.04회를 관측했다.
2. Single `DEALER_ROUTER_REQREP` requester는 측정 구간에서 `asyncio` event loop와
   `sleep(0)` yield로 진행한다. `PERF_SINGLE_TEST_POLICY.md` §1.1.2~§1.1.3과 이번
   작업의 명시 조건을 어긴다. 공개 `RequestOp`에는 async terminal과 reply까지 막는
   synchronous terminal만 있어, 공개 API를 유지한 runner 수정만으로는 정책의
   연속 admission·직접 completion drain을 표현할 수 없다.

따라서 REQREP를 유효한 Single 비용 지도에 넣지 않았고, 두 pattern 공통의 20% 지배
항목도 확정하지 않았다. 추측에 근거한 pass 1 변경은 하지 않았다.

## 조건과 원자료

- Core source: `release`
- Core package prefix:
  `/home/hep7/.cache/zlink/core-pinned/0.17.3-alpha`
- 실제 runtime:
  `/home/hep7/.cache/zlink/core-pinned/0.17.3-alpha/lib/libzlink.so.0.17.2`
- runtime SHA-256:
  `c4f4da4bb06573837d5945ccc489d6e940610a5e371885c154fd17409112fb39`
- suite/cell: Single, `DEALER_DEALER / tcp / 64 B / 2 parts / 1 run /
  duration 5 s / I/O thread 1 / auto-HWM balanced`
- 짝 측정 ticket:
  `2-1788843007-30624-codex-Python_Single_DD_cost_map_paired_before_`
  (rc=0)
- C report:
  `bindings/c/perf/results/single/report/perf_c_single_linux_20260908_135045_python-cost-map-before-c.txt`
- Python report:
  `bindings/python/perf/results/single/report/perf_python_single_linux_20260908_135056_python-cost-map-before-python.txt`
- 호출 계수 ticket:
  `2-1788843200-59257-codex-Python_Single_DD_sys.setprofile_calls_an`
  (rc=0)
- 호출 계수 원자료:
  `.artifacts/perf-queue/log/2-1788843200-59257-codex-Python_Single_DD_sys.setprofile_calls_an.log`
  (`tcp / 64 B / 2 parts / duration 1 s`, 완료 28,344건)

짝 측정 도중 저장소 `main`이 `5c62a8d2c3`에서 0.17.3 표기를 사용하는 commit으로
갱신됐다. 위 rc=0 짝 측정은 갱신 전 같은 시각에 C→Python 순서로 실행됐고 두 report의
runtime 경로와 SHA가 같다. 계획서의 0.17.2 측정값은 사용하지 않았다.

## 1. 비용 지도

### 1.1 비계측 wall time

| 항목 | C | Python | Python - C | C 대비 Python |
|---|---:|---:|---:|---:|
| throughput | 1,751,735.6 msg/s | 187,795.8 msg/s | -1,563,939.8 msg/s | 10.72% |
| 메시지당 wall time | 570.9 ns | 5,325.0 ns | **4,754.1 ns** | 9.33배 |

`1e9 / throughput`으로 환산했다. 이 값은 sender와 receiver가 동시에 진행하는 Single
cell의 end-to-end wall time이므로 두 thread의 CPU 시간을 합한 값이 아니다.

### 1.2 메시지당 작업

| 항목 | C / message | Python / message | C 대비 추가 | ns·격차 비율 |
|---|---:|---:|---:|---:|
| Core send/recv part 호출 | send 2 + recv 2 | send 2 + recv 2 | 0 | 공통 native 작업. C 전체 570.9 ns 안에 포함 |
| Python 함수 호출 전체 | 해당 없음 | **19.33회** | +19.33회 | CPU 귀속 미측정 |
| ↳ binding Python 함수 | 해당 없음 | **약 11.17회** | +11.17회 | CPU 귀속 미측정 |
| ↳ runner·stdlib Python 함수 | 해당 없음 | **약 8.16회** | +8.16회 | CPU 귀속 미측정 |
| Python→binding C 확장 경계 | 0 | **5.04회** | +5.04회 | CPU 귀속 미측정 |
| ↳ `recv_into` | 0 | 1.033회 | +1.033회 | 931회의 NO_DATA/종료 확인 포함 |
| ↳ `materialize_parts` | 0 | 1.000회 | +1.000회 | 2-part native storage 구성 |
| ↳ `submit_storage` | 0 | 1.000회 | +1.000회 | 내부에서 Core send part 2회 |
| ↳ received part `to_bytes` | 0 | 2.001회 | +2.001회 | payload와 empty tail 각각 한 번 |
| Python allocator 요청 | 0 | 미측정 | 미측정 | allocation ticket 실행 불가 |
| asyncio turn/yield | 0 | **0** | 0 | DD는 async 경로를 쓰지 않음 |
| GIL release/reacquire 지점 | 0 | send part당 1, 즉 시도당 2 | +약 2 | 실제 thread handoff 수는 미측정 |
| OS context switch | 미측정 | 미측정 | 미측정 | `perf` 미설치, 대체 ticket 실행 전 queue 중단 |

함수 계수는 `sys.setprofile`을 sender의 `send_loop`와 receiver의
`run_one_way_receiver` 진입부터 반환까지에만 켠 결과다. profiler 부담으로 처리량은
28,344 msg/s까지 낮아졌으므로 그 run의 시간은 성능 판정에 쓰지 않고 호출 횟수만 썼다.
sender는 28,358회 시도했고 receiver는 28,344건을 완료했다.

남은 binding Python 호출은 다음과 같다.

- sender 시도당 9회:
  `_MessageSocket.send`, `_ManagedSendOp.__init__`, `_ManagedSendOp.messages`,
  `_ManagedSendOp.submit_sync`, `_payload_or_raise`,
  `CompletionOwner.submit_send_sync`, `_materialize_native_parts`, `_submit_parts`,
  socket `_handle` property
- receiver 완료당 2회와 sparse poll 경로:
  `recv_into`, `to_bytes_list`; NO_DATA 때 `NativePoller.wait`, `ffi.lib`와 검증 함수
- runner hot path: `stamp_payload`, `send_routed_sync`, `measurement_parts`,
  `measurement_part_count`, `measurement_payload`, latency sampler `add`

이는 pass 2(`2ad52c4e11`) 뒤에도 public builder와 runner 계층의 호출이 남아 있음을
보여 준다. 그러나 호출 횟수만으로 각 항목의 4,754.1 ns 격차 기여도를 정할 수는 없다.

### 1.3 REQREP runner 정합 차단

현재 `bindings/python/perf/single/perf_single_reqrep.py`의 측정 경로는 다음과 같다.

- 86~88행: public async `RequestOp.submit()`을 await한다.
- 91~198행: `_run_requester_async`가 request coroutine과 pending task를 관리한다.
- 165~177행: 매 submit/drain turn에 `asyncio.create_task()`와
  `await asyncio.sleep(0)`을 실행한다.
- 201~216행: 전용 thread가 `asyncio.new_event_loop()`와
  `run_until_complete()`로 위 coroutine을 진행한다.

전용 OS thread 안에 private loop가 있다는 사실만으로 정합이 되지는 않는다. 정책
§1.1.2는 Single requester가 coroutine·event loop 없이 admission과 completion을 직접
교대로 진행하도록 요구하고, §1.1.3은 awaitable도 event loop에 맡기지 않도록 요구한다.
이번 작업 지시도 측정 구간의 async/event-loop yield를 명시적으로 금지했다.

공개 계약은 `bindings/python/src/zlink/contracts/sockets/operations.py:55-60`에서 다음 두
terminal만 제공한다.

| 공개 terminal | 동작 | Single 정책 결과 |
|---|---|---|
| `submit()` | admission backpressure와 reply를 합친 asyncio awaitable | event-loop 진행이 필요해 위반 |
| `submit_sync()` | request terminal 결과까지 block | 연속 admission이 끊겨 in-flight 1 RTT loop가 되므로 위반 |

binding 내부에는 DONTWAIT admission과 completion owner가 있지만 이를 runner가 직접
진행시키는 공개 terminal은 없다. private binding helper를 runner에 노출하면 공개 사용 비용을
재지 않는 별도 우회 경로가 되고, 새 public terminal은 계약 변경이다. 따라서 이 항목은
**계약**으로 표시하고 이번 작업에서 건드리지 않았다.

## 2. 지배 항목 판정

**20% 이상 지배 항목을 확정하지 못했다.** DD에서는 4,754.1 ns/message의 격차와 호출
개수를 확정했지만, 함수·경계·allocation·handoff별 ns를 얻기 전에는 어느 호출 묶음이
격차의 20% 이상인지 판정할 수 없다. DR REQREP는 runner가 정책을 위반하므로 그 profile을
DD와 합쳐 공통 지배 항목의 근거로 쓸 수 없다.

## 3. pass 1

구현하지 않았다. 지배 항목이 입증되지 않았고, REQREP 정합은 공개 API 계약에 묶여 있다.
따라서 before/after 및 Multi 확인 report는 없다. timeout·sleep·client 수·message size·HWM·
상한은 바꾸지 않았고 in-flight cap이나 오류를 삼키는 경로도 추가하지 않았다.

## 4. 남은 격차의 성격

| 구분 | 상태 | 근거 |
|---|---|---|
| 계약 | **확인** | Python request public terminal 두 개로는 event-loop 없는 연속 admission + direct completion drain을 표현할 수 없음 |
| runtime | **미확인** | DD의 Python 함수 19.33회, C 확장 경계 5.04회, GIL reacquire 지점 약 2회가 후보지만 ns 귀속 전 |
| allocation | **미확인** | PEP 445 allocator counter ticket이 repository VERSION 전환 뒤 prefix manifest 검사에서 실패 |
| thread handoff | **미확인** | `perf`가 설치되지 않아 최초 ticket rc=127; `getrusage` 대체 ticket 전에 queue runner 종료 |
| asyncio | DD 0, REQREP 측정 불가 | REQREP의 event-loop turn 자체가 Single 정책 위반 |

## 5. 중단된 ticket과 재개 조건

작업 중 `main`의 repository VERSION이 0.17.2에서 0.17.3으로 바뀌었다. 고정 alpha prefix의
provenance version은 0.17.2라 이후 `local_core_runtime.sh`가 mismatch를 거절했다. 이어 perf
queue runner가 종료됐고, `perf-ticket.sh`는 runner를 감독자가 띄워야 한다고 rc=71을
반환했다.

| ticket | rc | 결과 |
|---|---:|---|
| `2-1788843279-67778-codex-Python_Single_DD_allocation_count_bytes_` | 1 | repository/prefix version mismatch |
| `2-1788843852-29620-codex-Python_Single_DD_allocation_count_bytes_` | 1 | 같은 mismatch; allow flag가 prefix manifest 비교에는 적용되지 않음 |
| `2-1788843354-71048-codex-Python_Single_DD_C_versus_Python_context` | 127 | `perf: command not found` |
| `2-1788843413-74714-codex-Python_Single_DD_cProfile_CPU_attributio` | 1 | repository/prefix version mismatch |
| `2-1788843557-91217-codex-Python_Single_DD_per-thread_CPU_attribut` | 1 | repository/prefix version mismatch |
| `2-1788843893-32086-codex-Python_Single_DD_allocation_count_bytes_` | 대기 | 고정 runtime 직접 지정으로 재제출했으나 runner 부재로 caller rc=71; pending ticket은 보존됨 |

재개하려면 감독자가 queue runner를 다시 시작한 뒤, package prefix와 실제 runtime을 그대로
두고 `ZLINK_CORE_RELEASE_VERSION=0.17.2`를 명시하거나 실제 runtime 경로를 계측 process에
직접 전달해야 한다. allocation·CPU·`getrusage`를 채운 다음에만 20% 판정과 pass 1을 진행할
수 있다.
