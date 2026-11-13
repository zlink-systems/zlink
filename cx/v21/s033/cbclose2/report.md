C++ binding #4060의 close 대기 동작을 수정하였다. 작업 branch는 fix/4060-cpp-binding-submit-close이며 기준 commit은 f784975639다. 기존 미커밋 변경 위에서 작업하였고 commit, branch 변경, push, GitHub 작업은 하지 않았다. 수정 범위는 bindings/cpp다. 이하 Native·Messaging·Sockets 경로는 bindings/cpp/src/Runtime 아래다.

1. close 대기와 메모리 순서.

src/Runtime/Native/socket_handle.hpp:59의 reserve는 closed bit가 설정된 경우에만 false를 반환한다. closing만 설정되어 있으면 :68의 atomic wait(acquire) 뒤 상태를 다시 읽고 예약을 재시도한다. :90의 zlink_close가 성공하거나 SHUTDOWN을 반환하면 :91에서 closed를 release store한다. BUSY를 포함한 거절이면 0을 release store한다. 양쪽 모두 :92에서 notify_all을 호출한다. Native 결과와 그 이전의 변경은 release store와 acquire 관측으로 연결된다. wait는 상태가 바뀐 경우에만 반환하며, 예약 CAS와 close CAS는 같은 atomic modification order에서 순서가 정해진다. 예약 해제의 release RMW와 close CAS의 acquire는 admitted native 호출의 종료를 연결한다. :66–67에 결과 공개와 terminal 판정 근거를 두 줄로 적었다.

Messaging/operation_state.hpp:124의 share_runtime_state는 Native/socket_handle.hpp:49의 accepting_submits를 사용한다. 이 함수는 :44의 valid에 위임하고 closed만 검사한다. 따라서 closing 중 살아 있는 runtime을 terminal로 분류하지 않는다. Binding 수명 선행 검사에서 TERMINATED/ESHUTDOWN 예외를 생성하는 공통 predicate는 Native/socket_handle.hpp:23의 require_submit_open 하나다. Socket 소유자가 소멸한 경우도 이 predicate로 처리한다. submit native 경로에 mutex를 추가하지 않았다.

Binding 내부에서 zlink_close를 호출하는 위치는 Native/socket_handle.hpp:90 하나다. Sockets/socket.cpp:85–87 → Messaging/completion_owner.cpp:796–813 → socket_handle_t::close가 close 호출 경로다. socket.cpp:99–100의 completion shutdown은 native close가 반환한 뒤 실행된다. completion_owner.cpp:832·835의 terminate 처리도 native close 이후다. 이 경로에 close 실행 thread에서 같은 socket으로 submit하는 binding 코드가 없음을 확인하였다. Messaging/message.cpp:283–291은 외부 message free callback을 Core에 직접 전달한다. 이 사용자 callback의 임의 재진입은 binding 내부 submit 경로가 아니며, 사용자 callback까지 재진입 불가능하다고 보장한 것은 아니다. 고정된 설계에서 같은 thread의 close 안에서 submit하면 close 결과를 기다릴 수 있다. nested close만 BUSY를 반환하는 테스트를 유지하였다.

소유 계층은 C++ binding socket_handle의 native 수명 예약이다. 실제 API 수락과 BUSY는 Core가 소유한다.

Spec 근거는 bindings/doc/spec/README.ko.md:498·1851과 core/doc/spec/core/socket/README.ko.md:58·647·657이다.

교차언어 대조는 첫 검토에서 확인한 Java #4073의 기존 ownerLock 기반 수명 예약을 따른다.

변경 분류는 B다. 감독자가 확정한 C++ atomic state 설계를 유지한 기존 결함 수정이다. Spec은 수정하지 않았다.

2. 테스트.

신규 admission test의 closing_waits_for_outcome은 send, request, reply, publish 각각에 대해 성공 close와 BUSY close를 보류한다. Submit 시작 뒤 100ms 동안 완료 future가 준비되지 않고 native submit 진입 수가 늘지 않는 것을 검사한다. 성공 close 뒤에는 TERMINATED/ESHUTDOWN과 메시지 소유권 보존을 검사한다. BUSY 뒤에는 정확히 한 번 native submit에 진입하여 OK가 되는 것을 검사한다. 닫는 thread 안의 nested submit 검사는 제거하고 nested close의 BUSY/EBUSY만 검사한다. 독립 record 두 개의 동시 진입, completion receive 예약, 인자 준비 실패 후 예약 해제, 소유자 소멸 후 builder와 Received의 실패 검사도 유지하였다.

수정 후 admission은 stress-ng CPU worker 16개 아래 exit 0이었다. after-admission.log, after-admission-load.log, after-admission-memory.log에 기록하였다. assert는 Release 빌드에서도 -UNDEBUG로 활성화하였다.

수정 후 독립 경합은 네 경로 각각 1,000회이며 경로마다 submit 4,000건이다. after-races.log의 send OK/TERMINATED는 2020/1980, request는 1831/2169, reply는 1818/2182, publish는 1634/2366이다. Close OK/BUSY는 각각 683/317, 673/327, 657/343, 767/233이다. 예상 밖 결과와 ownership 실패는 모든 경로에서 0건이다. after-races-load.log와 after-races-memory.log는 같은 실행의 CPU 부하와 메모리를 기록한다.

같은 세션의 main-races.log에서 send OK/TERMINATED는 831/3169, request는 395/2493, reply는 766/3224, publish는 1166/2834였다. main의 close는 모든 경로에서 OK 1000회, BUSY 0회였다. Request에는 INVALID_STATE/108 1070건, INTERNAL_ERROR/71 40건, BACKPRESSURED/0 2건이 있었다. Reply에는 NOT_FOUND/2 10건이 있었다. Request ownership 실패는 40건이었다. 예상 밖 결과 1122건과 ownership 실패 40건은 별도 지표이며 중복 합산하지 않는다. Send와 publish의 예상 밖 결과 및 ownership 실패는 0건이었다. main의 경합 process exit는 1이었다.

main-admission.log는 두 wrapper 진입을 보류한 상태에서 close가 성공하여 reserved_calls_are_concurrent의 :149 assertion이 실패한 음성 대조다. Native 예약이 없는 main의 결함을 검출하였으며 process SIGABRT, wrapper exit 134였다.

최종 전체 binding CTest는 after-full-binding.log에서 27/27 통과하였고 skip과 실패는 0개였다. ctest --test-dir /root/jobs/fx/cx/v21/s033/cbclose2/build-after --output-on-failure -j3을 stress-ng CPU worker 16개 아래 실행하였으며 30.00초가 걸렸다. full-binding/LastTest.log를 보존하였다. 이 suite의 독립된 경합 1000회에서 send OK/TERMINATED는 1333/2667, request는 1595/2405, reply는 1259/2741, publish는 1419/2581이었다. Close OK/BUSY는 각각 817/183, 654/346, 771/229, 796/204였다. 예상 밖 결과와 ownership 실패는 모두 0건이었다. 기존 send-close stress도 제출·수신 50000건, 예상 밖 결과·ownership 실패·손상 record 0건으로 통과하였다.

처음에는 benchmark build 옵션을 켠 상태로 unfiltered CTest를 실행하여 binding 27개는 모두 통과했으나 추가 perf-regression smoke 한 개가 실패하였다. 이 smoke는 공식 bindings/cpp/build/CMakeCache.txt를 요구하고 이 작업은 별도 cbclose2/build-after를 사용하므로 native 진입 전에 실패하였다. after-full.log와 full-with-perf/LastTest.log에 27/28 결과를 보존하였다. Benchmark 바이너리를 빌드한 뒤 BUILD_BENCHMARKS=OFF인 원래 binding 테스트 구성으로 재설정하여 위의 unfiltered 27/27을 확인하였다. Perf smoke의 코드나 assertion, source fixture는 변경하지 않았다.

빌드는 /root/jobs/fx/cx/cpp-build-slot.sh 아래 -j3으로 실행하였다. Test는 /root/jobs/fx/cx/measure-shared.sh 아래 run-loaded.py로 실행하였으며 stress process는 finally에서 SIGTERM 뒤 wait로 종료하였다. Configure와 최종 build 결과는 configure-after.log, build-after-final.log, build-main.log, configure-tests-only.log에 있다. 초기 형식 복원 중의 중간 build 오류는 build-after.log에 남아 있으며 최종 build에서 해소하였다.

첫 변경의 close-wait 결함은 wait-proof.cpp로 별도 재현하였다. 실제 Native/socket_handle.hpp를 include하고 held zlink_close가 BUSY를 반환하도록 wrapper를 사용하였다. 첫 변경의 header는 waited=0, admitted_after_busy=0으로 exit 1이었다. 수정 후 header는 waited=1, admitted_after_busy=1로 exit 0이었다. 따라서 BUSY로 끝날 close 도중 terminal로 조기 거절하던 동작과 수정 후 대기·예약 성공을 양방향으로 확인하였다. close_rc는 양쪽 모두 401(BUSY)이었다. wait-proof-firstpass.log·wait-proof-after.log와 extra-results.json에 기록하였다. 독립 재현 프로그램의 최초 compile은 public C header include 누락으로 실패하였고, include를 추가한 build-proof.log의 최종 build는 성공하였다. Repo header는 이 진단 때문에 변경하지 않았다.

sizeof 출력과 이 재현 검사도 measure-shared.sh 및 CPU 부하 아래 실행하였으며 extra-result.json의 전체 exit는 0이다. 모든 test memory 표본의 최저 MemAvailable은 17.2368 GiB였다. Benchmark는 실행 전 각 표본에서 12 GiB 이상을 검사하였다. memory-summary.json과 각 *-memory.log에 표본을 보존하였다. 부하 process와 이 작업의 test·benchmark process는 모두 종료하였다.

3. 형식 변경 제거.

추가된 의미 없는 줄바꿈·들여쓰기·선언 정렬을 main 원문으로 되돌렸다. 전체 파일 formatter는 적용하지 않았다. git diff origin/main --stat와 git diff -w origin/main --stat의 출력은 동일하다. bindings/cpp만 제한한 두 출력도 동일하다. full-origin-stat.txt·full-origin-stat-w.txt와 final-stat.txt·final-stat-w.txt가 근거다. origin/main이 작업 중 기준 commit보다 앞서 갱신되어 전체 비교에는 이번 작업과 무관한 Framework 차이가 표시된다. 최종 change.diff와 아래 파일별 집계는 실제 작업 범위인 bindings/cpp만 포함한다. git diff --check는 통과하였다. 최종 change.diff를 main 사본에 적용한 뒤 새 파일을 포함한 23개 변경 파일이 worktree와 byte 단위로 일치함을 확인하였다(verify-final-patch.json).


bindings/cpp/CMakeLists.txt: +16/-0.

bindings/cpp/include/zlink/Contracts/Messaging/received.hpp: +0/-1.

bindings/cpp/include/zlink/Contracts/Sockets/socket_contracts.hpp: +3/-1.

bindings/cpp/src/Runtime/Messaging/completion_owner.cpp: +40/-16.

bindings/cpp/src/Runtime/Messaging/completion_owner.hpp: +5/-2.

bindings/cpp/src/Runtime/Messaging/operation_state.hpp: +14/-10.

bindings/cpp/src/Runtime/Messaging/operation_submit.hpp: +20/-16.

bindings/cpp/src/Runtime/Messaging/received.cpp: +0/-2.

bindings/cpp/src/Runtime/Messaging/received_access.hpp: +3/-12.

bindings/cpp/src/Runtime/Messaging/reply_operations.cpp: +11/-5.

bindings/cpp/src/Runtime/Messaging/request_reply.cpp: +0/-4.

bindings/cpp/src/Runtime/Messaging/send_operations.cpp: +6/-3.

bindings/cpp/src/Runtime/Native/native_send.hpp: +0/-56.

bindings/cpp/src/Runtime/Native/socket_handle.hpp: +78/-49.

bindings/cpp/src/Runtime/Sockets/dealer.cpp: +0/-2.

bindings/cpp/src/Runtime/Sockets/pair.cpp: +0/-1.

bindings/cpp/src/Runtime/Sockets/pubsub.cpp: +0/-5.

bindings/cpp/src/Runtime/Sockets/router.cpp: +1/-5.

bindings/cpp/src/Runtime/Sockets/socket.cpp: +28/-22.

bindings/cpp/src/Runtime/Sockets/socket_runtime_state.hpp: +5/-2.

bindings/cpp/src/Runtime/Sockets/stream.cpp: +0/-1.

bindings/cpp/tests/contract/test_cpp_contract_submit_close_admission.cpp: +461/-0. 신규 untracked 파일이므로 일반 git diff 통계에는 나타나지 않으며 전체 change.diff에는 포함한다.

bindings/cpp/tests/stress/test_cpp_submit_close_race.cpp: +157/-0. 신규 untracked 파일이므로 일반 git diff 통계에는 나타나지 않으며 전체 change.diff에는 포함한다.

4. Public header의 private member와 크기.

include/zlink/Contracts/Messaging/received.hpp에서는 std::uintptr_t _send_context_handle = 0을 제거하였다. 새 private data member나 private method는 추가하지 않았다. 나머지 private 선언은 main과 동일하다.

include/zlink/Contracts/Sockets/socket_contracts.hpp:166에서는 _socket을 std::unique_ptr<detail::socket_handle_t>에서 std::shared_ptr<detail::socket_handle_t>로 변경하였다. :164의 private method int close_native() noexcept를 추가하였다. 나머지 private 선언은 main과 동일하다. 함수 추가는 object 내 저장 공간을 사용하지 않지만 소유 pointer의 표현과 received_t의 field 제거는 layout을 바꾼다. Consumer는 새 header로 다시 컴파일해야 한다.

동일한 g++와 C++20 옵션으로 sizeof.cpp를 각 tree의 public header에 대해 빌드하고 실행하였다. static_assert는 사용하지 않았다. Linux x64에서 main은 sizeof(received_t)=464, sizeof(socket_t)=40이고 수정 후는 각각 464, 48이었다. sizeof-main.log·sizeof-after.log가 원시 출력이다. received_t의 전체 크기는 같지만 제거된 field 뒤 private layout은 달라지므로 기존 object file과의 ABI 호환을 뜻하지 않는다.

5. 동일 세션의 처리량 측정.

perf/README.md와 run_binding_single.sh에서 target·인자·환경변수를 확인하고, suite의 cpp_perf_pair와 cpp_perf_pubsub 바이너리를 직접 실행하였다. PUBSUB이 있으므로 publish 1 KiB도 측정하였다. Suite의 기존 request/reply는 여러 요청을 연속 제출하는 async 방식이므로, 요청마다 reply를 받은 뒤 다음 요청을 제출하는 serial.cpp를 별도로 두 tree의 정적 binding library에 동일하게 링크하였다. 이 프로그램은 public C++ request().message(...).timeout(...).submit()와 Received.reply()만 사용하며 request·reply payload는 각각 1024 byte다. Repo의 perf 구현은 변경하지 않았다.

모든 workload는 inproc이고 한 benchmark process 안의 thread들로 실행하였다. 따라서 source·target 별 process pin 대신 전체 process를 taskset -c 8-15로 고정하였다. Payload는 1024 byte, PERF_PART_COUNT=1, 활성 측정은 5초다. PAIR·PUBSUB은 suite의 throughput 계산을 그대로 사용하고, 직렬 request는 완료 요청 수를 실제 활성 경과 시간으로 나누었다. PAIR·PUBSUB의 단위는 msg/s이고 REQUEST_SERIAL은 request/s다. after는 이 worktree의 변경을 뜻한다.

각 workload의 RUNS=6은 인접 main/PR 쌍으로 구성하였다. 홀수 쌍은 main→PR, 짝수 쌍은 PR→main 순서다. 각 쌍은 하나의 flock -o /root/jobs/fx/cx/measure.lock 안에서 실행하였다. 한 쌍의 두 실행 사이에 다른 측정은 들어가지 않는다. bench-pair.py와 bench-all.py가 명령·순서·시각·exit·실행 전 메모리·uptime을 보존한다. 측정 중에는 다른 build와 test를 실행하지 않았다.

main-tree는 최초 미커밋 bindings/cpp를 복사한 뒤 ../cbclose/change.diff를 patch -R -p1으로 역적용하였다. git diff로 f784975639를 복원한 별도 사본과 git diff --no-index로 대조한 결과 exit 0이었다(main-copy-verify.diff, main-copy-verification.json). 그 뒤 비교용 test target과 동일 test source만 추가하였다. Runtime·public header·perf source는 main 그대로다. main은 build-main, 변경은 build-after에 각각 Release로 빌드하였다. 두 build 모두 같은 compiler·Core package·LTO 설정을 사용하였다. CMakeCache-main.txt·CMakeCache-after.txt와 binary-provenance.json에 설정과 hash를 보존하였다. 두 실행의 Core는 /root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so.1.23.0이며 SHA-256은 6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01이다. ldd 기록도 동일한 package 경로를 확인한다.

PAIR 1회(main→after): main 1755466.600 msg/s, PR 1616305.200 msg/s, PR/main 0.920727. 부하 원문: 01:47:42 up 24 days, 17:13,  3 users,  load average: 1.14, 2.23, 3.94. 로그: PAIR-1-attempt1-main.log, PAIR-1-attempt1-after.log.

PAIR 2회(after→main): main 1947609.200 msg/s, PR 1783262.400 msg/s, PR/main 0.915616. 부하 원문: 01:48:04 up 24 days, 17:13,  3 users,  load average: 1.44, 2.21, 3.89. 로그: PAIR-2-attempt1-after.log, PAIR-2-attempt1-main.log.

PAIR 3회(main→after): main 1956395.200 msg/s, PR 1900250.600 msg/s, PR/main 0.971302. 부하 원문: 01:48:26 up 24 days, 17:14,  3 users,  load average: 1.81, 2.25, 3.87. 로그: PAIR-3-attempt1-main.log, PAIR-3-attempt1-after.log.

PAIR 4회(after→main): main 2017202.400 msg/s, PR 1853360.200 msg/s, PR/main 0.918778. 부하 원문: 01:48:49 up 24 days, 17:14,  3 users,  load average: 2.08, 2.28, 3.83. 로그: PAIR-4-attempt1-after.log, PAIR-4-attempt1-main.log.

PAIR 5회(main→after): main 1922890.400 msg/s, PR 1804510.400 msg/s, PR/main 0.938436. 부하 원문: 01:49:11 up 24 days, 17:14,  3 users,  load average: 2.21, 2.29, 3.80. 로그: PAIR-5-attempt1-main.log, PAIR-5-attempt1-after.log.

PAIR 6회(after→main): main 1916350.800 msg/s, PR 1833510.600 msg/s, PR/main 0.956772. 부하 원문: 01:49:33 up 24 days, 17:15,  3 users,  load average: 2.13, 2.27, 3.75. 로그: PAIR-6-attempt1-after.log, PAIR-6-attempt1-main.log.

PUBSUB 1회(main→after): main 1744897.200 msg/s, PR 1682165.000 msg/s, PR/main 0.964048. 부하 원문: 01:47:52 up 24 days, 17:13,  3 users,  load average: 1.36, 2.24, 3.93. 로그: PUBSUB-1-attempt1-main.log, PUBSUB-1-attempt1-after.log.

PUBSUB 2회(after→main): main 1690132.400 msg/s, PR 1621544.000 msg/s, PR/main 0.959418. 부하 원문: 01:48:14 up 24 days, 17:13,  3 users,  load average: 1.60, 2.22, 3.87. 로그: PUBSUB-2-attempt1-after.log, PUBSUB-2-attempt1-main.log.

PUBSUB 3회(main→after): main 1557670.000 msg/s, PR 1675440.000 msg/s, PR/main 1.075607. 부하 원문: 01:48:37 up 24 days, 17:14,  3 users,  load average: 1.91, 2.26, 3.85. 로그: PUBSUB-3-attempt1-main.log, PUBSUB-3-attempt1-after.log.

PUBSUB 4회(after→main): main 1713387.200 msg/s, PR 1624865.400 msg/s, PR/main 0.948335. 부하 원문: 01:48:59 up 24 days, 17:14,  3 users,  load average: 2.15, 2.28, 3.82. 로그: PUBSUB-4-attempt1-after.log, PUBSUB-4-attempt1-main.log.

PUBSUB 5회(main→after): main 1712346.200 msg/s, PR 1674717.200 msg/s, PR/main 0.978025. 부하 원문: 01:49:21 up 24 days, 17:15,  3 users,  load average: 2.26, 2.30, 3.79. 로그: PUBSUB-5-attempt1-main.log, PUBSUB-5-attempt1-after.log.

PUBSUB 6회(after→main): main 1731314.600 msg/s, PR 1683886.400 msg/s, PR/main 0.972606. 부하 원문: 01:49:43 up 24 days, 17:15,  3 users,  load average: 2.18, 2.27, 3.74. 로그: PUBSUB-6-attempt1-after.log, PUBSUB-6-attempt1-main.log.

REQUEST_SERIAL 1회(main→after): main 79754.800 msg/s, PR 78814.300 msg/s, PR/main 0.988208. 부하 원문: 02:41:17 up 24 days, 18:06,  3 users,  load average: 2.03, 3.24, 3.38. 로그: REQUEST_SERIAL-1-attempt2-main.log, REQUEST_SERIAL-1-attempt2-after.log.

REQUEST_SERIAL 2회(after→main): main 79943.000 msg/s, PR 79518.900 msg/s, PR/main 0.994695. 부하 원문: 02:41:29 up 24 days, 18:07,  3 users,  load average: 1.87, 3.15, 3.35. 로그: REQUEST_SERIAL-2-attempt1-after.log, REQUEST_SERIAL-2-attempt1-main.log.

REQUEST_SERIAL 3회(main→after): main 79781.100 msg/s, PR 78709.500 msg/s, PR/main 0.986568. 부하 원문: 02:41:41 up 24 days, 18:07,  3 users,  load average: 1.97, 3.13, 3.34. 로그: REQUEST_SERIAL-3-attempt1-main.log, REQUEST_SERIAL-3-attempt1-after.log.

REQUEST_SERIAL 4회(after→main): main 77606.100 msg/s, PR 77431.900 msg/s, PR/main 0.997755. 부하 원문: 02:41:53 up 24 days, 18:07,  3 users,  load average: 1.97, 3.09, 3.32. 로그: REQUEST_SERIAL-4-attempt1-after.log, REQUEST_SERIAL-4-attempt1-main.log.

REQUEST_SERIAL 5회(main→after): main 78761.500 msg/s, PR 79080.400 msg/s, PR/main 1.004049. 부하 원문: 02:42:05 up 24 days, 18:07,  3 users,  load average: 1.77, 2.99, 3.28. 로그: REQUEST_SERIAL-5-attempt1-main.log, REQUEST_SERIAL-5-attempt1-after.log.

REQUEST_SERIAL 6회(after→main): main 76878.200 msg/s, PR 76743.900 msg/s, PR/main 0.998253. 부하 원문: 02:42:17 up 24 days, 18:07,  3 users,  load average: 1.81, 2.95, 3.27. 로그: REQUEST_SERIAL-6-attempt1-after.log, REQUEST_SERIAL-6-attempt1-main.log.

PAIR: 6쌍, 비율 기하평균 0.936707, 중앙값 0.929582, 범위 0.915616–0.971302.

PUBSUB: 6쌍, 비율 기하평균 0.982128, 중앙값 0.968327, 범위 0.948335–1.075607.

REQUEST_SERIAL: 6쌍, 비율 기하평균 0.994903, 중앙값 0.996225, 범위 0.986568–1.004049.

부하 초과로 연기한 시도는 REQUEST_SERIAL 1쌍 attempt 1이다. 실행 전 원문은 02:40:17 up 24 days, 18:05,  3 users,  load average: 4.79, 3.86, 3.58이다. 이 시도에서는 benchmark를 실행하지 않았고 60초 뒤 같은 쌍을 다시 확인하였다. 유효한 18쌍의 실행 전 1분 부하는 모두 4 이하였다.

비율 기하평균으로 PAIR은 main보다 6.33%, PUBSUB은 1.79%, 직렬 request는 0.51% 낮았다. 개별 쌍의 변동은 위 범위와 원시 로그에 그대로 기록하였다. 별도의 성능 합격 기준은 적용하지 않았다.

6. 리팩토링 점검.

종료 결과 판정 위치의 수는 첫 변경 1곳 → 이번 수정 1곳이다. require_submit_open은 Native/socket_handle.hpp:23 하나이며 Binding의 수명 선행 실패는 이 predicate를 호출한다. closing 자체를 terminal로 보던 하위 조건을 제거하였다. atomic state와 socket_call_t는 유지하였으며 새 flag·queue·lock·timer는 추가하지 않았다.

Messaging/operation_state.hpp:124의 runtime 선행 검사와 Native/socket_handle.hpp:127의 native 예약은 유지하였다. 전자는 completion 등록 전에 강한 runtime을 확보하는 경계이고 후자는 native pointer의 수명을 확보하는 경계다. 두 검사 모두 같은 socket_handle의 closed 사실을 읽고 공통 predicate를 사용한다. 별도의 lifecycle 상태는 없다. accepting_submits(:49)는 valid(:44)의 단순 위임이며 closing을 중복 판정하지 않는다.

Messaging/completion_owner.cpp:593·604·614의 _shutdown 검사는 completion 등록 경계이므로 유지하였다. :286의 settled 검사는 close가 먼저 종료한 최초 request가 BACKPRESSURED로 잘못 반환되지 않도록 같은 terminal predicate를 호출한다. :678의 drain loop shutdown 검사와 :681의 native 예약도 각각 completion owner와 native 수명 경계여서 유지하였다. 원래 loop 앞에 있던 같은 shutdown 선행 검사는 제거된 상태다.

Messaging/completion_owner.cpp:700의 재시도 직전 _shutdown 검사, :775의 public poller 소유권 이전 검사, :818의 shutdown 중복 호출 검사도 유지하였다. 각각 별도 registry 진행·설정·종료 경계이며 socket의 closed 판정을 새로 생성하지 않는다. 기존 _inline_drain_mutex와 _public_owner는 completion drain·poller 소유권을 정하며 native submit record의 직렬화를 추가하지 않는다.

Messaging/completion_owner.cpp:800의 close_in_progress 검사, :807의 기존 completion mutex try-lock, Native/socket_handle.hpp:83의 close CAS는 유지하였다. 같은 closing bit를 재진입 close에서 확인하여 현재 thread가 이미 보유한 mutex의 재획득을 피하고, 기존 completion 등록과 native close를 순서화하며, 실제 in-flight count와 close의 순서는 atomic 한 곳에서 결정한다. submit record를 직렬화하는 새 mutex는 없다. Sockets/socket.cpp:92의 null 소유 pointer 검사는 moved-from socket 처리이며, 원래 valid 선행 close 검사는 제거된 상태다.

operation_state.raw.socket과 Received._send_context_handle의 raw cache는 제거하였다. Sockets/pair.cpp:21, dealer.cpp:24·32, router.cpp:26·87·104, pubsub.cpp:28, stream.cpp:25, Messaging/received.cpp:182·194의 builder는 runtime 전달만 수행한다. Messaging/received_access.hpp:93은 raw handle 인자를 받지 않으며 :101의 context 검사는 metadata만 검사한다. runtime 소멸을 여기서 별도로 거절하던 검사는 제거되었다. 미사용 Native/native_send.hpp의 adapter와 Received send_handle getter, socket_handle의 move·reset·보호 accessor 중복도 제거된 상태다.

점검 결과는 중복 raw cache와 handle 전달, 중복 종료 결과 분류와 drain 선행 검사, 미사용 adapter의 제거를 유지한 것이다. 이번 두 번째 수정에서 새로 남긴 중복 lifecycle 상태는 0개다. 별도 이슈나 spec 변경으로 넘긴 항목은 없다. 일반 receive·subscribe·option은 이번 submit 예약 대상이 아니며, 그 API가 Core에서 실행 중일 때의 close 거절은 여전히 Core가 소유한다.

최종 산출물은 report.md와 신규 파일을 포함한 change.diff다. source-sha256.json의 변경 파일 hash는 최종 검증 뒤에도 일치하였다. 임시 복원·비교 사본과 종료한 대기 harness는 정리하였다. 요청한 main-tree와 별도 build, 재현 프로그램·로그는 검토용으로 보존하였다. 남은 binding test 실패는 없다. 별도 perf-regression smoke의 공식 build 경로 의존 실패는 위에 보존하였으며 이 작업에서 해당 runner를 수정하지 않았다.
