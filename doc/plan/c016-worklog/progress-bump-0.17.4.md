# bump-0.17.4 진행

- 2026-09-09: `main` 및 지정 경로(`core`, `bindings`, `scripts`, `vcpkg`, `VERSION`, `BINDINGS_VERSION`) clean 확인.
- 2026-09-09: 기준 커밋 `0761c1d4d0`, Rust 누락 보완 `4cdafee9b7`의 대상과 현재 0.17.3 표기를 조사 중.
- 2026-09-09: 0.17.4 메타데이터와 모든 Core·C/C++/Go/Rust 번들 헤더 상수를 갱신. 8개 binding 헤더 모두 해당 Core 헤더와 byte-for-byte 일치 확인.
- 다음: dev 빌드, 한정 ctest, release lib-only, C/C++ smoke, Rust cargo build를 순서대로 실행.
- 2026-09-09: `JOBS=4 scripts/build-core.sh dev` 성공(CMake 0.17.4 감지), `ctest --test-dir core/build-dev -R 'version|contract_surface|header' --output-on-failure` 성공(2/2).
- 다음: release lib-only 및 binding smoke 검증.
- 2026-09-09: release lib-only 성공, `libzlink.so.0.17.4` 생성 확인. C smoke(contract 10/10, sample 6/6)와 C++ smoke(contract 19/19, sample 7/7) 성공.
- 2026-09-09: Rust local cargo build는 설치 Cargo 1.75.0의 Rust 2024 edition 미지원으로 build.rs 실행 전에 중단. 보고서 작성 및 diff 최종 점검 완료.
