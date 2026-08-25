# 재작성 대장 — 01-spot-model · 02-spot-messaging · 03-mesh-node

> 옛 `11-spot-model.ko.md`(242 규칙), `12-spot-messaging.ko.md`(228 규칙),
> `13-mesh-node.ko.md`(151 규칙)를 재작성한 대장이다. [매핑표](mapping.ko.md) §5의
> `11-spot-model-R#`, `12-spot-messaging-R#`, `13-mesh-node-R#` id를 그대로 쓴다. 연속된
> R#가 같은 절로 갔으면 범위로 묶었다. 새 위치는 새 문서의 `##`/`###` 절 번호다. 각 행은
> grep으로 핵심 문구를 새 문서에서 실물 확인한 뒤에만 적었다(가이드 §2.5).
>
> 이 문서가 처리한 구조 문제: S1, S2, S3, S5, S9, S11, S13, S14(매핑표 §4). S4, S6~S8,
> S10, S12, S15~S20은 다른 문서(04-actor-model, 05-spot-actor-membership,
> 06-spot-address-messaging 등) 담당이라 이 대장에서 다루지 않는다.

## 01-spot-model.ko.md

| 규칙 ID | 새 위치 | 비고 |
|---|---|---|
| R1 | §1 Spot 모델 개요 | 세 Spot 공통점·차이점 정의 |
| R2–R4 | §1 | 다른 문서 소유 계약 링크(메시징=02, callback 순서=05, ID 발급=03, 생성·주소=06) — S2로 Entry Spot ID 소유를 03으로 명확화 |
| R5–R7 | §2 | 세 Spot이 준비되는 시점 (mermaid) |
| R8–R45 | §3 공통점과 차이점 (표) | 15행 비교표 전체 유지. R14(Entry Spot ID 발급)는 "형식과 충돌 처리 규칙은 MeshNode가 정의한다" 링크로 축소(S2) |
| R46 | §3 | Host relocation 공통 단계 링크(30-host-relocation-flow) |
| R47–R48 | §3 | Direct packet/timer는 Spot queue, Actor payload는 Actor queue |
| R49–R71 | §3.1 Relocation 중 temporary queue | 12단계 전부 유지(dispatch 순서, cutover, generation 검사) |
| R72–R84 | §3.1 (계속) | queue/execution mode 그림, Spot instance는 state 미소유, handler scope |
| R85–R96 | §3.2 Spot 종류별 lifecycle callback (표) | Configure~OnCreateActorAsync 9개 callback 표 전체 |
| R97–R101 | §3.3 Actor membership callback은 source/target 분리 | join 승인·commit 순서, 05-spot-actor-membership §4 링크 |
| R102 | §3.3 | 위와 동일 링크 |
| R103–R109 | §3.4 Spot instance 종료 callback | OnClosingAsync 종료 이유 표 4행 |
| R110–R111 | §3.4 | membership 잔존 시 close 미호출, standalone Actor 이동은 Entry Spot 미종료 |
| R112–R120 | §4.1 Object Server의 Actor 진입점 | 초기화 시점, "공개"로 어휘 교체(S14). ID 형식·충돌·mapping 상세(옛 R115–R120)는 **03-mesh-node §3.2로 이관(S2)** — §4.1은 "Framework가 발급한다. 형식·충돌 처리는 MeshNode가 정의" 한 문장으로 축소 |
| R121–R123 | §4.1 | Actor 생성·initial membership·Ready barrier, 업무 message는 Actor queue 직접 전달 |
| R124–R142 | §4.2 Entry Spot의 Actor lifecycle | 3상황 표, OnCreateActorAsync 승인/거절, command 44 sessionRelocationRoute 전체 절차 유지(이 절 자체는 S4/S17 소관 밖이라 원문 그대로 보존) |
| R143–R147 | §4.3 Entry Spot 자체는 이동하지 않는다 | relocation participant 아님, target 준비, OnClosing 미호출 |
| R148–R173 | §5 User Spot, §5.1 SpotWide relocation 경계 | Create/GetOrCreate, execution mode, Yield, FrameworkManaged/ApplicationSignaled, 처리 owner 표, RelocationReady().Defer() |
| R174 | §5.1 | exact generation 검사는 06-spot-address-messaging 링크 |
| R175–R209 | §5.2 User Spot lifecycle | lifecycle 순서, SpotWide/PerActor relocation, command 44 세션 갱신 전체 |
| R210 | §5.2 | command 44 one-way, 재전송 journal 없음 |
| R197–R213 | §6 Instance Spot, §6.1 lifecycle | 제공/미제공 기능, cold activation 문서 링크(06), close, IdleEvicted |
| R214–R219 | §6.2 쓰지 않고 남아 있는 Instance Spot 정리 | "유휴"→"쓰지 않고 남아 있는"(S14) 어휘 교체, 정리 조건 2가지, IdleEvicted 처리, NotFound |
| R220–R226 | §7 .NET에서 보이는 차이 | registration method, Context 계약, interface 발췌 |
| R227 | §7 | .NET Spot interface 링크 |
| R228–R232 | §8 문서 경계 (표) | 02/03/05/06/30 소유 계약 표, 새 슬러그로 갱신 |
| R233–R242 | §9 검증 요구 | Entry Spot ID 형식·replacement·충돌 검증 3건은 **03-mesh-node §10으로 이관(S2)** — 01 §9는 초기화 공개 시점, User/Instance 생성 제약, membership/Logical Multicast 지원 여부, Actor payload 경로, relocation 범위만 유지 |

## 02-spot-messaging.ko.md

| 규칙 ID | 새 위치 | 비고 |
|---|---|---|
| R1–R9 | §1 Spot 메시징 개요 (표) | Spot direct/Logical Multicast 대상 결정 표, weight·ready 조건 |
| R100–R115(§1.1 예시) | §1.1 .NET API 예시 | `IZLinkSpotClient` 등 interface 발췌 그대로 유지(계약 아님, 참고 예시) |
| R10–R16 | §2.1 Spot을 식별하는 값 | Spot ID 형식, MeshName/kind/stable type 중복 금지, Entry Spot ID 발급은 "형식은 MeshNode가 정의"로 링크(S2) |
| R17–R21 | §2.2 Object Client와 Object Server 역할 (표) | None/Client/Server 3행 표 |
| R22–R24 | §2.3 물리 연결 | ROUTER 공유, target RID 등 비노출 값 |
| R24(계속) | §2.3 | Spot 생성 개념 문단 + 정확한 절차는 06-spot-address-messaging 링크(S1) |
| R25–R26 | §2.4 Classic fanout과의 경계 | PUB/SUB 별도 기능, 물리 연결·구독 상태 미공유 |
| R27–R31 | §3.1 Ready Spot의 owner를 찾는 방법 | **positive route cache 조회 개념 한 문단만 유지, 정확한 field·수명·무효화는 08-routing 링크(S3)**; authority·ObjectGeneration·Ready 정의는 유지 |
| R32–R37 | §3.1 (계속) | owner fence, generation은 stale cache 구분용, 재생성 Spot 처리. **2차 리뷰 확인**: R37은 288행 "current Ready Spot에 payload를 넣는다"로 실물 확인(false alarm, rewritten wording) |
| R38–R51 | §3.2 Instance Spot이 없을 때 새로 준비하기 | **cold activation 11단계 절차(mermaid + 산문 + §3.4의 8단계 목록, 세 벌 중복)를 3~5문장 개념 설명 + [06-spot-address-messaging](06-spot-address-messaging.ko.md) 링크로 축소(S1)**. Instance intent·stable type·MeshName 지정, Call 형태 표는 유지(대상 선택 개념) |
| R45 | §3.2 (계속) | **2차 리뷰에서 누락 확인, 추가함** — "여러 MeshNode가 같은 type을 등록했어도 하나의 type으로 센다" 문장을 자동 선택 서술 뒤에 grep 확인 가능한 원문 그대로 삽입(R46의 "둘 이상이면 명시해야 한다"도 함께 복원) |
| R52–R66 | → **06-spot-address-messaging.ko.md** (S1) | activation envelope 구성요소 열거, 생성 권한 확보·durable inbox 확정·barrier 개방 절차, recovery scan은 06이 소유. 02는 링크만. **2차 리뷰 확인**: R53(envelope 구성요소 — operation identity, source node RID·lifecycle generation, optional source Spot ID, reply correlation, deadline, target descriptor fence, command 39 metadata)은 06 §4(299–300행) `select:` 문단에 grep으로 실물 확인함 — 위임 확정, 02는 원문 유지하지 않음 |
| R79–R81 | §3.2 비규범적 .NET 예시 | `InstanceSpot`/`InMesh`/`Timeout` 호출 형태는 Spot 메시징 자신의 계약(call surface)이므로 유지, mermaid diagram은 제거 |
| R67–R78 | → **06-spot-address-messaging.ko.md** (S1) | recovery pointer CAS 제거, process 재시작 복원, Serving gate 개방 순서. **2차 리뷰 확인**: R75("다른 target이 생성 권한을 먼저 확보했다면 현재 target은 Spot을 만들지 않는다")는 06 §4 577행에 rewritten wording으로 실물 확인 — 위임 확정. **R72(같은 reservation·generation으로 factory/initialize/durable inbox 복원 재개 또는 fence로 중단)와 R73(Ready commit 뒤 queue 선두 복원 전 종료 시 recovery root·cursor로 최초 record 우선 복원)은 06에서도 grep으로 찾지 못함 — 02·06 어느 문서에도 없는 진짜 손실.** 06은 다른 에이전트 소관이라 이 대장에서 직접 고치지 않고 spec-gap 후보로 등록(아래) |
| R82–R91 | §3.3 Spot direct send의 완료 의미 | Async만 제공, timeout·오류 kind, activation envelope 보존 정보 표(7행) — 완료 의미 설명은 이 문서 소관 |
| R92–R98 | §3.4 Spot direct의 공통 보장 | 재전송 없음, 중복 실행 처리 책임, Instance Spot 별도 create 없음 |
| R99 | → **06-spot-address-messaging.ko.md** (S1) | 8단계 처리 순서(옛 §3.4)는 §3.2와 동일 절차의 세 번째 중복본이라 제거, 02는 "정확한 순서는 06이 정의" 한 문장으로 대체. **2차 리뷰 확인**: 06 §4 mermaid sequence diagram(287–357행 부근, durable inbox 첫 record 확정→barrier 개방→recovery pointer 제거까지)에서 8단계 전부 실물 확인 — 위임 확정 |
| R100–R102 | §3.5 Spot에서 Channel 호출 | ChannelName 송신 경로 선택, 중계 없음. **2차 리뷰 확인**: R101은 410행에 그대로 실물 확인(false alarm) |
| R103–R109 | §3.6 Channel request의 실행 재개 (표 2개 + mermaid) | correlation 유지 정보, Async/Yield 처리, InvalidOperation 조건 |
| R110–R115 | §3.6 (계속) | Yield 제공/미제공 call 목록, reply 재전달 금지, 최종 결과 하나만 선택 |
| R116–R120 | §4.1 Target 범위 | (ChannelName, topic), 중복 등록 startup 실패 3가지. **2차 리뷰 확인**: R119는 520행 "다음과 같이 여러 송신 경로에 등록하면 host startup이 실패한다"로 실물 확인(false alarm, "다음처럼"→"다음과 같이" 표현만 다름) |
| R121–R129 | §4.2 Publish 처리 순서 | snapshot 고정, 5단계 처리, data 공유, 관측 미반환 |
| R130–R136 | §4.3 Publish 작업을 시작할 수 있는 조건 | worker 제한, DeadlineExceeded, ShuttingDown, 실패 미집계 |
| R137–R145 | §4.4 Publish가 시작된 이후의 처리 (표 + mermaid) | 시작 확정 시점, 부분 실패, rollback 없음 |
| R146–R150 | §4.5, §4.6 Publish 완료 | 0건도 정상 완료, 보장하지 않는 것(durable·replay·exactly-once), monitoring 미제공. **2차 리뷰 확인**: R144는 619–620행에 그대로 실물 확인(false alarm) |
| R151–R159 | §5.1, §5.2 Subscription 등록 값 | ChannelName/topic/packet name, 중복 등록 금지, Configure() 예시 |
| R155–R159 | §5.2 Spot 상태를 바꾸는 control 작업 | Spot control claim 정의, gate 공유/분리, 04-actor-model §4 링크 |
| R160–R179 | §5.3 Spot application queue에 들어가는 작업 (표 3행 + 포화 표 8행) | 큐 구분 표, 포화 결과 표는 **그대로 유지(S5, 최상위 README가 이 절을 기준 문서로 지정)** |
| R176–R179 | §5.3 (계속) | local/remote 구분 기준, control claim 별도 한도, "적체"→"밀림" 어휘 교체(S14). **2차 리뷰 확인**: R177("Spot control claim 작업은 application queue 한도를 공유하지 않는다")은 01-execution/04-application-job-queue-and-backpressure.ko.md에 없음(grep 무결과) — **위임되지 않았고 이 문서(02) 자신이 소유**. 748행 `**공유하지 않는다.**`(굵게 표시)로 이미 존재 — 리뷰의 plain-text grep이 markdown 강조 기호 때문에 놓친 false alarm |
| R180–R191 | §5.4, §5.5 Spot turn과 callback 순서 | turn 처리, Yield 우선순위, Framework 내부 작업 분리 목록. **2차 리뷰 확인**: R175는 739행, R187은 773행에 그대로 실물 확인(둘 다 false alarm) |
| R192–R199 | §6.1 Target과 request 실패 | Ready authority 없음, generation 불일치, handler 오류 reply. **2차 리뷰 확인**: R195는 812행 "오류 reply를 보내 request를 완료한다"로 실물 확인(false alarm, 문장 두 개를 하나로 합침) |
| R197–R199 | §6.2 Spot 종료 | drain deadline, subscription 제외 |
| R200 | §7.1 Metadata | 04-message-model 링크 |
| R201–R203 | §7.2 관측 정보 (표) | 6개 항목, publish 전용 미집계, topic/Spot ID label 금지 |
| R204–R226 | §8 검증 요구 (§8.1~§8.5) | 물리 연결, Missing Instance Spot(2건은 06 소유로 이관 표시), Channel 호출, Logical Multicast, Spot/Actor 전달. **2차 리뷰 확인**: R217은 887행 §8.3에 그대로 실물 확인(false alarm) |
| R227–R228 | §1.1 예시, §3.2 예시 | InMesh 생략 시 InvalidOperation, Timeout 단일 deadline — .NET 예시 주석에 반영 |

## 03-mesh-node.ko.md

| 규칙 ID | 새 위치 | 비고 |
|---|---|---|
| R1–R3 | §1 MeshNode 개요 | 메타 제목 "이 문서가 정의하는 범위"를 "MeshNode 개요"로 교체(S9), blockquote와 중복되던 서문 정리 |
| R4–R10 | §2 MeshNode가 가지는 identity와 설정 (표) | MeshName/RID/Endpoint/ChannelName set/Object role/generation/revision 7행 |
| R11–R17 | §2 (계속) | MeshName 비식별, 중복 등록 금지, "게시" → "공개"로 어휘 교체(S14) |
| R18–R23 | §3.1 Automatic discovery에서 사용하는 RID (표) | prefix/UUID/Full RID 계약, active conflict 즉시 실패 |
| R24–R30 | §3.2 Entry Spot ID | **Entry Spot ID 발급·형식·충돌 처리 전 규칙을 이 절이 단독 소유(S2)** — 01-spot-model §4.1·§9의 중복 문단을 이 절로 통합, "이 절이 Entry Spot ID의 발급·형식·충돌 처리 규칙을 소유한다" 문장 추가 |
| R31–R36 | §3.2 (계속) | Full ID 길이, "게시" → "공개"(S14), mapping 사용·parsing 금지 |
| R37–R38 | §3.3 Fixed RID | Location Store/automatic discovery 미사용 조건, 잘못된 조합 오류 |
| R39–R51 | §4 Object role과 등록할 수 있는 기능 (표 3행 + 산문) | None/Client/Server, peer connection 3규칙 |
| R52–R58 | §4 (계속) | factory 등록 예시, configure callback 동기 실행, stable type exact value, 중복 등록 오류 |
| R59–R65 | §5.1 Weight와 capacity (표 6행) | placement weight/Actor limit/Spot limit/type별 limit/Entry Spot/pending activation |
| R66–R74 | §5.1 (계속) | weight=0 제외, 기존 reservation 유지, capacity 우선 적용, overflow 방지 |
| R75–R80 | §5.2 Target node를 선택하는 조건 | caller 미지정 값, InMesh는 Mesh만 선택, 5단계 선택 조건 |
| R81–R85 | §6 등록과 startup 순서 | 5단계 startup 순서. Entry Spot 발급·초기화 시점을 2단계에 명시(01-spot-model §4.1과 정합) |
| R86–R88 | §6 (계속) | Location Store 등록 필수, Manual mode 계약 |
| R89–R94 | §7.1 Peer 연결 | handshake 교환 정보, admission 거부 조건, lifecycle generation은 opaque token, manual 재연결 3조건, automatic RID 작은 쪽 시작 |
| (다이어그램) | §7.1 mermaid | peer handshake·중복 connection 해소 sequence diagram 신설(S13) — 물리 connection 층만 그리고 §7.3 논리 target 선택과 분리함을 명시 |
| R95–R98 | §7.2 Channel weight 갱신 | weight 변경 시 revision만 증가, snapshot 적용, connection 재생성 없음 |
| R99–R104 | §7.3 메시징 방식별 target 선택 (표 5행) | Node direct/Channel/Logical Multicast/Actor direct/Spot direct, ObjectGeneration 사용처는 08-routing 링크 |
| R105–R113 | §7.3 (계속) | target 선택=submit 단일 operation, ROUTER 공유, Node direct 용도, ChannelName 사용 시점 |
| R114–R119 | §7.3 (계속) | application turn 직렬 처리, Core HWM 재시도, transport readiness에서 handler 미실행 |
| R120–R125 | §7.3 (계속) | handler namespace 구분, Logical Multicast node-local 검사, immutable storage 공유 |
| R126–R133 | §8 Drain과 종료 | Relocating 제외 대상, seal 후 Draining 전환, terminal까지 진행, Shutdown은 신규 relocation 미시작 |
| R134–R135 | §9 관측 정보 | snapshot·event 제공 항목, RID/endpoint는 metric label 미사용, 06-observability/01-runtime-monitoring 링크로 갱신 |
| R136–R151 | §10 검증 요구 | 16개 항목 전체 유지(startup 실패 조건, RID·Entry Spot ID 형식, weight·capacity, Channel weight, Logical Multicast, namespace, drain, target 미요구) |

## 나중에 anchor를 붙일 링크

같은 주제 안에서 병렬로 작성 중인 문서(다른 에이전트 담당)를 파일명만으로 링크했다.
해당 문서의 절 제목이 확정되면 아래에 anchor를 붙인다.

- `01-spot-model.ko.md` → `05-spot-actor-membership.ko.md` (§1, §3.3, §7 링크) — "Actor callback의 정확한 순서", "양방향 callback 비교와 정확한 commit 순서" 절 anchor
- `02-spot-messaging.ko.md` → `06-spot-address-messaging.ko.md` (§1, §2.3, §3.2, §3.3, §3.4, §8.2 링크) — cold activation 11단계 절차 절 anchor
- `02-spot-messaging.ko.md` → `08-routing.ko.md` (§1, §3.1, §3.4 링크) — positive route cache 절 anchor
- `02-spot-messaging.ko.md` → `04-actor-model.ko.md` (§5.2, §5.4 링크) — "Spot이 처리하는 Actor control" 절 anchor
- `03-mesh-node.ko.md` → `08-routing.ko.md` (§7.3 링크) — "ObjectGeneration을 어디에 쓰고 어디에 쓰지 않는가" 절 anchor(옛 §2.5, 새 절 번호 미확정)

## 이동 후 갱신할 링크

이 세 문서가 아직 옛 경로에 있는 다른 주제 문서를 링크하는 자리. 해당 주제가 이동하면
아래 상대 경로를 갱신해야 한다(§5 마지막 단계에서 en과 함께 일괄 처리).

- `../06-framework-api.ko.md` (01-spot-model §3.1) — 00-foundation 주제로 이동 예정
- `../30-host-relocation-flow.ko.md` (01-spot-model §3, §8 / 02-spot-messaging §6.2 / 03-mesh-node §8) — 05-location-relocation 주제로 이동 예정
- `../languages/dotnet/interfaces/05-spots.ko.md`, `../languages/dotnet/interfaces/01-common-runtime.ko.md`, `../languages/dotnet/interfaces/03-configuration-topology.ko.md` — `languages/` 트리는 이동하지 않는다고 §2가 명시했으므로 경로 변경 없음(확인용으로 기록)
- `../04-message-model.ko.md`, `../05-async-execution-policy.ko.md`, `../32-framework-error-model.ko.md`, `../10-network-listener-identity.ko.md`, `../07-channel-topology.ko.md` (02-spot-messaging, 03-mesh-node 전역) — 01-execution/02-channel-transport 주제로 이동 예정
- `../06-observability/01-runtime-monitoring.ko.md` (03-mesh-node §9) — 이미 이동 완료(06-observability), 최종 경로 확정됨 — 갱신 불필요로 확인됨

## spec-gap 후보

이번 재작성에서는 spec-gap을 새로 만들지 않았다(S1~S20 중 이 문서들이 처리한 항목은 전부
구조 재배치이며 규칙 내용을 바꾸지 않았다). S20(14/15의 `Failed`/`Aborted` leaf 라벨 불일치)은
04-actor-model·05-spot-actor-membership 담당 문서의 소관이라 이 대장에는 등록하지 않는다.

### 2차 리뷰(coordinator)에서 발견 — 문서 간 누락

- **옛 `12-spot-messaging-R72`, `R73`가 02와 06 어디에도 없다.** 두 규칙은
  "Target process가 `Reserve` 뒤 종료되면 startup의 complete authority scan이 Pending
  creation 정보를 다시 읽는다. 같은 reservation과 generation으로 factory, initialize와
  durable inbox 복원을 이어가거나 정확한 fence로 생성을 중단한다. `Ready` commit 뒤 queue
  선두를 복원하기 전에 종료되었다면 recovery root와 cursor로 최초 record를 먼저 복원한다"
  (옛 12-spot-messaging.ko.md 417–421행)는 cold activation 중 target process가 죽었다가
  재시작하는 recovery 절차다. S1에 따라 이 절차 전체는 `06-spot-address-messaging.ko.md`가
  소유해야 하지만, 그 문서를 grep한 결과(`재시작`·`restart`·`Pending creation`·`complete
  authority scan` 검색) 이 두 규칙에 해당하는 문장이 없다. `06-spot-address-messaging.ko.md`는
  다른 에이전트가 작성했고 이 대장이 다루는 파일 범위 밖이라 이 세션에서는 직접 고치지
  않았다 — 06 담당 에이전트 또는 코디네이터가 06 §4(cold activation 절)에 이 두 규칙을
  추가해야 한다. R75(같은 문단, 이웃 규칙)는 06에 있으므로 이 문단 전체가 아니라 R72·R73
  두 문장만 빠졌다.
