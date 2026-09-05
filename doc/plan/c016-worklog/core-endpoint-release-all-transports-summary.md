# Core D-098 — 전체 transport의 endpoint 해제 완료 경계

D-098 항목 1·2를 Core에서 수정했다. `zlink_close`와 `zlink_unbind`는 TCP·IPC·WS·TLS·WSS listener의 fd 해제가 완료된 뒤 반환한다. inproc unbind는 lane 수와 무관하게 peer command 처리를 구동한다. 판정은 **FIXED (분류 B)**이며 최종 전체 gate는 180/180 통과했다.

작업 위치는 `/home/hep7/project/zlink-core-a`, 기준은 detached `34dfa9d947`이다. Core 변경은 이 worktree에만 있으며 main의 `core/build-dev`는 사용하지 않았다. main에는 이 보고서만 작성했다. commit 없음. spec·binding·framework 변경 없음.

## 원인과 수정

### Listener의 fd 해제 경계

- 기준 코드 `core/src/runtime/transports/tcp/asio_tcp_listener.cpp:82-89`는 공통 acceptor 설정에 `enable_reuse_port=true`를 넘겼다. `core/src/runtime/transports/asio/asio_tcp_acceptor_config.hpp:74-87`이 `SO_REUSEPORT`를 설정하므로 이전 listener가 존재하는 동안에도 새 bind가 성공했다.
- `socket_base_endpoint.cpp:1101`의 기존 unbind는 `term_child` 명령만 보냈다. close는 `socket_base_api.cpp:172`의 `finish_close_reap`에서 inproc 등록만 해제하고 reaper에 넘겼다. TCP `process_term`(:121), IPC(:210), WS(:151), TLS(:130)의 I/O-thread 처리 시점에 fd가 닫혔다.
- `core/src/runtime/core/object.cpp:485`의 `release_endpoint`는 기존 mailbox에 해제 명령을 보내고 그 명령의 completion을 기다린다. listener는 기존 close 구현을 `process_release_endpoint`로 옮겨 재사용한다. `_terminating`을 설정한 뒤 acceptor를 닫으므로, 이미 queue에 있던 accept callback도 새 session을 만들지 않는다. command seqnum은 처리 중 listener의 수명을 보호한다.
- `socket_base_api.cpp:178-185`는 reaper에 넘기기 전에 bound listener 각각의 해제를 완료한다. `socket_base_endpoint.cpp:1101-1103`도 listener 해제를 완료한 다음 기존 child 종료를 요청한다. accepted session·pipe·linger 정리는 기존 종료 경로가 소유한다.
- TCP의 `SO_REUSEPORT` 사용과 공통 helper의 해당 옵션을 제거했다. 유지할 계약 근거는 없다. `SO_REUSEADDR`와 Windows의 `SO_EXCLUSIVEADDRUSE`는 기존 설정을 유지한다. WS/TLS도 같은 acceptor 설정을 사용하며 WSS는 WS listener 구현을 공유한다.

비교한 대안은 전체 child 종료 ACK 대기와 endpoint 해제만의 completion 대기다. 전체 종료 대기는 accepted session의 linger까지 공개 호출에 포함하므로 선택하지 않았다. 선택한 구현은 fd 소유자인 I/O thread에서 해제를 완료하고, transport별 fd close 구현을 복제하지 않는다. bind-side wait·retry 없음.

### Inproc 2-lane unbind 지연

- `core/src/runtime/sockets/common/socket_base_endpoint.cpp:954-978`의 `terminate_inproc_pipe_with_peer_progress`가 `get_transport_lane_count() == 1u`인 경우에만 peer executor를 시작했다. 유휴 ROUTER↔ROUTER의 두 lane에서는 peer ACK가 진행하지 않아 :1054-1063의 20×10ms 대기를 소진했다.
- :966의 lane-count 조건을 제거했다. 기존 peer 수명 보호, `set_nodelay`, executor 시작·반납 경로를 모든 lane에 그대로 적용한다. 대기 budget·재시도 횟수·completion 결과 규칙은 변경하지 않았다.

소유 계층: Core socket lifecycle·endpoint 관리, I/O-thread listener, inproc pipe command 처리.

spec 조항: `core/doc/spec/core/socket/README.ko.md` § `zlink_close`(:607-624)의 자원 해제, § `zlink_bind`(:794-818)의 주소 점유와 `EADDRINUSE`, § `zlink_unbind`(:843-853)의 binding 제거, §6 completion 표(:1149)의 명시적 endpoint 제거 → `ZLINK_REQUEST_NOT_FOUND`. D-098 항목 1·2가 적용 범위를 확정한다.

교차언어·transport 대조: Framework runtime 변경 없음. 공개 C API에서 검증했으며 별도 binding 언어 테스트는 실행하지 않았다. inproc의 반환 전 등록 해제와 TCP·IPC·WS·TLS·WSS의 반환 전 fd 해제를 같은 완료 규칙으로 맞췄다.

변경 분류: **B — 기존 결함**, D-098 항목 1·2의 승인 범위.

수정 전/후 규칙 수: endpoint 해제 시점 2(inproc 동기 / listener 비동기) → 1, peer 진행 2(lane 1 / lane 2) → 1, 합계 **4 → 2**. 영속 상태·타이머·registry 추가 없음. 공개 호출 동안만 존재하는 command completion을 사용하고 기존 `_terminating` 상태와 close 구현을 재사용한다.

## 변경 파일

- `core/src/runtime/core/{command.hpp,object.hpp,object.cpp}`: endpoint 해제 명령과 completion.
- `core/src/runtime/sockets/common/{socket_base_api.cpp,socket_base_endpoint.cpp}`: close·unbind 완료 경계, lane 조건 제거.
- `core/src/runtime/transports/asio/asio_tcp_acceptor_config.hpp`: `SO_REUSEPORT` 설정 제거.
- `core/src/runtime/transports/{tcp,ipc,ws,tls}/asio_*_listener.{hpp,cpp}`: 기존 fd close를 command handler로 재사용.
- `core/tests/integration/test_endpoint_release.cpp`, `core/tests/CMakeLists.txt`: 공개 C API integration test 등록(`integration;serial`).

Core diff는 16개 파일, +384/-45줄이다. 이 중 신규 테스트가 313줄이다.

## 검증 결과

신규 테스트는 TCP close·unbind 각각 100회, IPC·WS·TLS·WSS close·unbind 각각 10회, 유휴 2-lane unbind 50회, DISCONNECTED·NOT_FOUND 50회를 수행한다. 네트워크 listener가 점유 중일 때 중복 bind는 `EADDRINUSE`이고, 해제 직후 첫 rebind와 새 listener의 payload 수신은 성공해야 한다. unbind 후 이전 listener의 ACCEPTED 이벤트도 없어야 한다.

유휴 지연 측정에서는 READY를 확인한 monitor를 측정 전에 닫는다. peer monitor는 peer 진행을 직접 구동하며, caller monitor는 `process_commands`의 blocking wait를 생략하게 하므로 둘 다 원래 지연을 가린다. pending request 의미 검증은 양쪽 READY를 확인한 별도 case에서 수행한다. sleep 기반 동기화·assertion 완화 없음.

| 검증 | 결과 |
|---|---|
| 원인 경로를 기준 커밋으로 복원한 대조 build | TCP 중복 bind 허용 재현, WS/WSS 첫 rebind 실패, IPC unbind CLOSED 미도착. 유휴 2-lane은 첫 회 **191,719µs**로 `<50ms` 실패. TLS는 이 실행에서 통과 |
| `nice -n 10 env JOBS=4 scripts/build-core.sh dev` | PASS, worktree의 `core/build-dev` 사용 |
| 신규 테스트 `--repeat until-fail:10`, CPU busy-loop process 4개 병행 | **10/10 PASS**, 31.54s. 모든 transport 활성, ignored 0 |
| 위 반복의 TCP close·unbind | 총 **2,000회 PASS** |
| 위 반복의 유휴 2-lane unbind | 총 **500회 PASS**, 최대 **271µs** |
| 위 반복의 DISCONNECTED·NOT_FOUND | 총 **500회 PASS**, unbind 최대 46µs |
| 지정 targeted CTest 정규식 | **9/9 PASS**, 31.90s |
| 전체 `ctest --test-dir core/build-dev -j2 -E '^hotpath_gate$' --output-on-failure` | **180/180 PASS**, 239.91s, 전체 실행 1회 |
| `git diff --check HEAD -- core` | PASS |
| 생성 패치 `git apply --check --reverse core-d098.patch` | PASS |

지정 targeted 정규식: `test_close_releases_inproc_endpoint|test_transport_matrix|test_monitor|test_request_explicit_removal_not_found|test_socket_disconnect_boundary|test_router_same_socket_reconnect_policy`.

로그는 worktree의 `core-d098-build.log`, `core-d098-baseline.log`, `core-d098-repeat-load-final.log`, `core-d098-repeat-detail-final.log`, `core-d098-targeted.log`, `core-d098-full.log`에 보존했다. `hotpath_gate`는 요청대로 제외했으며 통과로 계산하지 않는다.

## 산출물과 BLOCKERS

패치: `/home/hep7/project/zlink-core-a/core-d098.patch` (32,401 bytes). 요청한 `git add -N . && git diff HEAD -- core > /home/hep7/project/zlink-core-a/core-d098.patch`로 생성했다.

BLOCKERS: **없음**. 최종 수정본의 신규 반복·지정 회귀·전체 gate에 남은 실패 없음.
