# Round 67: PUBSUB/DEALER one-way hot path 재검토

- goal:
  - `MULTI_PUBSUB`와 `MULTI_DEALER_DEALER` 64B one-way 미달을 줄일 수 있는
    core runtime hot path 후보를 찾는다.
  - 완료 기준: targeted 64B set에서 하락 항목 없이 순효과가 플러스이거나, 반복 `+5%`
    이상 개선 항목이 인접 set에서 회귀 없이 확인된다. 최종 plan 목표는 별도 full/reduced
    full 검증 전까지 완료 처리하지 않는다.
- 시작 시각: 2026-06-15 KST
- 기준 commit: 현재 작업트리
- 시작 git status:
  - core source diff는 SPOT logical queue 및 part-helper restore 계열만 남아 있다.
  - `framework/languages/dotnet/doc/guide/01-overview.ko.md` 변경과 untracked `_workspace/`,
    다수 perf log는 이번 라운드 범위 밖이다.
- corrected baseline:
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_231454_codex_full_refresh_c_multi_smoke_after_dd_stream_fixes_20260526.txt`
  - `bindings/c/perf/baseline/perf_c_multi_linux_20260526_233010_codex_full_refresh_c_multi_full_after_dd_stream_fixes_20260526.txt`
- 문제 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260614_103936.txt`
- 현재 retained 변경 기준 report:
  - `bindings/c/perf/results/multi/report/perf_c_multi_linux_20260615_104531_round65_final_spot_restore_all64_reduced_full.txt`
- 대상 pattern/transport/size:
  - `DEALER_DEALER`, `PUBSUB`, 필요하면 `SPOT`
  - `tcp,tls,ws,wss`
  - `64B`

## 가설

- 가설 1:
  - `DEALER_DEALER`는 `lb_t -> pipe_t`, `PUBSUB`는 `dist_t -> pipe_t`로 모인다.
    두 경로의 공통 비용은 pipe write/flush와 message close/init다.
- 가설 2:
  - 기존 단일 pipe fast path, prechecked HWM, empty-subscription active pipe 상태 추가는
    효과가 없거나 transport별 하락을 만들었다. 같은 상태 추가 방식은 피한다.
- 가설 3:
  - 남은 후보는 새 상태를 늘리는 fast path보다 기존 hot path의 중복 초기화, 반복 flag 조회,
    불필요한 map/vector touch를 줄이는 쪽이어야 한다.
- 선택한 가설:
  - `lb_t`, `dist_t`, `pipe_t`를 다시 읽고, 기존 인터페이스를 넓히지 않는 작은 중복 제거 후보가
    있는지 확인한다.

## 보안 하드닝 보존 확인

- 참조 report: `core/doc/report/odl/2026-06-13-core-src-security-review.ko.md`
- 이번 변경이 건드린 보안 항목:
  - 아직 source 변경 전이다.
- 보안 의미를 유지한 근거:
  - WS/WSS pending message, mtrie, 포트 파싱, IPC unlink, decoder/message/send guard,
    maxmsgsize 정책을 변경하지 않는다.
- 추가로 실행한 회귀 테스트:
  - source 후보가 생기면 기록한다.

## 읽은 코드와 POSD 기준 검토

- `core/src/runtime/sockets/internal/lb.cpp`
  - `DEALER` send는 `lb_t::sendpipe()`에서 pipe로 모인다.
  - 단일 active pipe hot path와 weighted schedule 분리는 이미 있다.
  - final single-message pipe helper 적용은 round 1, round 11, round 63 계열에서 이미
    반복 검증했고 안정적인 개선이 없었다.
- `core/src/runtime/sockets/internal/dist.cpp`
  - `PUB/SUB` fanout은 `dist_t::send_to_matching()`과 `write_at()`을 지난다.
  - 단일 matching pipe fast path와 VSM 분기는 이미 있다.
  - 64B payload는 현재 `msg_t::max_vsm_size`보다 커서 LMSG/refcount 경로를 탄다.
  - small-LMSG pool 후보는 round 10에서 혼합/하락으로 배제됐다.
- `core/src/runtime/core/pipe.cpp`
  - write/flush helper와 single-message helper는 이미 존재한다.
  - final single-message helper를 `DEALER`/`PUBSUB`에 확장하는 후보는 round 63에서
    `DEALER_DEALER,PUBSUB,SPOT` 전체 평균 `+0.9%`, 중앙값 `+0.4%`에 그쳐 원복됐다.
- `core/src/runtime/sockets/internal/fq.cpp`
  - `DEALER`/`SUB` receive side는 `fq_t::recvpipe()`로 모인다.
  - `normalize_state()`는 2026-04-09 release 변경에서 pipe index 검증, teardown 중
    transient miss 처리와 함께 들어간 방어 코드다.
  - 이를 hot path 비용으로 보고 제거하는 것은 불변식 복구 책임을 caller와 외부 상태 가정으로
    밀어 올리므로 POSD의 "복잡성을 아래로" 원칙에 맞지 않는다.
- `core/src/runtime/sockets/common/socket_base_msg.cpp`
  - public recv 경로는 command polling, `xrecv`, non-blocking retry, flag extraction을 수행한다.
  - command poll throttle, read-drain/batch류 후보는 이전 라운드에서 개선으로 확인되지 않았다.
- `core/src/runtime/sockets/pubsub/xsub.cpp`
  - empty subscription이면 `_has_empty_subscription`을 통해 `match()`가 이미 trie lookup을 건너뛴다.
  - 별도 `_empty_subscription_pipes`/single active pipe 상태 추가 후보는 round 65에서
    같은-window 기준 `tcp +3.66%`, `tls +1.23%`, `ws -5.19%`, `wss +1.13%`였고
    `ws` 하락과 상태 증가 때문에 배제했다.
- `core/src/runtime/sockets/pubsub/xpub.cpp`
  - repeated-topic matching cache 후보는 round 30에서 `PUBSUB/tcp +4.34%`였지만 full/current
    기준 개선이 명확하지 않아 원복됐다.

## 재검토한 후보와 판정

| 후보 | POSD 판단 | 성능 판단 | 판정 |
|------|-----------|-----------|------|
| `dist_t`/`lb_t` final single-message helper 확장 | 기존 helper 재사용이라 인터페이스 증가는 작지만 이미 반복 검증됨 | round 63 평균 `+0.9%`, 중앙값 `+0.4%` | 재시도 안 함 |
| PUBSUB empty-subscription active pipe 상태 | 새 상태와 invalidation 규칙이 늘어남 | same-window `ws -5.19%` | 배제 유지 |
| XPUB repeated-topic matching cache | topic/topology cache invalidation 지식이 `xpub_t`에 추가됨 | 단일 tcp `+4.34%`, full/current 기준 불명확 | 배제 유지 |
| `fq_t::normalize_state()` 제거 | teardown/pipe index 안전장치를 제거해 caller 불변식 의존 증가 | 성능 이득 미측정, 안전 리스크 큼 | 배제 |
| 64B VSM 확대 | `zlink_msg_t`/`msg_t` layout 의미를 건드릴 수 있음 | ABI/보안 위험이 큼 | 배제 |

## Round 67 중간 판정

- 이번 재검토에서는 새 source 후보를 적용하지 않았다.
- 이유:
  - 새 상태를 추가하는 후보는 이전 측정에서 transport별 하락이 있었다.
  - 기존 helper 재사용 후보는 이미 반복 검증했고 1% 안팎에 머물렀다.
  - 남은 receive-side 방어 코드 제거는 성능보다 안전/복잡도 리스크가 커서 POSD 기준에 맞지 않는다.
- 이 라운드에서 perf를 새로 돌릴 source 변경은 없다.
- 다음으로는 round 65 로그의 과거 후보 중 "하락 항목 없이 소폭 플러스"였던 후보가 실제로 있었는지
  같은-window 비교 기준으로 다시 분류한다.

## 과거 후보 재분류

사용자가 완화한 기준을 반영해, `+5%` 미만이라도 전체가 플러스이고 하락 항목이 없으면
유지 후보가 될 수 있는지 다시 봤다.

| 후보 | 비교 기준 | 결과 | 재판정 |
|------|-----------|------|--------|
| SPOT logical queue + part-helper restore | May26 full, same-env old commit, reduced full | `SPOT/ws` probe `6.82M`, non-tcp tls/ws 동급, same-env old보다 wss 높음 | 유지 |
| SPOT vector publish copy 복원 | standalone current | tls `+1.80%`, ws `-0.63%`, wss `-0.38%` | 효과 없음 |
| submit-retry fault hook thread-local | 원복 same-window | 12개 평균 `-1.50%`, `SPOT/tls -15.80%`, `SPOT/wss -19.49%` | 배제 |
| PUBSUB empty-subscription active pipe | 원복 same-window | tcp `+3.66%`, tls `+1.23%`, ws `-5.19%`, wss `+1.13%` | 하락 항목 때문에 배제 |
| SPOT routed-send encoded-bytes cache | 직전 standalone | tcp `-0.66%`, tls `-0.38%` | 배제 |
| dist/lb single-message helper | round 63 current | 평균 `+0.9%`, 중앙값 `+0.4%` | 하락 없는 명확한 플러스 묶음 아님 |
| STREAM wrapper API mutex 제거 | CTest | `test_stream_threadsafe` timeout | 기능 회귀로 배제 |

## Round 67 최종 판정

- 현재까지 재검토한 과거 후보 중, "하락 항목 없이 소폭이라도 플러스"라서 되살릴 후보는 없다.
- 유지할 변경은 이미 적용된 SPOT logical queue + part-helper restore뿐이다.
- POSD 기준:
  - 성능 근거가 약한 상태 추가는 유지하지 않는다.
  - 안전장치 제거형 후보는 측정 전에 배제한다.
  - 기존 helper 재사용이라도 반복 측정에서 효과가 없으면 남기지 않는다.
- source 변경 없음. 추가 build/perf는 필요하지 않다.
