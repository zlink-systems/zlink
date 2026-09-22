---
title: "Channel 동작 원리 · Node/TypeScript"
---

<!-- generated:start -->
<!-- 이 파일은 `common/guide/server/30-channel-patterns.ko.md`에서 생성한다. 직접 고치지 않는다.
     고칠 곳은 공통 소스이고, `python3 doc/site/scripts/generate_language_guides.py`로 다시 만든다. -->
<!-- generated:end -->

# Channel 동작 원리

<!-- framework-adapter-nav:start -->
[가이드 홈](README.ko.md) | [이전: Relocation](37-relocation.ko.md) | [다음: Handler와 메시지 처리](31-handler-dispatch.ko.md)
<!-- framework-adapter-nav:end -->

<!-- language-switch:start -->
다른 언어로 보기 — [C++](../../../cpp/guide/server/30-channel-patterns.ko.md) · [C#/.NET](../../../dotnet/guide/server/30-channel-patterns.ko.md) · [Java](../../../java/guide/server/30-channel-patterns.ko.md) · [Kotlin](../../../kotlin/guide/server/30-channel-patterns.ko.md) · **Node/TypeScript**
{ .zlink-langswitch }
<!-- language-switch:end -->

이 장의 코드는 [tutorial README](https://github.com/zlink-systems/zlink-node-examples/blob/main/tutorial/README.ko.md)와 [`TicTacToe`](https://github.com/zlink-systems/zlink-node-examples/blob/main/samples/TicTacToe/README.ko.md)·[`ZoneWorld`](https://github.com/zlink-systems/zlink-node-examples/blob/main/samples/ZoneWorld/README.ko.md) 샘플 README에서 가져왔다. [예제 저장소](https://github.com/zlink-systems/zlink-node-examples/blob/main/tutorial/README.ko.md)를 내려받아 각 README의 「내려받기와 설치」·「빌드」·「실행」 절을 따라 실행하면 설명한 연결과 대상 선택을 재현할 수 있다.

!!! info "이 장을 읽고 나면"

    각 구성이 어떤 연결을 열고, 누구를 대상으로 고르고, 언제 거부되는지 알 수 있다.
    이 장의 코드는 저장소의 샘플과 튜토리얼에서 가져왔다.

[Channel 메시징](20-channel-messaging.ko.md)이 등록하고 호출하는 방법을 다뤘다면, 이 장은
**왜 그렇게 되는지와 어디까지 되는지**를 다룬다. 패턴 사이의 차이, 대상 선택 규칙, 연결과
discovery, 시작 단계 검증, 그리고 실패했을 때 호출한 쪽이 보는 결과를 설명한다.

## 1. 패턴별 비교

| | RouteMesh | ClientServer | Fanout |
| --- | --- | --- | --- |
| 호출 방향 | 양쪽이 서로 호출한다 | client에서 server로만 | publisher에서 subscriber로만 |
| 수신자를 정하는 주체 | Framework | 호출하는 쪽이 연결한 server 집합 | 정하지 않는다 |
| 한 호출이 도달하는 node | 담당 node 하나 | 선택된 server 하나 | 구독한 전부 |
| 사용 가능한 호출 | `request` · `send` | `request` · `send` | `publish` |
| 응답 | `request`만 받는다 | `request`만 받는다 | 없다 |
| 연결 | mesh peer 연결 하나를 여러 channel이 공유 | server가 공개한 endpoint로 client가 연결 | publisher PUB endpoint마다 subscriber SUB socket |
| 보내는 node 자신이 Server일 때 | **후보가 아니다** | 다른 server와 같은 후보다 | 해당 없음 |
| 후보가 없을 때 | 즉시 대상 없음으로 실패 | 잠깐 기다린 뒤 실패 | 받는 node 없이 성공 |
| 손실 | 없다 | 없다 | **느린 구독자 몫을 버린다**(NoDrop이면 오류로 끝난다) |

호출 코드는 RouteMesh와 ClientServer가 같다. `sendToChannel`·`requestToChannel`은 ChannelName
하나로 process-local 송신 경로를 고르며, 그 경로가 RouteMesh인지 ClientServer인지는 등록이
정한다. **두 패턴을 바꾸는 작업은 등록만 바꾸는 작업이다.**

## 2. 물리 배선 — 구성마다 다른 socket을 연다

각 구성은 이름만 다른 것이 아니라 **서로 다른 socket을 연다.** 한 process가 이들을 모두 사용하면
listener도 셋이 따로 생긴다.

<iframe class="zlink-diagram" src="/common/diagrams/30-wiring.html" title="세 패턴은 서로 다른 소켓을 연다" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/30-wiring.html" target="_blank">↗ 크게 보기</a></p>

| | 여는 listener | 연결 단위 | channel을 늘리면 |
| --- | --- | --- | --- |
| RouteMesh | MeshNode마다 ROUTER 하나 | node 사이 peer 연결 | **socket이 늘지 않는다** |
| ClientServer | Server channel마다 하나 | channel 전용 연결 | channel 수만큼 listener가 는다 |
| Fanout | publisher마다 PUB 하나 | publisher ↔ subscriber SUB | publisher 수만큼 는다 |

### 2.1 RouteMesh — 연결 하나를 여러 channel이 공유한다

MeshNode는 routing id 하나와 peer가 연결할 ROUTER endpoint 하나를 가진다. 그 위에 등록한
ChannelName이 몇 개든 **같은 ROUTER 연결을 함께 사용한다.** ChannelName을 추가해도 socket이나
node 사이 연결은 추가되지 않는다.

그래서 mesh에 이미 참여한 node끼리는 channel을 새로 만드는 비용이 이름 등록뿐이다. 반대로
mesh에 참여하지 않은 process는 그 channel을 호출할 수 없다.

수동 연결은 연결할 endpoint를 MeshNode에 등록한다. endpoint만 적을 수도 있고, 그 자리에 있어야
할 node의 routing id를 함께 적을 수도 있다.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Api/tictactoe-api-module.ts:doc-manual-peer-connect"
```

### 2.2 ClientServer — channel마다 자기 listener를 연다

ClientServer Server는 mesh와 무관하게 **자기 포트를 열고 주소를 알린다.** channel이 둘이면
listener도 둘이고, 두 channel은 연결 대상과 수명을 공유하지 않는다. 같은 process가 mesh에도
참여한다면 ROUTER listener는 그것대로 따로 있다.

client는 그 주소로 직접 연결하므로 **mesh에 참여하지 않아도 호출할 수 있다.** 외부 팀이 만든
서비스나 별도 배포 단위를 호출할 때 이 구성을 사용하는 이유다.

### 2.3 Fanout — PUB/SUB 소켓 쌍을 따로 연다

Fanout publisher는 PUB listener를 열고, subscriber는 publisher endpoint마다 전용 SUB socket을
사용한다. MeshNode나 Spot과 소켓을 공유하지 않는다. 그래서 mesh 구성과 상관없이 전달되고,
[Logical Multicast](#51-logical-multicast--mesh-위의-spot-사이-이벤트)와 대상 집합도 공유하지
않는다.

RouteMesh·ClientServer·fanout publisher·STREAM server는 모두 별개의 listener이며 process 기본
network 값을 함께 사용한다. 특정 listener에만 다른 bind·advertise 주소가 필요하면 그 listener에
override를 지정한다.

## 3. 호출 방향

### 3.1 RouteMesh — role 등록이 방향을 정한다

RouteMesh 자체에는 방향이 없다. 방향은 node가 ChannelName마다 등록한 role이 정한다.

- `Server()`를 등록한 node만 그 channel의 호출을 받는다.
- `Client()`만 등록한 node는 호출을 시작할 수 있지만 받지는 않는다.
- **`Server()` role은 송신 기능을 포함한다.** 같은 ChannelName에 `Client()`를 다시 등록하지
  않는다.

한 MeshNode 위에 channel을 여러 개 등록할 수 있고 channel마다 role이 다를 수 있다.

```typescript
--8<-- "framework/languages/node/samples/ZoneWorld/Server/ZoneNode/zone-node-module.ts:doc-multi-channel-register"
```

그래서 A가 `api`의 Server이고 B가 `billing`의 Server이면 A→B와 B→A가 모두 성립한다. 두 호출은
같은 peer 연결을 사용하지만 서로 다른 channel이다. **channel 하나가 양방향이 되는 것이 아니라,
두 channel이 서로 반대 방향으로 등록된 것이다.**

role 목록은 startup 뒤에 바꿀 수 없다. 실행 중에 바꿀 수 있는 것은 Server membership의
weight뿐이다 — [실행 중에 새 요청만 멈추기](#43-실행-중에-새-요청만-멈추기).

### 3.2 ClientServer — 방향이 고정된다

ClientServer는 **단방향 service 경계**다. 다음이 함께 고정된다.

| 고정되는 것 | 내용 |
| --- | --- |
| 업무 호출 방향 | server는 client를 대상으로 새 호출을 시작하지 않는다 |
| 연결 시작 방향 | manual·automatic discovery 모두 client만 server로 connection을 시작한다 |

client가 server에서 받는 것은 **자신이 시작한 request에 대응하는 reply뿐**이다. client request
없이 server가 먼저 보낸 message는 받지 않는다.

반대 방향 호출이 필요하면 반대쪽 node가 자신의 endpoint를 공개하는 server가 되어야 하며, 그것은
별도 ChannelName을 가진 별도 channel이다.

한 process가 같은 ChannelName에 Client와 Server를 각각 한 번씩 등록할 수는 있다. 모니터링
snapshot의 `client_and_server`는 그 두 registration이 함께 있다는 표현이며 별도 role이 아니다.

## 4. ToChannel 호출의 대상 node 선택

`sendToChannel`·`requestToChannel`이 **어느 node로 갈지 정하는 규칙**이다. RouteMesh와
ClientServer 두 구성에만 해당한다. node를 직접 지정하는 호출과 Spot·Actor 호출은 대상이
이미 정해져 있으므로 선택이 없고, `publish`는 대상을 고르지 않는다.

### 4.1 대상 선택 — 같은 process의 Server를 RouteMesh에서만 제외한다

한 process가 **호출하는 쪽이면서 동시에 그 channel을 처리하는 쪽**일 수 있다. 이때 자기
process의 Server를 후보로 볼지는 두 구성이 다르다.

| | RouteMesh | ClientServer |
| --- | --- | --- |
| 같은 process에 그 channel의 Server 등록이 함께 있을 때 | **후보가 아니다** | 다른 server와 같은 후보다 |
| 그 process 말고 다른 곳에 Server가 없으면 | **대상 없음으로 실패한다** | 자기 process의 server가 선택된다 |
| 후보가 아직 하나도 없을 때 | 즉시 실패한다 | **잠깐 기다린 뒤** 실패한다 |

**ClientServer는 한 process가 같은 ChannelName에 Client와 Server를 각각 한 번씩 등록할 수
있다.** 그러면 그 process는 호출도 하고 처리도 한다. 이때 자기 process의 server는 remote
server와 같은 조건(준비 상태·weight·종료 절차 여부)으로 후보에 들어가며, local이라고 우선하거나 remote를
빼지 않는다.

선택되더라도 **handler를 직접 호출하지 않는다.** 정규 송신 경로를 그대로 거치므로 codec,
timeout, 취소, correlation이 remote와 똑같이 적용된다. local handler를 곧바로 호출하는 별도
경로는 제공하지 않는다.

**RouteMesh가 자기 process를 빼는 이유는 구조에 있다.** channel 등록은 새 socket을 만들지 않고
**이미 있는 peer 연결**을 사용하는데, MeshNode는 자기 자신과 peer 연결을 맺지 않는다. 그래서 그
channel의 Server가 자기 process에만 있으면 보낼 경로가 없다. 같은 process에서 처리되게 하려면
ClientServer를 사용한다.

기다리는 쪽도 이유가 있다. RouteMesh에서 후보가 없다는 것은 그 이름을 게시한 peer가 없다는
뜻이라 기다려도 생기지 않는다. ClientServer에서는 같은 process에 설정이 이미 있고 준비만 안
끝났을 수 있다. 그래서 **호출의 timeout과 5초 중 짧은 쪽**만큼 기다린다. 이 대기는 준비를
앞당기지 않고 진행 중인 준비가 끝나기를 기다릴 뿐이다.

### 4.2 후보 사이의 분배 — 기본값은 round-robin, 값을 바꾸면 가중치 분배

먼저 후보에서 빠지는 대상이 있다. 두 구성에서 같다.

| 제외되는 대상 | 이유 |
| --- | --- |
| ready가 아닌 target | 초기화와 discovery 기록이 끝나지 않았다 |
| weight가 `0`인 target | membership은 유지하되 새 선택의 후보가 아니다 |
| 안전 종료 절차에 들어간 target | 남은 요청을 처리하고 내려가는 중이다 |

**위 대상을 뺀 뒤 남는 대상이 하나도 없으면 호출은 `Unavailable`로 끝난다.** `request`와 `send`가
같은 결과를 낸다. 송신 경로와 연결은 그대로 있고 고를 대상만 없다는 뜻이라 `NotFound`가 아니다
— 그쪽은 routing id로 대상을 적었는데 그 id를 아는 node가 없을 때의 결과다. framework
0.16.0이 정한 값이다.

!!! warning "C++의 one-way send는 `Unavailable`과 다르다"

    0.16.0에서 .NET·Java·Kotlin·Node는 `request`와 `send` 모두 `Unavailable`을 낸다. **C++만
    `send`가 `NotFound`를 낸다. `request`는 C++에서도
    `Unavailable`이다.

남은 대상 사이의 분배는 **weight가 정한다.** weight는 `0..10000` 범위이고 기본값은 `100`이다.

**값을 지정하지 않으면 모두 `100`이므로 균등 round-robin이 된다.** 서버를 몇 대 띄우든 새 요청이 돌아
가며 분배된다는 뜻이다. 대부분의 구성은 여기서 끝난다.

값을 다르게 주면 그 비율로 분배된다. 두 후보가 `100`과 `300`이면 길게 보아 약 `1:3`이다 —
**호출 하나하나의 순서를 보장한다는 뜻은 아니다.** 사양이 다른 장비를 섞어 사용하거나 한 대에
트래픽을 덜 보내야 할 때 사용한다.

초기값은 등록할 때 정한다. channel role을 등록하는 자리에서 `setWeight(...)`로 준다. 값을 주지
않으면 `100`이다. 실행 중 변경은 [실행 중에 새 요청만 멈추기](#43-실행-중에-새-요청만-멈추기)가
다룬다.

<iframe class="zlink-diagram" src="/common/diagrams/05-node-select.html" title="round-robin 분산 · node 추가 자동 반영" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/05-node-select.html" target="_blank">↗ 크게 보기</a></p>

### 4.3 실행 중에 새 요청만 멈추기

유지보수나 순차 재시작을 앞두고, node를 내리지 않은 채 **새 요청만 받지 않게** 하고 싶을 때가
있다. weight를 `0`으로 바꾸면 된다. **실행 중에 바꿀 수 있는 값은 이것 하나다.** RouteMesh
runtime 옵션을 주입받아 ChannelName으로 지정한다.

```typescript
--8<-- "framework/languages/node/tutorial/Server/main.ts:weight-runtime"
```

- `Weight = 0`은 serving socket을 **닫지 않는다.** 이미 들어온 요청은 끝까지 처리하고 응답하며,
  다른 node가 이 node를 새 요청 대상에서만 뺀다. Location Store의 등록 정보도 그대로 남는다.
- **그 channel을 맡은 node가 이 하나뿐이면 새 호출이 `Unavailable`로 끝난다.** 후보가 비는
  것이지 경로가 끊긴 것이 아니다 — [후보 사이의 분배](#42-후보-사이의-분배--기본값은-round-robin-값을-바꾸면-가중치-분배).
- `Weight = 100`이 정상 복귀다.
- 전파는 즉시가 아니다. 보장되는 것은 **신호를 보냈다는 것까지**이고, 다른 node가 실제로
  후보에서 뺐는지는 그 node의 상태로 확인한다([모니터링](26-monitoring.ko.md)).
- 운영에서는 이 두 동작을 흔히 `drain`·`restore`라고 한다. application의 관리 API가 `Weight = 0`·`= 100`에
  그 이름을 붙인 것이며, **Framework가 그 이름의 API를 제공하지는 않는다.**

### 4.4 같은 ChannelName을 여러 node로 늘리기

처리량을 늘리려면 같은 MeshName과 ChannelName을 맡은 provider를 여러 개 실행한다.

**처리하는 쪽은 바꿀 것이 없다.** 같은 ChannelName을 `Server()`로 등록한 process를 한 대 더
띄우면 된다. 등록 코드는 node 수와 무관하게 같다.

**호출하는 쪽은 Location Store를 사용하면 역시 바꿀 것이 없다.** Store가 새 provider의 등록 정보를
갖고 있으므로 후보가 저절로 늘어난다.

주소를 직접 적는 구성이라면 provider endpoint를 모두 등록한다. 아래는 처리 node 두 대에
연결하는 코드다.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Api/tictactoe-api-module.ts:doc-manual-peer-connect"
```

이 경우 provider를 늘릴 때마다 호출하는 쪽 설정을 고치고 다시 시작해야 한다. 그래서 node 수가
변하는 구성에는 Location Store를 사용한다.

**등록되지 않은 ChannelName은 다른 곳에서 찾아 주지 않는다.** 같은 process에 다른 MeshNode나
ClientServer client가 있어도 그쪽으로 대신 보내지 않는다. 반대로 한 process에서 같은
ChannelName을 RouteMesh와 ClientServer 양쪽에 등록하는 것도 **시작할 때 거부된다** — 이름 하나가
송신 경로 하나만 가리켜야 한다.

특정 엔티티(주문 ID·사용자 ID)를 늘 같은 실행 단위가 처리해야 하면 channel이 아니라 Spot이나
actor를 사용한다([Spot](21-spot.ko.md)).

### 4.5 Spot handler가 시작하는 channel 호출

Spot handler와 timer는 channel send · request를 시작할 수 있다. **그 Spot을 소유한
MeshNode에 해당 ChannelName이 없어도 된다** — 같은 process에 그 이름의 송신 경로가
하나라도 등록되어 있으면 사용할 수 있다. 다른 RouteMesh의 경로여도, ClientServer client의
경로여도 된다.

**같은 process에 없으면 거기서 끝난다.** 다른 process나 다른 MeshNode를 중계로 삼아
찾아 주지 않으며 `NotFound`로 끝난다. 그래서 Spot을 배치할 node를 정할 때 **그 Spot이
호출할 channel의 송신 경로가 같은 process에 등록되어 있는지**를 함께 본다.

## 5. pub/sub의 형태

발행에는 **대상 선택이 없다.** 구독한 쪽이 받을 뿐이다. 그 발행·구독의 형태는 다음과 같이 나뉜다. 이름이 비슷해서 섞이기 쉬운데 **소켓·대상 범위·손실
규칙이 모두 다르다.**

| | Logical Multicast | Classic fanout |
| --- | --- | --- |
| 소켓 | 이미 연결된 mesh 소켓을 그대로 사용한다 | 독립 PUB/SUB 소켓 쌍을 연다 |
| 받는 대상 | 그 channel에서 같은 topic을 구독한 **Spot** | 연결된 **구독자 전원** |
| mesh 구성과의 관계 | 그 mesh 안으로 한정된다 | 무관하다 |
| 손실 | 해당 없음 | **허용한다**(NoDrop이면 오류로 끝난다) |
| filter | 실행되지 않는다 | 실행된다 |
| 등록 위치 | Spot이 시작할 때 | fanout channel builder |

### 5.1 Logical Multicast — mesh 위의 Spot 사이 이벤트

[Spot](21-spot.ko.md)은 id로 찾는 상태 객체이고 자기 앞으로 온 일을 한 줄로 세워 처리한다.
RouteMesh channel 위에서 그 Spot끼리 이벤트를 주고받는 것을 Logical Multicast라 한다. 별도
소켓이 없고, 받는 쪽은 그 channel에서 같은 topic을 구독한 Spot으로 한정된다.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/TicTacToeGameSpot/tictactoe-game-spot.ts:doc-multicast-publish"
```

발행 호출은 topic을 받는다. ChannelName까지 함께 받는 언어도 있고, Spot이 등록된 mesh를
그대로 사용하는 언어도 있다 — 위 탭에서 그 언어의 것을 본다.

구독은 그 topic을 받을 handler를 Spot이 시작할 때 등록해 연다. **등록에 topic을 적는 자리가
언어마다 다르다** — 등록 호출의 인자로 넘기거나, handler 타입에 표시한다.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Play/Infrastructure/ZLink/Spots/EntrySpot/play-entry-spot.ts:doc-multicast-subscribe"
```

Spot 밖에서 발행해야 하면 spot publisher client를 주입받아 같은 방식으로 보낸다.

### 5.2 Classic fanout — 손실을 허용하는 전달

fanout channel은 그 자체로 독립된 PUB/SUB 소켓 쌍을 연다. Spot이나 MeshNode와 무관하게 발행자
하나가 연결된 구독자 전원에게 전달한다.

**손실 규칙이 다르다.** 어느 구독자의 수신이 늦어 발행자의 송신 queue가 상한(`sendHighWaterMark`)에
닿으면 **그 구독자 몫을 버리고 발행은 성공으로 끝난다.** 나머지 구독자는 영향을 받지 않고,
발행자는 느린 구독자 하나 때문에 멈추지 않는다.

**느린 구독자 몫도 버리지 않으려면 NoDrop을 켠다.** 그러면 느린 구독자의 backpressure가
발행을 기다리게 하고, deadline 안에 비워지지 않으면 발행은 오류로 끝난다.

```typescript
builder.addFanoutChannel('events')
  .enablePublisher('tcp://*:7400')
  .setNoDrop(true);
```

NoDrop은 publisher capability가 있는 channel에서만 설정한다. Logical Multicast도 저장·재전송·ack는 제공하지 않는다.

### 5.3 Topic — 두 형태 모두 수신 대상을 고른다

topic은 두 형태에서 모두 수신 대상을 고른다. Logical Multicast는 Spot을 고르고, Classic fanout은 subscriber socket을 고른다.

| | Logical Multicast | Classic fanout |
| --- | --- | --- |
| 구독 등록 | ChannelName + **topic** | ChannelName + **topic** |
| topic이 전달을 거르는가 | **거른다.** 같은 topic을 구독한 Spot만 받는다 | **거른다.** 같은 topic을 구독한 subscriber만 받는다 |
| 받은 뒤 | 해당 Spot의 구독 handler가 처리한다 | packet 이름으로 handler를 찾고, 없으면 버린다 |

Classic fanout subscriber는 받을 topic을 `subscribe(topic)`으로 등록한다. 같은 topic에 발행한 event만 그 subscriber까지 전달된다.

```typescript
builder.addFanoutChannel('events')
  .enableSubscriber()
  .subscribe('order.created');
```

handler가 함께 받는 publish context에는 도착한 topic도 들어 있으므로, 하나의 handler가 여러 topic을 등록해 나누어 처리할 수도 있다.

### 5.4 발행할 때 topic을 정하는 방법

- `Publish(channelName, message)` — topic을 지정하지 않는다. 이때 event의 **packet 이름**이
  topic이 된다.
- `Publish(channelName, topic, message)` — topic을 직접 정한다.
- 예약된 topic을 넘기면 `ArgumentException`으로 거부한다. Framework가 연결 상태 확인에 사용하는
  값이다.

두 형태 모두 subscriber별 acknowledgement나 replay state를 갖지 않는다. 구독자가 연결되지 않은
동안 발행된 event는 나중에 전달되지 않는다.

## 6. 연결과 discovery

| | 주소를 공개하는 쪽 | 연결을 시작하는 쪽 | discovery |
| --- | --- | --- | --- |
| RouteMesh | 양쪽 node가 각자 endpoint를 연다 | 두 node가 동시에 시작하지 않도록 한쪽이 정해진다 | MeshNode descriptor |
| ClientServer | server | client | manual `connect` 또는 Location Store의 server descriptor |
| Fanout | publisher | subscriber | Location Store의 publisher descriptor |

### 6.1 Location Store — 누가 어디 있는지 적어 두는 곳

앞의 표에 계속 나오는 **Location Store**는 Framework 밖에 두는 저장소다. 시스템의 구성 요소
하나이며, 실행 중인 process들이 서로를 찾는 데 사용한다. 튜토리얼과 샘플은 Redis를 사용한다.

<iframe class="zlink-diagram" src="/common/diagrams/30-location-store.html" title="Location Store — 누가 어디 있는지 적어 두는 곳" style="width:100%;border:0"></iframe>
<p><a href="/common/diagrams/30-location-store.html" target="_blank">↗ 크게 보기</a></p>

**각 process가 시작할 때 자기 주소와 자기가 맡은 이름을 Store에 적고**, 살아
있는 동안 그 기록을 갱신한다. 멈추면 기록이 사라진다. 호출하는 쪽은 Store를 읽어 연결할 곳을
찾는다.

그래서 서버를 늘리거나 다른 주소로 다시 시작해도 **호출하는 쪽 설정을 고치지 않는다.** 이것이
`connect(...)`로 주소를 직접 적는 방식과 달라지는 지점이다.

Store가 보관하는 것은 주소만이 아니다.

| 적히는 것 | 사용하는 곳 |
| --- | --- |
| MeshNode의 주소와 맡은 ChannelName | RouteMesh 자동 연결 |
| ClientServer Server의 주소 | client가 연결할 대상 찾기 |
| Fanout publisher의 주소 | subscriber가 연결할 대상 찾기 |
| Spot·Actor가 지금 있는 node | id로 호출할 때 위치 해석 |
| 같은 Spot을 두 곳에서 만들지 않게 하는 생성 권한 | Spot 생성 |

**Spot과 Actor를 사용하려면 Store가 반드시 있어야 한다.** 그 기능은 "지금 어느 node에 있는가"를
Store에서 읽어 결정하기 때문이다. channel 메시징만 사용하고 주소를 직접 적는다면 Store 없이도
동작한다.

등록 코드는 [Spot](21-spot.ko.md#2-location-store--spot-등록의-선행-조건)에, 운영 조회는
[운영과 lifecycle](12-operations.ko.md#5-location-readiness와-운영-조회)에 있다.

**Location Store와 Relocation Store는 맡는 기록이 다르다.** Location Store는 작은 위치 기록의 원자적 변경을 맡고, Relocation Store는
옮기고 난 뒤에 남는 기록 — Instance Spot을 처음 깨운 기록과 이동 뒤에 완료되는 요청의 종결
기록 — 을 담는다. 옮겨 가는 상태와 queue와 timer 자체는 Store를 거치지 않는다 —
[Relocation](37-relocation.ko.md#1-옮겨도-그대로-남는-것)이 그 경로를 다룬다.

| Store | 언제 필요한가 |
| --- | --- |
| Location Store | object 역할이 client나 server인 MeshNode에 **필수**. 없으면 socket을 열기 전에 설정 오류로 끝난다 |
| Relocation Store | 이동 정책을 가진 factory나 Instance Spot factory가 **하나라도 있으면 필수**. 전부 이동을 끄고 Instance Spot factory도 없을 때만 없어도 된다 |

**각각 정확히 한 번 등록한다.** 둘을 하나의 등록 호출로 묶는 표면은 없고, 필요한데 빠뜨리거나
둘 이상 등록하면 socket을 열기 전에 설정 오류다. Store가 없을 때 Framework가 process 안에 대체
Store를 만들어 주지 않는다 — **단일 node로 조용히 동작하지 않고 실패한다.**

두 Store는 같은 Redis 배포를 사용해도 된다. key prefix는 서로 다르게 둔다. Framework는 Store를
가로지르는 transaction에 기대지 않으므로 필요하면 물리 Redis도 분리할 수 있다. Store를 등록한 뒤
application이 provider 동작을 직접 호출하거나 정리하지 않는다 — Framework가 Store의 수명과 호출
순서를 관리한다.

### 6.2 owner lease — 소유권이 살아 있다는 표시

owner는 자기가 살아 있다는 표시를 주기적으로 갱신하고, 그 표시가 만료되면 다른 node가 그 owner가
된다. 갱신 주기·만료 시간·갱신 한 번의 상한·만료 전에 미리 끊는 여유는 **서로 묶여 있다.**
관계를 어기면 시작할 때 오류다. 값을 바꿀 때는 이 값들을 함께 본다. 값 이름과
기본값은 언어별 옵션 장이 소유한다.

lease 갱신이 끊긴 host는 계산해 둔 시각을 넘는 순간 **새 작업을 받지 않는다** — 상태를 바꾸는
message와 timer 시작, factory와 복원 결과의 확정, 이동 상태 변경과 수용 공간 확보가 모두 막힌다.
이미 queue에 들어온 작업의 마무리와 정리는 계속한다. 새 owner와 옛 owner가 동시에 사용하는 것을 막는
장치다.

### 6.3 Store가 멎어 있는 동안

Store 장애에 대비한 유예 시간이 있다. 그것은 마지막으로 완전히 읽은 node 목록을 유지해 주는
시간이지, **소유권을 연장해 주는 시간이 아니다.**

| 유예 동안 | 결과 |
| --- | --- |
| 이미 맺은 연결 | 상태 판단을 계속한다 |
| 새 바깥 연결 | 만들지 않는다. 유예가 끝나도 node 목록 전체를 같은 시점으로 다시 읽기 전에는 만들지 않는다 |
| owner lease와 이동 기한 | **연장되지 않는다** |

이미 받은 요청은 유지되고, 멈추는 것은 대상 목록의 추가·제거 계산이다. store가 복구되면 최신
등록 정보를 기준으로 목록을 다시 맞춘다.

### 6.4 manual과 automatic

수동 연결은 MeshNode의 peer 목록에 설정한다.

```typescript
--8<-- "framework/languages/node/samples/TicTacToe.Ts/Server/Api/tictactoe-api-module.ts:doc-manual-peer-connect"
```

endpoint 인자는 startup 설정이다. host 시작 뒤 실행 중인 socket을 직접 제어하는 handle이 아니다.
실행 중에 바꿀 수 있는 값은 [실행 중에 새 요청만 멈추기](#43-실행-중에-새-요청만-멈추기)의 weight뿐이다.

자동 연결 모드는 peer 목록의 소유권이 Location Store에 있다. server가 새 endpoint로 다시
시작하면 store의 descriptor row가 갱신되고 client 연결도 따라 갱신되므로 별도 조작이 필요 없다.
**수동 연결은 설정을 바꾼 뒤 application을 다시 시작해야 적용된다.**

Fanout subscriber는 **automatic discovery와 manual endpoint를 함께 지정할 수 없다.** 둘을 같이
등록하면 시작할 때 거부된다.

### 6.5 store에서 찾았다고 바로 보내지 않는다

client는 등록 정보에서 endpoint를 얻은 뒤 **실제 연결에서 신원과 lifecycle generation을 다시 확인**하고
나서야 그 대상을 사용한다. 수동 연결도 같은 확인을 거친다. 그래서 store에 row가 있는데도 호출이 대상
없음으로 끝날 수 있다 — 그때는 store가 아니라 **연결이 맺어졌는지**를 본다.

**server를 재시작하면 lifecycle generation이 바뀐다.** 이는 node가 몇 번째 실행 중인지를 나타내는
값이다. endpoint가 같아도 이전 lifecycle generation의 연결은 새 대상으로 사용하지 않고, client가
새 값을 준비한 뒤 이전 연결을 해제한다. lifecycle generation 값은 숫자 크기로 순서를 판단하지 않는다.

늦게 도착한 reply는 **원래 요청이 아직 기다리고 있으면 그 결과가 된다** — 이전 세대에서 온
것이어도 그렇다. 반대로 timeout·취소·client 재시작으로 그 요청이 사라졌으면 버리고, **나중에
시작한 다른 요청의 결과로 사용하지 않는다.**

## 7. 호출이 끝났다는 것의 의미

| 호출 | 완료 시점 |
| --- | --- |
| `request` | 대상 handler가 반환한 reply가 도착했다 |
| `send` | source-local queue가 message를 수락했다 |
| `publish` | source-local publish admission이 끝났다 |

`send`와 `publish`의 완료는 **전달을 보장하지 않는다.** `publish`는 구독자 수도 수신 완료도
반환하지 않는다. 처리 결과가 필요하면 `request`를 사용할 수 있는 패턴을 고른다.

**handler가 없는 packet으로 보냈을 때**는 경로마다 다르다.

| 호출 | 결과 |
| --- | --- |
| `request` | error reply로 실패한다. 호출한 쪽은 예외로 받는다 |
| `send` | 조용히 drop된다 |

drop은 호출한 쪽에 reply가 없다는 뜻이지 관측 흔적이 없다는 뜻이 아니다. 구성한
logger·telemetry provider에는 dispatch 실패가 `no_handler`·`reply_error`·`drop` structured
record로 남는다([모니터링](26-monitoring.ko.md)).

## 8. 관련 문서

- 등록과 호출 방법 — [Channel 메시징](20-channel-messaging.ko.md)
- 공통 처리를 한곳에 모으기 — [Filter](31-handler-dispatch.ko.md#2-filter--공통-처리를-한곳에-모은다)
- 직렬화 codec — [Codec](31-handler-dispatch.ko.md#3-codec--payload를-바이트로-바꾼다)
- 이 장 코드의 실행본 — `framework/languages/dotnet/tutorial`

<script>
(function(){function s(f){try{var d=f.contentDocument;var h=d.body?d.body.scrollHeight:0;if(h<40&&d.documentElement)h=d.documentElement.scrollHeight;if(h>40)f.style.height=h+"px";}catch(e){}}document.querySelectorAll("iframe.zlink-diagram").forEach(function(f){f.addEventListener("load",function(){setTimeout(function(){s(f);},250);});});[400,1000,2000].forEach(function(t){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},t);});window.addEventListener("resize",function(){setTimeout(function(){document.querySelectorAll("iframe.zlink-diagram").forEach(s);},150);});})();
</script>
