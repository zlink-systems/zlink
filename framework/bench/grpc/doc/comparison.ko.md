# gRPC 비교 보고서 — RID 직접 지정 기준선

이 문서는 [벤치 규격](../README.ko.md)의 비교 축과 새 기준선의 공개 상태를 기록한다. 이전
측정값은 framework 행이 channel 단위 node 선택을 포함하고 패턴 집합도 달랐으므로 이 기준선과
비교하지 않는다. 새 3-run 값은 감독자가 조용한 머신에서 별도로 측정한 뒤 채운다.

## 1. 비교 행

같은 언어 안에서 아래 세 행만 비교한다. 각 셀의 target server(B)는 하나다.

| 행 | 측정 경로 |
|---|---|
| `grpc-<lang>` | server 하나에 연결한 표준 gRPC unary `Echo` / `Command` |
| `zlink-<lang>` | framework를 거치지 않는 raw binding의 ROUTER↔ROUTER RID 직접 경로 |
| `zlink-framework-<lang>` | RouteMesh `requestToNode` / `sendToNode`로 server RID를 직접 지정하는 경로 |

framework 행은 더 이상 `requestToChannel` / `sendToChannel`의 channel 단위 라운드로빈을
측정하지 않는다. 이로써 gRPC의 1:1 target과 같은 대상 선택 조건을 갖고, framework/raw 비율에
node 선택 비용이 섞이지 않는다.

## 2. wire와 패턴

라우팅 API를 바꿔도 application wire는 바뀌지 않는다. 세 언어의 framework 행은 기존과 같은
고정 ZLink envelope와 `BenchPayload { bytes body = 1; }` protobuf body를 사용한다. payload의
앞 29 bytes는 같은 측정 header이고 나머지는 business payload다.

| 패턴 | in-flight 의미 | request 처리량 기준 |
|---|---|---|
| `request-serial` | 1 | source의 정상 request/reply 완료 수 |
| `request-backpressure` | application 상한 없음; 도달 깊이는 결과로 기록 | source의 정상 request/reply 완료 수 |
| `send-saturation` | logical stream 8, stream당 제출 1개 | target이 active header로 받은 수 |

`request-window`는 제외한다. 외부에서 깊이 100을 강제하면 요청당 비용보다 그 깊이에 도달했는지에
좌우되기 때문이다. 집계기는 과거 원본을 진단 목적으로 읽을 수 있지만 현재 표와 판정에는 위 세
패턴만 낸다.

## 3. 측정 계약

warmup, active duration, admission/backpressure, in-flight 계측, active 종료 뒤 drain, 오류와
abandoned 집계, 결과 JSON 필드는 기존 계약을 그대로 유지한다. `send-saturation`은 source 제출
수가 아니라 target 수신 수로 계산하며, 오류·유실·drain bound 위반 셀은 게재 조건을 통과할 수
없다. 판정 기준 패턴은 `request-backpressure`이고 두 payload(1024, 4096 B)를 각각 판정한다.

## 4. 새 기준선 상태

2026-09-12에 백프레셔 단일 카운터 변경 뒤로 기준선을 5 run씩 새로 잡았다. 집계·판정은
`tools/bench_aggregate.py`가 소유한다 — 중앙값, G5 재현성, §7.2 비율, 그리고 **그 비율을
공개해도 되는지의 판정**까지.

| 언어 | 5 run | `request-backpressure` 판정 | peak in-flight (raw / framework) | 상태 |
|---|---|---|---|---|
| Java | 완료 | 1.291 / 1.255 | **99~130 / 35~40** | **패턴 미도달 — `#300`** |
| .NET | 완료 | 1.309 / (0.948) G5 31.4% | **132 / 6,717** | **패턴 미도달 — `#300`** |
| C++ | 완료 | (0.087) G5 90.2% / (0.072) G5 99.7% | 2,130 / 19,014 | **하네스 결함 — `#296`** |
| Node.js | 재측정 중 | — | — | run 2에서 hang |
| C (`zlink-c`) | 불가 | — | — | **빌드 안 됨 — `#295`** |

`request-backpressure`는 상한 없이 파이프가 찰 때까지 보내는 패턴이다. **C++만 그 조건에
도달한다.** Java는 raw도 framework도 두 자릿수이고 client CPU가 1.9%다. Java의 `1.291`은
깊이 35 대 99의 비교이지 계층 비용이 아니다. §7.2가 이미 적어 두었다 — "도달하지 못하는
조건에서 계산한 비율은 계층 비용이 아니라 그 조건을 잰 값이다."

**지금 이 표의 숫자로 계층을 판정하지 않는다.** 기준선을 내면서 측정 장치 쪽 결함 세 건이
드러났고, 셋 다 framework 성능이 아니라 하네스가 만든 숫자를 낸다.

| 이슈 | 내용 | 막는 것 |
|---|---|---|
| `#295` | C 기준 벤치가 `#63`의 whole-message 이관에서 빠졌다 | `zlink-<lang> / zlink-c`가 계산되지 않아 **분모(raw)가 건강한지 판정할 수 없다** |
| `#296` | C++ 벤치가 빈 라운드마다 고정 1ms 대기(정본은 `min(남은 시간, 50ms)` 블록) | C++ `request-backpressure` G5 90~100% |
| `#300` | .NET raw가 상한 없는 패턴에서 깊이 132로 자기 제한(C++ raw 대비 처리량 1/10, CPU 6%) | .NET의 "1.309 통과"가 분모 때문에 나온 값 |

`#300`이 보여주듯 분모가 병들 수 있다. 그래서 `#295`는 부가 정보가 아니라 **판정의
전제**다. 세 건을 고치고 재측정한 뒤에 §5 표를 확정한다.

raw 행 전환은 Issue #91~#93의 별도 작업이다. framework 행과 raw 행이 모두 규격 §1.3을 만족한
뒤에만 새 비율을 공개한다.

## 5. 결과 표 형식

재측정 결과는 패턴별로 묶고 각 payload에서 아래 행 이름을 그대로 사용한다.

| 언어 | gRPC | raw | framework |
|---|---|---|---|
| .NET | `grpc-dotnet` | `zlink-dotnet` | `zlink-framework-dotnet` |
| Java | `grpc-java` | `zlink-java` | `zlink-framework-java` |
| Node.js | `grpc-node` | `zlink-node` | `zlink-framework-node` |
| C++ | `grpc-cpp` | `zlink-cpp` | `zlink-framework-cpp` |

request 계열 단위는 KOPS, `send-saturation`은 KMSG/s다. 이전 분모로 계산한 배율이나 이전
`request-window` 수치는 이 문서에 옮기지 않는다.

## 5.1 결과 — 2026-09-13 기준선 (`request-backpressure`, 1 KiB)

하네스 결함 7건을 고친 뒤 5 run으로 잰 값이다. `zlink-c` = **603.4 KOPS**.

| 언어 | `zlink-<lang> / zlink-c` (binding) | `zlink-framework-<lang> / zlink-<lang>` (framework) |
|---|---|---|
| **C++** | **0.644** published, **fail** | (0.082) G5 58.3% |
| **.NET** | **0.300** published, **fail** | **0.266** published, **fail** |
| Java | (0.496) G6 — submit 스레드 0.98/1 포화 | (0.041) G6 |
| Node | (0.151) G6 — event loop 1.00/1 포화 | (0.090) G6+G5 |

통과선은 두 식 모두 0.80이다. **published된 세 값이 모두 fail이다.**

`zlink-cpp / zlink-c = 0.644`가 지금 유일하게 신뢰할 수 있는 binding 값이다. C++만
단일 실행 단위 제약이 없어 G6에 걸리지 않는다.

.NET은 두 식이 모두 published다 — `0.300 × 0.266 = 0.080`, 즉 framework 경로가 C API의
8%다.

### 판정이 나오지 않는 이유

| 언어 | 막는 것 | 이슈 |
|---|---|---|
| Java·Node | 단일 실행 단위(submit 스레드 1, event loop 1)가 포화 — C 하네스의 여러 client와 같은 조건이 아니다 | `#310` |
| C++ framework | G5 재현성 58.3%·92.5%. raw는 같은 조건에서 통과하므로 framework 쪽 변동이다 | `#7` |
| 4 KiB 전반 | `zlink-c` 분모가 G5 44.8% | — |

### 이전 값과 비교하면 안 되는 이유

하네스 수정 전 .NET은 `framework/raw = 1.309 "통과"`, Java는 `1.291 "통과"`였다.
**분모가 놀던 값이다** — .NET raw는 36.3 → 180.2 KOPS로 5배 올랐는데 framework는
47.5 → 47.9로 그대로였고, 그래서 비율이 뒤집혔다. 이전 숫자는 이 문서에 옮기지 않는다.

### 함께 기록하는 조건

- `zlink-c` 행과 언어 행이 **서로 다른 Core 바이너리**를 로드한다(`#308`). 이 표의
  formula 1 값은 그 차이를 포함한다.
- 높은 in-flight 깊이에서 Core byte HWM 회계가 언더플로로 abort하는 결함이 있다(`#309`).
  .NET 5 run 중 1건이 그 때문에 실패했다.
