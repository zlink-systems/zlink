# ALL-3 — 0.17.4 남은 Core 항목

필수 검증을 실행했지만 전체 목표는 미달이다. 큰 payload 수명 보존·C relay completion 진행·WS fixture를 수정했고, dev/TSan 전체는 통과했다. 최종64KiB latency/RSS ratio와 WS 비율, hotpath 양방향±5% gate는 실패했다. Windows 전체 실행도 실패했으며 확인된 이식성 결함을 수정한 뒤5개 suite 실패와 reconnect 간헐 위험이 남는다.

- 작업 worktree: `/home/hep7hep7/project/zlink-work/all3`, detached `1a79625d3d`(ALL-2 누적 + `f5d7cccde2`). 시작 2026-09-08 04:21:11 UTC, 종료 2026-09-08 08:17:37 UTC(3h 56m 26s), 상한 08:21:11 UTC 내.
- 소스 위치는 이 worktree 기준이다. 로그·원자료: `/home/hep7hep7/project/zlink-work/all-artifacts/all3/`.
- Windows 검증 clone: `D:\project\zlink-all3`. 다른 worktree 소스는 읽기만 한다. 스펙·public API·commit·stash 변경 없음.
- ALL-2 정식 보고서는 시작 시 지정 main 경로에 없었다. `progress-ALL-2.md`의 전체 dev210/210·TSan209/210(packet fixture 실패)·ASan13/13과 누적 patch를 대조했다.

## 항목별 판정

| 항목 | 상태 | 변경·근거 | 규칙 수 전/후 |
|---|---|---|---|
| A 큰 payload allocation | 최종 compact 구현·dev20회 통과, 성능 상한 미달 | `decoder_allocators.cpp`가 현재 read buffer를 유지한 채 같은 recycle state의 block으로 message를 생성. `decoder.hpp`는 큰 message로 직접 읽을 때 read buffer를 교체하지 않음 | allocator spare 자격의 크기 예외 1→0; storage 회수 owner1·spare1 유지 |
| A latency/RSS·D-BP29/34 | 목표 미달. C relay memory 개선·최종 TLS10회 완료 | TCP 대비 WS/WSS/TLS 64KiB mean latency·server peak RSS 각각≤3 gate 추가 | ratio 판정 owner1; runtime option/timer/retry 추가0 |
| B single TCP 교대·multi RR | single·RR3회 완료 | current→base→current→base, base `5304885197`; gate-all RR 결과 확인 후 부족한 측정 수행 | — |
| C synchronization 초안 | 코드 대조 완료 | 아래 초안만 제시. 스펙 원문 미수정 | 기존 lifecycle exclusion3종→1종(ALL-1 구현) |
| D shutdown fixture | 구현·dev/TSan 격리20회 통과 | ordered WS ping/pong을 모든 fragment의 peer-read fence로 사용. 전체 입력 개수·3초 drain-start·TERMINATED·잔여>0 유지 | timed ingestion 전제1→0; ordered fence1; drain-start timeout 유지 |
| E D 표·CRT | 코드·기존 Windows 관측 대조 완료 | 아래 표. CRT 옵션 미변경 | runtime 변경0 |
| F Windows | 전체 build PASS, 전체 CTest201/210 | 이식성 수정 후 해당 target 통과.5개 suite 실패·reconnect 간헐 위험 유지 | — |

## 변경 위치

| 범위 | 파일:행 | 변경 |
|---|---|---|
| Core payload storage | `core/src/runtime/protocol/decoder_allocators.cpp:139,161,290`, `decoder_allocators.hpp:49`, `decoder.hpp:41`, `zmp_decoder.cpp:286` | current read storage와 독립 payload의 단일 spare 회수 경로 |
| Allocator 검증 | `core/tests/unittest/unittest_zmp_decoder.cpp:873-1006` | 입력 pin·admission retry·cross-thread close·종료 race·layout·overflow 신규6개 |
| WS fixture | `core/tests/integration/test_stream_packet_progress.cpp:139,288` | ordered pong fence와 exact pending count |
| C relay | `bindings/c/perf/multi/common/perf_multi_relay_server.hpp:430,460`, `bindings/c/perf/tests/test_relay_completion_progress.cpp:149-178`, `bindings/c/perf/CMakeLists.txt:157-168` | completion 진행을 막는 무제한 receive drain 제거·native repro 등록 |
| Ratio gate | `bindings/c/perf/latency_rss_gate.py:164`, `bindings/c/perf/tests/test_latency_rss_gate.py:76` | 공식 mean·peak RSS의 TCP 비율≤3, 누락/중복/불완전 report 거부 |
| MSVC 선언·식별자 | `core/tests/testutil_unity.hpp:590`, `integration/test_option_type_public.cpp:36`, `integration/test_gap_h3_pubsub_contract.cpp:254`, `unittest/unittest_zmp_decoder.cpp:845`, `unittest/unittest_complete_record_admission.cpp:402` | IPC 가드·small 매크로 충돌·상수식 이식성 |
| Windows test 실행 | `core/tests/integration/test_stream_socket.cpp:244,267,1203`, `test_dealer_router_single_lane_contract.cpp:1581`, `core/tests/CMakeLists.txt:1114` | 실제 WinSock client·지원되는 Unity print·POSIX Callgrind 구성 |
| Windows RID helper | `core/tests/integration/routing-id/test_stream_routing_id_size.cpp:23,28,77` | 미구현 stub와 중복 POSIX connect 제거, 기존 connect_socket 재사용. 연결 owner2→1, payload·budget 유지 |
| Windows 검사 실행 | `core/tests/integration/monitoring/test_multi_stream_server_reassembly.cpp:43`, `core/tests/contract/check_public_surface.py:100,109,115,156,202` | Unity 초기화 전 longjmp 제거(기존 return77 유지), UTF-8 문서 인코딩 명시 |
| Unity fixture hook | `core/tests/unittest/unittest_mutex.cpp:11`, `unittest_socket_poller.cpp:12`, `unittest_ctx_runtime.cpp:10`, `unittest_ipc_address.cpp:10` | GCC weak 기본 hook 의존1→0, 빈 lifecycle hook을 플랫폼 공통으로 명시 |

ALL-3에서 vendored 소스 추가 수정0. 누적 patch에는 ALL-1/2의 기존 수정이 포함된다. 신규 option/timer/retry loop/hot-path lock과 public ABI 변경0.

## 설계 비교와 선택

큰 payload를 현재 allocator read buffer로 옮기면 `decoder.hpp`의 남은 입력과 admission retry 포인터가 아직 같은 block을 가리키므로 수명이 깨진다. 별도 deferred-growth 상태를 추가하는 안 대신, allocator의 기존 한 칸 spare와 refcount callback을 그대로 사용해 독립 payload block을 만든다. Decoder read target의 `_max_size`는 그대로 두므로 read 요청 크기를 늘리지 않는다. Spare를 크기별로 여러 개 보관하지 않으며, 전체 할당 크기가 맞지 않는 block은 다음 사용 시 반환한다. 초기 ALL-3 후보의 Native/Massif에서 payload65536B에76880B가 붙는 read-batch counter 낭비를 확인하여, 독립 payload에는 content record1개만 할당한다(65536+header24+content40=65600B, −14.67%; header24는 dev object의 test helper disassembly로 대조). ALL-2의 init_size 경로65576B와 비교하면 final은 recycle header24B가 추가된다. 따라서14.67%는 초기 ALL-3 후보 대비이며 ALL-2 대비 절감률이 아니다. Spare 회수·새 할당 선택은 두 호출부에서 공통 allocate_buffer 한 곳으로 모았다. 같은 payload capacity라도 content record 수가 다른 read batch와 잘못 재사용하지 않는 ASan 경계 unit을 추가했다. 단일 spare에서 성장 block만 제외하던 분기를 제거한 데 맞춰 allocator unit은 성장 block의 실제 pointer 재사용까지 검사한다. 공개 integration 기대값은 완화하지 않았다.

Fixture는 private STREAM hook을 추가하는 안과 WS control event를 쓰는 안을 비교했다. 같은 연결에서 data 뒤에 보낸 ping의 pong은 peer가 선행 data를 소비했음을 나타내므로 기존 transport protocol을 재사용했다. Drain 시작 timeout은 기존3초를 유지한다. Pong fence 자체의 무응답은 기존 동기 fragment 송신과 마찬가지로 CTest target timeout이 제한한다.

## 검증 표

| 검증 | 결과 | 로그 |
|---|---|---|
| dev build | allocator 증분 성공 | `dev-allocator-build.log` |
| 신규·변경 dev20회 | 최종 unittest_zmp_decoder42case(신규6개)·test_stream_packet_progress 각각20회 통과. 초기 spare unit의 current pin 누락 수정 | `compact-payload-repeat20.log`(18.39s); 초기 `dev-new-repeat20.log` |
| Python performance gates | 29/29 통과(새6 포함) | `python-performance-gates.log` |
| 전체 dev / 관련3회 | 최종210/210(261.33s), 관련61target×3 모두 통과(103.52s) | `dev-final-full.log`, `dev-final-related-repeat3.log` |
| TSan 관련·전체 / ASan 변경 | 최초 최종 TSan 관련60/61: ASan build 병행 중 packet fixture CTest10s timeout(10.02s), race report 없음. 격리 fixture20/20 통과(101.80s); 격리 관련61/61 통과(55.78s), 최종 전체210/210 통과(374.27s, suppression0). ASan 변경12/12 통과(21.56s) | `tsan-final-related.log`, `tsan-related-isolated.log`, `tsan-final-full.log`, `asan-final-related.log` |
| 최종 이식성 dev / native sanitizer | 최종 fixture6/6 PASS, raw helper2target×3 PASS. 최종 ASan fixture12/12 PASS(6.50s). 새 C relay native ASan·TSan PASS(각1회, suppression0) | `final-portability-dev-tests.log`, `dev-final-raw-helper-repeat3.log`, `asan-final-fixture-tests.log`, `relay-asan-test.log`, `relay-tsan-test.log` |
| Release library·static hotpath binary build | 성공. hotpath는 tests=OFF의 같은 Release archive와 -O3/-flto=4로 연결 | `release-allocator-build.log`, `hotpath-release-build.log` |
| hotpath5셀 / with_stream | hotpath3/5 범위 내, DD/DR은 Ir가 하한보다 작아 strict FAIL. with_stream6/6 complete,mismatch0 | `hotpath-final.log`, `with-stream/comparison.md` |
| WS 비율 / latency·RSS 비율 / B 교대 | WS clean recheck FAIL: WS0.455315<0.80, WSS1.011529≥0.80. latency·RSS FAIL(입력 오류0), B single8셀 완료 | `ws-idle-recheck-gate.log`, `latency-rss-final-gate.log` |
| Windows 전체 build·ctest | 전체 build PASS. 전체 CTest201/210,503.72s. 이식성 수정 후 대상2/2·flow 개별5/5 PASS; 남은5개 suite 실패 | `windows-hooks-build-ctest.log` |
| public interface / mirror12 | f5d7cccde2 대비 diff0,12/12 동일 | `public-interface-check.json` |

## 성능 표

단위: latency ms, RSS MiB. ALL-1 수치는 실행 조건과 샘플링 시점이 서로 다르므로 새 측정의 동일 run TCP 대조를 판정 기준으로 삼는다.

| Pattern | Transport | ops/s | Mean ms | RSS MiB | Lat/TCP | RSS/TCP | sampled Tx MiB |
|---|---|---:|---:|---:|---:|---:|---:|
| MULTI_DEALER_ROUTER_SENDSEND | tcp | 24198.0 | 77.645 | 102.68 | 1.000 | 1.000 | 51.59 |
| MULTI_DEALER_ROUTER_SENDSEND | tls | 6861.0 | 390.727 | 424.34 | 5.032 | 4.133 | 243.84 |
| MULTI_DEALER_ROUTER_SENDSEND | ws | 7624.0 | 456.079 | 431.23 | 5.874 | 4.200 | 185.14 |
| MULTI_DEALER_ROUTER_SENDSEND | wss | 5711.4 | 458.739 | 452.96 | 5.908 | 4.411 | 258.60 |
| MULTI_ROUTER_ROUTER_SENDSEND | tcp | 23220.4 | 29.660 | 35.45 | 1.000 | 1.000 | 2.41 |
| MULTI_ROUTER_ROUTER_SENDSEND | tls | 5559.4 | 440.904 | 394.88 | 14.865 | 11.139 | 205.34 |
| MULTI_ROUTER_ROUTER_SENDSEND | ws | 9680.8 | 449.194 | 485.62 | 15.145 | 13.699 | 269.01 |
| MULTI_ROUTER_ROUTER_SENDSEND | wss | 5125.4 | 431.670 | 414.56 | 14.554 | 11.694 | 254.02 |

최종 compact64 실행은12/12 complete(109s)이나 latency/RSS3배 gate는 미달이다. 초기 ALL-3 후보 DR WS390.297ms/1418.33MiB → 최종456.079ms/431.23MiB(서버 RSS−69.60%, latency 개선 없음). ALL-1 DR tcp102.460ms/ws441.622ms/wss419.634ms는 별도 실행 참고치다. 원자료 `final64/multi/report/perf_c_multi_linux_20260908_153157_ALL-3-final64.txt`, `final64-rss.json`, `final64-samples.jsonl`. Latency는 공식 runner mean metric이며 client의 half-RTT 정의를 따른다. 같은 정의의 TCP 비율에는 영향이 없다.

### Latency·RSS 단계 전후

아래 ‘전’은 초기 ALL-3 allocator 후보+수정 전 C relay(`candidate64`)이며 ALL-2 원본 binary 측정이 아니다. ‘후’는 최종 compact allocator+수정 relay(`final64`)다. 동일100client·5s·65536B DR_SENDSEND 구성이다. 이 비교로 allocator 단독 이득을 분리해 주장하지 않는다.

| Transport | 전 mean ms | 후 mean ms | 전 RSS MiB | 후 RSS MiB |
|---|---:|---:|---:|---:|
| tcp | 67.050 | 77.645 | 98.76 | 102.68 |
| tls | 393.789 | 390.727 | 918.92 | 424.34 |
| ws | 390.297 | 456.079 | 1418.33 | 431.23 |
| wss | 431.637 | 458.739 | 865.25 | 452.96 |

### WS 비율 gate

최종64KiB와 clean1024B 대조: Q(ws)1024=0.908752/65536=0.413769, Q64/Q1=0.455315로 FAIL. WSS는0.633498/0.640801, 비율1.011529로 PASS. clean1024는07:24:50 UTC 시작(load1=0.8335,ninja0,available10482MiB),6/6 complete이며1초 간격 resource timeline에서 compiler 겹침0이다. 원자료 `final1024-idle-recheck/multi/report/perf_c_multi_linux_20260908_162450_ALL-3-final1024-idle-recheck.txt`, 판정 `ws-idle-recheck-gate.log`. 앞선1024 측정은 compiler 겹침이 있어 그 PASS(WS1.325/WSS2.438)는 채택하지 않았다. 따라서 필수 WS gate 유지도 미달이며 전체 성능 변경 승인 근거가 아니다.

### Hotpath 최종5셀

기준 JSON은 수정하지 않았다. `hotpath-final.log`의 원래 양방향±5% 판정은3/5 PASS,2/5 FAIL이다. FAIL 두 셀은 명령 수가 기준보다 줄어 하한을 벗어난 것이며, 이를 처리량 회귀로 해석하지 않는다. 그래도 요청한 gate 전체 PASS로 바꾸지 않는다. 각 Valgrind 직전 ninja0·available≥6000MiB를 artifact wrapper에서 확인했고 측정 전체 compiler 겹침0이다.

| Cell | Reference Ir/msg | Final Ir/msg | Ratio | ±5% |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3230.922 | 3050.105 | 0.9440 | FAIL(하한) |
| dealer_router_reqrep_inproc | 16455.383 | 15354.465 | 0.9331 | FAIL(하한) |
| pair_inproc | 2348.457 | 2406.194 | 1.0246 | PASS |
| router_router_tcp | 2972.532 | 3005.760 | 1.0112 | PASS |
| stream_tcp | 13969.806 | 13809.179 | 0.9885 | PASS |

### with_stream 최종1회

CCU1000, zlink/asio,64/1024/65536B,각1회.6/6 complete,mismatch0. `with-stream/comparison.md`, `summary.json`, `metrics.csv`에 원자료를 보존했다. 시작 load1=1.021, 실행 중 compiler 겹침0이다. 이 표는 별도 STREAM 비교이며 A의100client routed echo ratio gate와 합치지 않는다.

| Size | zlink Kops/s | asio Kops/s | zlink/asio | zlink mean ms | asio mean ms | zlink RSS MiB |
|---|---:|---:|---:|---:|---:|---:|
| 64 | 276.90 | 341.06 | 0.812 | 1.80 | 1.47 | 45.25 |
| 1024 | 260.92 | 317.50 | 0.822 | 1.91 | 1.57 | 45.12 |
| 65536 | 29.85 | 39.08 | 0.764 | 16.66 | 12.75 | 272.47 |

### B(1) single TCP1024 교대2회

요청 순서 current→base→current→base, 각 교대120s 연속 load1<1·ninja0·compiler0·available≥6000MiB를 확인했다. LD_PRELOAD/LD_DEBUG 실제 init 경로는 current=`all3/core/build/lib/libzlink.so.0.17.2`, base=`base-5304885197/core/build/lib/libzlink.so.0.17.1`로8/8 검증됐다. 실제 측정8셀에1초 resource watcher compiler 겹침0(`single-resource-check.json`).

| Pattern | current1 ops/s | base1 | 비율1 | current2 | base2 | 비율2 | 판정 |
|---|---:|---:|---:|---:|---:|---:|---|
| PAIR | 771454.8 | 832731.4 | 92.6415% | 803853.2 | 809239.8 | 99.3344% | 두 교대 모두≤95% 아님 |
| DEALER_DEALER | 821194.8 | 850182.2 | 96.5904% | 810975.0 | 840138.0 | 96.5288% | 두 교대 모두≤95% 아님 |

첫 PAIR−7.36%를 숨기지 않는다. 요청한 수정 조건이 성립하지 않아 MP-5/G-11b를 추측으로 바꾸지 않았다. `single-alternating.log/.json`, `loader-*`에 원자료를 보존했다. 전체597.2s에는 idle 대기가 포함된다.

### B(2) gate-all 자료 확인

`gate-all-summary.md:25-32`를 직접 대조했다. Windows 기존 변경으로 후속 Linux perf/c를 실행하지 않았으므로 요청한 RR1024 3-run 자료는 없다. 자체 3-run을 수행했다. 결과277.866k/251.381k/242.990k ops/s, median251381.4 ops/s·mean0.941796ms로 phase2g242.5k 대비+3.66%다. 이전−16% 하락은 재현되지 않았다. 시작 load1=0.4302, ninja0, available10486MiB, 실행 중 compiler 겹침0. `rr1024-three.log`, `rr1024-three/multi/report/perf_c_multi_linux_20260908_170415_ALL-3-rr1024-three.txt`, `rr1024-three-resource-check.json`에 기록했다. ALL-1 초기303325.8/최종203539.6 ops/s는 서로 다른1-run이며, 3회 표본으로 합치지 않는다. 요청의−16% 기준은 phase2g242.5k ops/s 대비 ALL-1 최종203.540k(−16.07%)다. 이전 `attrib-0.17.2-summary.md:48-58`의 base/current2교대는94.69%/97.21%로 한 번만95% 이하였다. 이 과거 판정을 ALL-3 최종 측정으로 대체하지 않는다.

### A C relay 소유권 재검증(B)

감독이 `core/doc/spec/core/socket/README.ko.md:991-1018` 및 `core/doc/spec/core/05-polling.ko.md:60-68`을 직접 열어 검증했다. DONTWAIT 거절 payload 보관과 completion NO_DATA drain·재제출은 application 책임이다. `perf_multi_relay_server.hpp`의 무제한 receive drain은 단일 wait token이 생겨도 계속 payload를 pending deque에 보관하고, WRITABLE drain은 outer poll turn에서만 실행한다. 따라서 application event-loop 결함(B)으로 채택한다. Core lost wake나 allocator leak으로 판정하지 않는다. Receive quantum 추가나 RID별 token map보다 기존 단일 토큰에 맞춰 반송이 막히면 input 수신을 멈추는 한 규칙을 선택한다. `relay-pending-before.log`에서 pending16384까지 직접 재현했다. flush가 보관한 reply를 남기면 receive drain을 반환하고, pending이 있을 때 POLLIN을 등록하지 않도록 수정했다. Scheduling 규칙3개(무조건 EAGAIN drain·global enqueue·outer-only completion)→1개(반송 불가 중 input 수신 정지). 임시 trace는 소스에서 제거했다. 새 native regression은 같은16개64KiB 입력·1B reply HWM으로 이전 header에서 `!recv_drained` assertion 실패, 수정 header에서 until-fail20 통과를 확인했다(`relay-regression-before-proof.log`, `relay-regression-fenced-repeat20.log`). 첫 테스트 후보의 initial-readiness race는 같은 poller의 POLLIN event fence로 제거했으며 기존 token/pending oracle과 timeout은 유지했다.

### D-BP29 최종 C 반복 측정

C TLS RR_SENDSEND1024·clients100·5s,5/5 complete. 실행순 mean은3.637/5.836/2.628/2.722/3.556ms(median3.556ms), throughput median224.285k ops/s다. 150ms server socket-inode 샘플의5회 최대 Tx는248398/2294059/237476/168372/192176B(전체최대2.19MiB), server peak RSS57.85MiB다. 과거77/225ms 수준의 폭증과766MB OS queue는 이 최종5회에서 재현되지 않았다. C++ pending≤1 관측과 과거 실행의 원인이 같다는 증명은 아니며 OS queue 계약 변경이나 Core retry 보상은 넣지 않았다. 원자료 `bp29-final-five.log`, `bp29-final-five-samples.jsonl`, `bp29-final-five-rss.json`.

### D-BP34 재현

C TLS RR_SENDSEND65536, clients100·active5s·기존 drain15s로 **수정 전 relay 5/5 complete**. 개별 mean315.159/324.620/326.845/328.964/336.590ms(실행순서는 원로그 참조), echo 누락 실패0. `bp34-before-five.log`에5개 실행과 complete를 보존했다. 기존.NET의684 echo 누락4/5는 이번 C에서 재현되지 않았으므로 Core echo 정지의 원인을 확정하거나 해결됐다고 하지 않는다. 높은 latency는 별도로 남는다. 최종 동일 C 구성5/5도 complete/echo 누락0이며 mean357.947/361.193/322.623/335.767/310.494ms, median335.767ms다. server peak RSS467.03MiB, sampled Tx 최대285.61MiB다. `bp34-final-five.log`, `bp34-final-five-samples.jsonl`, `bp34-final-five-rss.json`에 보존했다. BP29/34 최종10회 모두 compiler 간섭0을 resource watcher로 확인했다. 수정 전에도 C에서 누락이 없었으므로 D-BP34 Core echo 정지 해결 판정은 불가하다.

### A native 메모리 진단

100client Massif diagnostic은 TCP29.93MB/WS34.07MB의 live heap을 보였으나 계측이 server 실행을 크게 늦춘다. Native `mallinfo2` 보조 계측은 arena 내 live heap 자체가GB로 증가하는 것을 확인했다. `heap-snapshot.c/.so`는 artifact에만 있으며 최종 runtime에 포함하지 않는다. C relay의 전역 pending FIFO와 WRITABLE servicing 결함을 위와 같이 재현·수정했다. C++ D-BP29의 pending≤1 관측은 별도이므로 같은 원인으로 묶지 않는다.

### Auto-HWM detail 해석

최종 runner의 server detail4096000B는 실제 accepted data pipe HWM의 근거로 쓰지 않는다. 감독이 `socket_base_monitor.cpp:62-65,121-130`을 확인했으며 registry-accounted pipe는 이 loop에서 제외되고 applied field는 socket options 기본값으로 fallback한다. Relay의 출력 시점도 bind 직후/client accept 전이다. `socket_base_api.cpp:635-647`의 application attach는 Auto-HWM 즉시 재계산을 호출하며, spec06의 연결 증가 시 새 목표 즉시 적용 조항(:154-162)과 일치한다. Balanced data max1MiB를 넘겨4MB가 유지된다는 가설은 이 자료로 입증되지 않아 기각했다. 표시값을 근거로 budget을 낮추거나 runtime을 수정하지 않았다.

### WS128KiB batch 메모리의 범위

`ws_batch_policy.hpp:10-15`는 encoder batch와 client masking scratch의 기본 경계를128KiB 한 곳에서 정한다. `asio_ws_listener.cpp:288-293`는 non-STREAM accepted connection의 out_batch_size에 이를 적용하고 `asio_zmp_engine.cpp:280-282`는 encoder를 한 개 만든다. 따라서100개 server connection의 **encoder buffer 부분만** 전부 없앤 이상적 상한도12.5MiB다. 최종 DR WS431.23MiB를 TCP102.68MiB의3배 이내로 내리는 데 필요한 약123MiB보다 훨씬 작다. `ws_transport_common_internal.hpp:41-45`의 Beast scratch 설정은 별도 사용 조건을 가지므로 설정 크기를 실제 server RSS로 더하지 않았다. 성장/회수 정책만으로 전체 차이를 해소한다고 판단하지 않았고, Q gate에 영향을 주는 batch 경계나 추가 성장 상태는 이번 patch에 넣지 않았다.

### TLS write 완료·재개 감사

감독이 `ssl_transport.hpp:63-68`, `ssl_transport.cpp:168-188`, `asio_engine.cpp:1028-1098,1277-1308`, `tcp_transport.cpp:519-563`을 직접 열어 독립 조사 결과를 대조했다. TLS 정상 데이터 경로는 speculative write를 끄고 composed `boost::asio::async_write`로 supplied buffer 전체 또는 error까지 진행한다. Engine의 단일 write-pending 소유권이 다음 encode를 막고 completion 이후 pointer/remaining을 전진한다. TCP 기본 async write도 같은 composed operation이다. 현재 정상 echo의 partial-write/EAGAIN lost resume 또는 output buffer 조기 회수 결함은 확인되지 않았다. 이 경로를 임의로 수정하지 않았다. OS queue 폭증은 별도 계약 판정이 필요하다.

## D 표 — 구현하지 않은 계약·정책 변경

| ID | 현재 코드에서 남은 이득 추정 | 계약·판정 근거 | 상태 |
|---|---|---|---|
| D-a app send N개/Tµs batching | 남은 handoff command 감소 여지는 있으나 이득 미측정. 제거되지 않은 mailbox 삽입의 기존 추정 약2.7 lock/msg가 절감 후보. 이 수치와 다른 빌드의 최종8.3861을 나눠 처리량 개선율로 환산하지 않음 | `object.cpp:365`, `pipe_write.cpp:1401`은 실제 activation을 보냄. 08-stream §4의 즉시 진행/edge 의미에 지연을 추가 | 감독 폐기/채택 대기, 미구현 |
| D-b polling command drain rdtsc skip | 이미 `process_commands(0,true)`에 throttle가 있어 추가 절감은 미측정·제한적 | `socket_base_lifecycle.cpp:455`, `socket_command_runtime.cpp:9`; 05-polling POLLIN/POLLOUT level 조건을 건너뛸 수 없음 | 미구현 |
| D-d credit published store 축소 | single frame의 불필요한 incomplete reset은 이미 없어 남은 이득은 작을 것으로 추정, 수치 미측정 | `pipe_receive.cpp:721-742`, `pipe_transport.cpp:679`; 06-auto-hwm queue dequeue credit와 snapshot 관측 경계를 바꾸면 계약 변경 | 미구현 |
| D-f 죽은 STREAM gather env 제거 | runtime 성능 이득0 추정. 죽은 접근자·문서3종을 없애는 구조 정리 이득만 있음 | `asio_stream_fastpath_policy.hpp:144-163`이 raw protocol의 gather header 부재에서 먼저 false. 스펙08 런타임 기본값 문장 삭제 승인 필요 | 미구현 |
| D-OS OS 송신 queue 기반 backpressure | kernel backlog를 줄일 가능성은 있으나 처리량·latency 이득 미측정 | 05-connection-memory §3.4: Completion lane에는 HWM/LWM/Core budget을 적용하지 않고 기본 SNDBUF/RCVBUF는 OS autotuning 유지. 06-auto-hwm §4: complete message dequeue에서 credit 반환. OS ACK까지 credit을 연장하거나 completion을 새 admission으로 막는 안은 계약 변경 | 미구현 |
| D-W1 Windows `/MT` 전환 | 현재 `/MD` DLL2,279,424B. MSVCP/VCRUNTIME 의존 제거는 기존0.17.0 `/MT` DLL에서 확인됨. `/MT` 크기 증가량·Java 충돌 해결 여부는 이번 비교 미측정 | 아래 CRT 검토 | 정책 결정 대기, 옵션 미변경 |

D-W1: `core/CMakeLists.txt`에는 `/MT`·`/MD`·`CMAKE_MSVC_RUNTIME_LIBRARY` 명시 설정이 없다. `scripts/local-package/README.ko.md:127-148`은 표준 MSVC dynamic CRT(`/MD`)와 Java 검증용 별도 `/MT` build directory를 구분한다. CI `.github/workflows/build.yml`도 별도 CRT override 없이 shared/static Core를 만든다. `core/builds/windows/build.ps1:219-263`의 CRT 복사 fallback은 `/MT`라는 주석만으로 의존 DLL이 없음을 증명하지 못하므로 실제 imports에 따라 판단해야 한다.

`/MT`는 Core에 CRT 코드를 포함하므로 Core DLL 자체가 커질 수 있다. OpenSSL 등 다른 shared dependency까지 static으로 바꾸는 옵션은 아니므로 그 DLL은 별도 검토 대상이다. Java22 `/MD`의 `msvcp140.dll` AV(`NativeContext.setUInt64Option`)가 별도 `/MT`에서 사라진 관측은 `doc/bug/2026-09-07-windows-framework-sample-execution.ko.md:76-83`에 있다. 정확한 DLL 로딩 경로와 최소 재현이 미확정이므로 `/MT`가 모든 Java 충돌을 해결한다고 판정하지 않는다. 두 변형을 배포하면 prefix·provenance와 binding에 복사하는 DLL 선택도 구분해야 한다.

이번 CI configure의 `libzlink.vcxproj` Release `RuntimeLibrary`는 `MultiThreadedDLL`(`/MD`)다. 실제 생성 DLL은2,279,424B이며 `dumpbin /DEPENDENTS`에서 `MSVCP140.dll`, `VCRUNTIME140.dll`, `VCRUNTIME140_1.dll`, UCRT API sets, `libssl-4-x64.dll`, `libcrypto-4-x64.dll`을 확인했다(`windows-dll-imports.txt`, `windows-dll-metadata.json`). `/MT` 비교 DLL은 만들지 않았으므로 크기 증가량과 Java 충돌 해결 여부는 미측정이다.

## 스펙11 반영 문장 초안 — 스펙 파일 미수정

§3.1: “socket lifecycle turn은 `public_api_state`의 sync bit를 다른 bit를 보존하는 RMW로 획득·해제하는 하나의 C2 소유권이다. Public send·receive·control, async mailbox command batch와 completion drain은 같은 turn에서 socket C2를 변경한다. 별도 receive-owner word, receive fallback mutex, `command_owner_sync`, `_completion_owner_sync`를 두지 않는다. `command_drain_active`와 async handoff는 배타 소유권이 아니라 command 진행과 재확인을 알리는 C3 발행이다.”

근거: `socket_lifecycle_runtime.cpp:58-109`, `socket_base_lifecycle.cpp:407-469`, `socket_runtime.hpp:280-332`. 재진입한 내부 scope는 바깥 turn을 해제하지 않는다.

§3.4: “Receive waiter는 lifecycle turn 안에서 waiter 수를 등록하고 turn을 해제한 뒤 plain mutex 아래 epoch를 재확인하고 CV에서 기다린다. Publisher는 progress epoch를 seq_cst로 증가시키고 등록된 waiter가 있을 때만 해당 plain mutex 아래 broadcast한다. 등록과 발행의 seq_cst 순서는 wake 유실을 막으며, CV mutex는 socket C2 소유권을 대신하지 않는다. Epoch가 달라졌으면 잠들지 않는다.”

근거: `socket_runtime.hpp:318-332`, `socket_base_lifecycle.cpp:1575-1610`. Agent 초안의 ‘epoch도 등록 시 turn 안에서 관측한다’는 표현은 caller가 넘긴 `observed_epoch_`와 다르므로 채택하지 않았다.

C3 목록 초안:

| 공유 사실 | 발행 경계 | 코드 |
|---|---|---|
| Receive progress epoch·waiter 등록 | seq_cst 등록/증가, plain CV 아래 재확인 | `socket_runtime.hpp:318-332` |
| Async handoff·command drain 진행 | command claim/종료와 pending hint 재확인 | `socket_base_lifecycle.cpp:439-469`, `socket_runtime.hpp:309-310` |
| Completion owner generation | completion 실행 owner 교체의 generation 관측 | `socket_base_dispatch.cpp:100-132` |
| Pipe ledger·peer credit | `_inbound_ledger_sequence`/`_outbound_ledger_sequence`의 coherent snapshot, `_published_msgs_read`/`_published_bytes_read`/`_published_incomplete_bytes_read`의 release 발행·peer acquire | `pipe.hpp:803-819`, `pipe_receive.cpp:721-742`, `pipe_transport.cpp:673-688` |
| Cold Auto-HWM·admission snapshot | `_hwm`/`_lwm`/`_inhwm`/`_max_message_bytes`, `_published_outbound_total_bytes`/`_published_outbound_provisional_bytes`의 foreign 관측 | `pipe.hpp:783-838`, `pipe_write.cpp:1311-1320,1419-1422` |
| Transport connection ID | socket-end admission에서 현재 ID 관측 | `pipe_write.cpp:806-823` |

이 표는 ALL-1의 owner 통합 뒤 유지해야 할 발행 목록이다. Atomic 타입이라는 이유만으로 C3로 분류하지 않는다. 예를 들어 `pipe.hpp:735-742`의 `_in_active` 주석은 이전 receive lease/command sync 분리를 설명하므로 현재 두 소유권의 근거로 인용하지 않는다.

STREAM route shard는 C3 counter가 아니라 C1 immutable map snapshot이다(`stream.hpp:92-104`, `stream.cpp:146-164`).

### 계획 §7.7 잠금 인벤토리 최종 표 초안

| 항목 | 기존 lock/msg | ALL-2 누적 코드의 현재 구조 | 최종 판정 |
|---|---:|---|---|
| mailbox 삽입 `_sync` | 2.7 | 다중 producer 삽입 직렬화 유지 | 구조상 유지 |
| public/command/receive socket 직렬화 | 1.47+1.28 | 하나의 lifecycle CAS turn. 중복 pthread mutex 제거 | ALL-1 구현 완료 |
| session-end `_out_sync` | 2.0 | I/O thread C2 + C3 ledger | G-11b 완료 |
| socket-end `_out_sync` | 1.0 | socket turn 재사용, cold transport lock만 유지 | ALL-1 완료 |
| STREAM route shard lock | 1.0 | C1 shared_ptr immutable snapshot | ALL-1 완료 |
| public poller handle 표 | 0.56 | `api/monitoring/poller_api.cpp:17,40`의 registry mutex가 hot acquire에 남음 | 미제거, 이번 변경 범위 밖 |
| Boost.Asio 내부 | 4.0 | Core 외부 | 유지 |

ALL-1의 실제 합계는537,372 lock/64,079 message=8.3861 lock/msg다. 계획 목표≈8.3과 가깝지만 G1 기준은 dev, ALL-1은 Release이므로 위 행의 이전 수치를 더해 최종 실측값으로 제시하지 않는다. ALL-3는 allocator를 바꾸며 hot path lock을 추가하지 않는다.

### Windows 테스트 이식성 수정

Unity 기본 setUp/tearDown을 제공하지 않던4개 unit도 플랫폼 공통 빈 hook을 명시했다. 이식 전 GCC weak default와 같은 동작이며 vendor Unity는 수정하지 않았다.

전체 CI build로 노출된 미지원 `TEST_MESSAGE`는 기존 `UnityPrint`로 바꾸고 Windows SDK `small` 매크로와 충돌하는 지역 변수3개를 변경했다. Auto-HWM snapshot helper 선언은 정의와 달리 IPC guard 안에 있어 밖으로 옮겼다. Multipart array size8은 MSVC lambda에서도 상수인 enum으로 표현했다. POSIX socket을 사용하는 Callgrind driver는 UNIX에만 구성하며 Windows에서 원래 등록되지 않던 hotpath gate의 실행 조건은 그대로다. `test_stream_socket.cpp`는 endian helper를 플랫폼 공통으로 옮기고 WinSock raw client를 기존 `connect_socket`/`fd_t`/close helper로 연결해 READY-before-payload를 동일 조건으로 실행하게 했다. 새 Windows skip 제안은 fixture 조건 완화이므로 기각했고 실제 client로 대체했다. 기존 timeout·payload·READY/종료 oracle은 바꾸지 않았다.

### Windows 전체 CTest 실패 분리

전체210개를 한 번 실행해201개 통과·9개 실패했다(503.72s). 최종 패치의 Windows 전용 fixture 수정 뒤 실패 target만 재검증하며, 원래 전체 실행 실패를 소거하지 않는다.

| Target | 실제 실패 | 판정·대응 |
|---|---|---|
| test_multi_stream_server_reassembly | SegFault | `:43`의 Windows main이 UNITY_BEGIN/RUN_TEST 전에 TEST_IGNORE_MESSAGE를 호출. `unity.c:1314-1336`에서 미초기화 AbortFrame으로 longjmp. puts로 바꾸고 기존 return77/CTest SKIP_RETURN_CODE77 유지. 새 skip 없음 |
| contract_public_surface | cp949 UnicodeDecodeError | UTF-8 source/spec를 read_text 기본 locale로 읽음. 다섯 read_text에 encoding=utf-8 명시. 재검증에서 nm -D가 DLL을 읽지 못해 MSVC CMAKE_LINKER /dump /exports로 연결. Linux 동일 gate PASS(function99/export 일치) |
| test_reconnect_options | `:203` expected5/actual−1 | 5B blocking directed reconnect; 실패-target 재검증 PASS, 원인 미확정·assertion 미변경 |
| test_stream_routing_id_size | 원래`:138` expected true/false | Windows helper가 항상 EOPNOTSUPP. `connect_socket`/`fd_t`로 통합하고 실제 WinSock send로 이식. payload·RID oracle 유지 |
| test_single_lane_wire_mandatory_count | `test_dealer_router_single_lane_contract.cpp:1436` expected true/false,0xc0000409 | handshake-rejected 관측 실패; 원인 미확정 |
| test_wake_invariants | `:569` unexpected extra completion | large HWM drain; 원인 미확정 |
| test_two_poller_wake | `:547` PAIR/ROUTER watchdog wake261/258ms | wake 진행 실패; 원인 미확정 |
| unittest_flow_state_socket | CTest10.04s timeout | 기존10s 제한 유지; 마지막 진행 transition과 격리 결과 확인 |
| unittest_mutex | `:66` plain mutex의 owner try_lock이 true | Windows CRITICAL_SECTION의 재귀 동작(`runtime/utils/mutex.hpp:36-49`); spec11:224-228은 다른 backend의 재진입 검출 미지원 명시. assertion 완화 금지에 따라 미변경 |

실패-target9개 재검증은 reconnect PASS·기존 skip 정상 종료, 나머지7개 실패였다(45.00s, `windows-failed-targets-recheck.log`). 두 poller PAIR/ROUTER watchdog wake는 재현됐다. 감독이 `runtime/core/socket_poller.cpp:264-294,298-333,365-393`을 직접 확인했다. Windows socket-only HANDLE 경로는 mailbox별 signaler를 등록하지만 timer를 포함한 poller의 fallback은 POSIX secondary notification 경로를 제외하고 동일 primary FD를 공유한다. 한 poller가 primary wake를 소비하는 경쟁과 관측이 일치하므로 **Core B 결함 후보**다. Spec05-polling:74-83,135-137은 false→true wake와 서로 다른 poller 동시 사용을 보장한다. 공개 `test_two_poller_wake` PAIR/ROUTER repro를 보존하며 새 poller/재시도/timeout 보상은 넣지 않았다. Baseline Windows 비교 없이 ALL-3가 새로 만든 결함 또는 과거부터 존재한 결함이라고 단정하지 않는다.

`test_wake_invariants.cpp:569`의 helper는 WRITABLE 전(:983), 수신 뒤(:1122), 재제출 뒤(:1167) 세 곳에서 호출된다. 현재 출력은 callsite를 구분하지 못하므로 premature-fixture assertion과 duplicate-completion Core 결함을 구분하지 않는다. Missing-lane 실패는 HANDSHAKE_IVL500ms 뒤3s close 미관측이며 `asio_zmp_engine.cpp:387-411` timer와 `wait_for_raw_close:329-337`의 WinSock 오류 분리가 남는다. Flow test는 첫20case PASS 후10s 제한에 도달하여 남은5case를 기존 ZLINK_TEST_CASE로 개별 분리한다. timeout은 늘리지 않는다.

Windows 전체 build는 새 clone의 CI shared/static/tests/TLS/C++17/Release 구성이다. 전체 실행 후 확인된 이식성 결함을 수정했으며 나머지 실패를 기존 결함으로 단정하지 않는다. 최종 routing-id와 contract_public_surface2/2 PASS, flow 남은5case는 같은 CTest10s 제한으로 개별5/5 PASS(`windows-routing-export-flow.log`). 따라서 flow의 전체 timeout은 aggregate fixture 실행시간 문제라는 근거가 강화됐으나 timeout·assertion·case 등록은 바꾸지 않았다. 남은 전체-suite 실패는 missing-lane close, large-HWM completion, two-poller wake, aggregate flow timeout, mutex backend assertion의5개다. Reconnect 첫 실패는 후속 PASS로 간헐 위험을 유지한다.

## 독립 조사 채택·기각

- C/E agent의 소스 인용을 감독이 lifecycle turn·CV·pipe ledger·route snapshot·CRT 문서에서 직접 확인했다. CV epoch 등록 문장은 위 이유로 고쳤고, poller handle 표는 직접 코드에서 registry mutex가 남았음을 확인했다.
- Windows helper의 `connect_socket`/`fd_t`/close 선언·정의를 감독이 `testutil.hpp:109-120`, `testutil.cpp:332-363`에서 확인하고 채택했다. 새 TEST_IGNORE는 기각했다.
- Fixture agent의 WS pong fence는 채택했다. Drain-start timeout 제거는 요청 범위에 필요하지 않아 기각하고 기존3초로 복원했다.
- Gate/sampler agent 결과는 실제 CMake binary 이름과 대조했다. RR 명칭을 ROUTER_ROUTER_SENDSEND로, sampler prefix를 실제 `comp_src_*`로 고쳤다.

## 실행 종료

요청한 검증은 모두 실행했다. 종료 시 ALL-3가 띄운 build/test/perf process는 남기지 않았고 resource watcher도 종료했다. 자원·공용 PERF_LOCK 대기 및 개별 실행 결과는 `progress-ALL-3.md`와 artifact `steps.jsonl`에 보존했다. 완료 판정이 아닌 **목표 미달 결과 보고**다. 스펙·공개 인터페이스·CRT 정책·다른 worktree 소스·commit·stash 변경 없음.

## 누적 patch

`/home/hep7hep7/project/zlink-work/all-artifacts/ALL-3-cumulative.patch`: `f5d7cccde2` 대비, untracked3개 포함,598,989B, SHA256 `2412d23c05271264faef815f7b4531264f0c0f2aa430c2f2ac591be21c9f2ff0`. 별도 temporary index로 적용 검사를 통과했다. 실제 worktree index는 비어 있으며 commit/stash 없음. Windows clone의 ALL-3 변경25개 파일은 LF 정규화 비교25/25 동일(`windows-source-parity.json`).

## 남은 위험

- A의 latency/RSS3배 상한과 WS Q64/Q1≥0.80 유지에 실패했다. 메모리 감소만으로 이 후보를 전체 목표 완료로 판정할 수 없다.
- Windows missing-lane close, large-HWM completion, two-poller wake, aggregate flow timeout, mutex backend assertion의5개 suite 실패가 남는다. Reconnect는 최초 실패 후 재검증 PASS로 간헐 위험이다.
- Hotpath DD/DR의 Ir가 기준보다5% 이상 작아 원래 양방향 gate가 실패한다. reference를 바꾸지 않았다.
- Spare는 한 칸이지만 마지막 block이 큰 경우 그 capacity를 다음 불일치 사용 또는 allocator 종료까지 보유한다. 성장 buffer 재사용의 메모리 tradeoff를 실제 RSS로 판정한다.
- D-BP29의 과거 큰 스파이크와 D-BP34의 .NET echo 누락은 최종 C 반복에서 재현되지 않았다. 최초 원인과 교차언어 동등성을 확정하지 못했고 OS queue 관련 안은 D 표에만 남겼다.
- 성능은 요청한 제한 횟수의 관측이며 장기 변동 폭을 입증하지 않는다. 초기 후보·ALL-1·최종 수치의 실행 단계를 구분했다.

소유 계층: Core decoder allocator가 입력·payload storage 수명을, engine/transport가 I/O 완료를 소유한다. C benchmark application은 거절 reply 보관·completion drain을 소유한다. Framework runtime 변경 없음.

스펙 조항: 01-zmp §9의 입력 byte 순서·bounded write, 05-connection-memory §3.4, 06-auto-hwm §4 allocation 전 admission·dequeue credit, socket README:991-1018 및05-polling:60-68의 application completion drain. 공개 계약 변경 없음.

교차언어 대조: 모든 binding이 같은 Core ABI를 사용한다. 언어별 우회는 추가하지 않는다. mirror 최종 검증은 별도 표에 기록한다.

변경 분류: B(allocator 비용·C relay completion 진행·fixture 결함 수정). 항목 C의 스펙 문장 초안은 구현 변경이 아님. D 표 아이디어는 미구현이다.
