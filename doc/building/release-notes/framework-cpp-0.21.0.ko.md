[English](./framework-cpp-0.21.0.md) | [한국어](./framework-cpp-0.21.0.ko.md)

# ZLink C++ Framework 0.21.0 릴리스 노트

Framework 0.21.0은 binding 1.2.1과 Core 1.2.0을 사용합니다. Framework 언어별 릴리스는 독립적으로 버전이 지정됩니다.

## 계약 변경

- 없음. 공개 계약은 0.20.0과 같습니다.

## 공통 변경

- tutorial·quickstart·samples 소스가 포매터 규칙 4(대입의 오른쪽을 `=` 줄에서 시작하고 체인은 그 아래에 이어 쓴다)로 정렬됩니다. (#847)
- quickstart README가 ko/en 표준 절차로 다시 쓰였고 포매터 범위에 들어갑니다. (#838)
- runtime과 공개 계약은 0.20.0과 같습니다. 이 릴리스는 tutorial·samples·quickstart와 저장소 도구를 정리합니다.
- quickstart·tutorial·samples는 언어별 examples 저장소(`zlink-<lang>-examples`)에서 받습니다. 미러 workflow가 실행 비트를 보존하고(#831), README 상단에 English | 한국어 선택 줄을 둡니다.
- tutorial CI의 C++ job이 vcpkg binary cache를 Actions cache에 둡니다. (#852)
- google-java-format 1.27.0으로 Java·Kotlin 포맷 검사가 JDK 25에서 그대로 돕니다. (#798)
- 사용되지 않던 v11 public-contract trace generator와 inventory를 제거했습니다. (#747)

## 설치

[`framework-cpp/v0.21.0` GitHub Release](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.21.0)에서 `zlink-framework-cpp-0.21.0.tar.gz`를 내려받고 `find_package(zlink_framework CONFIG REQUIRED)`를 사용합니다. vcpkg overlay port와 Conan recipe로도 설치할 수 있습니다. C++ binding 1.2.1과 Core 1.2.0이 필요합니다.

릴리스 태그는 [`framework-cpp/v0.21.0`](https://github.com/zlink-systems/zlink/releases/tag/framework-cpp%2Fv0.21.0)입니다.
