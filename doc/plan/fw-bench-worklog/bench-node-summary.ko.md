# Node with-grpc bench 요약 (Phase 2)

이 문서는 `node` 언어의 with-grpc bench 측정 결과를 하나로 모은 기록이다. Phase 6 보고서가
이 문서만 읽고도 `node` 행을 서술할 수 있도록 조건, 수치, 판정, 판정할 수 없는 항목을 함께
남긴다.

계획: [`../framework-bench-with-grpc-5lang-plan.ko.md`](../framework-bench-with-grpc-5lang-plan.ko.md)
결정: [`decisions.ko.md`](./decisions.ko.md)
규격: [`../../../framework/doc/framework/common/bench/with-grpc-local.ko.md`](../../../framework/doc/framework/common/bench/with-grpc-local.ko.md)

## 1. 결론 먼저

### 1.1 표준 workload에서 Node의 ZLink client 경로가 정지한다. request-window 셀은 처리량이 아니라 그 정지를 측정한 것이다

`request_window` 기본값 100에서 `zlink-node`의 request-window 셀 여덟 개 중 **일곱 개가
정지했다**(ROUTER 3회 × payload 2종 여섯 개 전부, DEALER 1회 중 @1024). 정지한 셀에는 완료되지
않고 request timeout 30초에 그대로 도달한 요청이 남고, error 수가 25에서 100 사이로 기록된다.

**이 셀들의 처리량 값을 Node의 request-window 성능으로 인용하면 안 된다.** 52/s나 30/s 같은
값은 이 경로가 지탱하는 속도가 아니라 완료를 멈춘 socket을 active 구간 길이로 나눈 산술
결과다. 그래서 이 문서는 정지한 셀에 처리량 값을 싣지 않고, §6에 정지 자체의 관측으로 싣는다.
같은 이유로 이 값을 `grpc-node`의 15.4 KOPS와 나란히 놓지 않는다. 두 값은 같은 종류의 측정이
아니다.

정지는 DEALER 구성에서도 발생했다. 소켓 종류에 딸린 성질이 아니며, 이는 bench 밖 최소 재현이
보여준 성질과 일치한다(FB-026).

### 1.2 정지하지 않을 때에도 이 경로는 transport보다 JS thread를 먼저 포화시킨다. 그래서 0.80 판정을 게재하지 않는다

정지가 없는 셀에서도 `zlink-node`는 선언 계측기인 event loop 사용률이 상한 `1.0`에 닿는다.
정지가 발생하지 않는 `request-serial` 네 셀은 `0.983`~`0.985`이고 G5도 1.0%~3.2%로 통과한다.
같은 run의 `grpc-node`는 `0.646`~`0.868`로 포화에 이르지 않는다.

곧 이 client에서 상한을 정한 것은 transport가 아니라 user 코드가 도는 JS thread다. 규격 §5.1은
포화 셀을 처리량 우열 판정에서 제외하므로, **Node는 0.80 판정을 게재하지 않는다. 이것은 자료의
구멍이 아니라 위 결론이 낳은 결과다**(FB-027).

### 1.3 framework 행은 규격이 정한 payload를 아예 전달할 수 없다. 공개 protobuf codec에 bytes 형이 없다

`zlink-framework-node`의 여섯 셀은 측정하지 않고 `unsupported`로 남긴다. 호스트는 동작하지만
공개 codec `packages/framework-codec-protobuf`에 bytes 형이 없어, 규격 §2가 payload 크기로
고정한 protobuf `bytes body`를 그대로 실을 수 없다(FB-028). 실측하면 1024 bytes가 20,412
bytes로 인코딩되고 디코딩하면 bytes가 아닌 object로 돌아온다. 자세한 사유는 §7이다.

### 1.4 게재할 수 있는 값

`grpc-node`는 12셀 전부가 정상이고 G5 0.3%~4.7%로 통과한다. `zlink-node`도 `request-serial`
네 셀과 `send-saturation` 네 셀은 정지 없이 통과한다. 이 값들은 그대로 게재한다.

## 2. 측정 대상과 조건

### 2.1 비교 대상

| 구현 이름 | 내용 |
|---|---|
| `grpc-node` | `@grpc/grpc-js` unary RPC. proto는 `Echo`와 `Command` 둘뿐이다(FB-002) |
| `zlink-node` | framework를 거치지 않는 raw binding. ROUTER↔ROUTER |
| `zlink-framework-node` | RouteMesh channel messaging. **여섯 셀 모두 `unsupported`**(§7) |

ZLink raw 행은 규격 §1.3대로 ROUTER↔ROUTER를 사용한다. client도 ROUTER를 만들고 자기
routing id를 설정한 뒤 상대 ROUTER의 routing id를 지정해 전송한다. wire 모양은 envelope 헤더
part 하나와 protobuf로 인코딩한 `BenchPayload` part 하나로, `zlink-c`·`zlink-dotnet`과 같다
(FB-024).

### 2.2 고정 조건

| 항목 | 값 |
|---|---|
| payload 크기 | `1024`, `4096` bytes |
| `request_window` | 100 |
| send concurrency | 8 |
| warmup | 1000 (`node`) |
| active duration | 5초 |
| request timeout | 30초 |
| 반복 | ROUTER 3회, DEALER 1회 |
| transport | loopback `127.0.0.1`, 포트 대역 5081-5087 |
| 대표값 | 중앙값 |

정지를 피하려고 `request_window`를 낮추지 않았다. 표준 workload에서 정지한다는 사실 자체가
측정 결과다.

### 2.3 실행 환경과 이력

| 항목 | 값 |
|---|---|
| CPU | Intel Core Ultra 7 265K, 논리 core 20개 |
| OS | Ubuntu 24.04.4 LTS, kernel 6.6.87.2-microsoft-standard-WSL2 |
| Node | v22.23.2 |
| gRPC | `@grpc/grpc-js` 1.14.4, `@grpc/proto-loader` 0.7.15 |
| gRPC server 구성 | `@grpc/grpc-js` `Server` 기본 옵션, insecure loopback |
| ZLink binding | `@zlink-systems/zlink` 0.17.0 |
| 측정 기준 commit | `9b47698915` |
| 측정 구간 | 2026-09-07T02:04:43+09:00 ~ 02:12:23+09:00 |

### 2.4 측정 격리

측정 구간 전체를 `flock --exclusive /tmp/zlink-perf.lock`으로 잠갔다. Node harness는 빌드
단계가 없으므로 컴파일이 측정 구간에 들어가지 않는다. 각 run을 시작하기 전 1분 load average를
확인했고 관측값은 `0.51`, `1.72`, `1.52`, `1.40`으로 모두 기준 2.0 미만이다. 4회 run이 모두
종료 코드 0으로 끝났다. runner는 시작 전에 포트 대역 5081-5087이 비어 있는지 확인하고, 사용
중이면 다른 포트로 옮기지 않고 중단한다(규격 §9).

같은 구간에서 `grpc-node`가 네 run 모두 @1024에서 15,389~15,483/s, error 0으로 수렴한다.
harness와 머신이 정상이었다는 근거이며, `zlink-node`의 흔들림이 환경 때문이 아니라는 대조군이다.

### 2.5 client 포화 계측기

FB-023에 따라 Node harness는 계측기와 상한을 함께 선언한다.

| 항목 | 값 |
|---|---|
| 선언 계측기 | `perf_hooks`의 `performance.eventLoopUtilization()` |
| 선언 상한 | `1.0` |
| 포화 기준 | 상한의 0.95배 이상 |

프로세스 사용 core 수는 관찰값으로 함께 기록하지만 포화를 판정하지 않는다. 이 client에서
프로세스 CPU는 binding의 native I/O thread를 함께 세어 `0.76`~`1.72` 코어로 읽히는데, 그
thread들은 user 코드를 실행하지 않기 때문이다.

## 3. 측정 표

ROUTER 구성 3회 run의 중앙값이다. 처리량 단위는 request 계열이 `KOPS`,
`send-saturation`이 `KMSG/s`다. CPU는 논리 core 20개 기준 백분율, memory는 RSS(MB)다.
`*`는 규격 §5.1의 포화 표시다.

### 3.1 payload 1024

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | client CPU% | client MB | server CPU% | server MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | `grpc-node` | 4.19 | 0.238 | 0.405 | 0.581 | 4.2 | 222.7 | 1.9 | 81.2 | — |
| request-serial | `zlink-node` | 9.99 | 0.100 | 0.160 | 0.267 | 6.5* | 230.1 | 5.9 | 66.4 | — |
| request-window | `grpc-node` | 15.43 | 6.483 | 8.537 | 13.264 | 5.6 | 241.6 | 2.5 | 135.4 | — |
| request-window | `zlink-node` | **처리량 없음 — 정지(§6)** | — | — | — | 5.0* | 243.3 | 0.1 | 66.8 | — |
| send-saturation | `grpc-node` | 11.37 | 0.314 | 0.468 | 0.598 | 4.4 | 241.9 | 2.3 | 118.4 | 219 |
| send-saturation | `zlink-node` | 291.43 | 71.199 | 94.267 | 98.894 | 6.0 | 248.3 | 6.7 | 114.6 | 609 |

### 3.2 payload 4096

| 패턴 | 구현 | 처리량 | 평균 ms | p95 ms | p99 ms | client CPU% | client MB | server CPU% | server MB | drain ms |
|---|---|---|---|---|---|---|---|---|---|---|
| request-serial | `grpc-node` | 4.52 | 0.221 | 0.326 | 0.505 | 3.8 | 260.6 | 2.1 | 120.1 | — |
| request-serial | `zlink-node` | 10.20 | 0.098 | 0.126 | 0.222 | 6.4* | 262.6 | 5.9 | 112.9 | — |
| request-window | `grpc-node` | 11.52 | 8.684 | 11.721 | 17.741 | 5.3 | 268.4 | 3.0 | 122.1 | — |
| request-window | `zlink-node` | **처리량 없음 — 정지(§6)** | — | — | — | 5.0* | 271.6 | 0.1 | 112.5 | — |
| send-saturation | `grpc-node` | 10.76 | 0.333 | 0.492 | 0.699 | 4.5 | 270.8 | 2.3 | 123.7 | 210 |
| send-saturation | `zlink-node` | 168.08 | 0.519 | 3.406 | 5.490 | 8.6* | 276.1 | 7.4 | 158.0 | 293 |

`zlink-node`의 request-window 두 칸은 값을 비운 것이 아니라 **처리량이 성립하지 않는 셀**이다.
집계기 원본에는 중앙값이 남아 있지만(0.38·0.37 KOPS) 그 값은 정지의 산술이므로 이 표에 싣지
않는다. client CPU와 memory 열은 정지 중에도 의미가 있으므로 남긴다.

### 3.3 포화와 깊이

| 패턴 | payload | 구현 | event loop 사용률 | 포화 | peak_in_flight | abandoned |
|---|---|---|---|---|---|---|
| request-serial | 1024 | `grpc-node` | 0.660 | 아니오 | 1 | 0 |
| request-serial | 1024 | `zlink-node` | 0.983 | **예** | 1 | 0 |
| request-serial | 4096 | `grpc-node` | 0.646 | 아니오 | 1 | 0 |
| request-serial | 4096 | `zlink-node` | 0.985 | **예** | 1 | 0 |
| request-window | 1024 | `grpc-node` | 0.868 | 아니오 | 100 | 0 |
| request-window | 1024 | `zlink-node` | 1.000 | **예** | 100 | 0 |
| request-window | 4096 | `grpc-node` | 0.811 | 아니오 | 100 | 0 |
| request-window | 4096 | `zlink-node` | 1.000 | **예** | 100 | 0 |
| send-saturation | 1024 | `grpc-node` | 0.701 | 아니오 | 8 | 0 |
| send-saturation | 1024 | `zlink-node` | 0.808 | 아니오 | 8 | 0 |
| send-saturation | 4096 | `grpc-node` | 0.708 | 아니오 | 8 | 0 |
| send-saturation | 4096 | `zlink-node` | 1.000 | **예** | 8 | 0 |

`peak_in_flight`가 설정한 window 100에 도달하고 abandoned가 0이므로, harness가 window를 채우지
못하는 FB-010 유형이 아니다. 정지한 셀에서 집계기가 내는 실제 깊이 582(설정값의 5.8배)는
정지한 요청이 평균 지연을 끌어올린 결과이며, 깊이가 아니라 정지를 다시 가리키는 값이다.

## 4. 0.80 판정

| 판정식 | payload | 값 | 상태 | 이유 |
|---|---|---|---|---|
| `zlink-node / zlink-c` | 1024 | — | `unsupported` | 분자 셀이 처리량을 내지 않는다(정지). 집계기 기준으로도 G5 480.0% 미달. 분모 `zlink-c`는 이번 구간 미측정 |
| `zlink-node / zlink-c` | 4096 | — | `unsupported` | 분자 셀이 처리량을 내지 않는다(정지). G5 510.5% 미달. 분모 미측정 |
| `zlink-framework-node / zlink-node` | 1024 | — | `unsupported` | 분자 미측정(§7, FB-028). 분모가 위와 같은 사유로 성립하지 않는다 |
| `zlink-framework-node / zlink-node` | 4096 | — | `unsupported` | 분자 미측정(§7, FB-028). 분모가 위와 같은 사유로 성립하지 않는다 |

**Node는 0.80 판정을 게재하지 않는다.** FB-027이 정한 대로 이것은 §1.2의 결론이 낳은 결과이며
자료의 결손이 아니다. 판정 패턴인 request-window의 `zlink-node` 셀은 포화 셀이면서 정지 셀이다.

`zlink-c` 기준값을 이번 구간에 다시 측정하지 않았다. 이 결정이 게재값을 바꾸지 않는다는 것은
표에서 직접 확인된다. 분자가 두 payload 모두에서 성립하지 않으므로 분모를 측정했더라도 두
판정은 같은 `unsupported`로 남는다.

## 5. G5 재현성

| 패턴 | payload | 구현 | 스프레드 | G5 |
|---|---|---|---|---|
| request-serial | 1024 | `grpc-node` | 1.9% | 통과 |
| request-serial | 1024 | `zlink-node` | 3.2% | 통과 |
| request-serial | 4096 | `grpc-node` | 0.5% | 통과 |
| request-serial | 4096 | `zlink-node` | 1.0% | 통과 |
| request-window | 1024 | `grpc-node` | 0.3% | 통과 |
| request-window | 4096 | `grpc-node` | 4.7% | 통과 |
| send-saturation | 1024 | `grpc-node` | 0.3% | 통과 |
| send-saturation | 1024 | `zlink-node` | 2.0% | 통과 |
| send-saturation | 4096 | `grpc-node` | 0.5% | 통과 |
| send-saturation | 4096 | `zlink-node` | 1.9% | 통과 |

정상 측정된 10셀이 모두 통과한다. 오염된 셀은 없고 모든 셀이 drain 상한 안에서 비워졌다.

`zlink-node`의 request-window 두 셀은 이 표에 넣지 않는다. 집계기가 내는 스프레드는
480.0%·510.5%인데, 이는 처리량의 재현성이 아니라 **정지가 얼마나 들쭉날쭉하게 발생하는지**를
나타내는 값이다. 처리량이 성립하지 않는 셀에 재현성 판정을 적용하면 값의 뜻이 바뀐다.

## 6. FB-026 정지의 실측

표준 workload(`request_window` 100)에서 `zlink-node` request-window 셀의 run별 관측이다.
`정지`로 표시한 셀은 완료되지 않고 request timeout 30초에 도달한 요청이 있는 셀이다.

| run | payload | 완료 | error | 평균 ms | p95 ms | p99 ms | 판정 |
|---|---|---|---|---|---|---|---|
| router-1 | 1024 | 10886 | 36 | 275.399 | 1.642 | 5.164 | 정지 |
| router-1 | 4096 | 1339 | 69 | 2131.655 | 30000.726 | 30001.394 | 정지 |
| router-2 | 1024 | 1877 | 59 | 1550.342 | 30000.627 | 30001.078 | 정지 |
| router-2 | 4096 | 11160 | 100 | 267.500 | 2.137 | 8.134 | 정지 |
| router-3 | 1024 | 260 | 98 | 8380.685 | 30001.510 | 30001.629 | 정지 |
| router-3 | 4096 | 1828 | 55 | 1594.068 | 30000.451 | 30001.094 | 정지 |
| dealer-1 | 1024 | 150 | 25 | 17143.678 | 30001.265 | 30001.304 | 정지 |
| **dealer-1** | **4096** | **424853** | **0** | **0.860** | **1.472** | **1.888** | **정상** |

정지 셀을 알아보는 표시는 두 가지 형태로 나타나며 둘 다 같은 원인이다.

- **p95·p99가 30,000 ms에 놓인다.** 정지한 요청이 표본의 5%를 넘을 때의 모양이다. 이 값은
  느린 요청이 아니라 request timeout 값 자체이므로, 완료되지 않고 timeout으로 끝난 요청이다.
- **평균이 p95보다 훨씬 크다.** 정지한 요청이 표본의 5%에 못 미칠 때의 모양이다. router-1
  @1024는 평균 275.4 ms인데 p95는 1.642 ms다. 대부분은 1 ms대에 끝나고 소수가 수십 초 멈춘다.

여덟 셀 중 **일곱이 정지했고 하나만 정상이다.** 정상인 dealer-1 @4096은 error 0에 p99
1.888 ms로 **424,853건을 완료했다.** 정지하지 않을 때 이 경로가 무엇을 하는지 보여주는 유일한
셀이며, 정지 셀의 산술값을 이 경로의 성능으로 읽으면 안 되는 이유이기도 하다.

**정지는 DEALER 구성에서도 발생했다**(dealer-1 @1024). ROUTER 전용 성질이 아니고, 소켓 종류를
바꾸어 피할 수 있는 것도 아니다. 이는 bench 밖 최소 재현이 보여준 성질과 일치한다.

같은 구간의 `grpc-node`는 네 run 모두 @1024에서 15,389~15,483/s, error 0으로 수렴하고 지연
분포도 정상이다. 정지가 머신이나 harness의 문제가 아니라는 대조군이다.

정지의 위치는 bench 밖 최소 재현으로 분리했다. 재현은
`framework/languages/node/bench/with-grpc/repro/`에 있고 `@zlink-systems/zlink`만 사용한다.
blocking recv server와 batch 제출 형태에서도 재현되며, 정지한 socket이 2.8초 동안 멈춰 있는
동안 같은 server에 새로 연결한 client ROUTER는 100건을 1.5 ms에 완료한다. 곧 정지는 client
socket 안에 있고 server는 정상이다. 동시 요청 100건을 한 번에 제출하면 1.1 ms에 모두
완료되므로 깊이 자체가 원인도 아니다. 이 캠페인은 원인을 고치지 않는다.

## 7. `zlink-framework-node`가 `unsupported`인 이유 (FB-028)

여섯 셀 모두 측정하지 않고 `unsupported`로 남긴다. 사유는 공개 API의 표현 한계다.

framework 호스트 자체는 동작한다. `@zlink-systems/nestjs`의 `ZLinkModule.forRootFactory`와
`zlinkFramework()`로 RouteMesh ROUTER↔ROUTER server가 기동하고, channel `bench`에 request
처리기와 send 처리기가 등록되며, client가 `ZLINK_ROUTE_CLIENT`를 해석한다.

막히는 곳은 codec이다. `packages/framework-codec-protobuf`는 `boolean`, `number`, `string`,
`object`만 다루는 dynamic value wire이고 **bytes 형이 없다.** `Buffer`는 `object`이므로 바이트
하나가 각각 키를 가진 항목으로 인코딩된다. 실측하면 1024 bytes의 `bytes body`가 **20,412
bytes(19.9배)** 로 인코딩되고, 디코딩하면 bytes가 아니라 일반 object로 돌아온다. 규격 §2는
payload 크기를 protobuf `bytes body`의 크기로 고정하므로, 이 codec으로는 규격이 정한 payload를
전달할 수 없다.

harness는 이 검사를 측정 구간 밖 기동 시점에 한 번 수행하고, 여섯 셀을 위 사유와 함께
실패로 기록한다. `packages/framework`의 `src/internal.ts`는 package.json이 내보내지 않는
내부 표면이므로 다른 codec 경로를 찾아 들어가지 않았다. 그렇게 하면 G4(공개 API만 사용)를
어긴다.

## 8. 이 자료로 결론지을 수 없는 것

- **`zlink-node`의 request-window 처리량을 확정할 수 없다.** 여덟 셀 중 일곱이 정지했다.
  정상인 한 셀(dealer-1 @4096, 424,853건 완료, p99 1.888 ms)은 표본이 하나뿐이라 G5를 적용할
  수 없다.
- **framework 계층 비용을 재지 못했다.** 판정식 2의 분자가 존재하지 않는다.
- **`grpc-node`와 `zlink-node`의 절대 처리량 우열을 판정에 쓸 수 없다.** `zlink-node`의
  request 계열 네 셀이 모두 포화 셀이다.
- **정지의 근본 원인을 특정하지 못했다.** 확인한 것은 client socket 안에서 발생한다는 것,
  깊이 자체가 원인이 아니라는 것, 지속적인 부하에서만 나타난다는 것, 간헐적이라는 것,
  ROUTER와 DEALER 모두에서 발생한다는 것이다. binding과 Core 중 어디인지는 계측하지 않았다.

## 9. 후속으로 넘긴 항목

| 항목 | 내용 |
|---|---|
| FB-026 | `zlink-node` raw request의 정지. 0.18.0 후보 우선순위 0. 재현은 `bench/with-grpc/repro/` |
| FB-028 | `packages/framework-codec-protobuf`에 bytes 형이 없어 `bytes` 필드를 가진 DTO를 전달할 수 없다. Node framework 행이 이 bench에 참여하려면 필요하다 |
| Node ZLink client의 JS thread 포화 | request 계열 네 셀이 event loop 사용률 0.98 이상이다. transport 비교를 하려면 client 쪽 비용을 먼저 낮춰야 한다 |
| `zlink-c` 기준값 | 이번 구간에 재측정하지 않았다. Phase 6 통합 측정에서 다섯 언어와 같은 조건으로 확보한다 |
