# Core 0.13.0 bindings 성능 작업 준비

## 준비 결과

- 상태: 계획 작성 완료, 측정 미시작
- 작업 브랜치: `core-0.13.0-bindings-performance`
- source commit: `67a1a8ce9f55dedb84e8e004ad57d8a603e4cb19`
- 준비 전 작업 트리: clean
- 계획 문서 생성 뒤 변경: `doc/perf/perf/bindings-0.13.0/` 추가
- 첫 대상: C++ Single `PAIR / tcp`

## 버전 gate

세 위치가 모두 `0.13.0`으로 일치한다.

| 위치 | 확인값 | 판정 |
|------|--------|------|
| `VERSION` | `LIBZLINK_VERSION=0.13.0` | 통과 |
| `core/CMakeLists.txt` | `project(zlink VERSION 0.13.0 ...)` | 통과 |
| `core/include/zlink.h` | major `0`, minor `13`, patch `0` | 통과 |

현재 source는 `core/v0.13.0` 이후 36개 commit이 추가된 상태다. 공식 paired 측정은 source의
`core/build`를 사용하지 않고 아래 release package를 사용한다.

## Release runtime gate

`bindings/tools/local_core_runtime.sh`를 release mode로 불러와 다음 값을 확인했다.

| 항목 | 값 |
|------|----|
| version | `0.13.0` |
| source | `release` |
| release mode | `1` |
| prefix | `/home/hep7hep7/.cache/zlink/core/0.13.0/linux-x64` |
| runtime | `lib/libzlink.so.0.13.0` |
| release tag | `core/v0.13.0` |
| release source revision | `dc9930877041649fc7400de0ebe5382ad9b33ff9` |
| runtime SHA-256 | `96f18853a19e06fe22b0c13bbbb70b9409516547102b5542e862386c3f9d06f7` |
| provenance dirty | `false` |

`share/zlink/core-package-provenance.json`의 version과 runtime 파일 존재 여부가 모두 통과했다.

## 호스트와 도구chain

| 항목 | 값 |
|------|----|
| OS | Ubuntu 24.04 on WSL2, kernel `6.6.87.2-microsoft-standard-WSL2` |
| CPU | Intel Core i7-1260P, 논리 CPU 16개 |
| memory | 11 GiB |
| CMake | 3.28.3 |
| GCC | 13.3.0 |
| .NET | 8.0.130 |
| Java | OpenJDK 21.0.11 |
| Node.js | 18.19.1 |
| Go | 1.22.2 |
| Rust | 1.75.0 |
| Python | 3.12.3 |

Node 측정 전에는 프로젝트가 고정한 Node runtime을 runner 환경에 명시해야 한다. 현재 기본
`node`는 18.19.1이고 `npm`은 Windows 설치 경로를 가리키므로 이 상태를 Node 공식 측정에
사용하지 않는다.

## 측정 시작 전 남은 gate

다음 항목을 완료하기 전에는 공식 결과를 계획 표에 기록하지 않는다.

1. C와 C++ runner의 pattern, transport, size, client 수, option inventory를 정책 및 상세 표와 대조한다.
2. 첫 대상 `C++ Single PAIR / tcp`의 C와 C++ smoke를 각각 1초 1회로 직렬 실행한다.
3. 두 report의 `status: complete`, runtime version, provenance, Effective Options, auto-HWM을 확인한다.
4. 같은 session tag와 조건으로 C를 먼저 실행한 직후 C++ before를 실행한다.
5. 첫 paired 결과에서 post-realignment 기준의 per-cell 예외와 aggregate 목표를 다시 도출한다.

측정 명령, report 경로, 프로파일과 후보 검토는 이후 날짜별 log에 추가한다.
