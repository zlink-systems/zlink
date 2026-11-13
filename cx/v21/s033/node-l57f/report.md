# Node L5~L7 smoke: Framework route delivery fault

## 판정

벤치 wiring 결함을 확인하지 못했다. L5~L7 server/client 스위치가 모두 적용됐고 route mesh 이름, target routing ID, packet name이 일치한다. 그런데 public Framework source의 send 완료 수가 늘어나는 동안 target handler 수신은 0이다. request probe도 응답하지 않아 source stats server가 시작되지 않는다. 이는 Framework route send 이후 target receive/dispatch 경로의 결함으로 분류하고 여기서 중단한다. production code와 bench code는 수정하지 않았다.

## 근거

| 확인 항목 | 결과 |
|---|---|
| Route / target | source `contract.MESH_NAME`와 server `contract.MESH_NAME` 모두 `bench`; target routing ID 양쪽 모두 `bench-server` (`framework/bench/grpc/node/client/framework-transport.js:41-45,57-60`, `zlink-framework-server/main.js:355-359`) |
| Handler / message | server는 request와 send를 `contract.PACKET_NAME` (`BenchPayload`)에 등록; client 양 경로 모두 `new contract.BenchPayload(body)` 사용 (`zlink-framework-server/main.js:358-359`, `framework-transport.js:60-61,71`) |
| Metrics wiring | Echo/Command handler에 동일한 `metrics` 인스턴스를 주입하고 `handle()` 진입 시 `record()` 호출 (`zlink-framework-server/main.js:364-367`, `bench-handlers.js:12-16,24-28`) |
| Env | 양쪽 프로세스에 `ZLINK_LADDER_L5_7_SERVER=1`, `ZLINK_LADDER_L5_7_CLIENT=1` 지정. server JSON codec 설정은 꺼지고, client는 public Framework transport를 선택 (`main.js:19,56-64,348-354`, `client/main.js:358-360`) |
| Send smoke | `submitted=77,301`, `completed=77,301`, `errors=0`, `server_errors=0`, target `received=0`; target stats도 `ready=true`, `errors=0`, `received=0`. 결과: [results.json](raw/smoke/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/results.json), [target-stats.json](raw/smoke/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/target-stats.json), [source.log](raw/smoke/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/source.log), [target.log](raw/smoke/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/target.log) |
| Request smoke | target Framework server starts, but source never exposes stats within runner's 30 s wait; source log is empty because transport creation waits for the route-ready request probe before starting stats (`client/main.js:362-373`, `framework-transport.js:51-62`). Runner output: [runner.log](raw/smoke/request-serial-1024/runner.log); process logs: [target.log](raw/smoke/request-serial-1024/run1/zlink-framework-node-request-serial-1024/target.log), [source.log](raw/smoke/request-serial-1024/run1/zlink-framework-node-request-serial-1024/source.log) |
| Scheduling | Send smoke source and target threads were `SCHED_OTHER`, source CPUs 0-5 and target CPUs 8-13 at start and mid-active: [scheduling.txt](raw/smoke/send-saturation-1024/run1/zlink-framework-node-send-saturation-1024/scheduling.txt) |

Framework source location: `framework/languages/node/packages/framework/src/runtime/channels/channel-outbound-operations.ts:370-400` (`routeSubmitScoped` awaits `router.send(...)` and then returns `Submitted`). The public client calls this route through `sendToNode(...).submit()` at `framework/bench/grpc/node/client/framework-transport.js:67-73`. At the observed target, neither registered handler records a message. This isolates the fault to the Framework route delivery/dispatch path after successful source submission, rather than the bench's route/handler/metrics wiring.

The smoke did not emit a Framework message-flow trace: this bench harness does not configure a flow listener/exporter, and its Nest logger is disabled (`zlink-framework-server/main.js:370-373`). Existing logs and counters are preserved as captured; no tracing-only code was introduced.

## 재현 명령

각각 독립적인 exclusive hold 안에서 실행했다(각 hold는 5분 제한, runner의 source/target `taskset`은 0-5 / 8-13, warmup 3 s / active 5 s).

```bash
timeout 300 flock -x /root/jobs/fx/cx/measure.lock env \
  ZLINK_LADDER_L5_7_SERVER=1 ZLINK_LADDER_L5_7_CLIENT=1 \
  IMPLEMENTATIONS=zlink-framework-node BENCH_TARGET_SERVER=zlink-framework-node \
  PATTERNS=send-saturation PAYLOAD_SIZES=1024 RUNS=1 SKIP_BUILD=1 \
  WARMUP_SECONDS=3 DURATION_SECONDS=5 SOURCE_CPUS=0-5 TARGET_CPUS=8-13 \
  SCHEDULING_RECORD=1 \
  OUTPUT_DIR=/root/jobs/fx/cx/v21/s033/node-l57f/raw/smoke/send-saturation-1024 \
  RUN_STAMP=node-l57f-send-203253 \
  bash /root/jobs/fx/cx/v21/s033/lad-node-s1/tools/node/run_local_framework.sh

timeout 300 flock -x /root/jobs/fx/cx/measure.lock env \
  ZLINK_LADDER_L5_7_SERVER=1 ZLINK_LADDER_L5_7_CLIENT=1 \
  IMPLEMENTATIONS=zlink-framework-node BENCH_TARGET_SERVER=zlink-framework-node \
  PATTERNS=request-serial PAYLOAD_SIZES=1024 RUNS=1 SKIP_BUILD=1 \
  WARMUP_SECONDS=3 DURATION_SECONDS=5 SOURCE_CPUS=0-5 TARGET_CPUS=8-13 \
  SCHEDULING_RECORD=1 \
  OUTPUT_DIR=/root/jobs/fx/cx/v21/s033/node-l57f/raw/smoke/request-serial-1024 \
  RUN_STAMP=node-l57f-request-203314 \
  bash /root/jobs/fx/cx/v21/s033/lad-node-s1/tools/node/run_local_framework.sh
```

요청된 `/root/jobs/fx/cx/v21/s033/lad-node-impl4c/report.md`는 디렉터리 자체가 없어 읽을 수 없었다. 존재하는 이전 구현 보고서 `/root/jobs/fx/cx/v21/s033/lad-node-impl4/report.md`와 L5~L7 call-shape를 읽었다.
