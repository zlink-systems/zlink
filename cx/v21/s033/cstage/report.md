| 항목 | (a) main binding request | (b) staged request |
|---|---:|---:|
| caller µs/request, 5회 중앙값 (최소–최대) | 4.377 (4.236–4.417) | 6.328 (6.198–6.458) |
| 처리율 req/s, 5회 중앙값 (최소–최대) | 228,294.945 (226,194.946–235,946.776) | 140,797.681 (136,889.658–143,708.245) |
| 동일 속도 실행의 실제 처리율 req/s, 60초 | 96,547.813 | 96,490.928 |
| source 전체 CPU µs/request, 60초 user+kernel | 27.254 | 33.139 |
| 스레드 분해 구간 길이 s | 58.000 | 58.000 |
| 스레드 분해 구간의 실제 처리율 req/s | 96,544.865 | 96,449.217 |
| source 전체 CPU µs/request, 분해 구간 | 27.256 | 33.141 |
| caller CPU µs/request, 분해 구간 (user+kernel) | 9.940 (7.201+2.739) | 9.769 (6.698+3.071) |
| body worker CPU µs/request, 분해 구간 (user+kernel) | 0.000 (0.000+0.000) | 7.170 (3.450+3.720) |
| Core I/O 1 (a: ZLINKbg/IO/0/961776, b: ZLINKbg/IO/0/937639) | 0.002 (0.000+0.002) | 0.000 (0.000+0.000) |
| Core I/O 2 (a: ZLINKbg/IO/0/961795, b: ZLINKbg/IO/0/937658) | 9.506 (1.655+7.851) | 9.308 (1.661+7.647) |
| Core I/O 3 (a: ZLINKbg/IO/1/961777, b: ZLINKbg/IO/1/937640) | 0.000 (0.000+0.000) | 0.000 (0.000+0.000) |
| Core I/O 4 (a: ZLINKbg/IO/1/961796, b: ZLINKbg/IO/1/937659) | 0.000 (0.000+0.000) | 0.000 (0.000+0.000) |
| Core I/O 5 (a: ZLINKbg/IO/2/961778, b: ZLINKbg/IO/2/937641) | 0.000 (0.000+0.000) | 0.000 (0.000+0.000) |
| Core I/O 6 (a: ZLINKbg/IO/2/961797, b: ZLINKbg/IO/2/937660) | 7.726 (1.445+6.281) | 6.811 (1.634+5.177) |
| Core I/O 7 (a: ZLINKbg/IO/3/961779, b: ZLINKbg/IO/3/937642) | 0.000 (0.000+0.000) | 0.000 (0.000+0.000) |
| Core I/O 8 (a: ZLINKbg/IO/3/961798, b: ZLINKbg/IO/3/937661) | 0.000 (0.000+0.000) | 0.002 (0.000+0.002) |
| 그 외 모든 스레드 CPU µs/request, 분해 구간 (user+kernel) | 0.073 (0.018+0.055) | 0.073 (0.016+0.057) |

Core에서는 가능했다. 아래 변경 파일의 file:line은 prototype-source 보존 사본 기준이며, 변경하지 않은 파일만 원본 경로 기준이다. 기존 pending request 하나(`core/src/api/socket/socket_request_reply_internal.hpp:133`)가 가진 correlation lease와 completion reservation을 유지하고, 기존 outgoing FIFO에 예약한 칸을 나중에 실제 message로 바꿨다. 새 staging queue와 admission budget counter는 추가하지 않았다. body 준비 상태를 나타내는 값은 추가했으므로, 새 flag가 전혀 없는 구현이라고 보고할 수는 없다.

caller 중앙값은 (a) 4.377, (b) 6.328 µs/request였다. (b)는 5.831 µs 목표를 충족하지 못했다. (b) ≤ (a) 조건도 충족하지 못했다. 차이는 1.951 µs/request다. 동일 속도 60초의 source 전체 CPU는 (b)−(a)=5.884 µs/request이며, (b) ≤ (a) 조건을 충족하지 못했다. 이 판정은 이번 측정값에 대한 판정이며, 최적화된 최종 제품의 한계를 뜻하지 않는다.

caller 값은 각 results.json의 submit_thread_cores × 1e6 / throughput_per_second를 계산한 뒤 5회의 중앙값과 범위를 구했다. condition B로 source는 CPU 0–7, target은 8–15에 두었다. 각 caller 실행은 warmup 30초와 active 5초였고, main과 staged를 번갈아 실행했다. 이번 요청의 a/b를 측정했고, 이전 보고서의 다른 측정군은 재실행하지 않았다. body 생성은 동일한 make_parts 함수다. application body는 1,024 byte, protobuf part는 1,027 byte, header part는 201 byte이며 함수 해시 일치 기록은 body-builder-sha.json에 있다.

동일 속도 CPU 비교는 staged의 60초 실측 처리율 96,490.928 req/s에 main을 맞췄다. 최종 pacing 입력은 main 98,120, staged 105,000 req/s이며, 두 실행 모두 warmup 30초와 active 60초다. 실제 60초 처리율 차이는 0.059%이며 중앙 분해 구간과 전체 구간 모두 0.5% 이내라는 조건을 확인했다. 최초에는 작은 비제한 처리율 중앙값의 75%를 1,000 단위로 내린 105,000을 둘 다 입력했으나, 실제 처리율이 main 102,436과 staged 96,491로 5.84% 달라 그 main 실행을 CPU 비교에서 제외했다. 별도 5초 rate calibration 뒤 main만 60초로 다시 실행했고, 원래 staged 60초 결과와 비교했다. 제외한 실행도 cpu-initial-mismatch와 summary-initial-rate-mismatch.json에 보존했다. pacing의 next_submission=max(next_submission+interval, now)는 누적 지연을 보존하지 않으므로 같은 입력이 같은 실제 처리율을 보장하지 않는다(`framework/bench/grpc/cpp/client/bench_cpp_client.cpp:744`). cbind 방식의 pacing은 1 ms 미만 예약 간격에서 poller.wait(0)로 completion을 진행하므로 caller의 busy spin도 CPU에 포함된다. 두 경로에 같은 pacing 알고리즘을 적용했다. 전체 CPU 판정은 getrusage(RUSAGE_SELF)의 user+kernel 합을 쓰는 60초 results.json 값이다(`framework/bench/grpc/cpp/common/bench_common.hpp:256`). 스레드 분해는 active 구간 안의 약 58초 동안 /proc의 user+kernel tick 차이를 같은 구간의 completed 차이로 나눴다. 표의 괄호는 user+kernel이며, 서로 다른 구간을 보간해 맞추지 않았다. 분해 구간에서 프로세스 합과 스레드 합의 차이는 각각 0.009, 0.007 µs/request다. 모든 스레드의 TID·이름·시간은 summary.json과 cpu-before.json·cpu-after.json에 남겼다.

분해 구간에서 caller CPU는 0.171 µs/request 감소했고, worker는 7.170 µs/request 추가됐다. Core I/O 합은 1.113 µs/request 감소했다. worker 값은 body 생성과 Core publish·조회·동기화까지 포함한 스레드 전체 CPU다. source 전체 CPU가 증가했으므로 caller의 작업을 이동한 것만으로 전체 CPU 조건을 충족하지 못했다.

기존 completion slot은 `core/src/api/socket/socket_request_reply_pending_api.cpp:321`에서 먼저 예약하며, 구조는 `core/src/api/socket/socket_completion_queue_internal.hpp:36`에 있다. 기존 pending request의 correlation과 pull_completion은 `core/src/api/socket/socket_request_reply_internal.hpp:133`에 있다. 이들은 원래 서로 다른 Core 구조였고, 프로토타입은 pending request에 같은 queue 칸의 주소를 저장했다. 새로운 completion record를 별도로 만들거나 body publish 때 completion을 재예약하지 않았다.

correlation admission의 byte·work·count는 `core/src/runtime/core/pipe_receive.cpp:607`가 소유한다. HWM admission은 `core/src/runtime/core/pipe_write.cpp:1190`가 기존 _bytes_written·peer read credit으로 처리한다. 실제 상태 필드는 `core/src/runtime/core/pipe.hpp:821`와 `core/src/runtime/core/pipe.hpp:834`에 있다. 예약 API(`core/src/api/socket/socket_request_reply_submit_api.cpp:677`)는 bound를 크기로 가진 cmsg를 기존 zlink_request DONTWAIT 경로에 넣는다. cmsg의 실제 body는 읽지 않으며, 기존 HWM·correlation·completion admission을 그대로 거친다.

같은 FIFO 칸을 예약하는 위치는 `core/src/runtime/core/pipe_write.cpp:1124`와 `core/src/runtime/core/ypipe.hpp:64`다. publish는 `core/src/runtime/core/pipe_write.cpp:1500`에서 그 칸의 message를 교체한다. 기존 _w publication 위치를 chunk cursor로 바꿨고, `core/src/runtime/core/ypipe.hpp:105`가 첫 body 대기 칸에서 멈춘다. 별도 FIFO·head·tail·ready index는 없다. 먼저 완성된 뒤 요청도 앞 예약을 넘어 reader에 보이지 않는다.

실제 body가 bound보다 크면 publish는 EMSGSIZE를 반환한다. 작으면 기존 outgoing ledger에서 차액을 차감하고, `core/src/api/socket/socket_request_reply_internal.cpp:169`가 기존 correlation byte·work charge를 실제 크기로 줄인다. correlation count는 reply 또는 failure까지 유지한다. body 생성 실패는 같은 pending request를 제거하고 같은 completion에 INTERNAL_ERROR를 발행한다(`core/src/api/socket/socket_request_reply_submit_api.cpp:707`). payload 예약분과 correlation은 반환하고, queue에 남은 취소 칸의 고정 비용은 reader가 소비할 때 기존 account_inbound_frame으로 반환한다. 취소 marker를 남기면서 저장 비용까지 즉시 반환하는 경로는 최종 구현에 없다.

worker는 `core/src/api/socket/socket_request_reply_submit_api.cpp:766`에서 기존 pending request live list를 읽고, binding의 기존 completion entry에서 같은 operation의 소유권을 이동한다(`bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:598`). 새 작업 queue·미처리 개수 counter·claimed flag는 없다. 기존 binding에는 executor가 없어 body worker 한 개를 만들었다. 기존 completion owner의 mutex·condition variable·shutdown 상태를 재사용했으며 새 poller는 만들지 않았다.

기존 operation state pool은 thread_local이다(`bindings/cpp/src/Runtime/Messaging/operation_state.hpp:327`). main은 admission 뒤 caller에서 반환하고 staged는 body 처리 뒤 worker에서 반환한다. 따라서 staged operation이 caller의 pool로 직접 반환되지는 않는다. 이번 caller·전체 CPU 수치에는 이 재사용 경계의 차이도 포함했으며, 비용을 빼거나 별도 반환 queue로 보정하지 않았다. Core admission의 단독 최저 비용을 측정한 값으로 해석하면 안 된다.

추가 상태는 정확히 다음과 같다. queue 칸마다 ready·body pending·cancelled를 구분하는 unsigned char state를 추가했다(정렬 padding은 별도다). 기존 more flag는 multipart 경계이므로 이 상태를 대신하지 못한다. 기존 pending request에는 최대 네 queue 칸의 pointer와 part 수를 저장했고, part 수 0은 publish 뒤 pointer를 더 이상 사용하지 않는다는 뜻이다. binding의 기존 operation에는 bounds·body callable·기존 sequence/cookie handle을 저장했다. bounds는 admission 뒤 비워서 활성 credit의 소유자는 Core 하나로 유지했다. TLS capture의 part 수는 호출 중 array 길이이며 admission counter가 아니다. binding message.parts는 worker가 그대로 채운다. _w는 기존 raw pointer를 cursor로 교체했으며 두 번째 publication 위치를 추가하지 않았다. 기존 Core queue·credit counter·completion reservation을 삭제한 것이 아니라 그대로 재사용한 것이다.

리팩토링 점검에서 수신 physical registry 반환 위치가 기존 read_internal의 세 분기에 중복되어 있음을 확인하고 account_inbound_frame 하나로 모았다(`core/src/runtime/core/pipe_receive.cpp:713`). 해당 갱신 규칙의 위치 수는 수정 전 3, 수정 후 1이다. 기존 HWM admission·correlation admission·FIFO publication의 소유 함수 수는 각각 1→1이다. 중복 반환 위치 1건을 수정했고 넘긴 항목은 0건이다. 추가 body 상태와 pointer metadata는 위에 모두 적었으며, 새 lock·budget·queue는 없다. bench의 timed ready queue는 두 경로의 동일 속도 pacing 계측에만 사용하고 binding/Core의 staging 작업을 보관하지 않는다.

기능 검사는 inproc ROUTER 쌍에서 실행했고 성능 측정은 TCP다. HWM 1 KiB 검사에서는 bound [16, 1024]를 예약하고 실제 part는 순번 문자열과 7 byte payload로 만들었다. 그중 1건이 즉시 수락됐고 31건이 BACKPRESSURED 뒤 기존 WRITABLE token으로 재제출됐다. 32건의 admitted·reply가 모두 성공했으며 body build 순서와 수신 순서가 32/32 일치했다. 별도 native 검사는 A→B로 예약하고 publish API를 B→A로 호출했다. B만 완성됐을 때 수신은 없었고, A를 완성한 뒤 reader에 보인 순서는 A→B였다. 예약 때의 completion ID·context 재사용도 2/2 확인했다. 4,160 byte 예약을 실제 70 byte로 줄였을 때 outgoing queue 차액 4,090 byte 반환도 확인했다.

body factory가 admission 뒤 의도적으로 예외를 던진 검사에서는 admitted는 성공을 유지하고 reply는 request_error로 끝났다. wire에 body가 나타나지 않았으며 다음 예약·reply가 성공했다. bound 4 byte에 9 byte body를 publish한 입력은 EMSGSIZE로 거부됐고 같은 completion의 INTERNAL_ERROR를 확인했다. body가 50 ms deadline을 넘긴 검사에서는 reply가 ETIMEDOUT으로 끝났고, 늦게 완성된 body는 보내지 않았으며 다음 요청이 성공했다. 다음 요청도 기존 admitted/WRITABLE 계약으로 처리했다.

취소 저장 비용 검사는 source에 기존 public completion poller 하나를 연결하고 I/O thread를 0개로 두어 target queue를 소비하지 못하게 했다. HWM은 512 byte다. 동일한 검사 실행 파일에서 수정 전 Core는 예약·실패 100회를 모두 허용했고 BACKPRESSURED가 없었다(exit 2, 의도한 불량 결과). 최종 Core는 3회 뒤 BACKPRESSURED를 반환했다. target이 취소 칸을 소비한 뒤 정확한 WRITABLE token을 받고 다시 예약할 수 있었다(exit 0). 테스트와 Core 두 버전의 해시는 prototype-test-status.json에 있다. default 기능 검사도 exit 0이다.

각 측정에서 submitted=completed=target.received, errors=abandoned=header_validation_failures=0, contaminated=false, drain_bound_hit=false를 확인했다. 오류·건수 불일치·혼입·drain 초과를 일부러 주입한 네 입력도 결과 검사에서 모두 거부했다. 실제 로드한 Core 경로·해시·CPU affinity와 staged body worker 존재를 meta.json에 남겼다. target 실행 파일과 target Core는 두 경로 모두 이번 main 빌드로 고정했다. 모든 실측은 flock -o measure.lock의 배타 잠금 안에서 실행했다. 관측된 MemAvailable 최솟값은 16.878 GiB였다.

Core와 C++ binding은 로컬 소스로 빌드했고 bench는 cpp/build.sh로 빌드했다. C++ 컴파일은 cpp-build-slot.sh, 기능 검사는 measure-shared.sh를 거쳤다. 원본 local-package script는 git rev-parse와 release Core 선택을 요구하므로 실행용 사본에서 repository root를 고정하고 로컬 Core prefix를 사용했다. 원본 package script는 수정하지 않았다. C++ package 1.23.0/Core 1.23.0 조합은 통과했고 Core 1.22.0은 거부됐다. 초기 복사 timestamp 때문에 남은 C++ 객체가 섞인 문제는 binding build 디렉터리를 지우고 전체 재컴파일해 제거했다. 두 경로의 Core는 Release·LTO off, bench는 -O3 -DNDEBUG -g -fno-omit-frame-pointer로 동일하게 빌드했다. 기존 5.831 µs 환경의 바이너리를 재사용한 비교가 아니라 이번 main과 prototype의 같은 설정 비교다.

기능 검사의 초기 multipart first_part 호출 오류와 I/O thread 0개에 completion owner를 두지 않은 설정 오류는 수정했다. HWM이나 timeout을 늘려 최종 검사를 통과시키지 않았다. 초기 GDB batch 조사 1회는 종료 신호를 명시하지 않아 SIGTERM/SIGINT만 사용했다는 조건 준수를 보증하지 못한다. 초기 fixture 실패에서는 SIGABRT(exit 134)로 끝난 실행도 있었다. 이후 GDB 조사는 명시 SIGTERM으로 종료했다. 측정 runner는 종료 시 SIGTERM, 필요 시 SIGINT만 사용한다.

이 결과는 C++ measurement-only prototype 범위다. 최대 네 part와 non-conflate queue만 지원한다. 임의의 builder가 끝나지 않을 때 shutdown이 기다리는 문제, handover·disconnect·복수 caller의 전체 lifecycle은 검증하지 않았다. 다른 언어·제품 spec은 바꾸지 않았고 제품 API 승인이나 배포를 주장하지 않는다.

변경 파일은 총 27개이며 전체 diff는 prototype.diff, 원복 전 소스와 줄 번호는 prototype-source에 보존했다. 위 줄 번호 중 변경하지 않은 completion queue·bench_common 파일은 원본 경로 기준이다.

- core/src/api/socket/의 staged_request_prototype.hpp, socket_request_reply_submit_api.cpp, socket_request_reply_internal.hpp, socket_request_reply_internal.cpp, socket_request_reply_pending_api.cpp, socket_request_reply_dispatch.cpp에서 private staged 진입점과 기존 pending/completion 수명을 연결했다.
- core/src/runtime/core/의 pipe.hpp, pipe_write.cpp, pipe_receive.cpp, pipe_transport.cpp, ypipe.hpp, ypipe_base.hpp, ypipe_conflate.hpp, yqueue.hpp에서 같은 FIFO 칸의 지연 publication과 기존 credit 반환을 구현했다.
- core/src/runtime/sockets/common/의 socket_base.hpp, socket_base_msg.cpp에서 기존 lifecycle turn으로 취소 칸을 정리한다.
- core/src/libzlink.vers에서 private prototype 심벌 다섯 개를 내보냈다. core/include는 열거나 수정하지 않았다.
- bindings/cpp/include/zlink/Contracts/Messaging/operation_contracts.hpp에 측정용 deferred builder를 넣었다.
- bindings/cpp/src/Runtime/Messaging/의 completion_owner.cpp, completion_owner.hpp, operation_state.hpp, operation_submit.hpp, request_reply.cpp에서 기존 request operation과 completion entry를 worker까지 유지했다.
- bindings/cpp/src/Runtime/Sockets/socket.cpp에서 native close 전에 body worker를 정리했다.
- bindings/cpp/tests/prototype/staged_request.cpp에 이번 전용 검사를 넣었다.
- framework/bench/grpc/cpp/client/bench_cpp_client.cpp와 common/bench_async.hpp에 body 위임·동일 속도 pacing·thread 이름 계측을 넣었다.

커밋·push·GitHub 작업은 하지 않았다. 측정 후 위 프로토타입 소스 변경은 작업 사본에서 원복하고 새 파일 두 개도 제거했다. 기존 네 언어 run_local.sh의 CPU pin 변경은 보존했다. worktree의 빌드 출력은 정리했고, 실제 실행 파일·package·raw 로그는 job 디렉터리에 측정 증거로 보존했다. 최종 상태는 final-git-status.txt에 있다.
