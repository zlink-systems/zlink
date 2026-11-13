C++ binding의 submit·publish와 request completion 수신에 handle 수명 예약을 적용했다. 각 경합 1,000회와 전체 C++ suite 27개가 통과했다. 수정 전 request·reply에서 확인한 예상 밖 오류와 ownership 실패는 수정 후 0건이었다.

1. main에서 확인한 경로와 경합 구간.

작업 위치는 /root/worktree/zlink-4060-cpp-binding이고 branch는 fix/4060-cpp-binding-submit-close다. 시작할 때 git status --short는 비어 있었다. branch는 .git에 기록된 worktree gitdir의 HEAD를 읽어 확인했다. 사용자가 제공한 시작 commit은 f784975639다. git은 status와 diff만 사용한다. 아래 main 줄 번호는 main-source/에 저장한 줄 번호를 붙인 원본을 기준으로 한다. 이 절의 Runtime/ 경로는 bindings/cpp/src/Runtime/ 아래다.

ROUTER builder는 Sockets/router.cpp:26의 send, :89의 request, :103의 reply에서 native_handle을 읽어 operation state의 raw.socket에 저장한다. PAIR·DEALER·STREAM도 같은 저장을 한다. PUB·XPUB은 Sockets/pubsub.cpp:32에서 같은 raw.socket을 저장한다. Received의 send와 reply는 Messaging/received.cpp:182, :195에서 수신 시 보관한 handle을 builder에 복사한다.

Messaging/operation_state.hpp:116의 bind_runtime_state는 runtime의 weak_ptr만 저장한다. :125의 share_runtime_state는 weak_ptr을 lock하지만 native 수명을 예약하지 않는다. Messaging/operation_submit.hpp:41의 send runtime 검사는 반환된 shared_ptr을 조건식 직후 버린다. runtime이 살아 있어도 socket_t::close는 native handle을 해제할 수 있다.

send의 native 진입은 Messaging/operation_submit.hpp:59, :64의 단일 part send·send_rid와 :98, :103의 multipart send·send_rid다. PUB publish는 같은 함수의 :69, :108에서 zlink_publish에 들어간다. builder 생성 이후, weak_ptr 확인 이후, native 배열 준비 중 어느 때든 close가 이기면 저장한 raw pointer가 해제된 상태로 Core에 전달된다.

request는 Messaging/operation_submit.hpp:165에서 zlink_request에 들어간다. Messaging/request_reply.cpp:80의 runtime 획득과 :85의 blocking completion 등록은 native close를 막지 않는다. async 경로의 :107과 completion_owner.cpp:585의 등록도 마찬가지다. 최초 제출과 writable 재제출은 같은 submit_raw_request_state를 사용한다. 동기 request는 최초 native 제출 뒤 completion_owner.cpp:687에서 zlink_completion_recv에도 들어간다. :681의 shutdown 검사와 native 진입 사이에도 close가 가능하다.

reply는 Messaging/reply_operations.cpp:21에서 runtime을 확보하고 token 소유자를 확인한 뒤 :34에서 zlink_reply를 호출한다. runtime과 token의 소유권 검사는 native close와 순서를 정하지 않는다. Received.reply와 router.reply 모두 이 terminal을 공유한다.

Sockets/socket.cpp:86의 close는 :88의 valid 확인 뒤 :90에서 socket_handle.close를 호출한다. Native/socket_handle.hpp:66의 open 확인과 :74의 zlink_close 사이에는 예약이 없다. Core가 close를 수락하면 native handle을 해제하지만 :76의 _open 변경과 Sockets/socket.cpp:95의 completion shutdown은 그 뒤다. :81의 handle과 :102의 native_handle은 저장한 raw pointer를 계속 반환한다. 따라서 close 진입 전 검사부터 해제 이후 completion shutdown 전까지 모두 경합 구간이다. 동일 handle의 두 close도 open 확인과 Core 진입 사이에서 서로 경합한다.

다른 native handle 사용은 보고만 한다. Sockets/socket.cpp의 bind:102, connect:109, unbind:116, disconnect:123, disconnect_rid:131, connection snapshot:217, TLS:261·274, receive-flow control:282는 raw handle을 읽고 바로 Core에 들어간다. 일반 receive는 :371의 recv_whole_envelope와 :391의 route-generation 조회를 별도로 수행한다. subscribe는 :438의 zlink_subscribe, subscription_event는 :471의 zlink_recv, filter 설정·해제와 조회는 :502·508·516의 직접 native 호출을 사용한다. Sockets/pubsub.cpp:76의 XPUB subscription-event 수신도 raw handle을 사용한다. Options/socket_options_detail.hpp:31의 native_option_handle은 :36에서 raw handle을 받고 :89와 :95의 zlink_get_option·zlink_set_option으로 넘긴다. 이 사이에도 close가 가능하다. Options/router_socket_options.cpp:53·66과 Options/pubsub_socket_options.cpp:83·91·99도 직접 native option API를 쓴다.  Sockets/router.cpp의 routing-id 설정·조회·route snapshot도 같은 종류의 간격이 있다. 이 작업에서 이 경로들을 해결했다고 판정하지 않는다. completion receive는 동기 request.submit의 native 진입이므로 이번 수정 범위에 포함한다. 보고만 하는 추가 경로의 최종 source 줄 번호는 Eventing/monitor.cpp:139의 socket monitor open, Eventing/poller.cpp:474·490·505·523의 socket add·modify·remove·poll 항목 생성, Sockets/stream.cpp:111의 packet 수신, Sockets/proxy.cpp:15·21의 proxy 진입이다. 이 경로들은 socket handle을 읽고 Core에 넘기는 사이에 같은 예약을 사용하지 않는다.

2. 설계와 소유권.

소유 계층은 C++ binding의 socket_handle이다. binding은 native pointer가 살아 있는 동안에만 Core에 전달할 책임을 갖고, Core는 실제 API 수락과 record 동시성을 결정한다. 근거는 bindings/doc/spec/README.ko.md:498의 socket-local multipart gate 금지와 :1851의 종료 후 reply.submit TERMINATED 계약, core/doc/spec/core/socket/README.ko.md:58, :647, :657의 BUSY·ESHUTDOWN·해제 pointer 금지 계약이다.

교차언어 대조는 ../bclose/report.md와 change.diff의 Java #4073 설계다. Java는 기존 ownerLock 안에서 closed 확인과 예약 증가를 합치고, 예약 해제는 AtomicInteger로 수행한다. C++의 즉시 send·reply·publish에는 대응하는 기존 owner mutex가 없으므로 native 호출 전체를 mutex 안에서 실행하는 대안과 원자 상태로 native 수명을 예약하는 대안을 비교했다. 전자는 독립된 record의 native 실행을 직렬화한다. 최종 수정은 기존 completion 등록 mutex를 유지하고 native 수명 예약은 원자 상태 하나로 수행한다. 변경 분류는 B다.

Native/socket_handle.hpp:96의 _state는 기존 _open을 대체하는 std::atomic<uint64_t>다. 상위 두 bit는 closing과 closed이고 나머지는 진행 중 native 호출 수다. 별도 counter나 closed flag를 다른 객체에 복제하지 않는다. 기존 completion_owner._socket은 void*에서 shared_ptr<socket_handle_t>로, socket_t._socket은 unique_ptr에서 같은 객체를 가리키는 shared_ptr로 바꿨다. runtime에는 socket 필드를 추가하지 않았다. operation_state.raw.socket과 Received._send_context_handle은 제거했다. socket_call_t의 임시 shared_ptr은 제출 중 socket_t 자체가 사라져도 handle 객체를 유지하기 위한 소유 참조다. socket_t와 Received의 private layout이 바뀌므로 C++ consumer의 기존 object file은 새 header로 다시 컴파일해야 한다. 새 mutex·timer·queue·옵션은 없다.

Native/socket_handle.hpp:53의 reserve는 acquire load 뒤 open 상태에서 count를 1 늘리는 compare_exchange_weak(acq_rel, acquire)를 수행한다. :67의 close는 count가 0인 open 상태만 compare_exchange_strong(acq_rel, acquire)으로 closing으로 바꾼다. 둘은 같은 atomic의 modification order에서 순서가 정해진다. reserve가 먼저 성공하면 close는 BUSY/EBUSY다. close가 먼저 성공하면 새 reserve는 실패하며 Core에 handle을 넘기기 전에 TERMINATED/ESHUTDOWN을 고른다. Native 실행 동안 예약을 잡은 submit끼리는 서로 기다리지 않는다. Linux x64에서 uint64_t atomic이 항상 lock-free라는 static_assert를 contract test에 넣었다.

:65의 release는 fetch_sub(1, release)다. 마지막 release의 0을 읽는 close CAS의 acquire가 native 실행 완료와 연결된다. 중간 예약·해제 RMW도 release sequence를 끊지 않는다. :78은 native close 성공 또는 Core의 SHUTDOWN이면 closed를 release store한다. Core가 close를 거부하면 open 0으로 release store한다. 이후 예약은 같은 원자 값을 acquire로 읽는다. closed 전이는 되돌리지 않으므로 count가 다시 0이 되는 경우에도 해제된 pointer가 open으로 관측되지 않는다.

TERMINATED를 고르는 predicate는 Native/socket_handle.hpp:21의 require_submit_open 하나다. :89의 is_open_for_submit은 같은 원자 값의 closing·closed bit를 검사하는 단일 함수다. 예약과 기존 runtime 획득 경계에서 이 조건을 사용한다. Messaging/operation_state.hpp:124는 request가 기존 등록 mutex에 들어가기 전에 runtime 소멸·closing·closed를 확인한다. native 인자 준비 앞에서는 reservation CAS로 다시 순서를 정한다. 첫 확인은 native handle을 읽거나 Core를 호출하지 않는다. Native close 안에서 같은 socket으로 request가 재진입하는 경우에도 등록 mutex를 기다리지 않고 TERMINATED가 된다.

예약의 실제 증가 위치는 Native/socket_handle.hpp:59 한 곳이고 감소 위치는 :65 한 곳이다. socket_call_t constructor :118이 증가를 요청하고 destructor :120가 감소한다. Messaging/operation_submit.hpp:41은 send·routed send·PUB/XPUB publish의 단일 part와 multipart를 모두 보호한다. 같은 파일 :150은 최초 request와 writable request 재제출을 보호한다. Messaging/reply_operations.cpp:23은 Received.reply와 router.reply가 공유하는 reply terminal을 보호한다. 정상 반환과 예외 모두 같은 RAII destructor로 해제한다. 종료 거절 시에는 메시지를 caller source로 돌려준 뒤 예외를 전달한다.

Sockets/socket.cpp:305·316·327·341의 보호된 send overload와 :406·419의 publish overload도 같은 guard를 사용한다. native 진입은 이 guard의 handle()을 통해서만 이루어진다. native 준비 중 메시지 검증·배열 준비·timeout 변환이 실패해도 예약이 해제된다. request는 최초 native 제출 후 예약을 해제하며 pending reply를 보유한다는 이유로 count를 유지하지 않는다.

동기 request의 completion 수신도 같은 handle 소유자를 쓴다. Messaging/completion_owner.cpp:683은 기존 _mutex 구간 안에서 shutdown 확인과 예약을 마치고, 잠금을 해제한 뒤 :691에서 zlink_completion_recv에 들어간다. errno를 보관한 뒤 :696에서 예약을 해제한다. :798의 close_native_socket은 같은 기존 mutex를 try_lock으로 한 번만 시도한다. 사용 중이면 BUSY/EBUSY를 반환한다. 잠금을 얻으면 유지한 채 socket_handle.close를 호출한다. 따라서 completion 예약은 Core close가 실패한 일시적인 closing 상태를 보지 않는다. Core가 BUSY를 반환하면 open 상태를 복원한 뒤 mutex를 해제하므로 completion 진행과 기존 pending을 유지한다. Core close가 성공한 뒤에는 guard가 거절하고 public close가 기존 shutdown으로 pending을 끝낸다. Native cleanup에서 close가 재진입하면 같은 원자 값의 closing bit를 먼저 확인해 BUSY를 반환한다. std::mutex의 try_lock에도 현재 thread가 해당 mutex를 소유하지 않는 전제가 있으므로 재진입 경로에서는 mutex를 시도하지 않는다. 근거는 [C++ standard draft의 mutex 요구 사항](https://eel.is/c++draft/thread.mutex.requirements.mutex.general#14)이다.

Sockets/socket.cpp:84의 close_native를 destructor :54, move assignment :66, public close :93이 함께 사용한다. native handle 객체의 최종 destructor는 마지막 shared_ptr이 사라졌을 때만 직접 close하므로 그 시점에는 completion owner나 native guard가 남아 있지 않다. Public close는 기존 completion owner의 try_lock 실패나 진행 중 예약에 대해 BUSY를 반환하며 내부에서 기다리거나 다시 시도하지 않는다. 호출자가 언제 다시 close할지 결정한다. close errno는 completion shutdown 전에 보관한다.

다음 lock 수는 열린 socket의 정상 제출 경로에서 completion_owner::_mutex에 직접 들어가는 횟수다. send.submit, reply.submit, publish.submit은 main 0회, 수정 후 0회다. send.async의 즉시 수락도 0회에서 0회이고 BACKPRESSURED 등록은 1회에서 1회다. request.async의 최초 등록과 blocking request의 최초 등록은 각각 1회에서 1회다. 실패 정리의 unregister 잠금, entry·async result의 기존 mutex와 inline drain mutex는 그대로다. 예약과 해제는 모든 경로에서 mutex 진입 0회다. completion_recv마다 기존 owner mutex 1회를 재사용하며 drain 시작 시의 중복 shutdown 잠금 1회는 제거했다. close는 main 0회에서 기존 owner mutex try_lock 1회가 된다.

수정 전/후 규칙 수는 binding이 runtime 소멸·owner shutdown을 합성 submit 오류로 바꾸던 결과 판정 위치 9곳에서 require_submit_open 1곳이다. main의 operation_submit.hpp:41·147, reply_operations.cpp:22, request_reply.cpp:81·108, send_operations.cpp:42와 completion_owner.cpp:589·601·612를 센 값이다. Runtime 소유 참조, 예약, 등록 경계의 사실 확인까지 한 번으로 없앴다고 세지 않는다. 최종 helper 호출 경계는 runtime 획득, native 예약, 초기 request가 이미 종료된 경우와 기존 등록 세 곳이다. Native 결과를 그대로 submit domain에 전달하는 공통 변환과 late completion 결과 판정은 이 합성 종료 판정 수에 포함하지 않았다.

3. 검증.

수정 전 경합 집계와 결정적 실패를 보존한 뒤 production 수정안을 적용했다. 최종 build-after2.log 빌드와 포맷 검사는 모두 통과했다. 경합 테스트는 네 경로 각각 1,000회, 회차마다 같은 handle을 쓰는 submit thread 네 개와 close 한 개를 barrier에서 동시에 출발시킨다. reply는 서로 다른 실제 request token을 사용한다. request는 async admission 결과를 검사한다. 결과는 OK와 TERMINATED/ESHUTDOWN만 허용하고 message ownership도 검사한다. close가 BUSY면 submit thread가 끝난 뒤 fixture 정리에서 close하며 submit을 다시 제출하지 않는다.

결정적 테스트는 native wrapper에서 두 submit을 동시에 멈춰 close BUSY를 검사한다. closing 상태에서는 native submit wrapper의 진입 수가 증가하지 않으면서 TERMINATED여야 한다. 종료된 socket과 runtime이 이미 사라진 builder도 같은 결과와 message ownership을 검사한다. 동기 request의 completion receive도 wrapper에서 멈춰 BUSY를 검사한다. 잘못된 message와 범위를 넘는 timeout, native close의 BUSY를 만들어 예약 해제와 owner 재사용도 확인한다.

수정 전의 전체 경합은 main.log와 main-outcomes.json에 있다. 같은 worktree에서 production 소스를 바꾸기 전에 library를 빌드했다. test와 CMake 등록만 추가했다. 네 경로 각각 1,000회와 제출 4,000건을 완료했으며 프로그램은 exit 1이었다.

send는 OK 1,466건, TERMINATED/108 2,534건이었다. close는 OK 999회, BUSY 1회였다. request는 OK 653건, TERMINATED/108 1,987건, INVALID_STATE/108 1,320건, INTERNAL_ERROR/EPROTO 37건, BACKPRESSURED/0 3건이었다. close는 OK 996회, BUSY 4회였다. request의 message ownership 실패 37건도 있었다. reply는 OK 781건, TERMINATED/108 3,153건, NOT_FOUND/ENOENT 66건이었다. close는 OK 994회, BUSY 6회였다. publish는 OK 1,370건, TERMINATED/108 2,630건이었다. close는 OK 981회, BUSY 19회였다. send·reply·publish의 ownership 실패는 0건이었다. EFAULT, ESTALE, INVALID_HANDLE은 네 경로 모두 0건이었다.

첫 전체 시도 main-aborted.log는 request async가 예상 밖의 BACKPRESSURED를 반환한 즉시 assertion에서 중단됐다. 이때 stress-ng는 main-aborted-load.log에 기록된 7.91초 동안 실행됐고 SIGTERM으로 종료했다. 이후 테스트의 허용 결과는 유지하면서 unexpected 결과를 모두 집계한 뒤 exit 1로 실패하도록 바꿨다. 그 동일한 테스트로 위 main.log의 1,000회를 완료했다. 추가 격리 실행 main-reply.log도 1,000회 중 NOT_FOUND/ENOENT 28건을 재현했다. main-send.log의 추가 send 1,000회는 OK 825건과 TERMINATED 3,175건이었다. 이 추가 실행은 main.log의 집계와 합산하지 않는다.

수정 후 단독 경합은 after-races.log와 after-outcomes.json에 있다. 네 경로 각각 1,000회, 제출 4,000건을 완료했고 exit 0이었다. send는 OK 1,208건과 TERMINATED/108 2,792건, close OK/BUSY는 810/190회였다. request는 OK 1,598건과 TERMINATED/108 2,402건, close OK/BUSY는 648/352회였다. reply는 OK 1,389건과 TERMINATED/108 2,611건, close OK/BUSY는 735/265회였다. publish는 OK 1,596건과 TERMINATED/108 2,404건, close OK/BUSY는 782/218회였다. EFAULT, ESTALE, ENOENT, INVALID_HANDLE, NOT_FOUND, INVALID_STATE, EPROTO, BACKPRESSURED와 기타 예상 밖 결과는 모두 0건이었다. message ownership 실패도 네 경로 모두 0건이었다.

결정적 검사는 after-admission.log에서 exit 0이었다. native wrapper에 두 독립 record가 함께 진입한 것을 확인한 뒤 close의 BUSY를 검사했다. closing·closed에서 native submit 진입 수가 늘지 않는 것, 같은 socket의 두 번째 close가 BUSY로 끝나는 것, Native close 경계에서 send·request·close가 재진입해도 대기하지 않는 것을 확인했다. Core close의 BUSY를 의도적으로 만든 뒤 send 성공과 기존 request의 reply 수신도 확인했다. invalid message와 uint32 범위를 넘는 timeout은 예상 오류로 실패하고 이후 close는 성공했다. socket 소멸 후의 send·request·reply·publish builder와 Received.send·reply도 TERMINATED와 원래 message 보존을 확인했다. main-admission.log는 수정 전 동일한 첫 예약 검사에서 close가 성공해 assertion으로 실패한 기록이다.

전체 C++ binding suite는 after-full.log와 results/full/LastTest.log에 있다. ctest --test-dir /root/jobs/fx/cx/v21/s033/cbclose/build --output-on-failure -j3을 한 번 실행해 27/27 통과했고 failure·skip은 0개였다. contract 25개와 stress 2개다. CTest 시간은 32.15초였다. 이 실행에 포함된 기존 test_cpp_send_close_stress도 제출·수신 50,000건, close 성공 5회·BUSY 5회, 예상 밖 결과·ownership 실패·손상 record 0건으로 통과했다. 전체 suite 안에서도 새 경합을 각 1,000회 수행했다. send의 OK/TERMINATED는 1,269/2,731건, request는 1,658/2,342건, reply는 1,363/2,637건, publish는 1,590/2,410건이었다. close OK/BUSY는 각각 790/210, 648/352, 759/241, 782/218회였다. 예상 밖 결과와 ownership 실패는 모두 0건이었다. 이 집계는 단독 경합과 합산하지 않는다.

빌드는 /root/jobs/fx/cx/cpp-build-slot.sh cmake --build /root/jobs/fx/cx/v21/s033/cbclose/build -j3으로 실행했다. 초기 configure는 cmake -S bindings/cpp -B /root/jobs/fx/cx/v21/s033/cbclose/build -DZLINK_CPP_BUILD_TESTS=ON -DZLINK_CPP_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core/1.23.0/linux-x64 -DCMAKE_BUILD_TYPE=RelWithDebInfo였고 같은 build wrapper를 사용했다. configure.log, build-main.log, build-main-race2.log, build-main-admission2.log, build-after2.log가 각 빌드 기록이다. 추가 admission test의 최초 빌드는 helper 정의 누락으로 실패했고 build-main-admission.log에 남겼다. 정의를 보완한 뒤 build-main-admission2.log에서 성공했다. 최종 검증은 build-after2.log의 library와 executable로 실행했다.

main의 전체 경합은 /root/jobs/fx/cx/measure-shared.sh python3 /root/jobs/fx/cx/v21/s033/cbclose/run-loaded.py main /root/jobs/fx/cx/v21/s033/cbclose/build/test_cpp_submit_close_race all 1000으로 실행했다. 수정 후에는 같은 wrapper 아래 run-loaded.py after-load python3 validation-steps.py after를 실행했다. validation-steps.py가 결정적 검사, 단독 경합, 전체 CTest를 차례로 실행했다. after-validation.json은 각 명령·exit code·시각·시간을 기록한다. 결정적 검사는 0.100초, 단독 경합은 30.642초, 전체 CTest process는 32.176초였다.

run-loaded.py가 measure-shared 안에서 stress-ng --cpu 16 --cpu-method loop --metrics-brief를 시작하고 각 test process를 실행했다. 수정 후의 세 검증은 같은 16 CPU 부하 아래 실행됐다. after-load-load.log에서 부하 시간은 62.96초였고 CPU stressor 16개가 모두 끝났다. runner의 finally에서 stress-ng에 SIGTERM을 보내고 wait로 종료를 확인했다. main-load.log와 각 추가 baseline의 *-load.log도 같은 방식으로 종료했다. main-memory.log와 after-load-memory.log는 MemAvailable을 1초마다 기록했다. 전체 main 경합의 최저는 19,010,772 kB, 18.13008 GiB였고 수정 후 검증의 최저는 18,395,912 kB, 17.54371 GiB였다. 추가 baseline을 포함한 모든 표본의 최저도 14.11491 GiB로 12 GiB 이상이었다. memory-summary.json에 각 log의 표본 수와 최저값을 보존했다.

Core runtime은 /root/.cache/zlink/core/1.23.0/linux-x64/lib/libzlink.so.1.23.0이다. SHA-256은 6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01로 Java 비교 실행과 같다. link-race.txt와 link-admission.txt가 실제 linker 입력과 rpath를 보존한다. main-provenance.json과 after-provenance.json에 source·static library·실행 파일·Core의 hash를 남겼다. after-provenance.json의 Core hash는 main과 같다. Source와 compiled binding은 이 worktree에서 갱신한 최종 결과다.

scripts/format/format.sh의 cpp source_roots는 framework/languages/cpp만 포함하므로 bindings/cpp의 변경 C++ 파일에만 저장소 .clang-format과 clang-format-18을 적용했다. touched-cpp-files.json의 모든 파일에서 clang-format-18 --dry-run --Werror가 exit 0이었고 git diff --check도 exit 0이었다. 새 test 파일의 git diff --no-index --check는 파일 차이를 뜻하는 exit 1이며 공백 진단은 없었다. format-and-whitespace.log와 .json에 기록했다. 초기 검사 wrapper가 이 exit 1을 실패로 잘못 판정한 기록은 format-and-whitespace-first.*에 분리했다. 판정을 바로잡고 의도적으로 형식이 틀린 C++ 입력과 trailing whitespace 입력도 검사했다. 전자는 exit 1과 formatter 진단, 후자는 exit 3과 whitespace 진단으로 검출됐으며 정상 파일들은 모두 통과했다. 음성 대조용 임시 파일은 검사 후 삭제했다.

변경 파일 전체는 changed-files.txt의 23개와 change.diff에 기록했다. Native/native_send.hpp는 삭제했고 신규 test는 tests/contract/test_cpp_contract_submit_close_admission.cpp와 tests/stress/test_cpp_submit_close_race.cpp다. 현재 변경 범위는 bindings/cpp뿐이며 Core·다른 binding·Framework·spec은 수정하지 않았다. commit·push·GitHub 작업은 하지 않았다. 남은 test 실패는 없다.

Runtime의 소유 계층은 C++ binding의 socket_handle이며 실제 API 수락은 Core가 소유한다.

Spec 근거는 bindings/doc/spec/README.ko.md:498·1851과 core/doc/spec/core/socket/README.ko.md:58·647·657이다.

교차언어 대조 결과는 Java #4073과 같은 native 호출 수명 예약이며 기존 mutex와 C++ 원자 상태를 사용했다.

변경 분류는 B다.

CMake cache·linker 입력·compiler flags와 CTest 결과는 이 디렉터리에 복사했고 임시 build 디렉터리는 검증 뒤 삭제했다. 중간 source 사본과 구현용 임시 스크립트도 삭제했다. test·부하 process는 모두 종료됐다. 재현용 run-loaded.py, validation-steps.py, check-format.py와 요청한 보고서·diff·로그는 보존했다.


4. PUB publish.

PUB·XPUB builder의 publish와 socket_t의 보호된 저수준 publish 두 overload 모두 동일한 native 해제 간격이 있었다. builder는 send와 같은 submit_raw_send_state의 raw_publish branch를 거친다. 같은 socket_handle 예약을 적용했으며 별도 PUB 상태나 lock은 없다. main의 PUB 경합 1,000회에서는 예상 밖 오류가 관측되지 않았지만 static 경로에는 같은 수명 간격이 있었고, 수정 후 native-entry 검사와 부하 경합을 모두 통과했다. 수정 후 PUB 1,000회의 제출 4,000건은 OK 1,596건과 TERMINATED 2,404건이며 Native 오류와 ownership 실패는 0건이었다. 전체 suite의 독립된 PUB 1,000회도 OK 1,590건과 TERMINATED 2,410건이었다.

PR 본문에는 다음 내용을 넣을 수 있다. C++ binding의 send·request·reply·PUB/XPUB publish는 같은 socket_handle 예약을 사용한다. close 중이거나 완료된 socket은 submit 단계에서 TERMINATED/ESHUTDOWN으로 끝나며 released handle을 Core에 넘기지 않는다. Native 호출 중 close는 BUSY이며 호출자가 다시 시도할 시점을 결정한다. PUB publish도 수정 범위에 포함했고 1,000회 부하 경합과 전체 C++ suite를 통과했다. Refs #4060.

5. 리팩토링 점검.

main의 operation_state.raw.socket과 raw.runtime은 같은 socket을 두 방식으로 가리켰다. Sockets/pair.cpp:21, dealer.cpp:24·33, router.cpp:26·89·103, stream.cpp:25, pubsub.cpp:32, Messaging/received.cpp:182·195에 있던 raw handle 할당 열 곳을 제거했다. operation_state.raw.socket 필드와 reset도 제거했다. 모든 builder는 runtime의 기존 completion owner를 통해 예약한 handle을 받는다.

main의 Received._send_context_handle과 received_access.hpp:93·97·115의 setter·getter도 같은 pointer를 다시 보관했다. 이 field와 getter를 제거하고 Sockets/socket.cpp:396, router.cpp:42의 두 수신 context 전달에서 raw handle 인자를 제거했다. received_access.hpp:107의 runtime.expired 선행 거절도 제거했다. routing context 유무는 metadata로 판단하고 소유자 종료는 submit의 공통 predicate가 판단한다. 메시지를 보유한 Received에서 socket 소멸 후 send·reply를 만드는 경우와 이미 만든 builder의 소유자 소멸을 별도로 검사한다.

main의 runtime 소멸 검사 여섯 곳과 completion 등록의 shutdown 검사 세 곳은 서로 다른 invalid_state errno를 고르던 중복 결과 판정이었다. 최종 결과 분류는 Native/socket_handle.hpp:21 하나다. 기존 request 등록 mutex의 shutdown 확인과 native 직전 reservation은 각각 completion 등록과 native pointer 수명이라는 다른 경계이므로 유지한다. 초기 request가 등록 후 close에 의해 종료되면 main completion_owner.cpp:282는 재제출과 같이 건너뛰어 BACKPRESSURED를 반환했다. 최종 :286은 최초 제출을 공통 TERMINATED predicate로 끝내고 이미 종료된 재제출은 기존처럼 끝낸다.

main Sockets/socket.cpp:88의 close 전 valid 확인은 제거했다. native close가 BUSY인지 terminal인지는 socket_handle이 결정하고 public close는 반환 결과에 따라 completion owner를 종료한다. main completion_owner.cpp:674의 drain 시작 시 shutdown 확인은 바로 이어지는 loop의 같은 잠금·검사와 중복돼 제거했다. completion 수신 예약은 그 loop의 기존 mutex 구간을 재사용한다.

사용되지 않는 member·helper는 main Native/socket_handle.hpp:30의 move constructor, :40의 move assignment, :81·82의 protected handle accessor와 :84의 reset_handle을 제거했다. socket_t는 공유 소유 참조를 옮기므로 handle 객체의 native identity를 바꾸는 이 경로들은 필요하지 않다. Native/native_send.hpp:19·28·42의 Received submit adapter 세 함수는 호출자가 없으며 operation-state 경로와 오류·ownership 전달을 중복 서술하므로 파일과 socket.cpp의 미사용 include를 제거했다. Received의 미사용 send_handle getter를 합쳐 미사용 함수 아홉 개를 제거했다.

원자 상태는 기존 open flag를 대체했고 임시 owning guard는 builder와 Received의 raw cache를 대체했다. Runtime에 별도 socket field를 더하지 않았다. reply token의 owner 검사, routing target·topic·message 검증, pending registry와 completion의 기존 mutex는 각 계약 경계의 검사이므로 유지했다. report-only로 열거한 일반 receive·options·subscribe·control 호출은 이번 수정으로 보호됐다고 판정하지 않는다.

점검 결과는 raw cache 두 개, raw handle builder 전달 열 곳과 수신 context 전달 두 곳, 중복 close 사전 검사와 drain 시작 검사, 중복 결과 분류를 정리하고 미사용 함수 아홉 개를 제거한 것이다. 별도 이슈로 넘긴 항목은 없으며 다른 handle 사용의 경합 위치는 이 보고서 1절에 남겼다.
