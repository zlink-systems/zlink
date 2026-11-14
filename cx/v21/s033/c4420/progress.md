# #4420 구현 전 결정

- branch: fix/4420-cpp-shutdown-after-run. 시작 시 기존 변경 없음. 사용자 지시에 따라 내부 문서·GitHub·fetch/pull·다른 worktree를 사용하지 않는다.
- 분류 B: 기존 Framework host lifecycle 결함. 수정 범위 app.cpp와 HTTP startup unit test. 사용자가 이 원인과 수정 방향을 명시적으로 승인했다.
- 규범: framework/doc/framework/common/spec/server/languages/cpp/interfaces/02-configuration-host.ko.md:95-104 (공유 operation, terminal result), :142-150 (stopped/none, force_stopped/teardown_failed). Host resource owner는 같은 경로 01-system-structure.ko.md:68-76.
- .NET: Runtime/Host/ZLinkFrameworkHostRuntimeCoordinator.cs:114-133은 일반 startup failure에서 CloseResourcesAsync를 수행한다. ZLinkFrameworkMaintenanceRuntime.cs:310-313,333-334는 이미 완료된 결과를 재사용한다. :571-590은 성공적인 정리를 Stopped/None으로 매핑하며 startup 오류 자체를 TeardownFailed로 매핑하지 않는다.
- 결정: run 종료 후 worker 시작 가능 여부의 유일한 소유자는 termination_operation(mutex 보호)이다. run teardown 시작 시 기존 started로 종료 operation을 예약한다. shutdown은 기존 terminal/started 검사만 유지한다. 이미 worker가 시작됐으면 stop_requested까지 기다려 worker의 framework 자원 접근이 끝난 다음 자원을 닫는다. teardown_complete/run_active는 기존 teardown barrier로만 유지한다. teardown 완료 후 worker를 join하거나, worker가 없으면 Stopped/None을 공통 terminal 게시 함수로 확정한다.
- 정상·실패 run의 중복 teardown을 하나로 모으며 terminal/result/waiter 게시도 기존 :4680-4692 코드를 한 함수로 이동해 공유한다. 새 runtime flag/counter/queue/lock/timer는 추가하지 않는다.
- 대안: shutdown에서 teardown_complete/run_active를 별도로 검사하는 방식은 종료 판정 위치와 상태 간 의존을 늘리므로 선택하지 않는다.

| 결정 위치 수 | 전 | 후(계획) |
|---|---:|---:|
| worker 시작/합류/완료 재사용 판정(shutdown) | 1 | 1 |
| run teardown 완료 게시 | 2 | 1 |
| terminal 결과 게시 | 1 | 1 |

- 완료 조건: 신규 테스트 수정 전 실패 로그, 수정 후 통과; 기존 Http/Shutdown/Termination/Lifecycle/App 및 host 관련 unit test; main bench source 그대로 stats 점유 10회 정상 비제로 종료; format --check cpp; 로컬 commit.

## 교차언어 대조

- Java ZLinkFrameworkRuntime.java:764-769,960-969: startup rollback도 close를 사용한다. :1859-1873: activeTermination이 있으면 independent waiter를 반환해 완료 결과를 유지한다. Kotlin은 이 runtime을 공유한다.
- Node packages/framework/src/runtime/host/index.ts:1095-1107: shutdownOperation을 재사용하며 이미 실행한 teardown operation에 합류한다. :1378 이후 completeTermination에서 결과를 한 번 게시한다.
- 공통 결론: 실패한 startup의 이미 정리된 runtime을 새 종료 worker가 다시 읽을 이유가 없다. 신규 API나 spec 변경 없이 기존 C++ 결함 B로 처리한다.
- 테스트 범위를 6개로 보완: teardown 진입 후 shutdown 합류, shutdown worker 시작 후 startup failure의 두 경합 방향도 public hosted_service barrier로 검증한다.
- .NET 추가 근거: ZLinkFrameworkRuntime.cs:323-327,365-381,455-462는 이미 제거된 state의 stream/Spot/channel drain을 완료로 처리한다. :1476-1480은 이미 Stopped인 StopAsync를 즉시 반환한다. 이 경로가 MaintenanceRuntime.cs:573-576의 Stopped/None으로 이어진다.
- 경합 barrier: worker의 stop_requested 게시를 기존 termination_teardown_mutex 임계구역 안으로 옮기고 기존 condition_variable을 알린다. run은 같은 mutex/predicate로 worker의 자원 접근 완료를 기다려 알림 유실 없이 teardown한다.

| 결정 | spec/최종 소유자 | 현재 판정·게시 위치 |
|---|---|---|
| 새 종료 시작/기존 결과 재사용 | C++ configuration-host §1 / Host termination_operation | cpp app.cpp:4387-4404; .NET MaintenanceRuntime.cs:310-330; Java Runtime.java:1859-1873(Kotlin 공유); Node host/index.ts:1095-1107 |
| terminal 결과 확정 | 같은 §1 결과 조합 / Host shared termination operation | cpp app.cpp:4680-4692(실패 run 경로 누락); .NET MaintenanceRuntime.cs:571-600; Java Runtime.java:1890-1920; Node host/index.ts:1378 이후 |

- 수정 전 bench 기록 확인: cbind2/raw/before-stats-results.json과 before-trigger-results.json 모두 -11 10/10. 새 raw/bench-before-summary.json에 원본 경로와 집계만 기록했다.
- 수정 전 빌드는 공유 quiet guard의 측정 보호에 따라 일시 정지/재개되고 있다. measure.lock을 직접 취득하거나 다른 작업의 프로세스·worktree·출력을 변경하지 않는다. 현재 빌드 설정과 source 동일성은 raw/build-config.txt 및 git diff로 확인했다.

## 수정 전 재현 및 적용

- 수정 전 빌드 rc 0, binary > app.cpp/test source 확인: raw/build-before-meta.json.
- 신규 6개 테스트를 별도 프로세스로 실행했다. request_stop, stop, shutdown 두 번, failure teardown 중 shutdown 네 경우 모두 -11. 정상 run 후 재호출 및 worker가 먼저 시작된 경합은 rc 0. raw/test-before-results.json과 test-before-*.log.
- 재현 뒤에만 app.cpp patch를 적용했다. shutdown 함수 본문은 변경하지 않았다. 공유 게시 함수를 옮기고 run 공통 teardown에서 기존 started를 예약한다. 기존 worker가 있으면 자원 접근 완료 신호를 기다린 뒤 teardown과 join을 완료한다.

## 수정 후 focused 검증

- 수정 후 빌드 rc 0. raw/build-after-meta.json에서 binary 최신성을 확인했다.
- 신규 6개 모두 rc 0: raw/test-after-results.json 및 test-after-*.log. 수정 전 -11 4개도 모두 통과했고 정상 종료 대조 2개도 유지됐다.
- 기존 CTest 34개와 bench_cpp_client를 같은 worktree build-c4420에서 빌드 중이다. bench target만 raw/bench-*.cmake로 추가했으며 bench source는 HEAD와 동일하다. 최종 CTest 발견 결과도 동일한 34개다.

## 완료

- 기존 테스트·bench 빌드 rc 0, 실행 파일 17개 최신성 확인.
- 기존 CTest 34/34 통과(19.14초), 신규 6/6 통과.
- main bench source의 stats 점유 재현 10/10 exit 2. SIGSEGV 없음. 각 로그의 bind 실패 포트와 host rc=1 확인.
- 전체 C++ format check rc 0, diff check 통과. 빌드 크기 6.2GB(25GB 이하).
- 로컬 커밋 bc1d62e53a582c4d2a8aa141e83c1d5a0a62e6ce. working tree clean. 최종 결과와 로그 경로는 report.md 및 raw/final-validation.json.
