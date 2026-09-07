# 게이트 g1-g3 요약

## 적용
- G-3(pipe.hpp/cpp) → G-1(ctx_physical_queue_registry.cpp, socket_base_dispatch.cpp, socket_runtime.hpp) 순서로 `git apply --3way` 모두 클린 적용(충돌 없음). base HEAD `08da256f1e`와 일치.
- `git diff --stat -- core/include core/src/libzlink.vers` 비어 있음(공개 인터페이스 불변 확인).
- mirror cmp: `core/include/{zlink.h,zlink_enum.h,zlink_errno.h}` vs bindings/{cpp,c,go,rust}/include — 12/12 OK (`scripts/gate/README.md`에 mirror 절차 없어 `find` 방식으로 대체).

## 빌드
- `JOBS=4 scripts/build-core.sh dev` 성공(core/build-dev).
- `JOBS=4 scripts/build-core.sh release --lib-only` 성공(core/build, libzlink.so.0.17.1).
- `cmake --build core/build-gate --target hotpath_bench -j4` 성공.

## ctest (core/build-dev, -j2, 208개)
204 PASS, 4 FAIL:
- `hotpath_gate` — dev 트리 ctest 경로에서는 원래 실패(브리프상 정상, 별도 valgrind 게이트로 재확인).
- `test_stream_socket_recv_multiclient_ready_regression` — 알려진 간헐. 단독 3회 재실행 모두 PASS → 간헐 확인.
- **`test_single_lane_flow_snapshot_accounting` / `unittest_single_lane_accounting`** — 목록에 없던 실패. 단독 재실행 각 3회/5회 모두 **100% 결정적 FAIL**(`test_sl_flow_snapshot_accounts_dr_reply_as_application`: integration은 `Expected TRUE Was FALSE`, unittest는 `Expected 4 Was 2`). 간헐이 아니라 이번 패치로 도입된 고정 회귀로 판단됨. 코드 판단·수정은 금지되어 원인 특정 없이 보고만 함.

## 5회 suite (패턴 `wake|poll|stream|pipe|mailbox|send|recv|router|dealer|pair|hwm|flow|registry`)
lost-wake 계열은 `--repeat until-fail:10`, TSan은 G-1/G-2 델타 0으로 생략(브리프 지시). 위 결정적 실패 확인 후 추가 반복은 동일 실패만 재현하므로 생략(시간 예산 고려).

## hotpath_gate 5셀 (valgrind, flock)
| cell | reference | measured | ratio |
|---|---:|---:|---:|
| dealer_dealer_inproc | 3423.533 | 3271.325 | 0.9555 |
| dealer_router_reqrep_inproc | 19682.196 | 18819.839 | 0.9562 |
| pair_inproc | 2348.457 | 2330.427 | 0.9923 |
| router_router_tcp | 2972.532 | 2917.050 | 0.9813 |
| stream_tcp | 14623.471 | 14469.257 | 0.9895 |

모두 PASS, −5% 초과 개선 없음(FAIL-by-improvement 없음). G-3 claim(−1.1%~−3.4%)·G-1 claim(STREAM −3.9%)과 방향 일치, 실측은 더 큰 개선.

## 성능
with_stream(`--stack zlink,asio --size all --ccu 1000 --runs 1 --reuse-build`, `ZLINK_CORE_SOURCE=local`, 측정 시작 load avg 5.27→2.08, 병행 빌드 G-11a 영향 가능):
| size | zlink | asio | ratio | Phase 2S 기준 ratio | Δ |
|---|---:|---:|---:|---:|---:|
| 64B | 319.99 | 389.13 | 0.822 | 0.821 | +0.1% |
| 1024B | 289.50 | 360.94 | 0.802 | 0.823 | −2.6% |
| 65536B | 33.15 | 43.33 | 0.765 | 0.787 | −2.8% |

perf/c 1024B tcp 경량 3셀(`ZLINK_CORE_SOURCE=local`) vs Phase 2G idle 기준(§7.4):
| 셀 | 기준 | 측정 | ratio |
|---|---:|---:|---:|
| single ROUTER_ROUTER | 732.2 Kmsg/s | 845.67 | 1.155 |
| multi RR_SENDSEND | 242.5 Kops/s | 314.60 | 1.297 |
| multi RR_REQREP | 170.1 Kops/s | 204.63 | 1.203 |

## 결론
포팅·빌드·인터페이스·hotpath·perf는 모두 정상/개선. 단, **`test_sl_flow_snapshot_accounts_dr_reply_as_application` 결정적 회귀**를 발견 — 채택 전 감독관 확인 필요. 워킹트리는 patch 적용 상태 그대로 둠(커밋 안 함).
