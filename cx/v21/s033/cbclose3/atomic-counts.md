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
