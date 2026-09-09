# fwb2-01 결과 — messaging bench 위치 통합

## 결과

- 언어별 messaging bench, C 기준 bench, 공용 집계기를 `framework/bench/grpc/` 아래로 통합했다.
- 다섯 복제본이던 `bench.proto`를 `framework/bench/grpc/proto/bench.proto` 하나로 통합하고 모든 빌드가 이 파일을 생성 입력으로 사용하게 했다.
- .NET, Node, Java/Kotlin, C++, C를 새 위치에서 빌드했고 집계기 테스트 50개를 통과했다.
- 각 언어의 `zlink-<lang>-request-serial@1024` 단일 smoke를 우선순위 2 perf ticket으로 실행했다. 최종 다섯 티켓은 모두 rc=0이고 RESULT 라인을 냈다.
- 기존 측정 조건, Core·binding runtime 소스, CI는 변경하지 않았다.

## 이동 표

이동은 tracked 파일에만 적용했다. 기존 `bin/`, `obj/`, `build/`, `node_modules/`, `log/` 산출물은 이동하거나 삭제하지 않았다.

| 원래 위치 | 통합 위치 |
|---|---|
| `framework/languages/dotnet/bench/with-grpc/` | `framework/bench/grpc/dotnet/` |
| `framework/languages/node/bench/with-grpc/` | `framework/bench/grpc/node/` |
| `framework/languages/java/bench/with-grpc/` | `framework/bench/grpc/java/` |
| `framework/languages/cpp/bench/with-grpc/` | `framework/bench/grpc/cpp/` |
| `bindings/c/bench/with_grpc/` | `framework/bench/grpc/c/` |
| `bindings/c/bench/BENCH_POLICY.md` | `framework/bench/grpc/c/BENCH_POLICY.md` |
| `framework/bench/tools/` | `framework/bench/grpc/tools/` |
| 언어별 proto 5개 | `framework/bench/grpc/proto/bench.proto` 1개 |
| 언어별 `log/` | `framework/bench/grpc/log/{dotnet,node,java,cpp,c}/` |

`BENCH_POLICY.md`는 이동 전후 SHA-256이 모두 `a9798e9e80cd0137102cf90885518a44ad57f5e414fa5f3750cf0a8e8f011223`로, 정책 문장을 그대로 유지했다.

## 경로 수정 파일

| 범위 | 파일과 수정 내용 |
|---|---|
| 공통 proto·log | `proto/bench.proto`, `log/.gitignore`, `log/{dotnet,node,java,cpp,c}/.gitkeep`, 저장소 루트 `.gitignore`. Java/.NET 생성 옵션을 한 proto에 유지하고 언어별 생성 입력을 공통 파일로 바꿨다. 이전 위치의 untracked log도 계속 무시한다. |
| .NET | `dotnet/Directory.Build.props`, `dotnet/Directory.Packages.props`, `Client/WithGrpcBench.Client.csproj`, `ZLinkServer/WithGrpcBench.ZLinkServer.csproj`, `Shared/WithGrpcBench.Shared.csproj`, `run_local.sh`, `README.ko.md`. 중앙 package 설정과 framework project reference, proto, 기본 log 경로를 새 깊이에 맞췄다. |
| Node | `node/.gitignore`, `package.json`, `client/main.js`, `grpc-server/main.js`, `run_local.sh`, `client/bench-core.js`, `shared/raw-wire.js`. 독립 `npm ci && npm run build`, framework workspace와 binding 0.17.6 사용, 공통 proto, 중앙 log 경로를 연결했다. smoke 선택을 위해 기존 client의 scenario·implementation 옵션을 runner 환경 변수로 전달하며 기본값은 기존과 같은 `all`이다. |
| Java/Kotlin | `java/settings.gradle.kts`, `gradlew`, `build.gradle.kts`, `shared/build.gradle.kts`, `kotlin-client/build.gradle.kts`, `run_local.sh`, `run_local_kotlin.sh`, 그리고 경로를 인용한 source comment 4개. framework composite build, version catalog/local Maven, 공통 proto, 중앙 log 경로를 새 깊이에 맞췄다. |
| C++ | `cpp/CMakeLists.txt`, `client/bench_cpp_client.cpp`, `grpc/bench_grpc_cpp_server.cpp`, `common/bench_common.hpp`, `run_local.sh`, `measure_span.sh`. `.artifacts/<platform>/install/zlink-{cpp,core}/<version>` package를 사용하고 공통 proto namespace/Empty 타입과 중앙 log 경로를 맞췄다. |
| C 기준 | `c/CMakeLists.txt`, `grpc/bench_grpc_client.cpp`, `grpc/bench_grpc_server.cpp`, `run_local.sh`, `bindings/c/bench/CMakeLists.txt`. 새 위치를 독립 CMake project로 만들고 설치 Core prefix, 공통 proto, 기존 optional ZeroMQ 경로와 binary 경로를 맞췄다. 기존 binding 기본 빌드의 `with_grpc` option/subdirectory는 제거했다. |
| 집계기 | `tools/bench_aggregate.py`, `tools/benchagg/readers.py`, `tools/README.ko.md`, `tools/verify_span.sh`. 예시 `--runs-glob`, reader 설명, 언어별 runner와 중앙 log 경로를 모두 새 구조로 바꿨다. |

Java 원본은 공통 위치로 `git mv`했고, .NET·Node·C++·C의 나머지 복제본 네 개는 제거했다. source/build 산출물을 제외한 `find ... -name bench.proto` 결과는 공통 파일 하나다.

## 남은 문서 참조 — 감독자 수정/판정 대상

요청된 grep을 tracked 문서에 적용했다. 아래 파일은 이 job의 문서 수정 금지 범위이므로 변경하지 않았다. 과거 실행 기록은 의도적으로 옛 경로를 보존할 수 있으므로 감독자가 변경 여부를 판정해야 한다.

| 구분 | 남은 참조 |
|---|---|
| 현재 framework bench 규격 | `framework/doc/framework/common/bench/with-grpc-local.{ko,en}.md`: ko `162`, en `38,70-71,183,485` |
| 현재 2차 계획 | `doc/plan/framework-bench-with-grpc-s2s-plan.ko.md`: `9,28,64-66,75,104,117-118,130,132,140,147` |
| 1차 계획 | `doc/plan/framework-bench-with-grpc-5lang-plan.ko.md`: `7,28,38-41,97,183,248,256,270,317` |
| 결정 기록 | `doc/plan/fw-bench-worklog/decisions.ko.md`: `10,14,16,18,83,289,350,735` |
| inventory | `doc/plan/c016-worklog/framework-perf-runner-inventory.md:14` |
| release 준비 기록 | `doc/building/release-prep/2026-09-08-rel-common.ko.md:23`, `doc/building/release-prep/2026-09-09-ci-warnings-and-cleanup.ko.md:38,40` |
| 과거 perf 기록 | `doc/perf/perf/bindings-0.17.0/log/2026-09-07-cpp-multi-reqrep-pass1.ko.md:15`, `2026-09-07-cpp-multi-reqrep-pass2.ko.md:19`, `2026-09-07-environment.ko.md:24`, `2026-09-07-go-rust-python-runner-parity.ko.md:194`, `2026-09-08-cpp-multi-sendsend-pass1.ko.md:15-16` |

`zlink-work/**`, 기존 build/obj 산출물의 절대 경로도 원문 grep에는 나오지만 tracked 문서가 아니며 이동 대상에서 제외했다.

## 빌드·테스트 검증

모든 빌드는 실행 직전에 `/proc/loadavg`의 1분 값을 확인했고 10 미만에서 하나씩 실행했다.

| 대상 | 명령 | ticket | rc | 소요 |
|---|---|---|---:|---:|
| .NET | `ZLINK_LOCAL_PACKAGE_ROOT=/home/hep7/project/zlink/.artifacts/wsl dotnet build framework/bench/grpc/dotnet/WithGrpcBench.sln -c Release` | 빌드, 해당 없음 | 0 | 10.28초 |
| Node | `cd framework/bench/grpc/node && npm ci && npm run build` | 빌드, 해당 없음 | 0 | 5.78초 |
| Java/Kotlin | `cd framework/bench/grpc/java && ./gradlew --no-daemon --max-workers=1 assemble` | 빌드, 해당 없음 | 0 | 41.27초 |
| Java smoke 준비 | 같은 위치에서 `./gradlew --no-daemon --max-workers=1 installDist` | 빌드, 해당 없음 | 0 | 7.15초 |
| C++ | `cmake -S framework/bench/grpc/cpp -B /tmp/zlink-sol-fwb2-01/cpp -G Ninja -DCMAKE_BUILD_TYPE=Release -DZLINK_FRAMEWORK_CPP_LOCAL_PACKAGE_ROOT=/home/hep7/project/zlink/.artifacts/wsl -DZLINK_FRAMEWORK_CPP_ZLINK_CPP_VERSION=0.17.3 -DZLINK_FRAMEWORK_CPP_ZLINK_CORE_VERSION=0.17.3 && cmake --build /tmp/zlink-sol-fwb2-01/cpp -- -j1` | 빌드, 해당 없음 | 0 | 18.01초 |
| C | `cmake -S framework/bench/grpc/c -B /tmp/zlink-sol-fwb2-01/c -G Ninja -DCMAKE_BUILD_TYPE=Release -DZLINK_C_CORE_BUILD_DIR=/home/hep7/.cache/zlink/core/0.17.5/linux-x64 && cmake --build /tmp/zlink-sol-fwb2-01/c -- -j1` | 빌드, 해당 없음 | 0 | 15.55초 |
| 집계기 | `python3 -m unittest discover -s framework/bench/grpc/tools/tests -p 'test_*.py'` | test, 해당 없음 | 0 | 0.04초, 50 tests |
| 정적 검사 | `bash -n`(runner 8개), package JSON parse, `git diff --check` | 정적 검사, 해당 없음 | 0 | 1초 미만 |

`pytest` module이 설치되어 있지 않아 `python3 -m pytest ...`는 실행할 수 없었다. 브리프가 허용한 기존 unittest discovery를 사용했고, 계획에 적힌 28개보다 현재 test case가 늘어 50개가 통과했다. Node의 `npm ci`는 rc=0이지만 현재 lockfile 기준 audit 경고 6개(중간 5, 높음 1)를 출력했다. 이 job은 dependency version이나 측정 조건을 바꾸지 않았다.

## 1셀 smoke

모든 명령은 `bash scripts/perf/perf-ticket.sh submit -p 2 -o sol-fwb2-01 -d <설명> -- <명령>`으로 제출하고 완료까지 기다렸다. smoke invocation은 active duration을 2초로, .NET/Node warmup을 100회로, Java warmup을 1초로 줄였으며 runner 기본값은 바꾸지 않았다. raw ZLink 구현 하나와 `request-serial@1024`만 선택했다.

| 언어 | ticket 내부 명령 요약 | ticket | rc | 소요 | RESULT 확인 |
|---|---|---|---:|---:|---|
| .NET | `SKIP_BUILD=1 PAYLOAD_SIZES=1024 DURATION_SECONDS=2 WARMUP=100 dotnet/run_local.sh --scenario request-serial --implementation zlink-dotnet` | `2-1788934502-5165-sol-fwb2-01-fwb2-01_dotnet_zlink_raw_request-serial_` | 0 | 9.01초 | `zlink-dotnet-request-serial`, 1024 |
| Node | `RUNS=1 RUN_DEALER=0 DURATION=2 WARMUP=100 PAYLOADS=1024 SCENARIO=request-serial IMPLEMENTATION=zlink-node node/run_local.sh` | `2-1788934310-88786-sol-fwb2-01-fwb2-01_node_zlink_raw_request-serial_10` | 0 | 약 9초 | `zlink-node-request-serial`, 1024 |
| Java | `RUNS=1 RUN_DEALER=0 SKIP_BUILD=1 DURATION=2 WARMUP_SECONDS=1 PAYLOADS=1024 SCENARIO=request-serial IMPLEMENTATION=zlink-java java/run_local.sh` | `2-1788934365-91480-sol-fwb2-01-fwb2-01_java_zlink_raw_request-serial_10` | 0 | 15.02초 | `zlink-java-request-serial`, 1024 |
| C++ | `BUILD_DIR=/tmp/zlink-sol-fwb2-01/cpp cpp/run_local.sh cpp-smoke --implementations zlink-cpp --patterns request-serial --payload-sizes 1024 --duration-seconds 2` | `2-1788934410-96376-sol-fwb2-01-fwb2-01_cpp_zlink_raw_request-serial_102` | 0 | 15.02초 | `zlink-cpp-request-serial`, 1024 |
| C | `SKIP_BUILD=1 BUILD_DIR=/tmp/zlink-sol-fwb2-01/c PAYLOAD_SIZES=1024 DURATION_SECONDS=2 GRPC_BENCH_SCENARIOS=none ZLINK_BENCH_SCENARIOS=request-serial c/run_local.sh` | `2-1788934465-2141-sol-fwb2-01-fwb2-01_c_zlink_request-serial_1024_smok` | 0 | 9.01초 | `zlink-c-request-serial`, 1024 |

검증 중 Node 전체 구현 smoke ticket `2-1788933947-76019-sol-fwb2-01-fwb2-01_node_request-serial_1024_smoke`도 rc=0이었으나, 계획에 이미 기록된 Node framework protobuf bytes 미지원 행은 failed cell로 남았다. 최종 표의 Node ticket은 이 범위 밖 결함을 피하고 이동 대상 raw runner 한 셀만 검증한다. `2-1788934281-86814-sol-fwb2-01-fwb2-01_node_zlink_raw_request-serial_10`은 잘못 입력한 `OUTROOT` 때문에 bench 시작 전에 rc=1이었고, 바로 다음 최종 ticket에서 수정했다.

## framework 기본 빌드 미포함 확인

`.NET` solution/csproj, Node workspace `package.json`, Java `settings.gradle.kts`, C++ top-level CMake, C binding CMake, `.github`를 대상으로 `framework/bench/grpc`, `bench/with-grpc`, `bench/with_grpc` 참조를 검색했으며 결과는 0건이었다. C binding의 기존 `ZLINK_C_BUILD_BENCH_GRPC_COMPARE`와 `add_subdirectory(with_grpc)`도 제거했다. 각 bench는 자기 위치의 독립 solution/package/composite build/CMake project로만 빌드된다.

## BLOCKERS

없음.

비차단 후속 항목은 감독자 소유의 규격·계획·사이트 문서 이동/참조 갱신과, 다음 S1~S3의 server-driven runner 개정이다. Node framework protobuf bytes 미지원도 계획에 이미 분리된 기존 결함이며 이번 위치 통합을 막지 않는다.
