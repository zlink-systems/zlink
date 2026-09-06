# Framework perf spec 개정 사본 변경 기록

대상은 [한국어 전체 사본](README.ko.md)과 [영어 전체 사본](README.en.md)이다.
[승인된 fitness review](../perf-spec-fitness-review-summary.md)의 §7 수정안 18개, §6의
13→11 병합과 baseline 변경, §5 이름 소유권, §4.1–4.5의 실행·관측 결정을 반영했다.
감독의 추가 coverage 결정에 따라 `channel-echo-only`의 RouteMesh/ClientServer request 셀을
모두 필수로 지정했다. Runtime·runner 구현 변경이나 원본 spec 적용은 이 작업에 포함하지 않았다.

수정 전/후 규칙 수: **표준 scenario 13→11; Spot→Channel request loop의 규격 소유자 3→1**.
일반 terminal/Yield × SpotId 1/16의 네 비교 질문은 유지한다. Channel baseline의 두 topology는
표준 scenario 수와 별개이며, Spot local baseline은 동일 결과를 참조한다.

## 1. 수정안별 원문 위치와 반영

아래 line은 수정하지 않은 원본 파일 기준이다. 새 위치는 두 사본에서 같은 절 번호를 사용한다.
수정 이유의 § 번호는 별도 표기가 없으면 [fitness review](../perf-spec-fitness-review-summary.md)를 가리킨다.

| Amendment ID | Old lines (file:line) | 바뀐 내용·사본 위치 | 이유·근거 |
|---|---|---|---|
| A01 | `framework/doc/framework/common/perf/README.ko.md:7,533`; `framework/doc/framework/common/perf/README.en.md:7,564` | 서문·§2·§8.4에서 common spec이 scenario/payload/CLI/schema를 소유하고 언어 계획은 도구·metadata만 보완하도록 명시 | §7.1, §5: 오래된 언어 계획 이름·측정 계층을 표준 이름으로 가져오지 않음 |
| A02 | `framework/doc/framework/common/perf/README.ko.md:117,194,353`; `framework/doc/framework/common/perf/README.en.md:127,207,376` | §2·§5.1·§6.3·모든 scenario의 `spotIds`, public manager 준비, Object Client/Server의 Location Store, run 전용 Redis를 명시하고 Registry role 제거 | §7.2, §2, §4.5, E-SPOT: RID와 logical ID, provider와 executable의 소유권 구분 |
| A03 | `framework/doc/framework/common/perf/README.ko.md:24,685,701,738`; `framework/doc/framework/common/perf/README.en.md:26,725,742,782` | §1·§10.5·§10.8에서 ordinary/Yield와 SpotWide User Spot을 명시; Actor-free 비교와 application counter를 사용; §21–22에서 자동 turn 반납 가정 제거 | §7.3, §2, E-TURN: ordinary await와 Yield는 같은 실행 의미가 아님 |
| A04 | `framework/doc/framework/common/perf/README.ko.md:657,683,699`; `framework/doc/framework/common/perf/README.en.md:697,723,740` | §10.5 하나에 ordinary/Yield × SpotId 1/16을 통합; Spot 내부 remote-call completion을 primary로 고정; §8.4·§18·§22도 11개로 변경 | §7.4, §6: 같은 remote request loop와 집계 규칙의 소유자를 3→1로 줄임 |
| A05 | `framework/doc/framework/common/perf/README.ko.md:762,781,895`; `framework/doc/framework/common/perf/README.en.md:807,827,948` | §10.9–10.10에서 global ActorId direct request/send, 준비 단계 Ref와 messaging ID 분리; §14의 `actor.sourceAdmission.latency.*`로 개명 | §7.5, §2, E-ACTOR/E-SUBMIT: remote source admission을 local mailbox 완료로 오인하지 않음 |
| A06 | `framework/doc/framework/common/perf/README.ko.md:754,774,894`; `framework/doc/framework/common/perf/README.en.md:799,820,947` | §13–14에서 13개 public ErrorKind 정규화, language cancellation·harness 오류 분리, 결과 상호 배타성과 진단 count 구분 | §7.6, §2, E-ERROR: 내부 route/worker reason을 공개 kind로 만들어 요구하지 않음 |
| A07 | `framework/doc/framework/common/perf/README.ko.md:326,854`; `framework/doc/framework/common/perf/README.en.md:349,905` | §4.2·§8.2·§13에서 call 시작 전 slot 확보, admission 대기 포함, 한 logical operation당 하나의 결과; local driver와 native attempt를 별도 KOPS로 세지 않음 | §7.7, E-SUBMIT 및 Core/binding 소유 계약: physical retry/drain/reconnect를 runner가 재구현하지 않음 |
| A08 | `framework/doc/framework/common/perf/README.ko.md:108,185,559,840`; `framework/doc/framework/common/perf/README.en.md:118,199,592,890` | §4.2·§5·§16에서 CS physical connectors와 server logical streams 분리; server-driven은 application HTTP trigger, `/perf/*`는 admin; batchSize 제거; role config가 부하를 소유 | §7.8, §4.1: session 없는 ActorCaller/Publisher에 STREAM connector가 접속한다는 전제 제거 |
| A09 | `framework/doc/framework/common/perf/README.ko.md:645,672,781,836`; `framework/doc/framework/common/perf/README.en.md:684,712,827,886` | §10.4·10.6·10.10·§13·§15에서 항상 harness correlation 사용; caller 전용 returnChannel과 명시적 returnSpotId; send admission·echo 완료 분리 | §7.9, E-CHANNEL/E-SUBMIT: select-one return 대상과 correlation 종료의 소유자를 명확히 함 |
| A10 | `framework/doc/framework/common/perf/README.ko.md:883,897,905,921,1417`; `framework/doc/framework/common/perf/README.en.md:936,950,958,975,1493` | §14·§15.5·§23에서 내부 Spot/worker/exact queue wait를 null+reason; application counts와 callback 구간을 별도 명칭으로 정의 | §7.10, §4.2, E-METRIC: host aggregate·콜백 계측을 내부 metric으로 재명명하지 않음 |
| A11 | `framework/doc/framework/common/perf/README.ko.md:119,743,751`; `framework/doc/framework/common/perf/README.en.md:129,787,796` | §5.2·§10.8에서 self-contained CPU workload, checksum/timing 반환, pool min/max·queue·idle·executor 기록; sleep 대체 부하 제거 | §7.11, §4.1, E-TURN/N-TURN: Node callback 직렬화 계약을 포함한 공개 worker 사용과 비교 가능성 |
| A12 | `framework/doc/framework/common/perf/README.ko.md:91,93,919,1051`; `framework/doc/framework/common/perf/README.en.md:100,102,973,1106` | §4·§13·§15–16에서 warmup drain→resetSeq ACK barrier, monotonic window, 별도 settle bound/count/histogram, 중복·만료 결산을 정의 | §7.12, §4.3, E-SUBMIT: 원자적 동시 reset과 settle을 섞은 throughput 방지 |
| A13 | `framework/doc/framework/common/perf/README.ko.md:836,916,1097`; `framework/doc/framework/common/perf/README.en.md:886,969,1152` | §15.2에서 DTO type·64-bit decimal string·Base64 logical payload·ns ticks·clock evidence 정의; Unix는 표기 전용, 미검증 one-way latency null | §7.13, §4.3, liveness 시간원/N-STATUS: bigint 손실과 서로 다른 epoch 차감 방지 |
| A14 | `framework/doc/framework/common/perf/README.ko.md:946,1032,926`; `framework/doc/framework/common/perf/README.en.md:1001,1087,982` | §15에서 셀별 디렉터리와 원본, CS/S2S/AC/PS 집계 owner, non-cumulative bucket·nearest-rank upper-bound·overflow null·정확한 sum/count/max 정의 | §7.14, §4.3: 파일 덮어쓰기, trigger 중복 합산, percentile 평균·tail clipping 방지 |
| A15 | `framework/doc/framework/common/perf/README.ko.md:794,811,849,919`; `framework/doc/framework/common/perf/README.en.md:841,858,900,973` | §10.11·§14–15에서 Classic fanout, Publisher 단독 sequence, window publish 성공 분모와 subscriber 원본 교차 집계, publish/delivery 단위 분리; PS-A2를 packet-name handler 선택으로 정정 | §7.15, §4.3, E-FANOUT/E-SPOT: publish admission과 delivery 구분, 존재하지 않는 subscriber transport topic filter 전제 제거 |
| A16 | `framework/doc/framework/common/perf/README.ko.md:1340,1378,1398,1405`; `framework/doc/framework/common/perf/README.en.md:1411,1454,1474,1481` | §23에서 effective CPU, 80/60 pause/resume와 current/epoch 관측, reset 보존, topology별 completion, lossless-only zero-drop; 내부 permit/FIFO 증명은 contract test로 참조 | §7.16, E-QUEUE/E-METRIC/E-FANOUT: public 관찰 범위와 RouteMesh/ClientServer 차이를 유지 |
| A17 | `framework/doc/framework/common/perf/README.ko.md:87,576,1049,1278`; `framework/doc/framework/common/perf/README.en.md:96,612,1104,1340` | §4·§5.1·§9·§16·§20에서 port 0/검증된 예약, 시작 전 role config/후 endpoint manifest, owned PID/container cleanup, Docker Redis, shared build-only lock, public readiness와 고정 liveness 참조 | §7.17, §4.4–4.5: 병행 실행 격리와 준비 완료 evidence; timeout/reconnect 보상 금지 |
| A18 | `framework/doc/framework/common/perf/README.ko.md:821,1240,1310`; `framework/doc/framework/common/perf/README.en.md:869,1297,1379` | §11·§18·§22에서 session baseline부터 시작; connector-only를 표준 선행/CLI 집합에서 제거; spot-local은 §10.7 결과 참조만 유지 | §7.18, §6, E-SESSION: public-only로 불가능한 선행 조건과 중복 baseline 제거 |

**추가 승인 coverage:** §11.2에서 manual RouteMesh와 manual ClientServer request 셀을 모두 필수로
정의했다. 두 셀 모두 ObjectRole.None, 별도 source/target process, Store 불필요 조건과 언어별
공개 Channel 호출을 명시했다. 이는 review §3·§7의 범위 선택에 대한 감독의 명시적 결정이다.
Instance Spot cold/hot, Logical Multicast, relocation-under-load, .NET/C++ HTTP는 §2.1의
“후속 후보”에만 두었다. 필수 scenario나 baseline·완료 선행 조건으로 추가하지 않았다.

## 2. 구조·이름 대응

Top-level §1–23과 기존 §10.1–10.6 번호를 보존했다. 병합 뒤 번호는 다음과 같다.

| 원문 | 개정 사본 |
|---|---|
| §10.5, §10.7 `spot-async-request-echo`, §10.8 `spot-await-contention` | §10.5 `s2s-spot-to-channel-request-echo`의 네 셀 |
| §10.9 `spot-no-await-echo` | §10.7, local baseline의 단일 결과 owner |
| §10.10 `spot-worker-offload-echo` | §10.8 |
| §10.11/10.12 Actor no-bind | §10.9/10.10 |
| §10.13 fanout | §10.11 |
| §11 baseline 표 | §11.1 session, §11.2 Channel 두 셀, §11.3 Spot local 결과 참조 |
| §22 완료 기준 | 같은 번호 유지; 입력과 public reply/callback/status/JSON 결과의 관찰로 교체 |

Review §5의 역사적 이름 대응은 아래처럼 해석했다. 이 표는 옛 label을 CLI alias로 추가하는
규칙이 아니다. 동일 측정이라는 근거가 없는 기존 결과를 새 이름으로 재표기하지 않는다.

| 옛 label | 표준과의 관계 |
|---|---|
| `client_server_request_reply` | ClientServer `channel-echo-only` request와 대응; Session/Actor CS와 다름 |
| `client_server_send` | One-way admission/delivery 정의가 필요한 별도 과거 측정; 필수 request 셀로 rename하지 않음 |
| `fanout_publish_1`, `fanout_publish_n` | `pubsub-fanout-echo`의 subscriberCount 1/N과 대응 |
| `dealer_mesh_request_reply` | 공개 topology와 동등성 미확인; 역사적 label로만 보존 |
| `route_mesh_request_reply` | Spot을 추가하지 않은 RouteMesh `channel-echo-only` request |
| `route_mesh_send` | One-way 측정; request/send-send echo로 직접 rename하지 않음 |
| `stream_request_reply` | Actor 없는 `session-echo-only` |
| `stream_send`, `bound_session_send` | 필수 echo와 같은 완료가 아님; 일대일 대응 없음 |
| `stream_actor_relay` | Request completion과 배치를 확인한 뒤 local/remote Session→Actor 셀로 대응 |
| `spot_to_spot_send`, `spot_to_spot_request_reply` | Channel↔Spot과 caller/target owner가 달라 일대일 대응 없음 |
| `spot_to_router_egress` | 전송 방식 확인 뒤 Spot→Channel request 또는 send/send |
| `router_to_spot_ingress` | 전송 방식 확인 뒤 Channel→Spot request 또는 send/send |
| `http_handler_roundtrip` | 후속 HTTP 후보이며 필수 공통 이름으로 가져오지 않음 |

옛 64B/1KB/4KB/64KB matrix, fake backend, `run_benchmarks.sh`는 표준 1024/4096 public-process
`run_perf.sh`/`run_single.sh`와 별도 계층이다. 개정 사본 §2·§3·§8.4·§9·§21에서 이 소유권을
적용했다. 기존 언어 계획 파일은 수정하지 않았다.

## 3. SUPERVISOR-DECIDE: 검토서가 수치를 정하지 않은 초안 선택

아래는 적용 승인을 받은 runtime 설계가 아니다. 세부 값이 없는 runner 계약을 검토 가능하게
만들기 위한 구체적인 사본 선택이며, 감독은 원본 반영 전에 유지·변경 여부를 결정한다.
기존 계약의 오류나 구현 미지원을 이 값 조정으로 보상하지 않는다.

| ID | 위치 | 사본의 구체적 선택 | 결정이 필요한 이유 |
|---|---|---|---|
| SUPERVISOR-DECIDE-01 | §5·§5.2 | Server logicalStreams 기본 10000; request/expiry 1000ms, settle/admin 5000ms, setup 30000ms; 일반 셀 closed-loop | Review §4.1은 consumer·명시적 값 기록을 요구하지만 수치는 정하지 않았다. Runtime queue/CPU가 이 부하를 수락한다는 보장은 아니다 |
| SUPERVISOR-DECIDE-02 | §5.2·§10.8 | Worker min=max=pool size(기본 8), maxQueueLength 4096, idle 60000ms; xorshift32-v1 고정 CPU workload와 1024 iteration마다 시간/cancellation 확인; ordinary/Yield 모두 필수 | Review는 self-contained 작업과 effective options 기록을 요구했다. 공통 pool 수치·algorithm·ordinary 대조군 필수 여부는 미정이었다 |
| SUPERVISOR-DECIDE-03 | §15.2–15.3 | Schema v2, 모든 64-bit 정수 decimal string, payload Base64, ns ticks, 기존 0.1..1024ms bucket의 nearest-rank 상한 추정, overflow percentile null, arbitrary-precision sumNs | Review는 표현·추정·overflow를 확정하라고 했지만 그 형식은 선택하지 않았다. 원본 결과가 없으므로 migration은 만들지 않았다 |
| SUPERVISOR-DECIDE-04 | §5.1·§16 | 표준 admin은 HTTP 하나; application trigger와 metrics listener 분리; 수신 role도 phase trigger로 window 시작; CS child는 같은 DTO의 stdin/stdout JSON control pipe | HTTP application trigger 허용은 승인됐다. Admin 대체 transport 허용 여부·CS barrier의 IPC 표현·수신 window 시작 인터페이스는 미정이었다 |
| SUPERVISOR-DECIDE-05 | §11.2 | 필수 Channel baseline의 두 topology를 manual discovery·ObjectRole.None으로 고정 | ClientServer 필수 추가는 감독 결정이다. Automatic discovery까지 baseline에 포함할지는 미정이어서 transport 비교와 Store 필요 조건이 명확한 manual 셀을 택했다 |
| SUPERVISOR-DECIDE-06 | §15.1·§15.4 | 비교 입력 bytes의 전체 SHA-256을 variant에 포함; CS echo rate는 owner별 count/자기 monotonic duration의 합, application rate는 측정 role별 rate 합, 복수 primary owner의 단일 measuredSeconds는 null | 셀 충돌 방지와 집계 owner는 review가 정했다. Hash 표현·서로 다른 clock owner의 rate 합산 방식은 사본이 구체화했다 |
| SUPERVISOR-DECIDE-07 | §15.4 | Fanout ratio는 window admission 성공 sequence만 분모로 사용; settle publish 성공은 별도 기록; subscriber window/settle unique range와 timing evidence로 최종 교차 집계 | 분모 의미는 review §4.3을 그대로 적용했다. 재집계 가능한 range/timing 원본 표현과 evidence 비용 기록 방식은 미정이었다 |
| SUPERVISOR-DECIDE-08 | §12·§15.2·§19 | 모든 echo의 전체 payload pattern 검증; process RSS 100ms sampling; PS baseline 채택에는 비교 계획의 minDeliveryRatio 필요 | 검증 범위·sample 주기·초기 PS baseline 채택 정책의 구체값은 review가 정하지 않았다. 빠른 셀의 검증 비용과 threshold 선택을 감독이 확인해야 한다 |
| SUPERVISOR-DECIDE-09 | §4.2·§10.5–10.6·§13 | 같은 process의 public Spot driver로 handler를 진입하고 remote interval만 primary 집계; window 뒤 도착한 driver는 started=false; echo가 send terminal보다 빠르면 terminal까지 slot 유지 | Merge가 정한 primary 구간을 public API만으로 진입·계수하기 위한 application 절차다. Local driver 보조 구간과 early-echo 경계의 세부 표현은 review가 미정으로 남겼다 |
| SUPERVISOR-DECIDE-10 | §23 | Workload manifest에 rate/burst·CPU profile·기한·loss policy·목표치를 모음; 기본 30s warmup/60s 측정×5; CoV>5%면 unstable로 보존 | 실행 중 측정 시간을 늘리는 옛 규칙은 고정 phase와 충돌하므로 자동 연장하지 않는 선택이다. 운영값 선택은 이 문서가 새 runtime 기본값으로 정하지 않는다 |

## 4. SUPERVISOR-DECIDE: 공개 계약·선언 불일치

- **SUPERVISOR-DECIDE-11 — C++ Session relay.** Review §1.4가 이미 식별했다.
  `framework/doc/framework/common/spec/server/languages/cpp/interfaces/05-actors.ko.md:289`의
  `relay(message_t)`/`relay(dispatch,message_t)`와 실제 public header의 `relay_request` 표현이 다르다.
  사본 §10.1–10.2는 연결·bind·relay 경로와 원래 STREAM 완료 관찰을 요구하되 해당 exact
  호출의 정합성 확인을 명시한다. Contract owner가 맞는 선언을 확정하기 전 이를 구현 완료로
  세지 않는다. Perf에서 raw frame codec이나 private helper를 추가하는 안은 제시하지 않는다.
- **SUPERVISOR-DECIDE-12 — .NET original STREAM reply.** 추가 확인에서
  `framework/doc/framework/common/spec/server/languages/dotnet/interfaces/07-stream-session.ko.md:172`
  는 `RelayAsync`를 one-way source admission으로 설명하고 session의 명시적 `Client.Reply`를
  요구한다. 반면 `framework/doc/framework/common/spec/server/04-session/02-session-actor-binding.ko.md:628,728`
  은 Actor handler의 reply가 original STREAM correlation을 완료한다고 정의한다. Review D-CS와
  현재 sample은 bound relay 경로를 근거로 들었다. 두 소유 문서의 관계를 감독/contract owner가
  확정해야 한다. 사본은 relay admission을 Actor echo 완료로 세지 않고 original request의
  검증된 echo만 측정하며 이 불일치를 제한으로 표시한다. 원본 어느 쪽도 수정하지 않았다.

Node RouteClient의 Spot 메서드 선언 차이는 review §1.3에서 공개 `ZLINK_SPOT_OUTBOUND` DI
경로로 이미 해결 근거를 제시했다. 사본 §10.3 등의 Node 행도 `ZLinkSpotOutbound`를 사용한다.
Internal cast로 RouteClient를 넓히지 않는다. Node worker는 callback 반환값으로 관측치를 전달하고
captured collector나 별도 worker pool에 의존하지 않는다. 새 runtime API 제안은 없다.

## 5. Glossary 검토가 필요한 용어

[Spec writing guide §3.4](../../../principal/documentation/spec-writing-guide.ko.md)의 glossary-first
원칙에 따라 다음 신규 측정 용어를 먼저 목록화했다. 사용자 지시대로 glossary는 수정하지 않았다.
이는 perf harness 용어이며 기존 Spot/Actor/turn/queue의 새 runtime 개념으로 등록하지 않는다.

| 용어 | 사본의 사용·제안 정의 | Glossary 처리 제안 |
|---|---|---|
| 측정 셀 / measurement cell | §2: scenario·payload·terminal·배치의 한 독립 실행과 결과 | Perf 문맥의 범위를 표시한 entry |
| Logical stream | §4.2: server process가 public workload를 반복하는 독립 부하 흐름; physical connector와 다른 단위 | 기존 STREAM session/connector와 구분하는 entry |
| Logical operation | §13: public 측정 call 하나가 시작한 workload 완료 단위; admission 대기를 포함 | 기존 operation identity·submit/terminal 용어를 참조하는 perf 한정 entry |
| Measured cohort | §4.1: owner의 측정 window 안 시작한 logical operation 집합 | Window 완료와 settle 완료의 분모를 설명하는 perf 한정 entry |
| Reset barrier / resetSeq | §4.1·§16: 모든 참여자의 동일 reset acknowledgement 후 측정 시작; 원자적 reset 의미 없음 | Application phase 용어로 entry 또는 measured cohort의 설명에 통합 |
| Source admission latency | §14: public Actor send 시작부터 정상 source admission terminal까지의 application 구간 | 기존 submitted/admission 용어를 참조; remote mailbox 완료라는 새 정의를 만들지 않음 |
| Clock domain | §15.2: epoch·단위가 공유됨을 근거로 확인한 monotonic 시간 비교 범위 | 일반 측정 용어로서 entry 필요 여부 감독 판단 |

Histogram bucket/overflow/nearest rank, Base64, JSON pointer, CPU quota 등은 일반 기술 용어이며
새 제품 계약을 도입하지 않는다. DTO·metric field 이름은 §15의 harness schema 이름이다.
`Spot`, `Spot ID`, `Spot turn`, `User Spot execution mode`, `Classic fanout`, `Location Store`,
`Relocation Store`는 기존 glossary entry를 링크했다.

## 6. 적용·확인 범위

- 두 사본은 같은 scenario·절 번호·CLI 옵션·DTO field·metric key·결과 의미를 사용한다.
- Bold rule과 이유를 묶고, runtime 동작은 owner 문서를 링크했다. §22는 내부 queue·permit
  불변식의 증명 대신 public call 결과·typed handler evidence·status·JSON 원본을 관찰한다.
- 사본 위치에서 링크가 열리도록 owner 문서 경로를 `../../../../framework/...` 등으로
  rebase했다. 감독이 원본 perf 경로에 적용할 때 reference definition의 상대 경로를 다시 맞춰야 한다.
- §22를 마지막으로 옮기라는 writing-guide 일반 권고보다 사용자의 번호 안정성 요구를 우선하여
  §23 운영값 측정 절을 유지했다. 새 계획서나 runtime 설계 문서는 만들지 않았다.
- 확인은 Markdown 구조·링크 대상·한/영 schema 대응의 정적 점검을 수행했고 오류가 없었다.
  사용자 지시대로 build/test/benchmark와 상태 변경 git 명령은 실행하지 않았다.
