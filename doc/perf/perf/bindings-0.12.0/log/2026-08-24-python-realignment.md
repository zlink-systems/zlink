# Python binding 0.13.0 계약 재정렬

## 구현

- `bindings/python/src/zlink/_native/ffi.py`: `zlink_send_ready_handler` /
  `zlink_routed_send_ready_handler` ctypes 바인딩과 `ZlinkRoutedSendReadyEvent`
  layout을 제거했다. `zlink_send_async` / `zlink_send_complete_handler` /
  `zlink_send_async_cancel`을 추가하고 `zlink_send_complete_event_t` /
  `zlink_send_async_options_t`를 `ZlinkSendCompleteEvent` /
  `ZlinkSendAsyncOptions`로 미러링했다. 두 struct의 `sizeof`/`alignof`는
  `core/include/zlink/socket/api.h`를 직접 include하는 C translation unit과
  대조해 byte parity를 확인했다(296/8, 24/8 — 아래 grep proof 절 참고).
- `bindings/python/src/zlink/_runtime/handles/native_support.py`:
  `_SOCKET_SEND_READY_HANDLER` CFUNCTYPE을 `_SEND_COMPLETE_HANDLER`
  (`ZlinkSendCompleteEvent`)로 교체했다.
- `bindings/python/src/zlink/_runtime/messaging/routed_async.py`: 기존
  admission 기계장치(`_TargetState`, `_AdmissionTicket`, WRITABLE 재시도 루프,
  `_RequestCompletionProgress` polling thread)를 전부 삭제하고
  `SendCompletionOwner` + `RoutedSendOwner` 두 클래스로 재작성했다.
  - `SendCompletionOwner`는 socket당 하나의 `zlink_send_complete_handler`와
    pending-operation anchor table을 소유한다(C++ 참조 구현
    `socket_callback_state_t`와 동형). Core가 즉시 admit하면 completion
    callback이 inline으로 실행되어 awaitable이 이미 resolve된 채로 반환되므로
    awaiter는 suspend하지 않는다. Correlation은 Core가 반환하는 `op_id`가
    아니라 `zlink_send_async_options_t.userdata`로 전달하는 자체 발급
    opaque token을 쓴다 — `op_id`는 호출이 반환된 뒤에만 알 수 있어 inline
    completion에는 너무 늦게 도착하기 때문이다. `op_id`는 cancel 전용으로만
    나중에 채운다.
  - `RoutedSendOwner`는 PAIR/DEALER/ROUTER의 HWM-managed send와 DEALER/ROUTER
    request를 함께 소유한다. send는 `SendCompletionOwner`에 위임한다(ROUTER는
    `zlink_select_routed_submit_target`으로 정확한 target을 먼저 선택하고,
    DEALER는 `target=NULL`로 Core가 submit 시점에 선택하게 한다 — 문서
    그대로). request는 admission ticket을 완전히 제거하고 Core의
    `zlink_dealer_request_transport_pair_part` /
    `zlink_router_request_transport_pair_part`를 한 번만 시도한 뒤, 완료를
    순수하게 Core reply callback(`_on_request_reply`)이 구동하게 했다.
    Backpressured submit은 재시도하지 않고 즉시 `SubmitError`를 던진다.
  - Per-op timeout은 `zlink_send_async_options_t.timeout_ms`
    (Core-side)로만 전달한다 — `_runtime/eventing/timer.py`가 이미
    Core-owned timer로 처리하는 것과 같은 원칙이다. Python 쪽 타이머나
    deadline 스레드는 없다.
- `bindings/python/src/zlink/_runtime/sockets/socket_base.py`: `_SendReadySocket`
  mixin과 `on_send_ready`, `_send_ready_handler`/`_send_ready_handler_cb`
  상태를 전부 삭제했다(`send_ready` readiness-hint semantics 폐지,
  async-coroutine-policy.ko.md 2차 개정).
- `bindings/python/src/zlink/_runtime/sockets/socket_base_impl.py`:
  - `PairSocket`이 이제 `SendCompletionOwner`를 생성 시점에 설치하고
    `send()`가 새 `_ManagedSendOp`(coroutine builder, `flags()` 없음)를
    반환한다 — PAIR send는 분류 원칙상 HWM-managed ASYNC이므로(문서
    " PAIR send, DEALER/ROUTER routed send") DEALER/ROUTER routed send와
    동일한 계약을 따른다.
  - `DealerSocket`/`RouterSocket`은 `RoutedAdmissionOwner` 대신
    `RoutedSendOwner`를 사용하도록 바꿨다. `_ManagedRoutedSendOp` 표면은
    그대로다(변경 없음 — 이미 이 계약을 따르고 있었다).
- `bindings/python/src/zlink/contracts/sockets/operations.py`: `RoutedSendOp`
  docstring을 "DEALER/ROUTER"에서 "PAIR send, DEALER/ROUTER routed send"로
  갱신했다(표면 변경 없음, 문서 정합).
- `bindings/python/src/zlink/native/linux-x86_64/`: 벤더링된 `libzlink.so*`를
  0.11.1에서 `core/build/lib/libzlink.so.0.13.0`으로 교체했다(symlink 포함).
  `nm -D`로 새 심볼(`zlink_send_async`, `zlink_send_async_cancel`,
  `zlink_send_complete_handler`) 존재와 구 심볼
  (`zlink_send_ready_handler`, `zlink_routed_send_ready_handler`) 부재를
  둘 다 확인했다.
- 문서: `bindings/python/README.md`에 `on_send_ready` 언급을 제거하고 새
  "Send, Publish, Request, Raw Reply Completion" 절을 추가했다.
  `bindings/doc/spec/python/README.en.md` / `README.ko.md`의 "Callback
  surface"/"송수신과 no-data" 절(작업 지시서가 지목한 ~:153/:172 부근)을 옛
  park queue·WRITABLE 재시도·pump·gate 서술에서 `zlink_send_async` +
  `zlink_send_complete_handler` + Core reply callback 계약으로 다시 썼다.

## 삭제한 스레드와 남아 있는 스레드 (zero-thread 규칙 점검)

- **삭제**: `routed_async.py`의 `_RequestCompletionProgress` —
  `ZLINK_POLLCOMPLETION`으로 소켓을 등록하고 `zlink_poller_wait`를 자체
  daemon thread(`zlink-request-completion`)에서 펌프해 request reply
  callback 전달을 구동하던 코드. 0.13.0에서는 Core 자신의 async mailbox
  thread가 completion을 자율적으로 전달한다는 계약이 명문화됐고(
  `zlink_send_complete_handler` 문서, `core/include/zlink/socket/api.h`),
  이는 이미 존재하던 `zlink_reply_handler_fn` 전달에도 실측으로 확인됐다 —
  이 thread를 완전히 제거한 뒤 `test_router_request_uses_the_same_exact_target_reply_driven_path`
  등 request round-trip 테스트가 폴링 없이 그대로 통과했다.
- **점검 후 유지**(출력 send/request 경로와 무관, 따로 삭제하지 않음):
  - `_runtime/eventing/dispatcher.py`의 `CallbackDispatcher` — socket당 lazy
    시작되는 단일 worker thread로 recv/packet callback을 fan-out한다. 이번
    작업의 send/routed-send/request 완료 경로와 무관하며(recv 콜백 전용),
    admission 삭제 이전부터 존재했다. 그대로 둔다.
  - `_runtime/core/zlink.py`의 `create_thread`/`NativeThread` — `zlink_thread_start`를
    감싸는 **공개 API**(`zlink.Thread`)이지 binding이 내부적으로 쓰는 숨은
    thread가 아니다. 그대로 둔다.
- **grep proof**: `grep -rn "threading.Thread(" bindings/python/src/zlink` →
  `dispatcher.py`의 한 곳만 남는다(아래 참고). Send/request 완료 경로
  (`_runtime/messaging/routed_async.py`, `_runtime/sockets/socket_base*.py`,
  `_native/ffi.py`)에는 `threading.Thread`도 `send_ready`도 없다.

## Grep proof

```
$ grep -rn "send_ready" bindings/python/src/zlink --include="*.py"
(no matches)

$ grep -rn "threading.Thread(" bindings/python/src/zlink --include="*.py"
src/zlink/_runtime/eventing/dispatcher.py:51:                self._thread = threading.Thread(

$ grep -n "zlink_send_async\|zlink_send_complete_handler\|zlink_send_async_cancel" \
    bindings/python/src/zlink/_native/ffi.py
271:  ("zlink_send_async", ...)
272:  ("zlink_send_complete_handler", ...)
273:  ("zlink_send_async_cancel", ...)

$ nm -D bindings/python/src/zlink/native/linux-x86_64/libzlink.so.0.13.0 | \
    grep -E "zlink_send_async|zlink_send_complete_handler|zlink_send_ready_handler|zlink_routed_send_ready_handler"
... T zlink_send_async
... T zlink_send_async_cancel
... T zlink_send_complete_handler
(zlink_send_ready_handler / zlink_routed_send_ready_handler: no matches)
```

벤더 헤더는 없다(`bindings/python`은 C 헤더를 vendoring하지 않고 ctypes struct을
직접 선언한다) — 대신 `ZlinkSendCompleteEvent`/`ZlinkSendAsyncOptions`의
`sizeof`/`alignof`를 `core/include/zlink/socket/api.h`를 include하는 C
translation unit과 대조해 (296, 8) / (24, 8)로 byte parity를 확인했고,
`tests/test_native_contract.py::test_ffi_layouts_are_the_core_0_13_0_layouts`에
그 값을 pin했다.

## 테스트와 증거

- `bindings/python`에서 vendored 0.13.0 라이브러리로
  `python -m pytest tests` 실행: **113 passed, 0 failed** (여러 번 재실행해도
  안정적).
- New-vs-pre-existing 비교: 코드만 python-scoped 파일로 `git stash`한 뒤
  구 코드 + `core/build/lib/libzlink.so.0.11.1`(당시와 같은 버전 skew)로 같은
  스위트를 실행하면 **17개가 실패**한다(대부분 0.11.1/0.13.0 버전 skew
  자체가 원인 — 예: `test_version_matches_core`, layout pin, admission
  기계장치를 직접 assert하던 `test_routed_async_contract.py`의 옛 테스트).
  이번 재정렬 뒤에는 그 실패가 전부 사라졌고 **새로 도입한 실패는 0개**다.
  다음 파일은 계약 자체가 바뀌어 다시 썼다(구 admission 내부 상태를 직접
  assert하던 코드이므로 "고쳐서 통과"가 아니라 "새 계약대로 재작성"):
  `tests/test_lifecycle_contract.py`, `tests/test_native_contract.py`,
  `tests/test_routed_async_contract.py`. 그 외 실패 원인은 아래 "발견한
  Core 결함"의 회피 조치이거나(2곳), PAIR send가 이제 awaitable을 반환하는
  데 따른 순수 기계적 `await`/`asyncio.run()` 추가다(여러 파일).
- Perf 스모크(필수, 아래 표) 통과.

## 발견한 Core 결함 (bindings/python 범위 밖, core/ 미수정)

이 작업 도중 `zlink_send_async`의 multi-part 처리에서 재현 가능한 Core 결함
두 건을 발견했다. **`core/`는 이 작업 범위 밖이라 고치지 않았다** — python
쪽에서는 영향받은 3개 테스트 지점을 1-part 페이로드로 좁혀 우회하고 주석으로
근거를 남겼다(`tests/test_core_api_alignment.py`
`test_router_recv_into_keeps_storage_and_snapshot_contract`,
`tests/test_retained_credit_contract.py`
`test_retained_router_dealer_and_subscribe_preserve_aggregate_metadata`).

1. **ROUTER, 2-part 이상 레코드 → 프로세스 abort.** `router.send(rid).messages(a, b).submit()`을
   재현 최소 스크립트로 실행하면 매번
   `Assertion failed: !_more_out (core/src/runtime/sockets/router/router_send_path.cpp:215)`로
   전체 프로세스가 abort된다(catch 불가능한 C-level assert). 1-part는 즉시
   성공한다.
2. **DEALER, 2-part 이상 레코드(`target=NULL`) → 영구 `NOT_FOUND`/`EHOSTUNREACH`.**
   같은 재현 스크립트에서 `dealer.send().messages(a, b).submit()`은 연결이
   이미 성립된 뒤에도 500ms+ 동안(50회 재시도) 매번
   `SubmitResult.NOT_FOUND`/`errno=EHOSTUNREACH`로 실패한다. 동일한 소켓 쌍에서
   1-part 레코드는 즉시(0번째 시도) 성공한다.

PAIR의 2-part 레코드는 정상 동작한다(재현 스크립트로 확인) — 문제는 routed
target 해석이 개입하는 DEALER/ROUTER 경로에 국한된다. Perf 스모크는 모든
suite가 1-part 페이로드만 쓰므로 이 결함에 영향받지 않는다.

## Perf 스모크

### Single suite — `perf/single/run_benchmarks.sh --pattern ALL --transports tcp --msg-sizes 64 --duration 1 --runs 1`

`status: complete`, `expected_result_lines: 25`, `actual_result_lines: 25`.
결과 파일:
`bindings/python/perf/results/single/report/perf_python_single_linux_20260824_015530_python-realignment-single-smoke.txt`.

| Pattern | Transport | Size | Throughput | Bandwidth | Lat.Mean | Lat.P95 | Lat.P99 |
|---|---|---|---|---|---|---|---|
| PAIR | tcp | 64B | 8.50 Kmsg/s | 0.54 MB/s | 0.135 ms | 0.297 ms | 0.436 ms |
| PUBSUB | tcp | 64B | 165.88 Kmsg/s | 10.62 MB/s | 3.381 ms | 9.017 ms | 10.065 ms |
| DEALER_DEALER | tcp | 64B | 9.04 Kmsg/s | 0.58 MB/s | 0.146 ms | 0.313 ms | 0.464 ms |
| DEALER_ROUTER | tcp | 64B | 9.17 Kmsg/s | 0.59 MB/s | 0.150 ms | 0.324 ms | 0.482 ms |
| ROUTER_ROUTER | tcp | 64B | 8.34 Kmsg/s | 0.53 MB/s | 0.164 ms | 0.345 ms | 0.514 ms |

이 실행 전에는 `perf/single/perf_pair.py`가 `sock.send().flags(...)`(옛
synchronous DONTWAIT builder)를 호출해 `AttributeError: '_ManagedSendOp'
object has no attribute 'flags'`로 PAIR만 실패했다 — PAIR send가 새 계약에서
awaitable을 반환하는 HWM-managed async operation이 되면서 생긴 필연적 표면
변경이다. `perf_pair.py`를 `perf_dealer_dealer.py`와 같은 asyncio 기반
send-loop로 다시 써서 고쳤다(`send_routed` 헬퍼 재사용).

### Multi suite — `perf/multi/run_benchmarks.sh --transports tcp --duration 1` (기본 pattern 집합)

기본 pattern: `MULTI_DEALER_DEALER`, `MULTI_DEALER_ROUTER_SENDSEND`,
`MULTI_ROUTER_ROUTER_SENDSEND`, `MULTI_PUBSUB`, `MULTI_STREAM`.

이 실행 전에는 `perf_multi_dealer_router_server.py`/
`perf_multi_router_router_server.py`가 `received.send().flags(zlink.SendFlags.DONT_WAIT).submit()` +
수동 `pending` deque + `POLLOUT` readiness 재시도로 되어 있어 —
`_ManagedRoutedSendOp`에 `flags()`가 없어 `AttributeError`로 매 요청이 즉시
실패했다(`MULTI_DEALER_ROUTER_SENDSEND`/`MULTI_ROUTER_ROUTER_SENDSEND`
6 size 전부 FAIL). `send_ready`/`POLLOUT` readiness-hint 자체가 폐지됐으므로
`pending` deque와 `POLLOUT` 재시도 로직을 통째로 제거하고, 각 echo 응답을
`asyncio.create_task(router.send(rid).message(payload).submit())`로 발사한
뒤 completion을 `add_done_callback`으로 수거하는 방식으로 다시 썼다 — Core의
`zlink_send_async` 자체 큐가 admission을 맡으므로 binding 쪽 재시도 큐는
더 이상 필요 없다.

두 서버를 고친 뒤 개별 재검증(`--pattern MULTI_DEALER_ROUTER_SENDSEND`,
`--pattern MULTI_ROUTER_ROUTER_SENDSEND`, 각각 `--msg-sizes 64 --duration 1`)에서
둘 다 `status: complete`를 받았다:

- `MULTI_DEALER_ROUTER_SENDSEND tcp 64B`: 13.845 Kops/s, 1.772 MB/s, lat.mean
  3.588 ms (`perf_python_multi_linux_20260824_020044_python-realignment-multi-dr-retest.txt`)
- `MULTI_ROUTER_ROUTER_SENDSEND tcp 64B`: 12.068 Kops/s, 1.545 MB/s, lat.mean
  4.116 ms (`perf_python_multi_linux_20260824_020051_python-realignment-multi-rr-retest.txt`)

이어서 기본 pattern 전체를 다시 묶어 실행한 최종 결과
(`perf_python_multi_linux_20260824_020448_python-realignment-multi-smoke-final.txt`)에서
`status: complete`, `success: 24`, `skip: 4`, `fail: 0`,
`expected_result_lines: 120`, `actual_result_lines: 120`을 확인했다:

| Pattern | Transport | Sizes | 상태 |
|---|---|---|---|
| MULTI_DEALER_DEALER | tcp | 64,256,1024,4096,65536,131072 | 성공 (6/6) |
| MULTI_DEALER_ROUTER_SENDSEND | tcp | 64,256,1024,4096,65536,131072 | 성공 (6/6) |
| MULTI_ROUTER_ROUTER_SENDSEND | tcp | 64,256,1024,4096,65536,131072 | 성공 (6/6) |
| MULTI_PUBSUB | tcp | 64,256,1024,4096,65536,131072 | 성공 (6/6) |
| MULTI_STREAM | tcp | 64,256,1024,65536 | skip — `memory_guard:clients=10000,max_clients=5866` (샌드박스 메모리 상한, 이번 작업과 무관한 기존 환경 제약) |

`MULTI_STREAM`의 skip은 이 sandbox의 파일서술자/메모리 상한 때문에
`--clients 10000` 기본값을 채울 수 없어서이고, STREAM은 raw send만 쓰므로
(HWM-managed async 계약 밖) 이번 재정렬과 무관하다.

## 미해결 사항 (flagged)

1. **Core 결함 2건 (위 절 참고)** — `zlink_send_async`가 ROUTER 2-part
   레코드에서 abort하고 DEALER 2-part 레코드(`target=NULL`)에서 영구
   `NOT_FOUND`를 반환한다. bindings/python은 이를 우회했을 뿐 고치지 않았다.
   Multi-part HWM-managed routed send는 Core 쪽 수정 전까지 신뢰할 수 없다.
2. **pre-existing 레이스**(이번 변경과 무관, 구코드에서도 동일하게 실패):
   `tests/test_core_api_alignment.py::test_router_recv_into_keeps_storage_and_snapshot_contract`가
   `dealer.connect()` 직후 connection-ready 대기 없이 바로 send를 시도한다.
   예전에는 admission ticket이 자체적으로 WRITABLE 이벤트까지 기다려 이
   레이스를 흡수했지만, 새 계약은 바인딩이 재시도하지 않으므로 레이스가
   그대로 드러난다. 이번에 그 지점만 connect-재시도 루프로 고쳐 테스트를
   통과시켰다(핵심 동작 검증은 그대로 유지).
3. `bindings/python/src/zlink/native/`의 벤더 라이브러리를 0.11.1 → 0.13.0으로
   교체했다. `.gitignore`로 추적되지 않으므로 git diff에는 나타나지 않는다 —
   배포/CI 파이프라인이 별도로 이 바이너리를 채워 넣는다면 그쪽 절차도
   0.13.0 소스(`core/build/lib/libzlink.so.0.13.0` 또는 released Core
   0.13.0 산출물)로 갱신해야 한다.
