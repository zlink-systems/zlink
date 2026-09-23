[English](README.md) | **한국어**

# ZLink Engine Lobby Axmol sample

Axmol 2.3.0 C++ client가 공용 Engine Lobby server에 연결한다. `EngineLobbyScene`은 connector를
소유하고 scheduler update에서 `dispatch()`를 호출한다. `PingReq` → `PingRes` → `JoinReq` →
`JoinRes` → `ChatMsg` → `ChatNotify` 결과를 화면 중앙과 log에 표시한다.
연결 전에 Engine Lobby push인 `ChatNotify`를 구독한다.

Packet 이름과 JSON field는 [engine-lobby sample contract](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.ko.md)를 따른다.

## 준비

- Axmol 2.3.0 source tree (`AX_ROOT`)
- C++20 compiler와 CMake 3.20 이상
- ZLink C++ framework package 0.22.0 (`zlink_framework` CMake config)
- `nlohmann_json` CMake package
- `framework/languages/cpp/connector/engines/axmol`의 connector source

Monorepo에서는 connector source를 `third_party/zlink_axmol_connector/`로 복사한다. Export
mirror에서도 같은 version의 connector source를 그 위치에 배치한다. CMake는 공개
`zlink::stream_connector` target에 연결한다.

```bash
mkdir -p third_party/zlink_axmol_connector
cp -a ../../cpp/connector/engines/axmol/. third_party/zlink_axmol_connector/
```

## Server와 client 실행

같은 checkout의 `../Server`에서 공용 server를 시작한다.

```bash
cd ../Server
./run_sample.sh build
./run_sample.sh run
cat .run/stream.port
```

기본 endpoint는 `Source/EngineLobbyScene.cpp`의 `ws://127.0.0.1:22700`이다. 출력된 port가
다르면 그 상수를 바꾼다. Axmol과 ZLink package가 준비된 Linux 환경에서는 다음처럼 build한다.

```bash
cd ../Axmol
cmake -S . -B build -DCMAKE_PREFIX_PATH=/path/to/zlink/package
cmake --build build --parallel 8
./build/EngineLobby
```

화면과 log에 `joined as axmol-player (...)` 뒤 `axmol-player: hello from Axmol`이 표시되는지
확인한다. 종료 후 `../Server/run_sample.sh stop`을 실행한다. `connect`, `pump`, `handler`,
`lifecycle` marker는 통합 가이드에서 발췌할 source 경계다.

## 최소 source 검증

Axmol이 없는 환경에서는 `Validation/axmol.h`가 화면과 scheduler 타입만 대신한다. 다음
명령은 실제 client source, C++ connector의 공개 Axmol header와 JSON 호출을 한 translation
unit으로 compile한다.

```bash
g++-13 -std=c++20 -Wall -Wextra -Werror -fsyntax-only \
  -I Validation -I Source \
  -I ../../cpp/connector/engines/axmol/include \
  Validation/ProtocolCompileCheck.cpp
```

이 검사는 Axmol engine link, 실제 scheduler 실행, connector link와 server 통신을 검증하지
않는다. 이 workspace에는 Axmol이 없어 engine build와 실행을 하지 않았다.

Scene은 연결 전에 `ChatNotify`를 구독하고 adapter callback으로 전달된 push를 표시한다.
