# Core 0.15.0 bindings 라이브러리 성능 개선 계획

> 시작일: 2026-08-31
>
> 작업 기준: `main`
>
> 현재 상태: 문서 준비 완료, 측정 대기

이 문서는 Core 0.15.0을 기준으로 각 binding 라이브러리의 비용을 C binding과
비교하려는 유지보수자를 위한 실행 계획이다. 측정 대상은 `Multi` suite뿐이다.
`Single` suite는 이 작업의 비교와 완료 판정에 포함하지 않는다.

Core 0.15.0에서는 request/reply를 구분하는 정보와 요청 sequence를 application
payload 앞의 envelope frame으로 보내지 않고 ZMP metadata로 전달한다. 따라서
REQ/REP도 SEND/SEND와 마찬가지로 perf가 만든 application payload만 message part로
전송한다. 이 wire 구조가 바뀌었으므로 0.14.x 수치와 완료 판정을 가져오지 않는다.
모든 C 기준값과 binding 결과를 0.15.0 release runtime으로 새로 측정한다.

## 1. 완료하려는 작업

이번 작업은 perf 수치를 높이기 위한 harness 조정이 아니라 binding 라이브러리의 실제
비용을 줄이는 것이 목적이다. 각 binding은 같은 Core C API와 같은 Multi workload를
사용하고, 다음 식으로 C 대비 처리량을 계산한다.

```text
binding ratio (%) = binding throughput / C throughput * 100
```

작업 순서는 다음과 같다.

1. Core 0.15.0 GitHub Release와 package provenance를 확인한다.
2. C Multi의 모든 pattern과 transport를 1024B로 측정해 새 기준을 만든다.
3. C++, .NET, Java, Node, Go, Rust, Python을 같은 조건으로 각각 측정한다.
4. 목표 미달 언어는 perf 의미를 바꾸지 않고 binding library hot path를 개선한다.
5. pattern 하나의 모든 transport가 끝날 때마다 이 문서의 상태와 결과 기록을 갱신한다.

현재 요청에 따라 이 문서 작성 뒤에는 측정과 구현을 시작하지 않고 대기한다.

## 2. 고정 측정 범위

### 2.1 Suite와 실행 모델

- 공식 비교에는 `Multi` suite만 사용한다.
- C reference는 nonblocking poller 모델을 사용한다.
- C 이외의 binding은 해당 언어의 coroutine 또는 async runtime을 사용한다.
- C++은 coroutine, .NET은 `Task`, Java는 `CompletionStage`, Node는 `Promise`,
  Go는 goroutine, Rust는 Future, Python은 `asyncio`를 사용한다.
- request client는 reply를 기다린 뒤 다음 request를 보내는 ping-pong 방식으로
  직렬화하지 않는다. admission backpressure를 만날 때까지 요청을 제출하고 여러 reply
  completion을 함께 진행한다.
- SEND/SEND와 REQ/REP는 모두 application payload 하나를 보낸다. REQ/REP 구분과
  sequence는 ZMP metadata가 담당하며 perf가 envelope part를 추가하지 않는다.

세부 실행 의미는 [`PERF_MULTI_TEST_POLICY.md`](../../PERF_MULTI_TEST_POLICY.md)를
따른다.

### 2.2 Pattern, transport와 message size

모든 언어에서 다음 pattern을 측정한다. runner에 빠진 pattern이 있으면 결과에서
제외하지 않고 C와 같은 의미로 perf를 먼저 보완한다. public async terminal이 없어서
같은 의미를 구현할 수 없다면 perf 내부 우회 API를 만들지 않고 binding 계약 문제로
분리해 보고한다.

| Pattern | 측정 단위 | 핵심 의미 |
|---|---|---|
| `MULTI_DEALER_DEALER` | msg/s | one-way send/receive |
| `MULTI_DEALER_ROUTER_SENDSEND` | ops/s | routed send/echo |
| `MULTI_DEALER_ROUTER_REQREP` | ops/s | routed request/reply completion |
| `MULTI_ROUTER_ROUTER_SENDSEND` | ops/s | 양쪽 routing identity를 쓰는 send/echo |
| `MULTI_ROUTER_ROUTER_REQREP` | ops/s | 양쪽 routing identity를 쓰는 request/reply completion |
| `MULTI_PUBSUB` | msg/s | publish/subscribe |
| `MULTI_STREAM` | ops/s | packet handler 기반 stream echo |

초기 기준과 개선 반복은 다음 조건으로 고정한다.

| 항목 | 값 |
|---|---|
| suite | `Multi` |
| transports | `tcp`, `tls`, `ws`, `wss` |
| message size | `1024` bytes |
| clients | `100` |
| I/O threads | `4` |
| build | 공식 Core 0.15.0 Release/LTO package |
| 비교 순서 | 같은 host에서 C 실행 후 binding 실행 |
| 초기 방향 확인 | 3회 중앙값 |
| 최종 판정 | 5회 중앙값 |

64B는 고정 비용 변화에 민감하고 큰 payload는 copy 비용을 확인하기 좋지만 이번 초기
비교에는 포함하지 않는다. 1024B에서 채택할 후보가 나온 뒤 크기별 효과를 일반화해야 할
때만 64B와 큰 payload를 별도 진단값으로 측정한다. 이 진단값은 1024B 완료 판정을
대체하지 않는다.

## 3. 측정 전 확인

측정 전에 다음 항목을 모두 확인한다. 하나라도 맞지 않으면 결과를 기록하지 않는다.

- `VERSION`, `core/CMakeLists.txt`, `core/include/zlink.h`가 0.15.0을 가리킨다.
- `core/v0.15.0` tag와 GitHub Release asset이 최종 Core 0.15.0 source에서 생성됐다.
- runner가 선택한 runtime과 `core-package-provenance.json`이 0.15.0을 보고한다.
- Core는 로컬에서 다시 build하지 않고 GitHub Release package를 사용한다.
- C와 binding report의 pattern, transport, 1024B, clients, I/O threads, duration,
  runs와 effective HWM이 같다.
- 모든 runner가 지원 option을 실제로 적용하며, 알 수 없는 option을 성공으로 무시하지 않는다.
- C와 binding의 REQ/REP application message part 수가 같다.
- C와 binding 모두 request completion을 기다려 다음 request를 제출하는 구조가 아니다.
- report가 `status: complete`이고 memory guard가 client 수를 줄이지 않았다.

## 4. 판정과 개선 원칙

0.14.x 목표치는 새 wire 구조의 한계를 뜻하지 않으므로 0.15.0의 첫 paired 측정에서
다시 검증한다. 초기 목표는 다음과 같이 두되, 목표를 낮춰서 측정 실패나 구조 차이를
완료 처리하지 않는다.

| 언어 | one-way 목표 | routed SEND/SEND 목표 | REQ/REP 목표 | STREAM 목표 |
|---|---:|---:|---:|---:|
| C++ | 90% | 85% | 85% | 80% |
| .NET | 80% | 70% | 70% | 70% |
| Java | 85% | 75% | 70% | 70% |
| Node | 60% | 60% | 60% | 50% |
| Go | 65% | 57% | 53% | 53% |
| Rust | 90% | 85% | 85% | 80% |
| Python | 60% | 60% | 60% | 50% |

- 목표는 같은 pattern의 네 transport 처리량 비율 중앙값으로 판정한다.
- transport 한 곳이 크게 낮으면 종합값으로 숨기지 않고 별도 병목으로 기록한다.
- C++·.NET·Java·Node를 먼저 진행하고, 이어서 Go·Rust·Python을 진행한다.
- perf는 C와 의미가 다르거나 정책을 위반한 경우에만 수정한다.
- allocation, VM/native copy, callback 전달, completion scheduling과 wrapper lifecycle
  비용은 binding library 내부에서 줄인다.
- public API, ownership, error와 callback lifetime을 바꾸거나 private C API로 우회하지 않는다.
- pool은 외부에서 identity를 관찰할 수 없고 반환 시점이 분명한 내부 객체에만 사용한다.
- STREAM은 다른 pattern과 callback·queue 구조가 다르므로 별도 hot path로 분석한다.
- 성능 차이가 없더라도 ownership과 책임 경계를 단순하게 만들고 회귀가 없는 구조 개선은
  근거를 기록한 뒤 채택할 수 있다. 수치 목표를 통과한 것으로 바꾸지는 않는다.

과거에 효과가 있었던 후보와 적용 경계는
[`bindings-performance-optimization-reference.ko.md`](../bindings-0.14.0/bindings-performance-optimization-reference.ko.md)를
참고한다. 과거 수치는 후보 선택에만 사용하며 0.15.0 판정에는 사용하지 않는다.

## 5. 진행 상태

### 5.1 C 기준

각 셀은 C report가 완료됐을 때 수치와 report 경로로 바꾼다.

| Transport | DD | DR SENDSEND | DR REQREP | RR SENDSEND | RR REQREP | PUBSUB | STREAM |
|---|---|---|---|---|---|---|---|
| `tcp` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |
| `tls` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |
| `ws` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |
| `wss` | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |

### 5.2 Binding 진행

각 pattern 상태는 네 transport가 모두 끝났을 때 갱신한다. `통과`에는 C 대비 비율과
paired report를, `미달`에는 가장 낮은 transport와 다음 조사 대상을 함께 기록한다.

| 언어 | DD | DR SENDSEND | DR REQREP | RR SENDSEND | RR REQREP | PUBSUB | STREAM |
|---|---|---|---|---|---|---|---|
| C++ | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |
| .NET | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |
| Java | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |
| Node | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |
| Go | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |
| Rust | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |
| Python | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 | 미측정 |

### 5.3 결과 기록

pattern 하나의 모든 transport가 끝날 때마다 한 행을 추가한다. 중간 report는 원시
기록에 남기되 이 표에는 최종 paired 결과만 적는다.

| 날짜 | 언어 | Pattern | 조건 | C 처리량 | Binding 처리량 | C 대비 비율 | 평균 latency 비율 | 판정 | Report |
|---|---|---|---|---:|---:|---:|---:|---|---|

## 6. 완료 조건

다음 조건을 모두 만족해야 0.15.0 성능 개선을 완료한다.

- C 기준의 7개 pattern과 4개 transport가 1024B로 모두 측정됐다.
- 7개 binding의 모든 pattern과 transport에 complete paired report가 있다.
- runner가 Single 결과를 이 작업의 비교나 완료 판정에 섞지 않는다.
- REQ/REP와 SEND/SEND가 같은 application payload part 수를 사용하고, pattern 구분은
  ZMP metadata가 담당한다.
- 모든 binding이 Multi 정책의 async 실행과 concurrent request completion 의미를 지킨다.
- 채택한 library 변경의 관련 contract test와 최종 5회 성능 비교가 통과한다.
- 미달 항목은 목표를 낮추지 않고 원인, 시도한 안전한 후보와 남은 구조적 비용을 기록한다.
- 측정값과 report 경로는 pattern이 끝날 때마다 이 문서에 반영한다.
