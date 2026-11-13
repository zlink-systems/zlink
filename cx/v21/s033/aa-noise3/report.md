# #4350 측정 서버 A/A 소음

## 판정

request-serial 유효 run에서 G5가 A 32.4%, B 14.1%였고 A/B 중앙값 비율은 0.946이었다. 빠른 단일 측정으로 판단하기에는 분산이 크다. send-saturation은 유효 run이 A와 B 각각 하나뿐이라 G5 판단은 불가하다. [request-serial A 결과](raw/results/session2/request-serial-4096/A/run1/run1/zlink-java-request-serial-4096/results.json), [A2](raw/results/session2/request-serial-4096/A/run2/run1/zlink-java-request-serial-4096/results.json), [A3](raw/results/session2/request-serial-4096/A/run3/run1/zlink-java-request-serial-4096/results.json), [A5](raw/results/session2/request-serial-4096/A/run5/run1/zlink-java-request-serial-4096/results.json), [request-serial B 결과](raw/results/session2/request-serial-4096/B/run1/run1/zlink-java-request-serial-4096/results.json), [B3](raw/results/session2/request-serial-4096/B/run3/run1/zlink-java-request-serial-4096/results.json), [B4](raw/results/session2/request-serial-4096/B/run4/run1/zlink-java-request-serial-4096/results.json), [B5](raw/results/session2/request-serial-4096/B/run5/run1/zlink-java-request-serial-4096/results.json), [send-saturation A1](raw/results/session2/send-saturation-4096/A/run1/run1/zlink-java-send-saturation-4096/results.json), [B1](raw/results/session2/send-saturation-4096/B/run1/run1/zlink-java-send-saturation-4096/results.json).

## 집계

유효 run만 중앙값과 G5에 포함했다. 비율은 A 중앙값 ÷ B 중앙값이다. 각 값의 근거 파일을 셀에 연결했다.

| 셀 | A 중앙값 (msg/s) | B 중앙값 (msg/s) | A/B | G5 A | G5 B | 스케줄링 무효 |
|---|---:|---:|---:|---:|---:|---|
| request-serial 4K | 11,366.3 ([A1](raw/results/session2/request-serial-4096/A/run1/run1/zlink-java-request-serial-4096/results.json), [A2](raw/results/session2/request-serial-4096/A/run2/run1/zlink-java-request-serial-4096/results.json), [A3](raw/results/session2/request-serial-4096/A/run3/run1/zlink-java-request-serial-4096/results.json), [A5](raw/results/session2/request-serial-4096/A/run5/run1/zlink-java-request-serial-4096/results.json)) | 12,013.2 ([B1](raw/results/session2/request-serial-4096/B/run1/run1/zlink-java-request-serial-4096/results.json), [B3](raw/results/session2/request-serial-4096/B/run3/run1/zlink-java-request-serial-4096/results.json), [B4](raw/results/session2/request-serial-4096/B/run4/run1/zlink-java-request-serial-4096/results.json), [B5](raw/results/session2/request-serial-4096/B/run5/run1/zlink-java-request-serial-4096/results.json)) | 0.946 ([A 유효 run](raw/results/session2/request-serial-4096/A/run1/run1/zlink-java-request-serial-4096/results.json), [B 유효 run](raw/results/session2/request-serial-4096/B/run1/run1/zlink-java-request-serial-4096/results.json)) | 32.4% ([A 유효 run](raw/results/session2/request-serial-4096/A/run1/run1/zlink-java-request-serial-4096/results.json)) | 14.1% ([B 유효 run](raw/results/session2/request-serial-4096/B/run1/run1/zlink-java-request-serial-4096/results.json)) | A4, B2 ([A4 환경](raw/run-env/session2/request-serial-4096-A-run4.txt), [B2 환경](raw/run-env/session2/request-serial-4096-B-run2.txt)) |
| send-saturation 4K | 197,980.0 ([A1](raw/results/session2/send-saturation-4096/A/run1/run1/zlink-java-send-saturation-4096/results.json)) | 195,307.6 ([B1](raw/results/session2/send-saturation-4096/B/run1/run1/zlink-java-send-saturation-4096/results.json)) | 1.014 ([A1](raw/results/session2/send-saturation-4096/A/run1/run1/zlink-java-send-saturation-4096/results.json), [B1](raw/results/session2/send-saturation-4096/B/run1/run1/zlink-java-send-saturation-4096/results.json)) | 산출 불가: 유효 run 1개 ([A1](raw/results/session2/send-saturation-4096/A/run1/run1/zlink-java-send-saturation-4096/results.json)) | 산출 불가: 유효 run 1개 ([B1](raw/results/session2/send-saturation-4096/B/run1/run1/zlink-java-send-saturation-4096/results.json)) | A2 ([A2 환경](raw/run-env/session2/send-saturation-4096-A-run2.txt)) |

## 무효 run과 이상치

무효 run은 프로세스 시작 직후 source와 target의 스레드에서 `SCHED_IDLE`이 관측되어 제외했다. 활성 중간 스냅샷에서는 해당 스레드가 `SCHED_OTHER`였지만 시작 시점 위반으로 무효 처리했다. [request A4](raw/run-env/session2/request-serial-4096-A-run4.txt), [request B2](raw/run-env/session2/request-serial-4096-B-run2.txt), [send A2](raw/run-env/session2/send-saturation-4096-A-run2.txt).

이상치는 request-serial 유효 8개 run의 통합 중앙값 대비 5% 초과 편차로 골랐다. [중앙값 산출에 쓴 A run](raw/results/session2/request-serial-4096/A/run1/run1/zlink-java-request-serial-4096/results.json), [B run](raw/results/session2/request-serial-4096/B/run1/run1/zlink-java-request-serial-4096/results.json).

| 이상치 | 시작 UTC | 처리량·편차 | 환경 기록 |
|---|---|---|---|
| A2 | 2026-11-13 16:49:59 ([환경](raw/run-env/session2/request-serial-4096-A-run2.txt)) | 10,827.2 msg/s, −9.2% ([결과](raw/results/session2/request-serial-4096/A/run2/run1/zlink-java-request-serial-4096/results.json)) | 평균 주파수 3.140 GHz, loadavg 2.03. `ps`에 `IDL` 클래스 `cc1plus` 세 개가 38.5%, 31.5%, 21.9%로 보인다. ([환경](raw/run-env/session2/request-serial-4096-A-run2.txt)) |
| A3 | 2026-11-13 16:50:59 ([환경](raw/run-env/session2/request-serial-4096-A-run3.txt)) | 7,686.6 msg/s, −35.6% ([결과](raw/results/session2/request-serial-4096/A/run3/run1/zlink-java-request-serial-4096/results.json)) | 평균 주파수 3.024 GHz, loadavg 2.29. `IDL` 클래스 `cc1plus` 세 개가 27.6%, 21.9%, 14.6%로 보인다. ([환경](raw/run-env/session2/request-serial-4096-A-run3.txt)) |
| B5 | 2026-11-13 16:54:24 ([환경](raw/run-env/session2/request-serial-4096-B-run5.txt)) | 10,323.8 msg/s, −13.4% ([결과](raw/results/session2/request-serial-4096/B/run5/run1/zlink-java-request-serial-4096/results.json)) | 평균 주파수 3.049 GHz, loadavg 2.00. `IDL` 클래스 `cc1plus` 세 개가 15.5%, 11.9%, 7.6%로 보인다. ([환경](raw/run-env/session2/request-serial-4096-B-run5.txt)) |

send-saturation에서 유효 A1과 B1은 임시 통합 중앙값 기준 5% 이내라 이상치로 표시하지 않았다. 다만 유효 표본이 한쪽당 하나뿐이다. ([A1](raw/results/session2/send-saturation-4096/A/run1/run1/zlink-java-send-saturation-4096/results.json), [B1](raw/results/session2/send-saturation-4096/B/run1/run1/zlink-java-send-saturation-4096/results.json), [A1 환경](raw/run-env/session2/send-saturation-4096-A-run1.txt), [B1 환경](raw/run-env/session2/send-saturation-4096-B-run1.txt)).

같은 `IDL` 클래스 빌드 프로세스는 정상 범위 run에도 보여 이상치만의 조건은 아니었다. 평균 주파수와 loadavg도 이상치와 정상 run 사이에 뚜렷하게 갈리지 않아, 기록한 환경 자료만으로 낮은 처리량의 단일 원인을 특정하지 못했다. [정상 A1 환경](raw/run-env/session2/request-serial-4096-A-run1.txt), [이상치 A2 환경](raw/run-env/session2/request-serial-4096-A-run2.txt), [이상치 A3 환경](raw/run-env/session2/request-serial-4096-A-run3.txt), [이상치 B5 환경](raw/run-env/session2/request-serial-4096-B-run5.txt).

## 범위와 참고

잠금 예산 경계에서 다음 run을 시작하지 않고 세션을 끝냈다. request-serial은 양쪽 5회 시도했고 A4와 B2가 스케줄링 무효라 유효 표본은 각 4회다. send-saturation은 A 2회, B 1회 시도했고 A2가 무효라 유효 표본은 각 1회다. 따라서 요청한 전체 반복 수를 채우지 못했으며 send-saturation 소음 판정은 미완료다. [request A5](raw/run-env/session2/request-serial-4096-A-run5.txt), [request B5](raw/run-env/session2/request-serial-4096-B-run5.txt), [request A4](raw/run-env/session2/request-serial-4096-A-run4.txt), [request B2](raw/run-env/session2/request-serial-4096-B-run2.txt), [send A2](raw/run-env/session2/send-saturation-4096-A-run2.txt), [send B1](raw/run-env/session2/send-saturation-4096-B-run1.txt).

첫 진입 시도는 복사 스크립트의 저장소 경로 오류로 측정 전에 실패했으며, 오류 로그를 보존했다. [오류 로그](raw/request-serial-4096-A-run1-driver.log). JAR은 Java 소스보다 최신이라 재빌드하지 않았다. 코드 변경과 커밋은 없다.
