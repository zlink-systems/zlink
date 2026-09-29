# Node.js messaging bench

이 문서는 [규격](../README.ko.md)의 server-driven 모델(§10)을 Node.js에서 어떻게 구현했는지와 실행
방법을 적는다. 비교 대상·패턴·단위·판정 규칙은 규격이 소유하고, 이 문서는 Node 고유의 값만 담는다.

## 1. 비교 대상

| 구현 | request | send/command | 사용 API |
|---|---|---|---|
| `grpc-node` | `BenchService.Echo` unary(callback을 Promise로 감쌈) | `BenchService.Command` unary, `Empty` reply | `@grpc/grpc-js`, `@grpc/proto-loader` |
| `zlink-node` | raw ROUTER `socket.request(peer).message(...).timeout(...).submit()` | raw ROUTER `socket.send(peer).message(...).submit()` | published `@zlink-systems/zlink` |
| `zlink-framework-node` | framework channel request | framework channel send | `@zlink-systems/framework`, `@zlink-systems/nestjs` |

gRPC와 raw는 같은 `BenchPayload` protobuf body와 body 앞의 29-byte header(규격 §6)를 쓴다. raw는
framework envelope와 protobuf body를 두 part로 보낸다.

## 2. 실행 방법

입력은 규격 [§11](../README.ko.md#11-runner-입력과-결과-배치)의 환경 변수가 전부다. CLI 옵션과 위치 인자는 없다. runner는 `SKIP_BUILD=1`이 아니면 먼저 `npm ci`와 `npm run build`를 실행한다.
측정은 항상 perf 티켓 큐로 낸다.

```bash
# 전체 matrix
bash scripts/perf/perf-ticket.sh submit -p 1 -o <owner> -d "node with-grpc run" -- \
  bash framework/bench/grpc/node/run_local.sh

# 한 셀
IMPLEMENTATIONS=zlink-node PATTERNS=request-serial PAYLOAD_SIZES=1024 RUNS=1 \
  bash framework/bench/grpc/node/run_local.sh
```

warmup 호출 수, process 상한, route·request·drain 상한과 settle quiet 구간은 runner가 정하는
고정값이다.

## 3. 프로세스 구성

| 구현 | source A | A 포트(trigger/stats) | target B | B 포트 |
|---|---|---|---|---|
| `grpc-node` | `client/main.js` — logical stream 수만큼 unary stub | 5220/5221 | `grpc-server/main.js` — echo/count | gRPC 5222, stats 5223 |
| `zlink-node` | `client/main.js` — request용 raw ROUTER 1개 또는 send용 8개 | 5225/5226 | `zlink-raw-server/main.js` — request·command ROUTER 분리 | request 5227, command 5228, stats 5229 |
| `zlink-framework-node` | `client/main.js` | 5232/5233 | `zlink-framework-server/main.js` | RouteMesh 5234, stats 5235 |

trigger·stats·phase 규칙은 `shared/bench-http-application.js` 한 곳(Node 표준 `http`)이 소유한다.
trigger client는 runner의 `curl`이며 부하를 만들지 않는다. 셀 순서는 규격 §10.4 그대로이고 셀마다
새 A/B process 쌍을 쓴다. runner는 시작 전과 셀 종료 뒤 5220-5239의 LISTEN 소켓을 확인하고,
있으면 포트를 바꾸지 않고 중단한다.

| 패턴 | `streams.count` | `streams.inFlightPerStream` | Node 구현 |
|---|---:|---:|---|
| `request-serial` | 1 | 1 | Promise 순차 loop 하나 |
| `request-backpressure` | 1 | 없음 | 상한 없이 Promise 생성, 256회마다 event loop에 양보 |
| `send-saturation` | 8 | 1 | stream마다 Promise worker 하나. **연결은 세 행 모두 하나다** — gRPC는 채널 하나를 stub 8개가 공유하고, raw는 ROUTER 하나를 stream 8개가 공유하며, framework는 RouteMesh socket 하나다 |

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

결과는 규격 §11의 배치(`<OUTPUT_DIR>/run<N>/<implementation>-<pattern>-<payload>/results.json`)로
쓴다. 셀 안에는 diagnostics인 `source.log`, `target.log`, `target-stats.json`도 남는다.
표와 판정은 집계기만 만든다. 전 언어를 한 번에 재고 보고서까지 만들 때는 `run_all.sh`를 쓴다(§11).
한 언어만 집계할 때는 다음처럼 C 결과와 함께 넘긴다.

```bash
python3 framework/bench/grpc/tools/bench_aggregate.py --lang node \
  --runs-glob 'framework/bench/grpc/log/<stamp>/c/run*' \
  --runs-glob 'framework/bench/grpc/log/<stamp>/node/run*' --format full
```

## 6. 알려진 제약

- raw 비교는 ROUTER↔ROUTER로 고정한다(DEALER 보조 run 없음).
