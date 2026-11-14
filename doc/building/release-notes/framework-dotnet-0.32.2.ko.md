[English](./framework-dotnet-0.32.2.md) | [한국어](./framework-dotnet-0.32.2.ko.md)

# ZLink .NET Framework 0.32.2 릴리스 노트

Framework 0.32.2는 0.32.1의 결함 하나를 고치는 hotfix입니다. .NET binding 1.23.0과 Core 1.23.0을 그대로 사용합니다.

## 결함 수정

- ClientServer client의 liveness ACK 제출이 실패하면 그 연결을 직접 닫았고, 연결 목록은 이를 반영하지 못해 닫힌 연결을 계속 썼습니다. 같은 노드에 Client와 Server를 함께 등록한 채널에서 이 일이 생기면 Server가 정상이어도 요청이 `NotConnected`로 영구히 실패할 수 있었습니다. 이제 ACK 실패는 오류로 보고만 하고, 연결 종료와 재연결은 transport monitor·liveness와 Core 재연결이 결정합니다. (#4345, #4424)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.32.2" />
<PackageReference Include="Zlink.HttpClient" Version="0.32.2" />
```

릴리스 태그는 [`framework-dotnet/v0.32.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.32.2)입니다.
