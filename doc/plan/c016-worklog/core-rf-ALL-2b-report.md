# core-rf-ALL-2b 보고서 — review-ALL-2 차단·비차단 항목 수정

- 대상 worktree: `/home/hep7hep7/project/zlink-work/all`, branch `wip/0.17.3-all2`, HEAD `1a79625d3d` 위 **미커밋** 변경.
- 누적 patch: `/home/hep7hep7/project/zlink-work/all-artifacts/ALL-2b.patch` (HEAD 대비, untracked 포함 `git add -N` 후 생성).
- 변경 규모: 23 파일. `git diff --stat -- core/include core/src/libzlink.vers` = 비어 있음(공개 헤더·ABI export 무변경).
- 이 job(ALL-2c)은 ALL-2b codex worker가 sanitizer 검증 직전에 외부 필터로 중단된 뒤 **이어받아** diff 검토·잔여 수정·전 검증·보고서를 수행했다.

## 1. 리뷰 항목별 수정

| 항목 | 판정 | 수정 내용과 파일:행 | 검증 |
|---|---|---|---|
| **B-ALL2-1** complete-record의 max 사전검증이 쓰기 전에 무효화 | 수정 | record admission 소유자가 **하나의 max 스냅샷**을 잡고 commit까지 그 값만 쓴다. `core/src/runtime/core/pipe_write.cpp:888`(사전검사에 `&max_message_bytes` 전달) → `:896`(part별 `write_message_unlocked(..., &max_message_bytes)`) → `:330`(스냅샷 우선 사용) → `:1188`·`:1203`(part별 HWM 재검사도 같은 스냅샷). 시그니처는 `core/src/runtime/core/pipe.hpp:637`, `:672`, `:678`의 optional `const uint64_t *` 인자로 확장(기본 NULL = 기존 acquire load). `set_max_message_bytes`의 release store는 그대로 두고 주석만 실제 규칙으로 교체(`pipe_write.cpp:1128-1133`). assertion 완화·caller retry·budget 증가 없음 | 신규 결정 테스트 2개: `core/tests/unittest/unittest_complete_record_admission.cpp:63`(commit hook), `:896`/`:898` 등록. PAIR는 정상 commit, DEALER는 정상 오류(`ZLINK_SUBMIT_INVALID_ARGUMENT`/`EMSGSIZE`)이며 prefix 미노출 |
| **W-ALL2-1** 무제한 command batch·종료 취소 없음 | 수정 | batch를 **기존 상수** `inbound_poll_rate`로 상한하고 `_ctx_terminated`를 batch 안에서 검사해 turn을 양보한다. `core/src/runtime/sockets/common/socket_base_lifecycle.cpp:506`, `:526-532`. 새 상수·옵션 없음 | `core/tests/unittest/unittest_receive_transaction.cpp:1146` — `inbound_poll_rate+1`개를 넣고 1차 drain이 정확히 `inbound_poll_rate`개, 2차가 나머지를 처리 |
| **W-ALL2-2** inproc 종료가 자기 turn을 쥔 채 상대 socket turn 획득·ack 대기 | 수정 | turn 안에서는 로컬 pipe 종료만 시작(`begin_inproc_pipe_termination`, `socket_base_endpoint.cpp:964`), 상대 socket executor 기동과 ack 대기는 turn을 놓은 뒤(`finish_inproc_endpoint_termination`, `:991`; 호출은 `term_endpoint()` `:1160-1168`). `socket_inprocs_t::erase_pipes`도 같은 2단계로 변경(`socket_endpoint_runtime.cpp:101`, `socket_runtime.hpp:138`). command 경로(`process_term_endpoint`, `socket_base_lifecycle.cpp:1436`)는 network endpoint만 도달하므로 두 vector가 비어 있음을 단정 | `core/tests/integration/test_endpoint_release.cpp:302` binder/connector 동시 unbind·disconnect |
| ↳ **ALL-2c 추가 수정** | 수정 | 위 분리가 peer의 `set_nodelay()`를 `terminate()` **뒤**로 밀어 `pipe_t::_delay`가 peer 소유자의 `process_pipe_term_ack()`와 경쟁했다(TSan: `pipe_transport.cpp:289` 읽기 vs `:383` 쓰기, `test_inproc_unbind_disconnected_and_not_found`). 원래 순서대로 종료 시작 **전**에 발행하도록 되돌림(`socket_base_endpoint.cpp:976-982`) | TSan `test_endpoint_release` 경고 0·통과(수정 전 경고 1 + 종료 지연 94~196 ms) |
| **W-ALL2-3** ws batch/scratch 128 KiB 고정 확대 | 수정 | 초기값을 16 KiB(encoder batch)/64 KiB(client masking scratch)로 되돌리고, 상한 128 KiB까지 **기존 encoder write target 성장 정책** 안에서 성장·회수한다. `core/src/runtime/transports/ws/ws_batch_policy.hpp:13`(초기)·`:18`(상한)·`:23`(scratch 초기), `ws_transport_common_internal.hpp:41`·`:48`(payload에 따른 scratch 선택), 적용점 `:223`·`:255`·`:288`. 성장·회수 규칙은 `asio_stream_fastpath_policy.hpp:413-460`의 기존 함수를 message-boundary transport로 확장(연속 full batch 2회 성장, 절반 미만이면 초기값으로 회수), engine 배선은 `asio_engine.cpp:170`·`:825`·`:844`·`:1110`·`:1436`·`:1469` | `unittest_asio_write_turn_policy.cpp:166`(성장·회수), `unittest_ws_transport_config.cpp:203`(초기/상한 값) |
| **W-ALL2-4** ws 비율 게이트가 복수 report의 비교 조건을 검사하지 않음 | 수정 | report가 2개 이상이면 `os·cpu·cores·build·core_revision·timestamp·load_avg·runs·clients` 9개 META를 **필수**로 요구하고 값이 서로 다르면 오류로 처리한다. `bindings/c/perf/ws_roundtrip_gate.py:23`(필드), `:50`(파서), `:124-144`(교차 검사). 실제 러너 report는 이 9개 META를 모두 내보내며(run 단위 값이므로 같은 실행이 나눠 쓴 report는 통과, 다른 실행끼리는 거부) | `bindings/c/perf/tests/test_ws_roundtrip_gate.py:135` 신규 + 기존 11개, 12/12 통과 |
| **W-ALL2-5/6** 서술 범위 | 문서 | 아래 §4 "증거의 한계"에 그대로 기록했다. 무동작변경 주장·원자 연산 수·성능 인과 주장을 확대하지 않는다 | — |
| **S-ALL2-1** 스펙 문장 초안 | 문서 | 스펙 파일은 수정하지 않았다. §3 초안 표 참조 | — |
| **S-ALL2-2** 이전 설계 주석 잔존 | 수정 | `socket_lifecycle_runtime.cpp:32`(“API 전체 동안 sync 소유” → turn 단위), `pipe_transport.cpp:58`(별도 동기화 domain → socket=lifecycle turn / session=I/O owner) 및 `:72`(“relaxed-ordered load” → acquire load, ALL-2c 추가), `pair.cpp:98`(“under the pipe lock” → owner turn), `ctx.cpp:219`(registry lock 안 monitor teardown → snapshot 후 외부 stop), `socket_base.hpp:1031`(receive sync → lifecycle turn), `pipe.hpp:747`(`_in_active`의 두 소유자 설명) | 빌드·전체 ctest |

### 설계 비교(선택 이유)

| 항목 | 대안 | 선택 |
|---|---|---|
| B-ALL2-1 | (a) max 변경을 socket turn 아래 command로 적용 (b) admission 소유자가 스냅샷을 잡고 record commit까지 재사용 | **(b)**. (a)는 I/O thread handshake에 새 command·소유권 왕복을 추가하고 turn 경합을 늘린다. (b)는 새 상태·플래그·규칙을 0개 추가하고 “사전검증에 쓴 정책과 그 검증에 의존한 commit은 같은 유효성 경계”라는 §5 규칙을 코드로 표현한다. 새 record는 여전히 최신 max를 관측한다 |
| W-ALL2-1 | (a) 새 batch budget 옵션 (b) 기존 `inbound_poll_rate` 재사용 | **(b)**. 새 상수·옵션 금지 규칙과 “규칙 수 줄이기”에 맞고, 이미 mailbox poll 주기를 뜻하는 값이다 |
| W-ALL2-2 | (a) 상대 socket 작업을 command로 위임 (b) turn 밖에서 수행 | **(b)**. (a)는 새 command 종류와 완료 통지 규칙을 추가한다. (b)는 기존 호출을 그대로 두고 **호출 위치만** turn 밖으로 옮겨 §3.4/§6(무소유 대기·foreign turn 금지)를 만족한다 |
| W-ALL2-3 | (a) 128 KiB 고정 (b) 초기 작게 + 기존 성장 정책 재사용 | **(b)**. 연결쌍당 +288 KiB 고정 증가를 없애고, STREAM용으로 이미 있는 write target 성장/회수 규칙 하나를 message-boundary transport로 넓힌다(새 정책 0개) |
| W-ALL2-4 | (a) 비교 셀을 한 파일로 제한 (b) provenance META 교차 검사 | **(b)**. CLI의 복수 report 수용을 유지하면서 “같은 host/load/run”만 통과시킨다. 러너가 run 단위로 찍는 META를 그대로 쓰므로 새 형식·필드를 만들지 않는다 |

## 2. 검증

빌드: dev(RelWithDebInfo, LTO OFF), TSan(GCC `-fsanitize=thread -fno-omit-frame-pointer`, LTO OFF, suppression 없음, `setarch x86_64 -R`), ASan(`ENABLE_ASAN=ON`), Release(LTO ON) lib + `hotpath_bench`.

| 검증 | 결과 |
|---|---|
| dev 전체 `ctest -E hotpath_gate` 1회 | **210/210 통과**, 244.95 s |
| dev 관련 정규식(`pipe\|pair\|dealer\|router\|hwm\|max\|flow\|ctx\|term\|inproc\|ws\|wss\|endpoint`) | **70/70 통과** |
| dev 신규·변경 5 target `--repeat until-fail:20` | **5/5 통과** |
| TSan 신규 5 target + `unittest_phase3_request_reply_owners` + `test_stream_*` (24 테스트) | **23/24**, 경고 0. 실패는 `test_stream_packet_progress` 1건 |
| TSan 전체 `ctest -E hotpath_gate` 1회 | **209/210**, 446.93 s, **ThreadSanitizer 경고 0건**. 실패는 위와 동일 |
| ASan 신규·변경 테스트 | **6/6 통과**, AddressSanitizer 오류 0건 (`unittest_complete_record_admission`·`unittest_receive_transaction`·`test_endpoint_release`·`unittest_asio_write_turn_policy`·`unittest_ws_transport_config`·`unittest_phase3_request_reply_owners`) |
| ws 비율 게이트 python 테스트 | **12/12 통과** |
| Release lib(LTO ON) + hotpath 5셀 | 아래 표 |
| ws 비율 게이트 실측 1회 | **PASS** — ws Q64/Q1 = 0.899753, wss Q64/Q1 = 1.456018 (기준 ≥ 0.80), reports=1, errors=0 |
| `git diff --stat -- core/include core/src/libzlink.vers` | 비어 있음 |

TSan 실패 1건은 `core/tests/integration/test_stream_packet_progress.cpp:84 test_shutdown_during_drain`의 “transport did not queue all fragments”이며, 262,153개 1-byte WS fragment를 제한 시간 안에 적재한다는 **fixture 처리량 가정**이 TSan 계측 아래에서 성립하지 않는 것이다. TSan 경고는 0건이고 dev에서는 통과한다. ALL-2 보고서가 기록한 것과 같은 실패이며 이번 patch가 새로 만든 것이 아니다.

### hotpath 5셀 (Release, LTO ON, callgrind Ir/msg)

| hotpath 셀 | reference Ir/msg | 측정 Ir/msg | 변화 | 도구 판정 |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3230.922 | 3104.855 | −3.90% | PASS |
| dealer_router_reqrep_inproc | 16455.383 | 15635.504 | −4.98% | PASS |
| pair_inproc | 2348.457 | 2456.727 | +4.61% | PASS |
| router_router_tcp | 2972.532 | 3041.963 | +2.34% | PASS |
| stream_tcp | 13969.806 | 13982.501 | +0.09% | PASS |

도구 자체 판정이 **5/5 PASS**다(ALL-1에서 개선 셀을 FAIL로 표기하던 대칭 게이트 문제가 이번 수치에서는 발생하지 않았다). ALL-1 최종 측정(3101.018 / 15504.176 / 2432.400 / 3073.674 / 13907.594) 대비 차이는 −0.4%~+0.9% 범위이며, W-ALL2-1의 batch 상한과 B-ALL2-1의 스냅샷 전달이 hot path Ir을 늘리지 않았다는 뜻이다. 측정은 `flock PERF_LOCK` 아래 1회, 시작 load average 4.41(직전 빌드 잔여, callgrind Ir 계수는 부하에 비민감하다). 로그 `hotpath.log`.

### ws 비율 게이트

`flock PERF_LOCK` 아래 1회 측정(88 s, 시작 load average 0.29, ninja 0). 러너: `bindings/c/perf/run_benchmarks_multi.sh --reuse-build --duration 5 --clients 100 --runs 1 --pattern DEALER_DEALER,DEALER_ROUTER_SENDSEND --transports tcp,ws,wss --msg-sizes 1024,65536`. report `all2b-wsgate/multi/report/perf_c_multi_linux_20260908_162837_ALL-2b-wsgate.txt`(status=complete), 게이트 로그 `all2b-ws-ratio-gate.log`.

| Size | Q(ws) | Q(wss) |
|---:|---:|---:|
| 1024 | 0.386078 | 0.402603 |
| 65536 | 0.347375 | 0.586196 |

- ws Q64/Q1 = **0.899753** ≥ 0.80 → PASS
- wss Q64/Q1 = **1.456018** ≥ 0.80 → PASS
- `Final: PASS (reports=1, errors=0)`

한계: 게이트는 **크기에 따른 추가 손실 비율**만 판정한다. Q 절대값은 ALL-1의 최종 실행(WS 1.851217 / WSS 2.585461)보다 낮은데, 이번 실행은 batch 초기값을 16 KiB로 되돌린 코드이고 분모인 TCP 대조값도 다른 실행이다. 두 실행의 절대 처리량을 직접 비교하지 않았고, WS 왕복 절대 성능이 개선/악화됐다는 결론을 여기서 내리지 않는다. runs=1의 변동 한계도 그대로다.

## 3. S-ALL2-1 — 스펙 문장 초안(스펙 파일 미수정)

| 절 | 초안 |
|---|---|
| synchronization11 §2/§5 | “record 사전검증에 사용한 정책 값과 그 검증에 의존하는 commit은 같은 유효성 경계에서 처리한다. 검증 이후 발행된 정책 변경은 다음 record부터 적용된다.” — B-ALL2-1 구현이 이 문장 그대로다 |
| §3.1 | “socket의 scheduler·route·receive 상태를 조회하거나 바꾸는 readiness, completion drain, attach/termination도 같은 turn을 사용한다. 하나의 수신 record를 구성하는 내부 처리 동안 소유권을 유지하며, 이미 소유한 내부 호출은 caller의 turn을 빌린다.” |
| §3.3 | “mailbox의 비동기 실행자 설치·해제는 command 소비 주체를 정할 뿐 socket C2 상태의 배타 방식을 바꾸지 않는다.” |
| §3.4 | “대기 직전에는 가장 바깥 소유 scope도 turn을 해제해야 한다. 내부 함수가 빌린 scope를 끝낸 것만으로는 turn 해제가 아니다.” — W-ALL2-2 수정으로 inproc endpoint 종료가 이 문장을 만족한다 |
| §6 | “등록과 알림 발행 양쪽은 필요한 store→load 순서를 보장하는 seq_cst 연산 또는 동등하게 증명된 fence 조합을 사용한다.” / foreign socket turn 금지 문장은 완화하지 않는다 |
| ZMP §8/§9 | 계약 수정 불필요. WS batch는 현재 확보한 bytes에 대한 유한 batch이며 초기 16 KiB·상한 128 KiB로 되돌아갔다 |

어느 문장도 다른 동작이 되도록 바꾸지 않았다. 공개 반환 계약(completion·READY/DISCONNECTED·POLLIN/POLLOUT level·WRITABLE wake)의 순서·조건은 그대로다.

## 4. 남은 위험과 증거의 한계

1. **W-ALL2-2의 잔여 범위.** turn 밖으로 옮긴 것은 `term_endpoint()` 공개 경로다. network bound endpoint의 `object.cpp` future wait(`socket_base_endpoint.cpp:1112` 계열)은 이번 범위에서 건드리지 않았다. inproc 교차 교착 반례는 닫혔지만 §3.4 전면 준수를 주장하지 않는다.
2. **`process_term_endpoint`의 단정.** command 경로에서 두 vector가 비어 있다는 전제는 “session은 network transport에만 존재한다”는 사실에 의존한다(`core/src/runtime/core/session_base.cpp:750`이 유일한 producer). 이 전제가 깨지면 `zlink_assert`가 Release에서도 abort한다.
3. **W-ALL2-3의 메모리 수용성.** 고정 증가는 없앴지만 큰 payload가 지속되면 연결당 128 KiB까지 올라간다. 1,000 연결쌍 RSS 실측은 이번에도 하지 않았다. WS-1의 connection 수 100/1,000/10,000 수용성은 여전히 미완이다.
4. **W-ALL2-4의 강도.** `timestamp`·`load_avg`까지 일치를 요구하므로 사실상 **같은 실행이 나눠 쓴 report만** 복수 입력으로 통과한다. 서로 다른 실행 2개를 합치는 사용은 이제 거부된다. 이것은 리뷰가 지적한 위험의 보수적 해석이며, 단일 report 판정에는 영향이 없다.
5. **W-ALL2-5.** ALL-1의 196/196 body SHA 일치는 분할 직전/직후의 증거이며, 이번 최종 patch 전체가 무동작변경이라는 뜻이 아니다. 이번 job에서도 body 비교 도구를 새로 돌리지 않았다.
6. **W-ALL2-6.** turn 1회의 CAS 1 + RMW 1은 turn 자체 비용이며 public recv 전체 원자 연산 수가 2개라는 뜻이 아니다. `recv_ticks` RMW는 별도다. 아래 hotpath 수치도 Ir 기준이며 pthread lock 횟수나 처리량의 인과를 대신하지 않는다.
7. **플랫폼.** Windows·big-endian 실행 검증은 하지 않았다. 새로 추가한 코드에 POSIX 전용 API는 없다.
8. **측정 조건.** 아래 성능 표는 각 1회 측정이다. 게이트를 통과할 때까지 반복하지 않았다.

## 5. 변경 분류

- B-ALL2-1: **B(기존 결함 수정)** — ALL-2 patch가 만든 논리 race를 admission 소유 경계에서 닫았다.
- W-ALL2-1/2 및 `set_nodelay` 순서 복원: **B**.
- W-ALL2-3/4: **B**(고정 메모리 증가 회수, 게이트 판정 조건 보강).
- S-ALL2-2: 주석 정리(동작 무변경).
- 계약 변경(D) 없음. 공개 인터페이스·ABI 변경 없음.

규칙 수: 새 상수·옵션·플래그·command 종류 **0개 추가**. `max` 스냅샷은 기존 admission 인자에 optional 포인터 1개, batch 상한은 기존 `inbound_poll_rate` 재사용, WS 성장 정책은 기존 STREAM write-target 함수 확장이다.
