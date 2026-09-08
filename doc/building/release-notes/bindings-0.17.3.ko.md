[English](./bindings-0.17.3.md) | [한국어](./bindings-0.17.3.ko.md)

# ZLink bindings 0.17.3 릴리스 노트 초안

> 초안이다. 실제 레지스트리 게시와 GitHub Release 생성 전에는 최종 배포 공지가 아니다.

## 주요 변경

- 네 언어 binding이 Core 0.17.3을 포함하거나 정확한 의존 버전으로 사용한다. Core 0.17.3은
  동시 send/receive 중 STREAM 수신이 멈출 수 있던 ownership 경쟁, Windows 난수 생성기의
  엔트로피 부족, context 종료 중 간헐적인 mailbox wake 처리를 수정한다.
- Node.js binding은 routing id가 없는 2-part 이상 수신을 N-API에서 `Buffer[]`로 바로 넘긴다.
  공개 `Received.parts`와 ownership 계약은 유지하면서 중간 객체 생성을 줄였다.
- C++ binding 계약은 다른 thread가 제출한 독립 multipart record를 원자적으로 허용하는
  Core 0.17.2 이후 동작과 일치한다.
- C++·Java·.NET의 공개 API와 C++ ABI에는 0.17.2 대비 의도한 변경이 없다. .NET NuGet
  산출물에는 MPL-2.0, repository·README·SourceLink 메타데이터와 snupkg 지원이 추가됐다.

성능 runner와 문서만 바꾼 commit은 이 목록에서 제외했다.

## 설치

### Java

```kotlin
dependencies {
    implementation("systems.zlink:zlink:0.17.3")
}
```

Maven Central의 group은 `systems.zlink`다.

### .NET

```bash
dotnet add package Systems.Zlink --version 0.17.3
```

nuget.org에서 `Systems.Zlink`를 설치한다.

### Node.js

```bash
npm install @zlink-systems/zlink@0.17.3
```

첫 공개 배포는 `zlink-systems` 계정으로 수동 게시하고, 이후 npm Trusted Publishing과
provenance를 사용한다.

### C++

- GitHub Release `cpp/v0.17.3`의 `zlink-cpp-0.17.3.tar.gz` 자산을 사용한다.
- Core C library는 ConanCenter의 `zlink/0.17.3` recipe 또는 vcpkg의 `zlink` port로 설치할
  수 있다. C++ binding 배포 자산과 Core package 버전을 모두 0.17.3으로 맞춘다.

## 호환성

- 요구 Core release: `core/v0.17.3`
- binding version: `0.17.3`
- 공개 API/ABI 변경: 없음
