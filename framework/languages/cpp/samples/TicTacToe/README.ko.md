# TicTacToe

이 샘플은 공통 TicTacToe 시나리오의 API 역할과 Play 역할을 C++ public API 위에서
보여 준다. runner는 API 실행 파일을 서로 다른 설정으로 두 번, Play 실행 파일을 서로 다른 설정으로
두 번 시작해 2개 API와 2개 Play 프로세스를 구성한다. 별도 Session 서버와 Registry 서버를 두지 않고,
API 서버와 Play 서버가
수동 endpoint로 연결된다. client는 HTTP `POST /games` 응답으로 받은 Play stream
endpoint에 직접 연결한다.

포함 범위는 아래와 같다.

- Play STREAM endpoint 구성
- API ChannelName handler/client 구성
- Play ChannelName handler 구성
- Entry Spot 등록
- actor factory 등록
- authenticate player flow
- handler logger 표면
- ensure player actor flow
- create game flow
- create room flow
- 공식 Redis location store를 통한 room Spot route 저장과 조회
- join game response
- place mark response
- Play session actor bind
- `on_create_actor`, actor join, place mark, player joined, game state 흐름
- room user Spot의 `leave_actor` 호출과 Entry Spot 복귀
- Entry Spot의 `destroy_actor` 호출
- lifecycle self-check는 `destroy_actor` 전후의 Entry Spot leave callback count가 같은지 검증한다
- JSON stream/channel/actor/Spot payload
- disconnect cleanup

샘플 이름에는 별도 접미사를 붙이지 않는다. 이 샘플은 수동 endpoint를 쓰는 직접 Play
연결 기준 샘플이다.

실행 파일은 아래 역할로 나뉜다.

- `sample_cpp_framework_tictactoe_api`: API ChannelName handler와 authenticate handler. runner가 `api-a`, `api-b`로 두 번 시작한다.
- `sample_cpp_framework_tictactoe_play`: Play ChannelName, Spot, actor factory, game flow. runner가 `play-a`, `play-b`로 두 번 시작한다.
- `sample_cpp_framework_tictactoe_client`: `TicTacToeClientScenario` 안에서 수행하는 HTTP `POST /games` 시작 요청과 Stream Connector 기반 client flow

client 실행 파일은 설정을 읽고 `tictactoe_client_scenario_t`를 실행한다. HTTP client `POST /games`
흐름은 `TicTacToeClientScenario` 안에 있다. client scenario는
`zlink::http_client`로 `POST /games`를 호출하고, 응답으로 받은 Play stream endpoint에
Stream Connector로 접속한다. API와 Play 실행
파일은 각각 실제 샘플 서버 역할을 보여 주며, 테스트 전용 fake 서버나 E2E 전용 sample
target은 샘플 트리에 두지 않는다.
script 실행 결과는 full client/server self-check 결과와 actor lifecycle sample gate 결과를
표준 출력으로 보여 준다. actor lifecycle sample gate는 sample source가
Entry Spot에서만 `destroy_actor`를 호출하는지 확인하고, runtime test로 `leave_actor` 후
Entry Spot destroy를 검증한다. runner는 현재 MeshNode Actor vertical과 ActorGateway 회귀를 먼저 실행해
Actor 등록과 session 정리 경로를 확인한다. sample self-check는 destroy 전후의 Entry Spot leave callback
count가 같아 추가 `on_leave_actor`가 없음을 확인하고, destroy 뒤 actor lookup에서 사라지는지와
같은 actor id 재생성이 가능한지도 확인한다. runner는 Play 서버와 API 서버를 별도 process로 계속 실행한 뒤
public client 실행 파일을 실행한다. client scenario는 HTTP `POST /games`, Stream Connector
인증, room join, gameplay notification, game 종료 뒤 같은 actor id 재인증까지 확인한다.

## 실행과 설정

서버 실행 파일은 `--config`로 받은 설정 파일을 `app.config()`로 읽고 `sample.topology`를
`sample_topology_t`에 bind한 뒤 자기 role 만 실행한다. 서버 role 을 계속 실행하려면
설정 파일의 `sample.host.keepRunning` 값을 `true`로 둔다.

Client 실행 파일은 framework app을 만들지 않는다. Client는 API HTTP endpoint만 설정으로
받고, Play stream endpoint는 `POST /games` 응답에서 받아 connector를 만든다. API HTTP endpoint는
검증된 `--api-http-endpoint` CLI option으로 받는다.

Linux 또는 WSL에서는 아래 script 를 실행한다.

```bash
./framework/languages/cpp/samples/TicTacToe/run_sample.sh
```

Windows PowerShell에서는 아래 script 를 실행한다.

```powershell
.\framework\languages\cpp\samples\TicTacToe\run_sample.ps1
```

script 는 CTest sample parity와 actor lifecycle runtime gate를 먼저 실행한다. 그 뒤 Play 서버 두 개와
API 서버 두 개를 별도 process로 실행하고 public client 실행 파일로 full
client/server self-check 를 수행한다.

script 는 전용 Redis Docker container를 loopback port로 시작하고 self-check가 끝나면 정상·실패와
관계없이 정리한다. Play 서버는 이 endpoint를 공식 Redis location store에 전달하고, room Spot의
위치를 저장하고 조회한다. script는 실행마다 고유한 `TICTACTOE_CPP_REDIS_KEY_PREFIX`도 전달하므로
같은 container 안의 room route key가 다른 실행과 섞이지 않는다. 이미 실행 중인 Redis나 host Redis
endpoint를 재사용하는 mode는 제공하지 않는다.
