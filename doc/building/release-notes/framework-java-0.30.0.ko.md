[English](./framework-java-0.30.0.md) | [한국어](./framework-java-0.30.0.ko.md)

# ZLink Java·Kotlin Framework 0.30.0 릴리스 노트

Framework 0.30.0은 Java·Kotlin binding 1.19.0과 Core 1.19.0을 사용합니다.

## 변경

- Classic fanout publisher의 socket 설정은 channel의 fanout builder 한 곳에서 시작 전에 정합니다. Send timeout과 NoDrop은 channel마다 따로 고를 수 있고, 모든 publisher는 linger 0으로 시작하며 종료할 때도 그 값을 유지합니다. Send HWM은 Framework가 설정하지 않고 Core 값을 사용합니다. (#1586)
- 같은 host의 Spot·Actor 호출과 local publish가 application job queue의 유효 최대값을 넘겨 대기하면 `zlink.runtime.host.local_job_backlog_exceeded` Warning을 대기 구간마다 한 번 기록합니다. Remote 수신 대기는 포함하지 않고, 수락·실행 동작은 바뀌지 않습니다. (#1466)
- 공개 API 제거: `ZLinkMeshNodeBuilder.configureSpotPublisher()`, `ZLinkSpotPublisherConfig`. Publisher send timeout은 channel의 fanout builder에서 정합니다. (#1586)

## 결함 수정

- Owner node가 강제 종료된 User·Instance Spot을 다시 만들면 `location owner lease is unavailable`로 계속 실패하던 결함을 수정했습니다. Reserve하는 node에 등록된 relocation 정책이 꺼진(Disabled) type은 owner lease가 끝난 Ready record를 해제하고 새 generation을 만듭니다. User Spot은 Store membership이 없을 때만 해제하며, 일반 message와 Find는 record를 회수하지 않습니다. (#1575)
- 토큰 없는 request가 completion slot 부족으로 거절되면 `Unavailable`로 보고합니다. 실제 request timeout은 계속 `DeadlineExceeded`입니다. (#1602)
- RouteMesh와 ClientServer의 peer liveness deadline(15초)을 현재 connection에서 받은 모든 record로 갱신합니다. (#1466)
- Fanout·ClientServer의 `listenerStatus`가 wildcard 주소를 게시하던 결함을 수정했습니다. Binding이 실제로 bind한 endpoint를 게시합니다. (#1466)
- 수신 round가 ordinary permit과 control 송신 수락을 기다리며 같은 router의 infrastructure 진행을 막던 결함을 수정했습니다. (#1466)
- Raw service port가 닫히는 중에 native 제출이 시작되어 close가 `EBUSY`로 실패할 수 있던 경합을 수정했습니다. (#1466)
- ClientServer Client DEALER에 host receive-flow 상태(PAUSED)를 걸지 않습니다. 이 상태는 이 host에게 요청이 들어오는 socket(RouteMesh ROUTER, ClientServer Server ROUTER)에만 겁니다. Client host가 바쁠 때 server가 그 client의 reply를 보내지 못하던 결함을 수정했습니다. (#1466)
- Liveness 만료(15초 동안 수신 없음)가 admission만 끝내고 물리 연결을 닫지 않던 결함을 수정했습니다. 만료된 연결의 target을 제거하므로 그 연결에서 대기하던 send가 종료됩니다. (#1466)
- 같은 Instance Spot에 대한 두 요청이 겹칠 때, 이미 Ready인 Spot에 대한 요청이 `Stored Instance activation is missing`(DataLost)으로 실패할 수 있던 결함을 수정했습니다. 현재 owner와 같은 generation의 local Spot이 있으면 recovery 기록을 읽기 전에 수락합니다. (#1575)
- Core 1.19.0을 사용합니다. Core 1.18.0에서 receive-flow 상태 전환이 socket turn 밖에서 pipe에 control을 써서, 다른 thread의 send와 겹치면 프로세스가 `pipe.cpp` assertion이나 SIGSEGV로 종료될 수 있던 결함이 고쳐졌습니다. Framework는 host 압력에 따라 이 전환을 호출하므로 부하가 높은 server에서 드러날 수 있었습니다. (#1622)
- Host 종료가 이미 수락한 Instance activation의 Store 정리(최초 메시지 terminal 기록·recovery 기록 삭제)를 기다리지 않던 결함을 수정했습니다. Spot 종료 단계가 이 정리를 drain deadline 안에서 기다립니다. (#1575)
- Instance Spot 요청이 이동한 owner의 거부(SpotMoving)나 framework Unavailable로 끝나도 Ready route cache를 지우지 않아, 다음 요청이 계속 이전 owner로 가던 결함을 수정했습니다. 이런 terminal을 받으면 cache를 무효화하고, 다음 독립 요청은 현재 owner를 다시 조회합니다. 끝난 요청은 다시 제출하지 않습니다. (#1575)

## 설치

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.30.0")
    implementation("systems.zlink:zlink-http-client:0.30.0")
}
```

릴리스 태그는 [`framework-java/v0.30.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-java%2Fv0.30.0)입니다.
[한국어](./framework-java-0.30.0.ko.md) | [English](./framework-java-0.30.0.md)
