# C perf Multi STREAM 10,000 접속 가능성 확인 (2026-09-08)

## 결론

- 고정 Core `release/0.17.3`에서 TCP transport 연결 자체는 10,000개가 약 1.03초에
  모두 성립했다. raw client의 connect callback도 10,000/10,000 성공했고 nofile, ephemeral
  port, 메모리 guard는 제한 원인이 아니었다.
- 측정 계약의 `SERVER_START_READY`는 5,000부터 실패했다. 서버가 관찰한
  `CONNECTION_READY`는 5,000 중 3,807개, 최종 10,000 측정에서 997개였다
  (앞선 10,000 진단 측정은 1,024개).
- 분류는 **B(러너/벤치 orchestration)** 이다. C STREAM 서버는 raw client의 모든 연결이 끝나
  runner가 `START`를 보낼 때까지 bounded/lossy Core monitor를 소비하지 않는다. Core spec이
  정한 overflow 동작과 일치하므로 Core 결함(C)으로 분류하지 않는다.
- HWM이나 connect concurrency를 바꾸지 않고 START 전 monitor drain과 event `value` 집계를
  시험했지만 5,000에서 최종 value가 3,872에 머물렀다. Core worker queue에서 burst 후반
  event가 이미 폐기되어 공개 API만으로 목표 count를 복구할 수 없었다. 측정 조건 변경이
  금지됐으므로 시험 코드는 되돌렸고 러너 소스 변경은 남기지 않았다.
- 10,000 단일 크기가 ready에 도달하지 못했으므로 `s10k-full` 6크기 측정은 실행하지 않았다.

## 1. 사전 자원 guard

근거 코드는 `bindings/c/perf/run_benchmarks_multi.sh:188-240`, `:248-329`,
`:656-666`, `:1380-1396`이다.

| 항목 | 계산/현재 값 | 10,000 판정 |
|---|---:|---|
| nofile 필요량 | `clients * 3 + 4096` = 34,096 | soft/hard 1,048,576이므로 통과 |
| MemAvailable | 90,296,932 KiB | 입력 값 정상 |
| memory usable | `MemAvailable * 70 / 100` = 63,207,852 KiB | base 512 MiB 차감 |
| memory max clients | `(63,207,852 - 524,288) / 1,024` = 61,214 | 10,000 이하이므로 통과 |
| connect concurrency | clients 10,000 이상이면 1,024, 그 미만은 128 | 1k=128, 5k=128, 10k=1,024 |
| IPv4 ephemeral port | 1024-65535, 64,512개 | 10,000 loopback client에 충분 |
| somaxconn | 65,535 | 10,000보다 큼 |
| fs.file-max | 2,097,152 | nofile 필요량보다 큼 |

`ulimit -n` 변경은 필요하지 않아 실행하지 않았다. sysctl도 변경하지 않았다.

Preflight 로그:

- `.artifacts/perf-queue/log/1-1788864784-69145-codex-stream10k-STREAM_10k____________________________.log`
- `.artifacts/perf-queue/log/1-1788865297-89696-codex-stream10k-STREAM_10k___________________________.log`

## 2. 단계별 결과

모든 단계는 `MULTI_STREAM`, `tcp`, 64 B, duration 5초, 1-run이며
`/home/hep7/.cache/zlink/core-pinned/0.17.3/lib/libzlink.so.0.17.3`을 사용했다.
연결 완료 시간은 티켓 시작 뒤 전역 TCP ESTABLISHED 행이 단계 시작 baseline보다
`clients * 2` 증가한 최초 snapshot 시각이다. loopback 연결의 client/server 양 끝을 세므로
2배를 사용했다.

| clients | TCP 전부 established | raw client connect | server ready | monitor 관찰 | 처리량 | 오류 |
|---:|---:|---:|---|---:|---:|---|
| 1,000 | 419 ms | 1,000/1,000 | 성공 | 1,000/1,000 | 447,102.2 ops/s | 없음 |
| 5,000 | 625 ms | 5,000/5,000 | 실패 | 3,807/5,000 | RESULT 없음 | `connection-ready barrier failed` |
| 10,000 | 1,033 ms | 10,000/10,000 | 실패 | 997/10,000 | RESULT 없음 | `connection-ready barrier failed` |

### `ss` snapshot

`ss -tan | wc -l`은 다른 프로세스 연결과 앞 단계 TIME_WAIT를 포함하는 전역 행 수다.

| clients | 시작 | 목표 established 시점 | 종료 | 목표 시점 `ss -s` estab |
|---:|---:|---:|---:|---:|
| 1,000 | 64 | 2,065 | 1,057 | 2,012 |
| 5,000 | 1,053 | 11,054 | 6,051 | 10,014 |
| 10,000 | 100 | 20,101 | 10,104 | 20,008 |

### stdout/stderr

- 1,000: server stderr 없음. client debug stderr의
  `on_connect_result success=1`은 1,000개이고 실패 callback은 없다. throughput 포함 RESULT
  5종이 출력됐다.
- 5,000: client 성공 callback 5,000개, 실패 callback 없음. server stderr는
  `[multi-stream-server] connection-ready barrier failed ready=3807 expected=5000`이다.
- 10,000: client 성공 callback 10,000개, 실패 callback 없음. server stderr는
  `[multi-stream-server] connection-ready barrier failed ready=997 expected=10000`이다.
- Core/API errno, `ACCEPT_FAILED`, client connect 오류는 관찰되지 않았다.

단계 로그:

- 1,000: `.artifacts/perf-queue/log/1-1788865555-4268-codex-stream10k-MULTI_STREAM_tcp_clients_1000_size_64_du.log`
- 5,000: `.artifacts/perf-queue/log/1-1788865604-6439-codex-stream10k-MULTI_STREAM_tcp_clients_5000_size_64_du.log`
- 10,000: `.artifacts/perf-queue/log/1-1788866616-39086-codex-stream10k-MULTI_STREAM_tcp_clients_10000_exact_s10.log`

결과 파일:

- `/home/hep7/.cache/zlink/stream10k/results/multi/report/perf_c_multi_linux_20260908_200558_s10k-1000.txt`
- `/home/hep7/.cache/zlink/stream10k/results/multi/report/perf_c_multi_linux_20260908_200648_s10k-5000.txt`
- `/home/hep7/.cache/zlink/stream10k/results/multi/report/perf_c_multi_linux_20260908_202812_s10k-10000.txt`

## 3. 분류 근거

### A(OS/환경) 기각

nofile와 메모리 guard가 모두 통과했고 10,000 TCP 연결이 실제 ESTABLISHED 상태에 도달했다.
client connect callback도 전부 성공했다. 따라서 port 고갈, fd 제한, 메모리 guard skip이나
WSL2 accept 한계가 이번 실패를 설명하지 않는다.

### B(러너/벤치 orchestration) 채택

1. `bindings/c/perf/multi/src/perf_multi_stream_server.cpp:147-182`는 monitor를 연 뒤
   `wait_for_start_from_stdin()`을 먼저 호출하고, START를 받은 뒤에야
   `wait_connect_ready_count()`로 monitor를 소비한다.
2. `bindings/c/perf/common/streamclient/perf_stream_bench_client.hpp:203-224`는 모든 raw
   client connect가 완료된 뒤 size barrier를 진행하고, `:682-697`에서 전부 연결됐을 때만
   `CLIENT_READY`를 출력한다. runner는 그 뒤에 START를 전달한다.
3. `bindings/c/perf/multi/common/perf_common.hpp:188-196`은 monitor HWM을 고정된
   4,096,000 bytes로 연다. `:325-388`의 drain/wait는 START 이후 실행된다.
4. Core monitor 계약 `core/doc/spec/core/06-monitoring.ko.md:103-128`은 queue가 bounded/lossy이며
   가득 차면 새 record를 버리고 producer를 block하지 않는다고 명시한다. 이번 누락은 이 계약과
   맞는다.
5. `bindings/c/perf/multi/common/perf_common.hpp:139-150`은 전달된
   `CONNECTION_READY` event 수를 세지만 Core 계약 `core/doc/spec/core/06-monitoring.ko.md:79-95`의
   `event.value`는 현재 ready transport count다. 다만 START 전 drain과 value 사용을 함께
   시험해도 5,000 중 3,872까지만 전달되어, 이미 폐기된 마지막 상태를 공개 API로 복구하지 못했다.

현재 계약을 유지한 채 가능한 수정은 확인되지 않았다. 통과시키려면 적어도 monitor queue
예산 또는 connect admission 방식을 바꿔야 하며, 둘 다 이번 과제에서 금지된 측정 조건 변경이다.

### C(Core) 기각

Core public API 순서는 `zlink_socket()` → `zlink_socket_monitor_open()` → `zlink_bind()` →
raw TCP accept/handshake → `zlink_socket_monitor_recv(DONTWAIT)`이다. 실제 TCP transport는 모두
성립했고 Core monitor overflow는 명시된 lossy 동작이다. Core 오류 코드나 실패 event는 없었다.
따라서 머신 B 이관용 Core 결함 문서는 만들지 않았다.

## 4. 변경과 검증

- 최종 소스 diff는 이 기록 파일뿐이다.
- 진단 중 START 전 monitor drain과 `CONNECTION_READY.value` 집계를 시험했으나 5,000 실패가
  유지되어 두 소스 변경을 모두 되돌렸다.
- 실험 뒤 `comp_src_stream_server` target을 다시 증분 빌드해 repository source와 실행 바이너리를
  맞췄다. Core 소스 수정과 Core 재빌드는 하지 않았다.
- 10,000 ready 실패 때문에 `s10k-full`은 실행하지 않았다.

## 5. 티켓과 rc

| 티켓 | 목적 | rc |
|---|---|---:|
| `1-1788864784-69145-codex-stream10k-STREAM_10k____________________________` | preflight 1차 | 0 |
| `1-1788865297-89696-codex-stream10k-STREAM_10k___________________________` | 메모리 계산 재확인 | 0 |
| `1-1788865555-4268-codex-stream10k-MULTI_STREAM_tcp_clients_1000_size_64_du` | 1,000 단계 | 0 |
| `1-1788865604-6439-codex-stream10k-MULTI_STREAM_tcp_clients_5000_size_64_du` | 5,000 단계 | 1 |
| `1-1788865703-12916-codex-stream10k-MULTI_STREAM_tcp_clients_10000_size_64_d` | 10,000 진단 측정(`s10k-10000-prefix`) | 1 |
| `1-1788865911-20293-codex-stream10k-MULTI_STREAM_tcp_clients_5000_size_64_po` | START 전 drain 실험(초기 stale binary) | 1 |
| `1-1788865951-22795-codex-stream10k-C_perf_MULTI_STREAM_server_target_increm` | C perf server 증분 빌드 | 0 |
| `1-1788865983-24390-codex-stream10k-MULTI_STREAM_tcp_clients_5000_size_64_re` | START 전 drain 실험 | 1 |
| `1-1788866123-27704-codex-stream10k-C_perf_MULTI_STREAM_server_rebuild_after` | value 집계 실험 빌드 | 0 |
| `1-1788866154-29202-codex-stream10k-MULTI_STREAM_tcp_clients_5000_size_64_ag` | drain+value 집계 실험 | 1 |
| `1-1788866337-32313-codex-stream10k-Restore_C_perf_STREAM_server_binary_to_r` | 원본 소스 상태로 C perf server 재빌드 | 0 |
| `1-1788866537-35508-codex-stream10k-Remove_bounded_STREAM_10k_temporary_snap` | 1차 임시 snapshot 정리 | 0 |
| `1-1788866556-36569-codex-stream10k-Final_STREAM_10k_report_and_scope_verifi` | 1차 최종 범위 검증 | 0 |
| `1-1788866616-39086-codex-stream10k-MULTI_STREAM_tcp_clients_10000_exact_s10` | 10,000 단계(`s10k-10000`) | 1 |
| `1-1788866960-50650-codex-stream10k-Remove_final_exact-tag_STREAM_10k_snapsh` | 최종 임시 snapshot 정리 | 0 |
