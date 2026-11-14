# #4420 C++ run 종료 후 종료 요청

- 원인(기준 6350275fac): `framework/languages/cpp/framework/src/runtime/host/app.cpp:3462-3474,3480-3486`에서 자원을 닫아도 termination operation은 미완료였다. 이후 `:4387-4404`가 새 worker를 만들고 `:4514`가 닫힌 channel runtime을 읽어 SIGSEGV가 발생했다. 기존 stack: `/root/jobs/fx/cx/v21/s033/cbind2/raw/gdb-run-01.log`.
- 변경: `app.cpp:573`의 공통 함수로 terminal/result/waiter 게시를 모았다. `:3492-3516`에서 기존 `started`로 teardown을 예약하고 정상·실패 teardown을 통합했다. 먼저 시작된 worker는 자원 접근 완료를 기다린 뒤 teardown하고 join한다. `shutdown()` 본문과 bench source는 변경하지 않았다. 변경 파일은 app.cpp와 `tests/Zlink.Framework.UnitTests/test_cpp_framework_http_startup.cpp` 두 개다.
- .NET 동작: `framework/languages/dotnet/src/Zlink.Framework/Runtime/Host/ZLinkFrameworkHostRuntimeCoordinator.cs:114-133`은 일반 시작 실패 후 자원을 닫는다. 같은 디렉터리 `ZLinkFrameworkRuntime.cs:323-327,365-381,455-462,1476-1480`은 이미 닫힌 runtime의 drain/stop을 완료로 처리한다. `ZLinkFrameworkMaintenanceRuntime.cs:573-590`에 따라 결과는 `Stopped/None`이며 `:310-313,333-334`처럼 완료 결과를 재사용한다.
- 소유 계층: Framework Host의 `termination_operation`이 종료 operation과 terminal 결과를 소유한다. `run_active/teardown_complete`는 기존 teardown barrier로만 사용한다.
- spec: `framework/doc/framework/common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md` §1(공유 operation과 결과 조합), `01-system-structure.ko.md:68-76`(Host 자원 수명).
- 교차언어: .NET의 이미 닫힌 runtime 처리와 결과 재사용을 따랐다. Java `ZLinkFrameworkRuntime.java:764-769,1859-1873`(Kotlin 공유), Node `packages/framework/src/runtime/host/index.ts:1095-1107,1378`도 종료 operation/결과를 재사용한다.
- 변경 분류: **B — 기존 결함 수정**. public API와 spec 변경 없음.

| 판정·게시 위치 수 | 전 | 후 |
|---|---:|---:|
| worker 시작/기존 결과 선택 (`shutdown`) | 1 | 1 |
| run teardown 완료 게시 | 2 | 1 |
| terminal 결과 게시 | 1 | 1 |

새 runtime flag·counter·queue·lock·timer는 없다. 중복 teardown과 terminal 게시 누락 2건을 같은 변경에서 처리했으며 넘긴 항목은 없다.

- 수정 전: Release, binding source ON, Core 1.25.0, -j3, CPU 6-7,14-15. 빌드 rc 0 및 binary가 두 source보다 최신임을 확인했다. [빌드](raw/build-before.log), [검증 기록](raw/build-before-meta.json).
- 신규 테스트 수정 전: 6개 중 request_stop, stop, shutdown 두 번, 실패 teardown 중 shutdown은 모두 `-11`; 정상 종료 후 재호출과 worker 선행 경합은 통과했다. [집계](raw/test-before-results.json), [요청한 첫 재현 로그](raw/test-before-RequestStopAfterFailedRunReturnsCompletedShutdown.log).
- 수정 후: **빌드-slot 대기 중**. 아직 수정 바이너리 테스트는 실행하지 않았다. [빌드 로그](raw/build-after-focused.log).
- 기존 테스트 예정: `ctest -R '[Hh]ttp|[Ss]hutdown|[Tt]ermination|[Ll]ifecycle|[Aa]pp|[Hh]ost'`, 34개. 전체 이름과 command는 [목록](raw/selected-tests.json)에 기록했다.
- bench 수정 전: stats 점유 `-11` 10/10. 원본 `/root/jobs/fx/cx/v21/s033/cbind2/raw/before-stats-results.json`; [집계](raw/bench-before-summary.json). 수정 후 10회 실행은 대기 중이다.
- format: 최종 테스트 후 실행 예정. `git diff --check`는 통과했다.
- 로컬 커밋: 검증 완료 후 생성 예정. push/GitHub/internal-docs/fetch/pull/다른 worktree 변경은 수행하지 않았다.
