# .NET PubSub E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-3-pubsub.ko.md`

10.0.0 목표에서는 publisher와 automatic subscriber가 location store를 사용한다. 현재 `.NET` E2E는
Redis 없이 manual endpoint로만 연결하므로 아래 PS-A1~C1은 manual 회귀 증거이고 automatic discovery
완료 증거가 아니다.

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| PS-A1 | 구현 | 세 subscriber의 `ConnectionReady` 뒤 공통 연속 sequence 수신 marker를 확인한다. |
| PS-A2 | 구현 | 서로 다른 packet name의 typed handler가 각 event를 한 번씩 처리하고 다른 handler가 처리하지 않은 marker를 확인한다. |
| PS-A3 | 구현 | 늦게 시작한 subscriber의 `ConnectionReady` 뒤 발행분 수신과 연결 전 발행분 non-replay를 확인한다. |
| PS-A4 | 구현 | 같은 subscriber process에서 외부 TCP fault proxy로 `Disconnected`·`ConnectionReady`를 만들고, 복구 뒤 수신과 disconnect 구간 non-replay를 확인한다. |
| PS-B1 | 구현 | 한 subscriber handler의 처리 지연 중에도 다른 subscriber가 계속 수신하는 marker를 확인한다. |
| PS-B2 | 구현 | 정상 종료한 publisher를 같은 manual endpoint로 재시작하고, 기존 subscriber의 `Disconnected`·`ConnectionReady`와 복구 뒤 수신 marker를 확인한다. |
| PS-C1 | 구현 | 미등록 packet name의 `no_handler`·`drop` marker와 이후 정상 event 수신을 확인한다. |

| 시나리오 | 상태 | 필요한 `.NET` 증거 |
|---|---|---|
| PS-D1 | source 구현·process 미검증 | automatic subscriber runtime과 publisher descriptor가 source·unit test에 존재한다. endpoint 없는 subscriber의 actual port 자동 연결 process selector와 evidence는 아직 없다. |
| PS-D2 | 미구현 | public `IZLinkFanoutRuntime` snapshot의 publisher identity·connection intent와 `excluded_draining` sealed event entry로 다른 ChannelName·descriptor 종류·drain 중 publisher를 제외하고, actual native SUB ready publisher만 handler에 도달 |
| PS-D3 | 미구현 | public fanout snapshot의 `ConnectionIntentCount=2`·`ReadyConnectionCount=2`와 실제 native disconnect 후 `PublisherChanged` sealed event의 `disconnected` entry로 publisher 추가·정상 제거 수렴 |
| PS-D4 | 미구현 | public fanout event의 기존 identity `disconnected`, 새 identity `reconnecting`·actual native `ready`, `excluded_stale` sealed entry와 최신 snapshot으로 lease 만료·재등록·낮은 generation/revision 거부 확인 |
| PS-D5 | 미구현 | public `LocationChanged` sealed event의 `degraded`·`ready` Location snapshot, `PublisherChanged` `reconnecting`·actual native `ready`·`excluded_stale` entry와 current connection intent snapshot으로 fail-static·복구 수렴 확인 |
| PS-D6 | 미구현 | port 0 재시작 뒤 새 advertised endpoint 연결 |
| PS-D7A | 미구현 | 느린 fanout status observer의 bounded coalescing과 snapshot resync를 검증하는 selector가 없다. |
| PS-D7B | 미구현 | Manual endpoint 변경이 automatic status를 바꾸지 않는지 검증하는 selector가 없다. |
| PS-E1 | 구현 | 현재 PS-A1~C1 manual endpoint runner를 store 없는 별도 회귀로 유지 |
| PS-E2A | 미구현 | Automatic subscriber의 Store 누락을 startup에서 거부하는 selector가 없다. |
| PS-E2B | 미구현 | Automatic과 manual mode를 한 registration에 섞을 때의 startup 오류 selector가 없다. |
| PS-E2C | 미구현 | Automatic publisher identity 누락과 중복을 거부하는 selector가 없다. |
| PS-F1 | 미구현 | Automatic·manual subscriber가 첫 valid application record 또는 liveness beacon 전에는 Ready가 되지 않는지 검증하는 selector가 없다. |
| PS-F2 | 미구현 | Publisher 하나의 단방향 packet blackhole과 publisher별 timeout·reconnect 격리를 검증하는 selector가 없다. |
| PS-F3 | 미구현 | Reserved beacon topic의 public 거부, prefix 비충돌과 malformed beacon fail-closed 처리를 검증하는 selector가 없다. |
| PS-F4 | 미구현 | Orderly disconnect 즉시 반영과 subscriber 종료 뒤 reconnect·beacon resource 정리를 검증하는 selector가 없다. |
| PS-F5 | 미구현 | 구독하지 않는 application topic 트래픽과 periodic beacon readiness가 독립적인지 30초 동안 검증하는 selector가 없다. |

## 검증 경로 판정

Pub/Sub fanout의 수신자는 client stream session이 아니라 subscriber 역할 server다. 공통 E2E README는
이 경우 subscriber handler가 남긴 bounded `/evidence/wait` marker를 성공 기준으로 사용할 수 있다고
정리한다. 따라서 이 feature map은 별도 client stream connector observer를 요구하지 않는다.
