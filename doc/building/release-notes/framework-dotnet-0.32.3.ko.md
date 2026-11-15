[English](./framework-dotnet-0.32.3.md) | [한국어](./framework-dotnet-0.32.3.ko.md)

# ZLink .NET Framework 0.32.3 릴리스 노트

Framework 0.32.3은 STREAM control 처리 중 application 진행이 멈추는 결함을 고치는 hotfix입니다. 0.32.2의 ClientServer liveness ACK 수정도 포함합니다. .NET binding 1.23.0과 Core 1.23.0을 그대로 사용합니다.

## 결함 수정

- STREAM control이 application turn과 연결 준비를 기다리면 ingress permit을 반환하지 못해 Session·Actor 요청이 멈출 수 있었습니다. Control은 기존 session state lane에서 처리하며, 세션이 없거나 연결 준비·종료 중이어도 처리합니다. Protocol error의 transport 종료도 application turn을 기다리지 않습니다. (#4345, #4552)
- ClientServer client의 liveness ACK 제출 실패는 오류로 보고합니다. 연결 종료와 재연결은 transport monitor·liveness와 Core 재연결이 결정하므로 닫힌 연결을 계속 사용해 요청이 영구히 실패하는 결함을 방지합니다. 이 수정은 0.32.2부터 포함합니다. (#4345, #4424)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.32.3" />
<PackageReference Include="Zlink.HttpClient" Version="0.32.3" />
```

릴리스 예정 태그는 `framework-dotnet/v0.32.3`입니다. 태그와 package 게시는 #4552 머지 뒤에 진행합니다.
