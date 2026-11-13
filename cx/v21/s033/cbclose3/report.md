C++ binding #4060의 submit/close 예약에서 소유권 atomic 비용을 줄였다. 사용자가 지정한 fix/4060-cpp-binding-submit-close worktree의 기존 미커밋 변경 위에서 작업하였다. cbclose2의 close 결과 대기, BUSY 뒤 재예약, 성공 close 뒤 TERMINATED, 메시지 소유권 복원과 기존 테스트를 유지하였다. commit·branch 작업·push·GitHub 작업·spec 수정은 하지 않았다. 다른 agent나 model은 사용하지 않았다.

1. Atomic 수.

정상 open socket, CAS 충돌 없음, builder 한 번 생성·반납, native submit 한 번을 기준으로 센다. 표의 각 셀은 RMW / plain atomic load다. Core 내부 atomic과 mutex 구현 내부 atomic은 포함하지 않는다. Completion registry·async result의 변하지 않은 연산은 아래 별도로 설명한다. weak_ptr::expired와 last-owner 파괴는 정상 hot path가 아니다.

| 경로 | main | cbclose2 | now |
|---|---:|---:|---:|
| builder send submit, blocking | 6 / 4 | 10 / 6 | 6 / 3 |
| builder publish submit, blocking | 6 / 4 | 10 / 6 | 6 / 3 |
| builder publish submit, DONTWAIT | 6 / 4 | 10 / 6 | 6 / 3 |
| async send, 첫 DONTWAIT 시도 | 8 / 7 | 12 / 10 | 8 / 6 |
| request, blocking·async 첫 submit | 8 / 7 | 12 / 10 | 8 / 6 |
| reply, reply_token copy 제외 | 6 / 4 | 10 / 6 | 8 / 6 |
| socket_t::send, blocking·DONTWAIT | 0 / 0 | 4 / 2 | 2 / 1 |
| socket_t::publish, blocking·DONTWAIT | 0 / 0 | 4 / 2 | 2 / 1 |
| retry native send/request 시도, builder 재생성 없음 | 2 / 3 | 6 / 5 | 2 / 1 |
| completion_recv 한 번 | 0 / 0 | 4 / 2 | 2 / 1 |

main의 builder 생성에는 socket_access_t::runtime_state 반환 shared_ptr의 +1/-1 RMW 2개와 weak_ptr 저장·반납 RMW 2개가 이미 있다. now는 weak_ptr 저장·반납을 handle shared_ptr 저장·반납으로 대체하므로 RMW 4개가 같다. strong handle 반납의 libstdc++ unique-owner 검사 load 하나가 증가한다. Builder send에 공개 DONTWAIT submit overload는 없으며 async()가 DONTWAIT 경로다. Publish flags(DONTWAIT)는 같은 sync submit helper를 사용한다.

std::weak_ptr::lock은 성공 시 CAS 1개와 strong 반납 RMW 1개, 시작 use_count load 1개, __shared_ptr weak 생성자에서 CAS 성공 뒤 use_count를 다시 확인하는 load 1개, strong 반납 unique-owner load 1개로 load 총 3개다. shared_ptr 복사·반납은 RMW 2개와 unique-owner load 1개다. weak 저장·반납은 RMW 2개이며 count를 따로 load하지 않는다. GCC 13 libstdc++ shared_ptr_base.h:269–283, :317–356, :200–224, :1731–1734를 기준으로 하였다. libc의 thread 활성 여부를 읽는 일반 메모리 load는 atomic load가 아니다.

Main의 cached raw handle 경로에도 operation_submit.hpp:41의 share_runtime_state가 있어서 실제 native helper에 weak lock 2 RMW가 이미 있다. Request helper :147에도 같은 호출이 있어 request·async send는 setup lock과 helper lock 두 번을 수행한다. 이를 제거한 now의 sync send/publish와 async send/request는 main과 전체 RMW 수가 같다. Main에 ownership atomic이 없다는 초기 설명은 재사용 main-tree의 소스와 binary에 맞지 않았다. Main PAIR helper의 assembly-submit-mangled-main.txt에는 0x3620b lock cmpxchg와 0x366f5 lock xadd가 있다. 기존 cbclose2/binary-provenance.json의 main binary와 hash가 같으므로 main을 바꿔 생긴 차이가 아니다.

cbclose2 sync submit은 builder 생성·반납 4 RMW에 runtime lock 2 RMW, handle 복사 2 RMW, reserve/release 2 RMW를 합쳐 총 10 RMW다. now는 handle 소유권을 생성 시 확보하므로 reserve CAS 1개와 release fetch_sub 1개만 더한다. reserve에는 초기 acquire load 1개가 있다. Async/request의 cbclose2 share_runtime_state는 추가 closed load 1개도 수행한다. now는 이를 제거하였다. Reply의 cbclose2는 runtime lock을 이미 직접 재사용하므로 10 RMW이며 12가 아니다.

Received.send()/reply()의 builder 생성은 socket_access 대신 Received runtime weak lock을 사용하므로 표의 socket factory 기준보다 세 버전 모두 atomic load 2개가 많다. Main만 has_send_context의 weak_ptr::expired() load 1개가 더 있어 main은 총 3개가 많다. reply token의 shared_ptr owner copy·파괴 RMW 2개와 strong release load 1개는 세 버전에 공통으로 더한다. 수신 record의 metadata 생성 비용은 포함하지 않는다.

CAS 실패는 성공 예약 RMW 수와 별도로 재시도 횟수만큼 CAS 명령을 추가하며, closing 대기의 wait와 재관측은 경합 횟수에 달린다. Backpressure retry마다 위 retry 행이 추가된다. Blocking request 전체에는 drain의 실제 completion_recv 호출 수 N에 따라 now +2N RMW, cbclose2 +4N RMW가 더해진다. 따라서 요청 전체의 증가를 +2라고 표현하지 않는다. 각 native 호출에 +2라는 요구를 적용하였다.

추가 completion/result 비용도 main에 이미 있다. 즉시 성공 async send는 반환된 immediate_send_result의 마지막 shared_ptr 파괴에 atomic load 1개가 있고 RMW는 0개다. 따라서 그 파괴까지 센 총수는 main 8/8, cbclose2 12/11, now 8/7이다. Result를 기다리는 사용자 코드의 ready()/co_await 횟수는 호출 형태에 따라 달라 고정된 per-message 수가 없다.

경합과 retry가 없는 blocking request에서 completion entry 등록·조회·해제는 RMW 4개, strong release load 3개를 추가한다. 이를 위 첫 submit 행에 더하고 실제 completion_recv 횟수를 N이라 놓으면, 전체 binding 요청은 main 12/10, cbclose2 (16+4N)/(13+2N), now (12+2N)/(9+N)이다. Client-side completion 하나와 NO_DATA 하나를 받는 N=2의 경우 각각 12/10, 24/17, 16/11이다. Server reply는 별도 reply 행이다. No-data polling을 한 번 더 하면 now는 예약 RMW 2개와 load 1개가 더 필요하며, 이를 native submit 한 번의 비용과 혼동하지 않는다.

Async request와 backpressured async send에는 bundle shared_ptr alias, result lifetime weak_ptr, registry shared_ptr, ready/terminal atomic, continuation 슬롯이 main부터 있다. 최초 submit까지의 표는 이 비변경 completion machinery를 제외한 소켓 수명·예약 비용이다. 임의 coroutine scheduler·ready polling까지 포함한 전체 메시지에 고정 숫자를 붙이지 않는다. Async/result 코드의 추가 연산은 세 버전이 같으며, 이번 수정으로 추가한 atomic은 native 호출별 예약·해제뿐이다.

고정할 수 있는 async 공통 비용도 수로 적는다. Continuation을 등록하지 않고 완료 후 결과를 소비하며, 조기 completion·취소·예외가 없는 경우다. P는 result.ready() 횟수, T는 result.take()/await_resume() 횟수다. Mutex 내부 atomic은 앞 표와 같이 제외한다.

| main부터 공통인 completion/result 연산 | RMW | plain atomic load | plain atomic store |
|---|---:|---:|---:|
| 즉시 성공 async send result 생성·파괴 | 0 | 1 | 0 |
| blocking request entry 등록·terminal capture·해제 | 4 | 3 | 0 |
| async request bundle·entry·결과 두 개의 전체 수명, terminal completion 한 개 | 18 | 9+P+T | 2 |
| backpressured async send bundle·entry·결과 한 개의 전체 수명, WRITABLE 한 개 후 성공 | 12 | 6+P+T | 1 |

Async request의 18 RMW는 bind_lifetime의 shared_ptr<void> 변환 임시 참조 두 쌍 4개, lifetime weak 저장·파괴 두 쌍 4개, entry alias 쌍 2개, registry 쌍 2개, admitted result alias 쌍 2개, drain entry 참조 쌍 2개, 최초 bundle의 마지막 strong·implicit weak 반납 2개다. Strong release load는 7개이고 admitted/reply terminal load가 각각 1개이므로 9+P+T다. Store 두 개는 각 terminal publication이다.

Backpressured async send의 12 RMW는 bind_lifetime 임시 strong 쌍 2개, lifetime weak 쌍 2개, entry alias 쌍 2개, registry 쌍 2개, drain entry 참조 쌍 2개, 최초 bundle의 마지막 strong·implicit weak 반납 2개다. Strong release load 5개와 complete의 terminal load 1개로 6+P+T다. 추가 poll·coroutine suspend는 별도 shared resume slot/weak lifetime lock을 실행하므로 이 정상 완료 예의 숫자에 포함하지 않는다.

예를 들어 retry 없는 async request 전체는 main RMW 26개, cbclose2 30+4N개, now 26+2N개다. Plain atomic load는 각각 16+P+T, 19+P+T+2N, 15+P+T+N이다. Result terminal store 2개는 세 버전이 같다. Backpressured async send에서 성공까지 native 시도가 A번, WRITABLE 처리 후 NO_DATA 확인을 포함한 completion receive가 N번이면 전체 RMW는 main 18+2A, cbclose2 18+6A+4N, now 18+2A+2N이다. 두 식 모두 builder 생성·반납을 한 번만 세며 위 공통 completion 비용을 포함한다.

Handle의 weak runtime link 설정·파괴는 socket 하나당 weak count RMW 2개다. 메시지마다 발생하지 않는다. 새 weak link의 저장 공간은 handle 하나당 weak_ptr 하나이며 기존 operation state의 weak_ptr를 strong shared_ptr로 대체해 state 크기를 늘리지 않는다.

2. 소유권과 thread 수명.

이하 소스 경로는 bindings/cpp 아래다. src/Runtime/Native/socket_handle.hpp:126의 socket_call_t는 socket_handle_t&를 빌리고 :134에서 예약만 해제한다. Strong owner는 호출 전체에 걸쳐 호출자가 보유한다는 설명을 :124에 두 줄로 적었다. native pointer를 argument 준비 전에 예약하고 결과와 errno를 얻은 뒤 해제하는 cbclose2의 범위는 바꾸지 않았다. :58의 reserve, :77의 release, :79의 close의 atomic 순서와 wait/notify 코드는 cbclose2 그대로다. reserve부터 close까지 함수 본문을 기존 snapshot과 byte 단위로 비교해 같음을 확인하였다(correctness-preservation.json).

src/Runtime/Sockets/socket.cpp:312·323·334·347·410·424의 sync send/publish는 socket_t::_socket이 handle owner다. close()는 이 pointer를 reset하지 않는다. 같은 socket 객체를 파괴·move하면서 그 객체의 member 함수를 호출하는 것은 일반 C++ 객체 수명 계약 밖이며 이번 close 경합의 보장이 아니다. Close와 submit의 경합에서는 객체의 소유 pointer를 수정하지 않는다.

src/Runtime/Messaging/operation_state.hpp:84의 operation state는 strong handle 하나를 보유한다. :115에서 builder 생성 시 기존 weak runtime 저장 대신 handle을 복사하고 :89에서 state 반납 시 해제한다. 기존 weak 저장·반납과 같은 RMW 두 개다. src/Runtime/Messaging/operation_submit.hpp:41·150은 이 owner에서 직접 handle을 빌린다. Sync send/publish submit의 runtime lock과 handle 복사는 0개가 되었다. State는 native submit 시도와 실패 복원 동안 계속 살아 있다. Async retry entry도 같은 operation state를 보유하므로 재시도마다 소유 pointer를 다시 잠그거나 복사하지 않는다.

Completion registry를 operation state가 강하게 보유하는 대안도 검토하였다. Operation→runtime→completion→entry→operation 순환이 생길 수 있어 선택하지 않았다. Handle은 completion registry를 강하게 보유하지 않는다. src/Runtime/Native/socket_handle.hpp:51의 weak runtime link를 socket.cpp:306에서 socket을 외부에 공개하기 전에 한 번 설정한다. 이후 이 link에 쓰는 곳은 없다. Per-builder weak runtime field를 제거하고 per-socket weak link로 옮겼다. 새 lifecycle flag·queue·lock·timer는 0개다.

src/Runtime/Messaging/operation_state.hpp:123의 runtime weak lock은 async send의 send_operations.cpp:43, blocking/async request의 request_reply.cpp:80·105, reply_operations.cpp:24에서 한 번만 수행한다. 기존에 completion setup을 위해 필요했던 lock이며 native helper는 이를 다시 수행하지 않는다. Runtime이 소멸한 경우는 동일한 TERMINATED/ESHUTDOWN predicate로 실패한다. Live runtime에 대한 accepting_submits()의 closed 선행 load는 제거하였다. Async send의 own_async_send_parts는 단일 message의 이동이며, 예약 실패 catch가 기존 source 복원 경로를 수행한다. Request의 completion 등록·결과 allocation은 기존 registry 계약이며 별도 socket closed 판정을 하지 않는다.

src/Runtime/Messaging/completion_owner.hpp:121의 _socket은 const shared_ptr다. Completion owner 생성 이후 pointer를 reset·대입할 수 없다. Runtime의 completion shared_ptr가 이 owner를 보유하므로 runtime 강한 참조가 살아 있는 동안 handle도 살아 있다. Shutdown은 entry를 정리하지만 handle pointer는 변경하지 않는다(completion_owner.cpp:815). Drain의 completion_owner.cpp:681은 이 owner의 _socket을 빌리며 native receive 종료와 errno capture 이후 :694에서 예약을 해제한다. Public poller 또는 runtime/inline drain 호출자가 completion owner 수명을 보유한다.

src/Runtime/Messaging/reply_operations.cpp:21–29는 runtime 강한 참조를 먼저 확보하고 같은 operation handle에서 예약한다. Runtime 확보 실패와 예약 실패 모두 기존 메시지 복원 catch 안에 있다. runtime->reply_owner는 token owner 판정을 위해 사용하며 새로운 handle 복사는 없다.

tests/contract/test_cpp_contract_submit_close_admission.cpp:338의 추가 검사는 socket 파괴 뒤 runtime weak_ptr가 만료되고 handle weak_ptr는 아직 유효한 것을 확인한다. 다른 thread로 builder를 이동한 뒤 submit이 TERMINATED/ESHUTDOWN을 반환하고 native 진입은 없으며 message가 보존되는지 검사한다. 그 thread에서 builder를 파괴하고 join한 뒤 handle weak_ptr도 만료되는지 확인한다. 따라서 handle 연장은 runtime 순환 소유나 thread 간 dangling reference를 만들지 않는다.

3. 테스트와 로그.

빌드는 /root/jobs/fx/cx/cpp-build-slot.sh cmake --build /root/jobs/fx/cx/v21/s033/cbclose3/build-after -j3으로 수행하였다. Release -O3 -DNDEBUG이며 -g와 LTO는 없다. Test target의 -UNDEBUG를 유지하였다. Core는 cbclose2와 같은 /root/.cache/zlink/core/1.23.0/linux-x64 package다. 최초 configure는 package prefix를 빠뜨려 runtime 탐색에 실패했다(configure.log). Prefix를 명시한 configure-package.log는 성공했다. 첫 build.log의 추가 lifetime test는 runtime 정의 include 누락으로 compile에 실패했고 include 추가 뒤 build-final.log는 100% 성공했다. 코드 동작 실패를 재실행으로 통과 처리한 사례는 없다.

모든 테스트는 /root/jobs/fx/cx/measure-shared.sh python3 cbclose3/run-loaded.py <label> <command>로 수행하였다. 이 스크립트는 cbclose2/run-loaded.py의 출력 경로만 바꾼 것이다. stress-ng --cpu 16 --cpu-method loop --metrics-brief를 실행하고 finally에서 SIGTERM과 wait로 종료한다. MemAvailable은 1초마다 기록하고 12 GiB 미만이면 실행을 중단한다.

Admission command는 build-after/test_cpp_contract_submit_close_admission이다. admission.log, admission-load.log, admission-memory.log, admission-result.json에 exit 0을 보존하였다. 성공/BUSY close가 끝날 때까지 네 submit 경로가 기다리는 검사, 독립 record의 동시 진입, completion receive 예약, invalid argument 뒤 예약 반납, socket 파괴 뒤 builder/Received의 실패와 새 thread 간 handle 수명 검사가 모두 통과하였다. 성공 입력뿐 아니라 closed owner·잘못된 준비 인자에서 native 진입과 메시지 이동이 없는지도 기존 assertion으로 확인한다.

독립 race command는 build-after/test_cpp_submit_close_race all 1000이다. races.log와 races-load.log, races-memory.log, races-result.json을 보존하였다. 각 경로는 submit 4000건이다. Send success/TERMINATED는 1193/2807, request는 1528/2472, reply는 1465/2535, publish는 1737/2263이다. Close OK/BUSY는 각각 818/182, 680/320, 719/281, 746/254다. 예상 밖 결과와 ownership 실패는 모두 0건이고 process exit는 0이다.

Benchmark binary를 만든 뒤 ZLINK_CPP_BUILD_BENCHMARKS=OFF로 재설정하여 cbclose2와 같은 binding CTest 27개를 등록하였다(configure-tests-only.log). 전체 command는 ctest --test-dir /root/jobs/fx/cx/v21/s033/cbclose3/build-after --output-on-failure -j3이다. full-binding.log에서 27/27, 실패·skip 0개, 46.94초, exit 0이다. LastTest.log를 별도로 보존하였다. Suite 내부 race 1000회에서는 send success/TERMINATED 1696/2304, request 2056/1944, reply 1990/2010, publish 1838/2162였다. Close OK/BUSY는 각각 757/243, 640/360, 652/348, 753/247이고 예상 밖 결과와 ownership 실패는 0건이다. 기존 send-close stress는 accepted/received 50000/50000, unexpected·ownership_failures·bad_records가 모두 0이다.

테스트 표본의 최소 MemAvailable은 admission 13.899 GiB, race 16.381 GiB, 전체 binding 23.918 GiB다. memory-summary.json에 표본 수와 최소값을 기록하였다. cbclose2의 main 음성 대조 로그는 다시 빌드하거나 변경하지 않았다.

4. 벤치마크.

cbclose2/bench-pair.py와 bench-all.py를 복사하여 출력 경로와 PR serial 바이너리 경로만 바꾸었다. Main tree는 cbclose2/main-tree, main library는 cbclose2/build-main이며 main을 재빌드하거나 수정하지 않았다. PAIR·PUBSUB은 기존 suite binary, 직렬 request는 cbclose2/serial.cpp의 public API 프로그램이다. Payload 1024 byte, inproc, PERF_PART_COUNT=1, PERF_SINGLE_DURATION_SECONDS=5, taskset -c 8-15다. PAIR·PUBSUB 단위는 msg/s, REQUEST_SERIAL은 request/s다.

홀수 쌍은 main→PR, 짝수 쌍은 PR→main이다. 각 쌍은 flock -o /root/jobs/fx/cx/measure.lock 안에서 두 process를 연속 실행하며 쌍 내부에는 다른 측정이 들어가지 않는다. 각 쌍 실행 전 uptime과 1분 load를 기록하고 load가 4를 넘으면 실행 없이 연기한다. 각 process 직전에 MemAvailable 12 GiB 이상을 검사한다. 이번 build/test는 모두 끝낸 뒤 benchmark를 시작하였다. 다른 작업의 shared/exclusive lock을 기다린 시간은 측정 시간에 포함하지 않는다.

binary-provenance.json과 ldd-main.log·ldd-after.log는 main/PR library·binary와 Core package를 기록한다. Main library, PAIR, PUBSUB, serial hash는 기존 cbclose2/binary-provenance.json과 모두 일치한다. Core SHA-256은 6f496304ef6e1287f57234567231bbd2b76ad2392f99e816001417d01215bf01이다. CMakeCache-after.txt와 cbclose2/CMakeCache-main.txt에서 compiler·Core·Release·LTO OFF 조건이 같다.

PAIR 1회(main→after): main 1645179.000, PR 1970260.000, PR/main 1.197596. 부하 원문: 03:05:30 up 24 days, 18:31,  3 users,  load average: 1.30, 5.03, 5.13. 로그: PAIR-1-attempt1-main.log, PAIR-1-attempt1-after.log. JSON: PAIR-1-attempt1.json.

PAIR 2회(after→main): main 1969315.400, PR 1900169.600, PR/main 0.964888. 부하 원문: 03:07:59 up 24 days, 18:33,  3 users,  load average: 1.01, 3.49, 4.53. 로그: PAIR-2-attempt1-after.log, PAIR-2-attempt1-main.log. JSON: PAIR-2-attempt1.json.

PAIR 3회(main→after): main 1929481.800, PR 1914178.000, PR/main 0.992068. 부하 원문: 03:11:25 up 24 days, 18:37,  3 users,  load average: 2.15, 2.84, 4.05. 로그: PAIR-3-attempt1-main.log, PAIR-3-attempt1-after.log. JSON: PAIR-3-attempt1.json.

PAIR 4회(after→main): main 1894213.400, PR 1898167.000, PR/main 1.002087. 부하 원문: 03:14:50 up 24 days, 18:40,  3 users,  load average: 3.07, 2.89, 3.82. 로그: PAIR-4-attempt1-after.log, PAIR-4-attempt1-main.log. JSON: PAIR-4-attempt1.json.

PAIR 5회(main→after): main 1780489.000, PR 1856386.400, PR/main 1.042627. 부하 원문: 03:18:16 up 24 days, 18:43,  3 users,  load average: 1.25, 2.26, 3.40. 로그: PAIR-5-attempt1-main.log, PAIR-5-attempt1-after.log. JSON: PAIR-5-attempt1.json.

PAIR 6회(after→main): main 1815904.200, PR 1864792.200, PR/main 1.026922. 부하 원문: 03:21:41 up 24 days, 18:47,  3 users,  load average: 2.12, 2.07, 3.08. 로그: PAIR-6-attempt1-after.log, PAIR-6-attempt1-main.log. JSON: PAIR-6-attempt1.json.

PAIR: 6쌍, 중앙값 1.014505, 기하평균 1.035103, 범위 0.964888–1.197596.

PUBSUB 1회(main→after): main 1693972.400, PR 1614220.400, PR/main 0.952920. 부하 원문: 03:05:40 up 24 days, 18:31,  3 users,  load average: 1.40, 4.93, 5.09. 로그: PUBSUB-1-attempt1-main.log, PUBSUB-1-attempt1-after.log. JSON: PUBSUB-1-attempt1.json.

PUBSUB 2회(after→main): main 1697730.600, PR 1619399.000, PR/main 0.953861. 부하 원문: 03:09:06 up 24 days, 18:34,  3 users,  load average: 1.37, 3.09, 4.31. 로그: PUBSUB-2-attempt1-after.log, PUBSUB-2-attempt1-main.log. JSON: PUBSUB-2-attempt1.json.

PUBSUB 3회(main→after): main 1644759.200, PR 1612665.000, PR/main 0.980487. 부하 원문: 03:12:32 up 24 days, 18:38,  3 users,  load average: 2.45, 2.80, 3.95. 로그: PUBSUB-3-attempt1-main.log, PUBSUB-3-attempt1-after.log. JSON: PUBSUB-3-attempt1.json.

PUBSUB 4회(after→main): main 1655236.800, PR 1468818.000, PR/main 0.887376. 부하 원문: 03:15:58 up 24 days, 18:41,  3 users,  load average: 3.33, 2.99, 3.79. 로그: PUBSUB-4-attempt1-after.log, PUBSUB-4-attempt1-main.log. JSON: PUBSUB-4-attempt1.json.

PUBSUB 5회(main→after): main 1622988.800, PR 1558796.400, PR/main 0.960448. 부하 원문: 03:19:23 up 24 days, 18:45,  3 users,  load average: 1.17, 2.04, 3.25. 로그: PUBSUB-5-attempt1-main.log, PUBSUB-5-attempt1-after.log. JSON: PUBSUB-5-attempt1.json.

PUBSUB 6회(after→main): main 1560145.600, PR 1580915.000, PR/main 1.013312. 부하 원문: 03:22:48 up 24 days, 18:48,  3 users,  load average: 2.69, 2.25, 3.08. 로그: PUBSUB-6-attempt1-after.log, PUBSUB-6-attempt1-main.log. JSON: PUBSUB-6-attempt1.json.

PUBSUB: 6쌍, 중앙값 0.957155, 기하평균 0.957307, 범위 0.887376–1.013312.

REQUEST_SERIAL 1회(main→after): main 79260.000, PR 77714.000, PR/main 0.980495. 부하 원문: 03:06:49 up 24 days, 18:32,  3 users,  load average: 1.00, 4.09, 4.79. 로그: REQUEST_SERIAL-1-attempt1-main.log, REQUEST_SERIAL-1-attempt1-after.log. JSON: REQUEST_SERIAL-1-attempt1.json.

REQUEST_SERIAL 2회(after→main): main 76304.300, PR 76362.300, PR/main 1.000760. 부하 원문: 03:10:15 up 24 days, 18:35,  3 users,  load average: 2.33, 3.01, 4.20. 로그: REQUEST_SERIAL-2-attempt1-after.log, REQUEST_SERIAL-2-attempt1-main.log. JSON: REQUEST_SERIAL-2-attempt1.json.

REQUEST_SERIAL 3회(main→after): main 75280.000, PR 78569.600, PR/main 1.043698. 부하 원문: 03:13:41 up 24 days, 18:39,  3 users,  load average: 3.12, 2.89, 3.90. 로그: REQUEST_SERIAL-3-attempt1-main.log, REQUEST_SERIAL-3-attempt1-after.log. JSON: REQUEST_SERIAL-3-attempt1.json.

REQUEST_SERIAL 4회(after→main): main 76991.700, PR 75031.000, PR/main 0.974534. 부하 원문: 03:17:07 up 24 days, 18:42,  3 users,  load average: 1.79, 2.60, 3.59. 로그: REQUEST_SERIAL-4-attempt1-after.log, REQUEST_SERIAL-4-attempt1-main.log. JSON: REQUEST_SERIAL-4-attempt1.json.

REQUEST_SERIAL 5회(main→after): main 80080.900, PR 76007.600, PR/main 0.949135. 부하 원문: 03:20:32 up 24 days, 18:46,  3 users,  load average: 1.88, 2.02, 3.14. 로그: REQUEST_SERIAL-5-attempt1-main.log, REQUEST_SERIAL-5-attempt1-after.log. JSON: REQUEST_SERIAL-5-attempt1.json.

REQUEST_SERIAL 6회(after→main): main 77706.800, PR 78403.700, PR/main 1.008968. 부하 원문: 03:23:00 up 24 days, 18:48,  3 users,  load average: 2.45, 2.22, 3.05. 로그: REQUEST_SERIAL-6-attempt1-after.log, REQUEST_SERIAL-6-attempt1-main.log. JSON: REQUEST_SERIAL-6-attempt1.json.

REQUEST_SERIAL: 6쌍, 중앙값 0.990627, 기하평균 0.992488, 범위 0.949135–1.043698.

PAIR 중앙값 1.014505은 목표 0.98을 충족하였다. 따라서 조건부 perf stat/record 단계는 실행하지 않았다.

유효한 18쌍의 실행 전 1분 load는 0.996094–3.329102, process 직전 최소 MemAvailable은 25.811 GiB다. 모든 실행의 exit는 0이다. 개별 수치를 제외하거나 쌍을 다시 골라 측정하지 않았다.


5. 파일별 변경량과 형식.

다음 집계는 기존 cbclose/cbclose2 변경을 포함한 bindings/cpp 전체 diff다. 신규 untracked test 두 개도 포함한다. 각 줄은 추가/삭제한 줄 수다.

bindings/cpp/CMakeLists.txt: +16/-0.

bindings/cpp/include/zlink/Contracts/Messaging/received.hpp: +0/-1.

bindings/cpp/include/zlink/Contracts/Sockets/socket_contracts.hpp: +3/-1.

bindings/cpp/src/Runtime/Messaging/completion_owner.cpp: +40/-16.

bindings/cpp/src/Runtime/Messaging/completion_owner.hpp: +5/-2.

bindings/cpp/src/Runtime/Messaging/operation_state.hpp: +10/-13.

bindings/cpp/src/Runtime/Messaging/operation_submit.hpp: +20/-16.

bindings/cpp/src/Runtime/Messaging/received.cpp: +0/-2.

bindings/cpp/src/Runtime/Messaging/received_access.hpp: +3/-12.

bindings/cpp/src/Runtime/Messaging/reply_operations.cpp: +12/-5.

bindings/cpp/src/Runtime/Messaging/request_reply.cpp: +0/-4.

bindings/cpp/src/Runtime/Messaging/send_operations.cpp: +6/-3.

bindings/cpp/src/Runtime/Native/native_send.hpp: +0/-56.

bindings/cpp/src/Runtime/Native/socket_handle.hpp: +84/-50.

bindings/cpp/src/Runtime/Sockets/dealer.cpp: +0/-2.

bindings/cpp/src/Runtime/Sockets/pair.cpp: +0/-1.

bindings/cpp/src/Runtime/Sockets/pubsub.cpp: +0/-5.

bindings/cpp/src/Runtime/Sockets/router.cpp: +1/-5.

bindings/cpp/src/Runtime/Sockets/socket.cpp: +31/-23.

bindings/cpp/src/Runtime/Sockets/socket_runtime_state.hpp: +5/-2.

bindings/cpp/src/Runtime/Sockets/stream.cpp: +0/-1.

bindings/cpp/tests/contract/test_cpp_contract_submit_close_admission.cpp: +496/-0.

bindings/cpp/tests/stress/test_cpp_submit_close_race.cpp: +157/-0.

git diff origin/main --stat -- bindings/cpp와 git diff -w origin/main --stat -- bindings/cpp는 byte 단위로 같다(stat.txt, stat-w.txt). git diff --check는 성공했다(diff-check.log). 원래 줄에 formatter를 적용하지 않았다. change.diff는 tracked 변경과 신규 test 두 개를 모두 포함한다. source-sha256.json은 최종 소스 hash다. Request benchmark용 serial.cpp는 cbclose2 파일을 그대로 사용해 이번 library에만 다시 링크하였다. Repo perf source는 변경하지 않았다.

6. 리팩토링 점검과 소유 계층.

Native 예약을 수락할지 결정하는 위치 수는 수정 전 1곳 → 수정 후 1곳이다. Native/socket_handle.hpp:58의 reserve가 closed/closing/in-flight를 한 atomic state에서 판정한다. Native/socket_handle.hpp:24의 require_submit_open은 실패 결과를 TERMINATED/ESHUTDOWN으로 만드는 공통 predicate다. Runtime link는 소유권 연결이며 별도 lifecycle 상태가 아니다. Per-operation weak runtime field 하나를 없애고 per-socket weak link 하나로 옮겼으며, strong handle 보유는 기존 weak 저장·반납을 대체한다.

남아 있는 검사와 소유권 전달은 다음과 같다. Messaging/operation_state.hpp:123에서 socket pointer 존재를 확인하고 runtime을 잠그며 :124에서 runtime 소멸을 확인한다. Async/send와 request 첫 native 시도에서 operation_submit.hpp:41·150은 같은 strong socket pointer의 존재를 한 번 더 확인하지만 atomic closed load나 runtime lock은 추가하지 않는다. Native/socket_handle.hpp:118의 submit_handle은 moved-from/소유자 소멸의 null 처리만 맡는다. 이 pointer 검사는 native 수락 결정을 하지 않는다.

Messaging/completion_owner.cpp:593·604·614의 _shutdown 검사는 registry 등록, :678의 검사는 drain 종료, :700의 검사는 보유 retry 처리, :775는 public poller 이전, :818은 shutdown 중복 호출을 처리한다. 이들은 기존 completion registry 경계이며 socket closed 사실을 다시 만들지 않는다. :800의 close_in_progress와 :807의 mutex try-lock은 재진입 close 및 기존 completion 등록의 순서를 처리한다. Native close와 예약의 실제 순서는 socket_handle.hpp:82와 :71의 동일 atomic에서만 정한다. 기존 _inline_drain_mutex와 _public_owner를 유지했고 submit record를 직렬화하는 mutex는 추가하지 않았다.

제거한 중복은 submit마다 runtime을 다시 잠그는 share_socket_handle, reservation의 shared handle 복사, accepting_submits/valid의 사전 closed 판정이다. 사용하지 않게 된 accepting_submits와 share_socket_handle을 제거하였다. Sync socket 경로의 shared handle 전달도 borrow로 바꿨다. 기존 cbclose2가 제거한 raw handle cache와 미사용 native_send adapter를 복구하지 않았다. Refactoring 점검에서 발견한 항목 3개(runtime 중복 lock, handle 소유권 복사 전달, closed 선행 판정)를 모두 고쳤고, 새로 남긴 중복 lifecycle 판정과 넘긴 항목은 각각 0개다.

소유 계층은 C++ binding socket_handle의 native 호출 수명 예약이다. 실제 API 수락과 BUSY는 Core가 소유한다.

Spec 근거는 승인된 cbclose2 보고서와 같은 bindings/doc/spec/README.ko.md:498·1851 및 core/doc/spec/core/socket/README.ko.md:58·647·657이다. 이번 작업은 이 계약을 바꾸지 않았다.

교차언어 대조는 cbclose2에서 승인된 Java #4073의 ownerLock 수명 예약 근거를 유지한다. C++ 내부 shared_ptr 보유 방식을 바꾼 것으로 다른 언어 동작은 바꾸지 않았다.

변경 분류는 B다. 승인된 lock-free 예약의 정확성 계약을 유지하면서 per-submit 소유권 비용만 줄였다.

남은 binding test 실패는 0개다. PAIR 목표는 충족하였다. 같은 세션에서 PUBSUB 기하평균은 main 대비 4.269% 낮고 직렬 request는 0.751% 낮았다. PUBSUB 여섯 쌍 중 다섯 쌍에서 PR이 낮았으며 이 차이는 해결되었다고 보고하지 않는다. PUBSUB의 별도 합격 기준은 요청에 없었고, perf 진단 조건인 PAIR 목표 미달은 발생하지 않았다. 이 측정만으로 PUBSUB 차이의 원인을 정량적으로 확정하지 않았다.

최종 산출물은 report.md, 신규 파일을 포함한 change.diff, benchmark-summary.txt·benchmark-results.json과 모든 raw log·JSON·assembly 출력이다. 테스트 부하 process와 benchmark process는 모두 종료되었다. cbclose3 크기는 0.03 GiB 미만이며 main-tree/build-main과 기존 사용자 변경은 정리하거나 삭제하지 않았다. source-verification.json의 모든 소스 hash와 format-verification.json의 stat 비교가 성공하였다.
