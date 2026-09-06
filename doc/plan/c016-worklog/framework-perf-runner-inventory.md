# Framework perf runner inventory @ `seven-samples-green-v2` (`4e8182af01`) — 2026-09-06, 머신 A (Claude Explore agent, read-only)

## 결론
- **7축 공통 perf 규격(`framework/doc/framework/common/perf/README.ko.md`)을 구현한 runner는 어느 언어에도 없다.** 규격이 요구하는 `framework/languages/{dotnet,java,java/perf/kotlin,node,cpp}/perf/` + `scripts/run_perf.sh`·`run_single.sh`·`collect_env.sh`, 공통 CLI, `perf-results/<run-id>/{config,result,summary,client-i,server-role}` 스키마, `/perf/reset|stats|ready` 서버 endpoint 모두 미구현(0/7 × 5언어).
- 언어별 계획 문서(`framework/doc/framework/perf/bindings/*.ko.md`, 2026-08-05)는 옛 시나리오 이름(`client_server_request_reply`, `fanout_publish_1`…)을, 공통 규격 §10은 새 이름(`cs-local-session-actor-echo`, `pubsub-fanout-echo`…)을 쓴다 — 두 체계가 충돌.
- CI는 framework perf를 전혀 돌리지 않는다(`framework-dotnet.yml`/`framework-node.yml`에 perf 없음; `build.yml`의 perf는 bindings C multi smoke뿐).

## 존재하는 것(규격 이전의 ad-hoc 벤치)
| 언어 | runner | 실행 | 범위 | 출력 | 기록 |
|---|---|---|---|---|---|
| cpp | `framework/languages/cpp/connector/perf/connector_perf_client.cpp` + `run_connector_perf.sh` | `run_connector_perf.sh --clients N … --report X.json`; ctest `-L connector-perf-smoke`(`CMakeLists.txt:1793-1802`) | stream connector 요청/대기 부하만(7축 아님) | 자체 JSON(`connector_perf_client.cpp:117-142`) | 없음 |
| cpp | `tests/Zlink.Framework.PerfTests/entry_spot_admission_burst.cpp` | target `test_cpp_framework_entry_spot_admission_burst`(`CMakeLists.txt:1457`), `--legacy` | Entry Spot admission burst micro | stdout | `framework/languages/cpp/entry-spot-baseline.txt`(`bd9500e9e4`, 2026-06-13; target 이름이 `perf_cpp_…`로 stale) |
| cpp | `tests/Zlink.Framework.PerfTests/http_perf_gate.cmake` | `ctest -L framework-http-perf`(`ZLINK_FRAMEWORK_CPP_REQUIRE_HTTP_PERF_REPORT` 켤 때만 등록) | 검증기만, 부하 생성기 없음 | — | 없음 |
| dotnet | `framework/languages/dotnet/bench/with-grpc/` | `run_local.sh [--scenario request-window]`, env `PAYLOAD_SIZES=1024,4096 DURATION_SECONDS WARMUP CONFIGURATION`(`run_local.sh:6-15,52-71`), 포트 5071-5077 | `request-serial`/`request-window`/`send-saturation` × grpc/zlink-raw/zlink-framework — channel request/reply 형태만 | `bench/with-grpc/log/<stamp>/results.json`(`Client/Program.cs:186-187`) | 없음(`log/.gitkeep`) |
| dotnet | `UnitTests/Runtime/SerialExecutionQueueBenchmarkTests.cs` | `dotnet test` | serial queue micro | xunit output | 없음 |
| java | 없음 | — | — | — | `framework/languages/java/entry-spot-baseline.txt`(`6d1cf11b21`): 명시된 벤치 클래스·gradle task가 트리에 없음(dead) |
| kotlin | 없음 | — | — | — | 없음 |
| node | `framework/languages/node/scripts/perf_entry_spot_admission_burst.js` | `node …js`, env `PERF_ATTEMPTS/PERF_PAYLOAD_BYTES/PERF_ROUNDS`; npm script 없음; `npm run build` 선행 | Entry Spot admission burst micro; **내부 dist 경로 require**(node plan §6 위반) | stdout | 없음 |

## 규격 이전 baseline으로 지금 실행 가능한 것
cpp connector perf(JSON) + entry-spot micro, dotnet with-grpc(1 KiB/4 KiB, channel request/reply), node entry-spot micro. java/kotlin은 `unsupported`. 이는 7축 커버리지가 아니므로 `measurement_layer`를 명시해 기록해야 한다.

## 정리 후보
`framework/languages/java/entry-spot-baseline.txt`(dead), `framework/languages/cpp/entry-spot-baseline.txt`(target 이름 stale), 언어별 perf 계획 문서와 공통 규격의 시나리오 이름 통일.
