[English](./framework-dotnet-0.18.2.md) | [한국어](./framework-dotnet-0.18.2.ko.md)

# ZLink .NET Framework 0.18.2 릴리스 노트

Framework 0.18.2는 binding 1.2.2와 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

공개 API는 바뀌지 않습니다. 0.18.1과의 차이는 binding 고정 버전뿐입니다.

## 수정

- binding 1.2.2를 고정합니다. 1.2.1은 NuGet 패키지에 `zlink.dll`만 실어 `LoadLibrary`가 의존 DLL(OpenSSL `libcrypto-3-x64.dll`·`libssl-3-x64.dll`, MSVC runtime)을 찾지 못하고, Windows에서 배포 패키지만으로는 여전히 `DllNotFoundException`이 났습니다. 1.2.2는 Core Windows 아카이브의 `bin/*.dll` 전부를 `runtimes/win-x64/native/`에 싣습니다. 0.18.1의 나머지 변경은 [0.18.1 릴리스 노트](./framework-dotnet-0.18.1.ko.md)를 참고합니다. (#702)

## 설치

```bash
dotnet add package Zlink.Framework.AspNetCore --version 0.18.2
```

릴리스 태그는 [`framework-dotnet/v0.18.2`](https://github.com/zlink-systems/zlink/releases/tag/framework-dotnet%2Fv0.18.2)입니다.
