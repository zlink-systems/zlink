# Java PubSub E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-3-pubsub.ko.md`

마지막 검증:

- 명령: `nice -n 10 timeout 600s ./run_e2e.sh all`
- 결과: passed
- 로그: `framework/languages/java/e2e/PubSub/logs/20260707-221458-3633382/`

현재 source와 runner는 Redis location store와 endpoint 없는 subscriber를 사용한다. 이 기록은 기본
자동 연결 전송은 확인하지만 전용 publisher descriptor, Publisher RID와 lease·store lifecycle 전체를
확인하지 않으므로 PS-D/E 완료 증거는 아니다.

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| PS-A1 | 10.0.0 전환 대상 | automatic subscriber의 `ConnectionReady` 뒤 public fanout client로 publish하고 subscriber `/evidence/wait`로 공통 연속 sequence를 확인한다. |
| PS-A2 | 10.0.0 전환 대상 | 서로 다른 packet name에 등록한 typed handler가 자기 event만 정확히 한 번 처리하는지 `/evidence/wait`와 snapshot으로 확인해야 한다. Transport filter나 payload field로 다시 분류하지 않는다. |
| PS-A3 | 구현 | `sub-3`를 pre-late publish 뒤에 시작하고, 구독 전 이벤트가 replay되지 않으며 이후 publish만 받는지 `/evidence/wait`로 확인한다. |
| PS-A4 | 차단 | 현재 구현은 `sub-1` 프로세스를 재시작하므로 application startup이 handler를 다시 등록한다. 공통 계약이 요구하는 동일 process의 transport 단절·복구와 기존 subscription 자동 재적용을 검증하지 않는다. subscriber 하나의 연결만 끊는 process-external network fault harness가 필요하다. |
| PS-B1 | 구현 | `sub-1` handler 지연 중에도 `sub-2`와 `sub-3`가 최신 이벤트를 계속 받는지 `/evidence/wait`로 확인한다. |
| PS-B2 | 10.0.0 전환 대상 | publisher를 같은 endpoint로 재시작하고 기존 subscriber의 `ConnectionReady` 뒤 새 event 수신을 확인한다. 동적 역할 readiness는 3초다. |
| PS-C1 | 구현 | 미등록 packet name publish가 subscriber observer evidence에 `HANDLER_MISSING`/`DROP`으로 남고 이후 정상 publish가 유지되는지 `/evidence/wait`로 확인한다. |

| 시나리오 | 상태 | 필요한 Java 증거 |
|---|---|---|
| PS-D1 | 전환 대상 | 전용 descriptor fixture·Publisher RID·actual port 자동 연결 |
| PS-D2 | 전환 대상 | public `ZLinkFanoutRuntime` snapshot의 publisher identity·connection intent와 `excluded_draining` sealed event entry로 다른 ChannelName·descriptor 종류·drain 중 publisher를 제외하고, actual native SUB ready publisher만 handler에 도달 |
| PS-D3 | 미구현 | public fanout snapshot의 `connectionIntentCount=2`·`readyConnectionCount=2`와 실제 native disconnect 후 `ZLinkFanoutPublisherChanged` sealed event의 `disconnected` entry로 publisher 추가·정상 제거 수렴 |
| PS-D4 | 미구현 | public fanout event의 기존 identity `disconnected`, 새 identity `reconnecting`·actual native `ready`, `excluded_stale` sealed entry와 최신 snapshot으로 lease 만료·재등록·낮은 generation/revision 거부 확인 |
| PS-D5 | 미구현 | public `ZLinkFanoutLocationChanged` sealed event의 `degraded`·`ready` Location snapshot, publisher changed `reconnecting`·actual native `ready`·`excluded_stale` entry와 current connection intent snapshot으로 fail-static·복구 수렴 확인 |
| PS-D6 | 미구현 | port 0 재시작과 advertised endpoint 갱신 |
| PS-D7 | 미구현 | capacity 1 observer의 bounded coalescing·sequence gap 후 public snapshot resync, `Flow.Subscription.cancel()` 격리, 정상 observer·dispatch 지속과 manual endpoint mutation의 automatic snapshot·event 격리 |
| PS-E1 | 미구현 | store 없는 manual subscriber 회귀 |
| PS-E2 | 미구현 | automatic subscriber store 누락, automatic/manual mode 혼합, 고정 Publisher RID와 자동 할당 둘 다 누락, fixed/allocated RID 동시 설정의 typed startup 오류와 store 없는 manual 조합 성공 |

## Evidence wait 검증

공통 E2E README는 Pub/Sub처럼 검증 대상이 client stream session이 아니라 subscriber 역할 server의
fanout delivery인 경우, subscriber handler가 남긴 bounded `/evidence/wait` marker를 성공 기준으로
쓸 수 있다고 정한다. Java PubSub client는 같은 snapshot GET을 반복하지 않고 `/evidence/wait`로
각 subscriber의 실제 dispatch marker를 기다린 뒤 필요한 snapshot만 대조한다.

## 공통 scenario parity gap — 2026-07-29

- `PS-F1`, `PS-F2`, `PS-F3`, `PS-F4`, `PS-F5`: 공통 scenario는 추가됐지만 Java actual
  fixture와 runner selector가 없다.
