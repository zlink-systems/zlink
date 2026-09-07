# C++ TLS SENDSEND 종료 drain 진단과 수정

## 결과와 수정 범위

C++ DR/RR client는 제출의 admission만 끝나면 socket을 닫아, relay에 남은 echo가
연결 종료 뒤 WRITABLE을 기다릴 수 있었다. C reference처럼 admission된 메시지의
모든 echo를 받은 뒤 client socket을 닫도록 수정한다. Server의 pending FIFO와
단일 sender는 유지한다.

- 소유 계층: C++ multi perf client의 측정 종료와 socket 수명 관리.
- 계약 근거: `doc/perf/PERF_MULTI_TEST_POLICY.md` §1.1의 admission 기반 연속 제출과
  §3.5의 active 구간 밖 종료 정리; `bindings/doc/spec/async-execution-model.ko.md` §4의
  단일 completion owner; `core/doc/spec/core/socket/README.ko.md:1009-1016`의
  transport 종료 시 SEND 대기 토큰 유지.
- 교차언어 대조: C client는 retained send와 미수신 echo를 모두 정리한다.
  C++ client는 admission만 기다려 이 종료 조건이 누락돼 있었다.
- 변경 분류: **B — C++ perf client의 기존 종료 결함**. Binding 라이브러리와 Core의
  계약 위반을 확인한 것이 아니다.

변경 파일:

- `bindings/cpp/perf/multi/common/perf_client_helpers.hpp`: DR/RR이 공유하는
  `echo_reply_drain_t`. 제출한 logical echo의 미완료 수와 종료 drain을 소유한다.
- `bindings/cpp/perf/multi/src/perf_dealer_router_client.cpp`: 제출·거절과 정상 echo
  수신을 기록하고 기존 deadline 안에 남은 echo를 받는다.
- `bindings/cpp/perf/multi/src/perf_router_router_client.cpp`: 같은 종료 조건을 적용한다.

`perf_multi_routed_relay.hpp`의 임시 진단은 제거했다. 이 파일의 최종 diff는 없다.
`core/**`, `framework/**`, 다른 언어, 정책·스펙·계획서·decisions 문서는 수정하지 않았다.
Commit과 push는 수행하지 않았다. 기존 untracked Framework smoke 로그는 보존했다.

## STOP 시점의 pending 관측

기존 relay에 STOP·timeout 시점의 pending 크기, 실행 중 최대 깊이, 수신 수,
FIFO pop 수와 drain poll 호출 수를 임시 기록했다. `sent`라는 진단 필드는 실제로
FIFO pop 수이며, stale-route 처리나 실패 후 pop도 포함하므로 admission 성공 수로
해석하지 않는다. 로그는 `/tmp/cpp-tls-drain2-diagnostic.log`에 보존했다.

진단 실행은 DR/TLS, 64·256·1024·4096·65536 B, duration 5초, runs 5이며
`PERF_DEBUG=1`, `PERF_DEBUG_TRANSITIONS=1`을 사용했다. 결과는
`bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_012447_drain2_diag.txt`
의 `status: partial`이다.

| run / 크기 | STOP pending | 실행 중 최대 pending | 수신 수 | STOP FIFO pop 수 | timeout pending | drain poll 호출 수 |
|---|---:|---:|---:|---:|---:|---:|
| 1 / 65536 B | 40,287 | 43,442 | 222,684 | 182,397 | 40,287 | 26 |
| 3 / 4096 B | 399,569 | 458,227 | 1,591,202 | 1,191,633 | 399,569 | 26 |
| 4 / 65536 B | 37,008 | 38,806 | 207,711 | 170,703 | 37,008 | 26 |
| 5 / 4096 B | 610,814 | 610,814 | 1,774,792 | 1,163,978 | 610,814 | 8,940,633 |

모든 실패에서 STOP부터 timeout까지 FIFO pop 수가 변하지 않았다. Timeout의
`server.close()` 뒤 pop 수가 1 증가하는 것은 대기 중 coroutine의 실패 처리이며,
reply 전송 성공이 아니다. Run 5의 많은 poll 호출은 수신하지 않는 종료 단계에도
`POLLIN`이 등록돼 있기 때문에 가능한 현상이다. 다른 실패는 200 ms wait 약 26회로
같은 정지를 보였다. Poll 호출 자체가 중단되거나, 매 reply마다 200 ms를 기다린 결과가 아니다.

수신은 backpressure 중에도 FIFO에 계속 쌓인다
(`perf_multi_routed_relay.hpp:171-197`). Sender가 admission을 기다리는 동안 수신 루프는
`recv`의 no-data까지 진행하므로, outer `poller.wait()`로 돌아가기 전에는 send
continuation도 진행하지 못한다. 큰 payload에서 수신이 reply admission을 앞지르며
backlog가 쌓인다. 이는 C에도 있는 pending 구조다. 같은 진단 run 1의 4096 B는
최대 575,720개가 쌓였지만 STOP 때 0개였으므로, 최대 깊이만으로 timeout을 설명할 수 없다.

실패의 종료 조건은 **미수신 echo가 남아 있는데 client가 연결을 닫는 것**이다.
C++의 `application_poller_coordinator_t::run_until_senders_drained()`는 active deadline과
sender 완료만 확인한다(`bindings/cpp/perf/common/perf_socket_adapter.hpp:240`).
기존 client의 recv loop도 deadline 이후에는 첫 메시지를 받은 다음 바로 빠져나갔다.
Client의 `run()` 반환 뒤 socket이 파괴되므로 server는 남은 reply를 받는 peer를 잃는다.

## C와 C++의 정확한 차이

| 경로 | C reference | 수정 전 C++ |
|---|---|---|
| 즉시 admission | `flush_pending_replies()`가 FIFO를 연속 제출 | `.async()`가 즉시 완료된 result를 반환하여 같은 coroutine에서 연속 제출 |
| backpressure | wait token의 WRITABLE을 받아 다음 제출 | public poller가 completion을 처리하고 단일 sender를 재개 |
| client 종료 | retained send와 미수신 echo가 모두 없어야 종료 | sender admission만 끝나면 종료 |

C의 연속 제출은
`bindings/c/perf/multi/common/perf_multi_relay_server.hpp:331-365`에 있다.
C++도 `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:77-82`에서 즉시 admission을
`immediate_send_result_t`로 반환한다. 이 result의 `ready()`는 항상 true다
(`async_operation_state.hpp:73-82`). Public awaiter는 이를 `await_ready()`에 그대로
반영한다(`bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp:80`).
따라서 **backpressure가 없는데 매번 suspend한다는 가설은 기각한다**.

C client는 `perf_multi_client_helpers.hpp:1241-1245`에서 미수신 echo를 차감하고,
`:1372-1407`에서 retained send와 미수신 echo가 모두 없어질 때까지 종료를 기다린다.
C의 현재 timeout은 duration에 따라 늘어나는 별도 계산이 있지만, 이번 수정은 이를
복제하지 않는다. C++의 기존 active deadline + `send_drain_timeout_ms`를 그대로 쓴다.

## 수정의 의미와 대안

`echo_reply_drain_t`는 제출 직전에 1을 더하고, 크기와 run/phase가 맞는 수신 echo 또는
제출 거절에서 1을 뺀다. Async admission 뒤 continuation이 ready queue에서 재개되기
전에 receive dispatch가 echo를 받을 수 있으므로, `co_await` 뒤에 더하면 수신이
계산을 앞지를 수 있다. 제출 전 계산은 이 순서에서도 같은 logical echo를 보존한다.
Counter는 client poll thread에서만 변경된다. Sender가
종료한 뒤 남은 echo도 동일한 poller와 기존 receive dispatch로 받는다. 미수신 수가
0이면 끝나고, 기존 drain deadline에 도달하면 ETIMEDOUT으로 실패한다.
미수신 수가 0인데 추가 정상 echo가 오면 EPROTO로 실패한다.

이 counter는 client의 종료 조건에만 쓰며 send 허용 여부에는 쓰지 않는다.
Inflight 상한, 새 timer, sleep, 별도 poller나 재제출 경로를 추가하지 않는다.
Active duration, client 100개, payload 크기와 header, async admission, FIFO 순서는
동일하다. Deadline 이후 echo는 정리만 하고 throughput·latency 집계에 포함하지 않는다.
Admission 대기와 echo 정리는 동일한 절대 deadline을 공유하므로 시간 예산을 새로
시작하지 않는다. Server의 ETIMEDOUT 분기도 유지한다.

비교한 대안은 server의 제출 경로 변경과 client의 종료 조건 정합이다.
Server는 이미 즉시 admission을 연속 처리하며, 상대 연결이 닫힌 뒤의 대기 토큰은
제출 루프를 바꿔도 진행할 수 없다. 따라서 C와 다른 종료 조건을 소유한 client를 수정한다.
DR/RR의 미수신 수와 종료 loop는 공통 helper 한 곳에 둔다.

수정 전/후 규칙 수: C/C++의 echo client socket 종료 기준 **2종 → 1종**
(C는 echo 수신까지, C++는 admission까지만 → 모두 admission된 echo 수신까지).
새 counter는 기존 지표 count의 복사본이 아니다. 지표는 active 수신만 세며,
종료 counter는 전체 제출에서 거절·active 수신·종료 수신을 뺀다.
Sender가 모두 끝난 뒤에는 정확히 admission된 메시지의 미수신 수다.

## 고정 Core와 검증

모든 실행에서 다음 환경을 사용한다.

```bash
export ZLINK_CORE_SOURCE=release
export ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1
```

CMake cache, 실행 META와 `ldd`에서 pinned
`/home/hep7/.cache/zlink/core-pinned/0.17.1/lib/libzlink.so.0.17.1`을 확인했다.
Core revision은 `4cd03b917304ea69d2744fcc4bf29fd528dc7b1f`이다.
C++ perf target만 증분 빌드했으며 Core는 재빌드하지 않았다.

공통 옵션은 `--msg-sizes 64,256,1024,4096,65536 --duration 5 --runs 5 --reuse-build`다.
각 실행 직전에 `bash scripts/perf/wait-for-idle-perf.sh`를 실행하고 load가 5 이하임을
확인한다. 측정은 한 번에 하나만 실행한다. DR/TLS 두 실행 사이에는 다른 측정을 넣지 않는다.

| 검증 | pattern / transport | 결과 | 시작 전 idle load | 보고서 |
|---|---|---|---:|---|
| 1 | MULTI_DEALER_ROUTER_SENDSEND / tls | complete | 3.61 | [DR/TLS 1차](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_013451_drain2_final_dr_tls_1.txt) |
| 3: 1번 연속 재실행 | MULTI_DEALER_ROUTER_SENDSEND / tls | complete | 4.73 | [DR/TLS 2차](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_013754_drain2_final_dr_tls_2.txt) |
| 2 | MULTI_ROUTER_ROUTER_SENDSEND / tls | complete | 4.40 | [RR/TLS](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_014052_drain2_final_rr_tls.txt) |
| 4: TCP 회귀 | MULTI_DEALER_ROUTER_SENDSEND / tcp | complete | 3.53 | [DR/TCP](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_014410_drain2_final_dr_tcp.txt) |
| 4: DEALER 회귀 | MULTI_DEALER_DEALER / tls | complete | 2.82 | [DD/TLS](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_014709_drain2_final_dd_tls.txt) |

실행 예시는 다음과 같다. 나머지는 표의 pattern·transport와 보고서 파일명의 tag를 사용한다.

```bash
bash scripts/perf/wait-for-idle-perf.sh
# idle 출력의 load가 5 이하일 때만 다음 명령을 시작한다.
bash bindings/cpp/perf/run_benchmarks_multi.sh \
  --pattern MULTI_DEALER_ROUTER_SENDSEND --transports tls \
  --msg-sizes 64,256,1024,4096,65536 --duration 5 --runs 5 \
  --reuse-build --results-tag drain2_final_dr_tls_1
```

위 표는 counter를 제출 전에 기록하는 최종 코드의 실행만 포함한다.
이보다 앞서 실행한 `drain2_dr_tls_1`, `drain2_dr_tls_2`도 complete였지만,
continuation 재개보다 echo 수신이 앞서는 경우를 보완하기 전 결과이므로 최종 판정에는 쓰지 않는다.

최종 실행은 모두 `status: complete`, fail 0, skip 0, 집계 result line 25/25,
exit code 0이다. DR/TLS 연속 2회와 RR/TLS, DR/TCP·DD/TLS 회귀가 모두 통과했다.
`git diff --check`와 보고서 링크의 파일 존재 확인도 통과했다. 남은 실패는 없다.

최종 diff는 C++ multi perf의 helper·DR client·RR client와 이 기록 파일뿐이다.
임시 진단을 제거한 relay server의 diff가 없음을 확인했다.
