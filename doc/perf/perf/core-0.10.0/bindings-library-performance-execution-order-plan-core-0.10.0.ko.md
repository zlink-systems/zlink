# Core 0.10.0 bindings 성능 측정·개선 실행 순서

이 문서는 Core `0.10.0`의 binding 성능 작업을 정해진 순서로 진행하기 위한 계획표다.
기준은 `main` branch와 `bindings/c/perf` C baseline이다. 각 언어의 세부 조건, 측정 시트,
실패 기록과 완료 기준은 연결된 개별 계획 문서가 소유한다.

한 언어의 측정과 개선은 해당 개별 계획 문서의 완료 기준을 모두 충족한 뒤에만 완료로
판정한다. 다음 행은 이전 행의 상태가 `complete`로 확정된 뒤 시작한다. `미측정`, `미달`,
`보류` 셀이 남아 있으면 다음 언어로 진행하지 않는다.

| 순서 | binding | 개별 계획 문서 | 실행 범위 | 언어별 완료 검증 | 다음 단계 진입 조건 | 계획 상태 |
|------:|---------|----------------|-----------|------------------|----------------------|-----------|
| 1 | `.NET` | [.NET 성능 개선 계획](dotnet/bindings-library-performance-improvement-plan-core-0.10.0-dotnet.ko.md) | C와 .NET의 `single`·`multi` 선택 셀을 같은 조건으로 paired 측정하고, public .NET 경로 안에서 개선한다. `multi STREAM`은 CCU `100`으로 측정한다. | paired complete report와 manifest, throughput·latency gate, .NET unit·integration·package consumer 검증 | .NET 계획 문서의 모든 대상 셀이 완료되고 상태가 `complete`이면 Node를 시작한다. | `planned` — 첫 단계 |
| 2 | `Node` | [Node 성능 개선 계획](node/bindings-library-performance-improvement-plan-core-0.10.0-node.ko.md) | .NET 완료 후 C와 Node를 paired 측정하고, event loop·Buffer·native transition 비용을 binding 내부에서 개선한다. | paired complete report와 manifest, throughput·latency gate, Node test·package consumer 검증 | Node 계획 문서의 완료 기준을 충족하고 상태가 `complete`이면 C++를 시작한다. | `planned` — 대기 |
| 3 | `C++` | [C++ 성능 개선 계획](cpp/bindings-library-performance-improvement-plan-core-0.10.0-cpp.ko.md) | Node 완료 후 C와 C++를 paired 측정하고, C++ public API 경로의 allocation·copy·dispatch 비용을 개선한다. | 모든 inventory 셀의 paired report와 조건 manifest, throughput·latency 판정, C++ test·package consumer 검증 | C++ 계획 문서에 `미측정`·`미달`·`보류`가 없고 완료 기준을 충족하면 Java를 시작한다. | `planned` — 대기 |
| 4 | `Java` | [Java 성능 개선 계획](java/bindings-library-performance-improvement-plan-core-0.10.0-java.ko.md) | C++ 완료 후 C와 Java를 paired 측정하고, JNI 경계·allocation·copy·callback·poller 비용을 binding 내부에서 개선한다. | inventory, paired report, manifest, ratio·latency, Java unit·contract·integration·package consumer 검증 | Java 계획 문서의 완료 기준을 충족하고 상태가 `complete`이면 Go를 시작한다. | `planned` — 대기 |
| 5 | `Go` | [Go 성능 개선 계획](go/bindings-library-performance-improvement-plan-core-0.10.0-go.ko.md) | Java 완료 후 C와 Go를 paired 측정하고, ownership·blocking 의미를 유지하면서 allocation·cgo transition·copy·goroutine scheduling 비용을 개선한다. | Go unit·contract·package consumer 검증과 모든 paired performance gate | Go 계획 문서의 완료 기준을 충족하고 상태가 `complete`이면 Python을 시작한다. | `planned` — 대기 |
| 6 | `Python` | [Python 성능 개선 계획](python/bindings-library-performance-improvement-plan-core-0.10.0-python.ko.md) | Go 완료 후 C와 Python을 paired 측정하고, Python 객체 생성·buffer copy·FFI·dispatch 비용을 binding 내부에서 개선한다. | Python unit·native contract·package consumer 검증과 paired throughput·latency gate | Python 계획 문서의 완료 기준을 충족하고 상태가 `complete`이면 Rust를 시작한다. | `planned` — 대기 |
| 7 | `Rust` | [Rust 성능 개선 계획](rust/bindings-library-performance-improvement-plan-core-0.10.0-rust.ko.md) | Python 완료 후 C와 Rust를 paired 측정하고, safe public API·ownership 계약을 유지하면서 allocation·copy·FFI boundary·polling 비용을 개선한다. | Rust unit·contract·package consumer 검증과 paired throughput·latency gate | Rust 계획 문서의 완료 기준과 최종 성능 비회귀 검증을 충족하면 전체 순서를 완료한다. | `planned` — 대기 |

## 공통 실행 규칙

- 각 행은 C를 먼저 실행한 뒤 같은 `suite`, `pattern`, `transport`, `message size`, `duration`, `runs`, client 수, I/O 조건으로 binding을 연속 실행한다. 두 report가 모두 `complete`일 때만 비교값을 기록한다.
- baseline과 개선 후 측정은 같은 조건을 유지한다. 비교 범위는 필요한 paired 셀을 우선 선택하고, 최종 완료 전에는 정책에 따른 전체 single·multi 성능 비회귀를 확인한다.
- retry, sleep, timeout·HWM·client 수 조정, inflight 제한, `UNSUPPORTED` 위장과 benchmark 전용 우회는 완료 조건으로 인정하지 않는다. 실패하면 `## Failures`와 결과 파일에 기록하고 원인을 수정한 뒤 같은 조건으로 다시 측정한다.
- Core 결함이 확인되면 재현 회귀 테스트, Core 수정, local runtime 재배포, 해당 binding package와 테스트 재검증을 마친 뒤 현재 행의 paired 측정을 다시 수행한다.
- 진행 로그와 실패 이력은 각 언어 폴더의 `log/`에 기록하고, 측정값은 개별 계획 문서의 측정 시트와 runner report에 기록한다. 이 순서표에는 각 행의 최종 상태만 반영한다.

참조 정책: [`PERF_POLICY.md`](../../PERF_POLICY.md), [`PERF_SINGLE_TEST_POLICY.md`](../../PERF_SINGLE_TEST_POLICY.md), [`PERF_MULTI_TEST_POLICY.md`](../../PERF_MULTI_TEST_POLICY.md)
