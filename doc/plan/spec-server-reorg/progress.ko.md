# 샘플 통일 작업 — 진행 상황

기준 시각: 2026-08-26. 이 문서는 **지금 어디까지 왔고 무엇이 남았는지**만 적는다.
판정 근거와 조사 기록은 [spec-gap.ko.md](spec-gap.ko.md), 캠페인 전체 계획은
[README.ko.md](README.ko.md)에 있다.

## 1. 한눈에

| 샘플 | 스펙 §10.1 | 구현 (5언어 실행 검증) |
|---|---|---|
| Bingo | 완료 | **완료** — dotnet·cpp·node·java·kotlin |
| TicTacToe | 완료 | **완료** — dotnet·cpp·node·java·kotlin |
| DeliveryDispatch | 완료 | **완료** — dotnet·cpp·node·java·kotlin |
| GameQuest | 완료 | **완료** — dotnet·cpp·node·java·kotlin |
| ShoppingMall | 완료 | **완료** — dotnet·cpp·node·java·kotlin (dotnet 러너 3/3 연속, 단위 1879/1879) |
| SupportChat | 완료 | **완료** — dotnet·cpp·node·java·kotlin |
| ZoneWorld | (기존) | **미완료** — node 통과, cpp·dotnet 실패(§8.1.4·§8.1.5) |

**스펙 6/6 완료, 6샘플 구현 6/6 완료.** 7번째 ZoneWorld는 이 캠페인 이전부터 있던 결함 둘이 남았다(§8.1.4·§8.1.5).

## 2. 무엇을 하는 작업인가

같은 샘플을 다섯 언어로 구현해 두었는데, **runner가 확인하는 사실과 그 문자열이 언어마다
달랐다.** 어떤 언어는 11건을 확인하고 어떤 언어는 1건만 확인했다. 같은 사실을 서로 다른
문자열로 적었고, 완료 marker조차 `=`가 있는 것과 없는 것으로 갈렸다.

각 샘플 스펙에 **§10.1**을 신설해 문자열·정확한 횟수·대기 예산·node 이름을 고정하고,
다섯 구현을 거기에 맞춘다. 통과 기준은 러너가 마지막에 `<sample>-placement=completed`를
출력하는 것이다.

## 3. 원인은 스펙이 갈라짐을 허가한 것이었다

세 샘플의 §10에 **언어별로 달라도 된다고 명시한 조항**이 있었다. 전부 삭제했다.

| 샘플 | 삭제한 조항 |
|---|---|
| DeliveryDispatch | "evidence marker의 이름은 해당 언어 runner가 실제로 출력하는 값을 사용" |
| GameQuest | "rehydrate나 scale-out처럼 특정 runner가 별도로 출력하는 marker는 해당 언어 runner의 실제 출력만 사용" |
| SupportChat | "authentication, assignment, reconnect 같은 self-check 이름을 공통 marker로 중복 선언하지 않는다" |

여섯 샘플에 공통으로 넣은 규칙은 넷이다.

1. **evidence는 샘플이 소유한 문자열이어야 한다.** framework가 찍는 줄
   (`ZLINK_FRAMEWORK_READY`, `message flow`, structured trace, 기동 boilerplate)은 완료 판정
   근거가 될 수 없다 — framework 사정으로 바뀌면 샘플 runner가 조용히 깨진다.
2. **readiness에 합성 요청을 보내지 않는다.** `/ready?targetRid=` 같은 runner 발신 probe 금지.
3. 대기는 `100 ms` 간격 최대 `300`회, **`.sh`와 `.ps1`이 같은 값**.
4. 전부 통과하면 `<sample>-placement=completed`, 하나라도 실패하면 미출력.

SupportChat 조사에서 나온 두 규칙은 다른 샘플에도 값어치가 크다.

- **marker는 그 사실이 일어난 자리에서 출력한다.** cpp가 8개 marker를 scenario 끝에서 조건 없이
  몰아 찍고 있었다. 앞에서 예외가 나면 한 줄도 안 찍히고 러너는 그 전에 종료 코드로 죽으므로,
  **8번의 `grep`이 "프로세스가 0으로 끝났다"를 여덟 번 확인**하는 것이 되어 있었다.
- **self-check는 실제 경로를 지나야 한다.** cpp self-check가 actor Spot·Session relay·wire codec을
  전부 우회하는 in-process 전용 경로로 미리 정해진 `…=verified` 문자열을 돌려주고 있었다.

## 4. 통일 과정에서 드러난 framework 버그 — 2건 수정

샘플이 덮고 있던 것이 걷히자 실제 버그가 나왔다. **둘 다 우회하지 않고 원인을 고쳤다.**

### 4.1 .NET — 고정 RID를 쓰면 object descriptor를 게시하지 않는다

`ZLinkSpotNodeInitializer.RequiresDescriptorClaim`이 `!HasExplicitRoutingId`를 조건에 넣고 있어,
**고정 RID를 쓰면 Location Store에 descriptor를 올리는 단계를 건너뛰었다.** 그 결과 object role
노드가 peer로는 READY로 보이면서 **User Spot 대상으로는 매칭되지 않았다**
(`No compatible User Spot target is available`). 증상은 타이밍 문제처럼 보였지만 아니었다.

조건에서 `!HasExplicitRoutingId`를 걷어냈다. 재claim 충돌은 기존 `RejectedConflict` 경로가 이미
처리하므로 새 계약이 필요 없었다. **dotnet 단위 1,879/1,879 통과(회귀 0).**

함께 정리한 것: 스펙 문서 다섯 곳(ko/en)에서 "고정 RID는 object role 없는 manual topology에서만"
제약을 걷어내고 **허용 근거를 문장으로 적었다** — 구현·시험 시나리오가 peer를 이름으로 지목해야
하는데 자동 UUID로는 그럴 수 없기 때문이다. 원래 문서에는 제약만 있고 이유가 없었다.

### 4.2 Java — 지연된 ADMIT이 건강한 peer를 교체하고 not-ready로 되돌린다

`ZLinkJavaRawMeshNode`에서 transport-pair identity가 없는 lane은 `HELLO`/`ADMIT`의 방향으로만
연결을 구분한다. 그래서 **지연·재전송된 ADMIT이 새 연결 ID를 받아 이미 admit된 peer를 교체하고
liveness를 not-ready로 되돌렸다.** canonical actor join이 `peer=true ready=false`를 보고 `JoinSpot`을
거부해 client가 영원히 기다렸다.

pair identity가 없는 단일 route 승인은 **기존 논리 연결 ID를 재사용**하도록 고쳤다. 아울러
`PEER_READY`가 매 관측마다 찍히던 것을 **not-ready → ready 전이에서만** 찍도록 고쳤다 — 같은 peer에
대해 15회 반복 출력되어 로그가 사실을 왜곡하고 있었다. 회귀 테스트 2건 추가.
**JVM core 단위 1,129 통과.**

이 버그를 java·kotlin 샘플이 **고정 `sleep 30`으로 덮고 있었다.** 스펙이 고정 sleep을 금지해
그것을 걷어내자 드러났다.

## 4.5 DeliveryDispatch에서 러너를 돌려 잡은 것

구현 에이전트 다섯이 모두 "§10.1 표에서 빠뜨린 것 없음"으로 보고했지만, **러너를 돌리니 세 건이
나왔다.** 보고를 판정 근거로 삼지 않는 이유다.

- **node — 브라우저 client의 marker를 읽을 파일이 없었다.** 러너가
  `assertLogCount('browser-client', …)`로 세는데, 공용 하네스의 `runBrowser()`는
  `stdio: 'inherit'`로 실행해 **로그 파일을 만들지 않는다**(`startBrowser()`만 만든다). 없는 파일을
  세니 0이었다. `runBrowser()`가 출력을 stdout과 `browser-client.log` 양쪽에 남기도록 고쳤다.
- **kotlin — readiness를 매직 넘버로 판정했다.** `readyPeerCount >= 3`으로 courier actor node
  준비를 판정하는데, java는 **지목한 peer가 READY인지** 확인한다. 원인은 kotlin이 courier node에
  자동 RID prefix(`delivery-courier-<uuid>`)를 써서 이름으로 지목할 수 없었던 것이다. java와 같이
  고정 RID(`courier-node-1`)로 바꾸고 판정도 이름 대조로 맞췄다.
- **kotlin — placement marker 뒤에 실패하는 단계가 있었다.** `runner-common.sh`의 cleanup이 graceful
  shutdown을 검증하는데 `ZLINK_SAMPLE_FRAMEWORK_ROLE_LOGS`가 비어 있어 실패했다. 이 검증은 지울 게
  아니라 동작하게 만드는 것이 맞아 role 목록을 채웠다. **§10.1의 "placement는 마지막 줄"이 지켜지지
  않으면 이런 실패가 marker 뒤에 숨는다.**

## 4.6 Bingo 회귀 — framework 수정이 러너를 깨뜨렸다

§4.2의 java 수정(`PEER_READY`를 전이에서만 기록) 뒤 **kotlin Bingo가 실패**했다. 그 러너가
`wait_framework_peer_ready_counts`로 **`ZLINK_FRAMEWORK_PEER_READY` 발생 횟수를 세고 있었고**
(api-a 3회, play-a 5회…), 그 개수는 **중복 재admit을 세던 것** — 즉 버그를 세고 있었다. 버그가
사라지자 개수가 모자라 timeout이 났다.

Bingo §10.1이 framework 문자열을 완료 판정 근거로 쓰는 것을 금지하므로 그 게이팅을 걷어냈다.
바로 아래에 샘플 소유 `bingo-ready` 행 8개가 이미 있어 커버리지 손실은 없다. java Bingo는 이
함수를 쓰지 않아 영향이 없었다.

**GameQuest(java·kotlin)와 SupportChat(java)에 같은 게이팅이 남아 있다** — 각 샘플 구현에서
함께 제거한다.

## 4.7 readiness가 아무것도 증명하지 않는 행 — 반복되는 패턴

세 샘플에서 같은 모양이 나왔다. **"endpoint가 열렸다"를 readiness로 쓰면 라우팅에 대해 아무것도
증명하지 못한다.** 그러면 route 수렴이 빠른 구현은 우연히 통과하고 느린 구현만 실패한다 — 그리고
그 차이를 고정 sleep으로 메우는 것이 정확히 §10이 금지하는 일이다.

| 샘플 | 부족했던 행 | 실제 실패 | 추가한 행 |
|---|---|---|---|
| TicTacToe | `kind=http`(Api) | java·kotlin의 첫 `JoinSpot` timeout | `kind=spot-route node=<NodeId> mesh=<SpotMeshName>` |
| GameQuest | `kind=stream`(Api) | cpp의 `JoinSessionReq`가 "Remote Actor creation transport did not complete" | `kind=spot-route node=<NodeId> mesh=<MeshName>` |
| SupportChat | `kind=public`·`kind=stream` | (예상) 첫 conversation 요청 | `kind=spot-route node=<NodeId> mesh=<MeshName>` — **실패 전에 선제 추가** |

ShoppingMall은 처음부터 `kind=object-route` 행이 있어 해당 없다. DeliveryDispatch도
`kind=actor-route`가 있다.

**교훈**: 새 샘플의 §10.1을 쓸 때 readiness 표의 각 행에 대해 "이 행이 참일 때 client의 첫 호출이
성공하는가"를 묻는다. endpoint listen만 증명하는 행은 그 질문에 답하지 못한다.

## 4.8 GameQuest에서 러너를 돌려 잡은 것

에이전트 다섯이 "§10.1 표에서 빠뜨린 것 없음"으로 보고했지만 러너를 돌리니 **내 스펙 오류 2건과
시나리오 설계 오류 3건**이 나왔다.

**내 스펙 오류**

- **배치 의존 요구를 넣었다.** "`event-routed`와 `processed`를 **각 node 로그에서 따로** 1건 이상"은
  만족할 수 없다. 실행 로그가 증명한다 — api-b가 stream으로 `CollectItemMsg`를 받았는데
  (03:32:51.048) 핸들러는 **actor가 사는 api-a에서 실행돼** 거기 `event-routed`가 찍혔다
  (03:32:51.082). `IZLinkEntrySpotActorSendHandler`는 stream이 도착한 node가 아니라 actor가 있는
  node에서 돈다. Bingo·TicTacToe에서 이미 쓴 **배치 독립성** 원칙과 충돌하는 요구였다.
  → "두 로그를 합쳐 4건 이상"으로 고치고, 원래 막으려던 `grep -q` OR 버그는 **"합계를 세고 하한과
  비교한다"**로 따로 명시했다.
- **TicTacToe에서 고친 것을 GameQuest에 옮기지 않았다** — §4.7의 `spot-route` 행.

**dotnet 시나리오 설계 오류 3건** (owner-loss 단계가 새로 들어오며 생겼다)

- §9-9 단계가 Alice quest 흐름 **중간**에 있어, owner를 죽인 뒤 quest 완료를 기다리다 죽었다.
  스펙 §9-9는 "다음 호출이 `Unavailable`"까지만 요구하고 **그 뒤 복구를 약속하지 않는다.**
  단계를 Alice 흐름의 **종착**으로 옮겼다.
- `Unavailable` 단언의 catch가 특정 error code에 묶여 **자기가 기대한 실패를 놓쳤다.** 연결기는
  `unavailable: Instance Spot '...' is currently unavailable`로 올린다. 의미(메시지)로 잡도록 바꿨다.
- reconcile 단계가 owner 상실 뒤에 남아 또 걸렸다 — 그것도 앞으로 옮겼다.

**node 시나리오 정렬 1건** — node가 player-alice reconcile을 2회 일으켰다(dotnet은 1회).
닫기 직후 sync 시점에 미발행 fact가 남아 있어서였다. 순서를 dotnet에 맞춰 의도적
`kill-without-publish` 하나만 reconcile을 일으키도록 했다. **러너 단언을 2로 완화하지 않았다** —
두 구현이 같은 시나리오를 돌아야 한다는 것이 요점이다.

## 4.9 ShoppingMall — 스펙이 요구한 검증이 실제로 작동한다

다섯 언어가 **모두 같은 지점에서 걸렸다**: §9.2-11 planned relocation.

```
expected planned-relocation replay exactly 1 time(s), found 0
```

이건 실패가 아니라 **설계대로 동작한 것**이다. §10.1에 "그 단계를 실행하지 않으면 이 행들은
통과할 수 없으며, 통과하지 못하는 것이 맞다"고 쓴 그대로다. 다섯 언어가 그동안
`RelocationStore`·`recreateOnRelocation`을 배선만 해 두고 relocation을 한 번도 일으키지 않았는데,
옛 러너는 그 사실을 물어본 적이 없어 아무도 몰랐다.

### 러너를 돌려 잡은 것 — 보고는 전부 "빠뜨린 것 없음"이었다

- **dotnet — 서버가 아예 뜨지 않았다.** 세 건이 겹쳐 있었다.
  1. `mesh.PeerConnections.Connect(...)`를 workflow·api 양쪽에 추가했는데, 그러면 peer 획득이
     manual로 바뀌어 그 mesh가 쓰는 RID prefix와 충돌한다
     (`can use a routing ID prefix only with automatic discovery`). ShoppingMall은 Location Store로
     peer를 찾으므로 **manual 연결이 애초에 불필요하다.** 걷어내고 이유를 주석으로 남겼다.
  2. `ApiInstanceTopology`를 만들어 놓고 **DI에 등록하지 않아** readiness reporter가 활성화되지
     못했다.
  3. 러너가 각 workflow에 **자기 endpoint만** 넘겨 peer endpoint가 빈 문자열이었다.
- **node — 컴파일조차 되지 않았다.** `npm run build`가 TS2499·TS2345 3건으로 실패한다. generation
  값이 bigint인데 number로 넘긴 것이 원인이다. **`tsc -b`는 통과하므로 드러나지 않았다** —
  샘플 빌드 스크립트가 별개다.
- **cpp — 나머지는 전부 통과**하고 relocation 행 하나만 남았다.

## 4.10 ShoppingMall — relocation 시나리오가 실제로 구현됐다

node와 cpp가 `shoppingmall-placement=completed`로 통과했다. **다섯 언어가 그동안 배선만 해 두고
한 번도 일으키지 않던 §9.2-11 planned relocation이 처음으로 실제 실행·검증됐다는 뜻이다.**

### 러너를 돌려 잡은 것 (계속)

- **kotlin — 러너 전체가 셸 버그 하나로 무력화돼 있었다.**
  `count() { rg -c -- "$1" "$2" 2>/dev/null || true; }`
  `rg -c`는 매치가 0이면 **아무것도 출력하지 않고 종료 코드 1**을 낸다. `|| true`가 오류만 삼키고
  빈 문자열은 그대로 남아 모든 `(( ... >= 1 ))`이 `syntax error: operand expected`로 깨졌다.
  매치가 없으면 `0`을 내도록 고쳤다. 고치자 다음 실패 지점(relocation 소스 workflow를 못 찾음)이
  비로소 드러났다.
- **dotnet — relocation 단계에 도달하기도 전에** projection 복구 fixture가 500으로 죽는다
  (`Order projection 'order-repair-001' does not exist`).

**패턴**: 러너 자체의 버그가 그 아래 있는 진짜 실패를 가리고 있었다. kotlin은 문법 오류를
고쳐야 relocation 문제가 보였고, node는 컴파일을 고쳐야 그다음이 보였다. **"실패가 하나"로
보이는 것이 실제로는 층층이 쌓여 있다.**

## 4.11 회귀 확인이 잡은 것 — "통과했다"는 유통기한이 있다

트리가 비는 시간마다 이미 통과한 샘플을 다시 돌렸다. **세 번 모두 무언가 깨져 있었다.**

| 발견 | 원인 |
|---|---|
| node DeliveryDispatch | 러너가 `bind-relayed courier=courier-a`를 **courier-node-1에 고정**해 기다렸다. actor handler는 actor가 사는 node에서 도므로 배치 의존이다. 이전엔 운으로 통과했다 |
| cpp Bingo·TicTacToe | `test_cpp_framework_sample_parity`가 **스펙이 금지한 것들을 여전히 요구**했다 — `/ready` 합성 probe 핸들러, `wait_route_ready`, 옛 노드 이름. 테스트를 새 계약에 맞추되 **금지를 검증하도록 뒤집었다**(`route_ready_http_handler_t`가 없어야 통과) |
| java TicTacToe | `actor-bound actor=player-x`가 **3회**(스펙 1회) — "reconnect한 client가 existing Actor에 binding됐다"를 뜻하는 행인데 모든 bind마다 찍고 있었다. `leave-completed`·`actor-destroy-complete`가 player-o에 없고, observer id가 아직 `player-observer`라 **`actor=observer` 0회 검사가 무의미**했다 |

**교훈**: 샘플이 한 번 통과했다고 끝이 아니다. 다른 언어의 수정, framework 수정, 스펙 갱신이
모두 남의 샘플을 깨뜨릴 수 있다. 특히 **배치 의존 단언은 운으로 통과하다가 어느 날 실패한다.**
마지막에 7샘플 × 5언어를 한 번에 돌리는 것으로는 부족하고, 진행 중에도 계속 돌려야 한다.

## 5. 스펙 교차 검증에서 잡은 내 오류

codex sol에 스펙 3건을 실제 구현과 대조시켜 **내가 쓴 스펙의 오류를 여러 개 잡았다.**

- **DeliveryDispatch node 이름을 틀리게 적었다.** 샘플은 6개 프로세스를 띄우는데 `tracking`을 빼고
  5개로 적었다. courier node 이름은 언어마다 네 가지로 갈려 있는데 근거 없이 하나를 골랐다.
- **DeliveryDispatch 횟수가 틀렸다.** `customer bound`를 1회로 적었으나 한 connector가 두 delivery를
  각각 구독한다. `pushed Delivered`는 "1회 이상"으로 뒀는데 실제로는 **정확히 2회**이고, 느슨하게
  두면 **한 흐름이 통째로 사라져도 통과**한다.
- **DeliveryDispatch §9-7이 성립하지 않는 방식이었다.** "후보가 없는 delivery를 만든다"고 썼으나
  `CreateDeliveryReq`에 후보 field가 없다. 기존 `CourierDecisionMsg`로 A·B를 차례로 거절해
  소진시키는 방식으로 바꿨다.
- **TicTacToe 0회 검사가 헛돌고 있었다.** observer actor id가 java·kotlin만 `player-observer`라
  `actor=observer` 0회 검사가 **실제 observer가 destroy돼도 통과**했다.
- **GameQuest에서 출력 주체를 안 적었다.** `unavailable` 행을 server evidence에 넣어 놓고 어느 node가
  찍는지 안 적었는데, **죽은 Mission node는 찍을 수 없다.**
- **GameQuest reconcile 횟수가 틀렸고**(node는 두 번 한다) **중복 행이 있었다**(`spot-ready`는
  `processed`가 이미 증명한다).

## 6. 아직 열려 있는 것

### 6.1 스펙 요구인데 다섯 언어 모두 실행하지 않는 시나리오

구현 단계에서 **실제로 그 상황을 만들어야** 통과한다. §10.1에 "실행하지 않으면 통과할 수 없으며,
통과하지 못하는 것이 맞다"로 못박아 두었다.

| 샘플 | 시나리오 |
|---|---|
| DeliveryDispatch §9-6 | 늦게 도착한 `CourierDecisionMsg`가 효과 없음 — **node만** 실제로 시험한다 |
| DeliveryDispatch §9-7 | 후보 소진 뒤 `Failed` 한 번 — 아무도 안 한다 |
| GameQuest §9-9 | Ready owner 종료 → 다음 호출 `Unavailable`, 자동 replacement 없음 — 아무도 안 한다 (§11 완료 기준) |
| ShoppingMall §9.2-11 | 계획된 relocation — 다섯 언어 모두 store만 배선하고 실행하지 않는다 |

### 6.2 ShoppingMall — 구현 4개가 스펙 §9를 어긴다

§9는 runner 전용 hook을 Client 프로세스 **밖에서** 호출하라고 명시하는데
cpp·node·java·kotlin이 Client 안에서 호출한다. **kotlin은 Client가 HTTP를 아예 쓰지 않고**
내부 mesh API로 직접 말해서, §9가 시험하려는 public order API 표면이 전혀 시험되지 않는다.

### 6.3 보류 — 판단 대기

- **ShoppingMall `fast_mutex.hpp:76` abort.** 일괄 실행에서만 재현. 조사 두 번 모두 codex 안전
  필터에 막혔다(메모리 수명 분석을 사이버보안 위험으로 분류). 재개 시 Claude 서브에이전트를 쓴다.
  **retain 누락은 취약점이지 확정 원인이 아니다** — `fast_mutex.hpp:76`은 모든 `scoped_fast_lock_t`
  소멸자가 지나가는 inline unlock이라 스택 없이는 어느 mutex인지도 모른다.
- **재claim 충돌 검증 부재.** dotnet만 `RoutingIdConflict`를 검사한다. cpp·java·kotlin·node에는
  같은 검증이 없다.

### 6.4 `.ps1` 누락

DeliveryDispatch(cpp·java·kotlin), GameQuest(cpp·java), ShoppingMall(cpp), SupportChat(cpp·java).
§10.1이 "다섯 언어 모두 `.sh`와 `.ps1`을 함께 제공한다"를 요구한다.

## 7. 작업 규칙

이 캠페인을 진행하는 동안 지키는 규칙이다. 어기면 나중에 되돌아와야 하므로 여기 적어 둔다.

### 7.1 Framework 버그는 우회하지 않고 고친다

**샘플을 증상에 맞춰 바꾸지 않는다.** 통일 작업의 목적이 언어별로 덮여 있던 것을 걷어내는
것이므로, 걷어냈을 때 드러나는 framework 버그는 그 자리에서 고친다.

우회의 예 — 전부 금지한다.

- 고정 sleep을 되살려 타이밍 문제를 덮는다
- readiness 검사를 느슨하게 해서 실패를 없앤다
- 스펙이 요구하는 검증을 "이 언어에서는 안 된다"며 내린다
- 샘플이 framework의 잘못된 동작을 견디도록 코드를 추가한다

§4의 두 건이 이 규칙으로 닫혔다. 둘 다 처음에는 타이밍 문제로 보였고, 우회했다면 그 언어만
영구히 다른 상태로 굳었을 것이다.

**고칠 때는 회귀 범위를 확인한다.** framework를 건드렸으면 그 언어 단위 테스트를 전부 돌리고
결과 수치를 남긴다(.NET 1,879 / JVM core 1,129). 원인을 못 찾으면 지어내지 말고
"확인하지 못했다"로 기록하고 보류한다.

### 7.2 스펙 수정 범위

| 대상 | 수정 |
|---|---|
| **샘플 시나리오 스펙** (`common/sample/**`) | **가능** — 오류 수정과 상세화 모두 |
| **server 스펙** (`common/spec/server/**`) | **금지** |

샘플 스펙은 이 캠페인이 소유한다. 갈라진 부분을 닫기 위해 §10.1을 신설하고, 갈라짐을 허가하던
조항을 삭제하고, 만족 불가능한 요구를 내리는 것은 모두 이 범위 안이다.

server 스펙은 이 캠페인이 소유하지 않는다. 구현이 server 스펙과 어긋나면 **스펙을 고치지 말고
구현을 고치거나, 스펙 쪽 판단이 필요하면 기록하고 사용자 판단을 받는다.**

> **기록된 예외 (2026-08-26)**: 고정 RID 제약 완화는 server 스펙
> (`languages/{dotnet,java,kotlin}/interfaces/*`, ko/en)을 수정했다. **사용자의 명시적 지시로
> 진행한 건이다** — "구현 테스트 시나리오상 필요하면 고정 rid 를 사용해야지",
> "필요하면 스펙에 명시하고 동일하게 사용하면 되". 이 규칙 이전의 결정이며, 이후로는 위 표를
> 따른다.

### 7.3 서브 에이전트 정책

| 역할 | 담당 |
|---|---|
| 조사 | `codex exec -m gpt-5.6-sol` |
| 작업(구현) | `codex exec -m gpt-5.6-terra` |
| 스펙 작성·판정·검토 | **Claude 본체가 직접 한다. 위임하지 않는다.** |

공통 옵션은 `-c model_reasoning_effort="high" -s danger-full-access --skip-git-repo-check`다.
**`-s danger-full-access`가 필수다** — 기본 sandbox는 loopback bind와 Docker를 EPERM으로 막아
샘플을 돌리지 못한다(§8).

- **구현은 언어별로 병렬**, **러너 실행은 직렬**. 다섯이 동시에 샘플을 돌리면 실패 귀속이 안 된다.
- 구현 에이전트에게는 **빌드까지만** 시키고 러너는 검토자가 돌린다.
- **에이전트 보고를 판정 근거로 삼지 않는다.** diff를 직접 읽고 러너를 직접 돌린다.
- **두 번째 조사자에게는 "확인"이 아니라 "반증"을 시킨다.** 1차 가설의 주장만 검증하면
  확증 편향에 빠진다(§8).
- codex 안전 필터가 막는 주제(메모리 수명·크래시 분석)는 Claude 서브에이전트로 우회한다.

## 8. 다음에 할 일

1. **DeliveryDispatch 구현** — 언어별 codex 5개 병렬(코드·빌드까지), 러너는 순차로 직접 검증.
2. GameQuest → ShoppingMall → SupportChat 순으로 같은 방식.
3. §6.1의 미실행 시나리오를 각 샘플 구현에서 함께 만든다.
4. 전 샘플 완료 후 전체 게이트 재실행.
5. **마지막 작업 — `fast_mutex.hpp:76` abort 원인 파악과 수정.**
   **착수 시점: 샘플 6개가 모두 끝난 뒤.** core 재빌드와 반복 재현은 기계를 무겁게 쓰는데, 이
   버그는 부하 민감이라 다른 빌드가 도는 중에 측정하면 전후 비교가 무의미해진다. 샘플 러너
   검증도 같은 이유로 오염된다.
   - 증상: dotnet 샘플 7개 일괄 실행 중 ShoppingMall이 `Aborted (core dumped)`,
     마지막 줄이 `Invalid argument (core/src/runtime/utils/fast_mutex.hpp:76)`.
     단독 실행에서는 15초에 통과한다. 부하가 트리거다.
   - 확인된 것: 그 줄은 `pthread_mutex_unlock` 뒤의 `posix_assert`다. `fast_mutex_t`는
     `PTHREAD_MUTEX_RECURSIVE`라 비소유 unlock은 `EPERM`이고, `EINVAL`은 그 저장소가 더 이상
     유효한 mutex가 아님을 뜻한다 — 소유 객체가 이미 파괴됐다는 신호다.
   - **아직 모르는 것: 어느 mutex 인스턴스였는지.** `fast_mutex.hpp:76`은 모든
     `scoped_fast_lock_t` 소멸자가 지나가는 inline unlock이라 **위치만으로는 객체를 특정할 수
     없다.** 1차 가설(`stream.cpp:835`의 packet dispatch가 pipe 소유 mutex를 잡은 채 핸들러를
     호출)은 2차 조사가 반증했다 — dispatch가 쓰는 pipe는 session쪽 `pipes[0]`이고
     `xpipe_terminated`가 건드리는 것은 socket쪽 `pipes[1]`이며, `send_pipe_term`은
     `send_pipe_command(..., false)`로 self-dispatch를 끄고 보내 항상 큐잉된다.
   - 남은 취약점(원인 아님): dispatch 경로가 `pipe_t::retain_lifetime_ref()`를 쓰지 않는다.
     `stream.cpp` 내 사용 0회, 다른 5개 파일은 모두 이 관용구를 쓴다. 실행자 소유 규칙이 지금은
     막아 주지만 그 규칙이 바뀌면 위험하다.
   - **접근**: 소스 읽기는 이미 벽에 부딪혔다. **재현이 먼저다.** core dump가 실제로 잡히도록
     `ulimit -c`와 `core_pattern`을 확인·설정하고, 스택에서 mutex 인스턴스를 특정한다.
     ASAN/TSAN 빌드가 가능하면 그쪽이 훨씬 강한 증거다. yama ptrace 제약 때문에 plain gdb는
     막히므로 `sudo -n gdb -p <pid>`를 쓰고, ELF pid는 `readlink /proc/<pid>/exe`로 고른다.
     수정 뒤에는 **수정 전 N회 중 몇 회 실패했는지 먼저 세고, 수정 후 같은 횟수에서 재현되지
     않음을 숫자로 보인다.**
   - **codex 안전 필터가 이 주제를 두 번 거부했다**(메모리 수명 분석을 사이버보안 위험으로 분류).
     Claude 서브에이전트로 진행한다.
   - 재현하지 못하면 메커니즘을 지어내지 말고 "N회 시도했으나 재현 실패, 가장 강한 단서는 이것"으로
     기록한다.

   **준비 조사 결과 (2026-08-26) — 재현 전에 반드시 해결할 것**

   1. **심볼이 없으면 재현해도 소용없다.** 배포되는 `libzlink.so`에 **debug section이 하나도 없다**
      (`readelf -S | grep -i debug` 무출력). `core/build/CMakeCache.txt`가 `CMAKE_BUILD_TYPE=Release`이고
      `scripts/local-package/core/build-wsl.sh`도 `CONFIGURATION=Release`가 기본이다. 이대로 core를
      떠도 **zlink frame이 전부 `??`로 나온다** — "어느 mutex인가"에 답할 수 없다.
      **빌드 스크립트는 이미 `CONFIGURATION` 환경변수를 받는다**(`build-wsl.sh:9`, 기본값 `Release`)
      — 스크립트 수정 없이 넘기면 된다. ASAN을 켜려면 `-DENABLE_ASAN=ON` 전달만 추가하면 된다.
      **(2026-08-26 해소)** `CONFIGURATION=RelWithDebInfo bash scripts/local-package/core/build-wsl.sh`로
      재빌드했다. `core/build/lib/libzlink.so.0.13.2`와 생성된 core 패키지 양쪽에 debug section
      8개(`.debug_info`·`.debug_line`·`.debug_str` 등)가 들어간 것을 `readelf -S`로 확인했다
      (이전 0건). 이제 core dump에서 zlink frame이 심볼로 풀린다.
      → 먼저 `CONFIGURATION=RelWithDebInfo`로 core를 다시 빌드하고
      `sync-local-core-libs.sh dotnet` → `dotnet/build-wsl.sh --core-prefix ...`로 반영한다.
      (`run_samples.sh`는 nupkg sha256으로 `NUGET_PACKAGES` 캐시를 키잉하므로 캐시 무효화는 자동이다.)
   2. **TSAN은 길이 아니다.** `core/CMakeLists.txt`가 직접 "libzlink를 TSAN으로 돌리는 건 의미가 크지
      않고 false positive가 많다"고 적고 `-tsan-instrument-memory-accesses=0`으로 race 탐지를 꺼 둔다.
      게다가 `-mllvm`은 clang 전용인데 이 빌드는 GCC 13.3이다. **ASAN이 실제 후보다**
      (`-DENABLE_ASAN=ON`, `RelWithDebInfo`를 자동으로 강제한다).
      단 `libzlink.so`는 계측되지 않은 `dotnet` host가 뒤늦게 `dlopen`하므로
      `LD_PRELOAD=$(gcc -print-file-name=libasan.so)`와
      `ASAN_OPTIONS=detect_leaks=0:abort_on_error=1`이 필요할 수 있다 — **미검증, 시도해 봐야 한다.**
   3. **core 저장 위치를 옮겨야 한다.** `/tmp`는 8 GB tmpfs에 여유 4.5 GB인데 샘플 `RUN_DIR`도 거기
      있다. CoreCLR full core는 수백 MB~GB급이라 **잘린 dump가 파일로는 남아 헛된 확신을 준다.**
      `/home`(649 GB 여유)으로 보낸다.
   4. **`core_pattern`은 원복해 두었다.** 앞선 세션이 `/tmp/zlink-fast-mutex-crash/...`로 바꿔 놓고
      복구하지 않았다. 원본 `|/wsl-capture-crash %t %E %p %s`로 되돌렸다
      (`/tmp/zlink-fast-mutex-crash/original_core_pattern.txt`에 사본 보관). 재현 시에만 다시 바꾸고
      끝나면 복구한다. **WSL 재시작이나 다른 세션이 되돌릴 수 있으므로 실행 직전에 값을 확인한다.**
   5. **부하의 정체가 달랐다.** `run_samples.sh`는 7개 샘플을 **순차로** 돌린다 — 원래 크래시는
      batch 내부 동시성이 아니라 **주변 부하**(남은 redis 컨테이너, 이전 프로세스)에서 왔다.
      재현을 무겁게 하려면 **ShoppingMall 러너를 N개 동시 실행**한다. 포트는 bind 확인으로 무작위
      할당되고 `RUN_ID`에 `$$-$RANDOM`이 들어가며 redis 컨테이너 이름도 run 단위라 안전하다.
   6. **PID→role 매핑을 직접 남겨야 한다.** 다섯 프로세스가 전부 `dotnet <assembly>`로 뜨므로
      core 파일명의 `%e`가 전부 `dotnet`이다. `%p`만 구분자이고 gdb의 argv 표시는 80자에서 잘린다.
      `start_server` 호출부에서 `name pid=$!`를 따로 기록한다.
   7. 도구 상태: `gdb` 15.0.50 설치됨, `sudo -n` 무암호 동작, `ptrace_scope=1`(사후 core 열기에는
      무관), `dotnet-dump` 9.0.66 설치됨, `lldb` 없음. 관리 frame까지 보려면 CoreCLR의 `createdump`
      경로를 쓴다.
6. `spec/` 트리 재잠금.

## 8.1 언어별 체크리스트

각 샘플 §10.1을 구현할 때 언어마다 확인하는 항목이다. 지금까지 다섯 샘플에서 실제로 걸린 것만
남겼다 — 추상적인 항목은 넣지 않았다.

| # | 항목 | 어디서 걸렸나 |
|---|---|---|
| 1 | readiness 행이 **라우팅을 증명**하는가. endpoint listen만 증명하는 행은 느린 언어에서만 실패한다 | TicTacToe(java·kotlin), GameQuest(cpp) |
| 2 | readiness를 **합성 요청**으로 증명하지 않는가 | Bingo(cpp), DeliveryDispatch(cpp) |
| 3 | **고정 sleep**이 readiness를 대신하지 않는가 | TicTacToe(java·kotlin, 30초) |
| 4 | 완료 판정이 **framework 문자열**에 걸려 있지 않은가 | Bingo(kotlin), GameQuest(java·kotlin), SupportChat(java) |
| 5 | marker를 **그 사실이 일어난 자리**에서 찍는가. 끝에서 몰아 찍으면 검사가 "종료 코드 0"의 반복이 된다 | SupportChat(cpp, 8개) |
| 6 | 러너가 client marker를 **직접 확인**하는가. 종료 코드·browser 판정으로 대신하지 않는가 | Bingo(node), DeliveryDispatch(node), ShoppingMall(kotlin·node) |
| 7 | 횟수가 **배치 독립적**인가. 어느 node가 처리하는지에 의존하면 만족할 수 없다 | GameQuest(내 스펙 오류) |
| 8 | `grep -q`에 파일을 둘 이상 넘기지 않는가(둘 중 하나만 맞아도 통과) | GameQuest(dotnet) |
| 9 | `.sh`와 `.ps1`의 **대기 예산이 같은가**, 그리고 `.ps1`이 있는가 | GameQuest(dotnet 20배 차이), DeliveryDispatch·ShoppingMall·SupportChat(`.ps1` 없음) |
| 10 | placement marker **뒤에 실행되는 것이 없는가** | DeliveryDispatch(kotlin, cleanup 검증이 뒤에서 실패) |
| 11 | 스펙이 요구하는 시나리오를 **실제로 실행**하는가 | DeliveryDispatch §9-6·§9-7, GameQuest §9-9, ShoppingMall §9.2-11 |
| 12 | runner 전용 hook을 **Client 밖에서** 부르는가 | ShoppingMall(cpp·node·java·kotlin) |

### 8.1.1 cpp ZoneWorld — kill 뒤 zone 재claim 실패 (2026-08-26 해소)

**증상.** `run_sample.sh`가 ZW-D1까지 통과한 뒤 transition node 재기동에서
`Zone Spot capacity did not settle. node=zone-node-1 zones=`로 멈춘다. 3회 연속 재현.
남은 ZW-E5·G2·G3·G5는 실행조차 되지 않았다.

**직전 세션의 가설은 틀렸다.** "`zone_bootstrap_service_t::start`가 `bootstrap()`을 await해서
startup이 zone claim과 교착한다"로 기록돼 있었으나, cold start는 정상이다 —
`zone-node-1.log:5`에 `topology=ready node=zone-node-1 zones=zone-se,zone-sw`가 찍히고
시뮬레이션이 tick 700까지 돈다. 실패는 **재기동 경로에만** 있다.

**판별 방법.** claim 예외를 통째로 삼키는 `main.cpp`의 catch 때문에 "submit이 던지는가"와
"submit은 성공하는데 owner가 안 되는가"를 구분할 수 없었다. 임시 계측을 넣어 한 번 돌렸다
(add-then-delete, 확인 후 제거함).

결과가 결정적이었다. 재기동한 node는 매 시도마다 zone 4개를 **예외 없이** submit하는데
`zoneworld-zone-ready`는 한 번도 찍히지 않는다. 즉 local Spot으로 붙기만 하고 owner가 되지 않는다.

**원인은 framework 버그가 아니라 cpp runner의 재기동 config 선택이다.** 스펙 §2.2가
"Ready owner 장애는 자동 replacement가 아니다"라고 정하므로, KILL된 owner는 zone을
돌려받지 않는 것이 맞다. 다른 언어가 이미 그렇게 한다 — java·kotlin은 **모든** node 재기동을
`zone-node-crash-replacement` config로 하고(`run_sample.sh:253`), cpp도 ZW-G4에서만은
`$g4_node-replacement`를 쓰고 있었다. transition·E5·G3 세 곳만 base config로 재기동해서
zone 2개를 요구했고, 그래서 영원히 settle하지 못했다.

**수정.** 세 곳을 `-replacement` config 재기동으로 맞췄다(`run_sample.sh:442`·`455`·`472`).
스펙 수정 없음, framework 수정 없음.

**남은 flake.** 수정 뒤 ZW-E5·G2·G3·G4·G5가 전부 PASS로 바뀌었다. 2회 중 1회에서 별개 증상으로
transition client가 `stream connector wait timed out`(stream-error-code=12)에 걸려 arm하지
못했다 — 부하 민감한 접속 flake이며 위 원인과 무관하다. 별도로 계측 중이다.

### 8.1.2 수정 뒤 spec gap 리뷰 (2026-08-26)

사용자 지시로 규칙을 추가했다 — **버그를 고치면 러너 통과 확인보다 먼저 그 수정이 스펙에 없는
동작을 만들지 않았는지 리뷰한다.** 수정마다 셋 중 하나로 분류한다: (1) 스펙이 이미 정함,
(2) 샘플 시나리오 스펙이 정할 일 → 상세화, (3) server spec이 정해야 하는데 비어 있음 → **gap 기록,
임의 수정 금지**.

오늘 수정분 분류.

| 수정 | 분류 | 처리 |
|---|---|---|
| cpp ZoneWorld 재기동 config | (2) | 아래 §8.1.3으로 샘플 스펙 상세화 완료(ko/en) |
| dotnet relocation target `OnInitializeAsync` 호출 | **(3) gap** | 아래 |
| dotnet native prepare reply를 captured `ReplyOperation`으로 제출 | (1) | `ZLinkManagedMeshNode.cs`가 이미 문서화한 관용구("The ReplyOperation captured from the request is the exact Core reply route") |

#### GAP-ZW-1 해소 (2026-08-26) — server spec 상세화 승인받아 반영

사용자 승인으로 server spec을 열어 `03-spot-actor/01-spot-model.{ko,en}.md` §5.2에 조항을
추가했다.

> Relocation target에서 User Spot instance를 다시 만들 때는 factory가 instance를 만든 뒤
> `Configure`와 `OnInitializeAsync`를 실행한다. Creation request가 없으므로 `OnCreateAsync`는
> 실행하지 않는다. `OnInitializeAsync`는 target admission seal이 외부 ingress를 막고 있는 동안
> 실행하며, 이 callback이 끝나야 target이 Ready가 된다.

**다섯 언어 대조 결과.**

| 언어 | 상태 |
|---|---|
| dotnet | 어기고 있었음 → 고침(`ZLinkFrameworkRuntimeSpotRetire.cs`·`ZLinkSpotActivationExecution.cs`) |
| cpp | **이미 적합** — `spot_runtime.cpp:9377`의 `if (staged_restore)` 분기가 relocation 경로에서 `on_create`를 건너뛰고, 뒤이어 `:9418`의 `on_initialize`는 실행한다 |
| java | 어기고 있었음 → 고침(`ZLinkSpotActivationFactory.activateRelocation`/`initializeRelocation` 신설, `prepareRelocationReserved`로 분리) |
| kotlin | codex 보고상 통과 — `:zlink-framework-kotlin:test` 그린 |
| node | codex 보고상 이미 적합 |

회귀 검증(직접 실행): dotnet 단위 **1879/1879**, JVM `:zlink-framework-core:test` **BUILD SUCCESSFUL**.
첫 JVM 실행에서 `ZLinkAsyncSerialQueueTest.queuedRelocationIntentCannotRacePastYieldRegistration`가
1건 실패했으나 단독 2/2 통과, 전체 재실행 그린 — full-run 한정 동시성 flake로 판정하고 알려진
flake 목록에 추가했다.

#### GAP-ZW-1 — relocation target의 `OnInitializeAsync` 호출 여부가 server spec에 없다

`03-spot-actor/01-spot-model.ko.md` §5.2는 **새로 만드는** User Spot의
`Configure → OnCreateAsync → OnInitializeAsync → Ready` 순서만 정한다. relocation target으로
**재생성**되는 경우 이 callback이 도는지는 어디에도 없다.

dotnet 수정은 `invokeCreate:false`로 만들어진 target에서 `OnInitializeAsync`가 아예 실행되지
않던 것을 실행하도록 바꿨다(ShoppingMall의 order replay가 그 안에 있어 relocation이 완료되지
않았다). §5.2와 callback 표("생성된 Spot instance의 application 초기화를 완료한다")를 읽으면
호출하는 쪽이 맞지만 **명시 조항이 없어 지금은 구현이 사실상 스펙이다.**

캠페인 규칙 §7.2상 server spec은 수정하지 않는다. 스펙 소유자 판정 대기 항목으로 남긴다.
다른 세 언어가 이 경우 어떻게 동작하는지도 아직 확인하지 않았다.

### 8.1.3 ZoneWorld 정지·재기동 고정값을 샘플 스펙에 명시했다

ZW-B4·C2·C3·E5·G3·G4가 전부 ZoneNode를 멈추는데 **어떤 신호로 멈추는지와 다시 띄울 때 zone을
갖는지가 스펙에 없었다.** 그래서 네 구현이 갈렸다 — java/kotlin은 B4=KILL·C2=TERM·C3=KILL에
모든 재기동을 crash-replacement config로, dotnet은 B4·C3·E5=crash·C2=graceful인데 E5만 base
config로, cpp는 B4+C2+C3을 **한 번의 KILL로 묶고** base config로 재기동했다.

`sample/zoneworld/README.{ko,en}.md`에 표로 고정했다: B4·C3·E5·G4는 급정지, C2·G3은 정상 종료,
**재기동은 방식과 무관하게 zone 0개 replacement 구성**(§2.2 자동 replacement 부재).

**이 상세화가 cpp를 비적합으로 만들었고, 고쳤다.** cpp는 B4+C2+C3을 한 번의 KILL로 묶어서 C2를
정상 종료로 시험할 수 없었다. C2만 별도 graceful lane으로 분리했다(B4·C3은 둘 다 급정지이므로
기존 묶음 유지).

- Client: `run_c2` 추가 — ops만 쓰고 `scenario ZW-C2 armed node=<id>`를 찍은 뒤 disconnect를 기다린다.
- Runner: G2 뒤에 `client-c2` lane을 두고 `stop_role zone-node-2 TERM` → replacement config 재기동.
- transition lane의 marker를 `ZW-B4-C2-C3` → `ZW-B4-C3`으로 바꾸고 C2 단언을 뺐다.

**첫 시도가 실패해서 원인을 찾았다 — 내 lane의 버그였다.** `node_status_notify_t`는 watch 중인
client에만 간다(ZW-C1). `watch_nodes_req_t` 구독 없이 wait만 걸면 영원히 기다린다. 구독을 넣고
`scenario ZW-C2 passed`를 확인했다.

### 8.1.4 dotnet ZoneWorld — mesh admission이 완료되지 않는다 (HEAD 기존 결함)

dotnet 7샘플 일괄에서 ZoneWorld만 실패한다. 나머지 6개는 placement marker 전부 통과했다
(따라서 §8.1.2의 `OnInitializeAsync` 트리 전역 변경이 다른 샘플을 깨지 않았다는 것이 확인됐다).

증상은 ops와 zone node가 `command=Hello`를 무한 반복하고 admission이 끝나지 않는 것이다.

```
!! neither side logged a completed mesh admission for zn-f33bb3db... and zn-0621809d...
!! ops never logged 'node status observed. node=zone-node-1, rid=zn-'
```

**이번 수정의 회귀가 아니다.** `ZLinkManagedMeshNode.cs`만 HEAD로 되돌려 다시 돌렸을 때
같은 admission 실패가 그대로 났다(파일은 원복함). HEAD에 이미 있던 결함이다. 단독 실행에서도
재현되므로 간헐이 아니다.

캠페인 초반에 java에서 고친 **pairless single-route admission**과 같은 모양이다. dotnet에 같은
계열 버그가 남아 있는 것으로 본다.

**시도했으나 해결되지 않았다.** codex terra에 java 수정을 단서로 주고 조사시켰고,
`ZLinkMeshPeerAdmission.cs`와 `ZLinkManagedMeshNode.cs`를 고치고 단위 test를 추가했다.
codex 스스로 "3회 연속 ZoneWorld 성공은 아직 관측하지 못했다"고 보고했고, **직접 3회 돌려
0/3으로 확인했다.** 단위 회귀는 없다.

**미해결.** 다음 세션은 `mesh_peer_admission_sent ... command=Hello`가 반복되는 쪽에서
상대가 ADMIT을 왜 보내지 않는지(또는 보낸 ADMIT이 왜 소비되지 않는지)부터 본다. java 수정의
두 축이 (1) pairless single-route admission이 기존 logical connection ID를 재사용하도록,
(2) PEER_READY를 not-ready→ready 전이에서만 찍도록 한 것이었으므로 dotnet의 대응 지점을
같은 각도로 대조한다.

## 8.2 게이트 실행 결과 (2026-08-26)

| 게이트 | 결과 |
|---|---|
| node 7샘플 일괄 (`samples/run_samples.sh`) | **통과** — 6개 placement marker + ZoneWorld |
| JVM `:zlink-framework-core:test` | **BUILD SUCCESSFUL** |
| node 계약 테스트 | **통과** — 실패 0건 |
| `check_doc_links.py framework` | 통과 |
| `check_doc_tabs.py framework` | 통과 |
| `check_guide_identifiers.py framework` | 통과 |
| `check_prose_neutrality.py framework` | 통과 |
| `generate_language_guides.py --check` | 통과 |
| `git diff --check` | 무결 |
| `mkdocs build --strict` | 경고 237건 — **아래 판정 참조** |

### 8.2.1 재확인 (2026-08-26 오전, 샘플 통일 이후)

| 게이트 | 결과 |
|---|---|
| node 7샘플 일괄 (`npx tsc -b --force` 후) | **통과** — 7/7, ZoneWorld 포함 |
| cpp 7샘플 일괄 | 6/7 — 6개 placement marker 통과, ZoneWorld만 실패(§8.1.5) |
| dotnet 7샘플 일괄 | 6/7 — 6개 placement marker 통과, ZoneWorld만 실패(§8.1.4) |
| dotnet 단위 (`Zlink.Framework.UnitTests`) | **1879 passed / 0 failed** |
| `check_doc_links.py framework` | 통과 |
| `check_doc_tabs.py framework` | 통과 |
| `check_guide_identifiers.py framework` | 통과 |
| `check_prose_neutrality.py framework` | 통과 |
| `generate_language_guides.py --check` | 통과 |
| `mkdocs build --strict` (`doc/site`에서) | 경고 235건 — 캠페인이 만든 것 0건(직전 237건에서 감소). 아래 판정 그대로 |

**site 잔여 항목은 닫혀 있다.** redirect 표는 캠페인 README §8에 **불필요**로 판정돼 있고
(옛 문서를 `archive/`에 남겼으며 `mkdocs-redirects` 미설치), 최상위 spec/server README 축소와
`languages/` 트리 어휘·형식 정리도 완료 기록이 있다.

**`spec/` 트리 재잠금은 의도적으로 하지 않았다.** 캠페인 README §8이 "사용자 리뷰가 열려 있는
동안 잠그면 그 리뷰 반영을 막는다"로 보류를 정해 뒀다. 리뷰가 닫히면
`chmod -R a-w framework/doc/framework/common/spec`.

### 8.1.5 cpp ZoneWorld — `stream-error-code=12` 세션 폐기 (미해결)

단독 실행은 통과하는데 7샘플 일괄과 일부 단독 실행에서 client lane이
`unhandled connector coroutine exception: stream connector wait timed out`으로 죽는다.
lane이 고정돼 있지 않다 — transition·main·C2 모두에서 관측했다.

gateway 쪽 증거가 핵심이다.

```
packet=bound_session_push actor=player-transition-source reason=target_closed
```

node 로그에는 `zoneworld-join-accepted` → `zoneworld-join-response`가 정상으로 찍힌다. 즉
**서버는 join을 승인했는데 그 응답 push가 세션 닫힘으로 폐기되고**, client는 응답을 못 받아
대기 만료로 죽는다.

**부하 문제가 아니다.** 20코어 머신에서 load average 2.4일 때 재현했다. 다만 같은 실행에서
zone tick이 스펙 100ms 대비 **약 156ms**로 돌고 있다(node-2 생존 125초 동안 801 tick). 이 초과가
초 단위 대기 만료를 설명하지는 못하므로 별개 관측으로만 기록한다.

`message_flow_log_mode_t::detailed`를 켜고 **7샘플 일괄**로 돌려 시퀀스를 잡았다. (계측은 제거함.
`app_t::set_message_flow_mode`는 `apply()` 전에 부르면 설정값에 덮이므로
`options.configure_dispatch ().message_flow (...)`를 써야 한다 — 코드 주석에 명시돼 있다.
단독 실행 3회로는 재현되지 않는다. **일괄이 재현 경로다.**)

#### 잡아낸 시퀀스 — binding은 끝까지 정합이고 물리 write만 실패한다

같은 actor에 대해 정상 push가 반복되다가 마지막 한 번만 실패한다.

```
stage=session_node_binding_resolve result=binding_present=true route_present=true
  session_rid=00000005 binding_generation=4 expected_binding_generation=4 sink_present=true match=true
stage=session_node_stream_write_submit  result=begin
stage=session_node_stream_write_terminal result=failed error_kind=5      <-- unavailable
stage=detached_delivery result=session_node_stream_write_terminal failed
stage=session_owner_route_tombstone result=connection_id=00000005 connection_generation=1 ...
```

**읽는 법이 중요하다.** 직전 성공 push와 실패 push의 resolve 줄이 **글자 그대로 같다** —
generation 4/4 일치, sink 존재, match=true. 즉 binding·route 장부는 마지막 순간까지 정합이고,
그 아래 physical stream write만 `unavailable`을 돌려준다. route tombstone은 실패 **뒤에** 찍히므로
원인이 아니라 결과다.

따라서 이 결함은 session/route 장부가 아니라 **stream connection lifetime 계층**에 있다.

#### 창(window)이 어디서 열리는지

`actor_gateway_runtime_t::admit_bound_session_delivery`가 `_state->mutex` 안에서
`bound_session_sinks`의 sink를 스냅샷으로 잡고, 실제 `(*sink)(...)` 호출은 **lock 밖 detached
executor에서** 일어난다. `shared_ptr`이라 sink 객체 자체는 살아 있지만 그 아래 물리 연결은
그 사이에 죽을 수 있다. 그리고 trace 순서가 보여 주듯 route tombstone은 write 실패 **뒤에**
적용된다 — 즉 **장부가 물리 연결보다 늦게 정리되고, 그 창 동안 죽은 연결로 push가 admit된다.**

client 입장의 결과가 나쁘다. join은 서버에서 승인됐는데 응답이 조용히 사라지고, client는
관측 가능한 disconnect 없이 대기 만료까지 간다. 스펙 §2.2가 정한 "framework가 physical
connection을 닫으며 client는 **관측 가능한** disconnect 뒤 재연결한다"와 다르다.

#### client가 왜 disconnect가 아니라 timeout으로 죽는가 — 서버 쪽 통지 누락

connector는 정상이다. `zlink_stream_calls.cpp`의 대기 루프는 `!is_transport_connected`를 검사해
`disconnected`나 `last_disconnect_error`를 돌려준다. **timeout이 났다는 것은 client 쪽 transport가
살아 있었다는 뜻**이다. 서버만 세션을 죽은 것으로 본 split-brain이다.

`stream_host_service.cpp`의 세션 종료 경로 셋 중 하나가 그 상태를 만든다.

| 경로 | 상황 | client 통지 |
|---|---|---|
| `:1681` `client_close` | socket monitor가 이미 끊김을 보고 | 불가·불필요 |
| `:2477` `connected_dispatch_error` | **client는 붙어 있는데 서버 connect dispatch만 실패** | **없음 — 결함** |
| `:2653` `disconnect_core_peer` | `disconnect_rid`로 물리 연결까지 끊음 | 정상 |

`close_core_session`은 `begin_core_session_close`를 `notify_reason` **없이** 부른다. 그래서 서버는
세션을 retire하고 route를 tombstone하지만 client에는 아무 것도 가지 않는다. client는 자기
deadline까지 기다린다.

**고쳤다.** `:2477`을 `disconnect_core_peer`로 바꿨다. 스펙 §2.2가 "framework가 physical
connection을 닫으며 client는 **관측 가능한** disconnect 뒤 재연결한다"를 요구하므로 통지만으로는
부족하고 연결을 끊는 쪽이 맞다.

**단, 이것이 ZoneWorld 실패 경로라고 확인하지는 못했다.** 관측된 실패는 성공 push가 여러 번 오간
뒤 발생하므로 connect 시점 경로가 아니다. 남은 후보는 `:1681` — socket monitor가 세션 도중
`disconnected`를 올리는데 client는 계속 연결로 보는 transport 수준의 split-brain이다. 이 세 경로에
close_reason trace가 없어 로그로 확정할 수 없었다. **다음 세션은 여기에 trace를 먼저 넣는다.**

**tombstone 순서 자체는 미착수.** teardown 순서를 바꾸는 일이라 검증 여유 없이 손대면 더 나빠질 수 있다고 판단했다.
대신 다음 세션이 지점을 특정할 수 있도록 write 실패 trace가 error kind만이 아니라 **메시지도
싣도록** 했다(`unavailable`은 STREAM host의 여러 곳에서 반환되므로 kind만으로는 지점을 못 고른다).

#### 부수적으로 고친 것 — queue-full을 `target_closed`로 적고 있었다

`actor_gateway_runtime.cpp`의 `trace_detached_bound_session_send_failure`가 세 가지 서로 다른
실패에 전부 `reason=target_closed`를 붙이고 있었고, 그중 하나는
`accepted=false detached_queue_full=true`(delivery executor 포화)였다. observability §3이
`reason` 값 집합을 고정하며 "송신 경로나 queue의 capacity가 일시적으로 부족한" 경우로
`backpressure`를 두고 있으므로 그쪽으로 바꿨다. **스펙 변경 없음.**

`stage`·`result`는 `detailed`에서만 찍히기 때문에 errors 모드 로그만 보면 세 경우가 구분되지
않고 전부 "상대 세션이 닫혔다"로 읽힌다. 실제로 이번 조사 초반에 그렇게 오독했다.

#### `fast_mutex.hpp:76` abort와 같은 계층일 가능성

둘 다 (1) 단독 실행에서는 안 나고 **7샘플 일괄에서만** 재현되며, (2) stream/pipe 연결 수명에
걸려 있다. abort는 `pthread_mutex_unlock`이 `EINVAL`을 돌려주는 것이므로 소유 객체가 이미
파괴된 신호다. 같은 원인의 두 증상일 수 있으니 하나를 잡을 때 다른 하나를 함께 확인한다.

### mkdocs 경고 237건에 대한 판정

**캠페인이 만든 것은 0건이다.** §10.1을 신설한 여섯 샘플 문서에서 나오는 경고는 없다.

`spec/server` 아래에서 16건이 잡히는데, 전부 **site 발행 범위 밖 파일**을 가리키는 링크다 —
`bindings/doc/spec/README.en.md`, `doc/principal/documentation/spec-writing-guide.ko.md` 같은
것들이고 **저장소에는 실재한다**(그래서 `check_doc_links.py`는 통과한다). 나머지 221건은
`bindings/reference/go` 64건을 비롯해 전부 캠페인 밖이다.

즉 mkdocs strict 실패는 **site의 발행 범위 설정 문제**이지 문서 오류가 아니다. 이 기준은 캠페인
README §10에 이미 기록돼 있다.

## 9. 일하는 방식에서 확인된 것

- **codex 기본 sandbox는 샘플을 못 돌린다**(loopback bind·Docker를 EPERM으로 차단).
  `-s danger-full-access`로 직접 기동해야 한다. 이걸 모르고 네 언어 작업을 받았을 때 전부
  "구현 완료, 실행 미검증"이었고 **그중 node는 실제로 깨져 있었다.**
- **언어별 병렬이 훨씬 빠르다.** 한 codex가 5언어를 순차 처리하던 것보다 확연히. 단
  **러너 실행은 직렬**로 둔다 — 실패 귀속이 안 되기 때문이다.
- **병렬화의 단위는 "언어"가 아니라 "빌드 트리"다.** 동시에 띄울 수 있는 작업 수는 트리 수로
  제한된다 — dotnet, cpp, node, JVM(java+kotlin 공용) 넷뿐이다. 한 트리가 이미 점유돼 있으면
  다음 샘플 작업을 미리 띄우지 말고 기다린다. 앞서 java SupportChat을 kotlin ShoppingMall과
  동시에 띄웠다가 같은 이유로 즉시 중단했다.
- **단, java와 kotlin은 병렬로 돌리지 않는다.** 둘은 같은 Gradle 프로젝트(`framework/languages/java`)를
  공유해서, 동시에 빌드하면 증분 상태가 깨진다. GameQuest에서 kotlin이
  `zlink-framework-kotlin` 컴파일 오류(`Unresolved reference 'ZLinkSpotCreateResponse'` 등)로 죽었는데,
  그 모듈은 **수정된 적이 없었다** — java 작업이 아직 빌드 중이어서 생긴 충돌이었다. java가 끝난 뒤
  다시 돌리니 그대로 통과했다. **증상이 "소스가 깨졌다"로 보이지만 원인은 동시성이다.**
- **구현 보고를 판정 근거로 삼지 않는다.** diff를 직접 읽고 러너를 직접 돌린다. 이 원칙으로
  이번에 잡은 것: node Bingo가 깨진 채 "완료" 보고된 건, dotnet TicTacToe를 기동 불가로 만든
  회귀, java framework 버그의 실증.
- **두 번째 조사자에게는 "확인"이 아니라 "반증"을 시킨다.** abort 건에서 1차 가설의 주장들을
  검증만 하고 반증할 코드를 찾지 않아 틀린 판정을 냈다. 병렬로 돌린 두 번째 조사가 그것을 뒤집었다.
