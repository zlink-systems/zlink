# Node.js messaging bench

이 문서는 [규격](../README.ko.md)의 server-driven 모델(§10)을 Node.js에서 어떻게 구현했는지와 실행
방법을 적는다. 비교 대상·패턴·단위·판정 규칙은 규격이 소유하고, 이 문서는 Node 고유의 값만 담는다.

## 1. 비교 대상

| 구현 | request | send/command | 사용 API |
|---|---|---|---|
| `grpc-node` | `BenchService.Echo` unary(callback을 Promise로 감쌈) | `BenchService.Command` unary, `Empty` reply | `@grpc/grpc-js`, `@grpc/proto-loader` |
| `zlink-node` | raw ROUTER `socket.request(peer).message(...).timeout(...).submit()` | raw ROUTER `socket.send(peer).message(...).submit()` | published `@zlink-systems/zlink` |
| `zlink-framework-node` | framework channel request(예정) | framework channel send(예정) | `@zlink-systems/framework`, `@zlink-systems/nestjs` — 현재 전 셀 `unsupported`(§6) |

gRPC와 raw는 같은 `BenchPayload` protobuf body와 body 앞의 29-byte header(규격 §6)를 쓴다. raw는
framework envelope와 protobuf body를 두 part로 보낸다.

## 2. 실행 방법

```bash
# 전체 matrix — 항상 perf 티켓 큐로
bash scripts/perf/perf-ticket.sh submit -p 1 -o <owner> -d "node with-grpc run" -- \
  bash framework/bench/grpc/node/run_local.sh

# 한 셀
env PAYLOADS=1024 SCENARIO=request-window IMPLEMENTATION=zlink-node \
  bash framework/bench/grpc/node/run_local.sh
```

| 입력 | 기본값 | 동작 |
|---|---|---|
| `RUNS` | `1` | runner process가 수행할 run 수(측정 티켓은 run 단위) |
| `RUN_DEALER` | `0` | 비교 계약상 `0`만 허용 |
| `DURATION` | `5` | active 시간(초) |
| `WARMUP` | `1000` | active 전 warmup 호출 수 |
| `PAYLOADS` | `1024,4096` | payload 목록. 두 값 밖은 preflight 실패 |
| `SCENARIO` | `all` | `all`, `request`, 네 패턴 이름 |
| `IMPLEMENTATION` | `all` | `all` 또는 세 구현 이름 |
| `WINDOW` | `100` | `request-window` in-flight. 다른 값은 preflight 실패 |
| `STAMP` | 실행 시각 | run ID와 결과 경로 |
| `OUTROOT` | `framework/bench/grpc/log/node/with_grpc_node_<stamp>` | run root |
| `SKIP_BUILD` | `0` | `1`이면 `npm ci`·`npm run build` 생략 |

고정값: send concurrency 8, process 상한 300초, route/request/drain 상한 30초, settle quiet 200ms.

## 3. 프로세스 구성

| 구현 | source A | A 포트(trigger/stats) | target B | B 포트 |
|---|---|---|---|---|
| `grpc-node` | `client/main.js` — logical stream 수만큼 unary stub | 5220/5221 | `grpc-server/main.js` — echo/count | gRPC 5222, stats 5223 |
| `zlink-node` | `client/main.js` — request용 raw ROUTER 1개 또는 send용 8개 | 5225/5226 | `zlink-raw-server/main.js` — request·command ROUTER 분리 | request 5227, command 5228, stats 5229 |
| `zlink-framework-node` | 시작하지 않음(unsupported) | 5232/5233(예약) | `zlink-framework-server/main.js` | RouteMesh 5234, stats 5235(예약) |

trigger·stats·phase 규칙은 `shared/bench-http-application.js` 한 곳(Node 표준 `http`)이 소유한다.
trigger client는 runner의 `curl`이며 부하를 만들지 않는다. 셀 순서는 규격 §10.4 그대로이고 셀마다
새 A/B process 쌍을 쓴다. runner는 시작 전과 셀 종료 뒤 5220-5239의 LISTEN 소켓을 확인하고,
있으면 포트를 바꾸지 않고 중단한다.

| 패턴 | `streams.count` | `streams.inFlightPerStream` | Node 구현 |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | Promise 순차 loop 하나 |
| `request-window` | 1 | 100 | 한 socket의 logical window를 Promise worker 100개가 공유 |
| `request-backpressure` | 1 | 없음 | 상한 없이 Promise 생성, 256회마다 event loop에 양보 |
| `send-saturation` | 8 | 1 | stream마다 Promise worker와 gRPC stub 또는 raw ROUTER 하나 |

## 4. 언어별로 다르게 둔 값

| 항목 | 값 |
|---|---|
| warmup | 1000회 (smoke 100회) |
| gRPC source | `@grpc/grpc-js` unary client stub, insecure loopback, 별도 tuning 없음 |
| gRPC target | `@grpc/grpc-js` `Server` 기본 옵션, insecure loopback |
| Node runtime | v22.23.2 (2026-09-09 측정) |
| gRPC package | `@grpc/grpc-js` 1.14.4, `@grpc/proto-loader` 0.7.15 |
| ZLink binding | published 0.17.6 |
| framework | workspace 소스(측정한 커밋을 원본에 기록) |

## 5. 결과 위치

```text
<OUTROOT>/
├── with_grpc_node_<stamp>.txt
├── unsupported.json            # 실행하지 않은 셀과 이유
└── <implementation>-<pattern>-<payload>-run<run>/
    ├── results.json            # with-grpc-cell-v1: role·trigger·streams·target_stats
    ├── report.txt
    ├── source.log / target.log
    └── target-stats.json
```

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang node --judgement-pattern request-window \
  --runs-glob '<run-1>/*' --runs-glob '<run-2>/*' --runs-glob '<run-3>/*' \
  --runs-glob 'framework/bench/grpc/log/c/<c-run>*' --format full
```

## 6. 알려진 제약

- `zlink-framework-node`의 모든 셀은 `unsupported`다. framework의 protobuf codec이 `bytes`를
  보존하지 못한다(제품 결함, 별도 작업). runner는 process를 시작하지 않고 `unsupported.json`에
  이유를 남긴다.
- `zlink-node request-window`는 binding 0.17.6에서 completion 유실(결정 기록 FB-049)이 재현되어
  오류 셀로 기록된다. 결함 수정 전에는 그 행을 게재하지 않는다.
- raw 비교는 ROUTER↔ROUTER로 고정한다(DEALER 보조 run 없음).
