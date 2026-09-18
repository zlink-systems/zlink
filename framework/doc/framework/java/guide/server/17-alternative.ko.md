---
title: "17. ZLink의 적용 범위 — 내부 서비스 통신과 실시간 상태 서버 · Java"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/17-alternative.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# 17. ZLink의 적용 범위 — 내부 서비스 통신과 실시간 상태 서버

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Session 묶음의 동작 원리](39-session-binding.ko.md) | [다음: 12. 운영 — 런타임 메트릭 · graceful drain · readiness](12-operations.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/17-alternative.ko.md) · [C#/.NET](../../../dotnet/guide/server/17-alternative.ko.md) · **Java** · [Kotlin](../../../kotlin/guide/server/17-alternative.ko.md) · [Node/TypeScript](../../../node/guide/server/17-alternative.ko.md)
{ .zlink-langswitch }
<!-- language-switch:end -->

!!! info "이 장을 읽고 나면"

    ZLink를 도입할 자리인지 판단하고, 도입하지 않을 자리를 함께 가릴 수 있다.

ZLink는 서버 간·실시간 메시징 계층이다. 논리 channel, 연결 수명, Spot, pub/sub, 위치 기반
자동 연결을 한 framework가 함께 제공한다. 내부 서비스 통신이나 실시간 상태 서버를 만들면서
gRPC나 Akka/Orleans를 고민하고 있다면 ZLink가 그 자리를 대체할 후보다.

"서비스가 어디 있는지", "client가 어디에 연결돼 있는지", "room·zone·symbol 같은 상태 단위를
어떻게 직렬 처리할지"가 반복 문제로 나올 때 적용 대상이다. [개요](01-overview.ko.md)가 "왜
필요한가"를 다뤘다면, 이 장은 그 판단을 기술 선택 수준에서 확인한다.

## 1. 한눈에 보는 사용처

먼저 적용 범위를 정한다. **모노리스나 모듈러 모노리스로 충분하면 ZLink를 먼저 적용하지
않는다.** 같은 process 안의 모듈 호출은 함수 호출이면 되고, 서버 간 transport가
필요 없다. ZLink는 여러 process/서버로 나뉘어야 하는 이유가 생겼을 때, 그 사이의
통신·연결·라우팅·상태 dispatch 복잡도를 줄이는 도구다.

| 상황 | ZLink가 좋은 이유 | 사용하는 기능 |
|------|--------------------|-----------|
| 내부 서비스끼리 자주 호출 | host/port/stub 대신 **channel name** 으로 호출 | channel + location store |
| 이벤트를 여러 서비스에 동시에 전달 | 별도 broker 없이 **transport fan-out** | fanout pub/sub |
| 게임 room·채팅 room·ride zone 같은 동적 상태 단위 | **단일 실행 queue**로 lock 없는 직렬 상태 처리 | Spot |
| 모바일·게임 client와 장기 연결 | 연결 수명·framing·재접속 흐름을 framework가 소유 | STREAM |
| 연결 서버와 로직 서버를 분리 | actor id 기준 binding으로 **재접속 이전성** | session actor dispatch |
| **서로 다른 언어로 구현된 서비스끼리 호출** | 언어 중립 wire protocol + codec 위 같은 channel 계약으로 **상호 호출** | cross-language binding |
| 초저지연 HFT·durable queue·외부 공개 API | **ZLink 주 영역 아님** | gRPC/REST/Kafka/FIX 유지 |

## 2. ZLink를 사용하는 상황

### 2.1 실시간 게임 서버 구축

**무엇이 어려운가.** 게임 서버에는 웹의 `ASP.NET Core`/Spring 같은 표준화된
framework가 없다. 우연이 아니라 이유가 있다.

- **장르마다 요구하는 네트워크 토폴로지가 다르다.** 웹은 어떤 서비스든 "client
  요청 → 서버 응답" 한 모양이라 framework가 표준화될 수 있었다. 게임은 다르다 —
  보드게임은 방 단위 매칭과 턴 진행, MORPG는 room/stage 서버와 매칭·로비의 분리,
  MMORPG는 zone/field 서버 mesh와 대규모 브로드캐스트, FPS는 소규모 세션의 저지연
  tick 루프를 요구한다. **장르가 토폴로지를 결정하니 하나의 정해진 형태가 없고**, 팀마다
  소켓 위에서 자기 토폴로지를 다시 짠다.
- **상태가 메모리에 유지된다.** 웹은 상태를 DB에 두고 stateless로 scale-out하면
  되지만, 게임은 빠른 처리를 위해 room·참가자 상태를 **in-memory**에 두고 멀티
  thread로 로직을 실행한다. 그 순간 lock, 경합, 데드락, "어느 thread가 이 room을
  바꾸는가"라는 동기화 문제가 업무 로직에 함께 들어온다.
- **연결 자체가 관리 대상이다.** 유저는 장기 연결을 유지한다. 소켓 framing과
  세션 수명을 직접 다루고, 재접속하면 어느 서버의 어느 room에 있었는지 이어 줘야
  하고, 배포·축소 때 접속 유저와 진행 중인 게임 상태를 유지해야 한다.

그래서 지금까지는 이걸 전부 직접 만들거나, 게임 서버 엔진이라는
**별도 runtime으로 옮겨가** 로직 작성 방식·설정·배포·운영을 엔진 방식으로 다시
배우는 수밖에 없었다.

**실제로는 어떻게 만들어 왔나.** 업계에서 통용되는 이름이 붙은 패턴으로 묶인다.
어느 패턴이든 login/auth, gateway, DB cache 같은 상자가 반복해서
등장하지만 — 그걸 받쳐 주는 공통 framework는 없어서, 팀은 자기 장르의 방식을
골라 그 구조를 소켓부터 다시 만든다.

<iframe class="zlink-diagram" src="/common/diagrams/01-arch-existing.html" title="게임 백엔드 4가지 유형 — 기존 방식" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-arch-existing.html" target="_blank">↗ 크게 보기</a></p>

- **① zone 분할.** 월드를 지리적 구역으로 나눠 구역마다 서버(node)가
  담당하고, 캐릭터가 경계를 넘으면 시뮬레이션을 인접 구역 서버로 넘긴다. 대규모
  오픈월드를 감당하기 위한 MMORPG의 대표적인 확장 방식이다. sharding(월드 전체를
  복제해 플레이어를 나눔), instancing(같은 구역의 독립된 사본을 여러 개 생성)도
  많은 동시 접속자를 처리하기 위해 함께 사용하는 대표적인 월드 분산 방식이다.
- **② lobby + room.** 유저를 lobby/매칭에서 받아 room에 배정하고, 그 room이 판이
  끝날 때까지 참가자 상태를 소유한다. room은 보통 한 process 안에 여러 개가
  함께 도는 논리 단위다. 캐주얼·모바일 MO·보드게임에서 흔하다.
- **③ session 기반 dedicated fleet.** 매칭 ticket이 모이면 fleet에서 판 전용
  서버 process를 하나 할당하고, client는 그 서버에 직접 접속한다. 판이 끝나면
  process가 반납된다. ②와 달리 **판 하나 = process 하나**가 기본 단위다.
  경쟁 FPS·배틀로얄 같은 세션 기반 게임의 표준 구성이다.
- **④ stateful actor.** 플레이어·길드 같은 엔티티 상태를 서버 메모리 위 actor로
  유지하고, DB는 주기적 저장소 역할만 한다. 읽기 편중 부하가 줄고 별도 캐싱
  계층이 필요 없어져, 메타·소셜 백엔드에서 흔히 쓰인다. 대표 framework는
  Orleans·Akka다. **개념 차이 하나** — Akka의 actor는 사용자 하나가 아니라 어디에나
  사용하는 범용 동시성 단위이고, ZLink는 이걸 Spot(실행 격리 단위)과 Actor(도메인
  엔티티)로 나눴다. Orleans의 virtual actor·grain에 더 가까운 건 ZLink Actor가
  아니라 이 방식이 사용하는 **Instance Spot**이다. 세부 비교는
  [분산 actor framework와의 비교](#7-참고--분산-actor-frameworkorleansakka와의-비교)가 다룬다.

**ZLink가 제공하는 것.** 어려움 하나하나에 기능이 대응한다.

| 어려움 | ZLink 기능 | 자세히 |
| --- | --- | --- |
| 장르별 토폴로지를 소켓부터 직접 만듦 | **channel 조합으로 토폴로지 선언** — 1:N 요청/응답, fan-out, node 지목 route mesh, room 단위 spot mesh를 등록 몇 줄로 조합, 연결은 location store가 자동 유지 | [계층 구조와 등록 지점](01-overview.ko.md#33-계층-구조와-등록-지점) · [Channel 메시징](20-channel-messaging.ko.md) · [Spot](21-spot.ko.md) · [Location](25-location.ko.md) |
| in-memory 상태의 lock·경합 | **SPOT 직렬 실행** — 한 room의 모든 메시지를 하나의 실행 줄로 세워 순서대로 실행. lock이 업무 로직에서 사라진다 | 아래 코드 · [Spot](21-spot.ko.md) |
| 소켓 framing·세션 수명 직접 구현 | **STREAM** — 연결 수명·framing·packet codec을 framework가 소유(TCP/TLS/WS/WSS) | [STREAM](23-stream.ko.md) |
| 재접속 유저 위치 추적 | **actor binding** — 재접속한 새 연결이 같은 actor로 이어진다 | [Session과 Actor 연결](24-actor-session.ko.md) |
| 배포 때 유저 연결 끊김 | **graceful drain** — 신규 차단, actor handoff, 진행 중 마무리 후 종료. application 코드 0줄 | [운영과 lifecycle](12-operations.ko.md) |

위 네 방식은 전부 같은 선언 모델 **위의 조합**이 된다. 방식마다 소켓부터
다시 만들 필요가 없다.

- **① zone 분할** — zone을 `addRouteMesh` + node 지목 route mesh로 잡는다. 경계를 넘는
  플레이어는 **actor 크로스node relocation**이 대신 넘겨준다([Relocation](37-relocation.ko.md)).
  [ZoneWorld](../../../common/sample/zoneworld/README.ko.md)가 이 방식 그대로다.
- **② lobby + room** — 입장·매칭은 Entry Spot, 방은 `getOrCreate`로 만드는 room spot이다.
  [Bingo](../../../common/sample/bingo/README.ko.md)가 이 방식 그대로다.
- **③ matchmaker + dedicated** — 매칭은 channel handler(HTTP 등)로 구현한다. **판마다 새
  process를 띄우는 대신** 매칭 결과로 `getOrCreate`된 room spot에 client가 STREAM으로
  접속한다. [TicTacToe](../../../common/sample/tictactoe/README.ko.md)가 이 흐름에 가장
  가깝다 — 매칭 요청 → room·접속 정보 응답 → 이미 준비된 room spot에 접속.
- **④ actor 서비스** — **Instance Spot**이 엔티티 ID로 cold activation되어, 여러 유저가
  동시에 건드리는 엔티티 상태를 Redis 분산 락 없이 직렬로 처리한다.
  [길드 서비스 예시](#22-하나의-엔티티에-대한-동시-접근)에서 이어진다.

위 "기존 방식" 그림과 같은 자리에서, ZLink로는 각 방식이 이렇게 구성된다.

<iframe class="zlink-diagram" src="/common/diagrams/01-arch-zlink.html" title="게임 백엔드 4가지 유형 — ZLink 방식" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-arch-zlink.html" target="_blank">↗ 크게 보기</a></p>

초록(굵은 테두리)이 SPOT 계열 primitive다. 위 "기존 방식" 그림과 대조되는 지점은
바로 여기다 — 기존에는 방식마다 인프라(전용 fleet orchestrator, sticky routing,
actor 클러스터)가 따로 필요했지만, ZLink에서는 네 방식 전부 같은
RouteMesh·Spot·Instance Spot 조합으로 구현한다. 방식이 바뀌어도 새로 배워야 할
runtime이 없다.

> 트위치 FPS의 **초저지연 snapshot netcode**는 유실을 허용하는 비신뢰 전송을 사용한다.
> 현재 STREAM이 제공하는 transport는 TCP/TLS/WS/WSS이며, **비신뢰 전송(QUIC
> datagram·WebTransport)은 지원 예정**이다. 다만 그런 게임에서도 매칭·로비·메타·
> 소셜은 이 방식들로 처리된다. 어디까지 되고 안 되는지는
> [ZLink의 경계](#5-zlink의-경계--다루지-않는-요구)가 다룬다.

**게임 서버 엔진·서비스와는 어떻게 다른가.** 직접 만들지 않는 길로는 엔진과
관리형 서비스가 있다. 이들이 제공하는 것을 영역별로 놓으면 ZLink가 담당하는 자리가 드러난다.

| 제공 영역 | 대표 제품 | 제공 형태 |
| --- | --- | --- |
| 연결·전송 최적화 — 소켓/세션 관리, 암호화·압축, TCP/UDP 병행, 네트워크 I/O와 로직 thread 분리 | [ProudNet](https://docs.proudnet.com/proudnet.eng) | 전용 서버 모듈 + client SDK |
| room·lobby·매칭 — room 생성/조회, lobby, 매치 초대 | [Photon](https://www.photonengine.com/)·[SmartFoxServer](https://docs2x.smartfoxserver.com/Overview/zones-room-architecture) | 자체 runtime 위의 room 모델 |
| 호스팅·fleet — dedicated 서버 할당, autoscaling, 매치메이킹 규칙 엔진(FlexMatch) | [AWS GameLift](https://aws.amazon.com/gamelift/servers/)·Agones | 클라우드 관리형 서비스 |
| 소셜·메타 기능 — 친구, 리더보드, 그룹, 채팅 | [Nakama](https://heroiclabs.com/nakama-gamelift/) | 백엔드 서버 제품 |

ZLink는 이 중 **연결·세션(STREAM), room·상태 단위(SPOT), 서버 간 메시징(channel),
참가자 상태(actor), 무중단 종료(host relocation)** 를 제공한다 — 단, 전용 runtime이나 관리형
서비스가 아니라 **사용하던 메이저 framework 위의 library 계층**으로.

- **호스팅·fleet은 ZLink의 몫이 아니다.** K8s든 GameLift든 그 위에서 ZLink 서버가
  돌면 된다 — 호스팅 서비스와 경쟁하지 않고 조합된다.
- **매치메이킹 규칙과 소셜 기능은 제품 기능이 아니라 application 로직이다.** channel
  handler와 spot으로 직접 작성한다. 미리 만들어진 기능은 적지만, 로직의 소유권과
  자유도가 앱에 남는다.

ZLink는 언어마다 처음부터 다시 만드는 대신, 어려운 runtime을 담은 **native Core(C API)**
하나를 두고 그 위를 언어별 계층으로 감싼다. 언어별 **`bindings`** 가 그 C API를 각 언어의
소켓 API로 잇고, 그 위에 언어별 **ZLink Framework** 가 RouteMesh · SPOT · actor · STREAM
같은 표면을 제공한다. 이렇게 얇은 계층 구조로 나눈 이유는 **다중 언어 지원**이다 — Core를
한 번만 구현하고 언어 표면만 갈아 끼우면 C++ · .NET · JVM · Node가 같은 코어를 공유한다.
`bindings`와 Core는 framework 내부 구현이라 public API에 노출되지 않고, 나중에 교체돼도
application 코드는 바뀌지 않는다 — 이 backend 경계는
[internals/backend-dependency-policy](../../internals/backend-dependency-policy.ko.md)가
별도로 설명한다.

<iframe class="zlink-diagram" src="/common/diagrams/overview-stack.html" title="ZLink 계층 관계 — 다중 언어를 위한 얇은 3계층" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/overview-stack.html" target="_blank">↗ 크게 보기</a></p>

**코드로 보면.** room 하나를 선언하고, 그 room의 진행 로직을 사용한다.

```java
// 등록 — room mesh 하나와 room 타입
ZLinkMeshNodeBuilder node = options.addRouteMesh("game.room");
node.listen("tcp://0.0.0.0:9001");
// mesh는 최소 1개 logical membership을 갖는다
node.channelName("game.room").server();
node.objects().server().addSpotFactory("room", BingoRoomSpot.class, factory -> factory.recreateOnRelocation());
```

```java
// bingo room의 진행 코드 — 이 안에서 동시성은 존재하지 않는다.
public final class MarkNumberHandler
    implements ZLinkSpotRequestHandler<BingoRoomSpot, MarkNumber, MarkResult> {

    @Override
    public CompletionStage<MarkResult> handle(BingoRoomSpot room, MarkNumber request) {
        // lock 없음
        room.board().mark(request.number());
        room.setLastActivity(Instant.now());
        return CompletableFuture.completedFuture(new MarkResult(room.board().hasBingo()));
    }
}
```

여러 플레이어가 동시에 요청을 보내고 timer가 도는 room인데 `lock`도,
`Interlocked`도, Redis 분산 락도 없다. framework가 한 room의 모든 메시지(요청,
구독 이벤트, timer tick, actor packet)를 **하나의 실행 줄에 세워 순서대로**
실행하기 때문이다. 여기서 직렬은 codec 직렬화가 아니라 **실행 순서의
직렬화**다([실행 모델](32-execution-model.ko.md)).

실행되는 근거 샘플: [TicTacToe](../../../common/sample/tictactoe/README.ko.md) ·
[Bingo](../../../common/sample/bingo/README.ko.md) · [GameQuest](../../../common/sample/event/gamequest.ko.md)

### 2.2 하나의 엔티티에 대한 동시 접근

**왜 어려운가.** 길드처럼 **서로 다른 여러 유저가 같은 엔티티를 동시에 수정**해야
하는 경우가 있다. 두 유저가 동시에 가입을 신청해 정원을 넘기거나, 두 기부가 동시에
반영돼 하나가 유실되는 것처럼, stateless API 서버 여러 대가 같은 row를 동시에
수정하면 race condition이 생긴다.

- **동시 수정이 충돌한다.** 여러 API 인스턴스가 같은 길드 row를 동시에
  읽고-고치고-사용하면 lost update가 생긴다.
- **직렬화 장치를 직접 만들어야 한다.** Redis 분산 락이나 DB row lock으로 길드
  단위 critical section을 만들어야 한다.
- **락 자체가 새 실패 모드다.** 락 획득 실패·타임아웃·데드락·락 만료 후 stale
  write 처리가 application의 책임으로 남는다.

**ZLink가 제공하는 것.** 락을 직접 구성하는 대신 그 엔티티를 직렬 실행 단위로 만든다.

| 직접 갖추던 것 | ZLink 기능 | 자세히 |
| --- | --- | --- |
| 길드 id별 Redis 분산 락 | **Instance Spot** — 길드 id로 cold activation되는 spot 하나가 그 길드의 모든 요청을 직렬 처리 | [Spot](21-spot.ko.md) |
| 락 획득·해제·타임아웃 처리 | **직렬 실행** — 락 개념 자체가 없어지고, 항상 spot queue 순서대로 처리된다 | [실행 모델](32-execution-model.ko.md) |
| 길드 spot을 찾는 서버 간 호출·LB | **channel name + location store** | [Channel 메시징](20-channel-messaging.ko.md)·[Location](25-location.ko.md) |
| 새 길드의 사전 프로비저닝 | 첫 요청이 오면 그 자리에서 cold activation — 별도 준비 불필요 | |

**기존 방식** — 락 획득·해제가 매 요청마다 왕복한다.

<iframe class="zlink-diagram" src="/common/diagrams/01-guild-existing.html" title="길드 상태 변경 — 기존 방식" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-guild-existing.html" target="_blank">↗ 크게 보기</a></p>

**ZLink 방식** — 락이 사라지고, 길드 id가 곧 그 요청이 도착할 spot 주소가 된다.

<iframe class="zlink-diagram" src="/common/diagrams/01-guild-zlink.html" title="길드 상태 변경 — ZLink 방식" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-guild-zlink.html" target="_blank">↗ 크게 보기</a></p>

같은 길드로 온 요청은 항상 같은 GuildSpot의 queue를 통과하므로, 두 번째 요청은 첫
번째가 끝난 뒤에야 처리된다 — 락을 잡고 있는 시간만큼 다른 요청이 막히는 게 아니라,
동시에 두 요청이 같은 상태를 만질 수 없다.

**코드로 보면.** 락 획득·해제가 있던 자리에 한 호출이 남는다.

```java
// 길드 가입 신청 — 길드 id로 바로 요청한다. 사전 락도, 사전 생성도 없다.
spots.requestToSpot(guildId, new JoinGuildReq(userId))
    .instanceSpot("guild")
    .inMesh("social")
    .submit(JoinGuildRes.class);
```

이 시나리오는 아직 실행 가능한 기준 샘플이 없다 — 위 코드는 GameQuest의
`PlayerQuestSpot` 등록·호출 방식과 같은 API 표면을 길드에 적용한 것이다.

### 2.3 기존 웹 서비스의 실시간 기능 추가

**왜 복잡도가 올라가는가.** **배달 주문 앱**이 그런 경우다 — 주문 넣기·조회는 평범한
HTTP 요청/응답이지만, "준비 중 → 배달 출발 → 곧 도착" 상태는 application을 새로고침하지 않아도
실시간으로 전달해야 한다. 대규모 웹 서비스의 표준 구성 — Spring/`ASP.NET Core` +
Redis(캐시) + Kafka(이벤트) + LB/K8s — 은 **stateless 요청/응답**에 최적화되어 있어서,
이런 실시간 기능을 추가하는 순간 전제들이 하나씩 안 맞으면서 복잡도가 올라간다.

- **연결이 상태가 된다.** HTTP 요청은 아무 인스턴스가 받아도 되지만, WebSocket
  연결은 특정 인스턴스에 설정되어 있다. 그래서 연결을 고정하는 sticky LB가 생기고,
  "이 사용자가 지금 어느 인스턴스에 연결돼 있지?"를 application이 Redis로 관리하기 시작한다.
- **서버 사이 실시간 전달이 우회한다.** 연결이 인스턴스마다 흩어져 있으니 서버 간
  전달은 브로커(Redis pub/sub, 또는 replay가 필요 없는데도 Kafka)를 경유한다 —
  운영할 인프라가 또 하나 늘어난다.
- **순서가 중요한 단위가 생긴다.** 주문·대화는 이벤트 처리 순서가 곧 정합성이다.
  여러 인스턴스가 같은 주문을 동시에 수정할 수 있으니 분산 락으로 직렬화한다.

기능 하나를 추가했을 뿐인데 WebSocket 서버, sticky LB, 브로커 경유, 분산 락 — 직접 갖춰야 하는 구성 요소
한 벌과 그 운영 부담이 늘어난다.

**ZLink가 제공하는 것.** 그 구성 요소마다 기능이 대응한다.

| 직접 갖추던 것 | ZLink 기능 | 자세히 |
| --- | --- | --- |
| WebSocket 서버 + sticky LB | **STREAM** — application 서버가 client 연결을 직접 받는다 | [STREAM](23-stream.ko.md) |
| 분산 락으로 순서 보장 | **SPOT owner routing** — 같은 주문·대화는 항상 자기 Spot 한 곳에서 직렬 실행 | [Spot](21-spot.ko.md) |
| 브로커 경유 실시간 전달 | **channel·fanout** — 서버 간 전달과 fan-out을 transport가 직접 | [Channel 메시징](20-channel-messaging.ko.md) |
| "누가 어디 연결돼 있지" 관리 | **actor binding + location store** — 재접속 이전성과 위치 조회를 framework가 소유 | [Session과 Actor 연결](24-actor-session.ko.md)·[Location](25-location.ko.md) |

같은 배달 주문 application — HTTP 주문 처리 + 실시간 배달 상태 push — 을 두 방식을 나란히 그리면
두 그림의 차이는 다음과 같다.

**기존 방식** — 실시간 기능을 위한 구성 요소(주황)가 본체만큼 추가된다.

<iframe class="zlink-diagram" src="/common/diagrams/01-delivery-existing.html" title="기존 방식 — 배달 주문 앱" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-delivery-existing.html" target="_blank">↗ 크게 보기</a></p>

**ZLink 방식** — 주황 조각이 전부 사라지고, node·actor·spot 위치정보를 제공하는
location store 하나가 남는다.

<iframe class="zlink-diagram" src="/common/diagrams/01-delivery-zlink.html" title="ZLink 방식 — 배달 주문 앱" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-delivery-zlink.html" target="_blank">↗ 크게 보기</a></p>

sticky LB · pub/sub 브로커 · 분산 락 — 이 인프라 구성 요소가 사라진다. 순서는
**Instance Spot**이, 실시간 연결은 shell 서버 대신 **Session 서버**(STREAM)가, 서버 간
전달은 **runtime 직접 연결**이 맡는다. 새로 두는 인프라는 **location store 하나**뿐이다.

**코드로 보면.** 분산 락과 sticky 라우팅이 있던 자리에 다음 코드가 남는다.

```java
// HTTP handler 안 — 주문 이벤트를 그 주문의 workflow Spot으로.
// 첫 요청이 OrderId 기준 spot을 cold-activate하고, 이후 요청은 이미 만들어진
// 같은 spot에 도착해 항상 한 곳에서 순서대로 처리된다(분산 락 없음).
// request는 이미 StartOrderWorkflowReq 바디다.
spots.requestToSpot(request.orderId(), request)
    .instanceSpot("order-workflow")
    .inMesh("commerce")
    .submit(StartOrderWorkflowRes.class);

// actor handler 안 — 재접속해도 같은 actor로 이어진 client에 push(sticky LB 없음).
actor.context().boundSession().send(new OrderStatusChanged(orderId, status)).submit();
```

실행되는 근거 샘플: [SupportChat](../../../common/sample/supportchat/README.ko.md) ·
[DeliveryDispatch](../../../common/sample/deliverydispatch/README.ko.md)

### 2.4 이벤트 중심 업무 처리 단순화

ZLink의 사용 지점은 실시간 기능만이 아니다. 주문 처리·정산·재고처럼 **같은 엔티티의
이벤트를 순서대로, 중복 없이 처리해야 하는** 업무는 화면에 실시간 push가 하나도 없어도
같은 복잡도 문제를 만난다.

**왜 복잡해지는가.** 이런 업무의 표준 답은 Kafka 같은 log 기반 파이프라인이다(이벤트
소싱 구성도 보통 이 위에 올린다). 그런데 log가 실제로 해결하는 것은 "같은 key를 한
곳에 모아 순서대로"인데, 그 하나를 위해 조각이 줄줄이 따라온다.

- **순서가 partition에 묶인다.** 같은 주문의 이벤트를 순서대로 처리하려면 key
  partition으로 모아야 하고, 소비자 수는 partition 수에 묶이며, consumer group의
  rebalance와 offset 관리가 운영 항목으로 따라온다.
- **소비자가 stateless라 상태는 매번 DB 왕복이다.** 이벤트 하나를 처리할 때마다 DB에서
  현재 상태를 읽고-고치고-사용한다. 반복 읽기를 줄이려 캐시를 붙이면 무효화 문제가
  따라온다.
- **at-least-once라 멱등성이 application 몫이 된다.** 재전달·rebalance·재처리로 같은 이벤트가
  두 번 올 수 있어, version check나 dedupe 정책 없이는 중복 반영된다.
- 처리 결과 조회용 read model을 따로 만들고, 파이프라인이 밀리면 lag 모니터링과
  재동기화 잡이 남는다.

stateful stream processor(Kafka Streams/Flink)로 상태를 소비자 곁에 두면 DB 왕복은
줄지만, partition 설계·state store 복구·rebalance가 운영 책임으로 남는다 — 이 비교의
상세는 [GameQuest 공통 시나리오 §3](../../../common/sample/event/gamequest.ko.md)이 다룬다.

같은 업무 — 주문 workflow — 를 두 방식을 나란히 그리면 조각 두 그림의 차이는 다음과 같다.

**기존 방식** — 순서 처리를 위한 파이프라인 조각(주황)이 본체만큼 추가된다.

<iframe class="zlink-diagram" src="/common/diagrams/01-order-existing.html" title="주문 처리 — 기존 방식" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-order-existing.html" target="_blank">↗ 크게 보기</a></p>

**ZLink 방식** — Kafka를 대체하는 것이 아니다. **주문 처리 경로에서** 파이프라인
조각(주황)이 사라지고, Kafka는 자기 본연의 자리 — 확정된 사실을 독립 시스템들에
전파하고 replay가 필요한 이벤트를 보존하는 durable log — 로 남는다(회색).

<iframe class="zlink-diagram" src="/common/diagrams/01-order-zlink.html" title="주문 처리 — ZLink 방식" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/01-order-zlink.html" target="_blank">↗ 크게 보기</a></p>

두 그림에서 Kafka의 색이 바뀐다. 처리 경로 **안에서** 순서를 담당하던
Kafka(주황)가 처리 경로 **밖으로** 나가 전파·보존만 맡는다(회색). 그러면서 순서
담당을 위해 직접 갖췄던 구성 요소 — 주문 처리 소비자 그룹(offset·rebalance·dedupe), 캐시,
조회용 read model, 재동기화 잡 — 이 사라진다. 같은 `OrderId`가 항상 같은 owner에서
직렬로 처리되므로, 파이프라인이 제공하던 순서·중복 방지를 따로 구성할 필요가 없어진
것이다.

**서버 간 호출의 LB도 사라진다.** 주문 처리는 재고·결제 같은 다른 서비스를 동기
호출하는데, 기존 방식은 그 경로마다 K8s Service나 service discovery로 상대를 찾아
분배해야 한다(주소를 코드에 하드코딩할 수 없기 때문이다). ZLink에서는 `"inventory"` 같은
channel name으로 호출하고 location store가 현재 사용 가능한 peer를 알려 주므로, **서버 간
호출용 LB 계층이 따로 필요 없다** — 그래서 after 그림에서 주황 `서버 간 호출용 LB`가
사라진다.

**남는 것은 남는다.** client HTTP 진입은 여전히 stateless라 L7 LB/Ingress가 평소처럼
API 서버에 분배하고(회색), 주문 상태는 여전히 DB에 저장한다. gRPC와 달리 이 HTTP 진입
경로에 L7 분배 장치를 **추가로** 요구하지도 않는다(그 이유는
[gRPC 단독 구성의 한계](#61-grpc-단독-구성의-한계)가 다룬다).

**ZLink가 제공하는 것.** "같은 key를 한 곳에 모아 순서대로"를 log가 아니라 **owner
routing**으로 풀면, 위 구성 요소의 대부분은 따로 구성할 필요가 사라진다.

| 직접 갖추던 것 | ZLink 기능 | 자세히 |
| --- | --- | --- |
| key partition + consumer group | **SPOT owner routing** — 같은 `OrderId`는 항상 같은 Spot에서 직렬 실행. 어느 API 인스턴스가 받아도 같은 owner로 route된다 | [Spot](21-spot.ko.md) |
| 이벤트마다 DB load-modify-store | **owner spot의 hot state** — 상태가 owner 메모리에 있고, 저장 시점은 업무 규칙에 맞춰 application이 결정한다 | [Spot](21-spot.ko.md) |
| 재전달 대비 version check·분산 락 | **직렬 실행** — 같은 단위에 동시 writer가 없어 정상 경로에서 락·version 경합이 없다 | [실행 모델](32-execution-model.ko.md) |
| 서버 간 호출용 LB·service discovery | **channel name + location store** — `"inventory"` 이름으로 호출하면 현재 사용 가능한 peer로 직접 전송한다 | [Channel 메시징](20-channel-messaging.ko.md)·[Location](25-location.ko.md) |
| offset·lag·재동기화 잡 운영 | 소비 파이프라인이 없으므로 해당 운영 항목 자체가 없다 | |

**기존 스택을 대체하는 것이 아니다.** Kafka는 내구성 있는 이벤트 스트림으로, Redis는
캐시/영속 보조로 그대로 남는다. ZLink가 줄이는 것은 그 사이에서 직접 갖추던
**연결·라우팅·상태 관리의 복잡도**다.

**경계는 그대로다.** durable log가 실제로 필요한 요구 — 이벤트 replay, 장기 보존, 독립
시스템들로의 광범위 fan-out — 는 Kafka가 맞고 그대로 남긴다([ZLink의 경계](#5-zlink의-경계--다루지-않는-요구)).
ZLink가 줄이는 것은 "엔티티 단위 순서 처리"만을 위해 log 파이프라인을 직접 구성하던
경우다. 순서와 정합성이 목적의 전부였다면, owner routing이 그 목적을 파이프라인 없이
직접 달성한다.

**코드로 보면.** partition 소비자 자리에 owner Spot handler가 온다.

```java
// 같은 OrderId의 처리는 항상 이 Spot 안에서 순서대로 실행된다 —
// partition도, offset도, 분산 락도, 멱등성 재시도 정책도 직접 갖추지 않는다.
public final class StartOrderWorkflowHandler
    implements ZLinkSpotRequestHandler<OrderWorkflowSpot, StartOrderWorkflowReq, StartOrderWorkflowRes> {

    @Override
    public CompletionStage<StartOrderWorkflowRes> handle(
        OrderWorkflowSpot spot, StartOrderWorkflowReq request) {
        // spot 상태에 lock 없이 접근
        return workflow.startInSpot(spot, request);
    }
}
```

실행되는 근거 샘플: [ShoppingMall](../../../common/sample/event/shoppingmall.ko.md) — 실시간 push
없이 HTTP API + 주문 workflow만으로 구성된 이 상황의 기준 샘플이다. 주문 상태
전이·보상 흐름·중복 방지·projection 재생성을 owner routing 위에서 검증한다.

지금까지 살펴본 상황들의 차이는 진입점일 뿐, 사용하는 표면은 같다. 기능 하나씩 제공하는 제품은
있어도 — RPC는 gRPC가, actor는 Orleans가, 연결은 게임 엔진이 — **메이저
framework 통합 · 직렬 실행 상태 단위 · 자동 연결 토폴로지를 함께 제공하는
조합**이 ZLink의 자리다.

## 3. 개발 모델 — framework가 맡는 범위

ZLink의 체감 장점은 인프라 구성 요소가 사라지는 데 있지 않고, **"개발자가 덜 고민한다"** 에 있다.
application은 도메인 단위(channel/spot/session)만 다루고, 나머지는 framework가 처리한다.

- **channel name만 알고 호출한다** — 대상 host/port/stub를 모른다.
- **service location과 peer 분배**는 location store 기반 자동 연결이 맡는다([Location](25-location.ko.md)).
- **request correlation과 reply 대기**는 framework가 맡는다.
- **client 연결 수명과 packet framing** 은 STREAM이 맡는다.
- **room/zone/symbol 상태 직렬성**은 Spot의 실행 queue가 처리한다.
- **재접속 후 actor/session binding** 은 framework가 복원한다.
- **handler/filter/DI 모델**이 기존 웹 framework 방식과 맞아 익숙하게 사용한다.

> ZLink는 이 문제들을 없애지 않는다. 처리 자리를 호출자 코드에서 **framework로 옮긴다.** 위치·연결·
> correlation·dispatch 직렬성을 framework가 처리하므로, application 코드가 transport
> 설정이 아니라 **업무 흐름처럼** 보인다.

### 3.1 cross-language — 언어가 달라도 같은 channel 계약

ZLink는 한 언어 전용이 아니다. 호출 계약이 **언어 중립 wire protocol(ZMP) +
codec(protobuf/json/messagepack) + 논리 channel/packet 이름** 이라, 서로 다른
언어로 구현된 서비스가 **같은 channel 위에서 상호 호출**한다. 예를 들어 게임
시스템에서 **room 서버는 C++, API·매치메이킹 서버는 .NET 또는 Java** 로 두고 같은
channel/spot 계약으로 메시징할 수 있다.

- 언어 간 계약 = **packet 이름 + codec으로 인코딩된 DTO**(교차 언어는 protobuf
  권장, 또는 합의된 JSON/MessagePack 스키마). gRPC처럼 service-stub 코드 생성이나
  HTTP/2를 강제하지 않는다 — payload 스키마만 공유한다.
- 각 언어 binding은 같은 core(C ABI, ZMP) 위에 handler/Spot/STREAM 표면을 올린다.
  그래서 handler 작성 언어가 달라도 wire 상으로는 같은 channel·packet 이다.

!!! note "다른 언어 binding"

    같은 channel·packet 계약을 언어별 binding이 자기 언어로 구현한다. 이 가이드의 예제는
    언어 탭으로 나뉘며, 어느 탭을 보든 같은 계약을 설명한다. 호출 계약이 binding 구현 언어와
    무관하다는 것이 ZLink의 설계 목표다.

## 4. ZLink 후보가 되는 증상

기술명보다 **증상**으로 판단한다. 아래가 반복되면 ZLink가 후보다.

- 서비스마다 gRPC stub·channel factory·deadline·서비스 위치 조회 설정이 반복된다.
- Kubernetes L4 LB로 gRPC 부하가 고르게 안 퍼져 mesh를 고민한다.
- 게임 room·채팅 room·ride zone처럼 상태 단위를 lock으로 보호하고 있다.
- 재접속 때 client가 어느 서버에 연결돼 있었는지 Redis로 따로 관리한다.
- 실시간 이벤트 fan-out 때문에 Kafka를 사용하는데, 실제로는 replay가 필요 없다.
- 외부 client 연결·내부 서비스 호출·room 상태 처리가 서로 다른 framework로 흩어져
  있다.

## 5. ZLink의 경계 — 다루지 않는 요구

적용 범위를 정하려면 적용하지 않는 자리도 적는다. 다음은 그대로 유지한다.

| 요구 | ZLink 판단 |
|------|------------|
| 외부 공개 HTTP API | REST/gRPC 유지 |
| durable queue·replay·consumer offset | Kafka/NATS 유지 |
| DB 조회·geo-index·audit trail | DB/Redis/event store 유지 |
| HFT 마이크로초 matching loop | Disruptor/Aeron/FIX 유지 |
| 내부 서비스 통신 + 실시간 상태 dispatch | **ZLink 적합** |

ZLink는 transport·dispatch 계층이지 **datastore·durable log·HFT 버스가
아니다.** 분산 데이터 일관성(saga·outbox·idempotency)·영속·중복 제어 같은
도메인 난제는 그대로 application과 인프라가 책임진다.

## 6. 참고 — gRPC·service mesh 스택과의 비교

[한눈에 보는 사용처](#1-한눈에-보는-사용처)의 "내부 서비스끼리 자주 호출" 이 왜 ZLink 후보인지, gRPC 스택과 비교해
근거를 본다.

### 6.1 gRPC 단독 구성의 한계

gRPC의 RPC 지연 자체는 추가 인프라 없이도 목표치를 만족한다. 문제는 이런 종류의 서비스를 **"프로덕션급"** 으로 만들려면
공식 베스트프랙티스가 곧바로 추가 인프라를 요구한다는 점이다.

- **channel/stub 재사용 강제.** "Always re-use stubs and channels when possible" —
  호출마다 channel을 만들면 지연이 크게 늘어 channel factory/pool로 수명을 직접
  관리한다. ([grpc.io performance](https://grpc.io/docs/guides/performance/))
- **deadline을 매 호출에.** 단일 느린 RPC가 상위 서비스를 막지 않도록 deadline을
  건다. ([Microsoft Learn](https://learn.microsoft.com/en-us/aspnet/core/grpc/performance))
- **기본 load balancer(L4, connection 단위 분배)로는 gRPC 부하가 고르게 분산되지 않는다.**
  gRPC는 HTTP/2 위에서 연결 하나를 오래 유지한 채 여러 요청을 함께 실어 보내므로, L4
  load balancer에는 연결이 1개로만 보여 그 연결이 처음 연결된 서버로 요청이 집중된다.
  HTTP/2 기반인 이상 요청 단위로 분배하는 L7 분배가 사실상 필수라, 보통 아래 중
  하나를 추가로 도입한다.
  - **client-side LB**: client가 서버 목록을 보관하고 직접 번갈아 호출하는 방식.
  - **headless service**(Kubernetes): 서비스를 단일 가상 IP 하나가 아니라 **뒤에
    있는 각 pod의 IP 목록**으로 노출해, client가 직접 골고루 분배하게 하는
    방식.
  - **Envoy/Istio service mesh sidecar**: 각 서비스와 함께 자동 배치되는 **proxy**가
    요청 단위(L7) 분배와 암호화(mTLS)를 대신 처리하는 방식.
  ([Kubernetes 블로그](https://kubernetes.io/blog/2018/11/07/grpc-load-balancing-on-kubernetes-without-tears/))
- **그 밖에** 서비스 위치 조회(Eureka/Consul/xDS), retry·hedging, `.proto` 파이프
  라인, mTLS, 그리고 **이벤트 fan-out은 또 별도 broker**(Kafka/NATS)로 간다.

L7 분배는 연결이 아니라 요청 하나하나를 보고 나누는 방식이다 — mesh sidecar나
client-side LB가 이 역할을 한다.

<iframe class="zlink-diagram" src="/common/diagrams/17-l7-distribute.html" title="L7 분배 — 요청 하나하나를 나눈다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-l7-distribute.html" target="_blank">↗ 크게 보기</a></p>

즉 "gRPC를 사용한다"는 실제로 **gRPC + L7 LB(보통 mesh) + 서비스 위치 조회 + event broker +
proto pipeline**을 함께 운영한다는 뜻이다.

### 6.2 배치 구조 비교

<iframe class="zlink-diagram" src="/common/diagrams/17-classic-mesh.html" title="기존 방식 — gRPC + service mesh + broker + WS edge" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-classic-mesh.html" target="_blank">↗ 크게 보기</a></p>

<iframe class="zlink-diagram" src="/common/diagrams/17-zlink-channel.html" title="ZLink 방식 — framework + location store" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-zlink-channel.html" target="_blank">↗ 크게 보기</a></p>

Envoy sidecar와 mesh control plane(서비스 위치 조회·L7 LB·mTLS) 자리가 framework와
location store 한 겹으로 들어온다. broker와 WS edge는 요구가 단순한 실시간 전파·연결
수용이면 fanout channel·STREAM이 대신 처리할 수 있고, 영속 queue·replay 나 HTTP edge
정책이 필요하면 그대로 둔다.

### 6.3 한 번의 호출이 지나는 경로

<iframe class="zlink-diagram" src="/common/diagrams/17-sidecar-path.html" title="sidecar 경로 — Envoy local → Envoy remote 두 홉" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-sidecar-path.html" target="_blank">↗ 크게 보기</a></p>

<iframe class="zlink-diagram" src="/common/diagrams/17-channel-path.html" title="channel 경로 — sidecar 없이 직접 호출" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-channel-path.html" target="_blank">↗ 크게 보기</a></p>

### 6.4 ZLink에서 사라지는 구성 요소

| gRPC 베스트프랙티스/필요 인프라 | ZLink에서 | 비고 |
| --- | --- | --- |
| "stub/channel을 재사용하라" | route client가 DI singleton이고 MeshNode 연결 수명은 framework가 관리 | 호출마다 만들 일 없음 |
| RPC deadline | `RequestToChannel(...).Timeout(...)` | reply 대기 시간 |
| L7 로드밸런싱(Envoy/Istio) | channel name + store 자동 연결이 peer 분배 | sidecar 불필요 |
| interceptor | handler filter | [Filter](31-handler-dispatch.ko.md#2-filter--공통-처리를-한곳에-모은다) |
| 이벤트 broker(Kafka/NATS) | fanout channel pub/sub | 실시간 fan-out 한정. 영속/replay는 broker 유지 |
| 통합 관측(mesh telemetry) | 상태 status stream과 표준 진단 | [모니터링](26-monitoring.ko.md) |
| 양방향 streaming | STREAM session | 외부 client 수용. HTTP edge 정책은 별도 |

이 비교는 우열을 일반화하려는 것이 아니다. gRPC는 외부 공개 API, 표준 RPC 계약, 조직
표준 tooling이 중요할 때 그대로 유지한다. 성능도 payload 크기·codec·네트워크·peer
수·배포 방식에 따라 달라지므로 수치로 단정하지 않는다. 여기서 말하는 이득은 **호출 경로와
운영 컴포넌트가 줄어든다**는 것이다 — HTTP/2 proxy·stub·별도 broker를 지나던 구성이
framework와 location store로 줄어든다. 조직 보안 정책이나 외부 ingress가 필요하면
기존 mesh·LB를 그대로 함께 둔다.

## 7. 참고 — 분산 actor framework(Orleans/Akka)와의 비교

[실시간 게임 서버 구축](#21-실시간-게임-서버-구축)의 ④ stateful actor 방식에 실제로 쓰이는 대표
framework가 Microsoft Orleans와 Akka다. ZLink의 Spot/actor는 같은 primitive
(mailbox 직렬화 + 위치 투명)를 제공하므로, 이 워크로드에서 후보가 겹친다.

### 7.1 Orleans/Akka 단독 구성의 한계

Orleans·Akka는 **actor primitive 하나에** 깊이 집중한다. 그런데 이 가이드가 다루는
"실시간 상태 서버 하나"를 만들려면 actor 밖의 것들을 여전히 따로 갖춰야 한다.

- **외부 client 연결이 없다.** 둘 다 client가 직접 grain/actor를 호출하는 프로토콜을
  내장하지 않는다. 웹 client는 보통 SignalR이나 별도 WebSocket 서버를 앞에 두고,
  그 서버가 actor를 호출하는 구조로 구성한다.
- **polyglot이 아니다.** Orleans는 `.NET` 전용, Akka는 JVM 전용이다(Akka.NET은 별도
  포트 구현). C++ room 서버와 `.NET` API 서버를 같은 계약으로 묶는 조합은 설계
  범위 밖이다.
- **서비스 간 메시징은 actor 호출과 별개다.** grain-to-grain 호출은 있지만, channel
  이름 기반 요청/응답이나 fanout 같은 일반 서비스 메시징 표면은 없다 — 필요하면
  gRPC나 메시지 브로커를 별도로 추가한다.

### 7.2 배치 구조 비교

<iframe class="zlink-diagram" src="/common/diagrams/17-orleans-cluster.html" title="Orleans/Akka — actor cluster + 별도 edge" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-orleans-cluster.html" target="_blank">↗ 크게 보기</a></p>

<iframe class="zlink-diagram" src="/common/diagrams/17-zlink-integrated.html" title="ZLink — 통합 스택" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/17-zlink-integrated.html" target="_blank">↗ 크게 보기</a></p>

client 연결·서비스 메시징·actor 상태를 한 framework가 함께 제공한다.
다만 이 그림에 나타나지 않는 차이가 있다 — Orleans/Akka가 오랜 기간에 걸쳐 미리
구현해 둔 persistence connector·reminder scheduler 같은 부가 도구까지 하나로 내려오는
건 아니다. 아래 표에서 어디까지가 원시 기능 차이고 어디부터가 이런 미리 구현된
도구의 유무 차이인지 나눠서 본다.

### 7.3 기능 비교 — 유리한 점과 불리한 점

| 항목 | Orleans / Akka | ZLink |
| --- | --- | --- |
| actor primitive(mailbox 직렬화 + 위치 투명) | ✅ | ✅ (Spot/actor) |
| 외부 client 연결 내장 | ❌ SignalR/WS를 따로 갖춘다 | ✅ STREAM |
| polyglot | ❌ 단일 언어(.NET 또는 JVM) | ✅ |
| 서비스 간 typed 메시징 + 토폴로지 선언 | ❌ gRPC 등을 따로 갖춘다 | ✅ channel + location store |
| actor 상태 persistence | ✅ 성숙한 provider 생태계 | ⚠️ lifecycle 훅은 있고 미리 구현된 storage connector는 없다(아래 ①) |
| relocation 뒤 Spot timer 복원 | ✅ | ✅ 등록과 pending tick을 payload에 포함해 자동 복원한다 |
| 없는 Actor를 만들거나 기존 Actor를 사용 | ✅ | ✅ `getOrCreate`가 같은 ActorId의 동시 생성을 조정한다 |
| 예정 시각에 dormant actor 활성화(reminder) | ✅ API 한 콜(Orleans Reminder) | ❌ 전용 API 없음 — 분산 scheduler로 구성한다(아래 ②) |
| 분산 트랜잭션 | Orleans 실험적 지원 | ❌ 없음(saga는 application이 구성) — 이는 실제 프로토콜 난이도의 문제라 기존 primitive로 우회할 수 없다 |
| 라이선스 | Orleans MIT / Akka BSL(연매출 기준 유료 트리거) | framework는 FSL-1.1-ALv2, core·binding은 MPL-2.0 — 매출 기준 유료 트리거가 없다 |
| 실전 검증 기간 | 10년 이상(Halo, Microsoft 365, Skype) | 짧음 — 이 프로젝트 자체가 진행 중 |

① **actor 상태 persistence** — `onCreate`·`onClosing` 같은 lifecycle 훅은
제공하지만, 어느 DB에 어떻게 저장할지는 application이 정한다. 미리 구현된 storage
connector 모음이 없다는 뜻이다([ShoppingMall](../../../common/sample/event/shoppingmall.ko.md)이 그 예다).

② **reminder** — Quartz.NET Clustered·Hangfire 같은 분산 scheduler가 정해진 시각에 Actor
`getOrCreate`나 message를 실행하도록 application이 구성한다.

**결론.** "실시간 상태 서버 하나를 구성 요소를 직접 갖추지 않고 만든다"는 이 가이드의 워크로드에는
ZLink가 대체 후보다. Actor·Spot lifecycle과 relocation timer 복원은 Framework가 제공한다.
영속 상태 provider와 예정 시각 reminder는 application이 별도 저장소와 scheduler로 구성해야 한다.
분산 transaction도 제공하지 않는다. 기존 Orleans/Akka 시스템의 전환 여부는 이 차이와 운영 경험을
함께 비교해 결정한다.

## 8. 라이선스 — 사용하는 데 드는 비용

기술 선택에는 라이선스 조건이 함께 들어간다. Akka는 연매출이 기준선을 넘으면 상용 계약이
필요한 BSL이고, Orleans는 MIT다. ZLink는 계층마다 라이선스가 다르다.

| 계층 | 라이선스 |
| --- | --- |
| `core`, `bindings` — 메시징 엔진과 언어별 native binding | [Mozilla Public License 2.0](../../../../../../LICENSE) |
| `framework` — 이 가이드가 다루는 Spot/actor·channel messaging·STREAM·drain | [Functional Source License 1.1, ALv2 Future License](../../../../../LICENSE) |
| 각 언어의 `http-client` 패키지 | Apache License 2.0 |

FSL-1.1-ALv2는 ZLink와 경쟁하는 제품으로 파는 것만 막는다. ZLink와 경쟁하는 제품으로 파는 것만 막고, 나머지는 다
허용하며, 각 릴리스는 공개 2년 뒤 Apache-2.0이 된다.

| | |
| --- | --- |
| 사용할 수 있다 | 자기 제품·서비스를 만들어 배포하고 판매하는 것, 사내 시스템, 교육·연구 |
| 사용할 수 없다 | ZLink 자체를 대체하거나 실질적으로 같은 기능을 제공하는 상용 제품·서비스 |
| 비용 | 없다. 사용료도, 연매출 같은 유료 전환 기준선도 없다 |
| 2년 뒤 | 그 릴리스가 Apache-2.0으로 자동 전환된다 |

**결론.** 게임 서버든 업무 서버든 만들어서 서비스하고 판매하는 데 비용도 제약도 없다. Akka
BSL처럼 매출이 커지면 유료로 바뀌는 트리거가 없다.

`core`와 `bindings`가 MPL-2.0인 이유는 `core`가 MPL-2.0인 [libzmq](https://github.com/zeromq/libzmq)
v4.3.5에서 출발했기 때문이다. `http-client`는 각 플랫폼의 통상적인 HTTP client library를
감싼 얇은 계층이라 Apache-2.0이다.

정확한 조건은 [framework/LICENSE](../../../../../LICENSE)가, 정책 배경은
[doc/license/README.md](https://github.com/zlink-systems/zlink/blob/main/doc/license/README.md)가 소유한다.

## 9. 관련 문서

- 공통 업무 시나리오: [Framework Common Sample Scenarios](../../../common/sample/README.ko.md)
- 사용 방법: [Channel Messaging](20-channel-messaging.ko.md)
- 표면 매핑: [Channel 메시징](20-channel-messaging.ko.md) §0, [13. Interface 카탈로그](13-interface-catalog.ko.md) §1.6
- 실행 코드로 보는 샘플: [14-samples](14-samples.ko.md)

### 9.1 참고 자료

- [gRPC Performance Best Practices](https://grpc.io/docs/guides/performance/)
- [Performance best practices with gRPC (.NET)](https://learn.microsoft.com/en-us/aspnet/core/grpc/performance)
- [gRPC Load Balancing on Kubernetes without Tears](https://kubernetes.io/blog/2018/11/07/grpc-load-balancing-on-kubernetes-without-tears/)
- [System Design Study: Netflix's adoption of Service Mesh](https://vivekbansal.substack.com/p/system-design-study-netflixs-adoption)
- [Scaling Microservices: Lessons from Netflix, Uber, Amazon, and Spotify](https://www.netguru.com/blog/scaling-microservices)
- [Orleans overview (Microsoft Learn)](https://learn.microsoft.com/en-us/dotnet/orleans/overview)
- [Akka License Change의 영향 (Coralogix)](https://coralogix.com/blog/akka-license-change/)

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=Math.max(d.body?d.body.scrollHeight:0,d.documentElement?d.documentElement.scrollHeight:0);if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
