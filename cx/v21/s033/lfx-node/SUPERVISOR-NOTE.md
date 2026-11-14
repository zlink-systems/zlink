
## 23:46 UTC — warmup 3초 (감독자, 성능 측정 가이드 §3)
Node는 JIT 정상 상태가 빨라 warmup을 3초로 줄입니다(Java·.NET만 10초). 아직 counted run을 시작하지 않은 단계부터 적용하고, 이미 잰 단계는 그대로 두되 보고서에 단계별 warmup 값을 적으십시오. 측정 lock은 측정하는 몇 분만 잡고, 빌드·표 작성은 lock 밖에서 합니다. 보고서 첫 줄에 "빠른 비교(1회, 판정 아님)"라고 밝히고 commit·머신(cloudv)·UTC 시각·셀·횟수를 적으십시오.

## 00:05 UTC — 측정 시간 설정 변경 (대표님 지시, 앞 메모의 warmup 값보다 우선)
binding perf 시험과 같은 기준으로 잽니다(이미 검증된 수치). 아직 counted run을 시작하지 않은 단계·셀부터 적용합니다.
- warmup: 1초(runner_common.sh가 허용하는 최소값, binding perf에는 별도 warmup 없음). 별도 discarded warm run 없음.
- active: 5초 (bindings/c/perf/run_benchmarks.sh PERF_SINGLE_DURATION_SECONDS=5).
- run 사이 간격: 3초 (bindings/c/perf/run_comparison.py PERF_RUN_COOLDOWN_MS=3000).
보고서에 단계·셀별로 어느 설정(이전 10초 warmup 또는 새 설정)으로 쟀는지 적으십시오. 같은 단계의 두 값(직전 단계와 이번 단계)은 반드시 같은 설정으로 잰 값끼리 비교합니다.
