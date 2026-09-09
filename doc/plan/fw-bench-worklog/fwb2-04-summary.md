# fwb2-04 결과 — Node server-driven bench runner

## 결과

`framework/bench/grpc/node/`의 측정 주체를 standalone client에서 source process A로 옮겼다.
runner는 각 셀에서 target B와 source A를 새 process로 시작하고, warmup·active HTTP trigger와
bounded settle을 차례로 수행한 뒤 B의 snapshot을 A의 `with-grpc-cell-v1` 원본에
`target_stats`로 병합한다.

`grpc-node`와 `zlink-node`의 `request-serial@1024`, `zlink-node`의
`send-saturation@1024` smoke는 rc=0이었다. `zlink-framework-node`는 공개 protobuf codec이
`bytes`를 보존하지 못하므로 process를 시작하지 않고 `unsupported` manifest에 기록했다.

`zlink-node request-window@1024`는 source 완료 220건, 오류 100건, target 수신 320건으로
기존 Node completion 전달 결함이 재현되어 rc=1이었다. runner의 count 대조가 이 불일치를
정상적으로 차단했다. 계획 §8 S2 지침에 따라 timeout·window·socket 수를 바꾸거나 재시도하지
않았고 3-run 측정도 수행하지 않았다.

- source A의 측정 구간은 outbound public call 직전부터 완료까지다.
- request 셀은 A의 완료 수와 B의 수신 수가 다르면 실패한다.
- `send-saturation` 집계 입력은 settle 뒤 `target_stats.received`다.
- source 셀은 `role`, 규격의 8필드 `trigger`, `streams`, `target_stats`를 기록한다.
- trigger HTTP 상태와 phase 규칙은 `shared/bench-http-application.js` 한 곳이 소유한다.
- Core, binding, Framework runtime, 공용 집계기와 보호 문서는 수정하지 않았다.

작업 시작과 종료 시 branch는 `main`이었다. 작업 시작 시 확인한 HEAD는
`16bb3feb2d2cf414824d84d32f65ac5364ab7e37`, 최종 점검 시 HEAD는
`9cebc5521483069874661184b27767737583530b`였다. 이 작업 밖에서 반영된 커밋과 기존 worktree
변경은 수정하거나 정리하지 않았다.

## Process 구성

| 구현 | source A | A 포트 | target B | B 포트 | 사용 모듈 |
|---|---|---|---|---|---|
| `grpc-node` | 공용 source가 logical stream 수만큼 unary stub을 만들고 B의 `Echo`·`Command`를 호출 | trigger `5220`, stats `5221` | 기존 `grpc-server`가 echo/count와 stats 제공 | gRPC `5222`, stats `5223` | `@grpc/grpc-js`, `@grpc/proto-loader` |
| `zlink-node` | request용 raw ROUTER 하나 또는 send용 raw ROUTER 8개가 B에 연결 | trigger `5225`, stats `5226` | 기존 `zlink-raw-server`가 request와 command ROUTER를 분리해 echo/count | request `5227`, command `5228`, stats `5229` | published `@zlink-systems/zlink` 0.17.6 |
| `zlink-framework-node` | codec 결함으로 시작하지 않음 | 예약 trigger `5232`, stats `5233` | codec 결함으로 시작하지 않음 | 예약 RouteMesh `5234`, stats `5235` | `@zlink-systems/nestjs`, `@zlink-systems/framework-codec-protobuf` |

runner는 시작 전과 셀 종료 뒤 TCP LISTEN 상태에서 `5220-5239` 전체가 비어 있는지 확인한다.
다른 listener가 있으면 포트를 바꾸지 않고 중단한다. 실제 셀 순서는 다음과 같다.

1. `5220-5239` LISTEN preflight
2. B 시작과 stats 응답 확인
3. A 시작과 `ready=true` 확인
4. `phase=warmup` trigger와 `phase=idle` 복귀 확인
5. `phase=active` trigger와 `phase=idle` 복귀 확인
6. A·B counter가 안정될 때까지 최대 30초 bounded settle
7. source 원본에 `target_stats` 병합, request이면 A 완료 수와 B 수신 수 대조
8. A·B process group 종료와 포트 해제 확인

## Trigger와 logical stream

`shared/bench-http-application.js`의 controller가 다음 규칙을 한 번만 구현한다.

- Node 표준 `http` 모듈로 `/bench/start`와 `/bench/stats`를 별도 listener에 제공한다.
- trigger body는 `runId`, `cellId`, `pattern`, `payloadBytes`, `phase`, `durationMs`,
  `requestWindow`, `sendConcurrency`만 허용한다.
- 같은 `runId/cellId/phase` trigger를 다시 받으면 최초 acknowledgement와 같은 값을 반환한다.
- phase는 `idle`에서 `warmup` 또는 `active`로 전이하고 성공하면 `idle`, 실패하면 `failed`가 된다.
- stats는 `ready`, `submitted`, `completed`, `errors`, `inFlight`, `currentInFlight`,
  `peakInFlight`와 실패 사유를 제공한다.

| 패턴 | `streams.count` | `streams.inFlightPerStream` | Node 구현 |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | Promise 순차 loop 하나 |
| `request-window` | 1 | 100 | 한 transport/socket의 logical window를 Promise worker 100개가 공유 |
| `request-backpressure` | 1 | `null` | application 상한 없이 Promise를 만들고 256회마다 event loop completion에 양보 |
| `send-saturation` | 8 | 1 | logical stream마다 Promise worker와 gRPC stub 또는 raw ROUTER 하나 |

raw 비교는 ROUTER↔ROUTER만 허용한다. `RUN_DEALER` 기본값은 `0`이고 다른 값은 preflight에서
거부한다.

## Framework unsupported 표현

집계기 `tools/benchagg/model.py`에는 normalized `Cell.status`가 있지만,
`tools/benchagg/readers.py`의 `_CELL_FIELDS`는 `with-grpc-cell-v1` 입력의 `status`와
`incomplete_reason`을 읽지 않는다. 따라서 source 모양의 셀에 `status: "unsupported"`만 넣으면
unsupported가 아니라 target record가 없는 `incomplete`가 된다.

runner는 Framework 셀을 실행·생성하지 않고 run root의 `unsupported.json`에 다음 별도 manifest를
쓴다.

```json
{
  "schema": "with-grpc-unsupported-v1",
  "cells": [
    {
      "implementation": "zlink-framework-node",
      "status": "unsupported",
      "reason": "framework-codec-protobuf has no bytes value kind; protobuf bytes body is encoded as an object and does not round-trip as bytes"
    }
  ]
}
```

집계기의 `--runs-glob '<OUTROOT>/*'`는 측정 셀 디렉터리만 읽으므로 이 manifest는 집계 입력에
들어가지 않는다. 즉 결과 표에는 잘못된 incomplete 셀을 만들지 않고, run 원본에는 생략 이유가
검증 가능하게 남는다.

## 변경 파일

| 파일 | 변경 내용 |
|---|---|
| `framework/bench/grpc/node/shared/bench-http-application.js` | 공용 trigger 검증, idempotent acknowledgement, phase controller, source HTTP listener 추가 |
| `framework/bench/grpc/node/client/main.js` | 공용 source A process, gRPC/raw transport adapter, route probe, `with-grpc-cell-v1` writer와 버전 metadata 구현 |
| `framework/bench/grpc/node/client/bench-core.js` | source counter·resource 계측, warmup/active 경계와 네 logical stream driver 구현 |
| `framework/bench/grpc/node/shared/bench-server-metrics.js` | target stats에 `ready`, `phase`, `received`, completion/in-flight 호환 필드 추가 |
| `framework/bench/grpc/node/grpc-server/main.js` | B 포트를 5222/5223으로 이동하고 request 수신도 계측 |
| `framework/bench/grpc/node/zlink-raw-server/main.js` | B 포트를 5227/5228/5229로 이동하고 request 수신도 계측 |
| `framework/bench/grpc/node/zlink-framework-server/main.js` | 예약 B 포트를 5234/5235로 이동하고 request 수신 계측 경로 정렬 |
| `framework/bench/grpc/node/run_local.sh` | 셀 matrix, LISTEN preflight, B→A lifecycle, HTTP trigger, settle·병합·count 대조, unsupported 기록 구현 |
| `doc/plan/fw-bench-worklog/fwb2-04-summary.md` | 구현·검증·결함·재검증 정보를 기록 |

수정 전/후 규칙 수: lifecycle·trigger·stream·result 소유권을 기준으로 **7 → 4**다. 수정 전에는
runner의 전체 B lifetime, client 내부 matrix와 transport lifetime, 패턴별 warmup/reset,
패턴별 stats 조회, 패턴별 drain, 결과 작성, unsupported 판단이 흩어져 있었다. 수정 후에는
runner가 셀 lifecycle·target 병합·unsupported, 공용 HTTP controller가 trigger phase,
source core가 pattern-to-stream과 source 계측, B가 echo/count snapshot을 각각 한 번만 소유한다.

## 언어별 문서 입력값

감독자가 계획 §5의 절 1~6으로 옮길 수 있도록 현재 구현값을 정리한다.

### 1. 비교 대상

| 구현 | request | send/command |
|---|---|---|
| `grpc-node` | `@grpc/grpc-js` `BenchService.Echo` unary callback API를 Promise로 감쌈 | `BenchService.Command` unary callback API와 `Empty` reply |
| `zlink-node` | raw ROUTER `socket.request(peer).message(...).timeout(...).submit()` | raw ROUTER `socket.send(peer).message(...).submit()` |
| `zlink-framework-node` | 공개 framework channel request API 예정, 현재 unsupported | 공개 framework channel send API 예정, 현재 unsupported |

gRPC와 raw 행은 같은 `BenchPayload` protobuf body와 body 앞의 29-byte metric header를 사용한다.
raw는 framework envelope와 protobuf body를 두 part로 전송한다.

### 2. 실행 방법

전체 matrix는 성능 티켓 안에서 실행한다.

```bash
bash scripts/perf/perf-ticket.sh submit -p 2 -o <owner> -d "node with-grpc run" -- \
  bash framework/bench/grpc/node/run_local.sh
```

한 셀의 예시는 다음과 같다.

```bash
bash scripts/perf/perf-ticket.sh submit -p 2 -o <owner> \
  -d "node raw request-window@1024" -- \
  env PAYLOADS=1024 SCENARIO=request-window IMPLEMENTATION=zlink-node \
  bash framework/bench/grpc/node/run_local.sh
```

| 입력 | 기본값 | 동작 |
|---|---|---|
| `RUNS` | `1` | runner process가 수행할 run 수. 측정 티켓은 run 단위 사용 |
| `RUN_DEALER` | `0` | 비교 계약상 `0`만 허용 |
| `DURATION` | `5` | active 시간(초) |
| `WARMUP` | `1000` | active 전에 수행할 호출 수 |
| `PAYLOADS` | `1024,4096` | 실행 payload 목록. 두 값 이외에는 preflight 실패 |
| `SCENARIO` | `all` | `all`, `request`, 네 exact pattern, `send`·`command` alias |
| `IMPLEMENTATION` | `all` | `all` 또는 세 exact 구현 이름 |
| `WINDOW` | `100` | request-window logical in-flight. 다른 값은 preflight 실패 |
| `STAMP` | 현재 시각 | run ID와 기본 결과 경로 식별자 |
| `OUTROOT` | `framework/bench/grpc/log/node/with_grpc_node_<stamp>` | run 결과 root |
| `SKIP_BUILD` | `0` | `1`이면 runner 안의 `npm ci`와 `npm run build` 생략 |

고정 조건은 send concurrency 8, process 상한 300초, route/request/drain 상한 30초,
settle quiet 구간 200ms다.

### 3. Process 구성

위의 “Process 구성” 표와 셀 순서를 사용한다. trigger client는 runner의 `curl`이며 workload를
생성하지 않는다. 각 지원 셀은 새 A/B process pair를 사용하고 Framework unsupported 셀은 process를
시작하지 않는다.

### 4. 언어별 값

| 항목 | 값 |
|---|---|
| warmup | 기본 1000회, smoke 100회 |
| active | 기본 5초, smoke 2초 |
| gRPC source | logical stream 수만큼 `@grpc/grpc-js` unary client stub, insecure loopback, 별도 tuning 없음 |
| gRPC target | `@grpc/grpc-js Server`, default options, insecure loopback |
| Node runtime | 최종 smoke `v22.23.2` |
| gRPC package | `@grpc/grpc-js` 1.14.4, `@grpc/proto-loader` 0.7.15 |
| ZLink binding | published `@zlink-systems/zlink` 0.17.6 |
| Framework | workspace `@zlink-systems/framework`, codec, NestJS adapter 0.10.0 |
| 실행 OS/kernel | WSL2 kernel `6.6.87.2-microsoft-standard-WSL2` |
| 실행 CPU | Intel Core Ultra 7 265K |

### 5. 결과 위치

```text
<OUTROOT>/
├── with_grpc_node_<stamp>.txt
├── unsupported.json
└── <implementation>-<pattern>-<payload>-run<run>/
    ├── results.json
    ├── report.txt
    ├── source.log
    ├── target.log
    └── target-stats.json
```

`results.json`은 `schema=with-grpc-cell-v1`과 source `cells[]`를 가지며 runner가 같은 셀에
`target_stats`를 병합한다. 집계 예시는 다음과 같다.

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang node \
  --runs-glob '<OUTROOT>/*' --format spec4
```

### 6. 알려진 제약

- `zlink-framework-node`의 모든 셀은 공개 protobuf codec이 `bytes`를 표현하지 못해
  `unsupported`다.
- `zlink-node request-window`는 0.17.6에서도 completion 전달 결함이 재현되어 유효한 결과를
  만들지 못한다.
- raw 비교는 ROUTER↔ROUTER로 고정하며 DEALER 보조 run은 없다.
- smoke만 수행했다. 공개 비교값과 G5 판정에 필요한 3-run은 blocker 해결 뒤 감독자가 수행해야
  한다.

## Build와 정적 검증

| 검증 | 결과 |
|---|---|
| `cd framework/bench/grpc/node && npm ci` | rc=0, 34 packages, 취약점 0 |
| 같은 디렉터리의 `npm run build` | rc=0; framework workspace TypeScript/browser build 포함. audit는 기존 6건(중간 5, 높음 1) 보고 |
| `bash -n framework/bench/grpc/node/run_local.sh` | rc=0 |
| 변경·공유 JavaScript 전체 `node --check` | rc=0 |
| phase controller 중복 trigger/phase 소단위 검사 | rc=0, 동일 acknowledgement와 `active → idle` 확인 |
| `git diff --check`와 신규 파일 whitespace 검사 | rc=0 |
| 성공 smoke 원본을 `bench_aggregate.py --lang node ... --format spec4`로 읽기 | rc=0, request와 send spec4 표 출력 |

build 시작 전 load average는 각각 5.67(`npm ci`)과 5.10(`npm run build`)으로 10 미만이었고,
한 번에 하나씩 실행했다.

## Smoke 검증

모든 bench smoke는 `bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-04 ...`로 제출해
완료를 기다렸다.

| 셀 | ticket | rc | RESULT throughput | source 완료 / target 수신 | 오류(source / target) | stream | drain |
|---|---|---:|---:|---:|---:|---|---:|
| `grpc-node request-serial@1024` | `2-1788940030-54263-sol-fwb2-04-node_server-driven_grpc_raw_request-seri` | 0 | `2511.500/s` | `5023 / 5023` | `0 / 0` | `1×1` | 274ms |
| `zlink-node request-serial@1024` | 같은 ticket | 0 | `8673.000/s` | `17346 / 17346` | `0 / 0` | `1×1` | 278ms |
| `zlink-framework-node request-serial@1024` | 같은 ticket | 0 | `UNSUPPORTED` | 실행하지 않음 | 해당 없음 | 해당 없음 | 해당 없음 |
| `zlink-node send-saturation@1024` | `2-1788940861-37306-sol-fwb2-04-node_server-driven_raw_send-saturation_s` | 0 | A boundary `223644.000/s`; settle 집계 `311200.500/s` | `622401 / 622401` | `0 / 0` | `8×1` | 1256ms |
| `zlink-node request-window@1024` | `2-1788940924-41711-sol-fwb2-04-node_server-driven_raw_request-window_sm` | **1** | ticket에는 없음; source 원본 `110.000/s` | **`220 / 320`** | **`100 / 0`** | `1×100` | 279ms |

성공한 source 원본은 trigger 8필드가 정확히 존재했고 `role=source`, `streams`, 병합된
`target_stats`를 모두 포함했다. `request-window`도 같은 원본 구조와 `peak_in_flight=100`을
남겼으나 count 불일치 때문에 유효한 셀로 집계하지 않았다.

성공한 serial·send 원본만 집계기에 넣은 `spec4` 결과는 request에서 `grpc-node` 2.51 KOPS,
`zlink-node` 8.67 KOPS, send에서 `zlink-node` 311.20 KMSG/s였다. smoke 값은 성능 판정에
사용하지 않는다.

## Runtime 결함

### Node raw request completion 전달 회귀

- 재현: 공개 API 호출은 `framework/bench/grpc/node/client/main.js:154-159`, completion을 기다리는
  100개 Promise는 `client/bench-core.js:275-286`이다. B가 active request 320건을 받았지만 A는
  220건만 완료하고 나머지 100건이 30초 timeout으로 끝났다.
- 소유 계층: Core의 request/reply 계약을 Promise로 전달하는 **Node binding completion owner**다.
  우선 조사 경계는 `bindings/node/src/zlink/runtime/messaging/completion_owner.ts:662-675`의
  `ensureRuntimeWatch()`와 `runtimeWake()`다. 이 줄을 원인으로 확정한 것은 아니며, 이 작업에서
  runtime 진단·수정은 수행하지 않았다.
- spec 조항: `core/doc/spec/core/socket/07-router.ko.md` §13 “Request completion”은 admission된
  FINAL이 reply·timeout·terminal 중 정확히 하나의 REQUEST completion으로 끝나야 한다고 정한다.
- 교차언어 대조: C와 C++는 같은 깊이 100을 오류 없이 지탱했고, fwb2-03의 .NET framework
  `request-window@1024`도 `5990/5990`, 오류 0으로 통과했다. 현재 증거는 Core 공통 경로보다
  Node 관리형 binding의 완료 전달 경계를 가리킨다.
- 변경 분류: **B — 기존 결함의 회귀**. 계획이 0.17.5에서 수정됐다고 기록한 FB-026이 published
  binding 0.17.6 smoke에서 다시 나타났다.

### Node Framework protobuf bytes 미지원

- 원인: `framework/languages/node/packages/framework-codec-protobuf/src/dynamic-value-wire.ts:7-13`의
  `ValueKind`에 bytes가 없고, `:58-63`에서 Buffer가 object로 인코딩되며, `:103-109`에서도
  bytes로 복원할 분기가 없다.
- 소유 계층: Node Framework protobuf codec.
- spec 조항: bench 규격 §2의 payload는 protobuf `bytes body` 크기여야 하므로 다른 payload나
  비공개 codec으로 우회할 수 없다.
- 교차언어 대조: .NET Framework protobuf codec 행은 같은 bytes payload로 fwb2-03 smoke를
  통과했다. Node codec의 구조적 차이다.
- 변경 분류: **B — 알려진 제품 결함**. 이 작업에서는 고치지 않고 Framework 행 전체를
  `unsupported`로 기록했다.

## BLOCKERS

- `zlink-node request-window@1024`가 count invariant를 위반해 필수 smoke rc=0 조건을 충족하지
  못했다. Node binding completion 전달 결함을 별도 runtime 작업에서 고치고 같은 ticket smoke를
  통과하기 전에는 3-run을 실행할 수 없다.
- Framework 행은 codec bytes 지원 전까지 계획대로 `unsupported`이며 Node 3자 비교에는 참여할
  수 없다.

## 감독자 재검증 명령

build 전에 load average를 확인하고 다음 명령을 한 번에 하나씩 실행한다.

```bash
cut -d' ' -f1 /proc/loadavg
cd framework/bench/grpc/node
npm ci
npm run build
cd ../../../..
bash -n framework/bench/grpc/node/run_local.sh
git diff --check
```

smoke는 반드시 ticket으로 실행한다.

```bash
bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-04 \
  -d "review: node grpc/raw request-serial and framework unsupported" -- \
  env SKIP_BUILD=1 RUNS=1 RUN_DEALER=0 DURATION=2 WARMUP=100 PAYLOADS=1024 \
  SCENARIO=request-serial IMPLEMENTATION=all STAMP=fwb2-04-review-serial \
  OUTROOT=/tmp/zlink-sol-fwb2-04/review-serial \
  bash framework/bench/grpc/node/run_local.sh

bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-04 \
  -d "review: node raw send-saturation" -- \
  env SKIP_BUILD=1 RUNS=1 RUN_DEALER=0 DURATION=2 WARMUP=100 PAYLOADS=1024 \
  SCENARIO=send-saturation IMPLEMENTATION=zlink-node STAMP=fwb2-04-review-send \
  OUTROOT=/tmp/zlink-sol-fwb2-04/review-send \
  bash framework/bench/grpc/node/run_local.sh

bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-04 \
  -d "review: node raw request-window regression" -- \
  env SKIP_BUILD=1 RUNS=1 RUN_DEALER=0 DURATION=2 WARMUP=100 PAYLOADS=1024 \
  SCENARIO=request-window IMPLEMENTATION=zlink-node STAMP=fwb2-04-review-window \
  OUTROOT=/tmp/zlink-sol-fwb2-04/review-window \
  bash framework/bench/grpc/node/run_local.sh
```

성공한 serial·send 원본의 집계 확인:

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang node \
  --runs-glob '/tmp/zlink-sol-fwb2-04/review-serial/*' \
  --runs-glob '/tmp/zlink-sol-fwb2-04/review-send/*' \
  --payload-sizes 1024 --format spec4
```
