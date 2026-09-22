[English](README.md) | **한국어**

# ZLink Engine Lobby Unity sample

하나의 `ZLinkClient` MonoBehaviour가 native player와 WebGL player에서 같은 코드로 Engine Lobby
server에 연결한다. 시작할 때 `Join`과 `Chat`을 보내고, `Update`에서 connector를 pump하며,
`ChatNotify`를 받으면 화면 중앙의 UI text를 갱신한다.

## Unity version

이 project는 **Unity 6 LTS 6000.0.83f1**로 고정한다. WebGL UPM package의 manifest 최소값은
Unity 2021.3이지만, 저장소의 실제 WebGL adapter 검증 project도 6000.0.83f1을 사용하므로 sample과
검증 기준을 하나로 맞췄다.

## Package 설치

`Packages/manifest.json`은 다음 package를 고정한다.

- NuGetForUnity `v4.5.0`
- `com.zlink.stream-connector.webgl` `framework-node/v0.22.0`
- Unity UI `2.0.0`

`Assets/packages.config`는 native Editor와 desktop/mobile/console player가 사용하는
`Zlink.Stream.Connector` `0.22.0`을 선언한다. 처음 열 때 package가 아직 없어 compile error가
보이면 Safe Mode로 들어가지 말고 **Ignore**를 선택한 다음 **NuGet → Restore Packages**를 실행한다.

두 connector assembly는 같은 public type을 정의하므로 build target마다 하나만 포함해야 한다.

1. Project 창에서 NuGetForUnity가 받은 `Systems.Zlink.Stream.Connector.dll`을 선택한다.
2. Plugin Inspector에서 **WebGL** 호환을 해제하고 Editor와 필요한 native platform만 켠다.
3. Native connector를 DLL이 아니라 source asmdef로 가져왔다면 그 asmdef에
   `"excludePlatforms": ["WebGL"]`을 둔다.
4. WebGL UPM package의 asmdef는 이미 `"includePlatforms": ["WebGL"]`이므로 수정하지 않는다.

## Scene 실행

1. `../Server`에서 server를 build하고 실행한다.

   ```bash
   ./run_sample.sh build
   ./run_sample.sh run
   cat .run/stream.port
   ```

2. Unity에서 `Assets/Scenes/EngineLobby.unity`를 연다.
3. `ZLinkClient` GameObject의 **Endpoint**를 출력된 port에 맞춰
   `ws://127.0.0.1:<port>`로 바꾼다.
4. Play를 누른다. 화면은 `joined as ...`를 거쳐 `unity-player: hello from Unity`로 바뀐다.
5. 종료한 뒤 server 디렉터리에서 `./run_sample.sh stop`을 실행한다.

`ZLinkClient.cs`의 `connect`, `pump`, `handler`, `lifecycle` snippet marker는 engine 통합 가이드가
그대로 읽는 source 경계다.

## Native player build

1. **File → Build Profiles**에서 Windows, macOS, Linux, Android 또는 iOS native profile을 고른다.
2. `Systems.Zlink.Stream.Connector.dll`이 선택한 platform과 호환되고 WebGL과는 호환되지 않는지
   Plugin Inspector에서 다시 확인한다.
3. `EngineLobby` scene 하나를 build한다.
4. Server endpoint에 접근할 수 있는 환경에서 player를 실행해 UI text가 chat notification으로
   바뀌는지 확인한다.

## WebGL build

1. **File → Build Profiles → Web**을 선택하고 **Switch Platform**을 누른다.
2. Native `Systems.Zlink.Stream.Connector.dll`이 WebGL에서 제외되고
   `Systems.Zlink.Stream.Connector.WebGL` assembly가 포함되는지 확인한다.
3. `EngineLobby` scene을 `Build/`에 build한다.
4. HTTP server로 build 결과를 제공하고 browser에서 연다. `file://`로 열지 않는다.
5. UI text가 `ChatNotify` 값으로 바뀌는지 확인한다. 이 결과는 IL2CPP reverse callback,
   UPM import, jslib 연결과 main-thread pump를 함께 검증한다.

## 이 머신에서 확인하지 못한 것

이 workspace에는 Unity Editor와 license가 없다. 따라서 project source, package pin, scene reference,
snippet marker와 native/WebGL platform 분리 절차만 정적으로 확인했다. Editor compile, native player
build와 WebGL IL2CPP build는 Unity license가 있는 runner에서 실행해야 한다.
