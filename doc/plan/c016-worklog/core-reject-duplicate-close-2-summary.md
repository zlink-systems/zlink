# Core REJECT duplicate close — round 2

2026-09-05. D-090의 pending REQUEST 종료 규칙을 공통 socket 소유자로 옮겼고,
round 1의 REJECT duplicate close를 유지했다. inproc overlap의 서버 term ACK 진행을
막던 executor 종료와 빈 receive의 거짓 progress 알림을 수정했다.
새 계약 검증은 **20회 × 7 case = 140/140 PASS**다. 최종 전체 CTest는
**166/171 PASS**이며, 기존 계약 assertion과 dev hotpath reference 때문에 gate는 green이 아니다.

구현 위치는 `/home/hep7/project/zlink-core-a`, 기준 HEAD는 `fadfac1c4e02c33d1a8eb19f39457e14e5a61b7d`다.
명시된 detached worktree에서 round 1 diff를 보존했고 commit하지 않았다.
아래 source 경로와 line은 이 worktree 기준이다. main에는 이 보고서만 작성했다.
main의 `core/build-dev`, worktree b, spec, binding 및 Framework source는 변경하지 않았다.

## 변경과 소유권

| 파일:line | 최종 변경 |
|---|---|
| `core/src/runtime/sockets/common/socket_base_api.cpp:1717` | `pipe_peer_terminated()`가 기존 `fail_pending_requests_for_transport_pair()`의 유일한 runtime 호출 소유자다. pair id/generation으로 해당 socket의 pending만 종결한다. `:1745`의 최종 detach도 이 소유자를 거친다. |
| `core/src/runtime/core/pipe.cpp:2900`, `:2908` | peer term을 받으면 DATA delimiter drain을 기다리는 경우에도 socket에 즉시 알린다. pending correlation 해제가 outbound gate를 다시 획득하므로 알림은 `_out_sync`를 놓은 뒤 전달한다. |
| `core/src/runtime/core/pipe.hpp:124`, `core/src/runtime/core/session_base.cpp:453` | 기존 이벤트에 현재 drain 완료 여부를 전달한다. socket은 즉시 REQUEST를 종결하고, session은 drain이 완료된 경우에 기존 engine 종료·reconnect 처리를 수행한다. 저장되는 상태를 추가하지 않았다. 선언은 `session_base.hpp`, `socket_base.hpp:654`에도 반영했다. |
| `core/src/runtime/sockets/router/router_admission.cpp:361`, `:409`, `:453`; `router.hpp:116` | REJECT의 `terminate_pipe` 처리를 유지한다. HANDOVER의 직접 pending-helper 호출과 pair id/generation action 필드를 없앴다. 기존 pipe를 잠시 retain하여 공통 종료 알림에 전달한다. |
| `core/src/runtime/sockets/common/socket_base_endpoint.cpp:438`, `:979` | inproc bind/disconnect에서 임시 peer executor를 강제 quiesce시키던 호출을 기존 idle-stop 요청으로 통일했다. |
| `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:1179` | attached pipe가 종료 중이면 mailbox가 잠깐 비어도 임시 executor를 유지한다. 기존 pipe lifecycle과 attached-pipe 목록만 사용하며, 목록 조회는 기존 `monitor_runtime().sync`로 보호한다. |
| `core/src/runtime/sockets/common/socket_base_api.cpp:1268` | 실제 held pipe의 해제·재분류가 있을 때만 receive progress를 알린다. 빈 receive 취소가 자기 wait를 깨우던 알림을 제거하고 기존 두 `if (held_pipe)`를 하나로 합쳤다. |
| `core/tests/integration/test_router_reject_duplicate.cpp:232`, `:292`, `:400`; `core/tests/CMakeLists.txt:123` | 공개 C API로 REJECT, pending 종료, 원래 timeout 이후 중복 completion 부재, 재연결 성공, WRITABLE 토큰 보존을 검증한다. |

기존 sink mock의 이벤트 signature만 변경한 파일은 다음 여섯 개다. assertion은 변경하지 않았다.

- `core/tests/integration/test_ctx_destroy.cpp:327` (`:349`, `:366` 포함)
- `core/tests/integration/test_flow_state_paired.cpp:51`
- `core/tests/integration/test_pubsub_filter_xpub.cpp:52`
- `core/tests/integration/test_router_concurrent_routed_recv.cpp:174`
- `core/tests/integration/test_router_multiple_dealers.cpp:43` (`:2060`, `:2170` 포함)
- `core/tests/integration/test_zmp_request_reply_receive_transaction.cpp:111`

HANDOVER standby는 기존 계약에 따라 물리 lane을 유지한다. 따라서 물리 detach 알림만으로는
supersession을 검출할 수 없다. `router_admission.cpp:456`에는 **공통 socket 종료 알림**을
남겼으며, ROUTER가 pending pool을 직접 종결하는 호출은 제거했다. 실제 detach가 뒤따라도
기존 pending의 terminal 소유자가 이미 제거한 항목을 다시 완료하지 않는다.

**수정 전/후 규칙 수: REQUEST 종료 원인별 3규칙 → submit 시점 pair 종료의 1규칙.**
거부 사유 wire 전달, pending index, timer, retry 정책 및 저장되는 종료 상태를 추가하지 않았다.
기존 helper의 구현과 pending pool은 재사용했다.

비교한 대안은 원인별 REJECT 통지·HANDOVER 처리·timeout 유지와 공통 pair 종료 처리다.
D-090에 따라 후자를 적용했다. 명령 진행에서도 bind/disconnect별 강제 executor 종료 대신
기존 idle-stop 소유자가 pipe 종료 절차의 완료를 판단하게 했다.

- 소유 계층: Core socket의 pending pool, Core pipe/session의 transport 종료, Core mailbox executor의 command progress.
- Spec 조항: main `core/doc/spec/core/socket/README.ko.md` §4 RID 정책·pair pinning (`:159`), §6 completion 표 (`:1141`), `core/doc/spec/core/05-polling.ko.md` §3 wake/command progress (`:73`); 감독 결정 D-090.
- 교차언어 대조: C++ `bindings/cpp/src/Runtime/Messaging/completion_owner.cpp:646`과 Python `bindings/python/src/zlink/_runtime/messaging/routed_async.py:907` 모두 Core completion을 소비한다. 두 binding의 기존 NOT_CONNECTED 처리와 최종 local-library 검증을 확인했고 binding runtime은 변경하지 않았다.
- 변경 분류: **A 계약 적응** — D-090의 REQUEST 종결 통일. **B 기존 결함** — 종료 ACK 전에 executor를 멈추는 경계와 실제 수신 진행 없이 알림을 보내는 경계.

## BLOCKER 2 원인과 결과

기존 inproc 경로는 peer의 bind/첫 term command를 처리한 뒤 executor를 강제로 멈췄다.
old pipe와 REJECT된 pipe의 후속 term ACK가 그 뒤 mailbox에 도착하면 서버의 다음 application
poll 또는 teardown까지 처리되지 않았다. 이 때문에 다음 pipe의 admission과 WRITABLE 회복이 정지했다.
종료 중인 attached pipe가 남아 있는 동안 기존 executor를 유지하여 ACK 왕복을 완료시킨다.
진단은 `overlap-before.log`, `overlap-stacks-before.log`에 보존했다.

idle-stop 적용 후 hotpath 시작 단계에서 드러난 별도 진행 결함도 같은 소유 경계에서 수정했다.
`end_public_part_receive_delivery_hold()`는 어떤 pipe도 hold하지 않은 빈 receive의 rollback에도
progress epoch를 증가시켰다. 디버거에서 wait의 observed epoch=11, 실제 epoch=12와
`rollback → end_public_part_receive_delivery_hold → notify_receive_progress_locked` 호출을 확인했다.
blocking receive가 자기 알림 때문에 계속 대기를 건너뛰어 pending bind 처리를 지연시켰다.
실제 held pipe를 해제하는 기존 분기 안으로 알림을 옮겼다. 증거는 `dd-wait-gdb.log`,
`dd-notify-gdb.log`다. 임시 source logging은 남기지 않았다.

## 검증

로그의 공통 위치는 `/home/hep7/project/zlink-core-a/core/build-dev/reject-2-validation/`이다.
최종 Core 빌드는 지정된 `nice -n 10 env JOBS=4 scripts/build-core.sh dev`로 성공했다
(`build-delivery.log`). 제출 상태에서 새 executable의 7/7 통과도 확인했다 (`contract-delivery.log`).

| 검사 | 최종 결과 |
|---|---|
| `test_router_reject_duplicate` ×20 | **20/20 실행, 140/140 case PASS**, 114.11초. `contract-20-final.log`. |
| 전체 `ctest --test-dir core/build-dev --output-on-failure -j2` | **166/171 PASS, 5 FAIL**, 485.95초. `full-ctest-final.log`. |
| `test_router_reciprocal_handover_lanes` | **16/16 case PASS**, 전체 CTest에서 26.03초. 기존 standby lane 유지와 REQUEST 종결 검증 포함. |
| `^test_disconnect_progress_` | **24/24 PASS**, 최종 전체 CTest에 포함. |
| `test_socket_disconnect_progress_without_app_poll` | 앞의 progress 8 case는 각각 20 sample PASS. 이후 TCP immediate case가 `:509`에서 옛 timeout assertion으로 실패하여 전체 executable은 FAIL. |
| 원래 inproc immediate overlap 단독 반복 | 원본 test body를 byte 동일하게 유지하고 main의 case 선택만 제한한 executable을 최종 archive에 링크했다. 반복은 **5 PASS 후 6회째 `:509` FAIL**에서 중단했다. `:473` WRITABLE 정지는 재현되지 않았다. **20/20 green으로 세지 않는다.** `overlap-original-20-final.log`. |
| `test_ctx_destroy` | **PASS**, 0.38초. monitor owner의 idle detach 동기화 case 포함. |
| `^test_single_lane_` ×2 | **28/29 PASS / 28/29 PASS**, 19.40초 / 63.91초. 동일한 기존 무응답 assertion 실패. `single-lane-final-{1,2}.log`. |
| C++ binding | **contract 16/16 + samples 7/7 PASS**. `cpp-build-final.log`, `cpp-contract-final.log`, `cpp-sample-smoke-final.log`. |
| Python binding | **190 tests + 4 subtests + samples 7/7 PASS**. `python-gate-final.log`. |
| raw header mirror / whitespace | **12/12 일치**, `git diff --check` PASS. `static-checks-final.log`. |

새 테스트 이름과 반복 범위는 다음과 같다.

| case | 검증 |
|---|---|
| `test_rejected_pending_request_inproc` | IO_THREADS=0에서 서버의 duplicate admission 전에 REQUEST를 제출한다. 거부된 pending은 timeout 1000ms에 비해 충분히 짧은 100ms 이내 NOT_CONNECTED, 이후 원래 timeout까지 중복 없음. ×20 PASS. |
| `test_reject_disconnect_reconnect_tcp`, `_inproc` | disconnect의 terminal 관찰 후 connect, READY 뒤 REQUEST. 거부되면 100ms 이내 NOT_CONNECTED를 받고 다음 READY에서 새 REQUEST 성공. 각 ×20 PASS. TCP에서는 20회 모두 실제 거부 분기를 관측했다. |
| `test_reject_duplicate_close_tcp`, `_inproc` | 기존 RID 소유자는 계속 사용 가능하고 duplicate는 닫힘. 기존 소유자 disconnect 후 duplicate의 자동 next attempt가 서버에서 admission되고 REQUEST 성공. 각 ×20 PASS. |
| `test_transient_request_tcp`, `_inproc` | pending REQUEST와 읽지 않은 DATA, SEND/REQUEST WRITABLE 토큰 두 개를 둔 뒤 서버 close. DATA drain 전에 NOT_CONNECTED, 재연결 뒤 기존 두 토큰 각각 한 번, 새 REQUEST 성공, 원래 timeout 뒤 중복 없음. 각 ×20 PASS. |

TCP rejected REQUEST 20개, 결정적 inproc rejected REQUEST 20개, transient TCP/inproc 각 20개의
완료 대기 측정값은 모두 정수 ms로 0이었다. inproc의 결정적 거부 pending 검증과 자동 재admission
검증은 위의 **별도 case**다. 한 inproc fixture에서 두 단계를 연속 강제한 20회로 보고하지 않는다.

C++는 round 1의 `core/build-dev/cpp-gate`를 다시 빌드했다. Python은 round 1의
`core/build-dev/reject-python-gate-repo` 복사본을 사용했고 tracked Python source 148개가
worktree 원본과 byte 동일함을 확인했다. 두 언어 모두 최종
`/home/hep7/project/zlink-core-a/core/build-dev/lib/libzlink.so.0.17.0`을 사용했다.
Python 실행은 `ZLINK_CORE_SOURCE=local`, 위 library 경로, 기존 `gate-venv/bin/python`,
`PYTHONDONTWRITEBYTECODE=1`, `run_tests.sh -p no:cacheprovider` 조합이다.

## Hotpath — dev 수치, release 판정 n/a

최종 코드의 별도 순차 측정은 `hotpath-final.log`에 있다. 단위는 instructions/message이며
REQUEST cell은 instructions/request다. HEAD dev 값은 round 1 보고서에 남긴 동일 HEAD 측정이다.

| cell | 반복 수 | 저장 reference | HEAD dev | round 2 dev |
|---|---:|---:|---:|---:|
| dealer_dealer_inproc | 20,000 | 3455.3810 | 4476.094250 | 4476.245600 |
| dealer_router_reqrep_inproc | 5,000 | 12054.8948 | 15146.265200 | 15139.728200 |
| pair_inproc | 20,000 | 2681.9566 | 3531.221800 | 3531.214000 |
| router_router_tcp | 20,000 | 2972.8817 | 3855.916150 | 3855.822150 |

HEAD dev 대비 변화는 약 -0.044%~+0.004%다. 저장 reference 대비 비율은 각각
1.2954 / 1.2559 / 1.3167 / 1.2970으로 **4/4 FAIL**이다. 최종 전체 CTest에서도 네 cell을 모두
측정했으며 동일하게 reference gate가 실패했다. dev 측정으로 release 성능을 판정하지 않는다.
Release/LTO 빌드와 reference 갱신은 하지 않았다.

## BLOCKERS

1. **기존 assertion이 D-090과 충돌하여 기능 gate가 green이 아니다.** 구현에 맞추기 위한
   assertion 변경과 원래 overlap test 변경을 금지한 작업 범위를 지켜 아래 assertion은 그대로 두었다.
   감독이 새 spec에 맞춘 테스트 변경 범위를 확정해야 한다.

   | 파일:line | 남은 충돌 |
   |---|---|
   | `core/tests/integration/test_socket_disconnect_progress_without_app_poll.cpp:509` | 거부된 pipe의 REQUEST에 TIMED_OUT(101)을 기대하지만 NOT_CONNECTED(109)가 도착한다. TCP 전체 실행과 inproc 원본 단독 반복에서 확인했다. |
   | `core/tests/integration/test_phase3_request_reply_contract.cpp:1086`, `:1102` | `test_admitted_request_survives_physical_detach_and_same_rid_reconnect_without_replay`가 detach 후 무응답과 원래 timeout을 기대한다. 최종 실행은 helper `:222`에서 NO_DATA(201) 대신 completion 수신 OK(0)로 실패했다. |
   | `core/tests/integration/test_phase3_request_reply_contract.cpp:2818` | responder physical close 후 남은 pinned pending에 NOT_FOUND(102)를 기대하지만 NOT_CONNECTED(109)가 도착한다. 명시적 logical RID 제거 검사는 통과했다. |
   | `core/tests/integration/test_zmp_metadata.cpp:2066` | invalid completion kind로 pipe가 닫힌 뒤 pending에 TIMED_OUT을 기대하지만 NOT_CONNECTED가 도착한다. |
   | `core/tests/integration/test_dealer_router_single_lane_contract.cpp:3664` | 이전 pair가 끊어진 뒤 `assert_no_completion()`을 요구한다. helper `:657`에서 payload 없는 REQUEST NOT_CONNECTED를 관측하여 실패한다. |

   전체 CTest의 60초/180초/45초 timeout 세 개는 해당 assertion 실패 및 강제 fixture cleanup
   메시지 이후 발생했다. 이를 통과로 처리하거나 timeout을 늘리지 않았다.

2. **요청한 모든 검증이 green인 상태는 아니다.** 원래 inproc overlap의 WRITABLE 정지는 해소됐지만,
   원본 테스트 20/20 성공은 위 계약 충돌 때문에 확인하지 못했다. D-088 inproc의 거부 pending과
   자동 재admission은 각각 20회 검증했으며, 두 단계를 강제한 단일 fixture의 20/20은 남아 있다.

3. **Hotpath release gate는 n/a이고 dev reference gate는 FAIL이다.** 수치와 원인을 위에 구분했다.
   reference를 변경하거나 release green을 주장하지 않는다.

구현 diff와 새 테스트는 worktree에 미커밋 상태로 남겼다.
