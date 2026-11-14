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
