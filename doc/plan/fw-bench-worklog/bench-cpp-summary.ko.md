# C++ with-grpc bench 요약 (Phase 5)

이 문서는 `cpp` 언어의 with-grpc bench 측정 결과를 하나로 모은 기록이다. Phase 6 보고서가
이 문서만 읽고도 `cpp` 행을 서술할 수 있도록 조건, 수치, 판정, 판정할 수 없는 항목을 함께
남긴다.

계획: [`../framework-bench-with-grpc-5lang-plan.ko.md`](../framework-bench-with-grpc-5lang-plan.ko.md)
결정: [`decisions.ko.md`](./decisions.ko.md)
규격: [`../../../framework/doc/framework/common/bench/with-grpc-local.ko.md`](../../../framework/doc/framework/common/bench/with-grpc-local.ko.md)
인접 요약: [`bench-kotlin-summary.ko.md`](./bench-kotlin-summary.ko.md) · [`bench-java-summary.ko.md`](./bench-java-summary.ko.md)

## 1. 결론 먼저

### 1.1 `zlink-cpp`는 window 100을 오류 없이 지탱한다. 완료 전달 결함은 관리형 런타임 binding에 한정된다

이 job이 존재한 이유는 하나였다. `bindings/cpp`는 C API의 얇은 계층이므로, C++ raw 행이 C처럼
깊이를 지탱하면 지금까지의 완료 전달 실패는 관리형 런타임 binding 쪽 문제로 좁혀지고,
C++도 무너지면 C bench의 특정 사용 형태 밖에서는 공통 문제라는 뜻이 된다.

**C++는 지탱한다.** ROUTER 3회와 DEALER 1회, payload 두 종, 곧 `zlink-cpp` request-window
여덟 셀 전부가 아래 서명을 낸다.

| 구성 | payload | 처리량 | 실제 깊이 | `peak_in_flight` | abandoned | 오류 | header 검증 실패 |
|---|---|---|---|---|---|---|---|
| ROUTER (3회 중앙값) | 1024 | 331,443/s | **99.9** | 100 / 100 | 0 | 0 | 0 |
| ROUTER (3회 중앙값) | 4096 | 278,284/s | **99.8** | 100 / 100 | 0 | 0 | 0 |
| DEALER (1회) | 1024 | 325,647/s | **99.9** | 100 / 100 | 0 | 0 | 0 |
| DEALER (1회) | 4096 | 291,560/s | **99.8** | 100 / 100 | 0 | 0 | 0 |

window는 채워졌고, 채워진 100개가 **전부 완료됐다.** 네 run 48셀 전체에서 오류 0, header 검증
실패 0, abandoned 0, 오염 0, 실패 셀 0이다.

이 값을 앞 네 언어와 나란히 놓으면 대비가 분명해진다.

| 실행 | request-window depth 100 설정에서의 관측 | C API 접근 |
|---|---|---|
| `zlink-c` | 깊이 86.9, 오류 0 | 직접 |
| **`zlink-cpp`** | **깊이 99.9, 오류 0, abandoned 0** | **얇은 wrapper** |
| `zlink-dotnet` | 깊이 8에 묶임. 제출 비용이며 유실은 없다(FB-016) | 관리형 |
| `zlink-node` | depth 8까지 정상, 16 이상 socket 정지(FB-026) | 관리형 |
| `zlink-java` | outstanding 2 이상 유실 시작, window 100에서 완료 0(FB-030) | 관리형 |
| `zlink-kotlin` | Java와 같은 서명(같은 binding 재사용, 독립 관측 아님) | 관리형 |

**C API를 직접 지나는 두 런타임은 둘 다 window를 지탱하고, 관리형 런타임 binding 세 개는 각각
다른 방식으로 무너진다.** 그러므로 FB-026·FB-030·FB-016은 "공유된 Core 경로일 수도 있다"에서
**"각 관리형 binding의 완료 전달 계층"** 으로 옮겨 조사하는 것이 맞다. 결정 기록 FB-037.

### 1.2 이 결과가 말하지 않는 두 가지

위 문장을 넘겨 읽지 않도록 한계를 함께 적는다.

- **이 실험이 Core를 지목하지 않는다는 것이지, Core가 건전하다는 것이 아니다.** C와 C++가
  지탱한 것은 이 bench가 만든 부하 형태에서다. 다른 사용 형태에서 Core가 같은 결함을 내지
  않는다는 근거는 이 자료에 없다.
- **관리형 binding 세 개가 서로 다른 방식으로 무너지므로 공통 원인 하나를 가정할 수 없다.**
  `.NET`은 제출 비용에 묶여 깊이가 오르지 않았고 유실은 없었다. Node는 socket이 정지했다.
  Java는 reply가 생성된 뒤 사라졌다. 세 관측은 증상이 다르고, "관리형 binding 계층"은
  조사 범위이지 원인이 아니다.

### 1.3 프로세스 CPU는 ZLink client의 포화 계측기로 쓸 수 없다 — 언어 중립 결론

C++는 이 캠페인에서 **세 번째로** 같은 정정을 요구했고, 이제 이것은 언어별 사정이 아니라
캠페인 수준의 결론이다.

| 언어 | 프로세스 CPU가 함께 센 것 | 선언한 계측기 |
|---|---|---|
| Node | binding의 native I/O thread (FB-023) | `event_loop_utilization` |
| Java · Kotlin | GC와 JIT thread (FB-032) | `jvm_thread_cores` |
| **C++** | **binding I/O thread (FB-037)** | **`submit_thread_cores`** |
| `.NET` | — | `client_cores` |

**세 언어, 세 가지 다른 이유, 같은 오답.** 다섯 언어 중 넷이 프로세스 CPU가 아닌 것을
선언했다. 규격 §5.1이 계측기를 언어별 **선언**으로 둔 것은 이 결과로 정당화된다.

문제는 임계값이 아니라 **판정식이 나누는 두 행 사이의 비교 가능성**이다. C++ 실측이 그것을
가장 짧게 보여준다. 같은 run, 같은 셀 좌표다.

| 행 (request-window @1024) | 선언 계측기 `submit_thread_cores` | 프로세스 core |
|---|---|---|
| `zlink-cpp` | 0.950 | **1.90** |
| `grpc-cpp` | 0.700 | **0.70** |

`grpc-cpp`는 두 값이 사실상 같고 `zlink-cpp`는 프로세스 core가 2배가 넘는다. 그 차이는 client
런타임이 더 바빠서가 아니라 **어느 행이 Core를 링크하는가**다. 프로세스 core로 포화를
판정하면 표시가 client 포화가 아니라 그 사실을 보고하게 된다.

집계기에 네 번째 계측기 `submit_thread_cores`를 추가했다(감독관 승인, DECISION 3). 상한 1,
`client_cores`를 관측값으로 나란히 싣는다.

### 1.4 formula 1 @1024는 `0.774`로 게재하고, @4096은 게재할 수 없다

| 판정식 | payload | 값 | 상태 | 판정 |
|---|---|---|---|---|
| `zlink-cpp / zlink-c` | 1024 | **0.774** | published | **미달** (기준 0.80) |
| `zlink-cpp / zlink-c` | 4096 | (0.893) | `unsupported` | — |
| `zlink-framework-cpp / zlink-cpp` | 1024 | n/a | `unsupported` | — |
| `zlink-framework-cpp / zlink-cpp` | 4096 | n/a | `unsupported` | — |

@1024는 분자와 분모가 **모두 G5를 통과**하므로(각각 0.4%, 7.4%) FB-011을 만족하고 게재한다.
값은 0.774로 기준 0.80에 **미달**한다. 캠페인 전체에서 게재 조건을 만족한 formula 1은
`.NET`의 0.084(FB-015)에 이어 이것이 두 번째다.

@4096은 **공유 분모 때문에** 게재할 수 없다. `zlink-c` request-window @4096이 G5 28.7%로
미달한다. FB-034가 기록한 대로 이것은 C++의 실패가 아니라 **캠페인 수준의 사실**이며, 규격
§7.2가 두 payload 크기를 모두 요구하므로(FB-005) 현재 기준선 위에서 formula 1 통과 자체가
어느 언어에서도 달성 불가능하다.

**FB-034의 구간 수를 정확히 적는다.** 지금까지 기록된 네 값 75.7 / 25.7 / 28.7 / 28.7 중
뒤의 둘은 **같은 측정을 재사용한 값**이다(Phase 3 Java 구간에서 측정, Phase 4 Kotlin이 재사용).
이 구간도 그 값을 다시 재사용했으므로(§2.7) **독립 측정은 세 구간이고 이 구간이 넷째를 더하지
않는다.** FB-034의 결론은 그대로 유지되지만 근거의 개수는 셋이다.

### 1.5 `zlink-framework-cpp` 여섯 셀은 측정하지 않았다

framework 행은 이 Phase에서 **구현하지 않았고 따라서 측정하지 않았다.** 감독관 지시에 따라
깊이 판정을 우선하고 예산을 늘리지 않았다. 네 판정 중 두 개가 `unsupported`인 사유는
**"이 Phase에서 미구현"** 이며, Node의 codec gap(FB-028, 공개 codec이 규격이 정한 payload를
실을 수 없다)과는 **성격이 다르다.** Node는 제품의 선언 격차였고 이쪽은 단순 미구현이다.
추정값을 채워 넣지 않는다.

## 2. 측정 대상과 조건

### 2.1 비교 대상

| 구현 이름 | 내용 | 상태 |
|---|---|---|
| `grpc-cpp` | 시스템 `libgrpc++` 1.51.1 async unary, `grpc_cpp_plugin` 생성 stub | 측정 |
| `zlink-cpp` | `bindings/cpp` raw, ROUTER↔ROUTER (규격 §1.3) | 측정 |
| `zlink-framework-cpp` | `framework/languages/cpp/framework` RouteMesh channel | **미구현·미측정** |
| `zlink-c` · `grpc-c` | 바닥 기준 (Java 구간 값 재사용, §2.7) | 재사용 |

### 2.2 제출 모델 — 단일 application thread, 그리고 그것이 옳은 이유

세 driver 모두 **제출 루프와 완료 드레인을 하나의 application thread에서** 돌리고, 동시성은
스레드 수가 아니라 **미완료 연산 수**로 표현한다. 선언한 제출 병렬도는 모든 셀에서 1이다.

이것은 편의가 아니라 formula 1이 성립하기 위한 조건이다. 분모인 `zlink-c`의 client가
단일 스레드 제출 루프이므로(`bindings/c/bench/with_grpc/zlink/bench_zlink_client.cpp`),
C++ 분자를 멀티 스레드로 두면 서로 다른 실험을 나누게 된다. FB-024가 wire 모양에 대해 요구한
것과 같은 이유를 driver 모양에 적용한 것이다.

C++ binding의 request 종단은 `async()`(awaitable)와 `submit()`(블로킹) 둘인데, 블로킹 종단은
한 번에 하나만 열 수 있다. 그래서 window 100은 **자기 reply를 기다리는 coroutine 100개**로
표현했고, 이는 `bindings/cpp/perf`가 쓰는 것과 같은 모양이다(하나의 application thread,
하나의 ready queue, `poller_t::wait`가 socket-local 완료 드레인을 구동). 공개 API만 사용했고
두 번째 poller도, 측정 구간 내 재시도도 없다(G4).

### 2.3 wire 모양은 `zlink-c`와 같다 (FB-024)

raw 행은 JSON envelope 헤더 part 하나와 손으로 인코딩한 protobuf `BenchPayload` part 하나,
모두 두 part를 보낸다. 측정 header 29바이트는 그 `bytes body` 안에 들어간다. protobuf
런타임에 의존하지 않도록 field 1을 직접 인코딩해 C bench와 바이트 단위로 맞췄다.

### 2.4 고정 조건

| 항목 | 값 |
|---|---|
| payload | 1024, 4096 bytes |
| `request_window` | 100 |
| send concurrency | 8 |
| active duration | 5 s |
| warmup | **5 s** (10구간 × 0.5 s, 근거 §8) |
| request timeout | 30,000 ms |
| drain 상한 | 30,000 ms (규격 §3 기준값) |
| 빌드 | Release (`-O3`) |
| endpoint | `127.0.0.1`, 규격 §9의 cpp 대역 5111–5117 |

### 2.5 실행 환경과 버전

| 항목 | 값 |
|---|---|
| CPU | Intel(R) Core(TM) Ultra 7 265K, 논리 core 20 |
| 커널 | 6.6.87.2-microsoft-standard-WSL2 |
| commit | `c4159de134bf53e883b044bd4d94d29e77141daf` |
| 컴파일러 | GCC 13.3.0, C++20 |
| gRPC | **시스템 `libgrpc++` 1.51.1** (2022년 배포판, vcpkg 미사용) |
| protobuf | 3.21.12 |
| gRPC server 구성 | `grpc::ServerBuilder` 동기 server, `InsecureServerCredentials`, plaintext loopback, **옵션 무변경** |
| 측정 시각 | 2026-09-07 05:23:04 ~ 05:33:47 (KST) |

gRPC 1.51.1은 오래된 버전이며 규격 §8.1이 기록을 요구한다. 다만 이 구간에서 `grpc-cpp`와
`grpc-c`가 같은 시스템 라이브러리를 쓰고 request-window에서 65.31 대 65.86 KOPS로 사실상
같은 값을 내므로, gRPC 쪽 수치는 두 bench 사이에서 일관된다.

### 2.6 측정 격리와 게이트 값 (G7)

빌드는 전부 측정 구간 **밖**에서 끝냈다. C++ framework Release 빌드(20코어를 점유한다)는
측정 시작 전에 완료했고, 측정 구간 안에서는 어떤 빌드·테스트·집계도 돌리지 않았다. 집계기
테스트와 집계는 구간 종료 뒤에 실행했다.

구간 전체를 `flock --exclusive /tmp/zlink-perf.lock` 하나로 잠갔고, **run마다** loadavg를
확인했다. 게이트는 2.0이고 넘으면 대기했다.

| run | 시작 loadavg1 | 게이트 | rc |
|---|---|---|---|
| `cpp-router-1` | 1.41 | 2.0 | 0 |
| `cpp-router-2` | 1.93 | 2.0 | 0 |
| `cpp-router-3` | 1.81 | 2.0 | 0 |
| `cpp-dealer-1` | 1.79 | 2.0 | 0 |

`cpp-router-3`은 앞 run의 부하가 2.98에서 내려오기를 33초 대기한 뒤 1.81에서 시작했다. 게이트가
실제로 발동한 기록이다. run마다 server를 새로 띄우므로 한 run의 잔여가 다음 run으로 넘어가지
않는다. 긴 스크립트는 bench 호출을 stub으로 둔 `DRY_RUN=1`로 먼저 예행했고, 포트 점유 감지와
loadavg 게이트가 각각 정상 중단하는 것을 확인한 뒤 실행했다. 실행 중에는 `ps`로 client와
server 프로세스의 생존을 확인했다.

### 2.7 `zlink-c` 기준선은 Java 구간의 값을 재사용했다

이 구간에서 C 기준 bench를 다시 측정하지 않고 Phase 3 Java 구간의
`bindings/c/bench/with_grpc/log/20260907_030655/c-router-*`를 재사용했다. **명시적으로 적는다.**

- 두 구간 사이에 바뀐 것은 bench 코드·집계기·문서뿐이며 Core·binding·framework source는
  바뀌지 않았다. 같은 머신, 같은 커널이다.
- 그러므로 이 구간은 FB-034에 **독립 관측을 더하지 않는다**(§1.4).
- @1024 판정이 0.774로 임계값 0.80에 가깝다는 점을 감안하면 같은 구간 안에서 기준선을 다시
  재는 편이 더 엄격했을 것이다. 다만 @4096이 어차피 게재 불가이므로 두 크기를 모두 만족해야
  하는 규격 §7.2 아래에서 판정 결과는 바뀌지 않는다.

## 3. 측정 표 — payload 1024 (ROUTER 3회 중앙값)

| Implementation | Size | Throughput | Bandwidth | Lat.Mean | Lat.P95 | Lat.P99 | Client CPU | Client Mem | Server CPU | Server Mem |
|---|---|---|---|---|---|---|---|---|---|---|
| `grpc-c` | 1024B | 15.28 KOPS | 15.64 MB/s | 0.064 ms | 0.091 ms | 0.119 ms | 1.9% | 14.9 MB | 2.4% | 13.8 MB |
| `grpc-cpp` | 1024B | 16.25 KOPS | 16.64 MB/s | 0.060 ms | 0.081 ms | 0.123 ms | 1.9% | 15.9 MB | 2.6% | 15.2 MB |
| `zlink-c` | 1024B | 8.29 KOPS | 8.48 MB/s | 0.120 ms | 0.139 ms | 0.170 ms | 2.3% | 7.2 MB | 1.3% | 6.6 MB |
| `zlink-cpp` | 1024B | 8.97 KOPS | 9.19 MB/s | 0.111 ms | 0.141 ms | 0.215 ms | 2.3% | 17.0 MB | 1.4% | 7.6 MB |

request-serial. 위는 `request-serial`, 아래는 `request-window`와 `send-saturation`이다.

| Implementation | Size | Throughput | Bandwidth | Lat.Mean | Lat.P95 | Lat.P99 | Client CPU | Client Mem | Server CPU | Server Mem |
|---|---|---|---|---|---|---|---|---|---|---|
| `grpc-c` | 1024B | 65.86 KOPS | 67.44 MB/s | 1.501 ms | 1.662 ms | 1.760 ms | 8.9% | 17.7 MB | 26.8% | 21.3 MB |
| `grpc-cpp` | 1024B | 65.31 KOPS | 66.88 MB/s | 1.530 ms | 1.712 ms | 1.821 ms | 3.5% | 20.7 MB | 27.3% | 34.6 MB |
| `zlink-c` | 1024B | 428.14 KOPS | 438.41 MB/s | 0.203 ms | 0.289 ms | 0.341 ms | 8.6% | 10.0 MB | 8.9% | 7.1 MB |
| `zlink-cpp` | 1024B | 331.44 KOPS | 339.39 MB/s | 0.301 ms | 0.353 ms | 0.408 ms | 9.5% | 21.4 MB | 8.8% | 32.8 MB |

request-window.

| Implementation | Size | Throughput | Bandwidth | Lat.Mean | Lat.P95 | Lat.P99 | Client CPU | Client Mem | Server CPU | Server Mem |
|---|---|---|---|---|---|---|---|---|---|---|
| `grpc-c` | 1024B | 62.97 KMSG/s | 64.48 MB/s | — | — | — | 9.4% | 95.9 MB | 26.1% | 89.6 MB |
| `grpc-cpp` | 1024B | 45.38 KMSG/s | 46.47 MB/s | 0.112 ms | 0.164 ms | 0.205 ms | 3.2% | 23.0 MB | 11.1% | 41.8 MB |
| `zlink-c` | 1024B | 699.35 KMSG/s | 716.13 MB/s | — | — | — | 8.1% | 35.8 MB | 4.7% | 8.3 MB |
| `zlink-cpp` | 1024B | 696.42 KMSG/s | 713.14 MB/s | 0.443 ms | 0.987 ms | 1.100 ms | 8.2%\* | 23.0 MB | 5.2% | 52.2 MB |

send-saturation. `\*` 포화 셀(§5). C 행의 latency가 비어 있는 것은 C bench가 send 셀에서
latency를 재지 않기 때문이고(0.000을 낸다), C 행의 처리량은 client 제출 수라서 G3 미달이다
(FB-014). C++ 행은 server 수신 수다.

## 4. 측정 표 — payload 4096 (ROUTER 3회 중앙값)

| Pattern | Implementation | Throughput | Bandwidth | Lat.Mean | Lat.P95 | Lat.P99 | Client CPU | Server CPU |
|---|---|---|---|---|---|---|---|---|
| request-serial | `grpc-c` | 14.77 KOPS | 60.49 MB/s | 0.066 ms | 0.084 ms | 0.122 ms | 1.9% | 2.5% |
| request-serial | `grpc-cpp` | 14.94 KOPS | 61.19 MB/s | 0.065 ms | 0.084 ms | 0.122 ms | 1.9% | 2.5% |
| request-serial | `zlink-c` | 8.35 KOPS | 34.20 MB/s | 0.119 ms | 0.138 ms | 0.163 ms | 2.4% | 1.6% |
| request-serial | `zlink-cpp` | 8.79 KOPS | 36.02 MB/s | 0.113 ms | 0.147 ms | 0.206 ms | 2.3% | 1.4% |
| request-window | `grpc-c` | 62.97 KOPS | 257.93 MB/s | 1.569 ms | 1.768 ms | 1.897 ms | 8.9% | 27.0% |
| request-window | `grpc-cpp` | 63.34 KOPS | 259.45 MB/s | 1.577 ms | 1.802 ms | 1.911 ms | 3.6% | 27.5% |
| request-window | `zlink-c` | 311.80 KOPS | 1277.13 MB/s | 0.280 ms | 0.421 ms | 0.502 ms | 9.3% | 9.5% |
| request-window | `zlink-cpp` | 278.28 KOPS | 1139.85 MB/s | 0.359 ms | 0.412 ms | 0.447 ms | 10.4% | 9.6% |
| send-saturation | `grpc-c` | 61.41 KMSG/s | 251.54 MB/s | — | — | — | 9.5% | 26.3% |
| send-saturation | `grpc-cpp` | 44.51 KMSG/s | 182.33 MB/s | 0.114 ms | 0.165 ms | 0.212 ms | 3.3% | 11.0% |
| send-saturation | `zlink-c` | 485.77 KMSG/s | 1989.70 MB/s | — | — | — | 8.7% | 6.0% |
| send-saturation | `zlink-cpp` | 514.68 KMSG/s | 2108.12 MB/s | 0.378 ms | 0.492 ms | 0.585 ms | 8.3% | 6.3% |

### 4.1 `send-saturation` 셀의 서술 (FB-002, 규격 §2.1)

이 셀을 전송 속도 차이로 서술하지 않는다. **응답이 필요 없는 명령을 처리할 때, gRPC는 unary
왕복을 치러야 하고 ZLink는 단방향 send로 끝난다. 이 조건에서 그 차이는 @1024에서 15.3배,
@4096에서 11.6배로 관찰됐다**(`zlink-cpp` 696.42 대 `grpc-cpp` 45.38 KMSG/s,
514.68 대 44.51 KMSG/s). 이 값은 두 스택이 같은 업무에 요구하는 구현이 다르다는 사실을 포함한
값이며, @1024 셀은 포화 셀이므로(§5) 처리량 우열 판정에는 쓰지 않는다.

## 5. 포화와 깊이 (규격 §5.1, G6, G8)

선언 계측기는 `submit_thread_cores`, 선언 상한은 **1**(제출과 완료 드레인을 도는 application
thread 하나)이다. 0.95배 이상이면 포화다.

| 패턴 | payload | 구현 | `peak_in_flight` | window | abandoned | 실제 깊이 | 계측기 값 | 프로세스 core | 포화 |
|---|---|---|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-cpp` | 1 | 1 | 0 | 1.0 | 0.368 | 0.37 | 아니오 |
| request-serial | 1024 | `zlink-cpp` | 1 | 1 | 0 | 1.0 | 0.182 | 0.46 | 아니오 |
| request-serial | 4096 | `grpc-cpp` | 1 | 1 | 0 | 1.0 | 0.382 | 0.38 | 아니오 |
| request-serial | 4096 | `zlink-cpp` | 1 | 1 | 0 | 1.0 | 0.180 | 0.47 | 아니오 |
| request-window | 1024 | `grpc-cpp` | 100 | 100 | 0 | 99.9 | 0.700 | 0.70 | 아니오 |
| request-window | 1024 | `zlink-cpp` | 100 | 100 | 0 | **99.9** | **0.950** | **1.90** | 아니오 |
| request-window | 4096 | `grpc-cpp` | 100 | 100 | 0 | 99.9 | 0.726 | 0.73 | 아니오 |
| request-window | 4096 | `zlink-cpp` | 100 | 100 | 0 | **99.8** | 0.928 | **2.08** | 아니오 |
| send-saturation | 1024 | `grpc-cpp` | 8 | 8 | 0 | 5.1 | 0.638 | 0.64 | 아니오 |
| send-saturation | 1024 | `zlink-cpp` | 8 | 8 | 0 | 308.8 | **0.964** | 1.64 | **예** |
| send-saturation | 4096 | `grpc-cpp` | 8 | 8 | 0 | 5.1 | 0.650 | 0.65 | 아니오 |
| send-saturation | 4096 | `zlink-cpp` | 8 | 8 | 0 | 194.4 | 0.878 | 1.67 | 아니오 |

읽을 때 주의할 점 셋을 적는다.

- **`zlink-cpp` request-window @1024는 포화 임계값을 0.00003 차이로 비켜갔다.** 세 run의
  판독값은 0.954 / 0.944 / 0.950이고 중앙값은 **0.94997**, 임계값은 0.95다. 곧 이 셀의
  판정(0.774)은 **client 포화 배제를 사실상 동점에서 면한 상태로 게재된 값**이다. 이 client
  구성이 transport보다 먼저 자기 application thread를 채웠다는 사실과 반드시 함께 읽어야
  하고, run 하나만 달랐어도 배제됐을 값으로 취급해야 한다. Node에서 같은 상황이 판정 게재를
  막았다(FB-027). **이 0.774를 "C++ binding이 C의 77%를 낸다"로 인용하면 안 된다.**
- **`zlink-cpp`의 깊이가 `zlink-c`보다 높은데 처리량은 낮다.** `zlink-c`는 같은 설정에서 깊이
  86.9인데 428.14 KOPS를 내고, `zlink-cpp`는 깊이 99.9에서 331.44 KOPS를 낸다. 미완료 수가 더
  많은데 완료가 더 적다는 것은 **요청당 client 비용이 C보다 크다**는 뜻이다. 요청마다 coroutine
  frame과 `async_result_t` 공유 상태가 하나씩 생기는 C++ 경로의 성질이 후보이지만,
  **프로파일하지 않았으므로 단정하지 않고 후보로만 적는다**(§12).
- **`send-saturation`의 실제 깊이가 설정 8을 크게 넘는다**(308.8, 194.4). 이 값은 Little's law로
  계산한 값인데, send 셀의 latency는 client 왕복이 아니라 **server가 header로 계산한 수신
  지연**이다(규격 §5). 곧 분자와 분모가 다른 지점을 재고 있어 이 셀의 "깊이"는 미완료 제출
  수로 읽을 수 없다. `peak_in_flight`는 정확히 8이고 abandoned는 0이다.

## 6. 0.80 판정 (규격 §7.2)

§1.4의 표가 결론이다. 게재 조건은 FB-011대로 분자와 분모가 **모두** G5를 통과할 때이며,
`zlink-cpp / zlink-c` @1024만 그 조건을 만족한다.

```text
zlink-cpp / zlink-c            @1024 = 331,443 / 428,138 = 0.774   미달 (게재)
zlink-cpp / zlink-c            @4096 = (0.893)                     unsupported — 분모 G5 28.7%
zlink-framework-cpp / zlink-cpp @1024 = n/a                        unsupported — 분자 미구현
zlink-framework-cpp / zlink-cpp @4096 = n/a                        unsupported — 분자 미구현
```

규격 §7.2가 두 크기를 모두 요구하므로 **`cpp`는 통과가 아니고 완전한 판정도 불가능하다.**

## 7. 완료 전달 관측

§1.1의 표가 이 항목의 본문이다. 여기서는 이 구간이 그 표에 더한 것만 적는다.

- **ROUTER와 DEALER 양쪽에서 같다.** 여덟 셀 전부 깊이 99.8 이상, abandoned 0, 오류 0이다.
  Kotlin이 정지를 두 소켓 종류에서 확인한 것과 대칭인 관측이다.
- **오류가 0이라는 것은 검증된 0이다.** request 계열은 reply의 29바이트 header를 client가
  검증하고(G2), 검증 실패는 완료가 아니라 실패로 센다. 48셀 전체에서 header 검증 실패 0이다.
- **`peak_in_flight`가 설정값에 정확히 도달한다.** FB-010이 `.NET`에서 잡아낸 "harness가
  window를 못 채운다" 유형이 아님을 이 한 줄이 배제한다.

## 8. warmup 안정화 증거 (규격 §8.2)

**warmup 5초를 사용했고, 근거는 이 구간 자체의 구간별 자료다.** Java·Kotlin의 20초를 물려받지
않았다. C++에는 JIT가 없어 예열할 대상이 없기 때문이다.

`cpp-router-1`의 request-window 셀, 0.5초 구간 10개와 active 값이다.

| 셀 | 구간별 처리량 | active |
|---|---|---|
| `zlink-cpp` @1024 | 325388, 327852, 332578, 329104, 334588, 331861, 333572, 331649, 331539, 334499 | 330,164 |
| `zlink-cpp` @4096 | 262725, 268076, 282945, 286128, 281840, 287566, 280463, 278481, 285662, 286140 | 278,284 |
| `grpc-cpp` @1024 | 63058, 66460, 61330, 63552, 65427, 66619, 66476, 66474, 66383, 66164 | 65,805 |
| `grpc-cpp` @4096 | 62860, 63830, 63890, 64230, 64257, 63779, 64236, 63977, 64225, 63819 | 63,449 |

**첫 0.5초 구간이 이미 active 값의 98.6%(`zlink-cpp` @1024)이고 상승 추세가 없다.** @4096은
첫 구간이 active의 94.4%이며, 열 구간 **전부가** 구간 중앙값 ±10% 안에 있다(첫 구간의 편차가
6.96%로 가장 크다). 곧 5초는 넉넉하고 첫 0.5초만으로도 정상 상태였다. Java가 request-serial에서
관측한 3.2배 상승(1580 → 5004) 같은 예열 곡선은 C++ 어느 셀에도 없다. warmup을 5초로 둔 것은
예열이 필요해서가 아니라 앞 셀의 잔여가 빠지는 시간을 공통으로 확보하기 위해서다.

## 9. G5 재현성

ROUTER 3회 기준. **`cpp` 행 12개 전부 통과**하고, 미달은 재사용한 `zlink-c` 행 세 개뿐이다.

| 패턴 | payload | `grpc-cpp` | `zlink-cpp` | `grpc-c` | `zlink-c` |
|---|---|---|---|---|---|
| request-serial | 1024 | 2.3% | 0.7% | 4.4% | **37.9% 미달** |
| request-serial | 4096 | 2.6% | 2.3% | 0.5% | **12.6% 미달** |
| request-window | 1024 | 1.4% | 0.4% | 1.0% | 7.4% |
| request-window | 4096 | 1.5% | 1.4% | 2.6% | **28.7% 미달** |
| send-saturation | 1024 | 0.9% | 2.8% | 3.3% | 2.9% |
| send-saturation | 4096 | 0.4% | 3.8% | 1.1% | 1.0% |

`zlink-cpp`의 최대 스프레드가 3.8%이고 판정 셀은 0.4%다. 이 구간에서 `cpp` 행은 재현성 문제가
없다. `zlink-c` request-serial 두 행의 미달(37.9%, 12.6%)은 판정 경로 밖이지만 기준 bench의
불안정이 request-window @4096에만 국한되지 않는다는 신호이므로 기록해 둔다.

## 10. 게이트

| 게이트 | 결과 |
|---|---|
| G1 계약 정합 | **부분.** 3패턴 × 2 payload × **2구현 = 12셀**이 run마다 실행되고 metric 9종이 채워진다. framework 6셀은 미구현이므로 18셀을 채우지 못했다 |
| G2 header 검증 | **통과.** request 계열이 reply의 29바이트 header를 검증한다. 48셀 전체 검증 실패 **0** |
| G3 send 무결성 | **통과.** `send-saturation` 처리량은 server 수신 수(`server_received_at_close`)다. 재사용한 C 행은 FB-014대로 미달 |
| G4 공개 API만 사용 | **통과.** 비공개 경로·reflection·두 번째 poller·측정 구간 내 재시도 없음 |
| G5 재현성 | **통과** (`cpp` 12행 전부). 미달은 재사용한 `zlink-c` 3행 |
| G6 포화 표시 | **통과.** 선언 계측기·상한·판독값을 셀마다 기록. 포화 셀 1개(§5) |
| G7 격리 | **통과.** 빌드 전부 구간 밖, `flock` 단일 구간, run마다 loadavg 기록(§2.6) |
| G8 깊이 보고 | **통과.** `peak_in_flight`와 Little's law 깊이를 셀마다 기록(§5) |

`cpp`는 **G1 미충족으로 "완료"가 아니다.** 12셀은 게이트를 통과했고 6셀은 실행되지 않았다.

## 11. 이 자료로 결론지을 수 없는 것

- **framework 계층 비용.** `zlink-framework-cpp`를 구현하지 않았으므로 formula 2에 대해 이
  구간은 아무 말도 하지 않는다. FB-033·FB-035가 좁힌 "깊이 약 4.5" 관측에 C++는 자료를 더하지
  않는다.
- **Core의 건전성.** §1.2대로 이 실험은 Core를 지목하지 않을 뿐이다.
- **관리형 binding 세 개의 공통 원인.** §1.2대로 증상이 셋 다 다르다.
- **`zlink-cpp`가 `zlink-c`보다 요청당 비용이 큰 이유.** §5의 후보(요청당 coroutine frame과
  공유 상태)는 프로파일로 확인하지 않았다. 0.774라는 값이 wrapper의 고정 비용인지 이 harness의
  driver 모양 때문인지 이 자료는 구분하지 못한다.
- **@4096의 binding 계층 판정.** 분모가 불안정한 한 어느 언어도 알 수 없다(FB-034).
- **언어를 가로지른 절대 처리량 우열.** 규격 §7.3대로 읽지 않는다.

## 12. 후속으로 넘긴 항목

| 항목 | 내용 | 우선순위 |
|---|---|---|
| `zlink-framework-cpp` 6셀 | 구현과 측정. Release `libzlink_framework.a`는 이미 빌드돼 있다 | Phase 6 이전 |
| `zlink-c` 기준선 안정화 | request-window @4096 G5 28.7%. formula 1의 공유 분모이므로 이것이 풀리기 전에는 어느 언어도 통과할 수 없다(FB-034). request-serial 두 행의 37.9%·12.6%도 함께 본다 | 0.18.0 후보 |
| `zlink-cpp` 요청당 비용 | 깊이 99.9에서 `zlink-c`의 77.4%. wrapper 고정 비용인지 driver 모양인지 프로파일로 분리 | 0.18.0 후보 |
| 관리형 binding 완료 전달 | FB-026(Node socket wedge)·FB-030(Java reply 유실)·FB-016(.NET 제출 비용)을 각 binding의 완료 전달 계층에서 조사. FB-037이 범위를 좁혔다 | 0.18.0 후보 우선 |
| 규격 §5.1 표의 갱신 | 규격 §5.1의 표는 `dotnet·java·kotlin·cpp`가 "프로세스 사용 core 수"를 쓴다고 적는데, Java·Kotlin은 FB-032로 `jvm_thread_cores`, C++는 FB-037로 `submit_thread_cores`를 선언한다. **표가 실제 선언과 어긋난다.** 이 job은 규격을 바꾸지 않았고 감독관 판단에 올린다 | 감독관 |

## 부록 — 산출물 위치

| 산출물 | 경로 |
|---|---|
| 셀 원본, server 로그, run 로그 | `framework/languages/cpp/bench/with-grpc/log/20260907_052304/` |
| 집계기 출력 (ROUTER 3회 + C 기준선) | 같은 경로의 `aggregate.md`, `aggregate.json` |
| 집계기 출력 (DEALER 1회) | 같은 경로의 `aggregate-dealer.md`, `aggregate-dealer.json` |
| 구간 타임라인과 게이트 값 | 같은 경로의 `timeline.txt`, `load-gates.txt` |
| 구현 | `framework/languages/cpp/bench/with-grpc/` |
| 재사용한 C 기준선 | `bindings/c/bench/with_grpc/log/20260907_030655/c-router-*` |
