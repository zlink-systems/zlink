# Kotlin PubSub E2E feature map

이 문서는 Config 3 Pub/Sub 공통 시나리오 중 Kotlin 전용 E2E 상태를 정리한다. runner와 scenario
code는 Kotlin public framework API로 작성해 Kotlin 호출 표면에서 검증한다.

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| PS-A1 | 10.0.0 전환 대상 | 모든 subscriber의 `ConnectionReady` 뒤 측정 구간을 시작하고 공통 연속 sequence를 같은 순서로 받는지 확인해야 한다. 현재 evidence wait만으로는 구독 readiness 경계를 증명하지 못한다. |
| PS-A2 | 10.0.0 전환 대상 | 서로 다른 packet name에 등록한 typed handler가 자기 event만 정확히 한 번 처리하고 cross-dispatch가 0회인지 subscriber evidence로 확인해야 한다. Transport filter나 payload field로 다시 분류하지 않는다. |
| PS-A3 | 구현 | late subscriber가 이전 publish를 replay 받지 않고 이후 publish만 받는지 subscriber evidence로 확인한다. |
| PS-A4 | 차단 | 현재 Client support는 subscriber process를 중단하고 다시 시작하므로 application startup이 subscription을 다시 등록한다. 동일 process의 transport 단절·복구와 기존 subscription 자동 재적용을 검증하지 못한다. subscriber 연결만 끊는 fault harness가 필요하다. |
| PS-B1 | 구현 | 느린 subscriber가 있어도 다른 subscriber가 마지막 sequence까지 받는지 bounded subscriber evidence wait로 확인한다. |
| PS-B2 | 10.0.0 전환 대상 | subscriber process와 등록한 typed handler를 유지한 채 같은 endpoint의 publisher를 재시작하고, 새 publisher의 `ConnectionReady` 뒤 새 event를 받는지 확인해야 한다. 현재 row 교체와 수신은 확인하지만 socket readiness evidence가 남아 있다. |
| PS-C1 | 구현 | 미등록 packet publish가 subscriber dispatch error/drop으로 기록되고 정상 publish가 회복되는지 subscriber evidence로 확인한다. |

| 시나리오 | 상태 | 필요한 Kotlin 증거 |
|---|---|---|
| PS-D1 | 전환 대상 | 전용 descriptor fixture·Publisher RID·actual port 자동 연결 |
| PS-D2 | 전환 대상 | public `ZLinkFanoutRuntime` snapshot의 publisher identity·connection intent와 `excluded_draining` Java sealed event entry로 다른 ChannelName·descriptor 종류·drain 중 publisher를 제외하고, actual native SUB ready publisher만 handler에 도달 |
| PS-D3 | 미구현 | public fanout snapshot의 `connectionIntentCount=2`·`readyConnectionCount=2`와 실제 native disconnect 후 `ZLinkFanoutPublisherChanged` sealed event의 `disconnected` entry로 publisher 추가·정상 제거 수렴 |
| PS-D4 | 미구현 | public fanout event의 기존 identity `disconnected`, 새 identity `reconnecting`·actual native `ready`, `excluded_stale` sealed entry와 최신 snapshot으로 lease 만료·재등록·낮은 generation/revision 거부 확인 |
| PS-D5 | 미구현 | public `ZLinkFanoutLocationChanged` sealed event의 `degraded`·`ready` Location snapshot, publisher changed `reconnecting`·actual native `ready`·`excluded_stale` entry와 current connection intent snapshot으로 fail-static·복구 수렴 확인 |
| PS-D6 | 미구현 | port 0 재시작과 advertised endpoint 갱신 |
| PS-D7 | 미구현 | capacity 1 observer의 bounded coalescing·sequence gap 후 public snapshot resync, `Flow.Subscription.cancel()` 격리, 정상 observer·dispatch 지속과 manual endpoint mutation의 automatic snapshot·event 격리 |
| PS-E1 | 미구현 | store 없는 manual subscriber 회귀 |
| PS-E2 | 미구현 | automatic subscriber store 누락, automatic/manual mode 혼합, 고정 Publisher RID와 자동 할당 둘 다 누락, fixed/allocated RID 동시 설정의 typed startup 오류와 store 없는 manual 조합 성공 |

## 검증 경로 판정

Pub/Sub fanout의 수신자는 client stream session이 아니라 subscriber 역할 server다. 공통 E2E README는
이 경우 subscriber handler가 남긴 bounded `/evidence/wait` marker를 성공 기준으로 사용할 수 있다고
정리한다. 따라서 Kotlin PubSub는 client stream connector observer를 추가하지 않고, 실제 subscriber
역할 server의 bounded evidence wait와 snapshot 단언으로 fanout, non-replay, negative path를 검증한다.

## 포팅 구조 상태

현재 Kotlin PubSub E2E는 `Shared`, `Client`, `Server/Publisher`, `Server/Subscriber` Gradle project로
나뉜다. Publisher와 endpoint 없는 subscriber가 Redis location store를 등록하지만 전용 fanout descriptor
계약과 PS-D/E lifecycle matrix는 아직 구현하지 않았다.
Client는 framework fanout client를 직접 들지 않고 publisher role의 HTTP endpoint를 호출한다. role
실행 설정은 각 role의 CLI option parser가 맡고, PS-A4/PS-B2 lifecycle 제어는 Client support의
process launcher가 맡는다. runner는 초기 role 시작, client 실행, cleanup을 담당한다. 파일별 대응
상태는 `porting-inventory.ko.md`에 기록한다.

## 완료 판정

표에서 `구현`으로 표시한 행은 해당 시나리오의 검증 로직이 존재한다. `PS-A1`·`PS-B2`는
`ConnectionReady`를 포함한 readiness 경계를 확인하기 전까지 전환 대상으로 유지한다. `PS-A4`는 동일 process에서
transport 연결만 복구하는 fault harness가 추가되기 전까지 차단 상태다. PS-D/E는 전용 descriptor와
automatic discovery, manual 회귀를 각각 실제 process로 실행한 뒤에만 완료로 판정한다.
