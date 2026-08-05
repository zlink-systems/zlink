# Node.js PubSub E2E feature map

| Scenario | 상태 | Node.js 검증 파일 | 비고 |
|----------|------|-------------------|------|
| `PS-A1` | 구현 | `Client/Scenarios/ps-a1-fanout-basic-delivery-scenario.ts` | warm-up barrier 뒤 모든 subscriber가 공유하는 연속 sequence marker를 실제 subscriber 역할 server의 `/evidence/wait`로 확인한다. |
| `PS-A2` | 10.0.0 전환 대상 | 기존 `PS-A2` scenario | 현재 transport filter 시나리오를 서로 다른 packet name의 typed handler가 자기 event만 정확히 한 번 처리하는 시나리오로 교체해야 한다. |
| `PS-A3` | 구현 | `Client/Scenarios/ps-a3-late-subscriber-scenario.ts` | 차단된 transport에서 ready 전 event를 한 번 발행하고, subscriber의 실제 `ConnectionReady` 뒤 첫 event 수신과 이전 event replay 부재를 확인한다. |
| `PS-A4` | 구현 | `Client/Scenarios/ps-a4-subscriber-reconnect-scenario.ts` | subscriber process를 유지한 채 transport를 끊고 복구해 기존 subscription 자동 재적용, disconnect 구간 non-replay, fast subscriber 지속 수신을 확인한다. |
| `PS-B1` | 구현 | `Client/Scenarios/ps-b1-slow-subscriber-scenario.ts` | slow subscriber delay evidence와 fast subscriber tail event marker를 실제 subscriber 역할 server evidence로 확인한다. |
| `PS-B2` | 구현 | `Client/Scenarios/ps-b2-publisher-restart-scenario.ts` | publisher를 같은 endpoint로 재시작한 뒤 기존 subscriber의 `ConnectionReady`와 첫 event 전달을 확인한다. |
| `PS-C1` | 구현 | `Client/Scenarios/ps-c1-missing-message-name-scenario.ts` | subscriber dispatch drop evidence와 이후 정상 publish delivery marker를 실제 subscriber 역할 server evidence로 확인한다. |

## 검증 경로 판정

기본 PS-A1~C1 실행 경로는 Redis를 등록하지 않고 manual endpoint를 사용한다. 이 경로는 manual
fanout 회귀 증거이며 automatic discovery 완료 증거가 아니다. PS-E2C는 이 경로와 분리된 Redis
Store-backed publisher 시작 검증으로 전용 Redis namespace를 사용한다.

| Scenario | 상태 | Node.js 목표 증거 |
|---|---|---|
| `PS-D1` | 구현 | 전용 Redis namespace에서 Publisher RID `pub-d1`을 등록하고 endpoint 없는 subscriber가 public fanout status의 Ready publisher를 확인한 뒤 typed event를 실제 handler에서 받는다. |
| `PS-D2` | 미구현 | public `ZLinkFanoutRuntime` snapshot의 publisher identity·connection intent와 `excluded_draining` discriminated event entry로 다른 ChannelName·descriptor 종류·drain 중 publisher를 제외하고, actual native SUB ready publisher만 handler에 도달 |
| `PS-D3` | 구현 | Redis Store에서 `pub-b`를 추가한 뒤 두 publisher의 event를 받고, `pub-a`를 orderly shutdown한 뒤 public status와 실제 delivery가 `pub-b`만 남은 상태로 수렴하는지 확인한다. |
| `PS-D4` | 구현 | Publisher A를 실제 process에서 강제 종료하고 ready set에서 제거된 뒤 같은 RID의 replacement를 port 0으로 시작하여 새 event를 받는지 확인한다. |
| `PS-D5` | 구현 | Subscriber의 Store 연결만 fault proxy로 차단한 동안 기존 publisher event를 받고 Ready 상태를 유지한 뒤 Store 복구 후에도 event delivery가 계속되는지 확인한다. |
| `PS-D6` | 미구현 | port 0 재시작과 advertised endpoint 갱신 |
| `PS-D7` | 미구현 | capacity 1 observer의 bounded coalescing·sequence gap 후 public snapshot resync, `AbortSignal` 격리, 정상 observer·dispatch 지속과 manual endpoint mutation의 automatic snapshot·event 격리 |
| `PS-D7A` | 구현 | Subscriber의 public fanout observer 두 개 중 하나를 첫 callback에서 대기시키고 publisher 추가·제거와 event delivery를 수행한 뒤 정상 observer와 handler가 계속 진행하고 slow observer만 취소되는지 확인한다. |
| `PS-E1` | 구현 | 현재 manual runner를 store 없는 별도 회귀로 유지 |
| `PS-F3` | 구현 | exact liveness topic은 public configuration error로 거부하고, 더 긴 prefix topic은 typed handler에 한 번 전달한다. |
| `PS-F2` | 구현 | Publisher B의 advertised endpoint를 전용 TCP proxy로 연결한 뒤 B 방향만 차단하고, A가 Ready와 delivery를 유지하며 B가 복구되는지 확인한다. |
| `PS-F4` | 구현 | 두 publisher가 Ready인 상태에서 한 publisher를 정상 종료하고, 15초 peer deadline보다 짧은 시간 안에 public status에서 제외한 뒤 다른 publisher의 event를 받는다. |
| `PS-F5` | 구현 | Automatic subscriber가 application evidence에 기록하지 않는 `billing` traffic을 16초 동안 받는 동안 publisher Ready를 유지하고, 이후 `orders` event를 정상 처리하는지 확인한다. |
| `PS-E2A` | 구현 | Store와 endpoint가 모두 없는 automatic subscriber가 listener를 열기 전에 startup configuration error로 종료되는지 확인한다. |
| `PS-E2B` | 구현 | 같은 subscriber registration에 automatic source와 manual endpoint를 함께 등록하면 listener를 열기 전에 startup configuration error로 종료되는지 확인한다. |
| `PS-E2C` | 구현 | Redis Store-backed publisher가 identity를 생략하거나 fixed RID와 automatic prefix를 함께 설정하면 listener bind 전에 startup configuration error로 종료되는지 두 child process에서 확인한다. |
| `PS-E2` | 부분 구현 | `PS-E2A`·`PS-E2B`·`PS-E2C`를 구현했다. 공통 E2E의 남은 Track D/F 항목은 별도 시나리오로 남아 있다. |

Classic fanout의 수신자는 client stream session이 아니라 subscriber 역할 server다. 공통 E2E README는
이 경우 subscriber handler가 남긴 bounded `/evidence/wait` marker를 성공 기준으로 사용할 수 있다고
정리한다. 따라서 이 feature map은 별도 client stream connector observer를 요구하지 않는다.

## 검증

- `timeout 420s framework/languages/node/e2e/PubSub/run_e2e.sh`
  - 기존 manual 경로 결과: `pubsub e2e result=passed`
  - 최신 automatic `PS-D1` 확인 로그 디렉터리: `log/20260805-032006-3683231`
  - 최신 automatic `PS-D3` 확인 로그 디렉터리: `log/20260805-032838-3733775`
  - 통과 scenario: `PS-A1`, `PS-A2`, `PS-A3`, `PS-A4`, `PS-B1`, `PS-B2`, `PS-C1`

이 로그에서 `PS-A2`라는 이름으로 통과한 항목은 기존 transport filter 시나리오다. Packet name별 typed
handler dispatch로 교체하기 전에는 공통 `PS-A2` 완료 증거로 사용하지 않는다.
