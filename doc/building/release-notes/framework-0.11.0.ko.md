[English](./framework-0.11.0.md) | [한국어](./framework-0.11.0.ko.md)

# ZLink Framework 0.11.0 릴리스 노트 초안

> 초안이다. CI가 다섯 플랫폼에서 녹색이 된 뒤 `framework/v0.11.0` 태그로 배포한다. 배포 전에
> "배포 전 확인" 절의 항목을 지운다.

## 의존 버전

Framework 0.11.0은 C++·.NET·Java·Node.js에서 ZLink binding 0.17.6(Core 0.17.5)을 사용한다.
0.10.0은 binding 0.17.3을 사용했다. 버전 규칙은 [버전 정책](../versioning.ko.md)을 따른다.

## 주요 변경

### 런타임

- .NET: ClientServer client의 admission `hello` request가 timeout(Core result 101)으로 끝나도
  terminal로 고정하지 않고 같은 physical generation/attempt fence 안에서 다음 `hello`를 재시작한다.
  느린 머신에서 1초 timeout 한 번이 connection intent를 영구 not-ready로 만들던 결함이다.
- Java: ClientServer의 공개 weight를 Core `peerWeight`에 전달하지 않는다. weight 0 서버가
  Unavailable로 분류돼 선택 제외(NotFound) 대신 선택되던 결함이다.
- C++: Boost 1.87 이상에서 제거된 waitable timer의 `cancel(error_code&)` 호출을 인자 없는
  `cancel()`로 바꿨다. 패키지 Boost로 빌드할 때 필요하다.

### 패키지와 빌드

- .NET: `Zlink.Framework.AspNetCore`와 `Zlink.HttpClient`의 AssemblyName을 package ID와 같은
  `Zlink.*`로 되돌렸다. 0.10.0 패키지는 `Systems.*` AssemblyName과 `InternalsVisibleTo`가 남아
  있어 게시된 패키지만으로 샘플을 빌드하면 실패했다.
- C++: 공개 자산이 `framework/languages/cpp`만 담아 설치가 불가능하던 것을 고쳐, source
  archive에 공유 runtime 입력(`runtime/`: generated protocol header, schema, golden, conformance)과
  LICENSE를 포함한다. 설치 config는 Core·binding·Boost·LZ4를 `find_dependency`로만 찾고 파일을
  복사하지 않는다. vcpkg overlay port `zlink-cpp`·`zlink-framework`와 Conan recipe 초안이 이
  archive를 빌드한다(공개 자산 게시 뒤 checksum 갱신 필요).
- C++: 저장소 빌드는 CMake preset(`vs2022`, `windows-ninja`, `linux-ninja`, `macos-ninja`,
  `dev`, `ci`)과 `scripts/dev/bootstrap-cpp.{sh,ps1}`(vcpkg + overlay port)으로 연다. Intel Mac은
  지원하지 않는다.
- 모든 언어: 7개 샘플과 언어별 시나리오 e2e를 기본 빌드·CI·배포에서 뺐다. 샘플은 저장소 안에서는
  framework 소스를, 저장소 밖으로 복사하면 게시된 패키지를 자동으로 참조한다(두 모드). 자세한
  내용은 [Framework 작업 공간 구성](../framework-workspace.ko.md).
- .NET·Java/Kotlin은 빌드 시 root `FRAMEWORK_VERSION`을 직접 읽는다. Node·C++ manifest는
  `sync-version.py`가 맞춘다.

## 언어별 패키지

- C++: `framework/v0.11.0` GitHub Release의 `zlink-framework-cpp-0.11.0.tar.gz`와 SHA-256 파일
- Java/Kotlin: Maven Central `systems.zlink` namespace의 framework module
- Node.js: npm `@zlink-systems` scope의 framework workspace package
- .NET: nuget.org의 `Zlink.Framework`, `Zlink.Framework.AspNetCore`, `Zlink.HttpClient`,
  `Zlink.Stream.Connector` 등

## 설치 예

```bash
npm install @zlink-systems/framework@0.11.0
dotnet add package Zlink.Framework.AspNetCore --version 0.11.0
```

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.11.0")
}
```

C++은 GitHub Release 자산을 내려받아 CMake package를 설치한 뒤
`find_package(zlink_framework_cpp CONFIG REQUIRED)`로 사용한다. 설치된 Core·binding package
prefix를 `CMAKE_PREFIX_PATH`에 함께 준다.

## 배포 전 확인

- Node.js ClientServer admission timeout 뒤 재시작(진행 중), Java·C++ 같은 경로 감사(진행 중)
  결과를 반영한다.
- macOS .NET 타이밍 테스트와 macOS Node 계약 테스트의 CI 실패 분류·수정을 반영한다.
- `FRAMEWORK_VERSION`을 0.11.0으로 올리고 `sync-version.py --write`를 실행한다.
