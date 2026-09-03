# C perf aggregate gate 확장 요약

## 변경 파일

- `/home/hep7/project/zlink/bindings/c/perf/perf_regression_gate.py`
- `/home/hep7/project/zlink/bindings/c/perf/tests/test_perf_regression_gate.py`
- `/home/hep7/project/zlink-work/c016/sweep2.sh`
- `/home/hep7/project/zlink-work/c016/gate-aggregate-progress.md`
- `/home/hep7/project/zlink-work/c016/gate-aggregate-summary.md`

원본 `/home/hep7/project/zlink-work/c016/sweep.sh`는 수정하지 않았다.

## 인터페이스와 판정

- `--baseline-single`, `--candidate-single`, `--baseline-multi`, `--candidate-multi`는 기존 한 번 지정 문법을 유지하면서 반복 지정할 수 있다. 반복 report의 cell은 suite/side별로 병합하며 중복 cell은 FAIL이다.
- cell 판정은 기존 기준을 유지한다: throughput/bandwidth ratio `>= 0.95`, latency 계열 ratio `<= 1.05`.
- 집계는 `(suite, pattern, transport, metric)`마다 size `64,256,1024,65536` ratio의 기하평균을 사용한다. throughput/bandwidth는 `>= 1.0`, latency 계열은 `<= 1.0`이다.
- 필수 size가 하나라도 없으면 aggregate 표에 누락 size를 명시하고 FAIL한다. cell과 aggregate가 모두 PASS일 때만 exit 0이다.
- 출력은 기존 cell 표에 이어 size별 ratio, `Geomean`, rule, status를 가진 aggregate 표와 cell/aggregate 실패 수를 포함한 Final 행을 제공한다. `--help`에도 반복 입력, 필수 size, geometric mean을 명시했다.
- `sweep2.sh`는 `--msg-sizes`를 제공하며 기본값은 `64,256,1024,65536`이다. 한 `(pattern, transport)`에서 baseline runner 한 번, candidate runner 한 번에 전체 size를 전달하고 즉시 gate한다.
- `sweep2.sh`는 실행 중인 기존 job과 충돌하지 않도록 `sweep2-results.md`, `sweep2.log`, `sweep2-*` tag/temporary marker를 사용한다. 결과 표에는 worst cell ratio와 worst aggregate를 `agg=<metric>:<geomean>` 형태로 기록한다. 기존 PASS/FAIL/ERROR/UNSUPPORTED 기반 resume 동작은 유지한다.

## 테스트 결과

- `python3 -m pytest bindings/c/perf/tests -q`: PASS, `12 passed in 0.02s`.
- `bash -n /home/hep7/project/zlink-work/c016/sweep2.sh`: PASS.
- `sweep2.sh --help`: PASS. 기본 `--msg-sizes 64,256,1024,65536` 노출 확인.
- `perf_regression_gate.py --help`: PASS. 반복 report와 geometric mean 설명 확인.
- `git diff --check -- bindings/c/perf/perf_regression_gate.py bindings/c/perf/tests/test_perf_regression_gate.py`: PASS.
- 기존 PAIR/tcp 1024-byte baseline/candidate report 쌍 gate dry-run: 호출 성공, rc=1. `64,256,65536` 누락을 5개 metric aggregate 행에 명시한 기대 FAIL이었다.
- benchmark와 `scripts/local-package/**`는 실행하지 않았다. git write도 수행하지 않았다.
- 원본 `sweep.sh` SHA-256: `4d944402196a7ab5a38cd4b6bf308af5ff8697b11faeed31086f7775ed3374a3` (시작·종료 동일).

## QUESTIONS

- 없음.
