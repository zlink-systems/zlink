# Core MP-3 동시 multipart 독립 리뷰 수정 보고서

## 결과

MP-2의 caller별 sequence 설계는 유지하면서 독립 리뷰의 차단 항목 B01~B06을 수정했다. 같은
REPLY token의 중복 checkout은 다시 `INVALID_STATE/EBUSY`로 수렴하고, logical RID 제거와
close·OOM·validation abort에서 registry slot, payload, context의 소유권과 해제 위치를 분리했다.
추가한 동시성·수명·OOM 테스트와 dev/ASan/TSan 직접 변경 target은 모두 통과했다.

구현 patch는 `/home/hep7hep7/project/zlink-work/mp2`에 미커밋으로 남겼다. 작업 시작 전 MP-2
diff는 지시된 `scratchpad/mp2-before-mp3.patch`에 보존했다. 스펙, 공개 헤더,
`libzlink.vers`는 수정하지 않았다.

소유 계층: caller별 logical 조립·폐기는 기존 part helper, physical admission·rollback은 socket
complete-send 경로, REPLY token·checkout·slot은 request/reply registry가 각각 단독 소유한다.

스펙 조항: main의 미커밋 Socket README `:1035-1038`(allocation failure), `:1123-1133`
(REPLY 실패·중복 token), `:1135-1142`(token/slot 수명) 및 MP-1 §4.1~4.3을 적용했다.

교차언어 대조: C++(`bindings/cpp/src/Runtime/Sockets/socket.cpp:186`), .NET
(`SocketKernel.SendCore.cs:139`), Node(`addon_core.cc:395`), Java(`Native.java:566`),
Rust(`send_ops.rs:129`), Go(`dealer_router_request.go:14`) 모두 Core의 같은 part API에 위임한다.
언어별 multipart registry를 추가하지 않았으므로 Core 한 곳의 변경으로 동작이 일치한다.

변경 분류: **B(기존 결함)**. 확정 계약을 상위 계층에서 보상하지 않고 Core 소유 모듈에서
수정했다.

규칙 수: MP-1의 대상 규칙 계산을 유지한다. 수정 전 6개(단일 slot·marker/suspend/resume 등)에서
수정 후 3개(caller sequence/정리, FINAL physical admission/rollback, pipe physical control 경계)다.

## 리뷰 ID별 수정

| ID | 수정 내용 | 구현 파일:행 | 직접 검증 |
|---|---|---|---|
| B01 | token 조회 결과를 checked-out, missing, consumed, revoked, invalid로 분리했다. checked-out은 `EBUSY`, 나머지 무효 token은 `ENOENT`이며 첫 sequence는 그대로 둔다. | `socket_request_reply_internal.hpp:549`, `socket_request_reply_runtime_io.cpp:677`, `socket_request_reply_submit_api.cpp:309` | `test_phase3_request_reply_contract.cpp:1617`의 같은 token EBUSY 및 `:1757`의 서로 다른 token 동시 성공 |
| B02 | 만료 node는 helper mutex 아래 `extract`로 소유권만 옮기고 `:793-795`에서 mutex 밖에서 해제한다. DONTWAIT SEND/REQUEST는 record 분리 뒤에만 complete scope를 얻으므로 zero-copy callback이 physical sync 안에서 실행되지 않는다. | `part_helper_api.cpp:86-114,789-805`, `socket_message_send_api.cpp:457-490`, `socket_request_reply_submit_api.cpp:904-948` | `test_helper_ownership.cpp:553`의 free callback 재진입 option 조회 |
| B03 | TLS identity를 지연 생성하고 생성/조회 예외 경계를 한 함수가 소유한다. 실패는 `ENOMEM`, ID 0, 현재 part 소비로 반환하며 abort 조회는 identity를 새로 만들지 않는다. | `part_helper_api.cpp:26-49,356`, `part_helper_internal.hpp:222` | `unittest_phase3_request_reply_owners.cpp:524` |
| B04 | close seal 뒤 vector `reserve`를 제거했다. helper store 전체를 무할당 `swap`으로 분리하고 helper mutex 밖에서 payload/context를 닫는다. | `part_helper_state.cpp:43-76` | `test_helper_ownership.cpp:693`의 살아 있는 caller 4개와 zero-copy free exact-once |
| B05 | logical RID revoke 시 checked-out entry는 revoked tombstone만 남기고 checkout·slot counter를 즉시 한 번 반환한다. 늦은 restore/commit은 tombstone만 지우며 counter를 다시 감소시키지 않는다. | `socket_request_reply_runtime_io.cpp:677-786,842-878` | `unittest_phase3_request_reply_owners.cpp:639` |
| B06 | SEND/SEND_RID/REQUEST/REPLY entry에서 validation보다 먼저 public handle을 pin하여 detached payload와 REPLY context 해제가 끝날 때까지 socket 파괴를 막는다. 실제 해제는 계속 helper mutex 밖이다. | `socket_message_send_api.cpp:596,675`, `socket_request_reply_submit_api.cpp:1128,1217`, `part_helper_api.cpp:1004` | validation abort/zero-copy errno 보존 `test_helper_ownership.cpp:390,531`, ASan+LSan suite |
| W01 | part 크기를 memcpy 전에 무조건 검사하고 `(caller,sequence)`의 중복·누락 없는 전체 집합을 확인한다. | `test_public_inproc_multipart_send.cpp:930-1070` | PAIR/DEALER/ROUTER 각각 4 caller × 100 record |
| W02 | 요청별 completion ID와 고유 context를 보관하고 reply payload와 함께 정확히 대조한다. 길이와 index 범위 검사를 먼저 한다. | `test_phase3_request_reply_contract.cpp:1820-2020` | REQUEST/REPLY 4 caller × 20 sequence, 80 completion 전수 |
| W03 | checkout 직후 context allocation 실패와 첫 MORE 성공 뒤 다음 MORE staging 실패를 분리해 token restore 및 다른 caller 재제출을 확인한다. | `socket_request_reply_submit_api.cpp:586-615`, `unittest_phase3_request_reply_owners.cpp:463,582` | 두 OOM case 모두 다른 thread FINAL 성공 |
| W04 | lookup과 prepare를 한 helper 임계 구간으로 결합하고 heterogeneous `owner_less<void>` lookup으로 임시 weak ownership을 없앴다. 만료 map은 entry마다 재스캔하지 않고 한 번 순회해 node를 이동한다. single REPLY는 heap context 대신 stack context를 쓴다. 새 TLS raw cache·이중 map은 없다. | `part_helper_internal.hpp:88`, `part_helper_api.cpp:57-114,758-805`, `socket_request_reply_submit_api.cpp:520-575` | hotpath 5셀 및 별도 warmed single/4-thread 셀 |
| W05 | SEND/SEND_RID/REQUEST/REPLY의 MORE staging 전에 `is_ctx_terminated()`를 검사하여 `TERMINATED/ETERM`을 유지한다. | `socket_message_send_api.cpp:640,732`, `socket_request_reply_submit_api.cpp:1180,1248` | `test_helper_ownership.cpp:759`에서 staged REQUEST FINAL이 실제 대기 중임을 확인한 뒤 shutdown으로 `ETERM` 종료 |
| W06 | API가 보유한 local strong identity 때문에 정상 함수 실행 중인 caller를 만료 scan이 회수하지 않는다. | `part_helper_api.cpp:26-49,782-805` | thread 종료 뒤 새 identity가 abandoned prefix를 잇지 않는 `test_helper_interleave.cpp:137` |
| W07 | DONTWAIT, 3-part의 두 번째 MORE OOM, 5-part spill, 서로 다른 RID 두 개의 동시 ROUTER, 같은 thread의 두 socket, 네 slot close, MORE/FINAL 실행 중 close와 staged+blocked FINAL+shutdown을 고정했다. worker init 실패도 barrier 단계에 참여하도록 해 동료 worker가 남지 않게 했다. | `test_helper_ownership.cpp:553,605,657,693,759`, `test_public_inproc_multipart_send.cpp:807,1126`, `unittest_phase3_request_reply_owners.cpp:582` | dev 및 갱신된 ASan/TSan target 통과 |
| S01 | sequence를 store에서 먼저 분리하고 `reset_send_sequence` 한 경로로 payload → scope/context 순서로 닫도록 통합했다. | `part_helper_api.cpp:105,667`, `part_helper_state.cpp:67-74` | close·expired·abort·OOM 테스트 |
| S02 | complete-send 주석은 public sync와 PUB/XPUB physical marker의 소유 범위를 구분했다. PUB/XPUB 잔존 코드는 삭제하지 않았다. | `socket_send_complete.cpp:358`, `part_helper_internal.hpp:199` | 전체 dev suite |

### W06 지원 범위와 스펙 문장 제안

지원 범위는 thread의 정상 함수 실행 중 호출과 library caller identity가 아직 살아 있는 application
TLS destructor 호출까지다. Library caller identity가 이미 소멸한 뒤 같은 thread의 더 늦은 TLS
destructor에서 Core API를 새로 호출하는 순서는 지원 범위 밖이다. 이번 작업은 스펙을 수정하지
않았다. 제안 문장은 다음과 같다.

> Thread 종료 시 application TLS destructor가 Core multipart API를 호출하려면 해당 호출은 Core가
> 그 thread의 caller identity를 파괴하기 전에 완료되어야 한다. 이후 호출의 동작은 정의하지 않는다.

## 검증 결과

| 검증 | 범위 | 결과 |
|---|---|---|
| dev build | `cmake --build core/build-dev -j4` | 성공 |
| dev 전체 ctest 1회 | `ctest --test-dir core/build-dev --output-on-failure -E hotpath_gate` | **207/207 통과**, 244.12초 |
| 관련 suite 반복 | `part|multipart|send|request|reply|router|dealer|pair|flow|hwm|close|wake`, `until-fail:5` | **80 target × 5 = 400/400 통과**, 678.73초 |
| lost-wake 반복 | CMake `wake-invariant` 라벨 4개, `until-fail:20` | **4 × 20 = 80/80 통과**, 620.10초 |
| ASan+LSan build/run | `detect_leaks=1:halt_on_error=1:abort_on_error=1`, close/abandoned/OOM 포함 6개 직접 target | **6/6 통과**, 16.37초, sanitizer·leak 오류 0. W07의 RID/close 및 blocked-FINAL 마지막 갱신 target도 각각 재빌드 후 1/1 통과 |
| GCC TSan build | `-fsanitize=thread -fno-omit-frame-pointer -fPIE`, `setarch x86_64 -R`, 기존 `/tmp/mp2-tsan.supp` | 성공, `libzlink.so`의 `__tsan_read*` 참조 확인 |
| TSan 신규·변경 executable | caller/close/REQUEST/REPLY/control 관련 | **8/8 통과**, 24.20초. W07의 RID/close 및 blocked-FINAL 마지막 갱신 target도 각각 재빌드 후 1/1 통과 |
| TSan 관련 suite 1회 | dev와 같은 관련 정규식 80개 | **75/80 통과**, 170.94초. 아래 기존 debt 5개만 실패 |
| Release+LTO lib | `JOBS=4 scripts/build-core.sh release --lib-only` | 성공 |
| diff 검사 | `git diff --check` | 통과 |
| 공개 API 검사 | `git diff --stat -- core/include core/src/libzlink.vers` | 출력 없음 |

TSan suppression은 기존 custom lock-free 경로만 포함하며 MP-2/MP-3 함수나 caller slot 경로를
숨기지 않는다.

| TSan 잔여 실패 | 관찰된 파일:행 | 분류 |
|---|---|---|
| `test_router_reject_duplicate` | `socket_base_lifecycle.cpp:842,1010,1057`, `ctx_auto_hwm_recalc.cpp:138`의 monitor/ctx/async-mailbox lock-order cycle | MP-2에서도 동일한 기존 debt, 변경 파일 밖 |
| `test_router_reject_disconnected_without_app_recv` | 위와 같은 monitor/ctx lock-order 계열 | 기존 debt |
| `test_router_same_socket_reconnect_policy` | 위와 같은 monitor/ctx lock-order 계열 | 기존 debt |
| `test_router_mandatory_hwm` | `test_router_mandatory_hwm.cpp:111,175`의 10ms 제한에 TSan 계측값 43/44ms | 계측 timing 한계; dev 반복은 통과 |
| `unittest_flow_state_socket` | `lb.cpp:159` read와 `lb.cpp:137` write의 기존 peer-weight race | 기존 flow-state debt, multipart 변경 밖 |

## 성능

### Release+LTO hotpath 5셀

지정된 `PERF_LOCK`을 얻고 한 번 실행했다. 시작 load average는 **1.57 / 1.46 / 1.19**였다.

| cell | reference Ir/msg | measured Ir/msg | 변화율 | 판정 |
|---|---:|---:|---:|---|
| `dealer_dealer_inproc` | 3230.922 | 3265.081 | +1.06% | PASS |
| `dealer_router_reqrep_inproc` | 18663.506 | 18531.551 | -0.71% | PASS |
| `pair_inproc` | 2348.457 | 2341.905 | -0.28% | PASS |
| `router_router_tcp` | 2972.532 | 2924.844 | -1.60% | PASS |
| `stream_tcp` | 14623.471 | 14239.100 | -2.63% | PASS |

### MP-1 §7 추가 dev 셀

임시 C++ runner를 `-O2`로 한 번 빌드하고 동일 binary를 `LD_LIBRARY_PATH`만 바꿔 PERF_LOCK 아래
교차 실행했다. main은 clean Core source의 `30144187d6`, MP-3은 `3a7db427bc` + 미커밋 patch다.
wall time은 sender와 receiver가 지정 record를 모두 처리할 때까지 측정했다. payload는 16 byte,
HWM은 unlimited다.

| 셀 | main(MP patch 없음) | MP-3 | 비교 |
|---|---:|---:|---:|
| 같은 PAIR socket에서 2-part 100회 후 single 100,000회 | 22.491ms | 27.821ms | **+23.70%** |
| 4 thread × 2-part 20,000회(총 80,000 record) | 20초 안에 완료 못함(timeout) | 110.396ms, **724,662 record/s** | main은 caller 독립 조립 전 상태라 유한 처리량 비교 불가 |

시작 load average는 single main/MP-3 모두 0.18/0.79/0.98, concurrent main
0.18/0.79/0.98, MP-3 0.13/0.73/0.96이었다. 이 dev wall-time 한 번으로 성능 개선을 주장하지
않는다.

## 변경 파일

구현은 `core/src/api/socket/part_helper_*`, `socket_message_send_api.cpp`,
`socket_request_reply_*`와 기존 complete-send 연결부에 있다. 테스트는
`core/tests/integration/{test_public_inproc_multipart_send,test_helper_ownership,test_helper_interleave,test_phase3_request_reply_contract}.cpp`,
관련 single-lane test와 `core/tests/unittest`의 ownership/admission test를 수정했다. 최종 diff는
25개 파일, +2373/-575이며 공개 API diff는 없다.

## 남은 위험

- warmed single-after-multipart dev 셀이 main보다 23.70% 느렸다. `owner_less<void>`와 임계구간
  통합으로 refcount RMW와 중복 lookup은 줄였지만 helper state가 생성된 socket의 single SEND는
  active caller 판정을 위해 helper mutex 한 번을 계속 얻는다. 이를 없애는 TLS raw cache나 두 번째
  active map은 금지된 중복 상태이므로 추가하지 않았다. 후속 최적화는 기존 helper 소유 구조 안에서
  단일 사실 소유권을 유지해야 한다.
- application TLS destructor가 library identity 파괴 뒤 API를 호출하는 순서는 위 제안대로 현재
  정의되지 않았다.
- TSan 전체 green을 막는 5개 기존 debt는 이번 multipart patch와 분리해 처리해야 한다.
- main 기준에는 MP-2 기준 commit 뒤의 G-11b 성능 commit이 포함되어 있으므로 추가 dev 셀은
  엄밀한 단일변수 microbenchmark가 아니다. 사용자 지시의 현재 clean main 비교값으로만 기록했다.
