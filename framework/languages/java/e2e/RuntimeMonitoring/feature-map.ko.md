# Java RuntimeMonitoring E2E feature map

기준 문서: `framework/doc/framework/common/e2e/config-7-monitoring.ko.md`

## 10.0.0 목표 판정

Java E2E는 각 host의 public runtime snapshot과 상태 변경 stream을 검증한다. 복구할 수 없는
runtime 오류는 application callback이 아니라 structured log로 기록한다. Client는 HTTP driver이고 framework
operation은 Trigger와 Service role이 실행한다.

| 시나리오 | 상태 | 현재 증거 | 남은 gap |
|---|---|---|---|
| MON-A1 | 구현 | `svc-a` 단독 snapshot과 `svc-b` ready 뒤 snapshot에서 MeshNode·peer·channel·claim·location·drain 값, sequence 증가와 이전 값의 불변성을 확인했다. | 없음 |
| MON-A2 | 구현 | Runtime observer를 연 상태에서 `svc-b`를 정상 종료·재시작해 peer event, 같은 RID, 새 lifecycle generation, descriptor revision, endpoint, admission·ready·last failure field를 snapshot과 대조했다. | 없음 |
| MON-A3 | 구현 | `svc-b` RouteMesh weight를 0으로 바꿔 local weight와 `svc-a` ready member 수 1을 확인하고 실제 select-one request가 `svc-a`만 선택함을 검증했다. 복원 뒤 ready member 수 2와 `svc-b` 재선택도 확인했다. | 없음 |
| MON-A4A | 구현 | `svc-b`를 정상 종료한 뒤 ready target 제거를 확인하고 같은 역할의 새 process를 시작해 ready 직후 request가 replacement에서 처리되는지 확인한다. | 없음 |
| MON-A4B | 구현 | `svc-b`를 crash endpoint로 종료하고 stale ready target 제거를 확인한 뒤 replacement의 ready와 request 처리를 확인한다. | 없음 |
| MON-A5 | 구현 | Redis를 정지·복구해 `zlink.runtime.location.store_changed`의 `degraded`·`ready` 전이, location state·last success·last failure, 장애 중 admitted messaging 유지와 복구 뒤 ready topology를 확인했다. | 없음 |
| MON-B1 | 부분 구현 | zero-target publish를 실제로 시작한 뒤 public snapshot·event에 publish 전용 필드가 없고, public record와 runtime class file에 제거한 type·metric·event 이름이 없음을 검사한다. | 막힌 remote target이 있어도 시작 뒤 정상 완료하며 rollback·자동 재시도가 없음을 process E2E에서 추가로 확인한다. |
| MON-B2 | 부분 구현 | local subscriber를 만든 뒤 publish하고 public snapshot·event에 publish 전용 관측값이 없음을 검사한다. | subscriber handler의 단일 처리와 막힌 local target, message-flow trace의 target별 결과 부재를 추가로 확인한다. |
| MON-C1 | 10.0.0 전환 대상 | Application callback과 monitoring observer의 격리 구현은 유지한다. | claim progress·request completion·느린 observer 격리·sequence gap 뒤 snapshot resync를 한 시나리오에서 다시 검증한다. |
| MON-D1A | 구현 | 등록하지 않은 MeshName 조회가 public runtime endpoint에서 configuration error로 끝나고 기존 Mesh 상태에는 영향을 주지 않는다. | 없음 |
| MON-D1B | 구현 | 동일 observer에서 peer crash와 restart를 세 번 반복하고 각 cycle의 ready 수렴과 replacement request를 확인한다. | 없음 |

## 실행 증거

- 명령: `timeout 300s ./run_e2e.sh MON-A2`, `MON-A4A`, `MON-A4B`, `MON-D1A`, `MON-D1B`
- 결과: 각 command가 `scenario <ID> passed`와 `monitoring e2e result=passed`를 반환했다.
- 로그: `logs/20260802-111330-88714/` (MON-A2),
  `logs/20260802-111314-86693/` (MON-A4A),
  `logs/20260802-111131-77639/` (MON-A4B),
  `logs/20260802-111204-80225/` (MON-D1A),
  `logs/20260802-111249-83327/` (MON-D1B)

위 focused runner 통과는 해당 scenario의 회귀 증거다. Config 7 전체 aggregate와
`MON-A1`·`MON-A3`·`MON-A5`의 현재 working-tree 재실행은 별도 증거가 필요하다. CA-D77 이전
MON-B1·MON-B2 실행 결과는 제거된 target별 publish 집계를 검증했으므로 현재 계약의 완료 증거로
사용하지 않는다. 표에서 `부분 구현`, `10.0.0 전환 대상` 또는 `11.0.0 전환 대상`으로 남긴
항목은 공통 Config7의 세부 gate를 모두 단언하지 않으므로 완료 증거로 사용하지 않는다.

## 공통 scenario parity gap — 2026-07-29

- `MON-A6`: 공통 scenario는 추가됐지만 Java actual fixture와 runner selector가 없다.
