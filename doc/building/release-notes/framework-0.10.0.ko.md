[English](./framework-0.10.0.md) | [한국어](./framework-0.10.0.ko.md)

# ZLink Framework 0.10.0 릴리스 노트 초안

> 초안이다. binding 0.17.3 공개 배포가 완료된 뒤 framework를 배포한다.

## 의존 버전

Framework 0.10.0은 C++·.NET·Java·Node.js에서 ZLink binding 0.17.3을 사용한다.
Framework 자체 버전은 0.10.0으로 유지한다.

## 언어별 패키지

- C++: `framework/v0.10.0` GitHub Release의
  `zlink-framework-cpp-0.10.0.tar.gz`와 SHA-256 파일
- Java/Kotlin: Maven Central `systems.zlink` namespace의 framework module
- Node.js: npm `@zlink-systems` scope의 framework workspace package
- .NET: nuget.org의 `Systems.Zlink.Framework.AspNetCore`, `Systems.Zlink.HttpClient`,
  `Systems.Zlink.Stream.Connector`

.NET의 기존 package ID `Zlink.Framework.AspNetCore`와 `Zlink.HttpClient`는 각각
`Systems.Zlink.Framework.AspNetCore`와 `Systems.Zlink.HttpClient`로 바뀐다. 소스 namespace는
호환성을 위해 기존 `Zlink.*` 이름을 유지한다.

## 설치 예

```bash
npm install @zlink-systems/framework@0.10.0
dotnet add package Systems.Zlink.Framework.AspNetCore --version 0.10.0
```

```kotlin
dependencies {
    implementation("systems.zlink:zlink-framework-core:0.10.0")
}
```

C++은 GitHub Release 자산을 내려받아 CMake package를 설치한 뒤
`find_package(zlink_framework_cpp CONFIG REQUIRED)`로 사용한다.

## 배포 순서

1. C++·Node.js·Java·.NET binding 0.17.3을 공개한다.
2. 공개 registry와 GitHub Release에서 0.17.3을 확인한다.
3. Framework 0.10.0을 공개한다. .NET package push는 Trusted Publishing 정책이 고정한
   `release-dotnet.yml`에서 수행한다.
