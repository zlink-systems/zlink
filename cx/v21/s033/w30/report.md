# warmup 30초 규칙 적용

**변경**
- 규칙과 미판정 기준: `framework/bench/grpc/README.ko.md:147,500,657` (영문 미러 `README.en.md:160,539,711`).
- 기본값: `runner_common.sh:49`, C `c/common/bench_common.hpp:320`, C++ `cpp/client/bench_cpp_client.cpp:60,1069`, .NET `dotnet/Client/Program.cs:1285`, Java/Kotlin `java/client/.../BenchOptions.java:41`, Node `node/client/main.js:30`.
- 기록 형식: C `c/common/bench_common.hpp:433`의 `metadata.warmupSeconds`; C++ `cpp/client/bench_cpp_client.cpp:878,987-989`의 밀리초 trigger와 초 결과 필드; .NET `dotnet/Client/Program.cs:1121`의 결과 `Warmup`; Java/Kotlin `java/client/.../BenchResultWriter.java:50,102`의 초 단위 trigger/metadata; Node `node/client/main.js:368`의 초 단위 trigger.
- 집계기 단위 정규화 `tools/benchagg/readers.py:88-113`, 비교 양쪽 모든 셀의 판정 `tools/benchagg/analysis.py:333-347`. 누락·비정상 warmup도 unsupported 처리.
- warmup 판정 위치 수: **0 → 1** (`judge_pair`). 커밋 `1cc4641741`.

**시험과 재집계**
- benchagg 단위 시험: 139개 통과. 30초 판정, 2초·누락·혼합 행 미판정 시험 포함.
- Java `:client:warmupResultIsolationTest`, .NET `BoundaryTests`, Node `client/bench-core.test.js` 12개 통과.
- Node 전체 `npm test`는 `@grpc/proto-loader` 부재로 raw-wire 시험을 시작하지 못함. C++ 빌드/시험은 요청 제한에 따라 생략.
- `/root/jobs/fx/cx/v21/s033/r4190m/raw` 무측정 재집계 결과: `aggregates/{cpp,dotnet,java,node}.{json,md}`. 네 언어 모두 8/8 판정이 `unsupported`, 사유는 `warmup 2s, spec requires 30s`.

**남은 문제**
- Node 전체 시험에 `@grpc/proto-loader` 의존성이 필요함. C++ 드라이버 시험은 미실행.
