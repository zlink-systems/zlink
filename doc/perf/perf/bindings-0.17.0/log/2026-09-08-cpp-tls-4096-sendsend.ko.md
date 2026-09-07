# C++ multi TLS 4096 B SENDSEND 실패 진단

감독자가 원인·수정 범위·측정 의미와 잔여 실패를 판정하기 위한 기록이다.

## 결과와 변경 범위

C++ routed echo server가 backpressure 중에도 수신마다 새 async send를 시작해
소켓별 completion reservation 65,536개를 소진한다. 65,537번째 미완료 제출은
`submit_result_t::out_of_memory(10)`, `ENOMEM(12)`으로 실패한다. Server 실패 후
client의 send drain이 끝나지 않아 최초 보고된 client exit 1로 이어진다.

수정 파일은 `bindings/cpp/perf/multi/common/perf_multi_routed_relay.hpp`다.
DEALER→ROUTER와 ROUTER→ROUTER server가 이 구현을 공유한다. Client에 넣은
임시 진단은 원문으로 되돌렸으며, Core·binding 라이브러리·Framework와 정책 문서는
수정하지 않았다. Commit·push는 수행하지 않았다.

## 실패 경로와 관측값

아래 줄 번호는 임시 로그를 넣기 전 소스 기준이다. Client 소스는 최종 상태에서도 같다.

| 위치 | 관측과 전파 |
|---|---|
| `bindings/cpp/perf/multi/common/perf_multi_routed_relay.hpp:185` | 수신마다 `submit_routed_reply_async()`를 새로 시작한다. 미완료 수가 65,536까지 증가한다. |
| 같은 파일 `:72` | 다음 send의 `submit_error_t`: result=10, internal_errno=12, errno=12, in_flight=65537. 실패 플래그를 설정한다. |
| 같은 파일 `:190–194` | 기존 진단 `dealer_router server: async send failed errno=12`를 출력하고 false를 반환한다. |
| `bindings/cpp/perf/multi/src/perf_dealer_router_client.cpp:323–325` | `run_until_senders_drained()`가 false를 반환한다. 로그 시점 errno=0. |
| `bindings/cpp/perf/common/perf_socket_adapter.hpp:242–243` | coordinator의 drain deadline 검사에서 false. Client recv 오류 분기·submit 오류 분기는 관측되지 않았다. |
| `bindings/cpp/perf/multi/src/perf_dealer_router_client.cpp:91–93,394–398` | run_phase의 false가 bench.run()과 최종 `DEALER_ROUTER_CLIENT_FAIL`/exit 1로 전파된다. |

Client의 errno=0을 recv 실패 원인으로 해석하면 안 된다. 종료 조건은 boolean deadline
실패이며 해당 분기는 errno를 설정하지 않는다. 임시 로그는 모든 client false 분기,
submit exception의 result/internal_errno, recv 비정상 반환의 rc/errno를 구분했다.
이 재현에서 client의 비복구 recv 반환 코드는 관측되지 않았고, 직접 관측한 submit 실패는
server의 result=10이다.

`PERF_DEBUG_TRANSITIONS=1`을 함께 켜야 Python 러너가 수집한 server/client stderr 전체를
외부 로그에 남긴다. 핵심 원문은 다음과 같다.

```text
[server] TLS_DIAG,server_pending=16384
[server] TLS_DIAG,server_pending=32768
[server] TLS_DIAG,server_pending=65536
[server] TLS_DIAG,server_submit,result=10,internal_errno=12,errno=12,in_flight=65537
[server] dealer_router server: async send failed errno=12
TLS_DIAG,false,original_line=325,errno=0
DEALER_ROUTER_CLIENT_FAIL,transport=tls,size=4096
```

전체 진단 로그: `/tmp/cpp-tls-diag-codes.log`, `/tmp/cpp-tls-diag-server.log`.
재현 report tag: `tls4096_diag_codes`, `tls4096_diag_server`(모두 partial).

## C와의 차이 및 duration 의존성

C의 `bindings/c/perf/multi/common/perf_multi_relay_server.hpp`는 다음 흐름을 사용한다.

- `drain_recv_and_relay():420–429`: 응답 payload와 routing ID를 pending FIFO에 보관한다.
- `try_send_reply_now():200–201`: 기존 wait token이 있으면 새 native send를 하지 않는다.
- `flush_pending_replies():345–363`: FIFO 선두의 admission 완료 후 다음 응답을 제출한다.
- `run_server_loop()`의 completion 처리 `:497–514`: 동일 token의 WRITABLE을 처리한 뒤
  보관한 응답을 재제출한다.

C++ 수정 전에는 보관해야 할 응답마다 별도 coroutine과 native wait token을 만들었다.
따라서 duration에 따라 증가하는 양은 메시지 크기 설정이나 deadline 값 자체가 아니라
**같은 server socket에 누적된 미완료 send와 그 completion reservation 수**다.
유입된 응답 수에서 admission·completion 처리로 해소한 응답 수를 뺀 backlog가 커질수록
reservation도 함께 증가했다.

65,536개 × 4096 B는 payload만 256 MiB다. 이것은 프로세스 메모리 부족을 측정한 값이
아니라 해당 개수의 payload 크기 환산이다. 직접 확인한 한계는 Core의 reservation 개수다.

감독자의 대조 결과에서 2초는 통과하고 5초는 실패했다. 이번 5초 진단은 65,537번째에서
실제 한계 초과를 확인했다. TLS의 처리 비용과 4096 B payload의 byte credit 부담 아래에서
공유 ROUTER가 입력을 받는 속도와 응답 admission을 해소하는 속도의 차이가 누적되는 것으로
해석한다. 작은 메시지·TCP·비routed 대조군은 감독자가 제시한 조건에서 이 실패를 만들지
않았다. TLS 암호화 비용과 각 queue의 기여율을 따로 계측한 것은 아니며, 모든 환경에서
4096 B·TLS에만 발생한다고 일반화하지 않는다. 선행 크기나 duration 대조는 재실행하지 않았다.

Core `core/v0.17.1`의 근거는 다음과 같다.

- `core/src/api/socket/socket_completion_queue_internal.hpp:25–28`: 예약부터 public
  completion dequeue까지 slot을 유지하며 상한은 65,536이다.
- `core/src/api/socket/socket_completion_queue_internal.cpp:107–110`: 상한 초과를 검출한다.
- 같은 파일 `:251–260`: SEND wait token 발급 실패의 EAGAIN을 ENOMEM으로 변환한다.
- `core/doc/spec/core/socket/README.ko.md:960–988`, 「Part send와 pending admission」:
  SEND reservation 상한 초과는 `OUT_OF_MEMORY + ENOMEM`, ID 0, 후속 completion 없음이다.

Core 동작과 C++의 오류 투영은 이 계약에 부합한다. 하위 계층 결함을 러너에서 보상한 수정이 아니다.

## 수정과 측정 의미

`routed_reply_state_t`가 `received_t` FIFO를 소유하고, 같은 소켓의 송신 coroutine 하나가
FIFO 선두를 public `send(...).message(...).async()`로 제출한다. Admission이 완료되면
선두를 제거하고 다음 응답을 제출한다. 수신 drain은 그동안에도 계속 payload를 보관한다.
소켓은 이미 `POLLIN | POLLCOMPLETION`으로 등록되므로 FIFO와 coroutine continuation은
public poller를 호출하는 thread에서 진행한다.

대안으로 수신 루프 자체를 send admission 동안 중단하면 native POLLIN drain을 바꾼다.
FIFO와 송신 coroutine을 분리하는 쪽을 선택해 C의 수신 의미를 유지했다. Native token,
errno별 재제출, 별도 POLLOUT 상태는 추가하지 않았다. 기존 C++ async terminal이 이를 소유한다.

소켓별 sender 수를 하나로 정한 것은 echo 왕복을 한 건으로 제한하는 것이 아니다. Admission
완료 후 다음 send를 시작하며 echo 수신을 기다리지 않는다. Client 수 100, active 5초,
메시지 크기·part 수, auto-HWM, 200 ms socket timeout, 5000 ms send drain timeout,
처리량 분모와 latency 산식은 그대로다. 오류 catch와 최종 false 전파도 유지한다.

수정 전/후 규칙 수: 응답마다 sender 생성 및 미완료 sender 카운트 관리(2) →
소켓의 FIFO 선두 admission 후 다음 응답 제출(1). 응답 소유 위치는 coroutine별 저장에서
공유 FIFO 한 곳으로 옮겼다.

- 소유 계층: C++ perf multi 러너의 echo 작업 제출 순서. Native admission·재제출·completion은 binding/Core.
- 사양: `doc/perf/PERF_MULTI_TEST_POLICY.md` §1.1(송신 admission await 후 다음 send 허용,
  POLLIN drain 유지), `bindings/doc/spec/async-execution-model.ko.md` §4(completion owner).
- 교차언어 대조: C relay의 FIFO와 단일 wait token을 대조했다. Framework runtime 변경은 없다.
- 변경 분류: **B — 기존 러너 결함 수정**.

## 검증

실행 환경은 `ZLINK_CORE_SOURCE=release`, pinned Core 0.17.1
(`/home/hep7/.cache/zlink/core-pinned/0.17.1`), revision
`4cd03b917304ea69d2744fcc4bf29fd528dc7b1f`, `PERF_DEBUG=1`이다.
추가 stderr 보존용으로 `PERF_DEBUG_TRANSITIONS=1`을 설정했다.
각 실행 전에 `bash scripts/perf/wait-for-idle-perf.sh`를 수행하고 load가 5 이하인지 확인했다.
모든 실행은 직렬이며 `--duration 5 --runs 1 --reuse-build`를 사용했다.

```bash
export ZLINK_CORE_SOURCE=release
export ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1
export PERF_DEBUG=1 PERF_DEBUG_TRANSITIONS=1
bash scripts/perf/wait-for-idle-perf.sh
# /proc/loadavg 첫 값이 5 이하여야 다음 명령을 실행한다.
bash bindings/cpp/perf/run_benchmarks_multi.sh \
  --pattern MULTI_DEALER_ROUTER_SENDSEND --transports tls \
  --msg-sizes 4096 --duration 5 --runs 1 --reuse-build --results-tag tls4096_fix1
```

| 검증 항목 | 실행 | 결과 |
|---|---|---|
| 1·2. 재현 및 연속 3회 | DEALER→ROUTER / TLS / 4096 B — [fix1](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_005923_tls4096_fix1.txt) | complete, fail=0 |
|  | 동일 — [fix2](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_005959_tls4096_fix2.txt) | complete, fail=0 |
|  | 동일 — [fix3](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_010006_tls4096_fix3.txt) | complete, fail=0 |
| 3. ROUTER→ROUTER | TLS / 4096 B — [rr](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_010013_tls4096_rr.txt) | complete, fail=0 |
| 4. 전 크기 | DEALER→ROUTER / TLS / 64,256,1024,4096,65536 B — [full_dr](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_010023_tls4096_full_dr.txt) | complete, fail=0 |
|  | ROUTER→ROUTER / TLS / 같은 전 크기 — [full_rr](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_010051_tls4096_full_rr.txt) | complete, fail=0 |
| 5. 회귀 | DEALER→DEALER / TLS / 4096 B — [reg_dd](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_010127_tls4096_reg_dd.txt) | complete, fail=0 |
|  | DEALER→ROUTER / TCP / 4096 B — [reg_tcp](../../../../../bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_010133_tls4096_reg_tcp.txt) | complete, fail=0 |

빌드는 기존 설정에서 해당 C++ perf executable target만 재컴파일했다. Build log에 Core
컴파일은 없으며, RESULT META와 `ldd`에서 pinned Core 경로를 확인했다.
`git diff --check` 통과. 정상 검증에는 최초 원인 진단용 로그가 없는 바이너리를 사용했다.

## 잔여 실패

ROUTER→ROUTER 전 크기 실행 `tls4096_full_rr`은 다섯 크기 RESULT 25줄을 출력하고
`status: complete`, `fail: 0`으로 종료했다. 하지만 65536 B RESULT 뒤에 server가
`router_router server: async send failed errno=110`을 남겼다.
이는 보고서의 complete와 별도로 감독자가 판정해야 하는 종료 실패다.
이 오류를 무시하는 코드나 timeout 변경은 추가하지 않았다.
추가 진단에서 65536 B 단독(`tls4096_shutdown_diag`)과 동일 전 크기
(`tls4096_shutdown_full_diag`)를 각각 한 번 실행했다. 둘 다 `complete`, `fail=0`이며
server 오류가 재현되지 않았다. 종료 deadline 분기에 pending 수·sender 상태를 기록하는
임시 로그도 발화하지 않았다. 따라서 최초 errno=110의 정확한 발화 분기와 근본 원인은
확정하지 않았다. 코드상 후보는 수정 파일 `:125–132`의 기존 종료 drain deadline이다.
이 한 번의 종료 오류를 해결됐다고 판정하지 않는다.

원문 로그는 `/tmp/cpp-tls-full_rr.log:76`이며 추가 진단 로그는
`/tmp/cpp-tls-shutdown-diag.log`, `/tmp/cpp-tls-shutdown-full-diag.log`다.
추가 진단용 로그도 모두 제거하고 두 server target을 다시 빌드했다. 최종 소스는
앞서 연속 3회와 전체 검증에 사용한 수정본과 동일하다.

잔여 오류 때문에 자동 생성 report의 `complete`만으로 server 종료까지 무오류라고
판정하면 안 된다. 본 변경의 4096 B reservation 고갈 해결과 이 종료 오류의 판정을
구분해 감독자에게 인계한다.
