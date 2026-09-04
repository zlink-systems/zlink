# Go binding 독립 리뷰 결과

Core 0.17.0 계약 B에 맞춰 `bindings/go/**`를 검토하고, 공개 API를 바꾸지 않은 채 correctness 1건, completion hot path 2건, perf runner 2건을 수정했다. BLOCKER는 없다.

## 계약 항목 판정

| 항목 | 판정 | 근거 |
|---|---|---|
| (a) DONTWAIT 1회 시도, OK/ID 0 즉시 종료 | PASS | `attemptSend`는 native FINAL을 한 번 호출하고 `err == nil && completionID == 0`이면 completion을 기다리지 않고 끝낸다. 성공 send 등록만으로 runtime poller를 만들던 비용도 제거했다. |
| (b) BACKPRESSURED payload 보유와 정확한 WRITABLE 상관관계 | PASS | binding 소유 `zlink_msg` snapshot을 유지한다. token, cgo context, RID를 모두 비교한 뒤 같은 packet만 다시 보내며, 재차 BACKPRESSURED이면 새 token을 저장한다. 다른 token/RID는 해당 대기자를 깨우지 않는다. |
| (c) WRITABLE TERMINAL 전달 | FIXED | 기존에는 모든 terminal을 `SubmitNotAdmitted`로 반환했다. `ENOENT`는 `SubmitNotFound`, `ESHUTDOWN`·`ETERM`은 `SubmitTerminated`로 반환하며 대기자를 끝낸다. |
| (d) ROUTER/STREAM route 없음 | PASS | ID 0인 `SubmitNotConnected/EHOSTUNREACH`를 즉시 반환한다. 공개 회귀 테스트가 통과했다. |
| (e) completion drain과 level readiness | FIXED | public poller owner 한 곳이 queue를 `NO_DATA`까지 비우고 REQUEST와 WRITABLE을 context별 O(1) map lookup으로 나눈다. WRITABLE-only wake는 `PollOut`만 노출한다. runtime owner의 timeout 0 spin을 blocking `POLLCOMPLETION` poll로 바꿨다. |
| (f) close/term 자원·thread safety | PASS | `shutdownOwner`가 runtime drain을 먼저 합류시키고 entry별 attempt lock 아래 snapshot, reply parts, cgo handle을 한 번만 해제한다. cancel 뒤 live token은 payload 없는 tombstone으로 남겨 handle 재사용을 막는다. 타깃 race test가 통과했다. |
| (g) 오류 타입·mapping | FIXED | terminal errno를 `*SubmitError`의 공개 result와 원래 errno에 보존한다. 잘못된 completion은 `SubmitInternalError/EPROTO`로 유지한다. |
| (h) REQUEST/blocking/PUB 회귀 | PASS | REQUEST completion 경로와 PUB/XPUB publish 경로는 변경하지 않았다. 전체 test/vet, async request sample, PUBSUB sample과 perf smoke가 통과했다. |

## 발견 버그

| 파일:행 | 증상 | 수정 |
|---|---|---|
| `bindings/go/internal/native/completion_owner.go:337` | completion 없는 즉시 성공 send도 매 건 native poller와 goroutine을 만들었다. | REQUEST는 기존처럼 즉시 drain을 시작하고, SEND는 nonzero wait token을 실제로 받은 때에만 runtime drain을 시작한다. |
| `bindings/go/internal/native/completion_owner.go:433` | runtime drain이 timeout 0 poll과 `Gosched`를 반복해 completion이 없을 때 CPU를 사용했다. | `POLLCOMPLETION`만 100ms bounded blocking poll한다. shutdown은 최대 한 poll 주기 안에 합류한다. |
| `bindings/go/internal/native/completion_owner.go:702` | WRITABLE terminal의 `ENOENT/ESHUTDOWN/ETERM`을 모두 `SubmitNotAdmitted`로 축약했다. | route 제거는 `SubmitNotFound`, lifecycle 종료는 `SubmitTerminated`로 구분하고 errno를 보존한다. |
| `bindings/go/perf/run_benchmarks.sh:398`, `bindings/go/perf/run_benchmarks_multi.sh:626` | `ZLINK_GO_NATIVE_DIR=/.../core/build/lib`를 주면 platform 하위 디렉터리를 강제로 붙여 prebuilt Core를 찾지 못했다. | 지정 디렉터리에 versioned runtime이 있으면 그 경로를 직접 사용한다. Core rebuild·clean은 하지 않았다. |
| `bindings/go/perf/multi/perf_multi_dealer_router.go:155` | 측정 종료 뒤 receive drain을 멈춘 후 sender가 마지막 WRITABLE을 기다리면 `senders.Wait()`가 영구 대기했다. 실제 TLS/1024 케이스가 300초 timeout에 걸렸다. | 측정 종료를 알리고 client socket을 닫아 lifecycle wake로 pending send를 끝낸 뒤 sender를 합류시킨다. 수정 후 해당 케이스는 3초 안에 끝났다. |

## 성능 검토

| 항목 | 판정 | 수정 | 측정 |
|---|---|---|---|
| payload snapshot | PASS | `Message`/`MoveMessage` snapshot과 재시도용 native part는 `zlink_msg_copy` 공유 복사다. 64B 초과 payload 본문은 복사하지 않는다. `Bytes`는 Go slice에서 native message를 만드는 필수 1회 복사만 수행한다. | 1024B 기준 payload 본문 추가 복사 없음. |
| 성공 send의 poller syscall·goroutine | FIXED | wait token을 받기 전에는 runtime completion poller를 만들지 않는다. | DEALER_ROUTER TCP throughput 81,519 → 190,141 msg/s. |
| 이벤트 없는 drain loop | FIXED | nonblocking spin 대신 bounded blocking native poll을 사용한다. 고정 sleep 없음. | 타깃 race test와 full smoke 통과. |
| waiter lookup | PASS | socket-local `map[uintptr]*completionEntry`로 token context를 평균 O(1)에 찾는다. public/runtime drain owner 전환은 한 owner만 허용한다. | mixed completion 기존 tests 통과. |

수정 전후 동일 조건: DEALER_ROUTER, TCP, 1024B, duration 3초, runs 1.

| 시점 | throughput (msg/s) | latency mean (ms) | p95 (ms) | p99 (ms) |
|---|---:|---:|---:|---:|
| 수정 전 | 81,519.333 | 0.620 | 2.727 | 7.859 |
| 수정 후 | 190,141.333 | 0.340 | 1.792 | 5.421 |

- 수정 전 원본: `/home/hep7hep7/project/zlink-work/c016/go-review-before.csv`
- 수정 후 원본: `/home/hep7hep7/project/zlink-work/c016/go-review-after.csv`

## 스모크 결과

| 검증 | 결과 |
|---|---|
| 타깃 public/internal 회귀, `-count=5` | PASS |
| 타깃 회귀, Go race detector | PASS |
| `bindings/go/tests/run_tests.sh` | PASS: 모든 package test, vet, raw-contract/hot-path guard |
| samples | PASS: 7/7 |
| single perf | PASS: 6/6, 전부 nonzero throughput |
| multi perf | PASS: 24/24, 전부 nonzero throughput, 120/120 result rows |
| `git diff --check`, `gofmt -l`, perf script `bash -n` | PASS |

Single perf, 1024B, duration 2초, runs 1:

| pattern | transport | throughput (msg/s) | mean latency (ms) |
|---|---|---:|---:|
| PAIR | tcp | 228,586.0 | 0.095 |
| PAIR | inproc | 236,985.0 | 0.036 |
| DEALER_ROUTER | tcp | 192,836.0 | 0.340 |
| DEALER_ROUTER | inproc | 227,172.0 | 0.042 |
| PUBSUB | tcp | 373,163.5 | 0.129 |
| PUBSUB | inproc | 433,057.0 | 0.126 |

Multi perf, CCU 8, duration 2초, runs 1; 값은 throughput(msg/s 또는 SENDSEND의 ops/s):

| pattern / size | tcp | tls | ws | wss |
|---|---:|---:|---:|---:|
| DEALER_DEALER / 1024 | 238,782.0 | 284,729.5 | 244,685.0 | 323,144.5 |
| DEALER_DEALER / 65536 | 68,339.0 | 27,172.0 | 48,996.0 | 6,594.0 |
| DEALER_ROUTER_SENDSEND / 1024 | 16,768.5 | 12,433.0 | 14,018.0 | 13,382.5 |
| DEALER_ROUTER_SENDSEND / 65536 | 7,872.0 | 71.5 | 5,419.0 | 13.0 |
| PUBSUB / 1024 | 313,186.5 | 292,757.5 | 294,129.0 | 161,491.0 |
| PUBSUB / 65536 | 77,644.5 | 43,747.0 | 75,948.5 | 22,953.0 |

- single 원본: `/home/hep7hep7/project/zlink-work/c016/go-review-single-smoke.csv`
- multi 원본: `/home/hep7hep7/project/zlink-work/c016/go-review-multi-smoke-final.csv`

## 변경 파일

- `bindings/go/internal/native/completion_owner.go`
- `bindings/go/internal/native/dealer_router_request.go`
- `bindings/go/internal/native/completion_owner_test.go`
- `bindings/go/writable_retry_test.go`
- `bindings/go/perf/run_benchmarks.sh`
- `bindings/go/perf/run_benchmarks_multi.sh`
- `bindings/go/perf/multi/perf_multi_dealer_router.go`

## BLOCKERS

없음.
