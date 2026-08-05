# Bingo

이 샘플은 공통 Bingo 시나리오의 Session, Api, Play 역할 구분을 C++ public
API 위에서 보여 준다. client는 Session stream 하나에 연결하고, Play 서버는
`Domain`, `Application`, `Infrastructure/ZLink` 구조로 게임 규칙과 framework 연결 코드를
나누어 둔다.

포함 범위는 아래와 같다.

- sample topology와 endpoint 이름
- 공유 location store 기반 자동 연결
- API ChannelName handler/client 구성
- Play ChannelName handler 구성
- session stream endpoint 구성
- authenticate player/session handler
- Entry Spot의 Actor 생성 승인과 player 초기화 callback
- match API handler와 actor match handler
- allocate room, join room, card submit handler
- bingo room state, 3 x 3 card, server draw, winner 판단
- Stream Connector public wait helper 기반 client push 검증
- Protobuf codec extension을 사용하는 stream/channel/actor/Spot payload
- channel request/reply handler
- handler logger와 callback log sink
- STREAM packet relay
- publish/subscribe 구성과 일반 event publish
- callback submit
- coroutine submit
- user Spot 생성과 room owner MeshNode 구성
- `on_create_actor`, room join, room leave callback 흐름
- room user Spot의 `leave_actor` 호출과 Entry Spot 복귀
- Entry Spot의 `destroy_actor` 호출
- lifecycle self-check는 `destroy_actor` 전후의 Entry Spot leave callback count가 같은지 검증한다
- SPOT timer 등록
- monitoring source 등록
- offload handler option

이 샘플은 공유 location store 기반 자동 연결과 session gateway 흐름을 맡는다. 수동 endpoint로
Play stream에 직접 연결하는 흐름은 TicTacToe 샘플이 맡는다.

실행 파일은 아래 역할로 나뉜다.

- `sample_cpp_framework_bingo_api`: API channel과 authenticate handler
- `sample_cpp_framework_bingo_play`: play channel, room domain, room handlers, publish, Spot timer
- `sample_cpp_framework_bingo_session`: STREAM endpoint와 session packet dispatch
- `sample_cpp_framework_bingo_client`: Stream Connector 기반 client flow와 push payload self-check

client scenario 실행 파일은 Stream Connector public API로 request reply와 push notification을
검증하는 시나리오를 담고 있다. Stream, channel, actor, Spot payload는 C++ framework의
Protobuf codec extension으로 등록된 typed message를 사용한다. 서버 실행 파일들은 API, Play, Session 역할을
각각 보여 주며, 테스트 전용 fake 서버나 E2E 전용 sample target은 샘플 트리에 두지 않는다.
script 실행 결과는 full client/server self-check 결과와 actor lifecycle sample gate 결과를
표준 출력으로 보여 준다. actor lifecycle sample gate는 sample source가
Entry Spot에서만 `destroy_actor`를 호출하는지 확인하고, runtime test로 `leave_actor` 후
Entry Spot destroy를 검증한다. runner는 현재 MeshNode Actor vertical과 ActorGateway 회귀를 먼저 실행해
Actor 등록과 session 정리 경로를 확인한다. sample self-check는 destroy 전후의 Entry Spot leave callback
count가 같아 추가 `on_leave_actor`가 없음을 확인하고, destroy 뒤 actor lookup에서 사라지는지와
같은 actor id 재생성이 가능한지도 확인한다. runner는 API, Play, Session 서버를 별도 process로 계속
실행한 뒤 public client 실행 파일로 authenticate, match, card submit, server draw, winner
판단 흐름을 검증한다.

## 실행과 설정

서버 실행 파일은 `--config`로 받은 설정 파일을 `app.config()`로 읽고 `sample.topology`를
`sample_topology_t`에 bind한 뒤 자기 role 만 실행한다. API, Play, Session 서버는
모두 별도 process 로 실행한다. 서버 role 을 계속 실행하려면 설정 파일의
`sample.host.keepRunning` 값을 `true`로 둔다.

Client 실행 파일은 framework app을 만들지 않는다. Client는 stream connector만 사용하며,
접속 endpoint는 검증된 `--stream-endpoint` CLI option으로 받는다.

Linux 또는 WSL에서는 아래 script 를 실행한다.

```bash
./framework/languages/cpp/samples/Bingo/run_sample.sh
```

Windows PowerShell에서는 아래 script 를 실행한다.

```powershell
.\framework\languages\cpp\samples\Bingo\run_sample.ps1
```

script 는 CTest sample parity와 actor lifecycle runtime gate를 먼저 실행한다. 그 다음
API, Play, Session 서버 실행 파일을 계속 실행 모드로 시작하고 public client
실행 파일로 full client/server self-check 를 수행한다.

script 는 전용 Redis Docker container를 Docker가 배정한 loopback port로 시작하고 self-check 가
끝나면 정상/실패와 관계없이 그 container를 정리한다. 따라서 room 할당 상태는 격리되어 개발자의
로컬 Redis를 건드리지 않는다. script 는 실행마다 고유한 `BINGO_REDIS_KEY_PREFIX`도 전달하므로
같은 Redis를 쓰는 다른 테스트의 match queue key와 섞이지 않는다.
