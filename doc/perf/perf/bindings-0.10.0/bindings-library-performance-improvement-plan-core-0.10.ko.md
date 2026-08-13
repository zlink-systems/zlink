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

아래 값은 `최소 기준 / 중앙값 목표`다. 언어 runtime과 binding 경계를 따로 반영하기 위해
언어를 묶지 않는다.

| 언어 | 단순 one-way | routed one-way | socket request/reply | multi routed echo |
|---|---:|---:|---:|---:|
| C++ | 85% / 90% | 80% / 85% | 75% / 85% | 80% / 85% |
| .NET | 64% / 85% | 75% / 80% | 50% / 70% | 50% / 70% |
| Java | 70% / 90% | 75% / 85% | 50% / 70% | 50% / 70% |
| Node | 35% / 60% | 33% / 60% | 30% / 60% | 30% / 60% |

사용자 지시에 따라 C++ 단순 one-way는 이 작업에서 중앙값 목표 90%를 사용한다. 각
pattern·transport 평균은 최소 기준과 중앙값 목표를 함께 판정한다.

## 현재 확인된 결과

아래 표에는 C와 해당 binding을 동일 조건에서 다시 실행해 확인한 결과만 기록한다.
`재측정 대기`는 기존 수치를 완료 근거로 사용하지 않는다는 뜻이다.

| 언어 | Suite / pattern | Transport | C 대비 throughput 비율 | 산술평균 | 최소 / 중앙값 | 판정 |
|---|---|---|---|---:|---:|---|
| C++ | 전체 | 전체 | 재측정 대기 | - | - | 진행 중 |
| C++ | `MULTI_DEALER_DEALER` | tcp | 86.22 / 89.26 / 145.71 / 78.45 / 85.90 / 79.45% | 94.17% | 85% / 90% | 중앙값 통과 |
| C++ | `MULTI_DEALER_DEALER` | tls | 91.83 / 93.01 / 97.90 / 98.25 / 82.89 / 95.52% | 93.23% | 85% / 90% | 중앙값 통과 |
| C++ | `MULTI_DEALER_DEALER` | ws | 105.72 / 99.13 / 102.61 / 92.84 / 99.38 / 104.15% | 100.64% | 85% / 90% | 중앙값 통과 |
| C++ | `MULTI_DEALER_ROUTER_SENDSEND` | tcp | 104.64 / 99.79 / 99.00 / 99.26 / 93.44 / 102.40% | 99.75% | 80% / 85% | 중앙값 통과 |
| C++ | `MULTI_DEALER_ROUTER_REQREP` | tcp | 94.14 / 95.85 / 96.88 / 92.95 / 89.58 / 95.86% | 94.21% | 75% / 85% | 중앙값 통과 |
| C++ | `MULTI_ROUTER_ROUTER_SENDSEND` | tcp | 101.23 / 98.71 / 101.15 / 104.87 / 98.59 / 99.16% | 100.62% | 80% / 85% | 중앙값 통과 |
| C++ | `MULTI_ROUTER_ROUTER_REQREP` | tcp | 94.88 / 88.96 / 98.76 / 95.35 / 99.90 / 100.49% | 96.39% | 75% / 85% | 중앙값 통과 |
| C++ | `MULTI_PUBSUB` | tcp | 106.66 / 91.72 / 100.34 / 82.24 / 93.89 / 91.80% | 94.49% | 85% / 90% | 중앙값 통과 |
| C++ | `MULTI_STREAM` | tcp | 99.50 / 99.83 / 96.60 / 99.53% | 98.85% | 80% / 85% | 중앙값 통과 |
| .NET | 전체 | 전체 | 재측정 대기 | - | - | 진행 중 |
| .NET | `MULTI_DEALER_DEALER` | tcp | 33.59 / 58.56 / 148.39 / 78.57 / 137.74 / 91.77% | 91.44% | 64% / 85% | 중앙값 통과 |
| .NET | `MULTI_PUBSUB` | tcp | 57.26 / 65.57 / 66.18 / 82.40 / 93.94 / 99.69% | 77.51% | 64% / 85% | 최소 통과, 중앙값 미달 |
| .NET | `MULTI_DEALER_ROUTER_SENDSEND` | tcp | 87.15 / 89.27 / 90.84 / 98.80 / 136.43 / 144.74% | 107.87% | 75% / 80% | 중앙값 통과 |
| .NET | `MULTI_DEALER_ROUTER_REQREP` | tcp | 66.64 / 61.97 / 73.83 / 82.80 / 128.78 / 121.84% | 89.31% | 50% / 70% | 중앙값 통과 |
| .NET | `MULTI_ROUTER_ROUTER_SENDSEND` | tcp | 77.97 / 81.97 / 92.59 / 97.65 / 134.55 / 141.92% | 104.44% | 50% / 70% | 중앙값 통과 |
| .NET | `MULTI_ROUTER_ROUTER_REQREP` | tcp | 70.67 / 67.18 / 77.63 / 81.99 / 114.60 / 318.01% | 121.68% | 50% / 70% | 중앙값 통과 |
| .NET | `MULTI_STREAM` | tcp | 82.50 / 71.84 / 87.08 / 87.99% | 82.35% | 50% / 70% | 중앙값 통과 |
| Java | `MULTI_DEALER_DEALER` | tcp | 51.97 / 69.07 / 94.88 / 84.75 / 105.66 / 86.39% | 82.12% | 70% / 90% | 최소 통과, 중앙값 미달 |
| Java | `MULTI_DEALER_DEALER` | tls | 54.49 / 92.42 / 92.81 / 83.10 / 89.08 / 81.13% | 82.17% | 70% / 90% | 최소 통과, 중앙값 미달 |
| Java | `MULTI_DEALER_DEALER` | ws | 59.93 / 83.06 / 98.78 / 80.03 / 117.48 / 88.95% | 88.04% | 70% / 90% | 최소 통과, 중앙값 미달 |
| Java | `MULTI_DEALER_DEALER` | wss | 64.00 / 88.39 / 101.26 / 87.47 / 89.22 / 84.45% | 85.80% | 70% / 90% | 최소 통과, 중앙값 미달 |
| Java | `MULTI_DEALER_ROUTER_REQREP` | tcp | 57.85 / 60.72 / 60.08 / 85.05 / 140.45 / 108.90% | 85.51% | 50% / 70% | 중앙값 통과 |
| Java | `MULTI_DEALER_ROUTER_SENDSEND` | tcp | 68.65 / 69.48 / 69.41 / 73.75 / 141.34 / 103.96% | 87.77% | 75% / 85% | 중앙값 통과 |
| Java | `MULTI_DEALER_ROUTER_SENDSEND` | tls | 84.84 / 105.18 / 79.15 / 78.41 / 96.98 / 93.19% | 89.63% | 75% / 85% | 중앙값 통과 |
| Java | `MULTI_DEALER_ROUTER_SENDSEND` | ws | 83.19 / 76.00 / 78.10 / 72.83 / 96.73 / 96.04% | 83.81% | 75% / 85% | 최소 통과, 중앙값 미달 |
| Java | `MULTI_DEALER_ROUTER_SENDSEND` | wss | 81.41 / 82.26 / 80.89 / 75.09 / 85.19 / 85.32% | 81.69% | 75% / 85% | 최소 통과, 중앙값 미달 |
| Java | `MULTI_ROUTER_ROUTER_SENDSEND` | tcp | 64.16 / 64.74 / 72.33 / 67.63 / 123.82 / 142.14% | 89.14% | 50% / 70% | 중앙값 통과 |
| Java | `MULTI_ROUTER_ROUTER_SENDSEND` | tls | 84.21 / 72.94 / 72.01 / 76.81 / 90.58 / 90.78% | 81.22% | 50% / 70% | 중앙값 통과 |
| Java | `MULTI_ROUTER_ROUTER_SENDSEND` | ws | 61.69 / 62.83 / 69.08 / 69.55 / 87.42 / 90.38% | 73.49% | 50% / 70% | 중앙값 통과 |
| Java | `MULTI_ROUTER_ROUTER_SENDSEND` | wss | 77.06 / 58.14 / 46.28 / 44.05 / 63.39 / 70.23% | 59.86% | 50% / 70% | 최소 통과, 중앙값 미달 |
| Java | `MULTI_ROUTER_ROUTER_REQREP` | tcp | 74.88 / 62.64 / 60.62 / 60.53 / 132.03 / 122.72% | 85.57% | 50% / 70% | 중앙값 통과 |
| Java | `MULTI_PUBSUB` | tcp | 64.14 / 62.78 / 67.31 / 52.90 / 66.90 / 67.42% | 63.57% | 70% / 90% | 최소 미달, 개선 후보 보류 |
| Java | `MULTI_STREAM` | tcp | 79.14 / 79.22 / 78.76 / 107.14% | 86.07% | 50% / 70% | 중앙값 통과 |
| Node | `MULTI_PUBSUB` | tcp | 25.39 / 34.25 / 31.47 / 59.04 / 44.19 / 46.05% | 40.07% | 35% / 60% | 최소 통과, 중앙값 미달 |
| Node | `MULTI_PUBSUB` | tls | 33.61 / 32.10 / 34.86 / 50.44 / 72.82 / 76.14% | 49.99% | 35% / 60% | 최소 통과, 중앙값 미달 |
| Node | `MULTI_PUBSUB` | ws | 33.88 / 35.47 / 31.29 / 45.38 / 81.35 / 78.24% | 50.94% | 35% / 60% | 최소 통과, 중앙값 미달 |
| Node | `MULTI_PUBSUB` | wss | 31.16 / 34.50 / 40.29 / 53.44 / 78.28 / 77.95% | 52.60% | 35% / 60% | 최소 통과, 중앙값 미달 |
| Node | `MULTI_DEALER_DEALER` | tcp | 27.17 / 39.57 / 31.30 / 48.56 / 42.68 / 62.30% | 41.93% | 35% / 60% | 최소 통과, 중앙값 미달 |
| Node | `MULTI_DEALER_DEALER` | tls | 12.03 / 50.31 / 47.74 / 46.94 / 108.35 / 106.98% | 62.06% | 35% / 60% | 중앙값 통과 |
| Node | `MULTI_DEALER_DEALER` | ws | 18.39 / 45.00 / 47.35 / 44.76 / 70.90 / 89.60% | 52.67% | 35% / 60% | 최소 통과, 중앙값 미달 |
| Node | `MULTI_DEALER_DEALER` | wss | 17.37 / 50.09 / 47.69 / 68.84 / 89.18 / 112.29% | 64.24% | 35% / 60% | 중앙값 통과 |
| Node | `MULTI_DEALER_ROUTER_SENDSEND` | tcp | 61.09 / 57.18 / 58.10 / 64.00 / 78.35 / 73.78% | 65.42% | 33% / 60% | 중앙값 통과 |
| Node | `MULTI_DEALER_ROUTER_SENDSEND` | tls | 60.26 / 61.85 / 64.21 / 58.05 / 88.45 / 93.47% | 71.05% | 33% / 60% | 중앙값 통과 |
| Node | `MULTI_DEALER_ROUTER_SENDSEND` | ws | 65.16 / 69.67 / 60.92 / 60.28 / 77.60 / 80.12% | 68.96% | 33% / 60% | 중앙값 통과 |
| Node | `MULTI_DEALER_ROUTER_SENDSEND` | wss | 64.53 / 63.02 / 64.68 / 68.87 / 97.47 / 99.49% | 76.34% | 33% / 60% | 중앙값 통과 |
| Node | `MULTI_ROUTER_ROUTER_SENDSEND` | tcp | 45.77 / 37.66 / 29.97 / 30.23 / 48.36 / 49.09% | 40.18% | 30% / 60% | 최소 통과, 중앙값 미달 |
| Node | `MULTI_ROUTER_ROUTER_SENDSEND` | tls | 56.91 / 53.92 / 56.70 / 57.61 / 90.23 / 94.32% | 68.28% | 30% / 60% | 중앙값 통과 |
| Node | `MULTI_ROUTER_ROUTER_SENDSEND` | ws | 51.41 / 48.08 / 48.34 / 52.98 / 67.56 / 74.32% | 57.12% | 30% / 60% | 최소 통과, 중앙값 미달, hot path 검토 중 |
| Node | `MULTI_ROUTER_ROUTER_SENDSEND` | wss | 64.69 / 46.39 / 76.62 / 59.55 / 103.86 / 94.20% | 74.22% | 30% / 60% | 중앙값 통과 |
| Node | `MULTI_STREAM` | tcp | 48.13 / 49.71 / 50.61 / 90.29% | 59.69% | 30% / 60% | 최소 통과, 중앙값 미달 |
| Node | `MULTI_STREAM` | tls | 56.50 / 59.85 / 62.92 / 102.94% | 70.55% | 30% / 60% | 중앙값 통과 |
| Node | `MULTI_STREAM` | ws | 52.74 / 52.62 / 58.55 / 101.96% | 66.47% | 30% / 60% | 중앙값 통과 |
| Node | `MULTI_STREAM` | wss | 60.28 / 62.84 / 62.32 / 98.28% | 70.93% | 30% / 60% | 중앙값 통과 |
| Node | non-tcp | 전체 | 재측정 대기 | - | - | 진행 중 |

Java 공개 API 경로 재측정 결과는
`log/2026-08-13-java-public-api-policy-audit.ko.md`에 있다. Node PUB/SUB의 C parity
결과는 `log/2026-08-12-node-pubsub-poll-parity.ko.md`에 있다.
Java multi runner는 `log/2026-08-13-java-native-poller-parity.ko.md`부터 C와 같이
native ready event를 직접 읽고 해당 socket을 `DONT_WAIT`로 drain한다.

## 언어별 현재 평균

아래 평균은 위 표에서 완료된 TCP pattern 평균을 같은 가중치로 계산한 값이다. 아직
`재측정 대기`인 transport와 pattern은 포함하지 않는다.

| 언어 | 포함 pattern 수 | C 대비 단순평균 |
|---|---:|---:|
| C++ | 9 | 96.93% |
| .NET | 7 | 96.37% |
| Java | 13 | 83.91% |
| Node | 20 | 60.18% |

## 완료 조건

각 언어의 runner가 등록한 모든 transport·pattern에 대해 C 기준 결과를 기록하고, 각
transport·pattern 평균이 이 문서의 목표를 충족해야 완료한다. 목표 미달이면 같은 대상의
binding hot path를 개선하고 C와 다시 비교한다.
