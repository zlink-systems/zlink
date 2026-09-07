# C++ Multi STREAM tail 정체 진단

## 판정

**러너 수정은 제출하지 않는다.** 재현된 실패는 dispatcher queue에 남은 echo를
STOP 처리 중 버리는 현상으로 설명되지 않는다. TCP 계수와 서버 packet 계수를 대조한
실행에서는 서버가 kernel에서 읽은 35 frame분의 데이터가 packet API로 반환되지 않았다.
서버가 반환받아 송신한 echo는 모두 client TCP에 도착했다.

추가 조사 대상은 고정 Core 0.17.1의 TCP 수신 이후 STREAM packet 반환 이전 경로다.
C++ 송수신·poller wrapper를 native C API로 대체한 임시 진단에서도 실패했다.
**Core 내부에서 정체를 만드는 정확한 statement는 아직 특정하지 못했다.** 아래 Core
소스 위치는 조사 경계이며 결함 statement로 확정한 위치가 아니다.

사용자가 Core 수정·재빌드를 금지했고 하위 계층 결함을 러너에서 보상하지 않도록
지시했으므로, 단일 thread 송신 변형을 최종 수정으로 채택하지 않았다. 요청한
`complete` 3회 연속 통과 조건은 달성하지 못했다.

## 환경과 변경 파일

- 작업 branch: `main`, HEAD `79380e3091`.
- `ZLINK_CORE_SOURCE=release`.
- `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1`.
- 실제 runtime: 위 prefix의 `lib/libzlink.so.0.17.1`.
- Core revision: `4cd03b917304ea69d2744fcc4bf29fd528dc7b1f`.
- `PERF_DEBUG=1`. 임시 진단에서는 기존 `PERF_DEBUG_TRANSITIONS=1`도 사용했다.
- 모든 perf 실행 직전에 `bash scripts/perf/wait-for-idle-perf.sh`를 실행했다.
  측정은 순차 실행했다.
- 유지하는 변경 파일은 이 기록뿐이다. 임시 카운터, native API 대조, 송신 thread 변경,
  poll timeout 후 receive probe는 모두 제거했다. 원본 STREAM 서버를 재빌드했다.
- Core·다른 언어·Framework·정책·스펙을 수정하지 않았다. commit/push하지 않았다.
  시작 시 존재하던 Framework의 untracked smoke 디렉터리는 보존했다.
- 러너 동작 규칙 수: 수정 전/후 동일. timeout, duration, client 수, 판정 기준도 동일하다.

빌드는 기존 pinned Core 설정을 확인한 뒤 다음 target만 실행했다.

```bash
cmake --build bindings/cpp/build --target cpp_comp_src_stream_server -j 4
```

## C와 C++ 경로 대조

| 경계 | C | C++ |
|---|---|---|
| 수신 후 echo 제출 | `bindings/c/perf/multi/common/perf_multi_stream_session.hpp:363`, `:371`에서 수신한 packet을 같은 event-loop thread에서 제출한다 | `bindings/cpp/perf/multi/src/perf_stream_server.cpp:243`, `:252`에서 packet을 만들어 queue에 넣고, `:280`, `:312`의 별도 dispatcher가 제출한다 |
| 수신 직후 stop 경합 | C session `:439`에서 처리한 뒤 `:447`에서 stop을 확인한다 | C++ server `:235`에서 즉시 반환할 수 있고, `:188`에서 queue 등록을 거부할 수 있다. `:252`의 호출자는 등록 실패 반환값을 사용하지 않는다 |
| stop 이후 | C session `:484`에서 stop을 읽고 outstanding이 없으면 `:487`에서 종료한다. 남은 제출은 completion을 처리한다 | C++ server `:352`에서 queue를 닫고, dispatcher는 `:280`에서 이미 등록된 packet을 계속 꺼낸다. `:285`에서 deadline과 미완료 송신을 확인한다 |
| 마지막 종료 순서 | C server `bindings/c/perf/multi/src/perf_multi_stream_server.cpp:234`에서 event loop를 실행하고, 이후 fenced close를 수행한다 | C++ server `:529` pull loop → `:530` queue close → `:531` dispatcher join |

C++에는 수신과 queue 등록 사이에 stop이 들어오면 packet을 버릴 수 있는 별도 경합이
있다. 그러나 이번 실행은 **stop=false 상태에서 이미 정체**했으므로 이 경합을 관측된
timeout의 원인으로 채택하지 않았다. Dispatcher의 deadline 이후 queue clear도
`drain_timed_out`을 설정하므로 조용히 성공 처리하는 경로는 아니다.

준비 구간의 monitor close/HWM recalculate 순서는 요청에 적힌 차이 그대로 확인했다.
C++ `:513`은 monitor를 먼저 닫고 `:514`에서 recalculate한다. C server는 `:195`에서
HWM을 적용하고 `:196`에서 recalculate한 뒤 `:210` snapshot, `:212` monitor close를
수행한다. 이 순서는 변경하거나 이번 실패의 원인으로 판정하지 않았다.

STREAM runner는 `bindings/cpp/perf/run_comparison.py:2031`에서 client 실행 결과를
받은 뒤 `:2054`에서 server STOP을 보낸다. 진단 로그에서도 tail 대기 중 stop은 false였다.
`timeout_error=1`은 남은 echo가 정확히 하나라는 뜻이 아니다.
`bindings/c/perf/common/streamclient/perf_stream_bench_client.hpp:599`는 outstanding이
양수이면 `:600`에서 오류 계수를 한 번 증가시킨다. 공유 client 변경은 하지 않았다.

## 계수와 TCP 증거

`cpp_stream_drain_wire_counts` 실행에서 active window 종료 후 정체 중인 연결 100개의
`ss -tinp` 계수를 합산했다. 모든 연결은 ESTAB이었고 양쪽 kernel의 Recv-Q·Send-Q 합은 0이었다.

| 관측 | 값 |
|---|---:|
| client TCP bytes_sent / server TCP bytes_received | 35,347,185 B |
| server TCP bytes_sent / client TCP bytes_received | 35,344,350 B |
| server TCP bytes_acked | 35,344,350 B |
| 서버 packet 수신 / queue 등록 / 송신 완료 | 각각 436,350 |
| 서버 stale route / 미완료 송신 | 각각 0 |
| stop | false |

64 B case의 wire frame은 81 B다: prefix 6 B + message name 11 B + body 64 B.
근거는 `bindings/c/perf/common/streamclient/perf_stream_client_session.hpp:477`과
`perf_stream_common.hpp:95`다. 따라서 수신 TCP 바이트는 436,385 frame분이고,
packet API 반환과 실제 echo 송신은 436,350 frame분이다. 차이는 2,835 B, 즉 35 frame분이다.
각 연결의 snapshot은 원자적이지 않지만 이 시점의 packet 계수는 반복된 idle 로그에서
변하지 않았다. 계수는 누적 바이트 증거이며 개별 frame ID를 추적한 결과는 아니다.

```text
DRAIN_DIAG stop=0 recv=436350 queued=436350 sent=436350 stale=0 inflight=0
perf_stream_client: case_failed size=64 connect_ok=100 connect_fail=0
  send_error=0 recv_error=0 timeout_error=1 size_mismatch=0
  throughput_bps=27922240.000 samples=436285 window_ok=0
```

이 실행의 5초 tail 대기 동안 같은 계수가 유지됐다. Poller timeout 후 임시로 호출한
public `recv_packet(DONTWAIT)` probe에서도 추가 packet은 반환되지 않았다. 송신 wrapper가
성공을 잘못 반환했거나 dispatcher가 마지막 echo를 보관하고 있다는 가설과 맞지 않는다.

원시 자료는 `/tmp/cpp_stream_drain_wire_counts.log`와
`/tmp/cpp_stream_drain_wire_counts_ss.txt`에 있다. 최종 result는
`bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_002115_cpp_stream_drain_wire_counts.txt`다.

## 임시 대조 결과와 소유 계층

각 대조는 tcp, 64 B, duration 1, runs 1, client 100과 고정 Core를 유지했다.
진단 카운터가 포함된 수치는 성능 판정에 사용하지 않는다.

| results-tag | 변경한 진단 경계 | 결과 |
|---|---|---|
| `cpp_stream_drain_diag` | 수신·queue 등록·송신 완료·stale 계수 추가 | partial. stop=false, 수신=등록=완료=412,384, 미완료=0 |
| `cpp_stream_drain_probe` | poll timeout 때 public receive probe 추가 | partial. 추가 입력 없음, 수신=등록=완료=416,811 |
| `cpp_stream_drain_owner_diag` | queue 등록 대신 pull thread에서 기존 public async 송신 시작 | complete 1회. 단독 성공이며 최종 검증으로 세지 않음 |
| `cpp_stream_drain_native_diag` | dispatcher에서 새 native message를 만들어 `zlink_send_part_rid(DONTWAIT, FINAL)` 호출 | partial. 수신=등록=완료=421,668 |
| `cpp_stream_drain_native_io_diag` | 송신뿐 아니라 poller와 packet receive도 native C API 사용 | partial. 수신=등록=완료=435,691 |
| `cpp_stream_drain_native_bytes_diag` | native I/O를 유지하고 thread 간 packet 전달도 `std::vector<std::byte>`로 대조 | partial. 수신=등록=완료=470,446 |

Native 송신 대조는 non-OK 또는 nonzero wait token을 오류로 처리했다. 결과의 stale·미완료
계수는 0이었다. 오류를 무시하거나 새 retry로 통과시킨 실험이 아니다. Native I/O 대조에는
진단 목적의 내부 handle 접근이 포함되므로 public API만으로 구성된 독립 repro라고
부르지 않는다. 원본 C++ perf 재현 명령 자체는 public binding API를 사용한다.

현재 증거는 C++ async wrapper보다는 Core의 STREAM 입력 진행 경로를 가리킨다.
다만 native I/O 대조에서도 context/socket 초기 설정은 C++ wrapper를 유지했다.
따라서 모든 C++ 코드를 제거한 독립 C repro나 정확한 내부 결함 statement까지 확정한 것은 아니다.

Core 소유 계약은 `core/doc/spec/core/socket/08-stream.ko.md` §6.2의 packet output·ownership과
§7의 thread safety, `core/doc/spec/core/socket/README.ko.md` §2다. 공개 계약은 thread를
분리한 send를 금지하지 않는다. 같은 thread로 옮겨 한 번 통과했다는 결과만으로
dispatcher 구조를 계약 위반으로 판정할 근거는 부족하다.

고정 Core와 같은 revision이며 clean인 `/home/hep7/project/zlink-core-0171`을 읽기만 했다.
후속 Core 조사 경계는 다음과 같다.

- `core/src/runtime/sockets/stream/stream.cpp:595`: raw pipe 입력을 packet queue로 옮기는 pump.
- 같은 파일 `:685`, `:691`: packet queue가 비었을 때 pump를 호출하고 EAGAIN을 반환하는 경계.
- 같은 파일 `:717`, `:731`: 공개 packet 수신으로 이어지는 경계.
- TCP/session 입력 buffer에서 이 pump까지의 전달·진행 조건은 아직 특정하지 못했다.

실패한 일부 진단에서는 STOP 이후 `core/src/runtime/sockets/internal/fq.cpp:39`의
`_pipes.empty()` assertion도 관측했다. 이는 client tail 실패보다 뒤의 사건이며,
같은 원인이라고 확정하지 않았다.

## 요청한 검증

공통 실행 환경과 명령은 다음과 같다. 각 호출 앞에서 idle script를 실행했다.

```bash
export ZLINK_CORE_SOURCE=release
export ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1
export PERF_DEBUG=1
bash scripts/perf/wait-for-idle-perf.sh
bash bindings/cpp/perf/run_benchmarks_multi.sh \
  --pattern MULTI_STREAM --transports tcp --msg-sizes 64 \
  --duration 1 --runs 1 --reuse-build --results-tag cpp_stream_drain_before
```

| 검증 항목 | 결과 |
|---|---|
| 1. 원본 재현 명령 | partial. connect_ok=100, send/recv_error=0, timeout_error=1, samples=416,480 |
| 2. 수정 후 같은 명령 연속 3회 complete | 미달성. 최종 수정 후보가 없어 연속 3회 검증을 실행하지 않음 |
| 3. STREAM 64,256,1024,65536 | 원본 복구 후 partial. 첫 64 B에서 실패하여 뒤 크기로 진행하지 못함. report의 네 실패 행은 같은 size=64 실패를 전파한 것이며 네 크기의 독립 실패가 아님 |
| 4. 대상 외 tcp 64 B smoke | 원본 복구 후 MULTI_DEALER_DEALER complete, MULTI_PUBSUB complete. 각 success=1, fail=0, result 5/5 |

최종 상태 검증의 result 파일:

- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_000630_cpp_stream_drain_before.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_001711_cpp_stream_drain_final_sizes.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_001806_cpp_stream_drain_regression_dd.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_001822_cpp_stream_drain_regression_pubsub.txt`

남은 작업은 Core 수신 경로에서 35 frame분이 packet output으로 진행하지 못한 내부 조건을
특정하는 것이다. 러너 수정으로 해결됐다고 판정하거나 commit할 변경은 없다.
