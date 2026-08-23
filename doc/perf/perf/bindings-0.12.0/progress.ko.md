# bindings 0.12.0 성능 개선 진행 시트

> 시작일: 2026-08-19
>
> 기준 Core: `0.12.0`
>
> 작업 브랜치: `codex/bindings-0.12.0-performance`
>
> 계획서: [bindings 0.12.0 성능 개선 계획](./bindings-library-performance-improvement-plan-core-0.12.0.ko.md)
>
> 원본 템플릿: [bindings 성능 개선 계획 Core 템플릿](../bindings-library-performance-improvement-plan-core-template.ko.md)

이 시트의 독자는 성능 개선 작업자다. 현재 통과한 gate와 다음 paired 측정 대상을 이 문서에서
확인하고, 실행 과정과 후보 검토는 같은 폴더의 `log/`에 기록한다. 상세한 size·transport·pattern
결과 표와 완료 기준은 계획서가 소유한다.

## 1. 작업 범위

같은 Core `0.12.0` release runtime으로 `bindings/c/perf`를 기준으로 삼고, 각 binding의
공식 perf runner 결과를 paired 비교한다. 개선은 현재 public API와 ownership·error contract를
유지한 binding 내부 구현을 대상으로 한다.

| 순서 | Binding | Perf 경로 | Single | Multi |
|------|---------|-----------|--------|-------|
| 1 | C++ | `bindings/cpp/perf` | 미측정 | 미측정 |
| 2 | .NET | `bindings/dotnet/perf` | 미측정 | 미측정 |
| 3 | Java | `bindings/java/perf` | 미측정 | 미측정 |
| 4 | Node | `bindings/node/perf` | 미측정 | 미측정 |
| 5 | Go | `bindings/go/perf` | 미측정 | 미측정 |
| 6 | Rust | `bindings/rust/perf` | 미측정 | 미측정 |
| 7 | Python | `bindings/python/perf` | 미측정 | 미측정 |

## 2. 초기 gate

| Gate | 상태 | 근거 또는 다음 작업 |
|------|------|---------------------|
| 버전 3곳 일치 | 확인됨 | `VERSION`, `core/CMakeLists.txt`, `core/include/zlink.h`가 모두 `0.12.0`이다. |
| 실제 release runtime과 package provenance | 미확인 | `core/v0.12.0` release asset과 `core-package-provenance.json`을 준비하고 확인한다. |
| 공식 runner inventory | 미확인 | C와 각 binding runner의 pattern·transport·size·client 수를 대조한다. |
| Multi size 정책 | 미확인 | 현재 runner의 `4 KiB` 포함 여부와 `MULTI_STREAM` 예외를 확인한다. |
| 무시되는 runner option | 미확인 | `--output`, `--pin-cpu`, I/O thread, HWM, buffer, timeout의 적용 여부를 확인한다. |
| memory guard와 실제 client 수 | 미확인 | C와 binding report의 client 수, STREAM client 수, cap 발생 여부를 확인한다. |
| 재현 환경 manifest | 미착수 | host, CPU 상태, runtime, toolchain, session tag와 명령을 `log/`에 기록한다. |

## 3. 현재 작업 위치

| 항목 | 상태 | 값 |
|------|------|-----|
| 현재 언어 | 미정 | C++부터 시작한다. |
| 현재 suite | 미정 | inventory gate 뒤 선택한다. |
| 현재 pattern | 미측정 | 하나의 pattern만 선택한다. |
| 현재 transport | 미측정 | Single은 `tcp`부터 선택한다. |
| paired C 기준 | 미측정 | 선택한 binding·suite·pattern·transport만 측정한다. |
| binding before | 미측정 | paired C가 `status: complete`인 뒤 바로 측정한다. |
| 자체 개선 pass | 미착수 | aggregate 목표 미달 시 hot path 후보를 한 번 검토한다. |
| Sol 리뷰 pass | 미착수 | 자체 pass 뒤 read-only review와 두 번째 개선 pass를 기록한다. |
| 커밋과 push | 미착수 | 검증된 변경만 별도 승인 범위에서 처리한다. |

## 4. Paired 측정 기록

두 report가 모두 `status: complete`이고 같은 manifest·session tag·조건을 사용할 때만 결과를
계획서의 상세 표에 반영한다. 부분 결과, runtime mismatch, client 수 불일치는 판정값으로 사용하지
않고 원인을 `log/`에 기록한다.

| 날짜 | 언어 | Suite / pattern / transport | 단계 | 결과 | C report | Binding report | 로그 |
|------|------|-----------------------------|------|------|----------|----------------|------|
| 2026-08-19 | 전체 | 작업 초기화 | 계획 작성 | 측정 전 | - | - | 이 시트와 계획서 |

## 5. 개선 후보 기록

후보는 public API, ownership, error semantics와 측정 의미를 유지하는지 확인한 뒤 기록한다.
성능 향상뿐 아니라 allocation·copy·dispatch·callback·poller 비용의 책임 위치와 POSDDD
위험 신호를 함께 적는다.

| 후보 | 대상 | 변경 요약 | Before | After | 판정 | 로그 |
|------|------|-----------|--------|-------|------|------|
| - | - | - | - | - | 미착수 | - |

## 6. 다음 작업

현재 다음 순서로 진행한다.

1. `core/v0.12.0` release runtime과 package provenance를 준비한다. Core source는 다시 build하지 않는다.
2. C와 각 binding runner의 inventory gate를 통과시킨다.
3. C++의 첫 `suite + pattern + transport`를 선택하고 64 B smoke를 C와 binding에서 순서대로 실행한다.
4. smoke가 complete이면 같은 조건의 모든 message size를 paired 측정하고 결과를 계획서와 이 시트에 기록한다.
5. 목표 미달이면 hot path 후보, 자체 pass, Sol 리뷰 pass 순서로 진행한다.

## 7. 갱신 규칙

- 실행 명령, profiler 결과, 후보 비교와 no-go 사유는 `log/`에 날짜별 파일로 남긴다.
- 계획서의 상세 표에는 complete paired report의 조건, 비율, latency, Effective Options, auto-HWM,
  client 수와 최종 상태만 기록한다.
- `미측정`, `미달`, `통과(비율%)`, `보류(비율%)`, `해당 없음`은 계획서의 상태 규칙을 따른다.
- 변동값이나 partial report만으로 상태를 `통과` 또는 `보류`로 바꾸지 않는다.
- 작업 브랜치 생성·전환, commit과 push는 별도 요청 또는 승인 없이 수행하지 않는다.
