# Unity WebGL

[← 목차](INDEX.ko.md) | [이전: 브라우저](02-browser.ko.md)

---

Unity WebGL 빌드는 브라우저 샌드박스에서 실행되므로 OS 소켓을 열 수 없고 `.NET` connector를
사용할 수 없다. `com.zlink.stream-connector.webgl` UPM 패키지가 이 TypeScript connector의 browser
bundle을 담고, Unity가 요구하는 jslib·C# 호출 경계를 제공한다. **별도 wire runtime은 없다** —
브라우저 client와 같은 protocol·codec을 사용한다.

정식 계약은 [Stream Connector 공통 스펙](../../../common/spec/stream-connector/32-stream-connector.ko.md)과
[TypeScript 공개 계약](../../../common/spec/stream-connector/languages/typescript/03-stream-connector.ko.md)이
소유한다. 이 챕터는 사용법을 다룬다.

## 어떤 connector를 사용하는가

| 빌드 대상 | connector |
|---|---|
| Editor, 데스크톱·모바일·콘솔 player | NuGet `Zlink.Stream.Connector` |
| WebGL player | UPM `com.zlink.stream-connector.webgl` |

C# 표면은 양쪽이 같다. 네임스페이스 `Systems.Zlink.Stream.Connector.Contracts`, 진입점
`ZlinkStreamConnectorFactory.Create(options)`, 반환 타입 `IZlinkStreamConnector`가 동일하므로 게임
코드는 빌드 대상에 따라 갈리지 않는다.

## 설치

Package Manager → **Add package from git URL**:

```
https://github.com/zlink-systems/zlink.git?path=framework/languages/unity/com.zlink.stream-connector.webgl#framework-node/v0.16.0
```

태그를 반드시 고정한다. `framework-node/v<version>`은 패키지에 담긴 browser bundle이 나온
`@zlink-systems/stream-connector` 릴리스의 태그이므로, 어댑터와 bundle이 같이 움직인다.

오프라인 프로젝트는 같은 릴리스의 `com.zlink.stream-connector.webgl-<version>.tgz`를 받아
**Add package from tarball**로 설치한다.

## 두 어셈블리를 함께 두기

두 패키지는 같은 타입 이름을 정의한다. 빌드 대상마다 하나만 컴파일되게 한다.

- UPM 패키지의 asmdef는 `"includePlatforms": ["WebGL"]`이다.
- NuGet으로 받은 `Systems.Zlink.Stream.Connector.dll`은 Plugin Inspector에서 WebGL 플랫폼
  체크를 해제한다. 소스로 사용한다면 그 asmdef에 `"excludePlatforms": ["WebGL"]`을 넣는다.

## 연결과 pump

dispatch 기본값은 `Manual`이고 WebGL은 단일 스레드다. 게임이 main thread에서 pump하지 않으면
아무것도 진행되지 않는다.

```csharp
using System;
using Systems.Zlink.Stream.Connector.Contracts;
using UnityEngine;

public sealed class ZlinkStreamClientBehaviour : MonoBehaviour
{
    private IZlinkStreamConnector _connector;

    private async void Start()
    {
        _connector = ZlinkStreamConnectorFactory.Create(new ZlinkStreamConnectorOptions
        {
            Endpoint = new Uri("wss://game.example.com/stream")
        });

        _connector.On("MatchAssigned", (message, _) =>
        {
            Debug.Log($"packet: {message.Name}, bytes: {message.Payload.Payload.Length}");
            return default;
        });

        await _connector.Connect.Async();
    }

    private async void Update()
    {
        if (_connector is not null) await _connector.Dispatch.Async();
    }

    private async void OnDestroy()
    {
        if (_connector is not null)
        {
            await _connector.Close.Async();
            await _connector.DisposeAsync();
            _connector = null;
        }
    }
}
```

`Dispatch.Async()`는 transport를 읽고, 그 다음 등록된 `On` handler와 error·disconnect·request
callback을 실행한다. `WaitFor`·`ExpectNone`·`WaitForSequence`는 transport를 직접 읽고 수신 큐를
소비하므로 등록된 callback을 실행하지 않으며, `Update`의 pump 여부와 무관하게 동작한다.

## 네이티브 빌드와 다른 점

| 멤버 | WebGL 동작 |
|---|---|
| `Endpoint` | `ws://`·`wss://`만 받는다. `tcp://`·`tls://`는 `Create`에서 `ConfigurationError`로 실패한다 |
| `SkipServerCertificateValidation` | `Create`에서 거부한다. `wss://` 인증서 검증은 브라우저가 소유하며 건너뛸 수 없다 |
| `CompressionCodec` | `Create`에서 거부한다. 압축 codec은 JavaScript connector에 있으므로 `Compression`으로 알고리즘만 고른다 |
| `PayloadCodec` | typed `Send<T>`·`Request<T>`·`WaitFor<T>`에 필요하다. Unity에는 `System.Text.Json`이 없어 JSON 기본값이 없고, 설정하지 않으면 `NotSupportedException`이다. `IZlinkStreamConnector`의 encoded API는 codec 없이 동작한다 |
| `PendingDispatchCount` | 다음 `Dispatch`를 기다리는 callback 수 |

나머지 멤버는 이름·시그니처·의미가 네이티브 패키지와 같다.

## Unity에서 직접 확인할 것

jslib 경계는 `framework/languages/node/test/contract/unity-webgl-jslib.test.js`가 실제 STREAM
서버를 상대로 자동 검증한다. Unity는 CI에 없으므로 패키지를 고친 뒤 다음을 직접 확인한다.

1. WebGL 빌드가 컴파일 오류 없이 끝난다.
2. 빌드 산출물 `Build/*.framework.js`에 `ZlinkStreamWebGlRuntime`과
   `ZlinkStreamConnectorBundle`이 들어 있다. 없으면 `.jspre`·`.jslib`의 WebGL 플랫폼 체크를
   확인한다.
3. 실제 STREAM 서버에 붙어 connect·request/reply·push handler가 동작한다.
4. `Update()`의 pump로 handler가 Unity main thread에서 실행된다(handler 안에서 `Transform`을
   건드려도 예외가 없다).
5. Editor Play mode는 NuGet 패키지로 컴파일·실행된다.

## 관련 문서

- [02 — 브라우저](02-browser.ko.md) — codec 주입, dispatch, flow 전달
- [.NET Stream Connector 가이드 02 — Unity(네이티브 빌드)](../../../dotnet/guide/stream-connector/02-unity.ko.md)
- [설치](../../../install.ko.md)
