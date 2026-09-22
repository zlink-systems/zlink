[English](README.md) | **한국어**

# ZLink Engine Lobby Unreal sample

Unreal Engine 5 C++ project가 Unity sample과 같은 Engine Lobby server에 연결한다. 하나의
`AZLinkClientActor`가 C++ stream connector를 소유하고, `Tick`에서 connector를 pump하며,
`Ping` → `Join` → `Chat` 순서와 `ChatNotify` 수신 결과를 화면과 log에 표시한다.

Packet 이름과 JSON field의 소유 문서는
[`engine-lobby` sample contract](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.ko.md)다.
이 project는 엔진 전용 alias나 field를 추가하지 않는다.

## Unreal과 connector version

- Unreal Engine 5.4
- C++20
- `framework/languages/cpp/connector/engines/unreal`의 `ZLinkStreamConnector` plugin

이 workspace에는 Unreal Editor가 없으므로 project descriptor는 5.4로 고정하지만 전체 Editor build는
실행하지 않았다. 다른 Unreal Engine 5 minor version으로 올릴 때는 project conversion과 plugin build를
그 version의 Unreal Build Tool로 함께 확인한다.

## Connector 설치

Unreal project를 열기 전에 `Plugins/ZLinkStreamConnector/`에 connector plugin과 현재 target에 맞는
native package를 준비한다.

Monorepo checkout에서는 다음처럼 plugin source를 project 아래에 복사한다.

```powershell
New-Item -ItemType Directory -Force Plugins | Out-Null
Copy-Item -Recurse -Force ../../cpp/connector/engines/unreal Plugins/ZLinkStreamConnector
```

그 뒤 plugin의 `ThirdParty/README.md` 절차로 C++ connector를
`Plugins/ZLinkStreamConnector/ThirdParty/ZLink/`에 package한다. Unreal Build Tool을 실행할 때
`ZLINK_UNREAL_COMPILER_ID`와 `ZLINK_UNREAL_COMPILER_VERSION`은 manifest를 만든 toolchain과 같아야
한다. Export mirror에서는 같은 version의 plugin source/package를 그 경로에 배치한다.

## Server 실행

같은 checkout의 `../Server`에서 공용 server를 실행한다.

```bash
./run_sample.sh build
./run_sample.sh run
cat .run/stream.port
```

출력된 port가 기본값 `22700`과 다르면 `EngineLobbyClientActor.h`의 `Endpoint` 기본값을 바꾼다.

## Project 실행

1. `EngineLobby.uproject`를 Unreal Editor 5.4로 열어 C++ module을 build한다.
2. 빈 level을 열고 **Play**를 누른다. `EngineLobbyGameMode`가 `AZLinkClientActor` 하나를 spawn한다.
3. 화면이 `joined as unreal-player (...)`를 거쳐 `unreal-player: hello from Unreal`로 바뀌는지
   확인한다.
4. Output Log에서도 같은 상태를 확인한다.
5. Play를 끝내면 Actor가 delegate를 해제하고 connector를 닫는다.
6. Server directory에서 `./run_sample.sh stop`을 실행한다.

`EngineLobbyClientActor.cpp`의 `connect`, `pump`, `handler`, `lifecycle` marker는 engine 통합 가이드가
읽는 source 경계다.

## 최소 source 검증

Unreal Editor가 없는 환경에서는 sample의 순수 packet-name header, C++ connector public header와
Unreal adapter public header가 한 translation unit에서 함께 compile되는지 확인한다.

```bash
g++-13 -std=c++20 -fsyntax-only \
  -I Source/EngineLobby/Public \
  -I ../../cpp/connector/core/include \
  -I ../../cpp/connector/engines/unreal/Source/ZLinkStreamConnector/Public \
  -I ../../../../bindings/cpp/include \
  Validation/ProtocolCompileCheck.cpp
```

이 검사는 contract packet 이름을 사용하는 `packet_t`, manual dispatch option과 Unreal adapter type이
같은 C++20 compile surface에서 유효함을 확인한다. Unreal reflection, generated header, module link,
Editor 실행과 실제 server 통신은 Unreal Editor/Build Tool이 있는 runner에서 확인해야 한다.
