# #4350 aa-noise5 — Java binding A/A

## 결과

클라이언트·공유·raw 서버 JAR은 각 최신 소스보다 새로워 재빌드하지 않았다. [JAR/소스 시각 비교](raw/build-check.txt) `measure.lock` 한 번을 사용했다. 요청 직렬화는 A/B 각 5회, 포화 전송은 A/B 각 4회까지 끝났다. 14분 30초 제한으로 포화 전송 A5 전에 세션이 종료되어 20회 중 18회만 있다. 스케줄 정책 위반은 관측되지 않았다. 두 누락 런을 위해 두 번째 락을 열지 않았다. [중단 기록](raw/lock-windows/aa-noise5-stop.txt)

warmup 10초, active 5초로 측정했다. C2/C3는 수집된 active 기록에서 disabled였다. [request-serial](raw/run-env/request-serial-4096-A-run1.txt) [send-saturation](raw/run-env/send-saturation-4096-A-run1.txt) 요청 직렬화에는 유효한 5개 결과가 있어 G5를 계산했다. 포화 전송 통계는 완료된 4개씩으로 계산했으므로 요청한 5회 판정은 미완료다. 처리량은 msg/s다. A/B는 A 중앙값 ÷ B 중앙값이다.

| 셀 | A 중앙값 | B 중앙값 | A/B | G5 A | G5 B | 무효 / 미실행 |
|---|---:|---:|---:|---:|---:|---|
| request-serial 4K | 11,405.0 [A1][A2][A3][A4][A5] | 11,702.8 [B1][B2][B3][B4][B5] | 0.975 [A1][A2][A3][A4][A5][B1][B2][B3][B4][B5] | 3.59% [A1][A2][A3][A4][A5] | 3.39% [B1][B2][B3][B4][B5] | 정책 위반 0회 [A1 정책](raw/run-env/request-serial-4096-A-run1.txt) [A2 정책](raw/run-env/request-serial-4096-A-run2.txt) [A3 정책](raw/run-env/request-serial-4096-A-run3.txt) [A4 정책](raw/run-env/request-serial-4096-A-run4.txt) [A5 정책](raw/run-env/request-serial-4096-A-run5.txt) [B1 정책](raw/run-env/request-serial-4096-B-run1.txt) [B2 정책](raw/run-env/request-serial-4096-B-run2.txt) [B3 정책](raw/run-env/request-serial-4096-B-run3.txt) [B4 정책](raw/run-env/request-serial-4096-B-run4.txt) [B5 정책](raw/run-env/request-serial-4096-B-run5.txt) / 미실행 0회 |
| send-saturation 4K | 198,770.2 [SA1][SA2][SA3][SA4] | 196,547.4 [SB1][SB2][SB3][SB4] | 1.011 [SA1][SA2][SA3][SA4][SB1][SB2][SB3][SB4] | 5.39% [SA1][SA2][SA3][SA4] | 4.36% [SB1][SB2][SB3][SB4] | 정책 위반 0회 [A1 정책](raw/run-env/send-saturation-4096-A-run1.txt) [A2 정책](raw/run-env/send-saturation-4096-A-run2.txt) [A3 정책](raw/run-env/send-saturation-4096-A-run3.txt) [A4 정책](raw/run-env/send-saturation-4096-A-run4.txt) [B1 정책](raw/run-env/send-saturation-4096-B-run1.txt) [B2 정책](raw/run-env/send-saturation-4096-B-run2.txt) [B3 정책](raw/run-env/send-saturation-4096-B-run3.txt) [B4 정책](raw/run-env/send-saturation-4096-B-run4.txt) / 미실행 A5·B5 2회 [중단 기록](raw/lock-windows/aa-noise5-stop.txt) |

처리량 결과의 각 JSON은 submitted = completed = received, 오류 0이었다. 수집한 모든 source/target thread 정책은 `SCHED_OTHER`였다. [request-serial A1–A5 정책 기록](raw/run-env/) [request-serial B1–B5 정책 기록](raw/run-env/) [send-saturation A1–A4 정책 기록](raw/run-env/) [send-saturation B1–B4 정책 기록](raw/run-env/)

## 5% 초과 편차와 환경

관측된 포화 전송 값 8개의 합동 중앙값 198,132.1 msg/s [SA1][SA2][SA3][SA4][SB1][SB2][SB3][SB4]에서 5% 넘게 벗어난 런은 A1과 B1이다. 두 런 모두 처리량이 낮았지만 CPU 주파수 하락은 보이지 않았다. 측정 CPU 0–5와 8–13의 active 샘플 중앙 주파수는 A1 3.295 GHz, B1 3.221 GHz였고, 더 빠른 A2·B2는 각각 3.059 GHz, 3.056 GHz였다. 부하, 기록된 상위 프로세스 분류, IRQ 차분에서도 이 두 런에만 해당하는 뚜렷한 차이는 확인되지 않았다. [A1 환경](raw/run-env/send-saturation-4096-A-run1.txt) [A2 환경](raw/run-env/send-saturation-4096-A-run2.txt) [B1 환경](raw/run-env/send-saturation-4096-B-run1.txt) [B2 환경](raw/run-env/send-saturation-4096-B-run2.txt)

| 런 | 시작 시각 UTC | 처리량 / 합동 중앙값 대비 | 환경 기록 |
|---|---|---:|---|
| A1 | 2026-11-13 17:48:09 [SA1 환경](raw/run-env/send-saturation-4096-A-run1.txt) | 188,059.8 msg/s / −5.08% [SA1 결과](raw/results/send-saturation-4096/A/run1/run1/zlink-java-send-saturation-4096/results.json) | 부하 3.48 / 2.40 / 2.71 [SA1 환경](raw/run-env/send-saturation-4096-A-run1.txt); source·target mask 0–5 / 8–13, 외부 프로세스 6–7·14–15 [SA1 환경](raw/run-env/send-saturation-4096-A-run1.txt); CPU 주파수는 정상 런보다 낮지 않음 [A2 환경](raw/run-env/send-saturation-4096-A-run2.txt) |
| B1 | 2026-11-13 17:49:05 [SB1 환경](raw/run-env/send-saturation-4096-B-run1.txt) | 187,987.2 msg/s / −5.12% [SB1 결과](raw/results/send-saturation-4096/B/run1/run1/zlink-java-send-saturation-4096/results.json) | 부하 2.49 / 2.26 / 2.63 [SB1 환경](raw/run-env/send-saturation-4096-B-run1.txt); source·target mask 0–5 / 8–13, 외부 프로세스 6–7·14–15 [SB1 환경](raw/run-env/send-saturation-4096-B-run1.txt); CPU 주파수는 정상 런보다 낮지 않음 [B2 환경](raw/run-env/send-saturation-4096-B-run2.txt) |

A1과 B1의 상위 프로세스 목록은 벤치 외 JVM과 실행 에이전트 위주였고, 에이전트는 `IDL` 또는 작은 CPU 점유로 기록됐다. A1/B1의 로컬 타이머 인터럽트 차분은 각각 28,559 / 29,188이며 A2/B2는 29,655 / 34,121로, 편차 런만의 증가 양상은 없었다. [A1](raw/run-env/send-saturation-4096-A-run1.txt) [A2](raw/run-env/send-saturation-4096-A-run2.txt) [B1](raw/run-env/send-saturation-4096-B-run1.txt) [B2](raw/run-env/send-saturation-4096-B-run2.txt)

## 수집 범위 한계

요청 직렬화 런의 affinity 기록은 도구 보완 도중 누락됐다. A1–A3 및 B1–B3에는 taskset 기록이 없고, A4·A5·B4는 시작 시점만 있으며 B5만 시작·중간 기록이 있다. B2의 외부 프로세스 사후 조회는 프로세스 종료 후라 실패했다. 해당 런들의 주파수, 부하, 프로세스, IRQ와 스케줄 정책 기록은 보존했다. 포화 전송은 완료된 8개 런에서 시작·중간 affinity를 모두 기록했고, guard가 적용한 0–5 / 8–13 분할과 외부 프로세스 6–7·14–15를 확인했다. [요청 직렬화 raw](raw/run-env/) [포화 전송 raw](raw/run-env/)

## 원자료

| 링크 | 파일 |
|---|---|
| A1–A5 | [A1](raw/results/request-serial-4096/A/run1/run1/zlink-java-request-serial-4096/results.json) · [A2](raw/results/request-serial-4096/A/run2/run1/zlink-java-request-serial-4096/results.json) · [A3](raw/results/request-serial-4096/A/run3/run1/zlink-java-request-serial-4096/results.json) · [A4](raw/results/request-serial-4096/A/run4/run1/zlink-java-request-serial-4096/results.json) · [A5](raw/results/request-serial-4096/A/run5/run1/zlink-java-request-serial-4096/results.json) |
| B1–B5 | [B1](raw/results/request-serial-4096/B/run1/run1/zlink-java-request-serial-4096/results.json) · [B2](raw/results/request-serial-4096/B/run2/run1/zlink-java-request-serial-4096/results.json) · [B3](raw/results/request-serial-4096/B/run3/run1/zlink-java-request-serial-4096/results.json) · [B4](raw/results/request-serial-4096/B/run4/run1/zlink-java-request-serial-4096/results.json) · [B5](raw/results/request-serial-4096/B/run5/run1/zlink-java-request-serial-4096/results.json) |
| 포화 A1–A4 | [A1](raw/results/send-saturation-4096/A/run1/run1/zlink-java-send-saturation-4096/results.json) · [A2](raw/results/send-saturation-4096/A/run2/run1/zlink-java-send-saturation-4096/results.json) · [A3](raw/results/send-saturation-4096/A/run3/run1/zlink-java-send-saturation-4096/results.json) · [A4](raw/results/send-saturation-4096/A/run4/run1/zlink-java-send-saturation-4096/results.json) |
| 포화 B1–B4 | [B1](raw/results/send-saturation-4096/B/run1/run1/zlink-java-send-saturation-4096/results.json) · [B2](raw/results/send-saturation-4096/B/run2/run1/zlink-java-send-saturation-4096/results.json) · [B3](raw/results/send-saturation-4096/B/run3/run1/zlink-java-send-saturation-4096/results.json) · [B4](raw/results/send-saturation-4096/B/run4/run1/zlink-java-send-saturation-4096/results.json) |

[A1]: <raw/results/request-serial-4096/A/run1/run1/zlink-java-request-serial-4096/results.json>
[A2]: <raw/results/request-serial-4096/A/run2/run1/zlink-java-request-serial-4096/results.json>
[A3]: <raw/results/request-serial-4096/A/run3/run1/zlink-java-request-serial-4096/results.json>
[A4]: <raw/results/request-serial-4096/A/run4/run1/zlink-java-request-serial-4096/results.json>
[A5]: <raw/results/request-serial-4096/A/run5/run1/zlink-java-request-serial-4096/results.json>
[B1]: <raw/results/request-serial-4096/B/run1/run1/zlink-java-request-serial-4096/results.json>
[B2]: <raw/results/request-serial-4096/B/run2/run1/zlink-java-request-serial-4096/results.json>
[B3]: <raw/results/request-serial-4096/B/run3/run1/zlink-java-request-serial-4096/results.json>
[B4]: <raw/results/request-serial-4096/B/run4/run1/zlink-java-request-serial-4096/results.json>
[B5]: <raw/results/request-serial-4096/B/run5/run1/zlink-java-request-serial-4096/results.json>
[SA1]: <raw/results/send-saturation-4096/A/run1/run1/zlink-java-send-saturation-4096/results.json>
[SA2]: <raw/results/send-saturation-4096/A/run2/run1/zlink-java-send-saturation-4096/results.json>
[SA3]: <raw/results/send-saturation-4096/A/run3/run1/zlink-java-send-saturation-4096/results.json>
[SA4]: <raw/results/send-saturation-4096/A/run4/run1/zlink-java-send-saturation-4096/results.json>
[SB1]: <raw/results/send-saturation-4096/B/run1/run1/zlink-java-send-saturation-4096/results.json>
[SB2]: <raw/results/send-saturation-4096/B/run2/run1/zlink-java-send-saturation-4096/results.json>
[SB3]: <raw/results/send-saturation-4096/B/run3/run1/zlink-java-send-saturation-4096/results.json>
[SB4]: <raw/results/send-saturation-4096/B/run4/run1/zlink-java-send-saturation-4096/results.json>
