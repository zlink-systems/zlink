# Round 73: XSUB match cache 후보 위험 검토

- goal:
  - round72 이후 남은 `PUBSUB/tls/64B` 결손에서 SUB 수신 측 `xsub_t::match()`의 lock/trie 비용을
    줄일 수 있는지 검토한다.
  - 완료 기준: 새 상태 없이 가능한 후보가 있으면 구현 후 focused CTest와 `PUBSUB tcp,tls,ws,wss`
    all-transport perf에서 하락 없이 `+5%` 이상 반복 개선. 새 상태가 필요하면 POSD 비용과
    계약 위험을 먼저 판단하고, 근거가 부족하면 구현하지 않는다.
- 시작 시각: 2026-06-15 13:07:57 KST
- 기준 commit: `3e0e3956b`
- 시작 load_avg:
  - `/proc/loadavg`: `4.81 13.18 10.89`
- corrected baseline:
  - May26 smoke:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - May26 full:
    `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- problem report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- current reports:
  - round70 reduced full:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_123133_round70_current_reduced_full_refresh.txt`
  - round71 PUBSUB/tls low-load:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_125240_round71_pubsub_tls_lowload_recheck.txt`
  - round72 mtrie match functor candidate:
    `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_130310_round72_mtrie_match_with_pubsub_tls_candidate.txt`
- 시작 git 상태:
  - core source diff는 SPOT logical queue 및 global part-helper restore 계열만 남아 있다.
  - round71 TLS 후보와 round72 mtrie functor 후보는 원복되어 source diff가 없다.
  - framework dotnet/java 문서 변경과 `_workspace/`, 기존 perf log untracked 파일은 이번 라운드 범위 밖이다.

## 현재 수치

- round71 low-load `MULTI_PUBSUB/tls/64B`:
  - `2,265,688.2`
  - May26 full 대비 `-13.62%`
  - problem report 대비 `-7.40%`
- round72 mtrie functor candidate:
  - `2,233,588.2`
  - round71 current 대비 `-1.42%`
  - May26 full 대비 `-14.85%`
  - 효과 없음으로 원복했다.

## 코드 관찰

- perf `MULTI_PUBSUB`는 empty-prefix가 아니라 topic `"bench"`를 사용한다.
  - client: `bindings/c/perf/multi/src/perf_multi_pubsub_client.cpp`
  - server: `bindings/c/perf/multi/src/perf_multi_pubsub_server.cpp`
- `xsub_t::match()`는 첫 frame마다 아래 순서로 동작한다.
  - `_has_empty_subscription`이 true이고 invert matching이 아니면 즉시 match.
  - 아니면 `_subscriptions_mu`를 잡는다.
  - 다시 `_has_empty_subscription`을 확인한다.
  - `_subscriptions.check(data, size)`를 호출한다.
  - 결과를 `options.invert_matching`과 xor한다.
- `"bench"` 단일 subscription에서는 empty-prefix fast path가 작동하지 않는다.

## 가설

- 가설 1:
  - `PUBSUB/tls`의 반복 gap 일부는 SUB 수신 측 `xsub_t::match()`의 mutex/trie lookup 비용이다.
    단일 prefix cache가 있으면 `"bench"` steady-state 수신을 빠르게 만들 수 있다.
- 가설 2:
  - 단일 prefix cache는 새 상태와 문자열 수명 관리를 추가한다. dispatch IO thread가 수신하고
    사용자 thread가 subscription을 바꿀 수 있으므로, lock 없이 문자열을 읽으면 data race가 생긴다.
    lock을 유지하면 hot-path 비용이 거의 줄지 않는다.
- 가설 3:
  - `xsub_t::match()`보다 transport/TLS 또는 pipe dequeue 쪽 비용이 더 커서, subscription cache를
    추가해도 `PUBSUB/tls` 개선은 잡음권일 수 있다.
- 먼저 검증할 가설:
  - 가설 2. 새 상태 없이 `xsub_t::match()`를 단순화할 수 있는지 확인한다. 불가능하면 구현하지 않고
    후보 위험으로 기록한다.

## POSD 기준

- 새 상태를 추가하는 변경은 정보 은닉과 동기화 복잡도가 늘어난다.
- 단일 subscription cache를 채택하려면 public 계약을 바꾸지 않고, unsubscribe/duplicate subscribe,
  invert matching, dispatch path에서 의미가 유지되어야 한다.
- `+5%` 미만이거나 한 transport라도 하락하면 유지하지 않는다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드릴 수 있는 보안 항목:
  - mtrie 비재귀화는 직접 수정하지 않는다.
  - decoder/message/send guard는 직접 수정하지 않는다.
- 보안 의미를 유지한 근거:
  - 후보를 구현하더라도 subscription trie 자체의 소멸/순회 구조는 바꾸지 않는다.
  - 원격 prefix 깊이에 따라 C++ 호출 스택을 깊게 쓰는 구현으로 되돌리지 않는다.
- 추가로 실행할 회귀 테스트:
  - 후보가 있으면 `test_pubsub`, `test_pubsub_filter_xpub`, `test_xpub_nodrop`,
    `test_pub_invert_matching`, `test_multi_socket_contract_regressions`, `test_transport_matrix`.

## 후보 설계 검토

### 선택지 A: lock 안에서 단일 subscription fast path

- 내용:
  - `_subscriptions_mu`를 잡은 뒤 trie lookup 대신 최근 단일 prefix와 `memcmp`한다.
- 장점:
  - 새 concurrent-read 문제는 만들지 않는다.
- 문제:
  - hot path의 mutex 비용은 그대로 남는다.
  - `PUBSUB/tls` gap이 lock/trie 양쪽 중 어느 쪽 때문인지 분리하지 못한다.
  - 상태를 추가하면서 줄이는 work는 trie traversal뿐이라 round72의 mtrie callback 후보와 같은
    잡음권일 가능성이 높다.
- 판정:
  - 구현하지 않는다.

### 선택지 B: lock 없는 단일 subscription cache

- 내용:
  - `"bench"` 같은 단일 prefix를 별도 cache에 저장하고 `match()`에서 lock 없이 먼저 비교한다.
- 필요한 추가 상태:
  - cache 활성 여부.
  - cached prefix 길이.
  - cached prefix bytes.
  - subscription 변경과 수신 thread 사이의 일관성을 보장할 version 또는 snapshot.
- 문제:
  - 단순 `std::string` cache는 subscription 변경 thread와 IO/recv thread 사이에서 data race가 난다.
  - atomic byte buffer 또는 immutable snapshot을 쓰면 구현 복잡도가 커진다.
  - duplicate subscribe, last unsubscribe, multiple subscription 추가 후 일부 제거, invert matching을
    모두 보존해야 한다.
  - 캐시가 stale이면 unsubscribe 직후에도 메시지를 통과시킬 수 있으므로 public contract 위험이 있다.
- 판정:
  - 성능 근거 없이 넣기에는 POSD 비용이 크다.
  - `+5%` 이상 반복 개선을 먼저 증명할 수 있는 더 작은 A/B가 없으므로 구현하지 않는다.

## 판정

- `xsub_t::match()` 단일 subscription cache는 현재 조건에서 채택하지 않는다.
- 이유:
  - 정확하게 만들려면 새 동기화 상태가 필요하다.
  - 단순하게 만들면 subscription 변경과 수신 사이의 의미가 약해진다.
  - round72에서 mtrie callback 제거도 효과가 없었기 때문에, trie lookup 주변의 작은 work 제거가
    `PUBSUB/tls` gap을 설명한다는 근거가 약하다.
- 이번 round source 변경:
  - 없음.
- 이번 round build/test/perf:
  - source 변경이 없어 실행하지 않았다.
- 다음 후보:
  - `PUBSUB/tls` 단독 후보를 계속 좁히기보다, round70 reduced full에서 May26 full 대비 크게 낮은
    `SPOT/wss`, `PUBSUB/wss`, `SPOT_SENDSEND tcp/tls`를 standalone low-load로 재확인해 실제 반복
    회귀인지 먼저 분리한다.
  - 반복 회귀가 확인된 항목만 core hot path 후보로 삼는다.
