# bump-0.17.2 진행

- 2026-09-08: main의 `core`, `bindings`, `scripts`가 깨끗한 상태에서 시작했다. `4cd03b9173`의 비-framework 버전 대상과 local-package 소비자 pin을 0.17.2로 갱신했다. Debian changelog에는 0.17.1 기록을 보존하고 새 0.17.2 UNRELEASED 항목을 추가했다.
- 검증 진행 중: Core dev build 및 버전 관련 ctest를 실행한다. 커밋·태그는 만들지 않는다.
- 2026-09-08: dev CMake가 0.17.2를 감지했고, version/contract_surface/header ctest 2개는 통과했다. dev와 release LTO 빌드가 아직 수행 중이므로 모두 끝난 뒤 다시 ctest와 release SONAME 확인을 한다.
- 2026-09-08: release `libzlink.so.0.17.2` 생성, Core 버전 ctest 2/2, C binding runner(contract 10/10, sample 6/6)를 확인했다. C++ runner는 version test를 포함한 18개 contract가 통과했으나 concurrent multipart 기존 contract 1개가 실패했다. Python은 pytest 부재, Go는 고정 native library 링크 오류로 선택 version test를 실행하지 못했다. 최종 보고서를 작성했으며 커밋·태그는 만들지 않았다.
