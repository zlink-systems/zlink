# gate s12 — S-12 (`begin_close_or_fail_busy` transient-admission backoff)

worktree: `~/project/zlink-work/s12` (`bc1519e105`), 1 file. main 시작 HEAD 9474365860.
(포팅 도중 다른 job이 doc/plan에 커밋을 계속 얹어 HEAD가 흘러갔으나 core/bindings/scripts는
그대로였음 — 무관.)

## 1. 적용
`git diff > s12.patch` → main에 `git apply --3way` — 충돌 없이 clean 적용.
`core/src/runtime/sockets/common/socket_lifecycle_runtime.cpp`: `begin_close_or_fail_busy()`가
in-flight 카운트를 즉시 EBUSY 대신 `public_api_sync_yield_limit` 회까지
`public_api_sync_backoff()`로 재시도 후 실패.
공개 인터페이스: `git diff --stat -- core/include core/src/libzlink.vers` 비어 있음(포팅 전/후 모두).

## 2. 빌드/테스트
- `build-core.sh dev`: 성공.
- `ctest --test-dir core/build-dev -j2` 전체 1회: 209개 중 2 실패 — `test_single_lane_flow_snapshot_accounting`(3회 단독 재실행 100% 통과 → 간헐, 전체 부하 시 재현), `hotpath_gate`(dev 트리 비최적화 빌드 대 release 기준치 비교라 항상 실패 — 브리프가 예고한 "dev 트리에 없어야 정상"과 달리 이번엔 등록돼 있었으나 §7의 build-gate 실측이 정본이므로 무시).
- 브리프 suite 패턴(`wake|poll|close|release|stream|pipe|mailbox|socket_runtime|lifecycle|timer_poller|flow_state`) 5회: 46/46 통과 ×5(총 230/230).

## 3. 인터페이스/미러
8헤더×4미러(cpp/c/go/rust) `cmp` 32건 전부 일치.

## 4. release lib
`build-core.sh release --lib-only`: 성공 (`libzlink.so.0.17.0`).

## 5. hotpath 5셀 (close-path 변경 — 불변 기대)
| cell | reference | measured | ratio | verdict |
|---|---:|---:|---:|---|
| dealer_dealer_inproc | 3423.533 | 3385.293 | 0.9888 | PASS |
| dealer_router_reqrep_inproc | 19682.196 | 19357.773 | 0.9835 | PASS |
| pair_inproc | 2527.834 | 2508.538 | 0.9924 | PASS |
| router_router_tcp | 2972.532 | 2953.159 | 0.9935 | PASS |
| stream_tcp | 14623.471 | 14624.745 | 1.0001 | PASS |

핫패스 불변 확인됨(close 경로만 바뀌었으므로 예상대로 5셀 모두 ratio≈1, PASS).

## 6. 성능(경량, load avg 기록)
- with_stream `--stack zlink,asio --size all --ccu 1000 --runs 1 --reuse-build`(`ZLINK_CORE_SOURCE=local`로 release download 우회): size64/1024/65536 모두 완료, mismatch 0. zlink vs asio throughput 비율 기존 추세와 동일 범위(별도 회귀 아님, close 경로만 바뀜).
- perf/c single ROUTER_ROUTER tcp 1024B ×3: median 756.00 Kmsg/s, 774.14 MB/s, lat mean 0.047ms.
- perf/c multi tcp 1024B ×1: RR_SENDSEND 255.48 Kops/s/523.22 MB/s; RR_REQREP 183.63 Kops/s/376.08 MB/s.
- load average 실행 구간: 시작 ~3.7/2.7/3.4, 종료 2.12/2.31/3.06.

## 7. 스트레스 재현(S-12 요약서 §1)
`test_close_completion_poller_release` 50회 루프 + 동시 `ctest -j4 -R 'stream|pipe'`, PERF_LOCK 없이(빌드/테스트 전용, perf 아님) 실행:
- **패치 적용 전(main에서 patch stash, dev 재빌드)**: **48/50 통과, 2/50 실패** (baseline 재현).
- **패치 적용 후(재빌드)**: **50/50 통과, 0/50 실패**.

CLOSE_BUSY 계약 테스트(`stream_socket`, `public_inproc_multipart_send`, `timer_poller`, `flow_state_c_api`) 3회: 4/4 통과 ×3(총 12/12).

## 8. 결론
patch는 main working tree에 적용된 채로 둠(커밋하지 않음). 게이트 관찰 결과 채택 권고.
