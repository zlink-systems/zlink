# core 0.10.1 bindings 성능 개선 결과

작업 브랜치는 `core-0.10.0-bindings-performance`다. 모든 비교는 release Core
`0.10.1`을 사용한다. Core source를 다시 build하지 않는다.

## 측정 기준

- C와 binding을 한 번에 하나씩 실행한다. 병렬 실행하지 않는다.
- 같은 transport, pattern, message size, duration, client 수, HWM과 timeout을 사용한다.
- throughput 비율은 `binding throughput / C throughput * 100`이다.
- 한 transport·pattern의 판정값은 모든 message size 비율의 산술평균이다.
- public contract의 public interface는 변경하지 않는다.
- 측정 명령, 후보 검토, Sol review와 원복 근거는 `log/`에 기록한다.

## 목표

| 언어 | 단순 one-way | routed one-way | socket request/reply | multi routed echo |
|---|---:|---:|---:|---:|
| C++ | 95% | 85% | 85% | 85% |
| .NET | 85% | 80% | 70% | 70% |
| Java | 90% | 85% | 70% | 70% |
| Node | 60% | 60% | 60% | 60% |

C++ 단순 one-way는 개선 시간이 과도하면 이 작업에서만 90%를 사용한다. 이 표의 값은
각 pattern·transport 평균의 완료 목표다.

## 현재 확인된 결과

아래 표에는 C와 해당 binding을 동일 조건에서 다시 실행해 확인한 결과만 기록한다.
`재측정 대기`는 기존 수치를 완료 근거로 사용하지 않는다는 뜻이다.

| 언어 | Suite / pattern | Transport | C 대비 throughput 비율 | 산술평균 | 목표 | 판정 |
|---|---|---|---|---:|---:|---|
| C++ | 전체 | 전체 | 재측정 대기 | - | - | 진행 중 |
| .NET | 전체 | 전체 | 재측정 대기 | - | - | 진행 중 |
| .NET | `MULTI_DEALER_DEALER` | tcp | 35.58 / 59.43 / 111.34 / 75.79 / 88.95 / 101.73% | 78.80% | 85% | 미달 |
| Java | `MULTI_DEALER_DEALER` | tcp | 52.85 / 68.37 / 93.33 / 66.47 / 51.17 / 75.65% | 67.97% | 90% | 미달 |
| Java | `MULTI_DEALER_ROUTER_REQREP` | tcp | 43.03 / 39.98 / 41.45 / 49.39 / 86.50 / 67.49% | 54.64% | 50% | 통과 |
| Java | `MULTI_DEALER_ROUTER_SENDSEND` | tcp | 51.19 / 51.17 / 52.36 / 55.15 / 117.45 / 104.06% | 71.90% | 70% | 통과 |
| Java | `MULTI_PUBSUB` | tcp | 39.08 / 44.13 / 48.73 / 49.57 / 58.33 / 71.98% | 52.64% | 90% | 미달 |
| Java | `MULTI_STREAM` | tcp | 73.87 / 72.57 / 66.41 / 92.46% | 76.33% | 70% | 통과 |
| Node | `MULTI_PUBSUB` | tcp | 19.78 / 19.94 / 19.56 / 28.28 / 48.63 / 44.72% | 30.15% | 60% | 미달 |
| Node | 나머지 | 전체 | 재측정 대기 | - | - | 진행 중 |

Java 재측정 결과와 C parity 변경 내용은
`log/2026-08-12-java-multi-harness-parity.ko.md`에 있다. Node PUB/SUB의 C parity
결과는 `log/2026-08-12-node-pubsub-poll-parity.ko.md`에 있다.

## 완료 조건

각 언어의 runner가 등록한 모든 transport·pattern에 대해 C 기준 결과를 기록하고, 각
transport·pattern 평균이 이 문서의 목표를 충족해야 완료한다. 목표 미달이면 같은 대상의
binding hot path를 개선하고 C와 다시 비교한다.
