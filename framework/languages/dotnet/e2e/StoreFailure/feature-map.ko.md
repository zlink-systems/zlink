# .NET StoreFailure E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-6-store-failure-recovery.ko.md`

이 문서는 `.NET` StoreFailure client가 실제로 실행하는 시나리오만 기록한다.

| 시나리오 | 상태 | 근거 |
|----------|------|------|
| SF-A1 | 구현 | 정상 store에서 consumer descriptor/runtime snapshot에 `api-a`와 `api-b`가 모두 보이고, request가 성공하며, consumer와 두 provider의 runtime status가 healthy store와 owner lease 갱신을 보고한다. |
| SF-A2 | 구현 | 별도 consumer가 opaque Store snapshot scan만으로 provider 추가와 제거를 descriptor/runtime status에 반영한다. Provider-specific watch/change-stamp public surface는 사용하지 않는다. |
| SF-B1 | 구현 | store 장애 중에도 기존 연결 request가 계속 성공하고, consumer runtime status가 store unhealthy와 owner lease heartbeat 실패를 기록한 뒤 복구 후 healthy로 돌아온다. |
| SF-B2 | 구현 | `logs/20260720-034627-2002949`: store 중단이 갱신 timeout 안에 unhealthy status로 관측되고, 기존 연결 request는 grace 이후에도 성공하며, store 재개 뒤 lease와 peer row가 복구됐다. |
| SF-D1 | 구현 | `logs/20260720-034731-2004889`: store 중단과 재개 전후에 established request가 유지되고 runtime status가 unhealthy에서 healthy로 전환됐다. |
| SF-D3 | 구현 | `logs/20260720-034938-2009369`: healthy → unhealthy와 오류 → healthy 순서를 확인했고, 복구 뒤 `LastRefreshAt`이 장애 전 시각보다 증가했다. |
| SF-C2 | 구현 | `logs/20260720-035008-2011571`: graceful drain 동안 descriptor가 draining으로 관측되고 종료 뒤 owner row와 lease가 제거됐다. |
| SF-C1 | 구현 | `logs/20260720-035008-2011571`: provider 강제 종료 뒤 lease 만료에 따라 stale descriptor가 성공 결과에서 제외됐다. |
| SF-D2 | 구현 | `logs/20260720-035008-2011571`: crash와 lease 만료 전후의 peer row 및 runtime 상태 전이가 계약대로 관측됐다. |
| SF-E1 | 구현 | `logs/20260720-035008-2011571`: store operation 지연을 주입한 상태에서 timeout과 복구 경계를 확인했다. |

현행 공통 Config 6에 추가된 아래 시나리오는 `.NET` runner에 등록되지 않았다.

| 시나리오 | 상태 | 누락 범위 |
|---|---|---|
| SF-B3 | 미구현 | Store failure grace와 stateful owner fence 분리 |
| SF-C3 | 미구현 | Stale owner lease token과 generation fencing |
| SF-C4 | 미구현 | Host lease와 여러 routing slot |
| SF-C5 | 미구현 | Bounded descriptor reconcile |
| SF-F1 | 미구현 | 언어 간 authority key·payload interop |
| SF-F2 | 미구현 | Current relocation renew과 orphan 정리 |
| SF-F3 | 미구현 | Relocation recovery horizon 초과 |
| SF-F4 | 미구현 | Authority generation 원자 전이와 exhaustion |
| SF-F5 | 미구현 | Durable authority와 owner lease 분리 |
| SF-F6 | 미구현 | Snapshot-consistent recovery scan |
| SF-F7 | 미구현 | Chunked relocation manifest |
| SF-F8 | 미구현 | Relocation target reservation lease fence |
| SF-F9 | 미구현 | Owner-token bulk cleanup fence |
| SF-F10 | 미구현 | Compact authority와 relocation completion |
| SF-F11 | 미구현 | Provider cancellation과 buffer lifetime |
| SF-G1 | 미구현 | Actor·Spot·stable type limit의 atomic reservation |
| SF-G2 | 미구현 | Unlimited·Entry Spot·activation concurrency 분리 |
| SF-G3 | 미구현 | User Spot aggregate relocation capacity vector |
