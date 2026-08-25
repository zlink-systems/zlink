# 06-framework-api 재작성 대장

> 캠페인: [spec/server 재구성 캠페인](../../README.ko.md) · 주제: [00-foundation](mapping.ko.md)
> 대상 옛 문서: `framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md` (871줄)
> 새 문서: [`00-foundation/06-framework-api.ko.md`](../../../../../framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md)

이 대장은 [매핑표](mapping.ko.md) §3.5·§5(R108~R240)를 근거로, 재작성 뒤 각 규칙이 새 문서의
어느 절에 있는지 기록한다. 새 절 제목 23개는 매핑표 §3.5를 그대로 따랐다.

## R# → 새 위치

| R# | 새 위치 | 비고 |
|---|---|---|
| R108 | §1 | Public contract/domain contract에 binding type 비노출; runtime 내부 socket/queue/dispatch table/adapter는 비공개 |
| R109 | §2 | Root 등록 12항목 표(RouteMesh/ClientServer/fanout/STREAM node/Location Store/Relocation Store/codec/handler·filter/worker/network identity/deployment identity/inbound dispatch) |
| R110 | §2 | 같은 root 중복 구성·MeshName 중복 등록은 startup 오류; 같은 ChannelName을 다른 topology에 등록해도 오류 |
| R111 | §2 | Root는 process당 runtime singleton 1개; host 전체 Relocate(mode 필수)와 별도 Shutdown; PlannedMaintenance/RollingUpdate; drain 없음 |
| R112 | §2 | Framework builder는 liveness interval/deadline 비공개; 공통 profile로 orderly disconnect vs half-open 구분 |
| R113 | §3 | Core context가 messaging budget 소유; 옵션 3종 |
| R114 | §3 | manual budget 우선; hard limit 초과 또는 비양수는 startup 오류; managed/native binding hint |
| R115 | §3 | Core queue 전달 후 byte charge 종료; retained receive 미사용 |
| R116 | §3 | Application job queue permit 옵션 5개 |
| R117 | §3 | manual 범위 위반은 startup 오류; effective processor 수 계산 |
| R118 | §3 | Profile별 processor당 job 수; overflow는 startup 오류 |
| R119 | §3 | pause/resume permit 계산; resume<pause 검증 |
| R120 | §3 | pre-receive supply permit 미사용; ordinary ingress shared permit 획득 규칙 |
| R121 | §3 | 상한 도달 시 cancellable 대기; reject/drop/LWM 금지 |
| R122 | §3 | Framework→Core feedback은 RUNNING/PAUSED뿐 |
| R123 | §3 | SessionRelocationSealTimeout 기본 3,000 ms |
| R124 | §3 | Relocation payload 직접 전송 설정 4개 |
| R125 | §3 | chunk가 frame 한도 초과 시 startup 오류; 예산 계상 기준 |
| R126 | §4 | RouteMesh 등록 = MeshName → MeshNode builder; 소유 항목 목록 |
| R127 | §4 | MeshName/ChannelName 관계; startup 후 불변 |
| R128 | §4 | RouteCacheMaxAge/MessageFollowDuration 기본값과 관계 |
| R129 | §4 | Channel Client/Server 역할 구분 |
| R130 | §4 | Channel Server weight 범위·기본값; SetWeight(0) |
| R131 | §4 | Object role 닫힌 값; factory 등록 규칙 |
| R132 | §4 | Object Client outbound-only 규칙 |
| R133 | §4 | Object Server placement weight |
| R134 | §4 | Actor/Spot limit 기본값과 범위 |
| R135 | §4 | 상한 판정 = active+reserved slot |
| R136 | §4 | Activation concurrency 기본값 |
| R137 | §4 | typed bundle 단위 capacity 예약 |
| R138 | §4 | 3종 weight 범위·계산 규칙 |
| R139 | §4 | Create call target RID 미제공 |
| R140 | §4 | MaxMessageSize=0 의미 |
| R141 | §4 | ClientServer listener MaxMessageSize 기본값 |
| R142 | §4 | StreamNode Core STREAM inbound 상한 |
| R143 | §4 | MeshNode builder drain policy 없음; 상태 정의 |
| R144 | §5 | Manual peer 2가지 intent |
| R145 | §6 | 메시징 API family 7행 표 |
| R146 | §6 | Node direct·channel selection+submit 1콜 |
| R147 | §6 | Channel client NotFound/Unavailable 구분 |
| R148 | §6 | Logical Multicast owner MeshNode 선택 |
| R149 | §6 | Application 호출은 raw Message 대신 업무 객체 |
| R150 | §7 | Call operation 6항목 |
| R151 | §7 | one-way/publish/STREAM reply async-only admission |
| R152 | §7 | Metadata immutable snapshot 규칙 |
| R153 | §8 | Logical Multicast 완료: bounded I/O executor admission |
| R154 | §8 | target별 결과 미반영; snapshot 0개도 정상 완료 |
| R155 | §9 | Handler key = owner+message kind 6행 표 |
| R156 | §9 | 공유 base context와 종류별 context |
| R157 | §9 | Runtime reflection/compile-time 등록 |
| R158 | §10 | Handler filter 적용 대상 6행 표 |
| R159 | §10 | Filter context dispatch 종류 5값 |
| R160 | §10 | Filter 실행 순서, next 최대 1회 |
| R161 | §10 | next 미호출 시 결과 3행 표 |
| R162 | §10 | Filter는 request reply 직접 생성 불가 |
| R163 | §10 | Handler 실행마다 새 scope |
| R164 | §10 | Classic fanout 다중 일치 시 별도 dispatch·scope |
| R165 | §11 | Handler 실행 객체 소유 범위 3행 표 |
| R166 | §11 | 별도 handler class 언어 activation당 1회 생성 |
| R167 | §11 | Spot member function 표현 언어 규칙 |
| R168 | §11 | Spot/Actor handler dependency scope |
| R169 | §11 | 복구 대상 state는 Spot/Actor 소유 |
| R170 | §11 | Activation 종료 시 dispatch 차단 순서 |
| R171 | §11 | Mailbox 한도 두 축 모두 강제 |
| R172 | §11 | Byte 회계 = payload+metadata+고정비용 |
| R173 | §11 | 두 축은 하나의 작업으로 예약 |
| R174 | §11 | owner의 scheduler 연속 점유 시간 상한 |
| R175 | §11 | Scheduler 도착 기반 wakeup; infrastructure 작업 영역 |
| R176 | §12 | JSON 기본 codec |
| R177 | §12 | codec extension content-type 등록 규칙 |
| R178 | §12 | Registry 검사·canonical form |
| R179 | §12 | wire canonical form만 기록; 불일치는 ProtocolError |
| R180 | §12 | HTTP client media type parameter 처리 |
| R181 | §12 | 송신 fallback JSON; 수신 불일치는 ProtocolError |
| R182 | §12 | 송신 codec 선택 입력은 선언 type |
| R183 | §12 | 여러 조건 동시 충족 시 등록 순서 늦은 것 우선 |
| R184 | §12 | 송신 선택 결과 캐시 최대 1,024개 |
| R185 | §12 | Node.js/C++ 언어별 declared type 처리 |
| R186 | §12 | 송신 기본값과 수신 검증은 다른 경계 |
| R187 | §12 | Codec은 payload bytes 변환만 |
| R188 | §12 | 언어별 codec 등록 표면 5행 표 |
| R189 | §12 | 두 등록 표면 같은 계약 투영 |
| R190 | §13 | location store 명시 등록 대상 |
| R191 | §13 | Redis connection·key prefix 설정 위치 |
| R192 | §13 | Object role None+manual peer만은 store 불요; Relocation Store 필수 조건 |
| R193 | §13 | Location provider capability 부재 시 startup 오류 |
| R194 | §13 | Location/Relocation Store interface 비상속 |
| R195 | §14 | Classic fanout 독립 channel 등록 |
| R196 | §14 | Automatic/manual subscriber 연결 규칙 |
| R197 | §14 | Automatic subscriber·RID allocation publisher store 필요 |
| R198 | §14 | Fanout handler namespace는 packet name 구분 |
| R199 | §14 | fanout liveness 예약 topic byte |
| R200 | §14 | Manual subscriber endpoint 연결 handle |
| R201 | §14 | Endpoint 없는 automatic subscriber 상태 관찰 |
| R202 | §14 | Fanout publish 완료 = local transport 수락 |
| R203 | §14 | Publish 공통 입력과 topic 생략 편의 호출 |
| R204 | §14 | Fanout publish 비동기 terminator 1개 |
| R205 | §15 | Spot·Actor factory 등록; type UTF-8 name |
| R206 | §15 | Entry Spot ID 형식 발급 규칙 |
| R207 | §15 | Entry Spot ID 충돌 처리; Instance Spot Actor 기능 없음 |
| R208 | §15 | Actor manager/User Spot manager Create/GetOrCreate/Find |
| R209 | §15 | Initial Mesh 선택 규칙 |
| R210 | §16 | Missing Instance Spot: source가 owner claim 안 만듦; target 저장·확인 순서 |
| R211 | §16 | 경쟁 승리 runtime만 factory 실행; barrier·Ready commit |
| R212 | §16 | Recovery pointer CAS 제거 조건 |
| R213 | §17 | Create/GetOrCreate 결과 구분; CAS 패배자 처리 |
| R214 | §17 | Actor creation callback 결과 4종 |
| R215 | §17 | Terminal record semantic envelope, TTL |
| R216 | §17 | relocation policy 3종 선택 규칙 |
| R217 | §17 | Relocation ID 등 application 비노출 |
| R218 | §17 | ActorRef/SpotRef 정의; Destroy/Close 규칙 |
| R219 | §17 | Manager Find; Location query paging |
| R220 | §18 | Actor factory/handler mailbox dispatch |
| R221 | §18 | Yield terminator 제공 범위 |
| R222 | §18 | SpotWide Yield gate 반납 범위 |
| R223 | §18 | Spot direct 시작 method와 Instance intent |
| R224 | §18 | STREAM node 독립 등록 |
| R225 | §19 | 공통 13개 ErrorKind 사용 |
| R226 | §20 | Operation 결과 변환: RID/global ID/binding token 유지 |
| R227 | §20 | select-one 시작 경계 |
| R228 | §20 | Operation 결과 변환 8행 표 |
| R229 | §20 | DeadlineExceeded/cancellation/exceptional completion 구분 |
| R230 | §20 | STREAM reply token 원자적 소비 |
| R231 | §20 | Direct pending one-way operation target 확정 경계 |
| R232 | §20 | Global object message 결과 구분 7행 표 |
| R233 | §20 | Create·GetOrCreate 오류 kind 구분 |
| R234 | §20 | request 실패 확인 시점 무관 1회 완료 |
| R235 | §20 | Request admission 뒤 terminal 결과 종류 |
| R236 | §21 | Dispatch 실패 record owner는 26-message-flow-tracing |
| R237 | §22 | Startup validation 21항목 목록 |
| R238 | §22 | 설정 오류는 lazy first call까지 미루지 않음 |
| R239 | §23 | Runtime query 반환 항목 |
| R240 | §23 | Monitoring event 제공 필드 |

행이 없거나 표에 없는 보장이 새 문서에 있으면 대조 실패다(가이드 §2.5). 위 133개 행(R108~R240) 모두
새 문서의 정확히 한 절에 배치했다 — 배치하지 못한 R#은 없다.

## §12 분리 근거 (S7)

옛 §12(Spot, Actor와 STREAM owner)는 120줄 문단 벽이었다. 매핑표 §4 S7의 분리 계획대로 독자 질문
기준 4절로 나눴다.

| 새 절 | 질문 | 담은 R# |
|---|---|---|
| §15 User·Instance Spot과 Actor factory 등록 | 무엇을 등록하는가 | R205~R209 |
| §16 Missing object 생성 — cold activation 순서 | 없는 객체를 어떻게 만드는가 | R210~R212 |
| §17 Create·GetOrCreate 결과와 relocation policy | 생성 결과는 어떻게 갈리는가 | R213~R219 |
| §18 `Yield`와 STREAM/Actor 등록 마무리 | 그 외 등록을 어떻게 마무리하는가 | R220~R224 |

§16에는 guide §7.2(세 주체 이상의 순서, 정상/실패 분기)에 따라 두 target이 같은 Missing Instance
Spot message를 동시에 받았을 때 한쪽만 reservation을 확보하는 경쟁 흐름을 Korean-label
sequence diagram으로 추가했다.

## S6 처리 — 옵션 표를 인라인 주석으로

옛 §2.1에 있던 옵션 설명 표 4개(가이드 §8.3 위반)를 전부 `contract pseudocode` 선언 블록 +
줄마다 인라인 주석으로 바꿨다(새 §3). 숫자·기본값·범위는 전부 보존했고 값 손실은 없다.

| 옛 표 | 새 형태 |
|---|---|
| `설정 \| 의미` (`CoreHwmMemoryLimitBytes`/`CoreHwmBudgetBytes`/`CoreHwmProfile`) | `RootInboundDispatchOptions` pseudocode 블록 1 |
| `설정 \| 의미` (`ApplicationJobQueueProfile`/`MaxQueuedApplicationJobs`/Pause·Resume/`EffectiveMaxQueuedApplicationJobs`) | `RootInboundDispatchOptions` pseudocode 블록 2 |
| `Profile \| Jobs per effective processor` | `Compact -> 32` 등 4행 text 블록 |
| `설정 \| 기본값 \| 적용 범위와 의미` (Relocation payload 4종) | `RootLocationOptions` pseudocode 블록 |

## 코드가 검색하는 문장

세 곳이 이 문서를 경로로 열어 문장을 needle로 검색한다. 아래 needle은 모두 원문 그대로
새 문서에 character-identical하게 남아 있음을 grep으로 확인했다.

### `framework/languages/cpp/tests/Zlink.Framework.ContractTests/test_cpp_framework_layout_contract.cpp`

`framework_api_documents_actor_destroy_lifecycle`가 raw substring(`std::string::find`, 공백
정규화 없음)으로 검색하는 문장 4개 — 전부 새 §15·§17·§20에 character-identical하게 있다.

- `Object Server의 Entry Spot, user Spot, typed Actor와 actor-free Instance Spot factory` — §15
- `` `PreserveStateWith`는 Actor factory에 `` — §17
- `` `ActorRelocationAdapter`, User·Instance Spot factory에 `SpotRelocationAdapter`를 지정한다. `` — §17
- `` | exact ActorRef destroy | idempotent `false` | `Unavailable` | `InvalidOperation` | `Unavailable` | `` — §20

### `scripts/verify-framework-submit-api.sh` (`--contract` 모드)

raw substring(공백 정규화 없음)으로 검색하는 문장 3개 — 전부 새 §8·§20에 character-identical하게
있다. 이 스크립트는 현재 옛 경로(`framework/doc/framework/common/spec/server/00-foundation/06-framework-api.ko.md`)를
읽으므로, 이 새 문서로의 실제 검증은 문서 이동(캠페인 §5) 시점에 경로를 갱신한 뒤 이뤄진다.

- `one-way send·publish는 결과값 없이 정상 완료` — §20 (Operation 결과 변환 표)
- `Target별 수락·실패 결과는 public publish 결과로 반환하거나 publish 전용 monitoring 값으로 집계하지` — §8 (Logical Multicast 완료)
- `` `ShuttingDown` `` — §20 (Operation 결과 변환 표; §8에도 등장하지 않음, 오류 kind 소개는 §19가 링크로 위임)

### `scripts/verify-framework-instance-spot-contracts.sh` (`--check`/`--self-test` 모드)

`06-framework-api.ko.md` fixture가 검색하는 문장 10개 — 공백을 정규화(`\s+` → 단일 space)한 뒤
비교하므로 줄바꿈 위치는 영향이 없다. 전부 새 §15~§17·§20에서 정규화 비교로 확인했다(이 스크립트도
현재 옛 경로를 읽으며, 문서 이동 시점에 경로 갱신이 필요하다).

- `actor-free Instance Spot factory` — §15
- `Instance Spot은 actor-free lifecycle을 사용하며 Actor handler, Actor membership과 Logical Multicast subscription을 등록할 수 없다.` — §15
- `` Actor manager와 User Spot manager는 global ID를 받는 `Create`, `GetOrCreate`, `Find` family를 제공한다. `` — §15
- `Instance Spot은 manager create family를 제공하지 않는다.` — §15
- `Instance intent를 명시한 경우에만 Missing authority의 cold activation을 시작한다.` — §15
- `선택한 Mesh의 serving descriptor에 등록된 distinct Instance type이 하나일 때만 자동 선택한다.` — §15
- `Source는 owner claim이나 reservation을 먼저 만들지 않는다.` — §16
- `확보한 runtime만 factory와 initialize를 실행하고, activation envelope의 message를 durable activation inbox의 첫 record로 확정한다.` — §16
- `Public object handle, directory, resolver와 unbounded list는 제공하지 않는다.` — §17
- `| exact SpotRef close |` — §20

## 이동 후 갱신할 링크

새 문서는 아직 옮기지 않은 다른 주제 문서를 `../<NN-slug>.ko.md` 형태(한 단계 위, 옛 전역 번호
그대로)로 링크한다. 그 주제가 재구성되어 이동하면 아래 링크를 새 경로·새 절 anchor로 갱신해야
한다(캠페인 §5.3 anchor 치환표 대상).

| 옛 경로 (현재 링크 형태) | 몇 곳 | 새 문서 절 | 예상 주제 |
|---|---:|---|---|
| `../05-async-execution-policy.ko.md` | 1 | §7 | 01-execution |
| `../50-internal-message-ownership.ko.md` | 1 | §3 | 01-execution |
| `../33-core-hwm-application-job-flow.ko.md` | 1 | §3 | 01-execution |
| `../10-network-listener-identity.ko.md` | 1 | §2 | 02-channel-transport |
| `../29-transport-liveness.ko.md` | 2 | §2, §14 | 02-channel-transport |
| `../08-channel-messaging.ko.md#6-classic-fanout과의-경계` | 1 | §14 | 02-channel-transport |
| `../24-runtime-monitoring.ko.md` | 1 | §14 | 06-observability |
| `../26-message-flow-tracing.ko.md#3-공통-attribute` | 1 | §21 | 06-observability |
| `../30-host-relocation-flow.ko.md` | 1 | §2 | 05-location-relocation |
| `../28-relocation-flow.ko.md` | 1 | §3 | 05-location-relocation |
| `../22-location-store-redis.ko.md` | 1 | §13 | 05-location-relocation |
| `../23-relocation-store-redis.ko.md` | 1 | §13 | 05-location-relocation |
| `../21-location-runtime.ko.md` | 1 | §13 | 05-location-relocation |

같은 주제 안(`02-glossary.ko.md#anchor`, `05-message-model.ko.md`, `07-framework-error-model.ko.md`)과
`../languages/...`·`../../stream-connector/...`(옮기지 않는 디렉터리) 링크는 이미 최종 경로다 —
추가 갱신이 필요 없다.

## spec-gap 후보

이번 재작성에서는 매핑표 §4 S6·S7·S8을 그대로 적용했을 뿐, 새로운 spec-gap 후보를 추가로 찾지
못했다. 매핑표 §5 spec-gap 후보 중 이 문서와 관련된 두 건(G-F2, G-F3)은 여전히 유효하다 —
자세한 내용은 [`mapping.ko.md` §5 spec-gap 후보](mapping.ko.md)를 따른다.

- **G-F2** §3(Core memory budget·Application job queue)의 permit 획득/반환 순서, mailbox 2축 회계,
  batch/1:N dispatch 규칙(R120~R122)이 01-execution 주제로 예정된
  `33-core-hwm-application-job-flow.ko.md`가 이미 상세히 소유하는 내용과 상당히 겹친다. 00-foundation과
  01-execution 경계를 어디서 그을지는 01-execution 매핑표 작성 시점에 판정 필요(코디네이터 확인).
- **G-F3** §22(Startup validation) 목록과 `08-layering.ko.md` §5(등록 선언 검증)가 "시작 시점에만
  검증, 통과 후 불변" 원칙을 서로 다른 각도(공개 계약 목록 vs 구조 원칙)에서 말한다. 모순은
  아니므로 spec-gap이 아니라 링크 여부만 판단하면 된다.
