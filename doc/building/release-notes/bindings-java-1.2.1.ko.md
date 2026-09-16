[English](./bindings-java-1.2.1.md) | [한국어](./bindings-java-1.2.1.ko.md)

# ZLink Java binding 1.2.1 릴리스 노트

Core 1.2.0 위에서 동작하는 릴리스입니다. 1.2.0과 같은 binding 코드이며, 패키지 구성만 다릅니다.

## 변경

- Windows x64 native(`native/windows-x86_64/zlink.dll`과 의존 DLL)를 jar에 포함합니다. 1.2.0 jar는 Linux x86_64 native만 담아 Windows에서 `ZLINK_LIBRARY_PATH` 없이 로드할 수 없었습니다.
- Core 1.2.0 native와 결합합니다. 패키지에 담긴 native와 provenance는 Core 1.2.0 릴리스 자산입니다.

## 검증

- binding 테스트 스위트가 Core 1.2.0 패키지 위에서 통과했습니다.
- 게시된 jar의 `native/` 항목에 Linux x86_64와 Windows x64가 함께 있는지 확인했습니다.

릴리스 태그는 `java/v1.2.1`입니다.
