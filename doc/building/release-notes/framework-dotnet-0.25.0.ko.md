[English](./framework-dotnet-0.25.0.md) | [한국어](./framework-dotnet-0.25.0.ko.md)

# ZLink .NET Framework 0.25.0 릴리스 노트

Framework 0.25.0은 .NET binding 1.7.0과 Core 1.7.0을 사용합니다.

## 수정

- Core 1.7.0은 libstdc++을 정적으로 링크한 host가 `dlopen()`으로 `libzlink`를 읽을 때 발생하던 static TLS 부족 문제를 해결합니다([#1041](https://github.com/zlink-systems/zlink/issues/1041)).
- DeliveryDispatch·Bingo·TicTacToe 샘플 README의 실행 명령을 샘플 디렉터리 기준으로 고쳤습니다 ([#1035](https://github.com/zlink-systems/zlink/issues/1035)).

## 설치

```xml
<PackageReference Include="Zlink.Framework" Version="0.25.0" />
<PackageReference Include="Zlink.HttpClient" Version="0.25.0" />
```

릴리스 태그는 [`framework-dotnet/v0.25.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.25.0)입니다.
