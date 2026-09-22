[English](./core-1.3.0.md) | [한국어](./core-1.3.0.ko.md)

# libzlink 1.3.0 릴리스 노트

C API와 ABI는 1.2.0과 같습니다(`LIBZLINK_ABI_SOVERSION=0`). 이 릴리스는 배포하는
정적·macOS 아카이브의 소비자 경계를 고칩니다.

## 주요 변경

**정적 아카이브는 공개 `zlink_*` C ABI만 내보냅니다.** `libzlink.a`를 만드는 과정에서
Core가 vendoring한 Boost 구현 심볼을 지역화합니다. 따라서 소비자가 다른 버전의 Boost를
링크해도 두 구현의 정의가 하나의 심볼로 합쳐지지 않습니다. 이 제한을 만들 수 없는
툴체인에서는 빌드가 실패하며, 제한되지 않은 아카이브를 배포하지 않습니다.

**macOS arm64 아카이브는 재배치 가능한 런타임 closure입니다.** 필요한 OpenSSL dylib를
함께 넣고 Core와 OpenSSL의 install name·의존성을 `@loader_path`로 다시 씁니다. 수정한
모든 Mach-O 바이너리는 ad-hoc 서명합니다. 패키지 검사는 closure와 서명, 그리고 깨끗한 C
소비자 링크·실행을 확인한 뒤에만 아카이브를 통과시킵니다.

## 소비자에게 미치는 영향

공개 C API와 ABI는 바뀌지 않았습니다. 기존 Core 소비자는 소스를 바꿀 필요가 없습니다.
정적 Core와 별도 Boost를 함께 링크하는 소비자는 1.3.0 아카이브를 사용해야 합니다.
macOS 소비자는 아카이브를 설치 prefix 전체로 유지한 채 어느 위치에서든 사용할 수
있습니다.

## 검증

릴리스 빌드는 아카이브의 public symbol 표면을 검사하고, macOS에서는 OpenSSL을 포함한
동적 라이브러리 closure·서명·깨끗한 C 소비자를 검사합니다. 이 검사는 실패하면 릴리스
자산을 만들지 않습니다.

릴리스 태그는 [`core/v1.3.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv1.3.0)입니다.

관련 이슈: [#424](https://github.com/zlink-systems/zlink/issues/424),
[#433](https://github.com/zlink-systems/zlink/issues/433),
[#855](https://github.com/zlink-systems/zlink/issues/855)
