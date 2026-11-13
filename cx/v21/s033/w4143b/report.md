# #4143 — Windows allocation stack 심볼화

## 변경 파일

- `framework/languages/cpp/tests/support/allocation_counter.cpp`: Windows `describe_allocations`에서 DbgHelp으로 함수명과 PDB 소스 위치를 출력한다. `record_allocation`은 그대로 두고, 심볼 조회는 추적 구간 밖에서 호출되는 `describe_allocations`에만 둔다.
- `framework/languages/cpp/CMakeLists.txt`: CTest에 등록되는 MSVC 테스트 타깃에만 Release `/O2`, `/Zi`, `/DEBUG`, `/OPT:REF`, `/OPT:ICF`를 주고 `dbghelp`를 링크한다. `build-windows.ps1`의 `/Od /DNDEBUG` 및 제품 라이브러리 플래그는 수정하지 않았다. Release 테스트에 `/O2`를 타깃 옵션으로 추가해 테스트 할당 동작을 최적화 빌드로 유지한다.
- 차이 파일: `/root/jobs/fx/cx/v21/s033/w4143b/change.diff`

## Windows 경로 2회 검토

1. 첫 검토 — `allocation_counter.cpp:7-13`: DbgHelp·mutex 헤더를 `_WIN32` 아래에서만 추가했다. `record_allocation`(`:34-58`)에는 심볼 조회를 넣지 않았고, 주석(`:62`)에 이 경계를 명시했다. 따라서 프레임 캡처/할당 집계 경로와 테스트 기대값은 바뀌지 않는다.
2. 첫 검토 — `:76-85`: DbgHelp 호출을 함수 정적 mutex로 보호한다. 초기화 시도 플래그와 성공 플래그를 분리해 `SymInitialize`는 프로세스당 한 번만 시도하며, 그 전에 요청된 세 `SYMOPT_*` 옵션을 설정한다.
3. 첫 검토 — `:86-96`: 기존 `GetModuleHandleExA` 및 모듈+오프셋 출력은 유지한다. `:97-114`는 주소를 `DWORD64`로 전달하고, `SYMBOL_INFO`의 이름 버퍼에 정렬과 충분한 공간을 제공하며 `SizeOfStruct`를 `ULONG`으로 지정한다. `SymFromAddr` 성공 시 이름과 변위를 출력하고, `IMAGEHLP_LINE64::SizeOfStruct`를 `DWORD`로 지정해 파일명·행을 출력한다.
4. 두 번째 검토 — `:79-84`의 초기화 상태는 mutex 아래에서만 읽고 쓴다. `:98-113`의 DbgHelp 주소·구조체 타입이 API 타입과 맞고, 줄 조회 구조체는 0 초기화 후 크기를 설정한다. 함수명 또는 줄 정보가 없는 프레임도 기존 모듈+오프셋을 유지한다.
5. 두 번째 검토 — CMake `:923-934`: `/O2`, `/Zi`, 링크 최적화 및 디버그 정보는 MSVC Release 구성의 테스트 실행 파일에만 생성된다. Debug 구성의 `/RTC1`과 충돌하지 않으며 제품 타깃에는 적용되지 않는다. `/DEBUG`는 실행 파일 옆 PDB 생성을 요청한다. Windows에서 컴파일·실행은 이 환경에서 하지 않았다.

POSIX 코드와 출력 경로(`allocation_counter.cpp:68-71, 116-123`)는 변경하지 않았다. 프레임별 심볼 출력은 플랫폼 API가 달라 별도 분기지만, 공통 주소·개행 출력은 한 곳에 있다. 같은 출력 로직이 중복된 변경은 없다.

## 검증

- `scripts/format/format.sh --check cpp`: 통과.
- `git diff --check`: 통과.
- Linux Release 빌드 및 실행: `/root/jobs/fx/cx/v21/s033/core124/verdict.md`가 확인되지 않아 지시 조건상 실행하지 않았다. 따라서 실행 로그는 없다. Linux 출력은 코드 경로상 변경되지 않았다.
- 테스트 카운트나 기대값은 수정하지 않았다.

## 예방 제안

PR에서 `*_allocations*.cpp`, `allocation_counter.*`, `*_cost.cpp` 경로가 추가·수정되면 win-x64 Framework C++ 테스트를 필수 실행하는 path-filtered PR job을 둔다. 대안으로 같은 경로 변경에 붙일 전용 라벨이 있을 때 framework-cpp win-x64를 dispatch하고, 결과를 merge check로 요구할 수 있다. 현재 `.github/workflows/framework-cpp.yml:283-284`는 Windows 실행을 schedule/manual/release로 제한하므로 PR에서 allocation 기대값을 바꾸어도 merge 전 Windows 결과를 보장하지 않는다. 워크플로 파일은 수정하지 않았다.
