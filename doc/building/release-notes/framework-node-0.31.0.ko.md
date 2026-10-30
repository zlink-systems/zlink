[한국어](./framework-node-0.31.0.ko.md) | [English](./framework-node-0.31.0.md)

# ZLink Node.js Framework 0.31.0 릴리스 노트

Framework 0.31.0은 Node.js binding 1.21.0과 Core 1.21.0을 사용합니다.

## 변경 사항

- handler에 선언한 static `messageType`으로 수신 메시지 클래스를 복원합니다. (#1731, #2715)
- Actor typed message와 reply를 wire 계약에 맞춰 전송하고, 응답을 요청 packet의 reply 계약으로 검사합니다. (#2267, #2507)
- runtime metric에서 `zlink.spot.count`의 label과 값을 통일하고, capacity 및 runtime 수집 지표를 보완했습니다. (#2149, #2189, #2199)
- Instance activation의 기한 초과를 `DeadlineExceeded`로 처리하고, one-way 생성 실패를 flow 진단에 기록합니다. (#2242)
- target Join lifecycle 실패를 보고하고, relocation outcome 처리 계약을 반영했습니다. (#2278, #2426)

## 결함 수정

- Closing 중인 Spot에 대한 Join·Close 요청이 먼저 접수된 작업의 terminal 결과를 따르도록 수정했습니다. (#2672, #2675)
- 자동 연결 reconcile이 다른 state lane의 완료를 기다리며 교착되던 문제를 수정했습니다. (#2673)
- liveness 만료 시 admission을 먼저 닫고, 이미 사라진 physical RID 정리를 완료로 처리합니다. (#2369, #2717, #2734)
- admission 거절 요청이 domain reply로 terminal 처리되도록 수정했습니다. (#2625)
- User Spot retire 중 `Continued` callback 순서를 보존합니다. (#2627)
- Actor ingress의 수락·Follow·stale 판단을 한 경로에서 처리하고, 늦은 membership generation 충돌을 분류합니다. (#2799, #2738)
- bound Session push의 현재성 판단과 frame 제출을 같은 registry lane turn에서 처리합니다. (#2757)
- 명시적 Spot close는 Close operation의 deadline을 사용합니다. (#2849)
- Node·Channel handler는 FIFO 순서로 시작하고, 앞 handler의 완료를 기다리지 않습니다. (#2758)
- RouteMesh peer readiness를 상대 ADMIT 수신으로 확정하고 첫 HELLO를 즉시 보냅니다. (#2790)
- 같은 endpoint에 대한 중복 connect, bind 등록 판정, close 실패 후 raw host 접근, Session 대기 중 Actor 정리, User Spot retire의 callback 순서 관련 결함을 수정했습니다. (#1710, #2140, #2627, #2797, #3177, #3191)
- Errors 진단 record의 Flow 포함 조건과 heartbeat 종료 처리를 수정했습니다. (#2243, #3170)
- enum ordinal을 schema 이름으로 사용하는 잘못된 입력을 거절합니다. (#1713)
- 연결이 준비되기 전에 들어온 remote request를 admission owner 하나가 판정합니다. 준비된 연결에서는 대기 구독 없이 바로 제출합니다. (#1710, #3258)
- `GetOrCreate`가 새 생성 capacity를 보기 전에 기존 authority를 확인합니다. capacity가 모두 쓰였을 때 이미 있는 User Spot을 다시 얻는 요청이 실패하던 문제를 수정했습니다. (#2799, #3242)

## 성능 개선

- request·send·mesh dispatch 경로에서 completion 처리와 admission callback을 통합하고, 불필요한 Promise 경계·배열 iterator·중간 복사를 줄였습니다. (#1640, #1675, #2232, #2237, #2351, #2603)
- request backpressure 수신 저장소를 재사용하고, ClientServer receive rotation당 permit을 예약합니다. (#1648, #2349)
- service reply multipart 생성에서 임시 배열과 iterator를 제거했습니다. (#3193, #3194)

## 알려진 제한

- 시험 코드의 private 멤버 접근을 공개 경로로 옮기는 작업을 Node·Java·.NET에서 9차까지 진행했습니다. 남은 범위는 다음 릴리스에서 이어 갑니다. 제품 동작에는 영향이 없습니다. (#2309)
- Windows에서 mailbox signaler가 AF_UNIX datagram 실패 뒤 stream으로 전환하면 socket마다 임시 경로를 만들고 지우는 데 약 2.5ms가 듭니다. 수정은 다음 Core 릴리스에 포함됩니다. (#3053)

## 설치

```bash
npm install @zlink-systems/framework@0.31.0 @zlink-systems/http-client@0.31.0
```

릴리스 태그는 [`framework-node/v0.31.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-node%2Fv0.31.0)입니다.
