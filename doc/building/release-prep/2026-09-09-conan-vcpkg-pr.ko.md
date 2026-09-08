# ConanCenter·vcpkg Draft PR 제출 기록 (2026-09-09)

## 결과

Core 0.17.3의 ConanCenter recipe와 vcpkg port를 각 upstream 최신 `master`에서 만든
`zlink/0.17.3` 브랜치로 제출했다. 두 PR은 모두 Draft 상태다.

- ConanCenter: https://github.com/conan-io/conan-center-index/pull/30935
- vcpkg: https://github.com/microsoft/vcpkg/pull/53846

원본 저장소에서는 Core 소스와 준비 문서를 수정하지 않았으며 commit과 push도 수행하지
않았다. 외부 저장소 commit과 push는 `/home/hep7/.cache/zlink/pr/` 아래 clone에서만
수행했다. force-push와 secret·token 생성은 수행하지 않았다.

## ConanCenter

작업 트리는 `/home/hep7/.cache/zlink/pr/conan-center-index`이고, fork는
`zlink-systems/conan-center-index`다. upstream 최신 `master`에서 브랜치를 만들고
`4fbe37f69416c98910e9764d5bf628fbd563d506` (`zlink: add 0.17.3`)을 push했다.

제출 파일은 다음과 같다.

- `recipes/zlink/config.yml`
- `recipes/zlink/all/conanfile.py`
- `recipes/zlink/all/conandata.yml`
- `recipes/zlink/all/test_package/CMakeLists.txt`
- `recipes/zlink/all/test_package/conanfile.py`
- `recipes/zlink/all/test_package/test_package.cpp`

ConanCenter의 `docs/adding_packages/`, package template, `CONTRIBUTING.md`와 유사한
`zeromq` recipe를 대조했다. 최신 버전만 등록하고 static을 기본값으로 설정했으며,
ConanCenter URL, MPL-2.0, homepage, topics, `package_type`, C++17 검증과 명시적
`test_package`를 반영했다. 호스트의 `libbsd` 유무가 결과를 바꾸지 않도록 자동 탐지를
껐고, Conan이 생성하는 CMake·pkg-config 메타데이터와 충돌할 수 있는 upstream 설치
메타데이터는 package에서 제거했다. patch는 사용하지 않았으므로 `conandata.yml`에
`patches` 항목을 만들지 않았다.

검증 결과는 다음과 같다.

- `0-1788889617-51503-codex-pr-ConanCenter_zlink_0.17.3_Release_conan_c` — rc=0.
  Release static package와 CMake `test_package`의 링크·실행이 통과했다.
- `0-1788890373-88065-codex-pr-ConanCenter_zlink_0.17.3_final_recipe_re` — rc=0.
  `libbsd` 탐지 차단과 package 메타데이터 정리 후 최종 recipe를 같은 명령으로 다시
  검증했다.
- 최종 명령:
  `conan create recipes/zlink/all --version 0.17.3 -s build_type=Release`
- 환경: Linux x86_64, GCC 13.3.0, Conan 2.32.0.

초기 PR 상태는 Draft·Open·Blocked다. `Related Pull Requests`는 통과했고,
`license/cla`는 미서명으로 pending이다. 신규 기여자용 `Job scheduler`는
`ACTION_REQUIRED`로 끝나 upstream 관리자의 CI 승인도 필요하다.

## vcpkg

작업 트리는 `/home/hep7/.cache/zlink/pr/vcpkg`이고, fork는
`zlink-systems/vcpkg`다. upstream 최신 `master`에서 브랜치를 만들고
`93fd46d90b54275311d7ffb1e47b1ec53f23459b` (`[zlink] new port 0.17.3`)을 push했다.

제출 파일은 다음과 같다.

- `ports/zlink/portfile.cmake`
- `ports/zlink/vcpkg.json`
- `ports/zlink/usage`
- `versions/z-/zlink.json`
- `versions/baseline.json`

현재 vcpkg 도구를 `/home/hep7/.cache/zlink/vcpkg-tool`에서 bootstrap하고 fork 루트에
배치했다. `format-manifest`를 실행한 뒤 `x-add-version zlink --overwrite-version`으로
version DB를 생성했다. 최종 port tree hash
`d5823a754903513653c8aa0def0fde1671806a8c`가 `versions/z-/zlink.json`의
`git-tree`와 일치한다. 태그 자동 archive의 SHA-512도 다시 계산해 portfile 값과
일치함을 확인했다.

시스템 `libbsd` 자동 탐지를 꺼 선택적 의존성이 설치 환경에 따라 달라지지 않게 했다.
TLS는 항상 켜고 OpenSSL을 manifest 의존성으로 선언했다. 첫 install 로그에서 Core가
사용하지 않는 `ZLINK_BUILD_CPP_BINDINGS` 옵션 경고를 확인해 최종 portfile에서 제거했다.

검증 결과는 다음과 같다.

- `0-1788889930-66532-codex-pr-Bootstrap_current_vcpkg_tool_for_zlink_0` — rc=0.
- `0-1788890556-96875-codex-pr-Official_vcpkg_tree_install_zlink_0.17.3` — rc=0.
  fork 루트의 `./vcpkg install zlink`가 x64-linux에서 Debug·Release 빌드와 post-build
  validation을 통과했다.
- `./vcpkg format-manifest ports/zlink/vcpkg.json` — 통과.
- `./vcpkg x-add-version zlink --overwrite-version` — version file과 baseline 생성·갱신
  통과.

초기 PR 상태는 Draft·Open·Blocked다. Microsoft CLA check와 전체 vcpkg CI가 pending이고,
arm64 macOS·Windows, Android, x64 Linux·Windows, x86 Windows 작업이 시작됐다.

## 사용자가 직접 해야 할 일

1. ConanCenter PR에서 `zlink-systems` 계정으로 Conan CLA에 서명한다. 서명 후 상태가
   갱신되지 않으면 CLA assistant의 recheck 링크를 사용한다.
2. vcpkg PR의 Microsoft CLA 안내를 읽고, 개인 또는 회사 명의 중 실제 권리 관계에 맞는
   동의 댓글을 직접 남긴다. 이 선택은 법적 확인이 필요하므로 자동으로 수행하지 않았다.
3. vcpkg 신규 port의 성숙도 근거를 검토한다. Git history는 2026-01-11에 시작했지만 공개
   GitHub 저장소 생성일은 2026-07-31이고 첫 공개 Core release는 2026-08-07이다. PR
   checklist는 maintainer 판단 전까지 완료 처리하지 않았다.
4. `zlink` 이름은 현재 Repology에 등록되지 않았고 검색 결과 screenshot도 PR에 없다.
   vcpkg 이름 연관성 checklist를 충족할 근거를 추가하거나 maintainer와 port 이름을
   협의한다.
5. 두 PR의 CI 결과와 maintainer 의견을 확인한다. CCI는 신규 기여자 job 실행에 maintainer
   승인이 필요하며, vcpkg의 여러 platform build는 제출 직후 진행 중이다.
6. CI와 미완료 checklist가 해결된 뒤에만 Draft를 해제한다.

## 검증 로그

티켓 로그는 원본 저장소의 `.artifacts/perf-queue/log/` 아래에 있으며 파일명은 위 티켓
이름과 같다. 이 경로의 산출물은 commit하지 않았다.
