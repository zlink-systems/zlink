
## 23:10 UTC — "Local HTTP client package was not found" 해결법 (감독자)
pre-pr.sh와 같은 절차를 각 worktree 루트에서 실행하십시오(push·GitHub 없이 로컬 build만):
1. `mkdir -p .artifacts/wsl/npm && cp /root/.cache/zlink/packages/local-1.25.0/node/npm/zlink-systems-zlink-1.25.0.tgz .artifacts/wsl/npm/`
2. `unset ZLINK_LOCAL_PACKAGE_ROOT ZLinkLocalPackageRoot; bash scripts/local-package/http-client/build-wsl.sh node`
3. 그 다음 `cd framework/languages/node && npm ci` 후 원래 build를 계속합니다.
Core binding version은 bindings/node/VERSION의 ZLINK_BINDING_VERSION(1.25.0)입니다.

## 00:05 UTC — 측정 시간 설정 변경 (대표님 지시, 앞 메모의 warmup 값보다 우선)
binding perf 시험과 같은 기준으로 잽니다(이미 검증된 수치). 아직 counted run을 시작하지 않은 단계·셀부터 적용합니다.
- warmup: 1초(runner_common.sh가 허용하는 최소값, binding perf에는 별도 warmup 없음). 별도 discarded warm run 없음.
- active: 5초 (bindings/c/perf/run_benchmarks.sh PERF_SINGLE_DURATION_SECONDS=5).
- run 사이 간격: 3초 (bindings/c/perf/run_comparison.py PERF_RUN_COOLDOWN_MS=3000).
보고서에 단계·셀별로 어느 설정(이전 10초 warmup 또는 새 설정)으로 쟀는지 적으십시오. 같은 단계의 두 값(직전 단계와 이번 단계)은 반드시 같은 설정으로 잰 값끼리 비교합니다.
