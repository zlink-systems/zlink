[English](./framework-dotnet-0.32.1.md) | [한국어](./framework-dotnet-0.32.1.ko.md)

# ZLink .NET Framework 0.32.1 릴리스 노트

Framework 0.32.1은 0.32.0의 결함 하나를 고치는 hotfix입니다. .NET binding 1.23.0과 Core 1.23.0을 그대로 사용합니다.

## 결함 수정

- ClientServer Server 채널에서 끊긴 client의 만료 정리가 transport의 `NOT_FOUND`를 받으면 채널 수신 루프 전체가 끝나, 같은 채널의 다른 정상 연결 요청이 응답을 받지 못했습니다. 0.31.0에서 생긴 회귀입니다. 이제 `NOT_FOUND`는 이미 끊긴 결과로 처리하고, 그 밖의 disconnect 오류는 채널 오류로 보고한 뒤 다른 peer 처리를 계속합니다. (#4301, #4304)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.32.1" />
<PackageReference Include="Zlink.HttpClient" Version="0.32.1" />
```

릴리스 태그는 [`framework-dotnet/v0.32.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.32.1)입니다.
