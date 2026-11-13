# #4094 C++ late-result hang 진단

읽기 전용 재현을 완료했다. 바이너리는 `binary-commit.txt`의 `b42f978451` 빌드이며, 저장소와 바이너리는 변경하지 않았다. 각 실행 전 `MemAvailable`은 12 GiB 이상이었다.

## 테스트 의도와 대기 지점

`submitted_actor_receiver_t.ShutdownWhileAdmissionPendingDeliversOnce`는 peer가 아직 admission되지 않은 상태에서 runtime을 종료하고, public 결과·physical 결과·observer callback이 각각 한 번만 종료되는지 확인한다. 테스트는 `peer(false)` 후 `verify(shutdown)`을 부른다 (`test_cpp_framework_actor_late_result.cpp:248-252`). `verify`는 `runtime->stop()`으로 shutdown terminal을 만들고, 이어서 `pending.result()`와 `physical.result()`를 기다린 다음 observer future를 최대 2초 기다린다 (`:214-225`). registry의 shutdown callback은 completion dispatcher가 실행하고, public result와 observer promise를 깨우는 경로다 (`operation_registry.cpp:281-293`, `public_host_runtime.cpp:69-139`). 이 테스트는 부하 2,000회와 무부하 1,000회에서 hang이 없었다.

`local_actor_receiver_t.LocalRegistryTerminalsReachPreparedReceiverOnce`는 준비된 local actor 요청에 transport failure, timeout, cancellation, shutdown, completion terminal을 차례로 적용하고 receiver가 한 번만 호출되는지 확인한다 (`test_cpp_framework_actor_late_result.cpp:895-929`). 각 반복은 제출 task의 `result()`를 기다리고, `dispatch_request()`가 최대 2초 동안 dispatch를 반복하며 request record를 찾고, terminal 처리 뒤 completion task의 `result()`를 기다린다 (`:909-920`, `:621-630`). completion wait에는 deadline이 없다. timeout terminal은 `end_local()`이 `next_deadline()`을 읽고 `expire(...).result()`를 호출해야 발생한다 (`:78-89`).

## 재현률과 명령

부하 실행은 `measure-shared.sh` 안에서 실행기를 돌렸다. 실행기는 `stress-ng --cpu 8 --taskset 8-15`를 함께 실행하고, 각 테스트를 `timeout -s TERM 30 taskset -c 8-11 <binary> [--gtest_filter=...]`로 실행했다. 전체 바이너리 실행은 filter를 생략했고, 격리 실행 filter는 `submitted_actor_receiver_t.ShutdownWhileAdmissionPendingDeliversOnce`와 `local_actor_receiver_t.LocalRegistryTerminalsReachPreparedReceiverOnce`였다. hang이 나면 timeout 전에 `gdb -p <pid> -batch -ex 'thread apply all bt'`를 실행하고, stress-ng와 테스트는 SIGTERM으로 종료했다.

전체 바이너리는 부하에서 69회 중 5회 hang, 7.25%였다. Shutdown 테스트는 부하 2,000회 중 0회였다. Local registry 테스트는 부하 317회 중 5회 hang, 1.58%였다. 둘 다 5회 hang 또는 2,000회 기준을 적용했다.

무부하 각 실행은 1,000회 수행했다. 전체 바이너리는 2/1,000, 0.20% hang이었다. Shutdown 테스트와 Local registry 테스트는 각각 0/1,000이었다.

전체 바이너리 부하 구간에서는 `/root/jobs/fx/cx/measure-shared.sh stress-ng --cpu 8 --taskset 8-15`와 `/root/jobs/fx/cx/measure-shared.sh timeout -s TERM 30 taskset -c 8-11 <binary>`를 함께 실행했다. 격리 테스트 부하 구간과 무부하 구간은 `/root/jobs/fx/cx/measure-shared.sh python3 /root/jobs/fx/cx/v21/s033/clate/reproduce.py`로 실행했고, 실행기 안에서 각각 위 `timeout`·`taskset`과 stress-ng 명령을 적용했다. 무부하 구간에는 stress-ng가 없었다.

## Stack과 원인 가설

12개 hang 모두에서 gdb가 timeout 전에 전체 thread stack을 저장했다. 대표 stack은 `stacks-whole-loaded-2.txt`, `stacks-local_registry-loaded-52.txt`, `stacks-whole-unloaded-12.txt`에 있다. main test thread는 `pthread_cond_wait` 안에서 `LocalRegistryTerminalsReachPreparedReceiverOnce_Test::TestBody()`에 머물렀다. 이는 `task_shared_state_t::result()`의 무기한 `_ready.wait()`와 일치한다 (`task.hpp:690-697`). completion dispatcher worker들은 Asio scheduler condition variable에서 idle이었고, deadline scheduler와 I/O worker들도 대기 상태였다. 저장된 각 hang 출력에는 `end_local()`의 `ASSERT_TRUE(deadline)` 실패와 SCOPED_TRACE terminal 값 `1`이 있었다 (`test_cpp_framework_actor_late_result.cpp:85-87, 899`).

완료를 알릴 주체는 operation registry가 terminal callback을 completion dispatcher에 게시하고, `local_completion_sink_t`가 `source->complete()`를 호출하는 경로다 (`operation_registry.cpp:281-293`, `public_host_runtime.cpp:135-139`). task 상태가 완료되면 `_ready.notify_all()`이 대기 thread를 깨운다 (`task.hpp:622-647`). 재현된 hang에서는 timeout 분기에서 `next_deadline()`이 아직 비어 있어 assertion이 실패하고 `expire()`가 호출되지 않았다. `register_operation()`은 owner lane 바깥에서 호출되면 deadline index 등록을 `_owner->try_post()`로 비동기 게시한다 (`operation_registry.cpp:428-447`). 테스트가 lane의 index 게시가 끝나기 전에 `next_deadline()`을 확인할 수 있고, ASSERT는 helper만 반환한다. 바깥 테스트는 계속 진행해 terminal callback을 만들지 않은 채 무기한 completion `result()`에 들어간다 (`test_cpp_framework_actor_late_result.cpp:86, 919-920`).

분류는 test-side wait이다. 제품 callback이 신호를 놓쳤다는 증거는 없으며, 재현 당시 timeout terminal을 발행하는 `expire()` 호출 자체가 assertion으로 건너뛰어졌다. 무부하에서도 같은 형태가 2회 재현되어 부하 전용 제품 ordering defect로 볼 근거는 없다. 부하는 race 관측률을 높였다.

중복 대기와 handoff도 확인했다. local 테스트는 제출 결과 대기, 2초 dispatch poll, 무기한 completion 대기를 순서대로 수행하며, timeout helper 안에서도 `expire().result()`를 기다린다 (`:87, 909-920`). 또 completion 대기가 끝난 뒤에야 held handler turn을 닫는 `native_reply(nullopt)`를 호출한다 (`:925-928`). 이 경로는 완료 조건을 여러 단계로 나누며 핵심 completion wait에는 deadline이 없다. Shutdown 테스트도 public 결과, physical 결과, observer future를 차례로 확인한다 (`:218-225`).

기존 CTest 로그의 shutdown 테스트 stats 출력 뒤에는 `[ OK ]`가 실제로 있다. stats `printf`가 테스트 본문에서 먼저 실행되고 Google Test가 반환 뒤 OK 줄을 출력하므로 출력 순서가 정상이다.

스택과 hang 시점 출력은 `stacks-*.txt` 및 `output-*.log`에 있다. 실행 요약은 `reproduction-summary.txt`에 있다.
