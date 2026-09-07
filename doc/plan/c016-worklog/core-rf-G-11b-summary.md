# Core RF G-11b 결과 — session endpoint `_out_sync` 제거 중단

## 1. 결과

지정된 설계 B ②를 그대로 적용할 수 없음을 TSan으로 확인해 runtime 변경을 전부 원복했다.
최종 diff에는 이 보고서만 남고 `core/**` 변경은 없다.

부록 A (a)의 전제인 “session endpoint의 outbound-owner turn 밖에서 쓰기 ledger를 읽는 곳은
`_peers_msgs_read`/`_peers_bytes_read`뿐”은 현재 `origin/main`(`c1ff4371e3`, G-11a 포함)에서
성립하지 않는다. 공개 monitor snapshot 경로가 반대 endpoint의 누적 쓰기 ledger를 직접 읽는다.

- `pipe.cpp:777-791`: `get_msgs_written()`·`get_bytes_written()`은 `_out_sync`를 잡고
  `_msgs_written`·`_bytes_written`을 읽는다.
- `pipe.cpp:832-869`: `get_rcv_pending_{msgs,bytes}_approx()`가 peer의 위 getter를 호출한다.
- `socket_base_monitor.cpp:62-74`: application thread의 monitor snapshot이 이 경로에 진입한다.
- session ASIO I/O thread는 `session_base_pipe_io.cpp:129`의 `pipe_t::write()`를 거쳐
  `write_message_unlocked()`에서 같은 두 ledger를 갱신한다. `write()`가 `_out_sync`를 생략하면
  getter가 잡는 mutex는 더 이상 writer와 동기화하지 않는다.

실험 패치의 TSan은 이 접근을 main thread의 read 대 `ZLINKbg/IO/*`의 write race로 직접 보고했다.
따라서 peer credit 두 개만 원자화하는 지정안은 C3 경계를 완성하지 못한다.

## 2. 소유권·교차 접근 재확인

session endpoint의 실제 writer와 `activate_write` command는 같은 `io_thread_t`의 ASIO context에서
실행된다는 부록 A의 첫 전제는 코드와 call stack에서 재확인했다. 문제는 그 turn 밖의 **reader**다.

요청에서 별도로 지목한 두 cold path는 잠금 제거 대상 필드와 겹치지 않음을 확인했다.

- `detach_peer_link()`는 종료 handshake 뒤 `_peer` atomic link만 exchange/CAS한다. write/flush는
  peer를 atomic snapshot으로만 얻고, local 쓰기 ledger나 `_out_active`를 이 함수와 함께 바꾸지 않는다.
- `stream_t::identify_peer()`는 bind/identity publication 순서에서 routing ID만 설정한다.
  write/flush의 credit·queue ledger와 필드가 겹치지 않는다. 따라서 두 함수의 `_out_sync`는 유지할 수 있다.

하지만 monitor snapshot reader는 teardown/identity와 달리 정상 실행 중 concurrent 접근이며, 순서 보장도
같은 스레드도 아니다. 이 반증 때문에 중단 조건을 적용했다.

교차언어 대조 결과, 이 경로는 모든 binding이 함께 쓰는 Core C ABI 아래에 있어 언어별 runtime 차이가
없다. G-11의 libzmq 대조처럼 SPSC ypipe writer 자체는 단일 owner로 둘 수 있지만, zlink가 추가한 공개
monitor snapshot reader 때문에 이 저장소에서는 누적 ledger publication 규칙이 별도로 필요하다.

## 3. 설계 비교와 선택

1. **지정안:** session `write`/`flush`만 lock-free로 하고 peer credit 두 개와 `_out_active`만
   atomic/CAS로 바꾼다. lock/msg 목표는 달성했지만 local 쓰기 ledger에 신규 race 21건을 만들어 기각했다.
2. **범위 확장안:** `_msgs_written`·`_bytes_written`도 C3 publication으로 바꾸거나 monitor snapshot을
   기존 `_published_outbound_*`/owner command 경로로 재설계한다. 첫 안은 메시지당 두 ledger의 모든
   산술·reset·conflate 규칙을 원자 publication 계약으로 넓히고, 둘째 안은 메시지 수 snapshot이 현재
   없어 관측 계약을 다시 정해야 한다. 둘 다 “turn 밖 접근은 peer credit뿐”이라는 승인 범위를 넘는다.
3. **선택:** 현재 `_out_sync`를 유지하고 멈춘다. 새 상태·플래그·우회 경로는 추가하지 않았다.

수정 전/후 규칙 수: 최종 runtime은 **1→1(변경 없음)**이다. 기각한 패치는 writer 규칙을 줄이는 대신
monitor ledger publication 규칙을 누락해 단순화가 아니었다.

## 4. 측정

모든 valgrind/benchmark는 ninja가 없을 때 `PERF_LOCK`의 `flock` 아래 foreground로 실행했다.

| 항목 | pristine | 기각 패치 | 변화 |
|---|---:|---:|---:|
| `stream_tcp`, `pthread_mutex_lock` calls/msg | 23.919 | 21.718 | **-2.201** |
| mutex self Ir/msg | 880.9 | 801.4 | -79.5 |
| 동일 축소 cell 전체 Ir/msg | 15,521.9 | 15,597.3 | +75.4 |

목표 `-2.0 lock/msg`는 구조적으로 확인됐지만 correctness gate가 실패했으므로 채택 근거가 아니다.
dev hotpath 5셀은 pristine/기각 패치가 각각
`4075.885/22385.566/2947.036/3557.431/15521.641`과
`4082.663/22478.219/2983.363/3559.742/15412.976` Ir/msg였다. 이는 Release/LTO reference에
대한 권위 있는 판정이 아니다. stop 조건 발동 뒤 unsafe patch의 `build-gate`와 with_stream을 계속
측정하지 않았다.

## 5. 빌드·테스트

| 검증 | 결과 |
|---|---|
| pristine `JOBS=4 scripts/build-core.sh dev` | PASS |
| 기각 패치 dev 증분 빌드(관련 5 target) | PASS |
| `ctest -R 'pipe|stream|router|dealer|pair'` 5회 | 4회 41/41 PASS; 5회차 40/41, 기존 간헐 `test_stream_socket_recv_multiclient_ready_regression` timeout |
| 위 실패 단독 `until-fail:3` | 3/3 PASS |
| lost-wake 5종 `until-fail:20` | **100/100 PASS**, 652.66 s |
| `test_close_completion_poller_release` `until-fail:50` | **50/50 PASS**, 34.50 s |
| 수동 TSan, LTO OFF, 4 target | pristine 20건 → 기각 패치 41건, **신규 21건** |

TSan의 기존 20건은 `ypipe_t<command_t>::check_read()` 17건과 receive guard 3건이다. 신규 21건은
`get_msgs_written()` 8건, `get_bytes_written()` 10건, 그 상대 write 위치 3건으로 모두 이번 잠금
생략에서 생겼다. `test_close_completion_poller_release`는 TSan에서도 0건이었다.

## 6. 스펙 재확인

- 05-polling §3: “Readiness는 level-trigger이므로 wake도 level에 따른다.”, “Readiness가 참인데
  caller가 timeout까지 잠드는 것(lost wake)은 계약 위반”이다.
- 06-auto-hwm: “charge가 queue 회계에 반영되기 시작하는 시점은 frame write부터다.”,
  “Core HWM charge의 종료 경계는 complete message를 queue에서 dequeue해 binding에 넘기는 시점”이다.
- Framework lane §4의 C3는 “정수 증가, 플래그 확인, 단일 참조 교체”에 atomic을 요구하고, §5는
  “한 번에 한 turn만 실행한다.”고 규정한다. monitor reader는 그 turn 밖이므로 별도 publication이 필요하다.
- S-1의 ypipe sleep/awake 진리표와 `flush_unlocked()`의 sleep episode당 `activate_read` 1회 규칙은
  건드리지 않았다.

최종 runtime 코드에서는 위 어느 문장도 다른 동작이 되지 않았다.

## 7. 변경 분류와 멈춘 지점

**변경 분류: C — 지정안 그대로는 monitor ledger race를 남기는 불완전한 잠금 우회이므로 기각·원복.**

멈춘 지점은 `_msgs_written`·`_bytes_written`의 publication 소유자를 정하는 설계 선택이다. 이 두
ledger까지 atomic으로 넓힐지, 기존 outbound byte snapshot을 monitor 계약의 소유자로 삼고 message
snapshot을 추가할지 감독 승인 없이는 진행하지 않았다. 공개 header·`libzlink.vers`·스펙 문서는 수정하지
않았고 commit/stash도 하지 않았다.
