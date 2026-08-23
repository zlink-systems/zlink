# Node binding 0.13.0 계약 재정렬

## 요약

Node binding의 send/request/publish 경로를 `async-coroutine-policy.ko.md` 3차
개정과 Core 0.13.0 API에 맞췄다. binding은 send-complete와 reply callback을
native TSFN으로 JavaScript에 전달할 뿐, 자체 thread·queue·retry·취소 API를
소유하지 않는다.

## 구현

- `bindings/node/native/src/addon_core.cc`에 socket당 하나의
  `zlink_send_complete_handler`를 설치했다. Core callback은 userdata operation과
  token을 보존하고, completion event만 TSFN으로 전달한다. JS callback은 token으로
  pending Promise를 resolve/reject하며 native callback 안에서 submit하지 않는다.
  Core submit 중 inline completion이 발생하면 native가 event를 반환하고 TypeScript가
  TSFN 왕복 없이 처리한다.
- PAIR send와 DEALER/ROUTER/STREAM managed routed send는
  `zlink_send_async`의 per-operation timeout을 사용한다. `ADMITTED`는
  `Promise<void>`를 resolve하고 `TIMED_OUT`/`TERMINAL`은 Core errno를 보존한
  `SubmitError`로 reject한다. Promise를 버리는 것은 취소가 아니며 별도 cancel
  surface를 추가하지 않았다.
- `addon_routed_admission.cc`, `routed_admission.ts`, `publisher_admission.ts`,
  `request_progress.ts`와 관련 readiness/대기 machinery를 삭제했다. source-layout
  test는 파일 부재뿐 아니라 native/TS bridge가 Core completion 경로를 사용하는지도
  검사한다.
- `bindings/node/include` vendored header directory는 존재하지 않아 별도 header
  resync 대상이 없었다.
- PUB/XPUB `publish(...).submit()`은 동기 `void` 종단이다. lossy PUB의 성공은
  즉시 반환하고 NODROP 실패는 즉시 `SubmitError`로 전달한다.
- DEALER/ROUTER request는 binding admission이나 progress timer 없이 Core reply
  callback만으로 `Promise<Message[]>`를 settle한다. raw reply와 STREAM raw
  relay는 기존 동기 one-shot 경로를 유지한다.
- Node message ownership은 기존 동기 submit과 같이 Core submit 성공 시 원본을
  consume한다. 실패한 동기 publish/request submit에서는 원본을 보존한다.
- Core 0.13.0의 알려진 routed multipart async 결함 범위에 맞춰
  `tests/routed_async_admission.test.ts`의 multipart async assertion은 1-part로
  제한하고, Core 수정이 반영되면 복원할 주석을 남겼다.

## Zero-thread/queue/retry 점검

send completion과 request reply에 사용하는 것은 N-API TSFN delivery뿐이다. 새
binding-owned worker thread, polling timer, admission queue, 재시도 loop는 없다.
기존 monitor/stream/event callback의 N-API delivery 시설과 Core socket option은
이번 경로의 binding retry로 해석하지 않는다.

```text
$ rg -n -i 'send[_-]?ready|sendReady' bindings/node/src bindings/node/native/src
(no matches)

$ rg -n 'std::thread|std::jthread|pthread|uv_thread' \
    bindings/node/src bindings/node/native/src
(no binding-owned thread matches)
```

## 테스트 결과

사용한 Core는 `/tmp/zlink-node-core`의 기존 설치 prefix이며 Core 자체는 재빌드하지
않았다.

- `npm run build`: 통과
- `npm run typecheck`: 통과
- `npx tsc -p tsconfig.tools.json --noEmit`: 통과
- `ZLINK_CORE_INSTALL_PREFIX=/tmp/zlink-node-core npm run rebuild-native`: 통과
  (기존 `router_recv_parts` 미사용 warning 1건)
- `ZLINK_CORE_INSTALL_PREFIX=/tmp/zlink-node-core ZLINK_CORE_SOURCE=release
  ZLINK_CORE_PACKAGE_PREFIX=/tmp/zlink-node-core npm test`: retained TCP test의
  첫 실패에서 중단. 개별 raw test 실행에서는 13개 파일이 통과했고
  `retained_hwm_credit.test.js`, `stream.test.js`만 아래 환경 오류로 실패했다.
- 실패 원인: 두 파일의 TCP 테스트가 `net.Server.listen(0, "127.0.0.1")`에서
  `listen EPERM`을 받았다. inproc send, request, HWM timeout/terminal,
  source-layout, multipart 및 나머지 binding test assertion은 통과했다.

### 기준선 비교

세션의 `.git` metadata가 읽기 전용이어서 범위 지정 `git stash`는 `rc=1`로
실패했고 working tree는 변경되지 않았다. 같은 목적의 안전한 대체 검증으로
`git archive HEAD`를 `/tmp/zlink-node-baseline.ufcs8C`에 풀고 Node
`node_modules`만 연결했다. 기준선의 `npm run build`는 통과했지만
`ZLINK_CORE_INSTALL_PREFIX=/tmp/zlink-node-core npm run rebuild-native`는 구
addon의 `zlink_send_ready_handler`가 Core 0.13.0 헤더에 없어 컴파일 실패했다.
따라서 이 기준선에서는 binding test 실행까지 진행할 수 없었으며, 해당 native
실패는 재정렬 전부터 존재하는 break로 분리했다.

## Perf smoke

두 실행 모두 요구한 `tcp`, duration `1`, local Core default를 사용했다. 결과 파일의
`status`가 요구 조건인 `complete`가 아니므로 throughput 수치는 기록하지 않고
실행 전 listen 실패를 기록한다.

### Single suite

명령: `npm run perf:single -- --reuse-build --pattern ALL --transports tcp
--msg-size 64 --duration 1 --runs 1`

결과 파일:
`bindings/node/perf/results/single/report/perf_node_single_linux_20260824_031331_node-realignment-final-single.txt`

| Pattern | Transport | Size | Result |
|---|---|---:|---|
| PAIR | tcp | 64B | FAIL — listen EPERM |
| PUBSUB | tcp | 64B | FAIL — listen EPERM |
| DEALER_DEALER | tcp | 64B | FAIL — listen EPERM |
| DEALER_ROUTER | tcp | 64B | FAIL — listen EPERM |
| DEALER_ROUTER_REQREP | tcp | 64B | FAIL — listen EPERM |
| ROUTER_ROUTER | tcp | 64B | FAIL — listen EPERM |
| ROUTER_ROUTER_REQREP | tcp | 64B | FAIL — listen EPERM |

report completion: `status: partial`, `expected_result_lines: 35`,
`actual_result_lines: 0` (7 pattern/transport/size 실행 모두 실패).

### Multi suite

명령: `npm run perf:multi -- --reuse-build --transports tcp --duration 1`

결과 파일:
`bindings/node/perf/results/multi/report/perf_node_multi_linux_20260824_031357_node-realignment-final-multi.txt`

| Pattern | Sizes | Result |
|---|---|---|
| MULTI_DEALER_DEALER | 64, 256, 1024, 4096, 65536, 131072B | 6/6 FAIL — listen EPERM |
| MULTI_DEALER_ROUTER | 64, 256, 1024, 4096, 65536, 131072B | 6/6 FAIL — listen EPERM |
| MULTI_ROUTER_ROUTER | 64, 256, 1024, 4096, 65536, 131072B | 6/6 FAIL — listen EPERM |
| MULTI_PUBSUB | 64, 256, 1024, 4096, 65536, 131072B | 6/6 FAIL — listen EPERM |
| MULTI_STREAM | 64, 256, 1024, 65536B | 4/4 FAIL — listen EPERM |

report completion: `success: 0`, `unsupported: 0`, `skip: 0`, `fail: 28`,
`status: partial`, `expected_result_lines: 140`, `actual_result_lines: 0`.

## Flagged items

1. 현재 sandbox는 loopback TCP listen을 `EPERM`으로 차단한다. network-enabled
   환경에서 위 두 smoke를 재실행해 `status: complete`와 실제 per-pattern
   수치를 확인해야 한다.
2. Core의 routed multipart async 결함(ROUTER 2-part abort, DEALER generic
   target 2-part `NOT_FOUND`)은 병렬 Core 수정 범위다. Node test는 해당 경로를
   1-part로 제한했으며 Node에서 Core를 수정하지 않았다.
3. baseline stash는 `.git` read-only 제약으로 생성하지 못했다. 대신 clean HEAD
   archive baseline의 native compile break를 측정했고, 작업 tree의 기존 변경은
   보존했다.

## REQREP 수정 (2026-08-24, network-enabled 재실행)

### 증상

Sandbox 밖 재실행(`perf_node_single_linux_20260824_033024.txt`)에서 PAIR /
PUBSUB / DEALER_DEALER / DEALER_ROUTER / ROUTER_ROUTER는 전 transport 통과했으나
`DEALER_ROUTER_REQREP`, `ROUTER_ROUTER_REQREP`만 6개 transport 전부 `FAIL`,
요약 사유는 `timeout`이었다.

### 근본 원인

`bindings/node/perf/single/perf_socket_reqrep.ts:198`

```
.timeout(options.recvTimeoutMs ?? 200).submit();
```

`parseSingleBinaryArgs`(`perf/single/perf_single_common.ts:677`)는
`PERF_SINGLE_RCVTIMEO_MS`가 설정되지 않으면 `recvTimeoutMs`를 `NaN`으로 채운다.
`NaN`은 nullish가 아니므로 `??`가 200으로 대체하지 못하고 `NaN`이 그대로
`RuntimeRequestOperation.timeout()`에 전달된다. 해당 builder는
`socket_operation_builders.ts`에서 `Number.isInteger` 검증을 하므로
`TypeError: timeoutMs must be an integer`를 던지고, 첫 request 제출 전에
프로세스가 종료된다. single runner는 실패 메시지에 `timeout` 문자열이 들어가면
사유를 `timeout`으로 분류하므로(`perf/single/run_benchmarks.ts:391`) 보고서에
30s 타임아웃처럼 기록됐다. 실제로는 즉시 실패다.

Promise 결선 자체는 정상이었다. `registerNativeRequest`는 Core submit 이전에
correlation을 설치하며(`runtime/messaging/request_executor.ts:59`),
`dealer_request`/`router_request`도 `create_core_request_js_state`로 TSFN 상태를
submit 전에 확보한다(`native/src/addon_core.cc:2890`). 즉 0379a8b6c1의 C++
register-after-submit race는 Node에는 존재하지 않았다.

Node runtime/native 코드 변경은 없었다. 회귀 유입 시점은 재정렬이 아니라
`Number.isInteger` 검증을 도입한 c2c5d5db1c/2fb9ced504이며, 그 이전 통과
기록(`..._20260812_024608_...`)은 검증 도입 전이라 `NaN`이 그대로 통과했다.

### 수정

`perf/single/perf_socket_reqrep.ts`에서 timeout을 루프 밖에서 한 번만 정규화한다.

```
const requestTimeoutMs = Number.isFinite(options.recvTimeoutMs)
  ? Math.trunc(options.recvTimeoutMs)
  : 200;
...
.timeout(requestTimeoutMs).submit();
```

이는 같은 파일의 다른 옵션 처리(`perf_single_common.ts:107`)와 동일한
`Number.isFinite` 규약이다. 계약 의미는 변하지 않는다 — completion 전달만
사용하고 binding thread를 추가하지 않는다.

### 증거

- binding test suite: `ZLINK_BINDING_RAW_TEST_ONLY=1 ./tests/run_tests.sh` —
  15개 test 파일, 71 pass / 0 fail.
- single REQREP smoke (local Core `core/build/lib/libzlink.so.0.13.0`,
  duration 1, 64B, 6 transport):
  `bindings/node/perf/results/single/report/perf_node_single_linux_20260824_074059_reqrep-fix-smoke.txt`
  — `status: complete`, `expected_result_lines: 60`, `actual_result_lines: 60`.

  | Pattern | inproc | ipc | tcp | tls | ws | wss |
  |---|---:|---:|---:|---:|---:|---:|
  | DEALER_ROUTER_REQREP | 66.10 Kops/s | 54.03 | 38.35 | 49.41 | 59.28 | 59.17 |
  | ROUTER_ROUTER_REQREP | 65.13 Kops/s | 54.09 | 53.67 | 57.03 | 58.23 | 49.32 |

  0.10.1 기준선(`..._20260812_024608_...`, tcp 64B 64.32 Kops/s)과 같은 자리수다.
- multi smoke (MULTI_DEALER_DEALER / MULTI_DEALER_ROUTER / MULTI_ROUTER_ROUTER /
  MULTI_PUBSUB, tcp, 64B, duration 1): `success: 4`, `fail: 0`,
  `status: complete`.

## MULTI_STREAM 진단

판정: **환경 fan-out 한계가 아니라 Node 경로의 결함이다.** 단, Node 재정렬이
유입시킨 회귀는 아니다.

측정(local Core 0.13.0, tcp, 64B, 동일 머신):

| 요청 client 수 | connect_ok | 결과 |
|---:|---:|---|
| 500 | 500 | PASS |
| 900 | 896 | FAIL (connect_fail=4) |
| 1200 | 896 | FAIL (connect_fail=304) |
| 2000 | 896 | FAIL (connect_fail=1104) |
| 10000 | — | harness memory guard가 SKIP (max_clients=4660) |

- 상한이 정확히 896으로 고정된다. connect concurrency(64/256/1024),
  server io_threads(1/4/8), auto-HWM on/off, `PERF_MAX_SOCKETS=10096`,
  SNDTIMEO/RCVTIMEO(200/5000ms) 어느 것에도 반응하지 않는다.
- 초과 peer는 TCP accept 후 즉시 닫힌다 — client가 첫 6-byte header 이전에
  `read_header_error code=2 End of file`을 본다(connect refused가 아니다).
- 서버 측에서도 동일하게 관측된다: STREAM packet handler가 보는 고유
  routing id 수가 정확히 896에서 멈춘다.
- 환경 한계가 아님의 결정적 근거: 같은 머신에서 같은 C client
  (`bindings/c/build/perf/perf_stream_client`, `--ccu 2000`)로 C STREAM 서버
  (`comp_src_stream_server`)를 때리면 정상 통과한다
  (`RESULT,current,MULTI_STREAM,tcp,64,throughput,290472`). 즉 kernel backlog /
  fd / ephemeral port 한계가 아니다.
- 재정렬 회귀가 아님의 근거: `perf/multi/perf_multi_stream_server.ts`는 HEAD와
  동일(diff 없음)하고, STREAM 재정렬은 send admission
  (`RoutedAdmission` → `SendCompletionOwner`)만 교체했다. 이 경로는 peer가
  연결된 뒤에야 도달하므로 accept 단계 상한과 무관하다.
- 보고서상 10000-client 기본값은 harness의 memory guard(가용 메모리 기준
  per-client 1MB 예산)에 따라 환경 조건에 따라 SKIP되기도 한다. 이는 위
  896 상한과는 별개의 환경 요인이다.

후속 범위: 896 상한의 정확한 위치는 addon STREAM packet-handler 경로 또는
Core의 STREAM peer admission이며, 이번 REQREP 작업(bindings/node perf harness)
범위를 벗어난다.
