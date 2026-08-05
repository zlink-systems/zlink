# Java GameQuest Sample Porting Inventory

계약 기준은 `framework/doc/framework/common/sample/event/gamequest.ko.md`와
`framework/doc/framework/common/sample/README.ko.md`다. `.NET` 구현은 언어별 동작 차이를
찾기 위한 비교 자료로만 사용하며, Java 완료 여부는 공통 계약과 Java runner 결과로 판단한다.

## 상태

| 영역 | Java 포트 | 비고 |
|------|-----------|------|
| Gradle wiring | 완료 | `settings.gradle.kts`, samples aggregate, standalone settings에 등록 |
| Shared contracts | 완료 | 공통 GameQuest 계약의 메시지 이름과 주요 필드를 유지한다. |
| GameApi 역할 | 구현 완료, runtime 확인 대기 | stream session이 `PlayerId` Session Actor를 만들고 Entry Spot에 배치한 뒤 연결에 bind한다. Gameplay event는 one-way로 owner Spot에 보내며, 처리 결과는 direct Actor send와 bound session으로 push한다. |
| QuestMission 역할 | 구현 완료, runtime 확인 대기 | 두 instance가 같은 RouteMesh에 참여하고 `PlayerId`를 전역 `SpotId`로 사용하는 Instance Spot을 제공한다. API별 역방향 ClientServer Channel은 사용하지 않는다. |
| GameplayStateStore | 완료 | API 노드가 Redis에 누적 gameplay fact를 기록하고 QuestMission의 sync가 같은 fact를 읽어 보정한다. 동일 event id는 한 번만 반영한다. |
| Client scenario | 완료 | join과 progress push를 검증하고, quest 완료 전에 같은 idempotency key를 재전송해 진행도가 증가하지 않는지 확인한다. Alice가 api-a 연결을 종료한 뒤 api-b로 다시 연결해 projection과 새 push를 검증하며, delete/rebuild, sync, server assertion도 실행한다. scale-out 단계는 두 player request/push를 동시에 in-flight로 만들고, 별도 rehydrate 단계는 owner process 재기동 뒤 정상 channel 조회로 복원 상태를 확인한다. |
| Runner | 차단 | topology는 Ready가 되지만 첫 remote Instance Spot 활성화가 `remote Instance Spot request was rejected`로 끝난다. compile과 runner topology 검증은 통과하며 runtime activation gap을 별도로 추적한다. |

## 남은 확인 사항

Session Actor·Entry Spot·direct Actor notify의 정적 계약과 compile gap은 닫혔다. 실제 process gate는
remote Instance Spot 활성화 거절이 해결된 뒤 다시 확인한다.

## 검증

- `./gradlew classes` 통과.
- `run_sample.sh`는 `topology=ready` 뒤 첫 `JoinSessionReq`에서 remote Instance Spot 활성화가 거절된다.
- 같은 runner가 mission-a를 종료·재기동한 뒤 `gamequest startup replay restored player-alice`를 확인한다.
- 재기동 뒤 두 번째 client가 정상 channel 조회로 `gamequest-rehydrate=completed`를 확인한다.
- runner는 `surface=SPOT`의 `GameplayMsg` flow를 확인해 channel-only 구현이 다시 들어오지 못하게 한다.
- runner는 `GameplayMsg`가 `SPOT_ROUTE`의 `SEND`이고 결과가 `PlayerId` Actor로 전달되는지 확인한다.
- runner는 두 scale-out player가 각각 mission-a와 mission-b owner Spot에서 처리되고 `gamequest-scale-out=completed`가 출력되는지 확인한다.
