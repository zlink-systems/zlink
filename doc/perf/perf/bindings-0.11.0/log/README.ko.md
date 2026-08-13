# core 0.11.0 성능 측정 기록

이 폴더에는 0.11.0 release runtime으로 실행한 C 기준과 binding의 측정 기록, 계산 결과,
개선 전후 비교만 저장한다. 후보 탐색 과정이나 장시간 작업 이력은 기록하지 않는다.

파일 이름은 `YYYY-MM-DD-언어-suite-pattern-transport.ko.md` 형식을 사용한다.

각 기록에는 다음 항목을 포함한다.

- Core와 binding package 버전, runtime provenance
- C와 binding의 실행 조건 및 원본 throughput
- message size별 C 대비 비율과 transport·pattern 산술평균
- 개선 전후 수치와 최종 판정

## 기록 형식 예시

아래 값은 형식만 보여 주는 예시이며, 실제 측정 결과가 아니다.

```text
대상: C++ / Single PAIR / tcp
Core runtime: 0.11.0 release, provenance=<path>
Binding package: 0.11.0
공통 조건: size=64,256,1024,65536,131072,262144; duration=<value>; clients=<value>
```

| Size | C throughput | Binding throughput | C 대비 비율 |
|---|---:|---:|---:|
| 64B | 1,000,000 | 900,000 | 90.0% |
| 256B | 1,000,000 | 920,000 | 92.0% |
| 1KiB | 1,000,000 | 880,000 | 88.0% |
| 64KiB | 1,000,000 | 920,000 | 92.0% |
| 128KiB | 1,000,000 | 900,000 | 90.0% |
| 256KiB | 1,000,000 | 880,000 | 88.0% |
| 산술평균 | - | - | 90.0% |

결과 행에는 `C++ / Single PAIR / tcp / 90.0% / 목표 90.0% / 통과`와 report 경로를
기록한다. 개선 전후가 있으면 같은 표를 두 번 두고, 최종 판정에 사용한 측정값을 명시한다.
