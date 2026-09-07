# 게이트 r5-r6r8-r9-r7r11 요약 (2026-09-07)

## 적용
main HEAD f67625990a에 순서대로 `git apply --3way` (모두 conflict 없이 클린 적용, 일부 hunk는 "Falling back to direct application"이었으나 exit=0):
- R5-A: `~/project/zlink-work/r5` — `core/src/api/socket/socket_request_reply_submit_api.cpp`
- R6R8-A: `~/project/zlink-work/r6r8` — `core/src/runtime/core/session_base.cpp`, `ctx_physical_queue_registry.{cpp,hpp}`, `sockets/internal/dist.cpp`, `sockets/router/router_send_path.cpp`
- R9-ABC: `~/project/zlink-work/r9` — `core/src/runtime/transports/{asio,ipc,tcp,tls,ws}/*` 12 파일
- R7R11: `~/project/zlink-work/r7r11` — `core/src/api/core/*`, `socket_message_send_api.cpp`, `sockets/pubsub/xsub.*`, `utils/radix_tree.{cpp,hpp}` 삭제 + `unittest_radix_tree.cpp` 삭제, `core/CMakeLists.txt`, `core/tests/unittest/CMakeLists.txt`, `core/builds/cmake/platform.hpp.in`

인용할 hunk 충돌 없음 — 4개 patch 전부 기계적으로 클린.

## 공개 인터페이스
`git diff --stat -- core/include core/src/libzlink.vers` 비어 있음 확인(적용 직후 및 최종 재확인). 8 헤더 × 4 mirror(c/cpp/go/rust) `cmp` 32건 전부 일치.

## 빌드
- dev(`JOBS=6 scripts/build-core.sh dev`): 성공.
- release --lib-only: 성공 (`libzlink.so.0.17.0`).
- gate: `hotpath_bench` 빌드 성공.

## ctest (core/build-dev, 전체 1회)
208 tests, 207 passed, 1 failed = `hotpath_gate` (dev 트리에 없어 정상, 브리프 예상대로).
간헐 후보(`test_single_lane_flow_snapshot_accounting`, `test_stream_socket_recv_multiclient_ready_regression`) 모두 이번 전체 실행에서 PASS.

## 변경 suite 5회 (합집합 패턴)
패턴: `request|reply|reqrep|registry|hwm|dist|router|dealer|pubsub|xsub|tcp|ipc|tls|ws|endpoint|listener|option|close|send|part|flags`
- 5회 모두 85/85 PASS, 0 실패.
`test_endpoint_release` 10x: 10/10 PASS (매회 ~3.7s).

## 바인딩 스모크 (release lib, ZLINK_CORE_SOURCE=local)
- `bindings/c/tests/run_tests.sh`: contract 10/10 PASS, sample-smoke 6/6 PASS.
- `bindings/cpp/tests/run_tests.sh`: contract 19/19 PASS, sample-smoke 7/7 PASS.
(R7R11의 ENOTSUP→EINVAL errno 변경으로 인한 회귀 없음.)

## hotpath_gate 5셀
| 셀 | reference | measured | ratio | verdict |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3390.715 | 0.9904 | PASS |
| dealer_router_reqrep_inproc | 19682.196 | 19507.001 | 0.9911 | PASS |
| pair_inproc | 2527.834 | 2513.576 | 0.9944 | PASS |
| router_router_tcp | 2972.532 | 2980.458 | 1.0027 | PASS |
| stream_tcp | 14623.471 | 14792.949 | 1.0116 | PASS |

## 성능 표
### with_stream (CCU 1000, size all, runs 1, reuse-build; §7.1 Phase 0 기준 대비)
| size | zlink kops/s | asio kops/s | zlink/asio | 기준(§7.1 Phase0) zlink/asio |
|---|---:|---:|---:|---|
| 64 B | 251.19 | 364.57 | 0.689 | 268.9/322.0=0.835 |
| 1024 B | 267.30 | 340.84 | 0.784 | 243.0/316.4=0.768 |
| 65536 B | 31.98 | 41.63 | 0.768 | 30.4/39.2=0.775 |

zlink 절대값: 64B 251.19 (기준 268.9 대비 0.934), 1024B 267.30(기준 243.0 대비 1.100), 65536B 31.98(기준 30.4 대비 1.052). 64B 셀만 기준 대비 다소 낮음(단일 run, 변동 가능성) — 감독관 확인 필요.

### perf/c 경량 3셀 (1024 B tcp, runs 1; §7.2 Phase 0 기준 대비)
| 셀 | Phase 0 기준 | 측정값 | 비율 |
|---|---:|---:|---:|
| single ROUTER_ROUTER | 744.4 Kmsg/s | 809.69 Kmsg/s | 1.088 |
| multi ROUTER_ROUTER_SENDSEND | 111.5 Kops/s | 264.92 Kops/s | 2.376 |
| multi ROUTER_ROUTER_REQREP | 73.0 Kops/s | 197.15 Kops/s | 2.700 |

(SENDSEND/REQREP는 Phase 0 이후 누적된 다른 landed 개선들의 효과 포함, 회귀 없음.)

Load average: 측정 구간 3~8 범위(hotpath 시작 7.57, with_stream 시작 6.68, perf/c 단일 4.06, multi 3.12).

## 결론
main 저장소 working tree는 4개 patch가 적용된 상태로 그대로 유지(커밋하지 않음). 감독관 검토 후 커밋 필요.
