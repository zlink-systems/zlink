[English](README.md) | **한국어**

# ZLink Engine Lobby Godot C++ 샘플

Godot 4.4 GDExtension scene이 기존 C++ Godot stream connector adapter를 사용한다.
`EngineLobbyNode`는 `_process`에서 `dispatch()`를 호출하므로 callback이 Godot main thread에서
`Status` Label을 갱신한다. 의도한 흐름은 `PingReq` → `PingRes` → `JoinReq` → `JoinRes` →
`ChatMsg` → `ChatNotify`다. Packet 이름과 JSON field는 [Engine Lobby 공통 계약](https://github.com/zlink-systems/zlink/blob/main/framework/doc/framework/common/sample/engine-lobby/README.ko.md)을 따른다.
Node는 연결 전에 `ChatNotify` push를 구독한다.

## 의존성과 빌드

- Godot 4.4 editor 및 같은 버전의 `godot-cpp` 4.4 source와 header
- C++20 compiler, CMake 및 C++ connector의 package 의존성
- `framework/languages/cpp`를 포함하는 이 monorepo checkout

Linux에서 editor와 `godot-cpp`의 minor version을 맞춰 빌드한다.

```bash
cmake -S . -B build \
  -DGODOT_CPP_ROOT=/path/to/godot-cpp \
  -DZLINK_FRAMEWORK_CPP_ROOT=/path/to/zlink/framework/languages/cpp \
  -DZLINK_FRAMEWORK_CPP_BUILD_TESTS=OFF \
  -DZLINK_FRAMEWORK_CPP_BUILD_CROSS_LANGUAGE=OFF
cmake --build build --target engine_lobby_godot -j8
```

생성된 공유 library는 `bin/`에 놓이고 `engine_lobby.gdextension`이 로드한다. 현재 project는
Linux x86_64 library를 선언한다. 다른 platform에는 대응하는 library 항목과 빌드가 필요하다.
C++ connector와 WebSocket transport는 같은 빌드에서 source로 link한다.

## 실행

1. [`../../Server`](../../Server)를 실행하고 `.run/stream.port`를 읽는다. Docker가 있으면
   `run_sample.sh`가 Redis를 준비한다.
2. Godot 4.4에서 `project.godot`를 연다. 할당된 port가 22700과 다르면 `EngineLobbyNode`의
   `endpoint`를 `ws://127.0.0.1:<stream.port>`로 바꾼다.
3. Label이 joined 상태에서 `godot-player: hello from Godot C++`로 바뀌는지 확인한다.
4. runner로 시작한 server는 `./run_sample.sh stop`으로 종료한다.

## 현재 검증 범위와 차단 요인

C++ node와 GDExtension 등록 translation unit은 실제 Godot adapter header와 Godot C++ API의
얇은 대역을 사용해 GCC 13에서 compile했다. `dispatch()` 철자를 일부러 잘못 쓰면 이 검사가
실패한다. 이 머신에는 Godot와 `godot-cpp`가 없어 editor build, scene 실행, 실제 Label 갱신은
확인하지 못했다.

Node는 연결 전에 `ChatNotify`를 구독한다. Adapter는 이 push를 등록된 packet callback으로
전달한다.

[`../csharp`](../csharp)의 C# variant는 WSL의 실제 공용 server에 연결해 두 client의 Ping,
Join, 양쪽 ChatNotify payload를 확인했다. 사용하지 않는 port에 연결한 검사는 예상대로
실패했다. 이 검증은 C++ adapter나 Godot scene 실행을 포함하지 않는다.
