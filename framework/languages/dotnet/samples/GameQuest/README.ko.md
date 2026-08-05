# GameQuest 샘플

`GameQuest`는 게임 API 역할과 퀘스트 미션 역할을 분리해서 플레이어별 퀘스트
진행을 검증하는 .NET Framework 샘플이다. 클라이언트는 stream session에
연결하고, GameApi는 gameplay action을 검증한 뒤 `PlayerId` 기준 owner route로
`PlayerQuestSpot`에 보낸다. owner spot은 quest event stream을 replay해서 진행을
판정하고, session push로 퀘스트 완료와 진행 상태를 보낸다.

## 실행

Linux 또는 WSL에서 전체 시나리오를 실행한다.

```bash
./run_sample.sh
```

Windows PowerShell에서는 다음 명령을 사용한다.

```powershell
pwsh -NoProfile -ExecutionPolicy Bypass -File .\run_sample.ps1
```

## 구성

- `Shared/`는 클라이언트와 서버가 함께 쓰는 요청, 응답, 알림 계약을 담는다.
- `Client/`는 두 플레이어의 stream connector를 열고 self-check 시나리오를 실행한다.
- `Server/GameApi/`는 플레이어 session과 게임 이벤트를 처리한 뒤 Player ID를 global SpotId로
  사용해 `PlayerQuestSpot`에 직접 메시지를 보낸다. 첫 메시지는
  `InstanceSpot("gamequest.player-quest")`를 명시하며, Framework가 owner node를 선택한다.
- `Server/QuestMission/`은 actor-free Instance Spot factory를 제공하고, quest event stream replay와
  projection 갱신을 담당한다.
- 서버 프로세스들은 별도 registry 서비스 없이 공유 Redis location store에 위치를 등록하고
  자동 연결한다. quest event stream, read model, gameplay fact도 같은 실행의 Redis에 저장한다.
  session binding은 framework actor/session lifecycle이 location store를 통해 관리하며, 샘플이
  별도 binding schema를 만들지 않는다.
- GameApi는 `IZLinkSpotManager.GetOrCreate`나 특정 NodeRid를 사용하지 않는다. 같은 Player ID의
  이후 요청은 Location Store에 공개된 current owner로 전달된다.
- shell/PowerShell runner는 실행마다 전용 Redis Docker 컨테이너를 직접 시작한다. 외부 Redis endpoint
  재사용 mode는 제공하지 않는다. container 이름, host port, Redis key prefix, log directory는
  sample 이름과 실행 id를 포함해 매 실행마다 달라지며 역할별 임시 config 파일에 기록한다.
- `Server/Configuration/`은 역할별 endpoint, channel, packet 설정을 모은다.

## 성공 조건

클라이언트 시나리오는 `JoinSessionReq` 이후 같은 stream으로 첫 사냥, 경매 개방, gameplay action,
진행 push, 멱등성, projection rebuild, owner spot close 뒤 재활성, 두 번째 플레이어 진행 동기화를
검증한다. projection delete/rebuild, owner 메시징 없이 누락된 gameplay fact를 만드는 보정 hook,
server assertion은 검증용 HTTP endpoint로 남긴다. 클라이언트 self-check가 실패하면 runner가 중단된다.
서버 evidence 확인이 끝나면 runner가
`gamequest-server-evidence=completed`를 출력한다.
