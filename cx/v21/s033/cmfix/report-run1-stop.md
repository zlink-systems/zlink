(0) 원자성과 소유권 검토 결과

0단계의 중단 조건에 따라 구현하지 않았다. send_socket_parts를 통한 기존 logical multipart 경로는 실패한 record의 pipe charge를 롤백하지만, 직접 전달받은 입력 part의 소유권을 caller에게 유지하지 않는다. 내부 admission 함수와 공개 API의 소유권 경계는 서로 다르므로 아래에서 구분한다. 이 보고서의 상대 소스 경로는 /root/worktree/zlink-4118-multipart 기준이다. 코드 발췌는 raw/atomicity-evidence.log에 보존했다.

API 결정 지점에서 send_socket_parts로 넘기면 routed multipart는 core/src/api/socket/socket_message_send_api.cpp:188의 logical_multipart_send_routed로, unrouted multipart는 같은 파일:150의 logical_multipart_send로 진입한다. 이 경로는 core/src/runtime/core/multipart_send_txn.cpp:315,392에서 입력 배열을 그대로 전달한다. send_frames_once는 같은 파일:85에서 원본 part를 직접 send_scoped에 넘기고, 실패하면 :93-95에서 rollback 후 실패한 part부터 나머지 part를 consume_frames_from으로 소비한다. consume_frames_from은 :49-59에서 close와 init을 수행한다. ROUTER의 route prefix 제출 자체가 실패해도 :175-179에서 원본 part 전체를 소비한다. 따라서 첫 part 거부와 중간 part 거부 모두 caller의 모든 원본 part를 보존한다는 전제는 성립하지 않는다.

Pipe charge와 수신 원자성은 별도로 처리한다. core/src/runtime/core/multipart_send_txn.cpp:94의 rollback_scoped는 core/src/runtime/sockets/common/socket_base_msg.cpp:640-641에서 scope의 sync를 재취득하고 xrollback을 호출한다. ROUTER는 core/src/runtime/sockets/router/router.cpp:515-519에서 현재 route 상태를 비우고 pipe를 롤백한다. Continuation write가 실패한 경우에는 core/src/runtime/sockets/router/router_send_path.cpp:192에서 해당 pipe를 먼저 롤백한다. core/src/runtime/core/pipe_write.cpp:1385-1394는 미완성 frame을 unwrite하고 provisional charge를 반환하며, :1395-1403은 누적 byte와 multipart 상태를 초기화하고 회계를 publish한다. ROUTER의 final flush는 core/src/runtime/core/pipe_write.cpp:69-72의 성공한 final write 뒤에만 수행한다. 이 코드상 rollback 근거를 실행 검증 결과로 대신하지 않았다.

다른 내부 generic 함수인 try_admit_send_parts_scoped는 소유권을 보존하는 경로다. core/src/runtime/sockets/common/socket_send_complete.cpp:332-359는 multipart part를 얕게 복사하여 시도한다. ROUTER는 final도 복사하며, PAIR final은 실패 시 원본을 건드리지 않는 경로를 직접 사용한다(:339-349, core/src/runtime/sockets/pair/pair.cpp:92-110). 실패하면 socket_send_complete.cpp:415-434에서 시도용 복사본을 close하고 미완성 record를 rollback한다. 이 내부 함수의 거부와 logical_multipart_send[_routed]의 입력 소비를 같은 동작으로 취급할 수 없다.

공개 C API는 거부 시에도 모든 입력 슬롯을 소비하도록 이미 규정되어 있다. core/doc/spec/core/socket/README.ko.md:1091-1093,1127-1128이 그 계약이며, core/src/api/socket/part_helper_internal.hpp:123-137이 모든 반환 결과 뒤 consume_send_part를 실행한다. 그러므로 공개 API caller의 원본 슬롯 보존을 새 요구로 적용하면 기존 계약과 충돌한다. 별도로 보관한 재시도 record의 소유권과 이번 호출에 넘긴 입력 슬롯의 소유권을 구분해야 한다. 스펙과 소유권 동작은 수정하지 않았다.

현재 소스의 호출 경로도 진단 보고서와 다르다. core/src/runtime/sockets/common/socket_send_submit.cpp:288-290은 이미 try_admit_send_parts_scoped를 호출한다. 이 함수의 complete-record fast path는 socket_send_complete.cpp:275-280에서 PAIR에만 허용되고, :312가 xtry_send_complete_record를 호출한다. pipe_t::try_write_complete_record_and_flush의 유일한 직접 호출자는 core/src/runtime/sockets/pair/pair.cpp:128이다. ROUTER는 socket_send_complete.cpp:394-411의 frame별 generic 경로로 진입한다. 호출자 검색 결과는 raw/complete-record-callers.log에 있다. 따라서 현재 ROUTER 경로에 PAIR의 fast-path refusal handoff를 추가하는 것만으로 해결된다고 판단할 수 없다.

Generic 경로에서는 MORE frame에도 incremental HWM 검사를 수행한다. ROUTER 첫 part는 core/src/runtime/sockets/router/router_send_path.cpp:489의 pipe_t::write를 거쳐 core/src/runtime/core/pipe_write.cpp:462의 write_message_unlocked(msg_, true, true, ...)로 들어간다. 같은 파일:1231-1234는 빈 queue 예외를 !more인 final에만 허용한다. 이 때문에 제시된 HWM 16 multipart 거부를 complete-record fast path만의 문제로 단정할 수 없다. core/doc/spec/core/systems/06-auto-hwm.ko.md:464-470의 incremental multipart와 크기를 이미 아는 complete record 계약을 구분하여 다음 변경 범위를 결정해야 한다.

(1) 변경과 판정 위치

제품 코드와 테스트 파일은 변경하지 않았다. branch는 git status의 fix/4118-multipart-resume으로 확인했다. 기존 untracked cmpart/는 그대로 두었다. Commit, staging, branch 변경, GitHub 작업과 스펙 수정은 수행하지 않았다. change.diff는 git diff의 원본 출력이며 크기는 0 byte다.

빈 queue 예외 허용 규칙의 소유 위치 수는 수정 전후가 같다. Complete-record fast path는 0→0곳이며 pipe_write.cpp:888-889에서 false를 전달하는 제외 지점은 1→1곳이다. Generic frame 경로는 1→1곳이며 동일한 final 자격 조건을 최초 검사(:1231-1234)와 credit 재검사(:1245-1249)에 쓰므로 호출 지점은 2→2곳이다. Single-frame 경로는 1→1곳이며 true를 전달하는 최초 검사(:977-978)와 credit 재검사(:983-984)의 호출 지점은 2→2곳이다. 이 수는 변경에 따른 개선을 뜻하지 않으며, 구현을 중단한 현재 상태를 나타낸다.

리팩토링 점검에서는 fast와 generic 경로 사이의 admission 검사 중복 4종을 확인했다. Pipe 준비 상태는 pipe_write.cpp:855와 :457-459, record charge 누적 및 overflow는 :861-881과 :1192-1203, MAXMSGSIZE는 :887과 :1207-1215, HWM은 :888-889와 :1231-1249에 있다. Fast path는 complete record를 미리 검사하고 generic 경로는 frame별로 검사한다. 중단 조건에 따라 제거한 항목은 0개이며 이 중복을 별도 결함으로 판정하지 않았다. 새 flag, counter, queue와 lock은 추가하지 않았다.

(2) 테스트와 로그

이번 작업에서는 0단계의 정적 검토만 수행했다. Core C API contract test, pipe unit test, C++ binding contract test를 추가하지 않았으며 빌드와 Core/C++ 전체 ctest를 실행하지 않았다. 따라서 HWM 16/2048, 단일 frame control, MAXMSGSIZE, non-empty queue와 거부 record의 부분 전달 방지에 대해 새로운 통과 결과를 주장하지 않는다. 빌드 디렉터리도 생성하지 않았다.

입력 진단의 기존 C API 행렬 로그는 raw/prior-core-api-matrix.log에 복사했다. HWM 16의 세 multipart는 WRITABLE 뒤 재제출해도 BACKPRESSURED였고 수신 0건이며, 단일 frame과 HWM 2048 control은 성공했다. 이는 이전 진단의 실행 결과이며 이번 세션의 재현 결과가 아니다. 현재 소스의 원자성 근거와 호출자 목록은 raw/atomicity-evidence.log, raw/complete-record-callers.log에 있다. git diff --check는 통과했고 출력은 raw/diff-check.log, 변경 상태는 raw/git-status.log에 있다.

(3) 성능 보호

제품 diff가 없으므로 admitted 경로와 refusal 경로 모두 변경되지 않았다. PAIR/ROUTER throughput 비교 측정은 실행하지 않았다. 구현과 성능 검증은 완료되지 않았으며, 입력 소유권 경계와 현재 ROUTER generic 경로를 반영한 변경 범위가 정해진 뒤 진행해야 한다.
