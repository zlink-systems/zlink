# #4350 measurement-server noise — binding A/A

**판정: 시간 상한 내 측정 미실행.** A/A 결과가 없어 median, G5 spread, A/B ratio, outlier 및 환경 원인 판정은 산출할 수 없다. 측정하지 않은 run의 환경 로그도 없다. 상태 증거는 [raw/wait-status.log](raw/wait-status.log)에 있다.

| 항목 | 결과 | 근거 |
|---|---|---|
| Java bench build | 시작 후 quiet guard에 의해 정지됨. JAR freshness 확인 전 상한 도달 | [raw/wait-status.log](raw/wait-status.log) |
| request-serial 4K | A/B 각 10회 미실행 | [raw/wait-status.log](raw/wait-status.log) |
| send-saturation 4K | A/B 각 10회 미실행 | [raw/wait-status.log](raw/wait-status.log) |
| lock | 선행 Java 측정 중 .NET 세션이 lock을 이어받았고, 이후 Java 측정 세션이 대기열에 있음 | [raw/wait-status.log](raw/wait-status.log) |

quiet guard와 공용 exclusive lock을 우회하지 않았다. 코드 변경·commit·push는 없다. `tools/`에는 요청대로 복사한 도구와 A/A 측정 수집기가 있다.
