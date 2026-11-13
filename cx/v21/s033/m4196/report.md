# 결과

**측정 가능한 회귀는 확인되지 않았다.** 네 셀 모두 BASE와 PR의 중앙값 차이가 두 구현 중 더 넓은 3회 처리량 범위보다 작았다. PR/BASE 중앙값 비율은 셀마다 달랐지만 회차별 변동 안에 있었다. [처리량·CPU 분석](</root/jobs/fx/cx/v21/s033/m4196/raw/final2/analysis.txt>) · [24회 원자료 경로와 수치](</root/jobs/fx/cx/v21/s033/m4196/raw/final2/summary.csv>)

단위는 msg/s, CPU는 µs/msg다. 처리량은 각 회차의 벤치 결과이며, `PR/BASE`는 두 중앙값의 비율이고, 괄호는 같은 회차별 비율 범위다. `Δ / 폭`은 두 중앙값 차이와 BASE·PR 중 더 큰 회차별 처리량 범위 폭이다.

| 셀 | BASE r1 / r2 / r3; 중앙값 [범위] | PR r1 / r2 / r3; 중앙값 [범위] | PR/BASE; 회차별 비율 범위 | CPU source·target BASE → PR | Δ / 폭 초과 여부 |
|---|---|---|---|---|---|
| Binding send-saturation 1024 | 227,345 / 217,822 / 193,358; **217,822** [193,358–227,345] | 218,872 / 218,908 / 221,753; **218,908** [218,872–221,753] | **1.005** (0.963–1.147) | 29.18→28.36 / 7.10→7.17 | 1,086 / 33,986; 아니오 |
| Binding send-saturation 4096 | 162,738 / 165,989 / 154,105; **162,738** [154,105–165,989] | 159,809 / 157,177 / 140,687; **157,177** [140,687–159,809] | **0.966** (0.913–0.982) | 34.93→36.24 / 10.87→11.33 | 5,562 / 19,123; 아니오 |
| Binding request-backpressure 1024 | 61,547 / 45,300 / 47,147; **47,147** [45,300–61,547] | 57,838 / 55,903 / 55,815; **55,903** [55,815–57,838] | **1.186** (0.940–1.234) | 88.14→87.76 / 41.00→37.75 | 8,756 / 16,246; 아니오 |
| Framework .NET send-saturation 1024 | 186,889 / 172,939 / 159,218; **172,939** [159,218–186,889] | 163,814 / 171,757 / 125,173; **163,814** [125,173–171,757] | **0.947** (0.786–0.993) | 19.77→20.50 / 14.57→15.30 | 9,125 / 46,584; 아니오 |

CPU는 active 경계의 `/proc/<pid>/task/<tid>/schedstat` before/after 차이를 source와 target 프로세스별 TID 합집합에서 합산한 뒤, 같은 snapshot 구간의 source 완료 수로 나눴다. 셀별 원본 `results.json`, 네 개의 `/proc` snapshot과 affinity 파일 경로는 [summary.csv](</root/jobs/fx/cx/v21/s033/m4196/raw/final2/summary.csv>)의 `result_path`와 각 셀 디렉터리에 있다. 24개 셀 모두 source 완료 수와 target 수신 수가 같고, CPU affinity는 source 0–7·target 8–15였다. [검증 요약](</root/jobs/fx/cx/v21/s033/m4196/raw/final2/verification.txt>) · [측정 로그](</root/jobs/fx/cx/v21/s033/m4196/logs/measurement-final2.log>)

# 검증(명령·로그 경로·원본 경로)

- 빌드: 각 worktree에서 `flock -s /root/jobs/fx/cx/measure.lock bash framework/bench/grpc/dotnet/build.sh`. 두 빌드 모두 `ZLINK_CORE_SOURCE=release`, `ZLINK_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core-release/1.24.0/linux-x64`, `ZLinkUseBindingsSource=true`를 사용했다. raw bench의 두 직접 패키지 참조는 JOB의 [임시 MSBuild 설정](</root/jobs/fx/cx/v21/s033/m4196/bindings-source.targets>)으로 해당 worktree의 binding project reference에 연결했다. [BASE 빌드 로그](</root/jobs/fx/cx/v21/s033/m4196/logs/build-base-sources.log>) · [PR 빌드 로그](</root/jobs/fx/cx/v21/s033/m4196/logs/build-pr-sources.log>)
- binding 출처: 각 worktree의 `bindings/dotnet/src/Zlink/bin/Release/net8.0/Systems.Zlink.dll`과 Client·ZLinkRawServer 출력 DLL의 SHA-256 및 수정 시각이 각각 일치했다. BASE 세 파일은 `4ef56782…d26c8dd`, PR 세 파일은 `25fcfec0…42bb686`이며 worktree 간 해시는 다르다. 비교 경로·전체 hash·mtime은 [빌드 provenance](</root/jobs/fx/cx/v21/s033/m4196/logs/build-provenance.txt>)에 있다.
- 측정: `flock -o /root/jobs/fx/cx/measure.lock bash /root/jobs/fx/cx/v21/s033/m4196/run-cells.sh`를 한 번 실행했다. 환경은 `OUTROOT=/root/jobs/fx/cx/v21/s033/m4196/raw/final2`, Core 1.24.0 `ZLINK_CORE_SOURCE=release`, `ZLINK_CORE_PACKAGE_PREFIX=/root/.cache/zlink/core-release/1.24.0/linux-x64`, `ZLINK_LIBRARY_PATH=$P/lib/libzlink.so.1.24.0`, `LD_LIBRARY_PATH=$P/lib`였다. 매 회차 BASE/PR 순서는 1회차 BASE→PR, 2회차 PR→BASE, 3회차 BASE→PR이며 각 셀 warmup 30초·active 5초다. [실행 로그](</root/jobs/fx/cx/v21/s033/m4196/logs/measurement-final2.log>) · [runner와 snapshot 수집 코드](</root/jobs/fx/cx/v21/s033/m4196/run-cells.sh>)
- 완료 후 두 worktree의 생성된 `bin/`·`obj/` 디렉터리 52개를 제거했다. worktree 추적 상태는 깨끗하며 worktree는 유지했다. [BASE 상태](</root/jobs/fx/cx/v21/s033/m4196/logs/base-worktree-status.txt>) · [PR 상태](</root/jobs/fx/cx/v21/s033/m4196/logs/pr-worktree-status.txt>) · [정리 기록](</root/jobs/fx/cx/v21/s033/m4196/logs/build-output-cleanup.txt>)

리팩토링 점검: 성능 0건, POSDDD 0건, 불필요 코드 0건 발견. 고친 항목 0, 넘긴 항목 0.

# 남은 문제

세 회차의 변동 폭이 커서 작은 차이를 판정하기 어렵다. 특히 Framework send-saturation의 중앙값은 PR에서 5.3% 낮았지만 회차별 폭 46,584 msg/s 안에 있다. 측정 가능한 회귀로 분류할 정도로 재현되지 않았다. [분산 분석](</root/jobs/fx/cx/v21/s033/m4196/raw/final2/analysis.txt>)

첫 실행은 `ZLINK_LIBRARY_PATH`가 빠져 native Core를 찾지 못했고, 다음 시도는 target 통계 병합 전 검증해 중단했다. 두 시도는 최종 집계에서 제외했으며 로그와 raw 파일은 보존했다. [첫 실행 로그](</root/jobs/fx/cx/v21/s033/m4196/logs/measurement.log>) · [제외된 셀 로그](</root/jobs/fx/cx/v21/s033/m4196/raw/final/round1/base/zlink-dotnet-send-saturation-1024/target.log>) · [두 번째 실행 로그](</root/jobs/fx/cx/v21/s033/m4196/logs/measurement-final.log>)
