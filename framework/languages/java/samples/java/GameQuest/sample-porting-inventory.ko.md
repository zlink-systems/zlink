# Java GameQuest Sample Porting Inventory

계약 기준은 `framework/doc/framework/common/sample/event/gamequest.ko.md`와
`framework/doc/framework/common/sample/README.ko.md`다. `.NET` 구현은 언어별 동작 차이를
찾기 위한 비교 자료로만 사용하며, Java 완료 여부는 공통 계약과 Java runner 결과로 판단한다.

## 상태

| 영역 | Java 포트 | 비고 |
|------|-----------|------|
| Gradle wiring | 완료 | `settings.gradle.kts`, samples aggregate, standalone settings에 등록 |
| Shared contracts | 완료 | 공통 GameQuest 계약의 메시지 이름과 주요 필드를 유지한다. |
| GameApi 역할 | 구현 완료, runtime 확인 완료 | stream session이 `PlayerId` Session Actor를 만들고 Entry Spot에 배치한 뒤 연결에 bind한다. Gameplay event는 one-way로 owner Spot에 보내며, 처리 결과는 direct Actor send와 bound session으로 push한다. |
| QuestMission 역할 | 구현 완료, runtime 확인 완료 | 두 instance가 같은 RouteMesh에 참여하고 `PlayerId`를 전역 `SpotId`로 사용하는 Instance Spot을 제공한다. API별 역방향 ClientServer Channel은 사용하지 않는다. |
| GameplayStateStore | 완료 | API 노드가 Redis에 누적 gameplay fact를 기록하고 QuestMission의 sync가 같은 fact를 읽어 보정한다. 동일 event id는 한 번만 반영한다. |
| Client scenario | 완료 | join과 progress push를 검증하고, quest 완료 전에 같은 idempotency key를 재전송해 진행도가 증가하지 않는지 확인한다. Alice가 api-a 연결을 종료한 뒤 api-b로 다시 연결해 projection과 새 push를 검증하며, delete/rebuild, sync, server assertion도 실행한다. owner close 뒤 새 Instance intent는 replay를 확인하고, owner 종료 stage의 다음 `KillMonsterReq`는 `Unavailable`을 확인한다. |
| Runner | 완료 | 실제 process runner가 sample-owned readiness, node별 route/processing evidence, reconcile, close 뒤 replay, Ready owner 종료 뒤 `Unavailable`과 replacement 부재를 확인한다. |

## 검증 결과

Session Actor·Entry Spot·direct Actor notify의 정적 계약과 compile gap을 닫았고, 실제 process gate도
통과했다. remote Instance Spot 활성화는 두 QuestMission process가 공유하는 RouteMesh에서 성공한다.

## 검증

- `:Server:GameApi:build :Server:QuestMission:build :Client:build` 통과.
- runner는 `gamequest-ready`, `gamequest-api event-routed`, `gamequest-mission processed`,
  `gamequest-mission reconciled`, `gamequest-mission replayed`,
  `gamequest-owner unavailable`와 replacement 0회를 확인한다.
- runner는 full client의 `gamequest=completed`와
  `gamequest-server-evidence=completed`를 각각 직접 확인한다.
