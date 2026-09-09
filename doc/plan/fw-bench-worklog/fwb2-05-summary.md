# fwb2-05 — C 기준선 4096B 재현성 진단

판정은 **(c) Core의 연결별 I/O 스레드 배치에 따른 성능 재현성 문제**다. 같은 0.17.5 바이너리에서 request client의 작업이 I/O 스레드 하나에 집중되는 낮은 처리량 모드와 여러 I/O 스레드에 분산되는 높은 처리량 모드를 재현했다. 시작 시 디버거로 낮은 모드의 application/completion session이 같은 I/O 스레드 객체를 사용함을 확인했다. 데이터 유실·readiness 실패는 재현하지 않았다.

**최종 G5: request-window@4096 25.0%, request-backpressure@4096 33.1%로 모두 실패했다. Formula 1의 4096B 분모는 게재 불가다.**

Core 수정은 요청 범위 밖이므로 C harness와 Core를 수정하지 않았다. 이번 작업의 저장소 변경은 이 문서뿐이다. 감독자가 검토·커밋한다.

## 실행 범위와 보존 자료

- 작업 branch: `main`, 시작 HEAD `9cebc55214` (`ed2347d0b0` 이후). 기존 Core 규격 문서·Node 벤치·타 작업 요약 변경은 보존했다.
- 모든 벤치와 디버거 진단은 `perf-ticket.sh submit -p 2 -o astra-fwb2-05`로 제출하고 완료를 기다렸다. 직접 벤치 실행·Core 변경·commit·push는 하지 않았다.
- `/tmp/zlink-astra-fwb2-05/`에 바이너리, 진단 스크립트, 원본, `/proc` 스케줄 표본, 집계기 출력을 보존했다. 원래 S1 Release 바이너리를 byte 단위로 복사해 재사용했으며 새 빌드는 없다.
- `provenance.json`: Core 0.17.5, source `766dd1bb6e860a45ac2268363b45e26533b1626b`, library SHA-256 `52facd448e380ab8a73e2a5fd2d65b089ebeeef9f8f5e017b9ad1046702f4586`. `ldd`로 동일 package 연결을 확인했고 아래 인용한 Core 소스는 이 revision과 HEAD 사이 diff가 없다.
- 환경: Linux/WSL, Intel Core Ultra 7 265K로 표시되는 논리 CPU 20개, gRPC++ 1.51.1. VM이므로 CPU 번호만으로 P/E 코어 종류를 단정하지 않는다.
- 정식 run: `DURATION_SECONDS=5`, payload `1024,4096`, window `100`, `MAX_OUTSTANDING=4096`, drain `5000 ms`, runner의 `all` 시나리오와 client-driven 모델 유지. MAX_OUTSTANDING은 별도 request-saturation 셀에만 적용하며 backpressure는 무제한이다 (`bench_zlink_client.cpp:677`).
- warmup: C client의 기존 500 ms sleep와 1024B readiness probe를 유지했다 (`framework/bench/grpc/c/zlink/bench_zlink_client.cpp:661`). 별도의 4096B 성능 warmup은 없다. 값을 바꾸지 않았다.

## 원인과 배제 근거

### Core의 I/O 배치

1. C client는 request와 send socket을 같은 context에 만들고 연속 connect한다 (`framework/bench/grpc/c/zlink/bench_zlink_client.cpp:634`, `:658`). 둘 다 ROUTER이며 request는 application/completion 연결을 사용한다.
2. application session 선택은 `core/src/runtime/sockets/common/socket_base_endpoint.cpp:516`, completion session 선택은 같은 파일 `:890`이다. 선택된 I/O 스레드로 session을 만든다 (`:643`).
3. 임시 connecter도 같은 선택기를 한 번 더 호출한다 (`core/src/runtime/core/session_base.cpp:850`). connecter가 만든 engine은 원래 session에 attach되고 (`core/src/runtime/transports/tcp/asio_tcp_connecter.cpp:320`), 그 session의 `_io_thread`에서 실행된다 (`core/src/runtime/core/session_base.cpp:484`). connecter의 추가 선택이 engine의 지속 실행 위치를 결정하지는 않는다.
4. 선택기는 context 전체의 `_next_transport_io_thread`를 증가시켜 `thread_count`로 나눈다. 현재 부하나 같은 pair의 기존 배치를 보지 않는다 (`core/src/runtime/core/ctx_io_thread_registry.cpp:96`). 기본 I/O thread 수는 4다 (`core/include/zlink/core/api.h`, `ZLINK_IO_THREADS_DFLT`).
5. 따라서 두 socket의 application 생성·connecter 생성·비동기 completion 생성이 같은 4칸 cursor를 소비한다. 예를 들어 application 두 개와 connecter 두 개가 먼저 선택되면 다음 completion은 첫 application과 같은 칸에 배정될 수 있다. 실제 호출 순서에 따라 배치가 달라진다. 위 구조는 코드 근거이며, 낮은 모드의 실제 동일 객체 배치는 아래 디버거 기록으로 확인했다.

`placement1/stdout.txt`의 request socket `0x5f716c8ee7c0`은 `connect_internal`과 `process_transport_pair_owner_request` 양쪽에서 I/O 객체 `0x5f716c8ebe40`을 선택했다. 두 `engine-plug`도 같은 객체·gdb thread 4에서 실행됐다. 이후 시작 breakpoint를 모두 해제하고 측정한 request-window/backpressure는 299.239/304.652 KOPS다. placement2·3에서도 request의 동일 I/O 객체 배치를 확인했다. 디버거는 시작 순서를 교란할 수 있으므로 이 값은 **진단**이며 정식 집계에 넣지 않았다.

디버거 없는 `free1`의 4096B 구간 가운데 2.76초 동안 client IO/0·IO/1은 각각 CPU 2.22·1.84초를 사용했다. `free2`는 IO/0만 2.51초이고 나머지는 0이다. `pinA1`, `pinB1`도 각각 한 I/O 스레드에 2.52·2.49초가 집중됐다. 같은 submit/completion 코드에서 발생한 배치 차이다. 4096B의 더 큰 송수신·복사 작업이 한 I/O 스레드의 처리 용량에 제한되는 설명과 일치한다. 1024B가 안정적이라는 이유만으로 이 병목을 배제할 수 없다.

소유 계층은 **Core**다. Context의 I/O pool은 `core/doc/spec/core/01-context.ko.md`의 Context 구성·옵션 절, ROUTER의 completion progress lane은 `core/doc/spec/core/socket/README.ko.md:215`가 규정한다. 현재 규격이 lane마다 별도 I/O 스레드를 보장한다고 주장하는 것은 아니다. 확인한 것은 Core가 소유한 배치 결정의 성능 재현성 문제다. Framework runtime 변경은 없어 교차언어 runtime 변경 분류 A/B/C/D는 적용하지 않았다.

### 가설별 판정

| 후보 | 판정과 근거 |
|---|---|
| (a) 외부 부하·CPU 스케줄링 | 수치에 영향을 주는 보조 요인이다. 그러나 낮은 외부 부하의 free1/free2에서도 양 모드가 나오고, CPU 0–7·8–15 고정에서도 낮은 모드가 나온다. active 스레드의 누적 runnable 대기는 해당 약 8.5초 진단에서 대체로 0–2 ms였다. 같은 CPU 0–7의 solo4–6에서는 높은 모드도 재현했다. 외부 부하는 모드 발생에 필요한 조건이 아니다. |
| (b) harness | readiness=true, Submitted=Completed, Errors=0, abandoned=0. window는 모든 해당 셀에서 100까지 찬다. 1024/4096 모두 같은 두 part 생성·malloc·memset 경로 (`bench_zlink_client.cpp:127`, `:178`)와 같은 제출·completion pump (`:193`, `:222`, `:377`)를 사용한다. server의 reply body도 같은 크기 인자의 malloc·memcpy 경로다 (`bench_zlink_server.cpp:95`). harness 수정 근거를 찾지 못했다. |
| (c) Core | 주원인 판정. 위 I/O 선택기·session/engine 배치와 실제 스레드 CPU 사용량이 두 모드를 설명한다. 8192B decode buffer와 frame 잔여 공간에 따른 할당 분기는 존재한다 (`core/src/runtime/core/options.cpp:91`, `core/src/runtime/protocol/zmp_decoder.cpp:281`). 그러나 그 경계를 고치거나 HWM을 조정해야 한다는 증거는 확보하지 않았다. |
| (d) 측정 구간·표본화 | S1 4096B 두 request 셀의 Completed/처리량으로 역산한 경과 시간은 5.0002–5.0010초다. drain까지 분모에 포함하는 구현 (`bench_zlink_client.cpp:414`, `:295`; `bench_common.hpp:302`)의 차이로 58.9%를 설명할 수 없다. latency p95/p99는 처음 200,000개만 보관하지만 mean·throughput은 전체 완료를 사용하고 percentile 정렬은 stop 뒤다 (`bench_common.hpp:154`). percentile 표본 편향과 이번 throughput 재현성은 구별한다. |

4096B만 실행한 solo 진단은 앞선 1024B·send 셀이 없어도 양 모드를 재현한다. 따라서 이전 셀의 warmup·drain·settle 상태가 두 모드 발생에 필요한 조건은 아니다. S1에는 당시 thread 배치 기록이 없으므로 각 S1 run의 실제 배치를 사후 확정할 수는 없다. **같은 바이너리로 재현한 양 모드의 원인을 S1의 같은 증상에 적용한 판정**이다.

### S1 원본의 깊이와 계수

| run | window KOPS | backpressure KOPS | BP Blocked | BP MaxOut | window SubmitMs | BP SubmitMs |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 419.755 | 415.560 | 8,779 | 406 | 1,476.892 | 1,413.008 |
| 2 | 264.236 | 265.908 | 157,541 | 406 | 1,051.364 | 1,276.306 |
| 3 | 261.273 | 263.601 | 160,726 | 406 | 1,091.352 | 1,301.363 |

모든 window 행은 MaxOut=100, Blocked=0이고 모든 위 행은 Submitted=Completed, Errors=0이다. S1 티켓 로그의 route ready=true와 abandoned=0도 확인했다. SubmitMs는 제출 호출 안의 누적 시간이며 전체 대기 시간이 아니다 (`bench_zlink_client.cpp:240`). 완료당 제출 시간은 window 0.704/0.796/0.835 µs, backpressure 0.680/0.960/0.987 µs다. 작은 절대 SubmitMs가 높은 처리량을 뜻하지 않는다.

FB-044의 557/14,111은 2초 smoke에서 관측한 깊이이며 고정해야 할 값이 아니다. 이번 1024B에서는 7,824~14,111 등 서로 다른 깊이에서 오류 없이 처리했고, 4096B 두 모드의 BP MaxOut은 모두 406이다. 따라서 readiness가 깨져 14,110건이 모두 만료됐던 FB-044 이전 증상도, window를 못 채운 FB-010 증상도 아니다.

원본 집계기 재실행에서 `grpc-c`도 request-backpressure@4096 G5 **24.6%**, send-saturation@1024 **12.8%**로 실패했다. 브리프의 “grpc-c 모든 셀 안정”은 원본과 다르다. S1 run3의 RUN_STAMP는 16:41:15이나 실제 티켓 시작은 16:44:51이다. 원본 집계는 `s1-aggregate.txt`에 보존했다.

## 진단 티켓과 결과

아래 값은 모두 **진단**이다. D1은 payload 1024→4096, D2는 4096만 실행했다. 셀마다 active 2초, window 100, MAX_OUTSTANDING 4096, drain 5000 ms를 유지했다. 표는 4096B만 요약한다. 각 행의 순서는 window/backpressure이며 Submitted=Completed, Errors=0, window Blocked=0·MaxOut=100, BP MaxOut=406이다. D1/D2의 `report.txt`, `stderr.log`, `scheduling.jsonl`, DG의 `stdout.txt`가 원본이다.

- D1: `2-1788940235-77476-astra-fwb2-05-diagnostic_2s_unchanged_binary_free_and_` (rc=0).
- DG: `2-1788941057-81804-astra-fwb2-05-diagnostic_startup-only_gdb_IO_placement` (rc=0). 시작 breakpoint만 사용한 3회.
- D2: `2-1788941256-85542-astra-fwb2-05-diagnostic_4096-only_six_starts_without_` (rc=0).

| 티켓·run | CPU affinity | KOPS W/BP | Submitted=Completed W/BP | BP Blocked | SubmitMs W/BP |
|---|---|---:|---:|---:|---:|
| D1 free1 | 미지정 | 429.478 / 454.672 | 859031 / 909568 | 6132 | 634.047 / 632.152 |
| D1 pinA1 | 0–7 | 290.279 / 293.351 | 580601 / 586848 | 72689 | 458.303 / 561.718 |
| D1 pinB1 | 8–15 | 278.697 / 289.646 | 557438 / 579458 | 75755 | 432.928 / 552.554 |
| D1 free2 | 미지정 | 286.929 / 285.043 | 573899 / 570243 | 76119 | 455.238 / 550.373 |
| D1 pinA2 | 0–7 | 286.614 / 264.337 | 573269 / 528826 | 70877 | 475.042 / 565.363 |
| D1 pinB2 | 8–15 | 285.847 / 260.108 | 571734 / 520351 | 71204 | 482.126 / 566.402 |
| DG placement1 | 미지정 | 299.239 / 304.652 | 598523 / 609494 | 77517 | 524.047 / 639.546 |
| DG placement2 | 미지정 | 292.599 / 309.854 | 585243 / 619872 | 79973 | 495.388 / 629.159 |
| DG placement3 | 미지정 | 307.733 / 287.222 | 615517 / 574601 | 75163 | 515.513 / 612.995 |
| D2 solo1 | 미지정 | 300.529 / 308.912 | 601096 / 618014 | 78744 | 518.240 / 635.779 |
| D2 solo2 | 미지정 | 391.512 / 430.671 | 783080 / 861427 | 2482 | 736.865 / 700.028 |
| D2 solo3 | 미지정 | 300.683 / 301.838 | 601415 / 603863 | 78582 | 515.571 / 620.191 |
| D2 solo4 | 0–7 | 365.894 / 437.009 | 731915 / 874119 | 2215 | 724.971 / 740.587 |
| D2 solo5 | 0–7 | 368.523 / 425.727 | 737110 / 851658 | 3392 | 718.486 / 723.480 |
| D2 solo6 | 0–7 | 388.752 / 459.586 | 777580 / 919424 | 13520 | 617.750 / 698.497 |

D1의 free1/pinA1/pinB1/free2에서 workload 밖의 CPU 사용 추정치는 각각 평균 0.43/0.31/0.16/0.33 CPU다. 이는 `/proc/stat`의 busy CPU 시간에서 유지된 벤치 스레드 CPU 시간을 뺀 관측치로, 정확한 격리 보장은 아니다. pinA2/pinB2와 D2에서는 외부 CPU 사용이 더 컸다. 그러므로 taskset만으로 외부 부하를 완전히 제거했다고 주장하지 않는다.

## 변경과 대안

- 변경 파일: `doc/plan/fw-bench-worklog/fwb2-05-summary.md`만 생성. 이번 작업에서는 C harness·Core·집계기·다른 언어·규격·계획·결정 기록을 변경하지 않았다.
- 수정 전/후 규칙 수: **runtime·harness 변경 규칙 0 → 0**. 새 상태·옵션·분기·재시도·warmup을 추가하지 않았다.
- 비교한 대안: harness에서 I/O thread 수·socket 생성 순서·sleep을 조정하면 Core 배치를 간접 강제하는 우회가 된다. 채택하지 않았다. Core 후속에서는 임시 connecter가 이미 선택된 session I/O 스레드를 재사용해 중복 선택을 없애는 방안을 검토할 수 있다. 이 제안은 검증·구현하지 않았고 Core 담당자의 설계·회귀 검토가 필요하다.
- 검증: 동일 S1 바이너리와 library hash 확인, 진단 15회, 기본 전체 runner 3회, 정식 집계기 spec4·G5·judgement 실행. 소스 변경이 없어 새 unit test나 재빌드는 수행하지 않았다.

## 최종 5초 전체 run ×3

아래 전체 run은 affinity 미지정이며 중간 진단을 최종 3-run 집합에 섞지 않았다. 각 run의 `report.txt`와 `[bench]`를 보존한 `stdout.txt`, 옵션을 담은 `conditions.json`, 외부 프로세스 CPU와 스레드 배치를 담은 `scheduling.jsonl`을 함께 보존했다. runner의 부가 request-saturation·send-blocking 셀도 그대로 실행했고 집계기가 규격 밖 행으로 제외한다.

| run | 티켓 | rc |
|---|---|---:|
| final1 | `2-1788940938-43144-astra-fwb2-05-final_full_unchanged_5s_run_1_of_3` | 0 |
| final2 | `2-1788941100-84313-astra-fwb2-05-final_full_unchanged_5s_run_2_of_3` | 0 |
| final3 | `2-1788941256-85702-astra-fwb2-05-final_full_unchanged_5s_run_3_of_3` | 0 |

**외부 부하 한계:** final1 약 129초 중 다른 Java 프로세스가 합계 약 230.5 CPU초, final2에서는 약 51.0 CPU초를 사용했다. load1 범위도 각각 3.47–7.22, 3.58–7.19였다. 시작 시 queue의 quiet 검사를 통과했어도 실행 중 다른 작업을 배제하지는 못한다. final3에서도 Java 약 345.4 CPU초, load1 4.45–8.49를 관측했다. 이 3회는 무부하를 보장한 데이터가 아니다.

원본 `stdout.txt`에서는 stdout buffer와 stderr의 `[bench]`가 같은 줄에 이어져 집계기가 4096B depth marker 일부를 놓쳤다. `collect_markers.py`로 **원본의 `[bench]`부터 newline까지를 순서 그대로** `bench.stdout`에 복사했다(각 run 18개). 원본·계수·metric은 수정하지 않았고 집계기 코드도 변경하지 않았다. 집계기가 기존 지원 파일명으로 depth를 읽게 하는 입력 준비다.

| 4096B 셀 | final1 KOPS | final2 KOPS | final3 KOPS | 중앙값 KOPS | G5 | peak / depth / abandoned |
|---|---:|---:|---:|---:|---|---|
| request-window | 249.478 | 263.165 | 328.864 | 263.165 | **fail 25.0%** | 100 / 93.7 / 0 |
| request-backpressure | 278.803 | 288.043 | 383.360 | 288.043 | **fail 33.1%** | 406 / 392.0 / 0 |

중앙값·G5·depth는 집계기 출력이다. depth는 집계기의 집계 방식(중앙값 throughput × 중앙값 mean latency)을 따른다. 각 최종 request 행도 Submitted=Completed, Errors=0이며 window Blocked=0이다. BP Blocked는 92,544 / 203,439 / 2,874회, SubmitMs는 1,427.083 / 1,398.219 / 1,573.012 ms다. 낮은 처리량 모드의 잦은 backpressure와 높은 처리량 모드의 낮은 latency가 다시 나타난다.

### 집계기 spec4 출력

## Report table (spec 4)

  > Benchmarking current for request-serial...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |       11.06 KOPS |   11.33 MB/s |     0.088 ms |     0.138 ms |     0.175 ms |       2.1% |    14.2 MB |       2.4% |    13.9 MB |
      | zlink-c                 | 1024B    |        6.71 KOPS |    6.87 MB/s |     0.148 ms |     0.231 ms |     0.289 ms |       2.2% |     6.9 MB |       1.3% |     6.7 MB |
      | grpc-c                  | 4096B    |       10.38 KOPS |   42.52 MB/s |     0.092 ms |     0.151 ms |     0.190 ms |       2.2% |  1072.3 MB |       2.3% |  1140.6 MB |
      | zlink-c                 | 4096B    |        6.29 KOPS |   25.76 MB/s |     0.158 ms |     0.235 ms |     0.292 ms |       2.2% |    37.4 MB |       1.5% |    11.4 MB |

  > Benchmarking current for request-window...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |       58.73 KOPS |   60.14 MB/s |     1.677 ms |     2.158 ms |     3.096 ms |       8.7% |    17.7 MB |      24.2% |    25.5 MB |
      | zlink-c                 | 1024B    |      445.87 KOPS |  456.57 MB/s |     0.192 ms |     0.286 ms |     0.348 ms |       8.6% |     9.7 MB |       8.6% |     7.0 MB |
      | grpc-c                  | 4096B    |       50.88 KOPS |  208.39 MB/s |     1.928 ms |     2.667 ms |     3.650 ms |       9.0% |  1072.3 MB |      23.7% |  1140.5 MB |
      | zlink-c                 | 4096B    |      263.17 KOPS | 1077.93 MB/s |     0.356 ms |     0.514 ms |     0.618 ms |       9.1% |    39.7 MB |       8.5% |    10.0 MB |

  > Benchmarking current for request-backpressure...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |       29.68 KOPS |   30.39 MB/s |  2325.235 ms |  3809.341 ms |  3874.811 ms |       6.7% |  1405.7 MB |      16.3% |  1184.2 MB |
      | zlink-c                 | 1024B    |      515.16 KOPS |  527.52 MB/s |     3.031 ms |     2.420 ms |     2.779 ms |       9.7% |    13.8 MB |       9.9% |    11.6 MB |
      | grpc-c                  | 4096B    |       22.72 KOPS |   93.07 MB/s |  3161.580 ms |  6026.586 ms |  6078.138 ms |       7.7% |  1821.7 MB |      10.9% |  1428.1 MB |
      | zlink-c                 | 4096B    |      288.04 KOPS | 1179.83 MB/s |     1.361 ms |     1.701 ms |     1.961 ms |       9.8% |    40.0 MB |       9.0% |    10.7 MB |

  > Benchmarking current for send-saturation...
    Testing local:
      | Implementation          | Size     |       Throughput |    Bandwidth |  Lat.Mean(ms) |   Lat.P95(ms) |   Lat.P99(ms) | Source CPU | Source Mem | Target CPU | Target Mem |
      |-------------------------|----------|------------------|--------------|--------------|--------------|--------------|------------|------------|------------|------------|
      | grpc-c                  | 1024B    |     53.45 KMSG/s |   54.73 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       9.4% |  1072.3 MB |      21.9% |  1140.6 MB |
      | zlink-c                 | 1024B    |    654.99 KMSG/s |  670.71 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       8.1% |    36.9 MB |       4.3% |    11.4 MB |
      | grpc-c                  | 4096B    |     46.27 KMSG/s |  189.51 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       9.1% |  1291.6 MB |      19.2% |  1340.2 MB |
      | zlink-c                 | 4096B    |    410.72 KMSG/s | 1682.30 MB/s |     0.000 ms |     0.000 ms |     0.000 ms |       8.6% |    40.2 MB |       5.7% |    16.3 MB |


## G5 reproducibility

G5 spread is the widest distance of any run from the median of the runs, as a percent of that median. The limit is 10%.

| Pattern | Size | Implementation | runs | spread | G5 |
|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-c` | 3 | 13.7% | **fail** |
| request-serial | 1024 | `zlink-c` | 3 | 31.8% | **fail** |
| request-serial | 4096 | `grpc-c` | 3 | 19.5% | **fail** |
| request-serial | 4096 | `zlink-c` | 3 | 48.8% | **fail** |
| request-window | 1024 | `grpc-c` | 3 | 5.8% | pass |
| request-window | 1024 | `zlink-c` | 3 | 5.0% | pass |
| request-window | 4096 | `grpc-c` | 3 | 9.1% | pass |
| request-window | 4096 | `zlink-c` | 3 | 25.0% | **fail** |
| request-backpressure | 1024 | `grpc-c` | 3 | 4.8% | pass |
| request-backpressure | 1024 | `zlink-c` | 3 | 9.4% | pass |
| request-backpressure | 4096 | `grpc-c` | 3 | 9.1% | pass |
| request-backpressure | 4096 | `zlink-c` | 3 | 33.1% | **fail** |
| send-saturation | 1024 | `grpc-c` | 3 | 1.3% | pass |
| send-saturation | 1024 | `zlink-c` | 3 | 4.2% | pass |
| send-saturation | 4096 | `grpc-c` | 3 | 9.5% | pass |
| send-saturation | 4096 | `zlink-c` | 3 | 12.3% | **fail** |



## Judgement (spec 7.2) on `request-window`

| Formula | Payload | Value | Status | Verdict (>= 0.80) | Reason |
|---|---|---|---|---|---|
| `zlink-c / zlink-c` | 1024 | 1.000 | published | pass | both rows pass G5 |
| `zlink-c / zlink-c` | 4096 | (1.000) | **unsupported** | — | numerator zlink-c-request-window@4096 fails G5 at 25.0% (limit 10%); denominator zlink-c-request-window@4096 fails G5 at 25.0% (limit 10%) |
| `zlink-framework-c / zlink-c` | 1024 | n/a | **unsupported** | — | numerator zlink-framework-c-request-window@1024 was not measured |
| `zlink-framework-c / zlink-c` | 4096 | n/a | **unsupported** | — | numerator zlink-framework-c-request-window@4096 was not measured; denominator zlink-c-request-window@4096 fails G5 at 25.0% (limit 10%) |

**c: incomplete** — 3 of 4 judgement(s) unsupported; spec 7.2 needs both payload sizes


## Judgement (spec 7.2) on `request-backpressure`

| Formula | Payload | Value | Status | Verdict (>= 0.80) | Reason |
|---|---|---|---|---|---|
| `zlink-c / zlink-c` | 1024 | 1.000 | published | pass | both rows pass G5 |
| `zlink-c / zlink-c` | 4096 | (1.000) | **unsupported** | — | numerator zlink-c-request-backpressure@4096 fails G5 at 33.1% (limit 10%); denominator zlink-c-request-backpressure@4096 fails G5 at 33.1% (limit 10%) |
| `zlink-framework-c / zlink-c` | 1024 | n/a | **unsupported** | — | numerator zlink-framework-c-request-backpressure@1024 was not measured |
| `zlink-framework-c / zlink-c` | 4096 | n/a | **unsupported** | — | numerator zlink-framework-c-request-backpressure@4096 was not measured; denominator zlink-c-request-backpressure@4096 fails G5 at 33.1% (limit 10%) |

**c: incomplete** — 3 of 4 judgement(s) unsupported; spec 7.2 needs both payload sizes

위 C-only 집계의 `zlink-c / zlink-c`는 분모의 G5 확인을 위한 자기 비교다. 실제 다른 언어의 formula 1을 측정한 결과가 아니다. `zlink-framework-c`는 존재하지 않는 행이므로 이 행의 unsupported와 C-only `incomplete`를 C 구현 누락으로 해석하지 않는다.

## 게재 판정과 BLOCKERS

- **Formula 1 @4096: 게재 불가.** 두 request 패턴의 C 분모가 G5를 실패한다. S2S 계획 §9·FB-011에 따라 이 분모를 사용한 비율을 게재하지 않는다. 1024B C 행 자체는 window 5.0%, backpressure 9.4%로 G5를 통과하지만, 실제 formula 1은 분자도 같은 검증을 통과해야 한다.
- **BLOCKER — Core 배치 재현성:** 같은 API 호출·같은 바이너리에서 지속 I/O 작업이 한 스레드에 집중되는 경우가 남아 있다. Core 담당자가 위 선택 경로와 reconnect를 함께 검토하고 고친 뒤 기준선을 다시 측정해야 한다. 이번 작업에서 Core를 변경하거나 harness로 보상하지 않았다.
- **BLOCKER — 실행 중 외부 부하:** queue의 시작 시 quiet 판정만으로 빌드·테스트와의 중첩을 막지 못했다. 최종 3회에도 다른 Java 프로세스가 CPU를 사용했다. Core 후속 검증 때 외부 작업을 통제해야 한다. 외부 부하가 없는 경우에만 나타나는 문제라는 뜻은 아니다.
- 나머지 G5 실패는 위 원문 표대로 request-serial의 네 행과 zlink-c send-saturation@4096(12.3%)이다. 범위 밖 원인을 임의 수정하지 않았다. C send-saturation은 기존 client 제출 수를 세는 G3 문제도 그대로 남아 있다 (`bench_zlink_client.cpp:484`, FB-014). 집계기는 `client (G3 fail)`과 `no client parallelism ceiling declared`를 보고한다. 이 셀을 server active 수신률로 인용하지 않는다.
- 규격·계획·판정 기록의 개정은 없다. 요청한 진단과 3회 확인은 완료했지만 **기준선 안정화는 Core 후속을 기다리는 상태**다. 실패를 없애려고 window·payload·5초 active·MAX_OUTSTANDING·drain·warmup을 바꾸지 않았다.

## 재검증 명령

현재 보존한 결과를 재집계하는 명령이다. 측정은 실행하지 않는다.

```bash
python3 /tmp/zlink-astra-fwb2-05/collect_markers.py
python3 framework/bench/grpc/tools/bench_aggregate.py \
  --lang c --judgement-pattern request-window \
  --runs-glob '/tmp/zlink-astra-fwb2-05/final[123]' --format full
python3 framework/bench/grpc/tools/bench_aggregate.py \
  --lang c --judgement-pattern request-backpressure \
  --runs-glob '/tmp/zlink-astra-fwb2-05/final[123]' --format judgement
```

같은 공개 C API repro를 다시 실행하는 명령이다. server/client는 원래 벤치 바이너리이며 Core internal API를 호출하는 repro 코드는 추가하지 않았다. 아래는 4096B·2초 **진단**이다. script는 server를 기본 `all`로 기동하고 client에서 두 request 셀만 실행한다.

```bash
bash scripts/perf/perf-ticket.sh submit -p 2 -o astra-fwb2-05 \
  -d 'diagnostic 4096 placement repeat' -- \
  python3 /tmp/zlink-astra-fwb2-05/diagnose4096.py \
  repro1:free repro2:free repro3:0-7
```

5초 전체 run은 새 디렉터리 이름으로 각 티켓을 완료까지 기다린다. Core 후속 바이너리를 검증할 때는 기존 `bin/`을 무심코 재사용하지 말고 새 빌드·실제 연결 library·provenance를 먼저 확인해야 한다. 아래는 이번에 보존한 0.17.5 바이너리를 재현하는 명령이다.

```bash
for run in 1 2 3; do
  bash scripts/perf/perf-ticket.sh submit -p 2 -o astra-fwb2-05 \
    -d "recheck full unchanged 5s run ${run}" -- \
    python3 /tmp/zlink-astra-fwb2-05/full_run.py "recheck${run}" free || break
done
```

새 빌드가 필요하면 측정과 겹치지 않는 시간에 load average <10을 확인하고 하나씩 `--parallel 2`로 빌드한다. 이번 조사에서는 S1과 같은 바이너리를 사용하기 위해 빌드를 수행하지 않았다.
