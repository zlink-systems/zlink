[English](./core-1.4.0.md) | [한국어](./core-1.4.0.ko.md)

# libzlink 1.4.0 릴리스 노트

C API와 ABI는 1.3.0과 같습니다(`LIBZLINK_ABI_SOVERSION=0`). 이 릴리스는 macOS
Core dylib가 릴리스 빌드 경로에 의존하지 않게 합니다.

## 주요 변경

**macOS 아카이브의 모든 `LC_RPATH`는 loader 기준 상대 경로입니다.** 릴리스 빌드는
CMake가 남긴 절대 `LC_RPATH`를 모두 지우고 `@loader_path`를 넣습니다. 서명 전에
Mach-O load command 결과를 확인합니다. 패키지 검사는 절대 경로나 다른 비상대
`LC_RPATH`를 거부합니다.

## 소비자에게 미치는 영향

공개 C API와 ABI는 바뀌지 않았으므로 기존 Core 소비자는 소스를 바꿀 필요가 없습니다.
macOS 소비자는 1.4.0 아카이브를 푼 뒤 dylib에 릴리스 빌드 머신 경로가 남지 않은 상태로
다른 위치로 옮길 수 있습니다.

## 검증

macOS 릴리스 빌드와 패키지 검사는 패키지의 모든 dylib `LC_RPATH` 항목을 읽습니다.
상대 경로가 아닌 항목이 있으면 아카이브를 만들기 전에 빌드가 실패합니다.

릴리스 태그는 [`core/v1.4.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv1.4.0)입니다.

관련 이슈: [#962](https://github.com/zlink-systems/zlink/issues/962)
