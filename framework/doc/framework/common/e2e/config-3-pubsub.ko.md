<!-- framework-adapter-nav:start -->
[E2E 목차](README.ko.md) | [이전: Spot 서비스](config-2-spot-service.ko.md) | [다음: 등록·codec](config-4-registration-codec.ko.md)
<!-- framework-adapter-nav:end -->

# Config 3 — Classic fanout publish와 subscriber

Classic fanout은 한 publisher가 보낸 event를 현재 준비된 여러 subscriber에게 전달한다. Publish 완료는
subscriber 수신 확인이 아니며, 늦게 연결했거나 연결이 끊긴 동안의 event를 replay하지 않는다. Automatic
mode에서는 subscriber가 endpoint를 입력받지 않고 Location Store에서 같은 ChannelName의 publisher를
찾는다.

이 config는 실제 publisher·subscriber process와 public fanout API만 사용하여 delivery, reconnect,
discovery와 liveness를 검증한다. Raw PUB/SUB frame, private descriptor와 socket monitor는 사용하지 않는다.

## 1. 확인 범위

- 준비된 여러 subscriber의 fanout delivery와 packet name handler 선택
- Late subscriber, subscriber reconnect와 publisher restart의 non-replay
- Automatic publisher discovery, lease·Store 장애와 port 변경
- Manual endpoint mode와 startup validation
- Publisher별 liveness, reserved topic validation과 orderly disconnect
- 느린 subscriber와 느린 status observer의 격리

## 2. 배포 구성

| 역할 | 수 | 하는 일과 분리 이유 |
|---|---:|---|
| Location Store | 1 | Automatic publisher descriptor와 owner lease를 제공한다. 실행마다 전용 namespace를 사용한다. |
| Publisher | scenario별 1~2 | `events` fanout Channel에서 typed event를 publish한다. Automatic mode는 port 0과 Publisher RID를 사용하고, manual mode는 별도 고정 endpoint를 사용한다. |
| Subscriber | scenario별 2~3 | Packet name별 typed handler와 public fanout status endpoint를 제공한다. Automatic 또는 manual mode 중 하나만 사용한다. |
| E2E client | 1 | Publisher의 application endpoint로 publish하고 subscriber의 public evidence를 조회한다. |

Subscriber handler는 publisher marker, packet name, sequence와 typed payload를 application state에 기록한다.
Fanout status와 observer가 반환한 값도 역할 server의 public evidence endpoint에서 확인한다. Transport
endpoint는 manual mode에서만 Application 설정에 들어간다.

## 3. 공통 실행과 판정 방법

Runner는 scenario마다 process, Store namespace와 sequence 범위를 새로 만든다. Automatic subscriber의
public status에서 publisher가 `Ready`인 것을 확인한 뒤 측정 event를 보낸다. Status observer는 중간
상태를 합칠 수 있으므로 모든 transition을 요구하지 않고 마지막 `GetStatus`와 실제 event 수신을
대조한다.

Publish terminal과 remote delivery는 별도로 판정한다. Publish는 local admission 결과로 확인하고 delivery는
subscriber handler evidence로 확인한다. Classic fanout은 replay를 제공하지 않으므로 연결 전이나 단절 중
event가 나중에 도착하면 실패다.

## 4. Scenario

### Track A — Event를 준비된 subscriber에게 전달

#### PS-A1 준비된 subscriber가 같은 event를 받는다

우선순위: `P0`

Publisher가 여러 subscriber에게 fanout할 때 준비된 각 subscriber가 event를 받을 수 있어야 한다. Classic
fanout은 subscriber 간 동일한 순서나 lossless delivery를 보장하지 않으므로 이 scenario는 cross-subscriber
순서를 판정하지 않는다.

**검증 질문:** 세 subscriber가 ready인 뒤 발행한 event marker를 각각 관찰하는가.

- 시작 조건: 세 subscriber의 public status에서 같은 publisher가 ready이고 각 handler gate가 열려 있다.
  작은 marker 하나를 사용하며 network block이나 의도적인 backpressure는 넣지 않는다.
- 절차: Publisher가 이 scenario 전용 `fanout-ready` marker를 publish한다. 각 subscriber의 application
  evidence를 bounded wait로 확인한다.
- 검증: 세 subscriber가 `fanout-ready` marker를 각각 기록한다. Publish terminal만으로 delivery를 판정하지
  않으며 subscriber 간 수신 순서와 누락되지 않은 전체 sequence는 요구하지 않는다.
- 세부 동작: [Framework API §11](../spec/06-framework-api.ko.md)의 fanout delivery를
  검증한다.

#### PS-A2 Packet name으로 typed handler를 선택한다

우선순위: `P0`

같은 fanout Channel에 여러 event type을 보내도 packet name에 등록된 handler만 실행되어야 한다.

**검증 질문:** 두 packet name의 event가 각각 대응하는 typed handler에서만 한 번 처리되는가.

- 시작 조건: Subscriber가 `InventoryChangedNotify`와 `PriceChangedNotify` handler를 등록하고 publisher가
  ready다.
- 절차: 두 event를 서로 다른 marker로 한 번씩 publish한다.
- 검증: 각 marker는 대응 handler evidence에만 한 번 기록되고 typed payload 값이 입력과 일치한다. Topic
  또는 payload field로 handler를 다시 선택하지 않는다.
- 세부 동작: [Framework API §11](../spec/06-framework-api.ko.md)의 handler namespace를
  검증한다.

#### PS-A3 Late subscriber는 ready 이후 event부터 받는다

우선순위: `P0`

Classic fanout은 과거 event를 보관하지 않는다. 늦게 시작한 subscriber는 연결 전에 발행한 event를 받지
않고 ready 이후의 새 event부터 받아야 한다.

**검증 질문:** Late subscriber가 ready 전 event를 replay하지 않고 ready 뒤 event만 받는가.

- 시작 조건: Publisher와 기존 subscriber가 ready이고 late subscriber process는 시작하지 않았다.
- 절차: `before-ready`를 publish한 뒤 late subscriber를 시작한다. Public status가 ready가 되면
  `after-ready`를 publish한다.
- 검증: Late subscriber는 `after-ready`를 한 번 받고 `before-ready`는 받지 않는다. 기존 subscriber는
  두 event를 모두 받는다.
- 세부 동작: [Framework API §11](../spec/06-framework-api.ko.md)의 non-replay delivery를
  검증한다.

#### PS-A4 Subscriber reconnect 뒤 새 event를 받는다

우선순위: `P1`

Transport가 끊겼다가 복구되어도 Application이 handler를 다시 등록할 필요는 없다. 다만 단절 중 event는
replay되지 않는다.

**검증 질문:** Subscriber가 기존 handler registration으로 reconnect 뒤 event를 받고 단절 중 event는
받지 않는가.

- 시작 조건: Subscriber A와 B가 같은 publisher를 ready로 보고 baseline event를 받았다.
- 절차: Runner가 A의 publisher 수신 network만 차단한다. A의 public status가 not-ready가 된 뒤
  `while-disconnected`를 publish하고 B 수신을 확인한다. 차단을 해제하여 A가 ready가 되면
  `after-reconnect`를 publish한다.
- 검증: B는 두 event를 모두 받고 A는 `after-reconnect`만 받는다. A process와 handler registration은
  전체 구간 유지된다.
- 세부 동작: [Transport liveness §6](../spec/29-transport-liveness.ko.md)의
  fanout reconnect를 검증한다.

### Track B — Subscriber 간 처리를 격리

#### PS-B1 느린 subscriber가 다른 subscriber를 막지 않는다

우선순위: `P1`

Subscriber는 각자 별도 process에서 event를 처리한다. 한 handler가 오래 걸려도 다른 subscriber의 handler는
계속 실행되어야 한다.

**검증 질문:** Subscriber A handler가 대기 중이어도 subscriber B가 후속 event를 처리하는가.

- 시작 조건: A와 B가 ready이며 A handler는 첫 marker에서 application signal을 기다리도록 구성한다.
- 절차: Gate marker를 publish하여 A가 handler에 들어간 것을 확인하고 `B-follow-up` marker를 publish한다.
  B의 evidence를 확인한 뒤 A gate를 해제한다.
- 검증: A가 대기 중인 동안 B는 `B-follow-up` marker를 처리한다. B의 수신 순서와 A의 catch-up 또는 drop
  수는 이 scenario의 통과 조건으로 정하지 않는다.
- 세부 동작: [Channel topology](../spec/07-channel-topology.ko.md)의 subscriber process와 handler dispatch
  격리를 검증한다.

#### PS-B2 Publisher 재시작 뒤 기존 subscriber가 새 event를 받는다

우선순위: `P1`

Publisher가 재시작되면 subscriber는 새 connection을 만들고 첫 정상 record 뒤 ready가 되어야 한다.
Application handler를 다시 등록하거나 과거 event를 replay하지 않는다.

**검증 질문:** Publisher 재시작 뒤 기존 subscriber가 새 event를 받고 중단 구간 event를 replay하지
않는가.

- 시작 조건: Publisher와 subscriber가 ready이고 baseline event를 받았다.
- 절차: Publisher를 정상 종료하고 subscriber status가 not-ready가 된 것을 확인한다. 같은 역할의
  publisher를 다시 시작하여 ready가 되면 새 marker를 publish한다.
- 검증: 기존 subscriber process가 새 marker를 한 번 받는다. Handler registration을 다시 호출하지 않고
  종료 중의 marker가 나중에 나타나지 않는다.
- 세부 동작: [Transport liveness §6](../spec/29-transport-liveness.ko.md)를
  검증한다.

### Track C — Automatic discovery와 publisher lifecycle을 처리

#### PS-D1 Endpoint 없이 publisher를 발견한다

우선순위: `P0`

Automatic subscriber는 publisher endpoint를 Application 설정이나 client 입력으로 받지 않는다. 같은
ChannelName의 live descriptor를 발견하여 연결한다.

**검증 질문:** Endpoint를 입력하지 않은 subscriber가 automatic publisher를 ready로 만들고 event를
받는가.

- 시작 조건: Publisher가 port 0으로 ready이며 Location Store에 current descriptor를 게시했다.
- 절차: Endpoint 설정이 없는 automatic subscriber를 시작하고 public status에서 publisher ready를
  기다린 뒤 marker를 publish한다.
- 검증: Ready publisher count는 1이고 subscriber handler가 marker를 한 번 받는다. Subscriber application
  입력에는 transport endpoint가 없다.
- 세부 동작: [Framework API §11](../spec/06-framework-api.ko.md)의 automatic discovery를
  검증한다.

#### PS-D2 같은 ChannelName의 live fanout publisher만 선택한다

우선순위: `P0`

Store에는 다른 fanout Channel, RouteMesh와 ClientServer descriptor가 함께 있을 수 있다. Subscriber는
descriptor 종류와 ChannelName이 모두 맞고 신규 작업을 받을 수 있는 publisher만 사용해야 한다.

**검증 질문:** `events` subscriber가 current `events` publisher의 event만 받는가.

- 시작 조건: Live `events` publisher, `audit` publisher와 별도 RouteMesh·ClientServer 역할이 Store에
  등록되어 있다. 두 fanout publisher가 각각 ready다.
- 절차: `events`와 `audit`에서 서로 다른 marker를 publish한다.
- 검증: `events` subscriber status와 handler evidence에는 live `events` publisher와 marker만 나타난다.
  `audit` event와 다른 topology 역할은 포함되지 않는다.
- 세부 동작: [Runtime monitoring §2.2](../spec/24-runtime-monitoring.ko.md)의 fanout status를
  검증한다.

#### PS-D3 Publisher 추가와 정상 제거에 수렴한다

우선순위: `P1`

같은 ChannelName의 publisher가 늘거나 줄면 subscriber는 process를 재시작하지 않고 current set을 따라가야
한다.

**검증 질문:** Publisher B 추가와 A 제거 뒤 status와 실제 event source가 current set과 일치하는가.

- 시작 조건: `pub-a` 하나가 ready이고 subscriber가 A의 baseline event를 받았다.
- 절차: `pub-b`를 시작하여 ready count 2를 확인하고 두 publisher에서 marker를 보낸다. `pub-a`를 정상
  종료하여 status에서 제거된 뒤 B에서 새 marker를 보낸다.
- 검증: 두 publisher가 ready일 때 두 marker를 모두 받고, A 제거 뒤 status에는 B만 ready로 남으며 B의
  새 marker를 계속 받는다.
- 세부 동작: [Transport liveness §5](../spec/29-transport-liveness.ko.md)을 검증한다.

#### PS-D4 Crash한 publisher를 replacement로 바꾼다

우선순위: `P0`

Publisher가 crash하면 owner lease 만료 뒤 이전 descriptor를 current connection으로 사용하지 않아야 한다.
Replacement가 ready가 되면 새 event부터 받는다.

**검증 질문:** Crash 뒤 subscriber가 이전 publisher를 제외하고 replacement의 event를 받는가.

- 시작 조건: `pub-a`가 ready이고 subscriber가 baseline event를 받았다.
- 절차: Runner가 `pub-a`를 강제 종료한다. Public status에서 이전 publisher가 not-ready 또는 제거된 것을
  확인하고 replacement를 시작한다. Replacement가 ready가 되면 새 marker를 publish한다.
- 검증: Status에는 replacement만 ready이며 subscriber는 새 marker를 한 번 받는다. Crash 구간 event는
  replay되지 않는다.
- 세부 동작: [Transport liveness §5](../spec/29-transport-liveness.ko.md)와
  [§7](../spec/29-transport-liveness.ko.md)을 검증한다.

#### PS-D5 Store 장애 중 기존 connection을 유지하고 복구한다

우선순위: `P1`

Location Store polling 실패는 이미 ready인 transport connection의 liveness를 대신하지 않는다. 기존
publisher가 계속 정상 record를 보내면 subscriber는 event를 계속 받을 수 있다.

**검증 질문:** Store 장애 중 기존 publisher event를 받고 Store 복구 뒤 current descriptor set에
수렴하는가.

- 시작 조건: Publisher와 subscriber가 ready이고 baseline event를 받았다.
- 절차: Runner가 subscriber의 Store 접근을 차단한 뒤 기존 publisher에서 marker를 보낸다. Store 접근을
  복구하고 public fanout status가 current 상태로 수렴할 때 새 marker를 보낸다.
- 검증: 장애 중과 복구 뒤 marker를 모두 한 번 받는다. Store 장애만으로 기존 publisher를 즉시
  not-ready로 바꾸지 않는다.
- 세부 동작: [Transport liveness §7](../spec/29-transport-liveness.ko.md)을
  검증한다.

#### PS-D6 Port 0 재시작으로 endpoint가 바뀌어도 다시 연결한다

우선순위: `P1`

Port 0 publisher는 재시작할 때 실제 port가 달라질 수 있다. Automatic subscriber는 이전 endpoint를
고정하지 않고 current descriptor를 따라가야 한다.

**검증 질문:** Publisher의 actual port가 바뀐 뒤에도 endpoint 재설정 없이 새 event를 받는가.

- 시작 조건: Port 0 publisher와 endpoint 입력이 없는 subscriber가 ready다.
- 절차: Publisher의 public listener status에서 첫 actual port를 기록하고 정상 종료한다. 같은 역할을 port
  0으로 재시작하여 다른 actual port와 subscriber ready를 확인한 뒤 marker를 publish한다.
- 검증: 두 actual port는 0이 아니고 서로 다르다. Subscriber application 설정은 바뀌지 않으며 새 marker를
  한 번 받는다.
- 세부 동작: [Network listener identity §4](../spec/10-network-listener-identity.ko.md)를
  검증한다.

#### PS-D7A 느린 fanout status observer를 격리한다

우선순위: `P1`

한 status observer가 callback을 늦게 처리해도 publisher connection, event dispatch와 다른 observer는
계속 진행해야 한다.

**검증 질문:** 느린 observer가 대기 중이어도 정상 observer와 fanout handler가 current 상태를
처리하는가.

- 시작 조건: Subscriber에서 느린 observer와 정상 observer를 열고 느린 observer의 첫 callback을
  application signal에서 대기시킨다.
- 절차: Publisher를 추가·제거하고 business event를 보낸다. 정상 observer와 handler evidence를 확인한 뒤
  느린 observer를 취소한다.
- 검증: 정상 observer는 latest status를 제공하고 handler는 event를 한 번 처리한다. 느린 observer의
  sequence에 gap이 있으면 `GetStatus`로 current 상태를 복원할 수 있으며 취소가 다른 observer를 끝내지
  않는다.
- 세부 동작: [Runtime monitoring §3](../spec/24-runtime-monitoring.ko.md)을
  검증한다.

#### PS-D7B Manual endpoint 변경은 automatic status를 바꾸지 않는다

우선순위: `P1`

Automatic과 manual subscriber는 별도 connection mode다. Manual Channel의 endpoint 변경이 automatic
Channel의 publisher set에 반영되어서는 안 된다.

**검증 질문:** Manual endpoint를 추가·제거해도 automatic Channel의 status와 delivery가 유지되는가.

- 시작 조건: Automatic `events` subscriber와 별도 이름의 manual subscriber가 각각 ready다.
- 절차: Manual subscriber의 public connection handle로 endpoint를 추가하고 제거한다. 전후에 automatic
  publisher에서 marker를 보낸다.
- 검증: Automatic subscriber의 ready publisher identity는 유지되고 두 marker를 모두 받는다. Manual
  변경만으로 automatic status sequence가 바뀌었다고 요구하지 않는다.
- 세부 동작: [Framework API §11](../spec/06-framework-api.ko.md)의 mode 분리를 검증한다.

### Track D — Manual mode와 startup validation을 확인

#### PS-E1 Store 없이 manual endpoint만 사용한다

우선순위: `P0`

Manual subscriber는 Application이 지정한 endpoint에만 연결하며 Location Store가 없어도 동작한다.

**검증 질문:** Manual subscriber가 Store 없이 지정한 publisher의 event만 받는가.

- 시작 조건: 별도 publisher를 고정 endpoint로 시작하고 subscriber에는 그 endpoint만 설정한다.
- 절차: Subscriber가 ready가 된 뒤 `before-late`와 `after-ready` marker를 이용해 normal delivery와 late
  non-replay를 실행한다.
- 검증: Ready 이후 marker를 받고 ready 이전 marker는 replay하지 않는다. Store process와 descriptor를
  사용하지 않는다.
- 세부 동작: [Framework API §11](../spec/06-framework-api.ko.md)의 manual mode를 검증한다.

#### PS-E2A Automatic subscriber의 Store 누락을 startup에서 거부한다

우선순위: `P0`

Endpoint가 없는 automatic subscriber는 publisher discovery에 Location Store가 필요하다.

**검증 질문:** Store를 등록하지 않은 automatic subscriber host가 configuration error로 종료되는가.

- 시작 조건: Negative host가 endpoint 없는 subscriber만 등록하고 Location Store를 등록하지 않는다.
- 절차: Runner가 host를 시작하여 process terminal과 health를 확인한다.
- 검증: Host는 listener와 ready status를 공개하지 않고 public configuration error로 종료된다.
- 세부 동작: [Framework API §7](../spec/06-framework-api.ko.md)의 prerequisite를
  검증한다.

#### PS-E2B Automatic과 manual mode를 한 registration에 섞으면 거부한다

우선순위: `P0`

한 subscriber registration이 Store discovery와 명시 endpoint를 동시에 사용하면 connection source가
모호해진다.

**검증 질문:** 두 mode를 함께 설정한 host가 startup configuration error로 종료되는가.

- 시작 조건: Negative host의 같은 subscriber registration에 automatic mode와 manual endpoint를 함께
  지정한다.
- 절차: Runner가 host를 시작한다.
- 검증: Host는 background connection을 시작하기 전에 configuration error로 종료된다.
- 세부 동작: [Framework API §11](../spec/06-framework-api.ko.md)의 mode validation을
  검증한다.

#### PS-E2C Automatic publisher identity 누락과 중복을 거부한다

우선순위: `P0`

Automatic publisher는 고정 RID 또는 automatic allocation 중 정확히 하나를 선택해야 한다.

**검증 질문:** Publisher identity를 선택하지 않거나 두 방식을 함께 고르면 startup에서 실패하는가.

- 시작 조건: 두 negative host를 만들고 하나는 RID 방식을 모두 생략하며 다른 하나는 둘 다 설정한다.
- 절차: Runner가 두 host를 각각 시작한다.
- 검증: 두 host 모두 listener bind 전에 원인에 맞는 public configuration error로 종료된다.
- 세부 동작: [Framework API §11](../spec/06-framework-api.ko.md)의 Publisher identity를
  검증한다.

### Track E — Publisher별 liveness를 확인

#### PS-F1 Automatic과 manual publisher가 Ready로 수렴한다

우선순위: `P0`

Descriptor 발견이나 connect 반환만으로는 event를 받을 수 있다고 판단할 수 없다. Public fanout status의
Ready는 첫 정상 application record 또는 liveness beacon까지 반영한다.

**검증 질문:** Automatic과 manual subscriber가 각 publisher를 Ready로 표시한 뒤 첫 event를 정상
처리하는가.

- 시작 조건: 두 subscriber를 publisher보다 먼저 시작하여 public status가 ready가 아님을 확인한다.
- 절차: Automatic publisher와 manual publisher를 각각 시작한다. 각 status가 ready가 되면 즉시 marker를
  publish한다.
- 검증: 두 subscriber가 자기 publisher marker를 한 번씩 받는다. 연결 전에는 ready target으로 사용하지
  않는다. 모든 짧은 중간 state를 observer에서 보았다고 요구하지 않는다.
- 세부 동작: [Transport liveness §4](../spec/29-transport-liveness.ko.md)와
  [§5](../spec/29-transport-liveness.ko.md)을 검증한다.

#### PS-F2 Publisher 하나의 수신 단절을 다른 publisher와 분리한다

우선순위: `P0`

Subscriber는 publisher마다 connection과 liveness deadline을 구분한다. 한 publisher에서 record가 오지
않아도 다른 publisher는 ready 상태와 delivery를 유지해야 한다.

**검증 질문:** Publisher B의 수신만 차단했을 때 A는 ready 상태로 event를 계속 전달하는가.

- 시작 조건: 같은 ChannelName의 A와 B가 한 subscriber에서 모두 ready다.
- 절차: Runner가 B에서 subscriber로 가는 network만 차단한다. A는 marker를 계속 publish한다. B가 public
  status에서 not-ready가 될 때까지 기다린 뒤 차단을 해제한다.
- 검증: B만 ready 목록에서 제외되는 동안 A marker는 계속 처리된다. Host 전체는 Error가 되지 않는다.
  B는 reconnect 뒤 다시 ready가 되고 새 marker를 전달한다.
- 세부 동작: [Transport liveness §4](../spec/29-transport-liveness.ko.md)의 publisher별
  liveness를 검증한다.

#### PS-F3 Reserved liveness topic을 Application publish에서 거부한다

우선순위: `P0`

Framework는 fanout liveness에 사용하는 exact topic을 Application event와 구분한다. Exact reserved 값은
거부하지만 같은 prefix의 더 긴 topic까지 금지해서는 안 된다.

**검증 질문:** Exact reserved topic은 argument error이고 prefix가 더 긴 topic은 정상 전달되는가.

- 시작 조건: Publisher와 subscriber가 ready이고 subscriber가 typed event handler를 등록했다.
- 절차: Public publish API로 exact reserved topic을 한 번 시도한다. 이어서 같은 prefix에 byte를 추가한
  topic으로 정상 event를 publish한다.
- 검증: 첫 호출은 transport admission 전에 public argument error로 끝나고 handler가 실행되지 않는다.
  두 번째 event는 handler에서 한 번 처리된다. Private beacon frame을 E2E에서 직접 만들지 않는다.
- 세부 동작: [Transport liveness §4](../spec/29-transport-liveness.ko.md)의 reserved topic을
  검증한다.

#### PS-F4 Orderly disconnect를 peer deadline 전에 반영한다

우선순위: `P1`

정상 종료 신호를 받은 subscriber는 15초 peer deadline을 기다리지 않고 publisher를 ready 목록에서
제외해야 한다.

**검증 질문:** Publisher 정상 종료가 public status에 즉시 반영되고 다른 publisher는 유지되는가.

- 시작 조건: A와 B publisher가 모두 ready다.
- 절차: Runner가 A를 정상 종료하고 common readiness timeout 안에서 status를 관찰한다. B에서 marker를
  publish한다.
- 검증: A는 고정 15초 deadline보다 먼저 ready 목록에서 빠지고 B는 ready를 유지하며 marker를 한 번
  전달한다.
- 세부 동작: [Transport liveness §5](../spec/29-transport-liveness.ko.md)을 검증한다.

#### PS-F5 구독하지 않은 traffic 중에도 liveness를 유지한다

우선순위: `P0`

Subscriber가 특정 topic의 Application event를 처리하지 않아도 Framework의 liveness record는 별도로
수신해야 한다. 그렇지 않으면 정상 publisher를 15초 뒤 끊을 수 있다.

**검증 질문:** 구독하지 않은 topic만 계속 publish해도 publisher가 peer deadline을 넘겨 Ready를
유지하는가.

- 시작 조건: Subscriber는 `events.b`만 구독하고 publisher는 ready다.
- 절차: Publisher가 `events.a` event를 peer deadline보다 긴 검증 구간 동안 주기적으로 보낸다. 검증
  구간은 fixed 15초 deadline에 runner tolerance를 더한 값으로 계산한다.
- 검증: `events.a` handler evidence는 없지만 publisher status는 ready를 유지한다. 이후 `events.b` marker를
  보내면 한 번 처리된다.
- 세부 동작: [Transport liveness §2](../spec/29-transport-liveness.ko.md)와
  [§4](../spec/29-transport-liveness.ko.md)를 검증한다.

### Track F — Handler가 없는 event를 처리

#### PS-C1 등록되지 않은 packet name을 drop한다

우선순위: `P0`

Subscriber에 packet handler가 없으면 그 event를 Application handler에 전달할 수 없다. 다른 정상 packet의
dispatch는 계속되어야 한다.

**검증 질문:** Handler 없는 packet은 public observer에서 `no_handler/drop`으로 보이고 다음 정상 event는
처리되는가.

- 시작 조건: Subscriber가 normal packet handler와 public message-flow observer를 등록하고 publisher가
  ready다.
- 절차: Handler가 없는 packet name을 publish한 뒤 normal packet을 publish한다.
- 검증: 첫 event는 handler evidence가 없고 public observer가 `no_handler/drop`을 한 번 제공한다. Normal
  event는 handler에서 한 번 처리된다.
- 세부 동작: [Framework API §11](../spec/06-framework-api.ko.md)과
  [Message flow tracing §2.2](../spec/26-message-flow-tracing.ko.md)을 검증한다.

## 5. 완료 기준

- 모든 scenario는 public fanout publish, status·observer와 역할 server의 application evidence만 사용한다.
- Raw frame, private descriptor, socket monitor와 protocol-negative publisher를 E2E assertion에 사용하지 않는다.
- Ready와 reconnect는 public status를 bounded polling하고 실제 event 수신으로 대조한다. 모든 중간 state가
  관찰된다고 가정하지 않는다.
- Publish terminal은 remote delivery evidence로 사용하지 않으며 연결 전·단절 중 event의 replay를 기대하지
  않는다.
- Liveness의 5초·15초 고정값을 검증할 때만 시간 경계를 사용하고 runner tolerance를 명시한다. 임의의
  settle sleep으로 정상 여부를 판정하지 않는다.
