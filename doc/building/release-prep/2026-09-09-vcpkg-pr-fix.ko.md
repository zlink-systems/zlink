# vcpkg PR #53846 Windows CMake config 경로 수정 기록 (2026-09-09)

## 결과

vcpkg PR #53846의 Windows CI 실패는 Core와 portfile이 서로 다른 CMake package
config 설치 경로를 사용해서 발생했다. fork의 `zlink/0.17.3` 브랜치에 다음 두
커밋을 일반 push했다.

- `453e81b4a3d134147f61854e5608e7f4b7e93ddd`
  (`[zlink] fix cmake config path on Windows`)
- `c64a1d1d1799093a4fd807faa0e1ccb8293c86e5`
  (`[zlink] remove empty bin directories in static builds`)

최종 Azure Pipelines build 137313은 13개 platform과 상위 check가 모두 성공했다.
force-push, PR 댓글, Draft 해제와 secret 생성은 수행하지 않았다.

원본 저장소에는 같은 portfile 수정을 반영했으며 commit과 push는 수행하지 않았다.

## 원인

Core 0.17.3의 `core/CMakeLists.txt:1675`는 `WIN32`에서
`ZLINK_CMAKECONFIG_INSTALL_DIR`의 기본값을 `CMake`로, 그 외 platform에서는
`${CMAKE_INSTALL_LIBDIR}/cmake/${PROJECT_NAME}`으로 정한다. `install(EXPORT ...)`,
`zlinkConfig.cmake`, `zlinkConfigVersion.cmake`는 모두 이 변수를 설치 경로로 사용한다.
이 값은 `CACHE STRING`이므로 configure 인자로 덮어쓸 수 있다.

기존 portfile은 플랫폼과 관계없이
`vcpkg_cmake_config_fixup(PACKAGE_NAME zlink CONFIG_PATH lib/cmake/zlink)`를 호출했다.
그 결과 debug와 release를 함께 만드는 Windows triplet에서는
`${CURRENT_PACKAGES_DIR}/debug/lib/cmake/zlink`가 없어서 fixup이 실패했다.
release-only triplet에서는 Core가 설치한 `CMake/`가 fixup 대상에서 빠져 package의
`cmake/`에 남았고, misplaced CMake files post-build 검사에서 실패했다.

Azure Pipelines build 137311의 platform 결과는 다음과 같다.

- 실패: `x64-windows`, `x64-windows-static`, `x64-windows-static-md`,
  `x64-windows-release`, `x86-windows`, `arm64-windows`,
  `arm64-windows-static-md`
- 성공: `x64-linux`, `arm64-linux`, `arm64-osx`, `x64-android`,
  `arm64-android`, `arm-neon-android`

Windows 로그 144, 146, 152, 153, 160, 168의 첫 실패는 모두 debug의
`lib/cmake/zlink` 부재였다. 로그 131의 `x64-windows-release`는
`cmake/zlinkConfig.cmake`, version과 targets 파일의 잘못된 위치만 post-build
검사에서 보고했다. pkg-config fixup, `usage`, copyright와 PDB 처리의 별도 실패는
없었다.

첫 수정 뒤 build 137312에서는 CMake와 pkg-config fixup이 통과한 다음 Windows
static build가 남긴 빈 `bin`과 `debug/bin` 디렉터리가 post-build 검사에서
발견됐다. static일 때 이 두 디렉터리를 제거하는 후속 수정을 추가했다.

## 변경

fork의 `ports/zlink/portfile.cmake`와 원본 저장소의
`vcpkg/ports/zlink/portfile.cmake` configure options에 다음 한 줄을 추가했다.

```cmake
-DZLINK_CMAKECONFIG_INSTALL_DIR=lib/cmake/zlink
```

Core의 지원 캐시 변수를 사용해 Windows와 비 Windows, debug와 release,
static과 dynamic에서 설치 경로를 하나로 통일했다. 기존
`vcpkg_cmake_config_fixup(... CONFIG_PATH lib/cmake/zlink)`는 그대로 사용한다.

`vcpkg_copy_pdbs()` 다음에는 static linkage에서만 빈 `bin`과 `debug/bin`을
제거한다. dynamic build의 DLL과 PDB 경로는 변경하지 않는다. fork에서는
`x-add-version`이 `versions/z-/zlink.json`의 최종 port tree hash를
`74de75621c39c2963313ba6537f742512c81d35c`로 갱신했다.

## 검증 티켓

| 티켓 | rc | 결과 |
| --- | ---: | --- |
| `0-1788892097-66366-codex-vcpkg-vcpkg_zlink_0.17.3_x64-linux_install_aft` | 0 | 기존 설치를 확인한 no-op이었다. |
| `0-1788892111-67393-codex-vcpkg-vcpkg_zlink_0.17.3_overwrite_version_reg` | 0 | `./vcpkg x-add-version zlink --overwrite-version`이 version tree hash를 갱신했다. |
| `0-1788892125-68471-codex-vcpkg-format_vcpkg_manifests_for_zlink_0.17.3_` | 1 | 인자 없는 `format-manifest`가 대상 파일 또는 `--all`을 요구했다. |
| `0-1788892173-69839-codex-vcpkg-format_zlink_vcpkg_manifest_explicitly` | 0 | `./vcpkg format-manifest ports/zlink/vcpkg.json`이 통과했다. |
| `0-1788892185-70848-codex-vcpkg-remove_installed_zlink_x64-linux_before_` | 0 | 실제 재빌드를 위해 기존 `zlink:x64-linux` 설치만 제거했다. |
| `0-1788892198-71864-codex-vcpkg-clean_vcpkg_zlink_0.17.3_x64-linux_build` | 0 | x64-linux debug·release clean build, config/pkg-config fixup과 post-build validation이 통과했다. |
| `0-1788893094-98471-codex-vcpkg-update_zlink_version_registry_after_stat` | 0 | static cleanup 뒤 version tree hash를 다시 갱신했다. |
| `0-1788893107-99627-codex-vcpkg-format_zlink_manifest_after_static_Windo` | 0 | 후속 수정 뒤 명시적 manifest 포맷이 통과했다. |
| `0-1788893120-1052-codex-vcpkg-remove_zlink_x64-linux_before_final_stat` | 0 | 최종 clean build 전에 기존 설치를 제거했다. |
| `0-1788893131-2174-codex-vcpkg-final_clean_zlink_x64-linux_build_with_C` | 0 | 후속 수정까지 포함한 x64-linux debug·release clean build와 post-build validation이 통과했다. |

clean build 결과 `share/zlink`에는 `zlinkConfig.cmake`,
`zlinkConfigVersion.cmake`, `zlinkTargets.cmake`, debug/release targets가 함께
설치됐다. Windows는 로컬 실행 환경이 없어 직접 실행하지 못했다. 경로 수정의
근거는 Core 0.17.3의 platform별 기본값과 명령줄에서 덮어쓸 수 있는 cache 선언이다.
실제 Windows 검증은 build 137313에서 dynamic, static, static-md, release-only,
x86과 arm64를 포함한 7개 triplet이 모두 성공한 것으로 확인했다.

티켓 로그는 원본 저장소의 `.artifacts/perf-queue/log/` 아래에 있다. 이 산출물은
commit하지 않는다.
