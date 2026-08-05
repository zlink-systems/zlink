# Round 72: PUBSUB mtrie/dist hot path 후보

- goal:
  - `MULTI_PUBSUB/tls/64B` 반복 하락의 core PUB/SUB 송신 경로에서 기존 상태를 재사용해
    줄일 수 있는 work가 있는지 확인한다.
  - 완료 기준: 후보가 있으면 focused CTest 통과, `PUBSUB/tls/64B` targeted perf에서 `+5%`
    이상 또는 하락 없는 명확한 반복 개선, 작업 로그 작성.
- 시작 시각: 2026-06-15 12:59:56 KST
- 기준 commit: `3e0e3956b`
- 시작 load_avg:
  - `/proc/loadavg`: `7.12 11.86 8.42`
- corrected baseline:
  - May26 smoke:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - May26 full:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- historical baseline:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260513_101034.txt`
  - 사용자 정정에 따라 판정 기준으로 쓰지 않는다.
- problem report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- current report:
  - round70 reduced full:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_123133_round70_current_reduced_full_refresh.txt`
  - round71 PUBSUB/tls low-load:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_125240_round71_pubsub_tls_lowload_recheck.txt`
- 시작 git 상태:
  - core source diff는 SPOT logical queue 및 global part-helper restore 계열만 남아 있다.
  - TLS `async_write_some()` 후보는 round71에서 원복했고 source diff가 없다.
  - framework dotnet/java 문서 변경과 `_workspace/`, 기존 perf log untracked 파일은 이번 라운드 범위 밖이다.

## 현재 수치

- round71 low-load `MULTI_PUBSUB/tls/64B`:
  - current: `2,265,688.2`
  - May26 full 대비: `-13.62%`
  - problem report 대비: `-7.40%`
  - round70 대비: `+0.05%`
- round70 reduced full May26 full 대비:
  - 전체 평균 `-0.51%`, 중앙값 `-0.35%`
  - one-way 평균 `-3.85%`, 중앙값 `-3.61%`
  - echo 평균 `+1.50%`, 중앙값 `+1.65%`

## 가설

- 가설 1:
  - `xpub_t::xsend()`가 매 64B publish마다 `_subscriptions.match()`와 `dist_t::match()`를 거친다.
    empty subscription 또는 단순 subscription 구성에서 기존 mtrie 정보를 더 직접 사용할 수 있으면
    PUBSUB/tls 하락을 줄일 수 있다.
- 가설 2:
  - `dist_t::send_to_matching()`은 이미 단일 matching pipe fast path와 matching HWM cache를 갖고 있다.
    남은 결손은 mtrie traversal이나 ASIO/TLS output 쪽이며, dist/pipe에 새 상태를 더해도 이득이 작거나
    하락 항목이 생길 수 있다.
- 가설 3:
  - PUBSUB/tls 결손은 TLS transport 경계와 PUB fanout이 결합된 현상이다. TLS write completion 방식
    단독 후보가 실패했으므로, 다음 후보는 transport 정책이 아니라 PUB/SUB matching 쪽에서 찾아야 한다.
- 먼저 검증할 가설:
  - 가설 1. mtrie match path를 읽고, 빈 prefix 또는 단일 prefix steady-state에서 기존 구조만으로
    work를 줄일 수 있는지 확인한다.

## POSD 기준

- 새 캐시나 상태를 추가하는 후보는 기본적으로 제외한다.
- 이미 유지 중인 구조가 보유한 정보만 재사용하는 변경을 우선한다.
- 인터페이스보다 구현 복잡도가 커지는 후보, 안전 가드 제거, perf 전용 shortcut은 채택하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드릴 수 있는 보안 항목:
  - mtrie 비재귀화.
- 보안 의미를 유지한 근거:
  - `generic_mtrie_t` 소멸자와 `visit_values`의 비재귀 동작을 되돌리지 않는다.
  - 원격 prefix 깊이에 따라 C++ 호출 스택을 깊게 쓰는 구현으로 바꾸지 않는다.
- 추가로 실행할 회귀 테스트:
  - 후보가 있으면 `test_pubsub`, `test_pubsub_filter_xpub`, `test_xpub_nodrop`,
    `test_transport_matrix`, `unittest_mtrie`.

## 후보: mtrie match functor overload

- 후보 변경:
  - `generic_mtrie_t::match_with()`를 추가해 기존 `match()`와 같은 순회를 하되 함수 포인터 callback 대신
    호출자가 넘긴 functor를 직접 호출하게 했다.
  - `xpub_t::xsend()`에서 `_subscriptions.match(..., mark_as_matching, this)` 대신
    `_subscriptions.match_with(..., [this] (pipe_t *pipe_) { _dist.match(pipe_); })`를 사용했다.
  - 수동 last-pipe 경로도 같은 의미로 lambda에서 `last_pipe == pipe_`를 확인하게 했다.
- 후보로 본 이유:
  - 새 상태나 캐시를 추가하지 않는다.
  - mtrie traversal, matching pipe 선택, HWM 의미는 그대로 둔다.
  - `PUBSUB` hot path에서 매 pipe마다 함수 포인터 callback을 거치는 비용만 줄이는 형태라,
    POSD 기준에서 검증 가능한 단순 후보였다.
- build:
  - `cmake --build core/build -j$(nproc)` 통과.
  - 첫 빌드에서 WSL clock skew 경고가 있어 같은 명령을 재실행했고, 두 번째 빌드는 경고 없이 통과했다.
- focused CTest:

```bash
ctest --test-dir core/build --output-on-failure \
  -R 'unittest_mtrie|test_(pubsub|pubsub_filter_xpub|xpub_nodrop|transport_matrix|multi_socket_contract_regressions|backpressure_oneway_matrix|backpressure_matrix)$'
```

- 결과:
  - 6/6 passed.
- targeted perf:

```bash
PERF_FAIL_FAST=1 PERF_MSG_SIZES=64 \
  bindings/c/perf/run_benchmarks_multi.sh \
  --reuse-build \
  --pattern PUBSUB \
  --transports tls \
  --duration 5 \
  --runs 7 \
  --connect-ready-timeout-ms 5000 \
  --results-tag round72_mtrie_match_with_pubsub_tls_candidate
```

- runner runtime:
  - `/home/hep7/project/kairos/zlink/core/build/lib/libzlink.so.6.0.4`
- runner meta load_avg:
  - `26.00 15.97 10.36`
- report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_130310_round72_mtrie_match_with_pubsub_tls_candidate.txt`
- completion:
  - success: 1
  - fail: 0
  - status: complete
- result:
  - `MULTI_PUBSUB/tls/64B`: `2,233,588.2`
- comparison:
  - round71 current 대비: `-1.42%` (`2,265,688.2` -> `2,233,588.2`)
  - May26 full 대비: `-14.85%` (`2,623,065.0` -> `2,233,588.2`)

## 판정

- `match_with()` 후보는 focused CTest를 통과했지만, targeted perf에서 개선 신호가 없었다.
- 시작 load가 높았다는 한계는 있지만, `+5%` 근처가 아니라 오히려 round71 current보다 낮았다.
- 함수 포인터 callback 제거만으로는 `PUBSUB/tls` gap을 설명하지 못한다.
- 변경 효과가 없으므로 후보 변경은 남기지 않았다.
- 원복 후:
  - `core/src/runtime/utils/generic_mtrie.hpp`
  - `core/src/runtime/utils/generic_mtrie_impl.hpp`
  - `core/src/runtime/sockets/pubsub/xpub.cpp`
  - `core/src/runtime/sockets/pubsub/xpub.hpp`
  - 위 네 파일의 후보 diff는 사라졌다.
  - `cmake --build core/build -j$(nproc)` 통과.

## 다음 판단

- `mtrie` callback dispatch 자체는 주된 병목으로 보기 어렵다.
- perf `MULTI_PUBSUB`는 empty-prefix가 아니라 topic `"bench"`를 사용한다.
  - client: `bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp`
  - server: `bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp`
  - 따라서 empty-prefix 전용 fast path는 이번 병목에 직접 맞지 않고, benchmark 조건을 바꾸는 방식도
    성능 개선으로 인정하지 않는다.
- 수신 측 `xsub_t::match()`는 각 첫 frame마다 `_subscriptions_mu`를 잡고 trie check를 한다.
  `"bench"` 단일 subscription 캐시는 가능한 후보지만, 새 상태와 동기화 의미가 추가된다. 현재 POSD 기준에서는
  하락 없는 큰 반복 개선 근거 없이 바로 채택할 수 없다.
- 이미 확인한 기각 후보:
  - TLS write completion 방식 변경.
  - mtrie match functor overload.
  - PUBSUB empty-subscription active pipe 상태 추가.
  - TLS speculative write enable.
  - ASIO handler allocator 전체 확대.
  - distributor final helper.
- 다음으로 볼 수 있는 후보는 `xsub_t::match()` lock/trie 비용이 실제 병목인지 instrumentation 없이
  source-level A/B로 검증하는 것이다. 다만 단일 subscription cache는 상태 추가 후보이므로, 적용한다면
  `PUBSUB tcp/tls/ws/wss` 전체에서 하락 없이 `+5%` 이상 반복될 때만 유지한다.
