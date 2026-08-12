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
| C++ | 90% | 85% | 85% | 85% |
| .NET | 85% | 80% | 70% | 70% |
| Java | 90% | 85% | 70% | 70% |
| Node | 60% | 60% | 60% | 60% |

사용자 지시에 따라 C++ 단순 one-way는 이 작업에서 90%를 사용한다. 이 표의 값은 각
pattern·transport 평균의 완료 목표다.

## 현재 확인된 결과

아래 표에는 C와 해당 binding을 동일 조건에서 다시 실행해 확인한 결과만 기록한다.
`재측정 대기`는 기존 수치를 완료 근거로 사용하지 않는다는 뜻이다.

| 언어 | Suite / pattern | Transport | C 대비 throughput 비율 | 산술평균 | 목표 | 판정 |
|---|---|---|---|---:|---:|---|
| C++ | 전체 | 전체 | 재측정 대기 | - | - | 진행 중 |
| C++ | `MULTI_DEALER_DEALER` | tcp | 86.22 / 89.26 / 145.71 / 78.45 / 85.90 / 79.45% | 94.17% | 90% | 통과 |
| C++ | `MULTI_DEALER_DEALER` | tls | 86.39 / 88.73 / 96.81 / 84.93 / 80.95 / 97.60% | 89.24% | 90% | 미달 |
| C++ | `MULTI_DEALER_ROUTER_SENDSEND` | tcp | 104.64 / 99.79 / 99.00 / 99.26 / 93.44 / 102.40% | 99.75% | 85% | 통과 |
| C++ | `MULTI_DEALER_ROUTER_REQREP` | tcp | 94.14 / 95.85 / 96.88 / 92.95 / 89.58 / 95.86% | 94.21% | 85% | 통과 |
| C++ | `MULTI_ROUTER_ROUTER_SENDSEND` | tcp | 101.23 / 98.71 / 101.15 / 104.87 / 98.59 / 99.16% | 100.62% | 85% | 통과 |
| C++ | `MULTI_ROUTER_ROUTER_REQREP` | tcp | 94.88 / 88.96 / 98.76 / 95.35 / 99.90 / 100.49% | 96.39% | 85% | 통과 |
| C++ | `MULTI_PUBSUB` | tcp | 106.66 / 91.72 / 100.34 / 82.24 / 93.89 / 91.80% | 94.49% | 90% | 통과 |
| C++ | `MULTI_STREAM` | tcp | 99.50 / 99.83 / 96.60 / 99.53% | 98.85% | 85% | 통과 |
| .NET | 전체 | 전체 | 재측정 대기 | - | - | 진행 중 |
| .NET | `MULTI_DEALER_DEALER` | tcp | 33.59 / 58.56 / 148.39 / 78.57 / 137.74 / 91.77% | 91.44% | 85% | 통과 |
| .NET | `MULTI_PUBSUB` | tcp | 66.58 / 60.71 / 57.22 / 76.19 / 110.03 / 103.58% | 79.05% | 85% | 미달 |
| Java | `MULTI_DEALER_DEALER` | tcp | 69.19 / 84.99 / 83.70 / 86.88 / 83.71 / 86.15% | 82.44% | 90% | 미달 |
| Java | `MULTI_DEALER_ROUTER_REQREP` | tcp | 68.20 / 62.14 / 49.81 / 53.55 / 101.99 / 107.66% | 73.89% | 70% | 통과 |
| Java | `MULTI_DEALER_ROUTER_SENDSEND` | tcp | 76.71 / 60.33 / 61.58 / 60.66 / 105.08 / 96.73% | 76.85% | 85% | 미달 |
| Java | `MULTI_PUBSUB` | tcp | 75.74 / 63.29 / 54.24 / 70.83 / 80.47 / 84.05% | 71.44% | 90% | 미달 |
| Java | `MULTI_STREAM` | tcp | 88.82 / 74.36 / 77.56 / 114.70% | 88.86% | 70% | 통과 |
| Node | `MULTI_PUBSUB` | tcp | 22.05 / 21.75 / 21.23 / 32.79 / 44.52 / 41.94% | 30.71% | 60% | 미달 |
| Node | `MULTI_DEALER_DEALER` | tcp | 18.38 / 32.11 / 34.51 / 61.13 / 40.19 / 46.82% | 38.86% | 60% | 미달 |
| Node | 나머지 | 전체 | 재측정 대기 | - | - | 진행 중 |

Java 재측정 결과와 C parity 변경 내용은
`log/2026-08-12-java-multi-harness-parity.ko.md`에 있다. Node PUB/SUB의 C parity
결과는 `log/2026-08-12-node-pubsub-poll-parity.ko.md`에 있다.

## 완료 조건

각 언어의 runner가 등록한 모든 transport·pattern에 대해 C 기준 결과를 기록하고, 각
transport·pattern 평균이 이 문서의 목표를 충족해야 완료한다. 목표 미달이면 같은 대상의
binding hot path를 개선하고 C와 다시 비교한다.
