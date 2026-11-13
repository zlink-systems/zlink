| variant | caller µs/request | req/s | source CPU µs/request |
|---|---:|---:|---:|
| V0 원본 main | 18.008 | 50,106 | 81.734 |
| V1 request task 유지 | 9.632 | 64,080 | 50.453 |
| V2 native awaitable 전달 | 6.767 | 71,194 | 31.394 |
| V3 driver → binding 직접 호출 | 5.831 | 73,365 | 27.742 |

| variant | bench driver | Framework call layer | task/coroutine | binding call 자체 | kernel/기타 |
|---|---:|---:|---:|---:|---:|
| V0 | 1.872 | 4.473 | 4.331 | 5.274 | 2.058 |
| V1 | 2.358 | 0.763 | 1.408 | 3.238 | 1.865 |
| V2 | 1.895 | 0.193 | 0.021 | 2.752 | 1.906 |
| V3 | 1.498 | 0.000 | 0.022 | 2.641 | 1.671 |

계층 표의 단위는 µs/request다. 각 profile의 caller 표본 비율을 같은 변형의 무프로파일 3회 caller 중앙값에 정규화했다. 함수의 inclusive 시간을 합한 값이 아니다. Frame-pointer symbol에 귀속한 추정치다. Inline된 call/observer 비용은 주변 caller, 특히 driver 항목에 남을 수 있다. 그 부분의 독립 µs와 추가 DWARF inline 정밀 분리는 측정 못 함이다.

V1에서 Framework의 반복 요청 encode·envelope/service framing·C1/C2 등록·operation owner turn을 생략한 caller floor는 9.632 µs/request다. 6.6 µs 예산을 3.032 µs 초과한다.

V2에서 Framework의 반복 요청 encode·envelope/service framing·C1/C2 등록·operation owner turn을 생략한 caller floor는 6.767 µs/request다. 6.6 µs 예산을 0.167 µs 초과한다.

Framework call layer까지 생략한 V3는 5.831 µs/request다. 6.6 µs 예산에서 0.769 µs가 남는다. 이 여유는 중앙값을 기준으로 계산한 산술값이다.

V2의 남은 caller 비용에서 가장 큰 계층은 binding call itself 2.752 µs/request, kernel/other 1.906 µs/request, bench driver 1.895 µs/request다.

V1−V2는 +2.865 µs/request, V2−V3는 +0.936 µs/request다. 앞의 차이는 request task/receiver와 그 제출 연결을 생략한 변형 사이의 차이다. 뒤의 차이는 같은 native wrapper와 driver에서 public call 객체 생성·timeout 지정·terminal 호출을 생략한 차이다. 각각 3회 중앙값의 차이이며 개별 함수의 독립 실행 시간으로 해석하지 않는다. V1은 native reply vector를 completion observer에서 해제하고, V2/V3는 driver slot에서 소비·해제하므로 정리 위치의 차이도 포함한다.

V2 실행 범위는 6.188–6.945 µs/request, V3는 5.605–6.624 µs/request다. Floor와 예산 비교는 3회 중앙값을 기준으로 했다. 이 값은 이번 driver·연결·부하 조건의 관측값이다.

Framework÷binding ≥ 0.6 및 caller 6.6 µs 기준은 유지했다. 이번에는 binding 행을 재측정하지 않았다. 이 실험의 반복 요청은 동일한 correlation·sequence·deadline을 사용하고, V1의 public reply는 빈 typed 값으로 완료하고, V2/V3는 native reply를 driver slot으로 직접 받는다. Reply decode와 내용 검증은 생략했다. 제품 동작을 유지한 경로에서 목표를 달성했는지는 측정 못 함이다.

V0는 2f8b8e635a의 원본 바이너리다. V1은 public request_to_node → async → request_task/receiver를 유지하고, erased submit 시작점에서 binding request로 직접 연결한다. 실제 binding reply가 도착한 뒤 public task를 완료한다. V2는 같은 public call 생성과 timeout 지정을 유지하되 측정 전용 cfloor_async terminal로 native submission을 반환한다. Bench의 wrapper는 native awaitable을 기다리는 coroutine 하나이며, native reply vector를 driver slot으로 옮긴다. Framework task 상태를 만들지 않고 native continuation scheduler도 캡처하지 않는다. Native 결과가 도착할 slot의 주소를 유지하도록 deque를 사용하며, phase 소유자가 늦은 완료까지 slot 수명을 보유한다. Admission 대기는 phase별 readiness 상태 하나로 유지한다. V3는 동일한 wrapper와 in-flight/admission/완료 관찰 루프에서 binding helper를 직접 호출한다.

V1–V3는 phase마다 정상 요청 한 번으로 실제 native frame을 준비한 뒤 같은 frame을 반복한다. 준비 요청도 submitted/completed/target received에 포함한다. 반복 요청에서도 bench의 1 KiB typed payload 생성·stamp, slot 관리, counters, admission 확인, completion signal, latency 기록은 유지한다. V3에서도 payload 생성이 최적화로 없어지지 않도록 compiler barrier를 뒀다. 소켓·연결·기존 completion poller를 재사용하며 새 poller나 worker를 만들지 않았다. 모든 target은 bin-V0의 동일한 Framework server 바이너리다.

조건 B는 source CPUs 0–7, target CPUs 8–15다. Request-backpressure, application payload 1024바이트, warmup 30초와 10 segments, active 5초, RUNS=3을 사용했다. 각 셀의 submit_thread_cores × 1e6 / throughput을 먼저 계산한 뒤 중앙값을 구했다. Source CPU는 client_cores × 1e6 / throughput이다. 각 열은 독립 중앙값이다. Profile은 변형마다 warmup 30초, active 20초 실행 중 약 8초를 perf cpu-clock 199 Hz, frame-pointer call graph로 수집했다.

계층 분류는 ccall의 leaf→caller 소유 함수 탐색을 사용한다. Allocator와 동기화 함수는 바깥의 소유 함수로 분류한다. 기존 encode/registration 경로는 Framework call layer로, request/task/receiver/coroutine 상태는 task/coroutine으로, bench loop와 completion_signal은 driver로 분류한다. Binding에는 C++ binding과 Core 사용자 공간 실행, 측정용 native_request_t의 part 복사·builder 호출을 포함한다. Kernel leaf와 소유자를 찾지 못한 표본은 kernel/기타에 둔다. Public call factory 및 그 callback의 생성·해제는 call layer에, 일반 task/result 저장소는 task/coroutine에 둔다. Template 인자에 등장한 BenchPayload를 payload 함수의 소유자로 판정하지 않는다. Template·inline·unwind 한계가 있는 표본 추정이므로 kernel/기타 전체를 제거 가능한 Framework 비용으로 간주하지 않는다.

| variant | 실행별 caller µs/request | caller 범위 µs/request |
|---|---|---|
| V0 | 18.008, 18.231, 17.875 | 17.875–18.231 |
| V1 | 9.443, 9.783, 9.632 | 9.443–9.783 |
| V2 | 6.945, 6.767, 6.188 | 6.188–6.945 |
| V3 | 5.605, 5.831, 6.624 | 5.605–6.624 |

| variant | profile caller TID | profile caller µs/request | caller cores | caller 표본 수 | capture req/s |
|---|---:|---:|---:|---:|---:|
| V0 | 140366 | 18.882 | 0.903 | 1260 | 47,817 |
| V1 | 205217 | 10.804 | 0.632 | 821 | 58,519 |
| V2 | 213834 | 7.554 | 0.505 | 632 | 66,910 |
| V3 | 222416 | 7.158 | 0.444 | 541 | 62,031 |

원본 결과는 log/V0–V3/run1–3/에, profile과 schedstat snapshot은 profiles/V0–V3/run1/에 있다. 계산값·계층 표·caller 함수별 self/inclusive 값은 [summary.json](/root/jobs/fx/cx/v21/s033/cfloor/summary.json)에 보존했다. Caller TID는 실제 driver loop가 있는 stack으로 식별했다. Driver callback의 함수 이름만 나타나는 completion thread는 caller로 선택하지 않았다.

검증 결과는 모든 측정 셀에서 submitted=completed=target received, errors=abandoned=header_validation_failures=0이며 contaminated·drain_bound_hit가 없었다. 집계 검사는 실제 정상 입력 통과와 고의 errors·received 불일치·contaminated·drain_bound_hit 입력 4종 거부를 확인했다(negative-check.log). Source의 내용 검증은 V0에서만 수행한다. V1–V3의 개수 일치는 native reply 도착과 handler 수신 계수의 일치이며 typed reply의 정확성을 검증한 결과가 아니다.

Local package는 C++ binding 1.23.0과 release Core 1.23.0으로 build/install/verify했고, 잘못된 Core 1.22.0 거부도 확인했다. Git 조회와 bindings/Core 갱신을 피하도록 package wrapper 대신 같은 C++ child script의 repo root를 고정했다. 기존 dependency install을 사용하고 vcpkg manifest install을 껐다. Bench는 cpp/build.sh로 Release, -O3 -DNDEBUG -g -fno-omit-frame-pointer를 사용했다. C++ compile은 cpp-build-slot.sh, 그 밖의 공유 검증은 measure-shared.sh를 사용했고, 모든 실행은 flock -o /root/jobs/fx/cx/measure.lock 아래 수행했다.

메모리 감시는 MemAvailable 12.921 GiB, 12.983 GiB에서 총 2회 SIGTERM으로 빌드를 중단했다. 이후 컴파일 동시 수를 2에서 1로 줄였다. 실행 코드의 compiler flag는 바꾸지 않았다. 기록한 최저 MemAvailable은 12.921 GiB다. 메모리 표본은 memory-build.jsonl과 memory-samples.jsonl에 있다. SIGTERM/SIGINT만 사용했고 셸 & 작업은 만들지 않았다.

측정용 수정 파일은 다음과 같다. 실험 소스는 experiment-source/, 전체 패치는 final.diff에 보존했다. 측정 후 임시 소스 9개를 원복했고 git diff는 비어 있다. 측정 바이너리와 원본 결과는 jobs 경로에 유지했다.

- framework/bench/grpc/cpp/client/bench_cpp_client.cpp
- framework/languages/cpp/framework/include/zlink/framework/contracts/channels/call.hpp
- framework/languages/cpp/framework/include/zlink/framework/detail/cfloor_measurement.hpp
- framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp
- framework/languages/cpp/framework/src/runtime/channels/channel_runtime.cpp
- framework/bench/grpc/cpp/run_local.sh
- framework/bench/grpc/dotnet/run_local.sh
- framework/bench/grpc/java/run_local.sh
- framework/bench/grpc/node/run_local.sh

Core·binding·spec 파일은 수정하지 않았다. zlink 저장소의 commit·push·branch 변경과 GitHub issue/PR 작업, sub-agent·다른 model 호출은 하지 않았다. 리팩토링 점검은 measurement-only 범위에 적용하지 않았다.

착수 때 internal-docs.sh를 실행해 Git rev-parse와 외부 docs 저장소 pull이 실행됐다. 사용자 Git 제한을 놓친 실행이다. 이후 직접 실행한 Git 명령은 허용된 apply/diff뿐이다.
