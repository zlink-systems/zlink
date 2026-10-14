# MyGame agent guidelines

## 책임

| node            | 소유 책임                                                                 |
| --------------- | ------------------------------------------------------------------------- |
| Session         | STREAM 종단, 첫 Join identity, Actor GetOrCreate·bind, relay, SessionInfo |
| Table           | Entry Spot, Table User Spot, Player Actor host                            |
| Table User Spot | 좌석·칩·덱·손패·차례·판 번호·판돈·결산                                    |
| Player Actor    | 공개 Actor identity와 bound Session; 게임 상태 사본 없음                  |
| Redis           | Framework Location·Relocation store                                       |

## 확장

게임 규칙은 `server/table/domain/table-state.ts`에서 바꾼다. 공개 callback과 I/O는 infrastructure가 소유한다. DTO는 `server/shared/messages.ts`, stable type과 packet 이름은 `server/shared/constants.ts`에서 관리한다. `server/session/main.ts`와 `server/table/main.ts`가 각 역할의 builder와 provider를 구성한다. `server/shared/hosting.ts`는 Location·Relocation store, diagnostics, READY·drain·shutdown 제어를 소유한다.

## 공개 API 경계

Framework와 binding의 공개 package만 사용한다. route·lease·membership·queue·binding 상태는 Framework가 소유하며 application snapshot에 넣지 않는다. Table은 SpotWide execution gate를 사용한다. 추가 lock·queue·retry·sleep으로 relocation이나 lifecycle 실패를 보상하지 않는다. 메시지는 Framework 기본 typed JSON serializer를 사용한다.

Table의 snapshot은 application 상태만 담는다. Player relocation adapter는 application 상태가 없는 빈 payload를 사용한다. 다른 node로 이동할 때 Framework가 Actor ID·generation·membership·binding을 보존한다.

## 검증

`npm ci && npm run build`, `npm run typecheck`, `npm run lint`, `npm run format:check`를 실행한다. Windows `run-e2e.ps1`과 Linux `run-e2e.sh`가 실제 structure 검사와 공개 connector e2e를 수행한다. 검증은 전체 e2e 1회와 `table,reconnect` 선택 2회를 실행한다. 최초 실패와 message flow를 보존하며 재실행 통과로 간헐 실패를 무시하지 않는다.

Framework 결함은 이 템플릿에서 수정하거나 우회하지 않는다. 사용법을 공개 guide와 sample에 대조한 뒤 재현·첫 실패 log·관련 source 위치를 보고한다. 정리는 이 runner가 소유한 process와 Compose project로 제한한다.

코드 주석은 영어, 보고는 격식 있는 기술 문체 한국어로 작성한다.

Actor identity는 Session binding의 공개 ref에서 SessionInfo로 답한다. Table은 게임 상태만 소유한다. 요청 응답은 받은 Actor의 bound Session으로 직접 보내며, 상태 push는 좌석 playerId로 Actor에게 전송한다. TableView·HandDealt·HandResult는 공개 등록 3개와 전달 handler 구현 하나를 사용한다. HandDealt는 판 시작과 재접속·TableViewRequest 응답에만 전송한다.
