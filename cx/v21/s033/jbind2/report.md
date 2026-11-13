# Java binding main·PR 재측정 결과 (#4060)

## 설정과 jar

main은 `6b24e01426c998b0b270d49c8a72a2e48f605317`, PR은 `b42f97845111c37d74d7b5315440611b16516d21`이다. `/root/jobs/fx/cx/v21/s033/jbind/main`과 `jbind/pr`의 벤치 사본을 `jbind2/main`, `jbind2/pr`로 복사해 사용했다. 기존 설치 산출물을 재사용했으며 SHA-256이 기준 보고서와 일치해 다시 빌드하지 않았다.

main server classpath의 jar 경로는 `/root/jobs/fx/cx/v21/s033/jbind2/main/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/lib/zlink-java-1.23.0.jar`이고 SHA-256은 `30c6dd5b726c68d7729dac4034c31b4afa9a8aeeff13642f36a9af4045648f1f`이다. PR server classpath의 jar 경로는 `/root/jobs/fx/cx/v21/s033/jbind2/pr/framework/bench/grpc/java/zlink-raw-server/build/install/bench-zlink-raw-server/lib/zlink-java-1.23.0.jar`이고 SHA-256은 `df0ac6f55a455d684fefbc5f2b512bfd7f86cafd0cb7951fcb9cd3147bb97150`이다. 두 실행 스크립트는 설치본의 `lib/*` classpath를 사용한다.

실행은 `zlink-java`, payload 1024 B, request window 100, send concurrency 8이었다. runner에 `WARMUP_SECONDS=10`, `DURATION_SECONDS=10`, `SKIP_BUILD=1`을 전달했다. `RUNS=1`인 side 실행을 여섯 번씩, 한 pair의 두 side를 한 `flock -o /root/jobs/fx/cx/measure.lock` 보유 안에서 실행해 총 여섯 인접 pair를 만들었다. 순서는 main/PR, PR/main, main/PR, PR/main, main/PR, PR/main이다. scratch runner의 `taskset`이 source JVM을 CPU 0–7, target JVM을 CPU 8–15에 고정했다. 각 side 실행에서 세 셀의 JVM affinity가 모두 확인됐다.

MemAvailable은 측정 시작 전 스냅샷에서 최저 25,333 MiB였다. 모든 side 실행은 시작 직전 1분 load가 4 이하일 때 시작했다. 최종 여섯 pair 중 다섯 side는 load가 4를 넘어 30초 기다린 뒤 시작했고, 모두 기준 이하로 내려왔다. 남은 한 건의 대기 로그는 제외한 중복 pair 1 실행에서 나왔다. 20분 제한에 도달한 실행은 없다.

## 실행별 부하·프로세스·주파수

주파수는 실행 직전 `grep MHz /proc/cpuinfo | sort | uniq -c`의 표본 최소–최대이며, `/proc/cpuinfo`에 나타난 표본 개수도 함께 적었다. 전체 상위 10개 프로세스 출력은 각 `logs/jbind2-pairN-{main,pr}-system.txt`에 보존했다. 대다수 실행의 상위 프로세스는 `claude` 3.7%, `htop` 2.5%, `kcompactd0` 2.2%, `containerd` 1.9%였고 나머지는 `tailscaled` 0.8% 이하 및 `codex` 프로세스였다. `ps` 자체가 상위에 나타난 pair 3 main과 pair 5 main 스냅샷은 그 항목을 제외하면 같은 배경 프로세스 집합이었다.

| Pair | Side | 시작 전 load 1m | MemAvailable MiB | load 대기 | 상위 프로세스 특이점 | MHz 표본 최소–최대 / 개수 |
|---:|---|---:|---:|---:|---|---:|
| 1 | main | 3.75 | 25,601 | 0초 | 배경 프로세스 집합 | 3128–4273 / 16 |
| 1 | PR | 2.61 | 25,615 | 30초 | 배경 프로세스 집합 | 400–4420 / 15 |
| 2 | main | 3.75 | 25,543 | 30초 | 배경 프로세스 집합 | 400–4448 / 14 |
| 2 | PR | 3.75 | 25,376 | 0초 | 배경 프로세스 집합 | 400–4349 / 15 |
| 3 | main | 2.58 | 25,615 | 30초 | `ps` 100%, 그 외 배경 프로세스 집합 | 400–4401 / 13 |
| 3 | PR | 3.51 | 25,593 | 0초 | 배경 프로세스 집합 | 400–3765 / 13 |
| 4 | main | 3.83 | 25,481 | 0초 | 배경 프로세스 집합 | 400–4324 / 13 |
| 4 | PR | 3.77 | 25,563 | 0초 | 배경 프로세스 집합 | 400–4363 / 14 |
| 5 | main | 2.63 | 25,629 | 30초 | `ps` 100%, 그 외 배경 프로세스 집합 | 400–4358 / 14 |
| 5 | PR | 3.59 | 25,570 | 0초 | 배경 프로세스 집합 | 400–3825 / 15 |
| 6 | main | 3.95 | 25,333 | 0초 | `python3` 2개가 각각 100% | 400–3241 / 14 |
| 6 | PR | 2.84 | 25,603 | 30초 | 배경 프로세스 집합 | 400–3877 / 12 |

pair 2 PR의 처리량은 앞뒤 PR 실행보다 세 셀에서 약 11–20% 낮았다. 그 실행 직전 상위 10개에는 CPU 사용률이 3.7%를 넘는 외부 작업이 없었고, 두 개의 100% `python3` 프로세스는 pair 6 main 직전 목록에만 나타났다. pair 6 main의 request-serial 처리량은 직전 main보다 9.5% 낮았지만, 양쪽 모두에서 약 30–40% 하락하는 단계 변화는 재현되지 않았다. 기록한 `comm` 정보만으로 pair 2 PR의 낮은 처리량을 특정 프로세스에 귀속할 수는 없다.

## 처리량과 pair 비율

처리량 단위는 ops/s이며, send-saturation은 sends/s로 해석한다. 값은 각 pair의 실행 순서대로 적었다. pair 비율은 `PR 처리량 / main 처리량 × 100`이다. 판정 조건의 main 인접 실행 변동 폭은 연속한 두 main 실행의 비율 변화 5개에서 계산했다.

| 셀 | main 값 1–6 | main 중앙값 | PR 값 1–6 | PR 중앙값 | pair별 PR/main 비율 1–6 | 비율 중앙값 및 범위 | main 인접 변동 폭 |
|---|---|---:|---|---:|---|---|---|
| request-serial | 15,104; 14,702; 14,247; 15,086; 14,588; 13,198 | 14,645 | 14,791; 11,920; 14,995; 15,007; 15,176; 15,111 | 15,001 | 97.93%; 81.08%; 105.25%; 99.47%; 104.03%; 114.49% | 101.75%; 81.08–114.49% | -9.53%–+5.89% |
| request-backpressure | 124,313; 129,501; 125,081; 119,290; 125,047; 126,424 | 125,064 | 125,948; 110,950; 128,207; 127,914; 123,835; 122,002 | 124,891 | 101.31%; 85.68%; 102.50%; 107.23%; 99.03%; 96.50% | 100.17%; 85.68–107.23% | -4.63%–+4.83% |
| send-saturation | 317,411; 313,182; 299,217; 295,956; 311,143; 317,565 | 312,162 | 283,219; 250,282; 315,664; 316,060; 313,736; 286,712 | 300,224 | 89.23%; 79.92%; 105.50%; 106.79%; 100.83%; 90.28% | 95.56%; 79.92–106.79% | -4.46%–+5.13% |

세 셀 모두 여섯 pair 비율이 같은 방향을 보이지 않았고, pair 비율 중앙값은 main 자체 인접 실행 변동 폭 밖에 있지 않았다. 따라서 세 셀 모두 차이를 확인하지 못했다. request-backpressure의 PR/main 중앙 pair 비율은 +0.17%로, 첫 측정의 PR 저하 경향은 재현되지 않았다.

전체 36개 셀에서 warmup과 active duration은 각각 10초였고 client/server 오류, abandoned 요청은 0이었다. 완료 수와 server 수신 수가 일치했다. 세부 결과 JSON, affinity 출력 및 system 스냅샷은 `/root/jobs/fx/cx/v21/s033/jbind2/results/`와 `/root/jobs/fx/cx/v21/s033/jbind2/logs/`에 보존했다.

pair 1은 runner를 중복 시작한 추가 측정이 한 번 있었고, 같은 결과 경로를 사용한 뒤 여섯 pair 순서의 최종 pair 1 실행이 해당 경로를 덮어썼다. 보고서의 pair 1 값과 실행 요약은 최종 여섯 pair 순서에 속한 결과만 사용했다.
