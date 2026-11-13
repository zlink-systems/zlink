# #1639 C++ request-backpressure 1024 A/B

## 결과

5회 중앙값 (범위). CPU는 µs/msg, context switch는 /msg입니다.

| 지표 | BASE | PR |
|---|---:|---:|
| source 처리량 (msg/s) | 43,350.0 (43,144.9–43,840.4) | 43,029.2 (41,750.1–43,524.7) |
| target 처리량 (msg/s) | 43,355.0 (43,155.9–43,851.0) | 43,038.6 (41,758.7–43,533.9) |
| source 총 CPU | 70.71 (67.60–72.40) | 71.76 (68.46–72.86) |
| target 총 CPU | 65.20 (54.04–73.88) | 64.54 (55.17–73.05) |
| target `zlink-mesh-app` CPU | 29.06 (18.35–37.95) | 28.89 (21.55–38.52) |
| target Core I/O CPU | 35.71 (33.77–35.87) | 34.25 (33.44–36.11) |
| target other CPU | 0.28 (0.23–0.32) | 0.28 (0.18–0.34) |
| target worker context switches v/nv | 0.5865 (0.4844–0.6963) / 0.0015 (0.0011–0.0022) | 0.4967 (0.3679–0.6798) / 0.0015 (0.0013–0.0023) |
| target Core I/O context switches v/nv | 0.7159 (0.6948–0.8194) / 0.0016 (0.0014–0.0017) | 0.8069 (0.7330–0.8770) / 0.0016 (0.0014–0.0017) |
| target other context switches v/nv | 0.0225 (0.0222–0.0227) / 0.0000 (0.0000–0.0000) | 0.0228 (0.0225–0.0234) / 0.0000 (0.0000–0.0000) |
| target worker live threads 시작→끝 | 3 (2–3) → 2 (2–3) | 3 (3–3) → 3 (3–3) |
| target Core I/O live threads 시작→끝 | 4 (4–4) → 4 (4–4) | 4 (4–4) → 4 (4–4) |
| target other live threads 시작→끝 | 26 (26–26) → 26 (26–26) | 26 (26–26) → 26 (26–26) |

paired target 총 CPU 차이(PR−BASE)는 round별 −9.34, +11.97, +12.44, −10.57, −3.86 µs/msg로 부호가 바뀌었습니다. **차이는 round-to-round 산포 안에 있고 어느 thread group도 일관된 증가를 보이지 않았습니다.** 전체 per-run source/target group 값은 [measurement-summary.csv](./raw/measurement-summary.csv), TID별 `comm`·CPU·context-switch 원본은 [attempt2 스냅샷](./measure/attempt2/)과 [analysis-summary.md](./raw/analysis-summary.md)에 있습니다.

## 측정 조건

- Framework C++ `request-backpressure`, 1024 B, warmup 30초, active 5초, 5라운드. 순서는 BASE/PR, PR/BASE, BASE/PR, PR/BASE, BASE/PR입니다. [measurement-attempt2.log](./raw/run/measurement-attempt2.log)
- exclusive `measure.lock` 안에서 CPU 0–15 governor를 performance로 설정했고 종료 trap에서 powersave로 복구했습니다. source affinity 0–7, target 8–15. [governor 기록](./raw/run/governor-and-lock-attempt2.log), 각 실행의 [system-before/after 스냅샷](./measure/attempt2/)에 uptime, load, free memory, CPU 0–15 `scaling_cur_freq`가 있습니다.
- 저장된 proc 스냅샷의 Core I/O 이름은 `ZLINKbg/IO/<n>`, worker 이름은 `zlink-mesh-app`입니다. [원본 예시](./measure/attempt2/1/request-backpressure/base/proc-window.json)

## 검증

- Release, `ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON`, Core prefix `/root/.cache/zlink/core-release/1.25.0/linux-x64`, `-j3`, 25 GB cap 빌드. 명령은 [build-commands.txt](./raw/build/build-commands.txt), 최종 성공 로그는 [BASE](./raw/build/base-rebuild2.log)와 [PR](./raw/build/pr-rebuild2.log)입니다.
- link line과 SHA-256에서 각각 자기 worktree의 `framework/libzlink_framework.a`를 링크했음을 확인했습니다: [BASE 증거](./raw/build/base-link-evidence.txt), [PR 증거](./raw/build/pr-link-evidence.txt). 커밋은 BASE `a2342b92`와 PR `1ddf4495`; [source-state.txt](./raw/build/source-state.txt).
- 유효 측정 명령: `ATTEMPT=attempt2 flock /root/jobs/fx/cx/measure.lock bash /root/jobs/fx/cx/v21/s033/m1639idle3/raw/run-locked.sh`. [명령 기록](./raw/run/measurement-command.txt), [실행 로그](./raw/run/measurement-attempt2.log). 10/10 실행이 rc 0으로 active window와 affinity를 기록했고, 20개 system snapshot에 CPU 0–15 주파수 키가 모두 있습니다.
- CPU는 `/proc/<pid>/task/*/stat`의 utime+stime 차이를 TID union으로 합산했고, context switch는 각 TID의 `/status` 차이로 계산했습니다. group 요약은 저장된 `comm`을 기준으로 재계산했습니다. [분석 스크립트](./raw/analyze.py)
- attempt1은 이전 JOB client의 `sendConcurrency`와 현재 runner의 `submitConcurrency` 불일치로 시작 요청이 HTTP 400이 되어 제외했습니다. 현재 worktree bench 입력으로 JOB 사본을 갱신해 재빌드한 뒤 attempt2를 측정했습니다. [attempt1 실패 로그](./raw/run/measurement-attempt1.log), [bench 입력 SHA-256](./raw/build/bench-input-sha256.txt).

리팩토링 점검: 성능 0건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목 0, 넘긴 항목 없음(측정 작업, 저장소 코드 변경 없음).

## 남은 문제

이 한 셀에서는 PR의 target CPU 증가가 재현되지 않아 특정 thread group에 원인을 귀속할 수 없습니다.
