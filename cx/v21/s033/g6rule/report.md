# G6 실행 모델 상한 결과

## 바꾼 것

- G6 일반 규칙과 §10.3 연결을 [README.ko.md](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/README.ko.md:271)에 적었다. thread 기반 상한은 셀 `streams.count`, Node 상한은 event loop `1.0`이다.
- 집계기가 셀의 선언 상한과 실행 모델 상한을 비교하고, G6 제외 여부와 `실행 모델 상한 도달` 상태를 md/json에 출력한다. 구현은 [analysis.py](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/tools/benchagg/analysis.py:142), [readers.py](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/tools/benchagg/readers.py:195), [render.py](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/tools/benchagg/render.py:173), [bench_aggregate.py](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/tools/bench_aggregate.py:165)에 있다.
- Node 결과에 event loop 실행 모델 상한을 기록한다([bench-core.js](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/node/client/bench-core.js:289)). 과거 Node raw에는 이 필드가 없어 기록된 ELU 계측기에서 1.0을 보완한다.
- C++는 선언 상한을 `stream_count`로 기록하고 `streams.count`도 쓴다([bench_cpp_client.cpp](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/cpp/client/bench_cpp_client.cpp:928), [같은 파일](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/cpp/client/bench_cpp_client.cpp:1007)). Java는 제출 병렬도와 stream 수를 기록한다([BenchDrivers.java](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchDrivers.java:157), [BenchResultWriter.java](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/java/client/src/main/java/systems/zlink/bench/withgrpc/client/BenchResultWriter.java:54)). .NET 결과 writer는 선언 상한을 논리 CPU 수에서 해당 셀의 stream 수로 바꿨다([Program.cs](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/dotnet/Client/Program.cs:1059); stream 정의: [같은 파일](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/dotnet/Client/Program.cs:954)).
- 새 경계 테스트는 [test_g6_execution_model_ceiling.py](/root/worktree/zlink-v21-s033/g6rule/framework/bench/grpc/tools/tests/test_g6_execution_model_ceiling.py:1)에 있다.

## 규칙 수

G6 제외 판정 위치 1 → 1. `g6_excluded` 한 규칙이 네 언어에 공통 적용된다.

## 시험

- benchagg 단위 테스트: 39개 통과.
- Node bench-core 테스트: 12개 통과.
- Python/Node 구문 검사와 `git diff --check`: 통과.
- `npm test`: 12개는 통과했으나 raw-wire 테스트가 설치되지 않은 `@grpc/proto-loader` 의존성 때문에 시작되지 않았다.
- framework/bench/grpc 안에서 별도 format/lint 설정이나 명령은 찾지 못했다.

## 재집계 전후

입력은 `/root/jobs/fx/cx/v21/s033/r4190m/raw`이며 측정은 하지 않았다. 결과는 [`aggregates/`](/root/jobs/fx/cx/v21/s033/g6rule/aggregates/) 아래 네 언어별 md/json이다.

| 언어 | 바뀐 판정 수 | 바뀐 판정 (이전 → 이후) |
|---|---:|---|
| C++ | 0 | 없음 |
| .NET | 0 | 없음 |
| Java | 0 | 없음 |
| Node | 5 | `request-backpressure` 1024: unsupported/— → published/fail; `request-serial` 1024, 4096: unsupported/— → published/fail; `send-saturation` 1024, 4096: unsupported/— → published/pass |

Node 다섯 판정은 모두 `실행 모델 상한 도달`로 표시된다. 다른 상태·판정 변경은 없다.

## 남은 문제 · 다음 할 일

- 전체 Node `npm test`는 `@grpc/proto-loader` 의존성을 사용할 수 없어 raw-wire 부분을 완료하지 못했다.

커밋: `111592ac16` (`bench: judge saturation at execution model ceiling`).
