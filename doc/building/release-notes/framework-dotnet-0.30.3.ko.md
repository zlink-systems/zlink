[English](./framework-dotnet-0.30.3.md) | [한국어](./framework-dotnet-0.30.3.ko.md)

# ZLink .NET Framework 0.30.3 릴리스 노트

Framework 0.30.3은 .NET binding 1.20.0과 Core 1.20.0을 사용합니다.

## 결함 수정

- User Spot이 `Close`를 진행 중일 때 `GetOrCreate`가 해당 Spot에 `Join`을 요청하면 실패하던 문제를 수정했습니다. (#2672)
- Close가 완료된 Spot의 이전 `SpotRef`로 `Close`를 호출하면 예외가 발생하던 문제를 수정했습니다. `Close`가 진행 중일 때 두 번째 요청은 첫 번째 요청에 합류해 같은 결과를 받습니다. (#2675)
- 다른 node의 Actor `Join`이 거부된 뒤 `ShutdownAsync`가 `Draining`에서 멈추던 문제를 수정했습니다. (#2682)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.30.3" />
<PackageReference Include="Zlink.HttpClient" Version="0.30.3" />
```

릴리스 태그는 [`framework-dotnet/v0.30.3`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.30.3)입니다.
