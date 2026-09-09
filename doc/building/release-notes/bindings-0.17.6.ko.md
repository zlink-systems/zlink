[English](./bindings-0.17.6.md) | [한국어](./bindings-0.17.6.ko.md)

# ZLink bindings 0.17.6 릴리스 노트

Core는 0.17.5 그대로이며, binding 패키지 버전만 0.17.6으로 올린다. 네 언어 binding은 하나의
`BINDINGS_VERSION`을 공유하므로 Node.js 수정 하나를 배포하기 위해 C++·Java·.NET도 같은 번호로
다시 게시한다. C++·Java·.NET의 공개 API와 동작에는 0.17.5 대비 변경이 없다.

## 주요 변경

- Node.js: backpressure 판정이 EAGAIN을 Linux 값(11)으로 고정해, macOS(EAGAIN=35)에서는 정상적인
  backpressured send가 항상 `backpressured send did not return an EAGAIN wait token` 오류로
  바뀌던 결함을 고쳤다. 이제 플랫폼의 `os.constants.errno.EAGAIN`을 사용한다. 회귀 테스트
  `platform_backpressure`를 추가했다.
- Node.js: native addon 소스가 MSVC의 `max` 매크로와 충돌해 Windows arm64에서 소스 빌드가
  실패하던 것을 고쳤다(`std::numeric_limits<size_t>::max`를 괄호로 보호). prebuild가 없는 플랫폼은
  설치 시 `ZLINK_CORE_SOURCE=release`·`ZLINK_CORE_PACKAGE_PREFIX`로 Core 릴리스 아카이브에 대해
  addon을 빌드한다.

## 설치

| 채널 | 패키지 | 버전 |
| --- | --- | --- |
| npm | `@zlink-systems/zlink` | 0.17.6 |
| nuget.org | `Zlink` | 0.17.6 |
| Maven Central | `systems.zlink:zlink`, `systems.zlink:zlink-ext-netty` | 0.17.6 |
| GitHub Release | `cpp/v0.17.6` source archive | 0.17.6 |

지원 플랫폼은 linux-x64, linux-arm64, macos-arm64, windows-x64, windows-arm64다. Intel Mac은
Core부터 지원하지 않는다. Linux 사전 빌드 런타임은 glibc 2.38 이상이 필요하다.
