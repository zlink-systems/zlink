# MERGE-1 — 0.17.4 릴리스 브랜치 조립·최종 게이트 보고

- 일자: 2026-09-08. 작업 worktree `/home/hep7hep7/project/zlink-work/rel174`, 브랜치 `wip/0.17.4`(push 완료).
- 시작 17:20 / 종료 21:25(4h 상한). main에는 커밋하지 않았고 tag도 만들지 않았다. 스펙·공개 인터페이스 변경 없음.
- Windows clone `D:\project\zlink-rel174`(wip/0.17.4). 로그: `/tmp/claude-1000/.../scratchpad/*.log`.
- **최종 판정: 미완. Linux 정확성 게이트는 전부 통과했으나 Windows 5개 target 실패와 WS 비율 gate FAIL이 남는다.**

## 1. 브랜치 커밋

| 순서 | 커밋 | 내용 |
|---|---|---|
| 1 | `50ae7ffb42` | ALL-2 누적(base `1a79625d3d` 재적용, rebase 후 해시) |
| 2 | `45a389ff7c` | ALL-2b 리뷰 수정 23파일(WS batch 성장·회수, endpoint release, asio write turn, receive transaction) |
| 3 | `fd1a055e31` | ALL-3 자체 delta 22 수정 + 신규 3(allocator payload storage, C relay completion 진행, Windows 이식성, latency_rss_gate.py) |
| 4 | `c414d95d52` | RR-1 REQ/REP part-count 경계 테스트 |
| 5 | `84d25131a6` | MAC-1+MAC-2 macOS 테스트·타임아웃 수정(mkdtemp 헤더 탐색, scale_test_timeouts.cmake) |

`origin/main`(`514906a682`) 위로 rebase 성공. 최종 head `84d25131a6`.

## 2. 충돌과 해결

| 위치 | 충돌 | 해결 |
|---|---|---|
| `core/tests/CMakeLists.txt` (MAC-2 적용 시) | RR-1의 `test_request_reply_part_count_boundary` TIMEOUT 300 블록 vs MAC-2의 `scale_test_timeouts.cmake` include | **둘 다 유지**. RR-1 블록을 먼저 두고 scale include를 파일 마지막에 두어 RR-1 deadline도 스케일 대상이 되게 했다(주석 "Applied last so that every deadline set above is scaled." 의도 준수). |
| ALL-2b(WS batch 성장·회수) vs ALL-3(allocator payload storage) | 예상했던 겹침 | 실제 충돌 없음. ALL-3는 `ws_batch_policy.hpp`를 건드리지 않는다. ALL-2b의 16 KiB 기본+성장 정책과 ALL-3의 payload storage 모두 그대로 남았다. |
| `unittest_complete_record_admission.cpp` | ALL-2b +140행 / ALL-3 5행 수정 | `git apply --3way`가 자동 병합, 양쪽 유지. |
| `test_dealer_router_single_lane_contract.cpp` | ALL-3 8행 / MAC-2 31행 | 자동 병합, 양쪽 유지. |
| rebase(origin/main) | LIN-1 테스트 수정·0.17.3 bump | **충돌 0**. main 쪽 `unittest_flow_state_monitor.cpp`·버전 파일이 그대로 유지됨을 diff 0으로 확인. |

## 3. Linux 게이트

| 검증 | 결과 | 수치 |
|---|---|---|
| `JOBS=4 scripts/build-core.sh dev` | PASS | 오류 0 |
| 전체 `ctest -E hotpath_gate` 1회 | **211/211 PASS** | 234.10 s, 재실행 필요한 실패 0 |
| 변경 suite(`stream\|pipe\|wake\|hwm\|flow\|credit\|poll\|completion\|router\|dealer\|pair\|sub\|monitor\|ws\|ctx\|term\|decoder\|zmp\|request\|reply`) `until-fail:3` | **147/147 PASS** | 529.11 s |
| 다섯 패치 신규·변경 11 target `until-fail:10` | **41/41 PASS** | 537.75 s |
| wake(lost-wake 포함) 5 target `until-fail:20` | **5/5 PASS** | 641.24 s |
| TSan 전체 ctest 1회 (GCC `-fsanitize=thread -fno-omit-frame-pointer`, LTO OFF, `setarch x86_64 -R`, suppression 없음) | **211/211 PASS, ThreadSanitizer 경고 0** | 405.69 s |
| ASan 신규·변경 target | **41/41 PASS** | 54.93 s |
| `git diff --stat origin/main -- core/include core/src/libzlink.vers` | **비어 있음** | 변경 0 |
| C 헤더 mirror (`check_c_header_mirror.py`) | PASS | core/include 8파일 ↔ bindings/c/include 8/8 동일 |
| 전체 바인딩 헤더 mirror(c·cpp·go·rust, 8×4=32) | 31 동일 / **1 불일치** | `bindings/rust/include/zlink/common.h`가 `ZLINK_VERSION_PATCH 2`. **origin/main에도 동일하게 존재하는 기존 결함**(`4cdafee9b7`이 `zlink.h`만 고쳤다). MERGE-1이 만든 것이 아니며 이번 브랜치에서 고치지 않았다. |

## 4. hotpath 5셀 (release-gate, idle, PERF_LOCK, load1 0.44)

| Cell | Reference Ir/msg | Measured | Ratio | ±5% |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3230.922 | 3104.854 | 0.9610 | PASS |
| dealer_router_reqrep_inproc | 16455.383 | 15623.436 | 0.9494 | FAIL(하한) — **개선 방향**(−5.06%) |
| pair_inproc | 2348.457 | 2457.258 | 1.0463 | PASS |
| router_router_tcp | 2972.532 | 3045.919 | 1.0247 | PASS |
| stream_tcp | 13969.806 | 13997.867 | 1.0020 | PASS |

밴드를 벗어난 한 셀은 명령 수가 기준보다 **작은** 쪽(개선)이다. reference JSON은 수정하지 않았다. ALL-3에서 하한 FAIL이던 dealer_dealer(0.9440)는 이번에 0.9610으로 밴드 안에 들어왔다.

## 5. with_stream 6 스택 (CCU 1000, runs 3, `ZLINK_CORE_SOURCE=local`)

측정 lib는 worktree의 `core/build/lib/libzlink.so.0.17.3`(release-gate)로 확인했다. **첫 실행은 러너 기본값 때문에 `~/.cache/zlink/core/0.17.3` 릴리스 lib를 썼으므로 폐기하고 재측정했다.**

| Size | stack | median kops | mean(ms) | mismatch | srv RSS(MiB) |
|---|---|---:|---:|---:|---:|
| 64 | zlink | 279.12 | 1.79 | 0 | 45.25 |
| 64 | asio | 350.06 | 1.43 | 0 | 19.88 |
| 64 | asio_pull | 192.38 | 2.60 | 0 | 19.88 |
| 1024 | zlink | 139.99 | 3.57 | 0 | 45.38 |
| 1024 | asio | 177.56 | 2.81 | 0 | 21.50 |
| 1024 | asio_pull | 160.67 | 3.11 | 0 | 22.00 |
| 65536 | zlink | 27.88 | 17.81 | 0 | 273.10 |
| 65536 | asio | 34.80 | 14.30 | 0 | 159.88 |
| 65536 | asio_pull | 20.08 | 24.72 | 0 | 187.50 |
| 전 사이즈 | cppserver / cppserver_pull / zmq | **측정 불가** | — | — | — |

zlink/asio 비율: 64 B 0.797, 1024 B 0.788, 64 KiB 0.801. ALL-3의 1-run(0.812/0.822/0.764)과 같은 수준이다.

**요청한 6 스택 중 3 스택을 측정하지 못했다.**
- `cppserver`, `cppserver_pull`: `stacks/cppserver/upstream`의 CppServer 상위 프로젝트가 이 worktree에 없다. `ccu1` worktree에서 복사해도 그 안의 중첩 CMake 모듈(`SetCompilerFeatures`, `SetCompilerWarnings`)이 없어 configure 실패.
- `zmq`: `stacks/zmq/libzmq_dist/linux-x64`에 헤더만 있고 `lib/libzmq.so`가 없어 `test_scenario_stream_zmq` target 자체가 생성되지 않는다(`ccu1`도 동일).

## 6. perf/c multi (tcp, 1024 B, runs 3, `ZLINK_CORE_SOURCE=local`, status=complete 4/4)

| Pattern | throughput ops/s | bandwidth MiB/s | mean ms | p95 ms | p99 ms |
|---|---:|---:|---:|---:|---:|
| MULTI_DEALER_DEALER | 991522.2 | 1015.319 | 1.151029 | 2.594417 | 5.205997 |
| MULTI_DEALER_ROUTER_SENDSEND | 327462.8 | 670.644 | 0.991901 | 2.203403 | 2.881554 |
| MULTI_ROUTER_ROUTER_SENDSEND | 266946.2 | 546.706 | 0.771313 | 1.455815 | 2.059276 |
| MULTI_DEALER_ROUTER_REQREP | 216996.0 | 444.408 | 0.727842 | 1.289209 | 1.716011 |

원자료 `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260908_191548.txt`(4 성공 / 0 실패, 119 s).

## 7. WS 비율 gate / latency·RSS gate

WS gate 입력은 한 report 안에 3 패턴 × tcp/ws/wss × 1024/65536이 모두 있어야 해서 전용 1-run(18셀, status=complete)을 돌렸다. 원자료 `perf_c_multi_linux_20260908_192744.txt`.

| Size | Q(ws) | Q(wss) |
|---:|---:|---:|
| 1024 | 0.566545 | 0.370218 |
| 65536 | 0.302540 | 0.467128 |

| gate | 결과 |
|---|---|
| ws `Q64/Q1 >= 0.80` | **FAIL** — 0.534009 |
| wss `Q64/Q1 >= 0.80` | PASS — 1.261766 |

ALL-3의 clean 재측정(ws 0.455315 FAIL / wss 1.011529 PASS)과 같은 실패 방향이다. 지시대로 고치지 않았다.

**latency/RSS gate(`latency_rss_gate.py`)는 실행하지 못했다.** 이 gate는 `--rss-sidecar` JSON(ALL-3가 별도 150 ms 샘플러로 만든 `final64-rss.json` 형식의 `cells[{pattern,transport,size,server_peak_rss_kib}]`)을 필수로 요구하는데, 그 샘플러 실행을 포함한 64 KiB 100-client 측정을 4 h 상한 안에 넣지 못했다. **미실행이며 PASS로 간주할 수 없다.**

## 8. Windows (호스트 MSVC, `D:\project\zlink-rel174`, CI 구성)

전체 build PASS(shared+static+tests, VS 17 2022 x64 Release, C++17, WITH_TLS=ON, OpenSSL-Win64).

전체 `ctest -C Release` 1회: **211개 중 198 PASS / 13 실패**. 실패 13개만 재실행한 결과 8개가 PASS로 바뀌었다(그 중 6개는 `BAD_COMMAND`/프로세스 미기동으로, WSL 쪽 TSan ctest와 동시 실행된 부하에서 나온 일시적 실패로 본다).

| Target | 전체 실행 | 재실행 | 판정 |
|---|---|---|---|
| test_asio_ws | BAD_COMMAND | PASS | 일시적 |
| test_public_inproc_multipart_send | BAD_COMMAND | PASS | 일시적 |
| test_close_completion_poller_release | BAD_COMMAND | PASS | 일시적 |
| test_zmp_request_reply | BAD_COMMAND | PASS | 일시적 |
| test_zmp_ws_wss | BAD_COMMAND | PASS | 일시적 |
| test_router_same_socket_reconnect_policy | Failed | PASS | 간헐 위험 유지 |
| test_endpoint_release | Timeout | PASS | 간헐 위험 유지 |
| **test_writable_resubmit_from_other_thread_while_sequence_open** | Failed | **Failed** | 남음 |
| **test_writable_resubmit_..._send** | Failed | **Failed** | 남음 |
| **test_wake_invariants** | Failed | **Failed** | 남음 |
| **unittest_flow_state_socket** | Timeout | **Timeout** | 남음 |
| **unittest_mutex** | Failed | **Failed** | 남음 |

남은 5개 상세:

| Target | 증상 | ALL-3 대비 |
|---|---|---|
| test_writable_resubmit_from_other_thread_while_sequence_open (+ `_send`) | exe가 **기동 자체를 못 한다**("애플리케이션 구성이 잘못되었습니다" = SxS/ApplicationFailedException, 0.1 s). 해당 target만 다시 링크해도 동일. 같은 소스의 ALL-3 clone(`D:\project\zlink-all3`) 빌드 바이너리는 정상 통과(2 Tests 0 Failures). exe는 zlink.dll을 import하지 않는 정적 링크이며 dumpbin DEPENDENTS는 표준 CRT/UCRT만 보인다. **원인 미확정 — ALL-3에는 없던 새 실패다.** | **신규 2건** |
| test_wake_invariants | `test_wake_invariants.cpp:1336` `test_multi_dealer_dealer_tcp_large_hwm_drain_wakes_all_pollout` FAIL. recovered=98/100, level_ready_clients=0, poll_errno=138, max_wait_ms=60013, delivered 800/3272 | ALL-3와 동일(large-HWM completion) |
| unittest_flow_state_socket | 20 case PASS 후 CTest 10.02 s timeout(aggregate 실행시간) | ALL-3와 동일 |
| unittest_mutex | Windows CRITICAL_SECTION 재귀 동작으로 owner try_lock이 true | ALL-3와 동일(백엔드 차이, assertion 완화 금지) |

ALL-3가 남겼던 5개 중 `test_single_lane_wire_mandatory_count`(missing-lane close)와 `test_two_poller_wake`는 이번 전체 실행에서 **PASS**했다. 대신 `writable_resubmit` 2건이 새로 실패한다.

## 9. 실패·미실행 정리 (요약)

**실패**
1. Windows `test_writable_resubmit_from_other_thread_while_sequence_open` / `..._send` — 실행 파일 기동 실패, 재현 100%, ALL-3에 없던 신규. **원인 미확정.**
2. Windows `test_wake_invariants`(large HWM drain), `unittest_flow_state_socket`(10 s aggregate timeout), `unittest_mutex`(CRITICAL_SECTION 재귀) — ALL-3에서 이월된 기존 실패.
3. WS 비율 gate `ws Q64/Q1 = 0.534009 < 0.80` — FAIL(wss는 1.2618 PASS). ALL-3와 같은 방향.
4. hotpath `dealer_router_reqrep_inproc` 0.9494 — 양방향 ±5% 기준으로는 FAIL이나 개선 방향이다.

**미실행 / 측정 불가**
5. latency·RSS gate — `--rss-sidecar` 샘플러 측정이 상한 안에 들어가지 않아 **미실행**.
6. with_stream `cppserver` / `cppserver_pull` / `zmq` — vendored upstream·libzmq 부재로 build_failed, **3/6 스택만 측정**.

**기존 결함(브랜치가 만든 것 아님)**
7. `bindings/rust/include/zlink/common.h`의 `ZLINK_VERSION_PATCH 2` — origin/main과 동일.

## 10. 남은 위험

- Windows `writable_resubmit` 2건은 같은 소스가 다른 clone에서 통과하므로 툴체인/클론 환경 요인일 가능성과 실제 결함 가능성이 모두 남는다. 릴리스 판정 전에 clean clone 재현이 필요하다.
- Windows 전체 실행의 `BAD_COMMAND` 6건은 동시 부하에서만 나왔다. 릴리스 게이트는 다른 작업이 없는 창에서 한 번 더 돌려야 한다.
- 성능 수치는 각 1회(with_stream만 3-run median)이며 장기 변동 폭을 입증하지 않는다. WS 비율과 latency/RSS 목표는 여전히 미달이다.
