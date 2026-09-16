[English](./core-1.2.0.md) | [한국어](./core-1.2.0.ko.md)

# libzlink 1.2.0 릴리스 노트

C API와 ABI는 1.1.0과 같습니다(`LIBZLINK_ABI_SOVERSION=0`). 이 릴리스는 **배포
아카이브의 내용**을 바꿉니다.

## 주요 변경

**아카이브가 설치 prefix 전체를 담습니다.** 이전에는 공유 라이브러리 하나와 공개 헤더
3개뿐이었습니다. 이제 `lib/cmake/zlink/*.cmake`, 정적 라이브러리, `lib/pkgconfig`,
전체 헤더 트리가 함께 들어갑니다. 아카이브를 푼 자리를 `CMAKE_PREFIX_PATH`에 주면
`find_package(zlink CONFIG REQUIRED)`가 성립합니다.

그래서 **Core를 소스에서 빌드하지 않고 내려받아 쓸 수 있습니다.** C++ binding과
framework는 C++ ABI 때문에 소비자 구성에 맞춰 빌드되어야 하지만, Core는 순수 C API만
노출하므로 그 제약이 없습니다.

**정적 라이브러리가 툴체인에 묶이지 않습니다.** 이전 빌드는 LTO가 전역으로 켜져
`libzlink.a`에 실제 오브젝트 코드 대신 GCC 바이트코드가 들어갔고, 그 아카이브는 만든
것과 같은 컴파일러 메이저 버전에서만 링크됐습니다. 정적 타깃만 LTO를 끕니다. 공유
라이브러리는 그대로 LTO로 빌드됩니다.

Windows 아카이브의 CMake config 위치가 `CMake/`에서 `lib/cmake/zlink/`로 바뀌어 네
플랫폼이 같은 레이아웃을 갖습니다.

## 소비자에게 미치는 영향

기존 경로는 그대로입니다. 아카이브 루트의 `libzlink.so`·`libzlink.dylib`·
`bin/zlink.dll`·`include/*.h`에 의존하는 도구는 고치지 않아도 됩니다. 추가일 뿐
이동이 아닙니다.

## 검증

CI가 아카이브를 실제로 소비합니다. 네 플랫폼 모두 CMake config와 정적 라이브러리가
있는지, 아카이브 멤버에 LTO 바이트코드가 섞이지 않았는지 확인하고, linux-x64는
아카이브를 풀어 `find_package(zlink CONFIG REQUIRED)`로 공유·정적 두 타깃에 C
프로그램을 링크해 실행합니다. 이 검증은 빌드 job과 **다른 GCC 메이저 버전**의
러너에서 돌아, 툴체인을 넘는 링크를 실제로 증명합니다.

릴리스 태그는 [`core/v1.2.0`](https://github.com/zlink-systems/zlink/releases/tag/core%2Fv1.2.0)입니다.

관련 이슈: [#397](https://github.com/zlink-systems/zlink/issues/397)
