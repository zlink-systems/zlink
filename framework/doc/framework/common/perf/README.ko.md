# Framework Performance 테스트 공통 규격

[문서 목록][docs] · [공통 스펙][common] · [Scenario E2E][e2e]

이 규격은 .NET, C++, Java, Kotlin, Node.js의 perf 작성자가 같은 공개 호출 흐름에서
처리량·지연·자원 사용량을 비교하기 위한 실행 조건과 결과 형식을 정한다.
표준 scenario 이름·payload matrix·CLI·결과 형식은 이 문서가 소유한다.
언어별 계획은 구현 도구와 runtime metadata만 보완한다.

Runtime 동작은 아래 계약이 소유한다. Perf는 그 공개 호출·callback·status를 측정하며
연결 선택, 재전송, turn, completion이나 오류 계약을 추가하지 않는다.

| 측정 경계 | 계약 소유 문서 |
|---|---|
| Turn과 일반 terminal/Yield | [Submit과 completion §2][submit], [Handler turn과 execution gate §2–4][turn] |
| One-way 수락·request 완료·deadline | [Submit과 completion §4–9][submit] |
| Global ActorId 메시징 | [Actor 모델 §5][actor] |
| Global SpotId·준비 | [Spot 주소 메시징 §2–3][spot-address], [MeshNode §4][mesh] |
| STREAM callback·bind·relay | [STREAM session][session], [Session과 Actor binding §5·12][binding] |
| ChannelName과 topology | [Channel messaging][channel], [ClientServer Channel §5][clientserver] |
| Classic fanout | [Framework API의 fanout 계약][api], [Config 3의 공개 관찰][fanout] |
| Host capacity·관측 한계 | [Application job queue][queue], [Runtime monitoring §4][monitor], [Runtime metrics §3·11][metrics] |
| Public 오류 | [Framework 오류 모델][errors] |
| 물리 admission·completion drain | [Core socket][core-socket], [Binding async execution model §4][binding-async] |

## 1. 목표

성능 결과는 다음 질문에 답할 수 있어야 한다.

- 많은 STREAM 연결에서 client와 server의 처리량·지연·CPU 사용량은 어떻게 달라지는가.
- Session과 Actor가 같은 local object node에 있을 때와 별도 process에 있을 때 비용은 얼마나 다른가.
- Channel↔Spot에서 request/reply와 send/send의 완료율·처리량·tail latency는 어떻게 다른가.
- [Spot turn][g-turn]을 유지하는 일반 terminal과 반납하는 Yield를 선택했을 때,
  Spot 수에 따라 remote call 완료와 다른 callback의 진행은 어떻게 달라지는가([소유 계약][turn]).
- CPU worker의 callback 실행 시간과 제출·결과 전달 구간은 전체 완료 시간에 얼마나 기여하는가.
- 1 KiB와 4 KiB payload에서 관찰되는 차이를 client·server·codec·transport의 병목 후보와 연결할 수 있는가.

## 2. 범위

표준 scenario는 §10의 이름을 사용하고 각각 `1024`, `4096` logical payload bytes로 실행한다.
한 scenario·payload·terminal·배치 조합을 독립적으로 실행한 결과를 이 문서에서는 **측정 셀**이라고 한다.
서로 다른 셀의 처리량을 평균내어 하나의 결과로 만들지 않는다.

| 계열 | 질문의 범위 | 대표 payload |
|---|---|---:|
| CS | Connector → session → local/remote Actor echo | 1024 |
| S2S | Channel → Spot, Spot → Channel request 또는 send/send | 4096 |
| Spot local·worker | Local public Spot dispatch와 CPU offload | 1024 |
| AC | Session 없는 ActorCaller → ActorId direct message | 4096 |
| PS | Classic fanout publish와 subscriber delivery | 1024 |

주소와 상태를 가진 논리 객체인 [Spot][g-spot]은 준비된 User Spot만 사용한다.
그 객체를 지정하는 [Spot ID][g-spot-id]는 `spotIds`에 기록한다.
Object role과 Store 필요성은 [MeshNode 계약][mesh]을 따르며, Store가 필요한 셀은 §20의
전용 Docker Redis를 사용한다. ActorRef는 create 결과 확인과 session bind에만 사용한다.
측정 메시징의 target은 [Actor 모델][actor]과 [Spot 주소 계약][spot-address]에 맞춘다.

§11의 `session-echo-only`, `channel-echo-only` RouteMesh/ClientServer request 셀과
`spot-no-await-echo` local 참조는 필수 기준 측정이다. Release 성능 threshold는 §19가 다룬다.
공통 완료에 fake backend나 micro benchmark 수치를 포함하지 않는다.

### 2.1 후속 후보

| 후보 | 별도로 답할 질문 | 소유 계약 |
|---|---|---|
| Instance Spot cold/hot | 새 ID의 최초 호출과 준비된 ID의 호출 비용 | [Spot 주소 메시징][spot-address] |
| Logical Multicast | 대상 node 선택과 local Spot delivery 비용 | [Spot messaging §4][spot] |
| Relocation under load | 이동 중 완료율·p99·중단과 재개 시간 | [Host relocation][relocation] |
| .NET/C++ HTTP | HTTP JSON binding·DI·handler 왕복 비용 | [.NET HTTP][http-dotnet], [C++ HTTP][http-cpp] |

이 후보는 표준 셀·CLI accepted scenario·완료 개수에 포함하지 않는다.

## 3. 성능 테스트와 다른 테스트의 관계

| 테스트 종류 | 목적 | 실행 경계 |
|---|---|---|
| unit/contract | 공개 동작·불변 조건의 정확성 | 계약에 맞는 최소 구성 |
| sample | Application 사용 흐름 | 실제 client/server |
| E2E | 배포 조합·장애·복구 | 실제 multi-process |
| perf | 완료율·처리량·지연·자원 사용량 | 실제 role process와 public API |

- **Perf server는 독립된 echo application으로 둔다.** Sample의 업무 규칙과 E2E의 장애 주입을
  측정 경로에 섞으면 같은 공개 호출 비용을 비교할 수 없기 때문이다.
- **기능의 정확성은 소유 계약의 contract/E2E 관찰과 대조한다.** Perf 수치가 내부 동작의
  정확성을 증명하는 것은 아니기 때문이다.

## 4. 공통 실행 모델

모든 runner는 아래 순서로 셀을 실행한다. Duration 기반 실행이 표준이며 짧은 count 기반
실행은 별도 smoke 결과다.

| Phase | 수행 주체와 결과 | 처리량 포함 |
|---|---|---|
| build/preflight | Script가 release 산출물·설정·OS·port·출력 경로를 확인한다 | 제외 |
| server start | Role별 process와 필요한 Store를 시작하고 §16 readiness를 확인한다 | 제외 |
| connect/setup | CS client가 연결하고 create/bind를 끝내며 server-driven caller는 객체·consumer를 준비한다 | 별도 setup 수치 |
| warmup | 실제 셀의 모든 connector 또는 logical stream으로 같은 호출을 실행한다 | 제외 |
| reset | 제출 정지·잔여 작업 종료 뒤 모든 참여자의 같은 resetSeq 응답을 확인한다 | 제외 |
| measured | 시작 barrier를 통과한 owner가 monotonic duration 동안 부하를 실행한다 | 포함 |
| settle | 새 측정 operation 없이 남은 결과를 유한 시간 동안 관찰한다 | 별도 |
| report | Owner별 원본과 histogram을 수집해 셀 결과를 만든다 | 제외 |
| cleanup | 이번 셀이 소유한 client/server를 종료하고 run 소유 자원을 정리한다 | 제외 |

### 4.1 Window와 reset

측정 구간에 시작한 operation 집합을 **measured cohort**로 구분한다.
각 집계 owner의 구간은 monotonic clock의 `[startTicks, endTicks)`이며, 시작 시각과
실제 경과 시간은 owner별 원본에 남긴다. 여러 process의 HTTP 응답 시각이 같다고 가정하지 않는다.

- **Warmup 제출을 멈추고 그 작업이 모두 terminal이 된 뒤 reset한다.** 이전 callback이 새
  구간의 counter에 섞이지 않아야 하기 때문이다. 준비 단계의 durable lifecycle도 이 전에
  끝내며 replay·deadline 의미는 [Actor §8.1][actor]을 따른다.
- **모든 role과 CS client가 같은 resetSeq를 확인한 뒤 시작 barrier를 연다.** Reset 호출은
  process 사이에서 원자적이지 않기 때문이다. Application counter 초기화와 public
  `ResetCapacityMetrics` 호출 결과는 각각 기록한다([metric epoch 소유 계약][metrics]).
- **Measured 종료 뒤 새 operation을 시작하지 않는다.** Settle은 이 cohort의 결과 관찰
  구간이다. §13의 첫 결과를 보존하며 timeout 뒤 새 deadline이나 재시도로 결과를 바꾸지 않는다.
- **Warmup drain이나 settle bound를 넘으면 실패 원본을 남긴다.** Connection 재생성이나
  임의 sleep으로 잔여 작업을 없앤 것처럼 처리하면 같은 조건을 비교할 수 없기 때문이다.

`messages.completed`는 cohort 중 window 안에 검증까지 끝난 성공 수다.
Window가 끝난 뒤 settle 안에 끝난 성공은 `messages.settleCompleted`와 별도 histogram에 둔다.
처리량 분모에는 settle 시간을 더하지 않는다. Settle 종료 시 미완료는 `messages.unresolved`다.
이는 Framework timeout의 재분류가 아니라 harness의 관찰 종료다([완료 계약][submit]).

### 4.2 Server-driven 부하

Server process가 public call을 반복하는 독립된 부하 흐름을 **logical stream**이라고 한다.
CS의 physical connector와 단위가 다르며 S2S·AC·PS·Spot local/worker에서 사용한다.
Standalone client 하나가 `applicationTriggerUrl`로 phase 시작을 알린다. 이 HTTP 호출과
응답은 측정 operation이나 KOPS가 아니다. `/perf/*`는 부하를 시작하지 않는다.

Role config가 logical stream 수, stream당 in-flight, duration과 deadline을 소유한다.
Trigger는 `runId`, `cellId`, `resetSeq`, phase만 전달하고 그 설정을 바꾸지 않는다.
하나의 phase는 한 번만 시작하며 중복 trigger는 같은 시작 acknowledgement를 돌려준다.

Spot handler 안에서 outbound call을 재는 셀은 같은 process의 application driver가 public
Spot request/send로 handler를 실행한다. Driver는 호출 전에 stream의 in-flight 자리를 확보하고
최종 echo 결과까지 유지한다. Local driver 호출을 별도 KOPS로 세지 않는다.
§10.5의 주 latency는 handler 안 remote call 직전부터 완료까지이며, driver부터의 전체 시간은
`driver.latency.*`에 따로 기록한다. Source admission 대기는 각 구간의 public call 시작부터 포함한다.
Measured 종료 뒤 도착한 local driver 요청은 outbound call을 시작하지 않고
`PerfDriveReply.started=false`로 끝내며 `driver.notStarted`로 기록한다.

## 5. 공통 CLI

Shell runner의 옵션 이름과 consumer는 다음과 같다. 미적용 옵션을 명시하거나 scenario에 맞지
않는 mode·terminal·topology를 지정하면 preflight 오류다. 기본값은 적용되는 consumer에만 전달한다.

| 옵션 | 기본값·범위 | Consumer와 의미 |
|---|---|---|
| `--scenario` | `run_single.sh` 필수; `run_perf.sh`는 전체 | Script가 §8.4·§11의 실행 이름을 선택한다 |
| `--connections` | 10000, 양의 int32 | CS connection pool만 소비하는 전체 physical connector 수 |
| `--logical-streams` | 10000, 양의 int32 | Server-driven source의 workload loop가 소비하는 stream 수 |
| `--client-count` | 1, 양의 int32 | Script·CS 분할 계획이 소비하는 client process 수; server-driven은 1 |
| `--client-index` | 0, `0 <= index < count` | Script가 child CS client에 부여하는 분할 index; top-level 입력은 받지 않음 |
| `--duration-seconds` | 30, finite > 0 | 집계 owner의 measured window |
| `--warmup-seconds` | 5, finite > 0 | 같은 owner의 warmup loop |
| `--payload-size` | scenario 대표값; 1024 또는 4096 | `run_single.sh`와 payload factory가 소비 |
| `--payload-sizes` | `1024,4096` | `run_perf.sh` matrix 확장; 단일값 옵션과 함께 받지 않음 |
| `--inflight` | 1, 양의 int32 | CS는 connector별, server-driven은 stream별 logical operation 상한; PS는 publish admission 상한 |
| `--connect-concurrency` | 256, 양의 int32 | CS pool의 동시 connect/setup 수; server-driven에는 없음 |
| `--spot-count` | 16, 양의 int32 | Spot Object Server의 User Spot 준비와 stream→Spot mapping; §10.5 표준 matrix는 1/16 |
| `--subscriber-count` | 8, 양의 int32 | PS script의 독립 Subscriber process 수 |
| `--worker-task-millis` | 5, 양의 int32 | Worker callback의 CPU 작업 시간 목표(§10.8) |
| `--worker-pool-size` | 8, 양의 int32 | Worker host의 public MaxThreads 설정 |
| `--mode` | scenario 고정값 | Scenario dispatcher가 `request`, `send-send`, `no-await`, `worker-offload`, `publish` 중 해당 값만 수락 |
| `--terminal` | 일반은 `ordinary`, worker는 `yield` | §10.5·§10.8 handler가 `ordinary`/`yield`를 소비; 나머지는 ordinary 고정 |
| `--channel-topology` | `routemesh` | `channel-echo-only` bootstrap이 `routemesh`/`clientserver`를 소비; 나머지 S2S Channel은 RouteMesh 고정 |
| `--codec` | `json`만 수락 | Typed payload 설정 검증과 serializer metadata 기록 |
| `--output` | `perf-results/<run-id>` | Writer·script가 사용하는 run root |
| `--run-id` | UTC 표기+고유 suffix | Script가 자원·로그·결과 identity에 사용; `[A-Za-z0-9_-]+` |
| `--endpoint-config` | Script 생성 파일 | Standalone client가 읽는 실제 endpoint manifest |
| `--workload-config` | 표준 echo는 생략 | §23 script가 읽는 운영 workload manifest; 일반 셀과 구분 |
| `--config` | Role 실행 시 필수 | 해당 server executable이 시작 전에 읽는 role config 파일 하나 |

`run_perf.sh`는 각 payload에서 §10.5의 `ordinary/yield × SpotId 1/16`, §10.8의
`ordinary/yield`, §11의 `routemesh/clientserver`를 모두 실행한다. `run_single.sh`는 한 셀을
선택한다. 단일 셀 결과만으로 전체 표준 matrix 완료를 보고하지 않는다.

### 5.1 Role config와 endpoint manifest

- **Script는 server 시작 전에 role config를 만든다.** Server가 시작할 때 필요한 Store,
  listener, topology와 workload 값을 시작 뒤 생성하는 endpoint 파일에 의존할 수 없기 때문이다.
- **Role executable은 config 파일 하나를 읽는다.** Endpoint·timeout을 환경 변수로 다시
  전달하면 설정 소유자가 둘이 되기 때문이다.
- **Client용 manifest에는 실제 listener 조회값 또는 검증된 예약값을 쓴다.** Port 0으로
  시작한 listener의 입력값 `0`은 접속 주소가 아니기 때문이다([Listener identity][listener]).

Role config는 identity(`runId`, `cellId`, `role`, `roleInstance`), listener bind/advertise 입력,
Store provider·namespace·endpoint, ChannelName·topology·membership, `spotIds`/`actorIds`,
execution mode, workload와 metrics listener를 포함한다. 불필요한 object role은 등록하지 않는다.
Manifest는 같은 identity와 config hash를 가지며 `roles`는 process별 배열이다.

```json
{
  "runId": "20260906-example",
  "cellId": "s2s-channel-to-spot-request-echo-4096-example",
  "roles": [
    {
      "role": "channel",
      "roleInstance": 0,
      "streamEndpoint": null,
      "applicationTriggerUrl": "http://127.0.0.1:49152/app/perf/start",
      "metrics": {"transport": "http", "baseUrl": "http://127.0.0.1:49153"},
      "transportEndpoints": {"mesh": "tcp://127.0.0.1:49154"},
      "spotIds": [],
      "actorIds": []
    }
  ]
}
```

Port 숫자는 형식 예시이며 예약 범위나 기본값이 아니다. `streamEndpoint`는 CS connector,
`applicationTriggerUrl`은 server-driven standalone client, `metrics.baseUrl`은 admin client가
소비한다. `transportEndpoints`는 server 구성·진단의 실제 endpoint이며 trigger 주소로 쓰지 않는다.
각 server role의 `applicationTriggerUrl`은 §16의 window 시작에도 사용한다.
CS 외 role의 `streamEndpoint`는 `null`이다.
Subscriber는 `role=subscriber`, `roleInstance=subscriberId`로 각각 한 항목을 갖는다.

### 5.2 공통 workload 값

표준 echo의 role config에는 `requestTimeoutMs=1000`, `correlationExpiryMs=1000`,
`settleTimeoutMs=5000`, `setupTimeoutMs=30000`, `adminTimeoutMs=5000`을 기록한다.
앞의 request 값은 public request call, expiry는 harness correlation, settle은 phase owner,
setup은 script/준비 caller, admin 값은 HTTP client가 소비한다. Family send timeout은
public socket 설정의 실제 값(표준 1000ms)을 기록한다([설정 소유 계약][submit]).

Worker config에는 `minThreads=workerPoolSize`, `maxThreads=workerPoolSize`,
`maxQueueLength=4096`, `idleTimeoutMs=60000`, `workerTimeoutMs=requestTimeoutMs`와 executor의
실효 제한을 기록한다. 적용은 각 언어의 public worker options만 사용한다(§10.8).
일반 workload는 각 stream이 완료 뒤 다음 operation을 시작하는 closed-loop다.
Rate·burst·Core/queue profile을 바꾸는 입력은 §23 manifest만 소유한다.

## 6. 표준 프로젝트 구조

- **Perf는 sample/E2E와 독립된 실행 프로젝트에 둔다.** Echo의 공개 호출과 측정 비용을
  업무 코드 없이 검토할 수 있어야 하기 때문이다.
- **측정 loop는 그 호출을 실행하는 process가 소유한다.** CS는 client, server-driven 셀은
  source server가 소유하므로 trigger client에 server 처리량을 복제하지 않는다.

```text
framework/languages/<lang>/perf/
|-- Shared/
|   |-- Contracts/
|   `-- Payload/
|-- Client/
|   |-- Program.*
|   |-- Scenarios/
|   `-- Support/
|-- Servers/
|   |-- SessionActorLocal/
|   |-- Session/
|   |-- Actor/
|   |-- Channel/
|   |-- Spot/
|   |-- ActorCaller/
|   |-- Publisher/
|   `-- Subscriber/
|-- ServerSupport/
|   |-- Metrics/
|   |-- Readiness/
|   |-- Logging/
|   `-- ProcessMetrics/
|-- scripts/
|   |-- run_perf.sh
|   |-- run_single.sh
|   `-- collect_env.sh
`-- perf-results/
```

Casing은 언어 관례를 따르되 같은 언어 안에서 일관되게 쓴다.

### 6.1 top-level 책임

| 위치 | 책임 |
|---|---|
| `Shared` | §12 DTO, payload pattern, §15 schema와 histogram |
| `Client/Scenarios` | CS public connector call 또는 server-driven application trigger |
| `Client/Support` | 설정·CS 분할·phase·admin 호출·결과 저장 |
| `Servers/<Role>` | Public host 구성과 scenario별 typed handler·server workload |
| `ServerSupport` | Application 계측, public status 수집, readiness와 process 자원 |
| `scripts` | Build·preflight·process 시작·barrier·원본 수집·cleanup |

`Program.*`은 옵션 해석, logging·DI·host 구성과 scenario 선택만 담당한다.
새 runtime adapter나 raw frame 처리 helper는 이 구조에 포함하지 않는다.

### 6.2 10,000 client 구동 모델

CS는 physical connector 10000개를 `client-count` process에 나눈다.
`N=connections`, `P=clientCount`, `i=clientIndex`, `q=floor(N/P)`, `r=N mod P`이면
현재 process는 `q + (i < r ? 1 : 0)`개를 맡고 시작 ID는 `i*q + min(i,r)`다.
`P <= N`이며 전역 client ID가 중복되지 않는다.

- **Connect/setup은 process당 connect-concurrency로 제한하고 connector마다 한 번 시작한다.**
  재연결의 진행은 [Connector 계약][connector]이 소유하므로 pool이 실패를 숨기는 재시도를
  추가하지 않는다. 최종 준비 성공·실패 수와 setup latency를 별도로 기록한다.
- **준비 성공률이 99% 미만이면 measured를 시작하지 않는다.** 이 값은 CS 연결 준비의
  유효성 기준이며 echo의 무오류나 PS delivery를 판정하는 기준이 아니다.
- **Warmup과 measured는 같은 준비된 연결 집합을 사용한다.** 연결 교체에 드는 비용을
  한 언어에서만 제외하지 않도록 §4의 drain/reset을 사용한다.

Public dispatch가 필요한 connector mode는 [각 언어 connector interface][connector]에 맞춰
진행시키고 실제 mode를 config에 남긴다. 별도 native poller나 binding completion pump를 추가하지 않는다.

Server-driven 셀은 physical connector를 만들지 않는다. Source process 하나가
`logicalStreams`개 stream을 실행하며 `clientCount=1`인 standalone client는 trigger만 담당한다.
Spot 셀은 `streamId mod spotCount`, AC는 stream ID와 ActorId의 1:1 mapping을 사용한다.
CS는 준비된 connector ID마다 Actor 하나를 bind한다. 이 mapping과 Actor 수는 config에 남기며
Slot·count의 의미는 §13, 집계는 §15가 소유한다.

### 6.3 server 역할 분리

| 실행 역할 | 포함하는 기능과 process 경계 |
|---|---|
| `SessionActorLocal` | STREAM session과 같은 local object node의 Object Server·Actor factory |
| `Session` | CS remote의 STREAM session·Object Client; session baseline은 STREAM만 |
| `Actor` | Object Server와 Actor factory·typed echo handler |
| `Channel` | Channel handler·필요한 public caller; Channel→Spot caller는 Object Client도 등록 |
| `Spot` | Object Server·User Spot factory·local public driver·Spot handler |
| `ActorCaller` | Session 없는 Object Client·direct Actor caller·전용 return Channel Server |
| `Publisher` | Automatic Classic fanout publisher와 application workload |
| `Subscriber` | Typed fanout handler와 subscriber별 evidence |

- **역할별 실행 프로젝트와 process별 admin endpoint를 둔다.** 하나의 process에 remote
  역할을 합치면 hop·자원 사용량의 비교 경계가 바뀌기 때문이다.
- **Location Store는 공유 의존성으로 등록한다.** [Object role 계약][mesh]과 automatic
  discovery가 필요로 하는 provider이며 별도 `Registry` server role을 만들 이유가 없다.

Channel echo target은 같은 `Channel` 실행 프로젝트를 사용한다. Subscriber는 같은 executable의
독립 process N개이며, 각 process에 다른 `roleInstance`와 metrics endpoint를 준다.

### 6.4 support 코드 분리

- **Payload 생성, histogram, phase와 correlation 저장만 공통 support에 둔다.** 이들은
  측정 장치의 책임이며 public call 종류를 바꾸지 않기 때문이다.
- **Request/send/Yield 호출과 완료 기록은 scenario handler 또는 loop에 보이게 둔다.**
  `EchoAsync` 같은 wrapper가 그 차이를 숨기면 완료 경계를 검토할 수 없기 때문이다.

## 7. 폴더 구성 규칙

책임이 반복될 때만 폴더를 만든다. 파일 하나를 위한 `Utils`·`Common` 폴더를 추가하지 않는다.

### 7.1 client 폴더

Scenario 파일은 §8.4 이름에 대응한다. 한 scenario의 비교 셀은 같은 파일의 설정으로 실행한다.
`PerfRunPlan`은 CS ID 분할, `ConnectionPool`은 public 연결 준비·정리,
`ScenarioRunner`는 phase, `MetricsClient`는 admin 호출, `ResultWriter`는 원본 저장을 맡는다.
Correlation과 in-flight 계측을 server에서도 쓰면 `Shared`에 한 번만 둔다.

### 7.2 server 폴더

각 `Servers/<Role>`은 독립 executable이며 `Program`, host 구성과 typed handler를 포함한다.
Handler가 늘어날 때만 `Handlers` 폴더를 만든다. 공통 metrics 코드의 endpoint 등록은 각 role이 한다.
측정 call을 직접 실행하는 source role에서 workload와 completion recorder를 찾을 수 있어야 한다.

### 7.3 result와 log 폴더

`perf-results`, run별 `logs`·`tmp`는 generated/gitignore 대상이다.
실제 셀 경로와 덮어쓰기 거부는 §15에 정의한다.

## 8. client scenario 작성 스타일

Scenario는 부하를 어디서 시작하고 무엇을 완료로 세는지 드러낸다.
CS는 connector 호출, server-driven은 HTTP trigger와 source server의 workload 위치를 보여 준다.

### 8.1 scenario 파일 첫머리

파일 첫 주석에는 측정 질문, role/process, 한 operation의 시작·완료 경계, payload,
mode·terminal·topology, Store 필요성과 null metric 사유를 적는다.

### 8.2 scenario 본문

다음은 측정 흐름을 설명하는 contract pseudocode이며 실제 Framework API가 아니다.
각 언어의 실제 호출은 §10·§11의 interface 링크를 따른다.

```text
reserve one logical in-flight slot
record start immediately before the measured public call
invoke the public request or initial send once
observe the first request terminal or harness echo outcome
validate the echoed identity and payload
record window/settle outcome; release the slot
```

Send/send는 correlation 등록과 return handler가 드러나야 한다.
Server-driven trigger의 HTTP acknowledgement와 위 echo 결과는 따로 기록한다.

### 8.3 helper 사용 기준

| 허용 책임 | 이유 |
|---|---|
| Payload 생성·검증 | 같은 논리 byte와 pattern을 사용한다 |
| Latency·histogram·counter 저장 | Application 계측을 공통으로 집계한다 |
| CS connection pool과 phase 제어 | 부하 준비와 종료를 일관되게 만든다 |
| Correlation의 첫 결과 기록 | Send/send의 application 식별을 처리한다 |
| Admin HTTP·result writer | Measured call 밖의 수집을 담당한다 |

Public request/send를 감싸 완료 의미를 숨기거나 runtime 객체를 받는 helper는 사용하지 않는다.
Physical retry·completion drain·reconnect는 [Core][core-socket]와 [binding][binding-async]의
소유 경계이며 perf support의 기능이 아니다.

### 8.4 naming

| Scenario | File/Class 이름 |
|---|---|
| `cs-local-session-actor-echo` | `CsLocalSessionActorEchoScenario` |
| `cs-remote-session-actor-echo` | `CsRemoteSessionActorEchoScenario` |
| `s2s-channel-to-spot-request-echo` | `S2sChannelToSpotRequestEchoScenario` |
| `s2s-channel-to-spot-send-send-echo` | `S2sChannelToSpotSendSendEchoScenario` |
| `s2s-spot-to-channel-request-echo` | `S2sSpotToChannelRequestEchoScenario` |
| `s2s-spot-to-channel-send-send-echo` | `S2sSpotToChannelSendSendEchoScenario` |
| `spot-no-await-echo` | `SpotNoAwaitEchoScenario` |
| `spot-worker-offload-echo` | `SpotWorkerOffloadEchoScenario` |
| `actor-no-bind-request-echo` | `ActorNoBindRequestEchoScenario` |
| `actor-no-bind-send-send-echo` | `ActorNoBindSendSendEchoScenario` |
| `pubsub-fanout-echo` | `PubSubFanoutEchoScenario` |

언어별 casing만 바꿀 수 있다. 같은 의미를 검증하지 않은 과거 result label을 표준 이름으로
다시 표시하지 않는다. 순수 Channel request와 STREAM session→Actor request는 다른 측정이다.
기준 셀의 실행 이름은 §11에 정의한다.

### 8.5 client가 직접 하지 말아야 할 일

- **Standalone client는 server host나 handler를 만들지 않는다.** Process 경계를 유지하기 위해서다.
- **성공은 reply·correlation·admin JSON으로 판정한다.** Console 문구는 결과 schema가 아니기 때문이다.
- **Server-driven 부하는 application HTTP trigger로만 시작한다.** STREAM session이 없는
  ActorCaller나 Publisher에 connector가 접속할 수 있다고 가정하지 않기 위해서다([Session][session]).

## 9. runner script 작성 스타일

| Script | 책임 |
|---|---|
| `run_single.sh` | 한 scenario·payload·variant 셀의 실행 |
| `run_perf.sh` | 필수 셀 전체의 순차 실행과 run index 작성 |
| `collect_env.sh` | 공개 OS/runtime 정보와 산출물 provenance 수집 |

Script는 §4의 phase를 실행하고 생성한 config·process handle·container ID를 기록한다.
Build는 Java/Kotlin이 공유하는 build-only lock 안에서 수행하고 process 실행 전에 lock을 해제한다.
Port와 Docker 격리는 §20을 따른다. Cleanup에서 이름·prefix로 process를 검색해 일괄 종료하지 않는다.

여러 CS client는 서로 다른 index와 같은 cell/reset identity를 사용한다.
Script는 count를 셀 사이에 합치지 않고 §15의 owner 원본을 모은다.
공통 bucket이나 identity가 다른 원본, 빠진 필수 role 원본은 실패 결과로 남긴다.
Measured loop와 Framework 호출은 shell에 넣지 않는다.

## 10. 공통 시나리오

각 표는 setup을 끝낸 steady echo의 측정 경계를 정한다. 공통 workload·window는 §4–5,
오류·null 표현은 §14–15를 따른다. 별도 표기가 없으면 Channel transport는 automatic RouteMesh,
User Spot은 `SpotWide`, Actor 없는 Spot direct workload다.
[User Spot execution mode][g-execution]는 callback들이 공유할 실행 권한을 정하는 등록값이며,
일반 terminal/Yield의 의미는 [execution 계약][turn]을 참조한다.
Actor/Spot 생성·bind·warmup은 latency와 KOPS에 포함하지 않고 setup 결과로 남긴다.

### 10.1 `cs-local-session-actor-echo`

Session과 Actor가 같은 local object node에 있을 때 connector·session·Actor dispatch를 합친 비용을 묻는다.
[Session binding과 original reply 계약][binding]을 사용하는 echo다.

| 항목 | 측정 조건 |
|---|---|
| Role/process | CS Client × clientCount, SessionActorLocal × 1; session actor route와 Actor owner가 같은 local object node |
| 부하·mode | Physical connectors; `request`, ordinary; 대표 1024 bytes |
| 완료·집계 | Client의 public request 직전부터 원래 STREAM request의 typed echo 검증 완료까지 |
| 준비 | Connector ID마다 Actor 하나를 public manager로 준비하고 그 Ref를 해당 session에 bind |
| Location Store/Docker | Object Server 때문에 필수; run 전용 Docker Redis |
| Null/unsupported | `actor.sourceAdmission.*`는 direct send가 없어 비적용; 내부 Spot 지표는 public 관측 미지원; worker/fanout은 비적용 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | Connector `Request(dto).Async<PerfEchoReply>()`, 준비 단계 `Actors.BindOrGetAsync(ref)`와 session `RelayAsync(payload)`를 사용한다([connector][d-connector], [session][d-session], [Actor][d-actor]). |
| C++ | Connector `request(dto).submit<PerfEchoReply>(callback)`·public `dispatch()`와 `stream.actors().bind_or_get(ref)`·session relay를 사용하며 relay의 exact 선언 확인은 §10.1의 제한을 따른다([connector][c-connector], [session][c-session], [Actor][c-actor]). |
| Java | Connector `request(dto).submit(PerfEchoReply.class)`, session `actors().bindOrGet(ref)`와 `relay(dispatch,payload)`를 사용한다([connector][j-connector], [session][j-session]). |
| Kotlin | Connector wrapper `request<PerfEchoReply>(dto).await()`, `bindOrGetActor(ref)`와 session wrapper `relay(dispatch,payload).await()`를 사용한다([connector][j-connector], [session][k-session]). |
| Node.js | Connector `request(dto).submit<PerfEchoReply>()`, session `actors.bindOrGet(ref)`와 `relay(dispatch,payload)`를 사용한다([connector][n-connector], [session][n-session]). |

C++의 session relay는 [exact Actor interface][c-actor]와 공개 header의 `relay_request` 표현에
차이가 있으므로 계약 소유자의 정합성 확인이 필요하다. 해당 언어 셀을 구현·검증하지 못하면
`unsupported`와 declaration mismatch 사유를 남기고 완료로 세지 않는다. Perf 전용 raw codec을
추가하지 않는다. .NET도 [exact Session interface][d-session]의 명시적 reply 설명과
[Session binding의 original reply 관찰][binding] 사이의 정합성 확인이 필요하다. Relay admission을
Actor echo 완료로 세거나 확인되지 않은 reply 경로를 perf가 만들지 않는다. 두 언어의 이 제한은
§10.2에도 적용하며, 계약·선언 불일치를 해소하지 못한 셀은 unsupported로 기록한다.

### 10.2 `cs-remote-session-actor-echo`

Session과 Actor를 별도 process에 배치했을 때 remote hop과 relay가 더하는 비용을 묻는다.
측정 호출은 [Session binding 계약][binding]을 따르며 local 셀과 Actor 수·payload를 맞춘다.

| 항목 | 측정 조건 |
|---|---|
| Role/process | CS Client × clientCount, Session(Object Client) × 1, Actor(Object Server) × 1; 서로 다른 server PID |
| 부하·mode | Physical connectors; `request`, ordinary; 대표 1024 bytes |
| 완료·집계 | Client의 public request부터 original STREAM echo 검증까지 |
| 준비 | Actor server의 Actor를 준비하고 각 Session에 Ref를 bind; create/bind latency 별도 |
| Location Store/Docker | 두 object role에 같은 run namespace의 Docker Redis 필수 |
| Null/unsupported | §10.1과 같은 metric 비적용·관측 제한; C++/.NET relay·reply 계약 확인 필요 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | Connector `Request(dto).Async<PerfEchoReply>()`, 준비 단계 `Actors.BindOrGetAsync(ref)`와 session `RelayAsync(payload)`를 사용한다([connector][d-connector], [session][d-session], [Actor][d-actor]). |
| C++ | Connector `request(dto).submit<PerfEchoReply>(callback)`·public `dispatch()`와 `stream.actors().bind_or_get(ref)`·session relay를 사용하며 relay의 exact 선언 확인은 §10.1의 제한을 따른다([connector][c-connector], [session][c-session], [Actor][c-actor]). |
| Java | Connector `request(dto).submit(PerfEchoReply.class)`, session `actors().bindOrGet(ref)`와 `relay(dispatch,payload)`를 사용한다([connector][j-connector], [session][j-session]). |
| Kotlin | Connector wrapper `request<PerfEchoReply>(dto).await()`, `bindOrGetActor(ref)`와 session wrapper `relay(dispatch,payload).await()`를 사용한다([connector][j-connector], [session][k-session]). |
| Node.js | Connector `request(dto).submit<PerfEchoReply>()`, session `actors.bindOrGet(ref)`와 `relay(dispatch,payload)`를 사용한다([connector][n-connector], [session][n-session]). |


### 10.3 `s2s-channel-to-spot-request-echo`

Channel process에서 global SpotId로 remote request를 시작할 때 주소 조회·전달·reply의 전체 비용을 묻는다.
대상 지정과 완료는 [Spot 주소][spot-address]·[Submit][submit] 계약을 참조한다.

| 항목 | 측정 조건 |
|---|---|
| Role/process | HTTP trigger Client × 1, Channel(Object Client) × 1, Spot(Object Server) × 1 |
| 부하·mode | Channel의 logical streams; `request`, ordinary; 대표 4096 bytes |
| 완료·집계 | Channel process의 RequestToSpot 직전부터 typed echo 검증 완료까지 |
| 준비 | Spot manager의 User Spot `spotIds`; streamId mod spotCount로 배정 |
| Location Store/Docker | Object Client/Server 모두 필수; run 전용 Docker Redis |
| Null/unsupported | Physical connections와 worker/actor/fanout metric은 비적용; Spot mailbox·turn 내부 지표는 public 관측 미지원 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | `IZLinkSpotClient.RequestToSpot(spotId,dto).Async<PerfEchoReply>()`와 typed Spot request handler를 사용한다([Spot interface][d-spot]). |
| C++ | `route_client_t.request_to_spot(spotId,dto).async<PerfEchoReply>()`와 typed Spot request handler를 사용한다([Channel][c-channel], [Spot][c-spot]). |
| Java | `ZLinkRouteClient.requestToSpot(spotId,dto).submit(PerfEchoReply.class)`와 typed Spot request handler를 사용한다([Channel][j-channel], [Spot][j-spot]). |
| Kotlin | Route wrapper `requestToSpot<PerfEchoReply>(spotId,dto).await()`와 suspending Spot request handler를 사용한다([Channel][k-channel], [Spot][k-spot]). |
| Node.js | Public DI의 `ZLinkSpotOutbound.requestToSpot(spotId,dto).submit<PerfEchoReply>()`와 typed Spot request handler를 사용한다([Spot interface][n-spot]). |

Node.js는 주입된 public Spot outbound를 사용한다. 실제 `ZLinkRouteClient`에 없는 member를
internal cast로 보충하지 않는다. 두 interface의 선언 차이는 소유자 확인 항목이다.

### 10.4 `s2s-channel-to-spot-send-send-echo`

같은 방향의 request와 비교하여 양방향 send의 완료율·처리량·왕복 시간이 어떻게 다른지 묻는다.
[Send 수락][submit]과 [Channel target 선택][channel]은 소유 계약을 참조한다.

| 항목 | 측정 조건 |
|---|---|
| Role/process | HTTP Client × 1, Channel(Object Client+return Channel Server) × 1, Spot(Object Server) × 1 |
| 부하·mode | Channel logical streams; `send-send`, ordinary; 대표 4096 bytes |
| 완료·집계 | Channel의 correlation 등록/첫 public send 직전부터 return Channel handler의 echo 검증까지 |
| Return | 이 caller만 Server인 run/cell 전용 ChannelName을 setup에서 설정 |
| Location Store/Docker | Global Spot 호출과 automatic RouteMesh에 run 전용 Docker Redis 필수 |
| Null/unsupported | Physical connections·worker·Actor·fanout 비적용; 내부 Spot metric 미지원 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | Source `SendToSpot(...).Async()`와 Spot `Outbound.SendToChannel(returnChannel,reply).Async()` 및 Channel send handler를 사용한다([Spot][d-spot], [Channel][d-channel]). |
| C++ | `route_client_t.send_to_spot(...).async()`와 public route client의 `send_to_channel(returnChannel,reply).async()` 및 typed send handler를 사용한다([Channel][c-channel], [Spot][c-spot]). |
| Java | `request` 대신 `sendToSpot(...).submit()`을 호출하고 Spot `outbound().sendToChannel(returnChannel,reply).submit()`을 사용한다([Channel][j-channel], [Spot][j-spot]). |
| Kotlin | Route wrapper `sendToSpot(...).await()`와 Spot outbound의 Channel send wrapper `await()`를 사용한다([Channel][k-channel], [Spot][k-spot]). |
| Node.js | 주입된 public Spot outbound `sendToSpot(...).submit()`과 Spot의 `outbound.sendToChannel(returnChannel,reply).submit()`을 사용한다([Spot][n-spot], [Channel][n-channel]). |


### 10.5 `s2s-spot-to-channel-request-echo`

같은 Spot→Channel remote request에서 terminal과 Spot 수가 완료 처리량·tail latency·다른 callback의
진행에 미치는 영향을 묻는다. Turn 의미는 [execution 계약][turn]이 소유한다.

| 항목 | 측정 조건 |
|---|---|
| Role/process | HTTP Client × 1, Spot(Object Server+local public driver) × 1, Channel echo target × 1 |
| 필수 셀 | `ordinary × 1`, `ordinary × 16`, `yield × 1`, `yield × 16` SpotId; 각 payload에서 모두 실행 |
| 부하·mode | Spot process logical streams; `request`; 대표 4096 bytes |
| 실행 | Actor 없는 `SpotWide` User Spot direct request handler; source stream을 SpotId에 균등 배정 |
| 완료·집계 | Spot handler의 remote Channel public request 직전부터 terminal 뒤 reply 검증까지; driver RTT 별도 |
| 비교 고정값 | Remote Channel process 구성·payload·logicalStreams·inflight·deadline·execution mode |
| Location Store/Docker | User Spot와 automatic mesh에 run 전용 Docker Redis 필수 |
| Null/unsupported | Exact suspended/resumed turn·resume latency·mailbox depth는 public 미지원; `spot.remoteCallLatency.*`와 application Yield 호출 수는 측정 가능; worker/Actor/fanout 비적용 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | Spot `Outbound.RequestToChannel(name,dto).Async<PerfEchoReply>()` 또는 `.Yield<PerfEchoReply>()`와 typed Channel request handler를 사용한다([Spot][d-spot], [Channel][d-channel], [terminal][d-common]). |
| C++ | Spot handler에서 주입된 `route_client_t.request_to_channel(name,dto).async<PerfEchoReply>()` 또는 `.yield<PerfEchoReply>()`를 사용한다([Channel][c-channel], [Spot][c-spot]). |
| Java | Spot `outbound().requestToChannel(name,dto).submit(PerfEchoReply.class)` 또는 `.yield(PerfEchoReply.class)`를 사용한다([Spot][j-spot], [Channel][j-channel]). |
| Kotlin | Spot outbound request의 public bridge `awaitReply<PerfEchoReply>()` 또는 `yieldReply<PerfEchoReply>()`를 사용한다([Spot][k-spot], [Channel][k-channel]). |
| Node.js | Spot `outbound.requestToChannel(name,dto).submit<PerfEchoReply>()` 또는 `.yield<PerfEchoReply>()`를 사용한다([Spot][n-spot], [Channel][n-channel]). |

- **같은 Actor 하나에 부하를 몰아넣지 않는다.** [Actor queue claim][turn]이 개입하면 Spot
  shared gate의 비교와 다른 실험이 되기 때문이다.
- **진행성은 application handler entry와 완료 counter로 기록한다.** Yield 호출 횟수는
  실제 turn 반납 횟수가 아니며 내부 시각을 추정해 resume latency로 표시하지 않는다.

Local driver의 public Spot 호출은 §10.3의 언어별 API를 사용하며 `PerfDriveRequest`를 보낸다.
Handler는 전달받은 echo DTO로 위 remote call을 한 번 실행한다. 네 셀의 primary histogram은
같은 remote call 구간만 담고, 공유 mutable 업무 상태를 대기 전후에 이어서 판단하지 않는다.

### 10.6 `s2s-spot-to-channel-send-send-echo`

Spot에서 Channel로 보낸 send가 원래 Spot의 별도 send handler로 돌아올 때 비용과 완료율을 묻는다.
[Spot outbound][spot]와 [one-way 완료][submit] 계약에 맞춰 application correlation을 측정한다.

| 항목 | 측정 조건 |
|---|---|
| Role/process | HTTP Client × 1, Spot(Object Server+driver) × 1, Channel(Object Client) × 1 |
| 부하·mode | Spot logical streams; `send-send`, ordinary; 대표 4096 bytes |
| 완료·집계 | Source Spot handler의 correlation 등록/Channel send 직전부터 원래 Spot return handler의 echo 검증까지 |
| Return | DTO `returnSpotId`에 source User SpotId 명시; Channel handler가 그 ID로 public send |
| Location Store/Docker | Source User Spot과 return caller Object Client에 run 전용 Docker Redis 필수 |
| Null/unsupported | Physical connections·worker·Actor·fanout 비적용; exact Spot 내부 metric 미지원 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | Spot `Outbound.SendToChannel(...).Async()`와 Channel handler의 주입된 `IZLinkSpotClient.SendToSpot(returnSpotId,reply).Async()`를 사용한다([Spot][d-spot], [Channel][d-channel]). |
| C++ | Spot handler의 public `send_to_channel(...).async()`와 Channel handler의 `route_client_t.send_to_spot(returnSpotId,reply).async()`를 사용한다([Channel][c-channel], [Spot][c-spot]). |
| Java | Spot `outbound().sendToChannel(...).submit()`과 Channel handler의 `sendToSpot(returnSpotId,reply).submit()`을 사용한다([Channel][j-channel], [Spot][j-spot]). |
| Kotlin | Spot outbound Channel send와 return Spot send를 각각 public wrapper `await()`로 기다린다([Channel][k-channel], [Spot][k-spot]). |
| Node.js | Spot `outbound.sendToChannel(...).submit()`과 Channel handler의 주입된 `ZLinkSpotOutbound.sendToSpot(returnSpotId,reply).submit()`을 사용한다([Spot][n-spot], [Channel][n-channel]). |

- **Source Spot handler는 첫 send의 수락을 관찰한 뒤 반환한다.** 같은 Spot return handler를
  기다리며 turn을 유지하면 [execution 계약][turn]이 정한 진행 경계를 가로막기 때문이다.
  Driver가 turn 밖에서 application correlation을 기다리고 최종 결과까지 in-flight를 유지한다.
- **Return 주소는 DTO가 전달한다.** Channel context에 source SpotId가 제공된다고 추정하지 않기 때문이다.

### 10.7 `spot-no-await-echo`

같은 process의 public Spot client부터 즉시 echo하는 User Spot까지의 비용을 묻는다.
[Public Spot dispatch][spot-address]를 사용하며 순수 mailbox 내부 비용으로 해석하지 않는다.

| 항목 | 측정 조건 |
|---|---|
| Role/process | HTTP Client × 1, Spot(Object Server+local application driver) × 1 |
| 부하·mode | Logical streams, `no-await`; caller는 ordinary request, handler는 즉시 typed reply; 대표 1024 bytes |
| 완료·집계 | Spot process의 local public RequestToSpot 직전부터 echo 검증까지; codec·local dispatch 포함, HTTP trigger 제외 |
| 준비 | Local Object Server만 placement 가능한 구성에서 User Spot 준비; Actor 수 0 |
| Location Store/Docker | Local User Spot에도 run 전용 Docker Redis 필수 |
| Null/unsupported | Remote call·worker·Actor·fanout 비적용; mailbox depth·실제 turn 지표 public 미지원 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | `IZLinkSpotClient.RequestToSpot(spotId,dto).Async<PerfEchoReply>()`와 typed Spot request handler를 사용한다([Spot interface][d-spot]). |
| C++ | `route_client_t.request_to_spot(spotId,dto).async<PerfEchoReply>()`와 typed Spot request handler를 사용한다([Channel][c-channel], [Spot][c-spot]). |
| Java | `ZLinkRouteClient.requestToSpot(spotId,dto).submit(PerfEchoReply.class)`와 typed Spot request handler를 사용한다([Channel][j-channel], [Spot][j-spot]). |
| Kotlin | Route wrapper `requestToSpot<PerfEchoReply>(spotId,dto).await()`와 suspending Spot request handler를 사용한다([Channel][k-channel], [Spot][k-spot]). |
| Node.js | Public DI의 `ZLinkSpotOutbound.requestToSpot(spotId,dto).submit<PerfEchoReply>()`와 typed Spot request handler를 사용한다([Spot interface][n-spot]). |


### 10.8 `spot-worker-offload-echo`

Public CPU worker에 제출한 계산과 결과 전달이 local Spot echo 비용에 더하는 시간을 묻는다.
Worker 실행과 terminal은 [Submit §3][submit]·[execution][turn]을 참조한다.

| 항목 | 측정 조건 |
|---|---|
| Role/process | HTTP Client × 1, Spot(Object Server+local driver+Framework worker) × 1; remote echo process 없음 |
| 부하·mode | Logical streams, `worker-offload`; ordinary/yield 비교, 기본 yield; 대표 1024 bytes |
| 실행 | Actor 없는 SpotWide User Spot; §10.7과 같은 local public driver |
| 완료·집계 | Spot process의 local RequestToSpot부터 worker 결과를 반영한 echo 검증까지; worker public call 구간 별도 |
| Workload | `worker-task-millis`와 §5.2 pool/executor 설정을 동일하게 고정 |
| Location Store/Docker | User Spot 때문에 run 전용 Docker Redis 필수 |
| Null/unsupported | Worker queue depth와 내부 Spot 지표 public 미지원; cross-worker clock domain 미검증이면 제출→시작·종료→재개 구간 null; remote/Actor/fanout 비적용 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | Spot context `RunCpuWorker(work).Timeout(...).Async()` 또는 `.Yield()`를 사용하고 public worker options를 설정한다([Spot][d-spot], [Worker][d-common]). |
| C++ | `run_cpu_worker(work).timeout(...).async()` 또는 `.yield()`와 `worker_options_t`를 사용한다([Spot][c-spot], [Worker][c-common]). |
| Java | `runCpuWorker(work).timeout(...).submit()` 또는 `.yield()`와 `ZLinkWorkerOptions`를 사용한다([Spot][j-spot], [configuration][j-config]). |
| Kotlin | Java worker call의 `.kotlin().await()` 또는 `.kotlin().yield()`와 Java public worker options를 사용한다([Spot][k-spot], [configuration][j-config]). |
| Node.js | `runCpuWorker(work).timeoutMs(...).submit()` 또는 `.yield()`와 `ZLinkWorkerOptions`를 사용한다([Spot][n-spot], [Worker][n-worker]). |

- **Callback은 같은 self-contained CPU workload를 실행하고 sleep을 사용하지 않는다.**
  Sleep은 CPU execution 비용과 다른 부하이기 때문이다.
- **Node callback은 captured mutable collector 없이 clone 가능한 결과를 반환한다.**
  [공개 worker callback 계약][n-worker]의 격리 조건 안에서 계측해야 하기 때문이다.

작업은 `xorshift32-v1`로 고정한다: unsigned 32-bit `x=0x12345678`에서
`x ^= x << 13; x ^= x >>> 17; x ^= x << 5`를 32-bit로 수행한다.
1024회마다 monotonic elapsed와 cancellation을 확인하고 목표 시간 이상이면 종료한다.
목표 duration은 bootstrap에서 callback이 스스로 읽을 수 있는 상수로 확정한다.
Callback은 `WorkerObservation`(§15)의 시작·끝 ticks, iteration 수와 checksum을 반환한다.
실제 callback 시간과 iterations도 기록하므로 설정한 5ms를 실측 5ms로 대체하지 않는다.

Worker 제출→시작은 admission·dispatch overhead를, 종료→caller continuation은 결과 전달과 gate
재획득을 포함한다. 오류는 [ErrorKind][errors]로 기록하며 worker 전용 내부 reason을 복원하지 않는다.

### 10.9 `actor-no-bind-request-echo`

Session bind 없이 global ActorId로 접근할 때 주소 조회와 remote request 비용을 묻는다.
대상 지정은 [Actor §5][actor], 완료는 [Submit][submit]이 소유한다.

| 항목 | 측정 조건 |
|---|---|
| Role/process | HTTP Client × 1, ActorCaller(Object Client) × 1, Actor(Object Server) × 1 |
| 부하·mode | ActorCaller logical streams; `request`, ordinary; 대표 4096 bytes |
| 완료·집계 | ActorCaller의 RequestToActor 직전부터 typed echo 검증 완료까지 |
| 준비 | Stream마다 ActorId 하나; public GetOrCreate 완료 후 Entry membership의 unbound Actor 사용 |
| Location Store/Docker | Actor authority에 run 전용 Docker Redis 필수 |
| Null/unsupported | Physical connections·Spot·worker·fanout 비적용; `actor.sourceAdmission.*`는 send가 없어 비적용 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | `RequestToActor(actorId,dto).Async<PerfEchoReply>()`와 typed Actor request handler를 사용한다([Actor interface][d-actor]). |
| C++ | `actor_client_t.request(actorId,dto).async<PerfEchoReply>()`와 typed Actor request handler를 사용한다([Actor interface][c-actor]). |
| Java | `requestToActor(actorId,dto).submit(PerfEchoReply.class)`와 typed Actor request handler를 사용한다([Actor interface][j-actor]). |
| Kotlin | Actor wrapper `requestToActor<PerfEchoReply>(actorId,dto).await()`와 suspending Actor request handler를 사용한다([Actor interface][k-actor]). |
| Node.js | `requestToActor(actorId,dto).submit<PerfEchoReply>()`와 typed Actor request handler를 사용한다([Actor interface][n-actor]). |


### 10.10 `actor-no-bind-send-send-echo`

Global ActorId send의 source admission 비용과 application echo 왕복 비용을 따로 묻는다.
측정 구간은 [Actor 메시징][actor]과 [source-local admission][submit]을 참조한다.

| 항목 | 측정 조건 |
|---|---|
| Role/process | HTTP Client × 1, ActorCaller(Object Client+return Channel Server) × 1, Actor(Object Server) × 1 |
| 부하·mode | ActorCaller logical streams; `send-send`, ordinary; 대표 4096 bytes |
| 완료·집계 | ActorCaller의 correlation 등록/SendToActor 직전부터 return Channel handler의 echo 검증까지 |
| 별도 구간 | 같은 call 시작부터 SendToActor terminal까지 `actor.sourceAdmission.*` |
| 준비·Return | Stream당 unbound Actor; caller만 Server인 run/cell 전용 RouteMesh return ChannelName |
| Location Store/Docker | 두 object role와 automatic return Channel에 run 전용 Docker Redis 필수 |
| Null/unsupported | Remote mailbox 수락 시각은 public 미지원; physical connections·Spot·worker·fanout 비적용 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | `SendToActor(actorId,dto).Async()`와 Actor에 주입한 public Channel client `SendToChannel(returnChannel,reply).Async()`를 사용한다([Actor][d-actor], [Channel][d-channel]). |
| C++ | `actor_client_t.send(actorId,dto).async()`와 Actor의 public route client `send_to_channel(returnChannel,reply).async()`를 사용한다([Actor][c-actor], [Channel][c-channel]). |
| Java | `sendToActor(actorId,dto).submit()`과 Actor에 주입한 `sendToChannel(returnChannel,reply).submit()`을 사용한다([Actor][j-actor], [Channel][j-channel]). |
| Kotlin | Actor wrapper `sendToActor(actorId,dto).await()`와 주입된 Channel wrapper `sendToChannel(returnChannel,reply).await()`를 사용한다([Actor][k-actor], [Channel][k-channel]). |
| Node.js | `sendToActor(actorId,dto).submit()`과 Actor에 주입한 public Channel client `sendToChannel(returnChannel,reply).submit()`을 사용한다([Actor][n-actor], [Channel][n-channel]). |

Actor의 응답은 public Channel send를 사용한다. BoundSession을 만들어 응답하면 no-bind 측정
전제가 달라진다. Remote source admission을 local mailbox handoff라는 metric으로 표시하지 않는다.

### 10.11 `pubsub-fanout-echo`

Publisher 하나의 local publish 처리량과 Subscriber N개의 실제 수신율·지연을 묻는다.
[Classic fanout][g-fanout]은 준비된 subscriber에 event를 보내는 별도 PUB/SUB 기능이며
완료·delivery 의미는 [소유 계약][fanout]을 참조한다.

| 항목 | 측정 조건 |
|---|---|
| Role/process | HTTP Client × 1, Publisher × 1, Subscriber × subscriberCount; 각 subscriber 별도 PID |
| 부하·mode | Publisher logical streams; `publish`, ordinary; 대표 1024 bytes |
| 완료·집계 | Publisher의 public publish admission과 각 Subscriber typed handler의 unique delivery를 각각 집계 |
| In-flight | Publisher-local admission 대기 수만 제한; subscriber ACK window 없음 |
| 준비 | Automatic discovery; subscriber별 public Ready와 warmup marker 최초 수신 확인 |
| Location Store/Docker | Standard automatic 구성에 run 전용 Docker Redis 필수 |
| Null/unsupported | Echo completed/KOPS/echo latency 비적용; clock domain 미검증 시 one-way latency null; Spot/worker/Actor 비적용 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | `IZLinkFanoutClient.Publish(channel,topic,event).Async()`와 `IZLinkFanoutHandler<PerfPublishEvent>`를 사용한다([Channel interface][d-channel]). |
| C++ | `publisher_t.publish(channel,topic,event).async()`와 typed fanout subscriber handler를 사용한다([Channel interface][c-channel]). |
| Java | `ZLinkFanoutClient.publish(channel,topic,event).submit()`과 `ZLinkFanoutHandler<PerfPublishEvent>`를 사용한다([Channel interface][j-channel]). |
| Kotlin | `ZLinkKotlinFanoutClient.publish(channel,topic,event).await()`와 typed/suspending fanout handler를 사용한다([Channel interface][k-channel]). |
| Node.js | Public fanout client `publish(channel,topic,event).submit()`과 typed fanout handler를 사용한다([Channel interface][n-channel]). |

Publisher 하나가 run 안에서 sequence를 단독 발급한다. Warmup과 measured 범위는 겹치지 않으며
측정 성공 publish 집합과 subscriber별 unique 수신 집합은 §15에서 교차 집계한다.
Echo KOPS 대신 publish admission ops/sec와 subscriber delivery ops/sec를 기록한다.

Packet name에 따른 typed handler 선택과 topic 범위는 [Framework API][api]를 참조한다.
Classic fanout의 subscriber별 transport topic filter를 전제로 하지 않는다.
[Config 3 PS-A2][fanout]는 packet name별 handler 선택을 검증한다.
Late join·restart·reconnect는 이 steady 셀에 주입하지 않는다. 누락은 deliveryRatio로 남기며
Framework 오류는 실제 public kind로 기록한다.

## 11. Baseline 시나리오

기준 측정도 §4의 phase와 §15의 셀별 원본 형식을 사용한다.
`session-echo-only`부터 측정 장치를 확인하고 Channel baseline의 두 topology를 필수로 실행한다.

### 11.1 `session-echo-only`

Actor를 경유하지 않는 STREAM request의 connector·codec·session 비용을 묻는다.
[Public session callback][session]에서 typed payload를 echo한다.

| 항목 | 측정 조건 |
|---|---|
| Role/process | CS Client × clientCount, Session × 1; Actor dispatch·object role 없음 |
| 부하·mode | Physical connectors, `request`, ordinary; 대표 1024 bytes, 4096도 실행 |
| 완료·집계 | Client public request 직전부터 typed echo 검증 완료까지 |
| Location Store/Docker | 순수 STREAM 구성에는 필요 없음; automatic discovery를 추가하지 않음 |
| Null/unsupported | Actor·Spot·worker·fanout metric 비적용; remote object 동작을 추정하지 않음 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | Connector `Request(dto).Async<PerfEchoReply>()`와 session typed handler의 `Client.Reply(reply).Async()`를 사용한다([connector][d-connector], [session][d-session]). |
| C++ | Connector `request(dto).submit<PerfEchoReply>(callback)`·`dispatch()`와 public session `reply_packet(reply).async()`를 사용한다([connector][c-connector], [session][c-session]). |
| Java | Connector `request(dto).submit(PerfEchoReply.class)`와 session `client().reply(reply).submit()`을 사용한다([connector][j-connector], [session][j-session]). |
| Kotlin | Connector wrapper `request<PerfEchoReply>(dto).await()`와 session wrapper `reply(reply).await()`를 사용한다([connector][j-connector], [session][k-session]). |
| Node.js | Connector `request(dto).submit<PerfEchoReply>()`와 session `client.reply(reply).submit()`을 사용한다([connector][n-connector], [session][n-session]). |

### 11.2 `channel-echo-only`

Spot·Actor가 없는 같은 request를 RouteMesh와 ClientServer로 실행하여 topology별 비용을 묻는다.
[Channel messaging][channel]과 [ClientServer request/reply][clientserver]를 참조한다.

| 항목 | RouteMesh 필수 셀 | ClientServer 필수 셀 |
|---|---|---|
| Role/process | HTTP Client × 1, Channel source × 1, Channel target × 1 | 동일한 process 분리 |
| 구성 | Object role None; manual RouteMesh peer와 Channel Client/Server | Manual ClientServer client와 remote Server |
| 부하·mode | Source logical streams, `request`, ordinary; 대표 4096 bytes, 1024도 실행 | 동일 |
| 완료·집계 | Source public Channel request 직전부터 typed echo 검증까지 | 동일 |
| Location Store/Docker | Object role·automatic discovery가 없어 불필요 | Manual endpoint이므로 불필요 |
| Null/unsupported | Physical connectors·Actor·Spot·worker·fanout 비적용 | 동일; 별도 Completion connection의 byte 수를 이 topology의 reply byte로 대체하지 않음 |

| 언어 | Public 호출과 exact interface |
|---|---|
| .NET | 두 셀 모두 `IZLinkRouteClient.RequestToChannel(name,dto).Async<PerfEchoReply>()`와 `IZLinkRequestHandler`를 사용한다([Channel][d-channel], [topology][d-config]). |
| C++ | 두 셀 모두 `route_client_t.request_to_channel(name,dto).async<PerfEchoReply>()`와 typed Channel request handler를 사용한다([Channel interface][c-channel]). |
| Java | 두 셀 모두 `ZLinkRouteClient.requestToChannel(name,dto).submit(PerfEchoReply.class)`와 typed Channel request handler를 사용한다([Channel interface][j-channel]). |
| Kotlin | 두 셀 모두 route wrapper `requestToChannel<PerfEchoReply>(name,dto).await()`와 suspending Channel handler를 사용한다([Channel interface][k-channel]). |
| Node.js | 두 셀 모두 `requestToChannel(name,dto).submit<PerfEchoReply>()`와 typed Channel request handler를 사용한다([Channel interface][n-channel]). |

One-way admission만 재는 Channel send는 이 request baseline과 다른 결과 단위이므로 필수 셀에
포함하지 않는다. 별도 측정으로 채택하려면 delivery·완료 단위를 먼저 확정한다.

### 11.3 Local Spot 참조

`spot-local-echo`는 §10.7 `spot-no-await-echo`의 local 셀을 가리키는 비교표 이름이다.
별도 executable, CLI scenario, result 또는 두 번째 완료 count를 만들지 않는다.
질문·roles·언어별 public 호출·완료·Store·null 조건은 모두 §10.7의 같은 셀을 따른다.

## 12. 메시지 계약

- **Framework message는 기본 typed JSON serializer로 전달한다.** 언어별 byte-array 기본
  표현을 맞추기 위한 별도 message codec을 만들지 않도록 payload의 JSON 표현을 §15에 고정한다.
- **Echo는 받은 identity와 payload를 그대로 돌려준다.** 빠른 재생성·압축·일부 반환으로
  잘못된 성공을 만들지 않기 위해서다. 수신 측은 길이와 전체 byte pattern을 검증한다.

| Application DTO | 사용하는 곳과 목적 |
|---|---|
| `PerfEchoRequest` | 측정 대상 public request 또는 initial send; identity·logical sequence·return 주소 |
| `PerfEchoReply` | Typed request reply 또는 return send; 같은 identity·payload |
| `PerfDriveRequest` / `PerfDriveReply` | §10.5·§10.6의 local public Spot driver; 측정 operation 시작 여부와 driver 완료 |
| `PerfTriggerRequest` / `PerfTriggerReply` | 별도 application HTTP endpoint의 phase 시작과 acknowledgement |
| `PerfPublishEvent` | Publisher가 단독 발급한 sequence와 fixed topic을 가진 typed fanout event |
| `WorkerObservation` | Public worker callback이 반환하는 timing·workload evidence |
| `PerfMetricsSnapshot` | §16 admin 조회 또는 client 원본 결과 |

Field type·JSON·ticks·payload pattern은 §15.2가 한 곳에서 정의한다.
Message 등록은 stable packet name과 typed handler를 사용한다([Framework API][api]).
`PerfDriveRequest`는 HTTP trigger나 remote Channel echo와 다른 application packet이다.

## 13. Request 방식과 Send/Send 방식의 공정성

한 **logical operation**은 측정 대상 public call 한 번으로 시작한 업무 단위다.
In-flight 자리는 public call 시작 전에 확보하고 request의 첫 terminal, 또는 send/send의
최종 echo correlation 결과까지 유지한다. Source admission 대기도 포함한다.
§4.2의 local Spot driver는 이 단위의 제출 장치이며 nested public call을 추가 KOPS로 세지 않는다.

- **Native DONTWAIT 시도·wait token·WRITABLE을 operation으로 세지 않는다.** 그 진행은
  [Core socket][core-socket]과 [binding completion owner][binding-async]가 소유하고 perf는
  public awaitable 하나의 결과만 관찰하기 때문이다.
- **Send/send는 모든 언어에서 harness correlationId를 사용한다.** Framework request
  correlation 제공 여부와 관계없이 두 one-way call은 application echo로 연결해야 하기 때문이다.
- **Request와 send/send는 같은 stream 수·inflight·payload·배치·deadline으로 비교한다.**
  무제한 send 적재나 다른 return target은 같은 완료 비용을 재지 않기 때문이다.

Request의 terminal 선택과 remote 업무가 남는 조건은 [Submit §9][submit]를 참조한다.
Harness는 public reply를 검증한 성공, public 실패, cancellation을 서로 배타적인 결과로 기록한다.
Send/send correlation은 첫 public send 직전에 등록하고 동일 시점에 expiry deadline을 고정한다.

| 관찰 | Harness 기록 |
|---|---|
| 첫 send의 정상 admission | `messages.admitted`; AC는 sourceAdmissionMs도 기록; echo 성공은 아직 아님 |
| Pending correlation에 첫 유효 reply | Echo 성공 1회; window/settle 구분은 §4 |
| Pending 중 첫 send 실패 | 그 public 실패를 최종 결과로 기록; expiry로 다시 세지 않음 |
| Pending 중 deadline 도달 | `messages.timeout` 1회와 그 부분집합 `messages.expired` 1회; harness `CorrelationExpired` |
| 성공으로 닫힌 correlation의 추가 reply | `messages.duplicateReply`; 성공 수·histogram 불변 |
| 실패·expiry로 닫힌 correlation의 reply | `messages.lateReply`; 원래 결과 불변 |
| 해당 cell에서 발급하지 않은 correlation | `messages.unknownCorrelation`; 기존 operation 결과는 바꾸지 않음 |
| Reply identity·payload 불일치 | `messages.failed`와 harness `PayloadMismatch` 또는 `IdentityMismatch` |

Correlation의 종료 이유와 identity는 최종 snapshot까지 유지한다. Reply와 실패·expiry가 경쟁하면
처음 확정한 결과만 반영한다. `errors.byKind`는 결과의 설명용 분류이며 실패 합계에 다시 더하지 않는다.
Request public `DeadlineExceeded`와 harness `CorrelationExpired`는 `messages.timeout`에 각각 한 번
기록하며 Framework 오류와 harness 오류의 namespace를 구분한다.

Echo cohort는 다음 식으로 결산한다. Count는 §15의 정수 문자열로 저장한다.

```text
messages.sent = messages.completed + messages.settleCompleted
              + messages.failed + messages.timeout
              + messages.cancelled + messages.unresolved
messages.expired <= messages.timeout
```

`sent`는 measured call을 시작한 logical operation 수이며 물리 전송 성공 수가 아니다.
Publish의 결산은 §15.4에서 별도로 정의한다. 실패·미완료 operation은 성공 latency에 넣지 않는다.

Send/send에서 echo가 first-send terminal보다 먼저 관측되면 echo 시각을 보존한다.
In-flight slot은 echo의 최종 결과와 first-send terminal을 모두 관찰한 뒤 반납하므로
admission 대기 중인 public call 위에 다음 operation을 겹쳐 제출하지 않는다. Terminal이 settle
bound까지 없으면 완료 echo를 다시 계수하지 않고 별도 수집 실패로 셀을 실패 처리한다.

## 14. 메트릭

Metric은 application 계측, public host status와 표준 provider 계기, 공개 OS/runtime process
API에서 얻는다. [Runtime monitoring][monitor]·[Runtime metrics][metrics]가 공개하지 않는
값은 근사값으로 대체하지 않는다. 결과 metric은 dotted key를 가진 `metrics` object에 둔다.

| Key 또는 key 집합 | 단위·형식 | 측정 경계 |
|---|---|---|
| `connections.requested/connected/failed` | count, u64 문자열 | CS setup의 최종 연결 결과; measured counter reset과 별도로 보존 |
| `load.logicalStreams`, `load.inflightPerStream`, `load.inflight.max` | count, u64 문자열 | Server-driven stream 수·설정 상한·application에서 관측한 전체 최대 미완료 수 |
| `messages.sent` | count, u64 문자열 | Window 안 측정 public call을 시작한 logical operation 수 |
| `messages.admitted` | count, u64 문자열 | Initial one-way send의 정상 public admission terminal 수; request는 null |
| `messages.completed`, `messages.settleCompleted` | count, u64 문자열 | §4의 window 성공·settle 성공 |
| `messages.failed/timeout/cancelled/unresolved` | count, u64 문자열 | §13의 서로 배타적인 cohort 결과 |
| `messages.expired/duplicateReply/lateReply/unknownCorrelation` | count, u64 문자열 | Send/send correlation 결과·추가 관찰; request는 비적용 |
| `applicationMessages.request/send/reply/event` | count, u64 문자열 | Window 안 application 측정 경로의 public call 시작 또는 typed reply 반환 수; wire frame 아님 |
| `applicationPayloadBytes.request/send/reply/event` | bytes, u64 문자열 | 위 각 관찰에 대응하는 logical payload bytes |
| `throughput.kops` | number, kop/s | Owner의 window echo 성공/sec/1000; CS 집계는 §15.4 |
| `throughput.messagesPerSec` | number, message/s | 위 application message count의 합/sec; native attempt·header·fragment·handshake·driver·admin 제외 |
| `throughput.megabytesPerSec` | number, MiB/s | 위 directional logical payload bytes의 합/sec/1048576; echo 양방향 포함, wire 대역폭 아님 |
| `latency.meanMs/p50Ms/p95Ms/p99Ms/maxMs` | number 또는 null, ms | Window echo 성공 histogram `latencyMs` |
| `settle.latency.*` | number 또는 null, ms | Settle echo 성공 histogram `settleLatencyMs` |
| `actor.sourceAdmission.latency.*` | number 또는 null, ms | AC send의 call 시작→정상 source admission; `sourceAdmissionMs`에서 계산 |
| `spot.remoteCallLatency.*` | number 또는 null, ms | §10.5의 call 시작→reply 검증; gate 재획득·continuation을 포함하며 `latencyMs`와 같은 구간 |
| `spot.applicationYieldCalls` | count, u64 문자열 | Handler에서 실제로 호출한 Yield 수; turn suspension 수가 아님 |
| `spot.applicationHandlerEntries` | count, u64 문자열 | Measured DTO를 받은 application Spot handler entry 수 |
| `driver.issued/notStarted/failed` | count, u64 문자열 | §4.2 local driver 호출과 측정 시작 전 거부·실패; KOPS와 분리 |
| `driver.latency.*` | number 또는 null, ms | Local driver public call→driver 결과의 application 구간 |
| `worker.callLatency.*` | number 또는 null, ms | Public worker call 직전→결과가 caller continuation에 도착한 시점 |
| `worker.submitToStart.*` | number 또는 null, ms | 같은 clock domain의 worker call 직전→callback 시작; admission·dispatch 포함 |
| `worker.taskLatency.*` | number 또는 null, ms | Callback의 monotonic 시작→끝 |
| `worker.resultToContinuation.*` | number 또는 null, ms | Callback 끝→caller 재개; 결과 전달·gate 재획득 포함 |
| `messages.published/publishedInWindow/settlePublished` | count, u64 문자열 | PS cohort의 성공 publish 전체/window/settle; §15.4 |
| `fanout.subscriberCount` | count, u64 문자열 | 별도 Subscriber process 수 |
| `fanout.uniqueDelivered/deliveredInWindow/settleDelivered` | count, u64 문자열 | Subscriber별 publisher window 성공 집합에 해당하는 검증된 unique 수신; §15.4 |
| `fanout.duplicateEvents` | count, u64 문자열 | 같은 measured identity의 추가 수신 횟수; publisher 성공 집합 교차와 별도 진단 |
| `fanout.outOfCohortEvents` | count, u64 문자열 | 최종 publisher window 성공 집합 밖의 unique sequence 수; delivery 분자에서 제외 |
| `fanout.deliveryRatio` | number 또는 null, ratio | Subscriber별 uniqueDelivered/publishedInWindow의 최솟값 |
| `fanout.publishOpsPerSec` | number 또는 null, op/s | publishedInWindow/publisher measuredSeconds |
| `fanout.deliveryOpsPerSec` | number 또는 null, event/s | Subscriber별 deliveredInWindow/자기 measuredSeconds; 전체는 합 |
| `fanout.deliveryLatency.*` | number 또는 null, ms | 검증된 공통 clock domain에서만 publish call 직전→subscriber handler entry |
| `fanout.settleDeliveryLatency.*` | number 또는 null, ms | 같은 fanout 완료 구간의 settle 수신; window histogram과 분리 |
| `process.cpuPercent` | number 또는 null, % | Process CPU 시간 증가/monotonic 측정 시간×100; core 하나가 100%, 100% 초과 가능 |
| `process.rssMb` | number 또는 null, MiB | 100ms 주기 process RSS sample의 최대값; 실제 sample 간격도 기록 |
| `process.allocatedMb` | number 또는 null, MiB | Runtime이 제공하는 measured 누적 allocation 증가 |
| `gc.gen0/gen1/gen2` | count 문자열 또는 null | 해당 generation을 공개하는 runtime의 window 증가분; JVM collector 수를 .NET generation으로 재명명하지 않음 |
| `errors.byKind` | object of count strings | 집계 owner가 관찰한 public Framework ErrorKind별 결과 수 |
| `errors.harness` | object of count strings | Application 계측·validation·correlation 실패 |
| `errors.language` | object of count strings | Public kind 없는 argument/configuration 오류·언어 cancellation; 실제 type 보존 |

`*` latency 집합은 `meanMs`, `p50Ms`, `p95Ms`, `p99Ms`, `maxMs`다.
표준 family가 비적용이면 그 family의 scalar key도 생략하지 않고 §15.5로 null을 기록한다.
`driver.*`는 §10.5–10.6의 보조 local driver에만 적용한다. §10.7–10.8의 local public
caller 구간은 이미 primary latency이므로 driver histogram에 복제하지 않는다.
Server-driven의 physical connection metric은 null이고,
CS의 `load.logicalStreams`는 null이다. CS in-flight의 기준은 connector별 설정과 실제 계측으로 남긴다.

### 14.1 Public 관측 미지원 값

다음 key는 관련 workload에서도 public 관측 미지원으로 null이다.
다른 workload에서는 비적용으로 null이다. Host aggregate를 Spot/worker별 값으로 복사하지 않는다.

| Key | 사유 |
|---|---|
| `spot.mailboxDepth.max/mean` | Public status는 host aggregate만 제공 |
| `spot.suspendedTurns`, `spot.resumedTurns` | 실제 turn마다 public counter 없음 |
| `spot.resumeLatency.p95Ms/p99Ms` | Remote reply-ready 내부 시각 미공개 |
| `worker.pool.queueDepth.max/mean` | Public worker options는 설정이며 queue depth snapshot 아님 |
| `host.queueWaitLatency.p50Ms/p95Ms/p99Ms` | Exact pre-receive→handler queue wait의 public hook 없음 |

### 14.2 오류와 자원 기록

`errors.byKind`의 key는 [Framework 오류 모델 §2][errors]의 이름을 사용한다.
각 언어 enum을 숫자 값으로 대조해 `NotFound`, `AlreadyExists`, `TypeMismatch`, `NotConfigured`,
`Rejected`, `Unavailable`, `CapacityExceeded`, `DeadlineExceeded`, `ShuttingDown`, `ProtocolError`,
`InvalidOperation`, `DataLost`, `InternalFailure`로 정규화한다. 새로운 kind를 perf가 만들지 않는다.
문자열 parsing으로 route·worker 내부 원인을 복원하지 않는다.

Harness key는 `CorrelationExpired`, `PayloadMismatch`, `IdentityMismatch`, `UnknownCorrelation`,
`DuplicateReply`, `SettleIncomplete`, `PhaseMismatch`, `SchemaMismatch`, `CollectionFailure`를 사용한다.
이것은 Framework enum이 아니다. 실제 public 오류가 없는 동기 language 오류는 `errors.language`에 둔다.
한 원격 오류가 caller와 target에서 함께 보이면 결과의 오류 합계는 §15 집계 owner의 관찰만 사용하고
target의 진단은 해당 role 원본에 남긴다.

Process 수치는 process별로 남기고 CPU percent·RSS를 임의로 더해 host metric으로 만들지 않는다.
GC가 없거나 공통 generation을 제공하지 않으면 null 사유를 남긴다. JVM heap/non-heap,
Node event-loop delay 등은 `runtimeMetrics`에 실제 이름·단위와 함께 보완한다.

## 15. 결과 파일 형식

### 15.1 셀별 파일과 identity

- **한 셀은 독립 디렉터리에 원본을 보존한다.** 같은 run에서 scenario·payload·terminal이나
  배치가 달라져도 앞 결과를 덮어쓰지 않아야 하기 때문이다.
- **같은 출력 경로가 이미 있으면 실패한다.** 반복 실행도 새 runId 또는 §23의 repetition을
  사용해야 비교 입력을 구분할 수 있기 때문이다.

```text
perf-results/<run-id>/
|-- index.json
|-- summary.txt
`-- <scenario>/
    `-- <payload-bytes>/
        `-- <variant>/
            |-- config.json
            |-- endpoints.json
            |-- role-configs/
            |-- result.json
            |-- summary.txt
            |-- client-<index>.json
            |-- server-<role>-<instance>.json
            |-- publisher-sequences.json
            |-- subscriber-<id>-sequences.json
            `-- logs/
                |-- client-<index>.log
                |-- server-<role>-<instance>.log
                `-- message-flow-<role>-<instance>.log
```

Sequence 파일은 PS만 생성한다. 일반 measured에서는 message-flow 파일을 만들기 위해 tracing을
추가로 켜지 않는다. 진단 실행은 §21에 따라 표준 성능 결과와 분리한다.
`variant`는 `<mode>-<terminal>-<topology>-s<spotCount>-n<subscriberCount>-<configHash>`다.
비적용 count/topology는 경로에서 `na`로 쓴다. `configHash`는 phase 시작 전에 고정한 비교 입력
JSON UTF-8 bytes의 SHA-256 전체 lowercase hex다. 그 정확한 입력 bytes를 config에 보존한다.

비교 입력에는 language, mode, terminal, topology/discovery, execution mode, Spot/Actor mapping 규칙·개수,
subscriber 수, connection/stream 분할, in-flight, timeout, worker workload/options, CPU·memory·
runtime option, serializer, §23 workload hash·repetition을 넣는다. RunId·PID·동적 port·출력 경로는
비교 hash 입력에서 제외하고 환경 metadata에 남긴다. 실제 run/cell별 SpotId·ActorId도 hash 확정 뒤
생성하며 hash에는 ID 문자열 대신 배치·분할 규칙을 넣는다. `cellId`는 run 안의 이 상대 디렉터리다.

### 15.2 DTO field type과 JSON

다음 선언은 공통 application DTO를 정의하는 **contract pseudocode이며 실제 Framework API가 아니다**.
모든 JSON field 이름은 표시한 lowerCamelCase다. Schema version은 정수 `2`다.

```text
U64 = decimal JSON string               // 0..18446744073709551615; "0" 또는 선행 0 없는 숫자
I64 = decimal JSON string               // -9223372036854775808..9223372036854775807; 부호 있는 ticks
Index = JSON integer                   // 0..2147483647; stream/client/subscriber index
Number = finite JSON number            // NaN/Infinity 미허용; 계산은 반올림 전 값으로 보관
Text = JSON string                     // UTF-8; identity는 대소문자를 구분
Phase = "warmup" | "measured"

Identity {
  runId: Text                           // run 자원 identity
  cellId: Text                          // run 안의 scenario/payload/variant 상대 경로
  resetSeq: U64                         // warmup 0, measured reset 이후 양의 값
  phase: Phase                         // warmup과 measured cohort를 구분
}
PerfEchoRequest extends Identity {
  clientId: Index                       // CS 전역 connector ID 또는 source logical stream ID
  sequence: U64                         // source가 clientId별로 발급, cell 안 재사용 없음
  correlationId: Text                   // cellId/phase/clientId/sequence; harness identity
  sentTicks: I64                        // 측정 call 직전의 monotonic nanoseconds
  clockDomainId: Text                   // 이 ticks의 epoch/unit을 식별
  returnSpotId: Text | null             // Spot return이 필요한 send/send만 지정
  returnChannel: Text | null            // Caller 전용 Channel return이 필요한 send/send만 지정
  payload: Text                         // logical bytes의 canonical padded Base64
}
PerfEchoReply extends Identity {
  clientId: Index
  sequence: U64
  correlationId: Text                   // request에서 그대로 복사
  receivedTicks: I64                    // target handler의 local monotonic 시각; 진단 전용
  clockDomainId: Text                   // receivedTicks의 domain; RTT는 caller 자신의 clock 사용
  payload: Text                         // 받은 Base64를 그대로 echo
}
PerfDriveRequest {
  echo: PerfEchoRequest                 // local driver가 요청한 measured operation
}
PerfDriveReply {
  started: boolean                     // window 종료 후 handler에 도착하면 false
  echo: PerfEchoReply | null            // request 방식의 결과; send/send admission ACK 또는 미시작은 null
}
PerfTriggerRequest extends Identity {}  // role config에 기록한 phase를 한 번 시작; batchSize 없음
PerfTriggerReply extends Identity {
  accepted: boolean
  state: "started" | "alreadyStarted" | "rejected"
  configHash: Text                      // role이 적용한 비교 입력 hash
  reason: Text | null                   // rejected이면 필수; 성공에는 null
}
PerfPublishEvent extends Identity {
  sequence: U64                         // Publisher 하나가 run 전체에서 단독 발급
  topic: Text                           // "perf.echo" 고정
  sentTicks: I64
  clockDomainId: Text
  payload: Text
}
WorkerObservation {
  startedTicks: I64
  endedTicks: I64
  clockDomainId: Text
  iterations: U64
  checksum: JSON integer                // xorshift32 결과 0..4294967295
}
```

Payload의 logical byte `b[i]`는 `(31*i + 17*floor(i/251) + 29) mod 256`이다.
`payloadSize`는 이 bytes의 수이며 Base64 길이·JSON property·Framework header는 포함하지 않는다.
Base64 변환은 application field 표현이고 message 전체의 수동 codec이 아니다.
같은 typed JSON serializer를 사용하고 packet별 encoder/decoder를 추가하지 않는다.
Snapshot의 `serializedMessageBytes`는 `{direction:Text, packetName:Text,
logicalPayloadBytes:U64, observedSerializedBytes:U64|null}` 배열이다. Direction은
`request|send|reply|event` 중 하나이며 서로 다른 DTO·payload 조합은 별도 행이다.
Public 관측이 없으면 observedSerializedBytes를 null+reason으로 둔다. 이 관측을 얻으려고
measured path에서 message를 두 번 serialize하지 않는다. Logical bytes를 wire bytes로 부르지 않는다.

Elapsed·deadline·retention은 [시간원 계약][liveness]에 맞춰 monotonic clock을 쓴다.
Ticks 단위는 **nanosecond**이며 native clock의 frequency에서 변환한 값을 기록한다.
Clock metadata의 type은 아래와 같다. Nullable alignment field는 자기 process RTT에는
비적용이며, 공통 domain을 주장할 때는 근거와 오차를 채운다.

```text
ClockMetadata {
  source: Text
  nativeFrequencyHz: U64
  ticksUnit: "ns"
  clockDomainId: Text
  scope: "process" | "host" | "aligned-hosts"
  alignmentMethod: Text | null
  maxErrorNs: U64 | null
  validFromTicks: I64 | null
  validThroughTicks: I64 | null
  evidence: Text[]                      // public OS/runtime 근거 또는 alignment 산출물 경로
}
```
Unix timestamp는 UTC 표기에만 쓰며 elapsed 계산에 사용하지 않는다.

공통 domain은 epoch와 단위가 같음을 공개 OS/runtime 근거로 확인한 경우에만 선언한다.
Process가 같아도 worker의 clock epoch가 다르면 직접 차감하지 않는다.
Cross-host는 alignment 방법·유효 구간·오차 상한을 증명하지 못하면 one-way latency를 null로 남긴다.
RTT는 caller의 clock 두 값으로 계산하므로 remote receivedTicks와 차감하지 않는다.
Node bigint와 모든 64-bit 값은 decimal string으로 저장하며 JSON number로 축소하지 않는다.

### 15.3 Histogram과 percentile

Application latency histogram은 다음 형식을 사용한다. Public provider의 자체 histogram을
이 application histogram과 혼합하지 않는다.

```json
{
  "unit": "ms",
  "ticksUnit": "ns",
  "bounds": [0.1, 0.25, 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512, 1024],
  "counts": ["0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0", "0"],
  "overflow": "0",
  "count": "0",
  "sumNs": "0",
  "maxNs": null,
  "percentileMethod": "nearest-rank-bucket-upper-bound"
}
```

- **Bucket는 `[0,b0]`, 이후 `(b[i-1],b[i]]`이며 누적 count가 아니다.** 같은 sample이
  하나의 bucket 또는 overflow에만 들어가야 합산할 수 있기 때문이다.
- **Percentile은 nearest rank의 bucket 상한으로 추정한다.** `rank=ceil(p*count)`의
  sample을 포함하는 첫 bucket의 상한을 사용한다. p50/p95/p99의 rank는 정수 연산
  `ceil(q*count/100)`(`q=50,95,99`)으로 계산해 count를 부동소수로 축소하지 않는다.
  0.1ms보다 빠른 p50도 `0.1`이라는
  quantized estimate이며 정확한 실측값이라고 표시하지 않는다.
- **선택된 rank가 overflow에 있으면 percentile은 null이다.** 마지막 상한으로 값을
  자르면 tail을 과소 표시하기 때문이다. `HISTOGRAM_OVERFLOW`와 `lowerBoundMs=1024`를 남긴다.
- **Mean과 max는 bucket에서 복원하지 않는다.** 모든 유효 sample(overflow 포함)의
  정확한 integer `sumNs`, `count`, `maxNs`에서 계산한다.

`count=sum(counts)+overflow`다. `count`, 각 count와 overflow는 U64이며 overflow 시 셀 실패다.
`sumNs`는 overflow 없는 arbitrary-precision 비음수 decimal string이다.
`maxNs`는 U64 또는 null이며 sample이 없으면 null+`NO_SAMPLES`다.
`meanMs=sumNs/count/1000000`, `maxMs=maxNs/1000000`이다.
Sample이 0이면 모든 percentile·mean·max는 null+`NO_SAMPLES`다.

집계는 같은 단위·bounds·cohort·구간의 count와 sum을 더하고 max의 최댓값을 취한다.
Process별 percentile이나 mean을 단순 평균하지 않는다. 서로 다른 bucket 원본은
`SchemaMismatch` 실패다. `latencyMs`와 `settleLatencyMs`는 합치지 않는다.
동일 구간인 `spot.remoteCallLatency.*`는 §10.5의 `latencyMs`를 참조해 계산하고 두 번째
histogram을 유지하지 않는다. 다른 구간은 아래의 별도 histogram으로 보존한다. 전체 latency key와 sample 집합을
하나의 표로 대응시켜 collector마다 다른 구간을 합치지 않도록 한다.

| Histogram key | Metric prefix·표본 |
|---|---|
| `latencyMs` / `settleLatencyMs` | `latency.*` / `settle.latency.*`; window/settle echo 성공 |
| `sourceAdmissionMs` | `actor.sourceAdmission.latency.*`; window에서 terminal이 된 AC initial send admission |
| `driverLatencyMs` | `driver.latency.*`; window에서 완료된 local driver |
| `workerCallLatencyMs` | `worker.callLatency.*`; window에서 완료된 worker call |
| `workerSubmitToStartMs` | `worker.submitToStart.*`; 위 worker call의 같은 callback |
| `workerTaskLatencyMs` | `worker.taskLatency.*`; 위 worker call의 같은 callback |
| `workerResultToContinuationMs` | `worker.resultToContinuation.*`; 위 worker call의 같은 callback |
| `fanoutDeliveryLatencyMs` / `fanoutSettleDeliveryLatencyMs` | `fanout.deliveryLatency.*` / `fanout.settleDeliveryLatency.*`; §15.4 window/settle unique 교차 집합 |

위 성공 sample은 모두 measured cohort에 속한다. 보조 구간의 settle 표본은 primary echo
histogram에 섞지 않는다. 비적용 histogram도 null+reason으로 남긴다.

### 15.4 집계 owner와 result schema

| 계열 | Primary owner·원본 | 집계 |
|---|---|---|
| CS와 session baseline | `client-<index>.json` | Echo count·histogram 합산; throughput은 각 client count/measuredSeconds의 합 |
| Channel→Spot와 Channel baseline | Source `server-channel-<instance>.json` | Source의 완료·correlation·clock 사용 |
| Spot→Channel·Spot local/worker | Source `server-spot-0.json` | §10에서 정한 source handler/caller 구간; HTTP trigger·driver를 추가 echo로 세지 않음 |
| AC | `server-actorCaller-0.json` | ActorCaller의 request/correlation과 source admission |
| PS | `server-publisher-0.json`과 모든 `server-subscriber-<id>.json` | Publish와 subscriber별 delivery를 분리; 아래 sequence 교차 집계 |

Application message/byte 계수는 각 public send/request call 시작과 typed reply 반환을 한 번씩
소유하는 process가 기록한다. Incoming callback을 같은 message의 두 번째 송신으로 세지 않는다.
CS의 session relay처럼 application 코드가 실제 호출하는 추가 send는 해당 방향에 포함한다.
`throughput.messagesPerSec`와 `throughput.megabytesPerSec`는 측정 role 각각의
window count·byte/자기 measuredSeconds를 합산한다. Trigger client는 이 합계에 넣지 않는다.
이 application rate 집계는 primary echo의 KOPS 집계와 다른 scope이며 각각 기록한다.
Local driver·HTTP·내부 service/control record는 제외한다. 각 role의 자기 window 값과
`messageCountScope="application-call-boundaries"`를 원본에 남긴다.

모든 Snapshot의 공통 구조는 다음과 같다. `Object`는 JSON object, `[]`는 JSON array다.

```text
PerfMetricsSnapshot {
  schemaVersion: JSON integer           // 2
  runId: Text; cellId: Text; resetSeq: U64
  language: "dotnet" | "cpp" | "java" | "kotlin" | "node"
  role: Text; roleInstance: Index
  configHash: Text
  phase: "setup" | "warmup" | "reset" | "measured" | "settle" | "complete"
  window: {
    startedAtUnixMs: I64 | null         // 표기 전용; 아직 시작하지 않았으면 null
    endedAtUnixMs: I64 | null
    startTicks: I64 | null
    endTicks: I64 | null
    measuredSeconds: Number | null     // (endTicks-startTicks)/1e9, 같은 owner clock
    settleSeconds: Number | null
  }
  clock: ClockMetadata                 // §15.2
  serializedMessageBytes: Object[]      // §15.2의 typed row
  metrics: Object                      // §14 dotted keys, count는 U64
  histograms: Object                    // §15.3 형식 또는 null
  nullReasons: Object                  // §15.5 JSON pointer → 사유
  publicStatus: Object | null          // 실제 public host snapshot; 해당 언어 field/enum 보존
  publicMetrics: Object[]              // name, kind, unit, labels, value; 소유 metric 이름/단위 보존
  runtimeMetrics: Object               // 언어별 process 보완; name/unit/type/value 포함
  provenance: Object                   // §19 환경·artifact·PID 정보
}
PerfResult {
  schemaVersion: JSON integer           // 2
  runId: Text; cellId: Text; configHash: Text
  language: Text; scenario: Text
  configFile: Text; endpointsFile: Text // 셀 내부 상대 경로
  status: "valid" | "failed" | "invalid" | "unsupported"
  baselineEligible: boolean             // §19의 성능 baseline 채택 조건
  reasons: Object[]                     // code, message, sourceFile
  metricOwners: Text[]                  // primary 원본 상대 경로
  ownerWindows: Object                 // 원본 파일명 → 그 window
  measuredSeconds: Number | null       // primary owner 하나일 때만 값; CS 복수 owner는 null+MULTIPLE_OWNERS
  aggregation: {
    rateMethod: "single-owner" | "sum-owner-rates"  // primary echo rate
    applicationRateMethod: "sum-role-rates"
    fanoutDeliveryRateMethod: "sum-subscriber-rates" | null // PS 외 비적용
  }
  metrics: Object; histograms: Object; nullReasons: Object
  clients: Text[]; servers: Text[]      // 필수 원본 상대 경로
  processes: Object[]                  // process별 자원·clock·public status 원본 참조
}
```

Public status의 64-bit field도 §15.2 표현을 쓰며 원래 type은 해당 exact interface를 참조한다.
Public metric `value`는 선언이 integer이면 문자열, 실수이면 finite number다.
Histogram·label·unit을 임의로 바꿔 같은 provider metric처럼 export하지 않는다.
`index.json`은 `{schemaVersion:2, runId, cells:[{cellId, resultFile, status}]}`다.
Run root summary는 셀별 행이며 셀 사이 throughput 합계가 없다.

PS Publisher는 측정 시작 sequence 범위와 성공 publish sequence 집합을 보존한다.
`published=publishedInWindow+settlePublished`이며 둘 다 measured cohort의 public admission 성공이다.
`sent=published+failed+timeout+cancelled+unresolved`로 결산한다.
`messages.completed`, echo `latency.*`, `throughput.kops`는 null+`NOT_APPLICABLE`다.

Sequence 파일의 공통 identity는 §15.2의 measured Identity다. 각 range는 양 끝 포함,
오름차순, 겹치지 않는 maximal contiguous interval이다.

```text
Range { first: U64; last: U64 }
PublisherSequences extends Identity {
  attemptedRanges: Range[]              // measured 동안 시작한 모든 publish
  windowSuccessRanges: Range[]          // publisher window 안 admission 성공
  settleSuccessRanges: Range[]          // settle에서 admission 성공; ratio 분모에서 제외
}
SubscriberSequences extends Identity {
  subscriberId: Index
  windowRanges: Range[]                 // 검증된 첫 수신이 subscriber window 안인 sequence
  settleRanges: Range[]                 // 첫 수신이 settle 안인 sequence; windowRanges와 겹치지 않음
  duplicateEvents: U64
  nullReasons: Object                  // §15.5의 JSON pointer와 reason
  timingEvidence: ReceiptTiming[] | null // 공통 clock domain 미검증이면 null+reason
}
ReceiptTiming {
  sequence: U64
  sentTicks: I64; publisherClockDomainId: Text
  receivedTicks: I64; subscriberClockDomainId: Text
  receivedIn: "window" | "settle"
}
```

Publisher 성공 집합에는 실패한 sequence를 넣지 않는다. 최종 delivery 집계는 각 subscriber의
windowRanges/settleRanges를 **windowSuccessRanges와만 교차**한다. Ratio 분모는
`publishedInWindow`다. Settle publish 성공을 별도로 보존하되 이 분모에 더하지 않는다.
Subscriber별 `uniqueDelivered=deliveredInWindow+settleDelivered`는 그 교차 집합의 크기다.
같은 sequence가 두 구간에서 수신되면 첫 수신만 unique이며 뒤 수신은 duplicateEvents다.
Cohort 밖 수신은 outOfCohortEvents이며 unique delivery와 histogram에서 제외한다.
각 subscriber의 monotonic 구간으로 window/settle을 나누고 시작 skew의 관찰 근거를 보존한다.
분모가 0이면 ratio는 null+`ZERO_DENOMINATOR`이고 셀은 `invalid`다.
수신 누락만으로 Framework error를 만들지 않는다.

Timing evidence는 unique 수신당 한 행이다. 최종 latency histogram도 위 교차 집합으로만
계산하고 window/settle을 분리한다. Evidence 비용은 subscriber process의 실제 자원 사용량에
포함하며 수집 방식·보관 byte 수를 provenance에 기록한다.

### 15.5 Null과 reason

- **비적용·관측 미지원·표본 부재를 null과 reason으로 구분한다.** 0은 관측한 값이 실제
  0일 때만 쓸 수 있기 때문이다.
- **미지원 metric과 미구현 scenario를 구분한다.** 내부 metric의 null은 유효하지만 필수
  공개 호출을 실행하지 못한 셀은 `unsupported`이며 완료로 세지 않기 때문이다.

`nullReasons`는 값의 JSON pointer를 key로 갖는다. 예:

```json
{
  "/metrics/spot.mailboxDepth.max": {
    "code": "PUBLIC_OBSERVATION_UNSUPPORTED",
    "reason": "Host capacity exposes no per-Spot mailbox depth",
    "owner": "spec/server/06-observability/01-runtime-monitoring"
  }
}
```

Reason code는 `NOT_APPLICABLE`, `PUBLIC_OBSERVATION_UNSUPPORTED`, `RUNTIME_METRIC_UNSUPPORTED`,
`CLOCK_DOMAIN_UNVERIFIED`, `NO_SAMPLES`, `HISTOGRAM_OVERFLOW`, `ZERO_DENOMINATOR`,
`MULTIPLE_OWNERS`, `PHASE_NOT_STARTED`, `COLLECTION_FAILED`다.
모든 null metric·histogram·window 값은 비어 있지 않은 reason을 갖는다.
DTO 자체가 nullable로 선언한 return 주소·reply·trigger reason은 그 field 주석의 조건을 따른다.
필수 관측의 수집 실패는 `COLLECTION_FAILED`와 셀 실패로 남기고 미지원으로 숨기지 않는다.

## 16. Server metrics endpoint

각 role은 application이 소유하는 admin HTTP endpoint를 제공한다. Framework 내장 URL이라는
뜻이 아니다. Endpoint는 public runtime status/reset과 application collector만 사용한다.
표준 runner의 transport는 HTTP이며 다른 admin transport는 이 schema의 표준 셀에 포함하지 않는다.

| Endpoint | Consumer·입력·관찰 결과 |
|---|---|
| `GET /perf/ready` | Script가 단계별 준비 evidence를 조회한다; workload를 시작하지 않는다 |
| `POST /perf/reset` | Coordinator가 drained 상태에서 `{runId,cellId,resetSeq}`를 전달한다 |
| `GET /perf/stats` | Collector가 현재 §15 PerfMetricsSnapshot을 받는다 |
| `POST /app/perf/start` | Application trigger client가 §15 PerfTriggerRequest로 phase를 시작한다; metrics URL과 별도 listener |

Application start는 수신 role의 측정 window를 열고 source role에서는 workload도 시작한다.
CS server도 window 시작을 받되 connector 부하는 CS client만 생성한다.
CS client process의 phase 제어는 script의 stdin/stdout JSON control pipe로 같은 Trigger DTO와
reset acknowledgement를 사용한다. 제어 메시지는 client application의 인터페이스이며 Framework API가 아니다.
Receiver들의 phase 시작을 확인한 뒤 source와 CS client를 시작한다. Coordinator의 같은
monotonic clock으로 각 trigger 발송·ack 수신을 기록해 시작 skew의 관찰 bound를 남긴다.
공통 clock domain 근거가 없으면 process 사이의 정확한 시작 차이는 null+reason이다.

### 16.1 Readiness

`GET /perf/ready`는 아래 공통 application DTO를 반환한다.

```text
PerfReady {
  runId: Text; cellId: Text; role: Text; roleInstance: Index
  infrastructureReady: boolean        // listener와 필요한 public topology가 준비됨
  objectsReady: boolean               // 이 셀의 public create/bind 결과가 확인됨; 비적용이면 true
  consumersReady: boolean             // PS는 모든 subscriber의 warmup marker evidence
  ready: boolean                      // 위 단계별 준비 조건의 conjunction
  observedAtUnixMs: I64               // 표기 전용
  evidence: Object[]                   // kind, source, observedValue; public status/typed reply/marker
  reasons: Text[]                     // 아직 준비되지 않은 조건
}
```

Server start는 infrastructureReady를 확인한다. Warmup은 infrastructureReady와 objectsReady 뒤에
시작하며, measured barrier는 consumersReady까지 확인한다. PS marker 이전에 ready=true를
요구해 warmup 자체를 막지 않는다.

확인할 topology와 object 준비는 [Runtime monitoring][monitor]과 [Spot 준비][spot-address]·
[Session bind][binding]를 참조한다. Host startup 성공만으로 ClientServer 후보가 준비됐다고
판정하지 않는다([Channel readiness][channel]). Subscriber는 public Ready와 실제 marker 수신
양쪽 evidence를 반환한다([Config 3][fanout]). Echo 셀은 준비된 각 대상에 probe echo 한 건도
확인하고 그 probe를 warmup 측정에 포함하지 않는다. PS는 echo 대신 위 marker를 사용한다.

### 16.2 Reset과 snapshot

Reset response는 `{ok:boolean,runId:Text,cellId:Text,role:Text,roleInstance:Index,resetSeq:U64,
applicationResetAtUnixMs:I64,capacityEpoch:U64|null,reason:Text|null}`다.
일치하는 resetSeq의 재요청은 저장한 acknowledgement를 반환하며 counter를 다시 지우지 않는다.
이전 seq, 다른 cell, active measured 구간의 reset은 HTTP 409와 비어 있지 않은 reason으로 거부한다.
잘못된 JSON·field type은 HTTP 400이며 state를 바꾸지 않는다. 성공 응답은 HTTP 200이다.

Application reset 완료 뒤 public capacity reset 결과를 관찰하고 acknowledgement에 epoch를 담는다.
Host runtime 없는 CS client의 capacityEpoch는 null이며 비적용 사유를 기록한다.
이것을 모든 process에서 같은 순간 발생한 reset이라고 표시하지 않는다.
Capacity reset의 보존 값과 epoch 의미는 [Runtime metrics §3][metrics]를 참조한다.

Public status/reset의 exact interface는 [.NET][d-status], [C++][c-status], [Java][j-status],
[Kotlin][k-status], [Node.js][n-status]가 소유한다.
Framework가 없는 trigger client는 application/process 값만 기록한다.

Snapshot에는 요청한 셀·resetSeq와 phase, window/settle counter·histogram, public capacity와
process 수치가 함께 들어간다. Measured 중 원본 수집은 저비용 sampler에 맡기고 HTTP snapshot
직렬화·histogram 합산은 report phase에서 수행한다. Admin endpoint failure는 수집 실패로 남긴다.

### 16.3 관측 비용

- **Hot path는 application counter와 latency recorder만 갱신한다.** 매 message마다 HTTP,
  console 출력이나 public status 전체 조회를 하면 부하 생성·관측이 병목이 되기 때문이다.
- **수신 evidence의 비용도 process 자원에 포함한다.** PS의 sequence 검증 비용을 결과에서
  숨기면 subscriber 측 처리량을 같은 조건으로 비교할 수 없기 때문이다.

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
|-- ZLink.Framework.Perf.ActorCallerServer/
|-- ZLink.Framework.Perf.PublisherServer/
|-- ZLink.Framework.Perf.SubscriberServer/
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
|-- actor-caller-server/
|-- publisher-server/
|-- subscriber-server/
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
|-- actor-caller-server/
|-- publisher-server/
|-- subscriber-server/
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
|-- actor-caller-server/
|-- publisher-server/
|-- subscriber-server/
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
|-- actor_caller_server/
|-- publisher_server/
|-- subscriber_server/
`-- scripts/
    |-- run_perf.sh
    |-- run_single.sh
    `-- collect_env.sh
```

C++ 구현은 release build 산출물을 사용한다. core runtime 또는 bindings runtime 경로가 source보다
오래되면 runner가 실패해야 한다. 성능 수치를 오래된 runtime으로 해석하면 안 된다.

## 18. 구현 순서

언어별 runner는 아래 순서로 같은 공개 호출과 결과를 맞춘다. 이 순서는 구현 의존 관계이며
실행·성능 검증이 끝났다는 뜻이 아니다.

1. Typed JSON DTO, cell schema, clock·histogram·error mapping과 격리 script를 작성한다.
2. Public status 기반 readiness, reset barrier, application trigger와 window/settle 기록을 연결한다.
3. `session-echo-only`로 connector·phase·집계를 확인한다.
4. `cs-local-session-actor-echo`, `cs-remote-session-actor-echo`를 구성한다.
5. `channel-echo-only`의 RouteMesh와 ClientServer 필수 셀을 구성한다.
6. `spot-no-await-echo` local 셀을 구성하고 local baseline 참조도 같은 결과를 사용한다.
7. Channel→Spot request와 send/send를 구성한다.
8. Spot→Channel request의 ordinary/Yield × SpotId 1/16과 send/send를 구성한다.
9. Worker ordinary/Yield, ActorId no-bind request와 send/send를 구성한다.
10. Classic fanout의 subscriber readiness와 sequence 원본을 연결한다.
11. `run_perf.sh`에서 필수 matrix와 §22의 interface 관찰을 확인한다.
12. 다른 언어의 공개 호출·결과 schema와 대조하고 선언 차이는 계약 소유자가 검토한다.

§23은 workload manifest와 운영 환경이 정해진 별도 실험이다. §2.1 후보는 승인된 범위가
확정된 뒤 별도 설계하며 표준 구현의 숨은 선행 조건으로 만들지 않는다.

## 19. 회귀와 비교 기준

같은 환경·시나리오·payload·terminal·배치의 결과를 비교한다.
임의의 최초 성능 threshold로 판정하지 않고 처리량·p99·오류·자원 사용량을 함께 남긴다.

`provenance`에는 commit hash, Core/binding/Framework version·artifact hash·실제 load 경로·build
mode, CPU model·effective processor·quota·cpuset·executor maximum, memory limit, OS·kernel·container,
file descriptor limit, role PID·host·endpoint, serializer 이름·version·설정과 clock 근거를 기록한다.
Config에는 실제 connection/stream 수와 분할, payload, duration, warmup, inflight, deadline,
Spot/Actor mapping, topology/discovery, worker 설정을 남긴다.

| Result status | 의미·비교 사용 |
|---|---|
| `valid` | 필요한 원본과 cohort 결산이 완전하며 workload 결과를 관측함 |
| `failed` | Public call·validation·phase·수집 실패를 관측; 원인과 count를 보존 |
| `invalid` | 분모 0, 준비 기준 미달, 서로 다른 schema 등으로 해당 비교의 전제가 성립하지 않음 |
| `unsupported` | 필요한 public 호출/선언을 해당 언어에서 확인·실행하지 못함; 필수 완료 수에 포함하지 않음 |

성공 echo baseline은 `valid`이고 실패·timeout·cancelled·unresolved·validation 오류가 0인 셀만
`baselineEligible=true`로 채택한다. PS는 lossless 계약이 아니므로 누락만으로 error를 만들지 않으며,
유효한 publish 분모와 subscriber 원본이 있으면 ratio를 보존한 채 비교한다.
PS baseline 채택의 허용 deliveryRatio는 비교 계획에 명시하며 미지정이면 `baselineEligible=false`다.
오류가 있는 수치를 무오류 baseline으로 저장하지 않는다.

Client CPU 포화, subscriber evidence 비용, clock 오차, null metric과 histogram 양자화는 summary에
함께 표시한다. Loss가 있는 PS 결과나 §23 포화 결과를 일반 echo의 완료 보장으로 해석하지 않는다.

## 20. 운영체제와 실행 환경

- **Port 0의 public listener 조회 또는 검증된 port 예약을 사용한다.** Sample/E2E의 포트
  구간과 충돌하지 않도록 하며 예시 숫자를 perf 전용 범위로 간주하지 않기 때문이다.
- **Store가 필요한 run은 전용 Docker Redis container와 cell별 namespace를 사용한다.**
  Local Actor/Spot과 automatic discovery도 [Store 계약][mesh]을 사용하고 독립된 데이터가
  필요하기 때문이다. Host Redis fallback은 허용하지 않는다. Run의 Redis는 Store가 필요한
  첫 셀 전에 한 번 준비하고 셀 사이에는 유지하며 run 종료·중단 때 정확한 container ID로 정리한다.
- **Cleanup은 이번 run의 정확한 PID/process handle·container ID만 대상으로 한다.**
  동시에 실행 중인 sample/E2E나 사용자 process를 정리하지 않기 위해서다.
- **Java/Kotlin은 repository의 build-only lock을 공유한다.** 같은 build root의 산출물 충돌을
  막되 실행 process의 수명까지 lock을 유지하지 않기 때문이다([격리 기준][sample-isolation]).

Container image digest, container ID, 실제 mapped port, provider version과 namespace를 config에 남긴다.
각 cell은 새 application process를 사용하고 Store namespace를 재사용하지 않는다.
Store 없는 manual Channel/session baseline을 실행할 때만 Docker 의존성이 없다.
Location Store와 Relocation Store의 계약을 합치지 않는다([Location][g-store], [Relocation][g-relocation-store]).

Script는 FD limit, ephemeral port 범위, listen backlog, TCP TIME_WAIT 정책, CPU·memory 제한,
client/server의 host 배치를 확인하고 부족한 항목을 preflight 실패로 보고한다. 전역 OS 값을 자동
변경하지 않는다. 같은 host의 loopback 결과는 CPU 경쟁을 포함한 결과로 기록한다.
다른 host 배치는 §15.2의 clock 근거와 함께 남긴다.

Readiness·liveness·shutdown은 [Channel][channel]·[liveness][liveness]·[host shutdown][relocation]
계약을 참조한다. Perf는 liveness 5초/15초나 ClientServer ready 대기 cap을 조정하지 않는다.
PAUSED 상태와 not-ready 관찰을 별도로 기록하며 포화 중 연결 변화는 계약과 대조한다.
Timeout 확대·manual reconnect·전파 sleep으로 그 결과를 가리지 않는다.

## 21. 금지 사항

- **Public API의 완료를 다른 계층에서 보상하지 않는다.** Raw socket·private runtime·reflection,
  두 번째 poller, retry/reconnect state를 넣으면 소유 계약과 다른 경로를 측정하기 때문이다.
- **Measured 중 per-message log나 내부 tracing으로 필수 metric을 만들지 않는다.**
  관측 비용과 public 경계가 바뀌기 때문이다. 필요하면 [message-flow tracing][flow]을 켠
  별도 진단 셀의 파일 로그를 보존하고 표준 성능 수치와 분리한다.
- **반환 payload 검증과 실패 기록을 생략하지 않는다.** 잘못된 echo나 실패를 성공 latency에
  넣으면 완료 성능을 비교할 수 없기 때문이다.
- **비교 중 timeout·budget·retry 횟수와 workload를 바꾸지 않는다.** 조건을 바꾸어 사라진
  실패는 원인 해결을 증명하지 않기 때문이다.
- **Turn 비교에는 공유 mutable 업무 상태의 대기 전후 가정을 넣지 않는다.**
  [Yield 계약][turn]과 무관한 application 경쟁이 결과에 섞이지 않아야 하기 때문이다.
- **Debug build·오래된 runtime·fake backend 결과를 release baseline으로 표시하지 않는다.**
  §19의 실제 실행 환경이 비교 근거이기 때문이다.

## 22. 완료 기준

Runner CLI, application admin/trigger JSON, typed reply·handler evidence, public status/reset,
저장된 결과 파일만으로 다음을 확인한다. 내부 queue·permit·transport 구현 상태는 판정하지 않는다.

**Matrix와 public 호출**

- `run_perf.sh`를 실행하면 §8.4의 각 이름에 1024/4096 셀 결과가 있고 §10.5의 네 비교 셀이 각각 조회된다.
- `channel-echo-only`를 실행하면 RouteMesh와 ClientServer 결과에 서로 다른 topology와 source/target PID가 기록된다.
- `session-echo-only`와 local Spot 기준을 실행하면 §11의 완료 경계가 기록되고 local Spot은 §10.7 결과 한 개를 참조한다.
- CS 요청을 보내면 같은 identity·payload의 original STREAM reply가 관측되고 setup의 create/bind 시간은 echo histogram에 포함되지 않는다.
- No-bind Actor 셀을 실행하면 config의 global ActorId와 public request/send 결과가 기록되고 session bind evidence는 없다.
- Request와 send/send 입력을 같게 실행하면 config의 logical in-flight 기준이 같고 send admission과 echo completion이 별도 metric으로 보인다.
- Worker 셀을 실행하면 public worker 반환값의 checksum·iterations·실제 callback 시간과 선택한 ordinary/Yield 값이 기록된다.

**Phase와 결과**

- Warmup 잔여 call이 있는 상태에서 reset을 요청하면 measured 시작 acknowledgement가 관측되지 않는다.
- 모든 참여자가 같은 resetSeq를 반환한 뒤 measured를 시작하면 각 원본에서 그 identity와 monotonic window가 조회된다.
- Window 종료 뒤 완료된 echo는 settle count·histogram에서만 증가하고 throughput의 완료 수는 바뀌지 않는다.
- 유효 reply·public failure·expiry가 같은 correlation에서 관측돼도 최종 결과 한 건만 결산되며 추가 reply는 별도 counter로 남는다.
- 필수 parameter의 consumer가 없는 셀이나 잘못된 mode·codec 입력은 preflight 오류를 반환한다.
- 서로 다른 셀을 실행하면 독립된 config/result/original 파일이 생성되고 같은 셀 경로 재사용 요청은 덮어쓰기 오류를 반환한다.
- 원본 histogram을 합산하면 §15.3의 count·sum·max와 percentile estimate가 일치하며 overflow percentile은 null 사유가 보인다.
- Public으로 관측할 수 없는 metric은 null과 구체적 reason을 반환하고 host aggregate가 Spot mailbox 값으로 표시되지 않는다.
- Window·setup·collection 실패가 발생하면 summary에서 source file, 실제 public kind 또는 harness 이유와 count를 조회할 수 있다.

**Fanout·격리·capacity**

- PS warmup 뒤 각 Subscriber의 Ready와 marker 수신 evidence를 조회할 수 있다.
- PS 결과를 수집하면 Publisher window 성공 sequence와 subscriber별 unique 교차 집합으로 ratio가 재계산되고 echo KOPS는 null이다.
- 공통 clock domain 근거가 없는 process의 one-way latency는 null이지만 delivery count와 ratio는 계속 조회된다.
- Store가 필요한 run의 config에서 Docker container ID와 cell namespace를 조회할 수 있고 cleanup 후 그 run의 소유 자원만 종료돼 있다.
- §23 manifest를 실행하면 public capacity snapshot과 outcome이 topology별 결과로 기록되며 내부 permit handoff 판정은 포함되지 않는다.

## 23. Core HWM과 Application job queue 운영값 측정

이 절은 운영 workload에서 Core byte budget과 Framework host queue 설정 후보를 비교한다.
설정·상한·pressure 의미는 [Framework API][api], [Application job queue][queue],
[Runtime monitoring][monitor]·[Runtime metrics][metrics]가 소유한다.
Perf는 public 설정값과 snapshot·완료 evidence를 기록한다.

### 23.1 고정할 workload와 CPU matrix

`--workload-config`는 이 실험의 모든 workload 값을 소유한다. 같은 값을 CLI와 manifest에
중복 지정하면 거부한다. Manifest에는 다음 입력과 consumer를 명시한다.

| Manifest 입력 | Consumer |
|---|---|
| `scenario`, `payloadDistribution`, `requestOneWayRatio` | Application workload generator; packet kind·logical bytes 비율 |
| `logicalStreams` 또는 `connections`, `inflight`, `ratePerSecond`, `burstRatePerSecond`, `burstDurationMs` | 해당 CS/source generator; steady/burst 부하 |
| `handlerCpuWork`, `handlerIoWork` | Public application handler/worker; 고정한 CPU·I/O 비율 |
| `warmupSeconds=30`, `measuredSeconds=60`, `repetitions=5`, 각 deadline·settle 값 | Phase owner; 명시적인 입력값으로 기록 |
| `requestedProcessors=[4,8,16]`, `cpuQuota`, `cpuset`, `executorMaximum` | Process/container 실행과 public executor 설정 |
| `memoryLimitBytes`, `runtimeOptions`, `gcOptions` | Process/container 실행 |
| `coreProfiles`, `coreBudgetCandidatesBytes`, `applicationQueueProfiles`, `manualQueueCandidates` | Public host configuration builder |
| `pauseThresholdPercent`, `resumeThresholdPercent` | Public application queue 설정; 기본 80/60과 실제 값 기록 |
| `capacitySampleIntervalMs=100` | Application의 public status sampler |
| `targetCpuRange`, `minThroughput`, `maxP99Ms`, `maxDeadlineMissRatio`, `maxMemoryBytes`, `lossPolicy`, `minDeliveryRatio`, `safetyMargin` | Report의 운영 후보 판정; `minDeliveryRatio`는 PS만 |

모든 수치의 단위·유효 범위를 manifest에 고정하고 적용할 public 옵션은 exact interface를 참조한다.
`ratePerSecond`는 logical operations/sec, burst duration은 monotonic milliseconds다.
`lossPolicy`는 `lossless` 또는 `observe-delivery`이며 Classic fanout은 후자를 사용한다.

Requested CPU 수와 함께 runtime constrained count, affinity/cpuset count, quota/period에서 얻은
count와 executor maximum을 기록한다. 실제 effective processor 수는 [API 소유 계약][api]의
public status에서 확인하며 단순 vCPU 표기와 혼동하지 않는다.
아래는 해당 effective 값일 때 Auto profile의 관측 기대다.

| Effective processors | Compact | LowLatency | Balanced | Throughput |
|---:|---:|---:|---:|---:|
| 4 | 128 | 256 | 512 | 1024 |
| 8 | 256 | 512 | 1024 | 2048 |
| 16 | 512 | 1024 | 2048 | 4096 |

Manual `1`, `2147483647`과 invalid `0`·음수·표현 범위 초과의 설정 결과는
[API validation 계약][api]의 공개 startup 결과로 확인한다. 내부의 socket bind 전 순서를
perf에서 계측하지 않는다. Core manual/profile 우선순위도 public effective snapshot으로 확인한다.

### 23.2 측정 phase와 reset 기준

각 반복은 §4의 warmup drain→reset acknowledgement→measured→settle 순서를 따른다.
최소 30초 warmup과 60초 measured를 5회 실행하며 steady/burst 구간은 manifest에 고정한다.
변동계수가 5%를 넘으면 불안정 결과로 표시하고 원본을 보존한다. 같은 실행의 duration·timeout을
늘려 통과 결과로 바꾸지 않는다.

Reset 전후의 public capacity snapshot에서 configured 값, current gauge·pressure state·current
pause duration, epoch·peak·transition·cumulative pause·config-failure 값을 기록한다.
보존·재기준화·초기화 의미는 [Runtime metrics §3][metrics]와 대조한다.
Concurrent event를 내부에서 어느 epoch에 넣었는지 perf가 추정하지 않는다.

Exact job queue wait p50/p95/p99는 §14.1대로 null이다. Sender RTT를 이 값으로 복사하지 않는다.
Sampler로 얻은 queue gauge 분포는 sample 간격과 `sampled=true`를 함께 표시한다.

### 23.3 Core HWM budget 선택

Auto profile과 manifest의 유한 양수 budget 후보를 같은 workload에서 비교한다.
Public snapshot의 configured/effective budget, applied HWM, accounted·completion accounted,
blocked ratio, active directional queues와 process RSS/heap, outcome·latency를 기록한다.
ABI reserved field를 활성 운영값으로 해석하지 않으며 field 의미는 [monitoring][monitor]을 참조한다.
Core budget과 process RSS는 서로 다른 값으로 판정한다.

후보 선택은 manifest의 throughput·latency·memory 조건을 만족하는 가장 작은 budget에
운영자가 명시한 margin을 적용한다. Completion 결과는 topology별로 해석한다.
RouteMesh의 별도 Completion connection과 ClientServer의 single FIFO·HWM·PAUSED 차이는
[Queue §3][queue]·[ClientServer §5][clientserver]에 대조하며 동일한 progress 보장을 요구하지 않는다.

### 23.4 Application job queue manual 상한 선택

Manual 후보는 public status가 제공하는 permits-in-use sample 분포와 peak를 근거로 정한다.
값의 정의는 [Application job queue][queue]를 참조하며 active handler·async wait 수를 더하지 않는다.

```text
candidateMaxQueuedApplicationJobs =
    sampled permits-in-use percentile or observed burst peak
    + operator-selected safety margin
```

후보별로 effective limit, reserved·queued·in-use·peak, capacity waits·duration,
pressure state·pause/resume threshold·current/cumulative pause·transition·flow-state config failure를
원래 이름·단위로 기록한다. Public handler 시작·완료·오류 evidence와 함께 해석한다.

Shared capacity wait, owner FIFO 포화와 worker capacity의 [오류·pressure 경계][queue]를
“HWM error” 하나로 합치지 않는다. Permit 반환마다 가장 오래된 source가 선택되는지,
batch/1:N 예약과 누수가 없는지의 내부 불변식은 소유 contract test의 책임이다.
Public gauge가 정상 범위인 snapshot만으로 그 내부 불변식을 증명했다고 보고하지 않는다.

### 23.5 Pass condition

운영 후보의 판정은 §22의 공개 interface 관찰과 manifest의 threshold를 사용한다.
Steady와 burst 각각 throughput·p99·deadline miss·memory 조건을 만족해야 한다.
`lossless` workload에서만 missing/duplicate application delivery 0을 요구한다.
Classic fanout은 ratio·subscriber별 count와 명시한 minDeliveryRatio로 판정한다.

Observed public in-use/peak가 effective limit과 일치하는 범위인지 기록하고 capacity wait·pressure
counter를 함께 보존한다. 내부 permit leak·source handoff 검증을 perf pass condition으로
대체하지 않는다. 수집한 `zlink.host.core_hwm.*`, `zlink.host.application_job_queue.*`의 이름·
단위·label은 [소유 metric 계약][metrics]에 맞춰 보존한다.

### 23.6 결과에 기록할 값

- Workload manifest bytes/hash, 셀별 repetition, 실제 CPU·memory·runtime·GC 설정
- Requested/effective processors와 constrained count·cpuset·quota·executor maximum 근거
- Public Core/queue configured·effective 값과 sample interval·count·peak
- Pause/resume threshold, pressure state, current/epoch pause·transition·config-failure 값
- Outcome·완료율·latency·RTT, null인 exact queue wait의 사유, topology별 해석
- Role별 process 자원, source/subscriber evidence, 선택한 candidate·margin·threshold 판정

[문서 목록][docs] · [공통 스펙][common] · [Scenario E2E][e2e]

[docs]: ../../../../../framework/doc/README.ko.md
[common]: ../../../../../framework/doc/framework/common/README.ko.md
[e2e]: ../../../../../framework/doc/framework/common/e2e/README.ko.md
[submit]: ../../../../../framework/doc/framework/common/spec/server/01-execution/01-submit-and-completion.ko.md
[turn]: ../../../../../framework/doc/framework/common/spec/server/01-execution/02-handler-turn-and-execution-gate.ko.md
[queue]: ../../../../../framework/doc/framework/common/spec/server/01-execution/04-application-job-queue-and-backpressure.ko.md
[errors]: ../../../../../framework/doc/framework/common/spec/server/00-foundation/07-framework-error-model.ko.md
[api]: ../../../../../framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md
[monitor]: ../../../../../framework/doc/framework/common/spec/server/06-observability/01-runtime-monitoring.ko.md
[metrics]: ../../../../../framework/doc/framework/common/spec/server/06-observability/02-runtime-metrics.ko.md
[flow]: ../../../../../framework/doc/framework/common/spec/server/06-observability/03-message-flow-tracing.ko.md
[session]: ../../../../../framework/doc/framework/common/spec/server/04-session/01-stream-session.ko.md
[binding]: ../../../../../framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md
[channel]: ../../../../../framework/doc/framework/common/spec/server/02-channel-transport/02-channel-messaging.ko.md
[clientserver]: ../../../../../framework/doc/framework/common/spec/server/02-channel-transport/03-client-server-channel.ko.md
[listener]: ../../../../../framework/doc/framework/common/spec/server/02-channel-transport/04-network-listener-identity.ko.md
[liveness]: ../../../../../framework/doc/framework/common/spec/server/02-channel-transport/05-transport-liveness.ko.md
[spot]: ../../../../../framework/doc/framework/common/spec/server/03-spot-actor/02-spot-messaging.ko.md
[spot-address]: ../../../../../framework/doc/framework/common/spec/server/03-spot-actor/06-spot-address-messaging.ko.md
[mesh]: ../../../../../framework/doc/framework/common/spec/server/03-spot-actor/03-mesh-node.ko.md
[actor]: ../../../../../framework/doc/framework/common/spec/server/03-spot-actor/04-actor-model.ko.md
[relocation]: ../../../../../framework/doc/framework/common/spec/server/05-location-relocation/05-host-relocation-flow.ko.md
[fanout]: ../../../../../framework/doc/framework/common/e2e/config-3-pubsub.ko.md
[sample-isolation]: ../../../../../framework/doc/framework/common/sample/README.ko.md
[connector]: ../../../../../framework/doc/framework/common/spec/stream-connector/32-stream-connector.ko.md
[core-socket]: ../../../../../core/doc/spec/core/socket/README.ko.md
[binding-async]: ../../../../../bindings/doc/spec/async-execution-model.ko.md
[http-dotnet]: ../../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/02-configuration-host.ko.md
[http-cpp]: ../../../../../framework/doc/framework/common/spec/server/languages/cpp/60-http-hosting.ko.md
[g-turn]: ../../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md#spot-turn
[g-spot]: ../../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md#spot
[g-spot-id]: ../../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md#spot-id
[g-execution]: ../../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md#user-spot-execution-mode
[g-fanout]: ../../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md#classic-fanout
[g-store]: ../../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md#location-store
[g-relocation-store]: ../../../../../framework/doc/framework/common/spec/server/00-foundation/02-glossary.ko.md#relocation-store
[d-session]: ../../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md
[d-channel]: ../../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/04-channel-messaging.ko.md
[d-spot]: ../../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/05-spots.ko.md
[d-actor]: ../../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/06-actors.ko.md
[d-common]: ../../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/01-common-runtime.ko.md
[d-config]: ../../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/03-configuration-topology.ko.md
[d-status]: ../../../../../framework/doc/framework/common/spec/server/languages/dotnet/interfaces/10-topology-monitoring.ko.md
[c-session]: ../../../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/06-stream-session.ko.md
[c-channel]: ../../../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/03-channel-messaging.ko.md
[c-spot]: ../../../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/04-spots.ko.md
[c-actor]: ../../../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md
[c-common]: ../../../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/01-common-runtime.ko.md
[c-status]: ../../../../../framework/doc/framework/common/spec/server/languages/cpp/interfaces/08-monitoring.ko.md
[j-session]: ../../../../../framework/doc/framework/common/spec/server/languages/java/interfaces/stream-session.ko.md
[j-channel]: ../../../../../framework/doc/framework/common/spec/server/languages/java/interfaces/channel-messaging.ko.md
[j-spot]: ../../../../../framework/doc/framework/common/spec/server/languages/java/interfaces/spots.ko.md
[j-actor]: ../../../../../framework/doc/framework/common/spec/server/languages/java/interfaces/actors.ko.md
[j-config]: ../../../../../framework/doc/framework/common/spec/server/languages/java/interfaces/configuration-host.ko.md
[j-status]: ../../../../../framework/doc/framework/common/spec/server/languages/java/interfaces/monitoring.ko.md
[k-session]: ../../../../../framework/doc/framework/common/spec/server/languages/kotlin/interfaces/stream-session.ko.md
[k-channel]: ../../../../../framework/doc/framework/common/spec/server/languages/kotlin/interfaces/channel-messaging.ko.md
[k-spot]: ../../../../../framework/doc/framework/common/spec/server/languages/kotlin/interfaces/spots.ko.md
[k-actor]: ../../../../../framework/doc/framework/common/spec/server/languages/kotlin/interfaces/actors.ko.md
[k-status]: ../../../../../framework/doc/framework/common/spec/server/languages/kotlin/interfaces/monitoring.ko.md
[n-session]: ../../../../../framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md
[n-channel]: ../../../../../framework/doc/framework/common/spec/server/languages/node/interfaces/02-channel-messaging.ko.md
[n-spot]: ../../../../../framework/doc/framework/common/spec/server/languages/node/interfaces/04-spots.ko.md
[n-actor]: ../../../../../framework/doc/framework/common/spec/server/languages/node/interfaces/05-actors.ko.md
[n-worker]: ../../../../../framework/doc/framework/common/spec/server/languages/node/interfaces/06-stream-worker.ko.md
[n-status]: ../../../../../framework/doc/framework/common/spec/server/languages/node/interfaces/03-location-observability.ko.md
[d-connector]: ../../../../../framework/doc/framework/common/spec/stream-connector/languages/dotnet/03-stream-connector.ko.md
[c-connector]: ../../../../../framework/doc/framework/common/spec/stream-connector/languages/cpp/03-stream-connector.ko.md
[j-connector]: ../../../../../framework/doc/framework/common/spec/stream-connector/languages/java/03-stream-connector.ko.md
[n-connector]: ../../../../../framework/doc/framework/common/spec/stream-connector/languages/typescript/03-stream-connector.ko.md
