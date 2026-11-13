BUSY 재시도를 고정 8회 대신 기존 30초 종료 기한으로 바꿨다. Reply/close 관련 검사와 최종 부하 경합 1,000회는 통과했다. 전체 CTest는 297/299개 통과, 2개 실패였다. 성능 측정 세 조건의 Framework÷binding은 모두 0.6 미만이었다.

`raw_route_port_t::reply()`는 기존 mutex 안에서 `_socket == nullptr`를 확인하고 binding operation을 만든다. `raw_route_port.cpp:542`에서 mutex를 풀고 native submit을 호출한다. Operation은 native handle과 weak runtime 참조를 보관하며 submit은 runtime의 strong 참조를 얻는다. C++ socket wrapper를 다시 읽지 않는다. Native admission과 close 수락은 binding/Core가 판정한다.

C++의 기존 기본 종료 기한은 `framework/languages/cpp/framework/include/zlink/framework/contracts/configuration/app.hpp:100`의 `app_t::shutdown()`에 있던 30초다. 같은 값이 `framework/languages/cpp/framework/src/runtime/host/app.cpp:495`에도 있었다. 현재 정의는 `framework/languages/cpp/framework/include/zlink/framework/contracts/configuration/lifecycle.hpp:13`의 `detail::default_termination_deadline` 한 곳이며 두 경로가 이를 참조한다.

`close_route_socket()`은 `framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:118` 한 곳에서 BUSY만 다시 시도한다. 고정 시도 횟수는 없다. 1ms 뒤와 절대 기한 중 빠른 시각까지만 기다리며 기한이 지나면 마지막 close 오류를 그대로 전달한다. 성공한 close 뒤에만 `unique_ptr`를 비운다. Mesh `close():654`와 client/server server `close():311`은 호출 입구에서 기한 하나를 만들고 자원 정리에 전달한다. 정리 함수와 재시도 loop에서 기한을 새로 만들지 않는다. Retained cleanup의 다음 호출에는 새 기한을 사용한다.

짧은 테스트 기한은 공통 함수의 절대 기한 인자로 주입했다. Constructor parameter와 저장 필드를 추가할 필요가 없었다. Runtime의 새 queue·lock·flag·counter·timer·member state는 없다.

아래 Framework 파일 경로는 `framework/languages/cpp/framework/src/` 기준이다.

| 종료 위치 | 진행 중인 reply와 겹칠 수 있는가 | 처리 |
|---|---|---|
| `runtime/backend/raw_route_port.cpp:552`, port close | 그렇다 | 기존 mutex로 poller를 분리하고 `:563`에서 socket 포인터를 비운다. 이후 reply는 null 검사에서 거절된다. Borrowed socket을 파괴하지 않는다. |
| `runtime/backend/raw_route_port.cpp:118`, 명시적 close와 `:126`의 reset | 그렇다 | Core BUSY에만 기한 안에서 다시 시도한다. 성공 뒤 wrapper를 파괴한다. 실패하면 소유권을 유지한다. |
| `runtime/mesh/raw_mesh_node_owner.cpp:654`, owner close | 그렇다 | `:67`의 공통 정리 함수가 port, poller, router를 정리한다. 실패한 자원은 owner에 돌려준다. |
| 같은 파일 `:477`의 destructor와 `:489`의 retained cleanup | 그렇다 | 같은 정리 함수를 사용한다. 실패한 context·mutex·port·poller·router를 기존 failure collector가 보관한다. |
| 같은 파일 `:512`의 start와 `:566`의 router 대입 | 정상 start 수명에서는 아니다 | 기존 `_closed`를 `:666`의 close turn에서 설정해 close 중 재시작과 보존된 router 덮어쓰기를 막는다. 새 상태를 추가하지 않았다. |
| `runtime/client_server/raw_client_server_owner.cpp:311`, server close | 그렇다 | `:158`의 공통 자원 정리가 port와 monitor를 먼저 분리하고 `:174`에서 같은 router close 함수를 사용한다. 실패한 소켓을 owner에 돌려준다. |
| 같은 파일 `:216`의 destructor와 retained cleanup | 그렇다 | 동일한 자원 정리를 실행하고 실패한 자원을 기존 collector에 보관한다. |
| 같은 파일 `:231`의 start와 `:279`의 router 대입 | 정상 start 수명에서는 아니다 | 기존 port와 closed 검사가 실행 중 또는 종료 뒤 소켓 교체를 막는다. |
| 같은 파일 `:285`의 외부 router 사용 | 외부 owner에 달렸다 | Framework가 wrapper를 소유하지 않는다. 외부 owner가 port close 뒤 명시적 socket close와 실패 처리를 완료한다. Fixture도 이 순서를 사용한다. |
| `bindings/cpp/src/Runtime/Sockets/socket.cpp:53`, destructor와 `Runtime/Native/socket_handle.hpp:28`의 destructor | 보호 없이 파괴하면 그렇다 | 두 destructor는 native close 실패를 반환하지 않는다. Prototype 대조군에서 두 번의 실제 BUSY 뒤 native socket이 열려 있음을 확인했다. Production owner는 명시적 close 실패 때 wrapper를 파괴하지 않는다. |
| `bindings/cpp/src/Runtime/Sockets/socket.cpp:62`, move assignment | Framework의 해당 경로에서는 호출하지 않는다 | Framework는 binding 객체가 아니라 `unique_ptr`를 이동한다. |

Close가 operation 생성보다 먼저 port를 닫으면 기존 null 검사로 false를 반환한다. Operation 구성 뒤 native 진입 전에 close가 수락되면 binding/Core 오류가 기존 submit error 경로로 처리된다. Native admission이 먼저 수락되면 Core가 BUSY 또는 안전한 close를 판정한다. Context를 유지한 socket close 경합을 검사했다.

집중 검사 37개는 `logs/deadline-focused.log`에서 모두 통과했다. 전체 Debug suite에서도 일반 경합 100회와 결정적 경합 2회가 통과했다. 일반 경합의 binding 결과는 OK(0) 3,774건이었다. 결정적 경합은 실제 Core BUSY 한 번 뒤 다음 close가 성공했고, binding 결과는 OK 1건과 TERMINATED(4) 1건이었다. 제출 직전에 멈춘 operation보다 close를 먼저 끝내는 경우도 검사했다. 특정 race 오류 코드만 허용하는 assertion은 없다. Port close 뒤 reply가 null 검사에서 거절되고 native 호출이나 binding 오류를 만들지 않는지 확인했다. 외부 buffer의 free callback으로 모든 reply message가 소비되거나 해제됐는지도 확인했다. 최종 결과는 `logs/ctest-unit-contract-LastTest.log`와 `test-summary.json`에 있다.

기존 일반 close failure 검사의 주입 오류는 BUSY에서 INTERNAL_ERROR(404)로 바꿨다. BUSY는 새 정책에서 정상 재시도 대상이므로, 즉시 실패 전파와 자원 보존 검사는 재시도하지 않는 오류로 계속 확인한다. 실패 assertion은 유지했고 mesh 재시작 거절 assertion을 추가했다.

계측은 test executable의 native reply wrapper로 OK와 Core 반환값을 기록하고, reply thread에서 발생한 binding `submit_error_t`를 test 전용 C++ exception wrapper로 집계한다. Native 진입 전 binding 오류도 포함한다. Runtime에는 이 계측을 추가하지 않았다.

20ms 동안 계속 BUSY인 검사에서 기한이 만료됐고 오류와 소유권이 보존됐다. BUSY 주입을 풀고 다음 close에서 정리했다. 같은 만료 기한을 다음 소켓에 전달하면 한 번의 close 뒤 즉시 BUSY를 전달한다. BUSY 외 오류는 한 번만 전달한다. 25ms 동안 BUSY를 유지한 경우에도 close가 성공했다. 관련 검사에서 25회 시도했으며 고정 8회 제한이 사라졌음을 확인했다.

`logs/deadline-prototype-destructor-control.log`의 최종 Debug 대조군은 prototype만 적용한 backend와 binding destructor를 사용했다. 100회 모두 실제 BUSY 두 번 뒤 native handle의 직접 close가 OK(0)였다. Native socket이 열린 채 남아 있었음을 확인하고 해당 직접 close로 정리했다. 종료 코드 1은 의도한 실패다. `negative-control.json`과 `prototype-control-source.json`에 입력과 결과를 보존했다.

`logs/stress-race-1000.log`의 최종 Debug 부하 경합 1,000회가 통과했다. 모든 close가 성공했고 모든 native handle의 후속 close는 SHUTDOWN(402)이었다. 이 실행에서는 자연 경합의 close가 모두 첫 시도에 성공했다. Binding 결과와 native 결과는 각각 OK(0) 10,177건, TERMINATED(4) 1건이었다. 모든 reply buffer의 소비 또는 해제를 확인했다. 같은 부하 실행에서 20ms BUSY 만료·소유권 보존·다음 close 정리와 25ms BUSY 뒤 성공 검사도 통과했다. `measure-shared.sh` 안에서 CPU 0–15에 `stress-ng --cpu 16 --cpu-load 90`을 실행했고 검사 뒤 SIGTERM으로 종료했다. 명령·종료 상태는 `stress-status.json`, 부하 로그는 `logs/stress-ng.log`다.

보존한 Release 부하 1,000회의 binding 결과는 OK(0) 37,663건과 INVALID_STATE(8) 1건이었다. Native 결과는 OK 37,663건이므로 INVALID_STATE는 native 호출 전에 발생했다. `logs/stress-race-1000-release.log`와 `stress-status-release.json`에 있다. Native만 집계한 별도 1,000회에서는 OK 24,380건과 NOT_FOUND(3) 1건을 관측했다. 이 로그는 `logs/stress-race-1000-native-counter.log`다. 두 실행도 모든 socket close와 message ownership 검사를 통과했다.

전체 unit·contract 검사는 생성 probe가 필요해 Debug·probe ON·install OFF로 구성했다. `verify_tooling_contract.cmake`가 사용하는 구성과 같다. Compiler 설정은 `-O0 -g1 -fno-omit-frame-pointer`와 GCC compile-time GC 설정이다. 전체 검사 299개에 필요한 target과 대조군 165개를 빌드했고, 마지막 411개 compile·link 단계가 모두 성공했다. 큰 object는 재사용했고 일반 단계는 `-j3`로 실행했다. 최종 빌드 로그는 `logs/build-unit-contract-final.log`다. 마지막 빌드는 41.60분 걸렸고, 관측한 최소 MemAvailable은 13.63GiB였다.

전체 unit·contract CTest는 299개 중 297개가 통과했고 2개가 실패했으며 skip은 없었다. 실행 시간은 164.74초였다. `logs/ctest-unit-contract.log`, `logs/ctest-unit-contract-LastTest.log`, `logs/ctest-unit-contract.xml`, `suite-status.json`에 보존했다. 전체 suite를 다시 실행하거나 assertion을 바꾸지 않았다.

남은 첫 실패는 `framework/languages/cpp/tests/Zlink.Framework.UnitTests/test_cpp_framework_operation_registry.cpp:983`이다. 128개 등록을 모두 해제하고 expire 한 번을 호출한 뒤 `!next_deadline() && index_size()==0`이라는 assertion이 실패했다. 해당 블록은 raw port나 reply/close 경로를 호출하지 않는다. `framework/languages/cpp/framework/src/runtime/foundation/operation_registry.cpp:746`의 scan은 `framework/languages/cpp/framework/src/runtime/dispatch/dispatch_limits.hpp:12`의 2ms budget이 지나면 중단하고 cursor를 보존한다. 단일 expire 호출에서 전체 정리가 끝난다는 테스트의 시간 의존성이 원인 후보이며, 이번 변경에서는 이 registry와 assertion을 수정하지 않았다. 격리 실행 1회는 통과했고 `logs/registry-isolated.log`·`logs/registry-isolated-LastTest.log`·`registry-isolated-status.json`에 남겼다. 격리 재실행 통과로 전체 suite의 실패를 해소했다고 판정하지 않았다.

두 번째 실패는 `framework/languages/cpp/tests/Zlink.Framework.ContractTests/verify_ctest_label_contract.cmake:88`이다. Bench의 add_subdirectory 구성에서 `framework/languages/cpp/CMakeLists.txt:2078`이 넘긴 build directory는 `framework/bench/grpc/cpp/build/framework`인데, CMake cache는 상위 `build/CMakeCache.txt`에 있다. 검사가 하위 `framework/CMakeCache.txt`를 읽으려다 실패했다. 기존 검사나 cache 경로 조건을 바꾸지 않았다.

성능 측정용 변경본은 test 구성으로 바꾸기 전에 `bin-direct/bench_cpp_framework_server`로 보존했다. 이 바이너리는 Release·probe OFF이며 기존 최종 source와 library를 연결했다. `CMakeCache-production.txt`, `production-binaries.json`, `binary-direct.json`에 설정과 해시를 보존했다. Probe 활성화 및 Debug test build는 이 성능 바이너리에 영향을 주지 않는다.

성능 수집 53회는 모두 유효했다. Unpaced 45회, paced perf·packet 6회, paced mutex 2회다. Binding도 같은 세션에서 새로 측정했다. 모든 실행에서 submitted=completed=target.received이고 errors·abandoned·header_validation_failures·target.errors는 0이었다. Contaminated와 drain_bound_hit는 false였다. 검사기는 완료 수·오류·abandoned·header 오류·혼입·drain 초과를 넣은 잘못된 입력 6종을 모두 거부했다. 원자료는 `runs/`, 검증·집계는 `summary.json`과 `logs/summary.log`, 실행 로그는 `logs/measurements.log`다.

모든 측정은 `flock -o /root/jobs/fx/cx/measure.lock`의 배타 잠금에서 실행했다. Condition B는 source CPU 0–7, target CPU 8–15다. 127.0.0.1 TCP와 sendConcurrency=8을 사용했다. Framework 두 variant의 source는 같은 보존된 main client이며 target만 main 또는 변경본이다. Main과 binding은 `creply/bin-main/`, 변경본 target은 `creply2/bin-direct/`의 바이너리다. Core와 C++ binding은 변경하지 않은 1.23.0이다. 각 `meta.json`에 binary hash·실제 로드한 Core·thread affinity를 보존했다. 측정 중 최소 MemAvailable은 24.919GiB였다.

Unpaced는 warmup 10초와 active 5초를 5회씩 실행했다. Variant 순서는 반복마다 main과 binding을 번갈아 앞에 두었다. 표의 µs/request는 처리율의 역수다. F÷B는 Framework throughput ÷ binding throughput이다. 요청당 예산은 binding µs/request ÷ 0.6이고, over budget by는 Framework µs/request에서 이 예산을 뺀 값이다.

| Pattern | Payload | 구현 | RUNS=5 중앙값 req/s | 최솟값–최댓값 req/s | F÷B | µs/request | over budget by µs/request |
|---|---:|---|---:|---:|---:|---:|---:|
| request-backpressure | 1KiB | binding | 273,219.9 | 262,048.9–283,613.4 | — | 3.660 | — |
| request-backpressure | 1KiB | main | 52,903.3 | 52,583.3–54,381.0 | 0.1936 | 18.902 | +12.802 |
| request-backpressure | 1KiB | 변경본 | 53,708.7 | 49,605.7–54,973.2 | 0.1966 | 18.619 | +12.519 |
| request-backpressure | 4KiB | binding | 155,076.1 | 148,142.7–160,879.3 | — | 6.448 | — |
| request-backpressure | 4KiB | main | 45,914.4 | 42,316.0–47,145.5 | 0.2961 | 21.780 | +11.032 |
| request-backpressure | 4KiB | 변경본 | 47,074.0 | 45,033.9–48,743.5 | 0.3036 | 21.243 | +10.496 |
| request-serial | 1KiB | binding | 16,756.1 | 16,494.7–17,020.9 | — | 59.680 | — |
| request-serial | 1KiB | main | 8,464.9 | 7,866.8–8,797.1 | 0.5052 | 118.135 | +18.669 |
| request-serial | 1KiB | 변경본 | 8,567.5 | 8,545.9–8,878.1 | 0.5113 | 116.720 | +17.254 |

| 40k paced 실행 | 실제 perf 구간 req/s | Reply data segments/request | 평균 data segment B | Retransmit delta |
|---|---:|---:|---:|---:|
| main 1 | 39,943.4 | 0.862518 | 1642.8 | 0 |
| 변경본 1 | 39,906.3 | 0.891555 | 1589.4 | 0 |
| binding 1 | 39,940.2 | 0.719348 | 1740.7 | 0 |
| main 2 | 39,944.8 | 0.881135 | 1607.7 | 0 |
| 변경본 2 | 39,945.5 | 0.881925 | 1606.7 | 0 |
| binding 2 | 39,940.2 | 0.717215 | 1745.6 | 0 |

| 40k paced mutex 실행 | 요청 수 | Reply lock 호출 수 | 경합 호출 수 | 누적 wait ns | Wait µs/request |
|---|---:|---:|---:|---:|---:|
| main | 1,200,000 | 1,200,000 | 136,188 | 1,098,826,199 | 0.915688 |
| 변경본 | 1,200,000 | 1,200,000 | 129,084 | 1,059,202,855 | 0.882669 |

세 조건 모두 변경본의 F÷B가 0.6 미만이다. 변경본은 각각 over budget by 12.519µs, 10.496µs, 17.254µs/request다. 처리율 중앙값은 증가했지만 세 조건 모두 main과 변경본의 실행 범위가 겹쳤다.

Paced perf·packet은 warmup 30초와 active 40초에서 perf 25초 및 그 안의 ss 구간을 사용했다. Packet은 TCP data segment 카운터로 측정했다. Reply 연결은 target data port의 bytes_sent가 가장 큰 연결이며 ss 구간의 완료 요청 수로 나눴다. 두 반복에서 변경본의 packet 감소를 관측하지 못했다. 모든 reply 연결의 retransmit delta는 0이었다. 집계는 `tables.json`, `aligned-ss-summary.json`, `perf-summary.json`에 있다.

Mutex는 별도 warmup 30초와 active 30초의 40k req/s 실행이다. 두 variant 모두 유효 요청 1,200,000건과 reply lock 호출 수가 일치했고 계측 오류는 0이었다. Trylock의 EBUSY 뒤 pthread_mutex_lock의 경과 시간을 누적했으므로 scheduler 지연을 포함한다. 관측된 wait 차이는 0.033019µs/request다. `runs/main-mutex/mutex-window.json`, `runs/direct-mutex/mutex-window.json`에 원자료를 보존했다. 이전 세션에서 완료한 계측기의 정상·의도한 경합·범위 밖 제외 검증은 `logs/mutex-validation.log`와 `tools/mutex-validation.json`이다.

성능 측정의 배타 잠금 대기는 약 47.75분, 마지막 전체 빌드의 측정 gate 대기는 116.30분이었다. `measurement-requested.json`·`measurement-entered.json`과 `logs/build-unit-contract-final-wait.json`에 기록했다. 최종 검증 전 공유 gate 대기도 7.31분이었다. 측정 잠금 대기는 작업 시간 제한에서 제외했다.

소유 계층은 Framework의 wrapper 수명·자원 소유권·close 기한과 binding/Core의 native admission·close 수락이다.

Spec 근거는 `bindings/doc/spec/README.ko.md`의 Multipart 제출·별도 송신 gate 금지와 `core/doc/spec/core/socket/README.ko.md` §2의 same-handle concurrency·close gate다. BUSY 재시도 기한은 사용자가 전달한 Java PM 결정을 적용했다.

교차언어 대조는 C++ port mutex, 로컬 Java 원본의 port state lane와 `ZLinkFrameworkRuntime.java:94`의 30초 기본값, .NET `ZLinkRawRouterServicePort.cs:247`과 Node `node-raw-binding-port.ts:260`의 직접 binding 제출이다. 로컬 Java 소스를 수정하지 않았다. 검증 범위는 요청된 C++이다.

변경 분류는 A, 기존 binding 동시 제출 계약과 승인된 종료 정책 적용이다. Mesh의 기존 close 상태 설정 시점과 중복 정리 경로 통합은 B, 기존 lifecycle 결함 수정이다.

원본 main 대비 native reply 제출 직렬화 판정 위치는 Framework mutex와 Core의 2곳에서 Core 1곳으로 줄었다. C++ wrapper와 close-start null 검사는 기존 mutex 1곳이다. 이어받은 8회 정책과 현재 deadline 정책의 재시도 판정 위치는 모두 공통 함수 1곳이다. 기본 종료 기한의 정의는 2곳에서 1곳으로 줄였고 재시도 attempt counter는 제거했다. Mesh 재시작 판정 위치와 기존 `_closed` 필드 수는 각각 1곳과 1개다.

현재 소스의 Framework 변경은 `change.diff`, CPU pin·측정·대조군 bench 변경은 `bench.diff`로 분리해 저장했다. `git diff --check`도 통과했다. 남은 검사 실패는 위의 registry assertion과 label cache 경로 두 건이다.

변경 파일은 C++의 `CMakeLists.txt`, configuration의 `app.hpp`·`lifecycle.hpp`, backend의 `raw_route_port.cpp`·`.hpp`, owner의 `raw_mesh_node_owner.cpp`·`raw_client_server_owner.cpp`, host의 `app.cpp`·`runtime_failure_collector.hpp`, 기존 close failure unit test, 새 reply close contract test와 test clock probe다. Bench 변경은 별도 diff에만 넣었다.

두 대안은 port가 borrowed socket까지 닫는 방법과 기존 socket owner가 port를 분리한 뒤 명시적으로 닫는 방법이었다. 소유권을 가진 기존 owner에서 close 오류와 보존을 처리하는 방법을 선택했다. Port의 소유 역할은 늘리지 않았다.

리팩토링 점검에서는 중복 판정·작업 4건을 확인했다. Native reply 직렬화 판정의 Framework 중복 1건을 제거했다. Binding reply token 구성(`bindings/cpp/src/Runtime/Sockets/router.cpp:114`, `bindings/cpp/src/Runtime/Messaging/reply_operations.cpp:24`), protobuf body 복사(`framework/languages/cpp/extensions/framework-codec-protobuf/include/zlink/codecs/protobuf.hpp:30`, `framework/languages/cpp/framework/src/runtime/protocol/service_wire_codec.cpp:3201`), routing ID 구성(`framework/languages/cpp/framework/src/runtime/backend/raw_route_port.cpp:535`, `bindings/cpp/src/Runtime/Sockets/router.cpp:118`) 3건은 이번 close 변경 범위를 벗어나 유지했다. Bindings와 Core는 수정하지 않았다. Owner 정리 경로와 종료 기한 기본값의 중복도 같은 변경에서 통합했다.
