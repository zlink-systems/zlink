[English](README.md) | **한국어**

# ZLink Engine Lobby Cocos Creator sample

Cocos Creator web client가 공용 Engine Lobby server에 연결한다. `EngineLobbyClient`는
`PingReq` → `PingRes` → `JoinReq` → `JoinRes` → `ChatMsg` 흐름을 실행하고 `ChatNotify`를 받는다.
`EngineLobbyBehaviour`는 Cocos의 `update()`에서 connector callback을 pump하고 Label에 상태를 표시한다.
Packet 이름과 JSON field는 [공통 engine-lobby 계약](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.ko.md)을 따른다.

## 대상과 transport

이 project의 실행 대상은 **Cocos Creator 3.8 web**이다. `@zlink-systems/stream-connector`
`0.22.0`의 package root를 사용하며 endpoint는 `ws://` 또는 `wss://`이다. 브라우저의
`WebSocket`이 연결과 프레이밍을 맡고 connector는 기본 제공 `zlinkStreamJsonCodec`으로 typed
payload를 처리한다. `Manual` dispatch를 사용하므로 Cocos `update()`가 callback을 실행한다.

TypeScript connector의 정식 제품 실행 환경은 브라우저 계열이다. Cocos Creator native의
`WebSocket` API가 존재하더라도 이 package를 native 경로로 배정한 계약은 없다. 공통 connector
스펙은 Cocos native를 **C++ Axmol adapter**에 배정하지만, 그 adapter는 Cocos Creator native
project에 직접 설치하는 패키지가 아니다. 따라서 이 project의 native build와 player 실행은
지원하거나 검증했다고 주장하지 않는다. native 대응에는 별도 Creator adapter 계약과 구현이 필요하다.

## 내려받기와 설치

Node.js 22와 npm이 필요하다. Cocos Creator 3.8 Editor는 scene 실행에 필요하다.

```bash
npm install
```

## 타입 검사

Editor 없이 connector package 선언과 엔진 독립 client class를 검사한다.

```bash
npm run typecheck
```

이 명령은 `EngineLobbyClient.ts`만 컴파일한다. `EngineLobbyBehaviour.ts`의 `cc` import와
scene serialization은 Cocos Creator Editor에서 확인해야 한다.

## Server 실행

Monorepo에서는 `../Server`, mirror에서는 `zlink-engine-server`를 준비한다. Server README의
install, build, run 단계를 실행하고 `./run_sample.sh run`이 준비되면 `.run/stream.port`를 읽는다.
Docker와 .NET 8 SDK가 필요하다.

## Scene 실행

1. Cocos Creator 3.8에서 이 디렉터리를 project로 연다.
2. `assets/EngineLobby.scene`을 연다. Canvas에 연결된 `EngineLobbyBehaviour`가 상태 Label을 생성한다.
3. Canvas component의 **Endpoint**를 `ws://127.0.0.1:<stream.port>`로 지정하고 browser preview를 실행한다.
4. Label이 `joined as cocos-player (...)`를 거쳐 `cocos-player: hello from Cocos Creator`로
   바뀌는지 확인한다. Server 종료는 Server README의 stop 명령을 사용한다.

Browser에서 server가 다른 host에 있으면 endpoint를 browser에서 접근할 수 있는 주소로 바꾼다.
HTTPS 페이지에는 유효한 인증서가 있는 `wss://` endpoint를 사용한다.

## Headless 검증

Server가 준비된 뒤 Node.js 22의 `WebSocket`을 browser transport 대역으로 사용해 같은
client class 두 개를 실행한다. 이 실행은 Node를 제품 대상이라고 주장하지 않는다.

```bash
ENGINE_LOBBY_ENDPOINT="ws://127.0.0.1:<stream.port>" npm run probe
```

Probe는 Alice와 Bob의 서로 다른 `actorId`, Ping/Join reply, 두 client의 동일한
`ChatNotify(actorId, name, text)`를 확인한 뒤 `cocos-engine-lobby-probe=ok`를 출력한다.

## Cocos Creator 3.8.8 검증 결과

포함된 scene을 로컬에서 다시 빌드한 connector에 연결해 Cocos Creator 3.8.8 CLI로
web-desktop에 빌드했다. 종료 코드 36은
[공식 CLI 문서](https://docs.cocos.com/creator/3.8/manual/zh/editor/publish/publish-in-command-line.html)에
정의된 빌드 성공 코드다. Headless Chromium에서 `JoinRes`를 수신하고 `ChatNotify` handler가
`{"actorId":"00000004","name":"cocos-player","text":"hello from Cocos Creator"}`를
출력했다. Server에도 client 연결이 기록됐다. Connector는 Cocos 빌드 변환 환경에서도
`Array.from(...)`으로 iterable callback 집합의 snapshot을 생성한다.
게시된 `0.23.0` package에는 이 수정이 아직 반영되지 않았다.
