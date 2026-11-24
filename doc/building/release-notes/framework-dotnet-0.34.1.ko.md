[English](./framework-dotnet-0.34.1.md) | [한국어](./framework-dotnet-0.34.1.ko.md)

# ZLink .NET Framework 0.34.1 릴리스 노트

Framework 0.34.1은 0.34.0의 결함 하나를 고치는 hotfix입니다. .NET binding 1.26.0과 Core 1.26.0을 그대로 사용합니다.

## 결함 수정

- Actor와 bound session이 서로 다른 node에 있을 때, 같은 Actor가 같은 session으로 순서대로 보낸 push가 client에 역순으로 도착할 수 있었습니다. 0.34.0에서 생긴 회귀입니다. 이제 relay된 push를 받은 순서 그대로 그 session의 송신 순서에 넣습니다. (#5459, #5469)

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.34.1" />
<PackageReference Include="Zlink.HttpClient" Version="0.34.1" />
```

릴리스 태그는 [`framework-dotnet/v0.34.1`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.34.1)입니다.
