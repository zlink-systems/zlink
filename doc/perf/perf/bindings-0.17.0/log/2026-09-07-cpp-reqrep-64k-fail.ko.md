# C++ multi REQREP 64 KiB 종료 실패 진단과 수정

## 범위와 고정 조건

- 대상: `MULTI_DEALER_ROUTER_REQREP`, `MULTI_ROUTER_ROUTER_REQREP`
- transport: TCP
- Core: release `0.17.1`, prefix
  `/home/hep7/.cache/zlink/core-pinned/0.17.1`
- client 100개, `PERF_MULTI_REQREP_MAX_OUTSTANDING=64`, 기본 2-part message,
  duration 2초, run 1회를 유지했다.
- Core, 공개 API, spec·policy 문서는 수정하지 않았다.

## 실제 오류와 발생 지점

기존 러너는 client stderr를 report에 싣지 않으므로, C++ client의 기존 예외 경계에 임시
진단 출력만 추가해 직접 실행한 뒤 제거했다. 확인한 실제 출력은 다음과 같다.

```text
REQREP_LAUNCH_ERROR,No such device or address (errno=22),code=6,errno=22
REQREP_COMPLETION_BINDING_ERROR,No such device or address (errno=22),code=6,errno=22,outstanding=688,slot_outstanding=12
```

숫자 결과는 `submit_result_t::invalid_argument`(`code=6`)와 `EINVAL=22`다. 즉
`ENOMEM`, `ENOBUFS`, HWM memory guard 실패가 아니다. 문자열과 숫자 errno의 표현이
일치하지 않으므로 판정은 typed result와 숫자 errno를 기준으로 했다.

Core의 기존 `ZLINK_ROUTED_PART_DEBUG=1` 진단을 켠 재현에서는 다음 출력이 반복됐다.

```text
[routed-part-debug] prepare_send_step busy family=5 active_family=5 same_thread=0
terminate called after throwing an instance of 'zlink::submit_error_t'
  what():  No such device or address (errno=22)
```

발생 경로는 다음과 같다.

1. C++ binding의 `submit_raw_request_state()`가 2-part request를
   `submit_message_parts()`로 part별 제출한다
   (`bindings/cpp/src/Runtime/Messaging/operation_submit.hpp:181-213`).
2. 첫 application thread의 multipart sequence가 열린 동안 runtime completion owner가
   WRITABLE token을 받아 같은 socket의 retained request를 다른 thread에서 재제출한다.
3. Core `prepare_send_step_state_locked()`가 열린 sequence의 `owner_thread`와 현재 thread가
   다른 것을 검출하고 `EINVAL`로 전체 attempt를 거부한다
   (`core/src/api/socket/part_helper_api.cpp:169-185`).

같은 65536 B 셀을 `--part-count 1`로 실행하면 통과했다. 따라서 크기 자체가 아니라 큰
2-part request가 backpressure를 만나면서 application submit과 runtime-owner retry가
교차하는 조건이 원인이다.

## 메모리 가설 검증

감독자 가설의 상한 계산은 맞다.

```text
100 clients * 64 outstanding * 65,536 B = 419,430,400 B = 400 MiB
```

하지만 최초 fatal은 전체 outstanding 688개, 해당 slot 12개에서 발생했다. payload 명목량은
`688 * 65,536 = 45,088,768 B`(43 MiB)이고 프로세스 최대 RSS는 56,808 KiB였다. 이 시점은
400 MiB 상한과 멀고, 확보한 오류도 allocation/HWM 오류가 아니라 위 `EINVAL`이다.

진단용 binding lock을 잠시 넣은 실행은 최대 RSS 467,968 KiB까지 올라가도 같은 5개 크기를
완료했다. 이 lock은 C++ 계약에 어긋나는 잘못된 수정이므로 전부 제거했지만, 400 MiB payload와
staging 복사본이 실제로 수백 MiB RSS를 만들 수 있다는 점과 그 메모리 수준 자체가 실패 원인은
아님을 확인하는 자료로만 사용했다. 조사 당시 host는 `MemAvailable` 약 86.9 GiB,
virtual-memory limit는 `unlimited`였다. runner resource guard도 발동하지 않았고
`ENOMEM`/`ENOBUFS`도 관찰되지 않았다.

올바른 runner 수정 후 5-size 최종 실행의 최대 RSS는 79,448 KiB였다. completion이 같은 active
thread에서 즉시 진행돼 실제 outstanding이 상한까지 쌓이지 않은 결과이며, 상한이나 admission
조건을 낮춘 결과가 아니다.

## 교차 언어 대조

Rust에서 동일한 Core, 두 pattern, TCP, 65536 B, client 100개, outstanding 상한 64로 실행했다.
두 셀 모두 통과했다.

```text
success: 2
fail: 0
status: complete
```

결과 파일:
`bindings/rust/perf/results/multi/report/perf_rust_multi_linux_20260907_192915_cpp-reqrep-64k-contrast.txt`

Rust runner는 모든 requester socket을 하나의 `POLLCOMPLETION` poller에 등록한다
(`bindings/rust/perf/multi/src/perf_multi_socket_reqrep.rs:247-258`). C reference도 하나의
active completion poller에서 admission과 callback drain을 진행한다
(`bindings/c/perf/multi/common/perf_multi_socket_reqrep.hpp:576-603`). 따라서 전체 언어의
상한 정책 문제가 아니라 C++ runner만 completion owner를 runtime thread에 남긴 차이다.

## 수정과 판정

수정 파일은
`bindings/cpp/perf/multi/common/perf_multi_reqrep.hpp` 하나다.

- setup에서 requester 100개 전부를 단일 public poller에 `pollcompletion` 단독 등록한다.
- 각 submit/progress turn은 `wait(..., 0)`을 정확히 한 번 호출한 뒤 application ready queue를
  한 round 진행한다. drain도 같은 경로를 쓴다.
- public poller 등록으로 completion owner가 `wait()` 호출 thread로 이전되어 최초 multipart
  submit과 WRITABLE exact-token retry가 같은 active application thread에서 실행된다.

이는 `PERF_MULTI_TEST_POLICY.md:233-252`의 completion-only poller와 turn당 nonblocking wait
1회 규칙, `bindings/doc/spec/async-execution-model.ko.md:67-70`의 completion-owner 이전 계약을
그대로 사용한 것이다. C++ binding 자체에 lock/gate를 넣는 안은
`bindings/doc/spec/cpp/README.ko.md:455-461`에 정면으로 어긋나므로 폐기했다.

- 소유 계층: 같은 socket의 concurrent multipart 직렬화는 application/perf runner 소유다.
- 계약 조항: C++ spec 455-461, async execution model 67-70, multi perf policy 233-252.
- 교차언어: C와 Rust 모두 single active completion poller에서 진행한다.
- 변경 분류: **B — 기존 C++ multi perf runner 결함**. binding/Core 결함 수정이 아니다.
- 규칙 수: 수정 전에는 application submit과 runtime retry라는 실행 owner 2개가 경합했으나,
  수정 후에는 public poller의 active thread owner 1개만 남는다.

client 수, 64 outstanding 상한, payload 크기, 2-part 구성, HWM, request timeout과 duration은
바꾸지 않았다. 따라서 측정 조건 완화가 아니라 기존 public execution 계약에 맞춘 dispatch-owner
수정이다.

## 검증

먼저 임시 binding 변경을 모두 제거한 뒤 C++ binding과 두 client를 재링크했다.

```text
cmake --build bindings/cpp/build \
  --target cpp_comp_src_dealer_router_reqrep_client \
           cpp_comp_src_router_router_reqrep_client -j2
```

관련 계약 테스트:

```text
test_cpp_contract_request_writable_retry  Passed
test_cpp_perf_application_ready_queue     Passed
100% tests passed, 0 tests failed out of 2
```

최종 perf는 각 실행 전에 `scripts/perf/wait-for-idle-perf.sh`로 idle을 확인하고 다음 조건으로
한 번 실행했다.

```text
--pattern DEALER_ROUTER_REQREP,ROUTER_ROUTER_REQREP
--transports tcp
--duration 2
--runs 1
--msg-sizes 64,256,1024,4096,65536
--reuse-build
```

결과:

```text
success: 10
unsupported: 0
skip: 0
fail: 0
status: complete
expected_result_lines: 50
actual_result_lines: 50
```

최종 결과 파일:
`bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260907_194520_reqrep-64k-runner-fix-final.txt`

