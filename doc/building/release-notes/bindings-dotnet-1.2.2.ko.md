[English](./bindings-dotnet-1.2.2.md) | [한국어](./bindings-dotnet-1.2.2.ko.md)

# ZLink .NET binding 1.2.2 릴리스 노트

Core 1.2.0 위에서 동작하는 릴리스입니다. Core는 바뀌지 않았고 패키징만 고쳤습니다.

## 변경

- NuGet 패키지의 `runtimes/win-x64/native/`에 `zlink.dll`이 의존하는 OpenSSL(`libcrypto-3-x64.dll`, `libssl-3-x64.dll`)과 MSVC runtime DLL을 함께 싣습니다. 1.2.1은 `zlink.dll`만 담고 있어 `LoadLibrary`가 의존 DLL을 찾지 못해 Windows에서 여전히 `DllNotFoundException`이 났습니다. Core Windows 아카이브의 `bin/*.dll` 전부를, Node prebuild(`prebuilds/win32-x64`)와 같은 구성으로 싣습니다. (#702)

## 검증

- `Zlink.1.2.2.nupkg`의 파일 목록에서 `runtimes/win-x64/native/zlink.dll`, `libcrypto-3-x64.dll`, `libssl-3-x64.dll`과 MSVC runtime DLL, Linux x64 runtime 세 파일을 확인했습니다.
- 배포 zip의 .NET tutorial이 Windows runner에서 기동하는지는 다음 framework 릴리스의 `standalone-zips` 검증이 확인합니다.

릴리스 태그는 `dotnet/v1.2.2`입니다.
