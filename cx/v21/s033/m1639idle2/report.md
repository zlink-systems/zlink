# #1639 receive-role handoff: C++ submit-path A/B

## 결과

| 셀 (1024 B) | BASE msg/s (중앙값; 범위) | PR msg/s (중앙값; 범위) | PR/BASE; 대응 라운드 차이 | CPU µs/msg BASE → PR (source / target 중앙값) | `zlink-mesh-app` 스레드 (source / target 시작→끝) | 산포 초과 여부 |
|---|---:|---:|---:|---:|---:|---|
| send-saturation | 149,743 (120,844–168,118) | 157,393 (127,832–158,781) | 1.051 (+5.1%); −6.4%, +6.0%, +5.8% | 5.29 / 14.92 → 4.74 / 11.96 | 모두 0→0 | 아니오; 두 반복 범위가 겹침 |
| request-backpressure | 47,618 (45,155–49,378) | 46,279 (38,617–47,481) | 0.972 (−2.8%); −14.5%, −3.8%, −2.8% | 59.02 / 47.69 → 63.49 / 53.48 | 모두 0→0 | 아니오; 두 반복 범위가 겹침 |

라운드별 원값과 CPU·스레드 계측은 [measurement-summary.csv](./raw/measurement-summary.csv), 실행별 원본은 [attempt4 proc-window 및 system 스냅샷](./measure/attempt4/)에 있다. 부하 중 `zlink-mesh-app` 스레드가 두 프로세스 모두에서 관측되지 않아 idle-list walk 자체를 이 benchmark가 실행했는지는 확인되지 않았다. 따라서 **이 두 Framework C++ benchmark에서 산포를 넘는 submit-path 회귀는 측정되지 않았지만, idle-worker 검사 비용을 직접 검증한 결과는 아니다.** 요약 계산은 [analysis-summary.txt](./raw/analysis-summary.txt)에 있다.

## 측정 조건

- PM 승인 조건(2026-11-11): 측정 때만 `flock /root/jobs/fx/cx/measure.lock` exclusive 보유, CPU 0–15 governor를 performance로 설정하고 종료 trap에서 powersave 복구. affinity source 0–7, target 8–15. 측정 12회 동안 잠금 유지 후 해제했고 governor 복구를 확인했다. 증거: [governor/lock 기록](./raw/run/governor-and-lock-attempt4.log), [flock 실행 로그](./raw/run/flock-run-attempt4.log).
- 매 실행 전후 uptime, load, `free -b`, CPU 0–15 `scaling_cur_freq`를 기록했다. 24개 시스템 스냅샷에서 1분 load 2.09–10.91, available memory 25.40–27.58 GB, 주파수 400,000–4,515,522 kHz였다. 상세값: [system-snapshots.json](./raw/system-snapshots.json) 및 각 실행의 `system-before.json` / `system-after.json` ([attempt4](./measure/attempt4/)).
- 각 셀 3회, 라운드 내 BASE/PR 교대, 같은 세션, warmup 30초와 active 5초. 두 variant 모두 affinity와 active phase가 수집됐다. 셀 순서·세부 실행값: [measurement-attempt4.log](./raw/run/measurement-attempt4.log), [measurement-summary.csv](./raw/measurement-summary.csv).

## 검증

- 커맨드: `bash /root/jobs/fx/cx/v21/s033/m1639idle2/raw/build/build.sh base` 및 `... build.sh pr`; 두 빌드는 `/root/jobs/fx/cx/cpp-build-slot.sh` 경유, `-j3`, 25 GB virtual-memory cap으로 수행. Release, `ZLINK_FRAMEWORK_CPP_USE_BINDINGS_SOURCE=ON`, Core prefix `/root/.cache/zlink/core-release/1.24.0/linux-x64`를 사용했다. 빌드 로그: [base](./raw/build/base-proto-retry.log) + [base resume](./raw/build/base-resume.log), [PR](./raw/build/pr.log); 입력 CMake/proto 사본은 [raw/build](./raw/build/) 및 [JOB scratch](./)에 보존했다.
- 바이너리 소속은 `ninja -t commands` link line, 실행 파일 SHA-256/mtime, link된 framework 및 binding archive 경로·SHA-256, `ldd`를 확인했다. BASE binary/archive 경로는 BASE worktree `.m1639idle2-build` 아래이고 PR도 PR worktree 아래다. 근거: [BASE link evidence](./raw/build/base-link-evidence.txt), [PR link evidence](./raw/build/pr-link-evidence.txt). 빌드 설정과 커밋 스냅샷은 같은 경로의 build 로그 및 [source-state.txt](./raw/source-state.txt).
- 측정 명령: `ATTEMPT=attempt4 flock /root/jobs/fx/cx/measure.lock bash /root/jobs/fx/cx/v21/s033/m1639idle2/raw/run-locked.sh`. 각 실행에서 `/proc/<pid>/task`의 `schedstat` runtime을 TID union으로 합산해 completed message로 나눴다. 결과·절차 원본: [run-measure.py](./raw/run-measure.py), [attempt4 실행 로그](./raw/run/measurement-attempt4.log), [12개 proc-window.json](./measure/attempt4/).
- 이전 1–3차 실행은 binary 경로 별칭 및 PID 포착 harness 문제로 제외했다. 해당 원본은 [attempt1–3](./measure/)와 [failures](./raw/failures/)에 보존했다. 성공 집계는 attempt4의 12/12 실행만 사용했다.

리팩토링 점검: 성능 0건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목 0, 넘긴 항목 없음(코드 변경 없음).

## 남은 문제

- `zlink-mesh-app` 스레드가 0개여서 PR 변경의 idle-list 검사 경로를 실제로 태웠다는 증거가 없다. 이번 결과만으로 해당 검사 비용 유무를 판정할 수 없다.
- request-backpressure의 PR 중앙 처리량은 2.8% 낮고 source/target CPU는 각각 7.6%/12.1% 높았으나 세 라운드의 variant별 처리량 범위가 겹친다. 원시 측정값은 [measurement-summary.csv](./raw/measurement-summary.csv)에 있다.
