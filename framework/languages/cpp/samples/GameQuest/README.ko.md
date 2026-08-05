# GameQuest C++ sample

GameQuest 샘플은 게임 플레이 이벤트를 GameApi stream endpoint로 받고, player id 기준 owner인
QuestMission role에 player quest Spot을 만든다. GameApi는 공개 spot route request로 해당 player
Spot에 progress sync와 gameplay event 적용을 보낸다. QuestMission은 player Spot 안에서 quest
projection을 갱신하고, GameApi는 현재 연결된 stream session에 progress/completion notify를 돌려준다.

runner는 Redis location store를 공유 location store로 사용한다. 별도 registry process를 두지
않고, 두 GameApi role과 두 QuestMission role이 같은 Redis prefix 아래에서 owner channel과 spot route
위치를 찾는다.

## 실행

```bash
./framework/languages/cpp/samples/GameQuest/run_sample.sh
```

runner는 실행에 필요한 Redis endpoint와 실행별 key prefix를 준비하고, server role이 준비된 뒤 client
scenario를 시작한다. 호출자는 별도 registry process나 수동 peer endpoint를 설정하지 않는다.

`run_sample.sh`가 `PASS GameQuest.Cpp`와 `gamequest sample result=passed`를 출력하면 C++ sample의
빌드, role 실행, client scenario, server evidence, flow trace가 함께 통과한 것이다.
