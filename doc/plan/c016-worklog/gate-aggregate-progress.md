## 2026-09-03 시작

- `main` 브랜치와 대상 저장소 파일의 clean 상태를 확인했다.
- `bindings/c/perf/AGENTS.md`와 `doc/plan/core-send-dontwait-completion-0.16.0-plan.ko.md` §5.2의 확정 기준을 확인했다.
- 기존 gate는 report path를 suite별 하나만 받고 1024-byte cell 판정만 출력하며, 기존 `sweep.sh`도 runner에 `--msg-sizes 1024`를 전달하는 상태다.
- 벤치와 `scripts/local-package/**`는 실행하지 않는다. 구현 후 unit test, `bash -n`, 기존 report pair를 이용한 gate 호출만 검증한다.

## 2026-09-03 구현 및 1차 검증

- gate 입력을 single/multi 각각 반복 지정할 수 있게 확장하고, report 간 cell을 병합하면서 중복은 FAIL 처리했다.
- cell 표 뒤에 `(suite, pattern, transport, metric)`별 `64/256/1024/65536` ratio, 기하평균, 집계 verdict 표를 추가했다. 필수 size 누락과 계산 불가 ratio는 집계 FAIL이다.
- `sweep.sh`를 byte-for-byte 복사해 `sweep2.sh`를 만든 뒤에만 수정했다. 기본 `--msg-sizes 64,256,1024,65536`, 별도 `sweep2-results.md`/`sweep2.log`, aggregate worst geomean 컬럼을 추가했다.
- focused test 첫 실행은 fixture duplicate 위치 기대 하나가 실패했고 fixture의 기존 1024 duplicate 의미를 보존해 수정했다. 재실행 및 전체 `bindings/c/perf/tests`: `12 passed`.
- `bash -n sweep2.sh`와 `sweep2.sh --help`, gate `--help`를 확인했다.
- 기존 PAIR/tcp 1024-byte baseline/candidate report 쌍으로 gate 호출을 dry-run했다. 호출 경로는 정상이고, 새 계약에 따라 `64,256,65536` 누락을 명시하며 rc=1이었다. 벤치는 실행하지 않았다.

## 2026-09-03 최종 검증

- `python3 -m pytest bindings/c/perf/tests -q`: `12 passed in 0.02s`.
- `bash -n /home/hep7/project/zlink-work/c016/sweep2.sh`: PASS.
- sweep2/gate `--help`, `git diff --check`를 확인했다.
- 원본 `sweep.sh` SHA-256은 시작·종료 모두 `4d944402196a7ab5a38cd4b6bf308af5ff8697b11faeed31086f7775ed3374a3`으로 동일하다.
- 범위 밖 기존 worktree 변경은 건드리지 않았고 git write, local-package, benchmark 실행은 하지 않았다.
