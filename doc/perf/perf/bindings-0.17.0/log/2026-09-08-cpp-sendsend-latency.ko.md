# C++ Multi SENDSEND latency 진단 — 2026-09-08

## 판정 범위

이 기록은 감독자가 C++ perf 러너 수정 여부와 다음 조사 범위를 판단하기 위한 자료다.
대상은 TLS RR 1024 B, WS RR 64 B, WSS DR/RR 256 B다. WS DR 1024 B는 제외한다.
**TLS RR 1024 B의 대량 체류 위치는 server의 OS TCP 송신 queue다.**
같은 5-run에서 정상 실행은 최대 232,371 bytes, 이상 실행은 766,533,530 bytes였다.
Relay `pending`은 최대 1건, coroutine suspend는 0회다. 같은 고정 Core의 C 러너에서도
latency 폭증을 새로 재현했다. 허용된 C++ 러너에서 고칠 결함은 확인하지 못했으며
Core·transport 또는 OS 경계의 원인 수정은 이번 요청의 금지 범위다. 동작 수정은 적용하지 않았다.
**이 새 C 표본은 기존 C 정상 표본을 대체하는 성능 기준선이 아니다.**

## 실행 조건과 원자료

- Branch `main`. 시작 HEAD `99cc3482f8`. 기존 Framework untracked smoke 디렉터리를 보존한다.
- `ZLINK_CORE_SOURCE=release`, `ZLINK_CORE_PACKAGE_PREFIX=/home/hep7/.cache/zlink/core-pinned/0.17.2`.
- Core runtime `lib/libzlink.so.0.17.2`, revision `dca377aa5e58202e1586bbac8d93329037a85e7d`.
- Core SHA-256 `72d508f73a5d261ff608c7ac49b5b55e7277e18e15fe4b5ac51affc0468012d3`.
- C++ build cache가 0.17.1을 가리켜 기존 `bindings/cpp/build`를 지정된 release prefix로
  다시 configure하고 C++ perf target만 빌드했다. Core는 빌드·수정하지 않았다.
- C는 `bindings/c/build-release-0.17.2`. 측정은 모두 `--reuse-build`, clients 100,
  I/O threads 4/4, balanced auto-HWM, 2-part, duration 5 s, runs 5다.
- 모든 실행 전에 `bash scripts/perf/wait-for-idle-perf.sh`를 호출하고 load ≤ 5를 확인했다.
  `diag2` 시작 1초 뒤 외부 C WSS DD 측정이 시작된 사실을 PID 시작 시각으로 확인했다.
  **`diag2`는 동시 실행 오염 때문에 성능 판정에서 제외한다.** 이후 외부 측정이 끝날 때까지 기다렸다.
- 전체 로그·임시 계측 코드·분석 JSON: `/tmp/zlink-cpp-sendsend-latency/`.
  진단 계측은 성능 최적화 after로 간주하지 않는다.

## TLS RR 1024 B 재현

값은 각 5-run 안의 개별 mean latency(ms)다. 공식 report의 aggregate는 이 값들의 중앙값이다.

| 실행 | Run 1 | Run 2 | Run 3 | Run 4 | Run 5 | 중앙값 |
|---|---:|---:|---:|---:|---:|---:|
| C++ 원본, `sslat-before` | 18.489 | 378.895 | 1.803 | 21.550 | 2.213 | 18.489 |
| C++ 계측, `sslat-diag1` | 0.790 | 369.401 | 199.556 | 76.631 | 69.617 | 76.631 |
| C 원본, `sslat-cbase` | 0.392 | 77.774 | 0.306 | 224.933 | 25.689 | 25.689 |

시작 load는 각각 4.06, 2.45, 1.98이다. 세 report 모두 `complete`다.
C의 정상·이상 실행이 한 5-run 안에서 함께 나타났다. 따라서 기존 C 정상 셀만으로
이 증상을 C++ 전용 결함이라고 확정할 수 없다. C++ 원본과 새 C 중앙값을 나누어 통과로
판정하지 않는다. 감독자가 제공한 정상 C latency 0.56 ms와 비교하면 C++ 원본 중앙값은 약 33.0배다.

원 report:

- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_074244_sslat-before.txt`
- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_074505_sslat-diag1.txt`
- `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_074712_sslat-cbase.txt`
- 오염되어 제외한 자료: `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_074815_sslat-diag2.txt`

## Relay와 client 계측

`diag1`의 정상 Run 1과 이상 Run 2를 비교한다. Timestamp는 원 payload를 바꾸지 않고
1024건마다 표본을 읽는다. 모든 admitted echo를 받은 뒤 종료하는 기존 동작을 유지한다.

| 관측 | 정상 Run 1 | 이상 Run 2 |
|---|---:|---:|
| Latency mean, ms | 0.790 | 369.401 |
| relay 수신·admission 수 | 1,814,167 | 2,281,700 |
| 즉시 admission 수 | 1,814,167 | 2,281,700 |
| suspend 수 | 0 | 0 |
| relay `pending` 최대 | 1 | 1 |
| client stamp → relay recv 평균, µs | 289.557 | 362.420 |
| relay enqueue → admission 평균, µs | 12.481 | 12.936 |
| relay poll 횟수 | 31,799 | 21,932 |
| relay recv turn 최대 건수 | 8,493 | 14,393 |
| client active coordinator poll 횟수 | 18,142 | 22,817 |
| 그중 zero-timeout poll | 18,142 | 22,817 |
| client poll 진입 사이 최대 간격, µs | 5,249 | 3,044 |
| client 한 socket recv drain 최대 건수 | 13 | 45 |
| relay monitor send pending 최대, bytes | 184,320 | 185,472 |
| relay monitor recv pending 최대, bytes | 3,995,136 | 2,237,184 |
| client monitor recv pending 합계 최대, bytes | 279,936 | 267,264 |

relay sample은 Core snapshot 조회 비용도 포함하므로 약 13 µs를 순수 send 비용이라고
해석하지 않는다. 정상·이상 실행 모두 `pending` 최대 1이고 전부 즉시 admission이다.
이 재현에서는 제시된 relay deque 가설과 backpressure 경계의 suspend 증가 가설이 배제된다.
Client poll도 수백 ms 동안 멈추지 않았다. 다만 작은 socket snapshot만으로 Core 내부 queue를
배제할 수는 없다. 현재 `socket_base_monitor.cpp`는 `uses_registry_accounting()`인 pipe를
제외하므로 공개 context budget snapshot을 함께 확인해야 한다.

## `await_ready()`와 계층 대조

- `bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp:80`:
  `await_ready()`는 `_state->ready()`를 반환한다.
- `bindings/cpp/src/Runtime/Messaging/send_operations.cpp:77`:
  즉시 admission이면 `immediate_send_result_t`를 반환한다.
- `bindings/cpp/src/Runtime/Messaging/async_operation_state.hpp:76`:
  그 state의 `ready()`는 `true`, `suspend()`는 `false`다.
- 실제 계측도 정상·이상 실행 모두 suspend 0회를 확인했다.
- C의 `perf_multi_relay_server.hpp:331`은 FIFO 앞 reply가 admit되는 만큼 연속 제출한다.
  C++ 단일 sender도 즉시 admission에서는 같은 turn에 다음 reply를 제출한다.
- 계약 소유권: `bindings/doc/spec/README.ko.md`의 Submit 결과 투영은 SEND OK/ID 0의
  즉시 terminal과 BACKPRESSURED 재제출을 binding에 맡긴다.
  `bindings/doc/spec/async-execution-model.ko.md` §4는 public poller의 completion drain 소유권을 정의한다.
  확인한 C++ 동작은 이 계약과 일치한다.

## TLS 경계와 OS TCP 송신 queue

`sslat-tcpprobe`는 기존 러너 계측에 `LD_PRELOAD`로 `SSL_read`/`SSL_write` 관측을 추가한
진단이다. 원 함수를 그대로 호출하고, 성공한 plaintext에서 기존 metric header의 `seq % 1024 == 0`
표본을 읽는다. Payload·송신 순서·HWM·duration·client 수는 바꾸지 않는다.
이 표본은 active와 기존 teardown drain을 모두 포함하므로 active-only RESULT와 같은 평균이 아니다.
양쪽 process가 같은 수의 timestamp 표본을 기록했으며 값은 stamp 이후 경과 시간이다.

TLS 호출에서 1초마다 해당 process가 소유한 socket inode를 `/proc/self/fd`로 모으고
`/proc/net/tcp`의 ESTABLISHED 행과 대조했다. `tx_queue`는 TCP에 제출되어 아직 확인되지 않은
outstanding bytes이며, 아직 송신하지 않은 데이터도 포함한다. 합계는 process별 실제 socket만 센다.
이는 메시지 개수가 아니며 TLS·protocol overhead도 포함한다. 계측에는 sleep이 없다.

| 관측 | 정상 Run 2 | 이상 Run 3 |
|---|---:|---:|
| RESULT latency, ms | 0.741 | 367.469 |
| RESULT throughput, ops/s | 357,012 | 285,666 |
| 양쪽 SSL 표본 수 | 1,743 | 2,139 |
| client SSL_write 평균, µs | 113.581 | 231.145 |
| server SSL_read 평균, µs | 220.712 | 288.616 |
| server SSL_write 평균, µs | 422.244 | 447.382 |
| client SSL_read 평균, µs | 942.374 | 988,883.197 |
| server TCP tx_queue 최대 합계, bytes | 232,371 | **766,533,530** |
| server 단일 TCP tx_queue 최대, bytes | 6,327 | **11,252,578** |
| relay pending 최대 / suspend 수 | 1 / 0 | 1 / 0 |
| relay Core send pending 최대, bytes | 273,024 | 228,096 |
| relay Core recv pending 최대, bytes | 2,514,816 | 2,165,760 |
| relay context queue 표본 최대, bytes | 2,586,176 | 2,173,824 |
| client context queue 표본 최대, bytes | 225,600 | 289,216 |
| client socket recv pending 합계 최대, bytes | 343,296 | 372,096 |
| client active poll 횟수 | 17,856 | 21,906 |
| client poll 진입 사이 최대 간격, µs | 2,780 | 1,923 |
| client 한 socket drain 최대 건수 | 25 | 64 |

이상 Run 3 server의 TCP tx_queue 합계는 1초 간격으로
606 → 73,012,563 → 250,826,725 → 428,726,920 → 601,425,149 → 766,533,530 bytes로 증가했다.
전체 시계열은 `tcp-probe-14996.log`에 있다. 관측한 Core application queue에는
수백 MB가 없었으며, 대량 체류는 OS TCP 송신 queue에서 직접 관측됐다. 이미 TLS write를
통과한 동일 timestamp가 client SSL_read에 도달할 때 약 1초 경과한 관측이 이를 뒷받침한다.
이미 client Core에 도착한 echo를 공개 recv에서만 수백 ms 늦게 꺼낸다는 설명은
이 경계 계측과 맞지 않는다. **Client의 recv 비용이 I/O 처리율에 미치는 영향이나
Core I/O 진행 속도까지 정상이라는 뜻은 아니다.**

HWM 대조:

- relay 공개 `send_hwm()`/`recv_hwm()`는 모두 4,096,000 bytes이며 socket auto-HWM snapshot의
  applied 값과 일치한다. 이 getter는 각각 native `ZLINK_OPT_SNDHWM`/`ZLINK_OPT_RCVHWM`에 대응한다.
- context snapshot의 `total_applied_hwm_bytes`는 209,715,200 bytes다.
  표의 context queue는 `core_queue_accounted_bytes`이며 Core application queue의 사용량이다.
  `application_accounted_bytes`는 ABI 예약 필드로 항상 0이다. Completion·monitor queue는
  별도 필드이므로 이 표로 Core의 모든 종류의 queue를 배제하지 않는다.
- 관측한 Core application queue 사용량은 TCP tx_queue의 수백 MB를 설명하지 못한다.
  따라서 Core HWM까지 메시지가 차서 async send가 suspend했다는 설명도 이 재현과 맞지 않는다.
- TLS는 TCP 위에서 동작하므로 TLS 셀의 OS queue가 TCP로 보이는 것은 정상이다.

이 5-run의 latency는 **31.735 / 0.741 / 367.469 / 125.630 / 117.310 ms**, 중앙값
117.310 ms다. 첫 실행의 server tx_queue 최대도 62,793,216 bytes로, 제시된 수만 건 규모의
적체에 대응하는 byte 규모를 직접 관측했다. 원 질문의 `throughput × latency`는 체류 건수의
추정이지 실제 queue count가 아니다. 러너가 기록하는 latency는 RTT의 절반이므로 RTT 기준
Little's law에 적용할 때는 2배를 반영해야 한다.

자료:

- `bindings/cpp/perf/results/multi/report/perf_cpp_multi_linux_20260908_081010_sslat-tcpprobe.txt`
- `/tmp/zlink-cpp-sendsend-latency/tcpprobe.log`, `tcp-queues.json`, `tls_probe.cpp`
- 정상 Run 2: `tcp-probe-13916.log`, `tcp-probe-13926.log`, `tls-probe-13916.log`, `tls-probe-13926.log`
- 이상 Run 3: `tcp-probe-14996.log`, `tcp-probe-15006.log`, `tls-probe-14996.log`, `tls-probe-15006.log`
- 앞선 TLS 경계 재현: `perf_cpp_multi_linux_20260908_080441_sslat-tlsprobe.txt`.
  Run 1~3의 process 종료 시각은 08:04:48 / 08:04:57 / 08:05:06이다.
  외부 .NET 측정이 08:05:11에 시작했으므로 그 묶음의 Run 4~5와 aggregate는 판정에서 제외한다.
  마지막 `sslat-tcpprobe`는 08:10:10 시작, 다음 외부 Go 측정은 08:11:42 시작으로 시간상 겹치지 않는다.

## Transport·크기 조건의 해석과 수정 범위

**TLS RR 1024 B에서는 같은 크기라도 OS 송신 queue가 작은 실행과 수백 MB까지 증가하는
실행이 함께 나타난다.** relay sender의 즉시 admission/suspend 전환이 원인이라는 가설은
관측과 다르다. Core admission 이후 server의 TLS 출력과 client의 TLS 입력 사이에서 적체가
증가하는 상태로 전환한다. 완료된 echo가 계속 나오면서 latency가 증가할 수 있는 이유다.

구조적 차이로 `ssl_transport_t`와 `ws_transport_t`는 `supports_speculative_write()`가
`false`이며 TCP는 이를 지원하는 별도 경로가 있다. 이는 transport에 따라 서비스 속도가
달라질 수 있는 코드 근거다. **어떤 조건이 처음 서비스 속도 차이를 만드는지, 특정 크기에서
왜 더 자주 발생하는지는 아직 확정하지 못했다.** OS receive window, TLS/WS I/O 처리량과
Core I/O scheduling 중 원인을 가르는 추가 조사는 후속 Core/OS 진단에서 다뤄야 한다.
이를 C++ 러너 수정으로 해결할 근거는 없으며 Core·transport 수정은 이번 요청에서 금지돼 있다.
WS RR 64 B와 WSS DR/RR 256 B까지 동일 원인이라고 일반화하지 않는다.

검토한 방향:

| 방향 | 판정 |
|---|---|
| relay FIFO·단일 sender 변경 | 기각. 이상 실행에서도 pending 최대 1이며 모든 send가 즉시 admission된다. |
| client in-flight 제한, HWM/buffer·duration 변경 | 기각. 원인을 숨기고 측정 조건을 바꾸며 사용자 제약에도 위반한다. |
| C++ binding async 수정 | 근거 없음. 즉시 terminal과 실제 suspend 수가 정상이고 native TLS 경계 이후에 지연된다. |
| Core/transport·OS queue 경계의 후속 진단·수정 | 필요. 이번 작업에서는 source 수정·재빌드하지 않았다. C 원본 공개 perf 경로로도 재현한다. |

C와 C++의 새 대조 결과 때문에 이 TLS 증상을 C++ binding 라이브러리 결함으로 보고하지 않는다.
Core나 OS 중 어느 구현의 특정 결함인지까지 확정한 것도 아니다. 확인한 것은 **queue 위치와
지연이 생기는 경계**, 그리고 기존 “같은 C 셀이 정상이라 Core/transport를 배제” 판단을
새 표본에 적용할 수 없다는 점이다.

## 요청한 검증의 상태

| 검증 | 상태 |
|---|---|
| TLS RR 1024 B, 수정 후 연속 5회 각 5-run latency ≤ 2.0x | **미충족.** 수정 전·진단 실행에서 폭증이 재현됐고 허용 범위의 수정 후보가 없다. 위의 여러 진단 5-run을 수정 후 연속 5회 검증으로 세지 않는다. |
| WS RR 64 B, WSS DR/RR 256 B 각각 3회 latency ≤ 2.0x | 미실행. TLS의 원인 경계를 확인했으나 수정이 없어 after 판정을 하지 않았다. |
| TCP SENDSEND 2종 전 크기 5-run, throughput ≥ 91%·latency ≤ 1.4x | 미실행. 동작 변경이 없으며 회귀 통과를 주장하지 않는다. |
| TLS MULTI_DEALER_DEALER 전 크기 5-run complete | 미실행. 동작 변경이 없으며 complete를 주장하지 않는다. |

## 변경과 보존

이 작업의 최종 변경 대상은 이 로그 문서다. 작업 도중 다른 언어의 변경이 추가됐으며
그 변경은 수정하거나 정리하지 않았다. 임시 계측은
`/tmp/zlink-cpp-sendsend-latency/instrumentation.patch`와 `*.original`에 보존하고 러너에서 제거했다.
공개 헤더·binding 라이브러리·Core·Framework·다른 언어·정책·스펙·계획서·decisions는 수정하지 않았다.
Commit·push하지 않았다. 기존 relay FIFO와 단일 sender, admitted echo teardown drain을 유지한다.

- 소유 계층: 관측된 대량 적체는 OS TCP 송신 queue이며 C++ admission terminal 이후다.
- Spec: binding README Submit 결과 투영, async-execution-model §4, Core socket part send의 local admission 계약.
- 교차언어: 동일 고정 Core·동일 셀에서 C 원본도 간헐 폭증한다. C++ 전용 원인으로 확정할 근거가 없다.
- 변경 분류: runtime 변경 없음. C++ 러너·binding의 B 결함을 확정하지 않았고 C 방식의 우회도 적용하지 않았다.
- 수정 전/후 규칙 수: 동일. 새 runtime 규칙 0개.

최종 복구 확인: routed server/client 4개 target을 계측 제거 상태로 재빌드했다
(`/tmp/zlink-cpp-sendsend-latency/restore-build.log`, exit 0).
바이너리 4개 모두 `SSDIAG` 문자열이 없고, `ldd`로 0.17.2 prefix의 Core 연결을 확인했다.
`bindings/cpp` diff는 0줄이고 Core SHA-256은 시작 값과 같다.
별도 기능 test와 미실행 perf gate의 통과를 이 빌드 결과로 대신하지 않는다.

문서 독립 리뷰에서 context snapshot을 전체 Core queue로 확대 해석할 수 있다는 지적을 채택했다.
`core/src/runtime/core/ctx_auto_hwm_recalc.cpp:307`을 직접 확인해
`core_queue_accounted_bytes`가 application queue만 포함하고 completion·monitor는 별도 필드임을
재검증했다. 위 결론을 관측한 Core application queue와 직접 관측한 OS TCP queue로 한정했다.
