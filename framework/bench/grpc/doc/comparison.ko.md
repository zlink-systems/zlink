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

| 언어 | framework 경로 | 패턴 | 기준선 수치 |
|---|---|---|---|
| Java | RID 직접 지정 | 3종 | 1라운드 구현 완료; 새 공동 기준선 재측정 대상 |
| .NET | RID 직접 지정 | 3종 | 스모크 검증 완료; 3-run 대기 |
| C++ | RID 직접 지정 | 3종 | 스모크 검증 완료; 3-run 대기 |
| Node.js | RID 직접 지정과 schema protobuf serializer | 3종 | 스모크 검증 완료; 3-run 대기 |

raw 행 전환은 Issue #91~#93의 별도 작업이다. framework 행과 raw 행이 모두 규격 §1.3을 만족한
뒤에만 새 비율을 공개한다. 이 작업의 짧은 스모크 값은 실행 가능성 확인용이며 기준선 숫자로
사용하지 않는다.

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
