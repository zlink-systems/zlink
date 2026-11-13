# #4190 .NET 결과

**결과**
- 승인된 §10.2–§10.3을 적용했다. 커밋 `8f9741b876` (`bench/4190-s4190d`, push 안 함).
- 제출 thread 수 판정 위치는 수정 전 request-backpressure의 공통·raw 두 루프와 send `Task.Run` 경로 3곳, 수정 후 공통 thread 생성 helper 1곳(`BenchMeasurement.cs:415-424`)이다. 패턴별 수는 backpressure `:250-251`, send `:377`, serial `:216-217`에서 정한다.
- sequence는 공유 atomic allocator(`Client/Program.cs:896-900`)로 stream 간 유일성을 유지한다. stream별 집계는 `BenchMeasurement.cs:334-335`, `:410-411`에서 phase 끝에 한 번 병합하고, 병합 구현은 `Client/Program.cs:782-809`다.

**바꾼 것**
- `Client/BenchMeasurement.cs`: request-backpressure·send-saturation을 dedicated `Thread`로 제출하고 stream별 counters·in-flight·latency samples를 병합한다. serial도 한 dedicated thread로 실행한다.
- `Client/Program.cs`: raw submission gate를 제거하고 별도 completion thread에서 poller를 구동한다. stream metadata를 8/1/1(혹은 backpressure 상한 없음)으로 기록한다.
- `Client/BenchHttpApplication.cs`, `BoundaryTests/Program.cs`, `dotnet/run_local.sh`: trigger와 CLI 이름을 `submitConcurrency`/`submit-concurrency`로 변경했다.
- `runner_common.sh:10,193`: 지정된 공통 변수·trigger 두 줄만 변경했다.

**검증**
- `bash framework/bench/grpc/dotnet/build.sh`: 성공, 오류 0, Release Framework assembly 복사 검증 완료. MSBuild 중복 import 경고 10건은 기존 경고다. 로그: `build-final.log`.
- BoundaryTests: serial, window, request-backpressure, send-saturation 모두 PASS. WireTests: 29B wire identity PASS. 로그: `boundary-tests.log`, `wire-tests.log`.
- `run_local.sh`로 raw binding·Framework의 request-backpressure, send-saturation, request-serial 각 1024B를 warmup 5초·active 5초 실행했다. 여섯 셀 모두 completed > 0, errors 0, runner count 검증 통과. JSON에서 submitConcurrency=8, backpressure/send streams=8, serial streams=1을 확인했다. 로그·결과: `smoke.log`, `smoke/run1/`.
- Framework request-backpressure source PID 3292909의 `/proc/<pid>/task/*/comm`에서 `bench-submit-0`…`bench-submit-7`을 확인했다. 전체 thread 목록은 `submit-threads.txt`.
- `bash -n`과 `git diff --check` 통과. 빌드 산출물 디렉터리는 정리했다. 언어별 stray `framework/bench/grpc/log/` 결과 파일은 발견되지 않아 제거한 파일은 없다.

**리팩토링 점검**
- 성능 0건, POSDDD 0건, 불필요 코드 3건 발견. raw submit gate, 중복 raw backpressure loop, 전용 admission observer를 제거했다. 넘긴 항목 0건.

**남은 문제**
- 없음.
