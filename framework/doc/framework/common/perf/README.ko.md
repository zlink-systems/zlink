<!-- framework-adapter-nav:start -->
[문서 목록](../../../README.ko.md) | [공통 스펙](../README.ko.md) | [Scenario E2E](../e2e/README.ko.md)
<!-- framework-adapter-nav:end -->

# Framework Performance 테스트 공통 규격

이 문서는 ZLink Framework의 언어별 성능 테스트를 같은 기준으로 만들기 위한 공통 규격이다.
여기서 말하는 성능 테스트는 공개 API 계약을 새로 정의하는 문서가 아니다. 각 언어가 이미 제공하는
framework public API와 stream connector public API를 사용해서, 같은 서버 구성과 같은 메시지 흐름에서
성능을 측정하는 실행 기준이다.

성능 테스트는 기능 검증을 대신하지 않는다. 기능이 맞는지는 contract, sample, e2e가 먼저 확인한다.
성능 테스트는 그 위에서 처리량, 지연 시간, 자원 사용량, 실패율을 안정적으로 측정한다. 따라서 테스트
코드는 기능을 우회하거나 내부 runtime API를 직접 호출하지 않는다.

## 1. 목표

성능 테스트의 목표는 숫자를 하나 얻는 것이 아니라, 어느 계층이 병목인지 설명할 수 있는 결과를 얻는
것이다. 모든 언어는 아래 질문에 답할 수 있어야 한다.

- stream connector가 많은 client 연결을 처리할 때 처리량과 지연 시간이 어떻게 변하는가.
- session과 actor가 같은 서버에 있을 때와 다른 서버에 있을 때 비용 차이가 얼마나 나는가.
- server 간 channel과 Spot messaging에서 request/reply 방식과 send/send 방식의 차이가 무엇인가.
- Spot handler가 remote request의 `Async` 완료를 기다릴 때 framework의 자동 turn 관리로
  head-of-line blocking이 얼마나 줄어드는가.
- Spot handler가 `runCpuWorker(...)`로 local worker pool에 작업을 맡기고 완료를 기다릴 때 turn
  관리가 queue 진행성과 continuation 재개 비용에 어떤 영향을 주는가.
- payload 크기가 1 KiB에서 4 KiB로 커졌을 때 처리량과 지연 시간이 어떻게 변하는가.
- client runner, server process, framework dispatch, codec, transport 중 병목 후보가 어디인지
  evidence로 분리할 수 있는가.

## 2. 범위

공통 성능 테스트는 `.NET`, Java, Kotlin, Node.js, C++ framework에서 같은 의미로 구현한다. 언어별
문법과 runner 도구는 달라도 시나리오 이름, payload 크기, 측정 구간, 메트릭 이름, 성공 조건은 맞춘다.

초기 범위는 아래 일곱 가지 축이다. 모든 표준 시나리오는 `1 KiB`와 `4 KiB` 두 payload 크기로 실행한다.
다만 결과를 해석할 때 CS(session/actor) 계열과 Spot execution 계열, PS(publish-subscribe) 계열은
`1 KiB`, S2S(channel/Spot)와 AC(actor client no-bind) 계열은 `4 KiB`를 대표 수치로 본다.

| 축 | 목적 | 대표 payload | 함께 실행할 payload |
|----|------|---------------|--------------------|
| connector → session → actor local echo | client/server 경계와 같은 서버 actor dispatch 비용 측정 | 1 KiB | 4 KiB |
| connector → session → remote actor echo | session 서버와 actor 서버가 분리된 구조의 비용 측정 | 1 KiB | 4 KiB |
| channel → remote Spot echo | server 간 channel에서 remote Spot으로 요청하거나 전송하는 비용 측정 | 4 KiB | 1 KiB |
| remote Spot → channel echo | remote Spot에서 channel server로 요청하거나 전송하는 비용 측정 | 4 KiB | 1 KiB |
| Spot worker echo | Spot handler의 remote request `async` 대기와 `runCpuWorker(...)` local worker pool offload 대기 비용, queue 진행성 측정 | 1 KiB | 4 KiB |
| actor client(no-bind) → actor echo | session 없는 server 측 caller가 `ActorRef`로 직접 send/request하는 비용 측정 | 4 KiB | 1 KiB |
| publish → subscriber fanout | publisher 하나가 여러 subscriber에게 이벤트를 뿌릴 때 fanout 처리량과 delivery latency 측정 | 1 KiB | 4 KiB |

`publish → subscriber fanout` 축은 `framework/doc/framework/common/spec/03-interaction-model.ko.md` §3.3과
`framework/doc/framework/common/spec/07-channel-topology.ko.md`가 정의하는 `publish-subscribe` 공용
상호작용 모델, 그리고 `framework/doc/framework/common/e2e/config-3-pubsub.ko.md`의 fanout 시나리오를
기준으로 한다. `publish-subscribe`는 이미 5개 언어 모두 구현된 공개 계약이므로, 이 축은 언어별
스킵 없이 다른 다섯 축과 동일하게 필수로 구현한다.

`actor client(no-bind) → actor echo` 축은
`framework/doc/framework/common/spec/14-actor-model.ko.md` 6.1절의 actor client 계약과
`framework/doc/framework/common/e2e/config-9-to-actor-messaging.ko.md` 시나리오를 기준으로 한다.
공개 actor client를 제공하는 모든 framework 언어는 이 축을 같은 필수 perf 범위로 유지한다.

payload 크기는 message payload 본문 크기를 뜻한다. framework header, connector frame, codec metadata는
포함하지 않는다. 각 언어는 같은 byte pattern을 만들어야 한다. 압축이나 문자열 interning 효과가 결과를
왜곡하지 않도록 payload는 반복 문자 하나로 채우지 않고 고정 seed 또는 index pattern으로 채운다.
공통 표준 실행은 `1 KiB`와 `4 KiB`만 요구한다. 더 작은 메시지나 더 큰 메시지는 ad-hoc 분석으로
추가할 수 있지만, 모든 언어가 반드시 구현해야 하는 공통 perf 범위에는 넣지 않는다.

## 3. 성능 테스트와 다른 테스트의 관계

| 테스트 종류 | 주 목적 | 프로세스 경계 | 결과 해석 |
|-------------|---------|---------------|-----------|
| unit/contract | API와 작은 동작의 정확성 | 대부분 in-process | 빠른 회귀 검출 |
| sample | 사용자가 따라 할 수 있는 정상 흐름 | 실제 서버와 client | public 사용법 검증 |
| e2e | 배포 형태와 실패 경로 검증 | 실제 multi-process | 기능 조합 검증 |
| perf | 처리량, 지연 시간, 자원 사용량 측정 | 실제 multi-process | 병목 후보와 회귀 추적 |

perf는 sample 또는 e2e 서버를 재사용하지 않는다. 성능 테스트 서버는 측정 대상이 명확해야 하므로,
도메인 규칙을 최소화한 echo 서버로 둔다. 다만 서버 구성은 sample에서 검증한 실제 framework 구조를
따른다. 예를 들어 local session/actor 구조는 TicTacToe형이고, remote session/actor 구조는 Bingo형이다.

## 4. 공통 실행 모델

모든 perf runner는 아래 phase를 같은 순서로 실행한다.

| Phase | 이름 | 설명 | 메트릭 포함 여부 |
|-------|------|------|------------------|
| 1 | build/preflight | release build, 포트 예약, OS limit 확인, 로그 디렉토리 준비 | 제외 |
| 2 | server start | 필요한 서버 process를 시작하고 readiness를 확인 | 제외 |
| 3 | connect | client connector를 만들고 인증 또는 세션 준비를 끝낸다 | 별도 기록, throughput 계산 제외 |
| 4 | warmup | JIT, codec cache, connection path, pool을 예열한다 | 제외 |
| 5 | reset | client/server 메트릭을 동시에 리셋한다 | 제외 |
| 6 | measured | duration 동안 실제 부하를 넣는다 | 포함 |
| 7 | settle | 마지막 응답과 server metric 반영을 기다린다 | throughput 계산 제외 |
| 8 | report | client/server 메트릭을 수집하고 결과 파일을 쓴다 | 제외 |
| 9 | cleanup | 서버와 client process를 종료한다 | 제외 |

벤치마크는 기본적으로 duration 기반이다. message count 기반 테스트는 짧은 smoke와 디버깅용으로만 둔다.
duration 기반으로 해야 언어별 runtime warmup 차이와 client 분산 실행을 비교하기 쉽다.

## 5. 공통 CLI

각 언어 runner는 같은 의미의 옵션을 제공한다. 옵션 이름은 언어별 관례에 맞게 바꿀 수 있지만,
shell runner에서는 아래 long option을 지원해야 한다.

| 옵션 | 기본값 | 의미 |
|------|--------|------|
| `--scenario` | 필수 | 실행할 시나리오 이름 |
| `--connections` | `10000` | 전체 connector client 수 |
| `--client-index` | `0` | 여러 load generator 중 현재 runner의 index |
| `--client-count` | `1` | 전체 load generator 수 |
| `--duration-seconds` | `30` | measured phase 시간 |
| `--warmup-seconds` | `5` | warmup phase 시간 |
| `--payload-size` | 시나리오 대표값 | 단일 payload byte 크기 |
| `--payload-sizes` | `1024,4096` | 여러 payload 크기를 순서대로 실행 |
| `--inflight` | `1` | client당 동시 요청 또는 미완료 echo 수 |
| `--connect-concurrency` | `256` | 동시에 연결을 시도하는 connector 수 |
| `--spot-count` | `16` | Spot execution 시나리오에서 부하를 분산할 Spot RID 개수. `spot-await-contention`은 이 값과 무관하게 `1`로 고정한다 |
| `--subscriber-count` | `8` | pub/sub 시나리오에서 fanout을 받는 subscriber process 수 |
| `--worker-task-millis` | `5` | Spot worker offload 시나리오에서 `runCpuWorker(...)`가 수행할 고정 비용 CPU 작업 시간 |
| `--worker-pool-size` | `8` | Spot worker offload 시나리오에서 framework worker pool의 최대 thread 수 |
| `--mode` | 시나리오 기본값 | `request`, `send-send`, `async-request`, `no-await`, `publish`, `worker-offload` 중 하나 |
| `--codec` | `json` | payload codec. 시나리오가 고정하면 override하지 않는다 |
| `--output` | `perf-results/<run-id>` | 결과 파일 디렉토리 |
| `--run-id` | timestamp | 로그와 결과를 묶는 실행 id |
| `--endpoint-config` | script가 생성 | server role별 app endpoint와 metrics endpoint를 담은 JSON 파일 |

`--client-index`와 `--client-count`는 10,000 connector가 한 process에 몰려 client runner가 병목이 되는
상황을 피하기 위한 필수 옵션이다. 예를 들어 `--connections 10000 --client-count 4`이면 각 runner는
2,500개 connector를 맡는다. 나누어 떨어지지 않으면 낮은 index부터 하나씩 더 맡는다.

`run_perf.sh`는 `--payload-sizes 1024,4096`을 기본으로 사용한다. `run_single.sh`는 디버깅 편의를 위해
`--payload-size` 단일 값 실행을 지원한다. 표준 결과 비교는 payload 크기별로 분리해서 기록하고,
서로 다른 payload 크기의 KOPS를 한 줄로 평균 내지 않는다.

`--endpoint-config`는 runner script가 server process를 띄운 뒤 생성한다. client runner와 server
trigger runner는 이 파일만 보고 role별 endpoint를 찾는다. multi-role scenario에서 command line에
endpoint option을 여러 개 늘어놓으면 언어별로 이름이 달라지기 쉬우므로, 공통 입력은 JSON 파일 하나로
고정한다.

```json
{
  "runId": "20260626-123000",
  "roles": {
    "sessionActorLocal": {
      "appEndpoint": "tcp://127.0.0.1:21001",
      "metricsUrl": "http://127.0.0.1:31001"
    },
    "session": {
      "appEndpoint": "tcp://127.0.0.1:21002",
      "metricsUrl": "http://127.0.0.1:31002"
    },
    "actor": {
      "appEndpoint": "tcp://127.0.0.1:21003",
      "metricsUrl": "http://127.0.0.1:31003"
    },
    "channel": {
      "appEndpoint": "tcp://127.0.0.1:21004",
      "metricsUrl": "http://127.0.0.1:31004"
    },
    "spot": {
      "appEndpoint": "tcp://127.0.0.1:21005",
      "metricsUrl": "http://127.0.0.1:31005",
      "spotRids": ["perf-spot-0", "perf-spot-1", "perf-spot-2", "perf-spot-3"]
    },
    "remoteEcho": {
      "appEndpoint": "tcp://127.0.0.1:21006",
      "metricsUrl": "http://127.0.0.1:31006"
    },
    "publisher": {
      "appEndpoint": "tcp://127.0.0.1:21008",
      "metricsUrl": "http://127.0.0.1:31008"
    },
    "subscribers": [
      { "appEndpoint": "tcp://127.0.0.1:21101", "metricsUrl": "http://127.0.0.1:31101", "subscriberId": 0 },
      { "appEndpoint": "tcp://127.0.0.1:21102", "metricsUrl": "http://127.0.0.1:31102", "subscriberId": 1 }
    ],
    "registry": {
      "appEndpoint": "tcp://127.0.0.1:21007",
      "metricsUrl": "http://127.0.0.1:31007"
    }
  }
}
```

필요 없는 role은 생략한다. `appEndpoint`는 해당 role의 framework 통신 endpoint이고, `metricsUrl`은
성능 테스트 전용 HTTP endpoint다. scenario가 benchmark 시작을 server에 알려야 하면 해당 role의
`appEndpoint`로 `PerfTriggerRequest`를 보낸다. HTTP metrics endpoint를 trigger 경로로 사용하지 않는다.

`subscribers`는 `spotRids`와 달리 한 role 안의 목록이 아니라, 독립 프로세스 role을 배열로 나열한
것이다. 배열 길이는 `--subscriber-count`와 같다. 각 항목은 자기 `metricsUrl`을 가진 별도
process이므로, `/perf/reset`·`/perf/stats`도 subscriber마다 따로 호출하고 결과도 subscriber별로
남긴다(§15 `server-<role>.json` 규칙을 `server-subscriber-<subscriberId>.json`으로 확장한다).

`spot` role의 `spotRids`는 Spot server가 미리 만들어 둔 Spot RID 목록이며, 길이는 `--spot-count`와
같다. client는 connector마다 이 목록을 순서대로 나눠 맡아 요청을 여러 Spot RID로 분산한다.
`spot-await-contention`은 `--spot-count 1`을 강제하므로 이 목록의 첫 번째 값만 사용하고, 모든
connector가 같은 Spot RID로 요청을 보낸다. 여러 Spot RID로 부하를 나누는 시나리오와 단일 Spot RID에
집중하는 시나리오는 이 목록 길이 하나로 구분되어야 하며, 언어별 구현이 임의로 RID 수를 정하면 안 된다.

## 6. 표준 프로젝트 구조

perf 프로젝트는 sample이나 e2e와 분리한다. 성능 테스트는 측정 경로가 단순해야 하므로, sample의
도메인 코드나 e2e의 장애 시나리오 코드를 재사용하지 않는다. 대신 PlayHouse benchmark처럼 server,
client, shared, metrics를 별도 실행 프로젝트로 나누고, runner script가 프로세스를 띄우고 정리한다.

언어별 실제 build 파일 이름은 달라도, 논리 구조는 아래를 따른다.

```text
framework/languages/<lang>/perf/
|-- README.ko.md
|-- Shared/
|   |-- Contracts/          echo request/reply, trigger, metrics DTO
|   `-- Payload/            payload 생성 규칙과 검증 helper
|-- Client/
|   |-- Program.*           CLI parsing과 scenario 선택만 담당
|   |-- Scenarios/
|   |   |-- CsLocalSessionActorEchoScenario.*
|   |   |-- CsRemoteSessionActorEchoScenario.*
|   |   |-- S2sChannelToSpotRequestEchoScenario.*
|   |   |-- S2sChannelToSpotSendSendEchoScenario.*
|   |   |-- S2sSpotToChannelRequestEchoScenario.*
|   |   |-- S2sSpotToChannelSendSendEchoScenario.*
|   |   |-- SpotAsyncRequestEchoScenario.*
|   |   |-- SpotAwaitContentionScenario.*
|   |   |-- SpotNoAwaitEchoScenario.*
|   |   |-- SpotWorkerOffloadEchoScenario.*
|   |   |-- ActorNoBindRequestEchoScenario.*
|   |   |-- ActorNoBindSendSendEchoScenario.*
|   |   `-- PubSubFanoutEchoScenario.*
|   |-- Support/
|   |   |-- PerfClientOptions.*
|   |   |-- PerfRunPlan.*
|   |   |-- ConnectionPool.*
|   |   |-- CorrelationTable.*
|   |   |-- InFlightLimiter.*
|   |   |-- MetricsClient.*
|   |   |-- ClientMetricsCollector.*
|   |   |-- ResultWriter.*
|   |   `-- ScenarioRunner.*
|   `-- README.ko.md
|-- Servers/
|   |-- SessionActorLocal/
|   |-- Session/
|   |-- Actor/
|   |-- Channel/
|   |-- Spot/
|   |-- RemoteEcho/
|   |-- ActorCaller/        actor client를 호출하는 session 없는 server
|   |-- Publisher/
|   |-- Subscriber/
|   `-- Registry/           registry가 필요한 언어/시나리오에서만 둔다
|-- ServerSupport/
|   |-- Metrics/
|   |-- Readiness/
|   |-- Logging/
|   |-- Payload/
|   `-- ProcessMetrics/
|-- scripts/
|   |-- run_perf.sh
|   |-- run_single.sh
|   `-- collect_env.sh
`-- perf-results/           gitignore 대상
```

언어별 naming convention 때문에 `Shared`, `Client`, `Servers` 대신 `shared`, `client`,
`servers`를 써도 된다. 다만 한 언어 안에서는 하나의 규칙을 유지하고, 공통 문서나 결과 파일에서는
위 논리 이름을 사용한다.

### 6.1 top-level 책임

| 위치 | 책임 | 금지 사항 |
|------|------|-----------|
| `Shared/` | client/server가 함께 쓰는 message 계약, payload 생성 규칙, result DTO | server host 구성, framework handler, client runner 로직 |
| `Client/` | CLI, scenario 실행, connector 생성, warmup/measured phase, 결과 저장 | server process 직접 구현, framework 내부 API 호출 |
| `Client/Scenarios/` | 시나리오별 connector public call 흐름과 검증 기준 | 공통 옵션 parsing, metrics endpoint 세부 구현 |
| `Client/Support/` | option, run plan, connection pool, metrics client, result writer | connector 호출을 감싸서 scenario 의미를 숨기는 helper |
| `Servers/<Role>/` | role별 실행 서버와 benchmark handler | `--role` 하나로 여러 서버 역할 바꾸기 |
| `ServerSupport/` | role들이 공유하는 metrics, readiness, logging, payload 검증 | 업무 echo 흐름, role-specific handler |
| `scripts/` | build, preflight, server start, client start, cleanup | 성능 측정 hot path 로직 |

`Program.*`은 얇게 유지한다. CLI parsing, logging 초기화, DI/host factory 호출, scenario 선택만 둔다.
PlayHouse benchmark의 `Program.cs`처럼 옵션을 읽고 runner를 호출하는 진입점은 괜찮지만, measured phase
loop나 echo completion 집계가 `Program.*`에 들어가면 안 된다.

### 6.2 10,000 client 구동 모델

10,000 connector client는 하나의 큰 helper가 아니라 client runner의 표준 실행 모델로 다룬다.
PlayHouse benchmark처럼 runner는 connector 배열 또는 connection pool을 만들고, 제한된 동시성으로
connect/auth를 끝낸 뒤, 같은 pool을 warmup과 measured phase에 넘긴다.

기본 구조는 아래와 같다.

```text
Client Program
  -> parse options
  -> build PerfRunPlan
  -> create ScenarioRunner
  -> run selected Scenario

ScenarioRunner
  -> reserve client id range
  -> create ConnectionPool
  -> connect/auth with max connect concurrency
  -> run warmup on a bounded subset or all connections
  -> reset client/server metrics
  -> run measured operation on every connected connector
  -> collect metrics and write result
  -> disconnect all connectors
```

`ConnectionPool`은 `connections`, `clientIndex`, `clientCount`를 기준으로 현재 process가 맡을 client id
범위를 계산한다. 예를 들어 전체 10,000개 connector를 4개 load generator로 나누면 각 process는
대략 2,500개 connector만 만든다. `clientId`는 전역 id로 유지해야 한다. 그래야 여러 load generator의
결과를 합쳐도 correlation id와 오류 로그가 충돌하지 않는다.

연결은 한 번에 10,000개를 동시에 시도하지 않는다. 기본 `--connect-concurrency 256`으로 제한하고,
각 connector는 최대 3회 정도 재시도한다. 연결 실패는 숨기지 않고 `connections.failed`에 기록한다.
measured phase는 연결에 성공한 connector만 사용한다. 단, 연결 성공률이 너무 낮아 결과가 의미 없으면
runner가 실패해야 한다. 기본 실패 기준은 연결 성공률 99% 미만이다.

warmup은 measured 결과에 섞지 않는다. warmup connection 수는 구현 언어가 감당 가능한 범위로 제한할 수
있지만, 제한했다면 결과 config에 `warmupConnections`를 기록한다. request callback, push wait,
send/send처럼 warmup 후 callback state가 남을 수 있는 구현은 PlayHouse benchmark처럼 warmup 후
connection을 재생성하거나 pending state를 완전히 비워야 한다.

measured phase에서는 connector마다 같은 operation loop를 실행한다. request 계열은 client당 `inflight`
개까지 미완료 request를 유지하고, send/send 계열은 correlation table의 미완료 항목 수를 `inflight`로
제한한다. connector 구현이 명시적인 dispatch pump를 요구하는 언어는 operation loop 안에서 주기적으로
public dispatch/poll API를 호출한다. 이 호출도 connector public API여야 하며, runtime 내부 pump를 직접
부르면 안 된다.

client runner가 병목인지 확인하기 위해 각 client process는 자기 CPU 사용률, event loop delay 또는
thread pool queue 같은 언어별 runner 상태를 기록한다. client process CPU가 포화된 실행은 server 성능
한계로 해석하지 않는다.

### 6.3 server 역할 분리

서버 역할이 다르면 별도 실행 프로젝트로 둔다. 하나의 binary에 `--role session`, `--role actor`,
`--role spot`처럼 역할을 바꾸는 옵션을 넣으면 실행 편의는 좋아지지만, 실제 배포 프로세스 경계와
성능 결과가 흐려진다.

| 서버 | 포함하는 것 | 측정 경로 |
|------|-------------|-----------|
| `SessionActorLocal` | stream session, local actor, actor factory, local echo handler | connector → session → actor |
| `Session` | stream session, remote actor relay, request reply 처리 | connector → session → remote actor |
| `Actor` | actor/Entry Spot 또는 actor owner Spot, echo actor handler | remote actor echo, actor client no-bind echo |
| `Channel` | channel request/send handler, trigger endpoint | channel ↔ Spot |
| `Spot` | Spot factory, Spot handler, timer가 필요하면 perf 전용 timer | Spot ↔ channel, automatic turn |
| `RemoteEcho` | Spot execution 시나리오에서 Spot handler가 호출하는 단순 channel echo server | 자동 turn 관리가 적용되는 remote request |
| `ActorCaller` | session을 만들지 않는 외부 caller, actor client의 `SendToActor`/`RequestToActor` 호출 실행 | actor client no-bind echo |
| `Publisher` | publish channel server, `EventPublish`/`Publish(...).Async()` 공개 API로 이벤트 발행 | publish fanout |
| `Subscriber` | subscribe handler, 수신 event를 evidence/metric으로 기록 | publish fanout |
| `Registry` | discovery가 필요한 구성의 registry | measured path 아님 |

server process는 각각 자기 metrics endpoint를 가진다. 여러 server가 하나의 metrics endpoint를 공유하면
어느 role이 병목인지 분리할 수 없다.

`ActorCaller`가 참여하는 시나리오는 actor 위치 resolve를 위해 공식 location store(예: Redis) 확장이
필요하다. 이 구성에서는 `Registry` role 대신 location store 자체를 공유 의존성으로 띄우고, endpoint-config에
role별 endpoint를 기록한다.

`Publisher`/`Subscriber`도 fanout 연결을 위해 같은 location store가 필요하다(config-3-pubsub과 동일).
`Subscriber`는 `--subscriber-count`개 별도 process로 띄우고, 하나의 binary가 `--role subscriber`로
여러 인스턴스를 흉내 내지 않는다.

### 6.4 support 코드 분리

support 코드는 측정 장치의 복잡성을 낮추기 위한 곳이다. scenario의 핵심 흐름을 숨기는 곳이 아니다.
client scenario는 가능하면 connector public API 호출만으로 본문을 구성한다. connector 표면이 이미
충분히 의도를 드러내므로, request/send/wait 호출을 다시 감싸는 helper는 기본적으로 만들지 않는다.

좋은 분리:

- `ConnectionPool`은 10,000 connector 생성, 연결 재시도, connect concurrency 제한을 맡는다.
- `ScenarioRunner`는 phase 순서와 cancellation을 맡는다.
- `ClientMetricsCollector`는 latency와 counter를 기록한다.
- `MetricsClient`는 `/perf/reset`, `/perf/stats`, `/perf/ready` 호출을 맡는다.
- `ResultWriter`는 `result.json`, `summary.txt`, per-role metric 파일을 쓴다.

나쁜 분리:

- `RunAllBenchmarkLogic(...)`처럼 scenario 흐름 전체를 숨기는 helper.
- `SendMagicEcho(...)`, `RequestEchoAsync(...)`, `WaitEchoReply(...)`처럼 connector public call을
  다시 감싸 request/send/send-send 차이를 호출 지점에서 보이지 않게 만드는 helper.
- server 역할별 framework 호출을 client support에 넣는 helper.
- public API가 아니라 runtime 내부 객체를 받아서 빠른 경로를 만드는 helper.

## 7. 폴더 구성 규칙

폴더는 책임이 반복될 때만 만든다. 파일 하나를 넣기 위한 `Utils/`, `Common/`, `Infrastructure/` 폴더는
만들지 않는다. 성능 테스트는 읽는 사람이 측정 경로를 빠르게 찾아야 하므로, 폴더 이름은 역할과 책임을
직접 드러내야 한다.

### 7.1 client 폴더

`Client/Scenarios/` 아래 파일은 시나리오 이름과 1:1로 대응한다. 예를 들어
`s2s-channel-to-spot-send-send-echo`는 `S2sChannelToSpotSendSendEchoScenario.*` 파일에 둔다.
한 파일에 여러 시나리오를 합치지 않는다.

`Client/Support/`에는 아래 성격만 둔다.

| 파일 | 역할 |
|------|------|
| `PerfClientOptions.*` | CLI 옵션을 typed 설정으로 변환 |
| `PerfRunPlan.*` | 전체 connections를 client-index/client-count 기준으로 분할 |
| `ConnectionPool.*` | connector 생성, 인증, 재시도, 정리 |
| `CorrelationTable.*` | send/send completion을 correlation id로 집계 |
| `InFlightLimiter.*` | request와 send/send의 client당 미완료 작업 수 제한 |
| `ScenarioRunner.*` | phase 순서 실행 |
| `ClientMetricsCollector.*` | client latency/counter 수집 |
| `MetricsClient.*` | server metrics endpoint 호출 |
| `ResultWriter.*` | 결과 파일 작성 |
| `PayloadFactory.*` | payload pattern 생성과 검증. connector 호출은 넣지 않음 |

### 7.2 server 폴더

각 `Servers/<Role>/`은 독립 실행 프로젝트다. role 내부 구조는 작게 시작한다.

```text
Servers/Spot/
|-- Program.*
|-- SpotServerHostFactory.*
|-- SpotPerfOptions.*
|-- EchoSpot.*
|-- Handlers/
|   |-- SpotEchoRequestHandler.*
|   `-- SpotEchoSendHandler.*
`-- README.ko.md
```

handler가 한두 개면 `Handlers/` 폴더 없이 server root에 둘 수 있다. handler가 많아져서 request,
send, trigger, metrics가 섞일 때만 폴더를 만든다. metrics endpoint는 공통 `ServerSupport/Metrics`를
사용하되, role별 endpoint 등록은 role server가 직접 한다.

### 7.3 result와 log 폴더

`perf-results/`, `logs/`, `tmp/`는 gitignore 대상이다. runner는 실행마다 고유 `run-id` 하위 폴더를
만든다. 같은 폴더에 이전 실행 결과를 덮어쓰면 비교가 어려워지므로 금지한다.

## 8. client scenario 작성 스타일

client scenario는 sample의 `ClientScenario`처럼 실제 사용자 흐름이 보이게 작성한다. PlayHouse
benchmark의 runner처럼 connect, warmup, measured phase는 공통 runner가 맡되, "무엇을 보낼지"와
"무엇을 완료로 볼지"는 scenario 파일에 드러나야 한다. 특히 connector가 이미 public API로
request, send, wait, dispatch 흐름을 잘 표현한다면 그 호출을 그대로 사용한다.

### 8.1 scenario 파일 첫머리

각 scenario 파일 첫머리에는 아래 내용을 짧게 적는다.

- 이 시나리오가 측정하는 서버 구성.
- measured operation 하나가 무엇을 뜻하는지.
- request 방식인지 send/send 방식인지.
- payload 크기 기본값.
- 실패를 어떤 metric으로 기록하는지.

주석은 코드의 반복 설명이 아니라 측정 의미를 설명한다. 예를 들어 "request를 보낸다"가 아니라
"client-visible echo completion 하나를 KOPS 1 operation으로 센다"처럼 적는다.

### 8.2 scenario 본문

scenario 본문은 아래 흐름을 유지한다.

1. `ScenarioContext` 또는 같은 의미의 객체에서 connector pool, payload factory, metrics collector를
   받는다.
2. warmup에서 사용할 operation을 정의한다.
3. measured phase에서 사용할 operation을 정의한다.
4. operation 완료 확정 기준을 metrics에 기록한다.
5. payload 크기와 byte pattern을 검증한다.

request 시나리오는 요청과 응답이 한 함수 안에서 보이게 둔다. 아래 흐름의 각 줄은 실제 언어별
connector public call 또는 그에 가까운 호출로 보여야 한다.

```text
send PerfEchoRequest
await PerfEchoReply
verify payload
record completion latency
```

send/send 시나리오는 correlation 등록, send, reply 수신, timeout 처리가 보이게 둔다.

```text
register correlation
send PerfEchoRequest
on PerfEchoReply:
  complete correlation
  verify payload
  record completion latency
expire old correlations:
  record timeout
```

이 흐름을 `EchoAsync(...)` 같은 이름 하나로 감추면 request와 send/send의 차이를 리뷰하기 어렵다.
반복을 줄이고 싶으면 "payload 생성", "latency 기록", "correlation table"처럼 좁은 책임만 helper로
뺀다.

### 8.3 helper 사용 기준

helper는 connector 호출을 짧게 만들기 위해 쓰지 않는다. scenario 본문에서 아래가 직접 보여야 한다.

- 어떤 connector가 어떤 packet/message를 보내는지.
- request인지 send인지.
- reply를 await하는지, push/wait callback으로 받는지.
- correlation id를 언제 등록하고 언제 완료하는지.
- timeout을 어떤 단위로 실패 처리하는지.

허용되는 helper는 측정 흐름을 숨기지 않는 것뿐이다.

| 허용 | 이유 |
|------|------|
| payload byte pattern 생성 | payload 생성 규칙은 모든 scenario가 공유한다 |
| latency recorder 호출 | 측정 저장 방식은 scenario 의미가 아니다 |
| percentile 계산 | 결과 집계 로직이다 |
| connection pool 준비 | 10,000 connector 생성은 측정 전 phase다 |
| server metrics HTTP client | measured path가 아니라 reset/snapshot 도구다 |

금지되는 helper는 아래와 같다.

| 금지 | 이유 |
|------|------|
| `EchoAsync(connector, payload)` | request/send/wait 차이를 숨긴다 |
| `SendAndWait(...)` | send/send correlation과 timeout 정책이 보이지 않는다 |
| `RunScenario(...)` 안에 connector 호출 전체 넣기 | scenario 파일이 이름만 남고 측정 흐름이 사라진다 |
| server trigger와 completion 집계를 한 helper에 합치기 | client-visible operation 정의가 불명확해진다 |

connector 호출이 반복되어 길어질 때는 먼저 connector API가 충분히 깊은지 확인한다. public connector
표면으로 읽기 어렵다면 perf helper로 숨기지 말고, framework/connector public API 개선이 필요한지
별도 설계 이슈로 분리한다.

### 8.4 naming

scenario class 또는 file 이름은 아래 canonical 이름을 따른다.

| Scenario | File/Class 이름 |
|----------|-----------------|
| `cs-local-session-actor-echo` | `CsLocalSessionActorEchoScenario` |
| `cs-remote-session-actor-echo` | `CsRemoteSessionActorEchoScenario` |
| `s2s-channel-to-spot-request-echo` | `S2sChannelToSpotRequestEchoScenario` |
| `s2s-channel-to-spot-send-send-echo` | `S2sChannelToSpotSendSendEchoScenario` |
| `s2s-spot-to-channel-request-echo` | `S2sSpotToChannelRequestEchoScenario` |
| `s2s-spot-to-channel-send-send-echo` | `S2sSpotToChannelSendSendEchoScenario` |
| `spot-async-request-echo` | `SpotAsyncRequestEchoScenario` |
| `spot-await-contention` | `SpotAwaitContentionScenario` |
| `spot-no-await-echo` | `SpotNoAwaitEchoScenario` |
| `spot-worker-offload-echo` | `SpotWorkerOffloadEchoScenario` |
| `actor-no-bind-request-echo` | `ActorNoBindRequestEchoScenario` |
| `actor-no-bind-send-send-echo` | `ActorNoBindSendSendEchoScenario` |
| `pubsub-fanout-echo` | `PubSubFanoutEchoScenario` |

언어별 casing은 바꿀 수 있지만 단어는 바꾸지 않는다. 예를 들어 C++ 파일명은
`s2s_channel_to_spot_request_echo_scenario.cpp`처럼 쓸 수 있다.

### 8.5 client가 직접 하지 말아야 할 일

- server framework host를 같은 process에서 만들지 않는다.
- server handler를 직접 호출하지 않는다.
- server 내부 metric collector 객체를 직접 참조하지 않는다.
- channel/spot client를 사용해서 server app endpoint를 우회하지 않는다. server 간 성능 시나리오에서도
  client는 trigger 요청만 보내고, 측정 대상 server 간 호출은 server 내부 handler가 실행한다.
- 성공을 console text parsing으로 판단하지 않는다. echo reply, correlation completion, metrics endpoint
  결과로 판단한다.

## 9. runner script 작성 스타일

각 언어는 최소 두 개의 script를 둔다.

| Script | 목적 |
|--------|------|
| `run_single.sh` | 개발 중 한 scenario와 한 mode를 빠르게 실행 |
| `run_perf.sh` | 표준 scenario 전체를 순서대로 실행하고 결과를 한 run-id 아래에 모음 |

script는 PlayHouse benchmark처럼 아래 일을 명확한 단계로 출력한다.

1. release build.
2. 기존 perf server process 정리.
3. free port 예약과 OS preflight.
4. server process 시작.
5. `/perf/ready`로 readiness 확인.
6. client runner 실행.
7. 결과 파일 위치 출력.
8. server process 종료와 로그 정리.

script는 measured path를 구현하지 않는다. measured loop는 client scenario와 runner 코드에 있어야 한다.

여러 client runner를 동시에 띄우는 경우 script는 각 process에 서로 다른 `--client-index`를 넘기고,
모든 process가 같은 `--run-id`와 같은 server endpoint 목록을 사용하게 한다. port는 실행 시작 시점에
예약하고 `config.json`에 기록한다. client process가 끝난 뒤에는 `client-<index>.json`을 모두 읽어
합산 result를 만든다. 합산은 count와 byte 수처럼 더할 수 있는 값만 더하고, latency percentile은
histogram bucket을 합쳐 다시 계산한다. histogram이 없거나 bucket 상한이 서로 다르면 runner가 실패해야
한다.

## 10. 공통 시나리오

### 10.1 `cs-local-session-actor-echo`

client connector가 server의 stream session에 `request`를 보내고, session은 같은 process 안의 actor에게
echo 요청을 전달한다. actor는 받은 payload를 그대로 session으로 돌려주고, session은 같은 request의
reply로 client에게 응답한다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `SessionActorLocalServer` 1개 |
| client 수 | 기본 10,000 |
| payload | 대표 1 KiB, 함께 4 KiB |
| mode | `request` |
| 측정 단위 | client-visible echo completion |
| 비교 목적 | connector + session dispatch + local actor dispatch 비용 |

이 시나리오는 TicTacToe형 구조를 단순화한 것이다. 별도 session gateway가 없고, session과 actor가
같은 server process에 있다.

### 10.2 `cs-remote-session-actor-echo`

client connector는 session server에 `request`를 보낸다. session server는 actor server에 있는 actor로
메시지를 relay하고, actor server는 session server를 통해 같은 request의 reply를 client에게 돌려준다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `SessionServer`, `ActorServer`, 필요하면 `Registry` |
| client 수 | 기본 10,000 |
| payload | 대표 1 KiB, 함께 4 KiB |
| mode | `request` |
| 측정 단위 | client-visible echo completion |
| 비교 목적 | remote actor routing, server 간 hop, reply relay 비용 |

이 시나리오는 Bingo형 구조를 단순화한 것이다. session과 actor가 다른 server process에 있어야 한다.
같은 process로 접히면 local 시나리오와 구분할 수 없으므로 실패로 본다.

### 10.3 `s2s-channel-to-spot-request-echo`

channel server가 remote Spot server의 Spot에 request를 보내고 reply를 받는다. client는 benchmark
trigger만 보내며, 측정 대상은 server 간 channel → Spot request/reply이다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `ChannelServer`, `SpotServer`, 필요하면 `Registry` |
| payload | 대표 4 KiB, 함께 1 KiB |
| mode | `request` |
| 측정 단위 | server 간 echo completion |
| 비교 목적 | channel에서 remote Spot request/reply 비용 |

### 10.4 `s2s-channel-to-spot-send-send-echo`

channel server가 remote Spot server에 send로 echo 요청을 보내고, Spot server가 channel server로 send
응답을 보낸다. request/reply correlation을 framework가 제공하지 않는 언어에서는 payload에
`correlationId`를 넣고, benchmark harness가 완료 수를 집계한다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `ChannelServer`, `SpotServer`, 필요하면 `Registry` |
| payload | 대표 4 KiB, 함께 1 KiB |
| mode | `send-send` |
| 측정 단위 | correlation 완료 수 |
| 비교 목적 | request/reply 없이 양방향 send를 사용할 때 최대 처리량과 누락률 |

### 10.5 `s2s-spot-to-channel-request-echo`

Spot server가 channel server에 request를 보내고 reply를 받는다. trigger는 Spot server에 들어가지만,
측정 대상은 Spot → channel request/reply이다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `SpotServer`, `ChannelServer`, 필요하면 `Registry` |
| payload | 대표 4 KiB, 함께 1 KiB |
| mode | `request` |
| 측정 단위 | server 간 echo completion |
| 비교 목적 | Spot handler 내부 outbound channel request 비용 |

### 10.6 `s2s-spot-to-channel-send-send-echo`

Spot server가 channel server에 send로 echo 요청을 보내고, channel server가 Spot server로 send 응답을
보낸다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `SpotServer`, `ChannelServer`, 필요하면 `Registry` |
| payload | 대표 4 KiB, 함께 1 KiB |
| mode | `send-send` |
| 측정 단위 | correlation 완료 수 |
| 비교 목적 | Spot에서 channel로 양방향 send를 사용할 때 최대 처리량과 누락률 |

### 10.7 `spot-async-request-echo`

client는 Spot server에 trigger 요청을 보낸다. Spot handler는 remote echo channel server에 request를
보내고 단일 `Async` terminator로 reply를 기다린 뒤 client-visible completion을 기록한다. framework는
대기 동안 Spot turn을 자동으로 반납하고 reply가 도착하면 원래 dispatcher 문맥에서 continuation을
재개한다. 이 시나리오는 이 자동 관리 경로의 비용과 queue 진행성을 측정한다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `SpotServer`, `RemoteEchoServer`, 필요하면 `Registry` |
| payload | 대표 1 KiB, 함께 4 KiB |
| mode | `async-request` |
| Spot 배치 | `--spot-count`개 Spot RID로 요청 분산 |
| 측정 단위 | Spot handler echo completion |
| 비교 목적 | Spot handler 내부 remote request 대기의 자동 turn 관리 비용과 queue 진행성 |

### 10.8 `spot-await-contention`

하나의 Spot RID에 많은 요청을 집중시킨다. handler는 remote echo channel server에 request를 보내고
단일 `Async` terminator로 대기한다. 같은 조건에서 여러 Spot RID로 분산하는
`spot-async-request-echo` 결과와 비교해 자동 turn 관리가 단일 mailbox의 head-of-line blocking을
얼마나 줄이는지 확인한다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `SpotServer`, `RemoteEchoServer`, 필요하면 `Registry` |
| payload | 대표 1 KiB, 함께 4 KiB |
| mode | `async-request` |
| Spot 배치 | `--spot-count 1` 고정 |
| 측정 단위 | 단일 Spot RID echo completion |
| 비교 목적 | 단일 Spot에 요청이 몰릴 때 자동 turn 관리가 queue 진행을 유지하는지 측정 |

이 시나리오는 `--spot-count 1`로 고정해서 여러 Spot RID로 부하를 분산하지 않는다. 여러 Spot으로
분산하면 자동 turn 관리의 queue 진행성 효과와 owner 분산 효과가 섞이기 때문이다. 비교할 때는
payload, in-flight, remote echo server를 같게 유지하고 Spot RID 개수만 다르게 한다.

### 10.9 `spot-no-await-echo`

Spot handler가 remote request를 호출하지 않고 payload를 바로 echo한다. 이 baseline은 Spot dispatch와
payload 검증 비용만 보기 위한 기준이다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `SpotServer` |
| payload | 대표 1 KiB, 함께 4 KiB |
| mode | `no-await` |
| 측정 단위 | Spot handler echo completion |
| 비교 목적 | 자동 turn 대기 경로와 비교하기 위한 Spot dispatch baseline |

Spot execution 시나리오의 handler는 공유 mutable state를 request 전후에 이어서 판단하지 않는다.
자동 turn 관리 중 다른 job이 진행될 수 있으므로, admission I/O나 단순 echo처럼 request 전후
공유 상태를 이어서 판단하지 않는 흐름만 측정한다.

### 10.10 `spot-worker-offload-echo`

client가 Spot server에 trigger 요청을 보낸다. Spot handler는 `runCpuWorker(...)`(언어별
`RunCpuWorker`/`runCpuWorker`/`run_cpu_worker`)로 고정 비용 CPU 작업을 framework worker pool에 맡기고,
단일 완료 terminator로 기다린 뒤 client-visible completion을 기록한다. framework는 worker 완료를
기다리는 동안 Spot turn을 자동으로 반납한다. `RemoteEcho`
서버는 필요 없다 — 이 축은 remote I/O가 아니라 local worker pool로의 offload 비용을 잰다. worker
작업 자체는 `--worker-task-millis`로 고정한 busy-wait 또는 sleep이며, 언어별로 임의의 CPU 작업을
넣지 않는다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `SpotServer` |
| payload | 대표 1 KiB, 함께 4 KiB |
| mode | `worker-offload` |
| worker 설정 | `--worker-task-millis`, `--worker-pool-size` |
| 측정 단위 | Spot handler echo completion |
| 비교 목적 | worker pool 완료 대기의 자동 turn 관리와 worker→Spot continuation 재개 비용 |
| 실패 분류 | `WorkerQueueFull`, `WorkerTimeout`을 `errors.byKind`에 구분 기록 |

이 시나리오는 `config-8-execution-turn.ko.md`의 TD-C3/TD-C4가 검증하는 worker offload 대기
경로를 같은 조건에서 측정한다. 여기서 재는 `runCpuWorker(...)`는 같은 프로세스 안의 Spot 전용
worker thread pool offload다. 여러 프로세스에 작업을 분산하는 별도 공개 계약을 뜻하지 않는다.

### 10.11 `actor-no-bind-request-echo`

session이 없는 `ActorCaller` server가 actor client의 `RequestToActor` 호출로 미리 얻은 `ActorRef`에
request를 보내고 reply를 받는다. client는 benchmark trigger만 `ActorCaller`에 보내며, 측정
대상은 server 간 actor client no-bind request/reply다. `RequestToActor`의 await 완료는 handler reply
도착을 뜻한다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `ActorCaller`, `Actor`, location store(Redis) |
| payload | 대표 4 KiB, 함께 1 KiB |
| mode | `request` |
| 측정 단위 | server 간 echo completion |
| 비교 목적 | session bind 없이 `ActorRef`로 request할 때 no-bind 전달 비용 |
| 실패 분류 | `ActorRouteNotFound`, `ActorLocationStale`, `RouteNotConnected`를 `errors.byKind`에 구분 기록 |

이 시나리오는 언제나 bind되지 않은 actor를 대상으로 한다. `config-9-to-actor-messaging.ko.md`의 bind
상태 매트릭스(TA-A1~A4)는 기능 검증이 목적이므로 perf에서 다시 만들지 않는다.

### 10.12 `actor-no-bind-send-send-echo`

`ActorCaller` server가 `SendToActor`로 `ActorRef`에 echo 요청을 보내고, actor handler가 같은
`ActorCaller`로 send 응답을 보낸다. `SendToActor` 자체의 await 완료(resolve 성공 + 로컬 mailbox
인계)와 실제 echo 왕복(correlation 완료)은 서로 다른 시점이므로 둘을 분리해서 기록한다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `ActorCaller`, `Actor`, location store(Redis) |
| payload | 대표 4 KiB, 함께 1 KiB |
| mode | `send-send` |
| 측정 단위 | correlation 완료 수 (로컬 인계 latency는 별도 기록) |
| 비교 목적 | session bind 없이 양방향 send로 actor에 접근할 때 최대 처리량과 로컬 인계 비용 |
| 실패 분류 | `ActorRouteNotFound`, `ActorLocationStale`, `RouteNotConnected`를 `errors.byKind`에 구분 기록 |

### 10.13 `pubsub-fanout-echo`

`Publisher` server가 고정된 하나의 topic으로 이벤트를 연속 발행하고, `--subscriber-count`개의
`Subscriber` server가 같은 이벤트를 받는다. client는 benchmark trigger만 `Publisher`에 보내며,
측정 대상은 publisher → 다수 subscriber fanout이다. `Publish(...).Async()`는 remote 수신을
보장하지 않으므로, 완료 기준은 client-visible echo completion이 아니라 아래 표의 subscriber별
delivery로 정의한다.

| 항목 | 값 |
|------|----|
| 서버 구성 | `Publisher`, `Subscriber` × `--subscriber-count`, location store(Redis) |
| payload | 대표 1 KiB, 함께 4 KiB |
| mode | `publish` |
| 측정 단위 | subscriber별 수신 event 수, `fanout.deliveryRatio`, delivery latency |
| 비교 목적 | fanout 폭(subscriber 수)에 따른 publish 처리량과 subscriber별 delivery latency 변화 |
| 실패 분류 | 수신 누락은 error가 아니라 `fanout.deliveryRatio` 하락으로 관측한다. transport 오류만 `errors.byKind`에 기록한다 |

이 시나리오는 topic 필터 기능(`config-3-pubsub.ko.md` PS-A2가 다룬다), late subscriber 합류,
publisher/subscriber 재시작 같은 동적 이벤트를 재현하지 않는다. 이런 기능 검증은 e2e Config 3의 몫이고,
perf는 이미 구독이 완료된 정상 상태에서 fanout 처리량만 측정한다. warmup phase에서 모든 subscriber가
최소 한 건을 수신할 때까지 기다린 뒤에만 measured phase로 넘어간다.

## 11. Baseline 시나리오

통합 시나리오만 있으면 결과를 해석하기 어렵다. 각 언어는 아래 baseline을 가능하면 함께 둔다.
baseline은 release gate의 필수 항목은 아니지만, 병목 분석 때 먼저 실행한다.

| Baseline | 목적 |
|----------|------|
| `connector-echo-only` | session/actor 없이 connector dispatch와 codec 비용만 측정 |
| `session-echo-only` | actor 없이 session handler가 바로 echo할 때 비용 측정 |
| `channel-echo-only` | Spot 없이 channel request/send 비용 측정 |
| `spot-local-echo` | remote hop 없이 Spot dispatch 비용 측정 |
| `spot-no-await-echo` | remote request 없이 Spot handler dispatch 비용 측정 |

baseline이 아직 없는 언어는 통합 시나리오 결과를 해석할 때 "병목 위치 미확정"으로 기록한다.

## 12. 메시지 계약

언어별 구현은 같은 의미의 메시지 계약을 사용한다. 이름은 언어별 casing을 따르되, 필드 의미는
바꾸지 않는다.

| 메시지 | 필드 | 의미 |
|--------|------|------|
| `PerfEchoRequest` | `runId`, `clientId`, `sequence`, `correlationId`, `sentTicks`, `payload` | echo 요청 |
| `PerfEchoReply` | `runId`, `clientId`, `sequence`, `correlationId`, `receivedTicks`, `payload` | echo 응답 |
| `PerfTriggerRequest` | `runId`, `batchSize`, `mode`, `payloadSize`, `payload` | server 간 echo batch 시작 |
| `PerfTriggerReply` | `accepted`, `completed`, `failed`, `message` | trigger 처리 결과 |
| `PerfPublishEvent` | `runId`, `sequence`, `topic`, `sentTicks`, `payload` | publisher가 발행하는 fanout 이벤트 |
| `PerfMetricsSnapshot` | 아래 §14 메트릭 필드 | client/server metric 조회 결과 |

`payload`는 수신 측에서 크기와 일부 byte pattern을 확인한다. echo 테스트에서는 payload를 그대로
돌려보내야 한다. payload를 새로 만들거나 압축하거나 일부만 돌려보내면 측정 의미가 달라지므로 실패로
본다.

`PerfPublishEvent`는 `PerfEchoRequest`와 달리 `clientId`/`correlationId`가 없다. 발행은 특정 수신자를
겨냥하지 않으므로 clientId 개념이 없고, reply가 없으므로 correlation도 없다. 대신 `sequence`가
subscriber별로 연속 수신 여부를 검증하는 유일한 키다. `topic`은 `pubsub-fanout-echo`에서 항상 같은
고정값을 쓴다. topic별 분기는 perf 범위가 아니다.

## 13. Request 방식과 Send/Send 방식의 공정성

`request`와 `send-send`는 의미가 다르므로 같은 숫자만 비교하면 안 된다.

- `request`는 호출자가 reply를 기다린다. framework request timeout과 cancellation 정책이 적용된다.
- `send-send`는 두 개의 단방향 메시지로 echo를 만든다. benchmark harness가 `correlationId`로 완료를
  세고 timeout을 따로 관리한다.
- 두 방식 모두 전체 in-flight 상한을 둔다. `send-send`가 무제한으로 밀어 넣으면 request와 비교할 수
  없다.
- `send-send` 결과에는 `sent`, `completed`, `expired`, `duplicateReply`, `unknownCorrelation`을
  반드시 기록한다.

`send-send` latency는 `correlationId`를 등록한 시각부터 reply가 도착한 시각까지로 계산한다. reply가
없는 메시지는 latency percentile 계산에 넣지 않고, timeout/error count에 넣는다.

## 14. 메트릭

모든 언어는 결과 파일에 같은 metric key를 쓴다.

| Metric | 단위 | 설명 |
|--------|------|------|
| `connections.requested` | count | 요청한 전체 connector 수 |
| `connections.connected` | count | measured phase 전 준비된 connector 수 |
| `connections.failed` | count | 연결 또는 인증 실패 수 |
| `messages.sent` | count | measured phase 동안 보낸 요청 또는 send 수 |
| `messages.completed` | count | echo 완료 수 |
| `messages.failed` | count | 실패 응답 또는 handler 오류 수 |
| `messages.timeout` | count | request timeout 또는 send/send correlation timeout |
| `throughput.kops` | ops/sec / 1000 | echo completion 기준 KOPS |
| `throughput.messagesPerSec` | msg/sec | 실제 전송 message 기준 처리량 |
| `throughput.megabytesPerSec` | MiB/sec | payload byte 기준 처리량 |
| `latency.meanMs` | ms | 평균 latency |
| `latency.p50Ms` | ms | p50 latency |
| `latency.p95Ms` | ms | p95 latency |
| `latency.p99Ms` | ms | p99 latency |
| `latency.maxMs` | ms | 최대 latency |
| `process.cpuPercent` | percent | process CPU 사용률 |
| `process.rssMb` | MiB | resident memory |
| `process.allocatedMb` | MiB | runtime이 제공할 수 있으면 allocated bytes |
| `gc.gen0`, `gc.gen1`, `gc.gen2` | count | GC 횟수. GC가 없는 언어는 `null` |
| `errors.byKind` | object | timeout, decode, route, connection, handler 등 오류 분류. `actor-no-bind-*` 시나리오는 `ActorRouteNotFound`, `ActorLocationStale`, `RouteNotConnected`도 별도 key로 기록 |
| `actor.localHandoff.latency.p95Ms` | ms | `SendToActor` await 완료(로컬 mailbox 인계)까지 걸린 시간 p95 |
| `actor.localHandoff.latency.p99Ms` | ms | `SendToActor` await 완료(로컬 mailbox 인계)까지 걸린 시간 p99 |
| `spot.mailboxDepth.max` | count | 측정 중 관측된 Spot mailbox 최대 depth |
| `spot.mailboxDepth.mean` | count | 측정 중 관측된 Spot mailbox 평균 depth |
| `spot.suspendedTurns` | count | framework가 비동기 완료를 기다리며 자동으로 반납한 Spot turn 수 |
| `spot.resumedTurns` | count | 완료 뒤 원래 dispatcher 문맥에서 재개된 Spot turn 수 |
| `spot.resumeLatency.p95Ms` | ms | reply 수신 가능 시점부터 coroutine 재개까지 p95 |
| `spot.resumeLatency.p99Ms` | ms | reply 수신 가능 시점부터 coroutine 재개까지 p99 |
| `spot.remoteRequestRtt.p95Ms` | ms | Spot handler가 호출한 remote request RTT p95 |
| `spot.remoteRequestRtt.p99Ms` | ms | Spot handler가 호출한 remote request RTT p99 |
| `worker.pool.queueDepth.max` | count | 측정 중 관측된 worker pool 대기열 최대 depth |
| `worker.pool.queueDepth.mean` | count | 측정 중 관측된 worker pool 대기열 평균 depth |
| `worker.pool.queueWaitLatency.p95Ms` | ms | `runCpuWorker(...)` 제출부터 worker thread가 집어들 때까지 p95 |
| `worker.pool.queueWaitLatency.p99Ms` | ms | `runCpuWorker(...)` 제출부터 worker thread가 집어들 때까지 p99 |
| `worker.taskLatency.p95Ms` | ms | worker thread에서 작업 실행(`--worker-task-millis`) 자체 소요 p95 |
| `worker.taskLatency.p99Ms` | ms | worker thread에서 작업 실행(`--worker-task-millis`) 자체 소요 p99 |
| `worker.resumeLatency.p95Ms` | ms | worker 작업 완료부터 원래 Spot mailbox continuation 재개까지 p95 |
| `worker.resumeLatency.p99Ms` | ms | worker 작업 완료부터 원래 Spot mailbox continuation 재개까지 p99 |
| `messages.published` | count | publisher가 measured phase 동안 발행한 event 수 |
| `fanout.subscriberCount` | count | 이 실행에 참여한 subscriber process 수 |
| `fanout.deliveryRatio` | ratio(0~1) | subscriber별 (수신한 고유 sequence 수 / `messages.published`) 중 최솟값 |
| `fanout.deliveryLatency.p95Ms` | ms | publish 시각부터 subscriber 수신 시각까지 p95 |
| `fanout.deliveryLatency.p99Ms` | ms | publish 시각부터 subscriber 수신 시각까지 p99 |

KOPS는 `messages.completed / measuredSeconds / 1000`으로 계산한다. request/reply echo는 내부적으로
여러 message를 만들 수 있으므로, KOPS와 `messagesPerSec`를 구분해서 기록한다.
Spot 관련 metric, `actor.*` metric, `fanout.*` metric, `worker.*` metric은 해당 시나리오가 지원하지 않으면 `null`로
둔다. 값을 임의로 `0`으로 채우면 지원하지 않는 것과 실제 0을 구분할 수 없다.
`pubsub-fanout-echo`의 `messages.completed`는 client-visible echo가 없으므로 항상 `null`이고,
대신 `fanout.deliveryRatio`와 `messages.published`로 성공을 판단한다.

latency percentile을 여러 client process에서 합산하려면 histogram bucket이 필요하다. 모든 client와
server snapshot은 아래 형식의 histogram을 함께 기록한다.

```json
{
  "latencyMs": {
    "unit": "ms",
    "bounds": [0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024],
    "counts": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
    "overflow": 0
  }
}
```

`bounds`는 bucket의 상한이고 `counts`는 같은 index의 bucket count다. `counts` 길이는 `bounds` 길이와
같아야 한다. 상한보다 큰 값은 `overflow`에 넣는다. 여러 client process 결과를 합칠 때는 같은
`bounds`를 사용하는 histogram만 합산한다. bounds가 다르면 runner가 실패해야 한다.

## 15. 결과 파일 형식

각 실행은 `perf-results/<run-id>/` 아래에 결과를 남긴다.

```text
perf-results/<run-id>/
|-- config.json
|-- result.json
|-- summary.txt
|-- client-<index>.json
|-- server-<role>.json
`-- logs/
    |-- client-<index>.log
    |-- server-<role>.log
    `-- message-flow-<role>.log
```

`result.json`은 machine-readable 결과다. 최소 필드는 아래와 같다.

```json
{
  "runId": "20260626-123000",
  "language": "dotnet",
  "scenario": "cs-local-session-actor-echo",
  "mode": "request",
  "payloadSize": 1024,
  "connections": 10000,
  "clientCount": 1,
  "durationSeconds": 30,
  "warmupSeconds": 5,
  "measuredSeconds": 30.002,
  "metrics": {
    "connections.requested": 10000,
    "connections.connected": 10000,
    "messages.sent": 120000,
    "messages.completed": 120000,
    "throughput.kops": 0.0,
    "latency.p99Ms": 0.0
  },
  "histograms": {
    "latencyMs": {
      "unit": "ms",
      "bounds": [0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024],
      "counts": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
      "overflow": 0
    }
  },
  "clients": [
    {
      "clientIndex": 0,
      "clientIdStart": 0,
      "clientIdEndExclusive": 10000,
      "metricsFile": "client-0.json"
    }
  ],
  "servers": {
    "sessionActorLocal": "server-sessionActorLocal.json"
  }
}
```

`client-<index>.json`은 한 client process의 원본 결과다. 최소 필드는 아래와 같다.

```json
{
  "runId": "20260626-123000",
  "clientIndex": 0,
  "clientCount": 4,
  "clientIdStart": 0,
  "clientIdEndExclusive": 2500,
  "warmupConnections": 2500,
  "metrics": {
    "connections.requested": 2500,
    "connections.connected": 2500,
    "messages.sent": 30000,
    "messages.completed": 30000
  },
  "histograms": {
    "latencyMs": {
      "unit": "ms",
      "bounds": [0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024],
      "counts": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
      "overflow": 0
    }
  }
}
```

`result.json`은 `client-<index>.json`의 count, byte, error metric을 합산하고, histogram을 합산한 뒤
percentile을 다시 계산한 값이다. `latency.p95Ms` 같은 percentile 값을 process별 percentile의 평균으로
계산하면 안 된다.

`summary.txt`는 사람이 읽는 결과다. 이 파일에는 설정, throughput, latency percentile, 실패 수,
client/server CPU와 memory를 한 화면에 보이게 적는다.

## 16. Server metrics endpoint

각 server role은 성능 테스트 전용 metrics endpoint를 제공한다. transport는 HTTP를 기본으로 한다.
HTTP server를 붙이기 어려운 언어는 별도 admin channel을 둘 수 있지만, runner가 같은 의미로
`reset`과 `snapshot`을 호출할 수 있어야 한다.

| Endpoint | 의미 |
|----------|------|
| `POST /perf/reset` | server metric을 리셋한다 |
| `GET /perf/stats` | 현재 server metric snapshot을 반환한다 |
| `GET /perf/ready` | server가 benchmark 요청을 받을 준비가 되었는지 반환한다 |

`POST /perf/reset` request와 response는 아래 형식을 따른다.

```json
{
  "runId": "20260626-123000",
  "scenario": "cs-local-session-actor-echo",
  "mode": "request",
  "payloadSize": 1024,
  "resetSeq": 1,
  "resetAtUnixMs": 1782466200000
}
```

```json
{
  "ok": true,
  "role": "sessionActorLocal",
  "runId": "20260626-123000",
  "resetSeq": 1,
  "resetAtUnixMs": 1782466200000
}
```

`GET /perf/ready` response는 아래 형식을 따른다.

```json
{
  "ready": true,
  "role": "sessionActorLocal",
  "runId": "20260626-123000",
  "endpoints": {
    "appEndpoint": "tcp://127.0.0.1:21001",
    "metricsUrl": "http://127.0.0.1:31001"
  },
  "message": ""
}
```

`GET /perf/stats` response는 아래 형식을 따른다.

```json
{
  "role": "sessionActorLocal",
  "runId": "20260626-123000",
  "scenario": "cs-local-session-actor-echo",
  "mode": "request",
  "window": {
    "startedAtUnixMs": 1782466205000,
    "endedAtUnixMs": 1782466235002,
    "measuredSeconds": 30.002
  },
  "metrics": {
    "messages.completed": 120000,
    "process.cpuPercent": 240.5
  },
  "histograms": {
    "latencyMs": {
      "unit": "ms",
      "bounds": [0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024],
      "counts": [0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0],
      "overflow": 0
    }
  }
}
```

metrics endpoint는 measured path에 끼면 안 된다. echo handler가 매 메시지마다 HTTP metric endpoint를
호출하거나 lock 경쟁을 크게 만들면 측정이 왜곡된다. hot path에서는 atomic counter와 저비용 latency
recorder만 사용한다.

## 17. 언어별 표준 위치

언어별 perf 코드는 기본적으로 `framework/languages/<lang>/perf/` 아래에 둔다. Kotlin은 Java runtime과
build root를 공유하므로 이 장의 Kotlin 위치를 따른다.

### 17.1 .NET

```text
framework/languages/dotnet/perf/
|-- ZLink.Framework.Perf.Shared/
|-- ZLink.Framework.Perf.Client/
|-- ZLink.Framework.Perf.SessionActorLocalServer/
|-- ZLink.Framework.Perf.SessionServer/
|-- ZLink.Framework.Perf.ActorServer/
|-- ZLink.Framework.Perf.ChannelServer/
|-- ZLink.Framework.Perf.SpotServer/
|-- ZLink.Framework.Perf.RemoteEchoServer/
`-- scripts/
    |-- run_perf.sh
    |-- run_single.sh
    `-- collect_env.sh
```

.NET 구현은 Release build를 기본으로 하고, server metrics에는 `GC.CollectionCount`,
`GC.GetTotalAllocatedBytes`, process RSS, process CPU를 포함한다.

### 17.2 Java

```text
framework/languages/java/perf/
|-- shared/
|-- client/
|-- session-actor-local-server/
|-- session-server/
|-- actor-server/
|-- channel-server/
|-- spot-server/
|-- remote-echo-server/
`-- scripts/
    |-- run_perf.sh
    |-- run_single.sh
    `-- collect_env.sh
```

Java 구현은 Gradle standalone runner를 제공한다. server metrics에는 JVM GC count, heap/non-heap 사용량,
process CPU를 포함한다.

### 17.3 Kotlin

Kotlin은 Java framework와 같은 build root를 공유하므로 표준 위치를
`framework/languages/java/perf/kotlin/`으로 고정한다. Java와 같은 scenario 이름과 result schema를
써야 한다.

```text
framework/languages/java/perf/kotlin/
|-- shared/
|-- client/
|-- session-actor-local-server/
|-- session-server/
|-- actor-server/
|-- channel-server/
|-- spot-server/
|-- remote-echo-server/
`-- scripts/
    |-- run_perf.sh
    |-- run_single.sh
    `-- collect_env.sh
```

### 17.4 Node.js

```text
framework/languages/node/perf/
|-- shared/
|-- client/
|-- session-actor-local-server/
|-- session-server/
|-- actor-server/
|-- channel-server/
|-- spot-server/
|-- remote-echo-server/
`-- scripts/
    |-- run_perf.sh
    |-- run_single.sh
    `-- collect_env.sh
```

Node.js 구현은 TypeScript source와 build output을 분리한다. metrics에는 event loop delay, RSS, heap used,
process CPU를 포함한다.

### 17.5 C++

```text
framework/languages/cpp/perf/
|-- shared/
|-- client/
|-- session_actor_local_server/
|-- session_server/
|-- actor_server/
|-- channel_server/
|-- spot_server/
|-- remote_echo_server/
`-- scripts/
    |-- run_perf.sh
    |-- run_single.sh
    `-- collect_env.sh
```

C++ 구현은 release build 산출물을 사용한다. core runtime 또는 bindings runtime 경로가 source보다
오래되면 runner가 실패해야 한다. 성능 수치를 오래된 runtime으로 해석하면 안 된다.

## 18. 구현 순서

모든 언어는 한 번에 최종 시나리오까지 구현하되, 작업 순서는 아래를 따른다. 순서는 디버깅 비용을
줄이기 위한 것이며, 기능을 나누어 릴리스하라는 뜻이 아니다.

1. 공통 message 계약과 result schema를 만든다.
2. metrics collector와 `POST /perf/reset`, `GET /perf/stats`, `GET /perf/ready`를 만든다.
3. client runner의 connect, warmup, measured, report phase를 만든다.
4. `connector-echo-only`와 `session-echo-only` baseline으로 runner와 metric이 맞는지 확인한다.
5. `cs-local-session-actor-echo`를 구현한다.
6. `cs-remote-session-actor-echo`를 구현한다.
7. `channel-echo-only`와 `spot-local-echo` baseline을 구현한다.
8. channel → Spot request, channel → Spot send/send를 구현한다.
9. Spot → channel request, Spot → channel send/send를 구현한다.
10. `spot-no-await-echo`, `spot-async-request-echo`를 구현한다.
11. `spot-await-contention`으로 단일 Spot RID 집중 부하를 구현한다.
12. `spot-worker-offload-echo`를 구현한다. `RemoteEcho`
    서버 없이 `SpotServer`만으로 `runCpuWorker(...)` 완료를 기다리는 경로이므로 10~11단계와 독립적으로
    진행할 수 있다.
13. actor client를 제공하는 모든 framework 언어는 `actor-no-bind-request-echo`,
    `actor-no-bind-send-send-echo`를 구현한다.
14. `pubsub-fanout-echo`를 구현한다. `Publisher`/`Subscriber` role과 `--subscriber-count` 분할을
    먼저 만들고, warmup의 "모든 subscriber 최초 수신 확인" barrier부터 맞춘다.
15. `run_perf.sh`가 모든 표준 시나리오를 실행하고 결과를 한 디렉토리에 모으게 한다.
16. Codex 에이전트로 문서, scenario 이름, result schema, public API 사용 여부를 리뷰한다.
17. 남은 이슈가 없을 때까지 수정과 리뷰를 반복하고, 마지막 리뷰가 `LOOP CLEAN`이면 완료로 본다.

## 19. 회귀와 비교 기준

성능 수치는 환경 영향을 크게 받으므로, 처음부터 고정 threshold로 실패시키지 않는다. 대신 아래 기준을
결과에 기록한다.

- git commit hash
- core/bindings/framework build mode와 runtime path
- CPU model, core 수, memory
- OS, kernel, container 여부
- `ulimit -n`
- client process 수와 client별 connector 수
- payload size(`1024` 또는 `4096`), duration, warmup, in-flight
- server role별 process id와 endpoint

회귀 판단은 같은 장비, 같은 설정, 같은 scenario의 baseline과 비교한다. release gate에 넣을 때는
처리량 하락률, p99 증가율, timeout/error 증가율을 함께 본다. 한 지표만으로 실패시키지 않는다.

## 20. 운영체제와 실행 환경

10,000 connector 테스트는 OS 설정에 민감하다. runner는 실행 전에 아래 항목을 확인하고, 부족하면
명확한 오류를 출력한다.

- file descriptor limit
- ephemeral port range
- listen backlog
- TCP TIME_WAIT 재사용 정책
- client runner CPU 포화 여부
- server와 client가 같은 host인지 다른 host인지

같은 host에서 client와 server를 함께 실행하면 loopback 성능과 CPU 경쟁이 섞인다. 이 모드는 개발과
회귀 추적에는 유용하지만, 네트워크 성능 수치로 일반화하지 않는다. 실제 한계치를 볼 때는 client
runner를 다른 host 또는 여러 host에 분산한다.

## 21. 금지 사항

- framework 내부 runtime API, private API, reflection 우회로 측정 경로를 만들지 않는다.
- sample 또는 e2e 코드를 복사해서 도메인 규칙이 많은 benchmark server를 만들지 않는다.
- measured phase 안에서 로그를 매 메시지마다 남기지 않는다. 오류와 집계 metric만 남긴다.
- readiness를 고정 sleep으로만 판단하지 않는다.
- request 방식과 send/send 방식을 in-flight 제한 없이 비교하지 않는다.
- 자동 turn 시나리오와 baseline을 서로 다른 remote echo server, payload, in-flight 조건에서 비교하지 않는다.
- worker offload 조건을 실행마다 다른 `--worker-task-millis`나 `--worker-pool-size`로 바꾸지 않는다.
- 자동 turn 성능 시나리오에서 request 전후 공유 mutable state를 이어서 판단하는 업무 로직을 넣지 않는다.
- payload 검증을 생략하지 않는다. 잘못된 echo가 빠르게 성공한 것처럼 보이면 결과가 무의미하다.
- 실패한 메시지를 latency percentile에 섞지 않는다. 실패는 별도 error metric으로 기록한다.
- 오래된 build 산출물이나 debug build 결과를 release 성능 수치로 기록하지 않는다.

## 22. 완료 기준

언어별 perf 구현은 아래 조건을 만족해야 완료로 본다.

- 표준 scenario 이름을 모두 지원한다.
- 각 scenario가 `1 KiB`와 `4 KiB` payload, 기본 connection 수를 따른다.
- client/server metrics가 공통 result schema로 저장된다.
- `request`와 `send-send`가 같은 in-flight 기준으로 실행된다.
- Spot 자동 turn 시나리오와 baseline이 같은 remote echo server, payload, in-flight 기준으로 실행된다.
- Spot worker offload 시나리오가 고정된 `--worker-task-millis`, `--worker-pool-size`, payload,
  in-flight 기준으로 실행된다.
- `pubsub-fanout-echo`는 모든 `Subscriber` process의 metrics가 개별 파일로 수집되고, `fanout.deliveryRatio`가
  결과에 기록된다.
- server metrics endpoint가 warmup 후 reset되고 measured phase 후 snapshot된다.
- 실패와 timeout이 0이 아니면 summary에 원인별 count가 나온다.
- `run_perf.sh`가 모든 표준 시나리오를 실행할 수 있다.
- Codex 에이전트 리뷰를 반복해서 남은 이슈가 없음을 확인한다.

## 23. Application HWM을 production workload로 결정하는 방법

이 절은 host 전체에서 수신 후 handler가 완료되지 않은 application payload의 HWM을 production
workload로 결정하는 공통 측정 규격이다. HWM의 설정 mode와 Auto 계산 계약은
[Framework API 「2.1 수신 payload가 memory를 계속 늘리지 않게 한다」](../spec/06-framework-api.ko.md)를
기준으로 한다. 이 절은 application별 양수 HWM을 선택하고 Auto HWM profile을 검증하는 실행
방법만 정한다.

### 23.1 먼저 고정할 조건

Production과 같은 CPU quota, process memory limit, connection 수, dispatch concurrency,
runtime·GC option을 사용한다. Job workload는 다음 특성을 함께 고정한다.

- Request와 one-way job의 비율
- Application payload 크기 분포
- CPU-bound와 I/O-bound handler의 비율
- Handler가 만드는 reply 크기와 nested request 비율
- Burst ingress rate와 지속 시간

HWM은 CPU 사용률을 제한하지 않는다. 목표가 할당된 CPU quota의 `50%`라면 dispatch
concurrency나 별도 rate limit을 먼저 조정한다. Backlog가 지속되는 동안 정규화한 process CPU가
`45~55%` 범위에 있는 실행만 목표 capacity 측정값으로 사용한다.

### 23.2 처리량 측정

최소 `30초` warm-up 뒤 `60초` measured phase를 `5회` 반복한다. 다섯 실행의 처리량
변동계수가 `5%`를 넘으면 measured phase를 늘린다. Measurement 동안 dispatch를 기다리는 job이
계속 존재해야 한다. 유입량이 부족해 handler가 대기한 실행은 처리 capacity 결과에서 제외한다.

각 실행의 처리량은 다음처럼 계산한다.

```text
runDrainBytesPerSecond =
    terminalPayloadBytes / measuredWallClockSeconds

measuredSustainableDrainBytesPerSecond =
    minimum(runDrainBytesPerSecond across five valid runs)
```

`terminalPayloadBytes`에는 measured phase 동안 handler terminal에 도달한 원본 job의
application payload byte를 정확히 한 번 포함한다. Reply byte, ingress byte와 순간 peak
throughput은 포함하지 않는다.

### 23.3 HWM 후보 계산

운영에서 허용할 최대 queue 지연을 정하고 처리량 기준 후보를 계산한다.

```text
queueDelayCandidateBytes =
    measuredSustainableDrainBytesPerSecond
    * maximumQueueDelaySeconds

candidateApplicationHwmBytes =
    measuredPeakActiveHandlerPayloadBytes
    + queueDelayCandidateBytes
```

`measuredPeakActiveHandlerPayloadBytes`는 measured phase에서 실행 중인 handler context가 보유한
payload byte 합계의 최대값이다. Application HWM은 실행 중인 handler payload도 포함하므로 이
값을 queue 지연 후보에 더한다.

`autoProfileHwmBytes`는 할당된 application memory에 선택한 profile 비율을 곱한 값이다.
`COMPACT`는 `2%`, `LOW_LATENCY`는 `5%`, `BALANCED`는 `10%`, `THROUGHPUT`은 `20%`를
사용한다. 네 Auto profile을 비교할 때는 같은 workload로 각각 실행한다. Memory와 queue 지연
조건을 만족하는 가장 큰 profile을 선택하며, 별도 선택이 없으면 `BALANCED`를 사용한다.

Application별 production 값을 고정하려면 `candidateApplicationHwmBytes`를 양수
`ApplicationHwmBytes`로 설정한 뒤 다시 측정한다. Backlog를 HWM까지 채우고 다음 조건을 모두
확인한다.

- Peak process memory가 process limit을 넘지 않는다.
- Queue 지연 p99와 최대값이 application의 목표를 만족한다.
- 처리량과 CPU가 HWM을 끈 capacity test에서 허용한 범위 안에 있다.
- Message drop 없이 pause와 source backpressure가 발생한다.
- HWM보다 큰 최대 message 한 건도 처리 중인 application job이 없을 때 수신된다.

조건을 만족하지 않으면 HWM을 낮추고 같은 test를 반복한다. 조건을 만족한 가장 큰 값을
production 값으로 사용한다.

### 23.4 결과에 반드시 기록할 값

- CPU quota·core 수, process memory limit, runtime·GC 설정과 Framework version
- Connection 수, dispatch concurrency, request·one-way 비율과 job 크기 분포
- 실행별 terminal job count, terminal payload bytes, wall-clock 처리량과 process CPU
- Peak RSS, peak pending application payload bytes와 active handler payload bytes
- Queue 지연 p50·p95·p99·최대값, pause count·duration과 backpressure 횟수
- 선택한 Auto HWM profile과 계산에 사용한 memory 값
- Auto profile HWM, queue 지연 후보, active handler payload, production HWM과 선택 근거
