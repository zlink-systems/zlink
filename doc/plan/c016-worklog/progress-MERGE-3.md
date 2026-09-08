# MERGE-3 준비 (SD-4 적용 대기)

- 2026-09-09 01:20. 브랜치 `wip/0.17.4` = `e215322ea5`.

## SD-4 충돌 사전 확인 (적용·커밋·빌드 없음)

`git apply --3way --check ~/project/zlink-work/all-artifacts/SD-4.patch` → **충돌 0, 성공**.
plain `git apply --check`도 성공(3-way fallback 없이 그대로 붙는다). worktree 무변경 확인.

| 파일 | +/− |
|---|---:|
| bindings/c/bench/with_stream/stacks/zlink/test_scenario_stream_zlink.cpp | 0 / 82 |
| core/src/runtime/engine/asio/asio_engine.cpp | 82 / 75 |
| core/src/runtime/engine/asio/asio_engine.hpp | 4 / 1 |
| core/src/runtime/engine/asio/asio_engine_pipeline.hpp | 2 / 8 |
| core/src/runtime/engine/asio/asio_stream_fastpath_policy.hpp | 11 / 49 |
| core/src/runtime/transports/ws/ws_batch_policy.hpp | 19 / 0 |
| core/tests/unittest/unittest_asio_write_turn_policy.cpp | 68 / 19 |
| doc/plan/c016-worklog/core-rf-SD-3-report.md | 105 / 0 (신규) |
| doc/plan/c016-worklog/core-rf-SD-4-report.md | 98 / 0 (신규) |

- `core/include/**`·`core/src/libzlink.vers` 포함 0. `core/tests/perf/hotpath_reference.json` 포함 0(보고서 본문 언급만).
- ALL-2b가 건드린 `ws_batch_policy.hpp`(+19/−0)와 `unittest_asio_write_turn_policy.cpp`(+68/−19)는 순수 추가·치환으로 붙는다.
- MAC-4/CCU-5가 건드린 파일과 겹치지 않는다.

## 최종 게이트 계획 (SD-4 적용 신호 뒤)

1. `git apply --3way SD-4.patch` → 커밋 → `core/include`·`libzlink.vers` diff 0 확인
2. `JOBS=4 scripts/build-core.sh dev`
3. `ctest -R 'stream|asio|decoder|ws|zmp_ws|auto_hwm|ctx' --repeat until-fail:5`
4. 전체 `ctest -E hotpath_gate` 1회
5. TSan **전체** ctest(`core/build-tsan` 증분, `setarch x86_64 -R`, 억제 없음, 경고 0 기대)
6. `JOBS=4 scripts/build-core.sh release-gate` (Release+LTO)
7. hotpath 5셀 — idle(ninja 0, load1<1.5 2분 유지), `flock /tmp/claude-1000/PERF_LOCK`
8. ws gate 12셀 1회 — `run_benchmarks_multi.sh --pattern DEALER_DEALER,DEALER_ROUTER_SENDSEND,ROUTER_ROUTER_SENDSEND --transports tcp,ws,wss --msg-sizes 1024,65536 --runs 1` (한 report 안에 전 셀이 있어야 gate가 판정한다) → `ws_roundtrip_gate.py`
9. with_stream pull 4 stack(zlink/asio_pull/cppserver_pull/zmq) 64/1024/65536 CCU 1000 1회 — **`ZLINK_CORE_SOURCE=local` 필수**(기본값은 ~/.cache의 릴리스 lib를 쓴다). cppserver_pull·zmq는 vendored upstream/libzmq가 없으면 build_failed로 빠지므로 사전에 확인 필요
10. `git fetch && git rebase origin/main` — 스펙 D-H1(06-auto-hwm ko/en)·08-stream 문장 유입, 충돌 시 보고
11. `git push --force-with-lease`

## MERGE-3 실행
- 02:50 SD-5.patch SHA-256 일치 확인, `--3way` 적용 충돌 0(23파일: core 16 + tests 4 + doc 3). public iface diff 0. 커밋 `ec230249f6`.
- 03:20 dev 빌드 PASS, 변경 suite 40개 until-fail:5 PASS(95.43s), 전체 ctest 212/212 PASS(235.08s), TSan 전체 212/212 PASS·경고 0(374.22s). 다음: Release+LTO.
- 03:40 Release+LTO 빌드 PASS. hotpath 5셀 idle(load 0.10) **5/5 PASS** (DD 0.9617, DR_REQREP 1.0010, PAIR 1.0474, RR_tcp 1.0213, STREAM 0.9939).
- 다음: ws gate 18셀 단일 run × 3.
- 03:55 ws gate 18셀 단일 run 3회 **3/3 PASS**(ws 0.848/1.112/1.295, wss 1.005/0.977/1.161). 세 run 모두 success 18/18, fail 0.
- 04:05 W-SD5-1 RR/ws/65536 단독 3회: **0/3 재현**(전부 complete, fail 0). 18셀 run 3회의 같은 셀까지 합치면 0/6.
- 04:25 with_stream pull 4 stack: cppserver upstream의 `upstream/cmake/`가 rel174에 빠져 있어 추가 복사(ignored) 후 cppserver_pull 빌드 성공. zmq는 65536에서만 client rc=2로 run_failed(64/1024는 성공).
- 04:35 `git rebase origin/main`(92b00fbc76) 충돌 0. 스펙 D-H1·08-stream·CHANGELOG 유입 확인(core/doc/spec diff vs main = 0). main이 514906a682 이후 core/src·core/include·core/tests를 바꾸지 않았으므로 게이트 재실행 불필요.
- 04:36 push 완료. head **a8cb7b3c9e**.
