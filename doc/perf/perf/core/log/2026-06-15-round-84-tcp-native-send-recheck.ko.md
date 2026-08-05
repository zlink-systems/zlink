# Round 84: TCP native send 후보 재검토

## 목적

이전 round에서 보류했던 `tcp_transport_t::write_some()` native `send()` 후보를 다시 확인했다.
사용자가 기준을 May26 보정 baseline으로 정정했고, 작은 개선이라도 하락 항목이 없으면 채택할 수
있는지 재검토하자는 방향을 제시했기 때문이다.

## POSD 점검

- 이 후보는 새 캐시나 상태를 추가하지 않는다.
- TCP `read_some()`이 이미 native `recv()`를 쓰고 있으므로, `write_some()`도 같은 수준의
  transport 내부 구현 선택으로 볼 수 있다.
- 공개 API, socket 계약, runner 조건은 바꾸지 않는다.
- 따라서 설계 복잡도 관점에서는 비교적 깨끗한 후보지만, 성능 개선 근거가 약하면 채택하지 않는다.

## 적용한 변경

- 파일: `core/src/runtime/transports/tcp/tcp_transport.cpp`
- `tcp_transport_t::write_some()`에서 Boost.Asio `socket.write_some()` 대신 native `send()`를
  호출하도록 임시 변경했다.
- Linux/Unix는 `MSG_DONTWAIT`와 `MSG_NOSIGNAL`을 사용했다.
- Windows는 `send()` 결과와 `WSAGetLastError()`를 기존 errno 의미로 매핑했다.

## 검증

### build

```bash
cmake --build core/build -j$(nproc)
```

- 결과: 통과

### test

```bash
ctest --test-dir core/build --output-on-failure -R 'test_(stream_|multi_stream_server_reassembly|transport_matrix|zmp_request_reply|multi_socket_contract_regressions)$'
```

- 결과: 4/4 통과
- 정규식이 실제 STREAM 테스트 일부를 놓쳐서 아래 명령으로 다시 실행했다.

```bash
ctest --test-dir core/build --output-on-failure -R 'test_stream|test_multi_stream_server_reassembly|test_transport_matrix|test_zmp_request_reply'
```

- 결과: 23/23 통과

### perf: STREAM/tcp 단독

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp --duration 5 --runs 5 --connect-ready-timeout-ms 5000 --results-tag round84_tcp_native_send_stream_tcp
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_142648_round84_tcp_native_send_stream_tcp.txt`
- load_avg: `6.29 10.49 10.93`
- `STREAM/tcp/64B`: `329,594.0 ops/s`

### perf: STREAM 전체 전송

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 bindings/c/perf/run_benchmarks_multi.sh --reuse-build --pattern STREAM --transports tcp,tls,ws,wss --duration 5 --runs 3 --connect-ready-timeout-ms 5000 --results-tag round84_tcp_native_send_stream_all
```

- report: `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_142708_round84_tcp_native_send_stream_all.txt`
- load_avg: `4.50 9.81 10.70`

| case | May26 full | round70 current | round84 candidate | vs May26 full | vs round70 |
|------|------------|-----------------|-------------------|---------------|------------|
| STREAM/tcp/64B | 305,177.4 | 332,250.4 | 330,880.6 | +8.42% | -0.41% |
| STREAM/tls/64B | 214,574.6 | 217,262.2 | 226,787.6 | +5.69% | +4.38% |
| STREAM/ws/64B | 251,311.4 | 282,312.6 | 270,375.6 | +7.59% | -4.23% |
| STREAM/wss/64B | 184,722.2 | 177,889.2 | 190,415.0 | +3.08% | +7.04% |

## 판정

- 핵심 목표인 `STREAM/tcp/64B`가 round70 현재값보다 낮다.
- May26 full baseline 대비로는 개선처럼 보이지만, 이미 현재 checkout에서 달성하던 수준을 넘지 못했다.
- `ws` 값도 round70보다 낮게 나와서, 이 후보를 "하락 항목 없음"으로 볼 수 없다. 이 후보가 WS 코드를
  직접 바꾸지는 않지만, 채택 근거로 쓰기에는 측정 결과가 충분히 깨끗하지 않다.
- 따라서 native `send()` 후보는 채택하지 않고 되돌렸다.

## 현재 상태

- source diff 없음.
- 이 후보는 POSD 관점에서는 나쁘지 않지만, 목표 성능 개선으로 인정할 근거가 부족하다.
