# C++ Pub/Sub E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-3-pubsub.ko.md`

이 문서는 Config 3 Pub/Sub 공통 시나리오 중 C++ framework E2E가 현재 검증하는 항목과,
public API 또는 harness 제어가 더 필요한 항목을 구분한다.

`logs/20260708-123833-1298240`은 Redis location store와 endpoint 없는 subscriber를 사용하는 현재
source의 PS-A1~C1 기록이다. 자동 연결의 기본 전송 경로 증거지만 전용 descriptor, Publisher RID,
ChannelName·descriptor 종류 격리와 lease lifecycle 전체를 검증하지 않으므로 새 PS-D/E 완료 증거로 사용하지 않는다.

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| `PS-A1` | 10.0.0 전환 대상 | automatic subscriber의 실제 `ConnectionReady` 뒤 Publisher role server가 측정 sequence를 발행하고 세 subscriber의 bounded `/evidence/wait`가 공통 sequence를 확인해야 한다. |
| `PS-A2` | 10.0.0 전환 대상 | 서로 다른 packet name에 등록한 typed handler가 자기 event만 정확히 한 번 처리하는지 확인해야 한다. Transport filter나 payload field로 다시 분류하지 않는다. |
| `PS-A3` | 10.0.0 전환 대상 | late automatic subscriber가 연결된 이후 발행분만 받고 연결 전 발행분은 replay되지 않는지 확인해야 한다. |
| `PS-A4` | 10.0.0 전환 대상 | 같은 subscriber process를 유지한 채 transport만 단절·복구하고, 기존 subscription 자동 재적용과 단절 구간 non-replay를 확인해야 한다. 현재 process 재시작 방식은 이 계약을 검증하지 않는다. |
| `PS-B1` | 10.0.0 전환 대상 | 한 automatic subscriber handler에 지연을 주입해도 다른 subscriber가 같은 발행 sequence를 계속 수신하는지 확인해야 한다. |
| `PS-B2` | 10.0.0 전환 대상 | Publisher를 같은 RID의 새 lifecycle로 다시 시작한 뒤 기존 subscriber가 새 descriptor 연결 이후 발행분을 받는지 확인해야 한다. |
| `PS-C1` | 10.0.0 전환 대상 | automatic topology에서 미등록 message name의 `no_handler`/`drop` evidence와 후속 정상 publish 복구를 확인해야 한다. |

| 시나리오 | 상태 | 필요한 C++ 증거 |
|---|---|---|
| PS-D1 | 전환 대상 | generic peer row가 아닌 전용 descriptor fixture와 actual port 자동 연결 |
| PS-D2 | 전환 대상 | public `fanout_runtime_t` snapshot의 publisher identity·connection intent와 `excluded_draining` discriminated event entry로 다른 ChannelName·descriptor 종류·drain 중 publisher를 제외하고, actual native SUB ready publisher만 handler에 도달 |
| PS-D3 | 미구현 | public fanout snapshot의 `connection_intent_count=2`·`ready_connection_count=2`와 실제 native disconnect 후 publisher changed discriminated event의 `disconnected` entry로 추가·정상 제거 수렴 |
| PS-D4 | 미구현 | public fanout event의 기존 identity `disconnected`, 새 identity `reconnecting`·actual native `ready`, `excluded_stale` discriminated entry와 최신 snapshot으로 lease 만료·재등록·낮은 generation/revision 거부 확인 |
| PS-D5 | 미구현 | public location changed discriminated event의 `degraded`·`ready` Location snapshot, publisher changed의 `reconnecting`·actual native `ready`·`excluded_stale` entry와 current connection intent snapshot으로 fail-static·복구 수렴 확인 |
| PS-D6 | 미구현 | port 0 actual endpoint가 바뀐 publisher 재시작 |
| PS-D7 | 미구현 | capacity 1 observer의 bounded coalescing·sequence gap 후 public snapshot resync, observation `close()` 격리, 정상 observer·dispatch 지속과 manual endpoint mutation의 automatic snapshot·event 격리 |
| PS-E1 | 미구현 | store 없는 manual subscriber 별도 process 회귀 |
| PS-E2 | 미구현 | automatic subscriber store 누락, automatic/manual mode 혼합, 고정 Publisher RID와 자동 할당 둘 다 누락, fixed/allocated RID 동시 설정의 typed startup 오류와 store 없는 manual 조합 성공 |

## 검증 경로 판정

Pub/Sub fanout의 수신자는 client stream session이 아니라 subscriber 역할 server다. 공통 E2E README는
이 경우 subscriber handler가 남긴 bounded `/evidence/wait` marker를 성공 기준으로 사용할 수 있다고
정리한다. C++ PubSub도 실제 subscriber role server의 `/evidence/wait`로 accepted, ignored, dispatch
error line을 기다리므로 별도 client stream connector observer gap은 남기지 않는다. 최신 runner는
검증 stdout을 `verify.log`에 저장하고, publisher role의 `/health`, `/evidence`,
`/evidence/clear`, `/shutdown` endpoint 동작도 operational log와 final evidence snapshot으로 남긴다.
