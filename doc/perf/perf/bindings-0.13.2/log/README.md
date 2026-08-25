# Core 0.13.2 bindings 성능 작업 로그

이 디렉터리에는 local Core 0.13.2에서 수행한 각 paired 측정, profiler/비용 분석,
개선 후보 A/B, Sol read-only review, contract 및 기능 회귀 결과를 기록한다.

각 log는 C report와 binding report의 절대 경로, Core revision, Effective Options,
aggregate throughput·latency 비율, 그리고 `통과` 또는 `미달`의 근거를 포함해야 한다.
`미달`이면 다음 transport·pattern·언어로 이동하기 전에 자체 개선 pass와 Sol pass의
후보 결과 또는 no-go 근거를 반드시 남긴다.
