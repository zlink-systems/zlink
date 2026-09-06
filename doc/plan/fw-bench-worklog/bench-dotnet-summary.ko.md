# .NET with-grpc bench 요약 (Phase 0)

이 문서는 `.NET` 언어의 with-grpc bench 측정 결과를 하나로 모은 기록이다. Phase 6 보고서가
이 문서만 읽고도 `.NET` 행을 서술할 수 있도록 조건, 수치, 판정, 판정할 수 없는 항목을 함께
남긴다.

계획: [`../framework-bench-with-grpc-5lang-plan.ko.md`](../framework-bench-with-grpc-5lang-plan.ko.md)
결정: [`decisions.ko.md`](./decisions.ko.md)
규격: [`../../../framework/doc/framework/common/bench/with-grpc-local.ko.md`](../../../framework/doc/framework/common/bench/with-grpc-local.ko.md)

## 1. 결론 먼저

- **`.NET`은 규격 §7.2의 0.80 기준을 통과하지 못했고, 완전한 판정도 불가능하다.** 네 판정값
  중 게시 조건을 만족하는 것은 하나뿐이며 그 값은 기준에 미달한다. 나머지 셋은 재현성 조건을
  만족하지 못해 `unsupported`다.
- 게시되는 값은 `zlink-dotnet / zlink-c`의 payload `1024`, **0.084**이다. 기준 0.80에 대해
  **미달**이다.
- 미달 원인은 확인됐다. `.NET` raw request 경로는 요청 하나마다 호출 thread에서 약
  32.2 마이크로초의 CPU를 사용하고 약 1.16 core 안에서 동작한다. 같은 조건 C는 약 4.0
  마이크로초다. 이 캠페인은 원인을 기록만 하고 고치지 않는다.
- 측정 과정에서 harness 결함 두 건을 고쳤고, 그 수정이 이전 값 하나를 뒤집었다. framework의
  send 우위는 표본화 시점 오류였으며 수정 뒤 사라진다(§5).

## 2. 측정 대상과 조건

### 2.1 비교 대상

| 구현 이름 | 내용 |
|---|---|
| `grpc-dotnet` | `.NET` gRPC unary RPC |
| `zlink-dotnet` | framework를 거치지 않는 raw binding. ROUTER↔ROUTER |
| `zlink-framework-dotnet` | framework channel messaging. RouteMesh ROUTER↔ROUTER |
| `zlink-c` | C 기준 bench의 raw binding. 판정식의 기준값 |

ZLink 두 행은 규격 §1.3대로 모두 ROUTER↔ROUTER를 사용한다. client도 ROUTER를 생성하고 자기
routing id를 설정한 뒤 상대 ROUTER의 routing id를 지정해 전송한다. 이전에 사용하던
DEALER→ROUTER 구성은 `--raw-socket dealer` 옵션으로 남아 있으며, 소켓 전환의 영향을 비교할
때만 사용한다.

### 2.2 고정 조건

| 항목 | 값 |
|---|---|
| payload 크기 | `1024`, `4096` bytes |
| `request_window` | 100 |
| send concurrency | 8 |
| warmup | 1000 (`.NET`) |
| active duration | 5초 (`.NET`), 3초 (C 기준 bench) |
| 반복 | ROUTER 3회, DEALER 1회 |
| build | Release |
| transport | loopback `127.0.0.1` |
| 대표값 | 중앙값 |

### 2.3 실행 환경과 이력

| 항목 | 값 |
|---|---|
| CPU | Intel Core Ultra 7 265K, 논리 core 20개 |
| OS | Ubuntu 24.04.4 LTS, kernel 6.6.87.2-microsoft-standard-WSL2 |
| `.NET` SDK | 8.0.130 |
| gRPC (`.NET`) | Grpc.AspNetCore 2.62.0, Grpc.Net.Client 2.62.0 |
| protobuf (`.NET`) | Google.Protobuf 3.29.1 |
| gRPC (C) | grpc++ 1.51.1, protobuf 3.21.12 |
| ZLink Core | 0.17.0 |
| 측정 기준 commit | `d96e4b7031` |
| 측정 구간 | 2026-09-07T00:33:48+09:00 ~ 00:52:17+09:00 |

### 2.4 측정 격리

측정 구간 전체를 `flock --exclusive /tmp/zlink-perf.lock`으로 잠갔다. 빌드는 측정 구간 밖에서
미리 끝내고 실행 script는 `SKIP_BUILD=1`로 동작했다. 각 run을 시작하기 전 1분 load average가
2.0 미만인지 확인했고 관측값은 1.63, 1.79, 1.76, 1.95, 1.95, 1.76, 1.93, 1.75이다. 8회 run이
모두 종료 코드 0으로 끝났다.

`dotnet build`는 MSBuild node 재사용 daemon을 남기고, 이 daemon이 lock file descriptor를
상속해 실행이 끝난 뒤에도 잠금을 유지한다. 그래서 측정 script는 `MSBUILDDISABLENODEREUSE=1`을
설정한다. 이 설정이 없으면 측정 직렬화가 조용히 무력화된다.

## 3. 채택한 측정 표

아래 값은 ROUTER 구성 3회 run의 중앙값이다. 처리량 단위는 request 계열이 `KOPS`,
`send-saturation`이 `KMSG/s`다. CPU는 논리 core 20개 기준 백분율, memory는 RSS(MB)다.
`send-saturation` 행에는 규격 §3의 settle 계약으로 관측한 drain 시간을 함께 적는다.

### 3.1 payload 1024

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | client CPU% | client MB | server CPU% | server MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | `grpc-dotnet` | 6.758 | 0.146 | 0.272 | 0.323 | 6.5 | 115.8 | 5.5 | 176.1 | — |
| request-serial | `zlink-dotnet` | 7.595 | 0.130 | 0.156 | 0.188 | 3.6 | 120.6 | 1.7 | 120.8 | — |
| request-serial | `zlink-framework-dotnet` | 1.995 | 0.500 | 0.656 | 0.834 | 3.4 | 142.2 | 8.2 | 437.2 | — |
| request-window | `grpc-dotnet` | 198.787 | 0.471 | 1.442 | 1.926 | 36.9 | 158.4 | 28.8 | 482.4 | — |
| request-window | `zlink-dotnet` | 36.034 | 0.221 | 0.356 | 0.420 | 5.8 | 166.0 | 2.2 | 341.1 | — |
| request-window | `zlink-framework-dotnet` | 3.663 | 28.045 | 104.709 | 174.524 | 3.7 | 195.4 | 5.2 | 486.1 | — |
| send-saturation | `grpc-dotnet` | 45.335 | 0.090 | 0.112 | 0.147 | 9.5 | 198.2 | 7.6 | 488.5 | 240 |
| send-saturation | `zlink-dotnet` | 411.875 | 0.377 | 1.141 | 9.716 | 16.0 | 207.7 | 6.8 | 500.8 | 360 |
| send-saturation | `zlink-framework-dotnet` | 46.629 | 1770.695 | 3359.400 | 3513.137 | 31.9 | 1284.3 | 50.9 | 2163.5 | 16674 |

### 3.2 payload 4096

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | client CPU% | client MB | server CPU% | server MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | `grpc-dotnet` | 6.857 | 0.143 | 0.190 | 0.234 | 5.7 | 1289.4 | 5.4 | 504.0 | — |
| request-serial | `zlink-dotnet` | 7.534 | 0.130 | 0.158 | 0.235 | 4.0 | 1289.4 | 1.6 | 548.3 | — |
| request-serial | `zlink-framework-dotnet` | 2.207 | 0.451 | 0.554 | 0.646 | 3.6 | 1339.8 | 7.6 | 2147.3 | — |
| request-window | `grpc-dotnet` | 127.193 | 0.753 | 1.732 | 2.097 | 33.9 | 1340.6 | 29.2 | 509.6 | — |
| request-window | `zlink-dotnet` | 34.390 | 0.225 | 0.362 | 0.439 | 5.7 | 1343.7 | 2.1 | 487.3 | — |
| request-window | `zlink-framework-dotnet` | 2.458 | 40.134 | 105.397 | 129.874 | 2.8 | 1402.2 | 3.9 | 1965.5 | — |
| send-saturation | `grpc-dotnet` | 41.515 | 0.102 | 0.129 | 0.171 | 9.7 | 1405.2 | 8.8 | 511.6 | 216 |
| send-saturation | `zlink-dotnet` | 383.077 | 0.615 | 4.302 | 8.406 | 19.0 | 1407.8 | 8.2 | 502.3 | 363 |
| send-saturation | `zlink-framework-dotnet` | 43.863 | 1475.071 | 3071.936 | 3190.037 | 30.8 | 3963.1 | 51.9 | 4051.0 | 13286 |

### 3.3 판정에 사용한 C 기준값

| 패턴 | payload | `zlink-c` 처리량 | 재현성 |
|---|---|---|---|
| request-window | 1024 | 430.617 KOPS | 통과(5.6%) |
| request-window | 4096 | 310.523 KOPS | 미달(25.7%) |

### 3.4 실제 유지된 동시 요청 수

`request_window`는 100이지만 각 구현이 실제로 유지한 미완료 요청 수는 다르다. 아래 값은
처리량과 평균 지연으로 계산한 값이다(Little's law).

| 구현 | 1024 | 4096 |
|---|---|---|
| `grpc-c` | 98.9 | 98.8 |
| `zlink-c` | 83.5 | 87.6 |
| `grpc-dotnet` | 93.6 | 95.8 |
| **`zlink-dotnet`** | **8.0** | **7.7** |
| `zlink-framework-dotnet` | 102.7 | 98.7 |

`zlink-dotnet`만 100에서 크게 벗어난다. 이 값의 의미는 §6에서 설명한다.

## 4. 0.80 판정

규격 §7.2의 판정식을 판정 패턴 `request-window`에 대해 payload 크기별로 계산한다. 결정
FB-011에 따라 분자와 분모가 모두 재현성 조건을 만족한 경우에만 값을 게시한다.

| 판정식 | payload | 값 | 상태 | 이유 |
|---|---|---|---|---|
| `zlink-dotnet / zlink-c` | 1024 | **0.084** | **게시, 기준 미달** | 양쪽 행이 재현성 조건을 만족한다 |
| `zlink-dotnet / zlink-c` | 4096 | (0.111) | `unsupported` | 분모 `zlink-c`의 재현성이 25.7%로 조건에 미달한다 |
| `zlink-framework-dotnet / zlink-dotnet` | 1024 | (0.102) | `unsupported` | 분자 framework 행의 재현성이 29.6%로 조건에 미달한다 |
| `zlink-framework-dotnet / zlink-dotnet` | 4096 | (0.071) | `unsupported` | 분자 framework 행의 재현성이 11.5%로 조건에 미달한다 |

괄호 안의 값은 참고용이며 판정에 사용하지 않는다.

규격 §7.2와 결정 FB-005는 `1024`와 `4096` 두 크기에서 모두 기준을 만족해야 그 언어가
통과라고 정한다. `.NET`은 게시 가능한 한 크기에서 기준에 미달했고 다른 크기는 판정할 수
없다. 따라서 **`.NET`은 통과가 아니며, 완전한 판정도 성립하지 않는다.**

## 5. G5 재현성

같은 조건 3회 run의 중앙값 대비 각 run이 ±10% 안에 들어오는지 본다.

### 5.1 조건을 만족한 행

| 행 | 1024 | 4096 |
|---|---|---|
| `grpc-dotnet` request-serial | 1.4% | 0.5% |
| `grpc-dotnet` request-window | 1.7% | 3.6% |
| `grpc-dotnet` send-saturation | 2.3% | 1.2% |
| `zlink-dotnet` request-serial | 7.8% | 2.0% |
| `zlink-dotnet` request-window | 1.0% | 2.0% |
| `zlink-dotnet` send-saturation | 5.0% | 2.9% |
| `zlink-framework-dotnet` request-serial | 2.6% | 2.0% |
| `zlink-framework-dotnet` send-saturation | 2.0% | 2.4% |
| `grpc-c` request-window | 3.1% | 2.0% |
| `grpc-c` send-saturation | 2.8% | 2.9% |
| `zlink-c` request-window | 5.6% | — |
| `zlink-c` send-saturation | — | 1.6% |

### 5.2 조건에 미달한 행

| 행 | payload | 3회 run 값 | 편차 |
|---|---|---|---|
| `zlink-framework-dotnet` request-window | 1024 | 3.1 / 4.7 / 3.7 | 29.6% |
| `zlink-framework-dotnet` request-window | 4096 | 2.5 / 2.3 / 2.7 | 11.5% |
| `zlink-c` request-window | 4096 | 387.9 / 310.5 / 230.8 | 25.7% |
| `zlink-c` request-serial | 1024 | 7.6 / 8.1 / 10.0 | 23.5% |
| `zlink-c` request-serial | 4096 | 9.4 / 8.3 / 10.8 | 14.6% |
| `zlink-c` send-saturation | 1024 | 704.8 / 680.3 / 852.2 | 20.9% |
| `grpc-c` request-serial | 1024 | 14.6 / 5.3 / 4.0 | 173.8% |
| `grpc-c` request-serial | 4096 | 15.0 / 5.1 / 5.7 | 165.0% |

`grpc-c` request-serial의 편차는 이전 측정에서 7.7%였다가 이번에 크게 벌어졌다. 원인을
확인하지 않았고 판정 경로에 있지 않다.

## 6. 두 가지 수정과 뒤집힌 결론

### 6.1 send 처리량의 표본화 시점

이전 구현은 drain이 끝난 뒤 server 통계를 읽었다. 그래서 active 구간이 끝난 뒤 수 초 동안
도착한 message까지 세고 active 구간 길이로 나눴다. 규격 §5는 server가 active phase에서 받은
message 수라고 정하므로 이 구현이 계약을 어긴 것이다. 표본화 시점을 active 구간 경계로
옮겼다.

`zlink-framework-dotnet` send-saturation, payload 1024, 한 run의 실측:

```text
received at active-window close   233,143
received after drain              712,881
drain time                         16,674 ms
```

| 구현 | payload | 수정 전 | 수정 후 | 변화 |
|---|---|---|---|---|
| `grpc-dotnet` | 1024 | 45.620 | 45.335 | -0.6% |
| `zlink-dotnet` | 1024 | 411.127 | 411.875 | +0.2% |
| `zlink-framework-dotnet` | 1024 | 125.881 | **46.629** | **-63%** |
| `grpc-dotnet` | 4096 | 40.473 | 41.515 | +2.6% |
| `zlink-dotnet` | 4096 | 395.952 | 383.077 | -3.3% |
| `zlink-framework-dotnet` | 4096 | 53.002 | **43.863** | **-17%** |

**뒤집힌 결론을 그대로 적는다. framework send가 gRPC보다 약 2.8배 빠르다는 이전 값은 표본화
시점 오류였고, 수정 뒤 그 우위는 사라진다.** payload 1024에서 framework는 46.629, gRPC는
45.335로 사실상 같다(비 1.03). framework가 gRPC보다 빠르다고 서술할 근거가 없다.

같은 수정에서 **raw 행의 우위는 유지된다.** `zlink-dotnet`은 411.875로 gRPC의 약 9.1배이고,
payload 4096에서도 383.077 대 41.515로 약 9.2배다. raw 행의 drain은 0.36초라서 표본화 시점을
어디로 잡아도 값이 같다.

이 수정은 재현성도 개선했다. drain 시간의 흔들림이 처리량에 섞이지 않게 되면서
`zlink-framework-dotnet` send 행의 편차가 22.8%에서 2.0%로, `zlink-dotnet` send 행이
13.3%에서 5.0%로 줄었다.

### 6.2 소켓 통일이 framework 비율에 준 영향

raw 행을 DEALER→ROUTER에서 ROUTER↔ROUTER로 바꾼 이유는 framework 행이 이미 ROUTER↔ROUTER라서
두 행의 소켓 패턴이 다르면 `zlink-framework-dotnet / zlink-dotnet` 값에 framework 계층 비용과
소켓 패턴 비용이 함께 들어가기 때문이다.

소켓 전환이 raw 행 자체에 준 영향은 크다. 판정 패턴에서 ROUTER 구성은 DEALER 구성 대비
payload 1024에서 0.727배, 4096에서 0.836배다. 즉 ROUTER 전환으로 raw 행의 처리량이 1024에서
약 27% 낮아졌다.

그런데 **framework 비율 자체는 소켓 구성을 바꿔도 같은 크기 범위에 머문다.** 두 차례 측정에서
payload 1024의 값은 다음과 같다.

| 측정 | DEALER 구성 | ROUTER 구성 | 변화 |
|---|---|---|---|
| 1차 | 0.078 | 0.081 | +4% |
| 2차 | 0.069 | 0.102 | +48% |

두 값의 변화 폭이 서로 크게 다르다. 분자인 framework 행이 재현성 조건을 만족하지 못하므로
(편차 29.6%) 이 변화 폭 자체를 신뢰할 수 없다. **변화 폭은 확정할 수 없지만, 두 측정 모두에서
비율이 0.069에서 0.102 사이에 머문다는 사실은 확정된다.** 소켓 구성과 무관하게 framework
행은 raw 행의 10분의 1 근처이며, 역수로 보면 약 9.8배에서 14배 차이다. 따라서 **framework와
raw 사이의 약 10배 차이는 소켓 패턴 때문에 생긴 것이 아니라 실제 차이다.**

## 7. 이 자료로 결론지을 수 없는 것

- **`.NET`의 통과 여부를 완전히 판정할 수 없다.** payload 4096의 `zlink-dotnet / zlink-c`는
  분모의 재현성이 조건에 미달해 판정에서 제외된다. 두 크기 판정이 모두 필요하므로 판정 자체가
  성립하지 않는다.
- **framework 계층 비용을 수치로 확정할 수 없다.** framework request-window 행이 두 payload
  모두에서 재현성 조건에 미달한다. 0.102와 0.071은 참고값이며 보고서의 판정 항목에 넣지
  않는다.
- **`zlink-dotnet`의 미달 폭을 다른 언어와 비교할 수 없다.** 이 값은 동시 요청 8개를 유지한
  실험과 83개를 유지한 실험을 나눈 값이다. 두 실험의 동시성이 다르다는 사실 자체가 측정
  결과이지만, 다른 언어의 raw 행이 몇 개를 유지하는지 아직 측정하지 않았다. 언어 사이 비교는
  다섯 언어의 동시 요청 수를 모두 확보한 뒤에 한다.
- **`zlink-dotnet`의 동시 요청 수가 8인 원인을 특정하지 못했다.** 확인한 사실은 다음과 같다.
  client는 최대 100개를 실제로 유지할 수 있다(관측된 최대값 100/100). `.NET` binding의 제출
  경로에는 잠금, 제한된 queue, 완료 직렬화가 없다. 요청 하나당 호출 thread에서 약 32.2
  마이크로초의 CPU를 사용하고 약 1.16 core 안에서 동작한다. 왕복 지연을 요청당 제출 비용으로
  나눈 값이 관측값과 일치한다(221 / 32.2 = 6.9, 관측 8.0). 즉 동시성 상한이 아니라 제출
  처리율 상한이며, `request_window` 값은 제약 조건이 아니다. Core의 socket 단위 제출 gate가
  후보이지만 Core를 계측하지 않았으므로 확정하지 않는다.
- **`grpc-dotnet`과 `grpc-c`의 절대 처리량 차이를 gRPC 자체의 성능 차이로 읽을 수 없다.** 두
  client 모두 미완료 요청 약 100개를 유지하지만 요청당 지연이 다르다. C client는 완료 처리를
  thread 하나로 하고 완료마다 mutex를 사용하며 요청마다 heap 할당을 한다. 이 요인들을 분리
  측정하지 않았다.
- **DEALER 구성의 C 기준값을 신뢰할 수 없다.** 1회 run만 실행했고 `zlink-c` request-serial
  1024가 1.229 KOPS, request-window 1024가 126.3 KOPS로 ROUTER 구성 대비 설명되지 않는 값이
  나왔다. 소켓 전환 비교에서 C DEALER 열을 인용하지 않는다.
- **framework 연결이 saturation 뒤 끊기는 문제의 재현 조건을 확정하지 못했다.** ROUTER 3회
  run에서는 발생하지 않았고 DEALER 1회 run에서 발생했다. 간헐 결함으로만 기록한다.

## 8. 후속으로 넘긴 항목

| 항목 | 내용 |
|---|---|
| raw 제출 처리율 | `.NET` raw request 경로가 요청당 32.2 마이크로초, 약 1.16 core로 제한된다. Core의 socket 단위 제출 gate가 후보 |
| framework send 지연 | `send-saturation`에서 framework의 p95가 3359 ms, 같은 조건 raw가 1.141 ms. drain은 16.7초 |
| framework 연결 유지 | saturation 부하 뒤 RouteMesh 연결이 끊기고 재연결되지 않는 간헐 결함 |
| framework 재현성 | request-window 행이 두 payload 모두에서 재현성 조건에 미달 |
| `grpc-c` request-serial | 재현성 편차 165% 이상. 판정 경로 밖 |
