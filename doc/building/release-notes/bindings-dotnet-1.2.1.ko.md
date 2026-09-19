[English](./bindings-dotnet-1.2.1.md) | [한국어](./bindings-dotnet-1.2.1.ko.md)

# ZLink .NET binding 1.2.1 릴리스 노트

Core 1.2.0 위에서 동작하는 릴리스입니다. Core는 바뀌지 않았고 패키징만 고쳤습니다.

## 변경

- NuGet 패키지에 `runtimes/win-x64/native/zlink.dll`을 함께 싣습니다. 1.2.0은 Linux x64 runtime만 담고 있어 Windows에서 `DllNotFoundException`이 발생했습니다. 이제 Windows x64에서 별도 Core 설치 없이 동작합니다. (#702)

## 검증

- `Zlink.1.2.1.nupkg`의 파일 목록에서 `runtimes/win-x64/native/zlink.dll`과 Linux x64 runtime 세 파일을 확인했습니다.

릴리스 태그는 `dotnet/v1.2.1`입니다.
