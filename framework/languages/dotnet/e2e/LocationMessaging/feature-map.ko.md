# .NET LocationMessaging E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-1-location-messaging.ko.md`

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| RM-A1 | 구현 | Store 자동 연결 request와 public topology query의 두 Ready node, RouteMesh status의 Ready peer를 검증한다(run_e2e.sh 전량 그린 실측). |
| RM-A2 | 구현 | 수동 endpoint request marker가 있다. |
| RM-A3 | 구현 | Linux actual-process에서 Automatic·Manual Object Client pair가 `NotRequired`와 Ready peer 0을 유지하고 20초 동안 reconnect하지 않음을 확인했다. 한쪽에 RouteMesh Channel Server를 등록하면 weight `100`과 `0` 모두 Automatic·Manual에서 Ready peer 1을 유지한다. Channel Client-only와 별도 ClientServer·classic fanout 등록은 연결 필요 판정을 바꾸지 않는다. 연결이 필요한 descriptor-only peer는 `NotConnected`·`Degraded`, Object Client Node-direct Send·Request는 `NotFound`이며 peer 수는 변하지 않는다. Object Client↔Object Server와 Object Server↔Object Server 대조군은 Ready다. 최신 증거는 `logs/20260729-043637-564415/`에 있다. |
| RM-A4 | 구현 | 같은 RID 교체 전·후 descriptor generation/endpoint와 ready peer 전환 marker가 있다. |
| RM-A6 | 구현 | MeshName 별 descriptor 집합·runtime snapshot 분리 marker가 있다. |
| RM-A7 | 미구현 | 같은 global ID를 Actor와 Spot에 동시에 예약하는 충돌, 실패 terminal 공유와 callback 단일 실행을 검증하는 role server·selector가 없다. |
| RM-B1 | 구현 | scale-out 뒤 신규 descriptor·ready ChannelName member 대기 marker가 있다. |
| RM-B2 | 구현 | 정상 종료 provider의 descriptor·ready member 제거 대기 marker가 있다. |
| RM-B3 | 구현 | owner lease 만료 후 stale descriptor 제외와 생존 provider 성공/bounded error marker가 있다. |
| RM-C1 | 구현·process 통과 | `api-a`의 local Channel server weight를 0으로 제외한 뒤 `api-a` 역할 server가 unique ChannelName만으로 request/send한다. Remote `api-b` reply·handler evidence와 source handler 0건을 확인했다. 실제 process 증거는 `logs/20260728-232700-3148011`이며 M73의 cross-MeshNode 호출 경계를 함께 검증한다. |
| RM-C2 | 구현 | targeted request by rid marker가 있다. |
| RM-C3 | 구현 | 다중 provider 분산 marker가 있다. |
| RM-C4 | 구현 | timeout 뒤 late reply 비오염 marker가 있다. |
| RM-C5 | 구현 | 미등록 packet 처리 marker가 있다. |
| RM-C7 | 구현 | `SetWeight`·descriptor `ChannelWeights`·runtime `Weight` 기반 75/25 분포 marker가 있다. |
| RM-C8 | 구현 | RouteMesh SS에 Framework-level `MaxMessageSize`를 설정하지 않고 1 byte, 4KiB, 256KiB, 1MiB payload 왕복 hash/length marker와 후속 정상 request를 검증한다. |
| RM-C9 | 구현 | non-blocking 즉시 backpressure vs blocking bounded admission 대조와 회복 marker가 있다. |
