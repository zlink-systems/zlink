# Core 0.17.1 — STREAM socket이 TCP로 받은 frame을 packet API로 반환하지 않고 정체한다

- 보고: 머신 A (bindings 0.17.0 성능 캠페인)
- 대상 Core: 고정 prefix `~/.cache/zlink/core-pinned/0.17.1`, 태그 `core/v0.17.1`, revision `4cd03b917304ea69d2744fcc4bf29fd528dc7b1f`, Build ID `101bdb24…`, `core_dirty=0`
- 분류: **Core 수신 경로 결함**(binding 결함 아님 — 아래 3번 근거)
- 영향: C++·.NET의 `MULTI_STREAM` perf 측정이 불가능하다. 두 언어의 Multi 표에 `미측정`이 남는다.

## 1. 증상

`MULTI_STREAM` tcp 64 B, clients 100, duration 1 s. active window가 끝난 뒤 client가 마지막 echo 하나를 못 받고 timeout한다.

```
perf_stream_client: case_failed size=64 connect_ok=100 connect_fail=0
  send_error=0 recv_error=0 timeout_error=1 size_mismatch=0
  throughput_bps=27922240.000 samples=436285 window_ok=0
```

연결 100/100 성공, 송수신 오류 0, 크기 불일치 0. 436,285 샘플이 정상으로 흐른 뒤 **timeout 하나**로 실패한다. 재현성이 있다.

## 2. 결정적 증거 — TCP는 받았는데 packet API가 반환하지 않는다

정체 중인 연결 100개의 `ss -tinp` 계수를 합산했다. **모든 연결이 ESTAB이고 양쪽 kernel의 Recv-Q·Send-Q 합은 0**이다. 즉 커널에 남은 데이터가 없다.

| 관측 | 값 |
|---|---:|
| client TCP `bytes_sent` / server TCP `bytes_received` | 35,347,185 B |
| server TCP `bytes_sent` / client TCP `bytes_received` | 35,344,350 B |
| server TCP `bytes_acked` | 35,344,350 B |
| 서버 packet 수신 / queue 등록 / 송신 완료 | 각각 436,350 |
| 서버 stale route / 미완료 송신 | 각각 0 |
| `stop` 플래그 | **false** |

64 B case의 wire frame은 81 B다(prefix 6 + message name 11 + body 64; `bindings/c/perf/common/streamclient/perf_stream_client_session.hpp:477`, `perf_stream_common.hpp:95`).

- 수신 TCP 바이트 35,347,185 B = **436,385 frame**
- packet API 반환·echo 송신 = **436,350 frame**
- 차이 2,835 B = **정확히 35 frame**

**이 35 frame은 TCP로 서버에 도착해 커널 버퍼에서 빠져나갔지만 packet API가 끝내 반환하지 않았다.** 5초 tail 대기 동안 계수가 변하지 않았고, poller timeout 뒤 public `recv_packet(DONTWAIT)` probe를 직접 호출해도 추가 packet이 없었다. `stop=false`이므로 종료 경합이 아니다.

## 3. binding 결함이 아닌 근거

- C++ 송수신·poller wrapper를 **native C API로 전부 대체한 진단에서도 같은 실패**가 재현됐다. C++ binding의 message 이동·async wrapper로 설명되지 않는다.
- 서버가 반환받아 송신한 echo는 **전부 client TCP에 도착**했다(bytes_acked 일치). 송신 경로는 정상이다.
- stale route 0, 미완료 송신 0.

## 4. C 러너는 통과하는데 C++만 실패하는 차이

| 경계 | C | C++ |
|---|---|---|
| 수신 후 echo 제출 | `bindings/c/perf/multi/common/perf_multi_stream_session.hpp:363,371` — 수신한 packet을 **같은 event-loop thread**에서 제출 | `bindings/cpp/perf/multi/src/perf_stream_server.cpp:243,252` — packet을 queue에 넣고 `:280,312`의 **별도 dispatcher thread**가 제출 |

즉 트리거는 **같은 STREAM socket에 대해 pull loop thread의 recv와 dispatcher thread의 send가 동시에 진행되는 구성**으로 보인다. 이 구성은 공개 계약이 금지하지 않는다(`core/doc/spec/core/socket/README.ko.md:49` — "`send`는 여러 thread에서 동시 호출을 허용하는 hot path"). Java·Rust·Go는 같은 공유 raw client 바이너리를 쓰면서 통과하므로 client 쪽 문제도 아니다.

**이 서명은 D-BP12(같은 socket 동시 multipart 제출)와 D-B209/D-B210(completion drain owner 공백 — "queue로 옮길 주체가 없다")과 같은 계열로 보인다.** 0.17.2의 MP-7 수정이 이미 덮었을 가능성이 있으니 먼저 그것부터 확인해 주기 바란다.

## 5. 조사 경계 (결함 statement로 확정한 위치가 아님)

고정 Core와 같은 revision의 clean 체크아웃 `/home/hep7/project/zlink-core-0171`을 **읽기만** 했다.

- `core/src/runtime/sockets/stream/stream.cpp:595` — raw pipe 입력을 packet queue로 옮기는 pump
- 같은 파일 `:685`, `:691` — packet queue가 비었을 때 pump를 호출하고 EAGAIN을 반환하는 경계
- 같은 파일 `:717`, `:731` — 공개 packet 수신으로 이어지는 경계
- TCP/session 입력 buffer에서 이 pump까지의 전달·진행 조건은 특정하지 못했다

일부 진단에서 STOP 이후 `core/src/runtime/sockets/internal/fq.cpp:39`의 `_pipes.empty()` assertion도 관측했다. client tail 실패보다 뒤의 사건이며 같은 원인으로 확정하지 않았다.

## 6. 재현

```bash
export ZLINK_CORE_SOURCE=release
export ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.1
export PERF_DEBUG=1
bash bindings/cpp/perf/run_benchmarks_multi.sh --pattern MULTI_STREAM --transports tcp \
  --msg-sizes 64 --duration 1 --runs 1 --reuse-build --results-tag streamsmoke
```

`status: partial`, 실패 사유 `non_zero_exit_2_perf_stream_client: case_failed ... timeout_error=1`.

원시 자료: `/tmp/cpp_stream_drain_wire_counts.log`, `/tmp/cpp_stream_drain_wire_counts_ss.txt`, report `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_002115_cpp_stream_drain_wire_counts.txt`. 진단 기록 `doc/perf/perf/bindings-0.17.0/log/2026-09-08-cpp-stream-drain.ko.md`.

## 7. 요청

1. 0.17.2의 MP-7 수정으로 이 증상이 사라지는지 먼저 확인해 달라.
2. 사라지지 않으면 위 pump 경계에서 원인을 특정해 0.17.2에 포함해 달라.
3. **회귀 테스트를 Core 통합 테스트에 추가해 달라** — 같은 STREAM socket에서 한 thread가 pull로 packet을 받는 동안 다른 thread가 echo를 send하고, 마지막 frame까지 packet API로 반환되는지. D-BP15 때와 같은 형태의 요청이다.

머신 A는 Core를 수정하지 않으며, 하위 계층 결함을 러너에서 보상하지 않는다(§5). 0.17.2가 나오면 이 셀을 다시 잰다.

---

## 8. 0.17.2 확인 결과 — **재현된다** (머신 A, 2026-09-08 04:55)

§7의 첫 요청("0.17.2의 MP-7 수정으로 이 증상이 사라지는지 먼저 확인해 달라")에 대한 답이다.

고정 prefix를 **0.17.2로 재고정**해 같은 재현 절차를 돌렸다.

- prefix `/home/hep7/.cache/zlink/core-pinned/0.17.2`
- 태그 `core/v0.17.2`, revision `dca377aa5e`
- Build ID `c5dec25e86de575e8efa79c5327b1e3d0d424f3c`, SHA-256 `72d508f73a5d261f…`
- 같은 prefix로 `MULTI_DEALER_DEALER` tcp 64 B smoke는 `status: complete`(재고정 자체는 정상)

`MULTI_STREAM` tcp 64 B 결과는 **0.17.1과 동일**하다:

```
- status: partial
- MULTI_STREAM current tcp 64B: non_zero_exit_2_perf_stream_client: case_failed size=64
  connect_ok=100 connect_fail=0 send_error=0 recv_error=0 timeout_error=1 size_mismatch=0
  throughput_bps=28976128.000 samples=452752 window_ok_size_64
```

연결 100/100 성공, 송수신 오류 0, 크기 불일치 0, 452,752 샘플이 정상으로 흐른 뒤 timeout 하나로 실패한다.

**따라서 §7의 2번과 3번 요청이 유효하다.** MP-7의 completion pull 수정은 이 증상을 덮지 않았다. §5의 pump 경계(`core/src/runtime/sockets/stream/stream.cpp:595,685,691,717,731`)에서 원인을 특정해 다음 릴리스에 포함하고, 회귀 테스트를 Core 통합 테스트에 추가해 주기 바란다.

머신 A는 C++·.NET의 `MULTI_STREAM` 4 transport를 계속 `보류(Core 결함)`로 두고 다음 릴리스에서 다시 잰다.
